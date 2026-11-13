// Our props of other pacs drawn in place. See props_ours.h and props_ours_core.h. S101 lane 84 (a copy of ring_ours.cpp's renderer side).
#include "props_ours.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "../renderer/problem.h"
#include "../renderer/shaders/spv/fx_registry.h"

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;
using rdr_adapter::KeepWhy;

constexpr uint32_t kIdleDrop = 300;   // frames with no arena in the record before the assets go (the load screen)

size_t MipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

}  // namespace

PropsOurs::PropsOurs(rdr::Renderer& renderer, const PropsOursOptions& options) : renderer_(renderer), options_(options), core_(options), root_(options.game_root) {
  core_.SetReleaseHook([this](PropsPrepared& p) { Release(p); });
}

PropsOurs::~PropsOurs() {
  if (worker_.joinable()) worker_.join();
  renderer_.WaitIdle();
  DropAll();
}

void PropsOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  std::lock_guard<std::mutex> lock(mu_);
  record_ = std::move(record);
}

void PropsOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  root_ = root;
}

void PropsOurs::Release(PropsPrepared& p) {
  if (p.gpu[0]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[0]));
  if (p.gpu[1]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[1]));
  p.gpu[0] = p.gpu[1] = 0;
}

// Review F2: the set (meshes, textures) does not depend on the arena: an idle drop releases only the GPU buffers and images; the next match re-uploads on first use. DropAll (the set too) is for a new load and the destructor.
void PropsOurs::ReleaseGpu() {
  core_.ReleaseAll();
  for (const auto& kv : images_)
    if (kv.second != rdr::kNoImage) renderer_.DestroyImage(kv.second);
  images_.clear();
}

void PropsOurs::DropAll() {
  core_.SetProps(nullptr);   // releases every prepared buffer through Release
  for (const auto& kv : images_)
    if (kv.second != rdr::kNoImage) renderer_.DestroyImage(kv.second);
  images_.clear();
  props_.reset();
  uploaded_ = false;
}

rdr::ImageId PropsOurs::ImageFor(int texture, uint32_t mips) {
  if (!props_ || texture < 0 || size_t(texture) >= props_->textures.size()) return rdr::kNoImage;
  const uint64_t key = (uint64_t(uint32_t(texture)) << 8) | std::min<uint32_t>(mips, 255);
  const auto it = images_.find(key);
  if (it != images_.end()) return it->second;
  return images_[key] = MakeTexture(props_->textures[size_t(texture)], mips);
}

rdr::ImageId PropsOurs::MakeTexture(const assets::Texture& t, uint32_t max_mips) {
  if (t.width == 0 || t.height == 0 || t.format == assets::TexFormat::kUnknown) return rdr::kNoImage;
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if ((bc && ((t.width % 4) || (t.height % 4))) || t.width > 16384 || t.height > 16384) return rdr::kNoImage;
  uint32_t mips = std::min(std::max(1u, t.mips), std::max(1u, max_mips)), mx = std::max(t.width, t.height), maxmips = 1;
  while (mx > 1) mx >>= 1, ++maxmips;
  mips = std::min(mips, maxmips);
  size_t total = 0;
  uint32_t fit = 0;
  for (uint32_t i = 0; i < mips; ++i) {
    const size_t sz = MipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (total + sz > t.data.size()) break;
    total += sz, ++fit;
  }
  if (fit == 0) return rdr::kNoImage;
  rdr::ImageDesc d;
  d.width = uint16_t(t.width);
  d.height = uint16_t(t.height);
  d.mips = uint8_t(fit);
  d.usage = rdr::kUsageSampled;
  switch (t.format) {
    case assets::TexFormat::kBC1: d.format = rdr::Format::BC1; break;
    case assets::TexFormat::kBC2: d.format = rdr::Format::BC2; break;
    case assets::TexFormat::kBC3: d.format = rdr::Format::BC3; break;
    case assets::TexFormat::kB8G8R8A8: d.format = rdr::Format::BGRA8; break;
    default: d.format = rdr::Format::RGBA8; break;
  }
  const rdr::ImageId id = renderer_.CreateImage(d);
  if (id == rdr::kNoImage) return id;
  size_t src = 0;
  std::vector<uint8_t> wide;
  for (uint32_t i = 0; i < fit; ++i) {
    const uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
    const size_t sb = MipBytes(t.format, mw, mh);
    if (t.format == assets::TexFormat::kB4G4R4A4) {
      wide.resize(size_t(mw) * mh * 4);
      for (size_t k = 0; k < size_t(mw) * mh; ++k) {
        const uint16_t px = uint16_t(t.data[src + 2 * k] | (t.data[src + 2 * k + 1] << 8));
        wide[4 * k] = uint8_t(((px >> 8) & 15) * 17), wide[4 * k + 1] = uint8_t(((px >> 4) & 15) * 17), wide[4 * k + 2] = uint8_t((px & 15) * 17), wide[4 * k + 3] = uint8_t(((px >> 12) & 15) * 17);
      }
      renderer_.UpdateImage(id, i, 0, wide.data(), wide.size());
    } else {
      renderer_.UpdateImage(id, i, 0, t.data.data() + src, sb);
    }
    src += sb;
  }
  return id;
}

// ---- loading ----------------------------------------------------------------------------------------------------------------------

// S101 lane 271 (SKIP_GAME_WORK_MAP 19.8): the in-game sequence's match card names the arena ~1.5 s before the first record that draws it. Starting the load then (on its worker,
// as FrameReady would) has the meshes ready when that record arrives instead of 0.1-8 s after it. A load already running or done, a record that already named an arena, or the
// same arena again does nothing; a wrong arena is a wasted load that FrameReady discards (loaded_.arena != arena_) and replaces.
void PropsOurs::Announce(int arena) {
  if (load_state_.load() == 2 && loaded_.arena != 1) {   // a finished early load for another arena (the player backed out after the card) must not block this one
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (arena < 0 || uploaded_ || arena_ >= 0 || load_state_.load() != 0 || load_for_ == 1) return;
  StartLoad(1);
}

void PropsOurs::StartLoad(int arena) {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mu_);
    root = root_;
  }
  if (root.empty()) {
    if (!said_no_root_) said_no_root_ = true, rdr::Log("[rdr][props] no game folder to load the props from: the stream's props stay");
    return;
  }
  if (worker_.joinable()) worker_.join();
  load_state_ = 1;
  load_for_ = arena;
  worker_ = std::thread([this, arena, root] {
    Loaded loaded;
    loaded.arena = arena;
    if (!assets::prop_gs::Init(root)) {
      loaded.error = "no game files at " + root;
    } else {
      auto pset = std::make_shared<assets::prop_gs::PropSet>();
      if (assets::prop_gs::Load(*pset, loaded.error)) loaded.props = std::move(pset);
    }
    loaded_ = std::move(loaded);
    load_state_ = 2;
  });
}

bool PropsOurs::Upload(Loaded& loaded) {
  DropAll();
  if (!loaded.props) {
    rdr::Log("[rdr][props] the props could not be loaded (%s): the stream's props stay", loaded.error.c_str());
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const assets::prop_gs::PropSet& r = *loaded.props;
  props_ = loaded.props;
  core_.SetProps(props_);
  upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  rdr::Log("[rdr][props] %zu prop meshes from %zu sources (%.1f MB), %zu file textures (images are made at first use, with the stream image's mip count)", r.meshes.size(), r.sources.size(), double(r.Bytes()) / 1048576.0, r.textures.size());
  loaded = Loaded{};
  uploaded_ = true;
  return true;
}

// ---- the frame --------------------------------------------------------------------------------------------------------------------

void PropsOurs::NoClaimFrame() {
  core_.NoClaimFrame();   // lane 115 F2: the core's clock also runs on frames where every object is foreign (nothing claimed -> FrameReady never runs), or the 300-frame verdicts never expire
  {   // keyed on the record, not on draws: a close-up with every ring draw culled still has the arena in the record
    std::lock_guard<std::mutex> lock(mu_);
    if (record_ && PropsCore::InMatch(*record_, options_.faults.editor_as_match, options_.faults.special_ids)) {
      idle_frames_ = 0;
      const int rec_arena = 1;   // S101 lane 84: the props are loaded once for any match (no arena key)
      if (rec_arena != arena_) arena_ = rec_arena;   // (a match again: the set is still loaded, buffers and images come back on first use)
      return;
    }
  }
  if (idle_frames_ + 1 == 12 * kIdleDrop && load_state_.load() == 2 && !uploaded_ && arena_ < 0) {   // lane 271: an early load nobody claimed (the player left after the card) gives its memory back
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
    load_for_ = -1;
    rdr::Log("[rdr][props] an early load was never claimed: released");
  }
  if (++idle_frames_ == kIdleDrop && (uploaded_ || arena_ >= 0)) {
    ReleaseGpu();
    arena_ = -1;
    rdr::Log("[rdr][props] no match for %u frames: the GPU buffers and images are released (the file set stays loaded)", kIdleDrop);
  }
}

bool PropsOurs::FrameReady(const rdr_adapter::LoadedFrame&) {
  idle_frames_ = 0;
  std::shared_ptr<const SceneTables> record;
  {
    std::lock_guard<std::mutex> lock(mu_);
    record = record_;
  }
  core_.ResetFrame();
  if (++frames_seen_ % 600 == 0) {
    const std::string fail = core_.TakeFirstFail();
    rdr::Log("%s%s%s", Report().c_str(), fail.empty() ? "" : "; first failed check since the last line: ", fail.c_str());
  }
  if (!record) return ++not_ready_frames_, false;
  const int arena = PropsCore::InMatch(*record, options_.faults.editor_as_match, options_.faults.special_ids) ? 1 : -1;   // S101 lane 84: "in a match": loaded once, not per arena
  if (arena < 0) return ++not_ready_frames_, false;
  arena_ = arena;   // (1 = in a match; the set is not per arena, review F2)
  if (!uploaded_) {
    const int state = load_state_.load();
    if (state == 2) {
      if (worker_.joinable()) worker_.join();
      if (loaded_.arena == arena_) Upload(loaded_);
      else loaded_ = Loaded{};
      load_state_ = 0;   // (a failed upload is not retried every frame: load_for_ still names this arena)
    } else if (state == 1 && load_for_ != arena_) {
      static int logged_for = -2;
      if (logged_for != arena_) {
        logged_for = arena_;
        rdr::Log("[rdr][props] early load for arena %d is being replaced by %d", load_for_, arena_);
      }
    } else if (state == 0 && load_for_ != arena_) {
      StartLoad(arena_);
    }
  }
  if (!uploaded_ && options_.verify_all && load_state_.load() == 1) {   // offline: wait for the worker, so every frame of a capture is judged
    if (worker_.joinable()) worker_.join();
    if (loaded_.arena == arena_) Upload(loaded_);
    else loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (!uploaded_) return ++not_ready_frames_, false;
  ++ours_frames_;
  return true;
}

bool PropsOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  if (!uploaded_ || in.vs < 0 || size_t(in.vs) >= std::size(kVariants)) {
    why = KeepWhy::NotReady;
    return false;
  }
  const rdr::fx::Variant* ps = in.ps >= 0 && size_t(in.ps) < std::size(kVariants) ? &kVariants[in.ps] : nullptr;
  uint8_t slots[rdr::kMaxTextures];
  uint32_t slot_count = 0;
  if (ps)
    for (uint32_t s = 0; s < ps->sampler_count && slot_count < rdr::kMaxTextures; ++s) slots[slot_count++] = ps->samplers[s].slot;
  const PropsCore::Decision d = core_.Decide(in, slots, slot_count);
  if (!d.ok) {
    why = d.why;
    return false;
  }
  PropsPrepared& p = *d.prepared;
  if (!p.gpu[0]) {
    p.gpu[0] = renderer_.CreateBuffer(p.vertex.size(), p.vertex.data());
    p.gpu[1] = renderer_.CreateBuffer(p.index.size() * 2, p.index.data());
    if (!p.gpu[0] || !p.gpu[1]) {   // a failure is remembered (no retry on every draw) and the draw stays the stream's
      Release(p);
      core_.UploadFailed(&p);
      return why = KeepWhy::Mismatch, false;
    }
  }
  out = rdr_adapter::ReplaceOut{};
  out.vertex = {rdr::BufferId(p.gpu[0]), 0};
  out.index = {rdr::BufferId(p.gpu[1]), 0};
  out.index_count = uint32_t(p.index.size());
  // The stream's own geometry description: our vertex IS in the stream draw's host layout and our index buffer IS the stream's strip.
  out.vertex_layout = in.stream_layout;
  out.topology = in.stream_topology;
  out.index_32bit = in.stream_index_32bit;
  const auto put = [&](int slot, int tex, uint32_t mips) {
    if (tex < 0 || !in.textures || in.textures->slots[slot].image == rdr::kNoImage) return;   // the contract: the stream draw must bind an image there
    const rdr::ImageId img = ImageFor(tex, mips);
    if (img == rdr::kNoImage) return;
    out.slots[slot].set = true;
    out.slots[slot].image = img;
  };
  put(0, d.diffuse, d.diffuse_mips), put(4, d.normal_map, d.normal_mips), put(5, d.specular, d.specular_mips);
  return true;
}

std::string PropsOurs::Report() const {
  char head[200];
  std::snprintf(head, sizeof head, "[rdr][props] ours frames %llu, not ready %llu, upload %.1f ms, prepared %zu; ", (unsigned long long)ours_frames_, (unsigned long long)not_ready_frames_, upload_ms_, core_.prepared_count());
  return std::string(head) + core_.Report();
}

}  // namespace rdr_scene
