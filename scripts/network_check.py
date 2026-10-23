"""Run the bundled MQTT-triggered or web-upload ESP8266 OTA fixture through libslirp."""
import argparse
import hashlib
import gzip
import json
import math
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from elfsyms import arduino_hooks
from mask import mask_text
from network_profile import build_profile

MQTT_PORT = 1883
VERSION_TOPIC = b'emulator/version'
OTA_TOPIC = b'emulator/ota'
MAX_MQTT_PACKET = 65536


class ProtocolError(ValueError):
    pass


def encode_remaining_length(length):
    if length < 0 or length > 268435455:
        raise ProtocolError('MQTT remaining length is outside protocol bounds')
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        if length:
            byte |= 0x80
        out.append(byte)
        if not length:
            return bytes(out)


def encode_packet(header, payload=b''):
    return bytes((header,)) + encode_remaining_length(len(payload)) + payload


class MqttStreamParser:
    """Incremental MQTT frame parser; TCP recv boundaries need not match packets."""
    def __init__(self, max_packet=MAX_MQTT_PACKET):
        self.buffer = bytearray()
        self.max_packet = max_packet

    def feed(self, data):
        self.buffer.extend(data)
        packets = []
        while len(self.buffer) >= 2:
            remaining = 0
            multiplier = 1
            end = 1
            complete_length = False
            for _ in range(4):
                if end >= len(self.buffer):
                    return packets
                byte = self.buffer[end]
                remaining += (byte & 0x7f) * multiplier
                end += 1
                if not byte & 0x80:
                    complete_length = True
                    break
                multiplier *= 128
            if not complete_length:
                raise ProtocolError('MQTT remaining length uses more than four bytes')
            total = end + remaining
            if remaining > self.max_packet or total > self.max_packet + 5:
                raise ProtocolError('MQTT packet exceeds configured size limit')
            if len(self.buffer) < total:
                return packets
            packets.append((self.buffer[0], bytes(self.buffer[end:total])))
            del self.buffer[:total]
        return packets


def mqtt_string(payload, offset):
    if offset + 2 > len(payload):
        raise ProtocolError('truncated MQTT string length')
    size = (payload[offset] << 8) | payload[offset + 1]
    end = offset + 2 + size
    if end > len(payload):
        raise ProtocolError('truncated MQTT string')
    return payload[offset + 2:end], end


def decode_publish(payload):
    topic, offset = mqtt_string(payload, 0)
    return topic, payload[offset:]


def parse_subscribe(payload):
    if len(payload) < 2:
        raise ProtocolError('truncated MQTT SUBSCRIBE packet id')
    packet_id = payload[:2]
    topics = []
    offset = 2
    while offset < len(payload):
        topic, offset = mqtt_string(payload, offset)
        if offset >= len(payload):
            raise ProtocolError('truncated MQTT subscription QoS')
        qos = payload[offset]
        offset += 1
        if qos not in (0, 1, 2):
            raise ProtocolError('invalid MQTT subscription QoS')
        topics.append((topic, qos))
    if not topics:
        raise ProtocolError('empty MQTT SUBSCRIBE packet')
    return packet_id, topics


def make_publish(topic, value):
    payload = struct.pack('>H', len(topic)) + topic + value
    return encode_packet(0x30, payload)


def build_checks(events, report, broker, http, uart, expected_image, emulator_finished_at,
                 method='mqtt', web_upload=None, source_mode=None,
                 expected_flash_mode=None, expected_mode_normalized=None):
    stages = [event for event in events if event.get('t') == 'stage']
    rom_boots = [event.get('boot') for event in stages if event.get('s') == 'rom']
    boot2_loop = any(event.get('s') == 'loop' and event.get('boot') == 2 for event in stages)
    banners = uart.split(' ets Jan  8 2013')
    boot1_uart = banners[1] if len(banners) == 3 else ''
    boot2_uart = banners[2] if len(banners) == 3 else ''
    flash_ok = expected_image and expected_image[0] == 0xe9 and http.get('flash_prefix') == expected_image
    checks = {
        'emulator_exited_cleanly': report.get('end') == 'quit' and not report.get('halt'),
        'exactly_two_rom_boots': rom_boots == [1, 2] and report.get('boots') == 2,
        'no_later_boot': not any(isinstance(boot, int) and boot > 2 for boot in rom_boots)
                         and len(banners) == 3,
        'boot2_reached_application_loop': boot2_loop,
        'boot1_wifi_mode_off_disconnected': 'network lifecycle mode-off disconnected' in boot1_uart,
        'boot1_wifi_station_reacquired_dhcp': 'network lifecycle station dhcp restored' in boot1_uart,
        'boot2_wifi_mode_off_disconnected': 'network lifecycle mode-off disconnected' in boot2_uart,
        'boot2_wifi_station_reacquired_dhcp': 'network lifecycle station dhcp restored' in boot2_uart,
        'boot1_reported_configuration_marker': 'configuration marker persisted-v1' in boot1_uart,
        'boot2_printed_version_b': 'network reference firmware B' in boot2_uart,
        'boot2_reported_configuration_marker': 'configuration marker persisted-v1' in boot2_uart,
        'mqtt_version_a_observed': 'A' in broker.get('versions', []),
        'mqtt_version_b_observed_after_reset': 'B' in broker.get('versions', []),
        'configuration_marker_survived_ota': any(
            entry.get('marker') == 'persisted-v1' and any(
                version.get('version') == 'B' and version.get('connection') == entry.get('connection')
                for version in broker.get('version_connections', []))
            for entry in broker.get('config_connections', [])),
        'configuration_marker_reported_on_a_connection': any(
            entry.get('marker') == 'persisted-v1' and any(
                version.get('version') == 'A' and version.get('connection') == entry.get('connection')
                for version in broker.get('version_connections', []))
            for entry in broker.get('config_connections', [])),
        'version_b_stable_before_emulator_exit': (
            broker.get('version_b_seen_at') is not None and emulator_finished_at is not None and
            emulator_finished_at - broker['version_b_seen_at'] >= 2.0),
        'version_b_used_new_mqtt_connection': any(
            entry.get('version') == 'B' and any(previous.get('version') == 'A' and
            previous.get('connection') < entry.get('connection') for previous in broker.get('version_connections', []))
            for entry in broker.get('version_connections', [])),
        'ota_subscription_observed': broker.get('subscribed', False),
        'ota_subscription_and_version_a_shared_connection': any(
            entry.get('version') == 'A' and entry.get('connection') in broker.get('subscription_connections', [])
            for entry in broker.get('version_connections', [])),
        'mqtt_protocol_clean': not broker.get('errors'),
        'flash_contains_new_image': bool(flash_ok),
    }
    if method == 'mqtt':
        checks.update({
            'ota_publish_sent_after_version_a': broker.get('ota_sent', False),
            'one_http_update_fetched': http.get('requests') == 1 and http.get('status') == 200,
        })
    else:
        checks.update({
            'no_mqtt_ota_publish': not broker.get('ota_sent', False),
            'no_http_pull': http.get('requests') == 0,
            'web_upload_succeeded': bool(web_upload and web_upload.get('sent') and
                                          web_upload.get('status') == 200 and
                                          b'Update Success!' in web_upload.get('response', b'')),
        })
    if source_mode is not None:
        network = report.get('network', {})
        checks.update({
            'replacement_profile_image_bound': network.get('profile_bound') is True and
                                               network.get('image_index') == 2,
            'flash_mode_metadata_matches_image': network.get('image_mode') == source_mode and
                                                 network.get('flash_mode') == expected_flash_mode and
                                                 network.get('flash_mode_normalized') is expected_mode_normalized,
        })
    return checks


def expected_flash_image(running_image, replacement_image, gzip_transfer):
    if len(running_image) < 3 or len(replacement_image) < 3:
        raise ValueError('ESP8266 update images are too short to contain a flash mode')
    source_mode = replacement_image[2]
    expected_mode = running_image[2] if not gzip_transfer else source_mode
    normalized = not gzip_transfer and source_mode != expected_mode
    expected = bytearray(replacement_image)
    if normalized:
        expected[2] = expected_mode
    return bytes(expected), source_mode, expected_mode, normalized


def make_multipart_upload(image, filename='reference.bin'):
    boundary = '----------------esp8266-emulator-ota-boundary'
    payload = (('--' + boundary + '\r\n'
                'Content-Disposition: form-data; name="firmware"; filename="' + filename + '"\r\n'
                'Content-Type: application/octet-stream\r\n\r\n').encode('ascii') + image +
               ('\r\n--' + boundary + '--\r\n').encode('ascii'))
    headers = (b'POST /update HTTP/1.1\r\nHost: 127.0.0.1\r\n'
               b'Content-Type: multipart/form-data; boundary=' + boundary.encode('ascii') + b'\r\n'
               b'Content-Length: ' + str(len(payload)).encode('ascii') +
               b'\r\nConnection: close\r\n\r\n')
    return headers + payload


def reserve_host_port():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]
    finally:
        listener.close()


class WebUpdateUploader:
    """POST a real multipart firmware upload through libslirp's host forward."""
    def __init__(self, port, image, ready_event, timeout, compress=False):
        self.port = port
        self.image = gzip.compress(image, compresslevel=9, mtime=0) if compress else image
        self.request = make_multipart_upload(
            self.image, 'reference.bin.gz' if compress else 'reference.bin')
        self.ready_event = ready_event
        self.timeout = timeout
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, name='ota-web-upload', daemon=True)
        self.status = None
        self.response = b''
        self.error = None
        self.attempts = 0
        self.sent = False

    def start(self):
        self.thread.start()

    def _run(self):
        deadline = time.monotonic() + self.timeout
        while not self.stop_event.is_set() and not self.ready_event.is_set():
            if time.monotonic() >= deadline:
                self.error = 'firmware A did not publish readiness before upload deadline'
                return
            self.ready_event.wait(0.2)
        while not self.stop_event.is_set() and time.monotonic() < deadline:
            self.attempts += 1
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=2) as conn:
                    conn.settimeout(15)
                    conn.sendall(self.request)
                    self.sent = True
                    chunks = []
                    while True:
                        try:
                            block = conn.recv(4096)
                        except socket.timeout:
                            break
                        if not block:
                            break
                        chunks.append(block)
                    self.response = b''.join(chunks)
                    first = self.response.split(b'\r\n', 1)[0].split()
                    self.status = int(first[1]) if len(first) >= 2 else None
                    if self.status is None:
                        self.error = 'web updater returned a malformed HTTP response'
                    return
            except (ConnectionError, OSError, socket.timeout) as error:
                if self.sent:
                    self.error = str(error)
                    return
                self.error = str(error)
                self.stop_event.wait(0.2)
        if not self.stop_event.is_set() and not self.sent:
            self.error = self.error or 'web updater did not accept an HTTP upload before deadline'

    def snapshot(self):
        return {'status': self.status, 'response': self.response, 'error': self.error,
                'attempts': self.attempts, 'sent': self.sent, 'body_bytes': len(self.image)}

    def close(self):
        self.stop_event.set()
        if self.thread.ident is not None:
            self.thread.join(timeout=2)


class HttpUpdateServer:
    def __init__(self, image, compress):
        self.image = gzip.compress(image, compresslevel=9, mtime=0) if compress else image
        self.expected_image = image
        self.requests = 0
        self.status = 0
        self.error = None
        self.response_sent = threading.Event()
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(8)
        self.listener.settimeout(0.25)
        self.port = self.listener.getsockname()[1]
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._serve, name='ota-http', daemon=True)

    def start(self):
        self.thread.start()

    def _serve(self):
        while not self.stop_event.is_set():
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                conn.settimeout(5)
                request = bytearray()
                try:
                    while b'\r\n\r\n' not in request and len(request) <= 65536:
                        chunk = conn.recv(4096)
                        if not chunk:
                            break
                        request.extend(chunk)
                    first = bytes(request).split(b'\r\n', 1)[0].split()
                    self.requests += 1
                    if len(first) != 3 or first[0] != b'GET' or first[1] != b'/update.bin':
                        self.status = 404
                        body = b'not found\n'
                        response = (b'HTTP/1.0 404 Not Found\r\nContent-Length: ' +
                                    str(len(body)).encode() + b'\r\nConnection: close\r\n\r\n' + body)
                    else:
                        self.status = 200
                        digest = hashlib.md5(self.image).hexdigest().encode('ascii')
                        response = (b'HTTP/1.0 200 OK\r\nContent-Length: ' +
                                    str(len(self.image)).encode() + b'\r\nX-MD5: ' + digest +
                                    b'\r\nConnection: close\r\n\r\n' + self.image)
                    conn.sendall(response)
                    self.response_sent.set()
                except (OSError, socket.timeout) as error:
                    self.error = str(error)

    def close(self):
        self.stop_event.set()
        self.listener.close()
        if self.thread.ident is not None:
            self.thread.join(timeout=2)


class MqttBroker:
    def __init__(self, update_url, send_update=True):
        self.update_url = update_url.encode('ascii')
        self.send_update = send_update
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', MQTT_PORT))
        self.listener.listen(8)
        self.listener.settimeout(0.25)
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._accept, name='mqtt-broker', daemon=True)
        self.clients = []
        self.client_threads = []
        self.lock = threading.Lock()
        self.versions = []
        self.connections = 0
        self.version_connections = []
        self.subscription_connections = []
        self.subscribed = False
        self.ota_sent = False
        self.errors = []
        self.version_b_seen = threading.Event()
        self.version_b_seen_at = None
        self.version_a_seen = threading.Event()
        self.config_markers = []
        self.config_connections = []

    def start(self):
        self.thread.start()

    def _accept(self):
        while not self.stop_event.is_set():
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            conn.settimeout(0.5)
            with self.lock:
                self.connections += 1
                connection_id = self.connections
                self.version_connections.append({'connection': connection_id, 'version': None})
                self.clients.append(conn)
            worker = threading.Thread(target=self._client, args=(conn, connection_id), daemon=True)
            self.client_threads.append(worker)
            worker.start()

    def _client(self, conn, connection_id):
        parser = MqttStreamParser()
        connected = False
        subscribed = False
        try:
            while not self.stop_event.is_set():
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    return
                for header, payload in parser.feed(chunk):
                    if not connected:
                        if header != 0x10:
                            raise ProtocolError('expected MQTT 3.1.1 CONNECT')
                        protocol, pos = mqtt_string(payload, 0)
                        if protocol != b'MQTT' or len(payload) < pos + 4 or payload[pos] != 4:
                            raise ProtocolError('client did not request MQTT 3.1.1')
                        connected = True
                        conn.sendall(b'\x20\x02\x00\x00')
                    elif header == 0x82:
                        packet_id, subscriptions = parse_subscribe(payload)
                        if (OTA_TOPIC, 0) not in subscriptions:
                            raise ProtocolError('client did not subscribe to emulator/ota at QoS 0')
                        subscribed = True
                        with self.lock:
                            self.subscribed = True
                            self.subscription_connections.append(connection_id)
                        conn.sendall(b'\x90\x03' + packet_id + b'\x00')
                    elif header == 0x30:
                        topic, value = decode_publish(payload)
                        if topic == VERSION_TOPIC:
                            version = value.decode('ascii', 'replace')
                            with self.lock:
                                self.versions.append(version)
                                self.version_connections.append({'connection': connection_id, 'version': version})
                                should_send = (self.send_update and version == 'A' and subscribed and
                                               not self.ota_sent)
                                if version == 'B' and self.version_b_seen_at is None:
                                    self.version_b_seen_at = time.monotonic()
                            if version == 'A':
                                self.version_a_seen.set()
                            if version == 'B':
                                self.version_b_seen.set()
                            if should_send:
                                conn.sendall(make_publish(OTA_TOPIC, self.update_url))
                                with self.lock:
                                    self.ota_sent = True
                        elif topic == b'emulator/config':
                            marker = value.decode('ascii', 'replace')
                            with self.lock:
                                self.config_markers.append(marker)
                                self.config_connections.append({'connection': connection_id,
                                                                'marker': marker})
                    elif header == 0xc0 and not payload:
                        conn.sendall(b'\xd0\x00')
                    elif header == 0xe0 and not payload:
                        return
                    else:
                        raise ProtocolError('unsupported MQTT client packet 0x%02x' % header)
        except ProtocolError as error:
            with self.lock:
                self.errors.append(str(error))
        except (ConnectionResetError, BrokenPipeError, OSError):
            # OTA reset closes the guest's network stack; the next boot reconnects.
            return
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def snapshot(self):
        with self.lock:
            return {'versions': list(self.versions), 'connections': self.connections,
                    'version_connections': list(self.version_connections),
                    'config_markers': list(self.config_markers),
                    'config_connections': list(self.config_connections),
                    'subscription_connections': list(self.subscription_connections),
                    'subscribed': self.subscribed, 'ota_sent': self.ota_sent,
                    'version_b_seen_at': self.version_b_seen_at,
                    'errors': list(self.errors)}

    def close(self):
        self.stop_event.set()
        self.listener.close()
        for conn in self.clients:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                conn.close()
            except OSError:
                pass
        self.thread.join(timeout=2)
        for worker in self.client_threads:
            worker.join(timeout=2)


def run(args):
    running = Path(args.running).resolve()
    running_elf = Path(args.running_elf).resolve()
    new = Path(args.new).resolve()
    new_elf = Path(args.new_elf).resolve()
    emulator = Path(args.emu).resolve()
    out = Path(args.out).resolve()
    for path in (running, running_elf, new, new_elf, emulator):
        if not path.is_file():
            raise ValueError('input file does not exist: ' + str(path))
    old_bytes, new_bytes = running.read_bytes(), new.read_bytes()
    expected_flash, image_mode, expected_flash_mode, mode_normalized = expected_flash_image(
        old_bytes, new_bytes, args.gzip)
    method = args.method
    setup_addr, loop_addr = arduino_hooks(new_elf)
    if not loop_addr:
        raise ValueError('replacement ELF has no loop() symbol for boot-2 proof')
    out.mkdir(parents=True, exist_ok=True)
    persistent = [out / name for name in ('network-profile.txt', 'network.pcap',
                 'emulator-report.json', 'emulator-events.jsonl', 'emulator.stderr.txt',
                 'uart.masked.txt', 'final-flash.bin', 'result.json')]
    if set(p.resolve() for p in persistent) & {running, running_elf, new, new_elf}:
        raise ValueError('output directory would overwrite a BIN or ELF input')
    profile = out / 'network-profile.txt'
    profile.write_text(build_profile([(running, running_elf), (new, new_elf)], '3.1.2'))
    forward_port = reserve_host_port() if method == 'web' else None
    update = HttpUpdateServer(new_bytes, args.gzip) if method == 'mqtt' else None
    url = 'http://10.0.2.2:%d/update.bin' % update.port if update else ''
    try:
        broker = MqttBroker(url, send_update=(method == 'mqtt'))
    except OSError:
        if update:
            update.close()
        raise
    if update:
        update.start()
    broker.start()
    uploader = (WebUpdateUploader(forward_port, new_bytes, broker.version_a_seen,
                                  max(60, args.seconds + 30), args.gzip)
                if method == 'web' else None)
    result = {'schema': 1, 'kind': 'network-service-smoke', 'verdict': 'FAIL',
              'service_scope': 'small MQTT 3.1.1 fixture: CONNECT, SUBSCRIBE, QoS 0 PUBLISH and PING only',
              'running_sha256': hashlib.sha256(old_bytes).hexdigest(),
              'new_sha256': hashlib.sha256(new_bytes).hexdigest(), 'gzip': args.gzip,
              'method': method, 'url': url or None}
    try:
        if uploader:
            uploader.start()
        with tempfile.TemporaryDirectory(prefix='esp-network-check-') as temporary:
            raw_uart = Path(temporary) / 'uart.raw'
            command = [str(emulator), '--flash', str(running), '--flash-out', str(out / 'final-flash.bin'),
                       '--speed', '1', '--max-seconds', str(args.seconds), '--no-stdin',
                       '--net-profile', str(profile), '--net-capture', str(out / 'network.pcap'),
                       '--net-host', 'broker.example.test=10.0.2.2',
                       '--report', str(out / 'emulator-report.json'), '--uart-log', str(raw_uart),
                       '--sym-loop', hex(loop_addr)]
            if args.chip:
                command += ['--chip', args.chip]
            if forward_port:
                command += ['--net-forward', '%d:80' % forward_port]
            if setup_addr:
                command += ['--sym-setup', hex(setup_addr)]
            started = time.monotonic()
            timed_out = False
            try:
                completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           timeout=max(120, args.seconds + 90), check=False)
                stdout_bytes, stderr_bytes = completed.stdout, completed.stderr
                result['emulator_exit_code'] = completed.returncode
            except subprocess.TimeoutExpired as error:
                timed_out = True
                stdout_bytes, stderr_bytes = error.stdout or b'', error.stderr or b''
                result['emulator_exit_code'] = None
            emulator_finished_at = time.monotonic()
            result['emulator_timed_out'] = timed_out
            result['host_wall_seconds'] = round(time.monotonic() - started, 2)
            broker.version_b_seen.wait(2)
            if update:
                update.response_sent.wait(2)
            if uploader:
                uploader.thread.join(timeout=3)
            stdout = stdout_bytes.decode('utf-8', 'replace')
            stderr = stderr_bytes.decode('utf-8', 'replace')
            Path(out / 'emulator.stderr.txt').write_text(stderr)
            events = [json.loads(line) for line in stdout.splitlines() if line.startswith('{')]
            Path(out / 'emulator-events.jsonl').write_text(''.join(json.dumps(event) + '\n' for event in events))
            uart = raw_uart.read_bytes().decode('latin-1') if raw_uart.exists() else ''
            Path(out / 'uart.masked.txt').write_text('\n'.join(mask_text(uart).splitlines()[:1000]) + '\n')
        report_path = out / 'emulator-report.json'
        report = json.loads(report_path.read_text()) if report_path.exists() else {}
        final_flash_path = out / 'final-flash.bin'
        flash = final_flash_path.read_bytes() if final_flash_path.exists() else b''
        http_snapshot = {'requests': update.requests if update else 0,
                         'status': update.status if update else 0,
                         'error': update.error if update else None,
                         'flash_prefix': flash[:len(expected_flash)]}
        upload_snapshot = uploader.snapshot() if uploader else None
        broker_snapshot = broker.snapshot()
        b_stable_seconds = (emulator_finished_at - broker_snapshot['version_b_seen_at']
                            if broker_snapshot['version_b_seen_at'] is not None else None)
        result['mqtt'] = broker_snapshot
        result['timing'] = {'version_b_publish_monotonic': broker_snapshot['version_b_seen_at'],
                            'emulator_finished_monotonic': emulator_finished_at,
                            'seconds_from_b_publish_to_emulator_exit': b_stable_seconds}
        result['http'] = {'requests': http_snapshot['requests'], 'status': http_snapshot['status'],
                          'error': http_snapshot['error']}
        result['image_mode_behavior'] = {'source_image_mode': image_mode,
                                         'expected_flash_mode': expected_flash_mode,
                                         'normalization_expected': mode_normalized,
                                         'gzip_transfer': args.gzip}
        if upload_snapshot:
            result['web_upload'] = {key: (value.decode('latin-1', 'replace') if key == 'response' else value)
                                    for key, value in upload_snapshot.items()}
        result['checks'] = build_checks(events, report, broker_snapshot, http_snapshot, uart,
                                        expected_flash, emulator_finished_at, method, upload_snapshot,
                                        image_mode, expected_flash_mode, mode_normalized)
        result['checks']['emulator_exited_cleanly'] = (
            result['checks']['emulator_exited_cleanly'] and result['emulator_exit_code'] == 0 and
            not result['emulator_timed_out'])
        result['verdict'] = 'PASS' if all(result['checks'].values()) and not broker_snapshot['errors'] else 'FAIL'
        result['report'] = report
        result['boot_events'] = [event for event in events
                                 if event.get('t') == 'stage' and event.get('s') in ('rom', 'loop')]
        Path(out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('network OTA %s: %s' % (result['verdict'], out / 'result.json'))
        return 0 if result['verdict'] == 'PASS' else 1
    finally:
        if uploader:
            uploader.close()
        broker.close()
        if update:
            update.close()


def positive_float(value):
    try:
        number = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError('must be a positive number') from error
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('must be a positive number')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--running', required=True)
    parser.add_argument('--running-elf', required=True)
    parser.add_argument('--new', required=True)
    parser.add_argument('--new-elf', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--method', choices=('mqtt', 'web'), default='mqtt')
    parser.add_argument('--chip', choices=('esp8266', 'esp12e', 'esp12f', 'esp8285', 'esp8285h16'))
    parser.add_argument('--seconds', type=positive_float, default=30)
    parser.add_argument('--emu', default=str(ROOT / 'build/esp8266emu-net'))
    parser.add_argument('--gzip', action='store_true',
                        help='send the new image gzip-compressed over the selected OTA method')
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print('error: %s' % error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
