// lane 147: unit test of src/render_signal.h (queue / flush / counters), the shipped header, no SDK. Exit 0 = pass.
#include "../../src/render_signal.h"
#include "../../src/renderer_scene/roster_signal.h"
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
  // RRS: the stream-free live loader's tracker (src/renderer_scene/roster_signal.h). The wait is simulated as input_script.h kWaitRender reads it: Count > 0 within the timeout, else it fails.
  using rdr_scene::RosterSignal;
  auto wait_frames = [](State& st, const char* what, int timeout, auto&& per_frame) {   // frames until Count(what) > 0, or -1 = the wait's timeout fired
    for (int f = 0; f < timeout; ++f) {
      per_frame();
      if (Count(st, what) > 0) return f;
    }
    return -1;
  };
  {
    State a;
    RosterSignal r(&a);
    r.Ask("pac\\ch\\ch001.pac"), r.Ask("pac\\ch\\ch002.pac"), r.Ask("pac\\bg\\bg05.pac");   // two bodies and a non-wrestler pac (ignored)
    r.Done("pac\\ch\\ch001.pac", true);
    int f = wait_frames(a, "roster_ready", 200, [&] { r.Poll(); });
    CHECK(f == -1 && Count(a, "roster_ready") == 0);   // one body still outstanding: not ready
    r.Done("pac\\ch\\ch002.pac", true);
    f = wait_frames(a, "roster_ready", 200, [&] { r.Poll(); });
    CHECK(f >= RosterSignal::kSettlePolls - 1 && f < 200);   // ready after the settle
    for (int i = 0; i < 100; ++i) r.Poll();
    CHECK(Count(a, "roster_ready") == 1 && Count(a, "roster_failed") == 0);   // exactly once
    std::string ff;
    Drain(a, [&](const std::string& w, const std::string& fl) { if (w == "roster_ready") ff = fl; });
    CHECK(ff.find("\"pacs\": 2") != std::string::npos && ff.find("\"models\": 2") != std::string::npos && ff.find("\"seconds\"") != std::string::npos);
  }
  {
    State a;   // one ok + one failed -> ready (with failed 1), never failed
    RosterSignal r(&a);
    r.Ask("pac\\ch\\ch001.pac"), r.Ask("pac\\ch\\ch002.pac");
    r.Done("pac\\ch\\ch001.pac", false);
    r.Done("pac\\ch\\ch002.pac", true);
    for (int i = 0; i < 100; ++i) r.Poll();
    CHECK(Count(a, "roster_ready") == 1 && Count(a, "roster_failed") == 0);
    std::string ff;
    Drain(a, [&](const std::string&, const std::string& fl) { ff = fl; });
    CHECK(ff.find("\"failed\": 1") != std::string::npos);
  }
  {
    State a;   // S103 DLW: every body failed to LOAD but the guest rows draw them (DLC packages the loader cannot read): answered and drawn = ready, never roster_failed
    RosterSignal r(&a);
    r.Ask("pac\\ch\\ch385.pac"), r.Ask("pac\\ch\\ch386.pac");
    r.Done("pac\\ch\\ch385.pac", false, true);
    r.Done("pac\\ch\\ch386.pac", false, true);
    for (int i = 0; i < 100; ++i) r.Poll();
    CHECK(Count(a, "roster_ready") == 1 && Count(a, "roster_failed") == 0);
    State b;   // PLANT (the old call, no guest_drawn): all failed = roster_failed
    RosterSignal r2(&b);
    r2.Ask("pac\\ch\\ch385.pac"), r2.Done("pac\\ch\\ch385.pac", false);
    for (int i = 0; i < 100; ++i) r2.Poll();
    CHECK(Count(b, "roster_failed") == 1 && Count(b, "roster_ready") == 0);
  }
  {
    State a;   // S103 DLF: a failed body counts as drawn only when the guest rows built a model (Poll's counter): 0 built = roster_failed
    RosterSignal r(&a);
    r.Ask("pac\\ch\\ch385.pac"), r.Done("pac\\ch\\ch385.pac", false);
    for (int i = 0; i < 100; ++i) r.Poll(0);
    CHECK(Count(a, "roster_failed") == 1 && Count(a, "roster_ready") == 0);
    State b;   // 1 guest actor drawn for 2 failed bodies: ready, payload guest 1 / failed 1
    RosterSignal r2(&b);
    r2.Ask("pac\\ch\\ch385.pac"), r2.Ask("pac\\ch\\ch386.pac");
    r2.Done("pac\\ch\\ch385.pac", false), r2.Done("pac\\ch\\ch386.pac", false);
    std::string ff;
    for (int i = 0; i < 100; ++i) r2.Poll(1);
    Drain(b, [&](const std::string&, const std::string& fl) { ff = fl; });
    CHECK(ff.find("\"guest\": 1") != std::string::npos && ff.find("\"failed\": 1") != std::string::npos);
  }
  {
    State a;   // re-arm: a new key asked during the settle delays ready until the second batch has settled too
    RosterSignal r(&a);
    r.Ask("pac\\ch\\ch001.pac"), r.Done("pac\\ch\\ch001.pac", true);
    for (int i = 0; i < RosterSignal::kSettlePolls - 1; ++i) r.Poll();
    r.Ask("pac\\ch\\ch002.pac");
    for (int i = 0; i < 100; ++i) r.Poll();
    CHECK(Count(a, "roster_ready") == 0);   // second body outstanding
    r.Done("pac\\ch\\ch002.pac", true);
    for (int i = 0; i < RosterSignal::kSettlePolls - 1; ++i) r.Poll();
    CHECK(Count(a, "roster_ready") == 0);
    r.Poll();
    CHECK(Count(a, "roster_ready") == 1);
  }
  {
    State a;   // every body failed -> roster_failed (once), never roster_ready
    RosterSignal r(&a);
    r.Ask("pac\\ch\\ch001.pac"), r.Ask("pac\\ch\\ch002.pac");
    r.Done("pac\\ch\\ch001.pac", false);
    r.Done("pac\\ch\\ch002.pac", false);
    for (int i = 0; i < 100; ++i) r.Poll();
    CHECK(Count(a, "roster_failed") == 1 && Count(a, "roster_ready") == 0);
  }
  {
    State a;   // PLANT: the signal is never posted (the loader's completion is not wired) -> the wait's timeout fires
    RosterSignal r(&a);
    r.Ask("pac\\ch\\ch001.pac");
    CHECK(wait_frames(a, "roster_ready", 300, [&] { r.Poll(); }) == -1);   // no Done: the timeout fires (this is the pre-RRS stream-free behaviour)
  }
  if (!g_fail) std::printf("render_signal_test: all pass\n");
  return g_fail ? 1 : 0;
}
