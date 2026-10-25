package com.visionforge.inferencebenchmark;

import android.app.Instrumentation;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.ComponentName;
import android.os.IBinder;
import android.os.Bundle;
import android.os.PowerManager;
import android.os.SystemClock;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import com.visionforge.inferencebenchmark.runtime.MobileRuntimeBinding;
import org.json.JSONArray;
import org.json.JSONObject;

/** Deterministic LAN replay owned by the production Activity/service lifecycle. */
final class VideoReplayTest {
    private final Instrumentation instrumentation;
    private MobileRuntimeBinding binding;
    VideoReplayTest(Instrumentation instrumentation) { this.instrumentation = instrumentation; }

    void run(Bundle options) throws Exception {
        Context context = instrumentation.getTargetContext();
        String local = options.getString("phone_ip");
        String host = options.getString("host_ip");
        int frames = Integer.parseInt(options.getString("frames", "1270"));
        int repeats = Integer.parseInt(options.getString("soak_repeats", "8"));
        File directory = new File(context.getExternalFilesDir(null), "video-replay");
        Files.createDirectories(directory.toPath());
        JSONObject report = new JSONObject().put("started_unix_ms", System.currentTimeMillis())
                .put("scope", "LAN_UDP_hardware_decode_preprocess_QNN_no_physical_control_no_desktop_capture")
                .put("device", android.os.Build.MODEL).put("soc", android.os.Build.SOC_MODEL);
        JSONArray phases = new JSONArray(); report.put("phases", phases);
        send(new JSONObject().put("launch_activity", true));
        Activity activity = instrumentation.startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        instrumentation.runOnMainSync(() -> activity.getWindow().addFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        CountDownLatch connected = new CountDownLatch(1);
        ServiceConnection connection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder service) {
                binding = (MobileRuntimeBinding) service; connected.countDown();
            }
            @Override public void onServiceDisconnected(ComponentName name) { binding = null; }
        };
        boolean bound = context.bindService(new Intent(context, MobileRuntimeService.class), connection, Context.BIND_AUTO_CREATE);
        String originalModel = MobileControlRuntime.get(context).activeModel().token;
        PowerManager.WakeLock wake = context.getSystemService(PowerManager.class)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "VisionForge:video-replay");
        wake.acquire(1800000L);
        try {
            if (!bound || !connected.await(10, TimeUnit.SECONDS)) throw new AssertionError("Service bind timeout");
            binding.setActivityForeground(true);
            awaitPrepared(originalModel);
            File paused = new File(directory, "host-paused");
            Files.deleteIfExists(paused.toPath());
            send(new JSONObject().put("pause_desktop_host", true));
            long pauseDeadline = SystemClock.elapsedRealtime() + 60000;
            while (!paused.isFile() && SystemClock.elapsedRealtime() < pauseDeadline) SystemClock.sleep(100);
            if (!paused.isFile()) throw new AssertionError("Desktop Host pause was not confirmed");
            for (int phase = 0; phase <= MobileModelCatalog.ALL.length; ++phase) {
                boolean soak = phase == MobileModelCatalog.ALL.length;
                MobileModelCatalog.Profile model = soak ? MobileModelCatalog.DEFAULT : MobileModelCatalog.ALL[phase];
                String name = soak ? "soak_" + model.token : model.token;
                int expected = frames * (soak ? repeats : 1);
                binding.setActivityForeground(false);
                long stoppedDeadline = SystemClock.elapsedRealtime() + 10000;
                while (QnnHtpBridge.isNativeVideoReceiverRunning() && SystemClock.elapsedRealtime() < stoppedDeadline)
                    SystemClock.sleep(100);
                if (QnnHtpBridge.isNativeVideoReceiverRunning()) throw new AssertionError("Receiver failed to stop");
                long preparedAt = SystemClock.elapsedRealtime();
                binding.selectGameModel(model.token);
                binding.setActivityForeground(true);
                awaitPrepared(model.token);
                QnnHtpBridge.beginNativeH264DetectionTrace(soak ? 0 : frames + 8);
                JSONObject stage = new JSONObject().put("name", name).put("model", model.token)
                        .put("prepare_ms", SystemClock.elapsedRealtime() - preparedAt)
                        .put("expected_frames", expected).put("samples", new JSONArray());
                phases.put(stage);
                send(new JSONObject().put("ready", name).put("repeat", soak ? repeats : 1));
                long deadline = SystemClock.elapsedRealtime() + expected * 1000L / 30 + 30000;
                long previous = 0;
                int stalled = 0;
                MobileRuntimeSnapshot state = null;
                while (SystemClock.elapsedRealtime() < deadline) {
                    SystemClock.sleep(1000);
                    state = snapshot();
                    stage.getJSONArray("samples").put(new JSONObject().put("unix_ms", System.currentTimeMillis())
                            .put("received", state.reassembledAccessUnits).put("executions", state.qnnExecutionCount)
                            .put("decoder", state.decoderReport));
                    if (!state.qnnReady || !state.decoderReady || state.consecutiveQnnFailures != 0)
                        throw new AssertionError("Pipeline unhealthy: " + state.decoderReport);
                    if (state.reassembledAccessUnits >= expected) break;
                    stalled = state.qnnExecutionCount > previous ? 0 : stalled + 1;
                    if (previous > 0 && stalled >= 5) throw new AssertionError("No inference progress for five seconds");
                    previous = state.qnnExecutionCount;
                }
                if (state == null || state.reassembledAccessUnits < expected)
                    throw new AssertionError("Replay incomplete: " + QnnHtpBridge.getNativeVideoReceiverReport());
                SystemClock.sleep(1000);
                state = snapshot();
                if (state.qnnExecutionCount < expected * 0.90)
                    throw new AssertionError("Less than 90 percent of 30 FPS replay inferred: " + state.decoderReport);
                stage.put("passed", true).put("decoder", state.decoderReport)
                        .put("receiver", state.videoReport).put("qnn", state.qnnReport)
                        .put("executions", state.qnnExecutionCount);
                if (!soak) Files.write(new File(directory, name + "-detections.csv").toPath(),
                        QnnHtpBridge.getNativeH264DetectionTraceCsv().getBytes(StandardCharsets.UTF_8));
                Files.write(new File(directory, name + "-metrics.csv").toPath(),
                        QnnHtpBridge.getNativePipelineMetricsCsv().getBytes(StandardCharsets.UTF_8));
                send(new JSONObject().put("completed", name).put("executions", state.qnnExecutionCount));
            }
            report.put("passed", true);
        } catch (Throwable failure) {
            report.put("passed", false).put("failure", android.util.Log.getStackTraceString(failure));
            throw failure;
        } finally {
            if (binding != null) {
                binding.selectGameModel(originalModel); binding.setActivityForeground(true);
                try { awaitPrepared(originalModel); report.put("settings_restored", true); }
                catch (Throwable ignored) { report.put("settings_restored", false); }
            }
            report.put("finished_unix_ms", System.currentTimeMillis());
            Files.write(new File(directory, "video-replay.json").toPath(),
                    report.toString(2).getBytes(StandardCharsets.UTF_8));
            if (wake.isHeld()) wake.release();
            if (bound) context.unbindService(connection);
            instrumentation.runOnMainSync(() -> activity.getWindow().clearFlags(
                    android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        }
    }
    private void awaitPrepared(String model) {
        long deadline = SystemClock.elapsedRealtime() + 30000;
        while (SystemClock.elapsedRealtime() < deadline) {
            MobileRuntimeSnapshot state = snapshot();
            if (state.receiverRunning && state.decoderReady && state.qnnReady
                    && state.qnnReport.contains("model=" + model + ";")) return;
            SystemClock.sleep(200);
        }
        throw new AssertionError("Service pipeline not prepared for " + model + ": " + snapshot().decoderReport);
    }
    private MobileRuntimeSnapshot snapshot() {
        return MobileRuntimeSnapshot.from(QnnHtpBridge.getNativeVideoReceiverReport(),
                QnnHtpBridge.getNativeH264DecoderReport(), QnnHtpBridge.getNativeQnnRealtimeReport(), "");
    }
    private void send(JSONObject value) {
        Bundle progress = new Bundle(); progress.putString("stream", value + "\n");
        instrumentation.sendStatus(0, progress);
    }
}
