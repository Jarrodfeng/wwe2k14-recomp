// wwe2k14 Android port (Session 71, ANDROID_PLAN F1): the touch controls' layout, drawing and touch logic. Plain Java
// with no Android types, so tools/android/touch_preview draws it and drives it with made-up touches on the PC; the
// Android side (TouchOverlay) only hands it a Painter and the touches.
//
// Layout (Jarrod's reference picture, 2026-10-28): left - LT in the top corner, LB inside it, the left stick upper
// left, the d-pad below and inward of it, L3 in the bottom corner; right - RT in the top corner, RB inside it, A B X Y
// (coloured rings: Y yellow, X blue, B red, A green), the right stick below and inward, R3 in the bottom corner; View
// and Start at the top centre ("Top centre is smart") (the picture's curved background bands left out at Jarrod's word). Positions are in
// units of the screen's height, measured from the nearer side edge, so the clusters stay at the edges on any width.
package io.github.jarrodfeng.wwe2k14;

public final class TouchPad {

    /** What TouchPad draws with (Android Canvas on the phone, Java2D in the preview). Colours are ARGB. */
    public interface Painter {
        void circle(float cx, float cy, float r, int argb, boolean fill, float stroke);
        void roundRect(float l, float t, float r, float b, float radius, int argb, boolean fill, float stroke);
        void line(float x0, float y0, float x1, float y1, int argb, float stroke);
        void text(String s, float cx, float cy, float size, int argb);
    }

    // SDL_GamepadButton numbers (src/android_touch.h): the bit of each button in buttons().
    public static final int A = 0, B = 1, X = 2, Y = 3, BACK = 4, START = 6, L3 = 7, R3 = 8, LB = 9, RB = 10,
            UP = 11, DOWN = 12, LEFT = 13, RIGHT = 14;
    // Controls that are not buttons in SDL's sense.
    private static final int LT = 100, RT = 101, LSTICK = 102, RSTICK = 103, DPAD = 104;

    private static final int GREEN = 0xFF6CC24A, RED = 0xFFE5533D, BLUE = 0xFF3B8EDE, YELLOW = 0xFFE8C23A;

    /** One control: where it is (pixels, set by layout()) and how it looks. */
    private static final class Control {
        final int id;
        final String label;
        final int color;
        float cx, cy, r;   // centre; r = radius (round controls) or half height (pills)
        float hw;          // pills: half width (0 = round)
        Control(int id, String label, int color) { this.id = id; this.label = label; this.color = color; }
        boolean hit(float x, float y, float slack) {
            if (hw > 0) return Math.abs(x - cx) <= hw * slack && Math.abs(y - cy) <= r * slack;
            float dx = x - cx, dy = y - cy, rr = r * slack;
            return dx * dx + dy * dy <= rr * rr;
        }
    }

    private final Control[] controls = {
        new Control(LT, "LT", 0), new Control(LB, "LB", 0), new Control(RT, "RT", 0), new Control(RB, "RB", 0),
        new Control(LSTICK, "", 0), new Control(RSTICK, "", 0), new Control(DPAD, "", 0),
        new Control(A, "A", GREEN), new Control(B, "B", RED), new Control(X, "X", BLUE), new Control(Y, "Y", YELLOW),
        new Control(L3, "L3", 0), new Control(R3, "R3", 0), new Control(BACK, "VIEW", 0), new Control(START, "START", 0),
    };

    private float width, height, scale = 1f, alpha = 0.7f;

    // Pad state.
    private int buttons;
    private float lx, ly, rx, ry, lt, rt;
    // Each finger (pointer id -> the control it went down on; 16 at most).
    private final int[] pointerControl = new int[16];
    private final float[] pointerX = new float[16], pointerY = new float[16];

    public TouchPad() {
        java.util.Arrays.fill(pointerControl, -1);
    }

    /** size: percent (60..150, the launcher's Size); opacity: percent (20..100). */
    public void layout(float w, float h, int sizePercent, int opacityPercent) {
        width = w;
        height = h;
        scale = Math.max(0.6f, Math.min(1.5f, sizePercent / 100f));
        alpha = Math.max(0.2f, Math.min(1f, opacityPercent / 100f));
        float H = h, s = scale;
        for (Control c : controls) {
            switch (c.id) {
                case LT: pill(c, left(0.12f), 0.20f, 0.075f * s, 0.045f * s); break;
                case LB: pill(c, left(0.40f), 0.20f, 0.075f * s, 0.045f * s); break;
                case RT: pill(c, right(0.12f), 0.20f, 0.075f * s, 0.045f * s); break;
                case RB: pill(c, right(0.40f), 0.20f, 0.075f * s, 0.045f * s); break;
                case LSTICK: round(c, left(0.27f), 0.47f, 0.135f * s); break;
                case RSTICK: round(c, right(0.60f), 0.74f, 0.135f * s); break;
                case DPAD: round(c, left(0.60f), 0.74f, 0.13f * s); break;
                case Y: round(c, right(0.27f), 0.47f - 0.125f * s, 0.058f * s); break;
                case X: round(c, right(0.27f) - 0.125f * s * H, 0.47f, 0.058f * s); break;
                case B: round(c, right(0.27f) + 0.125f * s * H, 0.47f, 0.058f * s); break;
                case A: round(c, right(0.27f), 0.47f + 0.125f * s, 0.058f * s); break;
                case L3: round(c, left(0.13f), 0.86f, 0.05f * s); break;
                case R3: round(c, right(0.13f), 0.86f, 0.05f * s); break;
                case BACK: pill(c, w / 2 - 0.13f * H, 0.08f, 0.075f * s, 0.035f * s); break;
                case START: pill(c, w / 2 + 0.13f * H, 0.08f, 0.075f * s, 0.035f * s); break;
                default: break;
            }
        }
    }
    private float left(float u) { return u * height; }
    private float right(float u) { return width - u * height; }
    private void round(Control c, float x, float yu, float ru) { c.cx = x; c.cy = yu * height; c.r = ru * height; c.hw = 0; }
    private void pill(Control c, float x, float yu, float hwu, float hhu) { c.cx = x; c.cy = yu * height; c.hw = hwu * height; c.r = hhu * height; }
    private Control find(int id) { for (Control c : controls) if (c.id == id) return c; return null; }

    // ------------------------------------------------------------------ touches

    /** A finger went down / moved (true = the pad state changed). */
    public boolean down(int pointer, float x, float y) {
        if (pointer < 0 || pointer >= pointerControl.length) return false;
        int best = -1;
        float bestD = Float.MAX_VALUE;
        // The nearest control within reach (sticks and the d-pad reach further: thumbs land loosely).
        for (int i = 0; i < controls.length; i++) {
            Control c = controls[i];
            float slack = (c.id == LSTICK || c.id == RSTICK) ? 1.5f : c.id == DPAD ? 1.35f : 1.35f;
            if (!c.hit(x, y, slack)) continue;
            float d = (x - c.cx) * (x - c.cx) + (y - c.cy) * (y - c.cy);
            if (d < bestD) { bestD = d; best = i; }
        }
        pointerControl[pointer] = best;
        pointerX[pointer] = x;
        pointerY[pointer] = y;
        return update();
    }
    public boolean move(int pointer, float x, float y) {
        if (pointer < 0 || pointer >= pointerControl.length || pointerControl[pointer] < 0) return false;
        pointerX[pointer] = x;
        pointerY[pointer] = y;
        return update();
    }
    public boolean up(int pointer) {
        if (pointer < 0 || pointer >= pointerControl.length) return false;
        pointerControl[pointer] = -1;
        return update();
    }
    /** All fingers gone (the view lost its touches). */
    public boolean cancel() {
        java.util.Arrays.fill(pointerControl, -1);
        return update();
    }

    /** Recomputes the pad from the fingers. True when something changed. */
    private boolean update() {
        int nb = 0;
        float nlx = 0, nly = 0, nrx = 0, nry = 0, nlt = 0, nrt = 0;
        for (int p = 0; p < pointerControl.length; p++) {
            if (pointerControl[p] < 0) continue;
            Control c = controls[pointerControl[p]];
            float x = pointerX[p], y = pointerY[p];
            switch (c.id) {
                case LSTICK: case RSTICK: {
                    float dx = (x - c.cx) / c.r, dy = (y - c.cy) / c.r;
                    float len = (float) Math.sqrt(dx * dx + dy * dy);
                    if (len > 1) { dx /= len; dy /= len; }
                    if (c.id == LSTICK) { nlx = dx; nly = dy; } else { nrx = dx; nry = dy; }
                    break;
                }
                case DPAD: {
                    float dx = x - c.cx, dy = y - c.cy;
                    float dead = c.r * 0.22f;
                    if (dx * dx + dy * dy < dead * dead) break;
                    double a = Math.toDegrees(Math.atan2(dy, dx));  // 0 = right, 90 = down
                    // Eight directions: each 45 degrees wide, the diagonals press both.
                    if (a > -67.5 && a < 67.5) nb |= 1 << RIGHT;
                    if (a > 22.5 && a < 157.5) nb |= 1 << DOWN;
                    if (a > 112.5 || a < -112.5) nb |= 1 << LEFT;
                    if (a > -157.5 && a < -22.5) nb |= 1 << UP;
                    break;
                }
                case LT: if (c.hit(x, y, 1.6f)) nlt = 1; break;
                case RT: if (c.hit(x, y, 1.6f)) nrt = 1; break;
                default:
                    // A button stays pressed while the finger is on it (sliding off lets go).
                    if (c.hit(x, y, 1.6f)) nb |= 1 << c.id;
                    break;
            }
        }
        boolean changed = nb != buttons || nlx != lx || nly != ly || nrx != rx || nry != ry || nlt != lt || nrt != rt;
        buttons = nb; lx = nlx; ly = nly; rx = nrx; ry = nry; lt = nlt; rt = nrt;
        return changed;
    }

    /** True when a finger is on a button that was not pressed before (for the vibrate-on-press setting). */
    public int buttons() { return buttons; }
    public float lx() { return lx; }
    public float ly() { return ly; }
    public float rx() { return rx; }
    public float ry() { return ry; }
    public float lt() { return lt; }
    public float rt() { return rt; }

    // ------------------------------------------------------------------ drawing

    private int a(int argb, float k) {
        int alpha8 = Math.round(((argb >>> 24) & 0xFF) * k * alpha);
        return (alpha8 << 24) | (argb & 0xFFFFFF);
    }

    public void draw(Painter p) {
        float H = height, stroke = Math.max(1.5f, 0.0055f * H * scale);
        // (No background bands: b44 drew rings of light from the bottom corners; Jarrod, 2026-10-28: "a little extreme
        // we should remove them".)
        int line = a(0xE6FFFFFF, 1), pressedFill = a(0x55FFFFFF, 1);
        for (Control c : controls) {
            switch (c.id) {
                case LSTICK: case RSTICK: {
                    boolean left = c.id == LSTICK;
                    float kx = left ? lx : rx, ky = left ? ly : ry;
                    p.circle(c.cx, c.cy, c.r, a(0x30FFFFFF, 1), true, 0);
                    p.circle(c.cx, c.cy, c.r, a(0x99FFFFFF, 1), false, stroke);
                    float kr = c.r * 0.45f;
                    float kcx = c.cx + kx * (c.r - kr * 0.6f), kcy = c.cy + ky * (c.r - kr * 0.6f);
                    p.circle(kcx, kcy, kr, a(0x66FFFFFF, 1), true, 0);
                    p.circle(kcx, kcy, kr, line, false, stroke * 1.4f);
                    break;
                }
                case DPAD: drawDpad(p, c, line, pressedFill, stroke); break;
                case A: case B: case X: case Y: {
                    boolean on = (buttons & (1 << c.id)) != 0;
                    p.circle(c.cx, c.cy, c.r, on ? a(c.color, 0.55f) : a(0x40000000, 1), true, 0);
                    p.circle(c.cx, c.cy, c.r, a(c.color, 1), false, stroke * 1.2f);
                    p.text(c.label, c.cx, c.cy, c.r * 0.95f, a(c.color, 1));
                    break;
                }
                case LT: case RT: case LB: case RB: case BACK: case START: {
                    boolean on = c.id == LT ? lt > 0 : c.id == RT ? rt > 0 : (buttons & (1 << c.id)) != 0;
                    p.roundRect(c.cx - c.hw, c.cy - c.r, c.cx + c.hw, c.cy + c.r, c.r * 0.6f,
                            on ? pressedFill : a(0x40000000, 1), true, 0);
                    p.roundRect(c.cx - c.hw, c.cy - c.r, c.cx + c.hw, c.cy + c.r, c.r * 0.6f, line, false, stroke);
                    p.text(c.label, c.cx, c.cy, c.r * (c.label.length() > 2 ? 0.8f : 1.0f), line);
                    break;
                }
                case L3: case R3: {
                    boolean on = (buttons & (1 << c.id)) != 0;
                    p.circle(c.cx, c.cy, c.r, on ? pressedFill : a(0x40000000, 1), true, 0);
                    p.circle(c.cx, c.cy, c.r, line, false, stroke);
                    p.text(c.label, c.cx, c.cy, c.r * 0.8f, line);
                    break;
                }
                default: break;
            }
        }
    }

    private void drawDpad(Painter p, Control c, int line, int pressedFill, float stroke) {
        float arm = c.r, half = c.r * 0.34f;
        int[] dirs = {UP, DOWN, LEFT, RIGHT};
        for (int d : dirs) {
            float l, t, r, b;
            switch (d) {
                case UP: l = c.cx - half; r = c.cx + half; t = c.cy - arm; b = c.cy - half; break;
                case DOWN: l = c.cx - half; r = c.cx + half; t = c.cy + half; b = c.cy + arm; break;
                case LEFT: l = c.cx - arm; r = c.cx - half; t = c.cy - half; b = c.cy + half; break;
                default: l = c.cx + half; r = c.cx + arm; t = c.cy - half; b = c.cy + half; break;
            }
            boolean on = (buttons & (1 << d)) != 0;
            p.roundRect(l, t, r, b, half * 0.25f, on ? pressedFill : a(0x40000000, 1), true, 0);
        }
        // The cross's outline (one path of twelve edges, drawn as lines).
        float[][] pts = {
            {c.cx - half, c.cy - arm}, {c.cx + half, c.cy - arm}, {c.cx + half, c.cy - half}, {c.cx + arm, c.cy - half},
            {c.cx + arm, c.cy + half}, {c.cx + half, c.cy + half}, {c.cx + half, c.cy + arm}, {c.cx - half, c.cy + arm},
            {c.cx - half, c.cy + half}, {c.cx - arm, c.cy + half}, {c.cx - arm, c.cy - half}, {c.cx - half, c.cy - half}};
        for (int i = 0; i < pts.length; i++) {
            float[] q0 = pts[i], q1 = pts[(i + 1) % pts.length];
            p.line(q0[0], q0[1], q1[0], q1[1], line, stroke);
        }
    }
}
