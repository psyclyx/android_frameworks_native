package org.lineageos.wayland;

import android.app.Activity;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.RemoteException;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceControl;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.text.InputType;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.FrameLayout;
import android.widget.ImageButton;

/**
 * Represents one Wayland xdg_toplevel as an Android Activity.
 * Shows up in recents, taskbar, gets focus and input like a normal app.
 * Reports its window's SurfaceControl back to the compositor for reparenting.
 * Captures touch/key input and forwards to the compositor.
 * Has a keyboard toggle button for on-screen keyboard support.
 */
public class WaylandWindowActivity extends Activity {
    private static final String TAG = "WaylandWindowActivity";

    static final String EXTRA_LAYER_ID = "layer_id";
    static final String EXTRA_TITLE = "title";
    static final String EXTRA_WIDTH = "width";
    static final String EXTRA_HEIGHT = "height";

    // Linux input event codes for mouse buttons (from linux/input-event-codes.h)
    private static final int BTN_LEFT = 0x110;
    private static final int BTN_RIGHT = 0x111;
    private static final int BTN_MIDDLE = 0x112;

    private int mLayerId;
    private SurfaceView mSurfaceView;
    private View mRootView;
    private ImageButton mKbButton;
    private View mImeAnchor;
    private boolean mImeVisible;
    private int mLastReportedHeight = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        mLayerId = getIntent().getIntExtra(EXTRA_LAYER_ID, -1);
        String title = getIntent().getStringExtra(EXTRA_TITLE);

        if (title != null && !title.isEmpty()) {
            setTitle(title);
            setTaskDescription(new android.app.ActivityManager.TaskDescription(title));
        }

        // Root layout: FrameLayout with system insets
        FrameLayout root = new FrameLayout(this);
        root.setFitsSystemWindows(true);

        // SurfaceView fills the available area
        mSurfaceView = new SurfaceView(this);
        root.addView(mSurfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // Hidden view to anchor the IME — TYPE_NULL makes the keyboard send
        // raw KeyEvents instead of composing text, so onKeyDown/onKeyUp handle everything
        mImeAnchor = new View(this) {
            @Override
            public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
                outAttrs.inputType = InputType.TYPE_NULL;
                outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN;
                return new BaseInputConnection(this, false);
            }

            @Override
            public boolean onCheckIsTextEditor() {
                return true;
            }
        };
        mImeAnchor.setFocusable(true);
        mImeAnchor.setFocusableInTouchMode(true);
        FrameLayout.LayoutParams imeParams = new FrameLayout.LayoutParams(1, 1);
        root.addView(mImeAnchor, imeParams);

        // Keyboard toggle button — bottom-right corner
        mKbButton = new ImageButton(this);
        mKbButton.setImageResource(android.R.drawable.ic_dialog_dialer);
        mKbButton.setBackgroundColor(0x80000000);
        mKbButton.setPadding(16, 16, 16, 16);
        mKbButton.setAlpha(0.6f);
        FrameLayout.LayoutParams kbParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        kbParams.gravity = android.view.Gravity.BOTTOM | android.view.Gravity.END;
        kbParams.setMargins(0, 0, 16, 16);
        mKbButton.setLayoutParams(kbParams);
        mKbButton.setOnClickListener(v -> showKeyboard());
        mKbButton.setFocusable(false);
        mKbButton.setFocusableInTouchMode(false);
        root.addView(mKbButton);

        setContentView(root);
        mRootView = root;

        // Detect keyboard show/hide via layout changes to send resize configures
        // and sync button visibility with actual IME state
        root.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            Rect r = new Rect();
            root.getWindowVisibleDisplayFrame(r);
            int visibleHeight = r.height();

            // Detect IME state from height change (>20% shrink = keyboard visible)
            int fullHeight = root.getRootView().getHeight();
            boolean imeNowVisible = fullHeight > 0
                    && (fullHeight - visibleHeight) > fullHeight / 5;

            if (imeNowVisible != mImeVisible) {
                mImeVisible = imeNowVisible;
                mKbButton.setVisibility(mImeVisible ? View.GONE : View.VISIBLE);
            }

            if (mLastReportedHeight != 0 && visibleHeight != mLastReportedHeight) {
                IWaylandWindowCallback callback = getCallback();
                if (callback != null) {
                    try {
                        callback.onWindowResized(mLayerId, r.width(), visibleHeight);
                    } catch (RemoteException e) {
                        Log.w(TAG, "Failed to send keyboard resize", e);
                    }
                }
            }
            mLastReportedHeight = visibleHeight;
        });

        mSurfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                SurfaceControl sc = mSurfaceView.getSurfaceControl();
                if (sc != null && sc.isValid()) {
                    WaylandWindowService service = WaylandWindowService.getInstance();
                    if (service != null) {
                        service.onWindowSurfaceReady(mLayerId, sc);
                    }
                }
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format,
                                      int width, int height) {
                IWaylandWindowCallback callback = getCallback();
                if (callback != null) {
                    try {
                        callback.onWindowResized(mLayerId, width, height);
                    } catch (RemoteException e) {
                        Log.w(TAG, "Failed to forward resize", e);
                    }
                }
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
            }
        });

        WaylandWindowService service = WaylandWindowService.getInstance();
        if (service != null) {
            service.registerActivity(mLayerId, this);
        }

        Log.i(TAG, "onCreate: layerId=" + mLayerId + " title=" + title);
    }

    @Override
    protected void onDestroy() {
        Log.i(TAG, "onDestroy: layerId=" + mLayerId);
        WaylandWindowService service = WaylandWindowService.getInstance();
        if (service != null) {
            service.onWindowDestroyed(mLayerId);
        }
        super.onDestroy();
    }

    private void showKeyboard() {
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (imm == null) return;
        mImeAnchor.requestFocus();
        imm.showSoftInput(mImeAnchor, InputMethodManager.SHOW_FORCED);
    }

    private boolean isTouchOnButton(float x, float y) {
        if (mKbButton == null) return false;
        int[] loc = new int[2];
        mKbButton.getLocationOnScreen(loc);
        return x >= loc[0] && x <= loc[0] + mKbButton.getWidth()
            && y >= loc[1] && y <= loc[1] + mKbButton.getHeight();
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        // Let the button handle its own touches without forwarding to compositor
        if (isTouchOnButton(event.getRawX(), event.getRawY())) {
            return super.dispatchTouchEvent(event);
        }

        IWaylandWindowCallback callback = getCallback();
        if (callback == null) {
            return super.dispatchTouchEvent(event);
        }

        long timeMs = event.getEventTime();
        // Translate from window coordinates to SurfaceView-local coordinates
        // (the SurfaceView is inset by status bar / system bars due to fitsSystemWindows)
        int[] svLoc = new int[2];
        mSurfaceView.getLocationInWindow(svLoc);
        float x = event.getX() - svLoc[0];
        float y = event.getY() - svLoc[1];

        try {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    callback.onPointerMotion(mLayerId, timeMs, x, y);
                    callback.onPointerButton(mLayerId, timeMs, BTN_LEFT, true);
                    break;

                case MotionEvent.ACTION_MOVE:
                    callback.onPointerMotion(mLayerId, timeMs, x, y);
                    break;

                case MotionEvent.ACTION_UP:
                    callback.onPointerMotion(mLayerId, timeMs, x, y);
                    callback.onPointerButton(mLayerId, timeMs, BTN_LEFT, false);
                    break;

                case MotionEvent.ACTION_CANCEL:
                    callback.onPointerButton(mLayerId, timeMs, BTN_LEFT, false);
                    break;
            }
        } catch (RemoteException e) {
            Log.w(TAG, "Failed to forward touch event", e);
        }

        return super.dispatchTouchEvent(event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        IWaylandWindowCallback callback = getCallback();
        if (callback == null) return super.onGenericMotionEvent(event);

        if (event.getActionMasked() == MotionEvent.ACTION_HOVER_MOVE) {
            try {
                int[] svLoc = new int[2];
                mSurfaceView.getLocationInWindow(svLoc);
                callback.onPointerMotion(mLayerId, event.getEventTime(),
                        event.getX() - svLoc[0], event.getY() - svLoc[1]);
            } catch (RemoteException e) {
                Log.w(TAG, "Failed to forward hover event", e);
            }
            return true;
        }

        return super.onGenericMotionEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        IWaylandWindowCallback callback = getCallback();
        if (callback == null) return super.onKeyDown(keyCode, event);

        int evdevKey = androidKeyToEvdev(keyCode, event.getScanCode());
        if (evdevKey >= 0) {
            try {
                callback.onKey(mLayerId, event.getEventTime(), evdevKey, true);
            } catch (RemoteException e) {
                Log.w(TAG, "Failed to forward key down", e);
            }
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        IWaylandWindowCallback callback = getCallback();
        if (callback == null) return super.onKeyUp(keyCode, event);

        int evdevKey = androidKeyToEvdev(keyCode, event.getScanCode());
        if (evdevKey >= 0) {
            try {
                callback.onKey(mLayerId, event.getEventTime(), evdevKey, false);
            } catch (RemoteException e) {
                Log.w(TAG, "Failed to forward key up", e);
            }
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    void updateTitle(String title) {
        runOnUiThread(() -> {
            setTitle(title);
            setTaskDescription(new android.app.ActivityManager.TaskDescription(title));
        });
    }

    private IWaylandWindowCallback getCallback() {
        WaylandWindowService service = WaylandWindowService.getInstance();
        return service != null ? service.getCallback(mLayerId) : null;
    }

    /**
     * Map Android KeyEvent keyCode to Linux evdev key code.
     * If scanCode > 0 (hardware keyboard), use it directly.
     * Otherwise, map from Android keyCode.
     */
    private static int androidKeyToEvdev(int keyCode, int scanCode) {
        if (scanCode > 0) return scanCode;

        switch (keyCode) {
            case KeyEvent.KEYCODE_ESCAPE: return 1;
            case KeyEvent.KEYCODE_1: return 2;
            case KeyEvent.KEYCODE_2: return 3;
            case KeyEvent.KEYCODE_3: return 4;
            case KeyEvent.KEYCODE_4: return 5;
            case KeyEvent.KEYCODE_5: return 6;
            case KeyEvent.KEYCODE_6: return 7;
            case KeyEvent.KEYCODE_7: return 8;
            case KeyEvent.KEYCODE_8: return 9;
            case KeyEvent.KEYCODE_9: return 10;
            case KeyEvent.KEYCODE_0: return 11;
            case KeyEvent.KEYCODE_MINUS: return 12;
            case KeyEvent.KEYCODE_EQUALS: return 13;
            case KeyEvent.KEYCODE_DEL: return 14;
            case KeyEvent.KEYCODE_TAB: return 15;
            case KeyEvent.KEYCODE_Q: return 16;
            case KeyEvent.KEYCODE_W: return 17;
            case KeyEvent.KEYCODE_E: return 18;
            case KeyEvent.KEYCODE_R: return 19;
            case KeyEvent.KEYCODE_T: return 20;
            case KeyEvent.KEYCODE_Y: return 21;
            case KeyEvent.KEYCODE_U: return 22;
            case KeyEvent.KEYCODE_I: return 23;
            case KeyEvent.KEYCODE_O: return 24;
            case KeyEvent.KEYCODE_P: return 25;
            case KeyEvent.KEYCODE_LEFT_BRACKET: return 26;
            case KeyEvent.KEYCODE_RIGHT_BRACKET: return 27;
            case KeyEvent.KEYCODE_ENTER: return 28;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 29;
            case KeyEvent.KEYCODE_A: return 30;
            case KeyEvent.KEYCODE_S: return 31;
            case KeyEvent.KEYCODE_D: return 32;
            case KeyEvent.KEYCODE_F: return 33;
            case KeyEvent.KEYCODE_G: return 34;
            case KeyEvent.KEYCODE_H: return 35;
            case KeyEvent.KEYCODE_J: return 36;
            case KeyEvent.KEYCODE_K: return 37;
            case KeyEvent.KEYCODE_L: return 38;
            case KeyEvent.KEYCODE_SEMICOLON: return 39;
            case KeyEvent.KEYCODE_APOSTROPHE: return 40;
            case KeyEvent.KEYCODE_GRAVE: return 41;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 42;
            case KeyEvent.KEYCODE_BACKSLASH: return 43;
            case KeyEvent.KEYCODE_Z: return 44;
            case KeyEvent.KEYCODE_X: return 45;
            case KeyEvent.KEYCODE_C: return 46;
            case KeyEvent.KEYCODE_V: return 47;
            case KeyEvent.KEYCODE_B: return 48;
            case KeyEvent.KEYCODE_N: return 49;
            case KeyEvent.KEYCODE_M: return 50;
            case KeyEvent.KEYCODE_COMMA: return 51;
            case KeyEvent.KEYCODE_PERIOD: return 52;
            case KeyEvent.KEYCODE_SLASH: return 53;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 54;
            case KeyEvent.KEYCODE_ALT_LEFT: return 56;
            case KeyEvent.KEYCODE_SPACE: return 57;
            case KeyEvent.KEYCODE_CAPS_LOCK: return 58;
            case KeyEvent.KEYCODE_F1: return 59;
            case KeyEvent.KEYCODE_F2: return 60;
            case KeyEvent.KEYCODE_F3: return 61;
            case KeyEvent.KEYCODE_F4: return 62;
            case KeyEvent.KEYCODE_F5: return 63;
            case KeyEvent.KEYCODE_F6: return 64;
            case KeyEvent.KEYCODE_F7: return 65;
            case KeyEvent.KEYCODE_F8: return 66;
            case KeyEvent.KEYCODE_F9: return 67;
            case KeyEvent.KEYCODE_F10: return 68;
            case KeyEvent.KEYCODE_F11: return 87;
            case KeyEvent.KEYCODE_F12: return 88;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 100;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 97;
            case KeyEvent.KEYCODE_INSERT: return 110;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 111;
            case KeyEvent.KEYCODE_MOVE_HOME: return 102;
            case KeyEvent.KEYCODE_MOVE_END: return 107;
            case KeyEvent.KEYCODE_PAGE_UP: return 104;
            case KeyEvent.KEYCODE_PAGE_DOWN: return 109;
            case KeyEvent.KEYCODE_DPAD_UP: return 103;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 108;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 105;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 106;
            case KeyEvent.KEYCODE_META_LEFT: return 125;
            case KeyEvent.KEYCODE_META_RIGHT: return 126;
            default: return -1;
        }
    }
}
