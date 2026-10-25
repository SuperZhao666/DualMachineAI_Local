package com.visionforge.inferencebenchmark;

/** User-visible target class or geometric aim point within the selected game model. */
public enum MobileAimTarget {
    BODY("body"),
    HEAD("head");

    public final String storageToken;

    MobileAimTarget(String storageToken) {
        this.storageToken = storageToken;
    }

    public static MobileAimTarget fromStorage(String value, MobileAimTarget fallback) {
        if (value != null) {
            for (MobileAimTarget target : values()) {
                if (target.storageToken.equals(value)) return target;
            }
        }
        return fallback;
    }
}
