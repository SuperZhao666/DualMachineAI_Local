package com.visionforge.inferencebenchmark;

import android.app.Instrumentation;
import android.content.Context;
import android.os.SystemClock;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONArray;
import org.json.JSONObject;

/** Bounded capability queries; never injects movement or changes persistent settings. */
final class MakcuReadOnlyProbe {
    static void run(Instrumentation instrumentation) throws Exception {
        Context context=instrumentation.getTargetContext();
        JSONObject result=new JSONObject().put("scope","read_only_firmware_queries_no_motion")
                .put("started_unix_ms",System.currentTimeMillis());
        JSONArray queries=new JSONArray();
        result.put("queries",queries);
        MakcuSerialController controller=new MakcuSerialController(context,new MobileEventLogger(context));
        File destination=new File(context.getExternalFilesDir(null),"makcu-read-only-probe.json");
        try {
            controller.connectFirstSupportedDevice();
            long deadline=SystemClock.elapsedRealtime()+15000;
            while(!controller.isReady() && SystemClock.elapsedRealtime()<deadline) SystemClock.sleep(20);
            if(!controller.isReady()) throw new AssertionError(controller.report());
            Object target=field(controller,"activeTransport");
            Object lock=field(controller,"serialIoLock");
            Method write=target.getClass().getDeclaredMethod("write",byte[].class,int.class);
            write.setAccessible(true);
            String[] names={"text_version","text_device","text_moving","binary_firmware_version","binary_device","binary_interpolation","final_text_version"};
            byte[][] requests={ascii("km.version()\r\n"),ascii("km.device()\r\n"),ascii("km.moving()\r\n"),
                    query(0x04),query(0x02),query(0x1F),ascii("\r\nkm.version()\r\n")};
            for(int i=0;i<requests.length;i++) {
                SystemClock.sleep(50);
                long started=SystemClock.elapsedRealtimeNanos();
                int written;
                synchronized(lock) { written=(Integer)write.invoke(target,requests[i],100); }
                if(written!=requests[i].length) throw new AssertionError("Partial capability query write");
                SystemClock.sleep(180);
                long now=SystemClock.elapsedRealtimeNanos();
                queries.put(new JSONObject().put("name",names[i]).put("started_elapsed_ns",started)
                        .put("recorded_elapsed_ns",now)
                        .put("read_trace",((MakcuReadTrace)field(controller,"readTrace")).report(now)));
            }
            result.put("queries_completed",true).put("controller",controller.report());
        } catch(Throwable failure) {
            result.put("queries_completed",false).put("failure",android.util.Log.getStackTraceString(failure));
            throw failure;
        } finally {
            controller.destroy();
            result.put("finished_unix_ms",System.currentTimeMillis());
            Files.write(destination.toPath(),result.toString(2).getBytes(StandardCharsets.UTF_8));
        }
    }
    private static byte[] query(int opcode) { return new byte[]{(byte)0xDE,(byte)0xAD,0,0,(byte)opcode}; }
    private static byte[] ascii(String text) { return text.getBytes(StandardCharsets.US_ASCII); }
    private static Object field(Object owner,String name) throws Exception {
        Field value=owner.getClass().getDeclaredField(name); value.setAccessible(true); return value.get(owner);
    }
}
