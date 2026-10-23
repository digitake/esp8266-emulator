"""Synthetic ELF/BIN fixtures for the fleet network-profile generator."""
import struct
import hashlib
import tempfile
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import network_profile


def crc8266(data):
    return network_profile.crc8266(data)


def crc8266_bitwise(data):
    """The bit-at-a-time definition the table-driven crc8266 must reproduce."""
    crc = 0xffffffff
    for byte in data:
        for bitmask in (0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01):
            bit = bool(crc & 0x80000000)
            if byte & bitmask:
                bit = not bit
            crc = (crc << 1) & 0xffffffff
            if bit:
                crc ^= 0x04c11db7
    return crc


def make_elf(symbol_names=None, aliases=False, bad_addr=False):
    names = list(network_profile.SYMBOLS if symbol_names is None else symbol_names)
    text = bytes(range(64))
    irom = b'\0' * 8 + bytes((i * 7) & 0xff for i in range(256))
    shstr = b'\0.text\0.irom0.text\0.shstrtab\0.symtab\0.strtab\0'
    section_name = {name: shstr.index(name.encode()) for name in ('.text', '.irom0.text', '.shstrtab', '.symtab', '.strtab')}
    strings = b'\0' + b'\0'.join(name.encode() for name in names) + b'\0'
    name_offsets = {}
    cursor = 1
    for name in names:
        name_offsets[name] = cursor
        cursor += len(name) + 1

    data = bytearray(b'\0' * 52)
    text_off = len(data)
    data.extend(text)
    irom_off = len(data)
    data.extend(irom)
    shstr_off = len(data)
    data.extend(shstr)
    str_off = len(data)
    data.extend(strings)
    sym_off = len(data)
    data.extend(b'\0' * 16)
    for i, name in enumerate(names):
        addr = 0x40201018 + 4 * i
        if aliases and i == 1:
            addr = 0x40201018
        if bad_addr and i == 0:
            addr = 0x40000000
        data.extend(struct.pack('<IIIBBH', name_offsets[name], addr, 4, 0x12, 0, 2))
    shoff = len(data)
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (section_name['.text'], 1, 0x6, 0x40100000, text_off, len(text), 0, 0, 4, 0),
        (section_name['.irom0.text'], 1, 0x6, 0x40201010, irom_off, len(irom), 0, 0, 16, 0),
        (section_name['.shstrtab'], 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0),
        (section_name['.symtab'], 2, 0, 0, sym_off, 16 * (len(names) + 1), 5, 1, 4, 16),
        (section_name['.strtab'], 3, 0, 0, str_off, len(strings), 0, 0, 1, 0),
    ]
    for section in sections:
        data.extend(struct.pack('<IIIIIIIIII', *section))
    data[:16] = b'\x7fELF\x01\x01\x01' + b'\0' * 9
    struct.pack_into('<HHIIIII', data, 16, 2, 94, 1, 0x40100000, 0, 0, 0)
    struct.pack_into('<HHHHHH', data, 40, 52, 0, 0, 40, len(sections), 3)
    struct.pack_into('<I', data, 32, shoff)
    return bytes(data), text, irom


def make_bin(text, irom, include_text=True):
    # Standard eboot image followed by the nested Arduino application image.
    raw = bytearray(b'\xff' * 0x2000)
    raw[:8] = struct.pack('<BBBBI', 0xe9, 1, 0, 0x30, 0x40100000)
    struct.pack_into('<II', raw, 8, 0x40100000, len(text))
    raw[16:16 + len(text)] = text
    app = 0x1000
    raw[app:app + 8] = struct.pack('<BBBBI', 0xe9, 2 if include_text else 1, 0, 0x30, 0x40100000)
    cursor = app + 8
    struct.pack_into('<II', raw, cursor, 0x40201010, len(irom))
    cursor += 8
    raw[cursor:cursor + len(irom)] = irom
    cursor += len(irom)
    if include_text:
        struct.pack_into('<II', raw, cursor, 0x40100000, len(text))
        cursor += 8
        raw[cursor:cursor + len(text)] = text
    raw[network_profile.CRC_LEN_OFFSET:network_profile.CRC_VALUE_OFFSET + 4] = b'\0' * 8
    crc = crc8266(raw)
    struct.pack_into('<I', raw, network_profile.CRC_LEN_OFFSET, len(raw))
    struct.pack_into('<I', raw, network_profile.CRC_VALUE_OFFSET, crc)
    return bytes(raw)


class NetworkProfileTests(unittest.TestCase):
    def make_pair(self, directory, names=None, aliases=False):
        elf, text, irom = make_elf(names, aliases)
        image = make_bin(text, irom)
        bin_path = Path(directory) / 'firmware with spaces.bin'
        elf_path = Path(directory) / 'firmware with spaces.elf'
        bin_path.write_bytes(image)
        elf_path.write_bytes(elf)
        return bin_path, elf_path, image, elf

    def test_table_crc_matches_the_bitwise_definition(self):
        samples = [b'', b'\0', b'\xff' * 7, b'123456789', bytes(range(256)) * 3,
                   hashlib.sha256(b'esp8266').digest() * 40]
        for data in samples:
            with self.subTest(size=len(data)):
                self.assertEqual(network_profile.crc8266(data), crc8266_bitwise(data))
        self.assertEqual(network_profile.crc8266(b'123456789'), 0x0376e6e7)   # CRC-32/MPEG-2 check value

    def test_generates_checked_profile_with_absolute_paths_and_hashes(self):
        with tempfile.TemporaryDirectory(prefix='network fixture ') as directory:
            bin_path, elf_path, image, elf = self.make_pair(directory)
            profile = network_profile.build_profile([(bin_path, elf_path)], '3.1.2')
            self.assertTrue(profile.startswith(network_profile.HEADER + '\n'))
            self.assertIn('# bin-sha256 ', profile)
            self.assertIn('# elf-sha256 ', profile)
            self.assertIn('image ' + str(bin_path.resolve()) + '\n', profile)
            self.assertIn('\nsha256 ' + hashlib.sha256(image).hexdigest() + '\n', profile)
            self.assertIn('symbol wifi_get_ip_info ', profile)
            self.assertEqual(profile.count('\nend\n'), 1)
            self.assertEqual(bin_path.read_bytes(), image)
            self.assertEqual(elf_path.read_bytes(), elf)

    def test_rejects_image_section_mismatch_and_missing_required_symbol(self):
        with tempfile.TemporaryDirectory() as directory:
            bin_path, elf_path, image, _ = self.make_pair(directory)
            changed = bytearray(image)
            changed[network_profile.CRC_LEN_OFFSET + 8] ^= 0xff
            bin_path.write_bytes(changed)
            with self.assertRaisesRegex(ValueError, 'does not match BIN'):
                network_profile.build_profile([(bin_path, elf_path)], '3.1.2')
            elf, text, irom = make_elf([n for n in network_profile.SYMBOLS if n != 'wifi_get_ip_info'])
            elf_path.write_bytes(elf)
            bin_path.write_bytes(make_bin(text, irom))
            with self.assertRaisesRegex(ValueError, 'missing required network symbol wifi_get_ip_info'):
                network_profile.build_profile([(bin_path, elf_path)], '3.1.2')

    def test_rejects_aliases_invalid_core_and_malformed_e9_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            bin_path, elf_path, _, _ = self.make_pair(directory, aliases=True)
            with self.assertRaisesRegex(ValueError, 'alias'):
                network_profile.build_profile([(bin_path, elf_path)], '3.1.2')
            with self.assertRaisesRegex(ValueError, 'unsupported core version'):
                network_profile.build_profile([(bin_path, elf_path)], '3.0.0')
            _, text, irom = make_elf()
            malformed = bytearray(make_bin(text, irom))
            struct.pack_into('<I', malformed, 12, 0xffffffff)
            bin_path.write_bytes(malformed)
            with self.assertRaisesRegex(ValueError, 'exceeds file bounds'):
                network_profile.build_profile([(bin_path, elf_path)], '3.1.2')

    def test_rejects_nonexecutable_symbol_addresses_and_more_than_eight_images(self):
        with tempfile.TemporaryDirectory() as directory:
            elf, text, irom = make_elf(bad_addr=True)
            bin_path = Path(directory) / 'bad.bin'
            elf_path = Path(directory) / 'bad.elf'
            bin_path.write_bytes(make_bin(text, irom))
            elf_path.write_bytes(elf)
            with self.assertRaisesRegex(ValueError, 'outside executable IRAM/cache memory'):
                network_profile.build_profile([(bin_path, elf_path)], '3.1.2')
            with self.assertRaisesRegex(ValueError, 'between 1 and 8'):
                network_profile.build_profile([(bin_path, elf_path)] * 9, '3.1.2')

    def test_rejects_iram_code_without_a_loaded_application_segment(self):
        with tempfile.TemporaryDirectory() as directory:
            elf, text, irom = make_elf()
            bin_path = Path(directory) / 'missing-iram.bin'
            elf_path = Path(directory) / 'missing-iram.elf'
            bin_path.write_bytes(make_bin(text, irom, include_text=False))
            elf_path.write_bytes(elf)
            with self.assertRaisesRegex(ValueError, 'IRAM section .text is not backed'):
                network_profile.build_profile([(bin_path, elf_path)], '3.1.2')


if __name__ == '__main__':
    unittest.main()
