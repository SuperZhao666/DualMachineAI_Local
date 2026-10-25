package com.visionforge.inferencebenchmark;

final class MobileResponseCalibrationSelfTest {
    static void run() {
        long now=1_792_315_129_606L;
        MobileResponseCalibration c=calibration(.664926f,.753563f,60_000,.955341f,.891751f,now);
        require(c.valid("ow_m_416","bluetooth_hid","23116PN5BC",now+60_000));
        require(!c.valid("ow_m_320","bluetooth_hid","23116PN5BC",now));
        require(!c.valid("ow_m_416","makcu_usb","23116PN5BC",now));
        require(!c.valid("ow_m_416","bluetooth_hid","another_phone",now));
        require(c.valid("ow_m_416","bluetooth_hid","23116PN5BC",now+86_400_001));
        require(!c.valid("ow_m_416","bluetooth_hid","23116PN5BC",now+MobileResponseCalibration.MAX_REFERENCE_AGE_MS+1));
        require(!c.valid("ow_m_416","bluetooth_hid","23116PN5BC",now-300_001));
        require(!calibration(Float.NaN,.75f,60_000,.95f,.89f,now).valid(c.model,c.route,c.device,now));
        require(!calibration(.66f,.75f,60_000,.81f,.89f,now).valid(c.model,c.route,c.device,now));
        require(!calibration(.66f,.75f,100_001,.95f,.89f,now).valid(c.model,c.route,c.device,now));
        require(c.sameSensitivity(c.model,c.route,c.device,c.x,c.y));
        require(!c.sameSensitivity(c.model,"makcu_usb",c.device,c.x,c.y));
        require(!c.sameSensitivity(c.model,c.route,c.device,1f,c.y));
        // Changing transport delay and its confidence must not renew the age
        // of the unchanged, restored sensitivity.
        MobileResponseCalibration later=c.withDelayEstimate(24_000);
        require(later.delayUs==24_000 && later.qualityX==c.qualityX && later.qualityY==c.qualityY);
        require(later.savedUnixMs==c.savedUnixMs);
        require(!later.valid(c.model,c.route,c.device,now+MobileResponseCalibration.MAX_REFERENCE_AGE_MS+1));
        MobileResponseCalibration scaled=c.forViewport("ow_m_640",416,416,640,1024);
        require(scaled.valid("ow_m_640",c.route,c.device,now));
        // The same 80 physical pixels must require the same hardware counts.
        require(Math.abs(80/c.x - (80*640f/1024)/scaled.x)<.001f);
        require(Math.abs(80/c.y - (80*640f/1024)/scaled.y)<.001f);
        require(scaled.savedUnixMs==c.savedUnixMs && scaled.delayUs==c.delayUs);
        require(scaled.valid("ow_m_640",c.route,c.device,now+86_400_001));
        require(!scaled.valid("ow_m_640",c.route,c.device,now+MobileResponseCalibration.MAX_REFERENCE_AGE_MS+1));
        MobileResponseCalibration physical=c.forViewport("ow_m_640",416,416,1024,1024);
        require(physical.x==c.x && physical.y==c.y && physical.savedUnixMs==c.savedUnixMs);
        MobileResponseCalibration upgraded=scaled.forViewport("ow_m_640",640,1024,1024,1024);
        require(Math.abs(upgraded.x-c.x)<.000001f && Math.abs(upgraded.y-c.y)<.000001f);
        // Cropping a smaller physical ROI does not change physical pixels per
        // hardware count. Only the old v3 model-pixel density needs conversion.
        MobileResponseCalibration native640=physical.forViewport("ow_m_640",1024,1024,640,640);
        require(Float.compare(native640.x,physical.x)==0 && Float.compare(native640.y,physical.y)==0);
        require(native640.delayUs==physical.delayUs && native640.savedUnixMs==physical.savedUnixMs);
        require(native640.qualityX==physical.qualityX && native640.qualityY==physical.qualityY);
        MobileResponseCalibration v3ToNative640=scaled.forViewport("ow_m_640",640,1024,640,640);
        require(Math.abs(v3ToNative640.x-c.x)<.000001f && Math.abs(v3ToNative640.y-c.y)<.000001f);
        MobileResponseCalibration legacyToNative640=c.forViewport("ow_m_640",416,416,640,640);
        require(Float.compare(legacyToNative640.x,c.x)==0 && Float.compare(legacyToNative640.y,c.y)==0);
    }
    private static MobileResponseCalibration calibration(float x,float y,long delay,float qx,float qy,long saved) {
        return new MobileResponseCalibration("ow_m_416","bluetooth_hid","23116PN5BC",x,y,delay,qx,qy,saved);
    }
    private static void require(boolean value) { if (!value) throw new AssertionError("Calibration scope or validity violated"); }
}
