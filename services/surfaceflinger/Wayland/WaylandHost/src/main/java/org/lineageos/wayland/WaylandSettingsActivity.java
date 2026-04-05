package org.lineageos.wayland;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

/**
 * Settings for the Wayland host: chroot path, etc.
 */
public class WaylandSettingsActivity extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(48, 48, 48, 48);

        TextView label = new TextView(this);
        label.setText("Linux Root Filesystem Path:");
        label.setTextSize(16);
        layout.addView(label);

        EditText pathEdit = new EditText(this);
        pathEdit.setText(WaylandConfig.getChrootPath(this));
        pathEdit.setSingleLine(true);
        layout.addView(pathEdit);

        Button saveButton = new Button(this);
        saveButton.setText("Save");
        saveButton.setOnClickListener(v -> {
            String path = pathEdit.getText().toString().trim();
            if (path.isEmpty() || !new File(path).isDirectory()) {
                Toast.makeText(this, "Invalid path: " + path, Toast.LENGTH_SHORT).show();
                return;
            }
            WaylandConfig.setChrootPath(this, path);
            Toast.makeText(this, "Saved", Toast.LENGTH_SHORT).show();
            finish();
        });
        layout.addView(saveButton);

        // Show status info
        TextView status = new TextView(this);
        status.setPadding(0, 32, 0, 0);
        String chrootPath = WaylandConfig.getChrootPath(this);
        File appsDir = new File(chrootPath, "usr/share/applications");
        int appCount = 0;
        if (appsDir.isDirectory()) {
            File[] files = appsDir.listFiles((d, n) -> n.endsWith(".desktop"));
            if (files != null) appCount = files.length;
        }
        status.setText("Status:\n"
                + "  Chroot: " + (new File(chrootPath, "usr").isDirectory() ? "OK" : "NOT FOUND") + "\n"
                + "  Desktop files: " + appCount + "\n"
                + "  Wayland socket: " + (new File("/data/wayland/wayland-0").exists() ? "OK" : "NOT FOUND"));
        layout.addView(status);

        setContentView(layout);
        setTitle("Wayland Settings");
    }
}
