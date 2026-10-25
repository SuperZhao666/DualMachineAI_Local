package com.visionforge.inferencebenchmark;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class MakcuDeviceDiagnosticsSelfTest {
    static void run() {
        readsFragmentedV4MetadataWithoutMutations();
        autoIsNotARejection();
        legacyRouteNeverReceivesV4Opcodes();
        malformedAndMissingVersionNeverBecomeFirmwareProof();
        cancellationStopsQueries();
        elapsedBudgetPreservesPartialReply();
        promptCanPrecedeTrailingBytes();
        boundedReadsAndBytesRemainOptional();
        lateAndCancelledCompleteRepliesNeverEnableBinaryQueries();
    }
    private static void readsFragmentedV4MetadataWithoutMutations() {
        FakePort p = new FakePort(); p.chunk = 1; p.spread = 80;
        MakcuDeviceDiagnostics.Snapshot s = MakcuDeviceDiagnostics.inspect(p, () -> false);
        // Fragmentation is not a timeout: the complete legal route fits both
        // the elapsed-time and byte budget, even when every read has one byte.
        require(s.firmwareVersion == 4102 && s.interpolation == 80);
        require(s.queryEvidence.contains("route_matched_calls"));
        p = new FakePort(); p.chunk = 5; p.spread = 80;
        s = MakcuDeviceDiagnostics.inspect(p, () -> false);
        require(s.firmwareVersion == 4102 && s.interpolation == 80);
        require(s.route.equals("R:MK;M:8uf;K:8uf;C:0uf"));
        require(p.writes.size() == 3);
        require(Arrays.equals(p.writes.get(0), "km.device()\r\n".getBytes(StandardCharsets.US_ASCII)));
        require(Arrays.equals(p.writes.get(1), new byte[]{(byte)0xde,(byte)0xad,0,0,4}));
        require(Arrays.equals(p.writes.get(2), new byte[]{(byte)0xde,(byte)0xad,0,0,31}));
        require(s.summary().contains("device_settings_read_only=true"));
    }
    private static void autoIsNotARejection() {
        FakePort p = new FakePort();
        MakcuDeviceDiagnostics.Snapshot s = MakcuDeviceDiagnostics.inspect(p, () -> false);
        require(s.interpolation == 255 && s.summary().contains("interpolation=auto"));
        p = new FakePort(); p.spread = 101;
        require(MakcuDeviceDiagnostics.inspect(p, () -> false).interpolation == -1);
    }
    private static void legacyRouteNeverReceivesV4Opcodes() {
        FakePort p = new FakePort(); p.route = "km.(mouse)\r\n>>> ";
        require(MakcuDeviceDiagnostics.inspect(p, () -> false).firmwareVersion == -1);
        require(p.writes.size() == 1);
        p = new FakePort(); p.route = "km.R:MK;M:8uf;K:8uf;C:0uf\r\n";
        require(MakcuDeviceDiagnostics.inspect(p, () -> false).firmwareVersion == -1);
        require(p.writes.size() == 1);
    }
    private static void malformedAndMissingVersionNeverBecomeFirmwareProof() {
        for(byte[] response : new byte[][] {
                {}, {(byte)0xde,(byte)0xad,1,0,4,(byte)0xff},
                {(byte)0xde,(byte)0xad,4,0,4,6,16},
                {(byte)0xde,(byte)0xad,4,0,5,6,16,0,0},
                {(byte)0xde,(byte)0xad,(byte)0xff,(byte)0xff,4,6,16,0,0}}) {
            FakePort p = new FakePort(); p.versionReply = response;
            MakcuDeviceDiagnostics.Snapshot s = MakcuDeviceDiagnostics.inspect(p, () -> false);
            require(s.firmwareVersion == -1 && s.interpolation == 255);
        }
        FakePort p = new FakePort();
        p.versionReply = new byte[]{0,13,10,(byte)0xde,(byte)0xad,1,0,31,80,
                (byte)0xde,(byte)0xad,4,0,4,6,16,0,0};
        require(MakcuDeviceDiagnostics.inspect(p, () -> false).firmwareVersion == 4102);
    }
    private static void cancellationStopsQueries() {
        FakePort p = new FakePort();
        require(MakcuDeviceDiagnostics.inspect(p, () -> true).firmwareVersion == -1);
        require(p.writes.isEmpty());
        p = new FakePort(); boolean[] cancelled = {false};
        p.onRead = () -> cancelled[0] = true;
        require(MakcuDeviceDiagnostics.inspect(p, () -> cancelled[0]).firmwareVersion == -1);
        require(p.writes.size() == 1);
    }
    private static void elapsedBudgetPreservesPartialReply() {
        FakePort p = new FakePort();
        p.route = "km.R:MK;M:8uf;K:8uf;C:0uf";
        p.chunk = 1;
        MakcuDeviceDiagnostics.Snapshot s = MakcuDeviceDiagnostics.inspect(
                p, () -> false, () -> p.reads * 50_000_000L);
        require(s.firmwareVersion == -1 && s.interpolation == -1);
        require(p.reads == 2 && p.writes.size() == 1);
        require(s.queryEvidence.contains("route_deadline_calls2_keptbytes2_hex6b6d"));
        require(!s.route.equals("query_timeout"));
    }
    private static void promptCanPrecedeTrailingBytes() {
        FakePort p = new FakePort();
        p.route += "unrelated_trailing_bytes";
        MakcuDeviceDiagnostics.Snapshot s = MakcuDeviceDiagnostics.inspect(p, () -> false);
        require(s.firmwareVersion == 4102 && s.interpolation == 255);
        require(s.route.equals("R:MK;M:8uf;K:8uf;C:0uf"));
    }
    private static void boundedReadsAndBytesRemainOptional() {
        FakePort silent = new FakePort(); silent.route = "";
        MakcuDeviceDiagnostics.Snapshot s = MakcuDeviceDiagnostics.inspect(silent, () -> false, () -> 0L);
        require(s.firmwareVersion == -1 && silent.reads == 128 && silent.writes.size() == 1);
        require(s.queryEvidence.contains("route_read_limit_calls128_keptbytes0_hexnone"));
        FakePort excessive = new FakePort(); excessive.route = "x".repeat(700);
        s = MakcuDeviceDiagnostics.inspect(excessive, () -> false, () -> 0L);
        require(s.firmwareVersion == -1 && excessive.writes.size() == 1);
        require(s.queryEvidence.contains("route_byte_limit_calls4_keptbytes512_"));
        require(excessive.bytesRead == 512);
        excessive = new FakePort(); excessive.route = "x".repeat(700); excessive.chunk = 30;
        s = MakcuDeviceDiagnostics.inspect(excessive, () -> false, () -> 0L);
        require(s.firmwareVersion == -1 && excessive.bytesRead == 512 && excessive.reads == 18);
    }
    private static void lateAndCancelledCompleteRepliesNeverEnableBinaryQueries() {
        FakePort late = new FakePort(); long[] clock = {0};
        late.onRead = () -> clock[0] = 100_000_000L;
        MakcuDeviceDiagnostics.Snapshot s = MakcuDeviceDiagnostics.inspect(late, () -> false, () -> clock[0]);
        require(s.firmwareVersion == -1 && late.writes.size() == 1);
        require(s.queryEvidence.contains("route_deadline_calls1_keptbytes"));
        require(s.queryEvidence.contains("_hex6b6d"));
        FakePort cancelled = new FakePort(); boolean[] cancel = {false};
        cancelled.onRead = () -> cancel[0] = true;
        s = MakcuDeviceDiagnostics.inspect(cancelled, () -> cancel[0], () -> 0L);
        require(s.firmwareVersion == -1 && cancelled.writes.size() == 1);
        require(s.queryEvidence.contains("route_cancelled_calls1_keptbytes"));
        require(s.queryEvidence.contains("_hex6b6d"));
    }
    private static void require(boolean value) { if (!value) throw new AssertionError(); }
    private static final class FakePort implements MakcuProtocolNegotiator.Port {
        String route = "km.R:MK;M:8uf;K:8uf;C:0uf\r\n>>> ";
        byte[] versionReply = {(byte)0xde,(byte)0xad,4,0,4,6,16,0,0};
        int spread = 255, chunk = 128;
        int reads, bytesRead;
        Runnable onRead;
        final List<byte[]> writes = new ArrayList<>();
        final ArrayDeque<Byte> pending = new ArrayDeque<>();
        @Override public boolean open(int baud) { throw new AssertionError("diagnostics must not reopen"); }
        @Override public void close() { throw new AssertionError("diagnostics must not close"); }
        @Override public int write(byte[] bytes, int timeout) {
            writes.add(bytes.clone());
            byte[] response = bytes[0] == 'k' ? route.getBytes(StandardCharsets.US_ASCII)
                    : bytes[4] == 4 ? versionReply
                    : new byte[]{(byte)0xde,(byte)0xad,1,0,31,(byte)spread};
            for(byte b : response) pending.add(b);
            return bytes.length;
        }
        @Override public int read(byte[] destination, int timeout) {
            ++reads;
            if(onRead != null) { Runnable callback = onRead; onRead = null; callback.run(); }
            int count = Math.min(Math.min(chunk,destination.length),pending.size());
            for(int i=0;i<count;++i) destination[i] = pending.removeFirst();
            bytesRead += count;
            return count;
        }
    }
}
