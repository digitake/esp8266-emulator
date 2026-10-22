"""GUI boundary regressions: no network listeners or firmware execution required."""
import email.message
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

CHIP = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gui_server', CHIP / 'gui/server.py')
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)
from profiles import MAX_PROFILE_BYTES, pin_roles, validate_profile

FAKE_EMU = '''#!/usr/bin/python3
import json, pathlib, sys
args=sys.argv
src=pathlib.Path(args[args.index('--flash')+1])
dst=pathlib.Path(args[args.index('--flash-out')+1])
data=src.read_bytes()
print(json.dumps({'t':'stat','loaded':True}),flush=True)
for line in sys.stdin:
    if line.strip()=='quit':
        dst.write_bytes(data)
        break
'''

class GuiTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        exe = self.work / 'fake-emulator'
        exe.write_text(FAKE_EMU)
        exe.chmod(0o700)
        self.hub = server.Hub()
        self.ready = threading.Event()
        send = self.hub.send
        def publish(ev, key=None):
            send(ev,key)
            if ev['t']=='stat':self.ready.set()
        self.hub.send = publish
        self.emu = server.Emulator(self.hub, str(self.work), str(exe), 1)
        self.image = dict(sha='a'*64, name='test.bin', size=16, flash_size=1048576,
                          profile='generic', elf=None)
        self.bin, self.flash, _ = map(Path, self.emu.paths(self.image['sha']))
        self.bin.write_bytes(b'original firmware')

    def tearDown(self):
        self.emu.stop()
        self.tmp.cleanup()

    def request(self, body=b'{}', path='/api/cmd', method='POST', headers=()):
        H = server.make_handler(self.hub, self.emu, 8276, str(self.work))
        h = object.__new__(H)
        h.path = path
        h.headers = email.message.Message()
        h.headers['Host'] = '127.0.0.1:8276'
        if not any(k.lower()=='content-length' for k, _ in headers):
            h.headers['Content-Length'] = str(len(body))
        for key, val in headers:
            h.headers[key] = val
        h.rfile = io.BytesIO(body)
        response = []
        h._send = lambda status, data, *args: response.append((status, data))
        getattr(h, 'do_'+method)()
        return response[-1]

    def test_firmware_state_tracks_ap_restart_and_debug_address(self):
        fw = server.FirmwareState()
        for line in ('add if1', 'dhcp server start:(ip:192.168.4.1,mask:255.255.255.0,gw:192.168.4.1)',
                     'bcn 100', 'bcn 0', 'del if1'):
            self.assertTrue(fw.line(line))
        self.assertFalse(fw.softap)
        self.assertIsNone(fw.ap_ip)
        self.assertTrue(fw.ap_note)
        for line in ('add if1', 'dhcp server start:(ip:192.168.4.1,mask:255.255.255.0,gw:192.168.4.1)',
                     'bcn 100', 'scandone'):
            self.assertTrue(fw.line(line))
        self.assertTrue(fw.softap)
        self.assertEqual(fw.ap_ip, '192.168.4.1')
        self.assertIsNone(fw.ap_note)
        self.assertEqual(fw.scans, 1)
        fw.reset()
        self.assertTrue(fw.line('AP started, IP 192.168.4.1'))
        self.assertTrue(fw.softap)
        self.assertEqual(fw.ap_ip, '192.168.4.1')
        self.assertIsNone(fw.ap_note)

    def read_uart_events(self, events, gen=None):
        raw = ''.join(json.dumps(e) + '\n' for e in events).encode()
        proc = SimpleNamespace(stdout=io.BytesIO(raw), poll=lambda: 0)
        with patch.object(server.threading, 'Thread'):
            self.emu._reader(proc, self.emu.gen if gen is None else gen)

    @staticmethod
    def uart(text, u=0):
        return {'t': 'uart', 'u': u, 'd': text.encode('latin-1').hex()}

    def test_firmware_state_uses_complete_masked_uart_lines(self):
        text = ('bcn 0\r\ndel if1\r\nadd if1\r\n'
                'AP started, IP 192.168.4.1\r\n'
                'scandone\r\nPASS:test-secret\r\n')
        for cut in range(1, len(text)):
            with self.subTest(cut=cut):
                self.hub.console.clear()
                self.read_uart_events([self.uart(text[:cut]), self.uart(text[cut:])])
                fw = self.hub.snap['fw']
                self.assertTrue(fw['softap'])
                self.assertEqual(fw['ap_ip'], '192.168.4.1')
                self.assertIsNone(fw['ap_note'])
                self.assertEqual(fw['scans'], 1)
                console = ''.join(e['text'] for e in self.hub.console)
                self.assertNotIn('test-secret', console)
                self.assertEqual(console, server.StreamMasker().feed(text, partial=True))

    def test_firmware_uart_buffers_are_per_uart_reset_and_generation(self):
        self.read_uart_events([self.uart('scand'), self.uart('add if', 1),
                               self.uart('one\n'), self.uart('1\n', 1)])
        self.assertEqual(self.hub.snap['fw']['scans'], 1)
        self.assertTrue(self.hub.snap['fw']['softap'])
        self.read_uart_events([self.uart('scand'), {'t': 'stage', 's': 'rom'},
                               self.uart('one\n')])
        self.assertEqual(self.hub.snap['fw']['scans'], 0)
        old = dict(self.hub.snap['fw'])
        self.read_uart_events([self.uart('scandone\n')], gen=self.emu.gen - 1)
        self.assertEqual(self.hub.snap['fw'], old)

    def test_atomic_storage_preserves_old_contents_until_publication(self):
        for suffix in ('.bin', '.elf', '.json'):
            with self.subTest(suffix=suffix):
                dest = self.work / ('stored' + suffix)
                old, new = b'old complete file', b'new complete file' * 8192
                dest.write_bytes(old)
                entered, release = threading.Event(), threading.Event()
                errors = []
                replace = os.replace
                def hold_publication(src, dst):
                    self.assertEqual(Path(src).parent, dest.parent)
                    self.assertEqual(Path(src).read_bytes(), new)
                    entered.set()
                    if not release.wait(2):
                        raise TimeoutError('publication was not released')
                    replace(src, dst)
                def write():
                    try:
                        server.atomic_write(str(dest), new)
                    except BaseException as e:
                        errors.append(e)
                with patch.object(server.os, 'replace', hold_publication):
                    writer = threading.Thread(target=write)
                    writer.start()
                    try:
                        self.assertTrue(entered.wait(2))
                        self.assertEqual(dest.read_bytes(), old)
                    finally:
                        release.set()
                        writer.join(3)
                self.assertFalse(writer.is_alive())
                self.assertEqual(errors, [])
                self.assertEqual(dest.read_bytes(), new)
                self.assertEqual(list(self.work.glob('.stored' + suffix + '.*.tmp')), [])

    def test_atomic_storage_failure_keeps_old_contents_and_removes_temp(self):
        dest = self.work / 'stored.bin'
        dest.write_bytes(b'old complete file')
        with patch.object(server.os, 'replace', side_effect=OSError('publication failed')):
            with self.assertRaises(OSError):
                server.atomic_write(str(dest), b'new complete file')
        self.assertEqual(dest.read_bytes(), b'old complete file')
        self.assertEqual(list(self.work.glob('.stored.bin.*.tmp')), [])

    def test_erase_waits_for_flash_save(self):
        self.flash.write_bytes(b'stored credentials')
        self.emu.start(self.image)
        self.assertTrue(self.ready.wait(2))
        status, _ = self.request(b'{"cmd":"erase"}')
        self.assertEqual(status, 200)
        self.assertFalse(self.hub.snap['image']['flash_persisted'])
        self.emu.stop()
        self.assertEqual(self.flash.read_bytes(), self.bin.read_bytes())

    def test_fresh_upload_waits_for_flash_save(self):
        import hashlib
        data=b'\xe9\x00\x00\x20'+b'\x00'*12
        sha=hashlib.sha256(data).hexdigest()
        bp, fp, _=map(Path,self.emu.paths(sha))
        bp.write_bytes(data);fp.write_bytes(b'stored credentials')
        self.emu.start(dict(self.image,sha=sha))
        self.assertTrue(self.ready.wait(2))
        status,_=self.request(data,'/api/image?fresh=1')
        self.assertEqual(status,200)
        self.assertFalse(self.hub.snap['image']['flash_persisted'])
        self.emu.stop()
        self.assertEqual(fp.read_bytes(),data)

    def test_unrecognized_one_mib_image_defaults_to_generic_gpio(self):
        image = b'\xe9\x00\x00\x20' + b'\x00' * 12
        for name in ('unknown.bin', 'switch.bin', 'plug.bin', 'esp8285.bin'):
            with self.subTest(name=name):
                status, body = self.request(image, '/api/image?name=' + name)
                self.assertEqual(status, 200)
                self.assertEqual(json.loads(body)['profile'], 'generic')
                self.assertEqual(self.hub.snap['profile']['gangs'], [])
                self.assertEqual(self.hub.snap['image']['flash_size'], 1048576)

    def test_custom_profile_changes_display_without_restarting_guest(self):
        profile = validate_profile({'name': 'example', 'kind': 'switch',
                                    'gangs': [{'button': 0, 'relay': 5, 'led': 2,
                                               'button_active_low': False, 'led_active_low': False}]})
        with patch.dict(server.PROFILES, {'example': profile}):
            self.emu.start(self.image)
            self.assertTrue(self.ready.wait(2))
            child = self.emu.proc
            status, _ = self.request(b'{"cmd":"profile","name":"example"}')
            self.assertEqual(status, 200)
            self.assertIs(self.emu.proc, child)
            self.assertIsNone(child.poll())
            self.assertEqual(self.hub.snap['profile'], {'t': 'profile', **profile})
            self.assertEqual(pin_roles(profile), {5: 'relay1', 2: 'led1', 0: 'button1'})

    def test_profile_file_validates_pins_fields_sizes_and_polarities(self):
        path = self.work / 'profile.json'
        valid = {'name': 'example', 'title': 'Example GPIO panel', 'kind': 'switch',
                 'gangs': [{'button': 0, 'relay': 5, 'led': 2}], 'status_led': 16}
        path.write_text(json.dumps(valid))
        profile = server.load_profile(path)
        self.assertEqual(profile['status_led'], 16)
        self.assertTrue(profile['gangs'][0]['button_active_low'])
        invalid = [[], dict(valid, name='x' * 65), dict(valid, name='<bad>'),
                   dict(valid, title='x' * 81), dict(valid, title='bad\nname'),
                   dict(valid, kind='unrecognized'), dict(valid, firmware_action='restart'),
                   dict(valid, gangs=[valid['gangs'][0]] * 9),
                   dict(valid, status_led=True), dict(valid, status_led=17),
                   dict(valid, status_led_active_low=1)]
        for field, value in [('button', -1), ('relay', 17), ('led', True),
                             ('button_active_low', 0), ('led_active_low', 'false')]:
            invalid.append(dict(valid, gangs=[dict(valid['gangs'][0], **{field: value})]))
        for data in invalid:
            with self.subTest(data=data):
                path.write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    server.load_profile(path)
        path.write_bytes(b' ' * (MAX_PROFILE_BYTES + 1))
        with self.assertRaisesRegex(ValueError, '64 KiB'):
            server.load_profile(path)
        self.assertEqual(pin_roles(server.load_profile()), {})

    def test_profile_pin_labels_have_stable_role_precedence(self):
        profile = validate_profile({'name': 'example',
                                    'gangs': [{'button': 0, 'relay': 5, 'led': 5},
                                               {'button': 5, 'relay': 4, 'led': 2}]})
        self.assertEqual(pin_roles(profile), {0: 'button1', 5: 'button2', 4: 'relay2', 2: 'led2'})

    def test_overlapping_starts_leave_only_one_process(self):
        real_popen = subprocess.Popen
        entered, release = threading.Event(), threading.Event()
        children=[]
        errors=[]
        def spawn(*args, **kwargs):
            p=real_popen(*args,**kwargs)
            children.append(p)
            if len(children)==1:
                entered.set()
                release.wait(2)
            return p
        def start():
            try:self.emu.start(self.image)
            except Exception as e:errors.append(e)
        try:
            with patch.object(server.subprocess, 'Popen', spawn):
                a=threading.Thread(target=start);a.start()
                self.assertTrue(entered.wait(2))
                b=threading.Thread(target=start);b.start()
                b.join(.15)
                release.set();a.join(3);b.join(3)
            self.assertFalse(a.is_alive() or b.is_alive())
            self.assertEqual(errors,[])
            self.assertEqual(sum(p.poll() is None for p in children),1)
        finally:
            release.set()
            self.emu.stop()
            for p in children:
                if p.poll() is None:p.kill();p.wait()

    def test_rejected_framing_does_not_read_body(self):
        for headers,status in [([('Content-Length','-1')],400),
                               ([('Content-Length',str(server.MAX_UPLOAD+1))],413),
                               ([('Content-Length','0'),('Content-Length','16')],400),
                               ([('Transfer-Encoding','chunked')],400),
                               ([('Content-Length','letters')],400)]:
            with self.subTest(headers=headers):
                actual,_=self.request(b'{"cmd":"reset"}',headers=headers)
                self.assertEqual(actual,status)

    def test_origin_and_host_guards(self):
        for method in ('GET','POST'):
            for headers in ([('Origin','http://foreign.invalid')],
                            [('Host','foreign.invalid')],
                            [('Origin','http://127.0.0.1:8276'),('Origin','http://foreign.invalid')]):
                with self.subTest(method=method,headers=headers):
                    status,_=self.request(method=method,path='/api/state',headers=headers)
                    self.assertEqual(status,403)

    def test_invalid_commands_return_json_error(self):
        for body in (b'[]', b'{"cmd":"pin"}', b'{"cmd":"pin","pin":{},"level":0}',
                     b'{"cmd":"speed","value":"bad"}'):
            with self.subTest(body=body):
                status,data=self.request(body)
                self.assertEqual(status,400)
                self.assertIn('error',json.loads(data))

    def test_old_reader_cannot_restore_stale_pins_after_restart(self):
        ready, release = threading.Event(), threading.Event()
        class Output:
            first = True
            def readline(self):
                if self.first:
                    self.first = False
                    return b'{"t":"pins","lv":"stale"}\n'
                ready.set()
                release.wait(2)
                return b''
            def close(self):pass
        class Child:
            stdout = Output()
            def poll(self):return 0
        reader = threading.Thread(target=self.emu._reader,args=(Child(),self.emu.gen))
        reader.start()
        self.assertTrue(ready.wait(2))
        self.emu.start(self.image)
        self.hub.send({'t':'pins','lv':'current'},key='pins')
        release.set();reader.join(2)
        self.assertFalse(reader.is_alive())
        self.assertEqual(self.hub.snap['pins']['lv'],'current')

if __name__=='__main__':
    unittest.main()
