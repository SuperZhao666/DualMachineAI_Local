package com.visionforge.inferencebenchmark;

/** Service-owned USB status, independent of video/model pipeline lifetime. */
final class MobileTransportStatusCache {
    static final long MAXIMUM_AGE_MILLIS = 2_000;
    static final class Sample {
        final long generation, sampledAtMillis;
        final String report;
        private final boolean ready;
        Sample(long generation, long sampledAtMillis, String report) {
            this.generation=generation;this.sampledAtMillis=sampledAtMillis;
            this.report=report==null?"":report;
            ready=MobileRuntimeSnapshot.makcuReadyFromReport(this.report);
        }
        boolean readyAt(long nowMillis) {
            return ready && sampledAtMillis>0 && nowMillis>=sampledAtMillis
                    && nowMillis-sampledAtMillis<=MAXIMUM_AGE_MILLIS;
        }
    }
    private static long generation;
    private static volatile Sample latest = new Sample(0,0,"");
    private MobileTransportStatusCache() {}
    static Sample snapshot() { return latest; }
    static synchronized long begin() {
        latest=new Sample(++generation,0,"");return generation;
    }
    static synchronized boolean publish(long owner,long sampledAtMillis,String report) {
        if (owner!=generation || sampledAtMillis<=0 || sampledAtMillis<latest.sampledAtMillis) return false;
        latest=new Sample(owner,sampledAtMillis,report);return true;
    }
    static synchronized boolean end(long owner) {
        if(owner!=generation)return false;
        latest=new Sample(++generation,0,"");return true;
    }
}
