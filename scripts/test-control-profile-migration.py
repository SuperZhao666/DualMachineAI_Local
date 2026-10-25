"""Compile the real ControlProfile constructor with isolated preference substitutes.

Requires JDK 17+ and the checked-in JSON test jar. Does not require an Android
device/SDK, load JNI, read user preferences, or install anything.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=root / 'audit/report-fixes-20261025/java/profile-migration-result.json')
    args = parser.parse_args()
    source_root = root / 'android_inference_benchmark/app/src/main/java/com/visionforge/inferencebenchmark'
    fixture = root / 'android_inference_benchmark/app/src/test/fixtures/control-profile-migration'
    names = ['ControlProfile', 'ControlProfilePort', 'MobileAimTarget', 'MobileModelCatalog',
             'MobileProfileStorageKeys', 'ModelViewportPreferenceMigration', 'MobileControlTuning',
             'ControlTrigger', 'PersonalTrajectoryProfile']
    sources = [source_root / (name + '.java') for name in names]
    sources.extend(sorted(fixture.rglob('*.java')))
    jar = root / 'vendor/java-tests/json-20180813.jar'
    for path in [*sources, jar]:
        if not path.is_file():
            raise SystemExit(f'Required source/dependency missing: {path}')
    java_home = Path(os.environ['JAVA_HOME']) / 'bin' if os.environ.get('JAVA_HOME') else None
    def executable(name):
        local = java_home / (name + ('.exe' if os.name == 'nt' else '')) if java_home else None
        resolved = str(local) if local and local.is_file() else shutil.which(name)
        if not resolved:
            raise SystemExit('JDK 17+ is required; set JAVA_HOME or add javac/java to PATH')
        return resolved
    javac, java = executable('javac'), executable('java')
    manifest = {str(path.relative_to(root)).replace('\\', '/'): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in [*sources, jar, Path(__file__).resolve()]}
    work_root = (root / 'build/java-profile-migration').resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    results = []
    passed = False
    with tempfile.TemporaryDirectory(prefix='run-', dir=work_root) as temporary:
        classes = Path(temporary).resolve()
        # Only this script-owned temporary tree is automatically removed.
        classes.relative_to(work_root)
        commands = [
            [javac, '--release', '17', '-encoding', 'UTF-8', '-cp', str(jar), '-d', str(classes),
             *map(str, sources)],
            [java, '-ea', '-cp', os.pathsep.join([str(classes), str(jar)]),
             'com.visionforge.inferencebenchmark.ControlProfileMigrationFixture'],
        ]
        for command in commands:
            result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace')
            results.append({'stage': 'compile' if len(results) == 0 else 'constructor_fixture',
                            'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
            print(result.stdout, end='')
            if result.stderr:
                print(result.stderr, end='')
            if result.returncode != 0:
                break
        passed = len(results) == 2 and all(item['returncode'] == 0 for item in results)
    report = {'passed': passed, 'scope': 'host-side real production constructor with in-memory SharedPreferences',
              'android_device_test': False, 'production_sources_modified': False,
              'source_sha256': manifest, 'stages': results}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Result: {args.output}')
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
