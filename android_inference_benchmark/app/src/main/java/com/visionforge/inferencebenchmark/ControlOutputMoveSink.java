package com.visionforge.inferencebenchmark;

/** Native move callback sink owned by the selected phone-side output transport. */
interface ControlOutputMoveSink {
    boolean offerMoveFromNative(
            int deltaX, int deltaY, long ticket, long remainingBudgetUs);

    /**
     * Original CLOCK_MONOTONIC deadline, preserved across JNI, locks and queues.
     * Absolute-aware transports override this; a legacy-only sink must reject
     * rather than silently rebase the original visual lifetime.
     */
    default boolean offerMoveBeforeDeadline(
            int deltaX,int deltaY,long ticket,long absoluteMonotonicNanos) {
        return false;
    }

    default boolean offerMoveFromNative(int deltaX, int deltaY, long ticket) {
        return offerMoveFromNative(
                deltaX, deltaY, ticket, ControlMoveDeadline.DEFAULT_BUDGET_US);
    }

    boolean suspendMoveDeliveryForNativeRecovery(long generation);

    boolean resumeMoveDeliveryAfterNativeRecovery(long generation);

    boolean failClosedNativeMoveDelivery();
}
