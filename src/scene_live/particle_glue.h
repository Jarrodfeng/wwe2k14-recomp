// The particle source: the game plumbing (Session 103, lane F2; docs\renderer\SEE_THROUGH_ORDER.md section 6, docs\formats\particle_items.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Gives the pure source (particle_source.h) its guest reader (the inputs glue's bounds- and fault-checked ReadChecked) and the node address, and registers the particle contributor with the
// scene writer. The node address is r3 of sub_82254E30 (the particle node's render entry, the pass object: its +16 / +3084 / +3088 / +20+i are what the entry's own listing reads): the
// existing hook on that function (scene_skip_nodes.h) calls NoteNode, one relaxed load when the scene switch is 0. Everything else runs at the frame's latch (game threads parked).
// Read only: nothing is written to the guest, no call is skipped.
#pragma once
#include "../thread_name.h"  // S103 H1
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "../renderer/problem.h"   // CAWH: rdr::Log (the run log)
#include "../dev_build.h"   // S103 PF: IsDevBuild (the namer miss diagnostic)
#include "../renderer_scene/live_assets.h"   // S103 F5: ForEachParticleSpriteDirectory (the NameIndex's boot-time source)
#include "inputs_glue.h"   // ReadChecked
#include "particle_contrib.h"
#include "../renderer_game/scene_stamp_rule.h"   // S103 FLAT
#include "particle_hasher.h"   // S103 F5
#include "particle_namer.h"    // S103 F5
#include "particle_source.h"
#include "scene_skip.h"   // wwe2k14::scene_skip::HookFrame

namespace wwe2k14::scene_particle_glue {

inline rdr_scene::live::particles::ParticleSource& Source() {
  static rdr_scene::live::particles::ParticleSource s;
  return s;
}
inline rdr_scene::live::particles::ParticleContributor& Contributor() {
  static rdr_scene::live::particles::ParticleContributor c([](uint64_t f) { return scene_frame::HookFrameOf(f); });   // the contributor accepts a snapshot of the HOOK clock's frame
  return c;
}

// S103 F5: the live TextureNamer (particle_namer.h wired for the game). The NameIndex (every sprite directory of the player's own pac\prtcls.pac, keyed by mip-0 content) is built ONCE on a worker
// thread started by Register(root) (never on a game or writer thread); until it is ready, and for any handle the namer cannot name, Name() answers false and the pass keeps kPiTexture (the white
// stand-in), counted. The registry walk, the six fetch dwords and the content hash run at the latch (game threads parked) on the writer's thread: guest reads only through ReadChecked / phys,
// never a write. Single consumer: the namer's own cache is not shared.
class LiveNamer final : public rdr_scene::live::particles::TextureNamer {
 public:
  ~LiveNamer() override {
    if (worker_.joinable()) worker_.join();
  }
  void SetRoot(const std::string& r) { root_ = r; }
  // S103 F5: started lazily by the first node note with the scene mode on (no disk work in a run that records nothing); idempotent, one relaxed load after the first.
  void StartOnce() {
    if (started_flag_.load(std::memory_order_relaxed) || root_.empty()) return;
    if (started_flag_.exchange(true)) return;
    Start(root_);
  }
  void Start(const std::string& game_root) {
    if (started_ || game_root.empty()) return;
    started_ = true;
    worker_ = std::thread([this, game_root] { wwe2k14::NameThread(L"particle_sprites");
      std::string err;
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = rdr_scene::BuildParticleSprites(game_root, index_, err);   // index + the decoded sprites (published) in one pass; the opened pac is freed inside
      const uint32_t dirs = 0;
      build_ms_.store(uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
      dirs_.store(dirs, std::memory_order_relaxed);
      if (!ok) { failed_.store(true, std::memory_order_release); return; }
      inner_ = std::make_unique<Inner>(index_);
      ready_.store(true, std::memory_order_release);   // the index and inner_ are complete before this store; no one touches them while it is false
    });
  }
  bool Name(uint32_t handle_id, uint32_t cached_ptr, std::string& pac, std::string& entry) const override {
    if (!ready_.load(std::memory_order_acquire)) { not_ready_.fetch_add(1, std::memory_order_relaxed); return false; }
    const bool ok = inner_->namer.Name(handle_id, cached_ptr, pac, entry);
    if (wwe2k14::IsDevBuild()) for (const std::string& l : inner_->namer.TakeDiag()) rdr::Log("%s", l.c_str());   // S103 PF: the first 8 misses, dev builds only (CAWH: the run log; stderr never reaches it)
    return ok;
  }
  // "[scene][particles] namer: ready R (failed F, dirs D, textures T, dup X, ms M), not_ready N, asked A, named B, cache_hits H, no_handle .., not_2d .., no_header .., unreadable .., no_key .., not_in_index .."
  std::string Census() const {
    char b[420];
    const auto L = [](const std::atomic<uint32_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    const bool r = ready_.load(std::memory_order_acquire);
    std::snprintf(b, sizeof b, "[scene][particles] namer: ready %d (failed %d, dirs %llu, textures %zu, dup %zu, ms %llu), not_ready %llu", r ? 1 : 0, failed_.load() ? 1 : 0, L(dirs_), r ? index_.size() : size_t(0), r ? index_.duplicates() : size_t(0), L(build_ms_),
                  (unsigned long long)not_ready_.load(std::memory_order_relaxed));
    std::string out = b;
    if (r) {
      const auto& st = inner_->namer.stats();
      std::snprintf(b, sizeof b, ", asked %llu, named %llu, cache_hits %llu, no_handle %llu, not_2d %llu, no_header %llu, unreadable %llu, no_key %llu, not_in_index %llu", (unsigned long long)st.asked, (unsigned long long)st.named,
                    (unsigned long long)st.cache_hits, (unsigned long long)st.no_handle, (unsigned long long)st.not_2d, (unsigned long long)st.no_header, (unsigned long long)st.wrapper_unreadable, (unsigned long long)st.no_key,
                    (unsigned long long)st.not_in_index);
      out += b;
      // S108 PRT: the index misses named from the game's own texture; guest_failed = the game's texture could not be read (each one a skipped batch: a fault)
      const auto& gs = rdr_scene::live::particles::GuestSprites::Process();
      // (review: `short` = the guest held fewer levels than the file, those it held equal: the chain past them was not checked)
      std::snprintf(b, sizeof b, ", guest %llu, guest_failed %llu, guest_check equal %llu differ %llu short %llu, guest_store %zu (%zu KB, evicted %llu)", (unsigned long long)st.guest, (unsigned long long)st.guest_failed,
                    (unsigned long long)st.guest_check_equal, (unsigned long long)st.guest_check_differ, (unsigned long long)st.guest_check_short, gs.entries(), gs.bytes() >> 10, (unsigned long long)gs.evicted());
      out += b;
    }
    return out;
  }
 private:
  struct Inner {
    explicit Inner(const rdr_scene::live::particles::NameIndex& idx)
        : mem(&wwe2k14::scene_inputs_glue::ReadChecked),
          hasher([](uint32_t a, uint32_t n, uint8_t* d) { return wwe2k14::scene_inputs_glue::CrowdGuestMem{}.phys(a, n, d); }),
          namer(mem, hasher, idx) {
      namer.EnableDiag(wwe2k14::IsDevBuild());
      namer.SetGuestStore(&rdr_scene::live::particles::GuestSprites::Process());   // S108 PRT: an index miss reads the game's own texture (a DLC pack's / mod's particle file is not in the index)
      if (wwe2k14::IsDevBuild())   // S108 PRT: the guest chain checked against the file on every newly named disc sprite (census guest_check)
        namer.SetFileSprite([](const std::string& entry) -> std::shared_ptr<const assets::Texture> {
          const auto map = rdr_scene::ParticleSpritesNow();
          if (!map) return nullptr;
          const auto it = map->find(entry);
          return it == map->end() ? nullptr : std::shared_ptr<const assets::Texture>(map, &it->second);
        });
    }
    rdr_scene::live::particles::FnGuestMem mem;
    rdr_scene::live::particles::GuestContentHasher<bool (*)(uint32_t, uint32_t, uint8_t*)> hasher;
    rdr_scene::live::particles::HandleTextureNamer namer;
  };
  rdr_scene::live::particles::NameIndex index_;
  std::unique_ptr<Inner> inner_;
  std::thread worker_;
  bool started_ = false;
  std::string root_;
  std::atomic<bool> started_flag_{false};
  std::atomic<bool> ready_{false}, failed_{false};
  std::atomic<uint32_t> build_ms_{0}, dirs_{0};
  mutable std::atomic<uint64_t> not_ready_{0};
};
inline LiveNamer& Namer() {
  static LiveNamer n;
  return n;
}

// Called by the node's render-entry hook (scene_skip_nodes.h). Mode 0: one relaxed load, no store.
// S103 MP: `r13` = the calling thread's block (the node's camera comes from the camera table slot it names), `rows` = the q2d tap's running 2D row count of the frame (kNoRows = tap off). Both are read here, at the entry,
// because the node rewrites the singleton's camera words in its own prologue: the camera table object is the node's real input.
// S103 FLAT: returns whether this call may ask the scene decision (scene_stamp_rule.h: only a call that draws with a non-screen camera; a menu backdrop call must not stamp its frame as 3D). true in mode 0 (no ask is made then).
inline bool NoteNode(uint32_t node, uint32_t r13 = 0, uint32_t rows = rdr_scene::live::particles::kNoRows) {
  if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return true;
  Namer().StartOnce();   // S103 F5: first use with the mode on
  rdr_scene::live::particles::NodeCall c;
  c.node = node, c.rows = rows;
  if (r13) rdr_scene::live::particles::ReadNodeCamera(&wwe2k14::scene_inputs_glue::ReadChecked, r13, c);
  rdr_scene::live::particles::ReadNodeWords(&wwe2k14::scene_inputs_glue::ReadChecked, node, c);   // S103 MPBF 2: +3084 / +3088 are read AT THE CALL (the latch sees them after the node's own writes)
  Source().NoteCall(c, wwe2k14::scene_skip::HookFrame());   // the clock is read only past the mode test: mode 0 is one relaxed load
  return scene_stamp_rule::ParticleCallAsksScene(c.have_camera, rdr_scene::live::particles::IsScreenCall(c), rdr_scene::live::particles::NodeDrawsNothing(c));
}

// Once, at start (wwe2k14_app.h OnPostSetup, after the inputs glue). Idempotent.
inline void Register(const std::string& game_root = std::string()) {
  static bool done = false;
  if (done) return;
  done = true;
  Source().SetReader(&wwe2k14::scene_inputs_glue::ReadChecked);
  if (!game_root.empty()) {   // S103 F5: the namer (the index builds on its own thread; no root = no namer: every texture stays unresolved, as before)
    Namer().SetRoot(game_root);
    Source().SetNamer(&Namer());
  }
  auto& c = Contributor();
  c.SetSource([](uint64_t writer_frame, rdr_scene::live::particles::ParticleSnapshot& out) { return Source().Take(writer_frame, out); });
  c.SetEmptySource([](uint64_t writer_frame, rdr_scene::live::particles::ParticleSnapshot& out) { return Source().TakeEmpty(writer_frame, out); });   // S103 MP (A): no node call in the frame = a whole empty pass
  c.SetCensusExtra([] { return Source().Census() + " " + Namer().Census(); });
  rdr_scene::live::Register(&c);
}

}  // namespace wwe2k14::scene_particle_glue
