"""Run the emulator headless on a firmware image and write an evidence JSON (WSL/Linux, python3, stdlib only).

usage: python3 scripts/evidence.py --bin firmware.bin [--elf firmware.elf] --profile-file examples/gpio-profile.json --seconds 10
                              [--script scenario.txt] --milestone BOOT --out /tmp/boot.json
The UART transcript in the evidence is masked (scripts/mask.py); the raw UART log is never written to the repo.
"""
import argparse, hashlib, json, math, os, subprocess, sys, tempfile, time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from mask import mask_text          # noqa: E402
from elfsyms import arduino_hooks   # noqa: E402
from support import cli, source_commit  # noqa: E402
from profiles import load_profile, pin_roles  # noqa: E402

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 16), b''):
            h.update(b)
    return h.hexdigest()


def tree_hash(paths):
    h = hashlib.sha256()
    for p in sorted(paths):
        h.update(os.path.relpath(p, ROOT).replace(os.sep, '/').encode() + b'\0')
        with open(p, 'rb') as f:
            h.update(f.read())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', required=True)
    ap.add_argument('--elf')
    ap.add_argument('--profile', help='optional label stored in evidence metadata')
    ap.add_argument('--profile-file', help='JSON GPIO layout (default: all pins, no device assumptions)')
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--script')
    ap.add_argument('--milestone', default='M1')
    ap.add_argument('--source', default='')
    ap.add_argument('--note', default='')
    ap.add_argument('--out', required=True)
    ap.add_argument('--emu-commit', help='override source commit (default: EMU_COMMIT or git HEAD)')
    a = ap.parse_args()
    a.pin_profile = load_profile(a.profile_file)
    a.profile = a.profile or a.pin_profile['name']
    if not math.isfinite(a.seconds) or a.seconds <= 0:
        ap.error('--seconds must be a finite positive number')

    exe = os.path.join(ROOT, 'build', 'esp8266emu')
    subprocess.run(['make', '-s', '-C', ROOT], check=True, timeout=300)
    with tempfile.TemporaryDirectory(prefix='esp-evidence-') as tmp:
        return collect(a, exe, tmp)


def collect(a, exe, tmp):
    report, uart_log = os.path.join(tmp, 'report.json'), os.path.join(tmp, 'uart.log')
    cmd = [exe, '--flash', a.bin, '--speed', '0', '--max-seconds', str(a.seconds), '--no-stdin',
           '--report', report, '--uart-log', uart_log]
    hooks = (None, None)
    if a.elf:
        hooks = arduino_hooks(a.elf)
        if hooks[0]:
            cmd += ['--sym-setup', hex(hooks[0])]
        if hooks[1]:
            cmd += ['--sym-loop', hex(hooks[1])]
    if a.script:
        cmd += ['--script', a.script]
    t0 = time.monotonic()
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         check=True, timeout=max(120, a.seconds * 20))
    wall = time.monotonic() - t0
    events = [json.loads(l) for l in out.stdout.decode('utf-8', 'replace').splitlines() if l.startswith('{')]
    uart = Path(uart_log).read_bytes().decode('latin-1')
    rep = json.loads(Path(report).read_text())

    named = pin_roles(a.pin_profile)
    # Custom layouts retain named boot pins; generic evidence records every pin.
    # Modes: o output, p input+pull-up, z input floating, u other function.
    pins, dropped = [], 0
    for e in events:
        if e['t'] != 'edges':
            continue
        dropped += e.get('dropped', 0)
        for t_us, p, lv, md in e['e']:
            if not named or p in named or t_us > 0:
                pins.append({'t_ms': round(t_us / 1000, 3), 'pin': 'GPIO%d%s' % (p, '(' + named[p] + ')' if p in named else ''),
                             'level': lv, 'mode': md})

    stages = [{'stage': e['s'], 'boot': e['boot'], 'reset_reason': e['reason'], 'boot_us': e['vt_us'],
               't_ms': round(e.get('t_us', e['vt_us']) / 1000, 3), 'instructions': e['insn']}
              for e in events if e['t'] == 'stage']
    logs = [e['m'] for e in events if e['t'] == 'log' and 'romfunc' not in e['m']]
    romfuncs = sorted({e['m'].split(']')[0].split('[')[1] for e in events if e['t'] == 'log' and 'romfunc' in e['m']}, key=int)
    halts = [e['m'] for e in events if e['t'] == 'halt']
    src = [os.path.join(ROOT, 'src', f) for f in os.listdir(os.path.join(ROOT, 'src'))]
    ev = {
        'schema': 1,
        'milestone': a.milestone,
        'date_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'note': a.note,
        'firmware': {
            'profile': a.profile, 'source': a.source,
            'bin': os.path.basename(a.bin), 'bin_sha256': sha256(a.bin), 'bin_bytes': os.path.getsize(a.bin),
            'elf_sha256': sha256(a.elf) if a.elf else None,
            'setup_addr': hex(hooks[0]) if hooks[0] else None, 'loop_addr': hex(hooks[1]) if hooks[1] else None,
            'modified': False,
        },
        'emulator': {
            'git_parent': source_commit(a.emu_commit),
            'src_tree_sha256': tree_hash(src),
            'binary_sha256': sha256(exe),
            'command': [os.path.basename(c) if c in (a.bin, exe) else c for c in cmd if not c.startswith(tmp)],
            'script': Path(a.script).read_text().splitlines() if a.script else None,
            'host_wall_seconds': round(wall, 2),
        },
        'report': rep,
        'stages': stages,
        'halt': halts,
        'pin_edges': pins[:2000],
        'pin_edge_count': len(pins),
        'pin_edges_dropped': dropped,
        'logs': logs[:200],
        'phy_romfunc_slots_called': romfuncs,
        'uart0_masked': mask_text(uart).splitlines(),
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w') as f:
        json.dump(ev, f, indent=1)
        f.write('\n')
    print('wrote %s: stage %s, setup %s, loop %s, %d pin edges, %d UART bytes, wall %.1fs' % (
        a.out, rep['stage'], rep['setup_calls'], rep['loop_calls'], len(pins), rep['uart0_bytes'], wall))


if __name__ == '__main__':
    sys.exit(cli(main))
