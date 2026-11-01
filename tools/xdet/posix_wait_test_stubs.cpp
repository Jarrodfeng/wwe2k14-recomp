// Session 77: the few SDK symbols threading_posix.cpp needs besides threading.cpp and timer_queue.cpp, for the
// stand-alone POSIX wait test (tools\xdet\posix_wait_test.cpp). Logging is off (no logger), the API level is the
// test's minimum (28), the host time is the real clock.
#include <rex/chrono/chrono.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <chrono>

namespace rex {
namespace cvar {
// The SDK's settings are not used by the test: every setting keeps its default.
std::optional<size_t> RegisterFlag(FlagEntry entry) {
  (void)entry;
  return std::nullopt;
}
void UnregisterFlag(std::string_view name) { (void)name; }
}  // namespace cvar
int GetAndroidApiLevel() { return 28; }
LogCategoryId RegisterLogCategory(const char* name) {
  (void)name;
  return LogCategoryId{0};
}
spdlog::logger* GetLoggerRaw(LogCategoryId category) {
  (void)category;
  return nullptr;
}
namespace chrono {
uint64_t Clock::QueryHostSystemTime() {
  // FILETIME-style 100 ns ticks since 1601 are not needed by the test: any monotonic value works.
  return uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
}
}  // namespace chrono
}  // namespace rex
