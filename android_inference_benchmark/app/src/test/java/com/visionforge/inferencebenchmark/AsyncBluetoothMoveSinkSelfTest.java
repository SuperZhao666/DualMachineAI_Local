package com.visionforge.inferencebenchmark;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicBoolean;

final class AsyncBluetoothMoveSinkSelfTest {
    static void run() throws Exception {
        slowBinderDoesNotBlockOffer();
        cancelledEpochCannotReviveAfterReenable();
        recoveryCancellationBeforeSendPreservesTheNativeRecovery();
        recoveryResumeBeforeWorkerStillDropsTheQueuedMove();
        explicitDisableDuringRecoveryStillRejectsTheQueuedMove();
        sessionLossDuringRecoveryStillFailsTheQueuedMove();
        inFlightApiCompletionIsNotReclassifiedAsRecoveryCancellation();
        queuedDeadlineExpiresWithoutSending();
        absoluteExpiredOfferIsHandledWithoutRefreshingItsBudget();
        absoluteDeadlineSurvivesCoreMonitorWait();
        absoluteDeadlineIncludesGenerationLookupWait();
        delayedApiCompletionStillFailsClosed();
        unexpectedApiFailureStillFailsClosed();
        mailboxIsBoundedAndCloseCancelsAllWaitingMoves();
        rejectedApiCompletesFailure();
    }
    private static void require(boolean value) { if (!value) throw new AssertionError(); }
    private static void await(CountDownLatch latch) {
        try { require(latch.await(3, TimeUnit.SECONDS)); }
        catch (InterruptedException e) { throw new AssertionError(e); }
    }
    private static final class Session implements BluetoothHidMouseTransportCore.SessionPort {
        final AtomicInteger sends = new AtomicInteger();
        CountDownLatch entered, release;
        boolean accepted = true;
        Runnable onSend;
        BluetoothHidOutputFailClosedPolicy.SessionState state =
                BluetoothHidOutputFailClosedPolicy.SessionState.ready();
        Runnable stateListener;
        public BluetoothHidOutputFailClosedPolicy.SessionState sessionState() {
            return state;
        }
        public boolean sendMouseReport(byte[] report) {
            sends.incrementAndGet();
            if (entered != null) entered.countDown();
            if (release != null) await(release);
            if (onSend != null) onSend.run();
            return accepted;
        }
        public void connectFirstSupportedHost() {}
        public void setSessionStateListener(Runnable listener) { stateListener = listener; }
    }
    private static final class PausedClock implements BluetoothHidMouseTransportCore.NanoClock {
        final CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        final AtomicLong time = new AtomicLong(1000000000L);
        public long nowNanos() {
            if (Thread.currentThread().getName().equals("vf-bluetooth-move")) {
                entered.countDown(); await(release);
            }
            return time.get();
        }
    }
    private static void slowBinderDoesNotBlockOffer() {
        Session session = new Session(); session.entered = new CountDownLatch(1); session.release = new CountDownLatch(1);
        CountDownLatch completed = new CountDownLatch(1);
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session,
                (ticket, us) -> { require(ticket == 1); completed.countDown(); return true; });
        core.setOutputDeliveryAllowed(true);
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core, ticket -> { throw new AssertionError(); });
        try {
            long started = System.nanoTime();
            require(sink.offerMoveFromNative(8, -3, 1, 2000000));
            require(System.nanoTime() - started < 100000000L);
            await(session.entered);
            require(completed.getCount() == 1);
            session.release.countDown(); await(completed);
        } finally { session.release.countDown(); sink.close(); }
    }
    private static void cancelledEpochCannotReviveAfterReenable() {
        Session session = new Session(); PausedClock clock = new PausedClock();
        CountDownLatch failed = new CountDownLatch(1);
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session, (ticket, us) -> true, clock);
        core.setOutputDeliveryAllowed(true);
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core, ticket -> { require(ticket == 4); failed.countDown(); }, clock);
        try {
            require(sink.offerMoveFromNative(5, 0, 4, 250000)); await(clock.entered);
            core.setOutputDeliveryAllowed(false); core.setOutputDeliveryAllowed(true);
            clock.release.countDown(); await(failed); require(session.sends.get() == 0);
        } finally { clock.release.countDown(); sink.close(); }
    }
    private static void recoveryCancellationBeforeSendPreservesTheNativeRecovery() {
        Session session = new Session(); PausedClock clock = new PausedClock();
        AtomicInteger failed = new AtomicInteger(), discarded = new AtomicInteger();
        CountDownLatch completed = new CountDownLatch(1);
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session,
                (ticket, us) -> { require(ticket == 19L); completed.countDown(); return true; }, clock);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core,
                ticket -> { failed.incrementAndGet(); core.failClosedNativeMoveDelivery(); },
                ticket -> discarded.incrementAndGet(), clock);
        try {
            require(sink.offerMoveFromNative(5, 0, 18L, 250000)); await(clock.entered);
            require(core.suspendMoveDeliveryForNativeRecovery(41L));
            clock.release.countDown(); awaitCancelledWorkerIdle(sink);
            boolean resumed = core.resumeMoveDeliveryAfterNativeRecovery(41L);
            System.out.println("recovery_before_send failed_callbacks=" + failed.get()
                    + " unsent_expiry_callbacks=" + discarded.get() + " api_reports=" + session.sends.get()
                    + " recovery_resumed=" + resumed);
            require(failed.get() == 0 && discarded.get() == 0 && session.sends.get() == 0);
            require(core.deliveryState().failureCount == 0 && !core.deliveryState().circuitOpen);
            require(resumed);
            require(sink.offerMoveFromNative(5, 0, 19L, 250000)); await(completed);
            require(session.sends.get() == 1);
        } finally { clock.release.countDown(); sink.close(); }
    }

    private static void recoveryResumeBeforeWorkerStillDropsTheQueuedMove() {
        final int trials = 200;
        int failed = 0;
        int sent = 0;
        int resumed = 0;
        int followed = 0;
        for (int trial = 0; trial < trials; trial++) {
            Session session = new Session();
            PausedClock clock = new PausedClock();
            AtomicInteger failures = new AtomicInteger();
            CountDownLatch completed = new CountDownLatch(1);
            BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session,
                    (ticket, us) -> { completed.countDown(); return true; }, clock);
            require(core.setOutputDeliveryAllowed(true));
            AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core,
                    ticket -> { failures.incrementAndGet(); core.failClosedNativeMoveDelivery(); },
                    ticket -> failures.incrementAndGet(), clock);
            try {
                require(sink.offerMoveFromNative(5, 0, 30L + trial, 250000));
                await(clock.entered);
                require(core.suspendMoveDeliveryForNativeRecovery(90L));
                if (core.resumeMoveDeliveryAfterNativeRecovery(90L)) resumed++;
                clock.release.countDown();
                awaitCancelledWorkerIdle(sink);
                if (failures.get() != 0) failed++;
                if (session.sends.get() != 0) sent++;
                require(core.deliveryState().failureCount == 0 && !core.deliveryState().circuitOpen);
                require(sink.offerMoveFromNative(6, 0, 10000L + trial, 250000));
                await(completed);
                if (session.sends.get() == 1) followed++;
            } finally {
                clock.release.countDown();
                sink.close();
            }
        }
        System.out.println("bluetooth_resume_before_worker trials=" + trials
                + " recovery_resumed=" + resumed
                + " failure_callbacks=" + failed
                + " stale_sends=" + sent
                + " post_recovery_sends=" + followed);
        require(resumed == trials && failed == 0 && sent == 0 && followed == trials);
    }

    private static void explicitDisableDuringRecoveryStillRejectsTheQueuedMove() {
        Session session = new Session(); PausedClock clock = new PausedClock();
        AtomicInteger failed = new AtomicInteger(), discarded = new AtomicInteger();
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session, (ticket, us) -> true, clock);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core, ticket -> failed.incrementAndGet(),
                ticket -> discarded.incrementAndGet(), clock);
        try {
            require(sink.offerMoveFromNative(5, 0, 20L, 250000)); await(clock.entered);
            require(core.suspendMoveDeliveryForNativeRecovery(42L));
            require(core.setOutputDeliveryAllowed(false));
            clock.release.countDown(); awaitCancelledWorkerIdle(sink);
            require(failed.get() == 1 && discarded.get() == 0 && session.sends.get() == 0);
            require(!core.resumeMoveDeliveryAfterNativeRecovery(42L));
            require(core.nativeDeliveryGeneration() == 0);
        } finally { clock.release.countDown(); sink.close(); }
    }

    private static void sessionLossDuringRecoveryStillFailsTheQueuedMove() {
        Session session = new Session(); PausedClock clock = new PausedClock();
        AtomicInteger failed = new AtomicInteger(), discarded = new AtomicInteger();
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session, (ticket, us) -> true, clock);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core, ticket -> failed.incrementAndGet(),
                ticket -> discarded.incrementAndGet(), clock);
        try {
            require(sink.offerMoveFromNative(5, 0, 21L, 250000)); await(clock.entered);
            require(core.suspendMoveDeliveryForNativeRecovery(43L));
            // Even before the asynchronous state listener runs, final session
            // validation must not misclassify a real disconnect as recovery.
            session.state = session.state.withHostConnected(false);
            clock.release.countDown(); awaitCancelledWorkerIdle(sink);
            require(failed.get() == 1 && discarded.get() == 0 && session.sends.get() == 0);
            require(core.deliveryState().circuitOpen && !core.resumeMoveDeliveryAfterNativeRecovery(43L));
        } finally { clock.release.countDown(); sink.close(); }
    }

    private static void inFlightApiCompletionIsNotReclassifiedAsRecoveryCancellation() {
        Session session = new Session(); session.entered = new CountDownLatch(1); session.release = new CountDownLatch(1);
        AtomicInteger failed = new AtomicInteger(), discarded = new AtomicInteger();
        CountDownLatch completed = new CountDownLatch(1), suspendStarted = new CountDownLatch(1);
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session,
                (ticket, us) -> { require(ticket == 22L); completed.countDown(); return true; });
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core, ticket -> failed.incrementAndGet(),
                ticket -> discarded.incrementAndGet(), System::nanoTime);
        AtomicBoolean suspended = new AtomicBoolean();
        Thread suspender = new Thread(() -> {
            suspendStarted.countDown(); suspended.set(core.suspendMoveDeliveryForNativeRecovery(44L));
        });
        try {
            require(sink.offerMoveFromNative(5, 0, 22L, 2000000)); await(session.entered);
            suspender.start(); await(suspendStarted);
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while (suspender.getState() != Thread.State.BLOCKED && System.nanoTime() < deadline) Thread.yield();
            require(suspender.getState() == Thread.State.BLOCKED && !suspended.get());
            session.release.countDown(); await(completed);
            try { suspender.join(3000); } catch (InterruptedException failure) { throw new AssertionError(failure); }
            require(!suspender.isAlive() && suspended.get());
            require(session.sends.get() == 1 && failed.get() == 0 && discarded.get() == 0);
            require(core.resumeMoveDeliveryAfterNativeRecovery(44L));
        } finally { session.release.countDown(); sink.close(); }
    }

    private static void awaitCancelledWorkerIdle(AsyncBluetoothMoveSink sink) {
        // Observe the worker after its terminal callback, rather than relying
        // on a sleep to prove that a failure callback did not happen.
        try {
            java.lang.reflect.Field field = AsyncBluetoothMoveSink.class.getDeclaredField("worker");
            field.setAccessible(true); Thread worker = (Thread) field.get(sink);
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while (System.nanoTime() < deadline) {
                if (sink.report().contains(" cancelled=1 ") && worker.getState() == Thread.State.WAITING) return;
                Thread.yield();
            }
            throw new AssertionError("Bluetooth worker did not finish cancellation");
        } catch (ReflectiveOperationException failure) { throw new AssertionError(failure); }
    }

    private static void queuedDeadlineExpiresWithoutSending() {
        Session session = new Session(); PausedClock clock = new PausedClock();
        CountDownLatch discarded = new CountDownLatch(1),completed = new CountDownLatch(1);
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session, (ticket, us) -> {completed.countDown();return true;}, clock);
        core.setOutputDeliveryAllowed(true);
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core, ticket -> {throw new AssertionError("An unsent expiry revoked the live session");},
                ticket -> {require(ticket==5);discarded.countDown();},clock);
        try {
            require(sink.offerMoveFromNative(5, 0, 5, 1000)); await(clock.entered);
            clock.time.addAndGet(2000000L); clock.release.countDown(); await(discarded);
            require(session.sends.get() == 0); require(sink.report().contains("expired=1"));
            require(!core.deliveryState().circuitOpen);
            require(sink.offerMoveFromNative(5,0,6,250000));await(completed);
            require(session.sends.get()==1);
        } finally { clock.release.countDown(); sink.close(); }
    }

    private static final class SnapshotClock implements BluetoothHidMouseTransportCore.NanoClock {
        final AtomicLong time=new AtomicLong(1000000000L);
        final AtomicInteger workerCalls=new AtomicInteger();
        final CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);
        public long nowNanos() {
            long snapshot=time.get();
            if(Thread.currentThread().getName().equals("vf-bluetooth-move") && workerCalls.incrementAndGet()==1){
                entered.countDown();await(release);
            }
            return snapshot;
        }
    }

    private static void absoluteExpiredOfferIsHandledWithoutRefreshingItsBudget() {
        Session session=new Session();AtomicLong time=new AtomicLong(1000000000L);
        CountDownLatch discarded=new CountDownLatch(1),completed=new CountDownLatch(1);
        BluetoothHidMouseTransportCore core=new BluetoothHidMouseTransportCore(session,
                (ticket,us)->{require(ticket==17);completed.countDown();return true;},time::get);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink=new AsyncBluetoothMoveSink(core,
                ticket->{throw new AssertionError("Already-expired absolute offer became a transport failure");},
                ticket->{require(ticket==16);discarded.countDown();},time::get);
        try {
            require(sink.offerMoveBeforeDeadline(5,0,16,time.get()-1));await(discarded);
            require(session.sends.get()==0 && !core.deliveryState().circuitOpen);
            require(sink.offerMoveBeforeDeadline(5,0,17,time.get()+250000000L));await(completed);
            require(session.sends.get()==1);
        } finally {sink.close();}
    }

    private static void absoluteDeadlineSurvivesCoreMonitorWait() {
        Session session=new Session();SnapshotClock clock=new SnapshotClock();
        CountDownLatch discarded=new CountDownLatch(1),completed=new CountDownLatch(1);
        BluetoothHidMouseTransportCore core=new BluetoothHidMouseTransportCore(session,
                (ticket,us)->{require(ticket==12);completed.countDown();return true;},clock);
        require(core.setOutputDeliveryAllowed(true));
        long generation=core.nativeDeliveryGeneration();
        AsyncBluetoothMoveSink sink=new AsyncBluetoothMoveSink(core,
                ticket->{throw new AssertionError("Unsent monitor expiry was classified as failure");},
                ticket->{require(ticket==11);discarded.countDown();},clock);
        try {
            require(sink.offerMoveBeforeDeadline(5,0,11,clock.time.get()+1000000L));await(clock.entered);
            synchronized(core){
                clock.release.countDown();clock.time.addAndGet(2000000L);
            }
            await(discarded);
            require(session.sends.get()==0 && !core.deliveryState().circuitOpen);
            require(core.nativeDeliveryGeneration()==generation);
            require(sink.offerMoveFromNative(5,0,12,250000));await(completed);
            require(session.sends.get()==1);
        } finally {clock.release.countDown();sink.close();}
    }

    private static void absoluteDeadlineIncludesGenerationLookupWait() {
        Session session=new Session();AtomicLong time=new AtomicLong(1000000000L);
        CountDownLatch offerClockRead=new CountDownLatch(1),discarded=new CountDownLatch(1);
        BluetoothHidMouseTransportCore.NanoClock clock=()->{
            long snapshot=time.get();
            if(Thread.currentThread().getName().equals("offer-before-monitor"))offerClockRead.countDown();
            return snapshot;
        };
        BluetoothHidMouseTransportCore core=new BluetoothHidMouseTransportCore(session,(ticket,us)->true,clock);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink=new AsyncBluetoothMoveSink(core,
                ticket->{throw new AssertionError("Unsent offer expiry was classified as failure");},
                ticket->{require(ticket==13);discarded.countDown();},clock);
        AtomicBoolean offered=new AtomicBoolean();
        Thread producer=new Thread(()->offered.set(sink.offerMoveFromNative(5,0,13,1000)),"offer-before-monitor");
        try {
            synchronized(core){producer.start();await(offerClockRead);time.addAndGet(2000000L);}
            try {producer.join(3000);}catch(InterruptedException e){throw new AssertionError(e);}
            require(!producer.isAlive() && offered.get());await(discarded);
            require(session.sends.get()==0 && !core.deliveryState().circuitOpen);
            require(core.nativeDeliveryGeneration()!=0);
        } finally {sink.close();}
    }

    private static void delayedApiCompletionStillFailsClosed() {
        Session session=new Session();AtomicLong time=new AtomicLong(1000000000L);
        CountDownLatch failed=new CountDownLatch(1);AtomicInteger discarded=new AtomicInteger(),completions=new AtomicInteger();
        session.onSend=()->time.addAndGet(2000000L);
        BluetoothHidMouseTransportCore core=new BluetoothHidMouseTransportCore(session,
                (ticket,us)->{completions.incrementAndGet();return true;},time::get);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink=new AsyncBluetoothMoveSink(core,ticket->{require(ticket==14);failed.countDown();},
                ticket->discarded.incrementAndGet(),time::get);
        try {
            require(sink.offerMoveFromNative(5,0,14,1000));await(failed);
            require(session.sends.get()==1 && completions.get()==0 && discarded.get()==0);
            require(core.deliveryState().circuitOpen && core.nativeDeliveryGeneration()==0);
        } finally {sink.close();}
    }

    private static void unexpectedApiFailureStillFailsClosed() {
        Session session=new Session();CountDownLatch failed=new CountDownLatch(1);AtomicInteger discarded=new AtomicInteger();
        session.onSend=()->{throw new LinkageError("test-only unavailable HID API");};
        BluetoothHidMouseTransportCore core=new BluetoothHidMouseTransportCore(session,(ticket,us)->true);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink=new AsyncBluetoothMoveSink(core,ticket->failed.countDown(),ticket->discarded.incrementAndGet(),System::nanoTime);
        try {
            require(sink.offerMoveFromNative(5,0,15,250000));await(failed);
            require(session.sends.get()==1 && discarded.get()==0 && core.nativeDeliveryGeneration()==0);
        } finally {sink.close();}
    }
    private static void mailboxIsBoundedAndCloseCancelsAllWaitingMoves() {
        Session session = new Session(); PausedClock clock = new PausedClock();
        CountDownLatch failed = new CountDownLatch(2);
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session, (ticket, us) -> true, clock);
        core.setOutputDeliveryAllowed(true);
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core, ticket -> failed.countDown(), clock);
        try {
            require(sink.offerMoveFromNative(5, 0, 6, 250000)); await(clock.entered);
            require(sink.offerMoveFromNative(5, 0, 7, 250000));
            require(!sink.offerMoveFromNative(5, 0, 8, 250000));
            sink.close(); clock.release.countDown(); await(failed); require(session.sends.get() == 0);
            require(!sink.offerMoveFromNative(5, 0, 9, 250000));
        } finally { clock.release.countDown(); sink.close(); }
    }
    private static void rejectedApiCompletesFailure() {
        Session session = new Session(); session.accepted = false;
        CountDownLatch failed = new CountDownLatch(1);
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session, (ticket, us) -> true);
        core.setOutputDeliveryAllowed(true);
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core, ticket -> failed.countDown());
        try {
            require(sink.offerMoveFromNative(5, 0, 10, 250000)); await(failed);
            require(core.deliveryState().circuitOpen); require(session.sends.get() == 1);
        } finally { sink.close(); }
    }
}
