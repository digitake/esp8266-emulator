#!/usr/bin/env python3
"""Local GUI for the ESP8266 emulator. Serves http://127.0.0.1:PORT/ on the loopback interface only.

Drop a firmware .bin (optionally with its .elf) on the page and the emulated chip boots it immediately.
The page shows the UART console (secrets masked), the pins (relay/LED/button roles per device profile),
the boot stage and what the firmware's UART says about Wi-Fi, and has button/reset/power controls.

stdlib only (python3 >= 3.8). Runs where esp8266emu runs (WSL/Linux).
"""
import argparse, hashlib, http.server, json, os, queue, re, signal, socketserver, subprocess, sys
import tempfile, threading, time, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
CHIP = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(CHIP, 'scripts'))
from mask import StreamMasker        # noqa: E402
from elfsyms import arduino_hooks    # noqa: E402
from profiles import load_profile   # noqa: E402

MAX_UPLOAD = 16 * 1024 * 1024
BACKLOG_LINES = 1500

PROFILES = {'generic': load_profile()}
DEFAULT_PROFILE = 'generic'


def flash_size_from_header(b3):
    return {0: 0x80000, 1: 0x40000, 2: 0x100000, 3: 0x200000, 4: 0x400000, 5: 0x200000, 6: 0x400000,
            8: 0x800000, 9: 0x1000000}.get(b3 >> 4, 0x400000)


def atomic_write(path, data):
    """Publish a complete file without truncating one a running child may read."""
    tmp = None
    try:
        with tempfile.NamedTemporaryFile(dir=os.path.dirname(path), prefix='.' + os.path.basename(path) + '.',
                                         suffix='.tmp', delete=False) as f:
            tmp = f.name
            f.write(data)
        os.replace(tmp, path)
        tmp = None
    finally:
        if tmp is not None:
            os.unlink(tmp)


class FirmwareState:
    """What the firmware's own UART output says (SDK messages are printed by release builds too)."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.softap = None          # True/False/None (unknown)
        self.sta = 'idle'
        self.sta_mac = self.ap_mac = None
        self.ap_ip = None
        self.scans = 0
        self.ap_note = None

    def line(self, text):
        t = text.strip()
        m = re.search(r'mode : (?:sta\(([0-9a-f:]+)\))?(?: \+ )?(?:softAP\(([0-9a-f:]+)\))?', t)
        if m:
            self.sta_mac, self.ap_mac = m.group(1) or self.sta_mac, m.group(2) or self.ap_mac
            return True
        if t == 'add if1' or t.startswith('bcn 100'):
            self.softap = True
            self.ap_note = None
            return True
        if t == 'del if1' or t == 'bcn 0':
            self.ap_note = 'stopped by the SDK'
            self.softap = False
            self.ap_ip = None
            return True
        m = re.search(r'\bAP started, IP ([0-9.]+)', t)
        if m:
            self.softap = True
            self.ap_ip = m.group(1)
            self.ap_note = None
            return True
        m = re.search(r'dhcp server start:\(ip:([0-9.]+)', t)
        if m:
            self.ap_ip = m.group(1)
            return True
        if t == 'scandone':
            self.scans += 1
            self.sta = 'scanned (no access point: the emulator has no radio)'
            return True
        if t.startswith('connected with'):
            self.sta = t
            return True
        if t.startswith('ip:'):
            self.sta = 'got IP ' + t
            return True
        if t.startswith('state:') or t.startswith('reconnect') or t.startswith('no ') and 'found' in t:
            self.sta = t
            return True
        return False

    def snapshot(self):
        return {'t': 'fw', 'softap': self.softap, 'ap_note': self.ap_note, 'sta': self.sta, 'sta_mac': self.sta_mac, 'ap_mac': self.ap_mac,
                'ap_ip': self.ap_ip, 'scans': self.scans,
                'network': 'unavailable: no host network bridge'}


class Hub:
    """Fan-out of events to the connected pages, plus the snapshot a newly opened page needs."""

    def __init__(self):
        self.lock = threading.Lock()
        self.clients = []
        self.console = []
        self.snap = {}

    def add(self):
        q = queue.Queue(maxsize=4000)
        with self.lock:
            self.clients.append(q)
            snap = [self.snap[k] for k in ('image', 'profile', 'stat', 'pins', 'fw', 'stages', 'halt') if k in self.snap]
            backlog = list(self.console)
        for e in snap:
            q.put(e)
        if backlog:
            q.put({'t': 'backlog', 'lines': backlog})
        return q

    def remove(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def send(self, ev, key=None):
        with self.lock:
            if key:
                self.snap[key] = ev
            if ev['t'] == 'console':
                self.console.append(ev)
                del self.console[:-BACKLOG_LINES]
            for q in self.clients:
                try:
                    q.put_nowait(ev)
                except queue.Full:
                    pass

    def clear_console(self):
        with self.lock:
            self.console.clear()


class Emulator:
    def __init__(self, hub, workdir, exe, speed):
        self.hub, self.workdir, self.exe = hub, workdir, exe
        self.proc = None
        self.lock = threading.RLock()  # serializes process changes and commands
        self.event_lock = threading.Lock()
        self.image = None           # dict: sha, name, size, flash_size, elf, profile
        self.speed = speed
        self.gen = 0
        self.fw = FirmwareState()

    def paths(self, sha):
        d = os.path.join(self.workdir, 'images')
        os.makedirs(d, exist_ok=True)
        return os.path.join(d, sha + '.bin'), os.path.join(d, sha + '.flash'), os.path.join(d, sha + '.json')

    def stop(self):
        with self.lock:
            with self.event_lock:
                self.gen += 1
                p, self.proc = self.proc, None
            if p:
                if p.poll() is None:
                    try:
                        p.stdin.write(b'quit\n'); p.stdin.flush()
                        p.wait(timeout=2)
                    except (OSError, subprocess.TimeoutExpired):
                        p.kill()
                        p.wait()
                if p.stdin:
                    p.stdin.close()

    def start(self, image, fresh=False):
        with self.lock:
            self.stop()
            self.image = image
            binp, flashp, _ = self.paths(image['sha'])
            # The outgoing process saves flash on exit. Erase only after it has exited.
            if fresh and os.path.exists(flashp):
                os.remove(flashp)
            src = flashp if os.path.exists(flashp) else binp
            cmd = [self.exe, '--flash', src, '--flash-out', flashp, '--speed', str(self.speed)]
            if image.get('elf'):
                try:
                    st, lp = arduino_hooks(image['elf'])
                    if st:
                        cmd += ['--sym-setup', hex(st)]
                    if lp:
                        cmd += ['--sym-loop', hex(lp)]
                except Exception as e:
                    self.hub.send({'t': 'log', 'm': 'ELF not usable: %s' % e})
            self.fw = FirmwareState()
            with self.hub.lock:
                self.hub.console.clear()
                self.hub.snap.clear()
            self.hub.send({'t': 'clear'})
            self.hub.send({'t': 'image', **{k: v for k, v in image.items() if k != 'elf'}, 'has_elf': bool(image.get('elf')),
                           'flash_persisted': src == flashp}, key='image')
            self.hub.send({'t': 'profile', 'name': image['profile'], **PROFILES[image['profile']]}, key='profile')
            self.hub.send(self.fw.snapshot(), key='fw')
            self.hub.send({'t': 'stages', 'list': []}, key='stages')
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            self.proc = p
            threading.Thread(target=self._reader, args=(p, self.gen), daemon=True).start()
            threading.Thread(target=self._stderr, args=(p, self.gen), daemon=True).start()

    def send(self, line):
        with self.lock:
            p = self.proc
            if p and p.poll() is None:
                try:
                    p.stdin.write((line + '\n').encode()); p.stdin.flush()
                    return True
                except OSError:
                    pass
        return False

    def _publish(self, gen, ev, key=None):
        with self.event_lock:
            if gen == self.gen:
                self.hub.send(ev, key=key)

    def _stderr(self, p, gen):
        with p.stderr:
            for line in p.stderr:
                self._publish(gen, {'t': 'log', 'm': 'emulator: ' + line.decode('utf-8', 'replace').rstrip()})

    def _reader(self, p, gen):
        fw = FirmwareState()  # state belongs to this process generation
        maskers = [StreamMasker(), StreamMasker()]
        fw_lines = ['', '']  # console chunks may end in the middle of an SDK message
        wire = [1, 1]
        pins_lock = threading.Lock()
        pins_box = {'ev': None}

        def pins_flusher():          # pin snapshots coalesced to at most one per 20 ms, never held back longer
            while gen == self.gen and p.poll() is None:
                time.sleep(0.02)
                with pins_lock:
                    ev, pins_box['ev'] = pins_box['ev'], None
                if ev:
                    self._publish(gen, ev, key='pins')
        threading.Thread(target=pins_flusher, daemon=True).start()
        last_uart = [0.0, 0.0]
        edge_counts = [0] * 17
        stages = []
        while True:
            line = p.stdout.readline()
            if not line:
                break
            if gen != self.gen:
                continue  # drain the old child while stop() waits for its flash save
            try:
                e = json.loads(line)
            except ValueError:
                continue
            t = e.get('t')
            if t == 'uart':
                u = e['u'] & 1
                wire[u] = e.get('w', 1)
                text = bytes.fromhex(e['d']).decode('latin-1')
                out = maskers[u].feed(text, partial=True)
                if out:
                    for piece in re.split(r'(?<=\n)', out):
                        if piece:
                            self._publish(gen, {'t': 'console', 'u': u, 'w': wire[u], 'text': piece})
                            fw_lines[u] += piece
                            if piece.endswith('\n'):
                                if fw.line(fw_lines[u]):
                                    self._publish(gen, fw.snapshot(), key='fw')
                                fw_lines[u] = ''
            elif t == 'pins':
                with pins_lock:
                    pins_box['ev'] = e
            elif t == 'edges':
                for _, pin, _, _ in e['e']:
                    edge_counts[pin] += 1
                self._publish(gen, {'t': 'edges', 'e': e['e'], 'dropped': e.get('dropped', 0), 'counts': edge_counts})
            elif t == 'stage':
                stages.append(e)
                if e['s'] == 'rom':
                    fw.reset()
                    fw_lines = ['', '']
                    self._publish(gen, fw.snapshot(), key='fw')
                self._publish(gen, {'t': 'stages', 'list': stages[-40:]}, key='stages')
            elif t == 'stat':
                self._publish(gen, e, key='stat')
            elif t == 'halt':
                self._publish(gen, e, key='halt')
            elif t == 'log':
                if 'romfunc' not in e.get('m', ''):
                    self._publish(gen, e)
        with pins_lock:
            if pins_box['ev']:
                self._publish(gen, pins_box['ev'], key='pins')
        if gen == self.gen:
            self._publish(gen, {'t': 'stopped'})

        p.stdout.close()

class BodyError(ValueError):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def make_handler(hub, emu, port, workdir):
    page = os.path.join(HERE, 'index.html')
    allowed_hosts = {'127.0.0.1:%d' % port, 'localhost:%d' % port}
    allowed_origins = {'http://' + h for h in allowed_hosts}

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        server_version = 'esp8266emu-gui'

        def log_message(self, fmt, *args):
            pass

        def _check(self, post=False):
            if len(self.headers.get_all('Host', [])) != 1 or self.headers.get('Host', '') not in allowed_hosts:          # DNS rebinding guard
                self._send(403, b'forbidden host\n', 'text/plain')
                return False
            origin = self.headers.get('Origin')
            if len(self.headers.get_all('Origin', [])) > 1 or origin is not None and origin not in allowed_origins:
                self._send(403, b'forbidden origin\n', 'text/plain')
                return False
            return True

        def _send(self, code, body, ctype='application/json'):
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            if code >= 400:
                self.close_connection = True  # unread rejected body must not become another request
                self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)

        def _json(self, code, obj):
            self._send(code, (json.dumps(obj) + '\n').encode())

        def do_GET(self):
            if not self._check():
                return
            path = urllib.parse.urlparse(self.path).path
            if path in ('/', '/index.html'):
                with open(page, 'rb') as f:
                    self._send(200, f.read(), 'text/html; charset=utf-8')
            elif path == '/events':
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Connection', 'keep-alive')
                self.end_headers()
                q = hub.add()
                try:
                    while True:
                        try:
                            ev = q.get(timeout=15)
                            data = 'data: ' + json.dumps(ev) + '\n\n'
                        except queue.Empty:
                            data = ': keep-alive\n\n'
                        self.wfile.write(data.encode())
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, OSError):
                    pass
                finally:
                    hub.remove(q)
                    self.close_connection = True
            elif path == '/api/state':
                self._json(200, {'image': hub.snap.get('image'), 'profiles': list(PROFILES)})
            else:
                self._send(404, b'not found\n', 'text/plain')

        def _body(self):
            lengths = self.headers.get_all('Content-Length', [])
            if self.headers.get('Transfer-Encoding') is not None:
                raise BodyError(400, 'Transfer-Encoding is not supported')
            if len(lengths) != 1 or not re.fullmatch(r'[0-9]+', lengths[0]):
                raise BodyError(400, 'one nonnegative Content-Length is required')
            try:
                n = int(lengths[0])
            except ValueError:
                raise BodyError(400, 'invalid Content-Length')
            if n > MAX_UPLOAD:
                raise BodyError(413, 'upload larger than %d bytes' % MAX_UPLOAD)
            body = self.rfile.read(n)
            if len(body) != n:
                raise BodyError(400, 'incomplete request body')
            return body

        def do_POST(self):
            if not self._check(post=True):
                return
            u = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(u.query)
            try:
                body = self._body()
            except BodyError as e:
                self._json(e.status, {'error': str(e)})
                return
            if u.path == '/api/elf':
                if body[:4] != b'\x7fELF':
                    self._json(400, {'error': 'not an ELF file'})
                    return
                sha = hashlib.sha256(body).hexdigest()
                d = os.path.join(workdir, 'images')
                os.makedirs(d, exist_ok=True)
                atomic_write(os.path.join(d, sha + '.elf'), body)
                self._json(200, {'elf': sha})
            elif u.path == '/api/image':
                if len(body) < 16 or body[0] != 0xE9:
                    self._json(400, {'error': 'not an ESP8266 flash image (first byte must be 0xE9): drop the '
                                              'firmware.bin that goes to flash address 0x0'})
                    return
                name = os.path.basename(qs.get('name', ['firmware.bin'])[0])[:80]
                sha = hashlib.sha256(body).hexdigest()
                binp, flashp, metap = emu.paths(sha)
                atomic_write(binp, body)
                fsize = flash_size_from_header(body[3])
                prof = qs.get('profile', [''])[0]
                if prof not in PROFILES:
                    prof = DEFAULT_PROFILE
                elf_sha = qs.get('elf', [''])[0]
                elf = os.path.join(workdir, 'images', elf_sha + '.elf') if re.fullmatch(r'[0-9a-f]{64}', elf_sha or '') else None
                if elf and not os.path.exists(elf):
                    elf = None
                image = {'sha': sha, 'name': name, 'size': len(body), 'flash_size': fsize, 'profile': prof, 'elf': elf}
                atomic_write(metap, json.dumps(image).encode('utf-8'))
                emu.start(image, fresh=qs.get('fresh', ['0'])[0] == '1')
                self._json(200, {'ok': True, 'sha': sha, 'profile': prof})
            elif u.path == '/api/cmd':
                try:
                    c = json.loads(body or b'{}')
                except ValueError:
                    self._json(400, {'error': 'bad json'})
                    return
                try:
                    ok = run_cmd(c)
                except (AttributeError, KeyError, TypeError, ValueError, OverflowError):
                    self._json(400, {'error': 'invalid command fields'})
                    return
                self._json(200, {'ok': ok})
            else:
                self._json(404, {'error': 'not found'})

    def run_cmd(c):
        cmd = c.get('cmd')
        if cmd == 'pin':
            pin, lv = int(c['pin']), int(c['level'])
            if 0 <= pin <= 16 and lv in (-1, 0, 1):
                return emu.send('in %d %d' % (pin, lv))
        elif cmd in ('reset', 'power'):
            return emu.send(cmd)
        elif cmd == 'speed':
            with emu.lock:
                emu.speed = max(0.0, min(float(c['value']), 4.0))
                return emu.send('speed %g' % emu.speed)
        elif cmd == 'rx':
            data = str(c.get('text', ''))[:512].encode('latin-1', 'replace')
            return emu.send('rx 0 %s' % data.hex()) if data else False
        elif cmd == 'profile':
            with emu.lock:
                name = c.get('name')
                if not emu.image or name not in PROFILES:
                    return False
                emu.image = dict(emu.image, profile=name)
                hub.send({'t': 'profile', **PROFILES[name]}, key='profile')
                return True
        elif cmd in ('erase', 'restart'):
            with emu.lock:
                if not emu.image:
                    return False
                img = dict(emu.image)
                emu.start(img, fresh=cmd == 'erase')
                return True
        return False

    return H


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global DEFAULT_PROFILE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--port', type=int, default=8266)
    ap.add_argument('--bin', help='boot this image at start')
    ap.add_argument('--elf', help='its ELF (setup()/loop() counting)')
    ap.add_argument('--profile-file', help='optional JSON GPIO labels and display panel')
    ap.add_argument('--speed', type=float, default=1.0, help='1 = real time (default), 0 = as fast as possible')
    ap.add_argument('--workdir', default=os.path.join(os.path.expanduser('~'), '.cache', 'esp8266-emulator'))
    a = ap.parse_args()
    if a.profile_file:
        try:
            profile = load_profile(a.profile_file)
        except (OSError, ValueError) as error:
            ap.error(str(error))
        if profile['name'] == 'generic':
            ap.error('custom profile name must differ from generic')
        PROFILES[profile['name']] = profile
        DEFAULT_PROFILE = profile['name']

    exe = os.path.join(CHIP, 'build', 'esp8266emu')
    if not os.path.exists(exe):
        sys.exit('build the emulator first: make -C %s' % CHIP)
    os.makedirs(a.workdir, exist_ok=True)
    hub = Hub()
    emu = Emulator(hub, a.workdir, exe, a.speed)
    httpd = Server(('127.0.0.1', a.port), make_handler(hub, emu, a.port, a.workdir))

    def bye(*_):
        emu.stop()
        os._exit(0)
    signal.signal(signal.SIGINT, bye)
    signal.signal(signal.SIGTERM, bye)

    if a.bin:
        body = open(a.bin, 'rb').read()
        sha = hashlib.sha256(body).hexdigest()
        binp, _, _ = emu.paths(sha)
        atomic_write(binp, body)
        elf = None
        if a.elf:
            elf_body = open(a.elf, 'rb').read()
            elf = os.path.join(a.workdir, 'images', hashlib.sha256(elf_body).hexdigest() + '.elf')
            atomic_write(elf, elf_body)
        fsize = flash_size_from_header(body[3])
        emu.start({'sha': sha, 'name': os.path.basename(a.bin), 'size': len(body), 'flash_size': fsize,
                   'profile': DEFAULT_PROFILE, 'elf': elf})
    print('ESP8266 emulator GUI: http://127.0.0.1:%d/  (loopback only; Ctrl+C to stop)' % a.port, flush=True)
    httpd.serve_forever()


if __name__ == '__main__':
    main()
