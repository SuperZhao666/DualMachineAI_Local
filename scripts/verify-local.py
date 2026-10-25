"""Audit the extracted source, packaged APK and matching local Host."""
from pathlib import Path
import hashlib, json, re, zipfile, shutil, argparse, subprocess, datetime, os
from file_transaction import commit_files

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--device', help='Paired ADB serial for installed APK verification')
parser.add_argument('--native-log', type=Path, default=Path('audit/live-20261018/native-rebound-production-build.log'))
parser.add_argument('--live-collection', type=Path, help='Current artifact-bound diagnostic collection')
parser.add_argument('--dynamic-report', type=Path, default=Path('audit/live-20261018/audit-dynamic-gate.json'))
parser.add_argument('--model-report', type=Path, help='Current graph numeric/phone validation, bound to hashes')
parser.add_argument('--pipeline-report', type=Path, required=True, help='Current sustained live pipeline validation bound to APK and Host')
parser.add_argument('--check-only', action='store_true', help='Validate the candidate without publishing dist')
args=parser.parse_args()
dist=root/'dist'
dist.mkdir(exist_ok=True)
apk=root/'android_inference_benchmark/app/build/outputs/apk/release/app-release.apk'
exe=root/'build/host/VFHost.exe'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
source=json.loads((root/'audit/source-manifest.json').read_text())
changed=[]
for row in source['files']:
    p=Path(source['source'])/row['path']
    if not p.is_file() or sha(p)!=row['sha256']: changed.append(row['path'])
forbidden=['DualMachineCardAuthorizationCoordinator','DualMachineAuthorizationRuntime',
           'DualMachineSidecarHttpClient','DualMachineUsageLeaseGate','DualMachineEntitlementStore',
           'visionforge.cloud','activateCard','HostDataPlaneAuthorizationGate','data_plane_permit',
           'pinForFormalSession']
source_hits=[]
for base in ['android_inference_benchmark/app/src/main','dual_machine_runtime/host','dual_machine_runtime/shared']:
    for p in (root/base).rglob('*'):
        if p.suffix not in {'.java','.cpp','.hpp','.h','.xml'}: continue
        value=p.read_text(encoding='utf-8',errors='replace')
        hits=[token for token in forbidden if token in value]
        if hits: source_hits.append({'path':str(p.relative_to(root)),'tokens':hits})
binary_hits=[]
models=[]
with zipfile.ZipFile(apk) as z:
    assert z.testzip() is None, 'APK CRC failed'
    dex=b''.join(z.read(n) for n in z.namelist() if re.fullmatch(r'classes\d*\.dex',n))
    for token in forbidden:
        if token.encode() in dex: binary_hits.append('APK:'+token)
    for p in (root/'android_inference_benchmark/app/src/main/jniLibs/arm64-v8a').glob('*.so'):
        if p.name.startswith('libQnn'): continue
        packaged=z.read('lib/arm64-v8a/'+p.name)
        # AGP strips debug sections; report both rather than assume byte identity.
        models.append({'name':p.name,'source_sha256':sha(p),
                       'apk_sha256':hashlib.sha256(packaged).hexdigest(),'apk_bytes':len(packaged)})
host=exe.read_bytes()
for token in forbidden:
    if token.encode() in host or token.encode('utf-16le') in host:
        binary_hits.append('Host:'+token)
assert not changed, 'Original source changed: '+str(changed)
assert not source_hits, 'Commercial source dependency remains: '+str(source_hits)
assert not binary_hits, 'Commercial binary dependency remains: '+str(binary_hits)
production=json.loads((root/'models/overwatch/production.json').read_text(encoding='utf-8'))
selected=production['selected_model']
expected_library=selected['qnn_library']
assert [m['name'] for m in models]==[expected_library], 'Expected only the selected Overwatch model'
with zipfile.ZipFile(apk) as z:
    bundled=[n for n in z.namelist() if re.fullmatch(r'lib/arm64-v8a/libow_.*\.so',n)]
    assert bundled==['lib/arm64-v8a/'+expected_library], bundled
    portable_name='assets/portable_models/'+selected['name']+'.onnx'
    if portable_name in z.namelist():
        portable=z.read(portable_name)
        assert len(portable)==selected['split_bytes'] and hashlib.sha256(portable).hexdigest()==selected['split_sha256']
    else:
        assert production.get('portable_model_bundled') is False and selected['classes']==1 and production['runtime'].startswith('QNN HTP'), 'Missing model asset without an explicit native-only deployment contract'
    assert all(n==portable_name for n in z.namelist() if n.startswith('assets/portable_models/') and n.endswith('.onnx'))
assert models[0]['source_sha256']==selected['qnn_sha256'], 'Bundled source model mismatch'
assert args.model_report, 'New model requires artifact-bound phone validation'
graph=json.loads(args.model_report.read_text(encoding='utf-8'))
assert graph['status']=='passed_numeric_and_phone_execution' and graph['validation_frames']>=22
assert graph['files'] and all(sha(root/name)==digest for name,digest in graph['files'].items()), 'Model evidence is stale'
pipeline=json.loads(args.pipeline_report.read_text(encoding='utf-8'))
assert pipeline['status']=='passed_current_pipeline' and pipeline['duration_seconds']>=360, 'Sustained current pipeline validation is incomplete'
assert pipeline['apk_sha256']==sha(apk) and pipeline['host_sha256']==sha(exe), 'Pipeline evidence is for other artifacts'
assert pipeline['files'] and all(sha(root/name)==digest for name,digest in pipeline['files'].items()), 'Pipeline evidence is stale'
assert all(not (root/n).exists() for n in ['server','src/main.py','main.py','app_gui.py'])
artifacts=[{'file':name,'bytes':p.stat().st_size,'sha256':sha(p)}
           for name,p in [('VisionForge-Local.apk',apk),('VFHost-Local.exe',exe)]]
native_path=args.native_log if args.native_log.is_absolute() else root/args.native_log
native_log=native_path.read_text(encoding='utf-8',errors='replace')
native_result=re.search(r'100% tests passed, 0 tests failed out of (\d+)',native_log)
assert native_result, 'Native suite did not pass'
skipped=re.findall(r'^\s*\d+\s+-\s+(\w+) \(Skipped\)', native_log, re.M)
selected_tests=set(re.findall(r'^\s*\d+/\d+\s+Test\s+#\d+:\s+(\w+)',native_log,re.M))
test_inventory=subprocess.run(['ctest','--test-dir',str(root/'build/host'),'-N','--show-only=json-v1'],
    capture_output=True,text=True,encoding='utf-8',errors='replace',check=True)
registered_tests={test['name'] for test in json.loads(test_inventory.stdout)['tests']}
assert len(selected_tests)==int(native_result.group(1)) and selected_tests<=registered_tests, 'Native test selection cannot be reconciled'
excluded_tests=sorted(registered_tests-selected_tests)
sdk=Path(os.environ.get('ANDROID_HOME',os.environ.get('ANDROID_SDK_ROOT','')))
signers=sorted((sdk/'build-tools').glob('*/apksigner.bat'),reverse=True)
assert signers, 'Android SDK apksigner unavailable'
signature=subprocess.run([str(signers[0]),'verify','--verbose',str(apk)],capture_output=True,text=True,encoding='utf-8',errors='replace')
assert signature.returncode==0 and 'Verifies' in signature.stdout, signature.stdout+signature.stderr
signature_text='APK SHA256: '+sha(apk)+'\n'+signature.stdout+signature.stderr
video_path=root/'audit/control-20261018/video-foreground-final2/video-headless/video-replay.json'
video=json.loads(video_path.read_text(encoding='utf-8'))
assert video['passed'], 'Final device replay has not passed'
assert video['process']=='com.visionforge.mobile.local:benchmark'
assert len(video['phases'])==1 and video['phases'][0]['expected_frames']==10160
local_video=json.loads((root/'audit/control-20261018/video-local-final/video-headless/video-replay.json').read_text(encoding='utf-8'))
assert local_video['passed']
foreground=json.loads((root/'audit/control-20261018/video-single-final/video-replay/video-replay.json').read_text(encoding='utf-8'))
foreground_phase=next(p for p in foreground['phases'] if p['name']=='ow_m_416')
assert foreground_phase['passed'] and foreground_phase['executions']==1270
bt=json.loads((root/'audit/control-20261018/bluetooth-dispatch-test.json').read_text(encoding='utf-8'))
assert bt['passed'], 'Real Bluetooth dispatch probe has not passed'
installed=None
if args.device:
    adb=['adb','-s',args.device]
    apk_path=subprocess.check_output(adb+['shell','pm','path','com.visionforge.mobile.local'],text=True).strip().removeprefix('package:')
    installed=subprocess.check_output(adb+['shell','sha256sum',apk_path],text=True).split()[0]
    assert installed==sha(apk), 'Installed APK does not match final build'
live=None
if args.live_collection:
    live_path=args.live_collection/'collection.json'
    live=json.loads(live_path.read_text(encoding='utf-8'))
    # The collector records dist as the last published artifact. Before a new
    # release is published, only the installed package identifies phone code.
    assert live['artifacts']['VFHost-Local.exe']['sha256']==sha(exe), 'Live evidence is for a different Host'
    assert live['installed_apk_sha256']['output'].split()[0]==sha(apk), 'Collected installed APK differs'
    fingerprints=json.loads((args.live_collection/'source-fingerprints.json').read_text(encoding='utf-8'))
    drift=[name for name,digest in fingerprints.items() if not (root/name).is_file() or sha(root/name)!=digest]
    assert not drift, 'Runtime source changed after live collection: '+str(drift)
dynamic_path=args.dynamic_report if args.dynamic_report.is_absolute() else root/args.dynamic_report
assert dynamic_path.is_file(), 'Bound dynamic validation report is required'
if dynamic_path.is_file():
    dynamic=json.loads(dynamic_path.read_text(encoding='utf-8'))
    assert dynamic['status']=='no_dynamic_regression_detected', 'Dynamic tracking regression: candidate must not be published'
    assert dynamic.get('candidate_execution',{}).get('verified') is True, 'Dynamic evidence must come from a successful bound build and test execution'
    if 'test_shape_change_during_closed_loop_tracking' in (root/'android_inference_benchmark/app/src/main/cpp/MobileObservationServoTests.cpp').read_text(encoding='utf-8'):
        assert (dynamic.get('shape_validation') or {}).get('status')=='no_dynamic_regression_detected', 'Shape/occlusion tracking comparison is required for this controller'
    fingerprint=dynamic.get('source_fingerprints',{})
    assert fingerprint and all((root/name).is_file() and sha(root/name)==digest for name,digest in fingerprint.items()), 'Dynamic tracking evidence is stale'
report={'verified_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'source_commit':source['commit'],'original_source_files_verified':len(source['files']),
        'original_source_changes':changed,'source_commercial_dependency_hits':source_hits,
        'artifact_commercial_dependency_hits':binary_hits,'packaged_models':models,
        'current_model_validation':{'path':str(args.model_report),'sha256':sha(args.model_report),
             'timing':graph['timing'],'coordinate_max_error_px':graph.get('coordinate_max_error_px',graph.get('head_coordinate_max_error_px')),
             'validation_frames':graph['validation_frames'],'scope':graph['scope']},
        'current_pipeline_validation':{'path':str(args.pipeline_report),'sha256':sha(args.pipeline_report),
             'duration_seconds':pipeline['duration_seconds'],'scope':pipeline['scope'],
             'metrics':pipeline['metrics'],'limitations':pipeline['limitations']},
        'native_tests_registered':len(registered_tests),
        'native_tests_selected':int(native_result.group(1)),
        'native_tests_excluded':excluded_tests,
        'native_test_log':str(native_path.relative_to(root)),'native_test_log_sha256':sha(native_path),
        'native_tests_passed':int(native_result.group(1))-len(skipped),'native_tests_skipped':skipped,
        'java_tests':json.loads((root/'audit/java-tests.json').read_text())['passed'],
        'device':video['device'],'soc':video['soc'],'installed_apk_sha256':installed,
        'current_live_collection':{'path':str(args.live_collection),'started_utc':live['started_utc'],
            'installed_apk_sha256':live['installed_apk_sha256']['output'].split()[0],
            'last_published_apk_sha256':live['artifacts']['VisionForge-Local.apk']['sha256'],
            'phone_process':live['phone_process']['output']} if live else None,
        'dynamic_validation':{'path':str(dynamic_path.relative_to(root)),
            'sha256':sha(dynamic_path),'status':dynamic['status'],
            'real_game_acceptance':dynamic.get('real_game_acceptance',False)} if dynamic_path.is_file() else None,
        'historical_validation':{'scope':'Previous APK versions; not rerun against the current control implementation',
        'video_replay':{'passed':video['passed'],'scope':video['scope'],
                        'foreground_activity_requested':video['foreground_activity_requested'],
                        'phases':[{k:p[k] for k in ('name','expected_frames','received','executions','passed')} for p in video['phases']]},
        'device_local_replay':{'passed':local_video['passed'],'scope':local_video['scope'],
                        'phases':[{k:p[k] for k in ('name','expected_frames','received','executions','passed')} for p in local_video['phases']]},
        'background_wireless_replay':{'passed':False,'sent':10160,'received':9608,'executions':9448},
        'foreground_video_replay':{'scope':foreground['scope'],
                        'phase':{k:foreground_phase[k] for k in ('name','expected_frames','executions','passed')},
                        'long_phase_interrupted':True},
        'bluetooth_zero_motion_probe':bt},
        'limitations':(['Native tests skipped: '+', '.join(skipped)] if skipped else []) +
                      (['Native tests excluded from this execution: '+', '.join(excluded_tests)] if excluded_tests else []) +
                      ['Real game control traces and anonymous Windows raw mouse arrivals are recorded; no externally calibrated physical end-to-end latency',
                       'Current controller remains under user evaluation; no claim of zero error, zero latency or all-hero lock accuracy',
                       'Head aim uses the user-selected body-box y=0.18 estimate; no all-hero labeled accuracy evaluation',
                       'Sparse detections in supplied edited video; no labeled recall measurement'],
        'artifacts':artifacts}
if args.check_only:
    print(json.dumps(report,ensure_ascii=False,indent=2))
    raise SystemExit(0)
# Read-only validation must finish before publishing anything. A mismatched
# installed APK or stale source evidence must not overwrite the usable dist
# pair and then raise, leaving its old verification record beside new files.
commit_files([
    (dist/'VisionForge-Local.apk', apk),
    (dist/'VFHost-Local.exe', exe),
    (dist/'使用说明.md', root/'README.md'),
    (dist/'SHA256SUMS.txt', ''.join(x['sha256']+'  '+x['file']+'\n' for x in artifacts).encode('utf-8')),
    (root/'audit/apk-signature.txt', signature_text.encode('utf-8')),
    (root/'audit/verification.json', json.dumps(report,ensure_ascii=False,indent=2).encode('utf-8')),
])
print(json.dumps(report,ensure_ascii=False,indent=2))
