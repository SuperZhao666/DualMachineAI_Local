package com.visionforge.inferencebenchmark;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

final class AsyncDiagnosticLogSelfTest {
    static void run() throws Exception {
        File directory = Files.createTempDirectory("vf-diagnostic-test").toFile();
        File file = new File(directory, "events.jsonl");
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AsyncDiagnosticLog log = new AsyncDiagnosticLog(file, line -> {
            if (line.equals("first")) {
                entered.countDown();
                try { release.await(3, TimeUnit.SECONDS); }
                catch (InterruptedException e) { throw new RuntimeException(e); }
            }
        }, 4, 128);
        log.offer("first");
        if (!entered.await(3, TimeUnit.SECONDS)) throw new AssertionError("writer never started");
        // Deliberately block the writer: saturation must drop logs, not stall
        // the control producer or grow memory without a bound.
        for (int index = 0; index < 20; ++index) log.offer("event-" + index);
        if (!log.status().contains("log_dropped=16")) throw new AssertionError(log.status());
        release.countDown(); log.flush();
        for (int index = 0; index < 15; ++index) {
            log.offer("{\"event\":\"rotation-" + index + "\"}"); log.flush();
        }
        ByteArrayOutputStream exported = new ByteArrayOutputStream();
        log.exportTo(exported);
        String text = exported.toString(StandardCharsets.UTF_8.name());
        if (!text.endsWith("{\"event\":\"rotation-14\"}\n")) throw new AssertionError(text);
        if (directory.listFiles().length != 3) throw new AssertionError("rotation count");
        for (File rotated : directory.listFiles()) if (rotated.length() > 128) throw new AssertionError("unbounded file");
        if (!log.status().contains("log_io_failures=0")) throw new AssertionError(log.status());
        File retainedFile=new File(directory,"extended.jsonl");
        AsyncDiagnosticLog extended=new AsyncDiagnosticLog(retainedFile,line->{},4,64,16);
        for(int i=0;i<20;++i){extended.offer("event-"+i+"-"+"x".repeat(45));extended.flush();}
        ByteArrayOutputStream retained=new ByteArrayOutputStream();extended.exportTo(retained);
        String history=retained.toString(StandardCharsets.UTF_8.name());
        if(history.contains("event-3-") || !history.startsWith("event-4-") || !history.contains("event-19-"))
            throw new AssertionError("extended rotation order: "+history);
        if(!extended.status().contains("log_retained_segments=16"))throw new AssertionError(extended.status());
        File traceFile=new File(directory,"large-trace.jsonl");
        AsyncDiagnosticLog full=new AsyncDiagnosticLog(traceFile,line->{throw new AssertionError("large logcat duplicate");},4,8192);
        String longLine="x".repeat(5000);full.offer(longLine);full.flush();
        if(!Files.readString(traceFile.toPath()).equals(longLine+"\n"))throw new AssertionError("local trace truncated");
        if(!full.status().contains("log_large_logcat_copies_omitted=1"))throw new AssertionError(full.status());
    }
}
