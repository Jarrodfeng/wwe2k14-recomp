// Offline proof for lane G (Session 103; docs\renderer\SCENE_PACING.md 4): the latest-record mailbox and the kept draw lists. No renderer, no game.
//   pacing_unit            all checks, exit 0 = green
//   pacing_unit --plant-queue    the mailbox queues instead of overwriting      -> must exit 1
//   pacing_unit --plant-nofree   the lists never free                            -> must exit 1
//   pacing_unit --plant-stale    the stamp compare ignores the world             -> must exit 1
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/scene_mailbox.h"
#include "../../src/renderer_scene/draw_lists.h"

using namespace rdr_adapter;
using namespace rdr_scene;

static int g_fail = 0;
static bool g_plant_queue = false, g_plant_nofree = false, g_plant_stale = false, g_plant_barrier = false, g_plant_specmask = false;
#define CHECK(c, ...)                                         \
  do {                                                        \
    if (!(c)) {                                               \
      ++g_fail;                                               \
      std::printf("FAIL %s:%d  ", __FILE__, __LINE__);        \
      std::printf(__VA_ARGS__);                               \
      std::printf("\n");                                      \
    }                                                         \
  } while (0)

// ---- the mailbox: ONE ordered queue, latest wins at the consumer's pop ----------------------------------------------------------------------------------

struct Msg {
  uint64_t seq = 0;        // post order
  MailKind kind = MailKind::Plain;
  std::shared_ptr<int> record;  // stands for the scene record
  std::array<uint64_t, 16> fill{};
  void Make(uint64_t s, MailKind k) { seq = s, kind = k, record = std::make_shared<int>(int(s)), fill.fill(s); }
  bool Whole() const {
    for (uint64_t v : fill) if (v != seq) return false;
    return record && uint64_t(*record) == seq;
  }
};
static MailKind KindOf(const Msg& m) { return m.kind; }

// Virtual clock: the game pushes one message every 1/g s; the renderer pops a run at t, handles each dropped one for `dc` s and draws the newest for c s.
static void SimRate(double g, double c, double dc, double seconds) {
  std::deque<Msg> q;
  MailStats st;
  const double dt = 1.0 / g;
  uint64_t posted = 0, shown = 0, last_shown = 0, handled_dropped = 0;
  double t = 0, worst_latency = 0, end_t = 0;
  const uint64_t total = uint64_t(seconds * g);
  bool order_ok = true, newest_ok = true, whole_ok = true;
  while (true) {
    while (posted < total && double(posted) * dt <= t + 1e-12) {
      Msg m;
      m.Make(posted + 1, MailKind::Plain);
      q.push_back(std::move(m));
      ++posted;
    }
    MailRun<Msg> run;
    if (!TakeRun(q, KindOf, run, &st, false, g_plant_queue)) {
      if (posted >= total) break;
      t = double(posted) * dt;
      continue;
    }
    for (const Msg& d : run.dropped) {
      whole_ok = whole_ok && d.Whole();
      order_ok = order_ok && d.seq > last_shown;
      last_shown = d.seq;  // (handled in order; not shown)
      t += dc;
    }
    handled_dropped += run.dropped.size();
    whole_ok = whole_ok && run.newest.Whole();
    order_ok = order_ok && run.newest.seq > last_shown;
    // the newest of the run is the newest posted so far (a plain run drains the queue)
    if (run.newest.seq != posted) newest_ok = false;
    worst_latency = std::max(worst_latency, t - double(run.newest.seq - 1) * dt);
    last_shown = run.newest.seq;
    ++shown;
    t += c;
    end_t = t;
  }
  const double rate = double(shown) / std::max(end_t, 1e-9);
  const double expect = std::min(g, 1.0 / (c + (dc > 0 ? 0.0 : 0.0)));
  CHECK(order_ok, "g=%.0f c=%.1fms: handled out of order", g, c * 1000);
  CHECK(whole_ok, "g=%.0f c=%.1fms: a half message", g, c * 1000);
  CHECK(newest_ok, "g=%.0f c=%.1fms: the drawn frame was not the newest at its take (the plant: a FIFO draws old frames)", g, c * 1000);
  CHECK(worst_latency <= std::max(dt, c) + 20 * dc + 1e-9, "g=%.0f c=%.1fms: latency %.1f ms too high", g, c * 1000, worst_latency * 1000);
  CHECK(rate >= (dc > 0 ? 0.9 : 0.97) * std::min(expect, g), "g=%.0f c=%.1fms dc=%.2fms: shown %.2f/s < min(game, renderer) %.2f/s", g, c * 1000, dc * 1000, rate, expect);
  CHECK(posted == shown + handled_dropped, "g=%.0f c=%.1fms: accounting posted %llu != drawn %llu + dropped %llu", g, c * 1000, (unsigned long long)posted, (unsigned long long)shown, (unsigned long long)handled_dropped);
  std::printf("  game %3.0f/s render %5.1f ms drop-cost %.1f ms: shown %6.2f/s (min = %6.2f), dropped %llu, worst latency %.1f ms (game frame %.1f ms)\n", g, c * 1000, dc * 1000, rate, expect,
              (unsigned long long)handled_dropped, worst_latency * 1000, dt * 1000);
}

static void MailboxVirtual() {
  std::printf("[mailbox, virtual clock]\n");
  const double games[] = {30, 60, 120};
  const double renders_ms[] = {4, 8, 16.7, 33, 50, 120};
  for (double g : games)
    for (double c : renders_ms) SimRate(g, c / 1000.0, 0.0, 4.0), SimRate(g, c / 1000.0, 0.0005, 4.0);
}

// Order, barriers, last frames: a random mix posted by a real thread against a real consumer. Every message is handled exactly once, in post order (dropped
// ones first, then the newest, then the next run); a Barrier is never dropped; a run never contains anything after a Last; a Barrier is never inside a run.
static void MailboxMixed() {
  std::printf("[mailbox, mixed kinds, real threads]\n");
  std::deque<Msg> q;
  std::mutex mu;
  std::condition_variable cv;
  constexpr uint64_t kN = 3000;
  bool done = false;
  std::thread prod([&] {
    uint32_t rng = 12345;
    for (uint64_t s = 1; s <= kN; ++s) {
      rng = rng * 1664525u + 1013904223u;
      const uint32_t r = (rng >> 16) % 100;
      Msg m;
      m.Make(s, r < 8 ? MailKind::Barrier : r < 12 ? MailKind::Last : MailKind::Plain);
      {
        std::lock_guard lock(mu);
        q.push_back(std::move(m));
      }
      cv.notify_one();
      if (s % 7 == 0) std::this_thread::sleep_for(std::chrono::microseconds(30));
    }
    {
      std::lock_guard lock(mu);
      done = true;
    }
    cv.notify_all();
  });
  MailStats st;
  uint64_t last = 0, handled = 0, dropped = 0, barrier_dropped = 0, last_dropped = 0, bad_order = 0, torn = 0, past_last = 0;
  while (true) {
    MailRun<Msg> run;
    {
      std::unique_lock lock(mu);
      cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return done || !q.empty(); });
      if (q.empty()) {
        if (done) break;
        continue;
      }
      TakeRun(q, KindOf, run, &st, g_plant_barrier);
    }
    for (const Msg& d : run.dropped) {
      ++dropped, ++handled;
      if (d.seq != last + 1) ++bad_order;
      last = d.seq;
      if (!d.Whole()) ++torn;
      if (d.kind == MailKind::Barrier) ++barrier_dropped;
      if (d.kind == MailKind::Last) ++last_dropped;
    }
    ++handled;
    if (run.newest.seq != last + 1) ++bad_order;
    last = run.newest.seq;
    if (!run.newest.Whole()) ++torn;
    (void)past_last;
    std::this_thread::sleep_for(std::chrono::microseconds(150));  // a draw
  }
  prod.join();
  CHECK(handled == kN, "mixed: %llu of %llu messages handled (each exactly once)", (unsigned long long)handled, (unsigned long long)kN);
  CHECK(bad_order == 0, "mixed: %llu messages handled out of post order", (unsigned long long)bad_order);
  CHECK(torn == 0, "mixed: a torn message");
  CHECK(barrier_dropped == 0, "mixed: %llu barrier messages (read parts / named pictures) were dropped", (unsigned long long)barrier_dropped);
  CHECK(last_dropped == 0, "mixed: %llu last (sheet) frames were dropped", (unsigned long long)last_dropped);
  CHECK(dropped > 0, "mixed: nothing was dropped: the consumer is not slower than the producer, the test proves nothing");
  std::printf("  posted %llu, dropped %llu, runs %llu, longest dropped run %llu, runs stopped before a barrier %llu\n", (unsigned long long)kN, (unsigned long long)dropped,
              (unsigned long long)st.takes, (unsigned long long)st.max_run_dropped, (unsigned long long)st.barrier_stops);
  // a Barrier ends a run before it, a Last after it: exact shapes
  {
    std::deque<Msg> d;
    const MailKind ks[] = {MailKind::Plain, MailKind::Plain, MailKind::Barrier, MailKind::Plain, MailKind::Last, MailKind::Plain};
    for (uint64_t i = 0; i < 6; ++i) { Msg m; m.Make(i + 1, ks[i]); d.push_back(std::move(m)); }
    MailRun<Msg> r1, r2, r3, r4;
    TakeRun(d, KindOf, r1, nullptr, g_plant_barrier);
    CHECK(r1.dropped.size() == 1 && r1.newest.seq == 2, "shape: run 1 = {1 dropped, 2 newest}, got %zu dropped, newest %llu", r1.dropped.size(), (unsigned long long)r1.newest.seq);
    TakeRun(d, KindOf, r2, nullptr, g_plant_barrier);
    CHECK(r2.dropped.empty() && r2.newest.seq == 3 && r2.newest.kind == MailKind::Barrier, "shape: the barrier is taken alone (dropped %zu, newest %llu)", r2.dropped.size(), (unsigned long long)r2.newest.seq);
    TakeRun(d, KindOf, r3, nullptr, g_plant_barrier);
    CHECK(r3.dropped.size() == 1 && r3.newest.seq == 5 && r3.newest.kind == MailKind::Last, "shape: the run ends AFTER a last frame");
    TakeRun(d, KindOf, r4, nullptr, g_plant_barrier);
    CHECK(r4.newest.seq == 6 && d.empty(), "shape: the rest");
  }
}

// ---- the lists ------------------------------------------------------------------------------------------------------------------------

// The fake effect: the field names are the real ones (so FieldMask is the real table); a field's value is a function of the inputs in its mask, so a
// stale block is visible. "Mystery" is unknown to the table: it depends on everything (class ALL).
struct Field { const char* name; uint32_t vec4s; };
static const Field kFields[] = {{"ViewProj", 4}, {"WorldMatrixArray", 4}, {"EyePos", 1}, {"Time", 1}, {"SkyCol", 1}, {"DifLghCol", 3}, {"SpecularCol", 1}, {"MatAmbCol", 1},
                                {"MdlAlpha", 1}, {"NoiseTile", 1}, {"FogDistance", 1}, {"RimLightParam", 1}, {"Mystery", 1}};
constexpr int kFieldCount = int(sizeof kFields / sizeof kFields[0]);

struct Inputs {  // what one draw sees this frame
  uint64_t world = 1, palette = 1, camera = 1, time = 1, light = 1, material = 1;
  bool present = true;
};
static Stamps StampsOf(const Inputs& in) {
  Stamps s;
  s.h[0] = in.world, s.h[1] = in.palette, s.h[2] = in.camera, s.h[3] = in.time, s.h[4] = in.light, s.h[5] = in.material;
  return s;
}
static void Resolve(const KeptField& f, const Inputs& in, float* out) {
  uint64_t h = 1469598103934665603ull;
  uint32_t mask = f.mask;
  if (std::strcmp(f.name, "SpecularCol") == 0) mask |= kStLight | kStMaterial;   // S103 SL: the truth (Resolve multiplies the light colour into the material), whatever the table says: a table without kStLight leaves a stale block
  const uint64_t v[kStampCount] = {in.world, in.palette, in.camera, in.time, in.light, in.material};
  for (int i = 0; i < kStampCount; ++i)
    if (mask & (1u << i)) h = (h ^ v[i]) * 1099511628211ull;
  h = (h ^ HashBytes(f.name, std::strlen(f.name))) * 1099511628211ull;
  for (uint32_t k = 0; k < f.vec4s * 4; ++k) out[k] = float((h >> (k % 40)) & 0xFFFF) + float(k);
}
static void BuildBlock(KeptBlock& b) {
  uint32_t off = 0;
  for (int i = 0; i < kFieldCount; ++i) off += kFields[i].vec4s * 16;
  b.Begin(off);
  off = 0;
  for (int i = 0; i < kFieldCount; ++i) {
    b.Add(kFields[i].name, off, kFields[i].vec4s);
    off += kFields[i].vec4s * 16;
  }
}
static std::vector<uint8_t> Oracle(const Inputs& in) {  // a full fresh build, every field
  KeptBlock b;
  BuildBlock(b);
  for (const KeptField& f : b.fields) {
    std::memset(b.bytes.data() + f.offset, 0, f.vec4s * 16);
    Resolve(f, in, reinterpret_cast<float*>(b.bytes.data() + f.offset));
  }
  return b.bytes;
}

struct World {  // M objects x D draws
  int objects, draws;
  std::vector<Inputs> in;  // [object * draws + draw]
  World(int m, int d) : objects(m), draws(d), in(size_t(m) * d) {}
};

// Draws one frame through the lists; returns the resolver calls. `check` compares every block with the oracle.
static uint64_t DrawFrame(DrawLists& L, const World& w, uint64_t* stale, uint64_t* resolver_calls_per_draw0 = nullptr) {
  L.BeginFrame();
  uint64_t calls = 0;
  for (int o = 0; o < w.objects; ++o)
    for (int d = 0; d < w.draws; ++d) {
      const Inputs& in = w.in[size_t(o) * w.draws + d];
      if (!in.present) continue;
      bool fresh = false;
      KeptDraw& k = L.Acquire({uint32_t(1000 + o), 7, uint32_t(d), 0, 0}, fresh);
      const uint32_t changed = DrawLists::Diff(k, StampsOf(in), fresh);
      if (fresh) BuildBlock(k.vs);
      const uint32_t n = L.Refresh(k.vs, changed, [&](const KeptField& f, float* out) { Resolve(f, in, out); }, g_plant_stale ? kStWorld : 0u);
      calls += n;
      if (o == 0 && d == 0 && resolver_calls_per_draw0) *resolver_calls_per_draw0 = n;
      if (k.vs.bytes != Oracle(in)) ++*stale;
    }
  L.EndFrame();
  return calls;
}

static int MaskCount(uint32_t bits) {  // fields a change of `bits` must rewrite
  int n = 0;
  for (int i = 0; i < kFieldCount; ++i) n += (FieldMask(kFields[i].name) & bits) != 0;
  return n;
}

static void ListsTests() {
  std::printf("[lists]\n");
  CHECK(FieldMask("Mystery") == kStAll, "an unknown field must be class ALL");
  CHECK(FieldMask("FogDistance") == 0, "FogDistance is static");
  CHECK(FieldMask("ViewProj") == kStCamera, "ViewProj is camera");
  CHECK(FieldMask("TexProjLight") == kStLight && FieldMask("ViewLight") == kStLight && FieldMask("ShadowPrm") == kStLight, "S103 D1 review 1: the stage FX_SHADOW rows follow the shadow map (light stamp), never static");
  CHECK(FieldMask("SpeLodParam") == kStMaterial, "SpeLodParam follows the material (g_fSpeLodNear / Far; H2 mask lint)");
  CHECK(FieldMask("UVScroll0") == kStMaterial && FieldMask("UVScroll1") == kStMaterial, "UVScroll0 / 1 follow the material (E6 g_fScrollU..V4; H2 mask lint)");
  {
    DrawLists L(1);
    L.PlantNoFree(g_plant_nofree);
    World w(40, 6);
    const int n = w.objects * w.draws;
    uint64_t stale = 0;
    // 1. the same record, 60 frames: each draw built once
    uint64_t calls_first = DrawFrame(L, w, &stale);
    uint64_t calls_rest = 0;
    for (int f = 1; f < 60; ++f) calls_rest += DrawFrame(L, w, &stale);
    const ListCounts c = L.counts();
    CHECK(c.built == uint64_t(n), "built %llu != draws %d", (unsigned long long)c.built, n);
    CHECK(c.reused == uint64_t(n) * 59, "reused %llu != %d", (unsigned long long)c.reused, n * 59);
    CHECK(calls_first == uint64_t(n) * kFieldCount, "the first frame resolves every field (%llu)", (unsigned long long)calls_first);
    CHECK(calls_rest == 0, "an unchanged record resolved %llu fields (must be 0)", (unsigned long long)calls_rest);
    CHECK(stale == 0, "%llu blocks differ from a fresh full build", (unsigned long long)stale);
    CHECK(c.blocks_full == uint64_t(n) && c.blocks_clean == uint64_t(n) * 59, "blocks full %llu clean %llu", (unsigned long long)c.blocks_full, (unsigned long long)c.blocks_clean);
    // 2. one object's world moves: only the world-class fields of its draws (+ the unknown field)
    stale = 0;
    for (int d = 0; d < w.draws; ++d) w.in[size_t(5) * w.draws + d].world = 99;
    const uint64_t moved = DrawFrame(L, w, &stale);
    const uint64_t want = uint64_t(w.draws) * MaskCount(kStWorld);
    CHECK(moved == want, "a moved object resolved %llu fields, expected %llu (world-class + unknown only)", (unsigned long long)moved, (unsigned long long)want);
    CHECK(moved < uint64_t(w.draws) * kFieldCount, "a moved object rewrote every field");
    CHECK(stale == 0, "after a world change %llu blocks differ from the oracle (the plant: the compare ignores the world)", (unsigned long long)stale);
    // 3. the light changes for one object
    stale = 0;
    for (int d = 0; d < w.draws; ++d) w.in[size_t(9) * w.draws + d].light = 5;
    const uint64_t lit = DrawFrame(L, w, &stale);
    CHECK(lit == uint64_t(w.draws) * MaskCount(kStLight), "a light change resolved %llu fields, expected %llu", (unsigned long long)lit, (unsigned long long)uint64_t(w.draws) * MaskCount(kStLight));
    CHECK(stale == 0, "after a light change %llu blocks stale", (unsigned long long)stale);
    // 4. the camera moves: every draw, only camera-class fields
    stale = 0;
    for (Inputs& in : w.in) in.camera += 1;
    const uint64_t cam = DrawFrame(L, w, &stale);
    CHECK(cam == uint64_t(n) * MaskCount(kStCamera), "a camera move resolved %llu, expected %llu", (unsigned long long)cam, (unsigned long long)uint64_t(n) * MaskCount(kStCamera));
    CHECK(stale == 0, "after a camera move %llu blocks stale", (unsigned long long)stale);
    // 5. an object blinks out for one frame (grace 1): not freed, not rebuilt
    const uint64_t built_before = L.counts().built;
    for (int d = 0; d < w.draws; ++d) w.in[size_t(3) * w.draws + d].present = false;
    DrawFrame(L, w, &stale);
    for (int d = 0; d < w.draws; ++d) w.in[size_t(3) * w.draws + d].present = true;
    DrawFrame(L, w, &stale);
    CHECK(L.counts().built == built_before, "an object absent for one frame was rebuilt (%llu built)", (unsigned long long)(L.counts().built - built_before));
    // 6. an object leaves for good: freed; the live count falls
    const uint64_t live_before = L.counts().live_draws;
    for (int d = 0; d < w.draws; ++d) w.in[size_t(3) * w.draws + d].present = false, w.in[size_t(4) * w.draws + d].present = false;
    for (int f = 0; f < 4; ++f) DrawFrame(L, w, &stale);
    const ListCounts after = L.counts();
    CHECK(after.live_draws == live_before - 2 * w.draws, "live draws %llu, expected %llu (two objects left)", (unsigned long long)after.live_draws, (unsigned long long)(live_before - 2 * w.draws));
    CHECK(after.freed_objects == 2 && after.freed_draws == 2 * uint64_t(w.draws), "freed objects %llu draws %llu", (unsigned long long)after.freed_objects, (unsigned long long)after.freed_draws);
    CHECK(after.live_objects == uint64_t(w.objects - 2), "live objects %llu", (unsigned long long)after.live_objects);
    // 7. it comes back: built again (a clean slate, no stale block)
    for (int d = 0; d < w.draws; ++d) w.in[size_t(3) * w.draws + d].present = true;
    stale = 0;
    DrawFrame(L, w, &stale);
    CHECK(L.counts().built == after.built + uint64_t(w.draws), "a returned object was not rebuilt whole");
    CHECK(stale == 0, "a returned object's block is stale");
    std::printf("  %s\n", L.Census().c_str());
  }
}

// S103 G review fixes: the grace counts guest frames, ReleaseAsset, duplicate keys, a rebuilt draw counts as built, SpecularCol follows the light.
static void ListsTests2() {
  std::printf("[lists, review fixes]\n");
  CHECK((FieldMask("SpecularCol") & kStLight) && (FieldMask("SpecularCol") & kStMaterial), "SpecularCol must depend on the light (Resolve multiplies by ls.col[0])");
  bool fresh = false;
  {
    DrawLists L(1);
    L.Tick(1), L.BeginFrame();
    L.Acquire({1, 7, 0, 0, 0}, fresh);
    L.EndFrame();
    // more Records in the SAME guest frame (a seed frame, a mirror next to a live one): no ageing
    for (int i = 0; i < 3; ++i) L.Tick(1), L.BeginFrame(), L.EndFrame();
    CHECK(L.counts().live_draws == 1, "records inside one guest frame aged a draw (live %llu)", (unsigned long long)L.counts().live_draws);
    L.Tick(2), L.BeginFrame(), L.EndFrame();
    CHECK(L.counts().live_draws == 1, "grace 1: absent for one guest frame must keep the draw");
    L.Tick(3), L.BeginFrame(), L.EndFrame();
    CHECK(L.counts().live_draws == 0, "absent for two guest frames must free the draw (live %llu)", (unsigned long long)L.counts().live_draws);
  }
  {
    DrawLists L(1);
    L.Tick(1), L.BeginFrame();
    L.Acquire({1, 7, 0, 0, 0}, fresh);
    L.Acquire({2, 7, 0, 0, 0}, fresh);
    L.Acquire({3, 9, 0, 0, 0}, fresh);
    L.EndFrame();
    L.ReleaseAsset(7);
    CHECK(L.counts().freed_objects == 2 && L.counts().freed_draws == 2, "ReleaseAsset(7) must free exactly asset 7's two draws (freed %llu)", (unsigned long long)L.counts().freed_draws);
    L.Tick(2), L.BeginFrame();
    L.Acquire({3, 9, 0, 0, 0}, fresh);
    CHECK(!fresh, "the other asset's draw must survive a release");
    L.EndFrame();
    L.Tick(3), L.BeginFrame();  // many objects with id 0 and one asset: one shared list
    for (int i = 0; i < 5; ++i) L.Acquire({0, 4, 0, 0, 0}, fresh);
    L.EndFrame();
    CHECK(L.counts().dup_keys == 4, "duplicate keys %llu, expected 4", (unsigned long long)L.counts().dup_keys);
    const uint64_t b = L.counts().built, r = L.counts().reused;
    L.NoteRebuilt();  // a draw whose binding changed was rebuilt whole: built, not reused
    CHECK(L.counts().built == b + 1 && L.counts().reused == r - 1, "NoteRebuilt must move one reuse to built");
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant-queue") g_plant_queue = true;
    else if (a == "--plant-nofree") g_plant_nofree = true;
    else if (a == "--plant-stale") g_plant_stale = true;
    else if (a == "--plant-barrier") g_plant_barrier = true;
    else if (a == "--plant-specmask") g_plant_specmask = true;   // S103 SL: SpecularCol's mask without the light (review B1); pixel-free: since L5 no plain arena draw has a highlight to move
    else { std::printf("usage: pacing_unit [--plant-queue|--plant-nofree|--plant-stale|--plant-barrier|--plant-specmask]\n"); return 2; }
  }
  if (g_plant_specmask) PlantFieldMask("SpecularCol", kStMaterial);   // before any block is built
  MailboxVirtual();
  MailboxMixed();
  ListsTests();
  ListsTests2();
  std::printf(g_fail ? "pacing_unit: %d FAILED\n" : "pacing_unit: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
