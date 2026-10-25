package com.visionforge.inferencebenchmark;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import java.io.File;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;

/** Exercises the real Android HID Binder with zero-delta reports: no pointer movement. */
final class BluetoothDispatchTest {
    static void run(Instrumentation instrumentation) throws Exception {
        Context context = instrumentation.getTargetContext();
        Activity activity = instrumentation.startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        instrumentation.runOnMainSync(() -> activity.getWindow().addFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        JSONObject result = new JSONObject().put("scope",
                "real_Android_HID_Binder_zero_delta_no_host_or_physical_ack")
                .put("device", android.os.Build.MODEL).put("started_unix_ms", System.currentTimeMillis());
        File destination = new File(context.getExternalFilesDir(null), "bluetooth-dispatch-test.json");
        BluetoothHidMouseTransportCore core = null;
        AsyncBluetoothMoveSink sink = null;
        try {
            MobileControlRuntime runtime = MobileControlRuntime.get(context);
            if (!runtime.isBluetoothHidOutputRouteActive()) throw new AssertionError("Bluetooth route is not selected");
            runtime.ensureSelectedOutputConnected();
            long deadline = SystemClock.elapsedRealtime() + 30000;
            while (!runtime.isBluetoothHidSessionReady() && SystemClock.elapsedRealtime() < deadline)
                SystemClock.sleep(100);
            if (!runtime.isBluetoothHidSessionReady()) throw new AssertionError("Bluetooth host is not ready");
            Field field = MobileControlRuntime.class.getDeclaredField("bluetoothHidAdapter");
            field.setAccessible(true);
            BluetoothHidSessionPort session = (BluetoothHidSessionPort) field.get(runtime);
            AtomicReference<CountDownLatch> completion = new AtomicReference<>();
            AtomicBoolean rejected = new AtomicBoolean();
            // Share the existing adapter without changing its production state listener.
            BluetoothHidMouseTransportCore.SessionPort zeroMotion = new BluetoothHidMouseTransportCore.SessionPort() {
                private final byte[] zero = new byte[4];
                public BluetoothHidOutputFailClosedPolicy.SessionState sessionState() { return session.sessionState(); }
                public boolean sendMouseReport(byte[] ignored) { return session.sendMouseReport(zero); }
                public void connectFirstSupportedHost() {}
            };
            core = new BluetoothHidMouseTransportCore(zeroMotion, (ticket, us) -> {
                completion.get().countDown(); return true;
            });
            if (!core.setOutputDeliveryAllowed(true)) throw new AssertionError("Test transport not ready");
            sink = new AsyncBluetoothMoveSink(core, ticket -> {
                rejected.set(true); completion.get().countDown();
            });
            for (boolean asynchronous : new boolean[]{false, true}) {
                long[] offer = new long[300], total = new long[300];
                for (int i = -20; i < offer.length; ++i) {
                    CountDownLatch done = new CountDownLatch(1); completion.set(done);
                    long start = System.nanoTime();
                    boolean accepted = asynchronous ? sink.offerMoveFromNative(1, 0, i + 1000, 250000)
                            : core.offerMoveFromNative(1, 0, i + 1000, 250000);
                    long offered = System.nanoTime();
                    if (!accepted || !done.await(2, TimeUnit.SECONDS) || rejected.get())
                        throw new AssertionError("HID API rejected or timed out");
                    if (i >= 0) { offer[i] = (offered - start) / 1000; total[i] = (System.nanoTime() - start) / 1000; }
                    SystemClock.sleep(8);
                }
                result.put(asynchronous ? "bounded_worker" : "synchronous_baseline",
                        new JSONObject().put("samples", offer.length)
                                .put("caller_us", distribution(offer)).put("completion_us", distribution(total)));
            }
            result.put("dispatch", sink.report()).put("passed", true);
        } catch (Throwable failure) {
            result.put("passed", false).put("failure", android.util.Log.getStackTraceString(failure));
            throw failure;
        } finally {
            if (sink != null) sink.close();
            if (core != null) core.dispose();
            result.put("finished_unix_ms", System.currentTimeMillis());
            Files.write(destination.toPath(), result.toString(2).getBytes(StandardCharsets.UTF_8));
            instrumentation.runOnMainSync(() -> activity.getWindow().clearFlags(
                    android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        }
    }
    private static JSONObject distribution(long[] values) throws Exception {
        Arrays.sort(values);
        return new JSONObject().put("p50", values[values.length / 2])
                .put("p95", values[(int) (values.length * .95)])
                .put("p99", values[(int) (values.length * .99)]).put("max", values[values.length - 1]);
    }
}
