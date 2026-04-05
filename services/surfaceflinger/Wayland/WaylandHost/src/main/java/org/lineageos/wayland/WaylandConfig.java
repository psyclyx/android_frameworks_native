package org.lineageos.wayland;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Configuration for the Wayland host. Chroot path and other settings
 * are stored in SharedPreferences and configurable via the settings Activity.
 */
public class WaylandConfig {
    private static final String PREFS_NAME = "wayland_config";
    private static final String KEY_CHROOT_PATH = "chroot_path";
    private static final String DEFAULT_CHROOT_PATH = "/data/arch";

    public static SharedPreferences getPrefs(Context context) {
        return context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    public static String getChrootPath(Context context) {
        return getPrefs(context).getString(KEY_CHROOT_PATH, DEFAULT_CHROOT_PATH);
    }

    public static void setChrootPath(Context context, String path) {
        getPrefs(context).edit().putString(KEY_CHROOT_PATH, path).apply();
    }
}
