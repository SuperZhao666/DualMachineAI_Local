package com.visionforge.inferencebenchmark;

/** A different verified peer may repair an initially selected, never-working route. */
final class WirelessHostRediscoveryPolicy {
    // Existing discovery permits an 8-second scan; allow a further live-window
    // grace before replacing a route that has never delivered any video.
    static final long STARTUP_GRACE_MILLIS = 10_000;
    private WirelessHostRediscoveryPolicy() {}
    static boolean shouldReset(MobileTransportEndpoint endpoint, long startedAtMillis,
            long nowMillis, long receivedAccessUnits, String verifiedFreshRequester) {
        return endpoint != null && !endpoint.isCat6() && endpoint.hostDiscovered
                && startedAtMillis > 0 && nowMillis >= startedAtMillis
                && nowMillis-startedAtMillis >= STARTUP_GRACE_MILLIS
                && receivedAccessUnits == 0
                && MobileTransportEndpoint.isUsableUnicastIpv4(verifiedFreshRequester)
                && !endpoint.localIpv4.equals(verifiedFreshRequester)
                && !endpoint.hostIpv4.equals(verifiedFreshRequester);
    }
}
