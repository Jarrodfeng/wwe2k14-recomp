// wwe2k14 Android port (Session 71, ANDROID_PLAN F1): the touch controls drawn over the game. TouchPad does the layout,
// drawing and touch logic; this view hands it a Canvas and the touches, and sends the pad state to the game library
// (src/android_touch.h: an SDL virtual pad). Shown always, never, or (automatic) only while no controller is
// connected - the launcher's choice; size, opacity and vibrate-on-press come from there too.
package io.github.jarrodfeng.wwe2k14;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

public class TouchOverlay extends View implements InputManager.InputDeviceListener {
    private static final String TAG = "wwe2k14";
    public static final int MODE_AUTO = 0, MODE_ALWAYS = 1, MODE_OFF = 2;

    // libmain.so (src/android_touch.h). nativeSetActive is false until the game's input system has started SDL's
    // joysticks; the view tries again until it takes.
    static native boolean nativeSetActive(boolean on);
    static native void nativeUpdate(int buttons, float lx, float ly, float rx, float ry, float lt, float rt);

    private final TouchPad pad = new TouchPad();
    private final int mode, size, opacity;
    private final boolean vibrate;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final CanvasPainter painter = new CanvasPainter();
    private boolean shown, active;

    public TouchOverlay(Context context, int mode, int size, int opacity, boolean vibrate) {
        super(context);
        this.mode = mode;
        this.size = size;
        this.opacity = opacity;
        this.vibrate = vibrate;
        setFocusable(false);
        setVisibility(GONE);  // updateShown shows it when wanted
        InputManager im = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
        if (im != null) im.registerInputDeviceListener(this, handler);
        updateShown();
    }

    public void release() {
        InputManager im = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
        if (im != null) im.unregisterInputDeviceListener(this);
        handler.removeCallbacksAndMessages(null);
    }

    /** A real controller (not the phone's own buttons, not a virtual device). */
    static boolean controllerConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return true;
        }
        return false;
    }

    private void updateShown() {
        boolean want = mode == MODE_ALWAYS || (mode == MODE_AUTO && !controllerConnected());
        if (want != shown) {
            shown = want;
            setVisibility(shown ? VISIBLE : GONE);
            Log.i(TAG, "touch controls " + (shown ? "shown" : "hidden") + " (mode " + mode + ")");
            if (!shown) pad.cancel();
        }
        syncActive();
    }

    // Attach / remove the game's virtual pad to match; retried every second until the game's input is up.
    private final Runnable retry = this::syncActive;
    private void syncActive() {
        handler.removeCallbacks(retry);
        if (shown == active) return;
        try {
            if (nativeSetActive(shown) || !shown) active = shown;
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "touch controls: the game library is not loaded yet");
        }
        if (shown != active) handler.postDelayed(retry, 1000);
    }

    @Override public void onInputDeviceAdded(int id) { updateShown(); }
    @Override public void onInputDeviceRemoved(int id) { updateShown(); }
    @Override public void onInputDeviceChanged(int id) { updateShown(); }

    // The layout is for a landscape screen only; while the view is taller than wide (a moment during rotation) it draws
    // nothing and lets touches through.
    private boolean portrait;

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        portrait = h > w;
        if (portrait) pad.cancel();
        else pad.layout(w, h, size, opacity);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (portrait) return;
        painter.canvas = canvas;
        pad.draw(painter);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!shown || portrait) return false;
        int before = pad.buttons();
        boolean changed = false;
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                changed = pad.down(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) changed |= pad.move(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                changed = pad.up(e.getPointerId(e.getActionIndex()));
                break;
            case MotionEvent.ACTION_CANCEL:
                changed = pad.cancel();
                break;
            default:
                break;
        }
        if (changed) {
            if (!active) syncActive();
            try {
                nativeUpdate(pad.buttons(), pad.lx(), pad.ly(), pad.rx(), pad.ry(), pad.lt(), pad.rt());
            } catch (UnsatisfiedLinkError ignored) {
            }
            if (vibrate && (pad.buttons() & ~before) != 0) buzz();
            invalidate();
        }
        return true;
    }

    private void buzz() {
        Vibrator v = (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);
        if (v != null && v.hasVibrator()) v.vibrate(VibrationEffect.createOneShot(12, VibrationEffect.DEFAULT_AMPLITUDE));
    }

    /** TouchPad's drawing on an Android Canvas. */
    private static final class CanvasPainter implements TouchPad.Painter {
        Canvas canvas;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        CanvasPainter() {
            paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setStrokeJoin(Paint.Join.ROUND);
        }
        private void set(int argb, boolean fill, float stroke) {
            paint.setColor(argb);
            paint.setStyle(fill ? Paint.Style.FILL : Paint.Style.STROKE);
            paint.setStrokeWidth(stroke);
        }
        @Override public void circle(float cx, float cy, float r, int argb, boolean fill, float stroke) {
            set(argb, fill, stroke);
            canvas.drawCircle(cx, cy, r, paint);
        }
        @Override public void roundRect(float l, float t, float r, float b, float radius, int argb, boolean fill, float stroke) {
            set(argb, fill, stroke);
            rect.set(l, t, r, b);
            canvas.drawRoundRect(rect, radius, radius, paint);
        }
        @Override public void line(float x0, float y0, float x1, float y1, int argb, float stroke) {
            set(argb, false, stroke);
            canvas.drawLine(x0, y0, x1, y1, paint);
        }
        @Override public void text(String s, float cx, float cy, float size, int argb) {
            set(argb, true, 0);
            paint.setTextSize(size);
            Paint.FontMetrics m = paint.getFontMetrics();
            canvas.drawText(s, cx, cy - (m.ascent + m.descent) / 2, paint);
        }
    }
}
