package org.lineageos.wayland;

import android.app.Activity;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.RemoteException;
import android.util.Log;
import android.util.SparseArray;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceControl;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.text.InputType;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import java.nio.charset.StandardCharsets;

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

    // Linux input event codes (from linux/input-event-codes.h)
    private static final int BTN_LEFT = 0x110;
    private static final int BTN_RIGHT = 0x111;
    private static final int BTN_MIDDLE = 0x112;
    private static final int KEY_ENTER = 28;
    private static final int KEY_LEFT = 105;
    private static final int KEY_RIGHT = 106;
    private static final int KEY_LEFTSHIFT = 42;
    private static final int KEY_HOME = 102;
    private static final int KEY_END = 107;

    private int mLayerId;
    private SurfaceView mSurfaceView;
    private View mRootView;
    private ImageButton mKbButton;
    private View mImeAnchor;
    private boolean mImeVisible;
    private int mLastReportedHeight = 0;

    // Text input protocol state.
    private boolean mTextInputActive = false;
    private int mTextInputType = InputType.TYPE_NULL;
    private String mSurroundingText = "";
    private int mSurroundingCursorByte = 0;
    private int mSurroundingAnchorByte = 0;

    // Dialog sub-windows hosted by this Activity
    private final SparseArray<DialogPanel> mDialogPanels = new SparseArray<>();

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

        // Hidden view to anchor the IME.
        // When text input protocol is active, provides a rich InputConnection
        // that forwards composed text to the compositor.
        // When inactive, uses TYPE_NULL for raw key events.
        mImeAnchor = new View(this) {
            @Override
            public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
                if (mTextInputActive) {
                    outAttrs.inputType = mTextInputType;
                    outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN;
                    return new WaylandInputConnection(this);
                }
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

        // Remove all dialog sub-windows hosted by this Activity
        WaylandWindowService service = WaylandWindowService.getInstance();
        for (int i = mDialogPanels.size() - 1; i >= 0; i--) {
            DialogPanel panel = mDialogPanels.valueAt(i);
            try {
                getWindowManager().removeView(panel.rootView);
            } catch (Exception e) {
                Log.w(TAG, "Failed to remove dialog panel on destroy", e);
            }
            if (service != null) {
                service.unregisterDialogHost(panel.layerId);
            }
        }
        mDialogPanels.clear();

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

    // --- Text input protocol methods (called from WaylandWindowService) ---

    void showTextInput(int contentHint, int contentPurpose,
                        int cursorX, int cursorY, int cursorW, int cursorH) {
        runOnUiThread(() -> {
            mTextInputActive = true;
            mTextInputType = mapContentType(contentHint, contentPurpose);
            Log.i(TAG, "showTextInput: hint=0x" + Integer.toHexString(contentHint)
                    + " purpose=" + contentPurpose + " inputType=0x"
                    + Integer.toHexString(mTextInputType));
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null) {
                mImeAnchor.requestFocus();
                imm.restartInput(mImeAnchor);
                imm.showSoftInput(mImeAnchor, InputMethodManager.SHOW_FORCED);
            }
        });
    }

    void hideTextInput() {
        runOnUiThread(() -> {
            mTextInputActive = false;
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.hideSoftInputFromWindow(mImeAnchor.getWindowToken(), 0);
            }
        });
    }

    void updateSurroundingText(String text, int cursorByte, int anchorByte) {
        runOnUiThread(() -> {
            mSurroundingText = text != null ? text : "";
            mSurroundingCursorByte = cursorByte;
            mSurroundingAnchorByte = anchorByte;
        });
    }

    void updateCursorRectangle(int x, int y, int w, int h) {
        // Could be used for InputMethodManager.updateCursorAnchorInfo in the future.
    }

    private static int mapContentType(int hint, int purpose) {
        int inputType;
        switch (purpose) {
            case 2: // digits
                inputType = InputType.TYPE_CLASS_NUMBER;
                break;
            case 3: // number
                inputType = InputType.TYPE_CLASS_NUMBER
                        | InputType.TYPE_NUMBER_FLAG_DECIMAL
                        | InputType.TYPE_NUMBER_FLAG_SIGNED;
                break;
            case 4: // phone
                inputType = InputType.TYPE_CLASS_PHONE;
                break;
            case 5: // url
                inputType = InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_VARIATION_URI;
                break;
            case 6: // email
                inputType = InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS;
                break;
            case 8: // password
                inputType = InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_VARIATION_PASSWORD;
                break;
            case 9: // pin
                inputType = InputType.TYPE_CLASS_NUMBER
                        | InputType.TYPE_NUMBER_VARIATION_PASSWORD;
                break;
            case 13: // terminal
                inputType = InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
                break;
            default:
                inputType = InputType.TYPE_CLASS_TEXT;
                break;
        }
        if ((hint & 0x1) != 0)
            inputType |= InputType.TYPE_TEXT_FLAG_AUTO_COMPLETE;
        if ((hint & 0x4) != 0)
            inputType |= InputType.TYPE_TEXT_FLAG_CAP_SENTENCES;
        if ((hint & 0x40) != 0)
            inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
        if ((hint & 0x200) != 0)
            inputType |= InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        return inputType;
    }

    private static int utf8ByteToCharOffset(String text, int byteOffset) {
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        if (byteOffset <= 0) return 0;
        if (byteOffset >= bytes.length) return text.length();
        String prefix = new String(bytes, 0, byteOffset, StandardCharsets.UTF_8);
        return prefix.length();
    }

    private static int charToUtf8ByteOffset(String text, int charOffset) {
        if (charOffset <= 0) return 0;
        if (charOffset >= text.length()) return text.getBytes(StandardCharsets.UTF_8).length;
        String prefix = text.substring(0, charOffset);
        return prefix.getBytes(StandardCharsets.UTF_8).length;
    }

    private class WaylandInputConnection extends BaseInputConnection {
        WaylandInputConnection(View view) {
            super(view, true);
        }

        @Override
        public CharSequence getTextBeforeCursor(int n, int flags) {
            int cursorChar = utf8ByteToCharOffset(mSurroundingText, mSurroundingCursorByte);
            int start = Math.max(0, cursorChar - n);
            return mSurroundingText.substring(start, cursorChar);
        }

        @Override
        public CharSequence getTextAfterCursor(int n, int flags) {
            int cursorChar = utf8ByteToCharOffset(mSurroundingText, mSurroundingCursorByte);
            int end = Math.min(mSurroundingText.length(), cursorChar + n);
            return mSurroundingText.substring(cursorChar, end);
        }

        @Override
        public CharSequence getSelectedText(int flags) {
            int cursorChar = utf8ByteToCharOffset(mSurroundingText, mSurroundingCursorByte);
            int anchorChar = utf8ByteToCharOffset(mSurroundingText, mSurroundingAnchorByte);
            int start = Math.min(cursorChar, anchorChar);
            int end = Math.max(cursorChar, anchorChar);
            if (start == end) return null;
            return mSurroundingText.substring(start, end);
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            IWaylandWindowCallback callback = getCallback();
            if (callback != null) {
                try {
                    callback.onCommitString(mLayerId, text.toString());
                } catch (RemoteException e) {
                    Log.w(TAG, "Failed to send commitText", e);
                }
            }
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            IWaylandWindowCallback callback = getCallback();
            if (callback != null) {
                try {
                    String t = text.toString();
                    int byteLen = t.getBytes(StandardCharsets.UTF_8).length;
                    callback.onPreeditString(mLayerId, t, byteLen, byteLen);
                } catch (RemoteException e) {
                    Log.w(TAG, "Failed to send setComposingText", e);
                }
            }
            return true;
        }

        @Override
        public boolean finishComposingText() {
            IWaylandWindowCallback callback = getCallback();
            if (callback != null) {
                try {
                    callback.onFinishComposingText(mLayerId);
                } catch (RemoteException e) {
                    Log.w(TAG, "Failed to send finishComposingText", e);
                }
            }
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            IWaylandWindowCallback callback = getCallback();
            if (callback != null) {
                try {
                    int cursorChar = utf8ByteToCharOffset(mSurroundingText, mSurroundingCursorByte);
                    int beforeStart = Math.max(0, cursorChar - beforeLength);
                    int afterEnd = Math.min(mSurroundingText.length(), cursorChar + afterLength);
                    int beforeBytes = charToUtf8ByteOffset(mSurroundingText, cursorChar)
                            - charToUtf8ByteOffset(mSurroundingText, beforeStart);
                    int afterBytes = charToUtf8ByteOffset(mSurroundingText, afterEnd)
                            - charToUtf8ByteOffset(mSurroundingText, cursorChar);
                    callback.onDeleteSurroundingText(mLayerId, beforeBytes, afterBytes);
                } catch (RemoteException e) {
                    Log.w(TAG, "Failed to send deleteSurroundingText", e);
                }
            }
            return true;
        }

        @Override
        public boolean setSelection(int start, int end) {
            IWaylandWindowCallback callback = getCallback();
            if (callback == null) return true;

            int cursorChar = utf8ByteToCharOffset(mSurroundingText, mSurroundingCursorByte);

            // Update local state immediately so subsequent getTextBeforeCursor/
            // getTextAfterCursor calls from the IME see the new position before
            // the Wayland round-trip completes.
            mSurroundingCursorByte = charToUtf8ByteOffset(mSurroundingText, start);
            mSurroundingAnchorByte = charToUtf8ByteOffset(mSurroundingText, end);

            try {
                long time = System.currentTimeMillis();
                if (start == end) {
                    // Simple cursor movement — synthesize arrow key presses.
                    int delta = start - cursorChar;
                    int key = delta > 0 ? KEY_RIGHT : KEY_LEFT;
                    for (int i = 0; i < Math.abs(delta); i++) {
                        callback.onKey(mLayerId, time, key, true);
                        callback.onKey(mLayerId, time, key, false);
                    }
                } else {
                    // Selection — move to start, then shift+arrow to end.
                    int moveDelta = start - cursorChar;
                    int moveKey = moveDelta > 0 ? KEY_RIGHT : KEY_LEFT;
                    for (int i = 0; i < Math.abs(moveDelta); i++) {
                        callback.onKey(mLayerId, time, moveKey, true);
                        callback.onKey(mLayerId, time, moveKey, false);
                    }
                    // Hold shift and arrow to select.
                    int selectDelta = end - start;
                    int selectKey = selectDelta > 0 ? KEY_RIGHT : KEY_LEFT;
                    callback.onKey(mLayerId, time, KEY_LEFTSHIFT, true);
                    for (int i = 0; i < Math.abs(selectDelta); i++) {
                        callback.onKey(mLayerId, time, selectKey, true);
                        callback.onKey(mLayerId, time, selectKey, false);
                    }
                    callback.onKey(mLayerId, time, KEY_LEFTSHIFT, false);
                }
            } catch (RemoteException e) {
                Log.w(TAG, "Failed to send setSelection", e);
            }
            return true;
        }

        @Override
        public boolean performEditorAction(int editorAction) {
            IWaylandWindowCallback callback = getCallback();
            if (callback != null) {
                try {
                    long time = System.currentTimeMillis();
                    callback.onKey(mLayerId, time, KEY_ENTER, true);
                    callback.onKey(mLayerId, time, KEY_ENTER, false);
                } catch (RemoteException e) {
                    Log.w(TAG, "Failed to send performEditorAction", e);
                }
            }
            return true;
        }
    }

    void updateTitle(String title) {
        runOnUiThread(() -> {
            setTitle(title);
            setTaskDescription(new android.app.ActivityManager.TaskDescription(title));
        });
    }

    // --- Dialog sub-window support ---

    private static class DialogPanel {
        final int layerId;
        final View rootView;
        final SurfaceView surfaceView;

        DialogPanel(int layerId, View rootView, SurfaceView surfaceView) {
            this.layerId = layerId;
            this.rootView = rootView;
            this.surfaceView = surfaceView;
        }
    }

    /**
     * Create a TYPE_APPLICATION_PANEL sub-window for a dialog (child toplevel).
     * The panel floats above this Activity, has no dimming, and receives its own
     * input events routed to the dialog's layerId.
     * Must be called on the UI thread.
     */
    void addDialogWindow(int dialogLayerId, String title, int width, int height) {
        Log.i(TAG, "addDialogWindow: dialogLayerId=" + dialogLayerId
                + " title=" + title + " size=" + width + "x" + height
                + " on host=" + mLayerId);

        // Remove existing panel with same layerId if any (e.g. popup recreated)
        removeDialogWindow(dialogLayerId);

        // Container: FrameLayout wrapping a SurfaceView
        FrameLayout container = new FrameLayout(this);
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        container.addView(sv, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // Forward touch events on the panel to the dialog's layerId
        container.setOnTouchListener((v, event) -> {
            IWaylandWindowCallback callback = getCallbackFor(dialogLayerId);
            if (callback == null) return false;

            long timeMs = event.getEventTime();
            // Coordinates are relative to the panel view
            float x = event.getX();
            float y = event.getY();

            try {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        callback.onPointerMotion(dialogLayerId, timeMs, x, y);
                        callback.onPointerButton(dialogLayerId, timeMs, BTN_LEFT, true);
                        break;
                    case MotionEvent.ACTION_MOVE:
                        callback.onPointerMotion(dialogLayerId, timeMs, x, y);
                        break;
                    case MotionEvent.ACTION_UP:
                        callback.onPointerMotion(dialogLayerId, timeMs, x, y);
                        callback.onPointerButton(dialogLayerId, timeMs, BTN_LEFT, false);
                        break;
                    case MotionEvent.ACTION_CANCEL:
                        callback.onPointerButton(dialogLayerId, timeMs, BTN_LEFT, false);
                        break;
                }
            } catch (RemoteException e) {
                Log.w(TAG, "Failed to forward dialog touch", e);
            }
            return true;
        });

        // Forward key events on the panel to the dialog's layerId
        container.setFocusable(true);
        container.setFocusableInTouchMode(true);
        container.setOnKeyListener((v, keyCode, event) -> {
            IWaylandWindowCallback callback = getCallbackFor(dialogLayerId);
            if (callback == null) return false;

            int evdevKey = androidKeyToEvdev(keyCode, event.getScanCode());
            if (evdevKey < 0) return false;

            try {
                boolean pressed = event.getAction() == KeyEvent.ACTION_DOWN;
                if (event.getAction() == KeyEvent.ACTION_DOWN
                        || event.getAction() == KeyEvent.ACTION_UP) {
                    callback.onKey(dialogLayerId, event.getEventTime(), evdevKey, pressed);
                    return true;
                }
            } catch (RemoteException e) {
                Log.w(TAG, "Failed to forward dialog key", e);
            }
            return false;
        });

        // When the SurfaceView is ready, report to compositor for reparenting
        sv.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                SurfaceControl sc = sv.getSurfaceControl();
                if (sc != null && sc.isValid()) {
                    WaylandWindowService service = WaylandWindowService.getInstance();
                    if (service != null) {
                        service.onWindowSurfaceReady(dialogLayerId, sc);
                    }
                }
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                IWaylandWindowCallback callback = getCallbackFor(dialogLayerId);
                if (callback != null) {
                    try {
                        callback.onWindowResized(dialogLayerId, w, h);
                    } catch (RemoteException e) {
                        Log.w(TAG, "Failed to forward dialog resize", e);
                    }
                }
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {}
        });

        // Use WRAP_CONTENT so the panel sizes to fit the dialog.
        // The Wayland client controls its own size; on first buffer commit
        // we'll know the actual dimensions and can update LayoutParams.
        // For now, use the display dimensions as a reasonable default.
        Rect displayBounds = getWindowManager().getCurrentWindowMetrics().getBounds();
        int panelW = width > 0 ? width : displayBounds.width();
        int panelH = height > 0 ? height : displayBounds.height();

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                panelW, panelH,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.CENTER;
        lp.token = getWindow().getDecorView().getWindowToken();
        lp.setTitle("WaylandDialog:" + dialogLayerId);

        getWindowManager().addView(container, lp);
        container.requestFocus();

        DialogPanel panel = new DialogPanel(dialogLayerId, container, sv);
        mDialogPanels.put(dialogLayerId, panel);

        WaylandWindowService service = WaylandWindowService.getInstance();
        if (service != null) {
            service.registerDialogHost(dialogLayerId, this);
        }

        Log.i(TAG, "Dialog panel added: layerId=" + dialogLayerId + " size=" + panelW + "x" + panelH);
    }

    /**
     * Create a TYPE_APPLICATION_SUB_PANEL sub-window for a popup (menu/tooltip).
     * Positioned relative to the parent surface at (popupX, popupY).
     * Must be called on the UI thread.
     */
    void addPopupWindow(int popupLayerId, int width, int height, int popupX, int popupY,
                        int parentLayerId) {
        Log.i(TAG, "addPopupWindow: popupLayerId=" + popupLayerId
                + " pos=" + popupX + "," + popupY
                + " size=" + width + "x" + height
                + " parent=" + parentLayerId + " on host=" + mLayerId);

        // Remove existing panel with same layerId if any
        removeDialogWindow(popupLayerId);

        FrameLayout container = new FrameLayout(this);
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        container.addView(sv, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // Forward touch events to the popup's layerId
        container.setOnTouchListener((v, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_OUTSIDE) {
                // Touch outside the popup -> dismiss it
                IWaylandWindowCallback callback = getCallbackFor(popupLayerId);
                if (callback != null) {
                    try {
                        callback.onWindowClosed(popupLayerId);
                    } catch (RemoteException e) {
                        Log.w(TAG, "Failed to send popup dismiss", e);
                    }
                }
                return true;
            }

            IWaylandWindowCallback callback = getCallbackFor(popupLayerId);
            if (callback == null) return false;

            long timeMs = event.getEventTime();
            float x = event.getX();
            float y = event.getY();

            try {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        callback.onPointerMotion(popupLayerId, timeMs, x, y);
                        callback.onPointerButton(popupLayerId, timeMs, BTN_LEFT, true);
                        break;
                    case MotionEvent.ACTION_MOVE:
                        callback.onPointerMotion(popupLayerId, timeMs, x, y);
                        break;
                    case MotionEvent.ACTION_UP:
                        callback.onPointerMotion(popupLayerId, timeMs, x, y);
                        callback.onPointerButton(popupLayerId, timeMs, BTN_LEFT, false);
                        break;
                    case MotionEvent.ACTION_CANCEL:
                        callback.onPointerButton(popupLayerId, timeMs, BTN_LEFT, false);
                        break;
                }
            } catch (RemoteException e) {
                Log.w(TAG, "Failed to forward popup touch", e);
            }
            return true;
        });

        sv.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                SurfaceControl sc = sv.getSurfaceControl();
                if (sc != null && sc.isValid()) {
                    WaylandWindowService service = WaylandWindowService.getInstance();
                    if (service != null) {
                        service.onWindowSurfaceReady(popupLayerId, sc);
                    }
                }
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                IWaylandWindowCallback callback = getCallbackFor(popupLayerId);
                if (callback != null) {
                    try {
                        callback.onWindowResized(popupLayerId, w, h);
                    } catch (RemoteException e) {
                        Log.w(TAG, "Failed to forward popup resize", e);
                    }
                }
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {}
        });

        // Position the popup relative to the parent's content area.
        // If the parent is a dialog panel, use its SurfaceView position;
        // otherwise use the main window's SurfaceView.
        int[] svLoc = new int[2];
        DialogPanel parentPanel = mDialogPanels.get(parentLayerId);
        if (parentPanel != null) {
            parentPanel.surfaceView.getLocationOnScreen(svLoc);
        } else {
            mSurfaceView.getLocationOnScreen(svLoc);
        }

        int panelW = width > 0 ? width : 200;
        int panelH = height > 0 ? height : 200;

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                panelW, panelH,
                WindowManager.LayoutParams.TYPE_APPLICATION_SUB_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.LEFT;
        lp.x = svLoc[0] + popupX;
        lp.y = svLoc[1] + popupY;
        lp.token = getWindow().getDecorView().getWindowToken();
        lp.setTitle("WaylandPopup:" + popupLayerId);

        getWindowManager().addView(container, lp);

        DialogPanel panel = new DialogPanel(popupLayerId, container, sv);
        mDialogPanels.put(popupLayerId, panel);

        WaylandWindowService service = WaylandWindowService.getInstance();
        if (service != null) {
            service.registerDialogHost(popupLayerId, this);
        }

        Log.i(TAG, "Popup panel added: layerId=" + popupLayerId
                + " at " + lp.x + "," + lp.y + " size=" + panelW + "x" + panelH);
    }

    /**
     * Remove a dialog sub-window. Must be called on the UI thread.
     */
    void removeDialogWindow(int dialogLayerId) {
        DialogPanel panel = mDialogPanels.get(dialogLayerId);
        if (panel != null) {
            getWindowManager().removeView(panel.rootView);
            mDialogPanels.remove(dialogLayerId);
            Log.i(TAG, "Dialog panel removed: layerId=" + dialogLayerId);
        }

        WaylandWindowService service = WaylandWindowService.getInstance();
        if (service != null) {
            service.unregisterDialogHost(dialogLayerId);
        }
    }

    private IWaylandWindowCallback getCallback() {
        return getCallbackFor(mLayerId);
    }

    private static IWaylandWindowCallback getCallbackFor(int layerId) {
        WaylandWindowService service = WaylandWindowService.getInstance();
        return service != null ? service.getCallback(layerId) : null;
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
