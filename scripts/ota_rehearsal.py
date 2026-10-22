"""OTA rehearsal without a network: everything after the download, on the unmodified images.

Stages what the running firmware's Updater + Update.end() leave behind (Arduino core 3.1.2 Updater.cpp):
  * the new image written at  slot = (_FS_start - 0x40200000) - roundUp4K(new size)   (end of the OTA window),
    refused by Updater.begin() if slot < roundUp4K(running sketch);
  * the eboot command in RTC memory 0x60001200: {magic 0xeb001000, COPY_RAW, src=slot, dst=0, size, crc32},
then does what ESP.restart() does (jump to the ROM reset vector). The real eboot of the running image then copies
the new image over flash 0x0 (gunzipping a .gz upload) and boots it. Checks: eboot's own output, the resulting
flash byte-compared with the new image, and the new image booting to its main loop.

usage (WSL): python3 scripts/ota_rehearsal.py --running A.bin --running-elf A.elf --new B.bin [--gzip] [--expect TEXT]
                                         --out evidence/<date>/<name>.json
"""
import argparse, gzip, hashlib, json, math, os, re, struct, subprocess, sys, tempfile, time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from mask import mask_text            # noqa: E402
from elfsyms import read_symbols      # noqa: E402
from support import cli, source_commit  # noqa: E402


def crc_update(crc, data):            # eboot_command.c: MSB-first CRC-32, poly 0x04c11db7, no reflection/final xor
    for c in data:
        for i in (0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01):
            bit = bool(crc & 0x80000000)
            if c & i:
                bit = not bit
            crc = (crc << 1) & 0xffffffff
            if bit:
                crc ^= 0x04c11db7
    return crc


def eboot_command(src, dst, size):
    words = [0xeb001000, 0x00000001, src, dst, size] + [0] * 26
    body = struct.pack('<31I', *words)
    return body + struct.pack('<I', crc_update(0xffffffff, body))


def round4k(n):
    return (n + 0xfff) & ~0xfff


def sha(b):
    return hashlib.sha256(b).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--running', required=True)
    ap.add_argument('--running-elf', required=True)
    ap.add_argument('--new', required=True)
    ap.add_argument('--gzip', action='store_true', help='upload the new image gzip-compressed (eboot gunzips it)')
    ap.add_argument('--expect', help='text the new image prints on UART when it runs (proves which image booted)')
    ap.add_argument('--seconds', type=float, default=6)
    ap.add_argument('--stage-at-ms', type=float, default=1500)
    ap.add_argument('--source', default='')
    ap.add_argument('--emu-commit', help='override source commit (default: EMU_COMMIT or git HEAD)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    if not math.isfinite(a.seconds) or a.seconds <= 0:
        ap.error('--seconds must be a finite positive number')
    if not math.isfinite(a.stage_at_ms) or a.stage_at_ms < 0:
        ap.error('--stage-at-ms must be a finite nonnegative number')

    A, B = Path(a.running).read_bytes(), Path(a.new).read_bytes()
    payload = gzip.compress(B, compresslevel=9, mtime=0) if a.gzip else B
    syms = read_symbols(a.running_elf)
    window_end = syms['_FS_start'] - 0x40200000
    running = round4k(len(A))
    slot = window_end - round4k(len(payload))
    fits = slot >= running
    result = {'schema': 1, 'kind': 'ota-rehearsal', 'date_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'source': a.source,
              'running': {'bin': os.path.basename(a.running), 'sha256': sha(A), 'bytes': len(A)},
              'new': {'bin': os.path.basename(a.new), 'sha256': sha(B), 'bytes': len(B), 'gzip': a.gzip,
                      'upload_bytes': len(payload), 'upload_sha256': sha(payload)},
              'layout': {'ota_window_end': hex(window_end), 'running_rounded': hex(running), 'slot': hex(slot),
                         'fits': fits, 'free_after': slot - running}}
    if not fits:
        result['verdict'] = 'Updater.begin() would refuse: new image does not fit next to the running one'
        write_result(a.out, result)
        print(result['verdict'])
        return 1

    with tempfile.TemporaryDirectory(prefix='esp-ota-') as tmp:
        return rehearse(a, B, payload, slot, result, tmp)


def write_result(path, result):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w') as stream:
        json.dump(result, stream, indent=1)
        stream.write('\n')


def rehearse(a, B, payload, slot, result, tmp):
    up, flash_out = os.path.join(tmp, 'upload.bin'), os.path.join(tmp, 'flash.bin')
    Path(up).write_bytes(payload)
    script = os.path.join(tmp, 'script.txt')
    t = a.stage_at_ms
    with open(script, 'w') as f:
        f.write('%g flashw %x %s\n' % (t, slot, up))
        f.write('%g rtcw 200 %s\n' % (t, eboot_command(slot, 0, len(payload)).hex()))
        f.write('%g softreset\n' % (t + 1))
    exe = os.path.join(ROOT, 'build', 'esp8266emu')
    subprocess.run(['make', '-s', '-C', ROOT], check=True, timeout=300)
    uart_log = os.path.join(tmp, 'uart.log')
    cmd = [exe, '--flash', a.running, '--flash-out', flash_out, '--speed', '0', '--max-seconds', str(a.seconds),
           '--no-stdin', '--script', script, '--uart-log', uart_log]
    t0 = time.monotonic()
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=600)
    wall = time.monotonic() - t0
    events = [json.loads(l) for l in out.stdout.decode('utf-8', 'replace').splitlines() if l.startswith('{')]
    uart = Path(uart_log).read_bytes().decode('latin-1')
    flash = Path(flash_out).read_bytes() if os.path.exists(flash_out) else b''

    stages = [(e['s'], e['boot'], round(e['t_us'] / 1000, 3)) for e in events if e['t'] == 'stage']
    halts = [e['m'] for e in events if e['t'] == 'halt']
    second = uart.split(' ets Jan  8 2013', 2)
    eboot_line = ''
    if len(second) > 2:
        for line in second[2].splitlines():
            if line.startswith('@') or line.startswith('~') or 'cp:' in line or line.startswith('ld') or line.startswith('e:'):
                eboot_line += line + ' | '
    copied = flash[:len(B)] == B
    erased_tail = flash[len(B):round4k(len(B))] == b'\xff' * (round4k(len(B)) - len(B))
    # The target ELF is optional: SDK entry is observable without Arduino hooks.
    booted = [s for s in stages if s[1] == 2 and s[0] in ('sdk', 'setup', 'loop')]
    expect_ok = bool(len(second) > 2 and a.expect in second[2]) if a.expect else None
    result.update({
        'emulator': {'git_parent': source_commit(a.emu_commit),
                     'host_wall_seconds': round(wall, 2), 'script': Path(script).read_text().replace(tmp, '<tmp>').splitlines()},
        'stages': stages, 'halt': halts,
        'eboot_output_after_restart': eboot_line.strip(' |'),
        'flash_0x0_equals_new_image': copied,
        'flash_after_image_in_last_sector_is_ff': erased_tail,
        'new_image_reached_sdk_main_loop': bool(booted),
        'expected_text_seen': expect_ok,
        'uart0_masked_after_restart': mask_text(second[2]).splitlines()[:80] if len(second) > 2 else [],
    })
    # '@' = valid command, 'cp:' copy, optional 'B' = bootloader sector identical so eboot skipped it, then the result
    copy_ok = bool(re.match(r'@cp:B?0( |$)', eboot_line))
    result['eboot_copy_result_ok'] = copy_ok
    ok = copied and erased_tail and bool(booted) and copy_ok and not halts and expect_ok is not False
    result['verdict'] = 'PASS' if ok else 'FAIL'
    write_result(a.out, result)
    print('%s: %s; slot %s, eboot "%s", flash==new %s, booted %s, expect %s' % (
        a.out, result['verdict'], hex(slot), result['eboot_output_after_restart'], copied, booted[:1], expect_ok))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(cli(main))
