"""Run retained Java behavior contracts against the compiled Android app classes."""
from pathlib import Path
import os, subprocess, json

root=Path(__file__).resolve().parents[1]
app=root/'android_inference_benchmark/app'
test=app/'src/test/java'
pkg=test/'com/visionforge/inferencebenchmark'
out=root/'build/java-tests'
out.mkdir(parents=True,exist_ok=True)
names='''BluetoothHidMouseTransportCore BluetoothHidOutputFailClosedPolicy BluetoothHidPendingOperationPolicy
Cat6MouseButtonEndpointPolicy Cat6MouseButtonLeaseState Cat6MouseButtonNotificationDispatcher
Cat6MouseButtonWorkerRecoveryPolicy Cat6ReadyAgentRecoveryPolicy Cat6ReadyLifecycleCoordinator
ControlMoveDeadline ControlOutputCoordinator ControlOutputRoutePolicy ControlTrigger
DecoderAccessUnitQueue DecoderOutputStallPolicy EthernetTransportContract
InferencePolicy MakcuCdcLineCoding MakcuConnectionAttemptTracker MakcuDeliveryCircuit MakcuDeliveryGate
MakcuDeviceIdentityPolicy MakcuDeviceDiagnostics MakcuProtocolNegotiator MakcuResponseStreamParser MakcuUsbInterfaceSelector
MobileControlTuning MobileInferenceBackendPolicy MobileInferenceFailurePolicy MobileModelCatalog
MobileProfileStorageKeys MobileRuntimeMetricsSampler MobileRuntimeSnapshot MobileSocCompatibilityPolicy
MobileThroughputTracker MobileTransportCatalog PersonalTrajectoryProfile PortableModelAssetInstaller
QnnAssetBundleInstaller QnnHtpCompatibilityPolicy'''.split()
names.extend(["MobilePipelineCoordinator","Cat6MouseButtonProtocol","AsyncDiagnosticLog","AsyncBluetoothMoveSink","MobileResponseCalibration","ControlTransportTiming"])
names=[name+'SelfTest' for name in names]
names.extend(['MoveDeliveryTraceSelfTest','DiagnosticTraceBatchSelfTest'])
names.append('MobileRuntimeRecoverySelfTest')
names.append('TransportLifecycleSelfTest')
names.append('MobileCalibrationPersistencePolicySelfTest')
runner=out/'LocalBehaviorTests.java'
runner.write_text('''package com.visionforge.inferencebenchmark;
public final class LocalBehaviorTests {
    public static void main(String[] args) throws Exception {
        for(String name:args) {
            Class<?> type=Class.forName("com.visionforge.inferencebenchmark."+name);
            try {
                java.lang.reflect.Method method=type.getDeclaredMethod("run");
                method.setAccessible(true); method.invoke(null);
            } catch(NoSuchMethodException e) {
                java.lang.reflect.Method method=type.getDeclaredMethod("main",String[].class);
                method.setAccessible(true); method.invoke(null,(Object)new String[0]);
            }
            System.out.println("PASS "+name);
        }
    }
}
''',encoding='utf-8')
sdk=Path(os.environ.get('ANDROID_HOME',os.environ.get('ANDROID_SDK_ROOT','')))
classes=app/'build/intermediates/javac/release/compileReleaseJavaWithJavac/classes'
cp=os.pathsep.join(map(str,[out,classes,root/'vendor/java-tests/json-20180813.jar',sdk/'platforms/android-35/android.jar']))
sources=[str(pkg/(n+'.java')) for n in names]+[str(p) for p in (test/'android').rglob('*.java')]+[str(pkg/'MobileTransportEndpointTestFixtures.java'),str(runner)]
subprocess.run(['javac','-encoding','UTF-8','-cp',cp,'-d',str(out),*sources],check=True)
result=subprocess.run(['java','-ea','-cp',cp,'com.visionforge.inferencebenchmark.LocalBehaviorTests',*names],capture_output=True,text=True)
print(result.stdout); print(result.stderr); result.check_returncode()
(root/'audit/java-tests.json').write_text(json.dumps({'passed':len(names),'classes':names,'output':result.stdout},indent=2))

