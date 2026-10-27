// wwe2k14 phone launcher - the event queue both platforms share (Session 71).
#include <deque>
#include <mutex>

#include "mobile_platform.h"

namespace mlauncher::platform {

namespace {
std::mutex g_mutex;
std::deque<Event> g_events;
}  // namespace

bool PollEvent(Event& e) {
  std::lock_guard lock(g_mutex);
  if (g_events.empty()) return false;
  e = std::move(g_events.front());
  g_events.pop_front();
  return true;
}

void Post(Event e) {
  std::lock_guard lock(g_mutex);
  g_events.push_back(std::move(e));
}

}  // namespace mlauncher::platform
