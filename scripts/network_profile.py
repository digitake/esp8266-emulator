"""Generate checked symbol profiles for the emulator's ESP8266 network hooks."""
import argparse
import hashlib
from pathlib import Path
import struct
import sys

CORE_VERSION = '3.1.2'
HEADER = 'esp8266-net-v1 arduino-3.1.2-lwip2'
FLASHMAP_BASE = 0x40200000
FLASHMAP_END = 0x40300000
IRAM_BASE = 0x40100000
IRAM_END = 0x40110000
MAX_BIN = 16 * 1024 * 1024
CRC_LEN_OFFSET = 4096 + 16
CRC_VALUE_OFFSET = CRC_LEN_OFFSET + 4

# Keep this list aligned with names[] in src/net.c. These entries mirror the
# net_enable() required set: indexes 0..12 plus IP_GET, EVENT_CB and MAC_GET.
SYMBOLS = (
    'glue2esp_linkoutput', 'esp2glue_alloc_for_recv', 'esp2glue_ethernet_input',
    'esp2glue_pbuf_freed', 'esp2glue_netif_update', 'esp2glue_netif_set_up1down0',
    'esp2glue_dhcp_start', 'esp2glue_dhcp_stop', 'glue2esp_ifupdown',
    'esp2glue_lwip_init', 'wifi_station_connect', 'wifi_station_disconnect',
    'wifi_station_get_connect_status', 'wifi_get_ip_info', 'wifi_set_ip_info',
    'wifi_set_event_handler_cb', 'wifi_station_dhcpc_start', 'wifi_station_dhcpc_stop',
    'wifi_station_dhcpc_status', 'wifi_station_get_rssi', 'wifi_get_macaddr',
    'wifi_station_get_config', 'wifi_station_set_config',
    'wifi_station_set_config_current', 'wifi_get_channel',
)
REQUIRED = frozenset(SYMBOLS[:13] + (SYMBOLS[13], SYMBOLS[15], SYMBOLS[20]))


def _slice(data, offset, size, what):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError('%s exceeds file bounds' % what)
    return data[offset:offset + size]


def parse_elf(data, label):
    if len(data) < 52 or data[:4] != b'\x7fELF' or data[4:6] != b'\x01\x01':
        raise ValueError('%s must be a 32-bit little-endian ELF' % label)
    shoff = struct.unpack_from('<I', data, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 46)
    if shentsize < 40 or not shnum or shstrndx >= shnum:
        raise ValueError('%s has invalid section headers' % label)
    _slice(data, shoff, shentsize * shnum, label + ' section table')
    raw_sections = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize)
                    for i in range(shnum)]
    names_header = raw_sections[shstrndx]
    names = _slice(data, names_header[4], names_header[5], label + ' section names')
    sections = []
    for index, raw in enumerate(raw_sections):
        name_offset, kind, flags, addr, offset, size, link, info, align, entsize = raw
        if name_offset >= len(names):
            raise ValueError('%s section %d has an invalid name' % (label, index))
        end = names.find(b'\0', name_offset)
        if end < 0:
            raise ValueError('%s section %d has an unterminated name' % (label, index))
        name = names[name_offset:end].decode('ascii', 'replace')
        if kind != 8:  # SHT_NOBITS has no bytes in the ELF file.
            _slice(data, offset, size, label + ' section ' + name)
        sections.append({'name': name, 'type': kind, 'flags': flags, 'addr': addr,
                         'offset': offset, 'size': size, 'link': link, 'entsize': entsize})
    symtabs = [s for s in sections if s['type'] == 2]  # Require an unstripped SHT_SYMTAB.
    if not symtabs:
        raise ValueError('%s has no unstripped symbol table' % label)
    symbols = {}
    for section in symtabs:
        if section['link'] >= len(sections):
            raise ValueError('%s has an invalid symbol string table' % label)
        strings_section = sections[section['link']]
        strings = _slice(data, strings_section['offset'], strings_section['size'],
                         label + ' symbol strings')
        stride = section['entsize'] or 16
        if stride < 16 or section['size'] % stride:
            raise ValueError('%s has an invalid symbol table size' % label)
        for pos in range(section['offset'], section['offset'] + section['size'], stride):
            name_offset, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, pos)
            if not name_offset:
                continue
            if name_offset >= len(strings):
                raise ValueError('%s has an invalid symbol name offset' % label)
            end = strings.find(b'\0', name_offset)
            if end < 0:
                raise ValueError('%s has an unterminated symbol name' % label)
            name = strings[name_offset:end].decode('latin-1')
            if name in SYMBOLS and (info & 0x0f) in (0, 1, 2):
                if name in symbols and symbols[name] != value:
                    raise ValueError('%s has conflicting definitions for %s' % (label, name))
                symbols[name] = value
    return sections, symbols


def parse_e9(data, offset, label):
    _slice(data, offset, 8, label + ' image header')
    if data[offset] != 0xe9:
        raise ValueError('%s is not an ESP8266 E9 image' % label)
    count = data[offset + 1]
    if count < 1 or count > 16:
        raise ValueError('%s has an invalid E9 segment count' % label)
    cursor = offset + 8
    segments = []
    for index in range(count):
        header = _slice(data, cursor, 8, '%s segment %d header' % (label, index))
        address, size = struct.unpack('<II', header)
        cursor += 8
        payload = _slice(data, cursor, size, '%s segment %d data' % (label, index))
        if size and address + size > 0x100000000:
            raise ValueError('%s segment %d address overflows' % (label, index))
        segments.append((address, size, cursor, payload))
        cursor += size
    return segments


def crc8266(data):
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


def check_image(image, elf, label):
    if not image or len(image) > MAX_BIN:
        raise ValueError('%s BIN must be between 1 byte and 16 MiB' % label)
    if len(image) < 0x1020:
        raise ValueError('%s BIN is too short for the eboot application image' % label)
    sections, symbols = parse_elf(elf, label + ' ELF')
    parse_e9(image, 0, label + ' eboot')
    app = parse_e9(image, 0x1000, label + ' application')
    if not any(addr == 0x40201010 for addr, size, pos, payload in app):
        raise ValueError('%s application image has no flash-mapped segment at 0x40201010' % label)

    irom = [s for s in sections if s['name'] == '.irom0.text']
    if len(irom) != 1:
        raise ValueError('%s ELF must contain exactly one .irom0.text section' % label)
    section = irom[0]
    if section['type'] != 1 or section['flags'] & 0x6 != 0x6:
        raise ValueError('%s .irom0.text must be allocated executable PROGBITS' % label)
    if section['addr'] < FLASHMAP_BASE or section['addr'] + section['size'] > FLASHMAP_END:
        raise ValueError('%s .irom0.text lies outside the flash cache window' % label)
    irom_offset = section['addr'] - FLASHMAP_BASE
    bin_irom = _slice(image, irom_offset, section['size'], label + ' flash-mapped section')
    elf_irom = elf[section['offset']:section['offset'] + section['size']]
    # Arduino core 3.1.2 elf2bin.py patches __crc_len/__crc_val at 0x1010/0x1014.
    if irom_offset != CRC_LEN_OFFSET or section['size'] < 8:
        raise ValueError('%s .irom0.text does not contain the expected core CRC fields' % label)
    if bin_irom[8:] != elf_irom[8:]:
        raise ValueError('%s .irom0.text does not match BIN at its flash address' % label)
    stored_len, stored_crc = struct.unpack_from('<II', bin_irom)
    if stored_len != len(image):
        raise ValueError('%s BIN CRC length does not match file size' % label)
    crc_image = bytearray(image)
    struct.pack_into('<II', crc_image, CRC_LEN_OFFSET, 0, 0)
    if stored_crc != crc8266(crc_image):
        raise ValueError('%s BIN has an invalid Arduino core CRC' % label)

    executable_sections = [s for s in sections if s['type'] == 1 and s['flags'] & 0x6 == 0x6 and s['size']]
    for name in REQUIRED:
        if name not in symbols:
            raise ValueError('%s ELF missing required network symbol %s' % (label, name))
    addresses = {}
    for name in SYMBOLS:
        if name not in symbols:
            continue
        address = symbols[name]
        if not (IRAM_BASE <= address < IRAM_END or FLASHMAP_BASE <= address < FLASHMAP_END):
            raise ValueError('%s symbol %s is outside executable IRAM/cache memory: 0x%x' %
                             (label, name, address))
        if not any(s['addr'] <= address < s['addr'] + s['size'] for s in executable_sections):
            raise ValueError('%s symbol %s is not in an executable ELF section' % (label, name))
        if address in addresses:
            raise ValueError('%s network symbols %s and %s alias at 0x%x' %
                             (label, addresses[address], name, address))
        addresses[address] = name

    # Check file-backed IRAM code against the corresponding segment in the nested app image.
    for s in executable_sections:
        if s['name'] == '.irom0.text':
            continue
        compared = False
        for address, size, pos, payload in app:
            if address <= s['addr'] and s['addr'] + s['size'] <= address + size:
                actual = payload[s['addr'] - address:s['addr'] - address + s['size']]
                expected = elf[s['offset']:s['offset'] + s['size']]
                if actual != expected:
                    raise ValueError('%s executable ELF section %s does not match BIN' %
                                     (label, s['name']))
                compared = True
                break
        if IRAM_BASE <= s['addr'] < IRAM_END and not compared:
            raise ValueError('%s executable IRAM section %s is not backed by an application segment' %
                             (label, s['name']))
    return symbols


def build_profile(images, core_version):
    if core_version != CORE_VERSION:
        raise ValueError('unsupported core version %s (expected %s)' % (core_version, CORE_VERSION))
    if not 1 <= len(images) <= 8:
        raise ValueError('provide between 1 and 8 --image BIN ELF pairs')
    output = [HEADER]
    for bin_path, elf_path in images:
        bpath, epath = Path(bin_path).resolve(), Path(elf_path).resolve()
        for p in (bpath, epath):
            if '\n' in str(p) or '\r' in str(p):
                raise ValueError('image paths may not contain newlines')
        image, elf = bpath.read_bytes(), epath.read_bytes()
        symbols = check_image(image, elf, str(bpath))
        output.extend(['# bin-sha256 %s %s' % (hashlib.sha256(image).hexdigest(), bpath),
                       '# elf-sha256 %s %s' % (hashlib.sha256(elf).hexdigest(), epath),
                       'image ' + str(bpath)])
        for name in SYMBOLS:
            if name in symbols:
                output.append('symbol %s 0x%x' % (name, symbols[name]))
        output.append('end')
    return '\n'.join(output) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', action='append', nargs=2, metavar=('BIN', 'ELF'), required=True,
                        help='BIN/ELF pair; repeat for each image')
    parser.add_argument('--core-version', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    output_path = Path(args.out).resolve()
    inputs = {Path(p).resolve() for pair in args.image for p in pair}
    if output_path in inputs:
        parser.error('--out must not overwrite an input BIN or ELF')
    try:
        content = build_profile(args.image, args.core_version)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(content)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print('wrote %s (%d images)' % (output_path, len(args.image)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
