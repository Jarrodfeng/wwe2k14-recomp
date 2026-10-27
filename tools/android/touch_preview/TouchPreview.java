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

    public static void main(String[] args) throws Exception {
        File dir = new File(args[0]);
        dir.mkdirs();
        int w = args.length > 2 ? Integer.parseInt(args[1]) : 3168, h = args.length > 2 ? Integer.parseInt(args[2]) : 1440;
        int size = args.length > 3 ? Integer.parseInt(args[3]) : 100, opacity = args.length > 4 ? Integer.parseInt(args[4]) : 70;
        BufferedImage bg = args.length > 5 ? ImageIO.read(new File(args[5])) : null;
        TouchPad pad = new TouchPad();
        pad.layout(w, h, size, opacity);
        render(pad, w, h, bg, new File(dir, "touch_idle.png"));

        // Made-up touches at the controls' own spots (the layout's numbers, in units of the height).
        float H = h;
        float aX = w - 0.27f * H, aY = (0.47f + 0.125f * size / 100f) * H;
        check("A pressed", pad.down(0, aX, aY) && pad.buttons() == 1 << TouchPad.A);
        check("left stick: a thumb inside it tilts it", pad.down(1, 0.27f * H + 0.06f * H, 0.47f * H) && pad.lx() > 0.3f && pad.lx() < 0.6f);
        check("left stick dragged far right: lx 1, A kept", pad.move(1, 0.27f * H + 0.4f * H, 0.47f * H) && pad.lx() == 1f && pad.ly() == 0f && (pad.buttons() & 1) != 0);
        check("d-pad up-left: UP + LEFT", pad.down(2, 0.60f * H - 0.08f * H, 0.74f * H - 0.08f * H)
                && (pad.buttons() & (1 << TouchPad.UP)) != 0 && (pad.buttons() & (1 << TouchPad.LEFT)) != 0);
        check("RT held", pad.down(3, w - 0.12f * H, 0.20f * H) && pad.rt() == 1f);
        render(pad, w, h, bg, new File(dir, "touch_pressed.png"));
        check("A released", pad.up(0) && (pad.buttons() & 1) == 0);
        check("d-pad slides to right only", pad.move(2, 0.60f * H + 0.1f * H, 0.74f * H)
                && (pad.buttons() & (1 << TouchPad.RIGHT)) != 0 && (pad.buttons() & (1 << TouchPad.UP)) == 0);
        check("start (top centre)", pad.down(4, w / 2f + 0.13f * H, 0.08f * H) && (pad.buttons() & (1 << TouchPad.START)) != 0);
        check("view (top centre)", pad.down(5, w / 2f - 0.13f * H, 0.08f * H) && (pad.buttons() & (1 << TouchPad.BACK)) != 0);
        check("a touch in the empty middle does nothing", !pad.down(6, w / 2f, 0.5f * H));
        check("cancel lets everything go", pad.cancel() && pad.buttons() == 0 && pad.lx() == 0 && pad.rt() == 0);
        System.out.println(failures == 0 ? "all checks passed" : failures + " check(s) failed");
        System.exit(failures == 0 ? 0 : 1);
    }
}
