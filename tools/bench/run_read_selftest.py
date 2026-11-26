#!/usr/bin/env python3
"""run_read_selftest.py - tools\\bench\\run_read.py against small synthetic logs the test writes itself (a temp folder, deleted at the end).

  py -I tools\\bench\\run_read_selftest.py [--tool PATH]     (--tool runs the same cases against another script, e.g. a stub that reads only
                                                              the first 2000 lines: the 5,000-line case must then FAIL)
Exit 0 all cases pass, 1 a case failed.
"""
# entry: main
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "run_read.py")


def ts(i):
    return "[2026-11-21 10:00:%02d.%03d]" % (i // 1000 % 60, i % 1000)


def L(i, rest, level="info"):
    return "%s [%s] [core] [t1] %s" % (ts(i), level, rest)


def census(cap, tagline="[scene][weapon_rows]"):
    return ("%s calls 5 objects 2, named but never drawn 1 (named 3, rows 90, unreadable 0, count_unknown 0, capped %d, overflow 0, late 0), "
            "stage pulls 1 frames 0 recheck_fail 0 capped 0 rigid_bounded 0" % (tagline, cap))


BASE = [L(0, "[wwe2k14] version v9.9.9"), L(1, "[wwe2k14] exe built (file time, local): 2026-11-21 09:59"),
        L(2, "[rdr][live] owner mode: the old path draws nothing (gpu_no_draw on)"),
        L(3, "[wwe2k14] modified: vsync = 'false' (source: command line)"),
        L(4, "[wwe2k14][input] line 11: bench: start scene synth (frame 1)")]
END = [L(900, "[wwe2k14][input] line 49: bench: done (frame 99)"), L(901, "[wwe2k14][input] line 50: quit (frame 99)")]


def write_log(root, name, lines, crlf=True):
    d = os.path.join(root, name, "logs")
    os.makedirs(d)
    p = os.path.join(d, "wwe2k14_001.log")
    with open(p, "wb") as fh:
        fh.write(("\r\n" if crlf else "\n").join(lines).encode("utf-8") + b"\r\n")
    return p


def run(tool, args):
    r = subprocess.run([sys.executable, "-I", tool] + args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.returncode, r.stdout, r.stderr


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", default=TOOL)
    a = ap.parse_args(argv)
    tool = a.tool
    root = tempfile.mkdtemp(prefix="run_read_selftest_")
    fails = []

    def check(name, cond, detail=""):
        print("%s %s%s" % ("ok  " if cond else "FAIL", name, (" -- " + detail) if (detail and not cond) else ""))
        if not cond:
            fails.append(name)

    try:
        # 1 a clean run: exit 0, route / done / quit / flags / renderer shown, no problems.
        p = write_log(root, "clean", BASE + [L(10, census(0))] + END)
        rc, out, _ = run(tool, [p])
        check("clean run exits 0", rc == 0, "rc %d\n%s" % (rc, out))
        check("clean run shows route, done, quit, flag, renderer", all(x in out for x in (
            "start scene synth", "done reached", "quit reached", "vsync=false", "owner mode: the old path draws nothing", "v9.9.9")), out)
        check("clean run: capped 0 is not a shortfall", "capped" not in out.split("SHORTFALLS")[1].split("SHOTS")[0], out)
        # S108 OPT: the top block's save-options row: from run_list's record, or said not to be there / not for this build
        rs = os.path.join(root, "run_save")
        os.makedirs(rs)
        os.environ["WWE_RUN_SAVE_DIR"] = rs
        rc, out, _ = run(tool, [p])
        check("OPT: no record -> 'save options: NOT RECORDED' (never silently absent)", "  save options: NOT RECORDED" in out, out[:600])
        line = "save options: blood off (S+81), captions off (the 30 Years caption word), unlock flags 0/193, dlc mask 0x00000000, options hash 6ea4a533; launch: direct x (captured match: blood off whatever the save says)"
        with open(os.path.join(rs, "wwe2k14_001.txt"), "w") as fh:
            fh.write(line + "\n" + os.path.join(root, "clean") + "\n")
        rc, out, _ = run(tool, [p])
        check("OPT: a recorded line is the row at the top", "  save options: " + line[len("save options: "):] in out.split("FLAGS:")[0], out[:600])
        with open(os.path.join(rs, "wwe2k14_001.txt"), "w") as fh:
            fh.write(line + "\n" + os.path.join(root, "other_exe") + "\n")
        rc, out, _ = run(tool, [p])
        check("OPT: a record of another exe folder is not shown as this run's", "NOT SHOWN" in out and "blood off" not in out.split("FLAGS:")[0], out[:600])
        del os.environ["WWE_RUN_SAVE_DIR"]

        # 2 a PROBLEM line fails the run, once with its count and frame.
        pl = L(20, "[rdr][PROBLEM] draw_shortfall: tassel asked 658 given 96 frame=4141")
        p = write_log(root, "prob", BASE + [pl, pl.replace("10:00:00.020", "10:00:00.021")] + END)
        rc, out, _ = run(tool, [p])
        sec = out.split("PROBLEMS")[1].split("LOADED")[0]
        check("PROBLEM line exits 1", rc == 1, "rc %d" % rc)
        check("PROBLEM line printed once with x2 and frame 4141", sec.count("draw_shortfall") == 1 and "x2" in sec and "4141" in sec, sec)

        # 3 capped 4374 then a later capped 4380: first non-zero and last value; the stage `capped` in the same block is separate.
        p = write_log(root, "cap", BASE + [L(30, census(0)), L(31, census(4374)), L(32, census(4380))] + END)
        rc, out, _ = run(tool, [p, "--full"])
        row = [l for l in out.splitlines() if "[scene][weapon_rows] capped:" in l]
        check("capped: first non-zero 4374, last 4380", len(row) == 1 and "first non-zero 4374 at" in row[0] and "max 4380, last 4380" in row[0], out)
        check("the stage capped (second in the block) is not reported", not any("capped#2" in l for l in out.splitlines()), out)

        # 4 two census blocks on one physical line (+ a counter in the second one).
        two = L(40, "[scene][live] frames 0-600: stale 0, dropped 0 " + census(7) + " [scene][2d] exit: fallback 3, refused 0")
        p = write_log(root, "two", BASE + [two] + END)
        rc, out, _ = run(tool, [p, "--full"])
        check("two blocks on one line: weapon_rows capped 7 and 2d fallback 3 both read",
              "[scene][weapon_rows] capped: first non-zero 7 at 2026-11-21 10:00:00.040, max 7, last 7" in out
              and "[scene][2d] fallback: first non-zero 3 at 2026-11-21 10:00:00.040, max 3, last 3" in out, out)
        # 4b a census block on a continuation line (no timestamp of its own) takes the timestamp of the line it continues.
        p = write_log(root, "cont", BASE + [L(45, "[scene][live] frames 0-600: stale 0"), census(6)] + END)
        rc, out, _ = run(tool, [p, "--full"])
        check("continuation-line census: first non-zero carries the previous timestamp",
              "[scene][weapon_rows] capped: first non-zero 6 at 2026-11-21 10:00:00.045" in out, out)

        # 5 --expect: a hit at line 5,000 of a 6,000-line log (the bug this tool exists for), and a miss.
        pad = [L(100 + i % 700, "[wwe2k14][file] filler line %d" % i) for i in range(6000)]
        pad[4994] = L(500, "[wwe2k14][weapon_rows] event prop id 2591 -> PACH 400000 (entry 4)")
        p = write_log(root, "big", BASE + pad + END)
        want = "event prop id 2591 -> PACH 400000 (entry 4)"
        rc, out, _ = run(tool, [p, "--expect", want])
        check("--expect hit at line ~5,000 passes (exit 0)", rc == 0 and "NOT FOUND" not in out and "EXPECTED: 1 of 1 found (line 5000)" in out, "rc %d\n%s" % (rc, out[-400:]))
        check("the line at ~5,000 is listed under LOADED", want in out.split("LOADED")[1].split("SHORTFALLS")[0], out[-600:])
        rc, out, _ = run(tool, [p, "--expect", want, "--expect", "event prop id 9999 -> PACH 1 (entry 0)"])
        check("--expect miss exits 1 and is named", rc == 1 and "event prop id 9999" in out.split("EXPECTED NOT FOUND")[1], "rc %d" % rc)

        # 6 --zero capped fails on a non-zero capped, passes when it is 0 or absent.
        rc, out, _ = run(tool, [os.path.dirname(os.path.dirname(os.path.join(root, "cap", "logs", "x"))), "--zero", "capped"])
        check("--zero capped fails when capped ends at 4380 (folder given)", rc == 1 and "ZERO FAILED" in out and "4380" in out.split("ZERO FAILED")[1], "rc %d\n%s" % (rc, out[-300:]))
        rc, out, _ = run(tool, [os.path.join(root, "clean"), "--zero", "capped", "--zero", "weapon_rows.overflow"])
        check("--zero passes at 0", rc == 0, "rc %d\n%s" % (rc, out[-400:]))

        # 7 no [rdr][live] line.
        p = write_log(root, "norl", [l for l in BASE if "[rdr][live]" not in l] + END)
        rc, out, _ = run(tool, [p])
        check("no renderer marker at all: NOT SEEN line", "NOT SEEN" in out.split("RENDERER")[1].split("PROBLEMS")[0], out)
        # 7b a player (stream-free) log: the build's own marker (src\wwe2k14_app.h, KEEP_STREAM=OFF) and no [rdr][live] census line: the
        #    stream-free marker is named first, never NOT SEEN; the owner-mode and `started` lines are named in that order when present.
        sf = L(0, "[wwe2k14] built with WWE2K14_KEEP_STREAM=OFF: the scene host is the renderer (present host: 3D + 2D rows)")
        p = write_log(root, "streamfree", [sf] + [l for l in BASE if "[rdr][live]" not in l] + END)
        rc, out, _ = run(tool, [p])
        rs = out.split("RENDERER")[1].split("PROBLEMS")[0]
        check("stream-free marker, no [rdr][live] line: renderer named, not NOT SEEN", "NOT SEEN" not in rs and "KEEP_STREAM=OFF" in rs, rs)
        p = write_log(root, "streamfree2", [sf] + BASE + [L(6, "[rdr][live] started on NVIDIA X (validation 0, fsi)"),
                                                          L(7, "[rdr][live] frames 0-600: calls 9 census long line")] + END)
        rc, out, _ = run(tool, [p])
        rs = out.split("RENDERER")[1].split("PROBLEMS")[0]
        check("RENDERER order: stream-free, owner mode, started; the census line counted, not printed",
              0 <= rs.find("KEEP_STREAM=OFF") < rs.find("owner mode:") < rs.find("started on") and "census long line" not in rs, rs)

        # 8 an unknown counter lands in UNSORTED once (a lossy-looking name the table lacks); a harmless unknown does not.
        p = write_log(root, "uns", BASE + [L(50, "[scene][widgets] lost_bits 12, calls 9"), L(51, "[scene][widgets] lost_bits 15, calls 9")] + END)
        rc, out, _ = run(tool, [p])
        uns = out.split("UNSORTED")[1]
        check("unknown lossy counter in UNSORTED once with last value", uns.count("lost_bits") == 1 and "lost_bits=15" in uns and "calls" not in uns, uns)
        check("an unknown counter is not in SHORTFALLS", "lost_bits" not in out.split("SHORTFALLS")[1].split("SHOTS")[0], out)

        # 9 (review D1) [error]-level lines are emulator noise in every real run: grouped in their own section, never the exit code by default.
        #   A heap-dump burst (consecutive error lines of one thread) is one group; ResolvePath(<path>) lines differing only in the path are one.
        def E(i, msg, t="t7", level="error"):
            return "%s [%s] [sys] [%s] %s" % (ts(i), level, t, msg)
        noise = [E(60, "BaseHeap::Alloc failed to find contiguous range"), L(61, "[wwe2k14][mem] dumping"),
                 E(62, "=========="), E(62, "Memory Dump"), E(62, "  Heap: 00000000-3FFFFFFF"), E(62, "  30000000-30001000      1p       4096b COM RW"),
                 E(62, "  30001000-30002000      1p       4096b COM RW"), L(63, "[wwe2k14] after the dump"),
                 E(64, "ResolvePath(UPDATE:\\) failed - device not found"), L(65, "x"), E(66, "ResolvePath(\\Device) failed - device not found"),
                 L(67, "x"), E(68, "Unhandled guest access violation: read of guest 0x41700001 (host 0x0000000141700001) on thread 0xF8000054")]
        p = write_log(root, "err", BASE + noise + [L(70, census(0))] + END)
        rc, out, _ = run(tool, [p])
        sec = out.split("ERROR-LEVEL LINES")[1].split("LOADED")[0] if "ERROR-LEVEL LINES" in out else ""
        check("D1: error-level noise exits 0", rc == 0, "rc %d\n%s" % (rc, out))
        check("D1: PROBLEMS stays none", "none" in out.split("PROBLEMS")[1].split("ERROR-LEVEL")[0], out)
        check("D1: the heap dump burst is ONE group, ResolvePath ONE group of 2, BaseHeap its own",
              sec.count("Memory Dump") == 1 and "Heap: 0" not in sec and sec.count("ResolvePath") == 1 and "x2" in sec and "BaseHeap::Alloc" in sec, sec)
        rc, out, _ = run(tool, [p, "--strict-errors"])
        check("D1: --strict-errors fails on them", rc == 1, "rc %d" % rc)
        rc, out, _ = run(tool, [p, "--strict-errors", "--allow-error", "BaseHeap|Memory Dump|ResolvePath|guest access violation"])
        check("D1: --strict-errors with each group allowed exits 0", rc == 0, "rc %d\n%s" % (rc, out))
        rc, out, err = run(tool, [p, "--allow-error", "BaseHeap"])
        check("D1: --allow-error without --strict-errors is refused (exit 2)", rc == 2, "rc %d" % rc)
        # 9b crash / device lost / route failure / fatal markers are PROBLEMS at ANY level ([crash] is critical, script FAILED and DEVICE LOST are error).
        for nm, ln in (("crash", E(80, "[wwe2k14][crash] unhandled exception 0xC0000005 at x.exe+0x1234 (if inside the exe). Backtrace:", level="critical")),
                       ("devlost", E(80, "[wwe2k14][frames] DEVICE LOST at guest frame 4000 (t=12.0 s): the GPU stopped drawing")),
                       ("scriptfail", E(80, "[wwe2k14][input] script FAILED at line 12: cvar x was not set")),
                       ("fatal", E(80, "[FATAL] Call to invalid or unregistered function at guest address 0x82BB6020"))):
            p = write_log(root, "m_" + nm, BASE + [ln] + END)
            rc, out, _ = run(tool, [p])
            check("marker %s is a PROBLEM (exit 1)" % nm, rc == 1 and "none" not in out.split("PROBLEMS")[1].split("ERROR-LEVEL")[0], "rc %d\n%s" % (rc, out))
        # 9c two PROBLEM names on one physical line: both listed.
        p = write_log(root, "twoprob", BASE + [L(85, "[rdr][PROBLEM] aaa_one: x [rdr][PROBLEM] bbb_two: y")] + END)
        rc, out, _ = run(tool, [p])
        check("two PROBLEMs on one physical line: both named", "aaa_one" in out and "bbb_two" in out.split("PROBLEMS")[1].split("ERROR-LEVEL")[0], out)
        p = write_log(root, "oddprob", BASE + [L(86, "[scene][live] census start"), "  tail of a long line [rdr][PROBLEM]  odd-name.x without a colon"] + END)
        rc, out, _ = run(tool, [p])
        check("a PROBLEM on a continuation line with odd spacing / name still fails", rc == 1 and "odd-name" in out.split("PROBLEMS")[1].split("ERROR-LEVEL")[0],
              "rc %d\n%s" % (rc, out))

        # 10 (review D2) setting echoes / positions / plainly-not-loss names are not SHORTFALLS / UNSORTED; CamelCase names are read as words.
        p = write_log(root, "d2", BASE + [L(90, "[wwe2k14][pacing] frame 0: late-frame threshold set to 100 (passed 0x64)"),
                                          L(91, "[host][2d] first refused row at record 4454 (once per mode entry): index 8"),
                                          L(92, "[scene][2d] run totals: frames 600, 2d-whole 600, quads2d: frames 614 (whole 614, with holes 0), clamp 84"),
                                          L(93, "[scene][wrestlers] skip frame 4218 obj skel:A8F78BE0"),
                                          L(94, "[scene][live] wrestlers frame 3828: SkippedSmall 18 CapRefused 3 EntryUnknownModel 2"),
                                          L(95, "[scene][weapon_rows] latch_proxy_rows_dropped 4, late 2")] + END)
        rc, out, _ = run(tool, [p])
        sf = out.split("SHORTFALLS")[1].split("SHOTS")[0]
        un = out.split("UNSORTED")[1]
        check("D2: no setting echo / record position in SHORTFALLS", "threshold" not in sf and "first_refused" not in sf, sf)
        check("D2: dropped / late / CapRefused are SHORTFALLS", "latch_proxy_rows_dropped 4" in sf and "late 2" in sf and "cap_refused 3" in sf, sf)
        check("D2: whole / clamp / skip frame / skipped small / unknown model not in UNSORTED",
              not any(w in un for w in ("whole", "clamp", "skip_frame", "skippedsmall", "skipped_small", "entryunknownmodel", "entry_unknown_model")), un)

        # 11 a windowed counter (non-zero in one window, 0 in the last) is a SHORTFALL and fails --zero: the max decides, not the last value.
        p = write_log(root, "win", BASE + [L(100, census(5)), L(101, census(0))] + END)
        rc, out, _ = run(tool, [p, "--zero", "capped"])
        check("windowed: capped 5 then 0 fails --zero capped", rc == 1 and "ZERO FAILED" in out and "capped" in out.split("ZERO FAILED")[1], "rc %d\n%s" % (rc, out[-500:]))
        check("windowed: capped 5 then 0 is a SHORTFALL", "capped" in out.split("SHORTFALLS")[1].split("SHOTS")[0], out)

        # 12 --zero NAME absent fails (the path never ran); --zero-or-absent NAME passes.
        rc, out, _ = run(tool, [os.path.join(root, "clean"), "--zero", "no_such_counter"])
        check("--zero on an absent counter fails and says absent", rc == 1 and "absent" in out.split("ZERO FAILED")[1], "rc %d\n%s" % (rc, out[-400:]))
        rc, out, _ = run(tool, [os.path.join(root, "clean"), "--zero-or-absent", "no_such_counter"])
        check("--zero-or-absent on an absent counter passes", rc == 0, "rc %d" % rc)

        # 13 a run that never reached `bench: done` (crashed / killed) is not clean; --allow-unfinished lets it through.
        p = write_log(root, "unfin", BASE + [L(110, census(0))])
        rc, out, _ = run(tool, [p])
        check("no bench: done fails (exit 1) and RUN says so", rc == 1 and "NOT reached" in out.split("FLAGS")[0], "rc %d\n%s" % (rc, out[:600]))
        rc, out, _ = run(tool, [p, "--allow-unfinished"])
        check("--allow-unfinished passes it", rc == 0, "rc %d" % rc)

        # 14 a logs folder with several runs: the newest BY NUMBER (not mtime), said out loud.
        d = os.path.join(root, "many", "logs")
        os.makedirs(d)
        for num, body in (("010", BASE + [L(120, "[wwe2k14] marker_ten")] + END), ("002", BASE + [L(120, "[wwe2k14] marker_two")] + END)):
            with open(os.path.join(d, "wwe2k14_%s.log" % num), "w", encoding="utf-8") as fh:
                fh.write("\n".join(body) + "\n")
        os.utime(os.path.join(d, "wwe2k14_010.log"), (1000000000, 1000000000))   # 010 is OLDER on disk: by mtime 002 would win
        rc, out, _ = run(tool, [os.path.join(root, "many"), "--expect", "marker_ten"])
        check("folder: newest by number (010) read and said", rc == 0 and "wwe2k14_010.log" in out and "2 run logs" in out, "rc %d\n%s" % (rc, out[:600]))

        # 15 rotated parts (wwe2k14_NNN.K.log, the highest K is the oldest: it holds the run's start) are joined in order.
        d = os.path.join(root, "rot", "logs")
        os.makedirs(d)
        parts = {"wwe2k14_001.2.log": BASE + [L(130, "[rdr][PROBLEM] early_one: in the oldest part")],
                 "wwe2k14_001.1.log": [L(131, census(9))], "wwe2k14_001.log": [L(132, census(0))] + END}
        for n, body in parts.items():
            with open(os.path.join(d, n), "w", encoding="utf-8") as fh:
                fh.write("\n".join(body) + "\n")
        rc, out, _ = run(tool, [os.path.join(d, "wwe2k14_001.log"), "--zero", "capped"])
        check("rotated parts joined: the oldest part's PROBLEM and route are read", rc == 1 and "early_one" in out and "start scene synth" in out, "rc %d\n%s" % (rc, out[:900]))
        check("rotated parts joined: 3 parts said, capped 9 in the middle part fails --zero", "3 parts" in out and "capped" in out.split("ZERO FAILED")[1], out[-600:])

        # 16 --expect is not met by a setting echo or by the route's own `log` line (they are the harness's words, not the game's event).
        p = write_log(root, "echo", BASE + [L(140, "[wwe2k14] modified: wwe2k14_note = 'event prop id 9 -> PACH 1' (source: command line)"),
                                            L(141, "[wwe2k14][input] line 7: want event prop id 9 -> PACH 1 (frame 3)")] + END)
        rc, out, _ = run(tool, [p, "--expect", "event prop id 9 -> PACH 1"])
        check("--expect met only by echoes fails", rc == 1 and "event prop id 9" in out.split("EXPECTED NOT FOUND")[1], "rc %d\n%s" % (rc, out[-400:]))

        # 17 (review D3) the default output is short: runtime/code flags counted not listed, shots first / last only; --full prints them.
        many = BASE + [L(150, "[wwe2k14] modified: knob_%02d = 'true' (source: runtime/code)" % i) for i in range(40)] + END
        p = write_log(root, "short", many)
        cap = os.path.join(root, "short", "captures")
        os.makedirs(cap)
        for i in range(30):
            open(os.path.join(cap, "001_sc_x_%02d.png" % i), "wb").close()
        open(os.path.join(cap, "001__sheet.jpg"), "wb").close()
        rc, out, _ = run(tool, [p])
        check("D3: default: runtime/code flags counted, not listed; shots first / last + sheet",
              "knob_05" not in out and "41 modified" in out and "001_sc_x_00.png" in out and "001_sc_x_29.png" in out and "001_sc_x_15.png" not in out
              and "001__sheet.jpg" in out and "vsync=false" in out, out)
        rc, out, _ = run(tool, [p, "--full"])
        check("D3: --full lists every flag and shot", "knob_05" in out and "001_sc_x_15.png" in out, out[-500:])

        # 18 (S111 BLACKFRAME) the host's runs of black frames: each [host] black frames line is listed with its branch, cause and inputs, in order, timed from
        #    frames_NNN.csv beside the log; a 2d_only run (menus) is counted and listed only with --full; the lines never decide the exit; no such line -> none.
        bl = [L(160, "[host] black frames 5000-5030 (n 30, held 1): no_last3d <- never_fed | inputs: ours 1 flat 0 no_scene 0 loading 0 never_fed [5 6] | why: never_fed: required record input(s) not stamped since this scene began (first: bit 5)"),
              L(161, "[host] black frames 5031-5086 (n 56, held 0): no_last3d <- miss_loading | inputs: ours 1 flat 0 no_scene 0 loading 56 never_fed - | why: not resident: 2 asset(s) loading"),
              L(162, "[host] black frames 6000-6400 (n 401, held 0): 2d_only | inputs: ours 0 flat 1 no_scene 0 loading 0 never_fed - | why: flat frame"),
              L(163, "[host] black spans 3 (frames black 487, held 1), lines printed 3")]
        p = write_log(root, "black", BASE + bl + END)
        with open(os.path.join(root, "black", "logs", "frames_001.csv"), "w") as fh:
            fh.write("frame,t_seconds,frame_ms\n" + "".join("%d,%.3f,16.7\n" % (f, f / 60.0) for f in range(4990, 5100)))
        rc, out, _ = run(tool, [p])
        sec = out.split("BLACK FRAMES")[1].split("UNSORTED")[0] if "BLACK FRAMES" in out else ""
        check("BLACK: black runs never decide the exit (exit 0)", rc == 0, "rc %d\n%s" % (rc, out[-600:]))
        check("BLACK: the two runs are listed in order with branch <- cause and their CSV times",
              0 <= sec.find("frames 5000-5030 (n 30, held 1) t 83.33-83.83 s (0.50 s): no_last3d <- never_fed") < sec.find("frames 5031-5086 (n 56, held 0) t 83.85-84.77 s (0.92 s): no_last3d <- miss_loading"), sec)
        check("BLACK: the inputs and the why are kept", "never_fed [5 6]" in sec and "loading 56" in sec and "why: not resident: 2 asset(s) loading" in sec, sec)
        check("BLACK: a 2d_only run is counted, not listed, by default; the total is said",
              "6000-6400" not in sec and "plus 1 2d_only run(s), 401 frames" in sec and "total: 3 run(s), 487 black frames, 1 held" in sec, sec)
        rc, out, _ = run(tool, [p, "--full"])
        check("BLACK: --full lists the 2d_only run", "frames 6000-6400 (n 401, held 0): 2d_only" in out, out[-800:])
        rc, out, _ = run(tool, [p, "--json"])
        try:
            j = json.loads(out)
            check("BLACK: --json carries the runs with their times", len(j["black_spans"]) == 3 and abs(j["black_spans"][0]["t0"] - 83.333) < 0.01
                  and j["black_spans"][1]["what"] == "no_last3d <- miss_loading" and j["black_total"]["spans"] == 3, out[:400])
        except (ValueError, KeyError, IndexError, TypeError) as e:
            check("BLACK: --json carries the runs", False, "%s %s" % (e, out[:300]))
        rc, out, _ = run(tool, [os.path.join(root, "clean")])
        check("BLACK: a run with no black line says none", "none (no `[host] black frames` line" in (out.split("BLACK FRAMES")[1] if "BLACK FRAMES" in out else ""), out[-600:])

        rc, out, _ = run(tool, [os.path.join(root, "nothing_here")])
        check("unreadable path exits 2", rc == 2)
        rc, out, _ = run(tool, [os.path.join(root, "m_crash"), "--json"])
        try:
            j = json.loads(out)
            check("--json is one object with the same exit", j["exit"] == 1 and "shortfalls" in j and "error_groups" in j and rc == 1, out[:300])
        except ValueError:
            check("--json is one object", False, out[:200])
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("run_read_selftest: %s" % ("FAILED " + ", ".join(fails) if fails else "all cases pass"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
