"""F17-F21 regressions. Uses synthetic traces, temporary files and controller stubs only."""
import csv
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(os.environ.get('VF_TEST_REPO', Path(__file__).resolve().parents[1]))
SCRIPTS = ROOT / 'scripts'
sys.path.insert(0, str(SCRIPTS))


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DiagnosticsTests(unittest.TestCase):
    def run_cli(self, name, rows):
        with tempfile.TemporaryDirectory() as folder:
            trace, output = Path(folder) / 'trace.csv', Path(folder) / 'result.json'
            with trace.open('w', newline='', encoding='utf-8') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            result = subprocess.run([sys.executable, str(SCRIPTS / name), str(trace), '--output', str(output)],
                                    capture_output=True, text=True, encoding='utf-8', errors='replace')
            report = json.loads(output.read_text(encoding='utf-8')) if output.exists() else None
            return result, report

    def rows(self, width=640, height=640, accepted=0, **extra):
        return [dict(trace_id='fixture', id=i, lock_id=1, track_id=1, flags=1 | 8,
                     observed_us=1_000_000+i*10000, processed_us=1_023_000+i*10000,
                     target_x=width/2, target_y=height/2, dx=3, dy=0,
                     velocity_x=0, velocity_y=0, predicted_error_x=0, predicted_error_y=0,
                     pending_x=0, pending_y=0, background_x=0, background_y=0,
                     accepted_at_us=1_000_000+i*10000 if i < accepted else 0,
                     accepted_dx=3 if i < accepted else 0, accepted_dy=0,
                     coordinate_width=width, coordinate_height=height, model_width=640, model_height=640,
                     **extra) for i in range(80)]

    def test_centers_for_control_not_model_dimensions(self):
        for width, height in [(640, 640), (416, 416), (1024, 1024), (1024, 640)]:
            with self.subTest(dimensions=(width, height)):
                result, report = self.run_cli('analyze-control-dynamics.py', self.rows(width, height, accepted=1))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(report['locks_after_250ms'])
                self.assertEqual(report['locks_after_250ms'][0]['error_p95'], 0)

    def test_crossing_center_is_not_same_side(self):
        for width in (416, 640, 1024):
            rows = self.rows(width, 640)[:2]
            rows[0].update(target_x=width/2-1, dx=3)
            rows[1].update(target_x=width/2+1, dx=-3)
            result, report = self.run_cli('analyze-control-jitter.py', rows)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(report['axes']['x']['strong_adjacent_reversals'], 1)
            self.assertEqual(report['axes']['x']['reversals_with_position_still_on_same_side'], 0)

    def test_legacy_dimensions_remain_416(self):
        rows = self.rows(416, 416, accepted=1)
        for row in rows:
            for key in ('coordinate_width', 'coordinate_height', 'model_width', 'model_height'):
                del row[key]
        result, report = self.run_cli('analyze-control-dynamics.py', rows)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report['locks_after_250ms'][0]['error_p50'], 0)

    def test_invalid_supplied_dimension_is_rejected(self):
        for dimension in (0, -1, 'nan', 'inf', '', None):
            for script in ('analyze-control-dynamics.py', 'analyze-control-jitter.py'):
                rows = self.rows(accepted=1)
                rows[0]['coordinate_width'] = dimension
                result, report = self.run_cli(script, rows)
                self.assertNotEqual(result.returncode, 0)
                self.assertIsNone(report)
                self.assertIn('invalid coordinate_width', result.stderr)

    def test_no_accepted_output_still_generates_diagnostics(self):
        for count in (0, 1, 3):
            result, report = self.run_cli('analyze-control-dynamics.py', self.rows(accepted=count))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(report['api_accepted_reports'], count)
            self.assertEqual(report['frames'], 80)
            self.assertEqual(report['overall_x'], [])
            self.assertEqual(report['overall_y'], [])

    def test_release_and_current_flag_layout_supported_without_future_guess(self):
        from control_trace_flags import ControlFlagInterpreter
        schema = json.loads((SCRIPTS / 'control-trace-schema.json').read_text(encoding='utf-8'))
        gradle = (ROOT / 'android_inference_benchmark/app/build.gradle').read_text(encoding='utf-8')
        current = int(re.search(r'versionCode\s+(\d+)', gradle)[1])
        for code in (65, current):
            interpreter = ControlFlagInterpreter(schema, 7)
            interpreter.consume(dict(process_id=7, trace_id='fixture', event='runtime_session_manifest',
                                     detail=f'version_code={code}'))
            resolved = interpreter.resolve('fixture')
            self.assertEqual(resolved['layout'], 'large_background_v1')
            self.assertEqual(resolved['evidence_level'], 'direct_retained_source')
        for code in (63, 64, current + 100):
            interpreter = ControlFlagInterpreter(schema, 7)
            interpreter.consume(dict(process_id=7, trace_id='unknown', event='runtime_session_manifest',
                                     detail=f'version_code={code}'))
            self.assertIsNone(interpreter.resolve('unknown')['layout'])


class TransactionTests(unittest.TestCase):
    def commit(self, changes, **kwargs):
        if (SCRIPTS / 'file_transaction.py').exists():
            return load('tested_transaction', 'file_transaction.py').commit_files(changes, **kwargs)
        # Exercise the original release commit's ordered-copy behavior for the
        # baseline; no verification gates or real artifacts are executed here.
        for target, source in changes:
            if isinstance(source, bytes):
                target.write_bytes(source)
            else:
                shutil.copy2(source, target)

    def test_release_commit_preserves_previous_pair_if_host_commit_fails(self):
        with tempfile.TemporaryDirectory() as value:
            directory = Path(value)
            old = {name: ('old-'+name).encode() for name in ('app.apk', 'host.exe', 'SHA256SUMS.txt', 'verification.json')}
            for name, data in old.items(): (directory / name).write_bytes(data)
            def replace(source, target):
                if target.name == 'host.exe' and source.name == 'candidate':
                    raise PermissionError('host busy')
                os.replace(source, target)
            with self.assertRaises(PermissionError):
                self.commit([(directory / name, b'new') for name in old], replace_file=replace)
            for name, data in old.items(): self.assertEqual((directory / name).read_bytes(), data)

    def test_every_commit_failure_restores_old_files_and_absent_state(self):
        for failure in ('app.apk', 'host.exe', 'SHA256SUMS.txt', 'verification.json'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as value:
                directory = Path(value)
                names = ('app.apk', 'host.exe', 'SHA256SUMS.txt', 'verification.json')
                for name in names[:-1]: (directory/name).write_bytes(b'old')
                def replace(source, target):
                    if source.name == 'candidate' and target.name == failure:
                        target.write_bytes(b'partial')
                        raise PermissionError('injected commit failure')
                    os.replace(source, target)
                with self.assertRaises(PermissionError):
                    self.commit([(directory/name, b'new') for name in names], replace_file=replace)
                for name in names[:-1]: self.assertEqual((directory/name).read_bytes(), b'old')
                self.assertFalse((directory/names[-1]).exists())

    def test_staging_failure_and_unchanged_host(self):
        with tempfile.TemporaryDirectory() as value:
            directory = Path(value)
            host, apk, source = (directory/name for name in ('host.exe', 'app.apk', 'source.apk'))
            host.write_bytes(b'same'); apk.write_bytes(b'old'); source.write_bytes(b'new')
            def fail_copy(source, destination): raise PermissionError('stage failure')
            with self.assertRaises(PermissionError):
                self.commit([(apk, source)], copy_file=fail_copy)
            self.assertEqual(apk.read_bytes(), b'old')
            touched = []
            def replace(source, destination):
                touched.append(destination.name); os.replace(source, destination)
            self.commit([(apk, source), (host, b'same')], replace_file=replace)
            self.assertEqual(touched, ['app.apk'])

    def test_rollback_failure_retains_backup_and_restores_other_targets(self):
        transaction = load('transaction_rollback', 'file_transaction.py')
        with tempfile.TemporaryDirectory() as value:
            directory = Path(value)
            a, b, c = (directory/name for name in ('a', 'b', 'c'))
            for path in (a, b, c): path.write_bytes(b'old')
            def replace(source, target):
                if source.name == 'candidate' and target == c: raise PermissionError('original failure')
                if source.name == 'restore' and target == b: raise PermissionError('rollback failure')
                os.replace(source, target)
            with self.assertRaises(transaction.FileRollbackError) as caught:
                transaction.commit_files([(p,b'new') for p in (a,b,c)], replace_file=replace)
            self.assertEqual(a.read_bytes(), b'old')
            self.assertEqual(c.read_bytes(), b'old')
            self.assertIsInstance(caught.exception.__cause__, PermissionError)
            self.assertEqual([p for p,_ in caught.exception.failures], [b])
            self.assertTrue(any((p/'previous').exists() for p in caught.exception.backups))

    @unittest.skipUnless(os.name == 'nt', 'Windows share-delete contract')
    def test_real_windows_locked_host_keeps_previous_pair(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        with tempfile.TemporaryDirectory() as value:
            directory = Path(value)
            app, host, sha = (directory/name for name in ('app.apk', 'host.exe', 'SHA256SUMS.txt'))
            for path in (app, host, sha): path.write_bytes(b'old')
            handle = kernel.CreateFileW(str(host), 0x80000000, 1, None, 3, 0, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            try:
                with self.assertRaises(PermissionError):
                    self.commit([(app,b'new'),(host,b'new'),(sha,b'new')])
                for path in (app,host,sha): self.assertEqual(path.read_bytes(),b'old')
            finally:
                kernel.CloseHandle(handle)


class RoutingTests(unittest.TestCase):
    def test_routing_transactions_with_failures_and_success(self):
        for module_name in ('configure-game-routing.py', 'refresh-game-routing.py'):
            for failure in ('script', 'runtime', 'check', 'reload', 'verify', 'metadata', None):
                with self.subTest(module=module_name, failure=failure), tempfile.TemporaryDirectory() as value:
                    module = load('routing_test', module_name)
                    routing = module.routing if hasattr(module, 'routing') else module
                    home = Path(value) / 'home'; backup = Path(value) / 'backup'
                    home.mkdir(); backup.mkdir()
                    originals = {'config.yaml': b'mode: global\n', 'clash-verge.yaml': b'mode: global\n',
                                 'clash-verge-check.yaml': b'old-check', 'script.js': b'old-script'}
                    for name,data in originals.items():
                        (home/name).write_bytes(data); (backup/name).write_bytes(data)
                    (backup/'candidate-script.js').write_bytes(b'new-script')
                    (backup/'candidate.yaml').write_bytes(b'mode: rule\n')
                    audit = Path(value)/'audit.json'
                    state = dict(status='prepared', backup=str(backup), script_relative='script.js',
                                 original_hashes={name:routing.digest(home/name) for name in originals},
                                 default_group='Proxy', residential_selection='residential', us_nodes=['us'])
                    audit.write_text(json.dumps(state), encoding='utf-8')
                    initial_audit = audit.read_bytes()
                    calls=[]
                    def api(method, path, *args):
                        calls.append((method,path))
                        if method == 'PUT':
                            if failure == 'reload' and sum(m=='PUT' for m,_ in calls)==1:
                                raise OSError('reload failed')
                            return None
                        if path == '/configs':
                            if failure == 'verify' and any(m=='PUT' for m,_ in calls):
                                return {'mode':'wrong'}
                            return {'mode':'rule' if any(m=='PUT' for m,_ in calls) else 'global'}
                        if path == '/proxies':
                            return {'proxies': {'GLOBAL':{'now':'Proxy'}, 'Proxy':{'now':'residential'}, routing.GROUP:{'now':'us'}}}
                        if path == '/rules':
                            return {'rules':[{'proxy':routing.GROUP} for _ in range(5)] + [{'type':'Match','proxy':'Proxy'}]}
                        raise AssertionError((method,path))
                    original_commit = getattr(module, 'commit_files', None)
                    def commit(changes, **kwargs):
                        def replace(source, target):
                            mapped={'script':'script.js','runtime':'clash-verge.yaml','check':'clash-verge-check.yaml'}
                            if source.name=='candidate' and target.name==mapped.get(failure):
                                target.write_bytes(b'partial'); raise PermissionError('injected file failure')
                            os.replace(source,target)
                        kwargs['replace_file']=replace
                        return original_commit(changes, **kwargs)
                    original_save=module.save
                    def save(state):
                        if failure=='metadata':
                            audit.write_bytes(b'partial'); raise PermissionError('metadata failed')
                        original_save(state)
                    with patch.object(module,'HOME',home), patch.object(module,'AUDIT',audit), \
                         patch.object(routing,'HOME',home), patch.object(routing,'api',api), \
                         patch.object(module,'save',save):
                        if original_commit:
                            with patch.object(module,'commit_files',commit):
                                if failure: self.assertRaises(Exception, module.apply)
                                else: module.apply()
                        else:
                            self.skipTest('new transaction interface absent on baseline')
                    if failure:
                        for name,data in originals.items(): self.assertEqual((home/name).read_bytes(),data)
                        self.assertEqual(audit.read_bytes(),initial_audit)
                    else:
                        self.assertEqual(json.loads(audit.read_text())['status'],'applied')
                        self.assertEqual((home/'script.js').read_bytes(),b'new-script')


if __name__ == '__main__':
    unittest.main(verbosity=2)
