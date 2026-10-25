package com.visionforge.inferencebenchmark;

import android.content.Context;
import android.os.Build;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import org.json.JSONObject;

/** Only the service diagnostic worker reads/writes this small local record. */
final class MobileResponseCalibrationStore {
    private final Context context;
    private final MobileRuntimeEventSink events;
    private String savedSignature="";
    private MobileResponseCalibration restoredCalibration;
    private JSONObject restoredRecord;
    private String incompatibleTarget="";
    private boolean incompatibleSaveReported;
    MobileResponseCalibrationStore(Context context,MobileRuntimeEventSink events) {
        this.context=context;this.events=events;
    }
    private File file(MobileControlRuntime runtime) {
        File directory=context.getExternalFilesDir(null);
        if (directory==null) directory=context.getFilesDir();
        return new File(new File(directory,"diagnostics"),
            "control-response-"+runtime.activeModel().token+"-"+runtime.activeOutputRoute().storageToken+".json");
    }
    void restore(MobileControlRuntime runtime) {
        synchronized (runtime) { restoreLocked(runtime); }
    }
    private void restoreLocked(MobileControlRuntime runtime) {
        restoredCalibration=null;
        restoredRecord=null;
        savedSignature="";
        incompatibleTarget="";
        incompatibleSaveReported=false;
        File file=file(runtime);
        boolean migrate=false;
        if (!file.isFile() && runtime.activeModel().token.equals("ow_m_640")) {
            file=new File(file.getParentFile(),"control-response-ow_m_416-"+runtime.activeOutputRoute().storageToken+".json");
            migrate=true;
        }
        if (!file.isFile()) return;
        if (file.length()>4096) {
            rejectPreservingFile(runtime,"record_too_large");return;
        }
        try {
            JSONObject j=new JSONObject(new String(Files.readAllBytes(file.toPath()),StandardCharsets.UTF_8));
            MobileResponseCalibration c=new MobileResponseCalibration(j.getString("model"),j.getString("route"),j.getString("device"),
                (float)j.getDouble("x"),(float)j.getDouble("y"),j.getLong("delay_us"),
                (float)j.getDouble("quality_x"),(float)j.getDouble("quality_y"),
                j.has("calibrated_unix_ms")?j.getLong("calibrated_unix_ms"):j.getLong("saved_unix_ms"));
            int version=j.optInt("version");
            String expectedModel=migrate?"ow_m_416":runtime.activeModel().token;
            boolean geometryValid=compatibleGeometry(j,version,migrate);
            if (!geometryValid || !c.model.equals(expectedModel)) {
                rejectPreservingFile(runtime,"scope_or_geometry_mismatch model="+c.model
                        +" expected_model="+expectedModel+" route="+c.route+" geometry_valid="+geometryValid
                        +" calibration_age_ms="+(System.currentTimeMillis()-c.savedUnixMs)
                        +" maximum_reference_age_ms="+MobileResponseCalibration.MAX_REFERENCE_AGE_MS);return;
            }
            // v3 alone measured 640 model pixels within a 1024 physical ROI.
            // v4 already measured physical pixels: a smaller ROI cannot change
            // pixels-per-count. Legacy 416/416 also has physical density one.
            double pixelScale=version==3?1.6:1.0;
            if (migrate) c=c.forViewport("ow_m_640",416,416,640,640);
            else if (version==3) c=c.forViewport("ow_m_640",640,1024,640,640);
            if (!c.valid(runtime.activeModel().token,runtime.activeOutputRoute().storageToken,Build.MODEL,System.currentTimeMillis())) {
                rejectPreservingFile(runtime,"scope_values_or_age_mismatch_after_unit_conversion");return;
            }
            if (migrate || version!=5) {
                j.put("coordinate_migrated_from_version",version).put("coordinate_migration_pixel_scale",pixelScale);
                if (migrate) j.put("migrated_from_model","ow_m_416");
                j.put("model",c.model);
                // Retain original JSON numeric values for a physical 1:1
                // migration, including their precision and measurement data.
                if (pixelScale!=1.0) j.put("x",c.x).put("y",c.y);
                currentGeometry(j);
                events.write("control_calibration_viewport_migrated","control_units=screen_roi_pixels pixel_scale="+pixelScale
                        +" source_schema="+version+" source_file_preserved_until_atomic_save=true calibrated_unix_ms="+c.savedUnixMs);
            }
            boolean restored=QnnHtpBridge.restoreNativeControlCalibration(c.x,c.y,c.delayUs,c.qualityX,c.qualityY);
            if (restored) { restoredCalibration=c; restoredRecord=j; }
            events.write("control_calibration_restored","applied="+restored+" x="+c.x+" y="+c.y+" delay_us="+c.delayUs
                +" calibrated_unix_ms="+c.savedUnixMs+" reference_age_ms="+(System.currentTimeMillis()-c.savedUnixMs)
                +" maximum_reference_age_ms="+MobileResponseCalibration.MAX_REFERENCE_AGE_MS+" route="+c.route+" model="+c.model
                +" source="+j.optString("source","legacy_unknown"));
        } catch (Exception invalid) { rejectPreservingFile(runtime,invalid.getClass().getSimpleName()); }
    }
    private void rejectPreservingFile(MobileControlRuntime runtime,String reason) {
        incompatibleTarget=file(runtime).getAbsolutePath();
        events.write("control_calibration_rejected",reason+" incompatible_record_preserved=true automatic_overwrite=false");
    }
    private static boolean number(JSONObject j,String key,double expected,boolean required) {
        if (!j.has(key)) return !required;
        Object value=j.opt(key);
        return value instanceof Number && Double.compare(((Number)value).doubleValue(),expected)==0;
    }
    private static boolean dimensions(JSONObject j,int input,int capture,int coordinate,boolean required) {
        return number(j,"input_width",input,required) && number(j,"input_height",input,required)
                && number(j,"capture_width",capture,required) && number(j,"capture_height",capture,required)
                && number(j,"coordinate_width",coordinate,false) && number(j,"coordinate_height",coordinate,false);
    }
    private static boolean compatibleGeometry(JSONObject j,int version,boolean legacy416) {
        if (!number(j,"version",version,true)) return false;
        if (legacy416) return (version==1 || version==2) && dimensions(j,416,416,416,false)
                && (!j.has("coordinate_space") || "screen_roi_pixels".equals(j.optString("coordinate_space"))
                    || "model_pixels".equals(j.optString("coordinate_space")))
                && number(j,"physical_pixels_per_model_pixel",1,false)
                && number(j,"physical_pixels_per_control_pixel",1,false);
        if (version==3) return dimensions(j,640,1024,640,true)
                && (!j.has("coordinate_space") || "model_pixels".equals(j.optString("coordinate_space")))
                && number(j,"physical_pixels_per_model_pixel",1.6,false)
                && number(j,"physical_pixels_per_control_pixel",1.6,false);
        if (version!=4 && version!=5) return false;
        int capture=version==5?640:j.optInt("capture_width");
        return (capture==640 || capture==1024) && dimensions(j,640,capture,capture,true)
                && number(j,"coordinate_width",capture,true) && number(j,"coordinate_height",capture,true)
                && "screen_roi_pixels".equals(j.optString("coordinate_space"))
                && number(j,"physical_pixels_per_model_pixel",capture/640.0,false)
                && number(j,"physical_pixels_per_control_pixel",1,false);
    }
    private static void currentGeometry(JSONObject j) throws Exception {
        j.put("version",5).put("input_width",640).put("input_height",640)
                .put("capture_width",640).put("capture_height",640).put("coordinate_width",640).put("coordinate_height",640)
                .put("coordinate_space","screen_roi_pixels").put("physical_pixels_per_model_pixel",1)
                .put("physical_pixels_per_control_pixel",1);
    }
    void save(MobileControlRuntime runtime) {
        synchronized (runtime) { saveLocked(runtime); }
    }
    private void saveLocked(MobileControlRuntime runtime) {
        if (file(runtime).getAbsolutePath().equals(incompatibleTarget)) {
            if (!incompatibleSaveReported) events.write("control_calibration_save_skipped","incompatible_record_preserved=true automatic_overwrite=false");
            incompatibleSaveReported=true;return;
        }
        double[] v=QnnHtpBridge.getNativeControlCalibration();
        if (v==null || v.length!=7 || v[5]!=1 || v[6]<=0) return;
        String signature=runtime.activeModel().token+"|"+runtime.activeOutputRoute().storageToken+"|"+java.util.Arrays.toString(v);
        if (signature.equals(savedSignature)) return;
        long now=System.currentTimeMillis();
        boolean sameReference=restoredCalibration!=null && restoredCalibration.sameSensitivity(
            runtime.activeModel().token,runtime.activeOutputRoute().storageToken,Build.MODEL,(float)v[0],(float)v[1]);
        // Restored X/Y remain frozen in the native estimator. Its update count
        // may increase for a delay fit only. Renewing the calibration date here
        // made a wrong reference live forever and mislabelled external evidence.
        long calibratedAt=sameReference?restoredCalibration.savedUnixMs:now;
        MobileResponseCalibration c=sameReference?restoredCalibration.withDelayEstimate((long)v[2]):
            new MobileResponseCalibration(runtime.activeModel().token,runtime.activeOutputRoute().storageToken,Build.MODEL,
                (float)v[0],(float)v[1],(long)v[2],(float)v[3],(float)v[4],calibratedAt);
        if (!c.valid(c.model,c.route,c.device,now)) return;
        try {
            File file=file(runtime), temporary=new File(file.getPath()+".tmp");
            Files.createDirectories(file.getParentFile().toPath());
            JSONObject j=sameReference && restoredRecord!=null?new JSONObject(restoredRecord.toString()):new JSONObject();
            currentGeometry(j);
            if (!sameReference || restoredRecord==null) j.put("x",c.x).put("y",c.y).put("quality_x",c.qualityX).put("quality_y",c.qualityY);
            j.put("model",c.model).put("route",c.route).put("device",c.device)
                .put("delay_us",c.delayUs)
                .put("saved_unix_ms",now).put("calibrated_unix_ms",calibratedAt)
                .put("delay_estimated_unix_ms",now).put("delay_fit_quality_x",v[3]).put("delay_fit_quality_y",v[4]);
            if (!sameReference) j.put("source","online_background_response_fit");
            Files.write(temporary.toPath(),j.toString().getBytes(StandardCharsets.UTF_8));
            Files.move(temporary.toPath(),file.toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);
            savedSignature=signature;
        } catch (Exception invalid) { events.write("control_calibration_save_failed",invalid.getClass().getSimpleName()); }
    }
}
