// Our props of other pacs drawn in place. See props_ours.h and props_ours_core.h. S101 lane 84 (a copy of ring_ours.cpp's renderer side).
#include "../thread_name.h"  // S102 H1
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
  FreeTestOnly();   // S102 T17
  for (const auto& kv : images_)
    if (kv.second != rdr::kNoImage) renderer_.DestroyImage(kv.second);
  images_.clear();
}

void PropsOurs::FreeTestOnly() {
  if (hole_buffer_ != rdr::kNoBuffer) renderer_.DestroyBuffer(hole_buffer_), hole_buffer_ = rdr::kNoBuffer;
  for (const rdr::ImageId id : twice_)
    if (id != rdr::kNoImage) renderer_.DestroyImage(id);
  twice_.clear();
}

void PropsOurs::DropAll() {
  FreeTestOnly();   // S102 T17: every TEST ONLY resource (tools\props\props_budget_check.py lints that this call stays)
  core_.SetProps(nullptr);   // releases every prepared buffer through Release
  for (const auto& kv : images_)
    if (kv.second != rdr::kNoImage) renderer_.DestroyImage(kv.second);
  images_.clear();
  props_.reset();
  pre_.reset();   // S102 T17
  uploaded_ = false;
}

rdr::ImageId PropsOurs::ImageFor(int texture, uint32_t mips) {
  if (!props_ || texture < 0 || size_t(texture) >= props_->textures.size()) return rdr::kNoImage;
  const uint64_t key = (uint64_t(uint32_t(texture)) << 8) | ImageKey(texture, mips);
  const auto it = images_.find(key);
  const bool twice = props_budget::g_plant.load(std::memory_order_relaxed) == 3;   // plant 3 image_twice: every request makes it again
  if (it != images_.end() && !twice) return it->second;
  if (it != images_.end()) twice_.push_back(it->second);
  const auto t0 = std::chrono::steady_clock::now();
  const rdr::ImageId id = MakeTexture(texture, mips);
  if (budget_on_) {
    frame_us_ += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    ++b_images_made_;
  }
  return images_[key] = id;
}

// The mips an image of (texture, mips) really has: a request for more mips than the file has is the same image (S102 T17 F10: always, the switch on or off; the picture does not change).
uint32_t PropsOurs::ImageKey(int texture, uint32_t mips) const {
  if (!props_ || texture < 0 || size_t(texture) >= props_->textures.size()) return std::min<uint32_t>(mips, 255);
  return props_budget::FitMips(props_->textures[size_t(texture)], mips);
}

size_t PropsOurs::ImageNeed(int texture, uint32_t mips) const {
  if (!props_ || texture < 0 || size_t(texture) >= props_->textures.size()) return 0;
  if (images_.count((uint64_t(uint32_t(texture)) << 8) | ImageKey(texture, mips))) return 0;
  return props_budget::UploadBytes(props_->textures[size_t(texture)], props_budget::FitMips(props_->textures[size_t(texture)], mips));
}

bool PropsOurs::RoleWanted(const rdr_adapter::ReplaceIn& in, int slot, int tex) const {
  if (tex < 0 || !in.textures) return false;
  rdr::ImageId img = in.textures->slots[slot].image;
  if (props_budget::g_force_noimage.load(std::memory_order_relaxed) == slot) img = rdr::kNoImage;   // (a role the draw does not bind: test knob)
  return img != rdr::kNoImage;   // the contract: the stream draw must bind an image there
}

size_t PropsOurs::Need(const rdr_adapter::ReplaceIn& in, const PropsCore::Decision& d, const PropsPrepared& p) {
  const int plant = props_budget::g_plant.load(std::memory_order_relaxed);
  size_t n = 0;
  if (!p.gpu[0]) n += p.vertex.size() + p.index.size() * 2;
  const struct { int slot, tex; uint32_t mips; } roles[3] = {{0, d.diffuse, d.diffuse_mips}, {4, d.normal_map, d.normal_mips}, {5, d.specular, d.specular_mips}};
  uint64_t seen[3] = {};
  int nseen = 0;
  for (const auto& r : roles) {
    if (r.tex < 0 || (plant != 6 && !RoleWanted(in, r.slot, r.tex))) continue;   // (plant 6 need_ignores_slot: the old rule, counts roles put() never makes)
    const uint64_t key = (uint64_t(uint32_t(r.tex)) << 8) | ImageKey(r.tex, r.mips);
    bool dup = false;
    for (int i = 0; i < nseen; ++i) dup = dup || seen[i] == key;
    if (dup) continue;   // two roles on one image make it once
    seen[nseen++] = key;
    if (plant == 7) {   // plant 7 need_zero_texture: the first texture asked about is never counted
      if (plant7_tex_ < 0) plant7_tex_ = r.tex;
      if (r.tex == plant7_tex_) continue;
    }
    n += ImageNeed(r.tex, r.mips);
  }
  return n;
}

void PropsOurs::NoteMade(uint64_t n) {
  b_made_bytes_ += n;
  props_budget::NoteMade(n);
}

rdr::ImageId PropsOurs::MakeTexture(int texture, uint32_t max_mips) {
  const assets::Texture& t = props_->textures[size_t(texture)];
  const uint32_t fit = props_budget::FitMips(t, max_mips);   // S102 T17: the one rule (props_budget.h; the lines that were here)
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
    const size_t sb = props_budget::MipBytes(t.format, mw, mh);
    if (t.format == assets::TexFormat::kB4G4R4A4) {
      if (pre_ && pre_->set == props_.get() && size_t(texture) < pre_->wide.size() && i < pre_->wide[size_t(texture)].size()) {   // S102 T17: the worker's widened texels (the set is asserted)
        renderer_.UpdateImage(id, i, 0, pre_->wide[size_t(texture)][i].data(), pre_->wide[size_t(texture)][i].size());
        NoteMade(pre_->wide[size_t(texture)][i].size());
      } else {
        props_budget::WidenMip(t, src, mw, mh, wide);
        renderer_.UpdateImage(id, i, 0, wide.data(), wide.size());
        NoteMade(wide.size());
      }
    } else {
      renderer_.UpdateImage(id, i, 0, t.data.data() + src, sb);
      NoteMade(sb);
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
  worker_ = std::thread([this, arena, root] { wwe2k14::NameThread(L"ours_props");
    Loaded loaded;
    loaded.arena = arena;
    if (!assets::prop_gs::Init(root)) {
      loaded.error = "no game files at " + root;
    } else {
      auto pset = std::make_shared<assets::prop_gs::PropSet>();
      if (assets::prop_gs::Load(*pset, loaded.error)) {
        if (props_budget::g_on.load(std::memory_order_relaxed)) loaded.pre = props_budget::Prepare(*pset);   // S102 T17: the part that needs no device
        loaded.props = std::move(pset);
      }
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
  pre_ = loaded.pre;   // S102 T17
  core_.SetPre(pre_);
  upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  rdr::Log("[rdr][props] %zu prop meshes from %zu sources (%.1f MB), %zu file textures (images are made at first use, with the stream image's mip count)%s", r.meshes.size(), r.sources.size(), double(r.Bytes()) / 1048576.0, r.textures.size(), loaded.pre ? ("; the worker made the file hashes and widened texels (" + std::to_string(loaded.pre->bytes / 1024) + " KB, props budget on)").c_str() : "");
  loaded = Loaded{};
  uploaded_ = true;
  return true;
}

// ---- the frame --------------------------------------------------------------------------------------------------------------------

void PropsOurs::NoClaimFrame() {
  BudgetTick(nullptr);   // S102 T17
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

bool PropsOurs::FrameReady(const rdr_adapter::LoadedFrame& frame) {
  BudgetTick(&frame);   // S102 T17
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
    if (budget_on_) rdr::Log("%s", BudgetLine().c_str());   // S102 T17: the live census
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
  if (!props_budget::g_record.load(std::memory_order_relaxed)) return ReplaceOne(in, out, why);
  const auto t0 = std::chrono::steady_clock::now();   // S102 T17 (fview --frame-times): the time inside the hook, per frame
  const bool ok = ReplaceOne(in, out, why);
  props_budget::FrameReplaceMs()[props_budget::g_frame_index.load(std::memory_order_relaxed)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return ok;
}

bool PropsOurs::ReplaceOne(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  if (!uploaded_ || in.vs < 0 || size_t(in.vs) >= std::size(kVariants)) {
    why = KeepWhy::NotReady;
    return false;
  }
  const rdr::fx::Variant* ps = in.ps >= 0 && size_t(in.ps) < std::size(kVariants) ? &kVariants[in.ps] : nullptr;
  uint8_t slots[rdr::kMaxTextures];
  uint32_t slot_count = 0;
  if (ps)
    for (uint32_t s = 0; s < ps->sampler_count && slot_count < rdr::kMaxTextures; ++s) slots[slot_count++] = ps->samplers[s].slot;
  const int plant = props_budget::g_plant.load(std::memory_order_relaxed);
  const uint64_t cap_b = uint64_t(props_budget::g_bytes.load(std::memory_order_relaxed)), cap_us = uint64_t(props_budget::g_us.load(std::memory_order_relaxed));
  const bool no_progress = props_budget::g_no_progress.load(std::memory_order_relaxed);
  const auto defer_note = [&] {
    ++b_deferred_;
    if (props_budget::g_record.load(std::memory_order_relaxed)) ++props_budget::FrameDeferred()[props_budget::g_frame_index.load(std::memory_order_relaxed)];
  };
  if (budget_on_ && plant != 4 && frame_us_ >= cap_us && (frame_made_ > 0 || no_progress) && !core_.Warm(in)) {   // S102 T17 F1: a COLD object (no join yet) is not even decided once the frame's making time is used up (plant 4 ignore_us skips this)
    defer_note();
    ++b_deferred_pre_;
    why = plant == 9 ? KeepWhy::NotReady : KeepWhy::BudgetDeferred;
    return false;
  }
  const auto td0 = std::chrono::steady_clock::now();
  const PropsCore::Decision d = core_.Decide(in, slots, slot_count);
  if (budget_on_) frame_us_ += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - td0).count());   // F1: Decide (join, layout, vertex writer, verify, content checks) is first-use work too
  if (!d.ok) {
    why = d.why;
    return false;
  }
  PropsPrepared& p = *d.prepared;
  if (budget_on_) {   // S102 T17: what this draw would make (buffers, images) against the frame's budget; past it the stream's draw stays for this frame and the draw is made on a later one
    const size_t need = Need(in, d, p);
    if (need > 0) {
      const bool past_b = frame_bytes_ + need > cap_b, past_us = plant != 4 && frame_us_ >= cap_us, past = past_b || past_us;
      if (past && (frame_made_ > 0 || no_progress) && plant != 1) {   // (plant 1 no_budget: never defers)
        defer_note();
        if (plant == 2) {   // plant 2 hole: an empty draw in place of the stream's (what a "ready" flag that lies would do)
          if (hole_buffer_ == rdr::kNoBuffer) {
            const uint8_t zeros[64] = {};
            hole_buffer_ = renderer_.CreateBuffer(sizeof zeros, zeros);
          }
          out = rdr_adapter::ReplaceOut{};
          out.vertex = {hole_buffer_, 0};
          out.index = {hole_buffer_, 0};
          out.index_count = 0;
          out.vertex_layout = in.stream_layout;
          out.topology = in.stream_topology;
          out.index_32bit = in.stream_index_32bit;
          return true;
        }
        core_.Deferred();
        why = plant == 9 ? KeepWhy::NotReady : KeepWhy::BudgetDeferred;   // plant 9 deferred_as_not_ready: the old answer (the thin learner reads "no evidence")
        return false;
      }
      if (past) ++(frame_made_ > 0 ? b_overrun_ : past_b ? b_fallback_ : b_forced_us_);   // fallback: the frame's first creation is made although its bytes alone pass the budget (progress); forced_us: made because the frame's time was gone before it; overrun: a SECOND one past the budget (only a plant)
      if (frame_made_ == 0) first_need_ = need;
      ++frame_made_;
      frame_bytes_ += need;
      if (props_budget::g_record.load(std::memory_order_relaxed)) props_budget::FrameBytes()[props_budget::g_frame_index.load(std::memory_order_relaxed)] += need;
    }
  }
  if (!p.gpu[0]) {
    const auto t0 = std::chrono::steady_clock::now();
    p.gpu[0] = renderer_.CreateBuffer(p.vertex.size(), p.vertex.data());
    p.gpu[1] = renderer_.CreateBuffer(p.index.size() * 2, p.index.data());
    if (budget_on_) frame_us_ += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    if (!p.gpu[0] || !p.gpu[1]) {   // a failure is remembered (no retry on every draw) and the draw stays the stream's
      Release(p);
      core_.UploadFailed(&p);
      return why = KeepWhy::Mismatch, false;
    }
    NoteMade(p.vertex.size() + p.index.size() * 2);
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
    if (!RoleWanted(in, slot, tex)) return;   // the same rule Need() counts by
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
  return std::string(head) + core_.Report() + (budget_on_ ? "; " + BudgetLine() : std::string());
}

void PropsOurs::BudgetTick(const rdr_adapter::LoadedFrame* frame) {
  budget_on_ = props_budget::g_on.load(std::memory_order_relaxed);
  if (!frame) return;   // NoClaimFrame: nothing was claimed, nothing was made; the counters belong to the frame FrameReady names
  const uint64_t key = frame->header.guest_frame * 1000003ull + frame->header.capture_frame;   // the guest frame (and the capture's own number: both change every frame)
  if (have_key_ && key == frame_key_) return;   // another bind of the same guest frame (a DrawAndRead part): the budget is per frame, not per bind
  have_key_ = true, frame_key_ = key;
  if (frame_made_ > 0) {
    ++b_slices_;
    b_bytes_ += frame_bytes_;
    b_max_frame_bytes_ = std::max(b_max_frame_bytes_, frame_bytes_);
    if (first_need_ > uint64_t(props_budget::g_bytes.load(std::memory_order_relaxed))) ++b_first_over_;   // a frame whose FIRST single need passes the budget: the only frames "fallback" may count
  }
  if (props_budget::g_plant.load(std::memory_order_relaxed) != 5) frame_bytes_ = frame_us_ = 0;   // (plant 5 no_reset: the spend of the last frame stays)
  frame_made_ = 0, first_need_ = 0;
  if (budget_on_) ++b_frames_;
}

std::string PropsOurs::BudgetLine() const {
  if (!budget_on_) return std::string();
  const bool open = frame_made_ > 0;   // the frame in progress counts too (the last frame of a run is never closed by a next one)
  const uint64_t first_over = b_first_over_ + (open && first_need_ > uint64_t(props_budget::g_bytes.load()) ? 1 : 0);
  char b[560];
  std::snprintf(b, sizeof b, "[rdr][props_budget] frames %llu, slices %llu, bytes %llu, fallback %llu, first_over %llu, forced_us %llu, overrun %llu, deferred %llu, pre_deferred %llu, most bytes in one frame %llu, images made %llu of %zu kept, made %llu bytes, budget %lld bytes / %lld us",
                (unsigned long long)b_frames_, (unsigned long long)(b_slices_ + (open ? 1 : 0)), (unsigned long long)(b_bytes_ + (open ? frame_bytes_ : 0)), (unsigned long long)b_fallback_, (unsigned long long)first_over, (unsigned long long)b_forced_us_,
                (unsigned long long)b_overrun_, (unsigned long long)b_deferred_, (unsigned long long)b_deferred_pre_, (unsigned long long)std::max<uint64_t>(b_max_frame_bytes_, open ? frame_bytes_ : 0),
                (unsigned long long)b_images_made_, images_.size(), (unsigned long long)b_made_bytes_, (long long)props_budget::g_bytes.load(), (long long)props_budget::g_us.load());
  return b;
}

}  // namespace rdr_scene
