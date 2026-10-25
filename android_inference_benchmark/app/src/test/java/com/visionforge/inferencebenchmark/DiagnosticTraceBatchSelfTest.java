package com.visionforge.inferencebenchmark;

import java.util.ArrayList;

final class DiagnosticTraceBatchSelfTest {
    static void run() {
        StringBuilder report = new StringBuilder("control_trace_lost=3 control_trace_v6=[");
        StringBuilder expected = new StringBuilder();
        for (int n = 0; n < 1024; ++n) {
            String row = n + "," + "-12.3456,".repeat(64) + "7";
            if (n > 0) { report.append(';'); expected.append(';'); }
            report.append(row); expected.append(row);
        }
        report.append("] dispatch_trace_v1=[9,42,100;10,43,101]");
        ArrayList<String> control = new ArrayList<>(), dispatch = new ArrayList<>();
        DiagnosticTraceBatch.write(report.toString(), (event, detail) -> {
            if (detail.length() > 24500 || !detail.contains("control_trace_lost=3")) throw new AssertionError("size/metadata");
            if (event.equals("runtime_control_trace_v6")) control.add(detail.substring(detail.indexOf('[') + 1, detail.length()-1));
            if (event.equals("runtime_dispatch_trace_v1")) dispatch.add(detail.substring(detail.indexOf('[') + 1, detail.length()-1));
        });
        if (!String.join(";", control).equals(expected.toString()) || !String.join(";", dispatch).equals("9,42,100;10,43,101"))
            throw new AssertionError("truncated, duplicated or mixed arrays");
        ArrayList<String> faults = new ArrayList<>();
        DiagnosticTraceBatch.write("control_trace_v6=[" + "1".repeat(25000) + ";2]", (event, detail) -> faults.add(event));
        if (!faults.contains("runtime_trace_format_error")) throw new AssertionError("oversize must be visible");
        DiagnosticTraceBatch.write("control_trace_v6=[]", (event, detail) -> {
            if (!event.equals("runtime_trace_batch")) throw new AssertionError("empty batch repeats data");
        });
        DiagnosticTraceBatch.write("a_lost=2 first_v1=[1,2] b_lost=7 second_v1=[3,4]",(event,detail)->{
            if(!detail.contains("a_lost=2") || !detail.contains("b_lost=7"))
                throw new AssertionError("per-ledger loss metadata disappeared");
        });
    }
}
