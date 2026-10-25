package com.visionforge.inferencebenchmark;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** Pure JVM boundary tests for the shared native-move deadline helpers. */
public final class ControlMoveDeadlineSelfTest {
    private ControlMoveDeadlineSelfTest() {
    }

    public static void main(String[] args) {
        run();
    }

    static void run() {
        require(ControlMoveDeadline.deadlineNanos(1_000L, 250L) == 251_000L);
        require(ControlMoveDeadline.deadlineNanos(1_000L, 0L) == 1_000L);
        require(ControlMoveDeadline.deadlineNanos(Long.MAX_VALUE - 10L, 1L)
                == Long.MAX_VALUE);
        require(ControlMoveDeadline.boundedDeadlineNanos(
                Long.MAX_VALUE - 10L, 20L) == Long.MAX_VALUE);
        require(ControlMoveDeadline.earlierDeadline(10L, 20L) == 10L);
        require(ControlMoveDeadline.isExpired(10L, 10L));
        require(!ControlMoveDeadline.isExpired(9L, 10L));
        require(ControlMoveDeadline.remainingNanos(9L, 10L) == 1L);
        require(ControlMoveDeadline.boundedTimeoutMillis(0L, 1L, 25) == 1);
        require(ControlMoveDeadline.boundedTimeoutMillis(
                0L, 24_000_001L, 25) == 25);
        require(ControlMoveDeadline.boundedTimeoutMillis(
                1L, Long.MAX_VALUE, 25) == 25);
        require(ControlMoveDeadline.boundedTimeoutMillis(10L, 10L, 25) == 0);
        absoluteNativeUnitsAreChecked();
        elapsedClockMappingNeverExtendsTheOriginalDeadline();
        dispatcherPreservesAnAgedAbsoluteDeadline();
        dispatcherForwardsAlreadyExpiredOffersForExplicitDiscard();
        staleSinkRemovalCannotEraseTheCurrentSink();
        legacySinkCannotSilentlyRebaseAnAbsoluteOffer();
    }

    private static void absoluteNativeUnitsAreChecked() {
        require(ControlMoveDeadline.monotonicMicrosToNanos(1L)==1000L);
        require(ControlMoveDeadline.monotonicMicrosToNanos(123456789L)==123456789000L);
        require(ControlMoveDeadline.monotonicMicrosToNanos(0L)==0L);
        require(ControlMoveDeadline.monotonicMicrosToNanos(-1L)==0L);
        long largest=Long.MAX_VALUE/1000L;
        require(ControlMoveDeadline.monotonicMicrosToNanos(largest)==largest*1000L);
        require(ControlMoveDeadline.monotonicMicrosToNanos(largest+1L)==0L);
    }

    private static void elapsedClockMappingNeverExtendsTheOriginalDeadline() {
        require(ControlMoveDeadline.elapsedDeadlineFromMonotonic(
                10_000_000L,3_000_000_000L,9_002_000L)==3_000_998_000L);
        require(ControlMoveDeadline.elapsedDeadlineFromMonotonic(
                9_000_000L,3_000_000_000L,9_002_000L)==3_000_000_000L);
        require(ControlMoveDeadline.elapsedDeadlineFromMonotonic(0,1000,1000)==0);
        require(ControlMoveDeadline.elapsedDeadlineFromMonotonic(2000,-1,1000)==0);
        require(ControlMoveDeadline.elapsedDeadlineFromMonotonic(2000,1000,-1)==0);
        require(ControlMoveDeadline.elapsedDeadlineFromMonotonic(
                Long.MAX_VALUE,Long.MAX_VALUE-10,1)==Long.MAX_VALUE);
        for(long offset:new long[]{0,10_000L,4_000_000_000L}){
            for(long samplingDelay:new long[]{0,1,1000,50000}){
                long monoBefore=1_000_000L,deadline=1_020_000L;
                long mapped=ControlMoveDeadline.elapsedDeadlineFromMonotonic(
                        deadline,monoBefore+offset,monoBefore+samplingDelay);
                require(mapped<=deadline+offset);
                require(ControlMoveDeadline.remainingNanos(monoBefore+samplingDelay+offset,mapped)
                        <=ControlMoveDeadline.remainingNanos(monoBefore+samplingDelay,deadline));
            }
        }
    }

    private static void dispatcherPreservesAnAgedAbsoluteDeadline() {
        AbsoluteSink target=new AbsoluteSink(true);
        AtomicBoolean accepted=new AtomicBoolean();
        ControlOutputMoveDispatcher.registerSink(target);
        Thread dispatch=new Thread(()->accepted.set(
                ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(7,-3,41L,1_000_000L)));
        try {
            dispatch.start();await(target.entered);
            // Simulate callback/transport waiting until the original visual
            // lifetime has run out. It must never become a fresh relative TTL.
            target.now=1_000_000_001L;target.release.countDown();join(dispatch);
            require(accepted.get() && target.discarded);
            require(target.absoluteDeadline==1_000_000_000L);
            require(target.lastTicket==41L && target.relativeCalls==0);
            require(ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(2,0,42L,2_000_000L));
            require(!target.discarded && target.absoluteDeadline==2_000_000_000L);
        } finally {target.release.countDown();ControlOutputMoveDispatcher.clearSink(target);}
    }

    private static void dispatcherForwardsAlreadyExpiredOffersForExplicitDiscard() {
        AbsoluteSink target=new AbsoluteSink(false);target.now=100_000L;
        ControlOutputMoveDispatcher.registerSink(target);
        try {
            require(ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(1,0,43L,1L));
            require(target.discarded && target.absoluteDeadline==1000L && target.calls==1);
            require(!ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(1,0,44L,0L));
            require(!ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(1,0,44L,Long.MAX_VALUE));
            require(!ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(1,0,0L,1000L));
            require(target.calls==1);
        } finally {ControlOutputMoveDispatcher.clearSink(target);}
    }

    private static void staleSinkRemovalCannotEraseTheCurrentSink() {
        AbsoluteSink previous=new AbsoluteSink(false),current=new AbsoluteSink(false);
        ControlOutputMoveDispatcher.registerSink(previous);ControlOutputMoveDispatcher.registerSink(current);
        try {
            ControlOutputMoveDispatcher.clearSink(previous);
            require(ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(1,0,45L,1_000_000L));
            require(current.calls==1 && previous.calls==0);
        } finally {ControlOutputMoveDispatcher.clearSink(current);}
        require(!ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(1,0,46L,1_000_000L));
    }

    private static void legacySinkCannotSilentlyRebaseAnAbsoluteOffer() {
        ControlOutputMoveSink legacy=new ControlOutputMoveSink(){
            public boolean offerMoveFromNative(int x,int y,long ticket,long budget){return true;}
            public boolean suspendMoveDeliveryForNativeRecovery(long generation){return true;}
            public boolean resumeMoveDeliveryAfterNativeRecovery(long generation){return true;}
            public boolean failClosedNativeMoveDelivery(){return true;}
        };
        ControlOutputMoveDispatcher.registerSink(legacy);
        try {
            require(ControlOutputMoveDispatcher.offerNativeMove(1,0,47L,1000L));
            require(!ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(1,0,48L,1_000_000L));
        } finally {ControlOutputMoveDispatcher.clearSink(legacy);}
    }

    private static final class AbsoluteSink implements ControlOutputMoveSink {
        final CountDownLatch entered=new CountDownLatch(1),release;
        volatile long now;
        long absoluteDeadline,lastTicket;
        int relativeCalls,calls;
        boolean discarded;
        AbsoluteSink(boolean block){release=new CountDownLatch(block?1:0);}
        public boolean offerMoveFromNative(int x,int y,long ticket,long budget){++relativeCalls;return false;}
        public boolean offerMoveBeforeDeadline(int x,int y,long ticket,long deadline){
            entered.countDown();await(release);
            ++calls;absoluteDeadline=deadline;lastTicket=ticket;
            discarded=ControlMoveDeadline.isExpired(now,deadline);
            return true; // handled; an old offer is discarded without execution.
        }
        public boolean suspendMoveDeliveryForNativeRecovery(long generation){return true;}
        public boolean resumeMoveDeliveryAfterNativeRecovery(long generation){return true;}
        public boolean failClosedNativeMoveDelivery(){return true;}
    }

    private static void await(CountDownLatch latch) {
        try {require(latch.await(3,TimeUnit.SECONDS));}
        catch(InterruptedException e){Thread.currentThread().interrupt();throw new AssertionError(e);}
    }
    private static void join(Thread thread) {
        try {thread.join(3000);require(!thread.isAlive());}
        catch(InterruptedException e){Thread.currentThread().interrupt();throw new AssertionError(e);}
    }

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Control move deadline contract failed");
    }
}
