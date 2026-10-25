package com.visionforge.inferencebenchmark;

import java.util.Arrays;

/** Bounded software-stage timings. Completion is never a physical-motion ACK. */
final class ControlTransportTiming {
    private static final int CAPACITY = 256;
    private final long[] queue = new long[CAPACITY], call = new long[CAPACITY];
    private final long[] completion = new long[CAPACITY];
    private int next, size;
    private long successful, failed, invalid;

    synchronized void record(long offered, long started, long returned, long completed,
                             boolean success) {
        if (started < offered || returned < started || completed < returned) {
            ++invalid;
            return;
        }
        queue[next] = (started - offered) / 1000;
        call[next] = (returned - started) / 1000;
        completion[next] = (completed - offered) / 1000;
        next = (next + 1) % CAPACITY;
        size = Math.min(CAPACITY, size + 1);
        if (success) ++successful; else ++failed;
    }

    synchronized String report() {
        return "window_samples=" + size + " successful=" + successful + " failed=" + failed
                + " invalid_timestamps=" + invalid + " " + quantiles("queue_us", queue)
                + " " + quantiles("write_call_us", call)
                + " " + quantiles("offer_to_completion_us", completion)
                + " physical_execution_measured=false";
    }

    private String quantiles(String name, long[] values) {
        if (size == 0) return name + "_p50=unknown " + name + "_p95=unknown " + name + "_p99=unknown";
        long[] sorted = Arrays.copyOf(values, size);
        Arrays.sort(sorted);
        return name + "_p50=" + percentile(sorted, 50)
                + " " + name + "_p95=" + percentile(sorted, 95)
                + " " + name + "_p99=" + percentile(sorted, 99);
    }

    private static long percentile(long[] sorted, int percent) {
        return sorted[Math.max(0, (sorted.length * percent + 99) / 100 - 1)];
    }
}
