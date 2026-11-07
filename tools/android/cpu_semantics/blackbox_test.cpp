// Session 73: the crash path of src\blackbox.h on ARM64 (run_blackbox_test.ps1: NDK static build, QEMU in Docker).
// Order as in the game: the black box's crash recorder first, then (Session 95) the SDK's real seh_initialize()
// (core/seh_posix.cpp, runtime.cpp:111), then a stand-in for the SDK's exception handler (core/exception_handler_posix.cpp,
// installed later by the MMIO handler): a fault it owns is fixed and returns; one it does not own puts the previous handler
// back and returns. Expected: the owned fault passes silently, then the chosen crash is recorded ("CRASH: <signal> ...")
// and ends the process by that signal - no hang, no silent continue.
//   blackbox_test [segv | bus | ill | fpe | fatal]    (default segv)
//   segv: a null write.  bus: a read past the end of a file mapping.  ill: an undefined instruction.
//   fpe: raise(SIGFPE) (si_code <= 0: a returning handler would continue).  fatal: "FATAL ERROR: ..." on stderr, then abort().
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/platform/exceptions.h>

#include "../../../src/blackbox.h"

namespace {
struct sigaction g_previous[32];
uint8_t* g_page = nullptr;

void SdkLikeHandler(int signal_number, siginfo_t* info, void*) {
  auto* address = static_cast<uint8_t*>(info->si_addr);
  if (g_page && address >= g_page && address < g_page + 4096) {
    mprotect(g_page, 4096, PROT_READ | PROT_WRITE);  // owned (as the SDK's MMIO / watch faults): fixed
    return;
  }
  sigaction(signal_number, &g_previous[signal_number], nullptr);  // not owned: on to the previous handler
}
}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "segv";
  wwe2k14::blackbox::Open();
  wwe2k14::blackbox::InstallCrashRecorder();
  wwe2k14::blackbox::Step("test: recorder installed");
  rex::initialize_seh();  // the real one: it keeps the recorder as the action to go on to
  wwe2k14::blackbox::Step("test: initialize_seh done");
  struct sigaction action = {};
  action.sa_sigaction = SdkLikeHandler;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &action, &g_previous[SIGSEGV]);
  sigaction(SIGILL, &action, &g_previous[SIGILL]);
  g_page = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  g_page[0] = 1;  // an owned fault
  wwe2k14::blackbox::Step("test: owned fault handled");
  std::printf("owned fault handled\n");
  std::fflush(stdout);
  if (!std::strcmp(mode, "segv")) {
    volatile int* null_pointer = nullptr;
    *null_pointer = 2;
  } else if (!std::strcmp(mode, "bus")) {
    int fd = open("bus_file.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
    void* map = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);  // the file is empty: the page has no data
    *static_cast<volatile int*>(map) = 3;
  } else if (!std::strcmp(mode, "ill")) {
    asm volatile(".inst 0x00000000");
  } else if (!std::strcmp(mode, "fpe")) {
    raise(SIGFPE);
  } else if (!std::strcmp(mode, "fatal")) {
    std::fprintf(stderr, "FATAL ERROR: %s\n", "test message: the GPU device was lost");
    std::fflush(stderr);
    std::abort();
  }
  std::printf("NOT REACHED\n");
  return 0;
}
