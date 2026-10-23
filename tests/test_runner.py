"""Runner host I/O: stdin commands after a reset or power cycle must not be lost."""
import functools
import json
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build/esp8266emu'


def idle_image():
    """A boot image whose only code is J to itself at 0x40100000."""
    segments = [(0x40100000, bytes.fromhex('06ffff00'))]
    body = struct.pack('<BBBBI', 0xe9, len(segments), 0, 0x20, 0x40100000)
    checksum = 0xef
    for address, data in segments:
        body += struct.pack('<II', address, len(data)) + data
        checksum = functools.reduce(lambda a, b: a ^ b, data, checksum)
    return body + bytes((-len(body) - 1) % 16) + bytes([checksum])


class RunnerStdinTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='esp8266-stdin-')
        image = Path(self.tmp.name) / 'idle.bin'
        image.write_bytes(idle_image())
        self.proc = subprocess.Popen([str(EXE), '--flash', str(image), '--speed', '1'],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.lines = queue.Queue()
        self.events = []
        threading.Thread(target=self.read_events, daemon=True).start()

    def tearDown(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        self.proc.stdin.close()
        self.proc.stdout.close()
        self.tmp.cleanup()

    def read_events(self):
        for line in self.proc.stdout:
            self.lines.put(json.loads(line))
        self.lines.put(None)

    def send(self, data):
        self.proc.stdin.write(data)          # one write: a single read() in the runner sees all of it

    def wait_for(self, what, match, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            try:
                event = self.lines.get(timeout=max(0, deadline - time.monotonic()))
            except queue.Empty:
                self.fail(f'no {what} within {timeout} s; boots: {self.boots()}')
            if event is None:
                self.fail(f'runner exited before {what}; boots: {self.boots()}')
            self.events.append(event)
            if match(event):
                return event

    def wait_boot(self, boot):
        return self.wait_for(f'boot {boot}', lambda e: e['t'] == 'stage' and e['s'] == 'rom' and e['boot'] == boot)

    def wait_low_edge(self, pin):
        return self.wait_for(f'GPIO{pin} edge to 0', lambda e: e['t'] == 'edges' and
                             any(edge[1] == pin and edge[2] == 0 for edge in e['e']))

    def boots(self):
        return [(e['boot'], e['reason']) for e in self.events if e['t'] == 'stage' and e['s'] == 'rom']

    def quit(self):
        self.send(b'quit\n')
        self.assertEqual(self.proc.wait(timeout=10), 0)

    def test_commands_after_reset_and_power_cycle_take_effect(self):
        self.wait_boot(1)
        self.send(b'reset\n')
        self.wait_boot(2)
        self.send(b'power\n')
        self.wait_boot(3)
        self.send(b'in 5 0\n')
        self.wait_low_edge(5)
        self.send(b'in 4 0\n')
        self.wait_low_edge(4)
        self.assertEqual(self.boots(), [(1, 1), (2, 2), (3, 1)])
        self.quit()

    def test_command_read_together_with_reset_runs_after_reboot(self):
        self.wait_boot(1)
        self.send(b'reset\nin 4 0\n')
        self.wait_boot(2)
        self.wait_low_edge(4)                # no further input: the buffered line must not wait for more
        self.assertEqual(self.boots(), [(1, 1), (2, 2)])
        self.quit()


if __name__ == '__main__':
    unittest.main()
