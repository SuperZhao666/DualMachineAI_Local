package com.visionforge.inferencebenchmark;

/** Explicit owner tokens distinguish configuration handoff from client loss. */
final class MobileActivityOwnership {
    private long owner;
    private long handoff;
    synchronized long claim() { return ++owner; }
    synchronized boolean current(long candidate) { return candidate == owner; }
    synchronized boolean ifCurrent(long candidate, Runnable action) {
        if (!current(candidate)) return false;
        action.run(); return true;
    }
    synchronized void beginHandoff(long candidate) { if (current(candidate)) handoff = candidate; }
    synchronized long takeHandoff() { long result = handoff; handoff = 0; return result; }
}
