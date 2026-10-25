package com.visionforge.inferencebenchmark;
import android.content.SharedPreferences;

/** Copy the user's original-model preferences once; never remove the old keys. */
final class ModelViewportPreferenceMigration {
    static void control(SharedPreferences preferences) {
        if(preferences.getBoolean("screen_pixel_control_150",false))return;
        SharedPreferences.Editor editor=preferences.edit();boolean changed=false;
        for(String key:new String[]{"gain.","deadzone.","maximum_delta.","switch_confirmation_ms."}) {
            String oldKey=key+"ow_m_416",newKey=key+"ow_m_640";
            // Older experimental 640 defaults may already exist. On this one
            // migration, the user's active 416 settings are authoritative.
            if(!preferences.contains(oldKey))continue;
            if(key.equals("maximum_delta.") || key.equals("switch_confirmation_ms."))
                editor.putInt(newKey,preferences.getInt(oldKey,0));
            else editor.putFloat(newKey,preferences.getFloat(oldKey,0f));
            changed=true;
        }
        editor.putBoolean("screen_pixel_control_150",true).apply();
    }
    static void inference(SharedPreferences preferences) {
        if(preferences.getBoolean("viewport_640_from_416_v1",false))return;
        SharedPreferences.Editor editor=preferences.edit();boolean changed=false;
        if(preferences.contains("confidence.ow_m_416")){
            editor.putFloat("confidence.ow_m_640",preferences.getFloat("confidence.ow_m_416",.2f));changed=true;
        }
        if(preferences.contains("aim_target.ow_m_416")){
            editor.putString("aim_target.ow_m_640",preferences.getString("aim_target.ow_m_416","head"));changed=true;
        }
        if("ow_m_416".equals(preferences.getString(MobileProfileStorageKeys.ACTIVE_MODEL,""))){
            editor.putString(MobileProfileStorageKeys.ACTIVE_MODEL,"ow_m_640");changed=true;
        }
        editor.putBoolean("viewport_640_from_416_v1",true).apply();
    }
}
