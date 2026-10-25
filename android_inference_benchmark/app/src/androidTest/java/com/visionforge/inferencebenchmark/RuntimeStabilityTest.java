package com.visionforge.inferencebenchmark;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.os.SystemClock;
import com.visionforge.inferencebenchmark.runtime.MobileRuntimeBinding;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** On-device production pipeline exercise. Requires a running host test source. */
final class RuntimeStabilityTest {
    private final Instrumentation instrumentation;
    private final JSONArray stages = new JSONArray();
    private final JSONArray samples = new JSONArray();
    private MobileRuntimeBinding binding;
    RuntimeStabilityTest(Instrumentation instrumentation) { this.instrumentation = instrumentation; }

    void run(Bundle arguments) throws Exception {
        Context context = instrumentation.getTargetContext();
        CountDownLatch connected = new CountDownLatch(1);
        ServiceConnection connection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                binding = (MobileRuntimeBinding) binder; connected.countDown();
            }
            @Override public void onServiceDisconnected(ComponentName name) { binding = null; }
        };
        Activity activity = instrumentation.startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        instrumentation.runOnMainSync(() -> activity.getWindow().addFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        MobileControlRuntime control = MobileControlRuntime.get(context);
        String originalModel = arguments.getString("restore_model", control.activeModel().token);
        boolean bound = context.bindService(new Intent(context, MobileRuntimeService.class),
                connection, Context.BIND_AUTO_CREATE);
        JSONObject report = new JSONObject().put("started_unix_ms", System.currentTimeMillis())
                .put("device", android.os.Build.MODEL).put("stages", stages).put("samples", samples);
        File destination = new File(context.getExternalFilesDir(null), "runtime-stability.json");
        try {
            if (!bound || !connected.await(10, TimeUnit.SECONDS)) throw new AssertionError("Service bind timeout");
            binding.setActivityForeground(true);
            for (MobileModelCatalog.Profile model : MobileModelCatalog.ALL) {
                long started = SystemClock.elapsedRealtime();
                binding.selectGameModel(model.token);
                awaitHealthy(model.token, 30000);
                sampleFor(model.token, 20000);
                stage("model_switch", model.token, started);
            }
            binding.selectGameModel(originalModel);
            awaitHealthy(originalModel, 30000);
            for (int cycle = 0; cycle < 3; ++cycle) {
                long started = SystemClock.elapsedRealtime();
                binding.setActivityForeground(false);
                long deadline = started + 10000;
                while (QnnHtpBridge.isNativeVideoReceiverRunning()
                        && SystemClock.elapsedRealtime() < deadline) SystemClock.sleep(100);
                if (QnnHtpBridge.isNativeVideoReceiverRunning()) throw new AssertionError("Hidden receiver did not stop");
                if (snapshot().nativeControlOutputEffective) throw new AssertionError("Hidden output stayed enabled");
                SystemClock.sleep(1000);
                binding.setActivityForeground(true);
                awaitHealthy(originalModel, 30000);
                sampleFor(originalModel, 3000);
                stage("foreground_stop_resume_" + cycle, originalModel, started);
            }
            long started = SystemClock.elapsedRealtime();
            int seconds = Math.max(30, Math.min(1800, Integer.parseInt(arguments.getString("soak_seconds", "300"))));
            sampleFor(originalModel, seconds * 1000L);
            stage("continuous_soak", originalModel, started);
            report.put("passed", true);
        } catch (Throwable failure) {
            report.put("passed", false).put("failure", android.util.Log.getStackTraceString(failure));
            throw failure;
        } finally {
            if (binding != null) {
                binding.selectGameModel(originalModel); binding.setActivityForeground(true);
                try { awaitHealthy(originalModel, 30000); report.put("settings_restored", true); }
                catch (Throwable restoreFailure) { report.put("settings_restored", false); }
            }
            report.put("finished_unix_ms", System.currentTimeMillis());
            Files.write(destination.toPath(), report.toString(2).getBytes(StandardCharsets.UTF_8));
            if (bound) context.unbindService(connection);
            instrumentation.runOnMainSync(() -> activity.getWindow().clearFlags(
                    android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        }
    }

    private MobileRuntimeSnapshot snapshot() {
        return MobileRuntimeSnapshot.from(QnnHtpBridge.getNativeVideoReceiverReport(),
                QnnHtpBridge.getNativeH264DecoderReport(), QnnHtpBridge.getNativeQnnRealtimeReport(), "");
    }

    private void awaitHealthy(String model, long timeout) {
        long deadline = SystemClock.elapsedRealtime() + timeout;
        while (SystemClock.elapsedRealtime() < deadline) {
            MobileRuntimeSnapshot state = snapshot();
            if (state.qnnReport.contains("model=" + model + ";") && state.qnnReady
                    && state.decoderReady && state.videoLinkLive && state.qnnExecutionLive
                    && state.qnnExecutionCount >= 10) return;
            SystemClock.sleep(200);
        }
        throw new AssertionError("Pipeline not live for " + model + ": " + snapshot().decoderReport);
    }

    private void sampleFor(String model, long duration) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + duration;
        long previous = snapshot().qnnExecutionCount;
        int stalls = 0;
        while (SystemClock.elapsedRealtime() < deadline) {
            SystemClock.sleep(1000);
            MobileRuntimeSnapshot state = snapshot();
            if (!state.qnnReady || !state.decoderReady || state.consecutiveQnnFailures != 0)
                throw new AssertionError("Unhealthy pipeline: " + state.decoderReport);
            if (!state.qnnReport.contains("model=" + model + ";")) throw new AssertionError("Unexpected model");
            stalls = state.qnnExecutionCount > previous ? 0 : stalls + 1;
            if (stalls >= 3) throw new AssertionError("Inference stopped progressing for 3 samples");
            previous = state.qnnExecutionCount;
            samples.put(new JSONObject().put("unix_ms", System.currentTimeMillis()).put("model", model)
                    .put("executions", previous).put("qnn_p50_ms", state.qnnP50Millis)
                    .put("qnn_p95", state.qnnP95).put("preprocess_p50_ms", state.preprocessP50Millis)
                    .put("decode_queue_p50_ms", state.decodeQueueP50Millis)
                    .put("decoder", state.decoderReport));
        }
    }

    private void stage(String name, String model, long started) throws Exception {
        JSONObject result = new JSONObject().put("stage", name).put("model", model)
                .put("elapsed_ms", SystemClock.elapsedRealtime() - started).put("passed", true);
        stages.put(result);
        Bundle progress = new Bundle(); progress.putString("stream", result + "\n");
        instrumentation.sendStatus(0, progress);
    }
}
