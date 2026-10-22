"""Prove a non-yielding guest resets via HW WDT, including repeated sub-1ms resets."""
import argparse
import functools
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from support import cli, source_commit

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--emu-commit', help='override source commit (default: EMU_COMMIT or git HEAD)')
    args = parser.parse_args()
    # A real boot image with tiny synthetic code: J to itself, no yield or feed.
    # Load reload registers before enabling the watchdog (one stage, 2^16 clocks).
    segments = [(0x40100000, bytes.fromhex('06ffff00')),
                (0x60000904, struct.pack('<II', 0, 0)),
                (0x60000900, struct.pack('<I', 0x3d))]
    body = struct.pack('<BBBBI', 0xe9, len(segments), 0, 0x20, 0x40100000)
    checksum = 0xef
    for address, data in segments:
        body += struct.pack('<II', address, len(data)) + data
        checksum = functools.reduce(lambda a, b: a ^ b, data, checksum)
    body += bytes((-len(body) - 1) % 16) + bytes([checksum])
    exe = ROOT / 'build/esp8266emu'
    with tempfile.TemporaryDirectory(prefix='esp8266-wdt-') as work:
        image, report = Path(work) / 'hang.bin', Path(work) / 'report.json'
        image.write_bytes(body)
        run = subprocess.run([str(exe), '--flash', str(image), '--speed', '0',
                              '--max-seconds', '0.003', '--no-stdin', '--report', str(report)],
                             capture_output=True, text=True, check=True, timeout=10)
        events = [json.loads(line) for line in run.stdout.splitlines()]
        boots = [e for e in events if e.get('t') == 'stage' and e.get('s') == 'rom']
        expected = [(1, 1, 0), (2, 4, 819), (3, 4, 1638), (4, 4, 2457)]
        actual = [(e['boot'], e['reason'], e['t_us']) for e in boots]
        assert actual == expected, actual
        uart = b''.join(bytes.fromhex(e['d']) for e in events if e.get('t') == 'uart')
        assert uart.count(b'rst cause:4') == 3, uart
        result = {'scenario': 'synthetic busy loop; single-stage hardware watchdog; 0.8192 ms deadline',
                  'synthetic_image_sha256': hashlib.sha256(body).hexdigest(),
                  'emulator_parent': source_commit(args.emu_commit),
                  'emulator_binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                  'boots': boots, 'report': json.loads(report.read_text()),
                  'uart': uart.decode('latin1'), 'pass': True}
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + '\n')
    print('watchdog busy-loop evidence: PASS', args.out)

if __name__ == '__main__':
    sys.exit(cli(main))
