package com.visionforge.inferencebenchmark;

import java.nio.charset.StandardCharsets;

/**
 * Dependency-free MAKCU serial connection state machine.
 *
 * <p>A successful result always leaves the supplied port open at 4 Mbaud.
 * Every failure closes it. No movement command is sent during negotiation.</p>
 */
final class MakcuProtocolNegotiator {
    static final int DEFAULT_BAUD_RATE = 115_200;
    static final int OPERATING_BAUD_RATE = 4_000_000;
    static final byte[] VERSION_QUERY =
            "km.version()\r\n".getBytes(StandardCharsets.US_ASCII);
    static final byte[] BAUD_CHANGE_FRAME = {
            (byte) 0xDE, (byte) 0xAD, 0x05, 0x00, (byte) 0xA5,
            0x00, 0x09, 0x3D, 0x00
    };

    private static final String RESPONSE_PROMPT = ">>> ";
    private static final int WRITE_TIMEOUT_MILLIS = 500;
    private static final int RESPONSE_READ_TIMEOUT_MILLIS = 50;
    private static final int MAXIMUM_RESPONSE_READ_CALLS = 128;
    // Preserve the former10 *50ms response-wait ceiling while allowing a
    // legal identity frame to arrive in more than10 nonblocking fragments.
    private static final long RESPONSE_BUDGET_NANOS = 500_000_000L;
    private static final int DRAIN_READ_TIMEOUT_MILLIS = 10;
    private static final int DRAIN_READ_ATTEMPTS = 16;
    private static final int MAXIMUM_RESPONSE_BYTES = 1_024;
    private static final long BAUD_CHANGE_SETTLE_MILLIS = 100L;
    private static final long OPERATING_REOPEN_SETTLE_MILLIS = 50L;

    interface Port {
        boolean open(int baudRate);
        int write(byte[] bytes, int timeoutMillis);
        int read(byte[] destination, int timeoutMillis);
        void close();
    }

    interface Sleeper {
        void sleep(long millis);
    }

    interface Clock {
        long nanoTime();
    }

    interface Cancellation {
        boolean isCancelled();
    }

    static final class Result {
        final boolean succeeded;
        final String stage;

        private Result(boolean succeeded, String stage) {
            this.succeeded = succeeded;
            this.stage = stage;
        }
    }

    private final Sleeper sleeper;
    private final Clock clock;

    MakcuProtocolNegotiator(Sleeper sleeper) {
        this(sleeper, System::nanoTime);
    }

    MakcuProtocolNegotiator(Sleeper sleeper, Clock clock) {
        this.sleeper = sleeper;
        this.clock = clock;
    }

    Result negotiate(Port port) {
        return negotiate(port, () -> false);
    }

    Result negotiate(Port port, Cancellation cancellation) {
        if (cancellation.isCancelled()) return fail(port, "cancelled");
        if (port.open(OPERATING_BAUD_RATE)
                && !cancellation.isCancelled()
                && verifyVersion(port, cancellation)) {
            return new Result(true, "existing_4mbaud_verified");
        }
        port.close();
        if (cancellation.isCancelled()) return fail(port, "cancelled");
        if (!switchFromDefaultBaud(port, cancellation)) {
            return fail(port, cancellation.isCancelled() ? "cancelled" : "baud_switch_failed");
        }
        if (cancellation.isCancelled()) return fail(port, "cancelled");
        if (!port.open(OPERATING_BAUD_RATE)) return fail(port, "4mbaud_reopen_failed");
        if (cancellation.isCancelled()) return fail(port, "cancelled");
        sleeper.sleep(OPERATING_REOPEN_SETTLE_MILLIS);
        if (cancellation.isCancelled()) return fail(port, "cancelled");
        if (!verifyVersion(port, cancellation)) {
            return fail(port, cancellation.isCancelled()
                    ? "cancelled" : "post_switch_identity_failed");
        }
        return new Result(true, "baud_switched_and_verified");
    }

    private boolean switchFromDefaultBaud(Port port, Cancellation cancellation) {
        if (cancellation.isCancelled()) return false;
        if (!port.open(DEFAULT_BAUD_RATE)) return false;
        if (cancellation.isCancelled() || !drainUntilQuiet(port, cancellation)) return false;
        if (port.write(BAUD_CHANGE_FRAME, WRITE_TIMEOUT_MILLIS)
                != BAUD_CHANGE_FRAME.length) return false;
        if (cancellation.isCancelled()) return false;
        sleeper.sleep(BAUD_CHANGE_SETTLE_MILLIS);
        if (cancellation.isCancelled()) return false;
        port.close();
        return true;
    }

    boolean verifyVersion(Port port, Cancellation cancellation) {
        if (!drainUntilQuiet(port, cancellation) || cancellation.isCancelled()) return false;
        if (port.write(VERSION_QUERY, WRITE_TIMEOUT_MILLIS) != VERSION_QUERY.length) return false;
        if (cancellation.isCancelled()) return false;
        return MakcuDeviceIdentityPolicy.isVerifiedProtocolResponse(
                readResponseFrame(port, cancellation));
    }

    private static boolean drainUntilQuiet(Port port, Cancellation cancellation) {
        byte[] buffer = new byte[256];
        int drainedBytes = 0;
        for (int attempt = 0; attempt < DRAIN_READ_ATTEMPTS; attempt++) {
            if (cancellation.isCancelled()) return false;
            int read = port.read(buffer, DRAIN_READ_TIMEOUT_MILLIS);
            if (cancellation.isCancelled()) return false;
            if (read <= 0) return true;
            drainedBytes += read;
            if (drainedBytes > MAXIMUM_RESPONSE_BYTES) return false;
        }
        return false;
    }

    private String readResponseFrame(Port port, Cancellation cancellation) {
        byte[] buffer = new byte[256];
        byte[] accumulated = new byte[MAXIMUM_RESPONSE_BYTES];
        int length = 0;
        long started = clock.nanoTime();
        for (int attempt = 0; attempt < MAXIMUM_RESPONSE_READ_CALLS; attempt++) {
            if (cancellation.isCancelled()) return "";
            long remaining = RESPONSE_BUDGET_NANOS - (clock.nanoTime() - started);
            if (remaining <= 0) return "";
            int remainingBytes = MAXIMUM_RESPONSE_BYTES - length;
            if (remainingBytes <= 0) return "";
            if (buffer.length > remainingBytes) buffer = new byte[remainingBytes];
            int timeout = (int) Math.min(RESPONSE_READ_TIMEOUT_MILLIS,
                    (remaining + 999_999L) / 1_000_000L);
            int read = port.read(buffer, Math.max(1, timeout));
            if (read > buffer.length) return "";
            if (read > 0) {
                // Retain every valid returned byte before evaluating a
                // cancellation or elapsed deadline. Neither can yield PASS.
                System.arraycopy(buffer, 0, accumulated, length, read);
                length += read;
            }
            if (cancellation.isCancelled()) return "";
            if (clock.nanoTime() - started >= RESPONSE_BUDGET_NANOS) return "";
            if (read <= 0) continue;
            for (int start = 0; start + RESPONSE_PROMPT.length() <= length; ++start) {
                if (accumulated[start] == '>' && accumulated[start+1] == '>'
                        && accumulated[start+2] == '>' && accumulated[start+3] == ' ')
                    return new String(accumulated, 0, start + RESPONSE_PROMPT.length(),
                            StandardCharsets.US_ASCII);
            }
        }
        return "";
    }

    private static Result fail(Port port, String stage) {
        port.close();
        return new Result(false, stage);
    }
}
