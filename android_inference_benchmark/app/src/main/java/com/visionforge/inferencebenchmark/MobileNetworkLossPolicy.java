package com.visionforge.inferencebenchmark;

/** Loss callbacks may preserve output only with evidence about the open data plane. */
final class MobileNetworkLossPolicy {
    private MobileNetworkLossPolicy() { }

    static boolean shouldFailClosed(long lostNetworkHandle,
            MobileTransportEndpoint activeEndpoint, boolean activeRouteStillAvailable) {
        return lostNetworkHandle <= 0L
                || activeEndpoint == null
                || !activeEndpoint.isReadyForDataPlane()
                || lostNetworkHandle == activeEndpoint.networkHandle
                || !activeRouteStillAvailable;
    }
}
