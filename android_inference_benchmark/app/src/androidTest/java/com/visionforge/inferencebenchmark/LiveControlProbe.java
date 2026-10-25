package com.visionforge.inferencebenchmark;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.content.ComponentName;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.SystemClock;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import com.visionforge.inferencebenchmark.runtime.MobileRuntimeBinding;
import org.json.JSONArray;
import org.json.JSONObject;

/** Bounded training-room disturbances while the real production loop is live. */
final class LiveControlProbe {
    static void run(Instrumentation instrumentation, boolean serviceOnly) throws Exception {
        Context context=instrumentation.getTargetContext();
        JSONObject report=new JSONObject().put("scope","live_production_loop_bounded_external_camera_offsets_no_buttons")
                .put("process_id",android.os.Process.myPid()).put("started_unix_ms",System.currentTimeMillis());
        JSONArray phases=new JSONArray();report.put("phases",phases);
        final MobileRuntimeBinding[] binding={null};
        CountDownLatch connected=new CountDownLatch(1);
        ServiceConnection connection=new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name,IBinder binder) {
                binding[0]=(MobileRuntimeBinding)binder;connected.countDown();
            }
            @Override public void onServiceDisconnected(ComponentName name) {binding[0]=null;}
        };
        boolean bound=false;
        if(serviceOnly) {
            // Test-owned foreground service lifetime. No lock-screen input or
            // credential operation; this does not validate the Activity UI.
            MobileRuntimeService.ensureRunning(context);
            bound=context.bindService(new Intent(context,MobileRuntimeService.class),connection,Context.BIND_AUTO_CREATE);
            if(!bound || !connected.await(10,TimeUnit.SECONDS))throw new AssertionError("Service bind failed");
            binding[0].setActivityForeground(true);
            report.put("ui_scope","instrumentation_owned_service_not_Activity_validation");
        } else {
            Activity activity=instrumentation.startActivitySync(new Intent(context,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            instrumentation.runOnMainSync(()->activity.getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
            report.put("ui_scope","MainActivity_foreground");
        }
        MobileControlRuntime runtime=MobileControlRuntime.get(context);
        try {
            if(runtime.activeOutputRoute()!=ControlOutputRoute.MAKCU_USB)throw new AssertionError("Requires existing MAKCU route");
            MakcuSerialController controller=(MakcuSerialController)field(runtime,"makcu");
            long deadline=SystemClock.elapsedRealtime()+30000;
            while(SystemClock.elapsedRealtime()<deadline) {
                if(runtime.isOutputEnabled() && controller.isReady())break;
                SystemClock.sleep(100);
            }
            if(!runtime.isOutputEnabled() || !controller.isReady())throw new AssertionError("Output unavailable: "+controller.report());
            report.put("ready_unix_ms",System.currentTimeMillis());
            Files.write(new File(context.getExternalFilesDir(null),"live-control-probe-ready.json").toPath(),
                    report.toString().getBytes(StandardCharsets.UTF_8));
            SystemClock.sleep(2000);
            phase(phases,"quiet_before",8000);
            final int[][] steps={{48,0},{-48,0},{0,48},{0,-48},{32,32},{-32,-32},{-32,32},{32,-32}};
            for(int i=0;i<steps.length;i++) {
                JSONObject phase=new JSONObject().put("name","step_"+i).put("begin_us",System.nanoTime()/1000);
                JSONArray sent=new JSONArray();phase.put("commands",sent);phases.put(phase);
                send(controller,steps[i][0],steps[i][1],sent);
                SystemClock.sleep(900);
                phase.put("end_us",System.nanoTime()/1000);
            }
            for(int axis=0;axis<2;axis++)for(int direction:new int[]{1,-1}) {
                JSONObject phase=new JSONObject().put("name","ramp_"+axis+"_"+direction).put("begin_us",System.nanoTime()/1000);
                JSONArray sent=new JSONArray();phase.put("commands",sent);phases.put(phase);
                for(int i=0;i<8;i++) {
                    send(controller,axis==0?direction*4:0,axis==1?direction*4:0,sent);
                    SystemClock.sleep(15);
                }
                SystemClock.sleep(900);phase.put("end_us",System.nanoTime()/1000);
            }
            phase(phases,"quiet_after",8000);
            report.put("passed",true).put("output_enabled_at_end",runtime.isOutputEnabled());
        } catch(Throwable failure) {
            report.put("passed",false).put("failure",android.util.Log.getStackTraceString(failure));
            throw failure;
        } finally {
            report.put("finished_unix_ms",System.currentTimeMillis());
            Files.write(new File(context.getExternalFilesDir(null),"live-control-probe.json").toPath(),report.toString(2).getBytes(StandardCharsets.UTF_8));
            if(serviceOnly && binding[0]!=null)binding[0].setActivityForeground(false);
            if(bound)context.unbindService(connection);
        }
    }
    private static void phase(JSONArray phases,String name,long millis) throws Exception {
        JSONObject p=new JSONObject().put("name",name).put("begin_us",System.nanoTime()/1000);phases.put(p);
        SystemClock.sleep(millis);p.put("end_us",System.nanoTime()/1000);
    }
    private static void send(MakcuSerialController controller,int dx,int dy,JSONArray sent)throws Exception {
        if(Math.abs(dx)>48 || Math.abs(dy)>48)throw new AssertionError("Probe magnitude exceeded");
        Object lock=field(controller,"serialIoLock");
        synchronized(lock) {
            if(!controller.isReady())throw new AssertionError("Lost production USB ownership");
            Object transport=field(controller,"activeTransport");
            long generation=(Long)field(controller,"activeConnectionGeneration");
            long lease=((AtomicLong)field(controller,"responseReaderLeaseGeneration")).get();
            String command="km.move("+dx+","+dy+")\r\n";byte[] bytes=command.getBytes(StandardCharsets.US_ASCII);
            Method begin=method(controller.getClass(),"beginExpectedResponse",String.class,long.class,long.class);
            Method write=method(transport.getClass(),"write",byte[].class,int.class);
            Method await=method(controller.getClass(),"awaitExecutedAcknowledgement",transport.getClass(),String.class,long.class,long.class,long.class);
            long mono=System.nanoTime()/1000,started=SystemClock.elapsedRealtimeNanos();
            begin.invoke(controller,command,generation,lease);
            int written=(Integer)write.invoke(transport,bytes,10);
            if(written!=bytes.length)throw new AssertionError("Probe partial write");
            long echo=(Long)await.invoke(controller,transport,command,started,generation,lease);
            sent.put(new JSONObject().put("at_us",mono).put("dx",dx).put("dy",dy).put("echo_us",echo));
            if(echo<0)throw new AssertionError("Probe echo missing");
        }
    }
    private static Object field(Object owner,String name)throws Exception {Field f=owner.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(owner);}
    private static Method method(Class<?> type,String name,Class<?>...p)throws Exception {Method m=type.getDeclaredMethod(name,p);m.setAccessible(true);return m;}
}
