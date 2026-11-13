// Our crowd camera flashes drawn in place. See flash_ours.h. Session 101, lane 68.
#include "flash_ours.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../renderer/problem.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "arena_ours_core.h"   // ArenaCore::ArenaOfRecord (the record's arena, the one rule the arena hook uses)
#include "fx_tables.h"

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;
using rdr_adapter::KeepWhy;

constexpr uint16_t kRGBA32F = 109, kRGB32F = 106, kRG32F = 103, kR32UInt = 98, kRGBA8Unorm = 37, kHalf2 = 83;   // VkFormat numbers of the stream's elements / ours
constexpr float kPosTolerance = 1e-3f;
constexpr float kUvTolerance = 2e-3f;
constexpr uint32_t kIdleDrop = 300;   // frames with no arena in the record before the assets go (the load screen)
constexpr int kDepthSlot = 17;        // the vertex stage's scene-depth copy (yFlashDefault textures[1])

const fxt::VariantTable* TableOf(int variant) {
  static std::vector<const fxt::VariantTable*> by_index;
  if (by_index.empty()) {
    by_index.assign(std::size(kVariants), nullptr);
    for (size_t i = 0; i < std::size(kVariants); ++i) {
      if (!kVariants[i].name) continue;
      for (const fxt::VariantTable& t : fxt::kTables)
        if (std::strcmp(t.variant, kVariants[i].name) == 0) {
          by_index[i] = &t;
          break;
        }
    }
  }
  return variant >= 0 && size_t(variant) < by_index.size() ? by_index[size_t(variant)] : nullptr;
}

size_t MipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

// float <- IEEE half bits
float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
  float v;
  if (e == 0) v = std::ldexp(float(m), -24);
  else if (e == 31) v = m ? std::nanf("") : INFINITY;
  else v = std::ldexp(float(m | 1024u), int(e) - 25);
  return sign ? -v : v;
}

// The stream draw's vertex element of a vk format (colour 37, uv 83): its host offset, or -1.
int ElementOffset(const rdr_adapter::ReplaceIn& in, uint16_t vk, uint32_t& stride) {
  if (!in.frame || !in.draw || in.draw->layout == 0xFFFFFFFFu || in.draw->layout >= in.frame->layouts.count) return -1;
  const rdr_adapter::VertexLayout& lay = in.frame->layouts[in.draw->layout];
  stride = lay.host_stride;
  for (uint32_t k = 0; k < lay.element_count && lay.element_first + k < in.frame->elements.count; ++k) {
    const rdr_adapter::VertexElement& e = in.frame->elements[lay.element_first + k];
    if (e.vk_format == vk) return int(e.host_offset);
  }
  return -1;
}

}  // namespace

FlashOurs::FlashOurs(rdr::Renderer& renderer, const FlashOursOptions& options) : renderer_(renderer), options_(options), root_(options.game_root) {}

FlashOurs::~FlashOurs() {
  if (worker_.joinable()) worker_.join();
  renderer_.WaitIdle();
  DropAll();
}

void FlashOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  std::lock_guard<std::mutex> lock(mu_);
  record_ = std::move(record);
}

void FlashOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  root_ = root;
}

void FlashOurs::DropAll() {
  if (vertex_ != rdr::kNoBuffer) renderer_.DestroyBuffer(vertex_);
  if (index_ != rdr::kNoBuffer) renderer_.DestroyBuffer(index_);
  for (auto& kv : images_)
    if (kv.second != rdr::kNoImage) renderer_.DestroyImage(kv.second);
  images_.clear();
  if (no_depth_image_ != rdr::kNoImage) renderer_.DestroyImage(no_depth_image_);
  vertex_ = rdr::kNoBuffer, index_ = rdr::kNoBuffer, no_depth_image_ = rdr::kNoImage;
  index_count_ = 0;
  mesh_.reset();
  verified_key_.clear();
  objects_.clear();
  uploaded_ = false;
}

// ---- loading ----------------------------------------------------------------------------------------------------------------------

// S101 lane 271 (SKIP_GAME_WORK_MAP 19.8): the in-game sequence's match card names the arena ~1.5 s before the first record that draws it. Starting the load then (on its worker,
// as FrameReady would) has the meshes ready when that record arrives instead of 0.1-8 s after it. A load already running or done, a record that already named an arena, or the
// same arena again does nothing; a wrong arena is a wasted load that FrameReady discards (loaded_.arena != arena_) and replaces.
void FlashOurs::Announce(int arena) {
  if (load_state_.load() == 2 && loaded_.arena != arena) {   // a finished early load for another arena (the player backed out after the card) must not block this one
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (arena < 0 || uploaded_ || arena_ >= 0 || load_state_.load() != 0 || load_for_ == arena) return;
  StartLoad(arena);
}

void FlashOurs::StartLoad(int arena) {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mu_);
    root = root_;
  }
  if (root.empty()) {
    if (!said_no_root_) said_no_root_ = true, rdr::Log("[rdr][flash] no game folder to load the flash model from: the stream's flashes stay");
    return;
  }
  if (worker_.joinable()) worker_.join();
  load_state_ = 1;
  load_for_ = arena;
  // The planted faults read another arena's pac than the record names.
  const int file_arena = options_.faults.first_arena ? 0 : options_.faults.wrong_arena ? 28 : arena;
  worker_ = std::thread([this, arena, file_arena, root] {
    Loaded loaded;
    loaded.arena = arena;
    if (!assets::flash_gs::Init(root)) {
      loaded.error = "no game files at " + root;
    } else {
      auto mesh = std::make_shared<assets::flash_gs::FlashMesh>();
      if (assets::flash_gs::Load(file_arena, *mesh, loaded.error)) loaded.mesh = std::move(mesh);
    }
    loaded_ = std::move(loaded);
    load_state_ = 2;
  });
}

// The model's texture as a sampled image with `levels` mip levels at most. The game's image has only the levels its fetch constant allows (the reader makes mips = max_level + 1,
// reader.h:761; the flash's constant says max_level 2, so 3 of the file's 7 levels): the file's smaller levels would be sampled where the game's clamps, so the count comes from the
// stream slot (ts->max_level + 1), one image per count, made on first use.
rdr::ImageId FlashOurs::ImageFor(uint32_t levels) {
  if (!mesh_ || !mesh_->has_diffuse) return rdr::kNoImage;
  const auto found = images_.find(levels);
  if (found != images_.end()) return found->second;
  rdr::ImageId id = rdr::kNoImage;
  const assets::Texture& t = mesh_->diffuse;
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if (t.width && t.height && t.format != assets::TexFormat::kUnknown && !(bc && ((t.width % 4) || (t.height % 4))) && t.width <= 16384 && t.height <= 16384) {
    uint32_t mips = std::min(std::max(1u, t.mips), std::max(1u, levels)), mx = std::max(t.width, t.height), maxmips = 1;
    while (mx > 1) mx >>= 1, ++maxmips;
    mips = std::min(mips, maxmips);
    size_t total = 0;
    uint32_t fit = 0;
    for (uint32_t i = 0; i < mips; ++i) {
      const size_t sz = MipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
      if (total + sz > t.data.size()) break;
      total += sz, ++fit;
    }
    if (fit) {
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
      id = renderer_.CreateImage(d);
      if (id != rdr::kNoImage) {
        size_t src = 0;
        std::vector<uint8_t> wide;
        for (uint32_t i = 0; i < fit; ++i) {
          const uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
          const size_t sb = MipBytes(t.format, mw, mh);
          if (t.format == assets::TexFormat::kB4G4R4A4) {   // (16-bit little endian: A bits 12-15, R 8-11, G 4-7, B 0-3 -> RGBA8)
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
      }
    }
  }
  images_.emplace(levels, id);
  return id;
}

bool FlashOurs::Upload(Loaded& loaded) {
  DropAll();
  if (!loaded.mesh) {
    rdr::Log("[rdr][flash] arena %d: could not be loaded (%s): the stream's flashes stay", loaded.arena, loaded.error.c_str());
    return false;
  }
  const assets::flash_gs::FlashMesh& m = *loaded.mesh;
  std::vector<FlashVertex> verts(m.vertices.size());
  for (size_t i = 0; i < verts.size(); ++i) {
    const assets::arena_gs::Vertex& s = m.vertices[i];
    FlashVertex& d = verts[i];
    std::memcpy(d.pos, s.pos, 12);
    d.bone_id = 0u;
    std::memcpy(d.normal, s.normal, 12);
    std::memcpy(d.color, s.color, 4);
    std::memcpy(d.uv, s.uv, 8);
  }
  vertex_ = renderer_.CreateBuffer(verts.size() * sizeof(FlashVertex), verts.data());
  index_ = renderer_.CreateBuffer(m.indices.size() * 4, m.indices.data());
  index_count_ = uint32_t(m.indices.size());
  if (vertex_ == rdr::kNoBuffer || index_ == rdr::kNoBuffer) {   // review F4: a missing buffer would make a non-indexed draw read past a 20-vertex one
    rdr::Log("[rdr][flash] arena %d: a buffer could not be made: the stream's flashes stay", loaded.arena);
    DropAll();
    return false;
  }
  if (options_.faults.no_depth) {   // the planted fault's image: depth 0 everywhere = "nothing nearer than the flash"
    rdr::ImageDesc d;
    d.width = 1, d.height = 1, d.mips = 1;
    d.usage = rdr::kUsageSampled;
    d.format = rdr::Format::RGBA8;
    no_depth_image_ = renderer_.CreateImage(d);
    const uint8_t zero[4] = {0, 0, 0, 0};
    if (no_depth_image_ != rdr::kNoImage) renderer_.UpdateImage(no_depth_image_, 0, 0, zero, 4);
  }
  mesh_ = loaded.mesh;
  rdr::Log("[rdr][flash] arena %d: the flash model (%zu vertices, %zu indices, %.1f KB), texture %s", loaded.arena, m.vertices.size(), m.indices.size(), double(m.Bytes()) / 1024.0,
           m.has_diffuse ? "held (images made on first use)" : "missing");
  loaded = Loaded{};
  uploaded_ = true;
  return true;
}

// ---- the frame --------------------------------------------------------------------------------------------------------------------

void FlashOurs::NoClaimFrame() {
  {   // key the release on the record, not on draws: a frame with no flash still has the arena in the record
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
    rdr::Log("[rdr][flash] an early load was never claimed: released");
  }
  if (++idle_frames_ == kIdleDrop && (uploaded_ || arena_ >= 0)) {
    DropAll();
    arena_ = -1;
    load_for_ = -1;
    rdr::Log("[rdr][flash] no arena for %u frames: the flash model is released", kIdleDrop);
  }
}

bool FlashOurs::FrameReady(const rdr_adapter::LoadedFrame&) {
  idle_frames_ = 0;
  std::shared_ptr<const SceneTables> record;
  {
    std::lock_guard<std::mutex> lock(mu_);
    record = record_;
  }
  if (++frames_seen_ % 600 == 0) {
    rdr::Log("%s%s%s", Report().c_str(), first_fail_.empty() ? "" : "; first failed check since the last line: ", first_fail_.c_str());
    first_fail_.clear();
  }
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
      load_state_ = 0;   // (a failed upload is not retried every frame: load_for_ still names this arena)
    } else if (state == 1 && load_for_ != arena_) {
      static int logged_for = -2;
      if (logged_for != arena_) {
        logged_for = arena_;
        rdr::Log("[rdr][flash] early load for arena %d is being replaced by %d", load_for_, arena_);
      }
    } else if (state == 0 && load_for_ != arena_) {
      StartLoad(arena_);
    }
  }
  if (!uploaded_ && options_.verify_content && load_state_.load() == 1) {   // offline: wait for the worker, so every frame of a capture is judged
    if (worker_.joinable()) worker_.join();
    if (loaded_.arena == arena_) Upload(loaded_);
    else loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (!uploaded_) return ++not_ready_frames_, false;
  ++ours_frames_;
  return true;
}

bool FlashOurs::BuildLayout(int vs, rdr::VertexLayoutDesc& desc) {
  desc = rdr::VertexLayoutDesc{};
  const fxt::VariantTable* t = TableOf(vs);
  if (!t || t->input_count == 0) return false;
  desc.strides[0] = uint32_t(sizeof(FlashVertex));
  for (uint32_t k = 0; k < t->input_count; ++k) {
    if (desc.attribute_count >= rdr::kMaxVertexAttributes) return false;
    const std::string name = t->inputs[k].name;
    rdr::VertexAttribute a{};
    a.location = t->inputs[k].location;
    if (name == "inPosition" && t->inputs[k].components >= 3) a.format = kRGB32F, a.offset = uint32_t(offsetof(FlashVertex, pos));
    else if (name == "inBoneId" && t->inputs[k].components == 1) a.format = kR32UInt, a.offset = uint32_t(offsetof(FlashVertex, bone_id));
    else if (name == "inNormal" && t->inputs[k].components == 3) a.format = kRGB32F, a.offset = uint32_t(offsetof(FlashVertex, normal));
    else if (name == "inColor" && t->inputs[k].components == 4) a.format = kRGBA8Unorm, a.offset = uint32_t(offsetof(FlashVertex, color));
    else if (name == "inUv" && t->inputs[k].components == 2) a.format = kRG32F, a.offset = uint32_t(offsetof(FlashVertex, uv));
    else return false;   // inWeights / inBoneIds (the skinned flash variants): our model has no skin words
    desc.attributes[desc.attribute_count++] = a;
  }
  return true;
}

uint16_t FlashOurs::LayoutFor(int vs) {
  const auto found = layouts_.find(vs);
  if (found != layouts_.end()) return found->second;
  uint16_t id = 0xFFFF;
  rdr::VertexLayoutDesc desc;
  if (BuildLayout(vs, desc)) id = renderer_.AddVertexLayout(desc);
  layouts_.emplace(vs, id);
  return id;
}

bool FlashOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  ++stats_.decisions;
  const auto keep = [&](KeepWhy w) {
    why = w;
    ++stats_.kept[uint8_t(w)];
    return false;
  };
  const auto fail = [&](uint64_t& counter, const char* what) {
    ++counter;
    if (first_fail_.empty()) {
      char b[200];
      std::snprintf(b, sizeof b, "%s (object 0x%08X part %u, stream %u vertices %u indices)", what, in.origin ? in.origin->object : 0u, in.origin ? in.origin->part : 0u, in.vertex_count, in.index_count);
      first_fail_ = b;
    }
    return keep(KeepWhy::Mismatch);
  };
  if (!uploaded_ || !mesh_ || in.vs < 0 || size_t(in.vs) >= std::size(kVariants)) return keep(KeepWhy::NotReady);
  const rdr_adapter::DrawOrigin* o = in.origin;
  if (!o || o->klass != rdr_adapter::DrawClass::ModelPart || o->pass_site != kFlashPassSite) return keep(KeepWhy::NotOurs);
  // The game names the object's file when its loader does ("pac:entry[:mesh]", names.h): an entry other than 31000 is not the flash.
  if (in.frame && o->asset) {
    const char* asset = in.frame->String(o->asset);
    int entry = -1;
    if (asset && *asset && std::sscanf(asset, "%*[^:]:%d", &entry) == 1 && entry != 31000) {
      ++stats_.bad_asset;
      return keep(KeepWhy::NotOurs);
    }
  }
  if (objects_.emplace(o->object, true).second) ++stats_.objects;
  const assets::flash_gs::FlashMesh& m = *mesh_;
  if (o->part != 0) return fail(stats_.bad_part, "part");
  if (in.vertex_count != m.vertices.size()) return fail(stats_.bad_vertices, "vertices");
  if (in.index_count != ExpectedIndexCount(m, options_.faults.count_off)) return fail(stats_.bad_indices, "indices");
  const uint16_t layout = LayoutFor(in.vs);
  if (layout == 0xFFFF) {
    if (!said_variant_) said_variant_ = true, rdr::Log("[rdr][flash] a vertex program (%s) reads an input our flash vertex lacks: those draws stay the stream's", kVariants[in.vs].name);
    return keep(KeepWhy::Variant);
  }
  // ---- the stream's vertices against the file's ----
  uint32_t stride = 0;
  const int pos_off = ElementOffset(in, kRGBA32F, stride);
  if (pos_off < 0 || stride == 0 || !in.read_geometry) return keep(KeepWhy::Unjoined);   // (not the 36-byte float4 layout, or the bytes are not readable)
  const bool full = options_.verify_content;
  const auto key_it = verified_key_.find(o->object);
  if (full || key_it == verified_key_.end() || key_it->second != in.vertex_offset) {
    std::vector<uint8_t> bytes;
    if (!in.read_geometry(in.vertex_offset, in.vertex_count * stride, bytes) || bytes.size() < size_t(in.vertex_count) * stride) return keep(KeepWhy::Unjoined);
    const size_t first = 0, last = m.vertices.size() - 1;
    const auto check = [&](size_t i) {
      float p[3];
      std::memcpy(p, bytes.data() + i * stride + size_t(pos_off), 12);
      for (int c = 0; c < 3; ++c)
        if (!(std::fabs(p[c] - m.vertices[i].pos[c]) <= kPosTolerance)) return false;
      {
        uint32_t s3 = 0;
        const int bo = ElementOffset(in, 41, s3);   // review F5: the stream's bone byte (vk 41, uint) must be 0 (ours is bone 0)
        if (bo >= 0 && bytes[i * stride + size_t(bo)] != 0) return false;
      }
      if (!full) return true;
      uint32_t s2 = 0;
      const int co = ElementOffset(in, kRGBA8Unorm, s2), uo = ElementOffset(in, kHalf2, s2);
      if (co >= 0 && std::memcmp(bytes.data() + i * stride + size_t(co), m.vertices[i].color, 4) != 0) return false;
      if (uo >= 0) {
        uint16_t h[2];
        std::memcpy(h, bytes.data() + i * stride + size_t(uo), 4);
        if (std::fabs(HalfToFloat(h[0]) - std::fabs(m.vertices[i].uv[0])) > kUvTolerance || std::fabs(HalfToFloat(h[1]) - m.vertices[i].uv[1]) > kUvTolerance) return false;   // (the stream's u has lost its sign: arena_ours.h uv_abs_u)
      }
      return true;
    };
    bool ok = full ? true : (check(first) && check(last));
    if (full)
      for (size_t i = 0; i < m.vertices.size() && ok; ++i) ok = check(i);
    if (!ok) {
      verified_key_.erase(o->object);
      return fail(stats_.bad_content, full ? "content" : "first/last vertex");
    }
    verified_key_[o->object] = in.vertex_offset;
    ++(full ? stats_.verified_full : stats_.verified_ends);
  }
  // ---- slot 0: the model's texture, when the stream's image there is plain and the file's size ----
  rdr::ImageId slot0_image = rdr::kNoImage;
  if (m.has_diffuse && in.textures && in.textures->slots[0].image != rdr::kNoImage && in.frame && in.draw) {
    const rdr_adapter::TextureSlot* ts = nullptr;
    for (uint32_t t = 0; t < in.draw->texture_count && in.draw->texture_first + t < in.frame->texture_slots.count; ++t)
      if (in.frame->texture_slots[in.draw->texture_first + t].slot == 0) ts = &in.frame->texture_slots[in.draw->texture_first + t];
    if (ts && ts->image != 0) {
      const bool plain = (!ts->swizzle[0] && !ts->swizzle[1] && !ts->swizzle[2] && !ts->swizzle[3]) || (ts->swizzle[0] == 3 && ts->swizzle[1] == 4 && ts->swizzle[2] == 5 && ts->swizzle[3] == 6);
      uint32_t sw = 0, sh = 0;
      if (ts->min_level != 0) {   // coordinator note (lane 70): a fetch that starts above level 0 keeps the stream's image
        ++stats_.bad_view;
        return keep(KeepWhy::Texture);
      }
      if (ts->view_format != 0 || !plain) {
        ++stats_.bad_view;
        return keep(KeepWhy::Texture);
      }
      if (options_.image_info && options_.image_info(ts->image, sw, sh) && sw && sh) {
        if (m.diffuse.width != sw || m.diffuse.height != sh) {
          ++stats_.bad_size;
          return keep(KeepWhy::Texture);
        }
        uint32_t levels = uint32_t(ts->max_level) + 1u;   // review F3: min(the stream image's mips, max_level + 1)
        rdr_adapter::ImageContent ic;
        if (options_.image_content && options_.image_content(ts->image, ic) && ic.mips) levels = std::min(levels, ic.mips);
        slot0_image = ImageFor(options_.faults.all_mips ? 16u : levels);   // (the levels the game's image holds)
        if (slot0_image == rdr::kNoImage) {
          ++stats_.bad_view;
          return keep(KeepWhy::Texture);
        }
      } else {
        ++stats_.size_unchecked;   // the owner cannot tell the size: the stream's image stays in slot 0 (the geometry is still ours)
      }
    } else {
      ++stats_.no_slot0;
    }
  } else {
    ++stats_.no_slot0;
  }
  out = rdr_adapter::ReplaceOut{};
  out.vertex = {vertex_, 0};
  out.index = {index_, 0};
  out.index_count = index_count_;
  out.index_32bit = 1;
  out.vertex_layout = layout;
  out.topology = 3;   // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
  if (slot0_image != rdr::kNoImage) {
    out.slots[0].set = true;
    out.slots[0].image = slot0_image;
  }
  if (options_.faults.no_depth && no_depth_image_ != rdr::kNoImage) {   // R: the scene-depth copy replaced: flashes are never hidden
    out.slots[kDepthSlot].set = true;
    out.slots[kDepthSlot].image = no_depth_image_;
  }
  ++stats_.replaced;
  return true;
}

std::string FlashOurs::Report() const {
  char b[420];
  std::snprintf(b, sizeof b,
                "[rdr][flash] ours frames %llu, not ready %llu, arena %d; decisions %llu, replaced %llu, objects %llu, mismatch: vertices %llu indices %llu part %llu content %llu; texture: size %llu view %llu, size unchecked %llu, stream slot 0 kept %llu; verified full %llu ends %llu",
                (unsigned long long)ours_frames_, (unsigned long long)not_ready_frames_, arena_, (unsigned long long)stats_.decisions, (unsigned long long)stats_.replaced, (unsigned long long)stats_.objects,
                (unsigned long long)stats_.bad_vertices, (unsigned long long)stats_.bad_indices, (unsigned long long)stats_.bad_part, (unsigned long long)stats_.bad_content,
                (unsigned long long)stats_.bad_size, (unsigned long long)stats_.bad_view, (unsigned long long)stats_.size_unchecked, (unsigned long long)stats_.no_slot0,
                (unsigned long long)stats_.verified_full, (unsigned long long)stats_.verified_ends);
  return b;
}

}  // namespace rdr_scene
