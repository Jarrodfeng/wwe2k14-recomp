// S102 U1 offline proof (docs\renderer\SWAP_TAIL.md): the scene record is the same when its emit runs after the update thread is released.
// 0 game runs. Built by tools\tail\tail_order_check.py with -DRDR_TAIL_PLANTS against the tree's own live_scene.cpp, record_io.cpp and the real
// contributors (crowd, arena, wrestlers, lights, circles, plane, arena worlds, ring rows, casters, spheres).
//
// The model: the game's render-command loop as tail_release.h describes it, with an "update thread" (a real std::thread) that records frame k+1 the
// moment it is released: crowd draw + palettes, arena light sets, wrestler skeleton palettes (stamped with the guest frame it reads), light-state model
// calls (stamped, through a fake guest memory). Per iteration: [arrive] -> present (old path: EndFrame; tail: TailDecide + LatchFrame) -> VdSwap (frame + 1)
// -> release (OnRelease; the update thread records the next frame) -> finish (EmitLatched). In the "tail" arm the release WAITS until the update thread
// has published the whole next frame before the finish runs: the worst case, every frame. In "tail_race" it does not wait (real overlap).
//
// Usage: tail_order_test ARM [CROWD_PATH] [FRAMES]
//   ARM: serial | tail | tail_race | tail_flip | serial_extra | tail_extra | plant_release_first | plant_latch_skip | plant_skip_I (I = 0 arena, 1 wrestlers, 2 crowd, 3 lights)
//        | serial_lock | tail_lock | plant_lock_nowait (review F1: a read-back lock on the update thread right after the release) | tail_throw | plant_throw_nocap (review F2)
//   CROWD_PATH: plain | ids | runs | pool (the crowd contributor's three Emit paths; pool = ids + T11b's scene string pool; default ids)
// Prints one line per record: "rec GUEST_FRAME HASH BYTES", then "stats ..." lines; the check script compares arms.
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/snapshot_keeper.h"
#include "../../src/renderer_adapter/tail_release.h"
#include "../../src/renderer_scene/live_scene.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/arena_contrib.h"
#include "../../src/scene_live/arena_world_contrib.h"
#include "../../src/scene_live/caster_contrib.h"
#include "../../src/scene_live/circle_contrib.h"
#include "../../src/scene_live/crowd_contrib.h"
#include "../../src/scene_live/light_state_contrib.h"
#include "../../src/scene_live/plane_contrib.h"
#include "../../src/scene_live/ring_rows_contrib.h"
#include "../../src/scene_live/sphere_contrib.h"
#include "../../src/scene_live/wrestler_contrib.h"

namespace live = rdr_scene::live;
namespace tl = rdr_adapter::tail;

namespace {

std::atomic<uint64_t> g_guest_frame{100};  // the game's frame counter (VdSwap moves it)

// ---- the sink: every record serialised and hashed ----
struct RecSink final : live::Sink {
  std::mutex mu;
  std::vector<std::string> lines;
  void OnRecord(const live::FrameInfo& f, const rdr_scene::SceneTables& t) override { OnRecordShared(f, std::make_shared<const rdr_scene::SceneTables>(t)); }
  void OnRecordShared(const live::FrameInfo& f, std::shared_ptr<const rdr_scene::SceneTables> t) override {
    const std::vector<uint8_t> b = rdr_scene::SerializeRecord(*t);
    uint64_t h = 1469598103934665603ull;
    for (uint8_t x : b) h = (h ^ x) * 1099511628211ull;
    char line[128];
    std::snprintf(line, sizeof line, "rec %llu %016llx %zu", (unsigned long long)f.guest_frame, (unsigned long long)h, b.size());
    std::lock_guard<std::mutex> lock(mu);
    lines.push_back(line);
  }
};

// ---- a contributor that cannot latch (the fallback arm) ----
struct Plain final : live::Contributor {
  std::mutex mu;
  uint32_t value = 0;  // set by the update thread
  const char* Name() const override { return "plain_test"; }
  void Emit(const live::FrameInfo&, rdr_scene::SceneTables& t, live::Counters&) override {
    uint32_t v;
    {
      std::lock_guard<std::mutex> lock(mu);
      v = value;
    }
    rdr_scene::ParamValue p;
    p.name = t.AddString("plain_test.value");
    p.value[0] = float(v);
    t.params.push_back(p);
  }
};
Plain g_plain;

// ---- the fake guest memory for the light-state tracker ----
struct FakeGuest {
  std::map<uint32_t, uint32_t> words;
  std::vector<uint8_t> records;  // the record array at kArr
  static constexpr uint32_t kG = 0x40000000, kArr = 0x41000000, kTls = 0x42000000, kBlk = 0x43000000;
  bool Word(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  bool Bytes(uint32_t a, void* dst, size_t n) const {
    if (a < kArr || a + n > kArr + records.size()) return false;
    std::memcpy(dst, records.data() + (a - kArr), n);
    return true;
  }
};
FakeGuest g_mem;
std::mutex g_mem_mu;  // the update thread writes the records; only it reads them (the tracker's reads happen on the update thread)

void PutBE(uint8_t* p, float f) {
  uint32_t w;
  std::memcpy(&w, &f, 4);
  w = __builtin_bswap32(w);
  std::memcpy(p, &w, 4);
}

// ---- review F1: the reader's hand-off modelled (reader.h TailFinishAt / OnTextureLock): one sink of frames and frame parts in index order ----
// A frame G is "submitted" with index = record_frame (then record_frame + 1), taking the keeper's ForSubmit(G); a lock read-back on a game thread draws a
// "part" of the frame being recorded (G + 1) with index = record_frame (then + 1), taking ForPart(G + 1). The reader lock is `rmu`; the tail's finish holds
// the State's FinishMutex from start to end (the real tail_release.h).
std::mutex g_rmu;
uint64_t g_record_frame = 0;
rdr_adapter::SnapshotKeeper g_keeper;
std::vector<std::string> g_events;   // under g_rmu
tl::State* g_st = nullptr;
live::Latched g_pending;
uint64_t g_pending_frame = 0;
int g_lock_mode = 0;                 // 0 none, 1 the update thread does a read-back lock each frame (with the flush), 2 PLANTED: without the flush

void SubmitLocked(uint64_t frame) {   // g_rmu held
  const auto rec = g_keeper.ForSubmit(frame);
  char b[128];
  std::snprintf(b, sizeof b, "ev submit %llu %llu %lld", (unsigned long long)frame, (unsigned long long)g_record_frame, rec ? (long long)rec->header.guest_frame : -1ll);
  g_events.push_back(b);
  ++g_record_frame;
}
void FinishModel(tl::Where where) {   // reader.h TailFinishAt, the same shape
  std::lock_guard<std::mutex> finish_lock(g_st->FinishMutex());
  if (!g_st->Pending()) return;
  g_st->FinishStarted();
  live::EmitLatched(g_pending);
  {
    std::lock_guard<std::mutex> lock(g_rmu);
    SubmitLocked(g_pending_frame);
  }
  g_st->FinishDone(where);
}
void LockReadBack() {   // reader.h OnTextureLock on a game thread
  if (g_lock_mode == 1) g_st->FlushIfPending(tl::kAtLock);
  std::lock_guard<std::mutex> lock(g_rmu);
  const uint64_t frame = g_guest_frame.load();
  const auto rec = g_keeper.ForPart(frame);
  char b[128];
  std::snprintf(b, sizeof b, "ev part %llu %llu %lld", (unsigned long long)frame, (unsigned long long)g_record_frame, rec ? (long long)rec->header.guest_frame : -1ll);
  g_events.push_back(b);
  ++g_record_frame;
}

// ---- review F2: a contributor registered before the trackers that latches and then throws in every Emit (the record is refused at it) ----
struct Thrower final : live::Contributor {
  const char* Name() const override { return "thrower_test"; }
  bool Latch(const live::FrameInfo&) override { return true; }
  void Emit(const live::FrameInfo&, rdr_scene::SceneTables&, live::Counters&) override { throw std::runtime_error("planted"); }
};
Thrower g_thrower;

// ---- the update thread's frame: every producer, frame k, stamped with the guest frame it reads now ----
void RecordFrame(uint32_t k, bool extra) {
  if (g_lock_mode) LockReadBack();   // first thing after the release: the worst case for F1 (before the swap thread's finish)
  const uint64_t stamp = g_guest_frame.load();
  // crowd: palettes into the write ring, then a draw that reads the same ring
  auto& crowd = live::crowd::The();
  const uint32_t ring = k & 1;
  for (uint32_t type = 0; type < 3; ++type) {
    std::vector<float> m(16 * 22);
    for (size_t i = 0; i < m.size(); ++i) m[i] = float(k * 0.25 + type + i * 0.001);
    crowd.SetPalette(ring, type, 0, m.data(), 22);
  }
  crowd.BeginDraw(ring);
  const uint32_t members = 40 + (k % 7);
  for (uint32_t i = 0; i < members; ++i) {
    live::crowd::MemberObs o;
    o.block = i % 3;
    o.lod = 0;
    o.index = i;
    for (int j = 0; j < 12; ++j) o.c[j] = float(k) + 0.01f * float(i) + float(j);
    crowd.NoteMember(o);
  }
  crowd.EndDraw();
  // arena: the per-frame light sets
  {
    std::vector<live::arena::SetSnap> sets(2);
    for (uint32_t s = 0; s < 2; ++s) {
      sets[s].set_id = s;
      for (int l = 0; l < 4; ++l)
        for (int c = 0; c < 3; ++c) sets[s].dir[l][c] = float(k + s + l + c), sets[s].col[l][c] = float(k) * 0.5f + float(c);
    }
    live::arena::PublishSets(5, 1.f + float(k) * 0.01f, std::move(sets));
    live::arena::PublishLook((k / 50) % 2 ? live::arena::Look::Match : live::arena::Look::Entrance);
  }
  // wrestlers: two skeletons, stamped
  {
    std::vector<rdr_scene::Mat4> skin(70);
    for (size_t i = 0; i < skin.size(); ++i)
      for (int j = 0; j < 16; ++j) skin[i].m[j] = float(k) + float(i) * 0.1f + float(j) * 0.01f;
    live::Wrestlers().SubmitSkin(0x1000, 0, uint32_t(skin.size()), skin.data(), stamp);
    if (k % 3) live::Wrestlers().SubmitSkin(0x2000, 1, uint32_t(skin.size()), skin.data(), stamp);
    live::Wrestlers().SetLightSet(k % 4);
  }
  // lights: model calls through the fake guest memory (stamped)
  {
    std::lock_guard<std::mutex> lock(g_mem_mu);
    for (uint32_t call = 0; call < 6; ++call) {
      const uint32_t slot = call % 2;
      g_mem.words[FakeGuest::kBlk + 32] = slot;
      uint8_t* r = g_mem.records.data() + slot * live::lights::kRecordSize;
      for (uint32_t i = 0; i < 64; ++i) PutBE(r + 4 * i, float(k) + float(call / 2) + float(i) * 0.5f);   // a new state every 2 calls, every frame
      auto rw = [](uint32_t a, uint32_t& v) { return g_mem.Word(a, v); };
      auto rb = [](uint32_t a, void* d, size_t n) { return g_mem.Bytes(a, d, n); };
      live::lights::States().tracker.OnModelCall(0x5000 + call, FakeGuest::kTls, stamp, rw, rb, [](const char*) {});
    }
  }
  if (extra) {
    std::lock_guard<std::mutex> lock(g_plain.mu);
    g_plain.value = k;
  }
}

// ---- the update thread: records a frame each time it is released ----
struct UpdateThread {
  std::mutex mu;
  std::condition_variable cv;
  uint64_t released = 0, done = 0;
  bool stop = false, extra = false;
  uint32_t next_k = 1;
  std::thread th;
  void Start() {
    th = std::thread([this] {
      std::unique_lock<std::mutex> lock(mu);
      for (;;) {
        cv.wait(lock, [&] { return stop || released > done; });
        if (stop) return;
        const uint32_t k = next_k++;
        lock.unlock();
        RecordFrame(k, extra);
        lock.lock();
        ++done;
        cv.notify_all();
      }
    });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mu);
    ++released;
    cv.notify_all();
  }
  void WaitArrived() {  // the barrier wait: every released frame recorded
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&] { return done == released; });
  }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mu);
      stop = true;
      cv.notify_all();
    }
    th.join();
  }
};

}  // namespace

int main(int argc, char** argv) {
  const std::string arm = argc > 1 ? argv[1] : "serial";
  const std::string crowd_path = argc > 2 ? argv[2] : "ids";
  const uint32_t frames = argc > 3 ? uint32_t(std::atoi(argv[3])) : 300;
  const bool extra = arm == "serial_extra" || arm == "tail_extra";
  const bool tail_on = arm != "serial" && arm != "serial_extra" && arm != "serial_lock";
  const bool race = arm == "tail_race";
  const bool flip = arm == "tail_flip";
  const bool release_first = arm == "plant_release_first";
  const bool lock_arm = arm == "serial_lock" || arm == "tail_lock" || arm == "plant_lock_nowait";
  g_lock_mode = arm == "plant_lock_nowait" ? 2 : (lock_arm ? 1 : 0);
  const bool throw_arm = arm == "tail_throw" || arm == "plant_throw_nocap";
  if (arm == "plant_throw_nocap") live::g_tail_plant = 100;   // the tracker latch without its cap (review F2's leak)
  if (arm == "plant_latch_skip") live::g_tail_plant = 1;
  if (arm.rfind("plant_skip_", 0) == 0) live::g_tail_plant = 2 + std::atoi(arm.c_str() + 11);   // plant_skip_I: only contributor I (registration order) does not latch
  live::crowd::g_crowd_ids_once.store(crowd_path == "ids" || crowd_path == "pool");
  rdr_scene::g_scene_strings_once.store(crowd_path == "pool");   // S102 T11b: the string pool seeded per record (crowd and arena pooled fields)
  live::crowd::g_crowd_run_markers.store(crowd_path == "runs");

  // the fake guest memory of the light tracker
  g_mem.words[live::lights::kGlobalWord] = FakeGuest::kG;
  g_mem.words[FakeGuest::kG + 4] = FakeGuest::kArr;
  g_mem.words[FakeGuest::kG + 208] = 1024, g_mem.words[FakeGuest::kG + 212] = 1024;
  g_mem.words[FakeGuest::kTls] = FakeGuest::kTls + 0x100;
  g_mem.words[FakeGuest::kTls + 0x100 + 1048] = FakeGuest::kBlk;
  g_mem.words[FakeGuest::kBlk + 32] = 0;
  g_mem.records.assign(size_t(live::lights::kRecordSize) * 4, 0);

  // registration in the game's order (wwe2k14_app.h OnPostSetup: arena, wrestlers, ...; the order only has to be the same in every arm)
  if (throw_arm) live::Register(&g_thrower);
  live::arena::RegisterArena();
  live::RegisterWrestlers();
  live::Register(&live::crowd::The());
  live::lights::RegisterLightStates();
  live::circles::RegisterCircles();
  live::plane::RegisterPlane();
  live::arenaworld::RegisterArenaWorlds();
  live::ringrows::RegisterRingRows();
  live::casters::RegisterCasters();
  live::spheres::RegisterSpheres();
  if (extra) live::Register(&g_plain);
  live::crowd::The().SetArena(5);
  {
    std::vector<live::arena::SlotSnap> slots(3);
    for (uint16_t i = 0; i < 3; ++i) slots[i].slot = uint16_t(10 + i), slots[i].light_set = i, slots[i].when = uint8_t(i == 1 ? 1 : (i == 2 ? 2 : 0));
    live::arena::PublishSlots(5, live::arena::Look::Entrance, std::move(slots), {});
  }
  RecSink sink;
  live::AddSink(&sink);
  live::AddSink(&g_keeper);

  tl::State st;  // the tail state of this run (the game's is tl::The())
  g_st = &st;
  st.SetFinish(&FinishModel);
  UpdateThread up;
  up.extra = extra;
  up.Start();
  up.Release();          // the first frame (frame 1)
  up.WaitArrived();

  for (uint32_t i = 0; i < frames; ++i) {
    // [the walk would be here] -> the barrier wait returned: every client arrived
    up.WaitArrived();
    st.OnArrive();
    if (release_first) {  // PLANTED: the update thread released before the present's latch
      st.OnRelease();
      up.Release();
      up.WaitArrived();
    }
    // the present
    const uint64_t g = g_guest_frame.load();
    const bool want = tail_on && (!flip || (i & 1));
    st.FlushIfPending(tl::kAtPresent);
    if (st.Decide(want ? tl::kWhyCount : uint8_t(tl::kOff))) {
      live::LatchFrame(g, live::kNoCaptureFrame, true, g_pending);
      g_pending_frame = g;
      st.SetPending(g_pending.emit_deferred);
    } else {
      live::EndFrame(g);
      std::lock_guard<std::mutex> lock(g_rmu);
      SubmitLocked(g);
    }
    g_guest_frame.fetch_add(1);  // VdSwap
    if (!release_first) {
      st.OnRelease();
      up.Release();
      if (!race) up.WaitArrived();  // the worst case: the whole next frame recorded before the finish
    }
    FinishModel(tl::kAtRelease);
  }
  up.WaitArrived();
  up.Stop();
  st.FlushIfPending(tl::kAtPresent);
  live::RemoveSink(&sink);
  live::RemoveSink(&g_keeper);
  for (const std::string& e : g_events) std::printf("%s\n", e.c_str());
  {
    const auto& L = live::lights::States().Latched();
    std::printf("trackerlatch kept %zu held %zu dropped %llu cap %u\n", L.Kept(), L.Held(), (unsigned long long)L.Dropped(), unsigned(live::lights::kRing));
  }

  for (const std::string& l : sink.lines) std::printf("%s\n", l.c_str());
  const tl::Stats& s = st.stats();
  const live::LatchTotals lt = live::GetLatchTotals();
  std::printf("stats presents %llu deferred %llu emit_deferred %llu late_latch %llu finished_at_release %llu finished_at_lock %llu finished_at_present %llu lock_in_window %llu\n", (unsigned long long)s.presents,
              (unsigned long long)s.deferred, (unsigned long long)s.emit_deferred, (unsigned long long)s.late_latch, (unsigned long long)s.finished[tl::kAtRelease],
              (unsigned long long)s.finished[tl::kAtLock], (unsigned long long)s.finished[tl::kAtPresent], (unsigned long long)s.lock_in_window);
  std::printf("latch latched %llu deferred %llu not_latchable %llu registry_changed %llu violations %llu first %s\n", (unsigned long long)lt.latched,
              (unsigned long long)lt.deferred, (unsigned long long)lt.not_latchable, (unsigned long long)lt.registry_changed, (unsigned long long)lt.violations,
              lt.first_violation.empty() ? "-" : lt.first_violation.c_str());
  const live::Totals tot = live::GetTotals();
  std::printf("totals frames %llu records %llu refused %llu\n", (unsigned long long)tot.frames, (unsigned long long)tot.records, (unsigned long long)tot.refused);
  return 0;
}
