package com.visionforge.inferencebenchmark;

/** Move-only Bluetooth HID transport core backed by Android's HID device API. */
final class BluetoothHidMouseTransportCore implements ControlOutputTransport, ControlOutputMoveSink {
    interface SessionPort {
        BluetoothHidOutputFailClosedPolicy.SessionState sessionState();

        /** Consumes this report synchronously; the caller reuses its buffer. */
        boolean sendMouseReport(byte[] report);

        void connectFirstSupportedHost();

        /** Publishes asynchronous Android profile/app/host state transitions. */
        default void setSessionStateListener(Runnable listener) {
        }
    }

    interface MoveCompletionPort {
        boolean reportApiAccepted(long ticket, long acceptanceMicros);
    }

    interface NanoClock {
        long nowNanos();
    }

    enum MoveSubmission {
        API_ACCEPTED,
        DISCARDED_BEFORE_SEND,
        CANCELLED_BEFORE_SEND_FOR_RECOVERY,
        REJECTED
    }

    private final SessionPort sessionPort;
    private final MoveCompletionPort moveCompletionPort;
    private final NanoClock nanoClock;
    private final MakcuDeliveryCircuit deliveryCircuit = new MakcuDeliveryCircuit(1);
    private final byte[] reusableMouseReport = new byte[4];
    private boolean outputDeliveryAllowed;
    private boolean deliveryAllowedBeforeRecovery;
    private boolean nativeRecoverySuspended;
    private long recoveryGeneration;
    private long recoveryCancelledDeliveryGeneration;
    /** Survives resume so a move queued before suspend is not sent afterward. */
    private long recoveryRetiredDeliveryGeneration;
    private ReconnectListener reconnectListener;
    private String lastSessionDecisionReason = "";
    private boolean disposed;
    private long deliveryGeneration = 1;

    BluetoothHidMouseTransportCore(
            SessionPort sessionPort,
            MoveCompletionPort moveCompletionPort) {
        this(sessionPort, moveCompletionPort, System::nanoTime);
    }

    BluetoothHidMouseTransportCore(
            SessionPort sessionPort,
            MoveCompletionPort moveCompletionPort,
            NanoClock nanoClock) {
        if (sessionPort == null) throw new IllegalArgumentException("sessionPort");
        if (moveCompletionPort == null) throw new IllegalArgumentException("moveCompletionPort");
        if (nanoClock == null) throw new IllegalArgumentException("nanoClock");
        this.sessionPort = sessionPort;
        this.moveCompletionPort = moveCompletionPort;
        this.nanoClock = nanoClock;
        BluetoothHidOutputFailClosedPolicy.Decision initialDecision =
                BluetoothHidOutputFailClosedPolicy.evaluate(sessionPort.sessionState());
        lastSessionDecisionReason = initialDecision.reason;
        sessionPort.setSessionStateListener(this::onSessionStateChanged);
    }

    @Override
    public synchronized boolean isReady() {
        return isReadyLocked();
    }

    private boolean isReadyLocked() {
        return !disposed && !deliveryCircuit.snapshot().circuitOpen
                && BluetoothHidOutputFailClosedPolicy.evaluate(sessionPort.sessionState())
                .outputAllowed;
    }

    @Override
    public void connectFirstSupportedDevice() {
        ReconnectListener listener;
        boolean ready;
        synchronized (this) {
            sessionPort.connectFirstSupportedHost();
            BluetoothHidOutputFailClosedPolicy.Decision decision =
                    BluetoothHidOutputFailClosedPolicy.evaluate(sessionPort.sessionState());
            lastSessionDecisionReason = decision.reason;
            if (decision.outputAllowed) deliveryCircuit.resetAfterSuccessfulReconnect();
            listener = reconnectListener;
            ready = isReadyLocked();
        }
        // Never invoke coordinator code while holding the transport monitor:
        // coordinator reconciliation calls back into this transport and would
        // otherwise create a Coordinator -> Transport / Transport -> Coordinator
        // lock inversion across the runtime and Bluetooth callback threads.
        if (listener != null) listener.onReconnectResult(ready);
    }

    @Override
    public synchronized void setReconnectListener(ReconnectListener listener) {
        reconnectListener = listener;
    }

    @Override
    public synchronized boolean setOutputDeliveryAllowed(boolean allowed) {
        if (!allowed) {
            ++deliveryGeneration;
            outputDeliveryAllowed = false;
            // An explicit close during a temporary recovery hold revokes
            // restoration too; the matching resume must not re-open output.
            deliveryAllowedBeforeRecovery = false;
            recoveryCancelledDeliveryGeneration = 0L;
            recoveryRetiredDeliveryGeneration = 0L;
            return true;
        }
        if (nativeRecoverySuspended || !isReady()) {
            ++deliveryGeneration;
            outputDeliveryAllowed = false;
            return false;
        }
        outputDeliveryAllowed = true;
        return true;
    }

    @Override
    public synchronized MakcuDeliveryState deliveryState() {
        return deliveryCircuit.snapshot();
    }

    @Override public String completionEvent() { return "control_output_bluetooth_api_accepted"; }
    @Override public String completionSemantics() {
        return "android_hid_stack_accepted_not_host_or_physical_ack";
    }

    @Override
    public boolean offerMoveFromNative(
            int deltaX, int deltaY, long ticket, long remainingBudgetUs) {
        if (ticket <= 0L || remainingBudgetUs <= 0L) return false;
        long deadlineNanos = ControlMoveDeadline.deadlineNanos(
                nanoClock.nowNanos(), remainingBudgetUs);
        return offerMoveBeforeDeadline(deltaX,deltaY,ticket,deadlineNanos);
    }

    @Override public boolean offerMoveBeforeDeadline(
            int deltaX,int deltaY,long ticket,long deadlineNanos) {
        if(ticket<=0L || deadlineNanos<=0L)return false;
        // Include monitor wait in the original visual lifetime as well.
        synchronized (this) {
            return sendMove(deltaX, deltaY, ticket, true, deadlineNanos)
                    == MoveSubmission.API_ACCEPTED;
        }
    }

    synchronized long nativeDeliveryGeneration() {
        return !disposed && outputDeliveryAllowed && !nativeRecoverySuspended ? deliveryGeneration : 0;
    }

    synchronized MoveSubmission offerMoveBeforeDeadlineAtGeneration(
            int x, int y, long ticket, long deadlineNanos, long generation) {
        if (ticket <= 0L || disposed)
            return MoveSubmission.REJECTED;
        if (generation != deliveryGeneration) {
            // Native keeps this ticket live until Java suspension has drained
            // active API work. Resume can finish before the worker reaches this
            // monitor; the retired generation still must not be sent or reported
            // as a transport failure.
            boolean recoveryCancellation = generation != 0L && (
                    (nativeRecoverySuspended && generation == recoveryCancelledDeliveryGeneration)
                    || (recoveryRetiredDeliveryGeneration != 0L
                    && generation <= recoveryRetiredDeliveryGeneration));
            if (recoveryCancellation) {
                BluetoothHidOutputFailClosedPolicy.Decision decision =
                        BluetoothHidOutputFailClosedPolicy.evaluate(sessionPort.sessionState());
                if (!decision.outputAllowed) {
                    recordDeliveryFailure(decision.reason);
                    return MoveSubmission.REJECTED;
                }
                return MoveSubmission.CANCELLED_BEFORE_SEND_FOR_RECOVERY;
            }
            return MoveSubmission.REJECTED;
        }
        // Never rebase a remaining budget after waiting for this monitor.
        return sendMove(x, y, ticket, true, deadlineNanos);
    }

    /** Debug-only direct HID send that never completes or mutates a native move ticket. */
    synchronized boolean sendDiagnosticMove(int deltaX, int deltaY) {
        return sendMove(deltaX, deltaY, 0L, false, Long.MAX_VALUE)
                == MoveSubmission.API_ACCEPTED;
    }

    private MoveSubmission sendMove(
            int deltaX,
            int deltaY,
            long ticket,
            boolean reportNativeApiAcceptance,
            long deadlineNanos) {
        int boundedX = clampMouseDelta(deltaX);
        int boundedY = clampMouseDelta(deltaY);
        if (boundedX == 0 && boundedY == 0) return MoveSubmission.REJECTED;
        if (disposed || !outputDeliveryAllowed || nativeRecoverySuspended)
            return MoveSubmission.REJECTED;
        if (reportNativeApiAcceptance
                && ControlMoveDeadline.isExpired(nanoClock.nowNanos(), deadlineNanos)) {
            return MoveSubmission.DISCARDED_BEFORE_SEND;
        }

        BluetoothHidOutputFailClosedPolicy.Decision decision =
                BluetoothHidOutputFailClosedPolicy.evaluate(sessionPort.sessionState());
        if (!decision.outputAllowed) {
            recordDeliveryFailure(decision.reason);
            return MoveSubmission.REJECTED;
        }

        reusableMouseReport[0] = 0;
        reusableMouseReport[1] = (byte) boundedX;
        reusableMouseReport[2] = (byte) boundedY;
        reusableMouseReport[3] = 0;
        long started = nanoClock.nowNanos();
        // A session-state query can block too. Check again at the actual API
        // boundary, while still proving that no HID report has been submitted.
        if (reportNativeApiAcceptance
                && ControlMoveDeadline.isExpired(started, deadlineNanos)) {
            return MoveSubmission.DISCARDED_BEFORE_SEND;
        }
        if (!sessionPort.sendMouseReport(reusableMouseReport)) {
            recordDeliveryFailure(BluetoothHidOutputFailClosedPolicy.REASON_SEND_REPORT_FALSE);
            return MoveSubmission.REJECTED;
        }
        long completed = nanoClock.nowNanos();
        if (reportNativeApiAcceptance
                && ControlMoveDeadline.isExpired(completed, deadlineNanos)) {
            recordDeliveryFailure(
                    BluetoothHidOutputFailClosedPolicy
                            .REASON_MOVE_DEADLINE_EXPIRED_AFTER_SEND);
            return MoveSubmission.REJECTED;
        }
        long acceptanceMicros = Math.max(1L, (completed - started) / 1_000L);
        deliveryCircuit.recordUsbWriteCompletion(acceptanceMicros);
        if (reportNativeApiAcceptance
                && !moveCompletionPort.reportApiAccepted(ticket, acceptanceMicros)) {
            recordDeliveryFailure(
                    BluetoothHidOutputFailClosedPolicy.REASON_NATIVE_MOVE_COMPLETION_REJECTED);
            return MoveSubmission.REJECTED;
        }
        return MoveSubmission.API_ACCEPTED;
    }

    @Override
    public synchronized boolean suspendMoveDeliveryForNativeRecovery(long generation) {
        if (generation <= 0L) return false;
        // A newer recovery supersedes the old resume token without losing
        // the delivery eligibility captured before the first hold.
        if (!nativeRecoverySuspended) {
            deliveryAllowedBeforeRecovery = outputDeliveryAllowed;
            recoveryCancelledDeliveryGeneration = outputDeliveryAllowed ? deliveryGeneration : 0L;
            if (recoveryCancelledDeliveryGeneration != 0L) {
                recoveryRetiredDeliveryGeneration = Math.max(
                        recoveryRetiredDeliveryGeneration, recoveryCancelledDeliveryGeneration);
            }
        }
        ++deliveryGeneration;
        outputDeliveryAllowed = false;
        nativeRecoverySuspended = true;
        recoveryGeneration = generation;
        return true;
    }

    @Override
    public synchronized boolean resumeMoveDeliveryAfterNativeRecovery(long generation) {
        if (!nativeRecoverySuspended || recoveryGeneration != generation) return false;
        nativeRecoverySuspended = false;
        recoveryGeneration = 0L;
        if (recoveryCancelledDeliveryGeneration != 0L) {
            recoveryRetiredDeliveryGeneration = Math.max(
                    recoveryRetiredDeliveryGeneration, recoveryCancelledDeliveryGeneration);
        }
        recoveryCancelledDeliveryGeneration = 0L;
        if (!isReady()) {
            outputDeliveryAllowed = false;
            deliveryAllowedBeforeRecovery = false;
            return false;
        }
        outputDeliveryAllowed = deliveryAllowedBeforeRecovery;
        deliveryAllowedBeforeRecovery = false;
        return outputDeliveryAllowed;
    }

    @Override
    public synchronized boolean failClosedNativeMoveDelivery() {
        ++deliveryGeneration;
        outputDeliveryAllowed = false;
        deliveryAllowedBeforeRecovery = false;
        nativeRecoverySuspended = false;
        recoveryGeneration = 0L;
        recoveryCancelledDeliveryGeneration = 0L;
        recoveryRetiredDeliveryGeneration = 0L;
        return true;
    }

    private void recordDeliveryFailure(String reason) {
        ++deliveryGeneration;
        deliveryCircuit.recordFailure(reason);
        lastSessionDecisionReason = reason == null ? "delivery_failure" : reason;
        outputDeliveryAllowed = false;
        deliveryAllowedBeforeRecovery = false;
        nativeRecoverySuspended = false;
        recoveryGeneration = 0L;
        recoveryCancelledDeliveryGeneration = 0L;
        recoveryRetiredDeliveryGeneration = 0L;
    }

    /**
     * Android delivers HID profile and host changes asynchronously. Translate
     * each effective state transition into the existing reconnect callback so
     * the coordinator can reconcile immediately instead of waiting for its
     * periodic health poll.
     */
    private void onSessionStateChanged() {
        ReconnectListener listener;
        boolean ready;
        synchronized (this) {
            if (disposed) return;
            BluetoothHidOutputFailClosedPolicy.Decision decision =
                    BluetoothHidOutputFailClosedPolicy.evaluate(sessionPort.sessionState());
            if (decision.reason.equals(lastSessionDecisionReason)
                    && !(decision.outputAllowed
                    && deliveryCircuit.snapshot().circuitOpen)) {
                return;
            }
            lastSessionDecisionReason = decision.reason;
            if (decision.outputAllowed) {
                deliveryCircuit.resetAfterSuccessfulReconnect();
            } else if (outputDeliveryAllowed || nativeRecoverySuspended) {
                recordDeliveryFailure(decision.reason);
            }
            listener = reconnectListener;
            ready = isReadyLocked();
        }
        if (listener != null) listener.onReconnectResult(ready);
    }

    void dispose() {
        synchronized (this) {
            if (disposed) return;
            disposed = true;
            ++deliveryGeneration;
            reconnectListener = null;
            outputDeliveryAllowed = false;
            deliveryAllowedBeforeRecovery = false;
            nativeRecoverySuspended = false;
            recoveryGeneration = 0L;
            recoveryCancelledDeliveryGeneration = 0L;
            recoveryRetiredDeliveryGeneration = 0L;
        }
        sessionPort.setSessionStateListener(null);
    }

    private static int clampMouseDelta(int value) {
        return Math.max(-127, Math.min(127, value));
    }
}
