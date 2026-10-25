package com.visionforge.inferencebenchmark;

/** Readiness and delivery boundary for the selected phone-owned output transport. */
interface ControlOutputTransport {
    interface ReconnectListener {
        void onReconnectResult(boolean succeeded);
    }

    boolean isReady();

    void connectFirstSupportedDevice();

    /** Receives the final result of synchronous or permission-driven reopen attempts. */
    void setReconnectListener(ReconnectListener listener);

    /**
     * Enables or synchronously closes physical output delivery for queued moves.
     * Enabling fails while the delivery circuit is open.
     */
    boolean setOutputDeliveryAllowed(boolean allowed);

    /** Returns transport-level delivery evidence without probing or opening hardware. */
    MakcuDeliveryState deliveryState();

    default String completionEvent() { return "control_output_usb_write_completed"; }

    default String completionSemantics() {
        return "local_usb_sync_write_complete_not_device_or_physical_ack";
    }
}
