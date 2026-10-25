package com.visionforge.inferencebenchmark;

/** Bounded USB-only flight recorder. Formatting occurs only on an ACK failure. */
final class MakcuReadTrace {
    private final byte[][] bytes = new byte[32][64];
    private final long[] ended = new long[32], duration = new long[32];
    private final int[] counts = new int[32];
    private int next, size;

    synchronized void record(byte[] data, int count, long startNanos, long endNanos) {
        counts[next] = count;
        ended[next] = endNanos;
        duration[next] = endNanos - startNanos;
        if (count > 0) System.arraycopy(data, 0, bytes[next], 0, Math.min(count, 64));
        next = (next + 1) % bytes.length;
        size = Math.min(size + 1, bytes.length);
    }

    synchronized String report(long now) {
        StringBuilder out = new StringBuilder();
        final char[] hex = "0123456789abcdef".toCharArray();
        for (int n = 0; n < size; ++n) {
            int i = (next - size + n + bytes.length) % bytes.length;
            out.append("[age_us=").append((now-ended[i])/1000)
                    .append(" duration_us=").append(duration[i]/1000)
                    .append(" count=").append(counts[i]).append(" hex=");
            for (int j=0; j<Math.min(counts[i],64); ++j) {
                int value=bytes[i][j]&255;
                out.append(hex[value>>>4]).append(hex[value&15]);
            }
            out.append(']');
        }
        return out.toString();
    }
}
