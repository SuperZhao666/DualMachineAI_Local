package com.visionforge.inferencebenchmark;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Debug;
import android.os.PowerManager;
import android.os.Process;
import android.os.SystemClock;

/** Low-rate context for interpreting numerical traces from different releases. */
final class DiagnosticSessionManifest {
    private long priorCpu, priorWall;
    String sample(Context context) {
        MobileModelCatalog.Profile m=MobileModelCatalog.DEFAULT;
        PowerManager power=context.getSystemService(PowerManager.class);
        long cpu=Process.getElapsedCpuTime(),wall=SystemClock.elapsedRealtime();
        long cpuPermille=priorWall>0 && wall>priorWall ? (cpu-priorCpu)*1000/(wall-priorWall) : -1;
        priorCpu=cpu;priorWall=wall;
        Intent battery=context.registerReceiver(null,new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int temperature=battery==null?-1:battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE,-1);
        int level=battery==null?-1:battery.getIntExtra(BatteryManager.EXTRA_LEVEL,-1);
        int scale=battery==null?-1:battery.getIntExtra(BatteryManager.EXTRA_SCALE,-1);
        int plugged=battery==null?-1:battery.getIntExtra(BatteryManager.EXTRA_PLUGGED,-1);
        java.io.File directory=context.getExternalFilesDir(null);
        return "diagnostic_schema=2 app_version="+BuildConfig.VERSION_NAME+" version_code="+BuildConfig.VERSION_CODE
                +" model="+m.token+" reference_model_sha256="+m.portableModelSha256
                +" qnn_model_sha256="+BuildConfig.QNN_MODEL_SHA256
                +" model_width="+m.inputWidth+" model_height="+m.inputHeight
                +" capture_width=640 capture_height=640 control_coordinate_space=screen_roi_pixels"
                +" coordinate_width=640 coordinate_height=640 physical_pixels_per_model_pixel=1"
                +" physical_pixels_per_control_pixel=1 geometric_head_ratio=0.18 wide_search=removed_redundant"
                +" device="+Build.MODEL+" soc="+Build.SOC_MODEL+" sdk="+Build.VERSION.SDK_INT
                +" pss_kib="+Debug.getPss()+" process_cpu_permille="+cpuPermille
                +" battery_temperature_deci_c="+temperature+" battery_level="+level+" battery_scale="+scale
                +" battery_plugged="+plugged+" interactive="+power.isInteractive()
                +" device_idle="+power.isDeviceIdleMode()+" power_save="+power.isPowerSaveMode()
                +" thermal_status="+power.getCurrentThermalStatus()
                +" diagnostic_free_bytes="+(directory==null?-1:directory.getUsableSpace())
                +" timing_scope=host_age_plus_phone_no_network_clock_sync physical_execution_measured=false";
    }
}
