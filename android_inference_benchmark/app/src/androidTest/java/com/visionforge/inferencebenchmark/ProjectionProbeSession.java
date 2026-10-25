package com.visionforge.inferencebenchmark;

import android.app.Application;
import android.content.Context;
import java.io.File;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

/** Android-test-only real native control path with a permanently device-free sink. */
final class ProjectionProbeSession {
    private final FakeSink fake=new FakeSink();
    private final int expectedMode;
    private final File traceFile,reportFile;
    private final JSONObject report=new JSONObject();
    private final Map<Long,long[]> offers=new HashMap<>(),dispatches=new HashMap<>(),projections=new HashMap<>();
    private boolean registered,nativeTouched,opened,closed;
    private long ackBefore,hidBefore,discardBefore,staleBefore,traceSamples;

    ProjectionProbeSession(Context context,File directory,int expectedMode) throws Exception {
        require(expectedMode>=0&&expectedMode<=2,"Projection expected mode must be0,1 or2");
        this.expectedMode=expectedMode;
        require((context.getPackageName()+":benchmark").equals(Application.getProcessName()),"Projection probe requires isolated :benchmark process");
        traceFile=new File(directory,"projection-probe-trace.jsonl");
        reportFile=new File(directory,"projection-probe.json");
        Files.write(traceFile.toPath(),new byte[0]);
        report.put("scope","test_only_real_NPU_projection_Core_JNI_dispatcher_with_unsent_discard_fake_no_USB_HID")
                .put("started_unix_ms",System.currentTimeMillis()).put("process",Application.getProcessName())
                .put("process_id",android.os.Process.myPid()).put("build_type",BuildConfig.BUILD_TYPE)
                .put("physical_USB_HID_opened_by_session",false).put("production_JNI_test_hook_added",false)
                .put("MobileControlRuntime_created_by_session",false)
                .put("expected_mode",expectedMode).put("profile_persisted",false).put("model","ow_m_640").put("geometric_head_ratio",0.18)
                .put("trace_file",traceFile.getName())
                .put("limitations",new JSONArray()
                        .put("Every fake offer reports a real native discard-before-send. No execution ACK or physical input occurs.")
                        .put("The prerecorded game is not affected by fake output; this is a native timing/receipt/deadline probe, not a physical closed-loop score.")
                        .put("Source/control identities come from production projection and dispatch traces, joined by control sequence and native ticket.")
                        .put("This checks the current release build; it adds no R8 or production-hook claim."));
    }

    void open(MobileModelCatalog.Profile model) throws Exception {
        require(!opened&&!registered,"Probe session was already opened");
        require("ow_m_640".equals(model.token)&&model.inputWidth==640&&model.inputHeight==640,"Probe requires original640 model");
        require(model.bodyClassIdFor(MobileAimTarget.HEAD)==0&&model.classIdFor(MobileAimTarget.HEAD)==0
                &&model.usesGeometricHead(MobileAimTarget.HEAD)&&!model.usesHeadBox(MobileAimTarget.HEAD)
                &&model.targetYRatio(MobileAimTarget.HEAD)==.18f,"Probe must preserve original body/18% policy");
        require(currentSink()==null,"Another sink already owns this isolated process; refusing to replace it");
        String before=QnnHtpBridge.getNativeH264DecoderReport();
        require(token(before,"output_requested")==0&&token(before,"output_enabled")==0,"Native output was already requested");
        require(token(before,"native_move_completion_pending")==0,"A native ticket was already pending");
        ControlOutputMoveDispatcher.registerSink(fake);registered=true;nativeTouched=true;
        require(QnnHtpBridge.bindNativeMakcuMoveBridge(ControlOutputMoveDispatcher.class),"Production dispatcher binding failed");
        // Capture counters while output is still closed. A LAN sender may
        // already have a decoded frame ready when configure(true) returns.
        String baseline=QnnHtpBridge.getNativeH264DecoderReport();
        ackBefore=token(baseline,"native_device_ack_completions");hidBefore=token(baseline,"native_bluetooth_hid_api_acceptances");
        discardBefore=token(baseline,"native_pre_send_discards");staleBefore=token(baseline,"stale_device_feedback");
        fake.authorize();
        require(configure(true),"Pure native probe profile configuration failed");
        String active=QnnHtpBridge.getNativeH264DecoderReport();
        require(token(active,"java_bridge_ready")==1&&token(active,"output_requested")==1,"Native probe request did not become active");
        opened=true;report.put("native_report_after_open",active).put("native_profile_configured",true);
    }

    // Exact production JNI configuration only. No preferences, runtime owner,
    // transport controller, USB permission, Bluetooth or hardware discovery.
    private static boolean configure(boolean requested) {
        return QnnHtpBridge.configureNativeMakcuControl(.95f,.5f,127,45,requested,
                false,0L,1f,1f,1f,480f,.42f,.75f,.20f,1f,2,24f,new float[0],
                "ow_m_640",0,-1,0,false,true,.18f);
    }

    void sample() throws Exception {
        if(!nativeTouched||closed)return;
        String controls=QnnHtpBridge.getNativeControlTraceReport();
        String pipeline=QnnHtpBridge.getNativePipelineTraceReport();
        for(String row:array(controls,"dispatch_trace_v1")){
            String[] fields=row.split(",");require(fields.length==10,"Unexpected native dispatch schema");
            long[] values=longs(fields);
            if(values[2]>0)dispatches.put(values[2],values);
        }
        for(String row:array(pipeline,"projection_trace_v1")){
            String[] fields=row.split(",");require(fields.length==32,"Unexpected native projection schema");
            long[] values=longs(fields);
            // Actual active projection consumed by Core, not just math success.
            if(expectedMode!=2&&values[2]==1&&values[13]>0)
                require(values[13]==values[4]&&values[15]==values[6],"Off/shadow control used projectedB instead of sourceA");
            if(values[2]==1&&values[25]==8){
                require(expectedMode==2&&values[1]==2,"Off/shadow mode published a projected observation");
                require(values[6]>0&&values[9]>values[6],"Projection did not advance real source pixels");
                require(values[13]==values[7]&&values[15]==values[9],"Control identity differs from projected pixels");
                require(values[17]>values[6]&&values[17]<=values[6]+50_000L,"Source decision deadline was extended");
                projections.put(values[13],values);
            }
        }
        FakeSink.Drain drain=fake.drain();JSONArray offerRows=new JSONArray();
        for(int p=0;p<drain.values.length;p+=FakeSink.WIDTH){
            long[] row=new long[FakeSink.WIDTH];System.arraycopy(drain.values,p,row,0,row.length);
            require(offers.put(row[0],row)==null,"Native ticket was offered twice");
            offerRows.put(new JSONArray().put(row[0]).put(row[1]).put(row[2]).put(row[3]).put(row[4]));
        }
        require(drain.lost==0,"Fake offer recorder overflowed; coverage incomplete");
        JSONObject snapshot=new JSONObject().put("sample",++traceSamples).put("unix_ms",System.currentTimeMillis())
                .put("monotonic_ns",System.nanoTime()).put("control_trace",controls).put("pipeline_trace",pipeline)
                .put("fake_offer_schema","ticket,dx,dy,offered_monotonic_ns,absolute_deadline_monotonic_ns")
                .put("fake_offers",offerRows);
        Files.write(traceFile.toPath(),(snapshot+"\n").getBytes(StandardCharsets.UTF_8),StandardOpenOption.APPEND);
    }

    void finishAndClose(boolean requireActiveEvidence) throws Exception {
        if(closed)return;Throwable failure=null;String activeState="",finalState="";
        try {
            if(nativeTouched){sample();activeState=QnnHtpBridge.getNativeH264DecoderReport();report.put("native_report_before_cleanup",activeState);}
        }catch(Throwable error){failure=error;}
        try {
            if(nativeTouched){
                require(configure(false),"Could not clear native user request");
                // configure(false) waits on the existing publish boundary;
                // drain the final in-flight dispatch after it has quiesced.
                sample();finalState=QnnHtpBridge.getNativeH264DecoderReport();
                report.put("native_report_after_request_closed",finalState);
            }
        }catch(Throwable error){if(failure==null)failure=error;else failure.addSuppressed(error);}
        finally {
            try {if(nativeTouched)QnnHtpBridge.failClosedNativeMakcuOutput();}
            catch(Throwable error){if(failure==null)failure=error;else failure.addSuppressed(error);}
            fake.revoke();
            if(registered)ControlOutputMoveDispatcher.clearSink(fake);
        }
        try {
            require(!registered||currentSink()==null,"Fake sink CAS cleanup did not remove its registration");
            if(nativeTouched){
                String cleanup=QnnHtpBridge.getNativeH264DecoderReport();
                require(token(cleanup,"output_requested")==0&&token(cleanup,"output_enabled")==0,"Native output remained requested after cleanup");
                require(token(cleanup,"native_move_completion_pending")==0,"Native ticket remained pending after cleanup");
                report.put("native_report_after_cleanup",cleanup).put("cleanup_output_disabled",true)
                        .put("cleanup_fake_removed",true).put("native_dispatcher_class_binding_retained",token(cleanup,"java_bridge_ready")==1);
            }
            if(requireActiveEvidence){
                require(opened,"Probe never opened");
                require(token(finalState,"pixel_projection_mode")==expectedMode,"Native projection mode differed from expected mode");
                if(expectedMode==2)require(token(finalState,"pixel_projection_control_publications")>0,"No active projected observation was consumed by Core");
                else require(token(finalState,"pixel_projection_control_publications")==0&&projections.isEmpty(),"Off/shadow mode emitted projected control");
                require(token(activeState,"output_requested")==1&&token(activeState,"output_enabled")==1,"Unsent discards closed the active output request");
                require(fake.total()>0&&fake.relativeCalls()==0,"No absolute native offers, or legacy relative offers occurred");
                require(token(finalState,"native_device_ack_completions")==ackBefore&&token(finalState,"native_bluetooth_hid_api_acceptances")==hidBefore,"Fake session unexpectedly acknowledged device execution");
                require(token(finalState,"native_pre_send_discards")-discardBefore>=fake.total(),"Fake offers were not matched native unsent discards");
                require(token(finalState,"stale_device_feedback")==staleBefore,"A fake discard had a stale native ticket");
                verifyDeadlines();
            }
        }catch(Throwable error){if(failure==null)failure=error;else failure.addSuppressed(error);}
        closed=true;
        report.put("fake",fake.summary()).put("trace_samples",traceSamples).put("retained_offer_count",offers.size())
                .put("active_projection_sequence_count",projections.size());
        report.put("finished_unix_ms",System.currentTimeMillis()).put("active_evidence_required",requireActiveEvidence)
                .put("passed",failure==null&&requireActiveEvidence);
        if(failure!=null)report.put("failure",android.util.Log.getStackTraceString(failure));
        Files.write(reportFile.toPath(),report.toString(2).getBytes(StandardCharsets.UTF_8));
        if(failure!=null)throw new AssertionError("Projection fake probe failed; inspect "+reportFile,failure);
    }

    private void verifyDeadlines() throws Exception {
        long checked=0,projected=0;JSONArray examples=new JSONArray();
        for(Map.Entry<Long,long[]> entry:offers.entrySet()){
            long[] offer=entry.getValue(),dispatch=dispatches.get(entry.getKey());
            require(dispatch!=null&&dispatch[8]==1,"Fake offer lacks accepted real native dispatch trace");
            require(offer[1]==dispatch[6]&&offer[2]==dispatch[7],"Dispatcher changed move coordinates");
            require(offer[4]%1000L==0&&offer[4]/1000L==dispatch[3]+dispatch[5],"Absolute native deadline was rebased across JNI");
            require(offer[4]-offer[3]<=50_000_000L,"Fake offer exceeded the original50ms visual horizon");
            ++checked;long[] projection=projections.get(dispatch[1]);
            if(projection!=null){
                require(offer[4]/1000L<=projection[17],"Projected control renewed sourceA deadline");++projected;
                if(examples.length()<32)examples.put(new JSONObject().put("ticket",entry.getKey())
                        .put("source_generation",projection[3]).put("source_sequence",projection[4])
                        .put("source_wire",projection[5]).put("source_observed_us",projection[6])
                        .put("pixel_sequence",projection[7]).put("pixel_wire",projection[8]).put("pixel_observed_us",projection[9])
                        .put("control_sequence",projection[13]).put("control_wire",projection[14]).put("control_observed_us",projection[15])
                        .put("original_source_deadline_us",projection[17]).put("actual_offer_deadline_us",offer[4]/1000L));
            }
        }
        require(checked>0,"No real Core→JNI→fake ticket/deadline join was observed");
        if(expectedMode==2)require(projected>0,"No projected Core→JNI→fake ticket/deadline join was observed");
        else require(projected==0,"Off/shadow fake received a projected ticket");
        report.put("absolute_deadline_checks",checked).put("projected_original_source_deadline_checks",projected)
                .put("joined_source_control_examples",examples)
                .put("source_control_metadata_schema","projection_trace_v1: sourceA[4..6], pixelB[7..9], control[13..15], previousL[16], Adeadline[17]");
    }

    JSONObject report(){return report;}
    private static Object currentSink() throws Exception {
        Field field=ControlOutputMoveDispatcher.class.getDeclaredField("sink");field.setAccessible(true);
        return ((AtomicReference<?>)field.get(null)).get();
    }
    private static String[] array(String text,String key){
        Matcher m=Pattern.compile("\\b"+Pattern.quote(key)+"=\\[([^\\]]*)\\]").matcher(text);
        return m.find()&&!m.group(1).isEmpty()?m.group(1).split(";"):new String[0];
    }
    private static long[] longs(String[] fields){long[] r=new long[fields.length];for(int i=0;i<fields.length;++i)r[i]=Long.parseLong(fields[i]);return r;}
    private static long token(String text,String name){Matcher m=Pattern.compile("(?:^|[\\s{])"+Pattern.quote(name)+"=(-?\\d+)\\b").matcher(text);require(m.find(),"Missing native token "+name);return Long.parseLong(m.group(1));}
    private static void require(boolean condition,String message){if(!condition)throw new AssertionError(message);}

    private static final class FakeSink implements ControlOutputMoveSink {
        static final int CAPACITY=4096,WIDTH=5;
        private final long[] ring=new long[CAPACITY*WIDTH];
        private long total,drained,expired,relative,suspends,resumes,failClosed;
        private boolean allowed;
        synchronized void authorize(){allowed=true;}
        synchronized void revoke(){allowed=false;}
        synchronized long total(){return total;}
        synchronized long relativeCalls(){return relative;}
        public synchronized boolean offerMoveBeforeDeadline(int x,int y,long ticket,long deadline){
            if(!allowed||ticket<=0||deadline<=0)return false;
            long now=System.nanoTime();int p=(int)(total++%CAPACITY)*WIDTH;
            ring[p]=ticket;ring[p+1]=x;ring[p+2]=y;ring[p+3]=now;ring[p+4]=deadline;
            if(now>=deadline)++expired;
            // This is the real JNI unsent-discard path, never an ACK/API success.
            QnnHtpBridge.reportNativeMoveDiscardedBeforeSend(ticket);return true;
        }
        public synchronized boolean offerMoveFromNative(int x,int y,long ticket,long relativeBudgetUs){++relative;return false;}
        public synchronized boolean suspendMoveDeliveryForNativeRecovery(long generation){++suspends;return true;}
        public synchronized boolean resumeMoveDeliveryAfterNativeRecovery(long generation){++resumes;return allowed;}
        public synchronized boolean failClosedNativeMoveDelivery(){++failClosed;allowed=false;return true;}
        synchronized Drain drain(){
            long first=Math.max(drained,Math.max(0,total-CAPACITY)),lost=first-drained;
            long[] copy=new long[(int)(total-first)*WIDTH];
            for(long id=first;id<total;++id)System.arraycopy(ring,(int)(id%CAPACITY)*WIDTH,copy,(int)(id-first)*WIDTH,WIDTH);
            drained=total;return new Drain(copy,lost);
        }
        synchronized JSONObject summary() throws Exception {return new JSONObject().put("offers",total).put("expired_at_fake",expired)
                .put("relative_offers",relative).put("recovery_suspends",suspends).put("recovery_resumes",resumes)
                .put("fail_closed_callbacks",failClosed).put("physical_IO_calls",0).put("semantics","all_valid_offers_discarded_before_send");}
        private static final class Drain {final long[] values;final long lost;Drain(long[] values,long lost){this.values=values;this.lost=lost;}}
    }
}
