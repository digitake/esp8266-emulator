"""Mask secrets in UART text before it is shown or stored.

Recognizes credential labels, JSON fields and URL query parameters, plus common
storage-dump and preset-auth block formats. Masking is heuristic: firmware may
print secrets in other formats, so review reports before sharing them. Masks are
length-preserving, allowing a line received in pieces to be masked without
revealing a partial secret.
"""
import re

_LABEL = re.compile(
    r'(?i)(\b(?:token|device[_ ]?key|devicekey|api[_ ]?key|secret|pass|password|passwd|pwd|psk)\s*[:=]\s*)(\S[^\r\n]*)')
_JSON = re.compile(r'(?i)("(?:token|device_token|device_key|devicekey|key|password|pass|wifi_pass|psk|secret)"\s*:\s*")([^"]*)')
_QUERY = re.compile(r'(?i)([?&](?:token|key|device_key|pass|password|psk)=)([^&\s]+)')


def _stars(s):
    return '*' * len(s)


def _body(line):
    b = line.rstrip('\r\n')
    return b, line[len(b):]


class LineMasker:
    """Masks one line at a time. The preset-auth block spans lines, so there is a little state: mask(line) with
    commit=False previews the result for an incomplete line without advancing that state."""

    def __init__(self):
        self.storage_dump = False
        self.preset_line = -1      # >= 0: inside the PRESET AUTH block, number of lines seen after its header

    def mask(self, line, commit=True):
        b, end = _body(line)
        nxt = self.preset_line + 1 if self.preset_line >= 0 else -1
        if self.storage_dump and b != '------End Storage-----':
            out = _stars(b) + end
        elif nxt == 3:                                         # token line in the preset-auth block
            out = _stars(b) + end
        elif nxt == 4:                                       # "ssid:pass"
            head, sep, tail = b.partition(':')
            out = (head + sep + _stars(tail) if sep else _stars(b)) + end
        else:
            out = _JSON.sub(lambda m: m.group(1) + _stars(m.group(2)), line)
            out = _QUERY.sub(lambda m: m.group(1) + _stars(m.group(2)), out)
            out = _LABEL.sub(lambda m: m.group(1) + _stars(m.group(2)), out)
        if commit:
            if b == '--------Storage-------':
                self.storage_dump = True
            elif b == '------End Storage-----':
                self.storage_dump = False
            if nxt >= 0:
                self.preset_line = -1 if nxt >= 4 else nxt
            if 'PRESET AUTH' in line:
                self.preset_line = 0
        return out

    def mask_line(self, line):
        return self.mask(line, True)


def mask_text(text):
    m = LineMasker()
    return ''.join(m.mask_line(l) for l in text.splitlines(keepends=True))


class StreamMasker:
    """Masks a stream that arrives in arbitrary chunks. feed() returns the masked text that can be released now:
    complete lines, plus (with partial=True) the incomplete current line. Whatever has been released of the current
    line is remembered; the next release masks the whole line again and emits only the new suffix. If a released
    piece ended right after a label ("Token:") or inside a value, the rest of that line is starred until newline,
    so a value split across chunks never shows."""

    _TAIL = re.compile(r'(?i)\b(?:token|device[_ ]?key|devicekey|api[_ ]?key|secret|pass|password|passwd|pwd|psk)\s*[:=]\s*\S*$')

    def __init__(self):
        self.lm = LineMasker()
        self.line = ''             # current line so far (raw)
        self.sent = 0              # how many characters of it were already released
        self.hide_rest = False

    def _release(self, raw_line, complete):
        if self.hide_rest:
            b, end = _body(raw_line)
            masked = raw_line[:self.sent] + _stars(b[self.sent:]) + (end if complete else '')
            masked = self.lm.mask(raw_line, complete)[:self.sent] + masked[self.sent:]
        else:
            masked = self.lm.mask(raw_line, complete)
        out = masked[self.sent:]
        if complete:
            self.line, self.sent, self.hide_rest = '', 0, False
        else:
            self.sent = len(raw_line)
            if self._TAIL.search(raw_line) or (self.lm.preset_line >= 0 and self.lm.preset_line + 1 in (3, 4)):
                self.hide_rest = True
        return out

    def feed(self, text, partial=False):
        out = []
        for ch_line in text.splitlines(keepends=True):
            self.line += ch_line
            if self.line.endswith('\n'):
                out.append(self._release(self.line, True))
        if partial and len(self.line) > self.sent:
            out.append(self._release(self.line, False))
        return ''.join(out)


if __name__ == '__main__':
    sample = ('Broker:mqtt://x\r\nToken:abcdef123456\r\nSSID:home\r\nPASS:hunter22\r\nPRESET AUTH: http://s\r\n'
              'mqtt://b\r\nmqtt://l\r\ntok-123\r\nmyssid:mypass\r\nDeviceKey verification: GET\r\nWrong password\r\n'
              '{"token":"zzz","ok":1}\r\nGET /a?key=777&x=1\r\n')
    out = mask_text(sample)
    for bad in ('abcdef123456', 'hunter22', 'tok-123', 'mypass', 'zzz', '777'):
        assert bad not in out, (bad, out)
    for keep in ('Broker:mqtt://x', 'SSID:home', 'DeviceKey verification: GET', 'Wrong password', 'myssid:', 'x=1',
                 'mqtt://b', 'mqtt://l'):
        assert keep in out, (keep, out)
    assert len(out) == len(sample)
    # every way of cutting the sample into two chunks, releasing partial lines after each chunk
    secrets = ('abcdef', '123456', 'unter2', 'tok-12', 'ypass', 'zzz', '777')
    for cut in range(1, len(sample)):
        sm = StreamMasker()
        got = sm.feed(sample[:cut], partial=True) + sm.feed(sample[cut:], partial=True)
        assert not any(s in got for s in secrets), (cut, got)
        assert 'Broker:mqtt://x' in got and 'DeviceKey verification: GET' in got or cut in range(0, 20), (cut, got)
    # three chunks, cut inside the token value and inside the label
    for a in range(1, 30):
        for b in range(a + 1, 40):
            sm = StreamMasker()
            got = sm.feed(sample[:a], True) + sm.feed(sample[a:b], True) + sm.feed(sample[b:], True)
            assert not any(s in got for s in secrets), (a, b, got)
    # Storage dumps may contain credential values without individual labels.
    storage = ('--------Storage-------\r\nhttp://server\r\nmqtt://broker\r\n'
               'legacy-token\r\nmyssid\r\nlegacy-password\r\n------End Storage-----\r\nSetup done\r\n')
    for cut in range(1, len(storage)):
        sm = StreamMasker()
        got = sm.feed(storage[:cut], True) + sm.feed(storage[cut:], True)
        assert len(got) == len(storage)
        for secret in ('legacy-token', 'legacy-password'):
            start = storage.index(secret)
            assert got[start:start + len(secret)] == '*' * len(secret), (cut, got)
        assert got.endswith('Setup done\r\n'), (cut, got)
    sm = StreamMasker()
    got = ''.join(sm.feed(ch, True) for ch in storage)
    for secret in ('legacy-token', 'legacy-password'):
        start = storage.index(secret)
        assert got[start:start + len(secret)] == '*' * len(secret), got
    assert got.endswith('Setup done\r\n'), got
    print('mask self-test ok')
