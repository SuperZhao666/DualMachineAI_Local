"""Collect only VF diagnostics and artifact identities from the paired phone/Host."""
import argparse, datetime, hashlib, json, os, shutil, subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--device', required=True, help='Current wireless ADB serial')
parser.add_argument('--label', default='live')
parser.add_argument('--max-log-segments',type=int,default=16,choices=range(1,33))
args = parser.parse_args()
label = ''.join(c for c in args.label if c.isalnum() or c in '-_')[:48] or 'live'
started = datetime.datetime.now(datetime.timezone.utc)
out = root / 'audit' / 'live-collections' / (started.strftime('%Y%m%dT%H%M%SZ-') + label)
out.mkdir(parents=True, exist_ok=False)
adb = ['adb', '-s', args.device]
def command(parts):
    result = subprocess.run(parts, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=40)
    return {'exit_code': result.returncode, 'output': (result.stdout + result.stderr).strip()}
def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(1024*1024), b''):
            value.update(block)
    return value.hexdigest()
report = {'started_utc': started.isoformat(), 'device': args.device, 'collection': str(out),
          'scope': 'VF application diagnostics only; sequential snapshots, not synchronized clocks',
          'phone_pulls': {}, 'artifacts': {}, 'files': []}
phone = '/sdcard/Android/data/com.visionforge.mobile.local/files/diagnostics/'
for suffix in ['']+['.'+str(i) for i in range(1,args.max_log_segments)]:
    name = 'visionforge-mobile-runtime.jsonl' + suffix
    report['phone_pulls'][name] = command(adb + ['pull', phone + name, str(out / name)])
host = Path(os.environ['LOCALAPPDATA']) / 'VisionForge' / 'DualMachineLocal'
for pattern in ['host-events*.jsonl*', 'host-metrics-v9.csv']:
    for path in host.glob(pattern):
        shutil.copyfile(path, out / path.name)
report['phone_process'] = command(adb + ['shell', 'pidof', 'com.visionforge.mobile.local'])
report['phone_battery']=command(adb+['shell','dumpsys','battery'])
report['phone_thermal']=command(adb+['shell','dumpsys','thermalservice'])
report['phone_app_exit_history']=command(adb+['shell','dumpsys','activity','exit-info','com.visionforge.mobile.local'])
production=root/'models/overwatch/production.json'
if production.is_file():shutil.copyfile(production,out/'model-production.json')
report['phone_apk_path'] = command(adb + ['shell', 'pm', 'path', 'com.visionforge.mobile.local'])
if report['phone_apk_path']['exit_code'] == 0:
    apk_path = report['phone_apk_path']['output'].removeprefix('package:').splitlines()[0]
    report['installed_apk_sha256'] = command(adb + ['shell', 'sha256sum', apk_path])
for name in ['VisionForge-Local.apk', 'VFHost-Local.exe']:
    path = root / 'dist' / name
    if path.is_file():
        report['artifacts'][name] = {'bytes': path.stat().st_size, 'sha256': digest(path)}
source = {}
for directory in ['android_inference_benchmark/app/src/main', 'dual_machine_runtime/host', 'dual_machine_runtime/shared']:
    for path in (root / directory).rglob('*'):
        if path.is_file() and path.suffix in {'.java', '.cpp', '.hpp', '.h', '.xml'}:
            source[str(path.relative_to(root))] = digest(path)
(out / 'source-fingerprints.json').write_text(json.dumps(source, indent=2), encoding='utf-8')
for path in sorted(out.iterdir()):
    if path.is_file():
        report['files'].append({'file': path.name, 'bytes': path.stat().st_size, 'sha256': digest(path)})
report['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
(out / 'collection.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps({'collection': str(out), 'artifacts': report['artifacts'],
                  'phone_process': report['phone_process'], 'files': len(report['files'])}, ensure_ascii=False, indent=2))
