package org.lineageos.wayland;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Shows installed Linux apps from .desktop files in the configured chroot,
 * plus a tab to view and kill running Linux processes.
 * Pull down to refresh either list.
 */
public class WaylandAppLauncherActivity extends Activity {
    private static final String TAG = "WaylandAppLauncher";

    private SwipeRefreshLayout mAppsRefresh;
    private ListView mAppsListView;
    private List<DesktopEntry> mApps = new ArrayList<>();

    private SwipeRefreshLayout mProcsRefresh;
    private ListView mProcsListView;
    private List<ProcessEntry> mProcs = new ArrayList<>();

    private int mCurrentTab = 0; // 0=Apps, 1=Processes

    private TextView mAppsTab;
    private TextView mProcsTab;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (getActionBar() != null) getActionBar().hide();

        android.widget.LinearLayout root = new android.widget.LinearLayout(this);
        root.setOrientation(android.widget.LinearLayout.VERTICAL);
        root.setFitsSystemWindows(true);

        // --- Title ---
        TextView title = new TextView(this);
        title.setText("Linux Apps");
        title.setTextSize(22);
        title.setTextColor(0xFFFFFFFF);
        title.setPadding(48, 32, 48, 16);
        root.addView(title);

        // --- Tab bar ---
        android.widget.LinearLayout tabBar = new android.widget.LinearLayout(this);
        tabBar.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        android.widget.LinearLayout.LayoutParams tabWeight =
                new android.widget.LinearLayout.LayoutParams(0,
                        android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f);

        mAppsTab = new TextView(this);
        mAppsTab.setText("Apps");
        mAppsTab.setTextSize(16);
        mAppsTab.setTextColor(0xFFFFFFFF);
        mAppsTab.setGravity(android.view.Gravity.CENTER);
        mAppsTab.setPadding(0, 36, 0, 36);
        mAppsTab.setOnClickListener(v -> showTab(0));
        tabBar.addView(mAppsTab, tabWeight);

        mProcsTab = new TextView(this);
        mProcsTab.setText("Processes");
        mProcsTab.setTextSize(16);
        mProcsTab.setTextColor(0xFFFFFFFF);
        mProcsTab.setGravity(android.view.Gravity.CENTER);
        mProcsTab.setPadding(0, 36, 0, 36);
        mProcsTab.setOnClickListener(v -> showTab(1));
        tabBar.addView(mProcsTab, tabWeight);

        root.addView(tabBar);

        // --- Content area ---
        android.widget.FrameLayout content = new android.widget.FrameLayout(this);
        android.widget.LinearLayout.LayoutParams contentLp =
                new android.widget.LinearLayout.LayoutParams(
                        android.widget.LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);

        // Apps
        mAppsListView = new ListView(this);
        mAppsRefresh = new SwipeRefreshLayout(this);
        mAppsRefresh.addView(mAppsListView);
        mAppsRefresh.setOnRefreshListener(this::refreshApps);
        mAppsListView.setOnItemClickListener((parent, view, pos, id) -> {
            if (pos >= 0 && pos < mApps.size()) launchApp(mApps.get(pos));
        });
        content.addView(mAppsRefresh);

        // Processes
        mProcsListView = new ListView(this);
        mProcsRefresh = new SwipeRefreshLayout(this);
        mProcsRefresh.addView(mProcsListView);
        mProcsRefresh.setOnRefreshListener(this::refreshProcs);
        mProcsListView.setOnItemClickListener((parent, view, pos, id) -> {
            if (pos >= 0 && pos < mProcs.size()) promptKillProcess(mProcs.get(pos));
        });
        mProcsRefresh.setVisibility(View.GONE);
        content.addView(mProcsRefresh);

        root.addView(content, contentLp);
        setContentView(root);

        showTab(0);
    }

    private void showTab(int tab) {
        mCurrentTab = tab;
        mAppsRefresh.setVisibility(tab == 0 ? View.VISIBLE : View.GONE);
        mProcsRefresh.setVisibility(tab == 1 ? View.VISIBLE : View.GONE);

        mAppsTab.setAlpha(tab == 0 ? 1.0f : 0.5f);
        mProcsTab.setAlpha(tab == 1 ? 1.0f : 0.5f);

        if (tab == 0) refreshApps(); else refreshProcs();
    }

    // --- Apps ---

    private void refreshApps() {
        mAppsRefresh.setRefreshing(true);
        new Thread(() -> {
            List<DesktopEntry> apps = scanApps();
            runOnUiThread(() -> {
                mApps = apps;
                mAppsListView.setAdapter(new ArrayAdapter<DesktopEntry>(this,
                        android.R.layout.simple_list_item_1, mApps) {
                    @Override
                    public View getView(int position, View convertView, ViewGroup parent) {
                        View view = super.getView(position, convertView, parent);
                        DesktopEntry entry = mApps.get(position);
                        TextView text = view.findViewById(android.R.id.text1);
                        text.setText(entry.name);
                        text.setCompoundDrawablePadding(24);
                        text.setMinHeight(128);

                        if (entry.icon == null && entry.iconPath != null
                                && entry.iconPath.endsWith(".png")) {
                            entry.icon = loadIconViaSu(entry.iconPath);
                        }
                        if (entry.icon != null) {
                            Bitmap scaled = Bitmap.createScaledBitmap(entry.icon, 72, 72, true);
                            android.graphics.drawable.BitmapDrawable d =
                                    new android.graphics.drawable.BitmapDrawable(
                                            getResources(), scaled);
                            text.setCompoundDrawablesRelativeWithIntrinsicBounds(
                                    d, null, null, null);
                        } else {
                            text.setCompoundDrawablesRelativeWithIntrinsicBounds(
                                    null, null, null, null);
                        }
                        return view;
                    }
                });
                mAppsRefresh.setRefreshing(false);
            });
        }).start();
    }

    // --- Processes ---

    private void refreshProcs() {
        mProcsRefresh.setRefreshing(true);
        new Thread(() -> {
            List<ProcessEntry> procs = scanProcesses();
            runOnUiThread(() -> {
                mProcs = procs;
                mProcsListView.setAdapter(new ArrayAdapter<ProcessEntry>(this,
                        android.R.layout.simple_list_item_2,
                        android.R.id.text1, mProcs) {
                    @Override
                    public View getView(int position, View convertView, ViewGroup parent) {
                        View view = super.getView(position, convertView, parent);
                        ProcessEntry proc = mProcs.get(position);
                        TextView text1 = view.findViewById(android.R.id.text1);
                        TextView text2 = view.findViewById(android.R.id.text2);
                        String indent = "";
                        for (int i = 0; i < proc.depth; i++) indent += "    ";
                        text1.setText(indent + proc.name);
                        text2.setText(indent + "PID " + proc.pid + "  " + proc.cmdline);
                        return view;
                    }
                });
                mProcsRefresh.setRefreshing(false);
            });
        }).start();
    }

    private List<ProcessEntry> scanProcesses() {
        String chrootPath = WaylandConfig.getChrootPath(this);

        // Single su call: readlink is a fast syscall, only read details for matches
        String output = suExec(
            "cd /proc && for p in [0-9]*; do"
            + " [ \"$(readlink $p/root)\" = '" + chrootPath + "' ] || continue;"
            + " read -r _ _ _ ppid _ < $p/stat 2>/dev/null;"
            + " read -r comm < $p/comm 2>/dev/null;"
            + " cmd=$(tr '\\0' ' ' < $p/cmdline 2>/dev/null);"
            + " echo \"$p $ppid $comm $cmd\";"
            + " done"
        );

        // Parse into entries, index by pid
        java.util.Map<String, ProcessEntry> byPid = new java.util.LinkedHashMap<>();
        for (String line : output.split("\n")) {
            line = line.trim();
            if (line.isEmpty()) continue;
            String[] parts = line.split(" ", 4);
            if (parts.length < 3) continue;
            ProcessEntry pe = new ProcessEntry();
            pe.pid = parts[0];
            pe.ppid = parts[1];
            pe.name = parts[2];
            pe.cmdline = parts.length > 3 ? parts[3].trim() : "";
            byPid.put(pe.pid, pe);
        }

        // Build tree: link children to parents
        for (ProcessEntry pe : byPid.values()) {
            ProcessEntry parent = byPid.get(pe.ppid);
            if (parent != null) {
                parent.children.add(pe);
            } else {
                pe.isLeader = true;
            }
        }

        // Flatten tree in DFS order with depth
        List<ProcessEntry> result = new ArrayList<>();
        for (ProcessEntry pe : byPid.values()) {
            if (pe.isLeader) {
                flattenTree(pe, 0, result);
            }
        }
        return result;
    }

    private void flattenTree(ProcessEntry node, int depth, List<ProcessEntry> out) {
        node.depth = depth;
        out.add(node);
        for (ProcessEntry child : node.children) {
            flattenTree(child, depth + 1, out);
        }
    }

    private void promptKillProcess(ProcessEntry proc) {
        String title = proc.isLeader ? "Kill process tree?" : "Kill process?";
        String msg = proc.name + " (PID " + proc.pid + ")\n" + proc.cmdline;
        if (proc.isLeader && !proc.children.isEmpty()) {
            msg += "\n\nThis will kill " + (countTree(proc) - 1) + " child process(es) too.";
        }
        final String killCmd = proc.isLeader
                ? "kill -- -$(ps -o pgid= -p " + proc.pid + " | tr -d ' ') 2>/dev/null; kill " + proc.pid + " 2>/dev/null"
                : "kill " + proc.pid;
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(msg)
                .setPositiveButton("Kill", (d, w) -> {
                    suExec(killCmd);
                    Toast.makeText(this, "Killed " + proc.name, Toast.LENGTH_SHORT).show();
                    refreshProcs();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private int countTree(ProcessEntry node) {
        int count = 1;
        for (ProcessEntry child : node.children) count += countTree(child);
        return count;
    }

    // --- Shared helpers ---

    private String suExec(String cmd) {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{
                "su", "0", "sh", "-c", cmd
            });
            StringBuilder sb = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(
                    new InputStreamReader(p.getInputStream()))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    sb.append(line).append('\n');
                }
            }
            p.waitFor();
            return sb.toString();
        } catch (IOException | InterruptedException e) {
            Log.e(TAG, "suExec failed: " + cmd, e);
            return "";
        }
    }

    private List<DesktopEntry> scanApps() {
        List<DesktopEntry> apps = new ArrayList<>();
        String chrootPath = WaylandConfig.getChrootPath(this);
        String appsDir = chrootPath + "/usr/share/applications";

        String output = suExec(
            "for f in " + appsDir + "/*.desktop; do "
            + "[ -f \"$f\" ] && echo '===FILE:'\"$f\"'===' && cat \"$f\"; "
            + "done"
        );

        if (output.isEmpty()) {
            Log.w(TAG, "No .desktop files found in " + appsDir);
            return apps;
        }

        String currentFile = null;
        List<String> currentLines = new ArrayList<>();

        for (String line : output.split("\n")) {
            if (line.startsWith("===FILE:") && line.endsWith("===")) {
                if (currentFile != null) {
                    DesktopEntry entry = parseDesktopLines(currentFile, currentLines);
                    if (entry != null && !entry.noDisplay && entry.exec != null) {
                        apps.add(entry);
                    }
                }
                currentFile = line.substring(8, line.length() - 3);
                currentLines.clear();
            } else {
                currentLines.add(line);
            }
        }
        if (currentFile != null) {
            DesktopEntry entry = parseDesktopLines(currentFile, currentLines);
            if (entry != null && !entry.noDisplay && entry.exec != null) {
                apps.add(entry);
            }
        }

        Collections.sort(apps, (a, b) -> a.name.compareToIgnoreCase(b.name));

        for (DesktopEntry entry : apps) {
            if (entry.iconName != null) {
                entry.iconPath = resolveIconPath(chrootPath, entry.iconName);
            }
        }

        Log.i(TAG, "Found " + apps.size() + " apps in " + appsDir);
        return apps;
    }

    private DesktopEntry parseDesktopLines(String filePath, List<String> lines) {
        DesktopEntry entry = new DesktopEntry();
        entry.desktopFile = filePath;
        boolean inDesktopEntry = false;

        for (String line : lines) {
            line = line.trim();
            if (line.equals("[Desktop Entry]")) {
                inDesktopEntry = true;
                continue;
            }
            if (line.startsWith("[") && line.endsWith("]")) {
                inDesktopEntry = false;
                continue;
            }
            if (!inDesktopEntry) continue;

            if (line.startsWith("Name=")) {
                entry.name = line.substring(5);
            } else if (line.startsWith("Exec=")) {
                entry.exec = line.substring(5).replaceAll("%[fFuUdDnNickvm]", "").trim();
            } else if (line.startsWith("Icon=")) {
                entry.iconName = line.substring(5);
            } else if (line.startsWith("NoDisplay=true")) {
                entry.noDisplay = true;
            } else if (line.startsWith("Type=") && !line.equals("Type=Application")) {
                return null;
            } else if (line.startsWith("Terminal=true")) {
                entry.terminal = true;
            }
        }

        if (entry.name == null) {
            String fileName = filePath.substring(filePath.lastIndexOf('/') + 1);
            entry.name = fileName.replace(".desktop", "");
        }

        return entry;
    }

    private String resolveIconPath(String chrootPath, String iconName) {
        if (iconName.startsWith("/")) {
            return chrootPath + iconName;
        }

        String[] sizes = {"48x48", "64x64", "128x128", "scalable"};
        String[] themes = {"hicolor", "Adwaita"};
        String[] categories = {"apps", "categories", "mimetypes"};
        String[] exts = {".png", ".svg", ".xpm"};

        StringBuilder testScript = new StringBuilder();
        for (String theme : themes) {
            for (String size : sizes) {
                for (String cat : categories) {
                    for (String ext : exts) {
                        String path = chrootPath + "/usr/share/icons/" + theme + "/"
                                + size + "/" + cat + "/" + iconName + ext;
                        testScript.append("[ -f '").append(path).append("' ] && echo '")
                                .append(path).append("' && exit 0; ");
                    }
                }
            }
        }
        for (String ext : exts) {
            String path = chrootPath + "/usr/share/pixmaps/" + iconName + ext;
            testScript.append("[ -f '").append(path).append("' ] && echo '")
                    .append(path).append("' && exit 0; ");
        }

        String result = suExec(testScript.toString()).trim();
        return result.isEmpty() ? null : result;
    }

    private Bitmap loadIconViaSu(String path) {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{
                "su", "0", "cat", path
            });
            InputStream is = p.getInputStream();
            ByteArrayOutputStream baos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = is.read(buf)) > 0) baos.write(buf, 0, n);
            p.waitFor();
            byte[] data = baos.toByteArray();
            if (data.length > 0) {
                return BitmapFactory.decodeByteArray(data, 0, data.length);
            }
        } catch (IOException | InterruptedException e) {
            Log.w(TAG, "Failed to load icon: " + path);
        }
        return null;
    }

    private void launchApp(DesktopEntry entry) {
        String chrootPath = WaylandConfig.getChrootPath(this);
        String exec = entry.exec;

        if (entry.terminal) {
            exec = "foot -e " + exec;
        }

        Log.i(TAG, "Launching: " + entry.name + " exec=" + exec);
        Toast.makeText(this, "Launching " + entry.name + "...", Toast.LENGTH_SHORT).show();

        final String cmd = exec;
        new Thread(() -> {
            try {
                // Minimal env — Wayland vars come from /etc/profile.d/wayland.sh
                String fullCmd = "chroot " + chrootPath + " /usr/bin/env"
                    + " HOME=/root"
                    + " PATH=/usr/bin:/bin:/usr/sbin:/sbin"
                    + " /bin/bash -lc 'dbus-run-session " + cmd.replace("'", "'\\''") + "'";
                Log.i(TAG, "Full command: su 0 sh -c '" + fullCmd + "'");
                Process p = Runtime.getRuntime().exec(new String[]{
                    "su", "0", "sh", "-c", fullCmd
                });
                p.waitFor();
            } catch (IOException | InterruptedException e) {
                Log.e(TAG, "Failed to launch " + entry.name, e);
            }
        }).start();
    }

    static class DesktopEntry {
        String name;
        String exec;
        String iconName;
        String iconPath;
        String desktopFile;
        boolean noDisplay;
        boolean terminal;
        Bitmap icon;

        @Override
        public String toString() { return name; }
    }

    static class ProcessEntry {
        String pid;
        String ppid;
        String name;
        String cmdline;
        int depth; // tree depth for display indentation
        boolean isLeader; // true if this is a process group leader (no parent in chroot)
        List<ProcessEntry> children = new ArrayList<>();

        @Override
        public String toString() { return name; }
    }
}
