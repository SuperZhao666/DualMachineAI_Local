package com.visionforge.inferencebenchmark;

import android.content.Context;
import android.content.SharedPreferences;
import java.util.*;

/** Runs the real production constructor against an in-memory SharedPreferences substitute. */
public final class ControlProfileMigrationFixture {
    private static final String TRAJECTORY = """
            {"profile_id":"preserved-custom-profile","quality":{"valid_for_auto_apply":true},
             "aggregate":{"median_movement_duration_ms":540,"median_peak_time_fraction":0.39,
              "median_bell_correlation":0.44,"median_mid_perpendicular_rms_px":11.68,
              "median_high_freq_energy_ratio":0.01069,"median_endpoint_sign_flips":0,
              "fitts_law_grouped":{"intercept_ms":-77.5,"slope_ms_per_bit":163.4}},
             "recommendations":{"control":{"personal_trajectory_jitter_amp_px":0.85,
              "personal_trajectory_jitter_max_px":3.5}},
             "trajectory_model":{"model_type":"empirical_envelope_v1",
              "speed_envelope_16":[0.17,0.24,0.40,0.65,0.97,1.0,0.92,0.86,0.62,0.48,0.37,0.23,0.19,0.13,0.14,0.21],
              "duration_bins":[{"id":3.5,"median_ms":435,"n":22}],
              "holdout_similarity":{"overall_score":0.836}}}
            """;

    public static void main(String[] args) {
        for (float previous : new float[]{2F,1.01F,.95F,.36F,.37F}) verifiesMigration(previous);
        System.out.println("RESULT constructor_cases=5 second_construction_idempotent=true other_keys_unchanged=true native_calls=0");
    }

    private static void verifiesMigration(float previous) {
        MobileModelCatalog.Profile model=MobileModelCatalog.DEFAULT;
        String gainKey=MobileProfileStorageKeys.controlGain(model);
        MemoryPreferences preferences=new MemoryPreferences();
        Map<String,Object> initial=preferences.values;
        // The .36/.5/127 triple used to be a preset. At version 2 it is a
        // user's existing value and must never be re-migrated to the new preset.
        boolean formerPreset=Float.compare(previous,.36F)==0;
        float deadzone=formerPreset?.5F:.73F;
        int maximum=formerPreset?127:101;
        initial.put("screen_pixel_control_150",true);
        initial.put(MobileProfileStorageKeys.CONTROL_TUNING_VERSION,2);
        initial.put(gainKey,previous);
        initial.put(MobileProfileStorageKeys.controlDeadzone(model),deadzone);
        initial.put(MobileProfileStorageKeys.controlMaximumDelta(model),maximum);
        initial.put(MobileProfileStorageKeys.controlSwitchConfirmation(model),67);
        initial.put(MobileProfileStorageKeys.CONTROL_TRIGGER,ControlTrigger.SIDE_BUTTON_1.storageToken);
        initial.put(MobileProfileStorageKeys.PERSONAL_PROFILE_JSON,TRAJECTORY);
        initial.put(MobileProfileStorageKeys.PERSONAL_ENABLED,true);
        initial.put(MobileProfileStorageKeys.PERSONAL_SPEED_SCALE,1.21F);
        initial.put(MobileProfileStorageKeys.PERSONAL_STABILITY_SCALE,.87F);
        initial.put(MobileProfileStorageKeys.PERSONAL_VARIATION_SCALE,1.13F);
        initial.put("gain.ow_m_416",.31F);
        initial.put("deadzone.ow_m_416",3.17F);
        initial.put("maximum_delta.ow_m_416",82);
        initial.put("switch_confirmation_ms.ow_m_416",91);
        initial.put("unrelated_token","keep-verbatim");
        initial.put("unrelated_counter",123456789L);
        Map<String,Object> before=new LinkedHashMap<>(initial);
        float expected=Math.min(previous,.95F);
        Map<String,Object> after=new LinkedHashMap<>(before);after.put(gainKey,expected);
        Context context=new Context(preferences);
        ControlProfile first=new ControlProfile(context,model,MobileAimTarget.HEAD);
        require(preferences.values.equals(after),"constructor changed unrelated saved settings: "+previous);
        require(Float.compare(first.gain(),expected)==0,"incorrect effective gain");
        require(Float.compare(first.deadzonePixels(),deadzone)==0,"deadzone changed");
        require(first.maximumAxisDelta()==maximum,"maximum delta changed");
        require(first.switchConfirmationMillis()==67,"switch confirmation changed");
        require(first.controlTrigger()==ControlTrigger.SIDE_BUTTON_1,"trigger changed");
        require(first.personalTrajectoryEnabled(),"personal profile was disabled");
        require(first.personalTrajectoryProfileId().equals("preserved-custom-profile"),"personal profile changed");
        require(first.personalTrajectorySpeedScale()==1.21F && first.personalTrajectoryStabilityScale()==.87F
                && first.personalTrajectoryVariationScale()==1.13F,"personal scales changed");
        require(preferences.getInt(MobileProfileStorageKeys.CONTROL_TUNING_VERSION,0)==2,"preset version changed");
        boolean changed=Float.compare(previous,expected)!=0;
        require(preferences.applyCalls==(changed?1:0),"unexpected migration write count");
        require(preferences.writtenKeys.equals(changed?Set.of(gainKey):Set.of()),"unexpected key rewritten");
        int writes=preferences.applyCalls;
        new ControlProfile(context,model,MobileAimTarget.HEAD);
        require(preferences.values.equals(after) && preferences.applyCalls==writes,"second construction was not idempotent");
        mathematicalEquivalence(previous,first.gain());
        System.out.println("PASS constructor old_gain="+previous+" effective_gain="+first.gain()
                +" edits="+writes+" preserved_keys="+(before.size()-1)+" second_constructor_edits=0");
    }

    private static void mathematicalEquivalence(float old,float migrated) {
        for (float seconds:new float[]{.002F,.008F,.016667F,.025F,.05F})
            for (float error:new float[]{1.2F,6F,28F,100F})
                for(float pursuit:new float[]{0F,.3F,1F}) {
                    float oldBase=Math.max(.01F,Math.min(.95F,old));
                    float oldResponse=Math.min(.95F,oldBase+(1-oldBase)*.5F*pursuit);
                    float newResponse=Math.min(.95F,migrated+(1-migrated)*.5F*pursuit);
                    double oldFeedback=(1-Math.pow(1-oldResponse,Math.min(seconds,.025F)*60F))*error;
                    double newFeedback=(1-Math.pow(1-newResponse,Math.min(seconds,.025F)*60F))*error;
                    require(Double.doubleToLongBits(oldFeedback)==Double.doubleToLongBits(newFeedback),
                            "old deployed response changed by migration");
                }
    }
    private static void require(boolean condition,String reason) { if(!condition)throw new AssertionError(reason); }

    private static final class MemoryPreferences implements SharedPreferences {
        final Map<String,Object> values=new LinkedHashMap<>();
        final Set<String> writtenKeys=new HashSet<>();
        int applyCalls;
        public Map<String,?> getAll(){return new LinkedHashMap<>(values);}
        public String getString(String key,String fallback){Object value=values.get(key);return value==null?fallback:(String)value;}
        @SuppressWarnings("unchecked")
        public Set<String> getStringSet(String key,Set<String> fallback){Object value=values.get(key);return value==null?fallback:(Set<String>)value;}
        public int getInt(String key,int fallback){Object value=values.get(key);return value==null?fallback:(Integer)value;}
        public long getLong(String key,long fallback){Object value=values.get(key);return value==null?fallback:(Long)value;}
        public float getFloat(String key,float fallback){Object value=values.get(key);return value==null?fallback:(Float)value;}
        public boolean getBoolean(String key,boolean fallback){Object value=values.get(key);return value==null?fallback:(Boolean)value;}
        public boolean contains(String key){return values.containsKey(key);}
        public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener){}
        public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener){}
        public Editor edit(){return new Edit();}
        private final class Edit implements Editor {
            final Map<String,Object> updates=new LinkedHashMap<>();
            boolean clear;
            public Editor putString(String key,String value){updates.put(key,value);return this;}
            public Editor putStringSet(String key,Set<String> value){updates.put(key,new HashSet<>(value));return this;}
            public Editor putInt(String key,int value){updates.put(key,value);return this;}
            public Editor putLong(String key,long value){updates.put(key,value);return this;}
            public Editor putFloat(String key,float value){updates.put(key,value);return this;}
            public Editor putBoolean(String key,boolean value){updates.put(key,value);return this;}
            public Editor remove(String key){updates.put(key,null);return this;}
            public Editor clear(){clear=true;return this;}
            public boolean commit(){apply();return true;}
            public void apply(){
                ++applyCalls;
                if(clear){writtenKeys.addAll(values.keySet());values.clear();}
                for(Map.Entry<String,Object> entry:updates.entrySet()) {
                    writtenKeys.add(entry.getKey());
                    if(entry.getValue()==null)values.remove(entry.getKey());
                    else values.put(entry.getKey(),entry.getValue());
                }
            }
        }
    }
}
