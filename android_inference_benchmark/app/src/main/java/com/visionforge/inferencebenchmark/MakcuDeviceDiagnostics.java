package com.visionforge.inferencebenchmark;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Read-only V4 discovery while the connection negotiator still owns the port. */
final class MakcuDeviceDiagnostics {
    static final String UNKNOWN = "unknown";
    private static final int MAXIMUM_BYTES = 512;
    private static final int MAXIMUM_READ_CALLS = 128;
    private static final long QUERY_BUDGET_NANOS = 100_000_000L;
    interface Clock { long nanoTime(); }
    static final class Snapshot {
        final long firmwareVersion;
        final int interpolation;
        final String route;
        final String queryEvidence;
        Snapshot(long firmwareVersion, int interpolation, String route) {
            this(firmwareVersion, interpolation, route, "not_sampled");
        }
        Snapshot(long firmwareVersion, int interpolation, String route, String queryEvidence) {
            this.firmwareVersion = firmwareVersion;
            this.interpolation = interpolation;
            this.route = route;
            this.queryEvidence = queryEvidence;
        }
        String summary() {
            return "firmware_version=" + (firmwareVersion > 0 ? firmwareVersion : UNKNOWN)
                    + " interpolation=" + (interpolation < 0 ? UNKNOWN : interpolation == 255 ? "auto" : interpolation)
                    + " route=" + route + " device_settings_read_only=true"
                    + " device_query_evidence=" + queryEvidence;
        }
    }
    static Snapshot unknown() { return new Snapshot(-1, -1, UNKNOWN); }
    private MakcuDeviceDiagnostics() { }

    static Snapshot inspect(MakcuProtocolNegotiator.Port port,
                            MakcuProtocolNegotiator.Cancellation cancellation) {
        return inspect(port, cancellation, System::nanoTime);
    }
    static Snapshot inspect(MakcuProtocolNegotiator.Port port,
                            MakcuProtocolNegotiator.Cancellation cancellation, Clock clock) {
        if (cancellation.isCancelled()) return unknown();
        byte[] routeQuery = "km.device()\r\n".getBytes(StandardCharsets.US_ASCII);
        if (port.write(routeQuery, 50) != routeQuery.length || cancellation.isCancelled()) return unknown();
        QueryRead routeRead = read(port, cancellation, clock, -1, 0);
        String routeReply = new String(routeRead.frame, StandardCharsets.US_ASCII);
        // Legacy binary opcodes differ. Only issue V4 GETs after its documented
        // route/report-period response, never by VID/PID or the MAKCU name alone.
        java.util.regex.Matcher route = java.util.regex.Pattern.compile(
                "R:[A-Z]+;M:[0-9]+uf;K:[0-9]+uf;C:[0-9]+uf").matcher(routeReply);
        if (!routeRead.status.equals("matched") || !routeReply.endsWith(">>> ") || !route.find()) {
            // Preserve a bounded diagnostic reason instead of collapsing a
            // legacy reply, timeout and incomplete frame into one "unknown".
            String token = routeReply.replaceAll("[^A-Za-z0-9:;_().=-]", "_");
            if (token.length() > 96) token = token.substring(0, 96);
            return new Snapshot(-1, -1, token.isEmpty() ? "query_" + routeRead.status : "unrecognized_" + token,
                    "route_" + routeRead.evidence());
        }
        String routeToken = route.group();
        QueryRead versionRead = query(port, cancellation, clock, 0x04, 4);
        byte[] version = versionRead.payload;
        long firmware = version.length == 4 ? (version[0] & 255L) | ((version[1] & 255L) << 8)
                | ((version[2] & 255L) << 16) | ((version[3] & 255L) << 24) : -1;
        QueryRead spreadRead = query(port, cancellation, clock, 0x1f, 1);
        byte[] spread = spreadRead.payload;
        int interpolation = spread.length == 1 ? spread[0] & 255 : -1;
        if (interpolation > 100 && interpolation != 255) interpolation = -1;
        return new Snapshot(firmware, interpolation, routeToken,
                "route_" + routeRead.evidence() + ";version_" + versionRead.evidence()
                        + ";interpolation_" + spreadRead.evidence());
    }

    private static QueryRead query(MakcuProtocolNegotiator.Port port,
                                MakcuProtocolNegotiator.Cancellation cancellation,
                                Clock clock, int opcode, int expectedBytes) {
        if (cancellation.isCancelled()) return new QueryRead("cancelled", 0, new byte[0], new byte[0]);
        // Empty payload is GET only for these two explicitly listed commands.
        byte[] request = {(byte) 0xde, (byte) 0xad, 0, 0, (byte) opcode};
        if (port.write(request, 50) != request.length)
            return new QueryRead("short_write", 0, new byte[0], new byte[0]);
        return read(port, cancellation, clock, opcode, expectedBytes);
    }

    private static final class QueryRead {
        final String status;
        final int calls;
        final byte[] frame, payload;
        QueryRead(String status, int calls, byte[] frame, byte[] payload) {
            this.status = status; this.calls = calls; this.frame = frame; this.payload = payload;
        }
        String evidence() {
            // A missing prompt used to erase even the bytes already received.
            // Keep a bounded raw prefix so legacy, partial and silent replies differ.
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < Math.min(frame.length, 24); ++i) {
                int b = frame[i] & 255;
                hex.append(Character.forDigit(b >>> 4, 16)).append(Character.forDigit(b & 15, 16));
            }
            return status + "_calls" + calls + "_keptbytes" + frame.length
                    + "_hex" + (hex.length() == 0 ? "none" : hex);
        }
    }

    private static QueryRead read(MakcuProtocolNegotiator.Port port,
                               MakcuProtocolNegotiator.Cancellation cancellation,
                               Clock clock, int opcode, int expectedBytes) {
        byte[] buffer = new byte[128], accumulated = new byte[MAXIMUM_BYTES];
        int length = 0, calls = 0;
        long started = clock.nanoTime();
        while (calls < MAXIMUM_READ_CALLS) {
            if (cancellation.isCancelled())
                return new QueryRead("cancelled", calls, Arrays.copyOf(accumulated, length), new byte[0]);
            long remaining = QUERY_BUDGET_NANOS - (clock.nanoTime() - started);
            if (remaining <= 0) return new QueryRead("deadline", calls, Arrays.copyOf(accumulated, length), new byte[0]);
            if (length == accumulated.length)
                return new QueryRead("byte_limit", calls, Arrays.copyOf(accumulated, length), new byte[0]);
            int timeout = (int) Math.min(10L, (remaining + 999_999L) / 1_000_000L);
            int capacity = Math.min(buffer.length, accumulated.length - length);
            byte[] destination = capacity == buffer.length ? buffer : new byte[capacity];
            int count = port.read(destination, Math.max(1, timeout));
            ++calls;
            if (count > destination.length)
                return new QueryRead("invalid_read", calls, Arrays.copyOf(accumulated, length), new byte[0]);
            if (count > 0) {
                System.arraycopy(destination, 0, accumulated, length, count);
                length += count;
            }
            // Preserve bytes already consumed, even on cancellation or a late
            // driver return. A complete but late frame is not query success.
            if (cancellation.isCancelled())
                return new QueryRead("cancelled", calls, Arrays.copyOf(accumulated, length), new byte[0]);
            if (clock.nanoTime() - started >= QUERY_BUDGET_NANOS)
                return new QueryRead("deadline", calls, Arrays.copyOf(accumulated, length), new byte[0]);
            if (count <= 0) continue;
            if (opcode < 0) {
                for (int start = 0; start + 4 <= length; ++start) {
                    if (accumulated[start] == '>' && accumulated[start+1] == '>'
                            && accumulated[start+2] == '>' && accumulated[start+3] == ' ')
                        return new QueryRead("matched", calls, Arrays.copyOf(accumulated, start+4), new byte[0]);
                }
                continue;
            }
            for (int start = 0; start + 5 <= length; ++start) {
                if ((accumulated[start] & 255) != 0xde || (accumulated[start+1] & 255) != 0xad) continue;
                int size = (accumulated[start+2] & 255) | ((accumulated[start+3] & 255) << 8);
                if ((accumulated[start+4] & 255) != opcode || start+5+size > length) continue;
                // INTERPOLATE=FF is AUTO, unlike a one-byte rejection of VERSION.
                return new QueryRead(size == expectedBytes ? "matched" : "unexpected_length", calls,
                        Arrays.copyOf(accumulated, length), size == expectedBytes
                        ? Arrays.copyOfRange(accumulated, start+5, start+5+size) : new byte[0]);
            }
        }
        return new QueryRead("read_limit", calls, Arrays.copyOf(accumulated, length), new byte[0]);
    }
}
