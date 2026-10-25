package com.visionforge.inferencebenchmark;

/** A same-device, same-model and same-route estimate with a bounded lifetime. */
final class MobileResponseCalibration {
    // Sensitivity is a device/game setting, not a daily measurement. A 24h
    // expiry discarded unchanged references between consecutive play sessions
    // and exposed the estimator to correlated manual movement again.
    static final long MAX_REFERENCE_AGE_MS=30L*24*60*60*1000;
    final String model, route, device;
    final float x,y,qualityX,qualityY;
    final long delayUs,savedUnixMs;
    MobileResponseCalibration(String model,String route,String device,float x,float y,long delay,
            float qx,float qy,long saved) {
        this.model=model;this.route=route;this.device=device;this.x=x;this.y=y;
        delayUs=delay;qualityX=qx;qualityY=qy;savedUnixMs=saved;
    }
    boolean valid(String expectedModel,String expectedRoute,String expectedDevice,long now) {
        return expectedModel.equals(model) && expectedRoute.equals(route) && expectedDevice.equals(device)
            && Float.isFinite(x) && Float.isFinite(y) && x>=.2f && x<=4f && y>=.2f && y<=4f
            && Float.isFinite(qualityX) && Float.isFinite(qualityY)
            && qualityX>=.82f && qualityX<=1f && qualityY>=.82f && qualityY<=1f
            && delayUs>=8000 && delayUs<=100000 && savedUnixMs>0
            && savedUnixMs<=now+300000 && now-savedUnixMs<=MAX_REFERENCE_AGE_MS;
    }
    /** A new delay/fit score is not a new measurement of a frozen sensitivity. */
    boolean sameSensitivity(String expectedModel,String expectedRoute,String expectedDevice,float newX,float newY) {
        return model.equals(expectedModel) && route.equals(expectedRoute) && device.equals(expectedDevice)
                && Float.compare(x,newX)==0 && Float.compare(y,newY)==0;
    }
    MobileResponseCalibration withDelayEstimate(long delay) {
        return new MobileResponseCalibration(model,route,device,x,y,delay,qualityX,qualityY,savedUnixMs);
    }
    /** Preserve physical screen displacement when model/ROI pixel density changes. */
    MobileResponseCalibration forViewport(String newModel,int oldInput,int oldView,int newInput,int newView) {
        if (oldInput<=0 || oldView<=0 || newInput<=0 || newView<=0)
            throw new IllegalArgumentException("invalid calibration viewport");
        float ratio=((float)newInput/newView)/((float)oldInput/oldView);
        return new MobileResponseCalibration(newModel,route,device,x*ratio,y*ratio,delayUs,qualityX,qualityY,savedUnixMs);
    }
}
