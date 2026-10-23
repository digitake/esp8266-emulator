"""Check physical flash limits and explicit chip selection through the runner."""
import functools
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build/esp8266emu'


def image_bytes():
    code = bytes.fromhex('06ffff00')  # jump to self
    body = struct.pack('<BBBBIII', 0xe9, 1, 0, 0x20, 0x40100000, 0x40100000, len(code)) + code
    checksum = functools.reduce(int.__xor__, code, 0xef)
    return body + bytes((-len(body) - 1) % 16) + bytes([checksum])


class ChipCliTests(unittest.TestCase):
    def test_chip_and_capacity_are_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            image, report = Path(tmp) / 'image.bin', Path(tmp) / 'report.json'
            image.write_bytes(image_bytes())
            cases = [([], 'esp8266', 1 << 20),
                     (['--chip', 'esp12f'], 'esp12f', 4 << 20),
                     (['--chip', 'esp8266', '--flash-size', '1048576'], 'esp8266', 1 << 20),
                     (['--chip', 'esp8285'], 'esp8285', 1 << 20),
                     (['--chip', 'esp8285h16'], 'esp8285h16', 2 << 20)]
            for options, chip, size in cases:
                with self.subTest(chip=chip, size=size):
                    subprocess.run([str(EXE), '--flash', str(image), '--report', str(report),
                                    '--speed', '0', '--max-seconds', '0.001', '--no-stdin',
                                    '--quiet', *options], check=True, capture_output=True, timeout=10)
                    result = json.loads(report.read_text())
                    self.assertEqual((result['chip'], result['flash_size']), (chip, size))

    def test_bad_size_and_wrong_embedded_capacity_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'image.bin'
            image.write_bytes(image_bytes())
            for options in (['--flash-size', '-1'], ['--flash-size', '4294967296'],
                            ['--flash-size', '0'], ['--flash-size', '300000'],
                            ['--chip', 'esp8285', '--flash-size', '2097152'],
                            ['--chip', 'unknown'], ['--flash-id', '16777216']):
                with self.subTest(options=options):
                    result = subprocess.run([str(EXE), '--flash', str(image), *options],
                                            capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, 2, result.stderr)
            image.write_bytes(image_bytes() + bytes(262145))
            result = subprocess.run([str(EXE), '--flash', str(image), '--flash-size', '262144'],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertIn(b'flash capacity', result.stderr)


if __name__ == '__main__':
    unittest.main()
