package org.lineageos.wayland;

import android.app.Service;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.util.Log;
import android.util.SparseArray;
import android.view.SurfaceControl;

/**
 * Bridges between the Wayland compositor (in SurfaceFlinger) and Android
 * Activities. The compositor calls createWindow() when a client creates
 * an xdg_toplevel; this service launches an Activity and reports the
 * window's SurfaceControl handle back via callback so the compositor
 * can reparent the Wayland layer under it.
 */
public class WaylandWindowService extends Service {
    private static final String TAG = "WaylandWindowService";
    static final String SERVICE_NAME = "wayland_window_manager";

    private static WaylandWindowService sInstance;

    private final SparseArray<WaylandWindowActivity> mWindows = new SparseArray<>();
    private final SparseArray<IWaylandWindowCallback> mCallbacks = new SparseArray<>();

    static WaylandWindowService getInstance() {
        return sInstance;
    }

    IWaylandWindowCallback getCallback(int layerId) {
        synchronized (mCallbacks) {
            return mCallbacks.get(layerId);
        }
    }

    private final IWaylandWindowManager.Stub mBinder = new IWaylandWindowManager.Stub() {
        @Override
        public void createWindow(int layerId, String title, String appId,
                                 int width, int height,
                                 IWaylandWindowCallback callback) {
            Log.i(TAG, "createWindow: layerId=" + layerId + " title=" + title
                    + " appId=" + appId + " size=" + width + "x" + height);

            synchronized (mCallbacks) {
                mCallbacks.put(layerId, callback);
            }

            Intent intent = new Intent(WaylandWindowService.this,
                    WaylandWindowActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                    | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);

            Bundle extras = new Bundle();
            extras.putInt(WaylandWindowActivity.EXTRA_LAYER_ID, layerId);
            extras.putString(WaylandWindowActivity.EXTRA_TITLE,
                    title != null ? title : (appId != null ? appId : "Wayland"));
            extras.putInt(WaylandWindowActivity.EXTRA_WIDTH, width);
            extras.putInt(WaylandWindowActivity.EXTRA_HEIGHT, height);
            intent.putExtras(extras);

            startActivity(intent);
        }

        @Override
        public void destroyWindow(int layerId) {
            Log.i(TAG, "destroyWindow: layerId=" + layerId);
            synchronized (mCallbacks) {
                mCallbacks.remove(layerId);
            }
            WaylandWindowActivity activity;
            synchronized (mWindows) {
                activity = mWindows.get(layerId);
                mWindows.remove(layerId);
            }
            if (activity != null) {
                activity.runOnUiThread(activity::finish);
            }
        }

        @Override
        public void setTitle(int layerId, String title) {
            WaylandWindowActivity activity;
            synchronized (mWindows) {
                activity = mWindows.get(layerId);
            }
            if (activity != null) {
                activity.updateTitle(title);
            }
        }

        @Override
        public void sendPointerMotion(int layerId, long timeMs, float x, float y) {
            // Forward directly to compositor callback.
            IWaylandWindowCallback callback;
            synchronized (mCallbacks) {
                callback = mCallbacks.get(layerId);
            }
            if (callback != null) {
                try {
                    callback.onPointerMotion(layerId, timeMs, x, y);
                } catch (RemoteException e) {
                    Log.w(TAG, "Failed to forward pointer motion", e);
                }
            }
        }

        @Override
        public void sendPointerButton(int layerId, long timeMs, int button, boolean pressed) {
            IWaylandWindowCallback callback;
            synchronized (mCallbacks) {
                callback = mCallbacks.get(layerId);
            }
            if (callback != null) {
                try {
                    callback.onPointerButton(layerId, timeMs, button, pressed);
                } catch (RemoteException e) {
                    Log.w(TAG, "Failed to forward pointer button", e);
                }
            }
        }

        @Override
        public void sendKey(int layerId, long timeMs, int evdevKey, boolean pressed) {
            IWaylandWindowCallback callback;
            synchronized (mCallbacks) {
                callback = mCallbacks.get(layerId);
            }
            if (callback != null) {
                try {
                    callback.onKey(layerId, timeMs, evdevKey, pressed);
                } catch (RemoteException e) {
                    Log.w(TAG, "Failed to forward key", e);
                }
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        sInstance = this;
        ServiceManager.addService(SERVICE_NAME, mBinder);
        Log.i(TAG, "Registered as " + SERVICE_NAME);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        sInstance = null;
        super.onDestroy();
    }

    void registerActivity(int layerId, WaylandWindowActivity activity) {
        synchronized (mWindows) {
            mWindows.put(layerId, activity);
        }
    }

    /**
     * Called by WaylandWindowActivity when its SurfaceView's Surface is ready.
     * Reports the SurfaceControl handle back to the compositor via callback.
     */
    void onWindowSurfaceReady(int layerId, SurfaceControl sc) {
        Log.i(TAG, "onWindowSurfaceReady: layerId=" + layerId + " sc=" + sc);
        IWaylandWindowCallback callback;
        synchronized (mCallbacks) {
            callback = mCallbacks.get(layerId);
        }
        if (callback != null) {
            try {
                // Extract the layer handle IBinder from the SurfaceControl.
                android.os.Parcel p = android.os.Parcel.obtain();
                sc.writeToParcel(p, 0);
                p.setDataPosition(0);
                p.readInt(); // width
                p.readInt(); // height
                p.readInt(); // hasObject
                p.readStrongBinder(); // client binder
                IBinder handle = p.readStrongBinder(); // layer handle
                p.recycle();

                callback.onWindowReady(layerId, handle);
            } catch (RemoteException e) {
                Log.e(TAG, "Failed to notify compositor of window ready", e);
            }
        }
    }

    void onWindowDestroyed(int layerId) {
        IWaylandWindowCallback callback;
        synchronized (mCallbacks) {
            callback = mCallbacks.get(layerId);
            mCallbacks.remove(layerId);
        }
        synchronized (mWindows) {
            mWindows.remove(layerId);
        }
        if (callback != null) {
            try {
                callback.onWindowClosed(layerId);
            } catch (RemoteException e) {
                Log.e(TAG, "Failed to notify compositor of window close", e);
            }
        }
    }
}
