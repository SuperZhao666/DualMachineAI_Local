package com.visionforge.inferencebenchmark;

/** Completion-based backoff. Network notifications do not reset package failures. */
final class MobileRuntimeRetryPolicy {
    private int attempts;
    private long nextAttempt;
    private boolean terminal;
    private String failureCode = "none";

    boolean mayAttempt(long now) { return !terminal && now >= nextAttempt; }
    boolean terminal() { return terminal; }
    String failureCode() { return failureCode; }
    long nextAttempt() { return nextAttempt; }

    boolean recoverFatal(MobileRuntimeSnapshot snapshot, Runnable fullStop,
                         java.util.function.LongSupplier completedClock) {
        if (!snapshot.requiresFullRecovery) return false;
        fullStop.run();
        failed(completedClock.getAsLong(), MobilePipelineCoordinator.FailureDisposition.RETRYABLE,
                "native_full_recovery_required");
        return true;
    }

    void failed(long completedAt, MobilePipelineCoordinator.FailureDisposition disposition,
                String code) {
        if (terminal) return; // Only an explicit configuration/user reset clears this latch.
        failureCode = code == null || code.isBlank() ? "unknown" : code;
        terminal = disposition == MobilePipelineCoordinator.FailureDisposition.TERMINAL_INCOMPATIBLE;
        attempts = Math.min(5, attempts + 1);
        long delay = Math.min(60_000L, 5_000L << (attempts - 1));
        nextAttempt = completedAt > Long.MAX_VALUE - delay ? Long.MAX_VALUE : completedAt + delay;
    }

    void reset() { attempts = 0; nextAttempt = 0; terminal = false; failureCode = "none"; }
}
