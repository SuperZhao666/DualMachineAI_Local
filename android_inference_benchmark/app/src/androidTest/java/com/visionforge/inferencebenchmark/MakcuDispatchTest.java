package com.visionforge.inferencebenchmark;

import android.app.Instrumentation;
import android.content.Context;
import android.os.SystemClock;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONArray;
import org.json.JSONObject;

/** Test-only USB/echo probe; bounded movement additionally needs PC RAWINPUT evidence. */
final class MakcuDispatchTest {
    static void run(Instrumentation instrumentation, boolean movementProbe) throws Exception {
        run(instrumentation, movementProbe, false, 0, 0);
    }
    static void run(Instrumentation instrumentation, boolean movementProbe, boolean verticalProbe,
            int finalDeltaX, int finalDeltaY) throws Exception {
        run(instrumentation, movementProbe, verticalProbe, finalDeltaX, finalDeltaY,
                MakcuProbeSchedule.DEFAULT_INTERVAL_MS);
    }
    static void run(Instrumentation instrumentation, boolean movementProbe, boolean verticalProbe,
            int finalDeltaX, int finalDeltaY, int intervalMillis) throws Exception {
        // Validate before constructing/connecting any USB owner.
        MakcuProbeSchedule.requireInterval(intervalMillis);
        // A training-room reacquisition fixture can leave a bounded offset for
        // the resumed production controller. Default probes remain balanced.
        final int kickX=movementProbe ? Math.max(-48,Math.min(48,finalDeltaX)) : 0;
        final int kickY=movementProbe ? Math.max(-48,Math.min(48,finalDeltaY)) : 0;
        final boolean hasKick=kickX!=0 || kickY!=0;
        Context context=instrumentation.getTargetContext();
        JSONObject result=new JSONObject().put("scope", movementProbe
                ? "bounded_relative_pattern_requires_independent_Windows_raw_input_verification"
                : "production_USB_zero_motion_echo_not_physical_execution")
                .put("started_unix_ms",System.currentTimeMillis())
                .put("axis",verticalProbe ? "y" : "x")
                .put("schema","makcu_motion_schedule_v2")
                .put("requested_interval_ms",intervalMillis)
                .put("movement_probe",movementProbe)
                .put("schedule_policy",movementProbe
                        ? "fixed_nominal_start_min_previous_actual_write_start_no_catchup_burst"
                        : "legacy_zero_motion_echo_stress_sleep_2_to_8ms")
                .put("echo_semantics","firmware_echo_and_prompt_not_physical_execution")
                .put("physical_execution_verified",false)
                .put("cleanup_completed",false);
        MakcuSerialController controller=new MakcuSerialController(context,new MobileEventLogger(context));
        File destination=new File(context.getExternalFilesDir(null),movementProbe ? "makcu-motion-probe.json" : "makcu-dispatch-test.json");
        try {
            controller.connectFirstSupportedDevice();
            long readyDeadline=SystemClock.elapsedRealtime()+15000;
            while (!controller.isReady() && SystemClock.elapsedRealtime()<readyDeadline) SystemClock.sleep(20);
            if (!controller.isReady()) throw new AssertionError(controller.report());
            Object target=field(controller,"activeTransport");
            Object lock=field(controller,"serialIoLock");
            long generation=(Long)field(controller,"activeConnectionGeneration");
            long lease=((AtomicLong)field(controller,"responseReaderLeaseGeneration")).get();
            Method begin=method(controller.getClass(),"beginExpectedResponse",String.class,long.class,long.class);
            Method await=method(controller.getClass(),"awaitExecutedAcknowledgement",target.getClass(),String.class,long.class,long.class,long.class);
            Method write=method(target.getClass(),"write",byte[].class,int.class);
            long[] latency=new long[movementProbe ? (hasKick ? 49 : 48) : 2000]; int successes=0, failures=0;
            JSONArray failedSamples=new JSONArray();
            JSONArray commands=new JSONArray();
            if(movementProbe) result.put("commands",commands);
            final long patternBase=SystemClock.elapsedRealtimeNanos();
            result.put("pattern_base_mono_ns",patternBase)
                    .put("clock_before_mono_ns",SystemClock.elapsedRealtimeNanos())
                    .put("clock_unix_ms",System.currentTimeMillis())
                    .put("clock_after_mono_ns",SystemClock.elapsedRealtimeNanos());
            long previousWriteStart=0;
            for (int i=0;i<latency.length;++i) {
                int delta=movementProbe && i<MakcuProbeSchedule.BALANCED_STEPS
                        ? MakcuProbeSchedule.balancedDelta(i) : 0;
                final int dx=hasKick && i==48 ? kickX : (verticalProbe ? 0 : delta);
                final int dy=hasKick && i==48 ? kickY : (verticalProbe ? delta : 0);
                final String command="km.move("+dx+","+dy+")\r\n";
                final byte[] bytes=command.getBytes(StandardCharsets.US_ASCII);
                final long nominal=movementProbe ? MakcuProbeSchedule.nominalStartNanos(patternBase,i,intervalMillis) : 0;
                final long eligible=movementProbe ? MakcuProbeSchedule.eligibleStartNanos(nominal,previousWriteStart,intervalMillis) : 0;
                if(movementProbe) sleepUntil(eligible);
                final long sentUnixMs=System.currentTimeMillis();
                long transactionStarted=0,writeStarted=0,writeFinished=0,echoStarted=0,echoFinished=0;
                long duration=-1;int written=-1;Throwable stepFailure=null;
                try {
                    synchronized(lock) {
                        transactionStarted=SystemClock.elapsedRealtimeNanos();
                        begin.invoke(controller,command,generation,lease);
                        writeStarted=SystemClock.elapsedRealtimeNanos();
                        try { written=(Integer)write.invoke(target,bytes,25); }
                        finally { writeFinished=SystemClock.elapsedRealtimeNanos(); }
                        if (written!=bytes.length) throw new AssertionError("Partial USB write: "+written);
                        echoStarted=SystemClock.elapsedRealtimeNanos();
                        try { duration=(Long)await.invoke(controller,target,command,transactionStarted,generation,lease); }
                        finally { echoFinished=SystemClock.elapsedRealtimeNanos(); }
                    }
                } catch(Throwable failure) {
                    stepFailure=failure;
                    throw failure;
                } finally {
                    // JSON/string work occurs after leaving the actual USB I/O
                    // lock; even a partial/failed write retains its timestamps.
                    if(movementProbe) commands.put(new JSONObject().put("index",i).put("dx",dx).put("dy",dy)
                            .put("sent_unix_ms",sentUnixMs).put("echo_us",duration)
                            .put("nominal_start_mono_ns",nominal).put("eligible_start_mono_ns",eligible)
                            .put("transaction_start_mono_ns",transactionStarted)
                            .put("write_start_mono_ns",writeStarted).put("write_end_mono_ns",writeFinished)
                            .put("echo_wait_start_mono_ns",echoStarted).put("echo_wait_end_mono_ns",echoFinished)
                            .put("written_bytes",written).put("expected_bytes",bytes.length)
                            .put("actual_write_start_interval_us",previousWriteStart==0 || writeStarted==0 ? -1 : (writeStarted-previousWriteStart)/1000)
                            .put("schedule_late_us",writeStarted==0 ? -1 : (writeStarted-nominal)/1000)
                            .put("step_failure",stepFailure==null ? JSONObject.NULL : stepFailure.toString()));
                    if(writeStarted!=0)previousWriteStart=writeStarted;
                }
                if (duration>=0) latency[successes++]=duration;
                else {
                    ++failures;
                    if(failedSamples.length()<8) failedSamples.put(new JSONObject().put("index",i)
                            .put("reads",((MakcuReadTrace)field(controller,"readTrace")).report(SystemClock.elapsedRealtimeNanos())));
                    // Avoid attributing a late response from the failed query to the next identical one.
                    SystemClock.sleep(100);
                }
                if(!movementProbe) SystemClock.sleep(2+i%7);
            }
            Arrays.sort(latency,0,successes);
            result.put("samples",latency.length).put("successful",successes).put("failed",failures)
                    .put("failures",failedSamples).put("passed",failures==0)
                    .put("report",controller.report());
            if(movementProbe) result.put("commands",commands);
            if(successes>0) result.put("echo_us_p50",latency[successes/2])
                    .put("echo_us_p95",latency[successes*95/100]).put("echo_us_p99",latency[successes*99/100]);
        } catch(Throwable failure) {
            result.put("passed",false).put("failure",android.util.Log.getStackTraceString(failure));
            throw failure;
        } finally {
            try {
                controller.destroy();
                result.put("cleanup_completed",true);
            } finally {
                result.put("finished_unix_ms",System.currentTimeMillis());
                Files.write(destination.toPath(),result.toString(2).getBytes(StandardCharsets.UTF_8));
            }
        }
    }
    private static void sleepUntil(long deadlineNanos) {
        long remaining;
        while((remaining=deadlineNanos-SystemClock.elapsedRealtimeNanos())>0) {
            SystemClock.sleep(Math.max(1,(remaining+999_999L)/1_000_000L));
        }
    }
    private static Object field(Object owner,String name) throws Exception {
        Field field=owner.getClass().getDeclaredField(name);field.setAccessible(true);return field.get(owner);
    }
    private static Method method(Class<?> type,String name,Class<?>... types) throws Exception {
        Method method=type.getDeclaredMethod(name,types);method.setAccessible(true);return method;
    }
}
