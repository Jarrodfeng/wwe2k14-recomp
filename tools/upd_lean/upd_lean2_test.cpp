// upd_lean2_test: the offline proof of lane U3 (S102; wwe2k14_rdr_upd_lean bit 16; docs\renderer\UPD_LEAN.md "Round 2"). 0 game runs, no SDK, no game memory.
//   upd_lean2_test              all arms (exit 0 when every check passed)
//   upd_lean2_test plant N      the same with planted fault N: it must exit 1 with a FAIL line
//     11 the in-place consumer palette keeps the frame's first palette -> "consumer.equal"
// It runs the SHIPPED code:
// wrestler_contrib.cpp SubmitConsumer(in_place) against the general Put, arena_world_hook.h Tracker with bit 16 flipped on and off against a pure-off run. What the readers consume
// (the emitted wrestler records, the arena worlds) must be byte-identical.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/upd_lean.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_arena_world.h"
#include "../../src/scene_live/arena_world_hook.h"
#include "../../src/scene_live/wrestler_contrib.h"

namespace ul = wwe2k14::upd_lean;
using namespace rdr_scene;
using namespace rdr_scene::live;

// Stand-ins for the writer's registry, which the contributor itself does not need.
namespace rdr_scene::live {
void Register(Contributor*) {}
bool Enabled() { return true; }
void EndFrame(uint64_t) {}
void AddSink(Sink*) {}
void RemoveSink(Sink*) {}
}  // namespace rdr_scene::live

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const char* why = "") {
  ++g_checks;
  std::printf("%s  %-28s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) ++g_fail;
}
static double NowNs() { return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- consumer palettes ---------------------------------------------------------------------------------------------------------------
static Mat4 Pose1(float seed, int i) {
  Mat4 m = IdentityMat4();
  m.m[0] = std::cos(seed + 0.01f * i), m.m[1] = std::sin(seed + 0.01f * i), m.m[4] = -m.m[1], m.m[5] = m.m[0];
  m.m[12] = seed * 3 + i, m.m[13] = 2.f * i - seed, m.m[14] = 0.5f * i;
  return m;
}
static std::vector<Mat4> Pose(int n, float seed) {
  std::vector<Mat4> v(n);
  for (int i = 0; i < n; ++i) v[i] = Pose1(seed, i);
  return v;
}
static std::vector<uint8_t> EmitBytes(WrestlerContributor& w, Counters& c, uint64_t frame) {
  SceneTables t;
  FrameInfo f;
  f.guest_frame = frame;
  w.Emit(f, t, c);
  return SerializeRecord(t);
}
static void ConsumerEqual() {
  std::mt19937 rng(81);
  WrestlerContributor a, b;   // a: the general Put, b: the in-place path (flipped on and off by the draw)
  const uint32_t skels[6] = {0x50001000, 0x50002000, 0x50003000, 0x50004000, 0x50005000, 0x50006000};
  const int counts[6] = {109, 109, 64, 40, 88, 12};
  uint64_t cf = 100;
  bool ok = true;
  uint64_t emits = 0, mism = 0;
  bool in_place = true;
  for (int step = 0; step < 60000 && ok; ++step) {
    const int op = int(rng() % 100);
    const int s = int(rng() % 6);
    const uint64_t tag = cf + (rng() % 5) - 2;   // frames tagged from different threads
    if (step % 700 == 0) in_place = !in_place;   // flip every ~700 steps (about 60 frames)
    if (op < 20) {   // derived palette from the update hook, with a slot the consumer path must pick up
      const int32_t slot = (rng() % 3 == 0) ? int32_t(rng() % 9) : -1;
      const std::vector<Mat4> p = Pose(counts[s], float(rng() % 50) * 0.1f);
      a.SubmitSkin(skels[s], slot, counts[s], p.data(), tag);
      b.SubmitSkin(skels[s], slot, counts[s], p.data(), tag);
    } else if (op < 85) {   // the draw's own read: many parts a frame, usually the same palette
      static std::vector<Mat4> pal[6];
      static uint64_t pal_frame[6] = {};
      if (pal[s].empty() || pal_frame[s] != cf || rng() % 9 == 0) pal[s] = Pose(counts[s], float(cf % 13) + float(rng() % 4)), pal_frame[s] = cf;
      a.SubmitConsumer(skels[s], counts[s], pal[s].data(), tag, false);
      b.SubmitConsumer(skels[s], counts[s], pal[s].data(), tag, in_place);
    } else if (op < 90) {
      const float d = float(rng() % 7) * 0.25f;
      a.ApplyDelta(skels[s], d, d, 0.f), b.ApplyDelta(skels[s], d, d, 0.f);
    } else if (op < 97) {
      cf += rng() % 2;
    } else {   // Emit at the frame: byte-compare the record and the counters
      Counters ca, cb;
      const std::vector<uint8_t> ea = EmitBytes(a, ca, cf), eb = EmitBytes(b, cb, cf);
      ++emits;
      if (ea != eb || ca.objects != cb.objects || ca.unknown != cb.unknown || ca.stale != cb.stale) ok = false, ++mism;
    }
  }
  char why[160];
  std::snprintf(why, sizeof why, "%llu emits byte-equal (records + counters), in place %llu consumer palettes", (unsigned long long)emits, (unsigned long long)ul::Get(ul::kConsumerInPlace));
  Check(ok && emits > 1000, "consumer.equal", why);
  Check(ul::Get(ul::kConsumerInPlace) > 8000, "consumer.took", "the in-place path took a real share of the consumer submits");
}

// ---- arena ---------------------------------------------------------------------------------------------------------------------------
namespace aw = rdr_scene::live::arenaworld;
struct AGuest {
  std::map<uint32_t, uint32_t> words;
  uint32_t frame = 0;
  void SetModel(uint32_t m, uint32_t pal, uint32_t node) {
    words[m + 200] = pal, words[m + 824] = 0x60000000u + m % 0x100000, words[0x60000000u + m % 0x100000 + 8] = 4, words[0x60000000u + m % 0x100000 + 12] = node;
  }
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  bool ReadMat(uint32_t a, float* m) const {
    for (int i = 0; i < 16; ++i) m[i] = ((a >> 4) & 0xFF) * 0.5f + float(i) + float(frame) * 0.25f;
    return true;
  }
};
struct Call { uint32_t model, pal; uint64_t frame; bool close_after; };
static std::vector<Call> ArenaCalls(AGuest& g) {
  std::mt19937 rng(91);
  std::vector<Call> calls;
  constexpr uint32_t kModels = 1300;   // past kMaxObjects (1024) in the big frames: the overflow path
  for (uint32_t m = 0; m < kModels; ++m) g.SetModel(0x70000000u + m * 0x400, 0x71000000u + m * 0x400, 1 + m % 3);
  for (uint64_t frame = 1; frame <= 150; ++frame) {
    const uint32_t distinct = (frame % 37 == 0) ? 1250 : 40 + rng() % 120;
    for (uint32_t i = 0; i < distinct; ++i) {
      const uint32_t m = (frame % 37 == 0) ? i : rng() % 200;
      const uint32_t model = 0x70000000u + m * 0x400;
      const uint32_t pal = (rng() % 9 == 0) ? 0x72000000u + m * 0x400 : (rng() % 20 == 0 ? 0 : 0x71000000u + m * 0x400);   // some foreign (a second key for the same object), some null
      const int parts = 1 + int(rng() % 6);
      for (int p = 0; p < parts; ++p) calls.push_back({model, pal, frame, false});
    }
    if (frame % 11 == 0) calls.push_back({0x70000000u, 0x71000000u, frame - 1, false});   // a late call of a closed frame
    calls.back().close_after = true;
  }
  return calls;
}
struct ArenaOut { std::vector<std::vector<uint8_t>> frames; aw::Stats st; };
static ArenaOut ArenaRun(const std::vector<Call>& calls, AGuest& g, int flip_every) {   // flip_every 0: the mask stays 0 (the pure-off baseline); else bit 16 toggles every N calls
  ArenaOut out;
  aw::Tracker t;
  auto rd = [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); };
  auto rm = [&](uint32_t a, float* m) { return g.ReadMat(a, m); };
  aw::FrameBuffer fb;   // reused, as the writer does (the swap returns its capacity to the tracker's ring)
  uint32_t n = 0;
  bool on = false;
  for (const Call& c : calls) {
    if (flip_every > 0 && ++n % flip_every == 0) on = !on;
    if (flip_every < 0) on = true;   // -1: on for the whole run
    ul::g_mask.store(on ? ul::kEntry2 : 0);
    g.frame = uint32_t(c.frame);
    t.OnModelCall(c.model, c.pal, c.frame, rd, rm, false);
    if (c.close_after) {
      t.CloseBefore(c.frame + 1);
      while (t.TakeNext(fb)) {
        std::vector<uint8_t> bytes(sizeof(uint64_t) + 8 + fb.worlds.size() * sizeof(rdr_scene::ArenaWorld));
        std::memcpy(bytes.data(), &fb.frame, 8);
        std::memcpy(bytes.data() + 8, &fb.origin, 4), bytes[12] = fb.origin_known, bytes[13] = fb.valid;
        if (!fb.worlds.empty()) std::memcpy(bytes.data() + 16, fb.worlds.data(), fb.worlds.size() * sizeof(rdr_scene::ArenaWorld));
        out.frames.push_back(std::move(bytes));
      }
    }
  }
  ul::g_mask.store(0);
  out.st = t.GetStats();
  return out;
}
static void ArenaSwapEqual() {
  AGuest g;
  const std::vector<Call> calls = ArenaCalls(g);
  const ArenaOut base = ArenaRun(calls, g, 0);
  const uint64_t swap0 = ul::Get(ul::kArenaSwap);
  bool ok = true;
  char why[200];
  for (int flip : {1, 97, -1}) {   // bit 16 flipped every call, every 97 calls (mid-frame), and on for the whole run
    const ArenaOut o = ArenaRun(calls, g, flip);
    if (o.frames != base.frames || o.st.recorded != base.st.recorded || o.st.overflow != base.st.overflow || o.st.foreign_palette != base.st.foreign_palette ||
        o.st.old_calls != base.st.old_calls || o.st.frames != base.st.frames || o.st.late != base.st.late)
      ok = false;
  }
  std::snprintf(why, sizeof why, "%zu frames of worlds byte-equal, stats equal (recorded %llu, overflow %llu); %llu frame swaps", base.frames.size(), (unsigned long long)base.st.recorded,
                (unsigned long long)base.st.overflow, (unsigned long long)(ul::Get(ul::kArenaSwap) - swap0));
  Check(ok && base.frames.size() > 100 && base.st.overflow > 0, "arena.swap_equal", why);
  Check(ul::Get(ul::kArenaSwap) - swap0 > 200, "arena.swap_took", "the swapped frame buffers took a real share of the frames");
}

// ---- micro-benchmarks (printed; the Surface number is the profile's, this is the PC's warm-cache floor) -------------------------------
static void Bench() {
  {
    WrestlerContributor a, b; const int N = 200000; std::vector<Mat4> p = Pose(109, 0.5f);
    a.SubmitSkin(0x50001000, 0, 109, p.data(), 1), b.SubmitSkin(0x50001000, 0, 109, p.data(), 1);
    a.SubmitConsumer(0x50001000, 109, p.data(), 1, false), b.SubmitConsumer(0x50001000, 109, p.data(), 1, true);
    double t0 = NowNs();
    for (int i = 0; i < N; ++i) a.SubmitConsumer(0x50001000, 109, p.data(), 1, false);
    double t1 = NowNs();
    for (int i = 0; i < N; ++i) b.SubmitConsumer(0x50001000, 109, p.data(), 1, true);
    double t2 = NowNs();
    std::printf("BENCH consumer palette (109 nodes = 7 KB, a repeat of the frame's call): general %.1f ns, in place %.1f ns a call\n", (t1 - t0) / N, (t2 - t1) / N);
  }
  {
    AGuest g; const std::vector<Call> calls = ArenaCalls(g);
    double t0 = NowNs();
    (void)ArenaRun(calls, g, 0);
    double t1 = NowNs();
    (void)ArenaRun(calls, g, -1);
    double t2 = NowNs();
    std::printf("BENCH arena (150 frames, %zu calls): plain %.1f ns, swapped buffers %.1f ns a call (map-less guest, the world read is a fake)\n", calls.size(), (t1 - t0) / calls.size(), (t2 - t1) / calls.size());
  }
}

int main(int argc, char** argv) {
#ifdef UPD_LEAN_PLANTS
  if (argc >= 3 && std::string(argv[1]) == "plant") ul::g_plant = std::atoi(argv[2]);
#endif
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const bool plant_run = argc >= 3 && std::string(argv[1]) == "plant";
  ul::g_census.store(true);   // the took-counters count only under a census
  ConsumerEqual();
  ArenaSwapEqual();
  if (!plant_run) Bench();
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "all checks passed", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
