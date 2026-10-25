package com.visionforge.inferencebenchmark;
import android.content.Context;
import java.io.File;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

final class SettingsSnapshot {
    static void save(Context context) throws Exception {
        JSONObject result=new JSONObject();
        for(String name:new String[]{"visionforge_control_profile","visionforge_inference_profile","visionforge_control_output"}){
            JSONObject store=new JSONObject();
            for(java.util.Map.Entry<String,?> e:context.getSharedPreferences(name,Context.MODE_PRIVATE).getAll().entrySet()){
                String key=e.getKey();
                if(key.equals("active_model") || key.equals("control_trigger") || key.equals("selected_route_v1")
                   || key.equals("control_tuning_version") || key.startsWith("gain.") || key.startsWith("deadzone.")
                   || key.startsWith("maximum_delta.") || key.startsWith("switch_confirmation_ms.")
                   || key.startsWith("confidence.") || key.startsWith("aim_target."))store.put(key,e.getValue());
            }
            result.put(name,store);
        }
        result.put("installed_version",context.getPackageManager().getPackageInfo(context.getPackageName(),0).versionName);
        Files.write(new File(context.getExternalFilesDir(null),"settings-snapshot.json").toPath(),result.toString(2).getBytes(StandardCharsets.UTF_8));
    }
}
