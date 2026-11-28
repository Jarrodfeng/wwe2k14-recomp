// S111 SHADERBAR (Jarrod, 2026-11-28: "a center screen progress bar and a clear shader compilation indicator"): what the phone's
// shader precompile screen says (GameActivity startPrecompileWatch / pollPrecompile), from the game's progress file
// (src/precompile.h Write: state, made, total, m6_done, m6_total) and the activity's own clock. The clock moves at every poll,
// so the screen shows the work going on between the game's progress writes. Plain Java (no Android classes): the PC check
// tools/android/precompile_bar.ps1 compiles and runs it with tools/android/precompile_bar/PrecompileBarTest.java.
package io.github.jarrodfeng.wwe2k14;

final class PrecompileBar {
    private PrecompileBar() {}

    static final String TITLE = "COMPILING SHADERS";
    static final String KEEP_OPEN = "This runs once after an install, an update or a graphics setting change.\n"
            + "Keep this app open: the game starts by itself when it finishes.";

    static final class Text {
        final String title, counts, detail;
        final int permille;  // the bar, 0..1000; -1: no counts yet (the bar moves without a value)

        Text(String title, String counts, String detail, int permille) {
            this.title = title;
            this.counts = counts;
            this.detail = detail;
            this.permille = permille;
        }
    }

    static String clock(long ms) {
        long s = Math.max(0, ms / 1000);
        long sec = s % 60;
        return (s / 60) + ":" + (sec < 10 ? "0" : "") + sec;
    }

    // The time left from the rate so far (made in elapsed): empty before 10 s, before the first one is made, or when none is left.
    static String estimate(int made, int total, long elapsedMs) {
        if (made <= 0 || made >= total || elapsedMs < 10000) return "";
        long leftMs = (long) ((double) elapsedMs * (total - made) / made);
        long min = (leftMs + 59999) / 60000;
        return min <= 1 ? "about a minute left" : "about " + min + " min left";
    }

    // The screen for the last counts read (the caller keeps them when a read fails) and the time since the watch started.
    static Text of(String state, int made, int total, int m6Done, int m6Total, long elapsedMs) {
        if ("done".equals(state))
            return new Text("SHADERS READY", total > 0 ? made + " / " + total : "", "Starting the game...", 1000);
        String elapsed = "Elapsed " + clock(elapsedMs);
        int d, t;
        String what;
        if (m6Total > 0 && m6Done < m6Total) {
            d = m6Done;
            t = m6Total;
            what = "shaders";
        } else if (total > 0) {
            d = Math.min(Math.max(made, 0), total);
            t = total;
            what = "pipelines";
        } else {
            return new Text(TITLE, "Starting...", elapsed, -1);
        }
        int pm = (int) (1000L * d / t);
        String counts = d + " / " + t + " " + what + "  (" + (pm / 10) + "%)";
        String left = d >= t ? "finishing..." : estimate(d, t, elapsedMs);
        return new Text(TITLE, counts, left.isEmpty() ? elapsed : elapsed + "  -  " + left, pm);
    }
}
