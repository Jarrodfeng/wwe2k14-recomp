// lane 147: unit test of src/render_signal.h (queue / flush / counters), the shipped header, no SDK. Exit 0 = pass.
#include "../../src/render_signal.h"
#include <cstdio>
#include <thread>
using namespace wwe2k14::render_signal;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
int main() {
  State s;
  CHECK(Count(s, "roster_ready") == 0);
  std::vector<std::string> got;
  CHECK(Drain(s, [&](const std::string& w, const std::string&) { got.push_back(w); }) == 0);   // empty queue: nothing emitted
  std::thread t([&] { Post(s, "roster_ready", "\"pacs\": 3, \"models\": 40, \"seconds\": 4.5"); Post(s, "roster_failed", ""); });   // the render thread
  t.join();
  CHECK(Count(s, "roster_ready") == 1 && Count(s, "roster_failed") == 1);
  CHECK(Count(s, "nonsense") == 0 && Known("roster_ready") && !Known("nonsense"));
  std::string f0;
  CHECK(Drain(s, [&](const std::string& w, const std::string& f) { got.push_back(w); if (got.size() == 1) f0 = f; }) == 2);
  CHECK(got.size() == 2 && got[0] == "roster_ready" && got[1] == "roster_failed");   // post order
  CHECK(f0.find("\"pacs\": 3") != std::string::npos);
  CHECK(Drain(s, [&](const std::string&, const std::string&) { got.push_back("again"); }) == 0 && got.size() == 2);   // flushed once only
  CHECK(Count(s, "roster_ready") == 1);   // the counter outlives the flush (the script's wait reads it)
  Post(s, "roster_ready", "");   // a second build counts again
  CHECK(Count(s, "roster_ready") == 2);
  if (!g_fail) std::printf("render_signal_test: all pass\n");
  return g_fail ? 1 : 0;
}
