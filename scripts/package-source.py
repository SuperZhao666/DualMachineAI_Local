"""Package the current one-model source tree; exclude private replay media and caches."""
from pathlib import Path
import hashlib, json, zipfile, os
root=Path(__file__).resolve().parents[1]
target=root.parent/'DualMachineAI_Local_Source.zip'
excluded={'build','.gradle','.gradle-user-home','.cxx','.git','__pycache__','node_modules','toolchains','qnn_sdk'}
files=[]
for name in ['android_inference_benchmark','dual_machine_runtime','vendor','scripts']:
    for directory, children, names in os.walk(root/name):
        children[:]=[child for child in children if child not in excluded]
        for filename in names:
            p=Path(directory)/filename
            if p.name=='local.properties' or p.suffix in {'.pyc','.pdb','.log'}: continue
            files.append(p)
production=json.loads((root/'models/overwatch/production.json').read_text(encoding='utf-8'))
selected=production['selected_model']
for name in ['README.md','models/overwatch/production.json',f"models/overwatch/{selected['name']}/split.onnx",selected['source'],
             'audit/source-manifest.json','audit/verification.json','audit/java-tests.json',
             'audit/apk-signature.txt','audit/control-20261018/RESULTS.md',
             'audit/control-20261018/validation-summary.json',
             'audit/live-20261018/RESULTS.md','audit/live-20261018/validation-summary.json',
             'audit/live-20261018/CODE-AUDIT.md','audit/live-20261018/audit-dynamic-gate.json',
             'audit/live-20261018/audit-regressions-before.txt','audit/live-20261018/audit-regressions-after.txt']:
    p=root/name
    if not p.is_file(): raise FileNotFoundError(p)
    files.append(p)
# Include the evidence for the currently published controller without bundling
# private gameplay recordings, raw device traces, or proxy credentials.
for name in ['RESULTS.md','deployment.json','live-validation.json','dynamic-gate.json',
             'artifact-check.json','replay-validation.json','user-feedback.json','native-tests.log','java-tests.log']:
    p=root/'audit/image-identity-20261019'/name
    if p.is_file():files.append(p)
for name in ['RESULTS.md','RESEARCH.md','final-dynamic-gate.json',
             'final-multiframe-comparison.json','final-crowd-comparison.json',
             'deployment.json','live-validation.json','native-changes.patch']:
    p=root/'audit/research-tracking-20261019'/name
    if p.is_file(): files.append(p)
for name in ['RESULTS.md','deployment.json','live-validation.json',
             'cadence-dynamic-gate.json','acquisition-comparison.json',
             'acquisition-audit.cpp','build-acquisition-audit.bat',
             'acquisition-before.csv','acquisition-after.csv','native-changes.patch']:
    p=root/'audit/response-jitter-20261019'/name
    if p.is_file(): files.append(p)
for name in ['RESULTS.md', 'deployment.json', 'live-validation.json',
             'live-before.json', 'dynamic-gate.json', 'native-test-scope.json',
             'reassembly-before.log', 'reassembly-after.log', 'changes.patch']:
    p=root/'audit/inference-interruption-20261019'/name
    if p.is_file(): files.append(p)
# Keep the small source baseline needed to rebuild the acquisition comparison.
for name in ['RESULTS.md', 'deployment.json', 'live-validation.json',
             'dynamic-gate.json', 'native-test-scope.json',
             'regressions-before.log', 'regressions-after.log',
             'pursuit-comparison.json', 'user-feedback.json', 'changes.patch']:
    p=root/'audit/lock-recovery-20261019'/name
    if p.is_file(): files.append(p)
for name in ['RESULTS.md', 'deployment.json', 'live-validation.json',
             'dynamic-gate.json', 'native-test-scope.json',
             'source-tests-before.log', 'source-tests-after.log', 'changes.patch',
             'artifact-checks.json', 'failure-before-restart.json',
             'restored-before-install.json', 'summarize-live.py']:
    p=root/'audit/connection-recovery-20261019'/name
    if p.is_file(): files.append(p)
for name in ['RESULTS.md', 'deployment.json', 'live-validation.json',
             'dynamic-gate.json', 'native-test-scope.json',
             'regressions-before.log', 'regressions-after.log', 'changes.patch',
             'real-log-findings.json', 'rejected-candidate-summary.json',
             'artifact-checks.json', 'user-feedback.json']:
    p=root/'audit/multi-target-lock-20261019'/name
    if p.is_file(): files.append(p)
# Exclude rejected executable experiments and all raw phone/gameplay traces.
for name in ['RESULTS.md', 'deployment.json', 'live-validation.json',
             'dynamic-gate.json', 'native-test-scope.json',
             'regressions-before.log', 'regressions-after.log', 'changes.patch',
             'real-log-findings.json', 'acquisition-comparison.json',
             'artifact-checks.json', 'user-feedback.json',
             'rejected-candidate-summary.json', 'host-preservation.json']:
    p=root/'audit/crowd-acquisition-20261019'/name
    if p.is_file(): files.append(p)
for name in ['RESULTS.md', 'deployment.json', 'live-validation.json',
             'dynamic-gate.json', 'native-test-scope.json',
             'regressions-before.log', 'regressions-after.log', 'changes.patch',
             'real-log-findings.json', 'sequence-comparison.json', 'holdout-comparison.json',
             'rejected-candidate-summary.json', 'artifact-checks.json', 'source-audit.json']:
    p=root/'audit/identity-root-cause-20261019'/name
    if p.is_file(): files.append(p)
for name in ['RESULTS.md', 'deployment.json', 'live-validation.json',
             'dynamic-gate.json', 'native-test-scope.json', 'source-audit.json',
             'regressions-before.log', 'regressions-after.log', 'changes.patch',
             'stop-before.log', 'stop-after.log', 'stop-comparison.json',
             'build-stop-comparison.bat', 'build-regressions-before.bat', 'compare-stop.py',
             'servo-fixture.cpp', 'regression-fixture.cpp',
             'artifact-checks.json', 'user-feedback.json', 'rejected-candidate-summary.json']:
    p=root/'audit/follow-response-20261019'/name
    if p.is_file(): files.append(p)
for name in ['RESULTS.md','RESEARCH.md','deployment.json','live-validation.json',
             'before-live-validation.json','device-discovery.json','artifact-checks.json','real-log-findings.json',
             'source-audit.json','native-test-scope.json','dynamic-gate.json','servo.log',
             'native-tests.log','java-tests.log','changes.patch','user-feedback.json',
             'rejected-candidate-summary.json','build-comparison.bat','servo-fixture.cpp',
             'regression-fixture.cpp','compare.py','finalize-audit.py','host-preservation.json']:
    p=root/'audit/manual-motion-20261019'/name
    if p.is_file():files.append(p)
manual=root/'audit/manual-motion-20261019'
for name in ['RESULTS.md','deployment.json','initial-deployment.json','live-validation.json',
             'flight-analysis.json','log-overhead.json','artifact-checks.json','source-audit.json',
             'dynamic-gate.json','native-tests.log','java-tests.log','servo.log','changes.patch',
             'host-preservation.json','native-test-scope.json']:
    p=root/'audit/telemetry-20261019'/name
    if p.is_file():files.append(p)
# Reproduction data stays local: no per-frame gameplay/box replay or input traces.
combat=root/'audit/combat-dynamics-20261019'
for name in ['RESULTS.md','analysis-summary.json','combat-findings.json','final-combat-12205.json',
             'pipeline-findings.json','flight-8832.json','final-flight-12205.json',
             'rejected-candidate-summary.json','runtime-restoration.json','host-preservation.json',
             'final-verification.json','final-native-tests.log','final-dynamic-gate.json','final-servo.log',
             'extractor-verification.json','geometry-before.json','geometry-after.json',
             'comparison.json','pose-comparison.json','analyze-combat.py','compare.py','compare-pose.py',
             'servo-fixture.cpp','pose-fixture.cpp','geometry-proof.cpp','build-comparison.bat',
             'build-pose.bat','build-geometry-proof.bat','changes.patch']:
    p=combat/name
    if p.is_file():files.append(p)
for folder in ['baseline','rejected-independent-estimates','rejected-anchor-interval',
               'rejected-confirmed-anchor-stop']:
    for p in (combat/folder).rglob('*'):
        if p.is_file() and p.suffix in {'.hpp','.h','.cpp','.json','.log'}:
            files.append(p)
for name in ['raw-arrivals-analysis.json']:
    p=manual/name
    if p.is_file():files.append(p)
for folder in ['baseline','rejected-frame-only-reversal','rejected-window-rebase']:
    for p in (manual/folder).rglob('*'):
        if p.is_file() and p.suffix in {'.hpp','.h','.cpp','.java','.json','.gradle','.md'}:
            files.append(p)
for name in ['RESULTS.md','rejected-candidate-summary.json','comparison.json']:
    p=root/'audit/settle-noise-20261019'/name
    if p.is_file():files.append(p)
# The standalone stop comparison compiles the same fixture against both trees.
follow_baseline=root/'audit/follow-response-20261019/baseline/android_inference_benchmark/app/src/main/cpp'
if follow_baseline.is_dir():
    files.extend(p for p in follow_baseline.iterdir() if p.suffix in {'.hpp','.h','.cpp'})
baseline=root/'audit/response-jitter-20261019/baseline'
if baseline.is_dir():
    files.extend(p for p in baseline.iterdir() if p.suffix in {'.hpp','.h','.cpp'})
for name in ['RESULTS.md','GOAL.md','deployment.json','live-validation.json','graph-validation.json',
             'dynamic-gate.json','native-tests.log','java-tests.log','restoration-artifact-check.json',
             'restored-native-sha256.json','build-cache-rejection.txt']:
    p=root/'audit/restore-body-head-20261019'/name
    if p.is_file(): files.append(p)
# Rejected head models, private video/frames and per-frame control logs remain local.
for folder in ['pipeline-timeline-20261019','background-sad-20261019','image-prefilter-20261019']:
    for name in ['RESULTS.md','GOAL-STATUS.md','deployment.json','live-validation.json',
                 'offline-verification.json','candidate-artifact.json','dynamic-gate.json',
                 'native-tests.log','java-tests.log','replay-comparison.json',
                 'combined-replay-comparison.json','live-pipeline-timeline.json']:
        p=root/'audit'/folder/name
        if p.is_file():files.append(p)
for folder in ['velocity-reversal-20261019','image-ncc-20261019']:
    p=root/'audit'/folder/'RESULTS.md'
    if p.is_file():files.append(p)
for name in ['RESULTS.md','raw-arrival-analysis.json','hid-schema-before.log','hid-schema-after.log']:
    p=root/'audit/combat137-20261019'/name
    if p.is_file():files.append(p)
# Final scoped identity fix; aggregate evidence only, no gameplay/raw traces.
for name in ['RESULTS.md','deployment.json','live-validation.json','dynamic-gate.json',
             'native-tests.log','java-tests.log','replay-summary.json',
             'offline-verification.json','changes.patch']:
    p=root/'audit/image-association-20261019'/name
    if p.is_file():files.append(p)
for name in ['RESULTS.md','deployment.json','host-deployment.json','live-validation.json',
             'offline-verification.json','dynamic-gate.json','native-tests.log','java-tests.log',
             'worker-probe.log','retained-apks.json','changes.patch']:
    p=root/'audit/wide-search-20261020'/name
    if p.is_file(): files.append(p)
for name in ['RESULTS.md','RESEARCH.md','deployment.json','live-validation.json',
             'dynamic-gate.json','native-tests.log','java-tests.log','candidate-artifact.json',
             'acquisition-comparison.json','root-cause-coverage.json','changes.patch']:
    p=root/'audit/acquisition-latency-20261021'/name
    if p.is_file(): files.append(p)
for name in ['AUDIT.md','RESULTS.md','deployment.json','live-validation.json',
             'dynamic-gate.json','native-tests.log','java-tests.log','candidate-artifact.json',
             'replay-comparison.json','independent-motion-check.json','large-step-log-evidence.json',
             'acquisition-comparison.json',
             'large-motion-tests.log','large-motion-baseline.log','changes.patch']:
    p=root/'audit/deep-control-20261021'/name
    if p.is_file(): files.append(p)
# Isolated calibration / rendering-latency evidence, aggregate only.
for name in ['CONTROLLED-RESULTS.md','measurement-summary.json','recovery-comparison.png']:
    p=root/'audit/stability-control-20261021'/name
    if p.is_file():files.append(p)
for name in ['RESULTS.md','RESEARCH.md','release-validation.json','candidate-artifact.json',
             'final-dynamic-gate.json','native-tests.log','java-tests.log','changes.patch',
             'sweep-comparison.json','low-delay-comparison.json','installed-current.json',
             'usb-deadline-baseline.json','makcu-ack-deadline-test-candidate.json',
             'makcu-dispatch-test-candidate.json','pixel-regression-proof/result.json']:
    p=root/'audit/iteration-142-20261021'/name
    if p.is_file():files.append(p)
for name in ['GOAL.md','RESULTS.md','RESEARCH.md','model-input-audit.json','model-validation.json',
             'pipeline-validation.json','dynamic-gate.json','native-final-all-applicable.log',
             'java-calibration-tests-fixed.log','deployment-current.json','pipeline-comparison.json',
             'readback-game-abba.json','color-conversion-test.log','cpu-resize-unit-fixed.log',
             'cpu-resize-encoder.log','host-final-deployment.json','numeric-comparison.json',
             'control-continuity-audit.json','control-continuity-audit.md',
             'delivery-cross-review.json','minimal-final-candidate-comparison.json',
             'continuity-soak60-summary.json','continuity-soak60-analysis.json',
             'continuity-final-soak30-summary.json','control-deadline-binding-test.json',
             'candidate-artifact-check.json','original-preservation-current.json',
             'candidate-delivery.json','host-relink-section-comparison.json']:
    p=root/'audit/model640-20261022'/name
    if not p.is_file():raise FileNotFoundError(p)
    files.append(p)
# Current graph validation and the rejected motion experiments remain visible
# to reviewers; raw gameplay/phone traces, inputs and additional model .so
# files stay local and are not copied into the source delivery.
for name in ['ROOT-CAUSE-AUDIT.md','pixel-active-control-review.md',
             'pixel-active-control-review.json','qnn-performance-vote-audit.md',
             'qnn-performance-vote-audit.json','proposal-continuity-oracle-results.md',
             'qnn-head16const-investigation/README.md',
             'qnn-head16const-investigation/readiness-report.json',
             'qnn-head16const-rerun-20261022T095916Z/input-overrides.json',
             'qnn-head16const-rerun-20261022T095916Z/decode-constant-identity.json',
             'qnn-head16const-rerun-20261022T095916Z/manifest.json',
             'fresh-pixel-prototype/INTEGRATION.md',
             'fresh-pixel-prototype/worker-phase-analysis.json',
             'fresh-pixel-prototype/same-build-mode-comparison.json',
             'fresh-pixel-prototype/same-build-repeat-comparison.json']:
    p=root/'audit/model640-20261022'/name
    if not p.is_file():raise FileNotFoundError(p)
    files.append(p)
for name in ['RESULTS.md','pipeline-validation.json','model-validation.json',
             'dynamic-gate.json','native-tests.log','java-tests.log',
             'candidate-artifact.json','candidate-deployment.json',
             'source-candidate-manifest.json','root-followup.json',
             'native-baseline-guard.json','release-validation.json']:
    p=root/'audit/iteration-153-20261022'/name
    if not p.is_file():raise FileNotFoundError(p)
    files.append(p)
files=sorted(set(files))
with zipfile.ZipFile(target,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for p in sorted(files): z.write(p, str(Path(root.name)/p.relative_to(root)))
with zipfile.ZipFile(target) as z:
    assert z.testzip() is None
    names=z.namelist()
    models=[n for n in names if n.endswith('.so') and '/libow_' in n]
    assert len(models)==1 and models[0].endswith('/'+selected['qnn_library']),models
    for p in files:
        member=str(Path(root.name)/p.relative_to(root)).replace('\\','/')
        assert hashlib.sha256(z.read(member)).digest()==hashlib.sha256(p.read_bytes()).digest(), member
report={'file':str(target),'bytes':target.stat().st_size,'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'files':len(files)}
(root/'audit/source-package.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
