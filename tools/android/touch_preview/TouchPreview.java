// Session 71: the touch controls (android/java/.../TouchPad.java) on the PC. Draws the layout with Java2D into a PNG at
// the phone's size and drives TouchPad with made-up touches, checking the pad state each one gives (exit code 1 when a
// check fails). Built and run by tools/android/touch_preview.ps1; no Android needed.
//   java TouchPreview OUT_DIR [WIDTH HEIGHT SIZE% OPACITY% [BACKGROUND.png]]
import io.github.jarrodfeng.wwe2k14.TouchPad;

import java.awt.BasicStroke;
import java.awt.Color;
import java.awt.Font;
import java.awt.FontMetrics;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.geom.Ellipse2D;
import java.awt.geom.Line2D;
import java.awt.geom.RoundRectangle2D;
import java.awt.image.BufferedImage;
import java.io.File;
import javax.imageio.ImageIO;

public class TouchPreview {
    static final class AwtPainter implements TouchPad.Painter {
        final Graphics2D g;
        AwtPainter(Graphics2D g) { this.g = g; }
        private void set(int argb, float stroke) {
            g.setColor(new Color(argb, true));
            if (stroke > 0) g.setStroke(new BasicStroke(stroke, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND));
        }
        public void circle(float cx, float cy, float r, int argb, boolean fill, float stroke) {
            set(argb, stroke);
            Ellipse2D e = new Ellipse2D.Float(cx - r, cy - r, 2 * r, 2 * r);
            if (fill) g.fill(e); else g.draw(e);
        }
        public void roundRect(float l, float t, float r, float b, float radius, int argb, boolean fill, float stroke) {
            set(argb, stroke);
            RoundRectangle2D rr = new RoundRectangle2D.Float(l, t, r - l, b - t, 2 * radius, 2 * radius);
            if (fill) g.fill(rr); else g.draw(rr);
        }
        public void line(float x0, float y0, float x1, float y1, int argb, float stroke) {
            set(argb, stroke);
            g.draw(new Line2D.Float(x0, y0, x1, y1));
        }
        public void text(String s, float cx, float cy, float size, int argb) {
            set(argb, 0);
            g.setFont(new Font("SansSerif", Font.BOLD, Math.round(size)));
            FontMetrics m = g.getFontMetrics();
            g.drawString(s, cx - m.stringWidth(s) / 2f, cy + (m.getAscent() - m.getDescent()) / 2f);
        }
    }

    static int failures = 0;
    static void check(String what, boolean ok) {
        System.out.println((ok ? "ok    " : "FAIL  ") + what);
        if (!ok) failures++;
    }

    static void render(TouchPad pad, int w, int h, BufferedImage bg, File out) throws Exception {
        BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
        Graphics2D g = img.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
        if (bg != null) g.drawImage(bg, 0, 0, w, h, null);
        else { g.setColor(new Color(0x2A2D33)); g.fillRect(0, 0, w, h); }
        pad.draw(new AwtPainter(g));
        g.dispose();
        ImageIO.write(img, "png", out);
        System.out.println("wrote " + out);
    }

    static boolean setAndCheck(String text, int w, int h, int size, int opacity, float[] aDefault) {
        TouchPad t = new TouchPad();
        t.setLayoutText(text);
        t.layout(w, h, size, opacity);
        return !t.moved() && t.centre(TouchPad.A)[0] == aDefault[0] && t.centre(TouchPad.A)[1] == aDefault[1];
    }

    public static void main(String[] args) throws Exception {
        File dir = new File(args[0]);
        dir.mkdirs();
        int w = args.length > 2 ? Integer.parseInt(args[1]) : 3168, h = args.length > 2 ? Integer.parseInt(args[2]) : 1440;
        int size = args.length > 3 ? Integer.parseInt(args[3]) : 100, opacity = args.length > 4 ? Integer.parseInt(args[4]) : 70;
        BufferedImage bg = args.length > 5 ? ImageIO.read(new File(args[5])) : null;
        TouchPad pad = new TouchPad();
        pad.layout(w, h, size, opacity);
        render(pad, w, h, bg, new File(dir, "touch_idle.png"));

        // Session 72 part 8 (a fold's ~1.1:1 inner screen): at every Size setting no two controls overlap and all stay
        // on the screen.
        for (int sz : new int[] {60, 100, 150}) {
            TouchPad t = new TouchPad();
            t.layout(w, h, sz, opacity);
            java.util.List<Object[]> b = t.boxes();
            String clash = null, off = null;
            for (int i = 0; i < b.size(); i++) {
                float[] p = (float[]) b.get(i)[1];
                if (p[0] < 0 || p[1] < 0 || p[2] > w || p[3] > h) off = (String) b.get(i)[0];
                for (int j = i + 1; j < b.size(); j++) {
                    float[] q = (float[]) b.get(j)[1];
                    if (p[0] < q[2] && q[0] < p[2] && p[1] < q[3] && q[1] < p[3]) clash = b.get(i)[0] + " / " + b.get(j)[0];
                }
            }
            check("size " + sz + "% (used " + t.sizeUsed() + "%): no controls overlap" + (clash != null ? " (" + clash + ")" : ""), clash == null);
            check("size " + sz + "%: all on the screen" + (off != null ? " (" + off + " off)" : ""), off == null);
            // Session 74: the edit mode's buttons clear of every control in the default layout.
            String under = null;
            for (int k = TouchPad.EDIT_DONE; k <= TouchPad.EDIT_CANCEL; k++) {
                float[] c = t.editButtonCentre(k);
                float hw = 0.1f * t.unit(), hh = 0.05f * t.unit();
                for (Object[] o : b) {
                    float[] q = (float[]) o[1];
                    if (c[0] - hw < q[2] && q[0] < c[0] + hw && c[1] - hh < q[3] && q[1] < c[1] + hh) under = (String) o[0];
                }
            }
            check("size " + sz + "%: the edit buttons clear of the controls" + (under != null ? " (" + under + ")" : ""), under == null);
        }

        // Made-up touches at the controls' own spots (from the layout).
        float H = pad.unit();
        float[] a = pad.centre(TouchPad.A), ls = pad.centre(TouchPad.LSTICK), dp = pad.centre(TouchPad.DPAD);
        float[] rt = pad.centre(TouchPad.RT), st = pad.centre(TouchPad.START), vw = pad.centre(TouchPad.BACK);
        check("A pressed", pad.down(0, a[0], a[1]) && pad.buttons() == 1 << TouchPad.A);
        check("left stick: a thumb inside it tilts it", pad.down(1, ls[0] + 0.06f * H, ls[1]) && pad.lx() > 0.3f && pad.lx() < 0.6f);
        check("left stick dragged far right: lx 1, A kept", pad.move(1, ls[0] + 0.4f * H, ls[1]) && pad.lx() == 1f && pad.ly() == 0f && (pad.buttons() & 1) != 0);
        check("d-pad up-left: UP + LEFT", pad.down(2, dp[0] - 0.08f * H, dp[1] - 0.08f * H)
                && (pad.buttons() & (1 << TouchPad.UP)) != 0 && (pad.buttons() & (1 << TouchPad.LEFT)) != 0);
        check("RT held", pad.down(3, rt[0], rt[1]) && pad.rt() == 1f);
        render(pad, w, h, bg, new File(dir, "touch_pressed.png"));
        check("A released", pad.up(0) && (pad.buttons() & 1) == 0);
        check("d-pad slides to right only", pad.move(2, dp[0] + 0.1f * H, dp[1])
                && (pad.buttons() & (1 << TouchPad.RIGHT)) != 0 && (pad.buttons() & (1 << TouchPad.UP)) == 0);
        check("start (top centre)", pad.down(4, st[0], st[1]) && (pad.buttons() & (1 << TouchPad.START)) != 0);
        check("view (top centre)", pad.down(5, vw[0], vw[1]) && (pad.buttons() & (1 << TouchPad.BACK)) != 0);
        check("a touch in the empty middle does nothing", !pad.down(6, w / 2f, h / 2f));
        check("cancel lets everything go", pad.cancel() && pad.buttons() == 0 && pad.lx() == 0 && pad.rt() == 0);

        // Session 74: the edit mode (moving the controls) and its saved layout.
        TouchPad e = new TouchPad();
        e.layout(w, h, size, opacity);
        float[] ls0 = e.centre(TouchPad.LSTICK), a0 = e.centre(TouchPad.A), y0 = e.centre(TouchPad.Y), dp0 = e.centre(TouchPad.DPAD);
        e.startEdit();
        check("edit: a press on A does not press it", e.editDown(0, a0[0], a0[1]) && e.buttons() == 0);
        e.editMove(0, a0[0] - 0.2f * H, a0[1] - 0.1f * H);
        float[] a1 = e.centre(TouchPad.A), y1 = e.centre(TouchPad.Y);
        check("edit: dragging A moves it", Math.abs(a1[0] - (a0[0] - 0.2f * H)) < 1 && Math.abs(a1[1] - (a0[1] - 0.1f * H)) < 1);
        check("edit: Y moves with A", Math.abs((y1[0] - y0[0]) - (a1[0] - a0[0])) < 1 && Math.abs((y1[1] - y0[1]) - (a1[1] - a0[1])) < 1);
        check("edit: lifting the finger is no button", e.editUp(0, a1[0], a1[1]) == TouchPad.EDIT_NONE);
        e.editDown(1, ls0[0], ls0[1]);
        e.editMove(1, ls0[0] - 5f * H, ls0[1] + 5f * H);  // far past the bottom-left corner
        java.util.List<Object[]> eb = e.boxes();
        float[] lsBox = null;
        for (Object[] o : eb) if (o[0].equals("LSTICK")) lsBox = (float[]) o[1];
        check("edit: a control dragged off the screen stops at its edge", Math.abs(lsBox[0]) < 1 && Math.abs(lsBox[3] - h) < 1);
        e.editMove(1, ls0[0] + 0.3f * H, ls0[1]);  // and back in: follows the finger again
        e.editUp(1, 0, 0);
        float[] ls1 = e.centre(TouchPad.LSTICK);
        check("edit: back on the screen it follows the finger", Math.abs(ls1[0] - (ls0[0] + 0.3f * H)) < 1 && Math.abs(ls1[1] - ls0[1]) < 1);
        check("edit: the d-pad stayed put", e.centre(TouchPad.DPAD)[0] == dp0[0] && e.centre(TouchPad.DPAD)[1] == dp0[1]);
        render(e, w, h, bg, new File(dir, "touch_edit.png"));
        float[] done = e.editButtonCentre(TouchPad.EDIT_DONE);
        e.editDown(2, done[0], done[1]);
        check("edit: DONE", e.editUp(2, done[0], done[1]) == TouchPad.EDIT_DONE && e.moved());
        e.endEdit(true);
        String text = e.layoutText();
        System.out.print(text);
        check("edit: the layout text names only the moved controls", text.contains("\nA ") && text.contains("\nY ")
                && text.contains("\nLSTICK ") && !text.contains("DPAD"));

        // The text in a fresh pad (the game's start) gives the same places; touches land on the moved controls.
        TouchPad g = new TouchPad();
        g.setLayoutText(text);
        g.layout(w, h, size, opacity);
        check("saved layout: A where it was moved to", Math.abs(g.centre(TouchPad.A)[0] - a1[0]) < 1 && Math.abs(g.centre(TouchPad.A)[1] - a1[1]) < 1);
        check("saved layout: left stick where it was moved to", Math.abs(g.centre(TouchPad.LSTICK)[0] - ls1[0]) < 1);
        check("saved layout: A pressed at its new place", g.down(0, a1[0], a1[1]) && g.buttons() == 1 << TouchPad.A);
        check("saved layout: nothing at A's old place", !g.down(1, a0[0], a0[1]) && g.buttons() == 1 << TouchPad.A);
        g.cancel();
        // The same text on a squarer screen (a fold) keeps every control on the screen.
        TouchPad f = new TouchPad();
        f.setLayoutText(text);
        f.layout(1600, 1450, size, opacity);
        String offFold = null;
        for (Object[] o : f.boxes()) {
            float[] q = (float[]) o[1];
            if (q[0] < -0.5f || q[1] < -0.5f || q[2] > 1600.5f || q[3] > 1450.5f) offFold = (String) o[0];
        }
        check("saved layout on a 1.1:1 screen: all on the screen" + (offFold != null ? " (" + offFold + " off)" : ""), offFold == null);
        check("broken layout text is skipped", setAndCheck("garbage\nA x y\nZZ 1 1\nA NaN 0\n", w, h, size, opacity, a0));

        // CANCEL puts back the places from before editing; RESET the defaults.
        g.startEdit();
        float[] gl = g.centre(TouchPad.L3);
        g.editDown(0, gl[0], gl[1]);
        g.editMove(0, gl[0] + 0.1f * H, gl[1]);
        g.editUp(0, 0, 0);
        check("edit: L3 moved", Math.abs(g.centre(TouchPad.L3)[0] - (gl[0] + 0.1f * H)) < 1);
        float[] cancel = g.editButtonCentre(TouchPad.EDIT_CANCEL);
        g.editDown(0, cancel[0], cancel[1]);
        check("edit: CANCEL", g.editUp(0, cancel[0], cancel[1]) == TouchPad.EDIT_CANCEL);
        g.endEdit(false);
        check("edit: CANCEL put L3 back", Math.abs(g.centre(TouchPad.L3)[0] - gl[0]) < 1 && Math.abs(g.centre(TouchPad.L3)[1] - gl[1]) < 1
                && Math.abs(g.centre(TouchPad.LSTICK)[0] - ls1[0]) < 1);
        g.startEdit();
        float[] reset = g.editButtonCentre(TouchPad.EDIT_RESET);
        g.editDown(0, reset[0], reset[1]);
        check("edit: RESET", g.editUp(0, reset[0], reset[1]) == TouchPad.EDIT_RESET && !g.moved()
                && g.centre(TouchPad.A)[0] == a0[0] && g.centre(TouchPad.LSTICK)[0] == ls0[0]);
        float[] rs = g.editButtonCentre(TouchPad.EDIT_RESET);
        g.editDown(0, rs[0], rs[1]);
        check("edit: a finger that slides off a button taps nothing", g.editUp(0, rs[0], rs[1] + 0.3f * H) == TouchPad.EDIT_NONE);
        // L3 dropped wholly under RESET is still picked up (controls before buttons).
        float[] l3 = g.centre(TouchPad.L3);
        g.editDown(0, l3[0], l3[1]);
        g.editMove(0, rs[0], rs[1]);
        g.editUp(0, rs[0], rs[1]);
        check("edit: L3 under RESET", Math.abs(g.centre(TouchPad.L3)[0] - rs[0]) < 1 && Math.abs(g.centre(TouchPad.L3)[1] - rs[1]) < 1);
        g.editDown(0, rs[0], rs[1]);
        check("edit: a press there drags L3, not RESET", g.editMove(0, l3[0], l3[1]) && g.editUp(0, l3[0], l3[1]) == TouchPad.EDIT_NONE
                && Math.abs(g.centre(TouchPad.L3)[0] - l3[0]) < 1);
        g.editDown(0, l3[0], l3[1]);
        g.editMove(0, rs[0], rs[1]);
        g.editUp(0, rs[0], rs[1]);
        g.editDown(0, rs[0], rs[1]);
        g.editMove(0, rs[0] + 2, rs[1] + 2);  // a tap's small wobble
        check("edit: a tap on L3 lying on RESET is RESET", g.editUp(0, rs[0] + 2, rs[1] + 2) == TouchPad.EDIT_RESET
                && Math.abs(g.centre(TouchPad.L3)[0] - l3[0]) < 1);
        g.endEdit(true);
        check("edit: after RESET + DONE nothing is moved", !g.moved() && g.layoutText().equals("wwe2k14 touch layout 1\n"));
        System.out.println(failures == 0 ? "all checks passed" : failures + " check(s) failed");
        System.exit(failures == 0 ? 0 : 1);
    }
}
