package com.visionforge.inferencebenchmark;

final class ControlTransportTimingSelfTest {
    static void run() {
        ControlTransportTiming timing = new ControlTransportTiming();
        require(timing.report().contains("queue_us_p50=unknown"));
        timing.record(1000, 11000, 15000, 31000, true);
        String report = timing.report();
        require(report.contains("queue_us_p50=10"));
        require(report.contains("write_call_us_p50=4"));
        require(report.contains("offer_to_completion_us_p50=30"));
        timing.record(1000, 0, 2000, 3000, true);
        require(timing.report().contains("invalid_timestamps=1"));
        for (int i=0; i<256; ++i) timing.record(1000, 2000, 3000, 4000, i != 0);
        report = timing.report();
        require(report.contains("window_samples=256"));
        require(report.contains("queue_us_p99=1"));
        require(report.contains("offer_to_completion_us_p99=3"));
        require(report.contains("failed=1"));
        require(report.contains("physical_execution_measured=false"));
        MakcuPendingMoveSlot slot = new MakcuPendingMoveSlot();
        slot.offer(1, 1, 9000, 1000);
        slot.offer(2, 2, 10000, 2000);
        MakcuPendingMoveSlot.PendingMove move = new MakcuPendingMoveSlot.PendingMove();
        require(slot.takeInto(move));
        require(move.ticket == 2 && move.offeredNanos == 2000 && move.deadlineNanos == 10000);
    }
    private static void require(boolean value) {
        if (!value) throw new AssertionError("transport timing stages/ring/mailbox");
    }
}
