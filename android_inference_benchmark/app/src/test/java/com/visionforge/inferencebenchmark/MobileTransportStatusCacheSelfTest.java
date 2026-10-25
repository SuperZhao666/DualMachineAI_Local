package com.visionforge.inferencebenchmark;

final class MobileTransportStatusCacheSelfTest {
    private static final String READY="serial_open=1 adapter_identity_verified=1 protocol_identity_verified=1 delivery_circuit_open=0";
    static void run() {
        long service=MobileTransportStatusCache.begin();
        long pipeline=MobileRuntimeReportStore.begin();
        // No Host or prepared model: the pipeline snapshot remains empty,
        // while a genuinely verified USB transport has its own status path.
        require(!MobileRuntimeReportStore.snapshot().snapshot.makcuReady);
        require(MobileTransportStatusCache.publish(service,100,READY));
        require(MobileTransportStatusCache.snapshot().readyAt(100));
        // Model failure, stop and restart clear only native pipeline reports.
        for(int transition=0;transition<3;++transition) {
            MobileRuntimeReportStore.begin();
            require(!MobileRuntimeReportStore.snapshot().snapshot.makcuReady);
            require(MobileTransportStatusCache.snapshot().readyAt(200));
        }
        require(!MobileRuntimeReportStore.publish(pipeline,100,
                MobileRuntimeSnapshot.from("","","",READY)));
        require(MobileTransportStatusCache.snapshot().readyAt(200));
        require(MobileTransportStatusCache.publish(service,250,"serial_open=0"));
        require(!MobileTransportStatusCache.snapshot().readyAt(250));
        require(!MobileTransportStatusCache.publish(service,200,READY));
        require(!MobileTransportStatusCache.snapshot().readyAt(250));
        long replacement=MobileTransportStatusCache.begin();
        require(MobileTransportStatusCache.publish(replacement,300,READY));
        require(!MobileTransportStatusCache.publish(service,400,"serial_open=0"));
        require(!MobileTransportStatusCache.end(service));
        require(MobileTransportStatusCache.snapshot().readyAt(300));
        require(MobileTransportStatusCache.snapshot().readyAt(2300));
        require(!MobileTransportStatusCache.snapshot().readyAt(2301));
        require(!MobileTransportStatusCache.snapshot().readyAt(299));
        require(MobileTransportStatusCache.end(replacement));
        require(!MobileTransportStatusCache.snapshot().readyAt(300));
        require(!MobileTransportStatusCache.publish(replacement,500,READY));
    }
    private static void require(boolean condition) { if(!condition)throw new AssertionError("transport status pipeline coupling regression"); }
}
