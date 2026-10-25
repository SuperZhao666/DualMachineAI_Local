package com.visionforge.inferencebenchmark;

final class MoveDeliveryTraceSelfTest {
    static void run() throws Exception {
        MoveDeliveryTrace trace = new MoveDeliveryTrace();
        for (int i = 1; i <= 2100; ++i)
            trace.record(i, -4, 7, 1000000, 1100000, 1200000, 1400000,
                    1300000, 10000000, 9, 1, 16);
        String batch = trace.drain("makcu_usb", "elapsed_realtime_us");
        if (!batch.contains("delivery_trace_lost=52")) throw new AssertionError(batch.substring(0, 100));
        String[] records = batch.substring(batch.indexOf('[') + 1, batch.length() - 1).split(";");
        if (records.length != 2048 || !records[0].equals("53,53,-4,7,1000,1100,1200,1400,1300,10000,9,1,16,400"))
            throw new AssertionError("ring order, units, identity or timestamps");
        if (!trace.drain("makcu_usb", "elapsed_realtime_us").endsWith("=[]")) throw new AssertionError("duplicate drain");
        MoveDeliveryTrace concurrent = new MoveDeliveryTrace();
        Thread worker = new Thread(() -> {
            for (int n = 0; n < 10000; ++n) concurrent.record(n, n, -n, 0, 0, 0, 0, 0, 0, 0, 7, -1);
        });
        worker.start(); long last = 0;
        do {
            String text = concurrent.drain("test", "test");
            for (String row : text.substring(text.indexOf('[') + 1, text.length() - 1).split(";")) {
                if (row.isEmpty()) continue;
                String[] parts = row.split(",");
                long id = Long.parseLong(parts[0]);
                if (parts.length != 14 || id <= last || Long.parseLong(parts[2]) != -Long.parseLong(parts[3]))
                    throw new AssertionError("torn or repeated record");
                last = id;
            }
        } while (worker.isAlive());
        worker.join();
    }
}
