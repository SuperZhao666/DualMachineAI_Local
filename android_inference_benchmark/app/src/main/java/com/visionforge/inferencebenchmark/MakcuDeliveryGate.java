package com.visionforge.inferencebenchmark;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * Pure-Java authorization state for physical MAKCU delivery.
 *
 * <p>User authorization and the temporary decoder-recovery hold are separate.
 * A matching recovery may restore delivery only when delivery was effective
 * before the hold; explicit disable, circuit-open and close all cancel that
 * restoration eligibility.</p>
 */
final class MakcuDeliveryGate {
    private final MakcuPendingMoveSlot pendingMoveSlot;
    private final AtomicInteger requiredPhysicalButtonMask = new AtomicInteger();
    private final AtomicInteger lastPhysicalButtonMask = new AtomicInteger();
    private final Object serialIoLock;
    private volatile boolean userAllowed;
    private boolean recoverySuspended;
    private boolean recoveryResumeEligible;
    private long recoveryGeneration;
    private long authorizationEpoch = 1L;
    /** Moves captured before this epoch were revoked by an eligible recovery. */
    private long recoverySilentBeforeEpoch;

    MakcuDeliveryGate() {
        this(new MakcuPendingMoveSlot(), new Object());
    }

    MakcuDeliveryGate(MakcuPendingMoveSlot pendingMoveSlot, Object serialIoLock) {
        this.pendingMoveSlot = pendingMoveSlot;
        this.serialIoLock = serialIoLock;
    }

    boolean setUserAllowed(boolean allowed) {
        if (allowed && !isPhysicalTriggerSatisfied()) return false;
        synchronized (this) {
            if (allowed && (recoverySuspended || !isPhysicalTriggerSatisfied())) return false;
            userAllowed = allowed;
            if (!allowed) revokeAuthorizationLocked();
        }
        if (!allowed) drainPendingAndAwaitSerialIdle();
        return true;
    }

    void suspendForRecovery(long generation) {
        synchronized (this) {
            if (!recoverySuspended) recoveryResumeEligible = userAllowed;
            authorizationEpoch = nextEpoch(authorizationEpoch);
            if (recoveryResumeEligible) recoverySilentBeforeEpoch = authorizationEpoch;
            recoveryGeneration = generation;
            recoverySuspended = true;
        }
        drainPendingAndAwaitSerialIdle();
    }

    synchronized boolean resumeAfterRecovery(long generation) {
        if (recoveryGeneration != generation || !recoverySuspended) return false;
        boolean outputEnabled = recoveryResumeEligible && userAllowed;
        recoverySuspended = false;
        recoveryResumeEligible = false;
        return outputEnabled;
    }

    void failClosed() {
        synchronized (this) {
            userAllowed = false;
            cancelRecoveryLocked();
            revokeAuthorizationLocked();
        }
        drainPendingAndAwaitSerialIdle();
    }

    /**
     * Removes one queued move and the epoch that authorized it as one snapshot.
     * A recovery that begins after this returns cannot be mistaken for the
     * epoch of a move already in the caller's hands.
     */
    boolean takePendingForCommit(MakcuPendingMoveSlot.PendingMove destination) {
        synchronized (this) {
            if (!pendingMoveSlot.takeInto(destination)) return false;
            destination.authorizationEpoch = authorizationEpoch;
            return true;
        }
    }

    /** Whether a move already removed from the mailbox may still enter device I/O. */
    enum TakenMoveCommit {
        WRITE,
        DROP_SILENT,
        DROP_AND_REPORT_FAILURE
    }

    synchronized TakenMoveCommit evaluateTakenMove(long capturedEpoch) {
        if (capturedEpoch == authorizationEpoch && userAllowed && !recoverySuspended
                && isPhysicalTriggerSatisfied()) {
            return TakenMoveCommit.WRITE;
        }
        boolean recoveryOwnsMove = recoverySilentBeforeEpoch != 0L
                && capturedEpoch < recoverySilentBeforeEpoch
                && (recoveryResumeEligible || !recoverySuspended);
        if (recoveryOwnsMove || (recoverySuspended && recoveryResumeEligible)) {
            return TakenMoveCommit.DROP_SILENT;
        }
        return TakenMoveCommit.DROP_AND_REPORT_FAILURE;
    }

    boolean isDeliveryAllowed() {
        if (!userAllowed || !isPhysicalTriggerSatisfied()) return false;
        synchronized (this) {
            return userAllowed && !recoverySuspended && isPhysicalTriggerSatisfied();
        }
    }

    /**
     * Changes the exact physical trigger enforced by both offer and final
     * serial-commit checks. Returns a queued ticket rejected by the change.
     */
    long setRequiredPhysicalButtonMask(int requiredMask) {
        requiredPhysicalButtonMask.set(Math.max(0, requiredMask));
        return revokeImmediatelyIfPhysicalTriggerReleased();
    }

    /**
     * Serial-reader fast path. A release revokes delivery and clears the
     * pending move without waiting on serial I/O, because the writer may be
     * waiting for the acknowledgement that this same reader must consume.
     */
    long updatePhysicalButtonMask(int completeMask) {
        lastPhysicalButtonMask.set(Math.max(0, completeMask));
        return revokeImmediatelyIfPhysicalTriggerReleased();
    }

    int requiredPhysicalButtonMask() {
        return requiredPhysicalButtonMask.get();
    }

    int currentPhysicalButtonMask() {
        return lastPhysicalButtonMask.get();
    }

    boolean isPhysicalTriggerSatisfied() {
        int requiredMask = requiredPhysicalButtonMask.get();
        return requiredMask == 0
                || (lastPhysicalButtonMask.get() & requiredMask) != 0;
    }

    synchronized boolean isRecoverySuspended() {
        return recoverySuspended;
    }

    private void cancelRecoveryLocked() {
        recoveryGeneration++;
        recoverySuspended = false;
        recoveryResumeEligible = false;
    }

    /** Explicit close, disable or trigger release. Recovery must not hide it. */
    private void revokeAuthorizationLocked() {
        recoveryResumeEligible = false;
        authorizationEpoch = nextEpoch(authorizationEpoch);
        recoverySilentBeforeEpoch = 0L;
    }

    private static long nextEpoch(long epoch) {
        long next = epoch + 1L;
        return next == 0L ? 1L : next;
    }

    private long revokeImmediatelyIfPhysicalTriggerReleased() {
        if (isPhysicalTriggerSatisfied()) return 0L;
        synchronized (this) {
            if (isPhysicalTriggerSatisfied()) return 0L;
            userAllowed = false;
            revokeAuthorizationLocked();
        }
        // Intentionally do not acquire serialIoLock on the reader thread.
        return pendingMoveSlot.clear();
    }

    private void drainPendingAndAwaitSerialIdle() {
        pendingMoveSlot.clear();
        // Never hold this gate's monitor while waiting for serial I/O. The
        // writer checks this gate while holding serialIoLock.
        synchronized (serialIoLock) {
            pendingMoveSlot.clear();
        }
    }
}
