// The crowd's sign / foam-hand draws drawn by us, in place. See crowd_props_ours.h and crowd_props_core.h. S101 lane 133 (a copy of flash_ours.cpp's renderer side).
#include "crowd_props_ours.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "../renderer/problem.h"
#include "arena_ours_core.h"   // ArenaCore::ArenaOfRecord (the record's arena, the one rule the arena hook uses)

namespace rdr_scene {
namespace {

using rdr_adapter::KeepWhy;

constexpr uint32_t kIdleDrop = 300;      // frames with no arena in the record before the assets go (the load screen)
constexpr int kLightMapSlotFragment = 13, kLightMapSlotVertex = 16;   // the page the crowd programs read: yAudDefault_lm_pixel (13), yAudience_lit_lm_vertex (16) [C: slot lists of the captures]

}  // namespace

CrowdPropsOurs::CrowdPropsOurs(rdr::Renderer& renderer, const CrowdPropsOptions& options) : renderer_(renderer), options_(options), core_(options.faults), root_(options.game_root) {}

CrowdPropsOurs::~CrowdPropsOurs() {
  if (worker_.joinable()) worker_.join();
  renderer_.WaitIdle();
  DropAll();
}

void CrowdPropsOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  std::lock_guard<std::mutex> lock(mu_);
  record_ = std::move(record);
}

void CrowdPropsOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  root_ = root;
}

void CrowdPropsOurs::ReleaseBuffers() {
  for (rdr::BufferId& b : vertex_buffers_)
    if (b != rdr::kNoBuffer) renderer_.DestroyBuffer(b), b = rdr::kNoBuffer;
  for (rdr::BufferId& b : index_buffers_)
    if (b != rdr::kNoBuffer) renderer_.DestroyBuffer(b), b = rdr::kNoBuffer;
  if (stale_image_ != rdr::kNoImage) renderer_.DestroyImage(stale_image_), stale_image_ = rdr::kNoImage;
}

void CrowdPropsOurs::DropAll() {
  ReleaseBuffers();
  vertex_buffers_.clear(), index_buffers_.clear();
  core_.SetProps(nullptr);
  props_.reset();
  uploaded_ = false;
}

// ---- loading ----------------------------------------------------------------------------------------------------------------------

// S101 lane 271 (SKIP_GAME_WORK_MAP 19.8): the in-game sequence's match card names the arena ~1.5 s before the first record that draws it. Starting the load then (on its worker,
// as FrameReady would) has the meshes ready when that record arrives instead of 0.1-8 s after it. A load already running or done, a record that already named an arena, or the
// same arena again does nothing; a wrong arena is a wasted load that FrameReady discards (loaded_.arena != arena_) and replaces.
void CrowdPropsOurs::Announce(int arena) {
  if (load_state_.load() == 2 && loaded_.arena != arena) {   // a finished early load for another arena (the player backed out after the card) must not block this one
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (arena < 0 || uploaded_ || arena_ >= 0 || load_state_.load() != 0 || load_for_ == arena) return;
  StartLoad(arena);
}

void CrowdPropsOurs::StartLoad(int arena) {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mu_);
    root = root_;
  }
  if (root.empty()) {
    if (!said_no_root_) said_no_root_ = true, rdr::Log("[rdr][crowd_props] no game folder to load the crowd props from: the stream's props stay");
    return;
  }
  if (worker_.joinable()) worker_.join();
  load_state_ = 1;
  load_for_ = arena;
  worker_ = std::thread([this, arena, root] {
    Loaded loaded;
    loaded.arena = arena;
    if (!assets::crowd_prop_gs::Init(root)) {
      loaded.error = "no game files at " + root;
    } else {
      auto set = std::make_shared<assets::crowd_prop_gs::PropSet>();
      if (assets::crowd_prop_gs::Load(arena, *set, loaded.error)) loaded.props = std::move(set);
    }
    loaded_ = std::move(loaded);
    load_state_ = 2;
  });
}

bool CrowdPropsOurs::Upload(Loaded& loaded) {
  DropAll();
  if (!loaded.props) {
    rdr::Log("[rdr][crowd_props] arena %d: could not be loaded (%s): the stream's props stay", loaded.arena, loaded.error.c_str());
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();
  props_ = loaded.props;
  core_.SetProps(props_);
  vertex_buffers_.assign(props_->meshes.size(), rdr::kNoBuffer);   // (buffers are made at first use)
  index_buffers_.assign(props_->meshes.size(), rdr::kNoBuffer);
  upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ++loads_;
  rdr::Log("[rdr][crowd_props] arena %d: %zu prop meshes from the audience set (%.1f KB; buffers are made at first use)", loaded.arena, props_->meshes.size(), double(props_->Bytes()) / 1024.0);
  loaded = Loaded{};
  uploaded_ = true;
  return true;
}

// ---- the frame --------------------------------------------------------------------------------------------------------------------

void CrowdPropsOurs::NoClaimFrame() {
  {   // key the release on the record, not on draws: a frame with no prop draw still has the arena in the record
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
    rdr::Log("[rdr][crowd_props] an early load was never claimed: released");
  }
  if (++idle_frames_ == kIdleDrop && (uploaded_ || arena_ >= 0)) {
    DropAll();
    arena_ = -1;
    load_for_ = -1;
    rdr::Log("[rdr][crowd_props] no arena for %u frames: the crowd props are released", kIdleDrop);
  }
}

bool CrowdPropsOurs::FrameReady(const rdr_adapter::LoadedFrame&) {
  idle_frames_ = 0;
  first_slot0_ = rdr::kNoImage;   // (the one_material plant's picture is per frame)
  std::shared_ptr<const SceneTables> record;
  {
    std::lock_guard<std::mutex> lock(mu_);
    record = record_;
  }
  ++frames_seen_;   // (the window line is WindowDetail's: the binder logs it every census window, F3)
  if (!record) return ++not_ready_frames_, false;
  int arena = ArenaCore::ArenaOfRecord(*record);
  if (arena < 0) return ++not_ready_frames_, false;
  if (core_.faults().arena_switch && frames_seen_ > 60) ++arena;   // planted: the record "names" another arena after 60 frames
  if (arena != arena_) {
    if (arena_ >= 0) rdr::Log("[rdr][crowd_props] arena %d -> %d: the prop set is dropped and reloaded (the buffers go with it)", arena_, arena);
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
        rdr::Log("[rdr][crowd_props] early load for arena %d is being replaced by %d", load_for_, arena_);
      }
    } else if (state == 0 && load_for_ != arena_) {
      StartLoad(arena_);
    }
  }
  if (!uploaded_ && options_.wait_for_load && load_state_.load() == 1) {
    if (worker_.joinable()) worker_.join();
    if (loaded_.arena == arena_) Upload(loaded_);
    else loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (!uploaded_) return ++not_ready_frames_, false;
  ++ours_frames_;
  return true;
}

bool CrowdPropsOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  if (!uploaded_ || !props_ || !in.origin || !in.frame || !in.draw) return why = KeepWhy::NotReady, false;
  if (in.stream_index_32bit || core_.faults().index32) return why = KeepWhy::Variant, ++index32_kept_, false;   // F2: our index buffer is u16
  const rdr_adapter::VertexLayout* lay = in.draw->layout != 0xFFFFFFFFu && in.draw->layout < in.frame->layouts.count ? &in.frame->layouts[in.draw->layout] : nullptr;
  const uint32_t stride = lay ? lay->host_stride : 0;
  std::vector<uint8_t> vbytes, ibytes;
  const bool readable = in.read_geometry && stride && in.read_geometry(in.vertex_offset, in.vertex_count * stride, vbytes) && vbytes.size() >= size_t(in.vertex_count) * stride &&
                        in.read_geometry(in.index_offset, in.index_count * 2u, ibytes) && ibytes.size() >= size_t(in.index_count) * 2u;
  const CrowdPropsCore::Decision d = core_.Decide(in.origin->call_site, in.vertex_count, in.index_count, stride, readable ? vbytes.data() : nullptr, vbytes.size(), readable ? ibytes.data() : nullptr,
                                                  ibytes.size());
  if (!d.ok) {
    why = d.why;
    return false;
  }
  const size_t k = size_t(d.mesh);
  const assets::crowd_prop_gs::PropMesh& m = props_->meshes[k];
  if (vertex_buffers_[k] == rdr::kNoBuffer) {
    vertex_buffers_[k] = renderer_.CreateBuffer(m.vertices.size(), m.vertices.data());
    index_buffers_[k] = renderer_.CreateBuffer(m.indices.size() * 2, m.indices.data());
  }
  if (vertex_buffers_[k] == rdr::kNoBuffer || index_buffers_[k] == rdr::kNoBuffer) {   // a missing buffer would make the draw read past nothing: the stream's stays
    ReleaseBuffers();
    vertex_buffers_.assign(props_->meshes.size(), rdr::kNoBuffer), index_buffers_.assign(props_->meshes.size(), rdr::kNoBuffer);
    return why = KeepWhy::Mismatch, false;
  }
  out = rdr_adapter::ReplaceOut{};
  out.vertex = {vertex_buffers_[k], 0};
  out.index = {index_buffers_[k], 0};
  out.index_count = uint32_t(m.indices.size());
  // The stream's own geometry description: our vertex IS in the stream draw's host layout (compared byte for byte above) and our index buffer IS the stream's strip.
  out.vertex_layout = in.stream_layout;
  out.topology = in.stream_topology;
  out.index_32bit = 0;   // (ours is u16; a 32-bit stream draw was kept above)
  const CropFaults& f = core_.faults();
  const auto slot_set = [&](int s) { ++slots_set_; out.slots[s].set = true; };
  if (f.one_material && in.textures && in.textures->slots[0].image != rdr::kNoImage) {   // R (lane 35's bug): one picture for every prop of the frame
    if (first_slot0_ == rdr::kNoImage) first_slot0_ = in.textures->slots[0].image;
    slot_set(0);
    out.slots[0].image = first_slot0_;
  }
  if (f.lightmap_stale && in.textures) {   // the light-map page replaced by one nobody rewrote
    if (stale_image_ == rdr::kNoImage) {
      rdr::ImageDesc desc;
      desc.width = 1, desc.height = 1, desc.mips = 1;
      desc.usage = rdr::kUsageSampled;
      desc.format = rdr::Format::RGBA8;
      stale_image_ = renderer_.CreateImage(desc);
      const uint8_t grey[4] = {128, 128, 128, 255};
      if (stale_image_ != rdr::kNoImage) renderer_.UpdateImage(stale_image_, 0, 0, grey, 4);
    }
    for (int slot : {kLightMapSlotFragment, kLightMapSlotVertex})
      if (stale_image_ != rdr::kNoImage && in.textures->slots[slot].image != rdr::kNoImage) {
        slot_set(slot);
        out.slots[slot].image = stale_image_;
      }
  }
  return true;
}

std::string CrowdPropsOurs::Report() const {
  const CropStats& s = core_.stats();
  std::string keys, sites;
  for (const auto& kv : s.joined_by_key) keys += (keys.empty() ? "" : ", ") + std::to_string(kv.first) + ":" + std::to_string((unsigned long long)kv.second);
  for (const auto& kv : s.replaced_by_site) {
    char b[48];
    std::snprintf(b, sizeof b, "%s0x%08X:%llu", sites.empty() ? "" : ", ", kv.first, (unsigned long long)kv.second);
    sites += b;
  }
  char head[420];
  std::snprintf(head, sizeof head, "ours frames %llu, not ready %llu, upload %.1f ms; decisions %llu, replaced %llu; mismatch: counts %llu vertices %llu indices %llu layout %llu no bytes %llu; stream 32-bit index kept %llu, texture slots set by us %llu, loads %llu; ",
                (unsigned long long)ours_frames_, (unsigned long long)not_ready_frames_, upload_ms_, (unsigned long long)s.decisions, (unsigned long long)s.replaced, (unsigned long long)s.bad_counts,
                (unsigned long long)s.bad_vertices, (unsigned long long)s.bad_indices, (unsigned long long)s.bad_layout, (unsigned long long)s.no_bytes, (unsigned long long)index32_kept_, (unsigned long long)slots_set_, (unsigned long long)loads_);
  return std::string(head) + "joined entries {" + keys + "}, sites {" + sites + "}";
}

// F3: no "[rdr][crowd_props]" prefix here (the caller adds one); the first failed check of the window rides along.
std::string CrowdPropsOurs::WindowDetail() const {
  const std::string& fail = core_.first_fail();
  return Report() + (fail.empty() ? "" : "; first failed check: " + fail);
}

}  // namespace rdr_scene
