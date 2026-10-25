package com.visionforge.inferencebenchmark;

import android.content.Context;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.time.Instant;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;

/** Bounded asynchronous local event log shared by all runtime components. */
final class MobileEventLogger implements MobileRuntimeEventSink {
    private static final String TAG = "VisionForgeMobile";
    private static final String DIAGNOSTICS_DIRECTORY_NAME = "diagnostics";
    private static final String EVENT_FILE_NAME = "visionforge-mobile-runtime.jsonl";
    private static final Object FILE_WRITE_LOCK = new Object();
    private static final String PROCESS_TRACE_ID = UUID.randomUUID().toString();
    private static final long PROCESS_START_ELAPSED_MILLIS = SystemClock.elapsedRealtime();
    private static final AtomicLong PROCESS_SEQUENCE = new AtomicLong();
    private static AsyncDiagnosticLog sharedLog;
    private final AsyncDiagnosticLog log;

    MobileEventLogger(Context context) {
        Context requiredContext = Objects.requireNonNull(context, "context");
        File external = requiredContext.getExternalFilesDir(null);
        File directory = new File(external != null ? external : requiredContext.getFilesDir(),
                DIAGNOSTICS_DIRECTORY_NAME);
        if (!directory.exists() && !directory.mkdirs()) {
            Log.e(TAG, "mobile diagnostics directory unavailable path=" + directory);
        }
        synchronized (FILE_WRITE_LOCK) {
            if (sharedLog == null) sharedLog = new AsyncDiagnosticLog(
                    new File(directory, EVENT_FILE_NAME), line -> Log.i(TAG, line));
            log = sharedLog;
        }
    }

    @Override
    public void write(String event, String detail) {
        String safeEvent = MobileLogSanitizer.sanitize(event);
        String boundedDetail = detail != null && detail.length() > 32000
                ? detail.substring(0, 32000) + " detail_truncated=true" : detail;
        String safeDetail = MobileLogSanitizer.sanitize(boundedDetail);
        String safeThreadName = MobileLogSanitizer.sanitize(
                Thread.currentThread().getName());
        synchronized (FILE_WRITE_LOCK) {
            long timestampUnixMillis = System.currentTimeMillis();
            long monotonicMicros = System.nanoTime() / 1000L;
            String line = "{\"timestamp_utc\":\"" + Instant.ofEpochMilli(
                    timestampUnixMillis) + "\",\"timestamp_unix_ms\":" + timestampUnixMillis
                    + ",\"monotonic_us\":" + monotonicMicros
                    + ",\"elapsed_ms\":"
                    + (SystemClock.elapsedRealtime() - PROCESS_START_ELAPSED_MILLIS)
                    + ",\"trace_id\":\"" + PROCESS_TRACE_ID + "\",\"sequence\":"
                    + PROCESS_SEQUENCE.incrementAndGet()
                    + ",\"process_id\":" + Process.myPid()
                    + ",\"thread_name\":\"" + escape(safeThreadName) + "\""
                    + ",\"event\":\"" + escape(safeEvent) + "\",\"detail\":\""
                    + escape(safeDetail) + "\"}\n";
            log.offer(line.trim());
        }
    }

    long exportTo(OutputStream output) throws IOException {
        Objects.requireNonNull(output, "output");
        return log.exportTo(output);
    }

    String status() { return log.status(); }

    void flushBeforeProcessExit() {
        if ("visionforge-diagnostics".equals(Thread.currentThread().getName())) return;
        try { log.flush(); } catch (IOException ignored) { Log.e(TAG, "crash diagnostic flush failed"); }
    }

    private static String escape(String value) {
        StringBuilder escaped = new StringBuilder(value.length() + 16);
        for (int index = 0; index < value.length(); index++) {
            char character = value.charAt(index);
            switch (character) {
                case '\"':
                    escaped.append("\\\"");
                    break;
                case '\\':
                    escaped.append("\\\\");
                    break;
                case '\b':
                    escaped.append("\\b");
                    break;
                case '\f':
                    escaped.append("\\f");
                    break;
                case '\n':
                    escaped.append("\\n");
                    break;
                case '\r':
                    escaped.append("\\r");
                    break;
                case '\t':
                    escaped.append("\\t");
                    break;
                default:
                    if (character < 0x20) {
                        escaped.append(String.format("\\u%04x", (int) character));
                    } else {
                        escaped.append(character);
                    }
                    break;
            }
        }
        return escaped.toString();
    }
}
