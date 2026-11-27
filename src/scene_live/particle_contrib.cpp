// The particles as a live contributor: see particle_contrib.h.
#include "particle_contrib.h"

#include <cstdio>
#include <utility>

namespace rdr_scene::live::particles {

void ParticleContributor::Publish(ParticleSnapshot&& snapshot) {
  std::lock_guard<std::mutex> lock(mu_);
  pending_ = std::move(snapshot);
  have_ = true;
}

bool ParticleContributor::Latch(const FrameInfo& f) {
  if (source_) {  // F2: the latch reads the live particles itself (mode 0: the source returns at its first test)
    bool ok = source_(f.guest_frame, source_buf_);
    if (!ok && empty_source_) source_buf_.Clear(), ok = empty_source_(f.guest_frame, source_buf_);   // S103 MP: no node call in the frame = a whole empty pass
    latch_ran_ = true;
    if (ok) {
      std::swap(latched_, source_buf_);  // (capacity cycles between the two: no allocation once grown)
      source_buf_.Clear();
      have_latched_ = true;
    } else {
      latched_.Clear(), have_latched_ = false;
    }
    return true;
  }
  std::lock_guard<std::mutex> lock(mu_);
  latch_ran_ = true;
  if (have_) {
    latched_ = std::move(pending_);
    pending_.Clear();
    have_ = false;
    have_latched_ = true;
  } else {
    latched_.Clear();
    have_latched_ = false;
  }
  return true;
}

uint32_t ParticleContributor::WriteTables(const ParticleSnapshot& s, uint32_t frame_stamp, SceneTables& t) {
  t.particles.Clear();
  if (s.status.reasons) t.particles.status.push_back(s.status);  // lint:reasons-ok (the row is written for ANY bit, the marker included) B2: the render side turns a not-whole pass into a hole (S103 MP: or reads the empty-pass marker)
  if (s.has_screen) t.particles.screen.push_back(s.screen);  // S103 MP
  std::vector<uint32_t> asset_of(s.keys.size(), kNoParticleTexture);
  std::vector<char> asset_done(s.keys.size(), 0);
  uint32_t open_key = 0xFFFFFFFFu;
  for (const ParticleSnapshot::Entry& e : s.entries) {
    if (t.particles.particles.size() >= kMaxParticlesPerFrame) break;
    if (e.key >= s.keys.size()) continue;
    if (open_key != e.key) {
      const BatchKey& k = s.keys[e.key];
      ParticleBatch b;
      b.first = uint32_t(t.particles.particles.size());
      b.count = 0;
      if (!asset_done[e.key]) {  // one AssetRef per distinct texture, made once per key
        asset_done[e.key] = 1;
        if (!k.pac.empty() || !k.entry.empty()) {
          const uint32_t pac = t.AddString(k.pac), entry = t.AddString(k.entry);
          for (size_t i = 0; i < t.assets.size(); ++i)
            if (t.assets[i].kind == uint8_t(AssetKind::Texture) && t.assets[i].pac == pac && t.assets[i].entry == entry) asset_of[e.key] = uint32_t(i);
          if (asset_of[e.key] == kNoParticleTexture) {
            AssetRef a;
            a.kind = uint8_t(AssetKind::Texture);
            a.pac = pac, a.entry = entry;
            t.assets.push_back(a);
            asset_of[e.key] = uint32_t(t.assets.size() - 1);
          }
        }
      }
      b.texture = asset_of[e.key];
      b.blend = k.blend, b.soft = k.soft, b.site = k.site, b.addr = k.addr;
      b.depth_always = k.depth_always;   // S111 PARTZ
      b.zoffset = k.zoffset, b.use_normal = k.use_normal;
      for (int i = 0; i < 4; ++i) b.light[i] = k.light[i];
      b.frame = frame_stamp;
      b.seq_after_p1 = k.seq_after_p1;  // S103 MP / S111 FLATSEQ
      t.particles.batches.push_back(b);
      open_key = e.key;
    }
    t.particles.particles.push_back(e.p);
    ++t.particles.batches.back().count;
  }
  return uint32_t(t.particles.particles.size());
}

void ParticleContributor::Emit(const FrameInfo& f, SceneTables& t, Counters& c) {
  ParticleSnapshot taken;
  const ParticleSnapshot* snap = nullptr;
  if (latch_ran_) {
    latch_ran_ = false;
    if (!have_latched_) { none_.fetch_add(1, std::memory_order_relaxed); return; }
    snap = &latched_;  // taken at the barrier: no shared read here
  } else if (source_) {
    // B1: Latch runs only when the tail decision is on (off by default), so with no latch the source runs HERE, as inputs_contrib.h does. A deferred emit must not read guest memory.
    if (InDeferredEmit()) { ++c.stale; return; }
    taken.Clear();
    if (!source_(f.guest_frame, taken) && !(empty_source_ && (taken.Clear(), empty_source_(f.guest_frame, taken)))) { none_.fetch_add(1, std::memory_order_relaxed); return; }   // (S103 MP: the empty source second)
    snap = &taken;
  } else {
    if (InDeferredEmit()) NoteViolation(Name());  // not latched: this read is of shared state
    std::lock_guard<std::mutex> lock(mu_);
    if (!have_) { none_.fetch_add(1, std::memory_order_relaxed); return; }
    taken = std::move(pending_);  // moved, not copied; pending is empty after use
    pending_.Clear();
    have_ = false;
    snap = &taken;
  }
  const uint64_t want = raw_compare_ ? f.guest_frame : HookFrame(f.guest_frame);
  if (snap->guest_frame != want) {  // older (or newer) than this record: never written (the stale count says the reader did not run)
    stale_.fetch_add(1, std::memory_order_relaxed);
    ++c.stale;
    if (have_latched_) latched_.Clear(), have_latched_ = false;
    return;
  }
  const uint32_t n = WriteTables(*snap, uint32_t(f.guest_frame), t);
  frames_.fetch_add(1, std::memory_order_relaxed);
  batches_.fetch_add(t.particles.batches.size(), std::memory_order_relaxed);
  particles_.fetch_add(n, std::memory_order_relaxed);
  dropped_.fetch_add(snap->dropped + uint32_t(snap->entries.size() > n ? snap->entries.size() - n : 0), std::memory_order_relaxed);
  if (have_latched_) latched_.Clear(), have_latched_ = false;
}

void ParticleContributor::Census(std::string& out) {
  char line[200];
  std::snprintf(line, sizeof line, ", particles: frames %llu, batches %llu, particles %llu, stale %llu, dropped %llu, no_snapshot %llu",
                (unsigned long long)frames_.load(std::memory_order_relaxed), (unsigned long long)batches_.load(std::memory_order_relaxed),
                (unsigned long long)particles_.load(std::memory_order_relaxed), (unsigned long long)stale_.load(std::memory_order_relaxed),
                (unsigned long long)dropped_.load(std::memory_order_relaxed), (unsigned long long)none_.load(std::memory_order_relaxed));
  out += line;
  if (census_extra_) out += " " + census_extra_();
}

}  // namespace rdr_scene::live::particles
