"""Build a Java transport repair while preserving the verified 1.2.10 native payload.

This does not publish or approve the experimental native controller. It fails if
any native library, model, or asset differs from the explicit baseline APK.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASELINE_SHA256 = '3a97b268e81f0f4b32108720962d779fcb2cee3956003f713cc00d72fd616ad4'


def sha(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def payload(path):
    with zipfile.ZipFile(path) as archive:
        return {entry.filename: hashlib.sha256(archive.read(entry)).hexdigest()
                for entry in archive.infolist()
                if not entry.is_dir() and entry.filename.startswith(('lib/', 'assets/'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    baseline = args.baseline.resolve()
    output = args.output.resolve()
    if sha(baseline) != BASELINE_SHA256:
        raise SystemExit('Refusing a different or modified 1.2.10 baseline APK')
    if output == baseline:
        raise SystemExit('The baseline APK must be retained for verification and rollback')
    env = os.environ.copy()
    env['PATH'] = str(Path(env['JAVA_HOME']) / 'bin') + os.pathsep + env['PATH']
    subprocess.run(['cmd', '/c', 'gradlew.bat', ':app:assembleRelease', '--offline',
                    '--console=plain', f'-PnativeBaselineApk={baseline}',
                    f'-PnativeBaselineSha256={BASELINE_SHA256}'],
                   cwd=ROOT / 'android_inference_benchmark', env=env, check=True)
    # These Android behavior tests load the classes compiled by Gradle above.
    subprocess.run(['python', str(ROOT / 'scripts/test-java.py')], cwd=ROOT, env=env, check=True)
    built = ROOT / 'android_inference_benchmark/app/build/outputs/apk/release/app-release.apk'
    expected = payload(baseline)
    actual = payload(built)
    if not expected or expected != actual:
        changes = sorted(name for name in expected.keys() | actual.keys()
                         if expected.get(name) != actual.get(name))
        raise SystemExit('Native/model/asset baseline mismatch: ' + ', '.join(changes))
    sdk = Path(env['ANDROID_HOME'])
    signer = sdk / 'build-tools/35.0.0/apksigner.bat'
    before_signer = subprocess.check_output(['cmd', '/c', str(signer), 'verify', '--print-certs', str(baseline)], text=True)
    after_signer = subprocess.check_output(['cmd', '/c', str(signer), 'verify', '--print-certs', str(built)], text=True)
    cert = lambda text: next(line for line in text.splitlines() if 'certificate SHA-256 digest:' in line)
    if cert(before_signer) != cert(after_signer):
        raise SystemExit('APK signing identity changed')
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, output)
    report = {'scope': 'Java transport repair; native controller is unchanged from 1.2.10',
              'baseline': str(baseline), 'baseline_sha256': BASELINE_SHA256,
              'output': str(output), 'output_sha256': sha(output),
              'signing_certificate': cert(after_signer),
              'native_and_assets_identical': True, 'payload': actual,
              'dynamic_controller_candidate_approved': False}
    output.with_suffix('.verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({key: value for key, value in report.items() if key != 'payload'}, indent=2))


if __name__ == '__main__':
    main()
