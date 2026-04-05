package org.lineageos.wayland;

interface IWaylandWindowCallback {
    // Called when the Activity's window Surface is ready.
    // windowHandle is the IBinder token of the window's SurfaceControl layer,
    // suitable for use as a reparent target.
    void onWindowReady(int layerId, IBinder windowHandle);

    // Called when the Activity window is destroyed.
    void onWindowClosed(int layerId);

    // Called when the Activity's content area changes size.
    void onWindowResized(int layerId, int width, int height);

    // Input events forwarded from Activity to compositor.
    void onPointerMotion(int layerId, long timeMs, float x, float y);
    void onPointerButton(int layerId, long timeMs, int button, boolean pressed);
    void onKey(int layerId, long timeMs, int evdevKey, boolean pressed);
}
