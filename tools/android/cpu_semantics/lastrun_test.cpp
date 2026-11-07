// Session 95 (DEATH): check::LastRun (src\launcher_mobile\phone_check.h) on planted logs folders (run_lastrun_test.ps1, host clang).
// Real report files are read from out\s95\reports and out\triage (r014, r021, report_005); the other cases are written here.
#include <cstdio>
#include <fstream>
#include <string>

#include "../../../src/launcher_mobile/phone_check.h"

namespace fs = std::filesystem;
static int g_fail = 0;

static void Put(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

static fs::path Copy(const fs::path& from, const fs::path& to) {
  fs::remove_all(to);
  fs::create_directories(to / "logs");
  for (const char* n : {"startup.txt", "last_game_exit.txt"}) {
    if (fs::exists(from / n)) fs::copy_file(from / n, to / "logs" / n);
  }
  return to;
}

static void Expect(const char* name, const std::string& got, const char* must_have, bool nothing = false) {
  const bool ok = nothing ? got.empty() : got.find(must_have) != std::string::npos;
  std::printf("%s %-34s -> %s\n", ok ? "PASS" : "FAIL", name, got.empty() ? "(nothing)" : got.c_str());
  if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
  const fs::path reports = argc > 1 ? argv[1] : "out/s95/reports";
  const fs::path triage = argc > 2 ? argv[2] : "out/triage/report_005_s93";
  const fs::path tmp = fs::temp_directory_path() / "lastrun_test";
  using mlauncher::check::LastRun;
  Expect("report_005 (SIGNALED 11)", LastRun(Copy(triage, tmp / "a"), 33), "crashed");
  Expect("report_005 is not low memory", LastRun(tmp / "a", 33), "SIGSEGV");
  Expect("r014 (CRASH line, status 6)", LastRun(Copy(reports / "r014", tmp / "b"), 34), "SIGABRT");
  Expect("r021 (a normal Home, UNKNOWN 0)", LastRun(Copy(reports / "r021", tmp / "c"), 34), "", true);
  Put(tmp / "d" / "logs" / "startup.txt", "   1.0  step: logging up\n   9.0  first frame\n");
  Put(tmp / "d" / "logs" / "stderr.txt", "FATAL ERROR: GPU device lost (VK_ERROR_DEVICE_LOST)\n");
  Expect("stderr FATAL ERROR", LastRun(tmp / "d", 34), "GPU device lost");
  Put(tmp / "e" / "logs" / "startup.txt", "   1.0  step: logging up\n   9.0  first frame\n");
  Expect("API 29, played, no quit marker", LastRun(tmp / "e", 29), "ended unexpectedly");
  Expect("API 34, played, no reason: nothing", LastRun(tmp / "e", 34), "", true);
  Put(tmp / "f" / "logs" / "startup.txt", "   1.0  step: logging up\n   9.0  first frame\n  50.0  quit to the launcher\n");
  Expect("API 29 with a quit marker: nothing", LastRun(tmp / "f", 29), "", true);
  Put(tmp / "g" / "logs" / "startup.txt", "   1.0  step: logging up\n   2.0  memory low (trim 15)\n");
  Put(tmp / "g" / "logs" / "last_game_exit.txt", "time: x\nreason: SIGNALED\nstatus: 9\n");
  Expect("SIGKILL (9) keeps its old words", LastRun(tmp / "g", 34), "low memory");
  // Session 95 (HANG): src\hang_guard.h's lines.
  const std::string played = "   1.0  step: logging up\n   9.0  first frame\n";
  Put(tmp / "h" / "logs" / "startup.txt", played + "  80.0  HANG? no guest frame for 20 s, guest frame 600\n 120.0  HANG: fence wait, no guest frame for 60 s, guest frame 600\n");
  Put(tmp / "h" / "logs" / "last_game_exit.txt", "time: x\nreason: EXIT_SELF\nstatus: 3\n");
  Expect("HANG names the side", LastRun(tmp / "h", 34), "froze: fence wait");
  Put(tmp / "i" / "logs" / "startup.txt", played + " 120.0  HANG: guest frame, no guest frame for 60 s, guest frame 600\n 130.0  HANG over: guest frames and presents go on, guest frame 610\n");
  Expect("HANG over clears it", LastRun(tmp / "i", 34), "", true);
  Put(tmp / "j" / "logs" / "startup.txt", played + "  80.0  HANG? no guest frame for 20 s, guest frame 600\n");
  Expect("HANG? alone is nothing", LastRun(tmp / "j", 34), "", true);
  Put(tmp / "k" / "logs" / "startup.txt", played + "  20.0  BLACK: no present for 10 s, 600 guest frames ran (presents 9, acquire timeouts 0)\n  50.0  quit to the launcher\n");
  Expect("BLACK survives a blind quit", LastRun(tmp / "k", 34), "went black");
  Put(tmp / "l" / "logs" / "startup.txt", played + "  20.0  BLACK: no present for 10 s\n  30.0  BLACK over: presents came back\n  50.0  quit to the launcher\n");
  Expect("BLACK over clears it", LastRun(tmp / "l", 34), "", true);
  Put(tmp / "m2" / "logs" / "startup.txt", played + "  60.0  BLACK: dark picture, all 3 captures, 30 to 120 s in\n  70.0  quit to the launcher\n");
  Expect("dark picture after a quit says nothing", LastRun(tmp / "m2", 34), "", true);
  Put(tmp / "m" / "logs" / "startup.txt", played + "  80.0  HANG: paint thread, no present for 60 s\n  90.0  CRASH: SIGSEGV (bad memory access), code 1, fault address 0x0, at libmain.so+0x10 (thread 5).\n");
  Expect("a crash after a HANG is the crash", LastRun(tmp / "m", 34), "crashed");
  std::printf(g_fail ? "FAILED %d\n" : "all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
