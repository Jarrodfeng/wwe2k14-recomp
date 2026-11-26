// The particles as a live contributor to the scene record (Session 103, lane F; docs\renderer\SEE_THROUGH_ORDER.md sections 5-6). No game or SDK includes:
// the source reader (section 6, route 2: the per-particle arrays read at the latch, billboards expanded against OUR camera) fills a ParticleSnapshot with decoded
// rows and publishes it; Latch takes it at the frame barrier; Emit writes it into the record's particle tables (ids 54 / 55). The tables are what
// src\renderer_scene\live_particles.h draws.
//
// Frame discipline: a snapshot is stamped with the HOOK clock's frame it was captured in; Emit writes it only when snap.guest_frame == hook_clock(f.guest_frame)
// (scene_frame::HookFrameOf: the hook clock runs one behind the writer's frame number by the board's rule). An older snapshot (the reader did not run this frame) is
// counted stale and nothing is written: never last frame's particles in this frame's record.
//
// Cost: a snapshot holds rows and a small table of distinct batch keys (interned): no string per particle. Latch moves the pending snapshot into the contributor's own
// member under the lock (S102 U1: the writer's Emit then reads nothing shared); pending is empty after use.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../renderer_scene/live_scene.h"
#include "../renderer_scene/particle_tables.h"

namespace rdr_scene::live::particles {

inline constexpr uint32_t kMaxParticlesPerFrame = 8192;  // Check B's heaviest frame holds 2,750 draws [C: docs\systems\particles.md]; past the cap the rest are counted, never silent

// What a draw of a batch shares: the key the snapshot builder starts a new batch on. `pac` + `entry` name the particle texture, as an AssetRef would.
struct BatchKey {
  std::string pac, entry;
  uint8_t blend = kParticleAlpha;
  uint8_t soft = 0;
  uint8_t site = kParticleSprite;
  uint8_t addr = 0;  // S103 F3: the sampler-address byte + 1 (0 = not set)
  uint8_t depth_always = 0;  // S111 PARTZ: 1 = the entry's +220 word is 0: the game draws with depth ALWAYS (sub_82A0AED8 +84)
  float zoffset = 0, use_normal = 0;
  float light[4] = {0, 0, 0, 0};
  uint32_t row_after_p1 = 0;  // S103 MP: the 2D row count at the node call that made the batch, + 1 (0 = not noted). A batch never spans two node calls: it is part of the key
  bool SameBatch(const BatchKey& o) const {
    return row_after_p1 == o.row_after_p1 && pac == o.pac && entry == o.entry && blend == o.blend && soft == o.soft && site == o.site && addr == o.addr && depth_always == o.depth_always && zoffset == o.zoffset && use_normal == o.use_normal &&
           light[0] == o.light[0] && light[1] == o.light[1] && light[2] == o.light[2] && light[3] == o.light[3];
  }
};

// One frame's particles in the game's draw order. Keys are interned: Add() returns the same index for equal keys, entries hold the index only.
struct ParticleSnapshot {
  uint64_t guest_frame = 0;     // the hook clock's frame
  std::vector<BatchKey> keys;   // the distinct keys (a handful a frame)
  struct Entry { uint32_t key; Particle p; };
  std::vector<Entry> entries;   // consecutive entries with the same key form one batch
  uint32_t dropped = 0;         // particles the reader could not keep (the cap): counted
  ParticleStatus status;        // why the pass is not whole (B2): carried into the record
  bool has_screen = false;      // S103 MP: the frame's batches were drawn with the node's own 2D screen camera: `screen` goes into the record (kScParticleScreen)
  ParticleScreen screen;
  uint32_t Intern(const BatchKey& k) {
    for (uint32_t i = 0; i < keys.size(); ++i)
      if (keys[i].SameBatch(k)) return i;
    keys.push_back(k);
    return uint32_t(keys.size() - 1);
  }
  void Add(uint32_t key, const Particle& p) { entries.push_back({key, p}); }
  void Clear() { guest_frame = 0, keys.clear(), entries.clear(), dropped = 0, status = ParticleStatus(), has_screen = false, screen = ParticleScreen(); }
};

class ParticleContributor final : public rdr_scene::live::Contributor {
 public:
  using HookClock = std::function<uint64_t(uint64_t)>;  // writer frame -> hook-clock frame
  // `clock` must be scene_frame::HookFrameOf in the game (the game's registration passes it); the default is the identity (offline tools).
  explicit ParticleContributor(HookClock clock = HookClock()) : clock_(std::move(clock)) {}
  // Test only: the contributor compares against the writer's frame instead of the hook clock (the planted fault).
  void PlantRawFrameCompare(bool on) { raw_compare_ = on; }

  const char* Name() const override { return "particles"; }
  // Lane F2: the latch's own source (particle_source.h): called in Latch with the writer's frame; true = it filled the snapshot (stamped with the hook clock). Null = Publish() only.
  using Source = std::function<bool(uint64_t writer_frame, ParticleSnapshot&)>;
  void SetSource(Source s) { source_ = std::move(s); }
  // S103 MP: asked when the source took nothing (no node call in the paired frame): true = `out` is the frame's EMPTY-PASS snapshot (kPiEmptyPass, stamped with the hook clock), written like a taken one. Null = off.
  void SetEmptySource(Source s) { empty_source_ = std::move(s); }
  void SetCensusExtra(std::function<std::string()> f) { census_extra_ = std::move(f); }
  // Any thread (the reader's). Replaces the pending snapshot (moved in).
  void Publish(ParticleSnapshot&& snapshot);
  bool Latch(const FrameInfo& f) override;
  void Emit(const FrameInfo& f, SceneTables& t, Counters& c) override;
  void Census(std::string& out) override;

  // The pure part, public for the offline tests: snapshot -> tables (+ asset refs in `t`). Returns the particle count written.
  static uint32_t WriteTables(const ParticleSnapshot& s, uint32_t frame_stamp, SceneTables& t);

 private:
  uint64_t HookFrame(uint64_t writer_frame) const { return clock_ ? clock_(writer_frame) : writer_frame; }
  std::mutex mu_;
  ParticleSnapshot pending_;
  bool have_ = false;
  ParticleSnapshot latched_;   // taken at the barrier; read only by Emit (writer thread)
  bool have_latched_ = false, latch_ran_ = false;
  HookClock clock_;
  Source source_, empty_source_;
  std::function<std::string()> census_extra_;
  ParticleSnapshot source_buf_;
  bool raw_compare_ = false;
  // Census (relaxed counters; the line is built only when the census prints)
  std::atomic<uint64_t> frames_{0}, batches_{0}, particles_{0}, stale_{0}, dropped_{0}, none_{0};
};

}  // namespace rdr_scene::live::particles
