"""Separate numerical tracking regression from functional/native test success.

Passing this gate only means the candidate did not materially worsen the same
deterministic scenes. It does not validate real-game accuracy or physical HID.
"""
import argparse, hashlib, json, re, subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--candidate', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--shape-baseline', type=Path,
                    help='Required when the candidate includes the shape/occlusion scenarios; compare all eight cases')
parser.add_argument('--run-candidate', action='store_true',
                    help='Build and run the entire candidate servo suite; bind this execution to source/executable hashes')
args = parser.parse_args()
root=Path(__file__).resolve().parents[1]
executable=root/'build/host/vfdual_mobile_observation_servo_tests.exe'
def fingerprints():
    paths=list((root/'android_inference_benchmark/app/src/main/cpp').glob('*.hpp'))
    paths+=list((root/'android_inference_benchmark/app/src/main/cpp').glob('*.cpp'))
    paths+=[root/'dual_machine_runtime/CMakeLists.txt',
            root/'dual_machine_runtime/shared/include/vfdual/model_contract.hpp']
    return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}
source_before=fingerprints()
execution={'verified':False, 'reason':'Imported text is not proof of executing the current source'}
if args.run_candidate:
    build=subprocess.run(['cmd.exe','/d','/c',r'scripts\build-native-target.bat',
                          'vfdual_mobile_observation_servo_tests'],cwd=root,
                         capture_output=True,encoding='utf-8',errors='replace',timeout=180)
    args.candidate.parent.mkdir(parents=True,exist_ok=True)
    build_log=args.candidate.with_suffix('.build.log')
    build_log.write_text(build.stdout+build.stderr,encoding='utf-8')
    if build.returncode: raise RuntimeError(f'Candidate build failed: {build_log}')
    executable_before=hashlib.sha256(executable.read_bytes()).hexdigest()
    run=subprocess.run([str(executable)],cwd=root,capture_output=True,
                       encoding='utf-8',errors='replace',timeout=120)
    args.candidate.write_text(run.stdout+run.stderr,encoding='utf-8')
    immutable=(source_before==fingerprints() and
               executable_before==hashlib.sha256(executable.read_bytes()).hexdigest())
    execution={'verified':run.returncode==0 and immutable,'returncode':run.returncode,
               'source_and_executable_unchanged':immutable,
               'build_log_sha256':hashlib.sha256(build_log.read_bytes()).hexdigest(),
               'test_executable_sha256':executable_before}
pattern = re.compile(r'servo_complex scenario=(\d+) rms=([\d.]+) p95=([\d.]+) switches=(\d+) held=(\d+)')
def read(path, scene='complex'):
    text = path.read_text(encoding='utf-8-sig')
    expression = pattern if scene=='complex' else re.compile(r'servo_shape scenario=(\d+) rms=([\d.]+) p95=([\d.]+) switches=(\d+) held=(\d+)')
    return {int(m[0]): dict(zip(['rms','p95','switches','held'], map(float, m[1:])))
            for m in expression.findall(text)}
baseline, candidate = read(args.baseline), read(args.candidate)
required = {1, 2, 3, 4, 7}
if set(baseline) != required or set(candidate) != required:
    raise ValueError('Missing or unexpected dynamic scenario; cannot pass an incomplete comparison')
failures = []
if args.run_candidate and not execution['verified']:
    failures.append({'metric':'candidate_execution','details':execution})
for scene in sorted(required):
    for metric in ['rms', 'p95']:
        if candidate[scene][metric] > baseline[scene][metric]*1.05 + .1:
            failures.append({'scenario':scene, 'metric':metric,
                'before':baseline[scene][metric], 'after':candidate[scene][metric]})
    if candidate[scene]['switches'] > baseline[scene]['switches']:
        failures.append({'scenario':scene,'metric':'switches'})
shape_validation=None
shape_candidate=read(args.candidate,'shape')
if shape_candidate or args.shape_baseline:
    if not args.shape_baseline:
        raise ValueError('Candidate has shape/occlusion scenarios: --shape-baseline is required, not optional coverage')
    shape_before=read(args.shape_baseline,'shape')
    if set(shape_before)!={0,1,2,3,4,7,16,17} or set(shape_candidate)!=set(shape_before):
        raise ValueError('Missing shape/occlusion scenario; cannot publish incomplete comparison')
    shape_failures=[]
    for scene in sorted(shape_before):
        for metric in ['rms','p95']:
            if shape_candidate[scene][metric]>shape_before[scene][metric]*1.05+.1:
                shape_failures.append({'scope':'shape_occlusion','scenario':scene,'metric':metric,
                    'before':shape_before[scene][metric],'after':shape_candidate[scene][metric]})
        if shape_candidate[scene]['switches']>shape_before[scene]['switches']:
            shape_failures.append({'scope':'shape_occlusion','scenario':scene,'metric':'switches'})
    failures.extend(shape_failures)
    shape_validation={'baseline':str(args.shape_baseline),
        'baseline_sha256':hashlib.sha256(args.shape_baseline.read_bytes()).hexdigest(),
        'before':shape_before,'after':shape_candidate,'failures':shape_failures,
        'status':'reject_dynamic_regression' if shape_failures else 'no_dynamic_regression_detected'}
report = {
    'status':'reject_dynamic_regression' if failures else 'no_dynamic_regression_detected',
    'real_game_acceptance':False,
    'semantics':'Deterministic synthetic scenes; model-space pixels, not measured physical accuracy. A pass is not deployment approval.',
    'regression_limit':'RMS/P95 no more than 5 percent plus 0.1px above the identical baseline scenario; no extra switches',
    'baseline':str(args.baseline), 'candidate':str(args.candidate),
    'baseline_sha256':hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
    'candidate_sha256':hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
    'before':baseline, 'after':candidate, 'failures':failures,
    'candidate_execution':execution,
    'shape_validation':shape_validation,
}
report['source_fingerprints']=source_before
report['test_executable_sha256']=hashlib.sha256(executable.read_bytes()).hexdigest()
args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'status':report['status'],'regressions':len(failures),'output':str(args.output)},ensure_ascii=False))
raise SystemExit(2 if failures else 0)
