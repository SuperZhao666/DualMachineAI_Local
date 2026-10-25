package com.visionforge.inferencebenchmark;

import java.util.concurrent.atomic.AtomicReference;

/** Stable JNI callback target that delegates native move delivery to the active transport. */
final class ControlOutputMoveDispatcher {
    private static final AtomicReference<ControlOutputMoveSink> sink = new AtomicReference<>();

    private ControlOutputMoveDispatcher() {
    }

    static void registerSink(ControlOutputMoveSink nextSink) {
        if (nextSink == null) {
            throw new IllegalArgumentException("nextSink is required");
        }
        sink.set(nextSink);
    }

    static void clearSink(ControlOutputMoveSink expectedSink) {
        if (expectedSink == null) {
            sink.set(null);
            return;
        }
        sink.compareAndSet(expectedSink, null);
    }

    static boolean offerNativeMove(int deltaX, int deltaY, long ticket) {
        return offerNativeMove(
                deltaX, deltaY, ticket, ControlMoveDeadline.DEFAULT_BUDGET_US);
    }

    static boolean offerNativeMove(
            int deltaX, int deltaY, long ticket, long remainingBudgetUs) {
        ControlOutputMoveSink current = sink.get();
        return current != null && current.offerMoveFromNative(
                deltaX, deltaY, ticket, remainingBudgetUs);
    }

    /** JNI receives the original native CLOCK_MONOTONIC deadline in microseconds. */
    static boolean offerNativeMoveBeforeDeadline(
            int deltaX,int deltaY,long ticket,long absoluteMonotonicMicros) {
        long deadline=ControlMoveDeadline.monotonicMicrosToNanos(absoluteMonotonicMicros);
        if(ticket<=0L || deadline<=0L)return false;
        ControlOutputMoveSink current=sink.get();
        // A valid offer that is already old still reaches the transport's
        // explicit unsent-discard path; returning false would classify it as
        // a broken delivery session in the native boolean offer protocol.
        return current!=null && current.offerMoveBeforeDeadline(deltaX,deltaY,ticket,deadline);
    }

    private static boolean suspendNativeDeliveryForRecovery(long generation) {
        ControlOutputMoveSink current = sink.get();
        return current != null && current.suspendMoveDeliveryForNativeRecovery(generation);
    }

    private static boolean resumeNativeDeliveryAfterRecovery(long generation) {
        ControlOutputMoveSink current = sink.get();
        return current != null && current.resumeMoveDeliveryAfterNativeRecovery(generation);
    }

    private static boolean failClosedNativeDelivery() {
        ControlOutputMoveSink current = sink.get();
        return current != null && current.failClosedNativeMoveDelivery();
    }
}
