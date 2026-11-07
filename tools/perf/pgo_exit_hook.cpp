// Session 95 (guest-code PGO training build only; tools/perf/guest_variant_exe.py --variant pgogen links this in).
// The game ends with std::_Exit, so the profile runtime's atexit writer never runs, and ReXApp::OnClosing has no hook.
// A thread writes the exe's profile (the instrumented recompiled guest code) every 10 s to LLVM_PROFILE_FILE; the last write
// before a run ends is at most 10 s old. Not compiled into any other build. Contains nothing from the game.
#include <windows.h>
extern "C" int __llvm_profile_write_file(void);
static DWORD WINAPI PgoWriter(LPVOID) {
  for (;;) {
    Sleep(10000);
    __llvm_profile_write_file();
  }
}
static struct PgoWriterStart {
  PgoWriterStart() { CreateThread(nullptr, 0, PgoWriter, nullptr, 0, nullptr); }
} g_pgo_writer_start;
