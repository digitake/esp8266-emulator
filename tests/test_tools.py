"""Evidence failures must fail the command and remove temporary raw UART files."""
import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import evidence
import ota_rehearsal
import regression
import support


class ToolTests(unittest.TestCase):
    @staticmethod
    def manifest_case(**updates):
        case = {'name': 'boot', 'kind': 'evidence', 'bin': 'app.bin', 'reference': 'reference.json'}
        case.update(updates)
        return case

    def test_manifest_rejects_commands_unsafe_names_and_invalid_options(self):
        invalid_cases = [
            [self.manifest_case(command='arbitrary executable')],
            [self.manifest_case(name='../escape')],
            [self.manifest_case(name='comparison')],
            [self.manifest_case(), self.manifest_case()],
            [self.manifest_case(kind='shell')],
            [self.manifest_case(seconds=True)],
            [self.manifest_case(seconds=float('nan'))],
            [self.manifest_case(seconds=0)],
            [self.manifest_case(script=None)],
            [{'name': 'update', 'kind': 'ota', 'running': 'a.bin', 'running_elf': 'a.elf',
              'new': 'b.bin', 'reference': 'ref.json', 'gzip': 'false'}],
        ]
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / 'manifest.json'
            for cases in invalid_cases:
                with self.subTest(cases=cases):
                    path.write_text(json.dumps({'schema': 1, 'cases': cases}))
                    with self.assertRaises(ValueError):
                        regression.load_manifest(path)
            path.write_text(json.dumps({'schema': 1, 'cases': [self.manifest_case()]}))
            self.assertEqual(regression.load_manifest(path), [self.manifest_case()])
            path.write_text(json.dumps({'schema': 1, 'cases': [], 'command': 'unexpected'}))
            with self.assertRaises(ValueError):
                regression.load_manifest(path)

    def test_manifest_runner_uses_fixed_tool_and_preserves_relative_scenario_argument(self):
        with tempfile.TemporaryDirectory() as work:
            base = Path(work).resolve()   # macOS: /var is a link to /private/var; the runner resolves
            image = base / 'app.bin'
            image.write_bytes(b'synthetic image')
            reference = {'firmware': {'bin_sha256': hashlib.sha256(image.read_bytes()).hexdigest()}}
            case = self.manifest_case(script='scenarios/button.txt', profile_file='board.json')
            with patch.object(regression.subprocess, 'run') as run:
                regression.run_case(case, reference, base, base / 'out.json')
            command = run.call_args.args[0]
            self.assertEqual(command[:3], [sys.executable, '-I', str(ROOT / 'scripts/evidence.py')])
            self.assertEqual(command[command.index('--script') + 1], 'scenarios/button.txt')
            self.assertEqual(command[command.index('--profile-file') + 1], str(base / 'board.json'))
            self.assertNotIn('--profile', command)
            self.assertEqual(run.call_args.kwargs['cwd'], base)
            self.assertTrue(run.call_args.kwargs['check'])
            self.assertFalse(run.call_args.kwargs.get('shell', False))
            with patch.object(regression.subprocess, 'run') as run:
                regression.run_case(dict(case, profile='custom-label'), reference, base, base / 'out.json')
            command = run.call_args.args[0]
            self.assertEqual(command[command.index('--profile') + 1], 'custom-label')
            image.write_bytes(b'changed image')
            with patch.object(regression.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                    regression.run_case(case, reference, base, base / 'out.json')
                run.assert_not_called()

    def test_ota_manifest_passes_new_elf_and_checks_its_reference_hash(self):
        with tempfile.TemporaryDirectory() as work:
            base = Path(work).resolve()   # macOS: /var is a link to /private/var; the runner resolves
            paths = {name: base / name for name in ('running.bin', 'running.elf', 'new.bin', 'new.elf')}
            for name, path in paths.items():
                path.write_bytes(name.encode())
            reference = {
                'running': {'sha256': hashlib.sha256(paths['running.bin'].read_bytes()).hexdigest()},
                'new': {'sha256': hashlib.sha256(paths['new.bin'].read_bytes()).hexdigest()},
                'new_elf': {'sha256': hashlib.sha256(paths['new.elf'].read_bytes()).hexdigest()},
            }
            case = {'kind': 'ota', 'name': 'update', 'reference': 'ref.json',
                    'running': 'running.bin', 'running_elf': 'running.elf',
                    'new': 'new.bin', 'new_elf': 'new.elf'}
            manifest = base / 'manifest.json'
            manifest.write_text(json.dumps({'schema': 1, 'cases': [case]}))
            self.assertEqual(regression.load_manifest(manifest), [case])
            with patch.object(regression.subprocess, 'run') as run:
                regression.run_case(case, reference, base, base / 'out.json')
            command = run.call_args.args[0]
            self.assertEqual(command[command.index('--new-elf') + 1], str(paths['new.elf']))
            paths['new.elf'].write_bytes(b'changed ELF')
            with patch.object(regression.subprocess, 'run') as run:
                with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                    regression.run_case(case, reference, base, base / 'out.json')
                run.assert_not_called()

    def test_comparison_excludes_only_provenance_and_preserves_exact_behavior(self):
        reference = {'date_utc': 'old', 'note': '', 'source': '',
                     'firmware': {'source': 'old', 'bin_sha256': 'same'},
                     'emulator': {'git_parent': 'old', 'src_tree_sha256': 'old',
                                  'binary_sha256': 'old', 'host_wall_seconds': 1,
                                  'command': ['emulator', '--speed', '0']},
                     'report': {'loop_calls': 2}, 'stages': [{'t_ms': 1.001}],
                     'pin_edges': [{'level': 1, 't_ms': 2.001}]}
        actual = copy.deepcopy(reference)
        actual['date_utc'] = 'new'
        actual['firmware']['source'] = 'new'
        actual['emulator']['git_parent'] = 'new'
        actual['emulator']['host_wall_seconds'] = 9
        self.assertEqual(regression.differences(regression.deterministic(reference),
                                               regression.deterministic(actual)), [])
        for field, value in (('report', {'loop_calls': 3}),
                             ('stages', [{'t_ms': 1.001001}]),
                             ('pin_edges', [{'level': True, 't_ms': 2.001}])):
            changed = copy.deepcopy(actual)
            changed[field] = value
            self.assertTrue(regression.differences(regression.deterministic(reference),
                                                   regression.deterministic(changed)))
        self.assertIn('git_parent', reference['emulator'])

    def test_manifest_output_cannot_overwrite_reference_through_symlink(self):
        with tempfile.TemporaryDirectory() as work:
            base = Path(work)
            manifest = base / 'manifest.json'
            manifest.write_text(json.dumps({'schema': 1, 'cases': [self.manifest_case()]}))
            reference = base / 'reference.json'
            reference.write_text('{}')
            out = base / 'out'
            out.mkdir()
            (out / 'boot.json').symlink_to(reference)
            argv = ['regression.py', '--manifest', str(manifest), '--out', str(out), '--compare-only']
            with patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    regression.main()
            self.assertEqual(error.exception.code, 2)
            self.assertEqual(reference.read_text(), '{}')

    def test_compare_only_manifest_reports_exact_then_real_edge_difference(self):
        with tempfile.TemporaryDirectory() as work:
            base = Path(work)
            manifest = base / 'manifest.json'
            manifest.write_text(json.dumps({'schema': 1, 'cases': [self.manifest_case()]}))
            reference = {'emulator': {'git_parent': 'reference'},
                         'pin_edges': [{'pin': 'GPIO2', 'level': 0, 't_ms': 1.001}]}
            (base / 'reference.json').write_text(json.dumps(reference))
            out = base / 'out'
            out.mkdir()
            actual = copy.deepcopy(reference)
            actual['emulator']['git_parent'] = 'current'
            record = out / 'boot.json'
            argv = ['regression.py', '--manifest', str(manifest), '--out', str(out), '--compare-only']
            for changed in (False, True):
                with self.subTest(changed=changed):
                    if changed:
                        actual['pin_edges'][0]['t_ms'] = 1.002
                    record.write_text(json.dumps(actual))
                    with patch.object(sys, 'argv', argv), \
                            patch.object(regression.subprocess, 'run') as run, \
                            contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(regression.main(), 1 if changed else 0)
                    run.assert_not_called()
                    report = json.loads((out / 'comparison.json').read_text())
                    self.assertEqual(report['exact'], not changed)
                    self.assertEqual(report['results']['boot']['exact'], not changed)
                    if changed:
                        self.assertIn('$.pin_edges[0].t_ms', report['results']['boot']['differences'][0])

    def test_commit_precedence_and_source_archive_fallback(self):
        with patch.dict(os.environ, {'EMU_COMMIT': 'environment'}), \
                patch.object(support.subprocess, 'check_output') as git:
            self.assertEqual(support.source_commit('explicit'), 'explicit')
            self.assertEqual(support.source_commit(), 'environment')
            git.assert_not_called()
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(support.subprocess, 'check_output', return_value='head\n') as git:
            self.assertEqual(support.source_commit(), 'head')
            self.assertEqual(git.call_args.kwargs['timeout'], 5)
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(support.subprocess, 'check_output', side_effect=FileNotFoundError):
            self.assertIsNone(support.source_commit())

    def test_failed_emulator_is_nonzero_and_removes_raw_temporary_files(self):
        errors = [subprocess.CalledProcessError(7, ['emulator'], stderr=b'private UART'),
                  subprocess.TimeoutExpired(['emulator'], 120, stderr=b'private UART')]
        real_temporary_directory = tempfile.TemporaryDirectory
        for error in errors:
            with self.subTest(error=type(error).__name__), real_temporary_directory() as work:
                created = []

                def temporary_directory(**kwargs):
                    directory = real_temporary_directory(dir=work, **kwargs)
                    created.append(Path(directory.name))
                    (created[-1] / 'uart.log').write_text('private UART')
                    return directory

                out = Path(work) / 'evidence.json'
                argv = ['evidence.py', '--bin', 'test.bin', '--out', str(out)]
                stderr = io.StringIO()
                with patch.object(sys, 'argv', argv), \
                        patch.object(evidence.tempfile, 'TemporaryDirectory', temporary_directory), \
                        patch.object(evidence.subprocess, 'run', side_effect=[None, error]) as run, \
                        contextlib.redirect_stderr(stderr):
                    self.assertEqual(support.cli(evidence.main), 1)
                self.assertTrue(run.call_args.kwargs['check'])
                self.assertIn('error:', stderr.getvalue())
                self.assertNotIn('private UART', stderr.getvalue())
                self.assertFalse(out.exists())
                self.assertEqual(len(created), 1)
                self.assertFalse(created[0].exists())

    def test_ota_refusal_is_nonzero_and_creates_output_directory(self):
        with tempfile.TemporaryDirectory() as work:
            running, new = Path(work) / 'running.bin', Path(work) / 'new.bin'
            running.write_bytes(b'A' * 4096)
            new.write_bytes(b'B' * 8192)
            output = Path(work) / 'nested/result.json'
            argv = ['ota_rehearsal.py', '--running', str(running), '--new', str(new),
                    '--running-elf', 'test.elf', '--out', str(output)]
            with patch.object(sys, 'argv', argv), \
                    patch.object(ota_rehearsal, 'read_symbols', return_value={'_FS_start': 0x40201000}), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(ota_rehearsal.main(), 1)
            self.assertFalse(json.loads(output.read_text())['layout']['fits'])

    def test_ota_failed_copy_or_missing_boot2_expected_text_is_nonzero(self):
        for copy_ok in (False, True):
            with self.subTest(copy_ok=copy_ok), tempfile.TemporaryDirectory() as work:
                directory = Path(work)
                target = b'new firmware'
                contents = target if copy_ok else b'old firmware'
                (directory / 'flash.bin').write_bytes(contents + b'\xff' * (4096 - len(contents)))
                (directory / 'uart.log').write_text(
                    ' ets Jan  8 2013\nboot\n ets Jan  8 2013\n@cp:0\n')
                args = SimpleNamespace(stage_at_ms=1500, running='test.bin', seconds=6,
                                       expect='new image marker', emu_commit='test',
                                       out=str(directory / 'result.json'))
                events = json.dumps({'t': 'stage', 's': 'sdk', 'boot': 2, 't_us': 1500000})
                result = subprocess.CompletedProcess(['emulator'], 0, stdout=events.encode())
                with patch.object(ota_rehearsal.subprocess, 'run', side_effect=[None, result]), \
                        contextlib.redirect_stdout(io.StringIO()):
                    status = ota_rehearsal.rehearse(args, target, target, 4096, {}, work)
                self.assertEqual(status, 1)
                record = json.loads(Path(args.out).read_text())
                self.assertEqual(record['verdict'], 'FAIL')
                self.assertFalse(record['expected_text_seen'])

    def test_ota_rejects_marker_from_later_boot_and_any_unexpected_reset(self):
        for later_reset in (False, True):
            with self.subTest(later_reset=later_reset), tempfile.TemporaryDirectory() as work:
                directory = Path(work)
                target = b'new firmware'
                (directory / 'flash.bin').write_bytes(target + b'\xff' * (4096 - len(target)))
                uart = ' ets Jan  8 2013\nold app\n ets Jan  8 2013\n@cp:0\n'
                if later_reset:
                    uart += 'new image marker\n ets Jan  8 2013\nreset\n'
                else:
                    uart += ' ets Jan  8 2013\nnew image marker\n'
                (directory / 'uart.log').write_text(uart)
                args = SimpleNamespace(stage_at_ms=1500, running='test.bin', seconds=6,
                                       expect='new image marker', emu_commit='test',
                                       out=str(directory / 'result.json'))
                events = [{'t': 'stage', 's': 'sdk', 'boot': 2, 't_us': 1500000}]
                if later_reset:
                    events.append({'t': 'stage', 's': 'rom', 'boot': 3, 't_us': 2500000})
                result = subprocess.CompletedProcess(
                    ['emulator'], 0,
                    stdout='\n'.join(json.dumps(event) for event in events).encode())
                with patch.object(ota_rehearsal.subprocess, 'run', side_effect=[None, result]), \
                        contextlib.redirect_stdout(io.StringIO()):
                    status = ota_rehearsal.rehearse(args, target, target, 4096, {}, work)
                self.assertEqual(status, 1)
                record = json.loads(Path(args.out).read_text())
                self.assertEqual(record['expected_text_seen'], later_reset)
                self.assertTrue(record['unexpected_later_reset'])

    def test_ota_passes_with_boot2_marker_and_optional_new_elf_loop_hook(self):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work)
            target = b'new firmware'
            (directory / 'flash.bin').write_bytes(target + b'\xff' * (4096 - len(target)))
            (directory / 'uart.log').write_text(
                ' ets Jan  8 2013\nold app\n ets Jan  8 2013\n'
                '@cp:0\nnew image marker\n')
            new_elf = directory / 'new.elf'
            new_elf.write_bytes(b'fixture ELF')
            args = SimpleNamespace(stage_at_ms=1500, running='test.bin', seconds=6,
                                   expect='new image marker', emu_commit='test',
                                   new_elf=str(new_elf), out=str(directory / 'result.json'))
            events = [{'t': 'stage', 's': 'sdk', 'boot': 2, 't_us': 1500000},
                      {'t': 'stage', 's': 'setup', 'boot': 2, 't_us': 1600000},
                      {'t': 'stage', 's': 'loop', 'boot': 2, 't_us': 1700000}]
            result = subprocess.CompletedProcess(
                ['emulator'], 0,
                stdout='\n'.join(json.dumps(event) for event in events).encode())
            with patch.object(ota_rehearsal, 'arduino_hooks', return_value=(0x40100000, 0x40100010)), \
                    patch.object(ota_rehearsal.subprocess, 'run', side_effect=[None, result]), \
                    contextlib.redirect_stdout(io.StringIO()):
                status = ota_rehearsal.rehearse(args, target, target, 4096, {}, work)
            self.assertEqual(status, 0)
            record = json.loads(Path(args.out).read_text())
            self.assertEqual(record['verdict'], 'PASS')
            self.assertTrue(record['expected_text_seen'])
            self.assertTrue(record['new_image_reached_sdk_main_loop'])
            self.assertTrue(record['application_loop_observed'])
            self.assertEqual(record['application_readiness'], 'confirmed')

    def test_ota_fails_when_new_elf_loop_hook_is_missing(self):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work)
            target = b'new firmware'
            (directory / 'flash.bin').write_bytes(target + b'\xff' * (4096 - len(target)))
            (directory / 'uart.log').write_text(
                ' ets Jan  8 2013\nold app\n ets Jan  8 2013\n'
                '@cp:0\nnew image marker\n')
            new_elf = directory / 'new.elf'
            new_elf.write_bytes(b'fixture ELF')
            args = SimpleNamespace(stage_at_ms=1500, running='test.bin', seconds=6,
                                   expect='new image marker', emu_commit='test',
                                   new_elf=str(new_elf), out=str(directory / 'result.json'))
            events = [{'t': 'stage', 's': 'sdk', 'boot': 2, 't_us': 1500000},
                      {'t': 'stage', 's': 'setup', 'boot': 2, 't_us': 1600000}]
            result = subprocess.CompletedProcess(
                ['emulator'], 0,
                stdout='\n'.join(json.dumps(event) for event in events).encode())
            with patch.object(ota_rehearsal, 'arduino_hooks', return_value=(0x40100000, 0x40100010)) as hooks, \
                    patch.object(ota_rehearsal.subprocess, 'run', side_effect=[None, result]), \
                    contextlib.redirect_stdout(io.StringIO()):
                status = ota_rehearsal.rehearse(args, target, target, 4096, {}, work)
            self.assertEqual(status, 1)
            hooks.assert_called_once_with(str(new_elf))
            record = json.loads(Path(args.out).read_text())
            self.assertEqual(record['verdict'], 'FAIL')
            self.assertTrue(record['expected_text_seen'])
            self.assertFalse(record['application_loop_observed'])
            self.assertEqual(record['application_readiness'], 'not_reached')

    def test_ota_stage_only_unexpected_reset_makes_readiness_unstable(self):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work)
            target = b'new firmware'
            (directory / 'flash.bin').write_bytes(target + b'\xff' * (4096 - len(target)))
            (directory / 'uart.log').write_text(
                ' ets Jan  8 2013\nold app\n ets Jan  8 2013\n'
                '@cp:0\nnew image marker\n')
            args = SimpleNamespace(stage_at_ms=1500, running='test.bin', seconds=6,
                                   expect='new image marker', emu_commit='test',
                                   out=str(directory / 'result.json'))
            events = [{'t': 'stage', 's': 'sdk', 'boot': 2, 't_us': 1500000},
                      {'t': 'stage', 's': 'setup', 'boot': 2, 't_us': 1600000},
                      {'t': 'stage', 's': 'loop', 'boot': 2, 't_us': 1700000},
                      {'t': 'stage', 's': 'rom', 'boot': 3, 't_us': 2500000}]
            result = subprocess.CompletedProcess(
                ['emulator'], 0,
                stdout='\n'.join(json.dumps(event) for event in events).encode())
            with patch.object(ota_rehearsal.subprocess, 'run', side_effect=[None, result]), \
                    contextlib.redirect_stdout(io.StringIO()):
                status = ota_rehearsal.rehearse(args, target, target, 4096, {}, work)
            self.assertEqual(status, 1)
            record = json.loads(Path(args.out).read_text())
            self.assertEqual(record['uart_boot_banners'], 2)
            self.assertTrue(record['unexpected_later_reset'])
            self.assertEqual(record['application_readiness'], 'not_stable')

    def test_ota_requires_erased_sector_tail_and_no_halt(self):
        for dirty_tail, halted in ((False, False), (True, False), (False, True)):
            with self.subTest(dirty_tail=dirty_tail, halted=halted), tempfile.TemporaryDirectory() as work:
                directory = Path(work)
                target = b'new firmware'
                tail = bytes([0 if dirty_tail else 0xff]) * (4096 - len(target))
                (directory / 'flash.bin').write_bytes(target + tail)
                (directory / 'uart.log').write_text(
                    ' ets Jan  8 2013\nboot\n ets Jan  8 2013\n@cp:0\n')
                args = SimpleNamespace(stage_at_ms=1500, running='test.bin', seconds=6,
                                       expect=None, emu_commit='test',
                                       out=str(directory / 'result.json'))
                events = [{'t': 'stage', 's': 'sdk', 'boot': 2, 't_us': 1500000}]
                if halted:
                    events.append({'t': 'halt', 'm': 'guest execution halted after SDK entry'})
                stdout = '\n'.join(json.dumps(event) for event in events).encode()
                result = subprocess.CompletedProcess(['emulator'], 0, stdout=stdout)
                with patch.object(ota_rehearsal.subprocess, 'run', side_effect=[None, result]), \
                        contextlib.redirect_stdout(io.StringIO()):
                    status = ota_rehearsal.rehearse(args, target, target, 4096, {}, work)
                success = not dirty_tail and not halted
                self.assertEqual(status, 0 if success else 1)
                record = json.loads(Path(args.out).read_text())
                self.assertEqual(record['verdict'], 'PASS' if success else 'FAIL')
                self.assertTrue(record['flash_0x0_equals_new_image'])
                self.assertTrue(record['new_image_reached_sdk_main_loop'])
                self.assertIsNone(record['application_loop_observed'])
                self.assertEqual(record['application_readiness'], 'unknown')
                self.assertEqual(record['flash_after_image_in_last_sector_is_ff'], not dirty_tail)
                self.assertEqual(bool(record['halt']), halted)


if __name__ == '__main__':
    unittest.main()
