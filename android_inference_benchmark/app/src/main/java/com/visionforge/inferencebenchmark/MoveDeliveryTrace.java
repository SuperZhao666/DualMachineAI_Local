package com.visionforge.inferencebenchmark;

/** Fixed primitive flight recorder. No strings, disk writes or allocations on record(). */
final class MoveDeliveryTrace {
    static final int CAPACITY = 2048, WIDTH = 14;
    private final long[] values = new long[CAPACITY * WIDTH];
    private long total, emitted;

    synchronized void record(long ticket, int x, int y, long offered, long started,
            long returned, long completed, long acknowledged, long deadline,
            long generation, int outcome, int wrote) {
        long id = ++total;
        int p = (int) ((id - 1) % CAPACITY) * WIDTH;
        values[p++] = id; values[p++] = ticket; values[p++] = x; values[p++] = y;
        values[p++] = offered / 1000; values[p++] = started / 1000;
        values[p++] = returned / 1000; values[p++] = completed / 1000;
        values[p++] = acknowledged / 1000; values[p++] = deadline / 1000;
        values[p++] = generation; values[p++] = outcome; values[p++] = wrote;
        values[p] = offered > 0 && completed >= offered ? (completed - offered) / 1000 : -1;
    }

    String drain(String route, String clock) {
        final long[] copy;
        final long first, last, lost;
        synchronized (this) {
            last = total;
            first = Math.max(emitted + 1, Math.max(1, last - CAPACITY + 1));
            lost = Math.max(0, first - emitted - 1);
            int count = (int) Math.max(0, last - first + 1);
            copy = new long[count * WIDTH];
            for (int n = 0; n < count; ++n)
                System.arraycopy(values, (int) ((first + n - 1) % CAPACITY) * WIDTH, copy, n * WIDTH, WIDTH);
            emitted = last;
        }
        StringBuilder text = new StringBuilder(copy.length * 10 + 128);
        text.append("route=").append(route).append(" delivery_clock=").append(clock)
            .append(" delivery_trace_lost=").append(lost).append(" delivery_trace_v1=[");
        for (int n = 0; n < copy.length; ++n) {
            if (n > 0) text.append(n % WIDTH == 0 ? ';' : ',');
            text.append(copy[n]);
        }
        return text.append(']').toString();
    }
}
