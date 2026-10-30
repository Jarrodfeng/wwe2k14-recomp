// wwe2k14 Android port (Session 71, ANDROID_PLAN F1): the touch controls drawn over the game. TouchPad does the layout,
// drawing and touch logic; this view hands it a Canvas and the touches, and sends the pad state to the game library
// (src/android_touch.h: an SDL virtual pad). Shown always, never, or (automatic) only while no controller is
// connected - the launcher's choice; size, opacity and vibrate-on-press come from there too.
//
// Session 74: the controls' places come from touch_layout.txt in the app's files folder (TouchPad.layoutText), and
// startEdit() lets the player drag them (in a game: Back -> MOVE TOUCH CONTROLS; from the launcher: TouchEditActivity,
// where the view is standalone: always shown, no game to send the pad to). DONE writes the file, CANCEL leaves it.
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

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public class TouchOverlay extends View implements InputManager.InputDeviceListener {
    private static final String TAG = "wwe2k14";
    public static final int MODE_AUTO = 0, MODE_ALWAYS = 1, MODE_OFF = 2;

    // libmain.so (src/android_touch.h). nativeSetActive is false until the game's input system has started SDL's
    // joysticks; the view tries again until it takes.
    static native boolean nativeSetActive(boolean on);
    static native void nativeUpdate(int buttons, float lx, float ly, float rx, float ry, float lt, float rt);

    /** Told when the edit mode ends (saved = DONE, else CANCEL / Back). */
    public interface EditListener { void onEditEnded(boolean saved); }

    private final TouchPad pad = new TouchPad();
    private final int mode, size, opacity;
    private final boolean vibrate, standalone;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final CanvasPainter painter = new CanvasPainter();
    private boolean shown, active;
    private EditListener editListener;

    public TouchOverlay(Context context, int mode, int size, int opacity, boolean vibrate) {
        this(context, mode, size, opacity, vibrate, false);
    }

    /** standalone: the launcher's editor (always shown, never talks to the game library). */
    public TouchOverlay(Context context, int mode, int size, int opacity, boolean vibrate, boolean standalone) {
        super(context);
        this.mode = mode;
        this.size = size;
        this.opacity = opacity;
        this.vibrate = vibrate;
        this.standalone = standalone;
        pad.setLayoutText(readLayout(context));
        setFocusable(false);
        setVisibility(GONE);  // updateShown shows it when wanted
        if (!standalone) {
            InputManager im = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
            if (im != null) im.registerInputDeviceListener(this, handler);
        }
        updateShown();
    }

    public void release() {
        if (!standalone) {
            InputManager im = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
            if (im != null) im.unregisterInputDeviceListener(this);
        }
        handler.removeCallbacksAndMessages(null);
    }

    // ------------------------------------------------------------------ the saved layout and the edit mode

    /** The saved places (the launcher checks the same file: mobile_main.cpp TouchLayoutPath). */
    static File layoutFile(Context c) {
        File files = c.getExternalFilesDir(null);
        return new File(files != null ? files : c.getFilesDir(), "touch_layout.txt");
    }
    private static String readLayout(Context c) {
        File f = layoutFile(c);
        if (!f.isFile()) return null;
        try {
            return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        } catch (Exception e) {
            Log.w(TAG, "touch controls: cannot read " + f, e);
            return null;
        }
    }
    // The file replaced whole (a .part then a rename); no file when every control is at its default place.
    private void writeLayout() {
        File f = layoutFile(getContext());
        if (!pad.moved()) {
            if (f.isFile() && !f.delete()) Log.w(TAG, "touch controls: cannot delete " + f);
            Log.i(TAG, "touch controls: default places");
            return;
        }
        File part = new File(f.getPath() + ".part");
        try (FileOutputStream out = new FileOutputStream(part)) {
            out.write(pad.layoutText().getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            Log.w(TAG, "touch controls: cannot write " + part, e);
            return;
        }
        if (!part.renameTo(f)) Log.w(TAG, "touch controls: cannot rename " + part);
        else Log.i(TAG, "touch controls: layout saved\n" + pad.layoutText());
    }

    /** True while the controls are on the screen (the game's Back menu offers moving them only then). */
    public boolean controlsShown() { return shown && !portrait; }
    public boolean editing() { return pad.editing(); }

    /** The edit mode: drags move the controls; nothing reaches the game until it ends. */
    public void startEdit(EditListener listener) {
        if (pad.editing()) return;
        editListener = listener;
        pad.startEdit();
        sendPad();
        Log.i(TAG, "touch controls: edit mode");
        invalidate();
    }
    /** Ends the edit mode without keeping the changes (Back while editing). */
    public void cancelEdit() { endEdit(false); }

    private void endEdit(boolean save) {
        if (!pad.editing()) return;
        pad.endEdit(save);
        if (save) writeLayout();
        Log.i(TAG, "touch controls: edit mode " + (save ? "done" : "cancelled"));
        invalidate();
        updateShown();
        EditListener l = editListener;
        editListener = null;
        if (l != null) l.onEditEnded(save);
    }

    private void sendPad() {
        if (standalone) return;
        try {
            nativeUpdate(pad.buttons(), pad.lx(), pad.ly(), pad.rx(), pad.ry(), pad.lt(), pad.rt());
        } catch (UnsatisfiedLinkError ignored) {
        }
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
        // (Kept on the screen while editing, even if a controller connects meanwhile.)
        boolean want = standalone || pad.editing() || mode == MODE_ALWAYS || (mode == MODE_AUTO && !controllerConnected());
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
        if (standalone || shown == active) return;
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
        if (pad.editing()) return onEditTouch(e);
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
            sendPad();
            if (vibrate && (pad.buttons() & ~before) != 0) buzz();
            invalidate();
        }
        return true;
    }

    private boolean onEditTouch(MotionEvent e) {
        boolean redraw = false;
        int done = TouchPad.EDIT_NONE;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                redraw = pad.editDown(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) redraw |= pad.editMove(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                done = pad.editUp(e.getPointerId(i), e.getX(i), e.getY(i));
                redraw = true;
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < e.getPointerCount(); i++) pad.editUp(e.getPointerId(i), -1, -1);
                redraw = true;
                break;
            default:
                break;
        }
        if (done == TouchPad.EDIT_DONE) endEdit(true);
        else if (done == TouchPad.EDIT_CANCEL) endEdit(false);
        else if (redraw) invalidate();
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
