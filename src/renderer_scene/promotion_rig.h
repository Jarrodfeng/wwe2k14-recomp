// S101 lane 47: the promotion switches (crowd / arena / wrestlers drawn by us) for a player that replays a capture with no window: rplay (src\renderer\player\rplay.cpp), so the
// replay farm (tools\farm\farm.py) can measure a promotion's GPU cost. The same factories and hooks fview uses (fview\session.cpp EnableCrowdOurs / EnableReplace / FeedCrowd),
// the same record pairing (stream frame j with scene record j-1, CROWD_PROMOTION.md), records read through CaptureReader::SceneRecordPath. Header only: the sources it needs are
// the ones in tools\fview\session_deps.txt (rplay's build compiles that list too). No game or SDK includes.
//
// Differences from fview, on purpose: the crowd's pairing proof runs every 30th frame (as live), not every frame (the offline proof costs a pass over the draws); the arena and
// wrestlers hooks are made in their OFFLINE mode (loads on the calling thread, verify every frame), so a frame is never "not ready" after the first; only the GPU number (the timestamps
// around the frame's command buffer) is the promotion's cost, the CPU columns include that verification.
#pragma once
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../renderer_adapter/binder.h"
#include "../renderer_adapter/capture_reader.h"
#include "../renderer_adapter/image_content_feed.h"  // S101 lane 66
#include "../renderer_adapter/image_info_table.h"
#include "crowd_ours.h"
#include "post_ours.h"   // S101 lane PO
#include "record_io.h"
#include "replace_factories.h"

namespace rdr_scene {

struct PromotionOptions {
  bool caw = false;   // lane 153
  bool crowd = false, arena = false, wrestlers = false, ring = false, ui = false, flash = false, props = false, shadow_casters = false, screens = false;  // S101 lane 115: screens;  // S101 lane 99: shadow casters; ring / ui / flash: S101 contracts; props: S101 lane 84
  bool ui_text = false;  // S101 lane 89 (UT)
  bool movie = false;    // S101 lane 131: the movie planes; needs no scene record and no game file
  // S101 lane PO (the post chain, post_hook.h): needs neither the game files nor scene records. post_fault: PostFaults::Parse's list ("" none); render_scale: rplay's --scale (the checks need it);
  // post_limits: "no_r32f_linear" drives the device-limits path.
  bool post = false;
  bool plane_shadow = false;   // lane 139: the plane shadow chain's constants (SH3 slice 2; no game files needed; a capture without plane sections replaces nothing)
  bool circles = false;   // lane 136: the circle blob shadows (SH3 slice 1; no game files and no scene record needed to run: a capture without circle sections replaces nothing)
  bool rim = false;   // lane 124: the rim light map's casters (after the shadow casters)
  bool small_passes = false;   // lane 135: the sweat map and skin pre-light passes (needs neither the game files nor scene records)
  bool rim_chain = false;   // lane 151: the rim light map's reductions and blur by our programs (needs neither the game files nor scene records)
  std::string post_fault, post_limits;
  float render_scale = 1.0f;
  bool crowd_cards = false;  // S101 lane CD: with crowd, the far cards ours (CrowdOursOptions::cards)
  std::string game_root;  // the player's game files (game_data\extracted)
  bool any() const { return crowd || arena || wrestlers || ring || ui || ui_text || flash || props || shadow_casters || screens || post || rim || movie || small_passes || circles || plane_shadow || rim_chain /* lane 151 */; }   // lane 124: rim  // S101 lane 115: screens
  bool any_game() const { return crowd || arena || wrestlers || ring || ui || ui_text || flash || props || shadow_casters || screens || rim; }   // lane 124: rim  // S101 lane PO: the classes that read the game files
  bool needs_scene_records() const { return crowd || arena || wrestlers || ring || flash || props || shadow_casters || screens || rim; }   // lane 124: rim (as shadow_casters: the class itself only joins through a record; fview exempts both so Bryan, a capture without records, runs with nothing replaced)  // S101 lane 115: screens; S101 lane 99; S101 lane 66: every class but the UI joins by scene record
};

class PromotionRig {
 public:
  // Switches the chosen classes on in `binder` (call before the first Bind). false + `error`: a class could not start (the replay then must not run: it would measure nothing).
  bool Enable(rdr::Renderer& renderer, rdr_adapter::Binder& binder, const rdr_adapter::CaptureReader& capture, const PromotionOptions& options, std::string& error) {
    capture_ = &capture;
    if (!options.any()) return true;
    if (options.any_game() && options.game_root.empty()) {   // S101 lane PO: the post chain alone needs no game files
      error = "--crowd-ours / --arena-ours / --wrestlers-ours need --game DIR (the player's game files)";
      return false;
    }
    if (options.post) {   // S101 lane PO
      PostOursOptions po;
      if (!PostFaults::Parse(options.post_fault, po.faults, error)) return false;
      if (!options.post_limits.empty() && options.post_limits != "no_r32f_linear") {
        error = "unknown --post-limits '" + options.post_limits + "' (no_r32f_linear)";
        return false;
      }
      po.limits.no_r32f_linear = options.post_limits == "no_r32f_linear";
      po.limits.render_scale = options.render_scale;
      post_ = std::make_unique<PostOurs>(renderer, po);
      binder.SetPostHook(post_.get());
      rdr_adapter::g_post_ours = true;
      if (!options.any_game() && !options.movie) return true;   // S101 lane 131: movie
    }
    // S101 lane 66: the UI needs no scene record (it joins by picture content); every other class does.
    if (options.needs_scene_records() && capture.scene_record_count() == 0) {
      error = "the capture has no scene records (scene\\frame_NNNNNN.rsc): taken without --wwe2k14_scene_records";
      return false;
    }
    images_.Set(capture.images().data(), capture.images().size());
    InstallReplaceFactories();
    if (options.crowd) {
      CrowdOursOptions o;
      o.game_root = options.game_root;
      o.verify_every = 30;
      o.cards = options.crowd_cards;  // S101 lane CD
      crowd_ = std::make_unique<CrowdOurs>(renderer, o);
      binder.SetCrowdHook(crowd_.get());
      rdr_adapter::g_crowd_ours = true;
    }
    handles_.reserve(std::size(kReplaceClasses));  // (one per class in the table: lane 84 review)
    if (options.arena && !AddClass(renderer, binder, options, "arena", g_arena_factory, &rdr_adapter::g_arena_ours, error)) return false;
    if (options.wrestlers && !AddClass(renderer, binder, options, "wrestlers", g_wrestlers_factory, &rdr_adapter::g_wrestlers_ours, error)) return false;
    if (options.ring && !AddClass(renderer, binder, options, "ring", g_ring_factory, &rdr_adapter::g_ring_ours, error)) return false;  // S101 ring contract
    if (options.flash && !AddClass(renderer, binder, options, "flash", g_flash_factory, &rdr_adapter::g_flash_ours, error)) return false;  // S101 flash contract
    if (options.screens && !AddClass(renderer, binder, options, "screens", g_screens_factory, &rdr_adapter::g_screens_ours, error)) return false;  // S101 lane 115
    if (options.props && !AddClass(renderer, binder, options, "props", g_props_factory, &rdr_adapter::g_props_ours, error)) return false;  // S101 lane 84
    if (options.caw && !AddClass(renderer, binder, options, "caw", g_caw_factory, &rdr_adapter::g_caw_ours, error)) return false;   // lane 153 (after the props: review B1)
    if (options.shadow_casters && !AddClass(renderer, binder, options, "shadow_casters", g_shadow_casters_factory, &rdr_adapter::g_shadow_casters_ours, error)) return false;  // S101 lane 99
    if (options.plane_shadow && !AddClass(renderer, binder, options, "plane_shadow", g_plane_shadow_factory, &rdr_adapter::g_plane_shadow_ours, error)) return false;  // lane 139
    if (options.circles && !AddClass(renderer, binder, options, "circles", g_circles_factory, &rdr_adapter::g_circles_ours, error)) return false;  // lane 136
    if (options.small_passes && !AddClass(renderer, binder, options, "small_passes", g_small_passes_factory, &rdr_adapter::g_small_passes_ours, error)) return false;  // lane 135 (its own call sites)
    if (options.movie && !AddClass(renderer, binder, options, "movie", g_movie_factory, &rdr_adapter::g_movie_ours, error)) return false;  // S101 lane 131 (before the UI)
    if (options.rim && !AddClass(renderer, binder, options, "rim_map", g_rim_factory, &rdr_adapter::g_rim_ours, error)) return false;  // lane 124
    if (options.rim_chain && !AddClass(renderer, binder, options, "rim_chain", g_rim_chain_factory, &rdr_adapter::g_rim_chain_ours, error)) return false;  // lane 151 (its own call sites)
    if (options.ui_text && !AddClass(renderer, binder, options, "ui_text", g_ui_text_factory, &rdr_adapter::g_ui_text_ours, error)) return false;  // S101 lane 89 (before the UI's)
    if (options.ui && !AddClass(renderer, binder, options, "ui", g_ui_factory, &rdr_adapter::g_ui_ours, error)) return false;  // S101 UI contract
    return true;
  }

  bool active() const { return crowd_ || post_ || !handles_.empty(); }

  // S101 lane 271: the match card's arena handed to every per-arena class before any record names it (live.cpp does the same on the live thread when session_log.h publishes the card).
  void Announce(int arena) {
    if (crowd_) crowd_->Announce(arena);
    for (ReplaceHookHandle& h : handles_)
      if (h.announce_arena) h.announce_arena(arena);
  }

  // The crowd's bodies load on a worker: wait (up to `seconds`) for the first frame with a crowd to have them, so the replay is not a fallback one. true: ready or no crowd on.
  bool Prime(double seconds, uint32_t frame_count, std::string& message) {
    if (!crowd_) return true;
    const auto start = std::chrono::steady_clock::now();
    const uint32_t limit = frame_count < 600 ? frame_count : 600;
    for (uint32_t f = 0; f < limit; ++f) {
      Feed(f);
      crowd_->FrameReady();
      if (crowd_->arena() < 0) continue;
      while (!crowd_->FrameReady()) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) {
          message = "the crowd's bodies for arena " + std::to_string(crowd_->arena()) + " were not loaded in " + std::to_string(int(seconds)) + " s (game folder wrong?)";
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      message = "crowd bodies for arena " + std::to_string(crowd_->arena()) + " ready after frame " + std::to_string(f) + "'s record";
      record_index_ = -2;
      return true;
    }
    message = "no scene record in the first " + std::to_string(limit) + " frames has a crowd";
    return false;
  }

  // The scene record for stream frame f to every hook (record f-1). Call before Bind of frame f.
  void Feed(uint32_t f) {
    if (!active()) return;
    if (ui_on_ && capture_) {  // S101 lane 66: frame f's content writes into the UI's content table (the same feeder the game's render thread uses)
      if (f == 0) content_.Clear();   // (a warm-up replays the frames from 0 again)
      rdr_adapter::LoadedFrame content_frame;
      if (capture_->LoadFrame(f, content_frame)) rdr_adapter::FeedImageContents(content_frame, *capture_, content_, content_scratch_);
    }
    const int index = int(f) - 1;
    if (index != record_index_) {
      record_index_ = index;
      record_.reset();
      if (index >= 0 && uint32_t(index) < capture_->frame_count() && capture_->HasSceneRecord(uint32_t(index))) {
        auto tables = std::make_shared<SceneTables>();
        std::string err;
        if (ReadRecord(capture_->SceneRecordPath(uint32_t(index)).string(), *tables, err)) record_ = tables;
        else std::fprintf(stderr, "[rplay] scene record %d: %s\n", index, err.c_str());
      }
    }
    if (crowd_) crowd_->SetRecord(record_);
    for (ReplaceHookHandle& h : handles_)
      if (h.set_record) h.set_record(record_);
  }

  // Census lines (the binder's counters + the hooks' own), for the log.
  std::string Report(const rdr_adapter::Binder& binder) const {
    std::string text;
    if (crowd_) {
      const rdr_adapter::CrowdCounters& c = binder.crowd_counters();
      char line[320];
      std::snprintf(line, sizeof line, "[rdr][crowd] ours frames %llu, fallback %llu, stream draws dropped %llu, members drawn %llu, persist-only %llu\n", (unsigned long long)c.ours_frames, (unsigned long long)c.fallback_frames,
                    (unsigned long long)c.draws_dropped, (unsigned long long)c.members_drawn, (unsigned long long)c.persist_only);
      text += line;
      text += "[rplay] " + crowd_->Report() + "\n";
    }
    if (post_) text += binder.PostCensus() + "\n";   // S101 lane PO
    for (size_t i = 0; i < handles_.size(); ++i) {
      text += binder.ReplaceCensus(handles_[i].hook->Name()) + "\n";
      if (handles_[i].report) text += "[rplay] [rdr][" + std::string(handles_[i].hook->Name()) + "] " + handles_[i].report() + "\n";
    }
    return text;
  }

  // Destroy the hooks (the caller destroys the binder first: it points at them).
  void Reset() {
    crowd_.reset();
    post_.reset();   // S101 lane PO
    handles_.clear();
  }

 private:
  bool AddClass(rdr::Renderer& renderer, rdr_adapter::Binder& binder, const PromotionOptions& options, const char* name, ReplaceFactory factory, bool* flag, std::string& error) {
    if (!factory) {
      error = std::string("no ") + name + " class is registered in this build";
      return false;
    }
    ReplaceFactoryArgs args;
    args.renderer = &renderer;
    args.game_root = options.game_root;
    args.image_info = images_.Supplier();  // the same supplier the live path uses (image_info_table.h)
    args.image_content = content_.Supplier();  // S101 lane 66: the UI's content supplier (fed by Feed)
    ReplaceHookHandle handle = factory(args);
    if (handle.wants_content) ui_on_ = true;   // S101 lane 66: Feed fills the content table only when a hook asked for it
    if (!handle.hook) {
      error = std::string(name) + ": " + (handle.error.empty() ? std::string("the class refused to start") : handle.error);
      return false;
    }
    handles_.push_back(std::move(handle));
    binder.AddReplaceHook(handles_.back().hook.get(), flag);
    *flag = true;
    return true;
  }

  const rdr_adapter::CaptureReader* capture_ = nullptr;
  rdr_adapter::ImageInfoTable images_;
  rdr_adapter::ImageContentTable content_;   // S101 lane 66 (declared before the hooks that call its supplier)
  rdr_adapter::LoadedTexture content_scratch_;
  bool ui_on_ = false;
  std::unique_ptr<CrowdOurs> crowd_;
  std::unique_ptr<PostOurs> post_;   // S101 lane PO
  std::vector<ReplaceHookHandle> handles_;
  int record_index_ = -1;
  std::shared_ptr<const SceneTables> record_;
};

}  // namespace rdr_scene
