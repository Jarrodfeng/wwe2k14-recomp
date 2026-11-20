// entry: wwe2k14::load_probe::LoadWatch::Update
// Unit test for src/load_watch.h (S105 A6W, fix round 1) + static checks of the hook sites. Build: clang++ -std=c++20 -I src
// tools/tests/load_watch_test.cpp (tools\tests\load_watch_test.ps1 does it and passes the repo root as argv[1]). 0 game runs.
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "load_stall.h"
#include "load_watch.h"
#include "load_watch_dump.h"
#include "guest_stack_walk.h"
using wwe2k14::load_probe::LoadWatch;
using wwe2k14::load_probe::Sample;
using wwe2k14::load_probe::StallWatch;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static const int64_t L = 5'000'000;
static uint64_t Frames(int64_t now) { return uint64_t(now / 16'667); }  // 60 fps guest frames derived from the time
// The watch thread's tick: every 250 ms.
static bool T(LoadWatch& w, Sample s, int64_t now) { return w.Update(s, Frames(now), now, L); }
static Sample S(uint32_t req, uint32_t st, uint64_t op = 0, uint64_t rd = 0, uint32_t job = 0, uint32_t q = 0, uint32_t pause = 0,
                uint64_t comp = 0) {
  Sample s;
  s.req = req, s.state = st, s.opens = op, s.reads = rd, s.job = job, s.queue = q, s.pause = pause, s.completions = comp;
  return s;
}
static std::string Slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  for (size_t r; (r = s.find('\r')) != std::string::npos;) s.erase(r, 1);
  return s;
}
int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : ".";
  // (a) THE PAST BUG (REPORT_025): the worker steps twice (state 9), then blocks; frames keep advancing; no opens, no reads. The old
  // StallWatch is fed only by worker steps, so it never fires; the new watch does after 5 s.
  {
    StallWatch old;
    LoadWatch nw;
    bool old_fired = false, new_fired = false;
    int64_t at = 0;
    old_fired |= old.Update(0x800, 9, 1'000'000, L, Frames(1'000'000));
    old_fired |= old.Update(0x800, 9, 1'002'000, L, Frames(1'002'000));
    for (int64_t t = 1'000'000; t <= 20'000'000; t += 250'000)
      if (T(nw, S(0x800, 9, 3, 40), t) && !new_fired) new_fired = true, at = t;
    CHECK(!old_fired);
    CHECK(new_fired);
    CHECK(at > 6'000'000 && at <= 6'500'000);
  }
  // NEW 1: the worker is blocked while OTHER threads keep reading (Wwise streams, Bink, saves). Those reads are on no PAC handle, so
  // the sink never counts them: the tick's counters stay flat and the watch fires.
  {
    using namespace wwe2k14::load_probe;
    LoadWatch w;
    bool any = false;
    for (int64_t t = 0; t <= 8'000'000; t += 250'000) {
      const uint64_t reads = g_pac_handles.Has(0x7777) ? uint64_t(t) : 0;  // a foreign read (a handle that never opened a .pac) is not counted
      any |= T(w, S(0x810, 9, 3, reads), t);
    }
    CHECK(any);
  }
  // NEW 2: the hand-off wait: req is 0 (the worker set [0x839400A4] = 0 after posting), the read-end job is busy, nothing moves: fires.
  {
    LoadWatch w;
    bool any = false;
    for (int64_t t = 0; t <= 5'000'000; t += 250'000) any |= T(w, S(0, 0, 3, 40, 0x820), t);
    CHECK(!any);
    CHECK(T(w, S(0, 0, 3, 40, 0x820), 5'250'000));
    // a finished completion is progress
    LoadWatch c;
    any = false;
    for (int64_t t = 0; t <= 60'000'000; t += 250'000) any |= T(c, S(0, 0, 3, 40, 0x820, 0, 0, uint64_t(t / 2'000'000)), t);
    CHECK(!any);
    // a non-empty queue with no request current and nothing moving also fires
    LoadWatch q;
    any = false;
    for (int64_t t = 0; t <= 6'500'000; t += 250'000) any |= T(q, S(0, 0, 3, 40, 0, 0x830), t);
    CHECK(any);
  }
  // NEW 3: the pause flag [0x8362A0B0] set (a stream / movie change skips the step on purpose): never fires, however long.
  {
    LoadWatch w;
    bool any = false;
    for (int64_t t = 0; t <= 60'000'000; t += 250'000) any |= T(w, S(0x840, 6, 3, 40, 0, 0, 1), t);
    CHECK(!any);
    for (int64_t t = 60'250'000; t <= 65'000'000; t += 250'000) any |= T(w, S(0x840, 6, 3, 40, 0, 0, 0), t);  // after it clears, a fresh 5 s must pass
    CHECK(!any);
    CHECK(T(w, S(0x840, 6, 3, 40, 0, 0, 0), 65'250'000));
  }
  // (b) the MVL.md picture: the worker keeps stepping, the pair never changes, no PAC opens / reads, 5 s: fires once.
  {
    LoadWatch w;
    bool any = false;
    for (int64_t t = 0; t <= 5'000'000; t += 250'000) any |= T(w, S(0x900, 7, 10, 20), t);
    CHECK(!any);
    CHECK(T(w, S(0x900, 7, 10, 20), 5'250'000));
    CHECK(!T(w, S(0x900, 7, 10, 20), 5'500'000));
  }
  // (c) a slow but healthy load: PAC reads / opens / a changing state every few seconds: never fires.
  {
    LoadWatch w, o, s;
    bool any = false;
    uint64_t reads = 0, opens = 0;
    for (int64_t t = 0; t <= 60'000'000; t += 250'000) {
      if (t % 2'000'000 == 0) ++reads;
      if (t % 3'000'000 == 0) ++opens;
      any |= T(w, S(0xA00, 9, 5, reads), t);
      any |= T(o, S(0xA01, 9, opens, 0), t);
      any |= T(s, S(0xA02, uint32_t(1 + (t / 3'000'000) % 20)), t);
    }
    CHECK(!any);
  }
  // (d) nothing loading (req, job, queue all 0: idle menu): never fires, however long.
  {
    LoadWatch w;
    bool any = false;
    for (int64_t t = 0; t <= 120'000'000; t += 250'000) any |= T(w, S(0, 0, 7, 7), t);
    CHECK(!any);
    CHECK(!T(w, S(0xB00, 1, 7, 7), 120'250'000));
    CHECK(!T(w, S(0xB00, 1, 7, 7), 124'000'000));
  }
  // (e) the world is stopped (frames do not advance): never fires; 99 frames no; 100 yes; a sleep of this thread restarts the clock.
  {
    LoadWatch w, a, b, z;
    bool any = false;
    for (int64_t t = 0; t <= 60'000'000; t += 250'000) any |= w.Update(S(0xC00, 9, 1, 1), 5000, t, L);
    CHECK(!any);
    for (int64_t t = 0; t <= 6'000'000; t += 250'000) any |= a.Update(S(0xC01, 9, 1, 1), 5000 + (t == 6'000'000 ? 99 : 0), t, L);
    CHECK(!any);
    for (int64_t t = 0; t <= 6'000'000; t += 250'000) any |= b.Update(S(0xC02, 9, 1, 1), 5000 + (t == 6'000'000 ? 100 : 0), t, L);
    CHECK(any);
    any = false;
    for (int64_t t = 0; t <= 1'000'000; t += 250'000) any |= T(z, S(0xC03, 9, 1, 1), t);
    for (int64_t t = 61'000'000; t <= 64'000'000; t += 250'000) any |= T(z, S(0xC03, 9, 1, 1), t);
    CHECK(!any);
    for (int64_t t = 64'250'000; t <= 67'000'000; t += 250'000) any |= T(z, S(0xC03, 9, 1, 1), t);
    CHECK(any);
  }
  // (f) fires once per stretch; re-arms after progress.
  {
    LoadWatch w;
    int fires = 0;
    int64_t t = 0;
    for (; t <= 12'000'000; t += 250'000) fires += T(w, S(0xD00, 9, 1, 1), t);
    CHECK(fires == 1);
    fires += T(w, S(0xD00, 9, 1, 2), t);
    CHECK(fires == 1);
    for (t += 250'000; t <= 24'000'000; t += 250'000) fires += T(w, S(0xD00, 9, 1, 2), t);
    CHECK(fires == 2);
    fires = 0;
    for (t += 250'000; t <= 36'000'000; t += 250'000) fires += T(w, S(0xD01, 9, 1, 2), t);
    CHECK(fires == 1);
  }
  // limit 0 never fires.
  {
    LoadWatch w;
    bool any = false;
    for (int64_t t = 0; t <= 30'000'000; t += 250'000) any |= w.Update(S(0xE00, 9, 1, 1), Frames(t), t, 0);
    CHECK(!any);
  }
  // The PAC filter and handle table: only paths ending .pac (any case) count as opens; only reads on those handles count.
  {
    using namespace wwe2k14::load_probe;
    CHECK(IsPacPath("game:\\MOT08.PAC", 15) && IsPacPath("a.pac", 5) && IsPacPath("a.Pac", 5));
    CHECK(!IsPacPath("game:\\Game_Sound_M.pck", 22) && !IsPacPath("a.pa", 4) && !IsPacPath("", 0) && !IsPacPath("x.pac.bak", 9));
    CHECK(!OpenCounts("game:/Game_Sound_M.pck", 22, 0));  // a non-pac open does not count
    CHECK(!OpenCounts("game:/ARENA.PAC", 15, 0xC0000034u));  // a failed pac open does not count
    CHECK(OpenCounts("game:/MOT08.PAC", 15, 0));
    PacHandles t;
    CHECK(!t.Has(0x3333));
    t.Add(0x3333);
    CHECK(t.Has(0x3333) && !t.Has(0x1111) && !t.Has(0));
    t.Add(0), t.Add(0xFFFFFFFFu);
    CHECK(!t.Has(0) && !t.Has(0xFFFFFFFFu));
    for (uint32_t h = 0x4000; h < 0x4000 + 200; ++h) t.Add(h);  // the ring wraps without trouble
    CHECK(t.Has(0x4000 + 199) && !t.Has(0x4000) && !t.Has(0x3333));
  }
  // Static checks on the real hook sites.
  {
    const std::string lp = Slurp(root + "/src/load_probe.h");
    const size_t a0 = lp.find("REX_HOOK_RAW(sub_82C37070) {"), b0 = lp.find("REX_HOOK_RAW(sub_82C37E48) {");
    CHECK(a0 != std::string::npos && b0 != std::string::npos && a0 < b0);
    const std::string hook = lp.substr(a0, b0 - a0);
    CHECK(hook.find("g_worker_last_step_us.store(") != std::string::npos);  // the one timestamp store
    CHECK(hook.find("mutex") == std::string::npos && hook.find("lock_guard") == std::string::npos);
    // the watch thread: named, sleeps with std::this_thread (not Sleep), catches everything, guest stacks only, no lock in its tick
    const size_t t0 = lp.find("inline void LoadWatchTick("), t1 = lp.find("// Current game sequence id");
    CHECK(t0 != std::string::npos && t1 != std::string::npos && t0 < t1);
    const std::string tick = lp.substr(t0, t1 - t0);
    CHECK(tick.find("NameThread(L\"load_watch\")") != std::string::npos);
    CHECK(tick.find("std::this_thread::sleep_for") != std::string::npos && tick.find("Sleep(") == std::string::npos);
    CHECK(tick.find("catch (...)") != std::string::npos);
    CHECK(tick.find("FlightDump(\"loader watch\", detail, nullptr, -1)") != std::string::npos);
    CHECK(tick.find("lock_guard") == std::string::npos && tick.find("unique_lock") == std::string::npos);
    CHECK(tick.find("TranslateVirtual") == std::string::npos);  // guest memory only through SafeRead32
    CHECK(tick.find("g_load_watch_stop") != std::string::npos && tick.find("REX_KERNEL_STATE()") != std::string::npos);  // teardown
    CHECK(tick.find("0x8362A0B0u") != std::string::npos && tick.find("0x8394024Cu") != std::string::npos &&
          tick.find("FillQueueHead(") != std::string::npos);  // pause flag, read-end object, queue
    CHECK(tick.find("LogGuestStacks()") != std::string::npos && lp.find("#ifndef _WIN32", t0) != std::string::npos);  // Android stacks
    // the completions counter: both paths of the read-end callback hook
    size_t cn = 0;
    for (size_t p = 0; (p = lp.find("g_loader_completions.fetch_add(", p)) != std::string::npos; ++p) ++cn;
    CHECK(cn == 2);
    // the counters: PAC opens noted BEFORE the log switch's early return, reads noted in both read overrides
    const std::string ko = Slurp(root + "/src/kernel_overrides.h");
    const size_t o0 = ko.find("load_probe::NoteOpen("), o1 = ko.find("if (!REXCVAR_GET(wwe2k14_log_file_opens)) return;");
    CHECK(o0 != std::string::npos && o1 != std::string::npos && o0 < o1);
    const std::string ft = Slurp(root + "/src/file_trace.h");
    size_t n = 0;
    for (size_t p = 0; (p = ft.find("load_probe::NoteRead(file_handle)", p)) != std::string::npos; ++p) ++n;
    CHECK(n == 2);
    const std::string app = Slurp(root + "/src/wwe2k14_app.h");
    CHECK(app.find("load_probe::StartLoadWatch()") != std::string::npos);
    const size_t sd = app.find("void OnShutdown() override");
    CHECK(sd != std::string::npos && app.find("g_load_watch_stop.store(true", sd) != std::string::npos);
  }
  // ---- S105 A6W2 ----
  // (1) the guarded stack walk (PC run 064: an unguarded read of guest 0x704AFEA0 crashed the dump). Synthetic guest memory: a map of words.
  {
    using namespace wwe2k14;
    std::map<uint32_t, uint32_t> mem;
    // stack 0x70000000..0x70010000: sp 0x70000100 -> caller 0x70000200 (ret at 0x700001F8) -> caller 0x70000300 (ret at 0x700002F8) -> 0x704AFEA0 (unmapped)
    mem[0x70000100] = 0x70000200, mem[0x700001F8] = 0x82001000;
    mem[0x70000200] = 0x70000300, mem[0x700002F8] = 0x82002000;
    mem[0x70000300] = 0x70000400;  // the caller's frame is mapped, its return slot (0x700003F8) is not
    int touched_unmapped = 0;
    auto guarded = [&](uint32_t a, uint32_t* v) {
      if ((a & 3) != 0 || a < 0x1000) return false;
      auto it = mem.find(a);
      if (it == mem.end()) return false;
      *v = it->second;
      return true;
    };
    GuestWalkResult g = WalkGuestStack(0x70000100, 0x70000000, 0x70010000, 24, guarded);
    CHECK(g.callers.size() == 2 && g.callers[0] == 0x82001000 && g.callers[1] == 0x82002000);
    CHECK(std::string(g.end) == "return slot unreadable");
    // sp itself unmapped / unaligned / out of the stack range: ends cleanly, no crash
    CHECK(std::string(WalkGuestStack(0x704AFEA0, 0x70000000, 0x70500000, 24, guarded).end) == "stack word unreadable");
    CHECK(std::string(WalkGuestStack(0x70000101, 0x70000000, 0x70010000, 24, guarded).end) == "stack word unreadable");
    CHECK(WalkGuestStack(0x80000000u, 0x70000000, 0x70010000, 24, guarded).callers.empty());
    // a back-chain that loops or runs backwards ends; the depth is bounded
    mem[0x70000500] = 0x70000500;
    CHECK(WalkGuestStack(0x70000500, 0x70000000, 0x70010000, 24, guarded).callers.empty());
    for (uint32_t a = 0x70001000; a < 0x70001000 + 100 * 16; a += 16) mem[a] = a + 16, mem[a + 8] = 0x82003000;
    CHECK(WalkGuestStack(0x70001000, 0x70000000, 0x70010000, 24, guarded).callers.size() <= 24);
    {
      const std::string t = FormatGuestWalk(0x70000100, 0x70000000, 0x70010000, 24, guarded, [](uint32_t a) { char b[16]; std::snprintf(b, sizeof b, "%08X", a); return std::string(b); });
      CHECK(t == "\n      <- 82001000\n      <- 82002000\n      <- (return slot unreadable)");
    }
    // PLANTED TWIN: the old unguarded walk (every read goes straight to memory) touches the unmapped page: the harness sees it.
    auto unguarded = [&](uint32_t a, uint32_t* v) {
      auto it = mem.find(a);
      if (it == mem.end()) { ++touched_unmapped; *v = 0; return true; }  // a real host read would fault here
      *v = it->second;
      return true;
    };
    WalkGuestStack(0x70000100, 0x70000000, 0x70010000, 24, unguarded);
    CHECK(touched_unmapped > 0);
    touched_unmapped = 0;
    WalkGuestStack(0x70000100, 0x70000000, 0x70010000, 24, guarded);
    CHECK(touched_unmapped == 0);
    // static: FlightGuestThreads walks only through the guarded reader and every FlightDump path reaches it
    const std::string fr = Slurp(root + "/src/flight_recorder.h");
    const size_t f0 = fr.find("inline std::vector<FlightGuestThread> FlightGuestThreads()"), f1 = fr.find("inline std::string FlightLogsPath()");
    CHECK(f0 != std::string::npos && f1 != std::string::npos && f0 < f1);
    const std::string fg = fr.substr(f0, f1 - f0);
    CHECK(fg.find("FormatGuestWalk(") != std::string::npos && fg.find("GuestWordReadable(") != std::string::npos);
    // the only guest loads: the KPCR struct (after its own GuestWordReadable check) and the guarded lambda; no raw word load anywhere
    CHECK(fg.find("load_and_swap") == std::string::npos);
    CHECK(fg.find("GuestLoadBe32Guarded(host, v)") != std::string::npos && fg.find("GuestWordCommitted(a)") != std::string::npos);
    size_t tv = 0;
    for (size_t p = 0; (p = fg.find("TranslateVirtual", p)) != std::string::npos; ++p) ++tv;
    CHECK(tv == 2);  // the KPCR pointer and the lambda's host pointer, nothing else
    CHECK(fg.find("g_load_watch_stop") != std::string::npos);  // teardown stop in the loop
    CHECK(fr.find("FlightGuestThreads);") != std::string::npos);  // FlightDump's helper uses it: every dump path (hang, crash, loader) shares the guard
  }
  // (1b) THE PLANTED TWIN OF RUN 064, rebuilt after review 1: the queue list [[0x839400A8]+4] is a POOL of request slots, never empty after the first
  // load. An idle loader holds slots in states 0 / 11 / 12 (the enqueue reuses those): must NOT fire. One state-1 slot (a request waiting) must fire.
  // The old sampler (the raw head word) fires on the idle pool: the twin is real.
  {
    using namespace wwe2k14::load_probe;
    std::map<uint32_t, uint32_t> mem;
    const uint32_t qobj = 0x70002000, sentinel = 0x70004000;
    mem[0x839400A8] = qobj, mem[qobj + 4] = sentinel;
    // nodes: node k at 0x70005000 + 0x20*k: +0 next, +8 item; items at 0x70006000 + 0x200*k: +104 state, +108 word
    auto build = [&](const std::vector<uint32_t>& states, const std::vector<uint32_t>& w108) {
      uint32_t prev = sentinel;
      for (size_t k = 0; k < states.size(); ++k) {
        const uint32_t node = 0x70005000 + 0x20 * uint32_t(k), item = 0x70006000 + 0x200 * uint32_t(k);
        mem[prev] = node, mem[node] = 0, mem[node + 8] = item, mem[item + 104] = states[k], mem[item + 108] = w108[k];
        prev = node;
      }
    };
    auto rd = [&](uint32_t a, uint32_t* v) { auto it = mem.find(a); if (it == mem.end() || (a & 3)) return false; *v = it->second; return true; };
    auto plausible = [](uint32_t p) { return p >= 0x1000; };
    auto QH = [](auto& r, auto& pl) { Sample q; FillQueueHead(q, r, pl); return q.queue; };
    build({0, 11, 12, 11, 0}, {0, 0, 0, 0, 0});
    LoadWatch fixed, old_sampler;
    bool fixed_fired = false, old_fired = false;
    for (int64_t t = 0; t <= 20'000'000; t += 250'000) {
      uint32_t raw = 0;
      rd(qobj + 4, &raw);
      fixed_fired |= T(fixed, S(0, 0, 3, 40, 0, QH(rd, plausible)), t);
      old_fired |= T(old_sampler, S(0, 0, 3, 40, 0, raw), t);  // the sampler of the first A6W2 build (raw list head)
    }
    CHECK(!fixed_fired);
    CHECK(old_fired);
    // one state-1 slot among idle ones: a request really waiting: the sample is that item and a stuck queue fires
    build({0, 11, 1, 12}, {0, 0, 0, 0});
    CHECK(QH(rd, plausible) == 0x70006000 + 0x200 * 2);
    LoadWatch stuck;
    bool any = false;
    for (int64_t t = 0; t <= 6'500'000; t += 250'000) any |= T(stuck, S(0, 0, 3, 40, 0, QH(rd, plausible)), t);
    CHECK(any);
    // the dequeue's other busy cases: states 13, 15, and +108 == 3
    build({0, 13}, {0, 0});
    CHECK(QH(rd, plausible) != 0);
    build({15, 0}, {0, 0});
    CHECK(QH(rd, plausible) != 0);
    build({0, 12}, {0, 3});
    CHECK(QH(rd, plausible) != 0);
    // unreadable / null / looping words never fire (return 0), bounded
    mem[0x70005000 + 0x20] = 0x70005000;  // node 1 -> node 0: a loop
    build({0, 12}, {0, 0});
    mem[0x70005000 + 0x20] = 0x70005000;
    CHECK(QH(rd, plausible) == 0);
    mem.erase(0x839400A8);
    CHECK(QH(rd, plausible) == 0);
    const std::string lp = Slurp(root + "/src/load_probe.h");
    CHECK(lp.find("FillQueueHead(s,") != std::string::npos && lp.find("SafeRead32(0x839400A8u, &s.queue)") == std::string::npos);
  }
  // (2) the dump fields, production order, from synthetic guest words. Heap: table 0x83A00000, count 2, index 1, entry at +36.
  {
    using namespace wwe2k14::load_probe;
    std::map<uint32_t, uint32_t> mem;
    const uint32_t req = 0x70000800, c = 0x70000900, obj = 0x70000A00, table = 0x83A00000, start = 0x70100000;
    mem[0x8392FFDC] = 2, mem[0x8392FFE4] = table;
    mem[req + 36] = 0x11111111, mem[req + 116] = 0x22222222, mem[req + 112] = 0xDEADDEAD, mem[req + 32] = 0xBADBAD01;
    mem[c + 36] = 0x33333333, mem[c + 40] = 0x44444444, mem[c + 32] = 0xBADBAD02;
    mem[0x839400A8] = 0x77777770, mem[0x77777770 + 4] = 0x55555555, mem[0x8362A0B0] = 0x66666666;
    mem[0x839400AC] = obj, mem[obj + 76] = 1;
    mem[table + 36 + 0] = start, mem[table + 36 + 4] = start + 0x100;  // 16 units
    // nodes (in 16-byte units from the start; w0 low 28 bits = the node's END unit): [0..4) flag 1, [4..6) flag 0, [6..16) flag 1
    mem[start + 0x00] = 4, mem[start + 0x08] = 1;
    mem[start + 0x40] = 6, mem[start + 0x48] = 0;
    mem[start + 0x60] = 16, mem[start + 0x68] = 1;
    auto rd = [&](uint32_t a, uint32_t* v) {
      if ((a & 3) != 0 || a == 0) return false;
      auto it = mem.find(a);
      if (it == mem.end()) return false;
      *v = it->second;
      return true;
    };
    const auto lines = FireLines(rd, req, c);
    CHECK(lines.size() == 3);
    CHECK(lines[0].find("request 0x70000800 +36=11111111 +116=22222222") != std::string::npos);
    CHECK(lines[0].find("read-end object 0x70000900 +36=33333333 +40=44444444") != std::string::npos);
    CHECK(lines[1].find("queue object [0x839400A8]=77777770 list head [[0x839400A8]+4]=55555555 pause gate [0x8362A0B0]=66666666") != std::string::npos);
    CHECK(lines[2].find("index 1 (obj+76)") != std::string::npos && lines[2].find("3 nodes walked") != std::string::npos);
    CHECK(lines[2].find("w8==0 (flag 0): 1 blocks 32 B") != std::string::npos);                          // 2 units
    CHECK(lines[2].find("w8!=0 (flag 1): 2 blocks 224 B, largest 160 B") != std::string::npos);          // 4 + 10 units, largest 10
    // PLANTED TWIN: a field read from the wrong offset would show the neighbour word: the checks above can fail
    CHECK(lines[0].find("DEADDEAD") == std::string::npos && lines[0].find("BADBAD01") == std::string::npos && lines[0].find("BADBAD02") == std::string::npos);
    mem.erase(req + 116);  // an unreadable word prints ???????? and nothing else changes
    CHECK(FireLines(rd, req, c)[0].find("+116=????????") != std::string::npos);
    // a node chain that does not advance, an unreadable node and the node bound stop the walk with a reason
    mem[start + 0x40] = 2;
    CHECK(std::string(BuildHeapStats(rd, table, 1).stop) == "node chain does not advance");
    mem[start + 0x40] = 6, mem.erase(start + 0x68);
    CHECK(std::string(BuildHeapStats(rd, table, 1).stop) == "node unreadable");
    mem[start + 0x68] = 1;
    CHECK(std::string(BuildHeapStats(rd, table, 1, 2).stop) == "truncated");
    CHECK(std::string(BuildHeapStats(rd, table, 2).stop) == "index out of range");
    // no request / no read-end object: those fields print "-", the rest is still produced
    CHECK(FireLines(rd, 0, 0)[0].find("+36=-") != std::string::npos);
    mem.erase(0x839400AC);
    CHECK(FireLines(rd, req, c).back().find("unreadable") != std::string::npos);
    // (3) the CPU window: a spinning worker (~100 %) vs a blocked one (0 %), re-armed when the window restarts
    CpuWindow cw;
    cw.Note(1'000, 1'000'000, 500'000);
    cw.Note(1'000, 2'000'000, 1'500'000), cw.Note(1'000, 6'000'000, 5'500'000);
    CHECK(CpuTextFor(cw, 6'000'000, 5'500'000) == "worker cpu 100 % over the window");
    CpuWindow idle;
    idle.Note(1'000, 1'000'000, 500'000), idle.Note(1'000, 6'000'000, 500'000);
    CHECK(CpuTextFor(idle, 6'000'000, 500'000) == "worker cpu 0 % over the window");
    cw.Note(9'000, 7'000'000, 5'600'000);  // the window restarted: the baseline moves
    CHECK(CpuTextFor(cw, 8'000'000, 5'700'000) == "worker cpu 10 % over the window");
    CHECK(CpuTextFor(cw, 8'000'000, -1).find("unknown") != std::string::npos && CpuTextFor(CpuWindow(), 8'000'000, 5).find("unknown") != std::string::npos);
    CHECK(std::string(WhichLoaderThread("PacComplete", 1, 2)) == "read-end thread" && std::string(WhichLoaderThread("FileReadEndProcessThread", 1, 2)) == "read-end thread");
    CHECK(std::string(WhichLoaderThread("CGameWorkerThread", 2, 2)) == "worker" && std::string(WhichLoaderThread("CGameWorkerThread", 3, 2)).empty() &&
          std::string(WhichLoaderThread("x", 0, 0)).empty());
    // static: the tick prints the fields through FireLines with SafeRead32 and reads thread times of g_worker_ktid
    const std::string lp = Slurp(root + "/src/load_probe.h");
    CHECK(lp.find("FireLines([](uint32_t a, uint32_t* v) { return SafeRead32(a, v); }, s.req, c)") != std::string::npos);
    CHECK(lp.find("ThreadCpuUs(g_worker_ktid") != std::string::npos && lp.find("{}, {} (please send this log)") != std::string::npos);
  }
  std::printf(fails ? "FAILED\n" : "OK\n");
  return fails;
}
