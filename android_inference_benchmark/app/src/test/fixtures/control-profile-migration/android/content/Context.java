package android.content;

public final class Context {
    public static final int MODE_PRIVATE = 0;
    private final SharedPreferences preferences;
    public Context(SharedPreferences preferences) { this.preferences=preferences; }
    public SharedPreferences getSharedPreferences(String name, int mode) {
        if (!"visionforge_control_profile".equals(name) || mode!=MODE_PRIVATE)
            throw new AssertionError("unexpected preference store");
        return preferences;
    }
}
