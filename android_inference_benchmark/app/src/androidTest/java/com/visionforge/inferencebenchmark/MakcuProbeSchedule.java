package com.visionforge.inferencebenchmark;

/** Test-only schedule arithmetic; no Android, USB or production output hooks. */
final class MakcuProbeSchedule {
    static final int DEFAULT_INTERVAL_MS = 80;
    static final int BALANCED_STEPS = 48;

    static int requireInterval(int millis) {
        if (millis != 8 && millis != 16 && millis != DEFAULT_INTERVAL_MS) {
            throw new IllegalArgumentException("movement interval_ms must be exactly 8, 16 or 80");
        }
        return millis;
    }

    static int balancedDelta(int index) {
        if (index < 0 || index >= BALANCED_STEPS) {
            throw new IllegalArgumentException("balanced pattern index outside 0..47");
        }
        return (1 + (index / 2 * 7) % 17) * (index % 2 == 0 ? 1 : -1);
    }

    static long nominalStartNanos(long base, int index, int intervalMillis) {
        if (index < 0 || index > BALANCED_STEPS) throw new IllegalArgumentException("step index");
        return Math.addExact(base, Math.multiplyExact((long) index, requireInterval(intervalMillis) * 1_000_000L));
    }

    static long eligibleStartNanos(long nominal, long previousWriteStart, int intervalMillis) {
        final long period = requireInterval(intervalMillis) * 1_000_000L;
        // A late echo/write must never make the next iterations catch up in a
        // burst faster than the explicitly requested minimum write interval.
        return previousWriteStart == 0 ? nominal : Math.max(nominal, Math.addExact(previousWriteStart, period));
    }
    private MakcuProbeSchedule() {}
}
