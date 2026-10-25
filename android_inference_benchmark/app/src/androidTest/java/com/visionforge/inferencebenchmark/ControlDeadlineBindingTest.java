package com.visionforge.inferencebenchmark;

import android.app.Application;
import android.app.Instrumentation;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import java.io.File;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

/** Test-only release callback binding; never opens USB/HID or starts inference. */
final class ControlDeadlineBindingTest {
    static void run(Instrumentation instrumentation) throws Exception {
        Context context=instrumentation.getTargetContext();
        String process=Application.getProcessName();
        JSONObject result=new JSONObject()
                .put("scope","release_native_callback_resolution_and_literal_static_invocation_with_fake_sink_no_device_IO")
                .put("started_unix_ms",System.currentTimeMillis())
                .put("process",process)
                .put("process_id",android.os.Process.myPid())
                .put("target_package",context.getPackageName())
                .put("app_version",context.getPackageManager().getPackageInfo(context.getPackageName(),0).versionName)
                .put("build_type",BuildConfig.BUILD_TYPE)
                .put("build_debug",BuildConfig.DEBUG)
                .put("target_debuggable",(context.getApplicationInfo().flags&ApplicationInfo.FLAG_DEBUGGABLE)!=0)
                .put("physical_USB_HID_opened",false)
                .put("production_JNI_test_hook_added",false)
                .put("minification_claim","not_claimed; current project release uses minifyEnabled=false; independently verify any future minified build")
                .put("offer_callback_invocation","reflection on the production Class after the real native binder resolves its literal method name and descriptor")
                .put("limitations",new JSONArray()
                        .put("No native detection frame is published, so this does not measure a native-producer-to-offer CallStaticBooleanMethod transaction.")
                        .put("The fake sink cannot write USB/HID; physical motion, firmware execution and game consumption are not measured.")
                        .put("Unmatched positive ticket values simulate old callbacks with no ticket pending; concurrent old/new in-flight isolation is covered by the desktop bridge harness."));
        File destination=new File(context.getExternalFilesDir(null),"control-deadline-binding-test.json");
        FakeSink fake=new FakeSink();
        boolean nativeLoaded=false,nativeTouched=false,fakeRegistered=false;
        Throwable failure=null;
        try {
            // Keep this test out of the normal foreground runtime. A test APK
            // built with -PheadlessBenchmark targets this isolated process.
            require((context.getPackageName()+":benchmark").equals(process),"Requires isolated :benchmark process");
            require("release".equals(BuildConfig.BUILD_TYPE) && !BuildConfig.DEBUG,"Requires release target classes");
            require((context.getApplicationInfo().flags&ApplicationInfo.FLAG_DEBUGGABLE)==0,"Target APK is debuggable");
            String before=QnnHtpBridge.getNativeH264DecoderReport();nativeLoaded=true;
            require(!QnnHtpBridge.isNativeVideoReceiverRunning(),"Video receiver is still active");
            require(token(before,"configured")==0,"Decoder is still configured");
            requireClosed(before);
            require(token(before,"native_move_completion_pending")==0,"A real native ticket is pending");
            result.put("native_report_before",before);

            ControlOutputMoveDispatcher.registerSink(fake);fakeRegistered=true;
            nativeTouched=true;
            require(QnnHtpBridge.bindNativeMakcuMoveBridge(ControlOutputMoveDispatcher.class),"Native could not resolve the production callback targets");
            String bound=QnnHtpBridge.getNativeH264DecoderReport();
            require(token(bound,"java_bridge_ready")==1,"Native bridge did not become bound");
            requireClosed(bound);
            result.put("jni_binding_resolved",true)
                    .put("runtime_dispatcher_class",ControlOutputMoveDispatcher.class.getName());

            Method offer=ControlOutputMoveDispatcher.class.getDeclaredMethod(
                    "offerNativeMoveBeforeDeadline",int.class,int.class,long.class,long.class);
            require(Modifier.isStatic(offer.getModifiers()) && offer.getReturnType()==boolean.class,"Callback descriptor differs from JNI (IIJJ)Z");
            offer.setAccessible(true);
            result.put("literal_callback_name",offer.getName());
            long futureUs=System.nanoTime()/1000L+1_000_000L;
            require(invoke(offer,7,-3,41L,futureUs),"Valid future offer was not handled");
            require(fake.calls==1 && fake.x==7 && fake.y==-3 && fake.ticket==41L,"Fake sink captured a different command");
            require(fake.deadlineNanos==futureUs*1000L && !fake.expired,"Absolute microseconds were rebased or converted incorrectly");
            result.put("future_offer",new JSONObject().put("native_deadline_us",futureUs)
                    .put("captured_deadline_ns",fake.deadlineNanos).put("exact_unit_conversion",true));

            long expiredUs=Math.max(1L,System.nanoTime()/1000L-5_000L);
            require(invoke(offer,-5,2,42L,expiredUs),"Expired valid offer became boolean transport failure");
            require(fake.calls==2 && fake.ticket==42L && fake.expired,"Expired offer did not reach explicit fake discard handling");
            require(fake.deadlineNanos==expiredUs*1000L,"Expired absolute deadline was refreshed");
            result.put("expired_offer",new JSONObject().put("handled",true).put("expired_at_sink",true)
                    .put("native_deadline_us",expiredUs).put("captured_deadline_ns",fake.deadlineNanos));

            require(!invoke(offer,1,0,43L,0L),"Zero absolute deadline accepted");
            require(!invoke(offer,1,0,43L,-1L),"Negative absolute deadline accepted");
            require(!invoke(offer,1,0,43L,Long.MAX_VALUE),"Absolute unit conversion overflow accepted");
            require(!invoke(offer,1,0,0L,futureUs),"Zero ticket accepted");
            require(fake.calls==2 && fake.relativeCalls==0,"Invalid or relative offers reached fake delivery");
            result.put("literal_callback_invoked",true).put("relative_offer_calls",fake.relativeCalls)
                    .put("invalid_offers_rejected",4);

            // Verify a real native-to-Java callback through the bound class,
            // while the fake endpoint remains unable to touch any device.
            int closedBefore=fake.failClosedCalls;
            QnnHtpBridge.failClosedNativeMakcuOutput();
            require(fake.failClosedCalls==closedBefore+1,"Bound native-to-Java fail-closed callback was not invoked");
            result.put("native_to_java_fail_closed_callback_invoked",true);
            String priorTickets=QnnHtpBridge.getNativeH264DecoderReport();requireClosed(priorTickets);
            long staleBefore=token(priorTickets,"stale_device_feedback");
            long discardsBefore=token(priorTickets,"native_pre_send_discards");
            long[] tickets={0L,-1L,Long.MAX_VALUE,41L,41L,42L};
            JSONArray probes=new JSONArray();
            for(long ticket:tickets){
                // These execute the new real JNI export. They never match an
                // in-flight ticket in this deliberately idle process.
                QnnHtpBridge.reportNativeMoveDiscardedBeforeSend(ticket);
                String state=QnnHtpBridge.getNativeH264DecoderReport();
                requireClosed(state);require(token(state,"native_move_completion_pending")==0,"Unmatched discard created a pending move");
                probes.put(new JSONObject().put("ticket",Long.toString(ticket)).put("output_still_disabled",true));
            }
            String afterTickets=QnnHtpBridge.getNativeH264DecoderReport();
            require(token(afterTickets,"stale_device_feedback")==staleBefore+tickets.length,"Unmatched tickets were not individually ignored");
            require(token(afterTickets,"native_pre_send_discards")==discardsBefore,"Unmatched tickets were treated as current discards");
            result.put("discard_JNI_symbol_callable",true).put("unmatched_ticket_probes",probes)
                    .put("native_report_after_probes",afterTickets).put("passed",true);
        }catch(Throwable caught){
            failure=caught;result.put("passed",false).put("failure",android.util.Log.getStackTraceString(caught));
        }finally{
            try {
                if(nativeTouched)QnnHtpBridge.failClosedNativeMakcuOutput();
            }catch(Throwable cleanupFailure){
                result.put("native_cleanup_failure",android.util.Log.getStackTraceString(cleanupFailure));
                result.put("passed",false);if(failure==null)failure=cleanupFailure;
            }finally{
                // Compare-and-set prevents a stale test cleanup from erasing
                // a different sink if another owner unexpectedly registered.
                if(fakeRegistered)ControlOutputMoveDispatcher.clearSink(fake);
            }
            try {
                if(fakeRegistered){
                    int capturedBeforeCleanupProbe=fake.calls;
                    require(!ControlOutputMoveDispatcher.offerNativeMoveBeforeDeadline(1,0,99L,System.nanoTime()/1000L+100000L),"Fake sink was retained after cleanup");
                    require(fake.calls==capturedBeforeCleanupProbe,"Cleanup probe reached the removed fake sink");
                    result.put("cleanup_fake_removed",true);
                }
                if(nativeLoaded){
                    String state=QnnHtpBridge.getNativeH264DecoderReport();requireClosed(state);
                    require(token(state,"native_move_completion_pending")==0,"A native ticket remained pending after cleanup");
                    // The native binder keeps the production dispatcher Class,
                    // not its active sink. bind(null) is a rejected no-op, not
                    // an unbind operation. Remove our Java sink by CAS and
                    // leave the normal process-lifetime Class binding intact.
                    result.put("native_report_after_cleanup",state)
                            .put("cleanup_output_disabled",true)
                            .put("cleanup_no_pending_native_ticket",true)
                            .put("native_dispatcher_class_binding_retained",token(state,"java_bridge_ready")==1)
                            .put("cleanup_binding_contract","The native production dispatcher Class may remain bound for the process lifetime; the fake sink is CAS-removed, new offers are rejected, output is disabled and no native ticket is pending. bind(null) is not an unbind API.");
                }
            }catch(Throwable cleanupFailure){
                result.put("binding_cleanup_failure",android.util.Log.getStackTraceString(cleanupFailure));
                result.put("passed",false);if(failure==null)failure=cleanupFailure;
            }
            result.put("finished_unix_ms",System.currentTimeMillis());
            Files.write(destination.toPath(),result.toString(2).getBytes(StandardCharsets.UTF_8));
        }
        if(failure!=null)throw new AssertionError("Control deadline binding test failed; inspect "+destination,failure);
    }

    private static boolean invoke(Method method,int x,int y,long ticket,long absoluteMicros) throws Exception {
        return Boolean.TRUE.equals(method.invoke(null,x,y,ticket,absoluteMicros));
    }
    private static long token(String report,String name){
        Matcher matcher=Pattern.compile("(?:^|[\\s{])"+Pattern.quote(name)+"=(-?\\d+)\\b").matcher(report);
        require(matcher.find(),"Missing native report token "+name);return Long.parseLong(matcher.group(1));
    }
    private static void requireClosed(String report){
        require(token(report,"output_requested")==0 && token(report,"output_enabled")==0,"Test unexpectedly enabled native output");
    }
    private static void require(boolean condition,String message){if(!condition)throw new AssertionError(message);}

    private static final class FakeSink implements ControlOutputMoveSink {
        int calls,relativeCalls,failClosedCalls,x,y;
        long ticket,deadlineNanos;
        boolean expired;
        public boolean offerMoveFromNative(int x,int y,long ticket,long budgetUs){++relativeCalls;return false;}
        public boolean offerMoveBeforeDeadline(int dx,int dy,long nextTicket,long absoluteNanos){
            ++calls;x=dx;y=dy;ticket=nextTicket;deadlineNanos=absoluteNanos;
            expired=System.nanoTime()>=absoluteNanos;
            return true; // Handled/captured; an expired task is discarded, with no device I/O.
        }
        public boolean suspendMoveDeliveryForNativeRecovery(long generation){return true;}
        public boolean resumeMoveDeliveryAfterNativeRecovery(long generation){return true;}
        public boolean failClosedNativeMoveDelivery(){++failClosedCalls;return true;}
    }
}
