// Session 95 (LADDER): src\launcher_mobile\ladder.h and the rung arguments in mobile_settings.h on the real report files
// (out\s95\reports r014 r021 r048 r192, out\triage\report_005_s93) and planted folders. Host clang, no game run.
#include <cstdio>
#include <fstream>
#include <string>

#include "../../../src/launcher_mobile/ladder.h"
#include "../../../src/launcher_mobile/mobile_settings.h"

namespace fs = std::filesystem;
using namespace mlauncher::ladder;
static int g_fail = 0;

static void Put(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}
static fs::path Copy(const fs::path& from, const fs::path& to) {
  fs::remove_all(to);
  fs::create_directories(to / "logs");
  for (const char* n : {"startup.txt", "stderr.txt", "last_game_exit.txt"})
    if (fs::exists(from / n)) fs::copy_file(from / n, to / "logs" / n);
  return to;
}
static void Check(const char* name, bool ok, const std::string& got = std::string()) {
  std::printf("%s %-56s %s\n", ok ? "PASS" : "FAIL", name, got.c_str());
  if (!ok) ++g_fail;
}
static const char* KindName(RunEnd::Kind k) {
  static const char* n[] = {"none", "good", "bad", "memory", "unknown", "precompile"};
  return n[k];
}
static Row R(const char* key, char cls, int rung = 0, const char* ver = "v1", const char* hash = "h") {
  Row r;
  r.key = key, r.version = ver, r.hash = hash, r.rung = rung, r.cls = cls;
  return r;
}

int main(int argc, char** argv) {
  const fs::path reports = argc > 1 ? argv[1] : "out/s95/reports";
  const fs::path triage = argc > 2 ? argv[2] : "out/triage/report_005_s93";
  const fs::path tmp = fs::temp_directory_path() / "ladder_test";
  fs::remove_all(tmp);

  // 1. The real reports.
  RunEnd e = Classify(Copy(triage, tmp / "a"), 33);
  Check("report_005 (SIGSEGV with memory warnings) = bad CRASH", e.kind == RunEnd::kBad && e.name == "CRASH", KindName(e.kind));
  e = Classify(Copy(reports / "r014", tmp / "b"), 34);
  Check("r014 (CRASH line, SIGABRT) = bad CRASH", e.kind == RunEnd::kBad && e.name == "CRASH", KindName(e.kind));
  e = Classify(Copy(reports / "r021", tmp / "c"), 34);
  Check("r021 (UNKNOWN 0 after the quit marker) = good", e.kind == RunEnd::kGood, KindName(e.kind));
  e = Classify(Copy(reports / "r048", tmp / "d"), 34);
  Check("r048 (SIGKILL after the quit marker) = good", e.kind == RunEnd::kGood, KindName(e.kind));
  e = Classify(Copy(reports / "r192", tmp / "e"), 34);
  Check("r192 (SIGKILL after the quit marker) = good", e.kind == RunEnd::kGood, KindName(e.kind));

  // 2. Planted ends.
  Put(tmp / "f" / "logs" / "startup.txt", "   1.0  step: logging up\n   9.0  step: first frame: the game is running\n");
  Put(tmp / "f" / "logs" / "last_game_exit.txt", "reason: UNKNOWN\nstatus: 0\nimportance: 100\n");
  e = Classify(tmp / "f", 34);
  Check("played, no quit, UNKNOWN 0 = unknown (never counts)", e.kind == RunEnd::kUnknown, KindName(e.kind));
  Check("unknown is not recorded", !Record(tmp / "f" / "logs" / "run_history.txt", e, "v1", "", "h", 0));
  Put(tmp / "g" / "logs" / "startup.txt", "   1.0  step: logging up\n  60.0  HANG: no frame for 20 s\n");
  e = Classify(tmp / "g", 34);
  Check("HANG: line = bad HANG", e.kind == RunEnd::kBad && e.name == "HANG", KindName(e.kind));
  Put(tmp / "h" / "logs" / "startup.txt", "   1.0  step: logging up\n  70.0  BLACK: no draws for 10 s\n");
  e = Classify(tmp / "h", 34);
  Check("BLACK: line = bad BLACK", e.kind == RunEnd::kBad && e.name == "BLACK", KindName(e.kind));
  // The guard's own "over" lines and the dark-picture guess, read as check::LastRun reads them (s95-android merge).
  const std::string kPlayed = "   1.0  step: logging up\n   9.0  step: first frame: the game is running\n";
  const std::string kQuit = " 400.0  the player quit to the launcher\n";
  Put(tmp / "h1" / "logs" / "startup.txt", kPlayed + "  40.0  HANG? no guest frame for 20 s\n  80.0  HANG: guest frame, no guest frame for 60 s\n  90.0  HANG over: guest frames and presents go on\n" + kQuit);
  e = Classify(tmp / "h1", 34);
  Check("HANG: then HANG over then a quit = good, no card", e.kind == RunEnd::kGood && !e.hang_pending, KindName(e.kind));
  Put(tmp / "h2" / "logs" / "startup.txt", kPlayed + "  40.0  HANG? no present for 20 s\n  45.0  HANG over: guest frames and presents go on\n" + kQuit);
  e = Classify(tmp / "h2", 34);
  Check("HANG? then HANG over = no pending warning", e.kind == RunEnd::kGood && !e.hang_pending, KindName(e.kind));
  Put(tmp / "h3" / "logs" / "startup.txt", kPlayed + "  40.0  HANG? no present for 20 s\n" + kQuit);
  e = Classify(tmp / "h3", 34);
  Check("HANG? still open at the quit = pending warning", e.kind == RunEnd::kGood && e.hang_pending, KindName(e.kind));
  Put(tmp / "h4" / "logs" / "startup.txt", kPlayed + "  40.0  BLACK: no present for 10 s\n  50.0  BLACK over: presents came back\n" + kQuit);
  e = Classify(tmp / "h4", 34);
  Check("BLACK: then BLACK over then a quit = good", e.kind == RunEnd::kGood, KindName(e.kind));
  Put(tmp / "h5" / "logs" / "startup.txt", kPlayed + " 130.0  BLACK: dark picture, all 3 captures, 30 to 120 s in\n" + kQuit);
  e = Classify(tmp / "h5", 34);
  Check("dark picture then a quit = good", e.kind == RunEnd::kGood, KindName(e.kind));
  Put(tmp / "h6" / "logs" / "startup.txt", kPlayed + " 130.0  BLACK: dark picture, all 3 captures, 30 to 120 s in\n");
  e = Classify(tmp / "h6", 34);
  Check("dark picture, no quit = bad BLACK", e.kind == RunEnd::kBad && e.name == "BLACK", KindName(e.kind));
  Put(tmp / "i" / "logs" / "startup.txt", "   0.5  black box\n");
  Put(tmp / "i" / "logs" / "last_game_exit.txt", "reason: SIGNALED\nstatus: 11\nimportance: 100\n");
  e = Classify(tmp / "i", 34);
  Check("a crash with no boot-stage note = unknown (needs a step line)", e.kind == RunEnd::kUnknown, KindName(e.kind));
  Put(tmp / "j" / "logs" / "startup.txt", "   1.0  step: logging up\n   5.0  step: first frame: the game is running\n  80.0  memory low\n");
  Put(tmp / "j" / "logs" / "last_game_exit.txt", "reason: LOW_MEMORY\nstatus: 0\nimportance: 100\n");
  e = Classify(tmp / "j", 34);
  Check("LOW_MEMORY in front = memory kill", e.kind == RunEnd::kMemory && e.foreground, KindName(e.kind));
  Put(tmp / "j" / "logs" / "last_game_exit.txt", "reason: LOW_MEMORY\nstatus: 0\nimportance: 400\n");
  e = Classify(tmp / "j", 34);
  Check("LOW_MEMORY in the background is not recorded", !Record(tmp / "j" / "logs" / "run_history.txt", e, "v1", "", "h", 0));
  Put(tmp / "k" / "logs" / "startup.txt",
      "   1.0  step: logging up\n   5.0  step: first frame: the game is running\n  50.0  the shader precompile ended: done (quit to the launcher)\n");
  e = Classify(tmp / "k", 34);
  Check("a precompile end is not a play", e.kind == RunEnd::kPrecompile, KindName(e.kind));
  Put(tmp / "l" / "logs" / "startup.txt",
      "   1.0  step: logging up\n   5.0  step: first frame: the game is running\n  60.0  the player quit to the launcher\n");
  e = Classify(tmp / "l", 34);
  Check("early quit (55 s after the first frame)", e.kind == RunEnd::kGood && e.play_seconds > 50 && e.play_seconds < 60);

  // 3. History: written once, decisions.
  const fs::path hist = tmp / "l" / "logs" / "run_history.txt";
  Check("record adds a row", Record(hist, e, "v1", "", "h", 0));
  Check("same run again adds nothing", !Record(hist, e, "v1", "", "h", 0));
  Advice a = Decide(ReadHistory(hist), "v1", "", "h", 0);
  Check("early quit asks the picture card", a.early_key == e.key, a.early_key);
  AnswerEarly(hist, e.key, true);
  a = Decide(ReadHistory(hist), "v1", "", "h", 0);
  Check("answered NO = a named bad end, no more card", a.early_key.empty() && a.bad_in_row == 1);

  Check("B B -> SAFE MODE step 1", Decide({R("1", 'G'), R("2", 'B'), R("3", 'B')}, "v1", "", "h", 0).safe_next == 1);
  Check("B G B -> nothing", Decide({R("1", 'B'), R("2", 'G'), R("3", 'B')}, "v1", "", "h", 0).safe_next == -1);
  Check("one B -> nothing", Decide({R("1", 'B')}, "v1", "", "h", 0).safe_next == -1);
  Check("B M B -> M is skipped, still two", Decide({R("1", 'B'), R("2", 'M'), R("3", 'B')}, "v1", "", "h", 0).safe_next == 1);
  Check("an update starts clean", Decide({R("1", 'B'), R("2", 'B')}, "v2", "", "h", 0).safe_next == -1);
  Check("a settings change starts clean", Decide({R("1", 'B'), R("2", 'B')}, "v1", "", "h2", 0).safe_next == -1);
  Check("two bad at rung 1 -> step 2", Decide({R("1", 'B', 0), R("2", 'B', 1), R("3", 'B', 1)}, "v1", "", "h", 1).safe_next == 2);
  Check("one bad at rung 1 after two at 0 -> nothing",
        Decide({R("1", 'B', 0), R("2", 'B', 0), R("3", 'B', 1)}, "v1", "", "h", 1).safe_next == -1);
  Check("rung 4 offers no further step", Decide({R("1", 'B', 4), R("2", 'B', 4)}, "v1", "", "h", 4).safe_next == -1);
  Check("M M -> memory card", Decide({R("1", 'M'), R("2", 'M')}, "v1", "", "h", 0).memory_card);
  Check("M G M -> no memory card", !Decide({R("1", 'M'), R("2", 'G'), R("3", 'M')}, "v1", "", "h", 0).memory_card);

  // 4. The rung file.
  const fs::path rf = tmp / "rung.txt";
  RungState rs;
  rs.rung = 3, rs.mediump_changed = true;
  WriteRung(rf, rs, "v1", "");
  Check("rung file read back", ReadRung(rf, "v1", "").rung == 3 && ReadRung(rf, "v1", "").mediump_changed);
  Check("rung file for another version = 0", ReadRung(rf, "v2", "").rung == 0);
  Check("rung file for another driver = 0", ReadRung(rf, "v1", "adreno_x").rung == 0);

  // 5. The arguments, and the precompile key.
  mlauncher::Settings s;
  mlauncher::LaunchContext c;
  auto has = [](const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); };
  auto args0 = mlauncher::GameArgs(s, c);
  Check("rung 0: no plain flag", !has(args0, "--gpu_plain_mode=on"));
  c.rung = 1;
  auto args1 = mlauncher::GameArgs(s, c);
  Check("rung 1: plain mode + one worker", has(args1, "--gpu_plain_mode=on") && has(args1, "--gpu_native_pipeline_workers=1"));
  Check("rung 1: shadows and fps as the player had them",
        !has(args1, "--gpu_dynamic_shadows=false") && !has(args1, "--wwe2k14_step_hz=30"));
  c.rung = 2;
  auto args2 = mlauncher::GameArgs(s, c);
  Check("rung 2: shadows off, crowd 30, 30 FPS",
        has(args2, "--gpu_dynamic_shadows=false") && has(args2, "--gpu_scene_crowd_density=30") && has(args2, "--wwe2k14_step_hz=30"));
  Check("rungs 1-2 do not change the precompile key",
        mlauncher::PrecompileKey(mlauncher::ApplyRung(s, 1), "v", "d") == mlauncher::PrecompileKey(s, "v", "d") &&
            mlauncher::PrecompileKey(mlauncher::ApplyRung(s, 2), "v", "d") == mlauncher::PrecompileKey(s, "v", "d"));
  Check("rung 3 (mediump off, saved) changes the key", [&] {
    mlauncher::Settings t = s;
    t.mediump = false;
    return mlauncher::PrecompileKey(t, "v", "d") != mlauncher::PrecompileKey(s, "v", "d");
  }());

  // 6. The advice from frames_NNN.csv.
  const double f21 = MeanFps(reports / "r021" / "frames_021.csv"), f192 = MeanFps(reports / "r192" / "frames_192.csv");
  const double f14 = MeanFps(reports / "r014" / "frames_014.csv"), f48 = MeanFps(reports / "r048" / "frames_048.csv");
  char b[96];
  std::snprintf(b, sizeof(b), "r021 %.1f r192 %.1f r014 %.1f r048 %.1f", f21, f192, f14, f48);
  Check("r021 gives the 30 FPS card; r014 is fine; r192 / r048 too short to say", f21 > 0 && f21 < 52 && f14 >= 52 && f192 == 0 && f48 == 0, b);

  std::printf("%d failure(s)\n", g_fail);
  return g_fail ? 1 : 0;
}
