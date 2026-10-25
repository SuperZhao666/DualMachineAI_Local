package com.visionforge.inferencebenchmark;

import java.util.function.LongConsumer;

/** Bounded Bluetooth dispatch: a slow Binder call never blocks the inference worker.
 * The native ticket gate permits one outstanding move; this mailbox adds no backlog. */
final class AsyncBluetoothMoveSink implements ControlOutputMoveSink, AutoCloseable {
    private final BluetoothHidMouseTransportCore transport;
    private final LongConsumer failedTicket;
    private final LongConsumer discardedBeforeSend;
    private final BluetoothHidMouseTransportCore.NanoClock clock;
    private final Object lock = new Object();
    private final ControlTransportTiming timing = new ControlTransportTiming();
    private final MoveDeliveryTrace deliveryTrace = new MoveDeliveryTrace();
    private Move pending;
    private boolean closed;
    private long accepted, rejected, cancelled, expired, recoveryCancelled, generationRetired, maximumQueueUs;
    private final Thread worker;

    private static final class Move {
        final int x, y;
        final long ticket, generation, offeredNanos, deadlineNanos;
        Move(int x, int y, long ticket, long generation, long offered, long deadline) {
            this.x=x; this.y=y; this.ticket=ticket; this.generation=generation;
            offeredNanos=offered; deadlineNanos=deadline;
        }
    }

    AsyncBluetoothMoveSink(BluetoothHidMouseTransportCore transport, LongConsumer failedTicket) {
        this(transport, failedTicket, failedTicket, System::nanoTime);
    }
    AsyncBluetoothMoveSink(BluetoothHidMouseTransportCore transport, LongConsumer failedTicket,
            BluetoothHidMouseTransportCore.NanoClock clock) {
        this(transport,failedTicket,failedTicket,clock);
    }
    AsyncBluetoothMoveSink(BluetoothHidMouseTransportCore transport,LongConsumer failedTicket,
            LongConsumer discardedBeforeSend,BluetoothHidMouseTransportCore.NanoClock clock) {
        this.transport=transport; this.failedTicket=failedTicket; this.discardedBeforeSend=discardedBeforeSend;this.clock=clock;
        worker=new Thread(this::run, "vf-bluetooth-move"); worker.setDaemon(true); worker.start();
    }

    @Override public boolean offerMoveFromNative(int x, int y, long ticket, long budgetUs) {
        if (ticket <= 0 || budgetUs <= 0 || (x == 0 && y == 0)) return false;
        long now=clock.nowNanos();
        return offerMoveBeforeDeadline(x,y,ticket,ControlMoveDeadline.deadlineNanos(now,budgetUs));
    }

    @Override public boolean offerMoveBeforeDeadline(int x,int y,long ticket,long deadlineNanos) {
        if(ticket<=0L || deadlineNanos<=0L || (x==0 && y==0))return false;
        long now=clock.nowNanos();
        Move retired = null;
        synchronized (lock) {
            if (closed) { ++rejected; return false; }
            // This volatile snapshot read is admission's linearization point.
            // A later close still invalidates the move at core's send boundary.
            BluetoothHidMouseTransportCore.DeliverySnapshot gate = transport.deliverySnapshot();
            if (gate.generation == 0) { ++rejected; return false; }
            if (pending != null && pending.generation != gate.generation) {
                retired = pending; pending = null;
                ++cancelled; ++generationRetired;
                if (retired.generation <= gate.recoveryRetiredThrough) ++recoveryCancelled;
            }
            if (pending != null) { ++rejected; return false; }
            pending=new Move(x,y,ticket,gate.generation,now,deadlineNanos);
            ++accepted; lock.notifyAll();
        }
        // Native has retired the prior generation before publishing a newer
        // outstanding ticket. Do not synchronously reenter JNI from offer:
        // native may still own its publish mutex. Preserve the old ticket in
        // the trace, without completing or failing the newly admitted move.
        if (retired != null) deliveryTrace.record(retired.ticket, retired.x, retired.y,
                retired.offeredNanos, now, now, now, 0, retired.deadlineNanos,
                retired.generation, 3, 0);
        return true;
    }

    private void run() {
        while (true) {
            Move move;
            synchronized (lock) {
                while (!closed && pending == null) {
                    try { lock.wait(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return; }
                }
                if (closed) return;
                move=pending; pending=null;
            }
            long now=clock.nowNanos();
            long remainingUs=ControlMoveDeadline.remainingNanos(now,move.deadlineNanos)/1000;
            synchronized (lock) {
                maximumQueueUs=Math.max(maximumQueueUs,Math.max(0,(now-move.offeredNanos)/1000));
            }
            BluetoothHidMouseTransportCore.MoveSubmission submission =
                    BluetoothHidMouseTransportCore.MoveSubmission.DISCARDED_BEFORE_SEND;
            try {
                if (remainingUs > 0) submission=transport.offerMoveBeforeDeadlineAtGeneration(
                        move.x,move.y,move.ticket,move.deadlineNanos,move.generation);
            } catch (RuntimeException | LinkageError failure) {
                transport.failClosedNativeMoveDelivery();
                submission=BluetoothHidMouseTransportCore.MoveSubmission.REJECTED;
            }
            boolean sent=submission==BluetoothHidMouseTransportCore.MoveSubmission.API_ACCEPTED;
            boolean unsentExpiry=submission==BluetoothHidMouseTransportCore.MoveSubmission.DISCARDED_BEFORE_SEND;
            boolean recoveryCancellation=submission==BluetoothHidMouseTransportCore.MoveSubmission.CANCELLED_BEFORE_SEND_FOR_RECOVERY;
            if (unsentExpiry) synchronized (lock) { ++expired; }
            if (recoveryCancellation) synchronized (lock) { ++recoveryCancelled; }
            long returned = clock.nowNanos();
            timing.record(move.offeredNanos, now, returned, returned, sent);
            deliveryTrace.record(move.ticket, move.x, move.y, move.offeredNanos, now,
                    returned, returned, 0, move.deadlineNanos, move.generation,
                    sent ? 1 : unsentExpiry ? 2 : recoveryCancellation ? 3 : 6, -1);
            if (!sent) {
                synchronized (lock) { ++cancelled; }
                if(recoveryCancellation)continue; // Native retires this ticket after suspension returns.
                if(unsentExpiry)discardedBeforeSend.accept(move.ticket);
                else failedTicket.accept(move.ticket);
            }
        }
    }

    String report() {
        synchronized (lock) {
            return "bluetooth_dispatch=bounded_worker queued="+(pending == null ? 0 : 1)
                    +" accepted="+accepted+" rejected="+rejected+" cancelled="+cancelled
                    +" expired="+expired+" recovery_cancelled="+recoveryCancelled+" max_queue_us="+maximumQueueUs
                    +" generation_retired="+generationRetired
                    +" timing_completion=android_api_return timing={"+timing.report()+"}";
        }
    }

    String drainTrace() { return deliveryTrace.drain("bluetooth_hid", "monotonic_us"); }

    @Override public boolean suspendMoveDeliveryForNativeRecovery(long generation) {
        return transport.suspendMoveDeliveryForNativeRecovery(generation);
    }
    @Override public boolean resumeMoveDeliveryAfterNativeRecovery(long generation) {
        return transport.resumeMoveDeliveryAfterNativeRecovery(generation);
    }
    @Override public boolean failClosedNativeMoveDelivery() {
        return transport.failClosedNativeMoveDelivery();
    }

    @Override public void close() {
        transport.failClosedNativeMoveDelivery();
        Move abandoned;
        synchronized (lock) {
            closed=true; abandoned=pending; pending=null; lock.notifyAll();
        }
        if (abandoned != null) failedTicket.accept(abandoned.ticket);
    }
}
