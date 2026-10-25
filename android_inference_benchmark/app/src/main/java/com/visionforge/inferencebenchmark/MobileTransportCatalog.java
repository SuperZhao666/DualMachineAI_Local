package com.visionforge.inferencebenchmark;

import java.util.LinkedHashMap;
import java.util.Map;

/** Owns live Android transport candidates and enforces CAT6-over-Wi-Fi selection. */
final class MobileTransportCatalog {
    private final Map<Long, MobileTransportEndpoint> candidates = new LinkedHashMap<>();
    private long selectedNetworkHandle;

    synchronized MobileTransportEndpoint upsert(MobileTransportEndpoint endpoint) {
        if (endpoint == null || endpoint.networkHandle <= 0L) {
            throw new IllegalArgumentException("valid transport endpoint is required");
        }
        MobileTransportEndpoint existing = candidates.get(endpoint.networkHandle);
        if (existing != null
                && existing.kind == endpoint.kind
                && existing.localIpv4.equals(endpoint.localIpv4)
                && existing.hostDiscovered
                && !endpoint.hostDiscovered) {
            endpoint = existing;
        }
        candidates.put(endpoint.networkHandle, endpoint);
        return selectPreferred();
    }

    synchronized MobileTransportEndpoint remove(long networkHandle) {
        candidates.remove(networkHandle);
        if (selectedNetworkHandle == networkHandle) selectedNetworkHandle = 0L;
        return selectPreferred();
    }

    synchronized MobileTransportEndpoint selected() {
        return selectPreferred();
    }

    synchronized MobileTransportEndpoint resetSelectedWirelessHostDiscovery() {
        MobileTransportEndpoint selected = selectPreferred();
        if (selected == null || selected.isCat6() || !selected.hostDiscovered) {
            return selected;
        }
        MobileTransportEndpoint discoveryEndpoint =
                selected.forWirelessHostDiscovery();
        candidates.put(discoveryEndpoint.networkHandle, discoveryEndpoint);
        return selectPreferred();
    }

    synchronized MobileTransportEndpoint
            resetWirelessHostDiscoveryIfSelectedRoute(
                    MobileTransportEndpoint expectedRoute) {
        MobileTransportEndpoint selected = selectPreferred();
        if (expectedRoute == null
                || !expectedRoute.hasSameDataPlaneRoute(selected)) {
            return selected;
        }
        return resetSelectedWirelessHostDiscovery();
    }

    synchronized void clear() {
        candidates.clear();
        selectedNetworkHandle = 0L;
    }

    private MobileTransportEndpoint selectPreferred() {
        MobileTransportEndpoint current = candidates.get(selectedNetworkHandle);
        if (current != null && current.isCat6()) return current;

        for (MobileTransportEndpoint candidate : candidates.values()) {
            if (candidate.isCat6()) {
                selectedNetworkHandle = candidate.networkHandle;
                return candidate;
            }
        }
        if (current != null) return current;
        for (MobileTransportEndpoint candidate : candidates.values()) {
            selectedNetworkHandle = candidate.networkHandle;
            return candidate;
        }
        selectedNetworkHandle = 0L;
        return null;
    }
}
