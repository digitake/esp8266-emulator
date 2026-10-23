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
import time
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

# Stands in for build/esp8266emu-net, which needs libslirp: refuses what the runner refuses at startup,
# then reports one connected station.
FAKE_NET = '''#!/usr/bin/python3
import json, sys
args=sys.argv
speed=float(args[args.index('--speed')+1])
if speed<=0:
    print('live networking requires a positive --speed (use 1)',file=sys.stderr);sys.exit(2)
if 'refuse' in open(args[args.index('--net-profile')+1]).read():
    print('network: network profile BIN SHA256 mismatch',file=sys.stderr);sys.exit(2)
print(json.dumps({'t':'stat','speed':speed}),flush=True)
print(json.dumps({'t':'net','bound':True,'image':1,'connected':True,'ip':'10.0.2.15',
                  'tx':3,'rx':4,'tx_rejected':0,'rx_dropped':0}),flush=True)
for line in sys.stdin:
    if line.strip()=='quit':
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
        self.events = []
        send = self.hub.send
        def publish(ev, key=None):
            send(ev,key)
            self.events.append(ev)
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

    def net_emulator(self, digest=None, extra='', forwards=((8080, 80),), hosts=('broker.example.test=10.0.2.2',)):
        exe = self.work / 'fake-emulator-net'
        exe.write_text(FAKE_NET)
        exe.chmod(0o700)
        profile = self.work / 'network-profile.txt'
        profile.write_text('%s\n# %s\nimage /fw.bin\nsha256 %s\nend\n' % (
            server.NET_PROFILE_HEADER, extra, digest or self.image['sha']))
        net = {'exe': str(exe), 'profile': str(profile), 'forwards': list(forwards), 'hosts': list(hosts),
               'capture': None}
        self.emu = server.Emulator(self.hub, str(self.work), self.emu.exe, 1, net)
        return net

    def wait_for(self, predicate, timeout=3):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate():
                return True
            time.sleep(.02)
        return predicate()

    def station(self):
        net = self.hub.snap.get('fw', {}).get('network')
        return net.get('virtual station') if isinstance(net, dict) else None

    def test_profile_bound_image_runs_on_the_network_runner(self):
        net = self.net_emulator()
        self.emu.start(self.image)
        self.assertTrue(self.wait_for(lambda: self.station() == 'connected'))
        args = self.emu.proc.args
        self.assertEqual(args[0], net['exe'])
        self.assertEqual(args[args.index('--net-profile') + 1], net['profile'])
        self.assertEqual(args[args.index('--net-forward') + 1], '8080:80')
        self.assertEqual(args[args.index('--net-host') + 1], 'broker.example.test=10.0.2.2')
        self.assertNotIn('--net-capture', args)
        self.assertTrue(self.hub.snap['image']['net'])
        self.assertEqual(self.hub.snap['fw']['network'],
                         {'virtual station': 'connected', 'IP': '10.0.2.15', 'packets': 'tx 3 · rx 4',
                          'guest :80': 'http://127.0.0.1:8080/'})

    def test_image_missing_from_profile_runs_offline_with_a_badge(self):
        self.net_emulator(digest='b' * 64)
        self.emu.start(self.image)
        self.assertTrue(self.ready.wait(2))
        args = self.emu.proc.args
        self.assertEqual(args[0], self.emu.exe)
        self.assertFalse([a for a in args if a.startswith('--net')])
        self.assertFalse(self.hub.snap['image']['net'])
        network = self.hub.snap['fw']['network']
        self.assertTrue(network.startswith('offline: this image'), network)
        self.assertIn('not in the network profile', network)
        status, _ = self.request(b'{"cmd":"speed","value":0}')
        self.assertEqual(status, 200)

    def test_unreadable_profile_runs_offline_with_the_reason(self):
        net = self.net_emulator()
        Path(net['profile']).write_text('esp8266-net-v1\n')
        self.emu.start(self.image)
        self.assertTrue(self.ready.wait(2))
        self.assertEqual(self.emu.proc.args[0], self.emu.exe)
        self.assertIn('network profile unusable', self.hub.snap['fw']['network'])

    def test_live_network_refuses_maximum_speed(self):
        self.net_emulator()
        self.emu.start(self.image)
        self.assertTrue(self.ready.wait(2))
        for value in (0, -1, 'NaN'):
            with self.subTest(value=value):
                status, data = self.request(json.dumps({'cmd': 'speed', 'value': value}).encode())
                self.assertEqual(status, 409)
                self.assertEqual(json.loads(data)['error'], server.NET_SPEED_ERROR)
                self.assertEqual(self.emu.speed, 1)
        status, data = self.request(b'{"cmd":"speed","value":0.25}')
        self.assertEqual((status, json.loads(data)), (200, {'ok': True}))
        self.assertEqual(self.emu.speed, 0.25)

    def test_maximum_speed_becomes_real_time_for_a_networked_image(self):
        self.net_emulator()
        self.emu.speed = 0
        self.emu.start(self.image)
        self.assertTrue(self.wait_for(lambda: self.station() == 'connected'))
        args = self.emu.proc.args
        self.assertEqual(args[args.index('--speed') + 1], '1.0')
        self.assertIn('speed set to 1×: ' + server.NET_SPEED_ERROR,
                      [e.get('m') for e in self.events if e['t'] == 'log'])

    def test_failed_network_runner_shows_why_it_stopped(self):
        self.net_emulator(extra='refuse')
        self.emu.start(self.image)
        self.assertTrue(self.wait_for(lambda: (self.station() or '').startswith('stopped')))
        self.assertEqual(self.station(), 'stopped: network: network profile BIN SHA256 mismatch')

    def test_net_events_update_badges_and_halt_marks_the_station(self):
        badges = {'virtual station': 'starting', 'IP': '-', 'packets': '-', 'guest :80': 'http://127.0.0.1:8080/'}
        fw = server.FirmwareState(dict(badges))
        self.assertTrue(fw.net_event({'t': 'net', 'bound': False, 'connected': False, 'ip': '0.0.0.0', 'tx': 0, 'rx': 0}))
        self.assertEqual(fw.network['virtual station'], 'waiting for the application')
        self.assertTrue(fw.net_event({'t': 'net', 'bound': True, 'connected': False, 'ip': '0.0.0.0', 'tx': 1, 'rx': 0}))
        self.assertEqual((fw.network['virtual station'], fw.network['IP']), ('not connected', '-'))
        e = {'t': 'net', 'bound': True, 'connected': True, 'ip': '10.0.2.15', 'tx': 9, 'rx': 7,
             'tx_rejected': 1, 'rx_dropped': 2}
        self.assertTrue(fw.net_event(e))
        self.assertFalse(fw.net_event(e))
        self.assertEqual(fw.network['packets'], 'tx 9 · rx 7 · 3 dropped')
        self.assertEqual(list(fw.network), list(badges))
        fw.reset()                                   # a reboot keeps the process's network badges
        self.assertEqual(fw.snapshot()['network']['IP'], '10.0.2.15')
        offline = server.FirmwareState()
        self.assertFalse(offline.net_event(e))
        self.assertEqual(offline.snapshot()['network'], server.OFFLINE)
        raw = ''.join(json.dumps(x) + '\n' for x in (e, {'t': 'halt', 'm': 'stop', 'pc': '0'})).encode()
        proc = SimpleNamespace(stdout=io.BytesIO(raw), poll=lambda: 0)
        with patch.object(server.threading, 'Thread'):
            self.emu._reader(proc, self.emu.gen, server.FirmwareState(dict(badges)))
        self.assertEqual(self.station(), 'halted')
        self.assertEqual(self.hub.snap['fw']['network']['IP'], '10.0.2.15')

    def auto_emulator(self, local=True, forwards=()):
        """Network on for any upload that passes the profile checks (the default without --net-profile)."""
        exe = self.work / 'fake-emulator-net'
        exe.write_text(FAKE_NET)
        exe.chmod(0o700)
        net = {'exe': str(exe), 'profile': None, 'forwards': list(forwards), 'hosts': [], 'capture': None,
               'local': local, 'gui_port': 8276}
        self.emu = server.Emulator(self.hub, str(self.work), self.emu.exe, 1, net)
        return net

    def upload(self, sha, elf=True):
        """An uploaded image as /api/image stores it: BIN, ELF and metadata in the work directory."""
        image = dict(self.image, sha=sha, name=sha[:4] + '.bin', elf=None)
        binp, _, metap = map(Path, self.emu.paths(sha))
        binp.write_bytes(b'firmware ' + sha.encode())
        if elf:
            image['elf'] = str(self.work / 'images' / (sha[:8] + '.elf'))
            Path(image['elf']).write_bytes(b'\x7fELF ' + sha.encode())
        metap.write_text(json.dumps(image))
        return image

    @staticmethod
    def fake_profile(images, core):
        return server.NET_PROFILE_HEADER + '\n' + ''.join(
            'image %s\nsha256 %s\nend\n' % (b, Path(b).stem) for b, _ in images)

    def test_uploaded_image_with_elf_runs_on_the_local_network(self):
        self.auto_emulator()
        image = self.upload('c' * 64)
        with patch.object(server, 'check_image', return_value={}) as check, \
             patch.object(server, 'build_profile', side_effect=self.fake_profile):
            self.emu.start(image)
            self.assertTrue(self.wait_for(lambda: self.station() == 'connected'))
        args = self.emu.proc.args
        self.assertEqual(args[0], self.emu.net['exe'])
        profile = args[args.index('--net-profile') + 1]
        self.assertEqual(profile, str(self.work / 'network-profile.txt'))
        self.assertEqual(server.net_profile_digests(profile), {'c' * 64})
        self.assertIn('--net-local', args)
        port = int(args[args.index('--net-forward') + 1].split(':')[0])
        self.assertNotEqual(port, 8276)
        self.assertEqual(args[args.index('--net-forward') + 1], '%d:80' % port)
        network = self.hub.snap['fw']['network']
        self.assertEqual(network['guest :80'], 'http://127.0.0.1:%d/' % port)
        self.assertTrue(network['internet'].startswith('off'))
        self.assertEqual(check.call_count, 1)
        self.emu.start(image)                         # the check is cached per BIN and ELF
        self.assertEqual(check.call_count, 1)

    def test_profile_includes_other_uploads_for_ota_between_them(self):
        self.auto_emulator(local=False, forwards=[(9090, 80)])
        old, other, no_elf, broken = (self.upload('d' * 64), self.upload('e' * 64), self.upload('f' * 64, elf=False),
                                      self.upload('9' * 64))
        def check(image, elf, label):
            if label == broken['name']:
                raise ValueError('ELF missing required network symbol')
            return {}
        with patch.object(server, 'check_image', side_effect=check), \
             patch.object(server, 'build_profile', side_effect=self.fake_profile):
            self.emu.start(old)
            self.assertTrue(self.wait_for(lambda: self.station() == 'connected'))
        args = self.emu.proc.args
        self.assertEqual(server.net_profile_digests(args[args.index('--net-profile') + 1]), {'d' * 64, 'e' * 64})
        self.assertNotIn('--net-local', args)
        self.assertEqual(args[args.index('--net-forward') + 1], '9090:80')
        self.assertNotIn('internet', self.hub.snap['fw']['network'])

    def test_upload_without_elf_or_network_symbols_runs_offline_with_why(self):
        self.auto_emulator()
        cases = [(self.upload('a1' * 32, elf=False), None, server.NEED_ELF),
                 (self.upload('b2' * 32), ValueError('ELF missing required network symbol esp2glue_netif_update'),
                  'offline: not an Arduino core 3.1.2/lwIP2 network image (ELF missing required network symbol')]
        for image, error, expected in cases:
            with self.subTest(expected=expected), patch.object(server, 'check_image', side_effect=error):
                self.ready.clear()
                self.emu.start(image)
                self.assertTrue(self.ready.wait(2))
                self.assertEqual(self.emu.proc.args[0], self.emu.exe)
                self.assertFalse([a for a in self.emu.proc.args if a.startswith('--net')])
                self.assertTrue(self.hub.snap['fw']['network'].startswith(expected), self.hub.snap['fw']['network'])

    def test_missing_network_runner_shows_how_to_install_it(self):
        self.emu = server.Emulator(self.hub, str(self.work), self.emu.exe, 1, None, server.NO_RUNNER)
        self.emu.start(self.image)
        self.assertTrue(self.ready.wait(2))
        self.assertEqual(self.hub.snap['fw']['network'], server.NO_RUNNER)
        self.assertIn('libslirp-dev', server.NO_RUNNER)

    def test_free_port_skips_ports_in_use(self):
        import socket
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            s.listen()
            busy = s.getsockname()[1]
            self.assertNotEqual(server.free_port(busy), busy)
            self.assertNotEqual(server.free_port(busy + 1, {busy + 1}), busy + 1)

    def test_network_profile_digests(self):
        path = self.work / 'profile.txt'
        header = server.NET_PROFILE_HEADER + '\n'
        path.write_text(header + 'image /a.bin\nsha256 ' + 'A' * 64 + '\nend\nimage /b.bin\nsha256 ' + 'b' * 64 + '\nend\n')
        self.assertEqual(server.net_profile_digests(path), {'a' * 64, 'b' * 64})
        for text in ('', 'esp8266-net-v1 arduino-3.1.2-lwip2\n', header, header + 'sha256 xyz\n'):
            with self.subTest(text=text):
                path.write_text(text)
                with self.assertRaises(ValueError):
                    server.net_profile_digests(path)

    def test_network_option_values(self):
        self.assertEqual(server.net_forward('8080:80'), (8080, 80))
        self.assertEqual(server.net_host('Broker.example.test=10.0.2.2'), 'Broker.example.test=10.0.2.2')
        for text in ('8080', '0:80', '8080:65536', '8080:80:1', 'a:80'):
            with self.subTest(forward=text), self.assertRaises(server.argparse.ArgumentTypeError):
                server.net_forward(text)
        for text in ('broker', 'broker=', '=10.0.2.2', 'bad_name=10.0.2.2', '-x.test=10.0.2.2', 'x.test=10.0.2',
                     'x.test=::1'):
            with self.subTest(host=text), self.assertRaises(server.argparse.ArgumentTypeError):
                server.net_host(text)
        self.emu.net = {'profile': '/p', 'forwards': [(8080, 80)], 'hosts': ['a.test=10.0.2.2'], 'capture': '/c.pcap'}
        self.assertEqual(self.emu.net_args(), ['--net-profile', '/p', '--net-forward', '8080:80',
                                               '--net-host', 'a.test=10.0.2.2', '--net-capture', '/c.pcap'])

    def test_launcher_rejects_network_options_before_building_anything(self):
        profile = self.work / 'profile.txt'
        profile.write_text(server.NET_PROFILE_HEADER + '\nimage /a.bin\nsha256 ' + 'a' * 64 + '\nend\n')
        bad = self.work / 'old-profile.txt'
        bad.write_text('esp8266-net-v1\n')
        cases = [(['--offline', '--net-forward', '8080:80'], 'cannot be combined'),
                 (['--offline', '--internet'], 'cannot be combined'),
                 (['--net-profile', str(profile), '--speed', '0'], 'positive --speed'),
                 (['--net-profile', str(bad)], 'not a version 2 network profile'),
                 (['--net-profile', str(profile), '--net-forward', '8277:80'], 'other than the GUI port'),
                 (['--net-profile', str(profile), '--net-forward', '8080:80', '--net-forward', '8080:81'],
                  'own host port'),
                 (['--net-profile', str(profile), '--net-host', 'a.test=10.0.2.2', '--net-host', 'A.test=10.0.2.3'],
                  'duplicate --net-host'),
                 (['--net-profile', str(profile), '--net-capture', str(self.work / 'missing' / 'x.pcap')],
                  'no such directory'),
                 (['--net-profile', str(profile), '--net-forward', '80'], 'HOST_PORT:GUEST_PORT')]
        for args, message in cases:
            with self.subTest(args=args):
                r = subprocess.run([sys.executable, '-I', str(CHIP / 'gui/server.py'), '--port', '8277',
                                    '--workdir', str(self.work), *args], capture_output=True, text=True, timeout=20)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn(message, r.stderr)

    def test_page_is_self_contained(self):
        # The page must work offline: no CDN scripts, fonts, stylesheets or remote images.
        page = (CHIP / 'gui/index.html').read_text(encoding='utf-8')
        self.assertNotRegex(page, r'''(src|href)=["']?(https?:)?//''')
        self.assertNotRegex(page, r'''@import|url\(\s*["']?(https?:)?//''')
        self.assertNotIn('<link', page)

if __name__=='__main__':
    unittest.main()
