// Our arena drawn in place. See arena_ours.h and arena_ours_core.h. Session 101, lane 40.
#include "arena_ours.h"

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

constexpr uint32_t kIdleDrop = 300;  // frames with no arena in the record before the assets go (the load screen)

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

ArenaOurs::ArenaOurs(rdr::Renderer& renderer, const ArenaOursOptions& options)
    : renderer_(renderer), options_(options), core_(options.faults, options.verify_content), root_(options.game_root) {
  core_.SetAbsU(options.uv_abs_u);
  core_.SetImageInfo(options.image_info);
}

ArenaOurs::~ArenaOurs() {
  if (worker_.joinable()) worker_.join();
  renderer_.WaitIdle();
  DropAll();
}

void ArenaOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  std::lock_guard<std::mutex> lock(mu_);
  record_ = std::move(record);
}

void ArenaOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  root_ = root;
}

void ArenaOurs::DropAll() {
  ++epoch_;  // lane 122 (thin draws): the templates that name these buffers and images are void
  for (MeshGpu& m : meshes_) {
    if (m.vertex != rdr::kNoBuffer) renderer_.DestroyBuffer(m.vertex);
    if (m.index != rdr::kNoBuffer) renderer_.DestroyBuffer(m.index);
  }
  meshes_.clear();
  for (rdr::ImageId id : images_)
    if (id != rdr::kNoImage) renderer_.DestroyImage(id);
  images_.clear();
  stage_.reset();
  core_.SetArena(nullptr, -1);
  uploaded_ = false;
}

rdr::ImageId ArenaOurs::MakeTexture(const assets::Texture& t) {
  if (t.width == 0 || t.height == 0 || t.format == assets::TexFormat::kUnknown) return rdr::kNoImage;
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if ((bc && ((t.width % 4) || (t.height % 4))) || t.width > 16384 || t.height > 16384) return rdr::kNoImage;
  uint32_t mips = std::max(1u, t.mips), mx = std::max(t.width, t.height), maxmips = 1;
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
        ArenaCore::B4G4R4A4ToRGBA8(px, &wide[4 * k]);
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
void ArenaOurs::Announce(int arena) {
  if (load_state_.load() == 2 && loaded_.arena != arena) {   // a finished early load for another arena (the player backed out after the card) must not block this one
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (arena < 0 || uploaded_ || arena_ >= 0 || load_state_.load() != 0 || load_for_ == arena) return;
  StartLoad(arena);
}

void ArenaOurs::StartLoad(int arena) {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mu_);
    root = root_;
  }
  if (root.empty()) {
    if (!said_no_root_) said_no_root_ = true, rdr::Log("[rdr][arena] no game folder to load the arena's meshes from: the stream's arena stays");
    return;
  }
  if (worker_.joinable()) worker_.join();
  load_state_ = 1;
  load_for_ = arena;
  const bool viewer_space = options_.faults.viewer_space;
  worker_ = std::thread([this, arena, root, viewer_space] {
    Loaded loaded;
    loaded.arena = arena;
    if (!assets::arena_gs::Init(root)) {
      loaded.error = "no game files at " + root;
    } else {
      auto stage = std::make_shared<assets::arena_gs::StageArena>();
      if (assets::arena_gs::Load(arena, *stage, loaded.error, viewer_space)) loaded.stage = std::move(stage);
    }
    loaded_ = std::move(loaded);
    load_state_ = 2;
  });
}

bool ArenaOurs::Upload(Loaded& loaded) {
  DropAll();
  if (!loaded.stage) {
    rdr::Log("[rdr][arena] arena %d: could not be loaded (%s): the stream's arena stays", loaded.arena, loaded.error.c_str());
    return false;
  }
  const auto upload_t0 = std::chrono::steady_clock::now();   // lane 271: the upload runs inside one FrameReady on the live thread; its length is logged
  const assets::arena_gs::StageArena& s = *loaded.stage;
  images_.resize(s.textures.size(), rdr::kNoImage);
  for (size_t i = 0; i < s.textures.size(); ++i) images_[i] = MakeTexture(s.textures[i]);
  meshes_.resize(s.meshes.size());
  std::vector<assets::arena_gs::Vertex> vertices;
  std::vector<assets::arena_gs::SkinVertex> skin_vertices;
  std::vector<uint32_t> indices;
  for (size_t i = 0; i < s.meshes.size(); ++i) {
    const assets::arena_gs::StageMesh& m = s.meshes[i];
    core_.FillVertices(m, vertices);
    core_.FillIndices(m, indices);
    MeshGpu& g = meshes_[i];
    if (core_.FillSkinVertices(m, skin_vertices)) {  // S101 lane 49: a mesh with skin pairs takes the 44-byte vertex (any variant reads its first 36 bytes the same way)
      g.wide = true;
      g.vertex = renderer_.CreateBuffer(skin_vertices.size() * sizeof(assets::arena_gs::SkinVertex), skin_vertices.data());
    } else {
      g.vertex = renderer_.CreateBuffer(vertices.size() * sizeof(assets::arena_gs::Vertex), vertices.data());
    }
    g.index = renderer_.CreateBuffer(indices.size() * 4, indices.data());
    g.index_count = uint32_t(indices.size());
  }
  stage_ = loaded.stage;
  core_.SetArena(stage_, loaded.arena);
  size_t made = 0;
  for (rdr::ImageId id : images_) made += id != rdr::kNoImage;
  rdr::Log("[rdr][arena] arena %d: %zu stage meshes (%.1f MB), %zu of %zu textures uploaded in %.1f ms", loaded.arena, s.meshes.size(), double(s.Bytes()) / 1048576.0, made, s.textures.size(),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - upload_t0).count());
  loaded = Loaded{};
  uploaded_ = true;
  return true;
}

// ---- the frame --------------------------------------------------------------------------------------------------------------------

void ArenaOurs::NoClaimFrame() {
  {  // key the release on the record, not on draws (the crowd's review F3): a close-up with every stage draw culled still has the arena in the record
    std::lock_guard<std::mutex> lock(mu_);
    if (record_ && ArenaCore::ArenaOfRecord(*record_) >= 0) {
      idle_frames_ = 0;
      return;
    }
  }
  if (idle_frames_ + 1 == 12 * kIdleDrop && load_state_.load() == 2 && !uploaded_ && arena_ < 0) {   // lane 271: an early load nobody claimed (the player left after the card) gives its memory back
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
    load_for_ = -1;
    rdr::Log("[rdr][arena] an early load was never claimed: released");
  }
  if (++idle_frames_ == kIdleDrop && (uploaded_ || arena_ >= 0)) {
    DropAll();
    arena_ = -1;
    load_for_ = -1;
    rdr::Log("[rdr][arena] no arena for %u frames: the meshes are released", kIdleDrop);
  }
}

bool ArenaOurs::FrameReady(const rdr_adapter::LoadedFrame&) {
  idle_frames_ = 0;
  std::shared_ptr<const SceneTables> record;
  {
    std::lock_guard<std::mutex> lock(mu_);
    record = record_;
  }
  core_.ResetFrame();
  if (++frames_seen_ % 600 == 0) {  // live: the hook's own census with the check counters and the first failing check of the window (the binder's line has only the reasons)
    const std::string fail = core_.TakeFirstFail();
    rdr::Log("%s%s%s", Report().c_str(), fail.empty() ? "" : "; first failed check since the last line: ", fail.c_str());
  }
  core_.SetRecord(record);
  if (!record) return ++not_ready_frames_, false;
  const int arena = ArenaCore::ArenaOfRecord(*record);
  if (arena < 0) return ++not_ready_frames_, false;
  if (arena != arena_) {
    DropAll();
    arena_ = arena;
  }
  if (!uploaded_) {
    const int state = load_state_.load();
    if (state == 2) {
      if (worker_.joinable()) worker_.join();
      if (loaded_.arena == arena_) Upload(loaded_);
      else loaded_ = Loaded{};
      load_state_ = 0;  // (a failed upload is not retried every frame: load_for_ still names this arena)
    } else if (state == 1 && load_for_ != arena_) {
      static int logged_for = -2;
      if (logged_for != arena_) {
        logged_for = arena_;
        rdr::Log("[rdr][arena] early load for arena %d is being replaced by %d", load_for_, arena_);
      }
    } else if (state == 0 && load_for_ != arena_) {
      StartLoad(arena_);
    }
  }
  if (!uploaded_ && options_.verify_content && load_state_.load() == 1) {  // offline (fview, the checks): wait for the worker, so every frame of a capture is judged
    if (worker_.joinable()) worker_.join();
    if (loaded_.arena == arena_) Upload(loaded_);
    else loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (!uploaded_) return ++not_ready_frames_, false;
  ++ours_frames_;
  return true;
}

uint16_t ArenaOurs::LayoutFor(int vs, bool wide) {
  const int key = vs * 2 + (wide ? 1 : 0);
  const auto found = layouts_.find(key);
  if (found != layouts_.end()) return found->second;
  uint16_t id = 0xFFFF;
  rdr::VertexLayoutDesc desc;
  if (ArenaCore::BuildLayout(vs, desc, wide)) id = renderer_.AddVertexLayout(desc);
  layouts_.emplace(key, id);
  return id;
}

bool ArenaOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  if (!uploaded_ || in.vs < 0 || size_t(in.vs) >= std::size(kVariants)) {
    why = KeepWhy::NotReady;
    return false;
  }
  const rdr::fx::Variant& vs = kVariants[in.vs];
  const rdr::fx::Variant* ps = in.ps >= 0 && size_t(in.ps) < std::size(kVariants) ? &kVariants[in.ps] : nullptr;
  uint8_t slots[rdr::kMaxTextures];
  uint32_t slot_count = 0;
  if (ps)
    for (uint32_t s = 0; s < ps->sampler_count && slot_count < rdr::kMaxTextures; ++s) slots[slot_count++] = ps->samplers[s].slot;
  float world[12];
  bool have_world = false;
  for (uint32_t r = 0; r < vs.constant_count && in.vs_block; ++r) {
    const rdr::fx::ConstantRange& range = vs.constants[r];
    if (range.reg == 76 && range.count >= 3 && uint32_t(range.offset) + 48 <= in.vs_block_bytes) {
      std::memcpy(world, in.vs_block + range.offset, sizeof world);
      have_world = true;
      break;
    }
  }
  const ArenaCore::Decision d = core_.Decide(in, vs.name, ps ? ps->name : "", slots, slot_count, have_world ? world : nullptr);
  if (!d.ok) {
    why = d.why;
    return false;
  }
  const uint16_t layout = LayoutFor(in.vs, meshes_[size_t(d.mesh)].wide);
  if (layout == 0xFFFF) {
    if (!said_variant_) said_variant_ = true, rdr::Log("[rdr][arena] a vertex program (%s) reads an input our arena vertex lacks: those draws stay the stream's", vs.name);
    why = KeepWhy::Variant;
    return false;
  }
  const MeshGpu& g = meshes_[size_t(d.mesh)];
  out = rdr_adapter::ReplaceOut{};
  out.vertex = {g.vertex, 0};
  out.index = {g.index, 0};
  out.index_count = g.index_count;
  out.index_32bit = 1;
  out.vertex_layout = layout;
  out.topology = 3;
  const auto put = [&](int slot, int tex) {
    if (tex < 0 || size_t(tex) >= images_.size() || images_[size_t(tex)] == rdr::kNoImage) return core_.strict_texture() ? false : (core_.NotePutKept(), true);   // lane 142: the slot keeps the stream's image
    if (!in.textures || in.textures->slots[slot].image == rdr::kNoImage) return core_.strict_texture() ? false : (core_.NotePutKept(), true);  // the contract: the stream draw must bind an image there (its sampler stays); lane 142: none bound = nothing to replace, the slot stays as it is
    out.slots[slot].set = true;
    out.slots[slot].image = images_[size_t(tex)];
    return true;
  };
  bool ok = true;
  if (d.diffuse >= 0) ok = put(0, d.diffuse) && ok;
  if (d.normal_map >= 0) ok = put(4, d.normal_map) && ok;
  if (d.specular >= 0) ok = put(5, d.specular) && ok;
  if (d.file_lightmap >= 0) put(13, d.file_lightmap);  // (planted fault only)
  if (!ok) {  // a texture the file has but the GPU could not take (an odd format): the whole draw stays the stream's
    out = rdr_adapter::ReplaceOut{};
    why = KeepWhy::Texture;
    return false;
  }
  return true;
}

std::string ArenaOurs::Report() const {
  char head[160];
  std::snprintf(head, sizeof head, "[rdr][arena] ours frames %llu, not ready %llu, arena %d; ", (unsigned long long)ours_frames_, (unsigned long long)not_ready_frames_, arena_);
  return std::string(head) + core_.Report();
}

}  // namespace rdr_scene
