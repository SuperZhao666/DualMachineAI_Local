package com.visionforge.inferencebenchmark;

/** Validates explicit delay evidence before any calibration file is changed. */
final class MobileCalibrationPersistencePolicy {
    static final class Decision {
        final boolean writable;
        final String reason;
        final double[] values;
        private Decision(boolean writable, String reason, double[] values) {
            this.writable=writable; this.reason=reason; this.values=values;
        }
    }
    static Decision evaluate(double[] values) {
        if (values == null || values.length != 11) return skip("delay_evidence_layout_unavailable");
        double[] v=values.clone();
        for (double value:v) if (!Double.isFinite(value)) return skip("calibration_snapshot_nonfinite");
        if ((v[5]!=0 && v[5]!=1) || (v[7]!=0 && v[7]!=1)) return skip("calibration_snapshot_invalid_flag");
        if (v[5]!=1) return skip("response_not_trusted");
        if (v[6]<=0 || Math.rint(v[6])!=v[6]) return skip("no_new_response_fit");
        if (v[7]!=1) return skip("delay_not_identifiable");
        if (v[0]<.2F || v[0]>4 || v[1]<.2F || v[1]>4
                || v[3]<.82F || v[3]>1 || v[4]<.82F || v[4]>1)
            return skip("calibration_snapshot_invalid_response");
        if (!microseconds(v[2]) || !microseconds(v[8]) || !microseconds(v[9])
                || v[8]>v[9] || v[2]<v[8] || v[2]>v[9])
            return skip("calibration_snapshot_invalid_delay_range");
        if (v[10]<1 || v[10]>24 || Math.rint(v[10])!=v[10])
            return skip("calibration_snapshot_invalid_delay_candidates");
        return new Decision(true,"identified_delay",v);
    }
    private static boolean microseconds(double value) {
        return value>=8_000 && value<=100_000 && Math.rint(value)==value;
    }
    private static Decision skip(String reason) { return new Decision(false,reason,null); }

    static final class SkipThrottle {
        private String scope;
        private long lastAt;
        private boolean reported;
        boolean shouldReport(String currentScope, long nowMillis) {
            boolean newScope=!currentScope.equals(scope);
            if (!newScope && reported && nowMillis>=lastAt && nowMillis-lastAt<5_000) return false;
            scope=currentScope; lastAt=nowMillis; reported=true; return true;
        }
    }
}
