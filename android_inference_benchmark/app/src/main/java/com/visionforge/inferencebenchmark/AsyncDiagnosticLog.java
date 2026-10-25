package com.visionforge.inferencebenchmark;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;
import java.util.ArrayList;

/** Bounded, rotating local log. Producers never wait for disk or logcat. */
final class AsyncDiagnosticLog {
    private final File file;
    private final long maximumBytes;
    private final int retainedSegments;
    private final Consumer<String> observer;
    private final ArrayBlockingQueue<Object> queue;
    private final Object ioLock = new Object();
    private final AtomicLong dropped = new AtomicLong();
    private final AtomicLong failures = new AtomicLong();
    private final AtomicLong writtenBytes = new AtomicLong(), writtenRecords = new AtomicLong();
    private final AtomicLong writeMicros = new AtomicLong(), maximumWriteMicros = new AtomicLong();
    private final AtomicLong queueHighWater = new AtomicLong(), observerOmitted = new AtomicLong();
    private BufferedOutputStream output;
    private long bytes;

    AsyncDiagnosticLog(File file, Consumer<String> observer) {
        this(file, observer, 256, 32L * 1024 * 1024, 16);
    }

    AsyncDiagnosticLog(File file, Consumer<String> observer, int capacity, long maximumBytes) {
        this(file, observer, capacity, maximumBytes, 3);
    }

    AsyncDiagnosticLog(File file, Consumer<String> observer, int capacity, long maximumBytes, int retainedSegments) {
        if (capacity < 1 || maximumBytes < 1 || retainedSegments < 1 || retainedSegments > 32)
            throw new IllegalArgumentException("invalid diagnostic storage bounds");
        this.file = file;
        this.observer = observer;
        this.maximumBytes = maximumBytes;
        this.retainedSegments = retainedSegments;
        queue = new ArrayBlockingQueue<>(capacity);
        Thread writer = new Thread(this::run, "visionforge-diagnostics");
        writer.setDaemon(true);
        writer.start();
    }

    void offer(String line) {
        if (!queue.offer(line)) dropped.incrementAndGet();
        queueHighWater.accumulateAndGet(queue.size(), Math::max);
    }

    String status() {
        return "log_queue=" + queue.size() + " log_dropped=" + dropped.get()
                + " log_io_failures=" + failures.get()
                + " log_max_segment_bytes=" + maximumBytes + " log_retained_segments=" + retainedSegments
                + " log_total_written_bytes=" + writtenBytes.get() + " log_written_records=" + writtenRecords.get()
                + " log_writer_us=" + writeMicros.get() + " log_writer_max_us=" + maximumWriteMicros.get()
                + " log_queue_high_water=" + queueHighWater.get() + " log_large_logcat_copies_omitted=" + observerOmitted.get();
    }

    private void run() {
        while (true) {
            Object item = null;
            try {
                item = queue.poll(1, TimeUnit.SECONDS);
                synchronized (ioLock) {
                    if (item instanceof String) {
                        long started = System.nanoTime();
                        String line = (String) item;
                        byte[] encoded = (line + "\n").getBytes(StandardCharsets.UTF_8);
                        if (encoded.length > maximumBytes) { dropped.incrementAndGet(); continue; }
                        if (output == null) open();
                        if (bytes + encoded.length > maximumBytes) rotate();
                        output.write(encoded);
                        bytes += encoded.length;
                        writtenBytes.addAndGet(encoded.length);
                        writtenRecords.incrementAndGet();
                        if (queue.isEmpty()) output.flush();
                        // Full trace records live in the local file. Logcat
                        // truncates large lines and duplicating them adds cost.
                        if (encoded.length <= 3500) observer.accept(line);
                        else observerOmitted.incrementAndGet();
                        long duration = (System.nanoTime() - started) / 1000L;
                        writeMicros.addAndGet(duration);
                        maximumWriteMicros.accumulateAndGet(duration, Math::max);
                    } else {
                        if (output != null) output.flush();
                    }
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt(); return;
            } catch (IOException | RuntimeException e) {
                failures.incrementAndGet();
                synchronized (ioLock) {
                    try { if (output != null) output.close(); } catch (IOException ignored) { }
                    output = null;
                }
            } finally {
                if (item instanceof CountDownLatch) ((CountDownLatch) item).countDown();
            }
        }
    }

    private void open() throws IOException {
        Files.createDirectories(file.toPath().getParent());
        bytes = file.length();
        output = new BufferedOutputStream(new FileOutputStream(file, true), 32 * 1024);
    }

    private File rotated(int index) { return new File(file.getPath() + "." + index); }

    private void rotate() throws IOException {
        output.close(); output = null;
        if (retainedSegments == 1) Files.deleteIfExists(file.toPath());
        else {
            for (int i = retainedSegments - 2; i >= 1; --i)
                if (rotated(i).isFile()) Files.move(rotated(i).toPath(), rotated(i + 1).toPath(), StandardCopyOption.REPLACE_EXISTING);
            if (file.isFile()) Files.move(file.toPath(), rotated(1).toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
        open();
    }

    void flush() throws IOException {
        CountDownLatch barrier = new CountDownLatch(1);
        try {
            if (!queue.offer(barrier, 5, TimeUnit.SECONDS)
                    || !barrier.await(5, TimeUnit.SECONDS)) throw new IOException("diagnostic flush timed out");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt(); throw new IOException("diagnostic flush interrupted", e);
        }
    }

    long exportTo(OutputStream destination) throws IOException {
        flush();
        ArrayList<SnapshotSegment> snapshot = new ArrayList<>();
        try {
            synchronized (ioLock) {
                if (output != null) output.flush();
                for (int index = retainedSegments - 1; index >= 0; --index) {
                    File source = index == 0 ? file : rotated(index);
                    if (source.isFile()) snapshot.add(SnapshotSegment.open(source));
                }
            }
            // Only local handles/aliases and byte limits are acquired under
            // the writer lock. Provider I/O cannot prevent rotation or writes.
            long copied = 0;
            byte[] buffer = new byte[16 * 1024];
            for (SnapshotSegment segment : snapshot) {
                long remaining = segment.length;
                while (remaining > 0) {
                    if (Thread.currentThread().isInterrupted())
                        throw new InterruptedIOException("diagnostic export cancelled");
                    int size = segment.input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
                    if (size < 0) throw new EOFException("diagnostic snapshot truncated");
                    destination.write(buffer, 0, size);
                    copied += size;
                    remaining -= size;
                }
            }
            destination.flush();
            return copied;
        } finally {
            IOException cleanupFailure = null;
            for (SnapshotSegment segment : snapshot) {
                try { segment.close(); } catch (IOException failure) { cleanupFailure = failure; }
            }
            if (cleanupFailure != null) failures.incrementAndGet();
        }
    }

    private static final class SnapshotSegment implements Closeable {
        final FileInputStream input;
        final long length;
        final java.nio.file.Path alias;
        private SnapshotSegment(FileInputStream input, long length, java.nio.file.Path alias) {
            this.input = input; this.length = length; this.alias = alias;
        }
        static SnapshotSegment open(File source) throws IOException {
            long length = source.length();
            java.nio.file.Path alias = source.toPath().resolveSibling(
                    "." + source.getName() + ".export-" + java.util.UUID.randomUUID());
            try {
                // NTFS aliases avoid Windows open-handle rename restrictions.
                // App-private Android filesystems may also support this path.
                Files.createLink(alias, source.toPath());
                return new SnapshotSegment(new FileInputStream(alias.toFile()), length, alias);
            } catch (IOException | UnsupportedOperationException unavailable) {
                Files.deleteIfExists(alias);
                // Android's emulated storage can reject hard links. A POSIX
                // open descriptor retains the inode through rename/unlink.
                return new SnapshotSegment(new FileInputStream(source), length, null);
            }
        }
        @Override public void close() throws IOException {
            try { input.close(); }
            finally { if (alias != null) Files.deleteIfExists(alias); }
        }
    }
}
