// S111 SHADERBAR: the phone's precompile screen text and bar (android/java/.../PrecompileBar.java), on the PC. Exit 1 on a failure.
//   pwsh -NoProfile -File tools\android\precompile_bar.ps1
package io.github.jarrodfeng.wwe2k14;

public class PrecompileBarTest {
    static int fails = 0;

    static void need(boolean ok, String what) {
        System.out.println((ok ? "ok   " : "FAIL ") + what);
        if (!ok) fails++;
    }

    // The old activity's line (GameActivity pollPrecompile before SHADERBAR): every poll started from "Preparing shaders..." and
    // replaced it only from a good read with counts still short; a failed read or made == total dropped the numbers.
    static String oldLine(boolean readOk, int made, int total) {
        String line = "Preparing shaders...";
        if (readOk && total > 0 && made < total) line = "Preparing pipelines " + made + " / " + total;
        return line;
    }

    public static void main(String[] a) {
        PrecompileBar.Text t = PrecompileBar.of("starting", 0, 0, 0, 0, 3000);
        need(t.title.equals("COMPILING SHADERS") && t.permille == -1 && t.counts.equals("Starting...") && t.detail.equals("Elapsed 0:03"),
                "no counts yet: the title, a moving bar, Starting..., the clock");
        t = PrecompileBar.of("running", 120, 480, 0, 0, 60000);
        need(t.permille == 250 && t.counts.equals("120 / 480 pipelines  (25%)"), "counts: 120 / 480 = 25 %, bar 250 of 1000");
        need(t.detail.equals("Elapsed 1:00  -  about 3 min left"), "estimate from the rate so far: 360 left at 2 a second = 3 min (" + t.detail + ")");
        PrecompileBar.Text later = PrecompileBar.of("running", 120, 480, 0, 0, 61000);
        need(!later.detail.equals(t.detail), "the same counts a second later still change the screen (the clock moves every poll)");
        t = PrecompileBar.of("running", 480, 480, 0, 0, 200000);
        need(t.permille == 1000 && t.counts.startsWith("480 / 480") && t.detail.endsWith("finishing..."), "all made, not done yet: 100 %, finishing");
        need(oldLine(true, 480, 480).equals("Preparing shaders..."), "planted twin: the old line dropped the numbers at made == total");
        need(oldLine(false, 120, 480).equals("Preparing shaders..."), "planted twin: the old line dropped the numbers on a failed read");
        t = PrecompileBar.of("running", 600, 480, 0, 0, 5000);
        need(t.permille == 1000, "made above total is clamped");
        t = PrecompileBar.of("running", 0, 0, 30, 120, 5000);
        need(t.permille == 250 && t.counts.equals("30 / 120 shaders  (25%)"), "the translation step's counts when the SDK reports them");
        t = PrecompileBar.of("done", 470, 480, 0, 0, 300000);
        need(t.title.equals("SHADERS READY") && t.permille == 1000, "done: ready, full bar");
        need(PrecompileBar.estimate(1, 1000, 5000).isEmpty(), "no estimate in the first 10 s");
        need(PrecompileBar.estimate(999, 1000, 60000).equals("about a minute left"), "under a minute: about a minute");
        need(PrecompileBar.clock(605000).equals("10:05"), "clock m:ss");
        System.out.println(fails == 0 ? "precompile_bar: all checks pass" : "precompile_bar: " + fails + " failed");
        System.exit(fails == 0 ? 0 : 1);
    }
}
