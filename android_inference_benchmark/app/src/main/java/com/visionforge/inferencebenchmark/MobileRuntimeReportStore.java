package com.visionforge.inferencebenchmark;

/** An immutable sampled report. UI readers never call a native execution lock. */
final class MobileRuntimeReportStore {
    /** A report is historical evidence, and must not overwrite a later
     * synchronous gate transition twice. The lifecycle worker owns a cursor. */
    static final class Cursor {
        private Sample consumed;
        boolean consume(Sample sample, Runnable update) {
            if (sample == consumed) return false;
            update.run();
            consumed = sample;
            return true;
        }
    }
    static final class Sample {
        final long generation, lifecycleGeneration, sampledAtMillis,authorizationRevision;
        final MobileRuntimeSnapshot snapshot;
        Sample(long generation, long lifecycleGeneration, long sampledAtMillis, MobileRuntimeSnapshot snapshot) {
            this(generation,lifecycleGeneration,sampledAtMillis,-1,snapshot);
        }
        Sample(long generation,long lifecycleGeneration,long sampledAtMillis,long authorizationRevision,MobileRuntimeSnapshot snapshot) {
            this.generation = generation;
            this.lifecycleGeneration = lifecycleGeneration;
            this.sampledAtMillis = sampledAtMillis;
            this.authorizationRevision=authorizationRevision;
            this.snapshot = snapshot;
        }
    }
    private static long generation;
    private static volatile Sample latest = new Sample(0, 0, 0,
            MobileRuntimeSnapshot.from("running=0", "configured=0", "ready=0", ""));
    private MobileRuntimeReportStore() {}
    static Sample snapshot() { return latest; }
    static synchronized long begin() {
        ++generation;
        latest = new Sample(generation, 0, 0,
                MobileRuntimeSnapshot.from("running=0", "configured=0", "ready=0", ""));
        return generation;
    }
    static synchronized boolean publish(long expectedGeneration, long sampledAtMillis,
                                        MobileRuntimeSnapshot snapshot) {
        return publish(expectedGeneration, 0, sampledAtMillis, snapshot);
    }
    static synchronized boolean publish(long expectedGeneration, long lifecycleGeneration,
                                        long sampledAtMillis, MobileRuntimeSnapshot snapshot) {
        return publish(expectedGeneration,lifecycleGeneration,sampledAtMillis,-1,snapshot);
    }
    static synchronized boolean publish(long expectedGeneration,long lifecycleGeneration,
            long sampledAtMillis,long authorizationRevision,MobileRuntimeSnapshot snapshot) {
        if (expectedGeneration != generation) return false;
        latest = new Sample(generation, lifecycleGeneration, sampledAtMillis,authorizationRevision, snapshot);
        return true;
    }
}
