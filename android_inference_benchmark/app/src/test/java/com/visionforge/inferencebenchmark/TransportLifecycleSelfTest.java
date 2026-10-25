package com.visionforge.inferencebenchmark;

import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Real coordinator/core/sink races; fake session never reaches Android/HID. */
final class TransportLifecycleSelfTest {
    static void run() throws Exception {
        int failures = 0;
        for (boolean successfulCallback : new boolean[]{false, true}) {
            try { disposedRouteCannotCloseTheReplacement(successfulCallback); }
            catch (AssertionError failed) { ++failures; System.out.println("FAIL stale callback " + successfulCallback); }
        }
        for (String mode : new String[]{"recovery", "explicit_reenable", "failclose_reenable", "reconnect"}) {
            try { revokedPendingDoesNotRejectTheNewGeneration(mode); }
            catch (AssertionError failed) { ++failures; System.out.println("FAIL pending generation " + mode); }
        }
        activeCallbackBeforeDisposeStillCloses();
        closedMailboxCannotBeReopened();
        admissionDoesNotTakeTheCoreApiMonitor();
        closeDrainsTheActiveApiBeforeReturning();
        require(failures == 0);
    }
    private static void require(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void await(CountDownLatch latch) {
        try { require(latch.await(3, TimeUnit.SECONDS)); }
        catch (InterruptedException failure) { throw new AssertionError(failure); }
    }
    private static Object field(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name); field.setAccessible(true); return field.get(object);
    }
    private static final class Session implements BluetoothHidMouseTransportCore.SessionPort {
        volatile BluetoothHidOutputFailClosedPolicy.SessionState state =
                BluetoothHidOutputFailClosedPolicy.SessionState.ready();
        Runnable listener;
        CountDownLatch sendEntered, sendRelease;
        AtomicInteger sends = new AtomicInteger();
        public BluetoothHidOutputFailClosedPolicy.SessionState sessionState() { return state; }
        public boolean sendMouseReport(byte[] report) {
            sends.incrementAndGet();
            if(sendEntered!=null)sendEntered.countDown();
            if(sendRelease!=null)await(sendRelease);
            return true;
        }
        public void connectFirstSupportedHost() {}
        public void setSessionStateListener(Runnable listener) { this.listener = listener; }
    }
    private static final class Profile {
        boolean enabled;
        int applyCalls, reads;
        ControlProfilePort port() {
            return (ControlProfilePort) Proxy.newProxyInstance(ControlProfilePort.class.getClassLoader(),
                    new Class<?>[]{ControlProfilePort.class}, (proxy, method, args) -> {
                        if (method.getName().equals("apply")) { enabled = (boolean) args[0]; ++applyCalls; return true; }
                        ++reads;
                        if (method.getName().equals("controlTrigger")) return ControlTrigger.ALWAYS;
                        if (method.getReturnType() == float.class) return 1F;
                        if (method.getReturnType() == int.class) return 25;
                        if (method.getReturnType() == boolean.class) return false;
                        if (method.getReturnType() == String.class) return "";
                        return null;
                    });
        }
    }
    private static final class ReadyTransport implements ControlOutputTransport {
        boolean enabled;
        public boolean isReady() { return true; }
        public void connectFirstSupportedDevice() {}
        public void setReconnectListener(ReconnectListener listener) {}
        public boolean setOutputDeliveryAllowed(boolean allowed) { enabled=allowed; return true; }
        public MakcuDeliveryState deliveryState() { return new MakcuDeliveryState(0,0,0,0,0,false,""); }
    }
    private static final class Buttons implements ControlButtonInput {
        Listener listener;
        int maskUpdates;
        public void setButtonStateListener(Listener listener) { this.listener=listener; }
        public void setRequiredTriggerMask(int mask) { ++maskUpdates; }
        public int currentButtonMask() { return 0; }
        public boolean isButtonStreamReady() { return true; }
    }
    private static void disposedRouteCannotCloseTheReplacement(boolean successfulCallback) throws Exception {
        Session session = new Session();
        if (successfulCallback) session.state = session.state.withHostConnected(false);
        BluetoothHidMouseTransportCore oldCore = new BluetoothHidMouseTransportCore(session, (ticket, us) -> true);
        Profile profile = new Profile(); ControlProfilePort shared = profile.port();
        Buttons buttons = new Buttons();
        ControlOutputCoordinator old = new ControlOutputCoordinator(oldCore, buttons, shared, (event, detail) -> {});
        ControlButtonInput.Listener staleButton = buttons.listener;
        if (!successfulCallback) old.reconcileAutomaticState(true, true, false, false, false);
        ControlOutputTransport.ReconnectListener callback = (ControlOutputTransport.ReconnectListener)
                field(oldCore, "reconnectListener");
        CountDownLatch captured = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        oldCore.setReconnectListener(ready -> {
            require(!Thread.holdsLock(oldCore));
            captured.countDown(); await(release); callback.onReconnectResult(ready);
        });
        session.state = session.state.withHostConnected(successfulCallback);
        Runnable stateListener = session.listener;
        Thread stale = new Thread(() -> { try { stateListener.run(); } catch (Throwable error) { failure.set(error); } });
        stale.start(); await(captured);
        old.dispose(); oldCore.dispose();
        ReadyTransport currentTransport = new ReadyTransport();
        ControlOutputCoordinator current = new ControlOutputCoordinator(currentTransport, shared, (event, detail) -> {});
        current.reconcileAutomaticState(true, true, false, false, false);
        int applies = profile.applyCalls, reads = profile.reads, masks=buttons.maskUpdates;
        require(profile.enabled && currentTransport.enabled && current.isOutputEnabled());
        release.countDown(); stale.join(3000);
        require(!stale.isAlive() && failure.get() == null);
        System.out.println("late_route_callback success=" + successfulCallback
                + " extra_applies=" + (profile.applyCalls-applies) + " native_enabled=" + profile.enabled);
        require(profile.enabled && profile.applyCalls == applies && profile.reads == reads);
        staleButton.onButtonMaskChanged(0x10);
        require(profile.enabled && profile.applyCalls == applies && profile.reads == reads
                && buttons.maskUpdates == masks);
        old.dispose(); old.disable(); old.failClosed("late_close");
        old.reconcileAutomaticState(true, true, false, false, false);
        require(profile.enabled && profile.applyCalls == applies);
        current.dispose(); require(!profile.enabled && !currentTransport.enabled);
    }
    private static void activeCallbackBeforeDisposeStillCloses() {
        Session session = new Session(); Profile profile = new Profile();
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session, (ticket, us) -> true);
        ControlOutputCoordinator coordinator = new ControlOutputCoordinator(core, profile.port(), (event, detail) -> {});
        coordinator.reconcileAutomaticState(true, true, false, false, false);
        require(profile.enabled && coordinator.isOutputEnabled());
        session.state=session.state.withHostConnected(false); session.listener.run();
        require(!profile.enabled && !coordinator.isOutputEnabled() && core.nativeDeliveryGeneration()==0);
        coordinator.dispose();
        require(!profile.enabled && core.nativeDeliveryGeneration()==0);
        core.dispose();
    }
    private static void revokedPendingDoesNotRejectTheNewGeneration(String mode) throws Exception {
        Session session = new Session(); AtomicInteger failures = new AtomicInteger(), discarded = new AtomicInteger();
        CountDownLatch completed = new CountDownLatch(1);
        BluetoothHidMouseTransportCore core = new BluetoothHidMouseTransportCore(session,
                (ticket, us) -> { require(ticket == 102); completed.countDown(); return true; }, () -> 1_000_000_000L);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink = new AsyncBluetoothMoveSink(core,
                ticket -> failures.incrementAndGet(), ticket -> discarded.incrementAndGet(), () -> 1_000_000_000L);
        boolean offered;
        try {
            Object mailbox = field(sink, "lock");
            synchronized (mailbox) {
                require(sink.offerMoveFromNative(5, 0, 101, 250_000));
                require(!sink.offerMoveFromNative(9, 0, 900, 250_000));
                if (mode.equals("recovery")) {
                    require(sink.suspendMoveDeliveryForNativeRecovery(71));
                    require(sink.resumeMoveDeliveryAfterNativeRecovery(71));
                } else if (mode.equals("failclose_reenable")) {
                    require(sink.failClosedNativeMoveDelivery());
                    require(core.setOutputDeliveryAllowed(true));
                } else if (mode.equals("reconnect")) {
                    session.state=session.state.withHostConnected(false); session.listener.run();
                    require(core.deliveryState().circuitOpen);
                    session.state=session.state.withHostConnected(true); core.connectFirstSupportedDevice();
                    require(core.setOutputDeliveryAllowed(true));
                } else {
                    require(core.setOutputDeliveryAllowed(false));
                    require(core.setOutputDeliveryAllowed(true));
                }
                offered = sink.offerMoveFromNative(6, 0, 102, 250_000);
                System.out.println("pending_before_take mode=" + mode + " new_offer=" + offered);
                require(offered);
                require(!sink.offerMoveFromNative(9, 0, 901, 250_000));
                require(session.sends.get() == 0);
            }
            await(completed);
            require(session.sends.get() == 1 && failures.get() == 0 && discarded.get() == 0);
            awaitWorkerIdle(sink);
            String trace = sink.drainTrace();
            String rows = trace.substring(trace.indexOf("delivery_trace_v1=[")+19,trace.length()-1);
            boolean retired=false,sent=false;
            for(String row:rows.split(";")) {
                String[] values=row.split(",");
                long ticket=Long.parseLong(values[1]);
                if(ticket==101) { require(values[11].equals("3") && values[12].equals("0")); retired=true; }
                else if(ticket==102) { require(values[11].equals("1")); sent=true; }
                else throw new AssertionError("unexpected trace ticket " + ticket);
            }
            require(retired && sent && sink.report().contains("generation_retired=1"));
        } finally { sink.close(); }
    }

    private static void closedMailboxCannotBeReopened() throws Exception {
        Session session=new Session(); AtomicInteger failures=new AtomicInteger();
        BluetoothHidMouseTransportCore core=new BluetoothHidMouseTransportCore(session,(ticket,us)->true,()->1_000_000_000L);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink=new AsyncBluetoothMoveSink(core,ticket->{require(ticket==201);failures.incrementAndGet();},()->1_000_000_000L);
        synchronized(field(sink,"lock")) {
            require(sink.offerMoveFromNative(5,0,201,250_000));
            sink.close();
            require(core.setOutputDeliveryAllowed(true));
            require(!sink.offerMoveFromNative(5,0,202,250_000));
        }
        require(session.sends.get()==0 && failures.get()==1);
        core.dispose();
    }
    private static void admissionDoesNotTakeTheCoreApiMonitor() throws Exception {
        Session session=new Session(); CountDownLatch offered=new CountDownLatch(1), completed=new CountDownLatch(1);
        BluetoothHidMouseTransportCore core=new BluetoothHidMouseTransportCore(session,
                (ticket,us)->{require(ticket==301);completed.countDown();return true;},()->1_000_000_000L);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink=new AsyncBluetoothMoveSink(core,ticket->{throw new AssertionError();},()->1_000_000_000L);
        AtomicReference<Throwable> failed=new AtomicReference<>();
        Thread producer=new Thread(()->{
            try { require(sink.offerMoveFromNative(5,0,301,250_000)); }
            catch(Throwable failure) { failed.set(failure); }
            finally { offered.countDown(); }
        });
        try {
            synchronized(core) { producer.start(); await(offered); require(failed.get()==null); }
            producer.join(3000); require(!producer.isAlive()); await(completed);
            require(session.sends.get()==1);
        } finally { sink.close(); }
    }
    private static void closeDrainsTheActiveApiBeforeReturning() throws Exception {
        Session session=new Session();
        session.sendEntered=new CountDownLatch(1);session.sendRelease=new CountDownLatch(1);
        CountDownLatch accepted=new CountDownLatch(1),closed=new CountDownLatch(1);
        AtomicInteger failures=new AtomicInteger();
        BluetoothHidMouseTransportCore core=new BluetoothHidMouseTransportCore(session,
                (ticket,us)->{require(ticket==401);accepted.countDown();return true;},()->1_000_000_000L);
        require(core.setOutputDeliveryAllowed(true));
        AsyncBluetoothMoveSink sink=new AsyncBluetoothMoveSink(core,ticket->failures.incrementAndGet(),()->1_000_000_000L);
        Thread closer=new Thread(()->{try{sink.close();}finally{closed.countDown();}});
        try {
            require(sink.offerMoveFromNative(5,0,401,250_000));await(session.sendEntered);
            closer.start();
            long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(3);
            while(closer.getState()!=Thread.State.BLOCKED && System.nanoTime()<end)Thread.yield();
            require(closer.getState()==Thread.State.BLOCKED && closed.getCount()==1);
            session.sendRelease.countDown();await(accepted);await(closed);closer.join(3000);
            require(!closer.isAlive() && failures.get()==0 && core.nativeDeliveryGeneration()==0);
            require(!sink.offerMoveFromNative(5,0,402,250_000) && session.sends.get()==1);
        } finally {session.sendRelease.countDown();sink.close();}
    }
    private static void awaitWorkerIdle(AsyncBluetoothMoveSink sink) throws Exception {
        Thread worker=(Thread)field(sink,"worker");
        long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(3);
        while(System.nanoTime()<end) { if(worker.getState()==Thread.State.WAITING)return; Thread.yield(); }
        throw new AssertionError("worker did not drain");
    }
}
