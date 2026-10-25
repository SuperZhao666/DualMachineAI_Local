package com.visionforge.inferencebenchmark;

import android.app.Instrumentation;
import android.app.Activity;
import android.os.Bundle;
import android.content.Context;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;

/** Runs the production JNI graph in the app sandbox; no control output is started. */
public final class OverwatchBenchmark extends Instrumentation {
    private Bundle options;
    @Override public void onCreate(Bundle arguments) { options=arguments; start(); }
    @Override public void onStart() {
        Bundle result=new Bundle();
        android.os.PowerManager.WakeLock wake=null;
        try {
            if(options!=null && "settings_snapshot".equals(options.getString("mode"))){
                SettingsSnapshot.save(getTargetContext());finish(Activity.RESULT_OK,result);return;
            }
            if(options!=null && "control_deadline_binding".equals(options.getString("mode"))){
                ControlDeadlineBindingTest.run(this);
                result.putString("stream","Release control deadline binding checks passed; saved control-deadline-binding-test.json\n");
                finish(Activity.RESULT_OK,result);return;
            }
            if (options != null && "live_control_probe".equals(options.getString("mode"))) {
                LiveControlProbe.run(this,"true".equals(options.getString("headless")));
                result.putString("stream", "Live control probe saved.\n");
                finish(Activity.RESULT_OK,result);
                return;
            }
            if (options != null && "makcu_ack_deadline".equals(options.getString("mode"))) {
                MakcuAckDeadlineTest.run(this);
                result.putString("stream", "Zero-movement delayed echo measurement saved.\n");
                finish(Activity.RESULT_OK,result);
                return;
            }
            if (options != null && "makcu_read_only".equals(options.getString("mode"))) {
                MakcuReadOnlyProbe.run(this);
                result.putString("stream", "Read-only MAKCU queries saved; inspect responses for actual capabilities.\n");
                finish(Activity.RESULT_OK, result);
                return;
            }
            if (options != null && ("makcu_dispatch".equals(options.getString("mode"))
                    || "makcu_motion_probe".equals(options.getString("mode")))) {
                MakcuDispatchTest.run(this,"makcu_motion_probe".equals(options.getString("mode")),
                        "y".equals(options.getString("axis")),
                        Integer.parseInt(options.getString("final_dx","0")),
                        Integer.parseInt(options.getString("final_dy","0")),
                        Integer.parseInt(options.getString("interval_ms","80")));
                result.putString("stream", "MAKCU measurement saved; inspect report counts and independent Windows input evidence.\n");
                finish(Activity.RESULT_OK, result);
                return;
            }
            if (options != null && "headless_video".equals(options.getString("mode"))) {
                new HeadlessVideoBenchmark(this, options).run();
                return;
            }
            if (options != null && "bluetooth_dispatch".equals(options.getString("mode"))) {
                BluetoothDispatchTest.run(this);
                result.putString("stream", "Bluetooth zero-motion dispatch checks passed; saved bluetooth-dispatch-test.json\n");
                finish(Activity.RESULT_OK, result);
                return;
            }
            if (options != null && "video_replay".equals(options.getString("mode"))) {
                new VideoReplayTest(this).run(options);
                result.putString("stream", "Video replay checks passed; saved video-replay.json\n");
                finish(Activity.RESULT_OK, result);
                return;
            }
            if (options != null && "runtime".equals(options.getString("mode"))) {
                new RuntimeStabilityTest(this).run(options);
                result.putString("stream", "Runtime stability checks passed; saved runtime-stability.json\n");
                finish(Activity.RESULT_OK, result);
                return;
            }
            Context c=getTargetContext();
            wake=c.getSystemService(android.os.PowerManager.class).newWakeLock(
                    android.os.PowerManager.PARTIAL_WAKE_LOCK,"VisionForge:overwatch-test");
            wake.acquire(600000L);
            QnnAssetBundleInstaller.Result assets=QnnAssetBundleInstaller.ensureInstalled(
                    c.getFilesDir(), name -> c.getAssets().open("qnn/"+name), BuildConfig.QNN_HTP_ARCHITECTURES);
            if (!assets.ready) throw new IllegalStateException("QNN asset installation failed");
            JSONArray runs=new JSONArray();
            String requested=options==null?null:options.getString("model");
            for (MobileModelCatalog.Profile model : MobileModelCatalog.ALL) {
                if (requested!=null && !requested.equals(model.token)) continue;
                int optimization=Integer.parseInt(options==null?"2":options.getString("optimization","2"));
                for (boolean power : new boolean[]{false,true}) {
                    int thermal=c.getSystemService(android.os.PowerManager.class).getCurrentThermalStatus();
                    String report=QnnHtpBridge.runBenchmarkProfile(
                            c.getApplicationInfo().nativeLibraryDir,
                            new File(c.getFilesDir(),"qnn").getAbsolutePath(), model.token, power, optimization);
                    if (!report.contains("graphExecute completed")) throw new IllegalStateException(report);
                    JSONObject entry=new JSONObject().put("model",model.token).put("power_vote",power)
                            .put("optimization",optimization).put("thermal_before",thermal)
                            .put("thermal_after",c.getSystemService(android.os.PowerManager.class).getCurrentThermalStatus())
                            .put("report",report);
                    runs.put(entry); Bundle progress=new Bundle(); progress.putString("stream",entry.toString()+"\n"); sendStatus(0,progress);
                }
            }
            JSONObject report=new JSONObject().put("device",android.os.Build.MODEL)
                    .put("soc",android.os.Build.SOC_MODEL).put("runs",runs);
            File destination=new File(c.getExternalFilesDir(null),"overwatch-benchmark.json");
            Files.write(destination.toPath(),report.toString(2).getBytes(StandardCharsets.UTF_8));
            result.putString("stream","Saved "+destination+"\n");
            finish(Activity.RESULT_OK,result);
        } catch (Throwable e) { result.putString("stream",android.util.Log.getStackTraceString(e)); finish(Activity.RESULT_CANCELED,result); }
        finally { if(wake!=null && wake.isHeld()) wake.release(); }
    }
}
