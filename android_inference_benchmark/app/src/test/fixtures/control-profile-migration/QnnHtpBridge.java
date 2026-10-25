package com.visionforge.inferencebenchmark;

/** Constructor migration must never call JNI or touch hardware. */
final class QnnHtpBridge {
    static boolean configureNativeMakcuControl(float gain,float deadzone,int maximum,int confirmation,
            boolean enabled,boolean personal,long seed,float speed,float stability,float variation,
            float duration,float peak,float bell,float jitter,float maximumJitter,int extra,float minimumError,
            float[] envelope,String model,int body,int head,int selected,boolean headBox,
            boolean geometricHead,float targetY) {
        throw new AssertionError("preference migration attempted native output");
    }
}
