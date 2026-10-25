package com.visionforge.inferencebenchmark;

final class WirelessHostRediscoveryPolicySelfTest {
    static void run() {
        MobileTransportEndpoint wifi=MobileTransportEndpointTestFixtures.wirelessLan(7,"192.168.1.42")
                .withDiscoveredHost("192.168.1.99");
        require(WirelessHostRediscoveryPolicy.shouldReset(wifi,100,10_100,0,"192.168.1.22"));
        require(!WirelessHostRediscoveryPolicy.shouldReset(wifi,100,10_099,0,"192.168.1.22"));
        require(!WirelessHostRediscoveryPolicy.shouldReset(wifi,100,20_000,1,"192.168.1.22"));
        for(String bad:new String[]{"none","0.0.0.0","127.0.0.1","169.254.1.1","239.1.1.1","192.168.1.42","192.168.1.99"})
            require(!WirelessHostRediscoveryPolicy.shouldReset(wifi,100,20_000,0,bad));
        require(!WirelessHostRediscoveryPolicy.shouldReset(wifi,100,99,0,"192.168.1.22"));
        require(!WirelessHostRediscoveryPolicy.shouldReset(wifi.forWirelessHostDiscovery(),100,20_000,0,"192.168.1.22"));
        require(!WirelessHostRediscoveryPolicy.shouldReset(MobileTransportEndpointTestFixtures.cat6(8),100,20_000,0,"192.168.1.22"));
        MobileTransportCatalog catalog=new MobileTransportCatalog();catalog.upsert(wifi);
        require(!catalog.resetWirelessHostDiscoveryIfSelectedRoute(wifi).hostDiscovered);
        catalog.upsert(wifi);catalog.upsert(MobileTransportEndpointTestFixtures.cat6(8));
        require(catalog.resetWirelessHostDiscoveryIfSelectedRoute(wifi).isCat6());
    }
    private static void require(boolean ok) { if(!ok) throw new AssertionError("restricted wireless rediscovery"); }
}
