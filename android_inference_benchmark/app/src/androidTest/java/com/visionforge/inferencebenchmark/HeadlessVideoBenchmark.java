package com.visionforge.inferencebenchmark;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.PowerManager;
import android.os.SystemClock;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONArray;
import org.json.JSONObject;

/** Isolated test process: uses production decode/preprocess/QNN, without Activity or HID output. */
final class HeadlessVideoBenchmark {
    private final Bundle options;
    private final Instrumentation instrumentation;
    HeadlessVideoBenchmark(Instrumentation instrumentation, Bundle options) {
        this.instrumentation=instrumentation; this.options=options;
    }
    void run() {
        Bundle result=new Bundle();
        final int captureWidth,captureHeight;
        try {
            captureWidth=DeviceVideoReplay.captureDimension(options,"capture_width");
            captureHeight=DeviceVideoReplay.captureDimension(options,"capture_height");
        } catch (IllegalArgumentException invalid) {
            result.putString("stream",android.util.Log.getStackTraceString(invalid));
            instrumentation.finish(Activity.RESULT_CANCELED,result);return;
        }
        QnnNativeVideoInferencePipeline pipeline=new QnnNativeVideoInferencePipeline();
        PowerManager.WakeLock wake=null;
        WifiManager.WifiLock wifi=null;
        DeviceVideoReplay localReplay=null;
        ProjectionProbeSession projectionProbe=null;
        JSONObject report=new JSONObject();
        JSONObject phase=null;
        MobileRuntimeSnapshot state=null;
        File directory=new File(instrumentation.getTargetContext().getExternalFilesDir(null),"video-headless");
        try {
            Context context=instrumentation.getTargetContext();
            Files.createDirectories(directory.toPath());
            boolean foregroundActivity="true".equals(options.getString("foreground_activity"));
            boolean projectionProbeRequested="true".equals(options.getString("projection_probe"));
            int projectionExpectedMode=2;
            if(projectionProbeRequested){
                projectionExpectedMode=Integer.parseInt(options.getString("projection_expected_mode","2"));
                if(projectionExpectedMode<0||projectionExpectedMode>2)throw new IllegalArgumentException("Projection expected mode must be0,1 or2");
                if(!(context.getPackageName()+":benchmark").equals(android.app.Application.getProcessName()))
                    throw new AssertionError("Projection probe requires isolated :benchmark process");
                if(foregroundActivity)throw new AssertionError("Projection probe must not launch foreground Activity/runtime");
            }
            if (foregroundActivity) {
                // The test owns decode in a separate process. Keep the real app visible
                // for Android's foreground-only Wi-Fi latency mode, but stop its pipeline.
                context.startActivity(new Intent(context,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                SystemClock.sleep(1500);
                context.startService(new Intent(context,MobileRuntimeService.class)
                        .setAction("com.visionforge.mobile.local.STOP"));
                SystemClock.sleep(2000);
            }
            PowerManager power=context.getSystemService(PowerManager.class);
            wake=power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"VisionForge:isolated-video-test");
            wake.acquire(1800000L);
            wifi=context.getSystemService(WifiManager.class).createWifiLock(
                    WifiManager.WIFI_MODE_FULL_HIGH_PERF,"VisionForge:isolated-video-test");
            wifi.acquire();
            String local=options.getString("phone_ip"), host=options.getString("host_ip");
            boolean deviceLocal=options.containsKey("local_replay");
            if (deviceLocal) host=local;
            ConnectivityManager connectivity=context.getSystemService(ConnectivityManager.class);
            long handle=0;
            for (Network network:connectivity.getAllNetworks()) {
                LinkProperties properties=connectivity.getLinkProperties(network);
                if (properties!=null && properties.getLinkAddresses().stream()
                        .anyMatch(a -> local.equals(a.getAddress().getHostAddress()))) {
                    handle=network.getNetworkHandle(); break;
                }
            }
            if (handle==0) throw new AssertionError("Requested local IPv4 has no Android network");
            QnnAssetBundleInstaller.Result assets=QnnAssetBundleInstaller.ensureInstalled(context.getFilesDir(),
                    name -> context.getAssets().open("qnn/"+name),BuildConfig.QNN_HTP_ARCHITECTURES);
            if (!assets.ready) throw new AssertionError("QNN assets unavailable");
            int frames=Integer.parseInt(options.getString("frames","1270"));
            int repeats=Integer.parseInt(options.getString("soak_repeats","8"));
            int fps=Integer.parseInt(options.getString("fps","30"));
            boolean saturation="true".equals(options.getString("saturation"));
            if(fps<1 || fps>240)throw new IllegalArgumentException("replay FPS out of range");
            MobileModelCatalog.Profile model=MobileModelCatalog.DEFAULT;
            report.put("started_unix_ms",System.currentTimeMillis()).put("device",android.os.Build.MODEL)
                    .put("soc",android.os.Build.SOC_MODEL).put("process",android.app.Application.getProcessName())
                    .put("run_id",options.getString("run_id",""))
                    .put("input_fps",fps).put("saturation_test",saturation)
                    .put("capture_width",captureWidth).put("capture_height",captureHeight)
                    .put("foreground_activity_requested",foregroundActivity)
                    .put("projection_probe_requested",projectionProbeRequested)
                    .put("scope",deviceLocal?"device_local_UDP_production_decode_preprocess_QNN_no_wifi_no_Activity_no_control_output":
                            "isolated_process_LAN_UDP_production_decode_preprocess_QNN_no_control_output");
            if (!pipeline.prepareQnn(context.getApplicationInfo().nativeLibraryDir,
                    new File(context.getFilesDir(),"qnn").getAbsolutePath(),model.token))
                throw new AssertionError(pipeline.qnnReport());
            if (!pipeline.configureInferenceDecoder(model.inputWidth,model.inputHeight))
                throw new AssertionError("Decoder configuration failed");
            if (!pipeline.startVideoReceiver(local,host,5000,handle))
                throw new AssertionError(pipeline.videoReceiverReport());
            if(projectionProbeRequested){
                projectionProbe=new ProjectionProbeSession(context,directory,projectionExpectedMode);
                projectionProbe.open(model);
                report.put("scope","isolated_benchmark_real_NPU_projection_Core_JNI_fake_unsent_discard_no_USB_HID")
                        .put("physical_control_output",false);
            }
            QnnHtpBridge.beginNativeH264DetectionTrace(frames);
            JSONArray phases=new JSONArray(), samples=new JSONArray();
            phase=new JSONObject().put("name","soak_"+model.token)
                    .put("expected_frames",frames*repeats).put("samples",samples);
            phases.put(phase); report.put("phases",phases);
            send(new JSONObject().put("ready","soak_"+model.token).put("repeat",repeats));
            if (deviceLocal) localReplay=new DeviceVideoReplay(new File(options.getString("local_replay")),local,frames,repeats,fps,captureWidth,captureHeight);
            long deadline=SystemClock.elapsedRealtime()+frames*repeats*1000L/fps+30000;
            long previous=0; int stalled=0;
            while (SystemClock.elapsedRealtime()<deadline) {
                SystemClock.sleep(1000); state=snapshot();
                if(projectionProbe!=null)projectionProbe.sample();
                samples.put(new JSONObject().put("unix_ms",System.currentTimeMillis())
                        .put("received",state.reassembledAccessUnits).put("executions",state.qnnExecutionCount)
                        .put("thermal_status",power.getCurrentThermalStatus()).put("decoder",state.decoderReport)
                        .put("receiver",state.videoReport));
                if (samples.length()%30==0) send(new JSONObject().put("progress_frames",state.qnnExecutionCount)
                        .put("received",state.reassembledAccessUnits).put("last_sequence",state.lastLogicalFrameSequence));
                if (localReplay!=null && localReplay.failure!=null) throw new AssertionError("Local replay failed",localReplay.failure);
                if (!state.qnnReady || !state.decoderReady || state.consecutiveQnnFailures!=0)
                    throw new AssertionError("Pipeline unhealthy: "+state.decoderReport);
                // The final sequence identifies EOF even if earlier UDP frames were lost.
                // Loss still fails the completeness check, but is not mislabeled as a QNN stall.
                if (state.lastLogicalFrameSequence>=frames*repeats) break;
                stalled=state.qnnExecutionCount>previous?0:stalled+1;
                if (previous>0 && stalled>=5) throw new AssertionError(
                        "No inference progress for five seconds; received="+state.reassembledAccessUnits+
                        " last_input_age_ms="+state.lastVideoAgeMillis+" receiver="+state.videoReport);
                previous=state.qnnExecutionCount;
            }
            SystemClock.sleep(1000); state=snapshot();
            if (state.reassembledAccessUnits<frames*repeats || state.qnnExecutionCount<frames*repeats*(saturation?.10:.90))
                throw new AssertionError("Replay incomplete: expected="+frames*repeats+
                        " received="+state.reassembledAccessUnits+" executions="+state.qnnExecutionCount+
                        " receiver="+state.videoReport);
            phase.put("passed",true).put("executions",state.qnnExecutionCount)
                    .put("full_rate_acceptance",!saturation)
                    .put("inference_fraction",(double)state.qnnExecutionCount/(frames*repeats))
                    .put("decoder",state.decoderReport).put("qnn",state.qnnReport).put("receiver",state.videoReport);
            Files.write(new File(directory,model.token+"-detections.csv").toPath(),
                    QnnHtpBridge.getNativeH264DetectionTraceCsv().getBytes(StandardCharsets.UTF_8));
            Files.write(new File(directory,model.token+"-metrics.csv").toPath(),
                    QnnHtpBridge.getNativePipelineMetricsCsv().getBytes(StandardCharsets.UTF_8));
            report.put("passed",true);
            result.putString("stream","Isolated video soak passed\n");
        } catch (Throwable failure) {
            try { report.put("passed",false).put("failure",android.util.Log.getStackTraceString(failure)); }
            catch (Exception ignored) {}
            result.putString("stream",android.util.Log.getStackTraceString(failure));
        } finally {
            if (localReplay!=null) localReplay.close();
            // Preserve receiver counters and traces on failure too; never reuse an older run's CSV.
            try {
                if (state!=null && phase!=null) phase.put("executions",state.qnnExecutionCount)
                        .put("received",state.reassembledAccessUnits).put("decoder",state.decoderReport)
                        .put("qnn",state.qnnReport).put("receiver",state.videoReport);
                Files.write(new File(directory,MobileModelCatalog.DEFAULT.token+"-detections.csv").toPath(),
                        QnnHtpBridge.getNativeH264DetectionTraceCsv().getBytes(StandardCharsets.UTF_8));
                Files.write(new File(directory,MobileModelCatalog.DEFAULT.token+"-metrics.csv").toPath(),
                        QnnHtpBridge.getNativePipelineMetricsCsv().getBytes(StandardCharsets.UTF_8));
            } catch (Exception error) { result.putString("stream",android.util.Log.getStackTraceString(error)); }
            try {
                if(projectionProbe!=null){
                    projectionProbe.finishAndClose(report.optBoolean("passed"));
                    report.put("projection_probe",projectionProbe.report());
                }
            }catch(Throwable error){
                try { report.put("passed",false).put("projection_probe_cleanup_failure",android.util.Log.getStackTraceString(error));
                    if(projectionProbe!=null)report.put("projection_probe",projectionProbe.report());
                }catch(Exception ignored){}
                result.putString("stream",android.util.Log.getStackTraceString(error));
            }
            pipeline.stop();
            if (wake!=null && wake.isHeld()) wake.release();
            if (wifi!=null && wifi.isHeld()) wifi.release();
            try {
                report.put("finished_unix_ms",System.currentTimeMillis());
                Files.write(new File(directory,"video-replay.json").toPath(),report.toString(2).getBytes(StandardCharsets.UTF_8));
            } catch (Exception error) { result.putString("stream",android.util.Log.getStackTraceString(error)); }
        }
        instrumentation.finish(report.optBoolean("passed")?Activity.RESULT_OK:Activity.RESULT_CANCELED,result);
    }
    private MobileRuntimeSnapshot snapshot() {
        return MobileRuntimeSnapshot.from(QnnHtpBridge.getNativeVideoReceiverReport(),
                QnnHtpBridge.getNativeH264DecoderReport(),QnnHtpBridge.getNativeQnnRealtimeReport(),"");
    }
    private void send(JSONObject value) {
        Bundle progress=new Bundle(); progress.putString("stream",value+"\n");instrumentation.sendStatus(0,progress);
    }
}
