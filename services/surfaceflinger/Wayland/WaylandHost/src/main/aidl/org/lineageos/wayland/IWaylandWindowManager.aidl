package org.lineageos.wayland;

import org.lineageos.wayland.IWaylandWindowCallback;

interface IWaylandWindowManager {
    // Called by the compositor when an xdg_toplevel is created.
    // The callback will be invoked when the Activity's window is ready,
    // providing the window's SurfaceControl handle for reparenting.
    void createWindow(int layerId, String title, String appId,
                      int width, int height, IWaylandWindowCallback callback);

    // Called when the xdg_toplevel is destroyed.
    void destroyWindow(int layerId);

    // Called when the client sets a new title.
    void setTitle(int layerId, String title);

    // Input events forwarded from Activity to compositor.
    void sendPointerMotion(int layerId, long timeMs, float x, float y);
    void sendPointerButton(int layerId, long timeMs, int button, boolean pressed);
    void sendKey(int layerId, long timeMs, int evdevKey, boolean pressed);
}
