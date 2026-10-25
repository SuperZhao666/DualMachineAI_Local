package com.visionforge.inferencebenchmark;

import android.app.Instrumentation;
import android.content.Context;
import android.os.SystemClock;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONArray;
import org.json.JSONObject;

/** Actual USB zero-movement echo, with an intentionally delayed parser handoff. */
final class MakcuAckDeadlineTest {
    static void run(Instrumentation instrumentation) throws Exception {
        Context context=instrumentation.getTargetContext();
        JSONObject result=new JSONObject().put("scope","zero_movement_post_write_echo_after_visual_deadline")
                .put("started_unix_ms",System.currentTimeMillis());
        JSONArray trials=new JSONArray();result.put("trials",trials);
        MakcuSerialController controller=new MakcuSerialController(context,new MobileEventLogger(context));
        try {
            controller.connectFirstSupportedDevice();
            long until=SystemClock.elapsedRealtime()+15000;
            while(!controller.isReady() && SystemClock.elapsedRealtime()<until)SystemClock.sleep(20);
            if(!controller.isReady())throw new AssertionError(controller.report());
            Object transport=field(controller,"activeTransport");
            Object ioLock=field(controller,"serialIoLock");
            Object responseLock=field(controller,"responseLock");
            long generation=(Long)field(controller,"activeConnectionGeneration");
            long lease=((AtomicLong)field(controller,"responseReaderLeaseGeneration")).get();
            Method begin=method(controller.getClass(),"beginExpectedResponse",String.class,long.class,long.class);
            Method wait=method(controller.getClass(),"awaitExecutedAcknowledgement",transport.getClass(),String.class,long.class,long.class,long.class,long.class);
            Method write=method(transport.getClass(),"write",byte[].class,int.class);
            String command="km.move(0,0)\r\n";byte[] bytes=command.getBytes(StandardCharsets.US_ASCII);
            int accepted=0;
            for(int i=0;i<6;i++) {
                long started,finished,latency;int written;
                synchronized(ioLock) {
                    synchronized(responseLock) {
                        started=SystemClock.elapsedRealtimeNanos();
                        begin.invoke(controller,command,generation,lease);
                        written=(Integer)write.invoke(transport,bytes,10);
                        if(written!=bytes.length)throw new AssertionError("Incomplete zero move");
                        // Echo bytes may already be in the kernel, but cannot
                        // be parsed until this monitor is released by wait().
                        SystemClock.sleep(12);
                        latency=(Long)wait.invoke(controller,transport,command,started,generation,lease,started+5_000_000L);
                        finished=SystemClock.elapsedRealtimeNanos();
                    }
                }
                if(latency>=0)accepted++;
                trials.put(new JSONObject().put("accepted",latency>=0).put("latency_us",latency)
                        .put("total_us",(finished-started)/1000).put("send_deadline_us",5000));
                SystemClock.sleep(60);
            }
            result.put("accepted",accepted).put("trials_count",6)
                    .put("reader_still_ready",controller.isReady());
            // Keeping the protocol window must not admit an arbitrarily late
            // echo, an absent response, or a different command's response.
            JSONArray negative=new JSONArray();result.put("negative",negative);
            for(int mode=0;mode<3;mode++) {
                long latency;
                synchronized(ioLock) {
                    synchronized(responseLock) {
                        long started=SystemClock.elapsedRealtimeNanos();
                        begin.invoke(controller,command,generation,lease);
                        if(mode!=1) {
                            byte[] request=mode==2?"km.version()\r\n".getBytes(StandardCharsets.US_ASCII):bytes;
                            if((Integer)write.invoke(transport,request,10)!=request.length)throw new AssertionError("Negative probe partial write");
                        }
                        if(mode==0)SystemClock.sleep(70);
                        latency=(Long)wait.invoke(controller,transport,command,started,generation,lease,started+5_000_000L);
                    }
                }
                negative.put(new JSONObject().put("case",mode).put("rejected",latency<0));
                if(latency>=0)throw new AssertionError("Invalid acknowledgement accepted");
                SystemClock.sleep(60);
            }
            result.put("controller",controller.report());
        } finally {
            controller.destroy();
            File file=new File(context.getExternalFilesDir(null),"makcu-ack-deadline-test.json");
            Files.write(file.toPath(),result.toString(2).getBytes(StandardCharsets.UTF_8));
        }
    }
    private static Object field(Object owner,String name)throws Exception {
        Field f=owner.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(owner);
    }
    private static Method method(Class<?> type,String name,Class<?>...parameters)throws Exception {
        Method m=type.getDeclaredMethod(name,parameters);m.setAccessible(true);return m;
    }
}
