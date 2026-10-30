// wwe2k14 Android port (Session 71, ANDROID_PLAN F1): the touch controls' layout, drawing and touch logic. Plain Java
// with no Android types, so tools/android/touch_preview draws it and drives it with made-up touches on the PC; the
// Android side (TouchOverlay) only hands it a Painter and the touches.
//
// Layout (Jarrod's reference picture, 2026-10-28): left - LT in the top corner, LB inside it, the left stick upper
// left, the d-pad below and inward of it, L3 in the bottom corner; right - RT in the top corner, RB inside it, A B X Y
// (coloured rings: Y yellow, X blue, B red, A green), the right stick below and inward, R3 in the bottom corner; View
// and Start at the top centre ("Top centre is smart") (the picture's curved background bands left out at Jarrod's word). Positions are in
// units of the screen's height, measured from the nearer side edge, so the clusters stay at the edges on any width.
//
// Session 72 part 8 (a playtester's unfolded Honor Magic V6, about 1.1:1: the two sides met in the middle, the d-pad
// on the right stick): each side reaches 0.735 units in from its edge, so the layout needs a screen at least
// kMinAspect times as wide as tall. On a squarer screen the unit is the width / kMinAspect instead of the height; the
// top row (triggers, bumpers, View / Start) keeps its place from the top, everything else from the bottom (the thumbs'
// corners). Phones (2:1 and wider) are unchanged.
//
// Session 74 (players asked to move the controls): an edit mode where a finger drags a control to a new place (the
// face buttons move as one), with CANCEL / RESET / DONE in the middle. Each control keeps an offset from its default
// place in units, so a layout made on one screen lands the same way on another; a control never leaves the screen.
// Size still shrinks to what fits the default layout, and the offsets come on top. Saved as text (layoutText).
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
    public static final int LT = 100, RT = 101, LSTICK = 102, RSTICK = 103, DPAD = 104;
    static final float kMinAspect = 1.65f;

    private static final int GREEN = 0xFF6CC24A, RED = 0xFFE5533D, BLUE = 0xFF3B8EDE, YELLOW = 0xFFE8C23A;

    /** One control: where it is (pixels, set by layout()) and how it looks. */
    private static final class Control {
        final int id;
        final String label;
        final int color;
        float cx, cy, r;   // centre; r = radius (round controls) or half height (pills)
        float hw;          // pills: half width (0 = round)
        float offX, offY;  // Session 74: moved this far from the default place (units)
        Control(int id, String label, int color) { this.id = id; this.label = label; this.color = color; }
        float halfWidth() { return hw > 0 ? hw : r; }
        String name() { return !label.isEmpty() ? label : id == LSTICK ? "LSTICK" : id == RSTICK ? "RSTICK" : "DPAD"; }
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

    private float width, height, unit, scale = 1f, alpha = 0.7f;

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
        unit = Math.min(h, w / kMinAspect);
        alpha = Math.max(0.2f, Math.min(1f, opacityPercent / 100f));
        // Part 8: the chosen Size, or the largest below it that fits (at 150% the face buttons ran into the right stick
        // and RB and B went off the edge, on phones too): 5% steps down until no two controls overlap.
        for (scale = Math.max(0.6f, Math.min(1.5f, sizePercent / 100f)); ; scale -= 0.05f) {
            placeDefaults();
            if (scale <= 0.6f || fits()) break;
        }
        applyOffsets();
    }

    private void place() {
        placeDefaults();
        applyOffsets();
    }

    private void placeDefaults() {
        float w = width, H = unit, s = scale;
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
    /** Every control on the screen and none overlapping another (boxes: a round control's square). */
    private boolean fits() {
        java.util.List<Object[]> b = boxes();
        for (int i = 0; i < b.size(); i++) {
            float[] p = (float[]) b.get(i)[1];
            if (p[0] < 0 || p[1] < 0 || p[2] > width || p[3] > height) return false;
            for (int j = i + 1; j < b.size(); j++) {
                float[] q = (float[]) b.get(j)[1];
                if (p[0] < q[2] && q[0] < p[2] && p[1] < q[3] && q[1] < p[3]) return false;
            }
        }
        return true;
    }
    /** The Size in use after layout (percent): the chosen one, or the largest below it that fits. */
    public int sizeUsed() { return Math.round(scale * 100); }
    private float left(float u) { return u * unit; }
    private float right(float u) { return width - u * unit; }
    // A height in units: the top row (yu <= 0.25) from the top edge, the rest from the bottom (the same on a phone,
    // where the unit is the height).
    private float y(float yu) { return yu <= 0.25f || unit == height ? yu * unit : height - (1 - yu) * unit; }
    private void round(Control c, float x, float yu, float ru) { c.cx = x; c.cy = y(yu); c.r = ru * unit; c.hw = 0; }
    private void pill(Control c, float x, float yu, float hwu, float hhu) { c.cx = x; c.cy = y(yu); c.hw = hwu * unit; c.r = hhu * unit; }
    private Control find(int id) { for (Control c : controls) if (c.id == id) return c; return null; }
    /** A control's centre in pixels (after layout), for the PC preview's checks; null for an unknown id. */
    public float[] centre(int id) { Control c = find(id); return c == null ? null : new float[] {c.cx, c.cy}; }
    /** The layout's unit in pixels: the height, or the width / kMinAspect on a squarer screen. */
    public float unit() { return unit; }
    /** Every control's box (left, top, right, bottom) in pixels with its label ("" for sticks / d-pad), for the checks. */
    public java.util.List<Object[]> boxes() {
        java.util.List<Object[]> out = new java.util.ArrayList<>();
        for (Control c : controls) {
            float hx = c.halfWidth();
            out.add(new Object[] {c.name(), new float[] {c.cx - hx, c.cy - c.r, c.cx + hx, c.cy + c.r}});
        }
        return out;
    }

    // ------------------------------------------------------------------ moving the controls (Session 74)

    /** What a tap in the edit mode's middle row did (editUp). RESET has already put the controls back. */
    public static final int EDIT_NONE = 0, EDIT_DONE = 1, EDIT_RESET = 2, EDIT_CANCEL = 3;
    private static final String[] kEditLabels = {"", "DONE", "RESET", "CANCEL"};
    private boolean editing;
    private float[] editStart;  // the offsets when editing began (CANCEL puts them back)
    // Each finger in the edit mode: the edit button it went down on, or (pointerControl) the control it drags, where
    // the finger started and the control's offset then.
    private final int[] pointerEdit = new int[16];
    private final float[] dragX0 = new float[16], dragY0 = new float[16], dragOffX0 = new float[16], dragOffY0 = new float[16];

    private static boolean face(int id) { return id == A || id == B || id == X || id == Y; }

    // The face buttons move as one (by A's offset), every other control on its own; each group is held wholly on the
    // screen, and the offset it ended up with is kept.
    private void applyOffsets() {
        java.util.List<Control> faceButtons = new java.util.ArrayList<>();
        for (Control c : controls) {
            if (face(c.id)) faceButtons.add(c);
            else shift(java.util.Collections.singletonList(c));
        }
        if (!faceButtons.isEmpty()) shift(faceButtons);
    }
    private void shift(java.util.List<Control> group) {
        Control lead = null;
        for (Control c : group) if (lead == null || c.id == A) lead = c;
        float dx = lead.offX * unit, dy = lead.offY * unit;
        float l = Float.MAX_VALUE, t = Float.MAX_VALUE, r = -Float.MAX_VALUE, b = -Float.MAX_VALUE;
        for (Control c : group) {
            float hx = c.halfWidth();
            l = Math.min(l, c.cx - hx); r = Math.max(r, c.cx + hx); t = Math.min(t, c.cy - c.r); b = Math.max(b, c.cy + c.r);
        }
        dx = Math.max(-l, Math.min(width - r, dx));
        dy = Math.max(-t, Math.min(height - b, dy));
        for (Control c : group) {
            c.cx += dx; c.cy += dy;
            c.offX = unit > 0 ? dx / unit : 0; c.offY = unit > 0 ? dy / unit : 0;
        }
    }

    public boolean editing() { return editing; }
    /** True when any control is away from its default place. */
    public boolean moved() {
        for (Control c : controls) if (c.offX != 0 || c.offY != 0) return true;
        return false;
    }
    /** The edit mode: the pad lets go of everything (nothing reaches the game while editing). */
    public void startEdit() {
        cancel();
        editing = true;
        editStart = new float[controls.length * 2];
        for (int i = 0; i < controls.length; i++) { editStart[2 * i] = controls[i].offX; editStart[2 * i + 1] = controls[i].offY; }
        java.util.Arrays.fill(pointerEdit, EDIT_NONE);
    }
    /** Leaves the edit mode: keep = the new places stay (DONE), else the ones from before editing (CANCEL). */
    public void endEdit(boolean keep) {
        if (!keep && editStart != null) {
            for (int i = 0; i < controls.length; i++) { controls[i].offX = editStart[2 * i]; controls[i].offY = editStart[2 * i + 1]; }
            place();
        }
        editing = false;
        java.util.Arrays.fill(pointerControl, -1);
        java.util.Arrays.fill(pointerEdit, EDIT_NONE);
    }
    /** Every control back to its default place. */
    public void resetLayout() {
        for (Control c : controls) { c.offX = 0; c.offY = 0; }
        place();
    }

    // The edit mode's buttons in the middle of the screen (between the sticks on a phone; the free band between the top
    // row and the thumbs' controls on a fold): CANCEL, RESET, DONE.
    private float[] editButton(int k) {
        float cx = width / 2 + (k == EDIT_CANCEL ? -0.23f : k == EDIT_RESET ? 0 : 0.23f) * unit, cy = height / 2;
        float hw = 0.1f * unit, hh = 0.05f * unit;
        return new float[] {cx - hw, cy - hh, cx + hw, cy + hh};
    }
    /** An edit button's centre in pixels, for the PC preview's checks. */
    public float[] editButtonCentre(int k) { float[] b = editButton(k); return new float[] {(b[0] + b[2]) / 2, (b[1] + b[3]) / 2}; }
    private int editButtonAt(float x, float y) {
        for (int k = EDIT_DONE; k <= EDIT_CANCEL; k++) {
            float[] b = editButton(k);
            if (x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]) return k;
        }
        return EDIT_NONE;
    }

    /** A finger went down in the edit mode (true = redraw). */
    public boolean editDown(int pointer, float x, float y) {
        if (!editing || pointer < 0 || pointer >= pointerControl.length) return false;
        // A control and a button both under the finger (a control dropped on a button): a tap is the button, a drag
        // moves the control (one dropped wholly under a button stays reachable). A button alone: the button; else a
        // control within a little more reach.
        pointerEdit[pointer] = editButtonAt(x, y);
        int best = nearestControl(x, y, 1f);
        if (best < 0 && pointerEdit[pointer] == EDIT_NONE) best = nearestControl(x, y, 1.15f);
        pointerControl[pointer] = best;
        if (best < 0) return pointerEdit[pointer] != EDIT_NONE;
        pointerControl[pointer] = best;
        dragX0[pointer] = x; dragY0[pointer] = y;
        dragOffX0[pointer] = controls[best].offX; dragOffY0[pointer] = controls[best].offY;
        return true;
    }
    private int nearestControl(float x, float y, float slack) {
        int best = -1;
        float bestD = Float.MAX_VALUE;
        for (int i = 0; i < controls.length; i++) {
            Control c = controls[i];
            if (!c.hit(x, y, slack)) continue;
            float d = (x - c.cx) * (x - c.cx) + (y - c.cy) * (y - c.cy);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }
    /** A finger moved in the edit mode (true = a control moved). */
    public boolean editMove(int pointer, float x, float y) {
        if (!editing || pointer < 0 || pointer >= pointerControl.length || pointerControl[pointer] < 0 || unit <= 0) return false;
        Control dragged = controls[pointerControl[pointer]];
        if (pointerEdit[pointer] != EDIT_NONE) {  // a tap on a button until the finger has really moved
            float mx = x - dragX0[pointer], my = y - dragY0[pointer], still = 0.025f * unit;
            if (mx * mx + my * my < still * still) return false;
            pointerEdit[pointer] = EDIT_NONE;
        }
        float ox = dragOffX0[pointer] + (x - dragX0[pointer]) / unit, oy = dragOffY0[pointer] + (y - dragY0[pointer]) / unit;
        for (Control c : controls) {
            if (c == dragged || (face(dragged.id) && face(c.id))) { c.offX = ox; c.offY = oy; }
        }
        place();
        return true;
    }
    /** A finger went up in the edit mode: the edit button tapped (the finger went down and up on it), else EDIT_NONE. */
    public int editUp(int pointer, float x, float y) {
        if (!editing || pointer < 0 || pointer >= pointerControl.length) return EDIT_NONE;
        int k = pointerEdit[pointer] != EDIT_NONE && editButtonAt(x, y) == pointerEdit[pointer] ? pointerEdit[pointer] : EDIT_NONE;
        pointerEdit[pointer] = EDIT_NONE;
        pointerControl[pointer] = -1;
        if (k == EDIT_RESET) resetLayout();
        return k;
    }

    /** The moved controls as text (the file touch_layout.txt): a first line, then "NAME dx dy" in units per control. */
    public String layoutText() {
        StringBuilder sb = new StringBuilder("wwe2k14 touch layout 1\n");
        for (Control c : controls) {
            if (c.offX == 0 && c.offY == 0) continue;
            sb.append(c.name()).append(String.format(java.util.Locale.ROOT, " %.4f %.4f\n", c.offX, c.offY));
        }
        return sb.toString();
    }
    /** Places the controls from layoutText's text (unknown names and broken lines are skipped; null = defaults). */
    public void setLayoutText(String text) {
        for (Control c : controls) { c.offX = 0; c.offY = 0; }
        if (text != null) {
            for (String line : text.split("\n")) {
                String[] f = line.trim().split("\\s+");
                if (f.length != 3) continue;
                for (Control c : controls) {
                    if (!c.name().equals(f[0])) continue;
                    try {
                        float x = Float.parseFloat(f[1]), y = Float.parseFloat(f[2]);
                        if (Float.isFinite(x) && Float.isFinite(y)) {
                            c.offX = Math.max(-3f, Math.min(3f, x));
                            c.offY = Math.max(-3f, Math.min(3f, y));
                        }
                    } catch (NumberFormatException ignored) {
                    }
                }
            }
        }
        if (unit > 0) place();
    }

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
        float H = unit, stroke = Math.max(1.5f, 0.0055f * H * scale);
        // (No background bands: b44 drew rings of light from the bottom corners; Jarrod, 2026-10-28: "a little extreme
        // we should remove them".)
        int line = a(0xE6FFFFFF, 1), pressedFill = a(0x55FFFFFF, 1);
        if (editing) p.roundRect(0, 0, width, height, 0, 0x73000000, true, 0);  // the game dimmed behind the edit mode
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
        if (editing) drawEdit(p, stroke);
    }

    // The edit mode on top: an outline per control (yellow while dragged, red where it overlaps another control or the
    // edit buttons), a hint, and CANCEL / RESET / DONE. Not faded by the Opacity setting.
    private void drawEdit(Painter p, float stroke) {
        java.util.List<Object[]> b = boxes();
        for (int i = 0; i < controls.length; i++) {
            Control c = controls[i];
            float[] q = (float[]) b.get(i)[1];
            boolean clash = false;
            for (int j = 0; j < controls.length && !clash; j++) {
                if (j == i || (face(c.id) && face(controls[j].id))) continue;
                float[] o = (float[]) b.get(j)[1];
                clash = q[0] < o[2] && o[0] < q[2] && q[1] < o[3] && o[1] < q[3];
            }
            for (int k = EDIT_DONE; k <= EDIT_CANCEL && !clash; k++) {
                float[] o = editButton(k);
                clash = q[0] < o[2] && o[0] < q[2] && q[1] < o[3] && o[1] < q[3];
            }
            boolean dragged = false;
            for (int pc : pointerControl) {
                if (pc < 0) continue;
                if (pc == i || (face(c.id) && face(controls[pc].id))) dragged = true;
            }
            int color = dragged ? 0xFFFFD54A : clash ? 0xFFFF4D4D : 0x99FFFFFF;
            float pad = 0.012f * unit, hx = c.halfWidth() + pad, hy = c.r + pad;
            if (c.hw > 0) p.roundRect(c.cx - hx, c.cy - hy, c.cx + hx, c.cy + hy, hy * 0.6f, color, false, stroke * 1.3f);
            else p.circle(c.cx, c.cy, c.r + pad, color, false, stroke * 1.3f);
        }
        p.text("DRAG A CONTROL TO MOVE IT", width / 2, height / 2 - 0.12f * unit, 0.04f * unit, 0xFFFFFFFF);
        for (int k = EDIT_DONE; k <= EDIT_CANCEL; k++) {
            float[] r = editButton(k);
            boolean down = false;
            for (int pe : pointerEdit) down |= pe == k;
            int fill = k == EDIT_DONE ? (down ? 0xFFBF2630 : 0xFF8C1C21) : (down ? 0xFF4A4E57 : 0xE62A2D33);
            float rad = (r[3] - r[1]) * 0.3f;
            p.roundRect(r[0], r[1], r[2], r[3], rad, fill, true, 0);
            p.roundRect(r[0], r[1], r[2], r[3], rad, 0xE6FFFFFF, false, stroke);
            p.text(kEditLabels[k], (r[0] + r[2]) / 2, (r[1] + r[3]) / 2, 0.04f * unit, 0xFFFFFFFF);
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
