// Session 73: runtime functions the generated PPC instruction tests link against but never reach (logging set-up, the
// indirect-call table). Built only into the ARM64 test executable (run_ppc_tests_arm64.ps1); each says so and stops if
// a test does reach it.
#include <cstdio>
#include <cstdlib>

#include <rex/logging/api.h>
#include <rex/ppc/func.h>

namespace rex {
LogCategoryId RegisterLogCategory(const char*) { return LogCategoryId(0); }
spdlog::logger* GetLoggerRaw(LogCategoryId) { return nullptr; }
namespace runtime {
PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  std::fprintf(stderr, "ppc_tests stub: ResolveIndirectFunction(0x%08X) reached\n", guest_address);
  std::abort();
}
}  // namespace runtime
}  // namespace rex

extern "C" int __android_log_write(int, const char*, const char* text) {
  std::fprintf(stderr, "%s\n", text ? text : "");
  return 0;
}
