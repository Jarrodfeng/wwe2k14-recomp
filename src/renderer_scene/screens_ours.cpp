// Our stage screens drawn in place. See screens_ours.h and screens_ours_core.h. S101 lane 115 (a copy of ring_ours.cpp's renderer side, geometry only).
#include "screens_ours.h"

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

ScreensOurs::ScreensOurs(rdr::Renderer& renderer, const ScreensOursOptions& options) : renderer_(renderer), options_(options), core_(options), root_(options.game_root) {
  core_.SetReleaseHook([this](ScreensPrepared& p) { Release(p); });
}

ScreensOurs::~ScreensOurs() {
  if (worker_.joinable()) worker_.join();
  renderer_.WaitIdle();
  DropAll();
}

void ScreensOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  std::lock_guard<std::mutex> lock(mu_);
  record_ = std::move(record);
}

void ScreensOurs::SetGameRoot(const std::string& root) {
  std::lock_guard<std::mutex> lock(mu_);
  root_ = root;
}

void ScreensOurs::Release(ScreensPrepared& p) {
  if (p.gpu[0]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[0]));
  if (p.gpu[1]) renderer_.DestroyBuffer(rdr::BufferId(p.gpu[1]));
  p.gpu[0] = p.gpu[1] = 0;
}

void ScreensOurs::DropAll() {
  core_.SetScreens(nullptr);   // releases every prepared buffer through Release
  set_.reset();
  uploaded_ = false;
}

// ---- loading ----------------------------------------------------------------------------------------------------------------------

// S101 lane 271 (SKIP_GAME_WORK_MAP 19.8): the in-game sequence's match card names the arena ~1.5 s before the first record that draws it. Starting the load then (on its worker,
// as FrameReady would) has the meshes ready when that record arrives instead of 0.1-8 s after it. A load already running or done, a record that already named an arena, or the
// same arena again does nothing; a wrong arena is a wasted load that FrameReady discards (loaded_.arena != arena_) and replaces.
void ScreensOurs::Announce(int arena) {
  if (load_state_.load() == 2 && loaded_.arena != arena) {   // a finished early load for another arena (the player backed out after the card) must not block this one
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
  }
  if (arena < 0 || uploaded_ || arena_ >= 0 || load_state_.load() != 0 || load_for_ == arena) return;
  StartLoad(arena);
}

void ScreensOurs::StartLoad(int arena) {
  std::string root;
  {
    std::lock_guard<std::mutex> lock(mu_);
    root = root_;
  }
  if (root.empty()) {
    if (!said_no_root_) said_no_root_ = true, rdr::Log("[rdr][screens] no game folder to load the screens from: the stream's screens stay");
    return;
  }
  if (worker_.joinable()) worker_.join();
  load_state_ = 1;
  load_for_ = arena;
  worker_ = std::thread([this, arena, root] {
    Loaded loaded;
    loaded.arena = arena;
    if (!assets::screen_gs::Init(root)) {
      loaded.error = "no game files at " + root;
    } else {
      auto set = std::make_shared<assets::screen_gs::ScreenSet>();
      if (assets::screen_gs::Load(arena, *set, loaded.error)) loaded.set = std::move(set);
    }
    loaded_ = std::move(loaded);
    load_state_ = 2;
  });
}

bool ScreensOurs::Upload(Loaded& loaded) {
  DropAll();
  if (!loaded.set) {
    rdr::Log("[rdr][screens] arena %d: the screens could not be loaded (%s): the stream's screens stay", loaded.arena, loaded.error.c_str());
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const assets::screen_gs::ScreenSet& r = *loaded.set;
  set_ = loaded.set;
  core_.SetScreens(set_);
  upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  rdr::Log("[rdr][screens] arena %d: %zu screen meshes in %zu keys (%.1f MB; geometry only, every texture slot stays the stream's)", loaded.arena, r.meshes.size(), r.keys.size(), double(r.Bytes()) / 1048576.0);
  loaded = Loaded{};
  uploaded_ = true;
  return true;
}

// ---- the frame --------------------------------------------------------------------------------------------------------------------

void ScreensOurs::NoClaimFrame() {
  core_.NoClaimFrame();   // lane 115 F2: the core's clock also runs on frames where every object is foreign (nothing claimed -> FrameReady never runs), or the 300-frame verdicts never expire
  {   // keyed on the record, not on draws: a frame with every screen culled still has the arena in the record
    std::lock_guard<std::mutex> lock(mu_);
    if (record_ && ScreensCore::ArenaOfRecord(*record_) >= 0) {
      idle_frames_ = 0;
      const int rec_arena = ScreensCore::ArenaOfRecord(*record_);
      if (rec_arena != arena_) {   // another arena in the record while no screen draw was claimed: the old set goes now, the next ready frame loads the new one
        DropAll();
        arena_ = rec_arena;
        load_for_ = -1;
      }
      return;
    }
  }
  if (idle_frames_ + 1 == 12 * kIdleDrop && load_state_.load() == 2 && !uploaded_ && arena_ < 0) {   // lane 271: an early load nobody claimed (the player left after the card) gives its memory back
    if (worker_.joinable()) worker_.join();
    loaded_ = Loaded{};
    load_state_ = 0;
    load_for_ = -1;
    rdr::Log("[rdr][screens] an early load was never claimed: released");
  }
  if (++idle_frames_ == kIdleDrop && (uploaded_ || arena_ >= 0)) {
    DropAll();
    arena_ = -1;
    load_for_ = -1;
    rdr::Log("[rdr][screens] no arena for %u frames: the screens are released", kIdleDrop);
  }
}

bool ScreensOurs::FrameReady(const rdr_adapter::LoadedFrame&) {
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
  const int arena = ScreensCore::ArenaOfRecord(*record);
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
        rdr::Log("[rdr][screens] early load for arena %d is being replaced by %d", load_for_, arena_);
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

bool ScreensOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  if (!uploaded_ || in.vs < 0 || size_t(in.vs) >= std::size(kVariants)) {
    why = KeepWhy::NotReady;
    return false;
  }
  const ScreensCore::Decision d = core_.Decide(in, in.ps >= 0 && size_t(in.ps) < std::size(kVariants) ? kVariants[in.ps].name : nullptr);
  if (!d.ok) {
    why = d.why;
    return false;
  }
  ScreensPrepared& p = *d.prepared;
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
  // The stream's own geometry description: our vertex IS in the stream draw's host layout and our index buffer IS the stream's strip. No texture slot is set: they all stay the stream's.
  out.vertex_layout = in.stream_layout;
  out.topology = in.stream_topology;
  out.index_32bit = in.stream_index_32bit;
  return true;
}

std::string ScreensOurs::Report() const {
  char head[200];
  std::snprintf(head, sizeof head, "[rdr][screens] ours frames %llu, not ready %llu, upload %.1f ms, prepared %zu; ", (unsigned long long)ours_frames_, (unsigned long long)not_ready_frames_, upload_ms_, core_.prepared_count());
  return std::string(head) + core_.Report();
}

}  // namespace rdr_scene
