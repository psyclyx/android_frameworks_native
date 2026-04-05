package org.lineageos.wayland;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import java.io.File;
import java.io.IOException;

public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = "WaylandBootReceiver";

    @Override
    public void onReceive(Context context, Intent intent) {
        // Use device-protected storage so this works before user unlock.
        Context deviceContext = context.createDeviceProtectedStorageContext();

        deviceContext.startService(new Intent(deviceContext, WaylandWindowService.class));
        String chrootPath = WaylandConfig.getChrootPath(deviceContext);
        new Thread(() -> setupChrootMounts(chrootPath)).start();
    }

    private void setupChrootMounts(String chrootPath) {
        // Wait for the su daemon socket to appear (init may not have started it yet)
        for (int i = 0; i < 30; i++) {
            if (new File("/dev/socket/su_daemon").exists()) break;
            try { Thread.sleep(1000); } catch (InterruptedException e) { return; }
        }
        if (!new File("/dev/socket/su_daemon").exists()) {
            Log.e(TAG, "su daemon not available, cannot set up chroot mounts");
            return;
        }

        try {
            // Single su call with mountpoint checks to avoid stacking mounts
            String script = "C=" + chrootPath + ";"
                + " mountpoint -q $C/proc       || mount -t proc proc $C/proc;"
                + " mountpoint -q $C/dev        || mount -o bind /dev $C/dev;"
                + " mountpoint -q $C/dev/pts    || mount -o bind /dev/pts $C/dev/pts;"
                + " mountpoint -q $C/tmp        || mount -t tmpfs tmpfs $C/tmp;"
                + " mkdir -p $C/run/wayland;"
                + " mountpoint -q $C/run/wayland || mount -o bind /data/wayland $C/run/wayland;";
            Runtime.getRuntime().exec(new String[]{"su", "0", "sh", "-c", script}).waitFor();
            Log.i(TAG, "Chroot mounts set up for " + chrootPath);
        } catch (IOException | InterruptedException e) {
            Log.e(TAG, "Failed to set up chroot mounts", e);
        }
    }
}
