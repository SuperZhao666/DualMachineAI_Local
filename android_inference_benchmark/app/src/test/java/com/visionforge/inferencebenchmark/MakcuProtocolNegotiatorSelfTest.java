package com.visionforge.inferencebenchmark;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.List;

/** Dependency-free regression contract for the documented MAKCU baud handshake. */
final class MakcuProtocolNegotiatorSelfTest {
    static void run() {
        verifiesAlreadyConfiguredDevice();
        verifiesDefaultBaudSwitchSequence();
        rejectsEchoAndClosesPort();
        rejectsStaleMoveResponseBeforeIdentity();
        cancellationStopsFurtherHardwareAccess();
        verifiesLegalIdentityOneByteAtATime();
        completeLateIdentityNeverPasses();
        partialDeadlineShortensLastRead();
        cancelledCompletedReadNeverPasses();
        readAndByteBudgetsAreFinite();
        invalidReadCountCannotOverflowBuffer();
        keepsIdentityBytesStrictAndFirstPromptFraming();
        monotonicCounterWrapRetainsShortElapsedBudget();
    }

    private static void verifiesAlreadyConfiguredDevice() {
        FakePort port = new FakePort(true);
        FakeSleeper sleeper = new FakeSleeper();
        MakcuProtocolNegotiator.Result result = negotiator(sleeper).negotiate(port);
        require(result.succeeded);
        require("existing_4mbaud_verified".equals(result.stage));
        require(port.openBauds.equals(List.of(4_000_000)));
        require(port.isOpen);
        require(port.writes.size() == 1);
        require(Arrays.equals(port.writes.get(0), MakcuProtocolNegotiator.VERSION_QUERY));
        require(sleeper.delays.isEmpty());
    }

    private static void verifiesDefaultBaudSwitchSequence() {
        FakePort port = new FakePort(false);
        FakeSleeper sleeper = new FakeSleeper();
        MakcuProtocolNegotiator.Result result = negotiator(sleeper).negotiate(port);
        require(result.succeeded);
        require("baud_switched_and_verified".equals(result.stage));
        require(port.openBauds.equals(List.of(4_000_000, 115_200, 4_000_000)));
        require(port.writes.size() == 3);
        require(Arrays.equals(port.writes.get(1), MakcuProtocolNegotiator.BAUD_CHANGE_FRAME));
        require(Arrays.equals(port.writes.get(2), MakcuProtocolNegotiator.VERSION_QUERY));
        require(sleeper.delays.equals(List.of(100L, 50L)));
        require(port.isOpen);
    }

    private static void rejectsEchoAndClosesPort() {
        FakePort port = new FakePort(true);
        port.identityResponse = "km.version()\r\n>>> ";
        MakcuProtocolNegotiator.Result result = negotiator(millis -> { }).negotiate(port);
        require(!result.succeeded);
        require(!port.isOpen);
    }

    private static void rejectsStaleMoveResponseBeforeIdentity() {
        FakePort port = new FakePort(true);
        port.staleReads.add("km.move(1,2)\r\n>>> ".getBytes(StandardCharsets.US_ASCII));
        MakcuProtocolNegotiator.Result result = negotiator(millis -> { }).negotiate(port);
        require(result.succeeded);
        require(port.isOpen);
    }

    private static void cancellationStopsFurtherHardwareAccess() {
        FakePort port = new FakePort(true);
        boolean[] cancelled = {false};
        port.onRead = () -> cancelled[0] = true;
        MakcuProtocolNegotiator.Result result = negotiator(millis -> { })
                .negotiate(port, () -> cancelled[0]);
        require(!result.succeeded);
        require("cancelled".equals(result.stage));
        require(port.writes.isEmpty());
        require(!port.isOpen);
        require(port.openBauds.equals(List.of(4_000_000)));
    }

    private static MakcuProtocolNegotiator negotiator(MakcuProtocolNegotiator.Sleeper sleeper) {
        return new MakcuProtocolNegotiator(sleeper);
    }

    private static void verifiesLegalIdentityOneByteAtATime() {
        FakePort port = new FakePort(true);
        port.maximumChunk = 1;
        port.nanosPerResponseRead = 30_000_000L;
        MakcuProtocolNegotiator negotiator = new MakcuProtocolNegotiator(millis -> { }, () -> port.nowNanos);
        MakcuProtocolNegotiator.Result result = negotiator.negotiate(port);
        require(result.succeeded && result.stage.equals("existing_4mbaud_verified"));
        require(port.responseReadCalls == 14 && port.responseBytesRead == 14);
        require(port.writes.size() == 1 && port.isOpen);
        // The post-diagnostics identity recheck uses the same fixed reader.
        require(negotiator.verifyVersion(port, () -> false));
        require(port.responseReadCalls == 14 && port.writes.size() == 2);
    }

    private static void completeLateIdentityNeverPasses() {
        for(long elapsed : new long[]{500_000_000L, 500_000_001L}) {
            FakePort port = new FakePort(true);
            port.nanosPerResponseRead = elapsed;
            require(!verifyWithFakeClock(port, () -> false));
            require(port.responseReadCalls == 1 && port.responseBytesRead == 14);
            require(port.writes.size() == 1 && port.responseTimeouts.equals(List.of(50)));
        }
        FakePort justBefore = new FakePort(true);
        justBefore.nanosPerResponseRead = 499_999_999L;
        require(verifyWithFakeClock(justBefore, () -> false));
        require(justBefore.responseReadCalls == 1);
    }

    private static void partialDeadlineShortensLastRead() {
        FakePort port = new FakePort(true);
        port.maximumChunk = 1;
        port.nanosPerResponseRead = 40_000_000L;
        require(!verifyWithFakeClock(port, () -> false));
        require(port.responseReadCalls == 13 && port.responseBytesRead == 13);
        require(port.responseTimeouts.get(port.responseTimeouts.size()-1) == 20);
        for(int timeout : port.responseTimeouts) require(timeout >= 1 && timeout <= 50);
    }

    private static void cancelledCompletedReadNeverPasses() {
        FakePort port = new FakePort(true);
        boolean[] cancelled = {false};
        port.onIdentityRead = () -> cancelled[0] = true;
        require(!verifyWithFakeClock(port, () -> cancelled[0]));
        require(port.responseReadCalls == 1 && port.responseBytesRead == 14 && port.writes.size() == 1);
    }

    private static void readAndByteBudgetsAreFinite() {
        FakePort silent = new FakePort(true);
        silent.identityResponse = "";
        require(!verifyWithFakeClock(silent, () -> false));
        require(silent.responseReadCalls == 128 && silent.responseBytesRead == 0);
        FakePort excessive = new FakePort(true);
        excessive.maximumChunk = 255;
        excessive.identityResponse = "x".repeat(1100);
        require(!verifyWithFakeClock(excessive, () -> false));
        require(excessive.responseBytesRead == 1024 && excessive.responseReadCalls == 5);
        require(excessive.responseDestinations.get(4) == 4);
        require(excessive.writes.size() == 1);
    }

    private static void invalidReadCountCannotOverflowBuffer() {
        FakePort port = new FakePort(true);
        port.invalidReadCount = Integer.MAX_VALUE;
        require(!verifyWithFakeClock(port, () -> false));
        require(port.responseReadCalls == 1);
    }

    private static void keepsIdentityBytesStrictAndFirstPromptFraming() {
        for(String invalid : new String[]{"km.version()\r\n>>> ", "km.MAKCU.more\r\n>>> ",
                "km.MAKCU\n>>> ", "km.version()\r\nkm.MAKCU\r\n>>> ", "km.MAKCU\r\n>>>"}) {
            FakePort port = new FakePort(true);
            port.maximumChunk = 1; port.identityResponse = invalid;
            require(!verifyWithFakeClock(port, () -> false));
        }
        FakePort trailing = new FakePort(true);
        trailing.identityResponse += "unrelated trailing bytes";
        require(verifyWithFakeClock(trailing, () -> false));
    }

    private static void monotonicCounterWrapRetainsShortElapsedBudget() {
        FakePort port = new FakePort(true);
        port.nowNanos = Long.MAX_VALUE - 100_000_000L;
        port.maximumChunk = 1; port.nanosPerResponseRead = 30_000_000L;
        require(verifyWithFakeClock(port, () -> false));
        require(port.responseReadCalls == 14);
    }

    private static boolean verifyWithFakeClock(FakePort port, MakcuProtocolNegotiator.Cancellation cancelled) {
        port.open(MakcuProtocolNegotiator.OPERATING_BAUD_RATE);
        return new MakcuProtocolNegotiator(millis -> { }, () -> port.nowNanos)
                .verifyVersion(port, cancelled);
    }

    private static final class FakeSleeper implements MakcuProtocolNegotiator.Sleeper {
        final List<Long> delays = new ArrayList<>();
        @Override public void sleep(long millis) { delays.add(millis); }
    }

    private static final class FakePort implements MakcuProtocolNegotiator.Port {
        final List<Integer> openBauds = new ArrayList<>();
        final List<byte[]> writes = new ArrayList<>();
        final Deque<byte[]> staleReads = new ArrayDeque<>();
        final boolean respondsAtInitialOperatingBaud;
        boolean isOpen;
        boolean switched;
        int currentBaud;
        String identityResponse = "km.MAKCU\r\n>>> ";
        byte[] pendingResponse;
        Runnable onRead;
        Runnable onIdentityRead;
        int maximumChunk = Integer.MAX_VALUE, invalidReadCount = -1;
        int responseReadCalls, responseBytesRead;
        long nowNanos, nanosPerResponseRead;
        boolean identityQueryActive;
        final List<Integer> responseTimeouts = new ArrayList<>();
        final List<Integer> responseDestinations = new ArrayList<>();

        FakePort(boolean respondsAtInitialOperatingBaud) {
            this.respondsAtInitialOperatingBaud = respondsAtInitialOperatingBaud;
        }

        @Override public boolean open(int baudRate) {
            close();
            currentBaud = baudRate;
            openBauds.add(baudRate);
            isOpen = true;
            return true;
        }

        @Override public int write(byte[] bytes, int timeoutMillis) {
            writes.add(Arrays.copyOf(bytes, bytes.length));
            if (Arrays.equals(bytes, MakcuProtocolNegotiator.VERSION_QUERY)) {
                identityQueryActive = true;
                responseReadCalls = responseBytesRead = 0;
                responseTimeouts.clear(); responseDestinations.clear();
            }
            if (Arrays.equals(bytes, MakcuProtocolNegotiator.BAUD_CHANGE_FRAME)
                    && currentBaud == MakcuProtocolNegotiator.DEFAULT_BAUD_RATE) {
                switched = true;
            } else if (Arrays.equals(bytes, MakcuProtocolNegotiator.VERSION_QUERY)
                    && currentBaud == MakcuProtocolNegotiator.OPERATING_BAUD_RATE
                    && (respondsAtInitialOperatingBaud || switched)) {
                pendingResponse = identityResponse.getBytes(StandardCharsets.US_ASCII);
            }
            return bytes.length;
        }

        @Override public int read(byte[] destination, int timeoutMillis) {
            if (onRead != null) {
                Runnable callback = onRead;
                onRead = null;
                callback.run();
            }
            if (identityQueryActive) {
                ++responseReadCalls; nowNanos += nanosPerResponseRead;
                responseTimeouts.add(timeoutMillis); responseDestinations.add(destination.length);
                if (onIdentityRead != null) {
                    Runnable callback = onIdentityRead; onIdentityRead = null; callback.run();
                }
            }
            boolean stale = !staleReads.isEmpty();
            byte[] response = stale ? staleReads.removeFirst() : pendingResponse;
            if (response == null) return 0;
            int count = Math.min(Math.min(response.length, destination.length), maximumChunk);
            System.arraycopy(response, 0, destination, 0, count);
            byte[] remaining = count == response.length ? null : Arrays.copyOfRange(response, count, response.length);
            if (stale) { if (remaining != null) staleReads.addFirst(remaining); }
            else pendingResponse = remaining;
            if (identityQueryActive) responseBytesRead += count;
            return invalidReadCount >= 0 && identityQueryActive ? invalidReadCount : count;
        }

        @Override public void close() {
            isOpen = false;
            currentBaud = 0;
            pendingResponse = null;
            identityQueryActive = false;
        }
    }

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("MAKCU protocol negotiation contract failed");
    }
}
