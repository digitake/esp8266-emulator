"""Minimal ELF32 little-endian symbol reader (stdlib only): enough to find setup()/loop() in firmware.elf."""
import struct
from pathlib import Path


def read_symbols(path):
    data = Path(path).read_bytes()
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise ValueError('not a 32-bit little-endian ELF')
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for name, typ, flags, addr, off, size, link, info, align, entsize in secs:
        if typ != 2:                                  # SHT_SYMTAB
            continue
        stroff = secs[link][4]
        for i in range(size // 16):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', data, off + i * 16)
            end = data.index(b'\0', stroff + st_name)
            nm = data[stroff + st_name:end].decode('latin-1')
            if nm and (st_info & 0xf) in (0, 1, 2):   # NOTYPE (linker symbols such as _FS_start), OBJECT, FUNC
                syms.setdefault(nm, st_value)
    return syms


def arduino_hooks(path):
    """Addresses of setup() and loop() (Arduino sketches export them unmangled)."""
    s = read_symbols(path)
    return s.get('setup'), s.get('loop')


if __name__ == '__main__':
    import sys
    st, lp = arduino_hooks(sys.argv[1])
    print('setup=%s loop=%s' % (hex(st) if st else None, hex(lp) if lp else None))
