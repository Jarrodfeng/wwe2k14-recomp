// Session 73: the crash path of src\blackbox.h on ARM64 (run_blackbox_test.ps1: NDK static build, QEMU in Docker).
// Order as in the game: the black box's crash recorder first, then a stand-in for the SDK's fault handler with its
// Session 73 behaviour (core/exception_handler_posix.cpp): a fault it owns is fixed and returns; one it does not own puts
// the previous handler back and returns. Expected: the owned fault passes silently, the null write is recorded
// ("CRASH: SIGSEGV ...") and then ends the process with SIGSEGV - no hang.
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>

#include "../../../src/blackbox.h"

namespace {
struct sigaction g_previous;
uint8_t* g_page = nullptr;

void SdkLikeHandler(int signal_number, siginfo_t* info, void*) {
  auto* address = static_cast<uint8_t*>(info->si_addr);
  if (g_page && address >= g_page && address < g_page + 4096) {
    mprotect(g_page, 4096, PROT_READ | PROT_WRITE);  // owned (as the SDK's MMIO / watch faults): fixed
    return;
  }
  sigaction(signal_number, &g_previous, nullptr);  // not owned: on to the previous handler
}
}  // namespace

int main() {
  wwe2k14::blackbox::Open();
  wwe2k14::blackbox::InstallCrashRecorder();
  wwe2k14::blackbox::Step("test: recorder installed");
  struct sigaction action = {};
  action.sa_sigaction = SdkLikeHandler;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &action, &g_previous);
  g_page = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  g_page[0] = 1;  // an owned fault
  wwe2k14::blackbox::Step("test: owned fault handled");
  std::printf("owned fault handled\n");
  std::fflush(stdout);
  volatile int* null_pointer = nullptr;
  *null_pointer = 2;  // a crash
  std::printf("NOT REACHED\n");
  return 0;
}
