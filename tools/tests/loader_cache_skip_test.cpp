// entry: wwe2k14::loader_cache::CacheInsertSkips
// Unit test for src/loader_cache_skip_pred.h and the hook src/loader_cache_skip.h itself (S105 A6S): the SDK is stubbed (WWE2K14_LCS_UNIT),
// the hook runs against a fake guest memory and a fake "original". Build: clang++ -std=c++20 -I src tools/tests/loader_cache_skip_test.cpp
// (tools\tests\loader_cache_skip_test.ps1 does it, argv[1] = repo root). 0 game runs.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#define WWE2K14_LCS_UNIT 1
struct Reg { union { uint64_t u64; uint32_t u32; }; };
struct Ctx { Reg r3, r4; };
static bool cv_wwe2k14_loader_cache_copy = false, cv_wwe2k14_loader_cache_hits = false;
static int g_hitOrigCalls = 0;
static uint64_t g_hitOrigRet = 1;  // the stand-in for a name hit
static int g_origCalls = 0, g_logs = 0, g_warns = 0;
static uint64_t g_origRet = 1;
static std::map<uint32_t, uint32_t> g_mem;
static uint32_t g_ptrBad = 0;  // a request pointer the stub calls implausible
namespace wwe2k14::load_probe {
inline bool SafeRead32(uint32_t a, uint32_t* o) { auto it = g_mem.find(a); if (it == g_mem.end()) return false; *o = it->second; return true; }
}
namespace wwe2k14::safe_pages {
inline bool PlausibleGuestPtr(uint32_t p, uint32_t) { return p != 0 && p != g_ptrBad; }
}
#define REXCVAR_DEFINE_BOOL(name, def, cat, help) static bool& cvh_##name = cv_##name;
#define REXCVAR_GET(name) (cvh_##name)
#define REXLOG_INFO(...) (++g_logs)
#define REXLOG_WARN(...) (++g_warns)
#define REX_HOOK_RAW(name) static void name(Ctx& ctx, uint8_t* base)
static void __imp__sub_82C54DE0(Ctx& ctx, uint8_t*) { ++g_origCalls; ctx.r3.u64 = g_origRet; }  // the game's copy / move: "returns 1" stand-in
static void __imp__sub_82C54A00(Ctx& ctx, uint8_t*) { ++g_hitOrigCalls; ctx.r3.u64 = g_hitOrigRet; }  // the game's name lookup: "returns 1" = a hit
#include "loader_cache_skip.h"
using namespace wwe2k14::loader_cache;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static std::string Slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  for (size_t r; (r = s.find('\r')) != std::string::npos;) s.erase(r, 1);
  return s;
}
// The planted twins.
static bool TwinA6(uint32_t, uint32_t f116) { return f116 != 0xFFFFFFFEu; }   // A6.md's predicate: skip when +116 != -2 alone
static bool TwinNeverSkips(uint32_t, uint32_t) { return false; }

// Runs the hook with a request holding (f140, f116); returns the guest r3 afterwards.
static uint64_t Run(uint32_t f140, uint32_t f116, uint32_t cacheWord = 0x70001000, uint32_t r3 = 0x70001000) {
  const uint32_t req = 0x40002000;
  g_mem.clear();
  g_mem[0x839400AC] = cacheWord;
  g_mem[req + 140] = f140;
  g_mem[req + 116] = f116;
  Ctx c{};
  c.r3.u64 = r3;
  c.r4.u64 = req;
  g_origCalls = 0;
  sub_82C54DE0(c, nullptr);
  return c.r3.u64;
}

// S110 CACHEHIT: runs the lookup hook with the cache word / r3 given; returns r3 afterwards.
static uint64_t RunLookup(uint32_t cacheWord = 0x70001000, uint32_t r3 = 0x70001000) {
  g_mem.clear();
  g_mem[0x839400AC] = cacheWord;
  Ctx c{};
  c.r3.u64 = r3;
  c.r4.u64 = 0x40002000;
  g_hitOrigCalls = 0;
  sub_82C54A00(c, nullptr);
  return c.r3.u64;
}
// Planted twin hits_kept: the old behaviour (the lookup always runs the game's code).
static uint64_t HitsKeptTwin() {
  Ctx c{};
  c.r3.u64 = 0x70001000;
  c.r4.u64 = 0x40002000;
  __imp__sub_82C54A00(c, nullptr);
  return c.r3.u64;
}

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : ".";
  // Predicate: the dump's own values (A6S.md): M102 own buffer +116 = 0x2B, +140 = 0 -> skip (the stall).
  CHECK(CacheInsertSkips(0, 0x2B));
  CHECK(!CacheInsertSkips(1, 0xFFFFFFFEu));  // E032 loader-allocated: the game's MOVE
  CHECK(CacheInsertSkips(0, 0xFFFFFFFEu));   // synthetic (ADV 6)
  CHECK(CacheInsertSkips(1, 0x2B));
  CHECK(TwinA6(0, 0xFFFFFFFEu) != CacheInsertSkips(0, 0xFFFFFFFEu));  // planted twin 1 fails here
  CHECK(TwinNeverSkips(0, 0x2B) != CacheInsertSkips(0, 0x2B));
  // The hook end to end.
  g_skipped = 0, g_moved = 0;
  CHECK(Run(0, 0x2B) == 0 && g_origCalls == 0);             // M102: skipped, r3 = 0, the original (the copy) NOT called
  CHECK(g_skipped.load() == 1 && g_moved.load() == 0 && g_logs == 1);  // the first skip logs
  CHECK(Run(1, 0xFFFFFFFEu) == 1 && g_origCalls == 1);  // E032: the MOVE path calls the original
  CHECK(g_skipped.load() == 1 && g_moved.load() == 1);
  CHECK(Run(1, 0x2B) == 0 && g_origCalls == 0);              // loader-allocated flag but own tag: skipped
  CHECK(Run(0, 0xFFFFFFFEu) == 0 && g_origCalls == 0);       // synthetic class: skipped (the A6.md twin would copy)
  CHECK(g_skipped.load() == 3);
  // F2: a move counts only when the original returned 1 (the size gate refused otherwise).
  g_origRet = 0;
  CHECK(Run(1, 0xFFFFFFFEu) == 0 && g_origCalls == 1 && g_moved.load() == 1);
  g_origRet = 1;
  // Any failed check = the game's own code.
  CHECK(Run(0, 0x2B, 0x70001000, 0x70009999) == 1 && g_origCalls == 1);  // r3 is not the cache object
  CHECK(Run(0, 0x2B, 0, 0x70001000) == 1 && g_origCalls == 1);                    // cache word differs
  g_ptrBad = 0x40002000;
  CHECK(Run(0, 0x2B) == 1 && g_origCalls == 1);                          // implausible request pointer
  g_ptrBad = 0;
  CHECK(g_fallback.load() == 3 && g_warns == 1);  // F1: every guard-fail path counted, one WARN on the first
  // The test-only switch restores the game's copy.
  cv_wwe2k14_loader_cache_copy = true;
  CHECK(Run(0, 0x2B) == 1 && g_origCalls == 1);
  cv_wwe2k14_loader_cache_copy = false;
  CHECK(g_skipped.load() == 3 && g_fallback.load() == 3);  // the switch on is not a fallback
  // Census line cadence: the first of each kind, then every kLogEvery-th event in total.
  g_skipped = 0, g_moved = 0, g_fallback = 0, g_logs = 0;
  for (uint64_t i = 0; i < kLogEvery; ++i) Run(0, 0x2B);
  CHECK(g_logs == 2);  // the first skip and the kLogEvery-th
  Run(1, 0xFFFFFFFEu);
  CHECK(g_logs == 3);  // the first move

  // S110 CACHEHIT: the name lookup sub_82C54A00 answers "miss" without the game's code.
  g_skipped = 0, g_moved = 0, g_fallback = 0, g_hits_dropped = 0, g_logs = 0;
  CHECK(RunLookup() == 0 && g_hitOrigCalls == 0 && g_hits_dropped.load() == 1 && g_logs == 1);  // guard ok, switch off: r3 = 0, original NOT called
  cv_wwe2k14_loader_cache_hits = true;
  CHECK(RunLookup() == 1 && g_hitOrigCalls == 1 && g_hits_dropped.load() == 1);  // switch on: the game's own hit
  cv_wwe2k14_loader_cache_hits = false;
  CHECK(RunLookup(0x70001000, 0x70009999) == 1 && g_hitOrigCalls == 1 && g_hits_dropped.load() == 1);  // r3 is not the cache object: the original
  CHECK(RunLookup(0) == 1 && g_hitOrigCalls == 1);  // cache word unreadable / differs: the original
  g_hitOrigRet = 0;
  CHECK(RunLookup() == 0 && g_hitOrigCalls == 0);  // an original miss would be 0 too; dropped all the same
  g_hitOrigRet = 1;
  CHECK(HitsKeptTwin() != 0);  // planted twin hits_kept: calls the original, so the hit comes back; the real hook returns 0 (case 1 above)
  g_hitOrigCalls = 0;
  g_hits_dropped = 0; g_logs = 0;
  for (uint64_t i = 0; i < kLogEvery; ++i) RunLookup();
  CHECK(g_logs == 2);  // the first drop and the kLogEvery-th

  // Source checks: default false, test-only help text, no Win32 in the hook, included in the app after load_probe.h and outside the #if.
  // Source checks: default false, test-only help text, no Win32 in the hook, included in the app after load_probe.h and outside the #if.
  const std::string h = Slurp(root + "/src/loader_cache_skip.h");
  CHECK(!h.empty());
  CHECK(h.find("REXCVAR_DEFINE_BOOL(wwe2k14_loader_cache_copy, false") != std::string::npos);
  CHECK(h.find("TEST ONLY") != std::string::npos);
  CHECK(h.find("REXCVAR_DEFINE_BOOL(wwe2k14_loader_cache_hits, false") != std::string::npos);
  CHECK(h.find("_WIN32") == std::string::npos && h.find("windows.h") == std::string::npos);
  const std::string app = Slurp(root + "/src/wwe2k14_app.h");
  const size_t inc = app.find("#include \"loader_cache_skip.h\"");
  CHECK(inc != std::string::npos && inc > app.find("#include \"load_probe.h\"") && inc < app.find("#if !WWE2K14_STREAM()"));
  std::printf(fails ? "loader_cache_skip_test: %d FAILED\n" : "loader_cache_skip_test: all passed\n", fails);
  return fails ? 1 : 0;
}
