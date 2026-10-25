package com.visionforge.inferencebenchmark;

/** Final lifecycle authority; sampling never grants a later lifecycle permission. */
final class MobileRuntimeAuthorization {
    private long generation;
    private boolean foreground;
    private boolean destroyed;

    synchronized long generation() { return generation; }
    synchronized boolean foreground() { return foreground && !destroyed; }

    synchronized long updateForeground(boolean value, Runnable close) {
        if (destroyed) return generation;
        if (foreground != value || !value) ++generation;
        foreground = value;
        if (!value) close.run();
        return generation;
    }

    synchronized void destroy(Runnable close) {
        ++generation;
        foreground = false;
        destroyed = true;
        close.run();
    }

    synchronized boolean current(long expected) {
        return !destroyed && foreground && generation == expected;
    }

    synchronized boolean ifCurrent(long expected, Runnable action) {
        if (!current(expected)) return false;
        action.run();
        return true;
    }
}
