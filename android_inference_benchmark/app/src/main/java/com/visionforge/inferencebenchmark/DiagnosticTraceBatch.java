package com.visionforge.inferencebenchmark;

import java.util.function.BiConsumer;

/** Chunks independent numeric trace arrays at complete records, retaining loss metadata. */
final class DiagnosticTraceBatch {
    static final int MAX_RECORD_PAYLOAD = 24000;
    static int write(String report, BiConsumer<String, String> sink) {
        int arrays = 0, scan = 0;
        // Metadata may occur between arrays from independently drained ledgers.
        // Retain every loss counter, not only the prefix of the first array.
        StringBuilder meta = new StringBuilder();
        int at=0;
        while(at<report.length()) {
            int marker=report.indexOf("=[",at);
            if(marker<0){meta.append(report,at,report.length());break;}
            int nameStart=report.lastIndexOf(' ',marker)+1;
            meta.append(report,at,Math.max(at,nameStart));
            int end=report.indexOf(']',marker+2);
            if(end<0)break;
            at=end+1;
        }
        String metadata=meta.toString().trim()+" ";
        while (scan < report.length()) {
            int marker = report.indexOf("=[", scan);
            if (marker < 0) break;
            int nameStart = report.lastIndexOf(' ', marker) + 1;
            String name = report.substring(nameStart, marker);
            int end = report.indexOf(']', marker + 2);
            if (end < 0) { sink.accept("runtime_trace_format_error", "trace=" + name + " reason=missing_end"); break; }
            int begin = marker + 2;
            while (begin < end) {
                int stop = Math.min(begin + MAX_RECORD_PAYLOAD, end);
                if (stop < end) stop = report.lastIndexOf(';', stop);
                if (stop <= begin) { sink.accept("runtime_trace_format_error", "trace=" + name + " reason=oversized_record"); break; }
                sink.accept("runtime_" + name, metadata + name + "=[" + report.substring(begin, stop) + "]");
                begin = stop + 1;
            }
            ++arrays; scan = end + 1;
        }
        // Loss counters also survive an empty batch.
        sink.accept("runtime_trace_batch", metadata.trim() + " arrays=" + arrays);
        return arrays;
    }
}
