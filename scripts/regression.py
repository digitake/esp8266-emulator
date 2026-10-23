"""Run user-supplied firmware cases and compare deterministic evidence exactly.

A JSON manifest selects the built-in evidence or OTA runner, input files, run
options and one reference JSON per case. Paths are relative to the manifest.
No firmware images or application-specific references are bundled.

Only provenance, wall time and editorial text are excluded from comparison.
Guest times, instruction/loop counts and pin edges have no rounding tolerance.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from support import cli

COMMON_FIELDS = {'name', 'kind', 'reference', 'seconds'}
CASE_FIELDS = {
    'evidence': COMMON_FIELDS | {'bin', 'elf', 'profile', 'profile_file', 'script', 'milestone'},
    'ota': COMMON_FIELDS | {'running', 'running_elf', 'new', 'new_elf', 'gzip', 'stage_at_ms', 'expect'},
}
REQUIRED_FIELDS = {
    'evidence': {'name', 'kind', 'reference', 'bin'},
    'ota': {'name', 'kind', 'reference', 'running', 'running_elf', 'new'},
}
STRING_FIELDS = {'name', 'kind', 'reference', 'bin', 'elf', 'profile', 'profile_file',
                 'script', 'milestone', 'running', 'running_elf', 'new', 'new_elf', 'expect'}


def load_manifest(path):
    manifest = json.loads(path.read_text())
    if not isinstance(manifest, dict) or set(manifest) != {'schema', 'cases'}:
        raise ValueError('manifest requires exactly schema and cases fields')
    if type(manifest['schema']) is not int or manifest['schema'] != 1:
        raise ValueError('manifest schema must be 1')
    cases = manifest['cases']
    if not isinstance(cases, list) or not cases:
        raise ValueError('manifest cases must be a nonempty list')
    seen = set()
    for index, case in enumerate(cases):
        if not isinstance(case, dict):
            raise ValueError('case %d must be an object' % index)
        kind = case.get('kind')
        if not isinstance(kind, str) or kind not in CASE_FIELDS:
            raise ValueError('case %d kind must be evidence or ota' % index)
        unknown = set(case) - CASE_FIELDS[kind]
        missing = REQUIRED_FIELDS[kind] - set(case)
        if unknown or missing:
            raise ValueError('case %d has unknown fields %s or missing fields %s' %
                             (index, sorted(unknown), sorted(missing)))
        for key in STRING_FIELDS & set(case):
            if not isinstance(case[key], str) or not case[key] or '\0' in case[key]:
                raise ValueError('case %d %s must be a nonempty string without NUL' % (index, key))
        name = case['name']
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,79}', name) or name == 'comparison':
            raise ValueError('case name must be a safe filename (comparison is reserved)')
        if name in seen:
            raise ValueError('duplicate case name: ' + name)
        seen.add(name)
        for key in ('seconds', 'stage_at_ms'):
            if key not in case:
                continue
            value = case[key]
            if type(value) not in (int, float) or not math.isfinite(value):
                raise ValueError('case %s %s must be finite numeric' % (name, key))
            if value < 0 or (key == 'seconds' and value == 0):
                raise ValueError('case %s %s is outside its allowed range' % (name, key))
        if 'gzip' in case and type(case['gzip']) is not bool:
            raise ValueError('case %s gzip must be boolean' % name)
    return cases


def deterministic(record):
    result = copy.deepcopy(record)
    for key in ('date_utc', 'note', 'source'):
        result.pop(key, None)
    result.get('firmware', {}).pop('source', None)
    for key in ('git_parent', 'src_tree_sha256', 'binary_sha256', 'host_wall_seconds'):
        result.get('emulator', {}).pop(key, None)
    return result


def differences(expected, actual, path='$'):
    if type(expected) is not type(actual):
        return [path + ': type differs']
    if isinstance(expected, dict):
        result = []
        for key in sorted(expected.keys() | actual.keys()):
            child = path + '.' + key
            if key not in expected or key not in actual:
                result.append(child + ': missing/extra key')
            else:
                result.extend(differences(expected[key], actual[key], child))
        return result
    if isinstance(expected, list):
        result = [] if len(expected) == len(actual) else [path + ': length differs']
        for i, (left, right) in enumerate(zip(expected, actual)):
            result.extend(differences(left, right, path + '[%d]' % i))
        return result
    return [] if expected == actual else [path + ': %r -> %r' % (expected, actual)]


def check_hash(path, expected):
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise ValueError('firmware hash mismatch: ' + str(path))


def resolve_path(base, value):
    return (base / value).resolve()


def run_case(case, reference, base, output):
    if case['kind'] == 'ota':
        image = resolve_path(base, case['running'])
        elf = resolve_path(base, case['running_elf'])
        new = resolve_path(base, case['new'])
        check_hash(image, reference['running']['sha256'])
        check_hash(new, reference['new']['sha256'])
        cmd = [sys.executable, '-I', str(ROOT / 'scripts/ota_rehearsal.py'),
               '--running', str(image), '--running-elf', str(elf), '--new', str(new),
               '--seconds', str(case.get('seconds', 6)),
               '--stage-at-ms', str(case.get('stage_at_ms', 1500))]
        if case.get('gzip', False):
            cmd += ['--gzip']
        if 'expect' in case:
            cmd += ['--expect', case['expect']]
        if 'new_elf' in case:
            new_elf = resolve_path(base, case['new_elf'])
            check_hash(new_elf, reference['new_elf']['sha256'])
            cmd += ['--new-elf', str(new_elf)]
    else:
        image = resolve_path(base, case['bin'])
        check_hash(image, reference['firmware']['bin_sha256'])
        cmd = [sys.executable, '-I', str(ROOT / 'scripts/evidence.py'), '--bin', str(image),
               '--seconds', str(case.get('seconds', 10)),
               '--milestone', case.get('milestone', reference.get('milestone', 'BOOT'))]
        if 'profile' in case:
            cmd += ['--profile', case['profile']]
        if 'elf' in case:
            elf = resolve_path(base, case['elf'])
            check_hash(elf, reference['firmware']['elf_sha256'])
            cmd += ['--elf', str(elf)]
        if 'profile_file' in case:
            cmd += ['--profile-file', str(resolve_path(base, case['profile_file']))]
        if 'script' in case:
            # Preserve the user's path spelling in evidence; cwd resolves it.
            cmd += ['--script', case['script']]
    timeout = max(660, case.get('seconds', 10) * 20 + 60)
    subprocess.run(cmd + ['--out', str(output)], cwd=base, check=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--compare-only', action='store_true')
    parser.add_argument('--case', action='append', dest='cases', help='case name; repeat to select several')
    args = parser.parse_args()
    args.manifest = args.manifest.resolve()
    args.out = args.out.resolve()
    cases = load_manifest(args.manifest)
    base = args.manifest.parent
    protected = {args.manifest}
    for case in cases:
        for key in ('reference', 'bin', 'elf', 'profile_file', 'script', 'running', 'running_elf', 'new', 'new_elf'):
            if key in case:
                protected.add(resolve_path(base, case[key]))
    if args.cases:
        unknown = set(args.cases) - {case['name'] for case in cases}
        if unknown:
            parser.error('unknown case names: ' + ', '.join(sorted(unknown)))
        cases = [case for case in cases if case['name'] in args.cases]
    # Resolve every reference before running anything, and never overwrite one.
    references = {case['name']: resolve_path(base, case['reference']) for case in cases}
    outputs = {(args.out / (case['name'] + '.json')).resolve() for case in cases}
    outputs.add((args.out / 'comparison.json').resolve())
    if protected & outputs:
        parser.error('output files must differ from the manifest and all input files')
    records = {name: json.loads(path.read_text()) for name, path in references.items()}
    if any(not isinstance(record, dict) for record in records.values()):
        raise ValueError('reference evidence must be JSON objects')
    if not args.compare_only:
        subprocess.run(['make', '-s'], cwd=ROOT, check=True, timeout=300)
    args.out.mkdir(parents=True, exist_ok=True)
    results = {}
    for case in cases:
        name = case['name']
        reference = records[name]
        output = args.out / (name + '.json')
        if not args.compare_only:
            run_case(case, reference, base, output)
        actual = json.loads(output.read_text())
        delta = differences(deterministic(reference), deterministic(actual))
        results[name] = {'exact': not delta, 'differences': delta}
        print(('DIFF ' if delta else 'EXACT ') + name, flush=True)
        for line in delta[:20]:
            print('  ' + line)
        if len(delta) > 20:
            print('  ... %d additional differences in comparison.json' % (len(delta) - 20))
    (args.out / 'comparison.json').write_text(json.dumps({
        'manifest': str(args.manifest), 'references': {k: str(v) for k, v in references.items()},
        'results': results, 'exact': all(r['exact'] for r in results.values()),
    }, indent=2) + '\n')
    return 0 if all(r['exact'] for r in results.values()) else 1


if __name__ == '__main__':
    sys.exit(cli(main))
