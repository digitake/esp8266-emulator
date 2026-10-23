#!/usr/bin/env python3
"""Local GUI for the ESP8266 emulator. Serves http://127.0.0.1:PORT/ on the loopback interface only.

Drop a firmware .bin (optionally with its .elf) on the page and the emulated chip boots it immediately.
The page shows the UART console (secrets masked), the pins (relay/LED/button roles per device profile),
the boot stage and what the firmware's UART says about Wi-Fi, and has button/reset/power controls.
With --net-profile, images that the profile binds run on the experimental virtual network (esp8266emu-net).

stdlib only (python3 >= 3.8). Runs where esp8266emu runs (WSL/Linux).
"""
import argparse, hashlib, http.server, ipaddress, json, math, os, queue, re, signal, socketserver, subprocess
import sys, tempfile, threading, time, urllib.parse

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

OFFLINE = 'unavailable: no host network bridge'
NET_PROFILE_HEADER = 'esp8266-net-v2 arduino-3.1.2-lwip2'
NET_SPEED_ERROR = 'live networking needs a positive speed: the network runner refuses 0 (max)'
NET_MAX = 16  # forwards and DNS overrides the runner accepts


def net_profile_digests(path):
    """SHA-256 of every BIN a network profile binds. The runner halts on any other application image."""
    with open(path, 'rb') as f:
        data = f.read(1 << 20)
    lines = data.decode('utf-8', 'replace').splitlines()
    if not lines or lines[0].rstrip() != NET_PROFILE_HEADER:
        raise ValueError('not a version 2 network profile; make one with scripts/network_profile.py')
    digests = set()
    for line in lines:
        if line.startswith('sha256 '):
            digest = line[7:].strip().lower()
            if not re.fullmatch(r'[0-9a-f]{64}', digest):
                raise ValueError('invalid sha256 line in the network profile')
            digests.add(digest)
    if not digests:
        raise ValueError('the network profile binds no image')
    return digests


def net_forward(text):
    m = re.fullmatch(r'([0-9]{1,5}):([0-9]{1,5})', text)
    if not m or not all(1 <= int(p) <= 65535 for p in m.groups()):
        raise argparse.ArgumentTypeError('expected HOST_PORT:GUEST_PORT, both 1-65535')
    return int(m.group(1)), int(m.group(2))


def net_host(text):
    name, _, ip = text.partition('=')
    labels = name.rstrip('.').split('.')
    if not (0 < len(name.rstrip('.')) <= 253 and
            all(re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?', label) for label in labels)):
        raise argparse.ArgumentTypeError('expected NAME=IPv4 with a valid DNS name')
    try:
        ipaddress.IPv4Address(ip)
    except ValueError:
        raise argparse.ArgumentTypeError('expected NAME=IPv4 with a dotted IPv4 address')
    return name + '=' + ip


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
    """What the firmware's own UART output says (SDK messages are printed by release builds too).

    network is per process, not per boot: a string offline, or the virtual station's badges (dict)."""

    def __init__(self, network=OFFLINE):
        self.network = network
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
                'network': dict(self.network) if isinstance(self.network, dict) else self.network}

    def net_event(self, e):
        """Fold the network runner's periodic net event into the badges. True if anything shown changed."""
        if not isinstance(self.network, dict):
            return False
        net = dict(self.network)
        net['virtual station'] = ('connected' if e.get('connected') else 'not connected' if e.get('bound')
                                  else 'waiting for the application')
        ip = str(e.get('ip') or '')
        net['IP'] = ip if e.get('connected') and ip != '0.0.0.0' else '-'
        packets = 'tx %d · rx %d' % (int(e.get('tx', 0)), int(e.get('rx', 0)))
        lost = int(e.get('tx_rejected', 0)) + int(e.get('rx_dropped', 0))
        net['packets'] = packets + (' · %d dropped' % lost if lost else '')
        changed = net != self.network
        self.network = net
        return changed

    def net_stopped(self, why):
        if isinstance(self.network, dict):
            self.network = dict(self.network, **{'virtual station': why})


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
    def __init__(self, hub, workdir, exe, speed, net=None):
        self.hub, self.workdir, self.exe = hub, workdir, exe
        self.net = net              # None, or dict: exe, profile, forwards [(host, guest)], hosts [NAME=IP], capture
        self.net_live = False       # the running process is the network runner
        self.proc = None
        self.lock = threading.RLock()  # serializes process changes and commands
        self.event_lock = threading.Lock()
        self.image = None           # dict: sha, name, size, flash_size, elf, profile
        self.speed = speed
        self.gen = 0
        self.fw = FirmwareState()

    def network_for(self, image):
        """(live, badge): the network runner only for an image the profile binds by its exact SHA-256."""
        if not self.net:
            return False, OFFLINE
        try:
            digests = net_profile_digests(self.net['profile'])   # read again: the profile may have been regenerated
        except (OSError, ValueError) as e:
            return False, 'offline: network profile unusable (%s)' % e
        if image['sha'] not in digests:
            return False, ('offline: this image (sha256 %s…) is not in the network profile; regenerate the profile '
                           'with scripts/network_profile.py for this BIN and its ELF' % image['sha'][:12])
        badges = {'virtual station': 'starting', 'IP': '-', 'packets': '-'}
        for host, guest in self.net['forwards']:
            key = 'guest :%d' % guest
            badges[key if key not in badges else '%s via %d' % (key, host)] = 'http://127.0.0.1:%d/' % host
        return True, badges

    def net_args(self):
        args = ['--net-profile', self.net['profile']]
        for host, guest in self.net['forwards']:
            args += ['--net-forward', '%d:%d' % (host, guest)]
        for entry in self.net['hosts']:
            args += ['--net-host', entry]
        if self.net.get('capture'):
            args += ['--net-capture', self.net['capture']]
        return args

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
            live, network = self.network_for(image)
            note = None
            if live and not self.speed > 0:
                self.speed, note = 1.0, 'speed set to 1×: ' + NET_SPEED_ERROR
            self.net_live = live
            cmd = [self.net['exe'] if live else self.exe, '--flash', src, '--flash-out', flashp, '--speed', str(self.speed)]
            if live:
                cmd += self.net_args()
            if image.get('elf'):
                try:
                    st, lp = arduino_hooks(image['elf'])
                    if st:
                        cmd += ['--sym-setup', hex(st)]
                    if lp:
                        cmd += ['--sym-loop', hex(lp)]
                except Exception as e:
                    self.hub.send({'t': 'log', 'm': 'ELF not usable: %s' % e})
            self.fw = fw = FirmwareState(network)
            with self.hub.lock:
                self.hub.console.clear()
                self.hub.snap.clear()
            self.hub.send({'t': 'clear'})
            self.hub.send({'t': 'image', **{k: v for k, v in image.items() if k != 'elf'}, 'has_elf': bool(image.get('elf')),
                           'flash_persisted': src == flashp, 'net': live}, key='image')
            self.hub.send({'t': 'profile', 'name': image['profile'], **PROFILES[image['profile']]}, key='profile')
            self.hub.send(fw.snapshot(), key='fw')
            self.hub.send({'t': 'stages', 'list': []}, key='stages')
            if note:
                self.hub.send({'t': 'log', 'm': note})
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
            self.proc = p
            threading.Thread(target=self._reader, args=(p, self.gen, fw), daemon=True).start()
            threading.Thread(target=self._stderr, args=(p, self.gen, fw), daemon=True).start()

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

    def _stderr(self, p, gen, fw=None):
        last = ''
        with p.stderr:
            for line in p.stderr:
                text = line.decode('utf-8', 'replace').rstrip()
                last = text or last
                self._publish(gen, {'t': 'log', 'm': 'emulator: ' + text})
        if fw is not None and isinstance(fw.network, dict) and p.wait() != 0:
            fw.net_stopped('stopped: ' + (last or 'exit status %d' % p.returncode))   # e.g. a rejected profile
            self._publish(gen, fw.snapshot(), key='fw')

    def _reader(self, p, gen, fw=None):
        fw = FirmwareState() if fw is None else fw  # state belongs to this process generation
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
            elif t == 'net':
                if fw.net_event(e):
                    self._publish(gen, fw.snapshot(), key='fw')
            elif t == 'halt':
                self._publish(gen, e, key='halt')
                if isinstance(fw.network, dict):
                    fw.net_stopped('halted')        # the next boot's net event brings it back
                    self._publish(gen, fw.snapshot(), key='fw')
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
                except BodyError as e:
                    self._json(e.status, {'error': str(e)})
                    return
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
                speed = max(0.0, min(float(c['value']), 4.0))
                if emu.net_live and not speed > 0:
                    raise BodyError(409, NET_SPEED_ERROR)
                emu.speed = speed
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


def check_net_options(ap, a):
    """Refuse at launch what the network runner would refuse each time it starts."""
    if not (math.isfinite(a.speed) and a.speed > 0):
        ap.error('--net-profile needs a positive --speed (1 = real time): the network runner refuses 0')
    a.net_profile = os.path.abspath(a.net_profile)
    try:
        net_profile_digests(a.net_profile)
    except (OSError, ValueError) as error:
        ap.error('--net-profile %s: %s' % (a.net_profile, error))
    hosts = [h for h, _ in a.net_forward]
    if len(a.net_forward) > NET_MAX or len(a.net_host) > NET_MAX:
        ap.error('at most %d --net-forward and %d --net-host options' % (NET_MAX, NET_MAX))
    if len(set(hosts)) != len(hosts) or a.port in hosts:
        ap.error('each --net-forward needs its own host port, other than the GUI port %d' % a.port)
    names = [h.split('=')[0].rstrip('.').lower() for h in a.net_host]
    if len(set(names)) != len(names):
        ap.error('duplicate --net-host name')
    if a.net_capture:
        a.net_capture = os.path.abspath(a.net_capture)
        if not os.path.isdir(os.path.dirname(a.net_capture)):
            ap.error('--net-capture: no such directory %s' % os.path.dirname(a.net_capture))


def build_net_runner():
    """make build/esp8266emu-net (a no-op when current), or exit with what to install."""
    exe = os.path.join(CHIP, 'build', 'esp8266emu-net')
    try:
        code = subprocess.run(['make', '-s', '-C', CHIP, 'network']).returncode
    except OSError as error:
        sys.exit('cannot run make to build %s: %s' % (exe, error))
    if code or not os.path.exists(exe):
        sys.exit('--net-profile needs the network runner %s, and building it needs libslirp.\n'
                 'Install it, then start again:  sudo apt install -y libslirp-dev libglib2.0-dev pkg-config' % exe)
    return exe


def main():
    global DEFAULT_PROFILE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--port', type=int, default=8266)
    ap.add_argument('--bin', help='boot this image at start')
    ap.add_argument('--elf', help='its ELF (setup()/loop() counting)')
    ap.add_argument('--profile-file', help='optional JSON GPIO labels and display panel')
    ap.add_argument('--speed', type=float, default=1.0,
                    help='1 = real time (default), 0 = as fast as possible (offline only)')
    ap.add_argument('--workdir', default=os.path.join(os.path.expanduser('~'), '.cache', 'esp8266-emulator'))
    net = ap.add_argument_group('experimental virtual network (Arduino core 3.1.2/lwIP2, needs libslirp)')
    net.add_argument('--net-profile', metavar='FILE',
                     help='run images this profile binds (scripts/network_profile.py) on build/esp8266emu-net')
    net.add_argument('--net-forward', metavar='HOST:GUEST', type=net_forward, action='append', default=[],
                     help='forward 127.0.0.1:HOST to guest port GUEST (repeatable)')
    net.add_argument('--net-host', metavar='NAME=IP', type=net_host, action='append', default=[],
                     help='answer the guest\'s DNS query for NAME with IP, e.g. 10.0.2.2 for this host (repeatable)')
    net.add_argument('--net-capture', metavar='FILE', help='write guest Ethernet frames to a PCAP file')
    a = ap.parse_args()
    if a.net_profile:
        check_net_options(ap, a)
    elif a.net_forward or a.net_host or a.net_capture:
        ap.error('--net-forward, --net-host and --net-capture need --net-profile')
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
    netcfg = None
    if a.net_profile:
        netcfg = {'exe': build_net_runner(), 'profile': a.net_profile, 'forwards': a.net_forward,
                  'hosts': a.net_host, 'capture': a.net_capture}
    os.makedirs(a.workdir, exist_ok=True)
    hub = Hub()
    emu = Emulator(hub, a.workdir, exe, a.speed, netcfg)
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
    if netcfg:
        print('virtual network: images bound by %s run on %s%s' % (
            netcfg['profile'], netcfg['exe'], ''.join('; http://127.0.0.1:%d/ -> guest port %d' % f for f in netcfg['forwards'])),
            flush=True)
    httpd.serve_forever()


if __name__ == '__main__':
    main()
