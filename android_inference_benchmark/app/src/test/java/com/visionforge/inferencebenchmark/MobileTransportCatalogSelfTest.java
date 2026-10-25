package com.visionforge.inferencebenchmark;

/** Dependency-free contract for automatic CAT6 preference and Wi-Fi fallback. */
public final class MobileTransportCatalogSelfTest {
    public static void main(String[] arguments) {
        MobileTransportCatalog catalog = new MobileTransportCatalog();
        MobileTransportEndpoint wifi = MobileTransportEndpointTestFixtures.wirelessLan(
                41L, "192.168.1.42");
        require(catalog.upsert(wifi) == wifi, "Wi-Fi is selected when CAT6 is absent");
        require(!wifi.hostDiscovered, "Wi-Fi starts in multicast discovery mode");

        MobileTransportEndpoint discovered = wifi.withDiscoveredHost("192.168.1.18");
        require(discovered.hasSameNetworkRoute(wifi),
                "routine OS callbacks preserve the route during model loading");
        require(!discovered.hasSameNetworkRoute(null), "a lost route cancels preparation");
        require(!discovered.hasSameNetworkRoute(
                MobileTransportEndpointTestFixtures.wirelessLan(42L, "192.168.1.42")),
                "a replacement network cancels preparation even with the same address");
        require(!discovered.hasSameNetworkRoute(
                MobileTransportEndpointTestFixtures.wirelessLan(41L, "192.168.1.43")),
                "an address change cancels preparation on the same network");
        require(wifi.hasSameDataPlaneRoute(wifi),
                "an unchanged endpoint keeps the same data-plane route");
        require(!wifi.hasSameDataPlaneRoute(discovered),
                "Host discovery changes the data-plane route binding");
        require(catalog.upsert(discovered).hostDiscovered, "discovered Host is retained");
        require(catalog.upsert(MobileTransportEndpointTestFixtures.wirelessLan(
                41L, "192.168.1.42")).hostDiscovered,
                "routine Wi-Fi callbacks do not erase Host discovery");
        MobileTransportEndpoint differentDiscoveredHost =
                wifi.withDiscoveredHost("192.168.1.19");
        require(catalog.resetWirelessHostDiscoveryIfSelectedRoute(
                        differentDiscoveredHost).hostDiscovered,
                "a stale observation cannot reset a newer selected Host route");
        MobileTransportEndpoint rediscovery =
                catalog.resetWirelessHostDiscoveryIfSelectedRoute(discovered);
        require(!rediscovery.hostDiscovered,
                "a failed observation can clear its selected Wi-Fi Host binding");
        require(MobileTransportEndpoint.WIRELESS_DISCOVERY_IPV4.equals(
                        rediscovery.hostIpv4),
                "explicit reset returns to multicast discovery");
        require(MobileTransportEndpoint.WIRELESS_DISCOVERY_IPV4.equals(
                        discovered.readyAnnouncementIpv4()),
                "a discovered Wi-Fi Host never replaces the multicast ready destination");
        require(MobileTransportEndpoint.WIRELESS_DISCOVERY_BROADCAST_IPV4.equals(
                        discovered.readyAnnouncementBroadcastIpv4()),
                "Wi-Fi readiness also announces over limited LAN broadcast");
        require(EthernetTransportContract.HOST_IPV4.equals(
                        MobileTransportEndpointTestFixtures.cat6(56L)
                                .readyAnnouncementIpv4()),
                "CAT6 readiness remains fixed unicast");
        require(MobileTransportEndpointTestFixtures.cat6(56L)
                        .readyAnnouncementBroadcastIpv4().isEmpty(),
                "CAT6 readiness never emits a broadcast announcement");

        MobileTransportEndpoint cat6 = MobileTransportEndpointTestFixtures.cat6(57L);
        require(!cat6.hasSameDataPlaneRoute(
                        MobileTransportEndpointTestFixtures.cat6(58L)),
                "a network-handle change invalidates preflight evidence");
        require(catalog.upsert(cat6) == cat6, "CAT6 replaces Wi-Fi automatically");
        require(catalog.upsert(MobileTransportEndpointTestFixtures.wirelessLan(
                42L, "192.168.1.43")) == cat6,
                "Wi-Fi callbacks cannot displace live CAT6");
        require(catalog.resetSelectedWirelessHostDiscovery() == cat6,
                "explicit Wi-Fi reset cannot displace live CAT6");
        require(catalog.remove(57L).networkHandle == 41L,
                "Wi-Fi resumes automatically after CAT6 loss");
        require(catalog.remove(41L).networkHandle == 42L,
                "remaining Wi-Fi candidate is selected");
        require(catalog.remove(42L) == null, "catalog empties after all links are lost");
        networkLossRequiresCurrentActiveRouteEvidence();
    }

    private static void networkLossRequiresCurrentActiveRouteEvidence() {
        MobileTransportEndpoint active = MobileTransportEndpointTestFixtures.cat6(57L);
        require(!MobileNetworkLossPolicy.shouldFailClosed(41L, active, true),
                "a secondary Wi-Fi loss preserves a proven live CAT6 data plane");
        require(MobileNetworkLossPolicy.shouldFailClosed(57L, active, true),
                "loss of the actual active network closes even if OS evidence is lagging");
        require(MobileNetworkLossPolicy.shouldFailClosed(0L, active, true),
                "an unknown lost identity cannot preserve output");
        require(MobileNetworkLossPolicy.shouldFailClosed(41L, null, true),
                "an unknown active identity cannot preserve output");
        require(MobileNetworkLossPolicy.shouldFailClosed(41L, active, false),
                "an unverified or replaced active route cannot preserve output");
        MobileTransportEndpoint discovering = MobileTransportEndpointTestFixtures.wirelessLan(
                41L, "192.168.1.42");
        require(MobileNetworkLossPolicy.shouldFailClosed(57L, discovering, true),
                "host discovery does not prove an open data plane");
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
