// The live sink (Session 100, live.h, docs\renderer\LIVE_ADAPTER.md). No game or SDK includes.
#include "live.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <rex/logging.h>  // S101 lane 216: RegisterQuitFlush
#include <unordered_set>

#include "../renderer/frame_builder.h"
#include "../renderer/player/png.h"
#include "../renderer/problem.h"
#include "../renderer/overlay.h"  // lane 256: rdr::g_presented
#include "../renderer/renderer.h"
#include "binder.h"
#include "photo_save.h"  // lane 201: photo mode's save (retry, named failures, the shown shape)
#include "../renderer/frame_blend.h"
#include "capture_reader.h"
#include "lasting_prune.h"  // lane 307: a skipped frame binds only the passes its lasting copies need
#include "snapshot_keeper.h"  // lane 215: the scene-record keeper (ForPart)
#include "image_info_table.h"  // S101 lane 47: the live image-info supplier
#include "image_content_feed.h"  // S101 lane 66: the live image-content supplier (the UI)
#include "origin_intern.h"  // S101 lane 89: origin::g_labels_wanted
#include "../perf_file_write.h"  // lane 302: NoteFileWrite
#include "../case/case_hook.h"  // S101 lane 5: the bug button's ring takes what the sink takes
#include "../renderer_scene/crowd_ours.h"  // S101 lane 35: the crowd's first promotion
#include "../renderer_scene/post_ours.h"   // S101 lane PO: the post chain's promotion
#include "../renderer_scene/replace_factories.h"  // S101 lane 41: the arena's and wrestlers' in-place hooks
#include "../renderer_scene/live_scene.h"
#include "../caw_text.h"  // S101 lane 118: the Create a Superstar editor's drawn state (the `!caw` verb); one atomic test per frame while off
#include "../cost_counters.h"  // S101 lane 83: the replace-check column of the [cost] line

#include <rex/perf/hang_beats.h>  // lane 256: owner mode's presents beat the hang guard
#include <rex/perf/frame_events.h>  // S100: owner mode's presents counted where the SDK presenter counts its own (frames_*.csv presented)

namespace rdr_adapter {

namespace {

// What the binder reads through (binder.h's Source): the images the render thread knows, and the store's bytes.
struct LiveSource {
  std::vector<ImageRecord> records;
  std::mutex* mu = nullptr;
  const std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>>* geometry = nullptr;  // lane 273: immutable buffers, shared out under the lock and read after it
  std::unordered_map<uint64_t, std::vector<uint8_t>>* textures = nullptr;  // contents are moved out (the entry is erased after the frame applies it)

  const std::vector<ImageRecord>& images() const { return records; }
  bool ReadGeometry(uint64_t key, uint32_t bytes, std::vector<uint8_t>& out) const {
    // Lane 273: the lock is held for the find and a handle copy only (the Append of the game thread waited behind a copy of up to MBs here);
    // the bytes are read after it, and a DropGeometry meanwhile only drops the store's handle.
    std::shared_ptr<const std::vector<uint8_t>> held;
    {
      std::lock_guard lock(*mu);
      StoreTimer hold(0);
      if (auto it = geometry->find(key); it != geometry->end()) held = it->second;
      hold.Stop();
    }
    if (!held || held->size() < bytes) {
      out.assign(bytes, 0);
      rdr::Problem("live_geometry_missing", "key %llu, %u bytes", static_cast<unsigned long long>(key), bytes);
      return false;
    }
    out.assign(held->begin(), held->begin() + bytes);
    return true;
  }
  bool ReadTexture(const ContentRef& ref, LoadedTexture& out) const {
    {
      std::lock_guard lock(*mu);
      auto it = textures->find(ref.offset);
      if (it == textures->end() || it->second.empty()) {
        rdr::Problem("live_texture_missing", "image %u", ref.image);
        return false;
      }
      out.bytes = std::move(it->second);  // S100 speed: not copied (erased once the frame has applied its contents)
    }
    return CaptureReader::ParseTexture(out);
  }
};

// S101 lane 66: the UI's content feeder reads a frame's texture contents WITHOUT taking them (LiveSource::ReadTexture moves the store's bytes out: ApplyContents must still find
// them). The bytes are lent (moved out under the lock, hashed, moved back), only while the UI switch is on, before ApplyContents.
struct LiveFeedSource {
  const LiveSource& live;
  const std::vector<ImageRecord>& images() const { return live.records; }
  bool ReadTexture(const ContentRef& ref, LoadedTexture& out) const {
    {
      std::lock_guard lock(*live.mu);
      const auto it = live.textures->find(ref.offset);
      if (it == live.textures->end() || it->second.empty()) return false;
      out.bytes = std::move(it->second);   // LENT: nothing is copied under the lock; GiveBack puts it back before ApplyContents runs (same thread)
    }
    return CaptureReader::ParseTexture(out);
  }
  void GiveBack(const ContentRef& ref, LoadedTexture& out) const {
    std::lock_guard lock(*live.mu);
    (*live.textures)[ref.offset] = std::move(out.bytes);
    out.data = nullptr, out.subresources = nullptr;
  }
};

// A frame's first part, drawn and one image read back (DrawAndRead); the reader waits on it.
struct ReadRequest {
  uint32_t image = 0, mip = 0, layer = 0;  // the reader's image id
  std::mutex mu;
  std::condition_variable cv;
  bool done = false, ok = false;
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  std::chrono::steady_clock::time_point posted, taken, drawn, read;  // lane 232: the blocking path's timeline
};

struct Message {
  std::vector<uint8_t> frame;
  uint64_t guest_frame = 0;
  std::string picture;  // non-empty: write this frame's picture under this name
  bool has_images = false;
  std::vector<ImageRecord> images;
  std::vector<uint32_t> released;
  std::vector<uint8_t> display_curve;  // non-empty: the game's display table changed (256 x RGBA8)
  uint64_t seq = 0;                    // lane 232: the submit count of this message (SubmitFrame numbers its frames; a DrawAndRead part carries the count so far)
  std::shared_ptr<ReadRequest> read;   // non-null: a frame's first part, always drawn, never shown; this image read back after it
  std::shared_ptr<const rdr_scene::SceneTables> d3_record;  // lane 152 (review B1): THIS present's own record (stamps = the hook's frames up to guest_frame - 1): stream frame j needs stamp j-1, which lives in record j, not j-1 (the crowd's pairing)
  std::shared_ptr<const rdr_scene::SceneTables> record;  // S101 lane 35: the scene record the crowd is drawn from (the PREVIOUS snapshot: pairing offset +1)
};

// S101 lane 35 / lane 215: SnapshotKeeper (the owner's scene sink) lives in snapshot_keeper.h.

}  // namespace

struct LiveSink::Impl {
  LiveOptions options;
  std::atomic<bool> active{false};

  // The store: bytes by key. Geometry stays until the reader drops its key; texture contents go once a frame has applied them.
  mutable std::mutex store_mu;
  std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> geometry;  // lane 273: shared handles, see LiveSource::ReadGeometry
  std::unordered_map<uint64_t, std::vector<uint8_t>> textures;
  std::atomic<uint64_t> next_key{1};

  // S101 lane 47: the image records as the render thread knows them (width / height per capture image id) for the replace hooks' size checks.
  ImageInfoTable image_info;
  ImageContentTable image_content;  // S101 lane 66: the UI hook's content supplier (width / height / format / mips / hash per stream image), fed on the render thread from each frame's contents

  // What the next frame carries.
  bool images_pending = false;
  std::vector<ImageRecord> images;
  std::vector<uint32_t> released;
  std::vector<uint8_t> display_curve;

  // The mailbox.
  std::mutex mail_mu;
  std::condition_variable mail_cv;
  std::condition_variable room_cv;  // a frame left the queue
  std::deque<Message> mail;
  bool stopping = false;
  // S101 lane 216: the end-of-run census flush. quit_req: asked (the SDK quit flush before the hard exit, or the black box's exit zip); quit_ack: the render
  // thread printed every class's final census line since the last frame it handled (cleared by the next frame, so a later request waits again).
  std::atomic<bool> quit_req{false}, quit_ack{false};
  bool quit_registered = false;
  std::thread thread;
  // S101 lane 302: WaitForRoom's give-up state (guarded by mail_mu; written by the game's swap thread only).
  bool pace_off = false;
  int pace_timeouts = 0;
  uint32_t pace_give_ups = 0;
  uint64_t pace_skipped_frames = 0;
  // S101 lane 302: pictures written on a worker. The GPU read-back (and the display curve / stretch) stays on the render thread; the PNG encode,
  // the file write and the rename go to pic_thread so a stalled disk (run 7202: 47-62 s) cannot hold the render thread, and through it the paced
  // game. A named picture's picture_done is held until every queued write is on disk (the route's quit waits for it, pending_work.h).
  struct PicJob {
    std::filesystem::path path;
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
  };
  std::mutex pic_mu;
  std::condition_variable pic_cv;
  std::deque<PicJob> pic_q;
  int pic_jobs = 0;           // queued + being written
  int pic_deferred_done = 0;  // picture_done calls waiting for pic_jobs to reach 0
  bool pic_stop = false;
  std::thread pic_thread;

  // Counts.
  std::atomic<uint64_t> drawn{0}, skipped{0}, draws{0}, bound{0}, no_shader{0}, pngs{0};
  std::atomic<uint64_t> parts{0};  // frames' first parts drawn for a read-back copy (DrawAndRead)
  std::atomic<uint64_t> submit_seq{0};  // lane 232: frames submitted by SubmitFrame so far
  struct AsyncTag {
    AsyncReadback::Key key;
    uint64_t frame = 0, seq = 0;
  };
  AsyncReadback async;  // lane 232 (3d): finished copies of watched images (async_readback.h), published by the render thread after it draws a frame holding a copy into one
  std::vector<AsyncReadback::Key> async_due;
  std::vector<uint32_t> async_images;
  std::atomic<uint64_t> persistent_only{0};  // skipped frames whose lasting work (copies into textures) still ran
  uint64_t last_logged = 0;  // render thread only
  uint32_t vblank_debt = 0;  // S9: vblanks of presents since the last drawn frame (render thread only)
  rdr::TickBlender tween;    // S9b: the last drawn tick, for the next tick's halfway picture (render thread only)
  rdr::TickBlender motion_ticks;  // S10b: the last drawn tick, whose vertex constants the DLSS velocity draws read (render thread only)
  // Settings parity S11: the app's overlay, handed over by any thread, applied by the render thread before it renders.
  struct OverlayTexture {
    uint32_t id, width, height;
    std::vector<uint8_t> rgba;
  };
  std::mutex overlay_mu;
  std::shared_ptr<const rdr::OverlayList> overlay_next;
  bool overlay_changed = false;
  std::vector<OverlayTexture> overlay_textures;
  std::filesystem::path last_picture_path;  // S101 lane 19: write the last presented picture here now (mail_mu)
  std::filesystem::path photo_path;  // photo mode: the next shown picture goes here (overlay_mu); kept until written (lane 201: retried, then named)
  uint32_t photo_tries = 0;          // lane 201: failed attempts of the request in photo_path (render thread; reset by SavePicture under overlay_mu)
  void ApplyOverlay() {  // render thread
    std::vector<OverlayTexture> textures;
    std::shared_ptr<const rdr::OverlayList> list;
    bool changed = false;
    {
      std::lock_guard lock(overlay_mu);
      textures.swap(overlay_textures);
      if (overlay_changed) list = overlay_next, changed = true, overlay_changed = false;
    }
    for (const OverlayTexture& t : textures) renderer->SetOverlayTexture(t.id, t.width, t.height, t.rgba.data());
    if (changed) renderer->SetOverlay(std::move(list));
  }
  std::atomic<uint64_t> tweens{0}, tween_draws{0};  // S9b: halfway pictures drawn; draws mixed in them

  std::unique_ptr<rdr::Renderer> renderer;
  std::unique_ptr<SnapshotKeeper> keeper;  // S101 lane 35 (null unless options.crowd_ours)
  std::vector<uint8_t> shown_curve;  // render thread: the table the present applies (empty: identity), also applied to written pictures

  void Log(const char* line) const {
    if (options.log) options.log(line);
  }
  template <class B>
  void LogD3(const B& binder) const {   // lane 204 (O): the D3 census, readiness and record report (what fview prints at its end; session.cpp)
    if (d3::g_arena_d3) {   // lane 228: the arena's three lines only when the arena chain is on (the caster chain alone prints its own block below)
      Log(binder.D3Census().c_str());
      Log(binder.D3Readiness().c_str());
      Log(binder.D3RecordReport().c_str());
    }
    if (d3::g_caster_d3) Log(binder.D3CReport().c_str());   // lane 228 (the live seam, 17.12): the caster chain's census, readiness, reasons and the hooks' path histogram
    if (d3::g_ring_d3) Log(binder.D3RReport().c_str());     // lane 246 (the live seam, 16.9): the ring chain's census, readiness, reasons and the hook's path histogram
    if (d3::g_wrestlers_d3) Log(binder.D3WReportText().c_str());   // lane 245 (the live seam, 18.9): the wrestler chain's census, the verify line, the hooks' path histogram and the sphere offset census
  }

  bool WantPng(uint64_t guest_frame, uint64_t drawn_count) const {
    if (options.png_every && drawn_count % options.png_every == 0) return true;
    return std::find(options.png_frames.begin(), options.png_frames.end(), guest_frame) != options.png_frames.end();
  }

  void Run() {
    // S101 lane 35: the crowd's first promotion (declared before the binder: the binder goes first and points at it)
    std::unique_ptr<rdr_scene::CrowdOurs> crowd;
    if (options.crowd_ours) {
      rdr_scene::CrowdOursOptions co;
      co.game_root = options.crowd_game_root ? options.crowd_game_root() : std::string();
      co.floor = options.crowd_floor;  // S101 lane 82
      co.cards = options.crowd_cards;  // S101 lane CD
      co.cards_thin = options.crowd_cards && options.crowd_cards_thin;  // lane 175
      co.thin_offset = wwe2k14::card_thin::kHookToStreamOffset;
      co.thin_probe = options.crowd_cards && options.crowd_cards_thin_probe;   // SF2
      co.thin_counts = [](uint64_t f, uint32_t& seen, uint32_t& dropped, uint64_t& world) { return wwe2k14::card_thin::g_counts.Lookup(f, seen, dropped, world); };   // the hook's per-frame tally (review B1)
      co.member_probe = options.crowd_members_probe || options.crowd_members_thin;   // lane 272
      co.members_thin = options.crowd_members_thin;
      co.member_offset = wwe2k14::member_thin::kHookToStreamOffset;
      co.member_counts = [](uint64_t f, int site, uint32_t& seen, uint32_t& dropped, uint64_t& world) { return wwe2k14::member_thin::g_counts.Lookup(f, site, seen, dropped, world); };   // the hook's per-frame tally of the site
      co.verify_every = 30;  // the +1 pairing is checked on every 30th frame (a census count, never assumed)
      crowd = std::make_unique<rdr_scene::CrowdOurs>(*renderer, co);
    }
    struct CrowdReportAtEnd {
      std::unique_ptr<rdr_scene::CrowdOurs>& c;
      const Impl& impl;
      void Print() const {  // (lane 216: called by the final flush below; the destructor no longer prints, the flush runs first at every exit)
        if (c) impl.Log(("[rdr][crowd] " + c->Report()).c_str());
      }
    } crowd_report{crowd, *this};
    // S101 lane PO: the post chain's promotion (declared before the binder like the crowd's hook: the binder goes first and points at it). No scene record needed.
    std::unique_ptr<rdr_scene::PostOurs> post;
    if (options.post_ours) {
      rdr_scene::PostOursOptions po;
      po.limits.render_scale = options.render_scale;
      post = std::make_unique<rdr_scene::PostOurs>(*renderer, po);
    }
    // S101 lane 41: the in-place replace hooks (arena, wrestlers), made once here when their switch is on and a class registered its factory. The crowd's hook goes first (own seam).
    std::vector<rdr_scene::ReplaceHookHandle> replace_hooks;
    const auto make_replace = [&](bool on, const char* name, rdr_scene::ReplaceFactory factory) -> rdr_scene::ReplaceHookHandle* {
      if (!on) return nullptr;
      if (!factory) {
        Log((std::string("[rdr][") + name + "] the switch is on but no class registered (nothing is replaced)").c_str());
        return nullptr;
      }
      rdr_scene::ReplaceFactoryArgs args;
      args.renderer = renderer.get();
      args.game_root = options.crowd_game_root ? options.crowd_game_root() : std::string();
      args.live = true;
      args.low_ram = options.ui_low_ram;   // lane 126
      args.image_info = image_info.Supplier();  // S101 lane 47: the live supplier (fed where the binder syncs its images, below)
      args.image_content = image_content.Supplier();  // S101 lane 66: the UI's content supplier (fed before each frame's contents are applied, below)
      rdr_scene::ReplaceHookHandle handle = factory(args);
      if (!handle.hook) {
        Log((std::string("[rdr][") + name + "] not started: " + handle.error).c_str());
        return nullptr;
      }
      replace_hooks.push_back(std::move(handle));
      return &replace_hooks.back();
    };
    replace_hooks.reserve(std::size(rdr_scene::kReplaceClasses));  // (the pointers above stay valid: one slot per class in the table; a new class cannot outgrow it: lane 84 review)
    rdr_scene::InstallReplaceFactories();
    rdr_scene::ReplaceHookHandle* arena_hook = make_replace(options.arena_ours, "arena", rdr_scene::g_arena_factory);
    rdr_scene::ReplaceHookHandle* wrestlers_hook = make_replace(options.wrestlers_ours, "wrestlers", rdr_scene::g_wrestlers_factory);
    rdr_scene::ReplaceHookHandle* caw_hook = make_replace(options.caw_ours, "caw", rdr_scene::g_caw_factory);   // lane 153 (N3)
    rdr_scene::ReplaceHookHandle* ring_hook = make_replace(options.ring_ours, "ring", rdr_scene::g_ring_factory);  // S101 ring contract
    rdr_scene::ReplaceHookHandle* flash_hook = make_replace(options.flash_ours, "flash", rdr_scene::g_flash_factory);  // S101 flash contract
    rdr_scene::ReplaceHookHandle* screens_hook = make_replace(options.screens_ours, "screens", rdr_scene::g_screens_factory);  // S101 lane 115 (another pass site: order does not matter)
    rdr_scene::ReplaceHookHandle* crowd_props_hook = make_replace(options.crowd_props_ours, "crowd_props", rdr_scene::g_crowd_props_factory);  // S101 lane 133 (the crowd loops' own call sites: order does not matter)
    rdr_scene::ReplaceHookHandle* props_hook = make_replace(options.props_ours, "props", rdr_scene::g_props_factory);  // S101 lane 84 (after flash, before ui)
    rdr_scene::ReplaceHookHandle* shadow_hook = make_replace(options.shadow_casters_ours, "shadow_casters", rdr_scene::g_shadow_casters_factory);  // S101 lane 99 (after props, before ui)
    rdr_scene::ReplaceHookHandle* movie_hook = make_replace(options.movie_ours, "movie", rdr_scene::g_movie_factory);  // S101 lane 131 (its own call site; before ui)
    rdr_scene::ReplaceHookHandle* rim_hook = make_replace(options.rim_ours, "rim_map", rdr_scene::g_rim_factory);  // lane 124 (after the shadow casters, before ui)
    rdr_scene::ReplaceHookHandle* circles_hook = make_replace(options.circles_ours, "circles", rdr_scene::g_circles_factory);  // lane 136 (after the rim map: its site is the circle call, no colour hook claims it)
    rdr_scene::ReplaceHookHandle* plane_hook = make_replace(options.plane_shadow_ours, "plane_shadow", rdr_scene::g_plane_shadow_factory);  // lane 139 (after the circles and the shadow casters: it takes the plane draws they pass on)
    rdr_scene::ReplaceHookHandle* small_passes_hook = make_replace(options.small_passes_ours, "small_passes", rdr_scene::g_small_passes_factory);  // S101 lane 156 review: lane 135's class was in the factory table but never made live (its switch only logged "switch on"); call sites of its own, order does not matter
    rdr_scene::ReplaceHookHandle* rim_chain_hook = make_replace(options.rim_chain_ours, "rim_chain", rdr_scene::g_rim_chain_factory);  // lane 151 (its own call sites 0x82C1F48C / 0x82814080; no colour hook claims them; before ui)
    rdr_scene::ReplaceHookHandle* ui_text_hook = make_replace(options.ui_text_ours, "ui_text", rdr_scene::g_ui_text_factory);  // S101 lane 89 (UT)
    rdr_scene::ReplaceHookHandle* ui_hook = make_replace(options.ui_ours, "ui", rdr_scene::g_ui_factory);  // S101 UI contract (lane US feeds args.image_content)
    bool wants_content = false;   // S101 lane 66: some registered hook reads the content table (the UI, the ring, the flashes): only then is it fed (no hashing with every switch off)
    for (const rdr_scene::ReplaceHookHandle& h : replace_hooks) wants_content = wants_content || h.wants_content;
    Binder binder(*renderer);
    if (crowd) binder.SetCrowdHook(crowd.get());
    if (post) binder.SetPostHook(post.get());  // S101 lane PO
    if (d3::g_arena_d3) binder.D3SetRecordRoute(d3::kRecordRouteBuilt, d3::g_verify, -1);   // lane 152: Mode::Record is the live default; the route is on only when built; the verify arm by its switch
    if (d3::g_caster_d3) {   // lane 228 (the live seam, 17.12): the caster chain on the live globals; D3CSetHooks stays uncalled on purpose (slots_used is built into the binder, palette_count is an optional override, the geometry test defaults to ready)
      binder.SetD3CasterLive(d3::kRecordRouteBuilt, d3::g_caster_verify, d3::g_caster_plane);
      Log(d3::g_caster_verify ? "[rdr][d3][casters] live chain on (VERIFY arm: markers written, the game's passes always run)" : "[rdr][d3][casters] live chain on (skip arm: a proven pass on cache path 1 is replaced by a marker)");
    }
    if (d3::g_ring_d3) {   // lane 246 (the live seam, 16.9): the ring chain on the ring-only live globals (d3_arena.h g_rpackets / g_rmarks / g_rgate / g_rtally); D3RSetHooks stays uncalled (the palette size is learned from the recorded-in-full frames' parts)
      binder.SetD3RingLive(d3::kRecordRouteBuilt, d3::g_ring_verify);
      Log(d3::g_ring_verify ? "[rdr][d3][ring] live chain on (VERIFY arm: markers written, the game's ring calls always run)" : "[rdr][d3][ring] live chain on (skip arm: a proven ring call on cache path 1 with its ring rows recorded is replaced by a marker)");
    }
    if (d3::g_wrestlers_d3) {   // lane 245 (the live seam, 18.9): the wrestler chain on its own live globals (g_w*), never the arena's / the ring's / the casters'
      binder.SetD3WrestlersLive(d3::kRecordRouteBuilt, d3::g_wrestlers_verify);
      Log(d3::g_wrestlers_verify ? "[rdr][d3][wrestlers] live chain on (VERIFY arm: markers written, the game's colour calls always run)" : "[rdr][d3][wrestlers] live chain on (skip arm: a proven wrestler call on cache path 1 is replaced by a marker)");
    }
    struct PostReportAtEnd {  // S101 lane PO (declared after the binder: it goes first, while the binder and the hook are alive)
      Binder& binder;
      const Impl& impl;
      bool on;
      void Print() const {
        if (on) impl.Log(binder.PostCensus().c_str());
      }
    } post_report{binder, *this, post != nullptr};
    struct D3ReportAtEnd {  // lane 204 (O): the D3 census, readiness and record lines at exit too (K1 had them only in fview); also every 600 drawn frames (below)
      Binder& binder;
      const Impl& impl;
      void Print() const {
        if (d3::AnyD3Chain()) impl.LogD3(binder);
      }
    } d3_report{binder, *this};
    if (arena_hook) binder.AddReplaceHook(arena_hook->hook.get(), &g_arena_ours);
    if (wrestlers_hook) binder.AddReplaceHook(wrestlers_hook->hook.get(), &g_wrestlers_ours);
    if (ring_hook) binder.AddReplaceHook(ring_hook->hook.get(), &g_ring_ours);  // after the arena and the wrestlers (both pass ring objects on as NotOurs)
    if (flash_hook) binder.AddReplaceHook(flash_hook->hook.get(), &g_flash_ours);  // S101 flash contract
    if (screens_hook) binder.AddReplaceHook(screens_hook->hook.get(), &g_screens_ours);  // S101 lane 115
    if (crowd_props_hook) binder.AddReplaceHook(crowd_props_hook->hook.get(), &g_crowd_props_ours);  // S101 lane 133
    if (props_hook) binder.AddReplaceHook(props_hook->hook.get(), &g_props_ours);  // S101 lane 84: after the ring (it passes these objects on as NotOurs)
    if (caw_hook) binder.AddReplaceHook(caw_hook->hook.get(), &g_caw_ours);   // lane 153: AFTER the wrestlers, the ring and the props (review B1)
    if (shadow_hook) binder.AddReplaceHook(shadow_hook->hook.get(), &g_shadow_casters_ours);  // S101 lane 99: the shadow passes' sites (no colour hook claims them); its joins read the wrestlers' and the ring's hooks
    if (movie_hook) binder.AddReplaceHook(movie_hook->hook.get(), &g_movie_ours);  // S101 lane 131: before the UI (which would keep the movie quad)
    if (rim_hook) binder.AddReplaceHook(rim_hook->hook.get(), &g_rim_ours);  // lane 124: the rim light map's site (no colour hook claims it); its joins read the wrestlers' hook
    if (circles_hook) binder.AddReplaceHook(circles_hook->hook.get(), &g_circles_ours);  // lane 136: the circle blob shadows (call 0x828131E4)
    if (plane_hook) binder.AddReplaceHook(plane_hook->hook.get(), &g_plane_shadow_ours);  // lane 139: the plane mask (site 0x82BF4D38) and blur (call 0x827EEC84) constants
    if (rim_chain_hook) binder.AddReplaceHook(rim_chain_hook->hook.get(), &g_rim_chain_ours);  // lane 151: the rim light map's reductions and blur (needs no scene record, no game file)
    if (small_passes_hook) binder.AddReplaceHook(small_passes_hook->hook.get(), &g_small_passes_ours);  // S101 lane 213 finding: lane 156 made the hook but never added it here, so the class was never asked (its own call sites: order does not matter)
    if (ui_text_hook) binder.AddReplaceHook(ui_text_hook->hook.get(), &g_ui_text_ours);  // S101 lane 89 (UT): before the UI textures' hook (a glyph draw ours laid out is not offered on)
    if (ui_hook) binder.AddReplaceHook(ui_hook->hook.get(), &g_ui_ours);  // S101 UI contract: last
    struct ReplaceReportAtEnd {  // (declared after the binder: it goes first, while the binder and the hooks are alive)
      Binder& binder;
      const Impl& impl;
      std::vector<rdr_scene::ReplaceHookHandle*> hooks;
      // S101 lane 83: the binder's cumulative replace-check ms per hook, for the [cost] line (live thread; one load when the switch is off).
      void PublishCost() const {
        if (!wwe2k14::cost::On()) return;
        for (rdr_scene::ReplaceHookHandle* h : hooks)
          if (const ReplaceCounters* c = binder.replace_counters(h->hook->Name())) wwe2k14::cost::PublishReplaceMs(wwe2k14::cost::ReplaceClassOf(h->hook->Name()), c->ms);
      }
      // S101 lane 216: the hooks' own end reports (the census lines themselves come from Binder::FlushCensus, same text as a window line).
      void Print() const {
        PublishCost();
        if (const std::string cost_line = wwe2k14::cost::FinalLine(wwe2k14::cost::TicksPerNs()); !cost_line.empty()) impl.Log(cost_line.c_str());
        for (rdr_scene::ReplaceHookHandle* h : hooks)
          if (h->report) impl.Log(("[rdr][" + std::string(h->hook->Name()) + "] " + h->report()).c_str());
      }
    } replace_report{binder, *this, {}};
    for (rdr_scene::ReplaceHookHandle* h : {arena_hook, wrestlers_hook, caw_hook /* lane 153 (review S4) */, ring_hook, flash_hook, props_hook, shadow_hook, screens_hook, crowd_props_hook, movie_hook, rim_hook /* lane 133 (rim, lane 124's omission) */, circles_hook /* lane 136 */, plane_hook /* lane 139 */, small_passes_hook /* lane 156 review */, rim_chain_hook /* lane 151 */, ui_text_hook, ui_hook})  // S101 lane 84 / 89 / 115
      if (h) replace_report.hooks.push_back(h);
    // S101 lane 216: every class's FINAL census line, once at the end of every normal run. The process hard-exits ("Title terminated; hard-exiting process"),
    // so the destructors never ran and a scene shorter than one 600-frame window (the victory scene, the last seconds of a match) printed nothing. This runs on
    // the render thread: from the SDK's quit flush (rex::RunQuitFlushes, before TerminateTitle), from the black box's exit zip (before it reads the log), and
    // from the destructor below on an orderly stop. Idempotent: it prints again only when frames were handled since the last print. Never throws.
    struct FinalCensus {
      Impl& impl;
      Binder& binder;
      std::function<void()> reports;  // the hooks' own reports, the crowd's, the post chain's, the D3 lines
      uint64_t printed_at = ~0ull;
      void Run(const char* why) {
        try {
          const uint64_t now = impl.drawn.load(std::memory_order_relaxed) + impl.skipped.load(std::memory_order_relaxed);
          if (now == printed_at) return;
          printed_at = now;
          impl.Log((std::string("[rdr][end] end of run (") + why + "): the lines below are each class's final census, same shape as a window line").c_str());
          binder.FlushCensus();
          if (reports) reports();
          if (impl.renderer) impl.renderer->FlushPipelineStore();  // lane 276: the process hard-exits, ~Renderer never runs: the pipeline list / cache are saved here (repeatable)
        } catch (...) {
        }
      }
      ~FinalCensus() { Run("stop"); }
    };
    FinalCensus final_census{*this, binder, [&] { replace_report.Print(); crowd_report.Print(); post_report.Print(); d3_report.Print(); }};
    rdr::FrameBuilder builder;
    LiveSource source;
    LoadedTexture content_scratch;  // S101 lane 66
    source.mu = &store_mu;
    source.geometry = &geometry;
    source.textures = &textures;
    int announced_seen = -1;   // S101 lane 271: the arena the match card named, as last handed to the per-arena loads (a change is handed over once)
    for (;;) {
      replace_report.PublishCost();  // S101 lane 83
      Message message;
      size_t waiting = 0;
      bool have_message = false;
      std::filesystem::path now_path;  // S101 lane 19: a request for the last presented picture, served without a new frame
      {
        std::unique_lock lock(mail_mu);
        // (lane 216: a timed wait, so a quit request from another thread is seen even while no frame comes)
        mail_cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping || !mail.empty() || !last_picture_path.empty() || quit_req.load(std::memory_order_acquire); });
        now_path.swap(last_picture_path);
        if (quit_req.exchange(false, std::memory_order_acq_rel)) {
          lock.unlock();
          final_census.Run("route quit / title termination");
          quit_ack.store(true, std::memory_order_release);
          lock.lock();
        }
        if (mail.empty() && now_path.empty()) {
          if (stopping) return;  // stopping, nothing left
          continue;
        }
        if (!mail.empty()) {
          message = std::move(mail.front());
          mail.pop_front();
          waiting = mail.size();
          have_message = true;
        }
      }
      if (!now_path.empty()) {
      WritePictureTo(binder, now_path);
      // S101 lane 1 (review): say why a requested last picture is missing, instead of only "NO PICTURE" in the zip line 5 s later.
      std::error_code ec;
      if (!std::filesystem::exists(now_path, ec))
        Log(binder.last_present().source.image == rdr::kNoImage ? "[rdr][live] WARN last picture not written: nothing presented yet"
                                                               : "[rdr][live] WARN last picture not written: the presented image is not readable");
    }
      if (!have_message) continue;  // only the picture request
      room_cv.notify_all();  // (a paced game waits for room: WaitForRoom)
      // A named picture is reported done however this frame ends (a route's quit waits for it).
      struct PictureDone {
        bool named;
        Impl* self;
        ~PictureDone() {
          if (named) self->NamedPictureDone();  // lane 302: after the queued picture writes (see pic_q)
        }
      } picture_done{!message.picture.empty(), this};
      // Images first: a frame never names an image the table lacks. Releases after the frame that last used them (they came with a newer one).
      if (message.has_images) {
        source.records = std::move(message.images);
        image_info.Set(source.records.data(), source.records.size());  // S101 lane 47
        binder.SyncImages(source);
      }
      if (message.display_curve.size() == 1024) {
        renderer->SetDisplayCurve(message.display_curve.data());
        shown_curve = std::move(message.display_curve);
        Log("[rdr][live] display table from the game applied");
      }
      const auto t_start = std::chrono::steady_clock::now();
      LoadedFrame frame;
      frame.bytes = std::move(message.frame);
      if (!CaptureReader::ParseFrame(frame)) {
        rdr::Problem("live_frame_unreadable", "guest frame %llu", static_cast<unsigned long long>(message.guest_frame));
        continue;
      }
      // Contents always land, drawn or not: an image written once (a file texture) is never sent again.
      if (wants_content) FeedImageContents(frame, LiveFeedSource{source}, image_content, content_scratch);  // S101 lane 66: before ApplyContents takes the store's bytes
      if (d3::AnyD3Chain() && message.d3_record) {   // lane 246: + the ring chain (D3AddRecord feeds its core too);   // lane 228: the caster chain's records come the same way (D3AddRecord feeds both cores); lane 152 (D3-F, review B1): the present's own record feeds the record route's light / world sources, joined by the hook's stamp (stream frame j needs stamp j-1, written in record j)
        binder.D3AddRecord(message.d3_record);
        binder.D3EvictRecords(message.guest_frame);
      }
      if (d3::g_arena_d3) {
        const std::string latch = binder.D3TakeLatchMessage();   // lane 152 (B2)
        if (!latch.empty()) { Log(latch.c_str()); if (options.on_d3_latch) options.on_d3_latch(latch.c_str()); }   // lane 301: the case ring takes the moment
      }
      if (d3::g_caster_d3) {   // lane 228: the caster gate's latch message (D3CEndFrame)
        const std::string latch = binder.D3CTakeLatchMessage();
        if (!latch.empty()) { Log(latch.c_str()); if (options.on_d3_latch) options.on_d3_latch(latch.c_str()); }   // lane 301: the case ring takes the moment
      }
      if (d3::g_ring_d3) {   // lane 246: the ring gate's latch message (D3REndFrame; it also clears g_ring_hook_on)
        const std::string latch = binder.D3RTakeLatchMessage();
        if (!latch.empty()) { Log(latch.c_str()); if (options.on_d3_latch) options.on_d3_latch(latch.c_str()); }   // lane 301: the case ring takes the moment
      }
      if (d3::g_wrestlers_d3) {   // lane 245: the wrestler gate's latch message (D3WEndFrame)
        const std::string latch = binder.D3WTakeLatchMessage();
        if (!latch.empty()) { Log(latch.c_str()); if (options.on_d3_latch) options.on_d3_latch(latch.c_str()); }   // lane 301: the case ring takes the moment
      }
      binder.ApplyContents(frame, source);
      {
        std::vector<std::vector<uint8_t>> freed;  // lane 273: texture bytes are freed after the lock (the game thread's Append waits on store_mu)
        {
          std::lock_guard lock(store_mu);
          struct Hold3 { StoreTimer t{3}; ~Hold3() { t.Stop(); } } hold3;
          for (const ContentRef& ref : frame.contents) {
            if (auto it = textures.find(ref.offset); it != textures.end()) {
              freed.push_back(std::move(it->second));
              textures.erase(it);
            }
          }
        }
      }
      window.parse += Ns(t_start);
      ++window.frames;
      // Lane 276 (SKIP_GAME_WORK_MAP 19.10): the listed pipelines are built in the menus / loading only. A scene is live while this frame has a scene record or the match card names an arena (the in-game sequence); the renderer then builds nothing from the list in slices (what is left stays on demand).
      renderer->SetPipelinePrebuildAllowed(!message.record && !(options.announced_arena && options.announced_arena() >= 0));
      if (arena_hook || wrestlers_hook || caw_hook || ring_hook || flash_hook || props_hook || shadow_hook || screens_hook || crowd_props_hook || rim_hook /* lane 133 (rim, lane 124's omission) */ || circles_hook /* lane 136 */ || plane_hook /* lane 139 */ || ui_text_hook || ui_hook) {  // S101 lane 115: screens_hook  // S101 lane 41: the same record (shared, read only) for the replace hooks; the game folder may only be known now
        for (rdr_scene::ReplaceHookHandle* h : {arena_hook, wrestlers_hook, caw_hook /* lane 153 (review S4) */, ring_hook, flash_hook, props_hook, shadow_hook, screens_hook, crowd_props_hook, movie_hook, rim_hook /* lane 133 (rim, lane 124's omission) */, circles_hook /* lane 136 */, plane_hook /* lane 139 */, small_passes_hook /* lane 156 review */, rim_chain_hook /* lane 151 */, ui_text_hook, ui_hook}) {  // S101 lane 84 / 89 / 115
          if (!h) continue;
          if (h->set_game_root) {
            if (const std::string root = options.crowd_game_root ? options.crowd_game_root() : std::string(); !root.empty()) h->set_game_root(root);
          }
          if (h->set_record) h->set_record(message.record);
        }
      }
      if (crowd) {  // S101 lane 35: the record this frame's crowd is drawn from (null: none yet, the stream's crowd stays); the game folder may only be known now
        if (const std::string root = options.crowd_game_root ? options.crowd_game_root() : std::string(); !root.empty()) crowd->SetGameRoot(root);
        crowd->SetRecord(std::move(message.record));
      }
      if (const int announced = options.announced_arena ? options.announced_arena() : -1; announced != announced_seen) {   // S101 lane 271 (SKIP_GAME_WORK_MAP 19.8)
        announced_seen = announced;
        if (announced >= 0) {   // the match card names the arena before any record does: every per-arena load starts on its worker now (the game folder was handed over above)
          for (rdr_scene::ReplaceHookHandle* h : {arena_hook, ring_hook, flash_hook, props_hook, screens_hook, crowd_props_hook})
            if (h && h->announce_arena) h->announce_arena(announced);
          if (crowd) crowd->Announce(announced);
          rdr::Log("[rdr][live] match card names arena %d: the per-arena loads start now", announced);
        }
      }
      PollAsync();  // lane 232: read-back copies whose frame's fence has signalled (never waits)
      if (message.read) {  // a frame's first part, split at a copy the game reads back (DrawAndRead): drawn, kept, read, never shown
        ReadRequest& request = *message.read;
        request.taken = std::chrono::steady_clock::now();
        BindStats stats;
        builder.Begin(message.guest_frame);
        binder.frame_part_open = true;  // S101 lane 50: more of this guest frame follows
        binder.Bind(frame, source, builder, stats);
        binder.frame_part_open = false;
        rdr::Frame part = builder.Finish();
        part.keep_contents = true;
        renderer->RenderFrame(part);
        request.drawn = std::chrono::steady_clock::now();
        draws += stats.draws;
        bound += stats.bound;
        no_shader += stats.no_shader;
        ++parts;
        rdr::ImageRef ref;
        ref.image = binder.Mapped(request.image);
        ref.mip = uint8_t(request.mip);
        ref.layer = uint8_t(request.layer);
        std::vector<uint8_t> rgba;
        uint32_t width = 0, height = 0;
        const bool ok = ref.image != rdr::kNoImage && renderer->ReadImage(ref, rgba, width, height);
        request.read = std::chrono::steady_clock::now();
        {
          std::lock_guard lock(request.mu);
          request.ok = ok;
          request.rgba = std::move(rgba);
          request.width = width;
          request.height = height;
          request.done = true;
        }
        request.cv.notify_all();
        continue;
      }
      streaks.by_image = options.perframe_by_image;
      streaks.Note(frame);
      if (wwe2k14::caw::g_on.load(std::memory_order_relaxed))  // S101 lane 118: `!caw on` - every frame the sink parses (drawn or skipped), whole frames only (a read-back part returned above)
        wwe2k14::caw::OnFrame(frame, [&source](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return source.ReadGeometry(offset, bytes, out); });
      // S100 (strip-360 A1): a frame is skipped when newer frames wait (latest wins), never so long that nothing reaches the screen.
      const bool starved = options.window && last_present != std::chrono::steady_clock::time_point{} &&
                           std::chrono::steady_clock::now() - last_present > std::chrono::milliseconds(100);
      // Settings parity S9 (Jarrod 08:1x: "120fps absolutely stays as long as a decouplement is there properly, same for 30fps"): the 30
      // fps mode keeps the game's 60 updates and draws a frame once draw_vblanks vblanks of presents have passed; the frames between are
      // handled as skipped ones (their lasting work runs, their state steps are caught up by the drawn frame: A1b).
      bool rate_skip = false;
      if (options.draw_vblanks > 1 && message.picture.empty()) {
        vblank_debt += std::max<uint32_t>(1, frame.header.present_interval);
        if (vblank_debt < options.draw_vblanks) {
          rate_skip = true;
        } else {
          vblank_debt = 0;
        }
      }
      if ((waiting >= options.max_queued || rate_skip) && message.picture.empty() && !starved) {  // a named picture is always drawn
        ++skipped;
        // S100: a skipped frame still does the work that lasts beyond it (copies into textures and the passes they need: an attire's
        // build and pack during a load, when most frames are skipped; Heenan's jacket came out black without it, run 6911). Strip-360 A1:
        // only that: a copy into the same image and rectangle in each of the last kPerFrameStreak frames handled is per-frame work (shadow
        // maps, reflections, the post chain: ~43 a frame in a match) that the drawn frame redoes, so it is left out; with nothing lasting
        // left the frame costs its contents only. (Runs 6951 / Surface 177: lasting work ran ~2-3 ms a skipped frame, the queue never
        // drained and the screen starved.)
        // A per-frame copy whose image this frame reads before the copy writes it carries state from frame to frame (an exposure or
        // adaptation value nudged each frame). Left out, that state advances on drawn frames only (run 6952: 1-2 levels darker in an
        // outrun fast-forward); kept (`keep_state_copies`), the frame's whole scene feeds it and the skip saves nothing (run 6953: 10.6
        // frames a second shown). Default: left out (a GPU that cannot keep up drops frames, never slow motion: Jarrod); checks pace the
        // game instead (--wwe2k14_rdr_pace_game). (Two images used in turn are written every other frame: never per-frame by the streak.)
        bool lasting = false;
        streaks.images = &source.images();   // lane 307: kUsageLasting destinations are never dropped by the image rule
        streaks.Classify(frame, options.keep_state_copies, lasting_seed, [&](const rdr::CopyOp& c) {
          ++per_frame_copies_left;
          NoteMissed(c);  // A1b: the drawn frame catches its state step up
        });
        for (uint8_t v : lasting_seed) lasting |= v != 0;
        // lane 307: bind only what the lasting copies need (lasting_prune.h); `dropped_ops` is the rest (their D3 packets are discarded below, as a skipped frame's)
        lasting::Stats prune{};
        if (lasting && options.prune) {
          pruner.Select(frame, lasting_seed, lasting_ops, dropped_ops, prune, options.prune_plant);
        } else if (lasting) {   // --wwe2k14_rdr_skip_prune=false: the pre-lane-307 bind (every op but the per-frame copies)
          lasting_ops.clear();
          dropped_ops.clear();
          for (uint32_t i = 0; i < frame.ops.count; ++i) {
            if (frame.ops[i].kind == OpKind::Copy && !lasting_seed[i]) dropped_ops.push_back(frame.ops[i]);
            else lasting_ops.push_back(frame.ops[i]);
          }
          prune.ops = frame.ops.count, prune.kept_ops = uint32_t(lasting_ops.size());
          prune.draws = prune.kept_draws = frame.draws.count;
        }
        // lane 204 (P, Check K1): the D3 packets of a skipped frame are discarded ONLY when no lasting bind follows. A lasting bind keeps every draw, the markers included, and its BuildMini takes the packets;
        // discarding them first (as lane 152 did) made every marker of that bind a hole (run 7093: 346 skipped frames ran lasting work). Uncounted either way (not `unconsumed`, which would latch D3 off).
        if (lasting && d3::AnyD3Chain() && !dropped_ops.empty()) {   // lane 307: the packets of the markers left out of the lasting bind are discarded (uncounted); the kept ones are taken by the bind
          const Table<Op> all = frame.ops;
          frame.ops = Table<Op>{dropped_ops.data(), uint32_t(dropped_ops.size())};
          binder.D3FrameSkipped(frame);
          frame.ops = all;
        }
        if (lasting) {   // lane 307 census
          ++prune_frames;
          prune_ops_in += prune.ops, prune_ops_kept += prune.kept_ops, prune_draws_in += prune.draws, prune_draws_kept += prune.kept_draws;
          prune_passes_in += prune.passes, prune_passes_kept += prune.kept_passes, prune_grouped += prune.grouped_kept, prune_seeds += prune.seeds, prune_promoted += prune.promoted_copies, prune_blind += prune.d3_blind() ? 1 : 0;
          window.prune_draws_in += prune.draws, window.prune_draws_kept += prune.kept_draws;
        }
        if (!lasting && d3::AnyD3Chain()) binder.D3FrameSkipped(frame);   // lane 246: + the ring's markers (D3FrameSkipped already takes them)   // lane 228: a skipped frame's caster packets are discarded the same way (D3FrameSkipped takes both classes' markers)
        if (lasting) {
          const auto t_kept = std::chrono::steady_clock::now();
          const Table<Op> all_ops = frame.ops;
          frame.ops = Table<Op>{lasting_ops.data(), uint32_t(lasting_ops.size())};
          BindStats stats;
          builder.Begin(message.guest_frame);
          binder.frame_part_open = true;  // S101 lane 50: a persistent-only pass is no guest frame for the crowd gate
          binder.persist_only_bind = true;  // S101 lane 224: the post hook plans nothing on it (its per-frame copies are left out: every chain input would read "not written this frame")
          binder.d3_skip_observe = prune.d3_blind();   // lane 307: no object pass kept: the D3 / thin begin and end do not run (as a skip with no bind)
          binder.Bind(frame, source, builder, stats);
          binder.d3_skip_observe = false;
          binder.frame_part_open = false;
          binder.persist_only_bind = false;
          QueueAsync(frame, binder, message.guest_frame, message.seq);  // lane 232: a demanded picture is read after this frame too (its lasting copies are in it)
          frame.ops = all_ops;
          renderer->RenderFrame(builder.Finish(), rdr::RenderMode::PersistentOnly);
          ++persistent_only;
          window.kept += Ns(t_kept);
          ++window.kept_frames;
        }
      } else {
        if (starved && waiting >= options.max_queued) ++starved_draws;
        const auto t_bind = std::chrono::steady_clock::now();
        BindStats stats;
        builder.Begin(message.guest_frame);
        {
          // A1b: the state steps the skipped frames before this one left out, caught up here (CatchUp). The isolates below bind the
          // frame as it came (no second catch-up).
          const Table<Op> all_ops = frame.ops;
          if (options.catch_up_state && CatchUp(frame)) frame.ops = Table<Op>{catch_up_ops.data(), uint32_t(catch_up_ops.size())};
          binder.Bind(frame, source, builder, stats);
          frame.ops = all_ops;
        }
        const rdr::Frame built = builder.Finish();
        QueueAsync(frame, binder, message.guest_frame, message.seq);  // lane 232: a watched picture this frame copies into is read back by this frame's own command buffer
        const auto t_record = std::chrono::steady_clock::now();
        ApplyOverlay();  // settings parity S11
        // Settings parity S9b (the decoupled 120): the halfway picture between the last drawn tick and this one first (it does this tick's
        // lasting work: Full), then this tick's own picture (DisplayOnly). FIFO at the display's 120 Hz shows them a vblank apart.
        // A named picture also writes the halfway one as <name>_half (checks: the in-between is otherwise never seen in a picture); the
        // named picture itself is then this tick's DisplayOnly redraw.
        // (S9b fix, Jarrod's phone test: a screen or GPU that cannot keep up made the game unsteady: no halfway picture while a newer
        // frame already waits, so the in-betweens drop first.)
        if (options.tween && tween.valid() && built.presents.count && (waiting == 0 || !message.picture.empty())) {
          uint32_t matched = 0;
          renderer->RenderFrame(tween.Blend(built, 0.5f, matched));
          if (!message.picture.empty()) WritePicture(binder, message.picture + "_half");
          renderer->RenderFrame(built, rdr::RenderMode::DisplayOnly);
          ++tweens;
          tween_draws += matched;
        } else if (options.dlss_mode >= 0 && built.presents.count) {
          // S10b: the previous tick's vertex constants for the DLSS velocity draws (theirs where the binder finds them in it, else this
          // tick's: no motion of their own on a draw's first tick).
          rdr::Frame with_prev = built;
          uint32_t matched = 0;
          with_prev.prev_constants = motion_ticks.valid() ? motion_ticks.Blend(built, 0.0f, matched).constants : built.constants;
          renderer->RenderFrame(with_prev);
          motion_ticks.Keep(built);
        } else {
          renderer->RenderFrame(built);
        }
        if (options.tween && built.presents.count) tween.Keep(built);
        if (options.window && built.presents.count) NotePresent();  // S100: a frame that reached our swapchain (owner mode)
        {  // S101 lane 19 (review fix 6): timing.csv, taken before the photo-mode PNG write: this frame's bind + RenderFrame (record + submit + any wait the renderer does inside it, e.g. FIFO), and the gap since the previous picture reached the swapchain
          const double render_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_bind).count();
          case_hook::OnRenderTiming(message.guest_frame, render_ms, options.window && built.presents.count ? present_gap_ms : -1.0);
        }
        if (built.presents.count) ServePhoto(binder);  // photo mode: the picture just shown
        window.bind += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t_record - t_bind).count());
        window.record += Ns(t_record);
        window.draws += stats.draws;
        ++window.drawn;
        const uint64_t count = ++drawn;
        draws += stats.draws;
        bound += stats.bound;
        no_shader += stats.no_shader;
        if (!message.picture.empty()) {
          WritePicture(binder, message.picture);
          // The same frame again with only one effect's draws (or all but it, "!NAME"): which effect paints what (S100 Heenan's jacket).
          for (const std::string& name : options.picture_isolates) {
            BindStats only;
            only.isolate = name.c_str();
            builder.Begin(message.guest_frame);
            binder.Bind(frame, source, builder, only);
            renderer->RenderFrame(builder.Finish());
            std::string label = message.picture + "_" + (name[0] == '!' ? "not_" + name.substr(1) : name);
            for (char& ch : label) {
              if (ch == '|') ch = '+';
            }
            WritePicture(binder, label);
          }
        }
        else if (WantPng(message.guest_frame, count)) WritePicture(binder, std::to_string(message.guest_frame));
      }
      for (uint32_t id : message.released) binder.ReleaseImage(id);
      if (drawn % 600 == 0 && drawn && drawn != last_logged) {  // once per 600 drawn (skipped frames leave `drawn` where it was)
        if (options.tween) {
          char tween_line[160];
          std::snprintf(tween_line, sizeof(tween_line), "[rdr][live][tween] %llu halfway pictures, %.0f draws mixed in each", static_cast<unsigned long long>(tweens.load()),
                        tweens.load() ? double(tween_draws.load()) / double(tweens.load()) : 0.0);
          Log(tween_line);
        }
        last_logged = drawn;
        if (d3::AnyD3Chain()) LogD3(binder);   // lane 204 (O); lane 228: the caster block too; lane 246: the ring block
        std::string live_line = "[rdr][live] " + Report();   // lane 307: a std::string (the census made this line longer than the old 512 byte buffer)
        {  // lane 293: the missing-geometry count in every 600-frame line (a sign board read as zeros shows here, not only at the end of the run)
          rdr::ProblemCount counts[64];
          const int n = rdr::ProblemCounts(counts, 64);
          unsigned long long missing = 0;
          for (int i = 0; i < n && i < 64; ++i) {
            if (counts[i].name && std::strcmp(counts[i].name, "live_geometry_missing") == 0) missing = counts[i].count;
          }
          live_line += "; live_geometry_missing " + std::to_string(missing);
        }
        Log(live_line.c_str());
        char line[640];
        // S100 speed: this thread's time over the window, per frame handled (parse + contents), per frame drawn (bind, record), per
        // skipped frame whose lasting work ran.
        const double f = window.frames ? double(window.frames) : 1.0, d = window.drawn ? double(window.drawn) : 1.0;
        const double k = window.kept_frames ? double(window.kept_frames) : 1.0;
        const double seconds = double(Ns(window.start)) / 1e9;
        std::snprintf(line, sizeof(line),
                      "[rdr][live][time] %llu frames handled (%llu drawn, %llu skipped with lasting work) in %.2f s (%.0f drawn a second): ms a "
                      "frame: parse + contents %.3f; drawn: bind %.3f (%.0f draws, %.2f us a draw), record + submit %.3f; skipped: lasting work %.3f; lasting binds kept %llu of %llu draws",
                      static_cast<unsigned long long>(window.frames), static_cast<unsigned long long>(window.drawn),
                      static_cast<unsigned long long>(window.kept_frames), seconds, seconds > 0 ? double(window.drawn) / seconds : 0.0,
                      window.parse / f / 1e6, window.bind / d / 1e6, window.draws / d,
                      window.draws ? window.bind / double(window.draws) / 1e3 : 0.0, window.record / d / 1e6, window.kept / k / 1e6,
                      static_cast<unsigned long long>(window.prune_draws_kept), static_cast<unsigned long long>(window.prune_draws_in));
        Log(line);
        window = Window{};
      }
    }
  }

  // Lane 232 (3d a2): BEFORE a drawn frame is rendered, each watched image (async.Images) that one of its copies writes (dest by the reader's id) is queued for a read-back at the end of that frame's own
  // command buffer (Renderer::QueueReadback: a persistent staging buffer, no drain), at most every `every_frames` guest frames (AsyncReadback::Due). PollAsync (the next message) hands the finished ones
  // (their frame's fence signalled) to the mailbox. Render thread only; one atomic-free mutex lookup when nothing is watched. In tween mode the copy goes into the first RenderFrame of the tick (the blend,
  // which does the lasting work), so the DisplayOnly redraw after it changes nothing.
  void QueueAsync(const LoadedFrame& frame, Binder& binder, uint64_t guest_frame, uint64_t seq) {
    if (!async.AnyWatched()) return;
    const auto t0 = std::chrono::steady_clock::now();
    async.Images(async_images);
    for (uint32_t image : async_images) {
      if (async.DemandOnly(image)) {  // deferred delivery (the replay thumbnail): read at the end of the first frame handled after the demand, wherever its copy was
        async.Due(image, guest_frame, async_due, seq);
        for (const AsyncReadback::Key& key : async_due) {
          rdr::ImageRef ref;
          ref.image = binder.Mapped(key.image);
          ref.mip = uint8_t(key.mip);
          ref.layer = uint8_t(key.layer);
          const uint64_t tag = ++async_tag;
          if (ref.image != rdr::kNoImage && renderer->QueueReadback(ref, tag)) async_tags[tag] = {key, guest_frame, seq};
        }
        continue;
      }
      // SF-2: only a copy into the picture from the SAME source the first blocking lock saw (a shared image may be written by other targets' copies too).
      const uint32_t want_source = async.SourceOf(image);
      uint32_t copy_source = 0;
      bool copied = false;
      for (uint32_t i = 0; i < frame.ops.count && !copied; ++i) {
        const Op& op = frame.ops[i];
        if (op.kind != OpKind::Copy || uint32_t(frame.copies[op.index].dest.image) != image) continue;
        if (want_source && uint32_t(frame.copies[op.index].source.image) != want_source) continue;
        copied = true;
        copy_source = uint32_t(frame.copies[op.index].source.image);
      }
      if (!copied) continue;
      async.Due(image, guest_frame, async_due);
      for (const AsyncReadback::Key& key : async_due) {
        rdr::ImageRef ref;
        ref.image = binder.Mapped(key.image);
        ref.mip = uint8_t(key.mip);
        ref.layer = uint8_t(key.layer);
        const uint64_t tag = ++async_tag;
        const rdr::ImageId source = want_source ? binder.Mapped(copy_source) : rdr::kNoImage;
        if (ref.image != rdr::kNoImage && renderer->QueueReadback(ref, tag, source)) async_tags[tag] = {key, guest_frame, seq};
      }
    }
    async.NoteRenderMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  void PollAsync() {
    const auto t0 = std::chrono::steady_clock::now();  // SF-1: always polls (PollReadbacks returns at once when nothing is in flight or cancelled)
    async_done.clear();
    renderer->PollReadbacks(async_done);
    for (rdr::Renderer::ReadbackResult& r : async_done) {
      const auto it = async_tags.find(r.tag);
      if (it == async_tags.end()) continue;
      if (r.cancelled) {
        async_tags.erase(it);
        continue;
      }
      async.Publish(it->second.key, it->second.frame, std::move(r.rgba), r.width, r.height, it->second.seq);
      async_tags.erase(it);
    }
    async.NoteRenderMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  uint64_t async_tag = 0;
  std::unordered_map<uint64_t, AsyncTag> async_tags;
  std::vector<rdr::Renderer::ReadbackResult> async_done;
  // S100: owner mode presents through our swapchain, not the SDK presenter, so its frames reached a present are counted here, in the
  // SDK's own counters (frames_*.csv `presented` / `present_gap_ms`, the `[wwe2k14][frames] on screen` line): on a GPU-bound machine the
  // game loop keeps stepping at 60 while the sink skips drawing, and only this says what reached the screen.
  std::chrono::steady_clock::time_point last_present{};
  // S100 (strip-360 A1): how often each copy's destination (image, mip, layer, rectangle) was written in consecutive frames handled.
  // S100 (strip-360 A1b, rule 6): a per-frame copy that carries state from frame to frame (an exposure / adaptation value: the pass that
  // renders the copy's source reads the copy's destination, the value it nudges) advances one step a game frame. Frames our renderer skips
  // leave it out (A1: its feed is the whole scene), so the drawn frame after them repeats that pass and its copy once for each step left
  // out, right after the copy, with this frame's inputs: the effect follows game frames, not drawn frames (run 6952: an outrun fast-forward
  // drew 1-2 levels darker without it). Only small passes (the state is a value, not a picture: at most kCatchUpTexels) and at most
  // kMaxCatchUp steps (an exponential approach is settled by then).
  static constexpr uint32_t kMaxCatchUp = 16;
  static constexpr uint64_t kCatchUpTexels = 64 * 64;
  std::vector<Op> catch_up_ops;
  std::atomic<uint64_t> catch_up_steps{0}, catch_up_copies{0}, catch_up_not_state{0}, catch_up_too_big{0};
  bool owed = false;  // a skipped frame left a per-frame copy out since the last drawn frame
  void NoteMissed(const rdr::CopyOp& c) {
    const auto it = streaks.copy_seen.find(streaks.Key(c));
    if (it == streaks.copy_seen.end()) return;
    if (it->second.missed < kMaxCatchUp) ++it->second.missed;
    owed = true;
  }
  bool CatchUp(const LoadedFrame& frame) {
    if (!owed) return false;
    owed = false;  // (this frame's copies settle what they owe; a copy missing from it broke its streak, NoteCopies dropped its count)
    catch_up_ops.clear();
    bool any = false;
    const uint32_t count = frame.ops.count;
    for (uint32_t i = 0; i < count; ++i) {
      const Op& op = frame.ops[i];
      catch_up_ops.push_back(op);
      if (op.kind != OpKind::Copy) continue;
      const rdr::CopyOp& c = frame.copies[op.index];
      const auto it = streaks.copy_seen.find(streaks.Key(c));
      if (it == streaks.copy_seen.end() || !it->second.missed) continue;
      const uint32_t steps = it->second.missed;
      it->second.missed = 0;
      if (uint64_t(c.dest_rect.width) * c.dest_rect.height > kCatchUpTexels) {
        ++catch_up_too_big;
        continue;
      }
      // The pass that rendered the copy's source: back over its draws and clears to the SetTargets that named the source.
      uint32_t j = i;
      bool reads_dest = false, renders_source = false;
      while (j > 0) {
        const Op& p = frame.ops[j - 1];
        if (p.kind == OpKind::Draw) {
          const Draw& d = frame.draws[p.index];
          for (uint32_t s = 0; s < d.texture_count; ++s) reads_dest |= frame.texture_slots[d.texture_first + s].image == c.dest.image;
          --j;
          continue;
        }
        if (p.kind == OpKind::Clear) {
          --j;
          continue;
        }
        if (p.kind == OpKind::SetTargets) {
          const rdr::TargetsOp& t = frame.targets[p.index];
          renders_source = t.color_count && t.color[0].image == c.source.image;
          --j;
        }
        break;
      }
      if (!renders_source || !reads_dest) {
        ++catch_up_not_state;
        continue;
      }
      for (uint32_t r = 0; r < steps; ++r) catch_up_ops.insert(catch_up_ops.end(), frame.ops.data + j, frame.ops.data + i + 1);
      catch_up_steps += steps;
      ++catch_up_copies;
      any = true;
    }
    return any;
  }
  lasting::CopyStreaks streaks;   // lane 307: the lasting classification (lasting_prune.h)
  lasting::Pruner pruner;
  std::vector<uint8_t> lasting_seed;
  std::vector<Op> lasting_ops, dropped_ops;
  std::atomic<uint64_t> per_frame_copies_left{0}, starved_draws{0};  // skipped frames' per-frame copies left to the drawn frame; draws forced by the 100 ms guard
  std::atomic<uint64_t> prune_frames{0}, prune_ops_in{0}, prune_ops_kept{0}, prune_draws_in{0}, prune_draws_kept{0}, prune_passes_in{0}, prune_passes_kept{0}, prune_grouped{0}, prune_seeds{0}, prune_promoted{0}, prune_blind{0};
  double present_gap_ms = -1;  // S101 lane 19: the gap NotePresent just measured (render thread only)
  void NotePresent() {
    auto& events = rex::perf::frame_events::Get();
    const auto now = std::chrono::steady_clock::now();
    present_gap_ms = -1;  // S101 lane 19: -1 = the first picture, no gap to report
    if (last_present != std::chrono::steady_clock::time_point{}) {
      events.present_gap_us.store(uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now - last_present).count()), std::memory_order_relaxed);
      present_gap_ms = std::chrono::duration<double, std::milli>(now - last_present).count();
    }
    last_present = now;
    events.presents.fetch_add(1, std::memory_order_release);
    // Lane 256: the hang guard's "presents" beat is the SDK presenter's; owner mode's swapchain never beat it (a false "BLACK: no present" after 10 s).
    // Beat only when the swapchain really presented (rdr::g_presented counts accepted presents), so a surface that fails to come back says BLACK.
    const uint64_t shown = rdr::g_presented.load(std::memory_order_relaxed);
    if (shown != beat_seen) {
      beat_seen = shown;
      rex::perf::hang_beats::Beat(rex::perf::hang_beats::Get().presents);
    }
  }
  uint64_t beat_seen = 0;  // lane 256: rdr::g_presented at the last hang beat (render thread only)

  // S100 speed: this thread's time, per 600-drawn-frame window (render thread only).
  struct Window {
    uint64_t frames = 0, drawn = 0, kept_frames = 0, draws = 0, prune_draws_in = 0, prune_draws_kept = 0;
    uint64_t parse = 0, bind = 0, record = 0, kept = 0;  // ns
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  };
  Window window;
  static uint64_t Ns(std::chrono::steady_clock::time_point since) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
  }

  void WritePicture(Binder& binder, const std::string& label) {
    std::error_code ec;
    std::filesystem::create_directories(options.png_folder, ec);
    WritePictureTo(binder, options.png_folder / ("live_" + label + ".png"), rdr::kNoImage, false, /*async=*/true);
    if (const rdr::ImageId dlss = renderer->dlss_output(); dlss != rdr::kNoImage)  // S10b: DLSS's picture of it (before the HUD)
      WritePictureTo(binder, options.png_folder / ("live_" + label + "_dlss.png"), dlss, false, /*async=*/true);
  }

  // Photo mode (lane 201, GATE_FAULTS row 31): the request SavePicture left is served by the first presented frame, and KEPT until a
  // frame has been written (a failed read or write, or no picture yet, tries again on the next presented frame, up to photo::kMaxTries),
  // then dropped with the reason in the log. Every outcome is logged; before lane 201 a failure after "saving ..." was silent.
  void ServePhoto(Binder& binder) {
    std::filesystem::path photo;
    uint32_t tries = 0;
    {
      std::lock_guard lock(overlay_mu);
      photo = photo_path;
      tries = photo_tries;
    }
    if (photo.empty()) return;
    if (photo::Admit(g_hide_hud.load(std::memory_order_relaxed)) == photo::Action::GiveUp) {  // photo mode ended before a frame served the request
      Log(("[rdr][live][photo] WARN " + photo::Utf8(photo) + ": photo mode ended before the picture was written; dropped").c_str());
      std::lock_guard lock(overlay_mu);
      if (photo_path == photo) photo_path.clear(), photo_tries = 0;
      return;
    }
    // The picture the player sees: DLSS's output when this present drew it and nothing was drawn after DLSS's input copy (the present shows
    // dlss_out + (final - before_hud): a 30 Years filter pass kept by photo mode's HUD hiding draws after the copy, and the bare output would
    // miss it), already the shown size; else the game's presented image (the whole picture, HUD hidden), stretched back to the window's shape
    // when the screen-shape setting squeezed it (RecordPresent, present_fill).
    const bool dlss = renderer->dlss_in_last_present() && renderer->dlss_output() != rdr::kNoImage && binder.kept_after_dlss_copy() == 0;
    const rdr::ImageId image = dlss ? renderer->dlss_output() : rdr::kNoImage;
    const photo::Result result = WritePictureTo(binder, photo, image, /*stretch=*/!dlss && options.present_fill);
    const photo::Action action = photo::AfterAttempt(result, tries);
    if (result != photo::Result::Ok) {
      char line[1024];
      std::snprintf(line, sizeof(line), "[rdr][live][photo] WARN %s: %s (attempt %u)%s", photo::Utf8(photo).c_str(), photo::Why(result), tries,
                    action == photo::Action::GiveUp ? "; giving up" : "; trying on the next presented frame");
      if (action == photo::Action::GiveUp || tries == 1) Log(line);  // the first failure and the last: not 90 lines
    } else if (dlss) {
      Log("[rdr][live][photo] the picture is DLSS's output (shown size)");
    } else if (binder.kept_after_dlss_copy() != 0 && renderer->dlss_in_last_present()) {
      Log("[rdr][live][photo] DLSS ran but draws came after its input copy (30 Years filter): the picture is the game's presented image");
    }
    std::lock_guard lock(overlay_mu);
    if (photo_path != photo) return;  // a newer request replaced it meanwhile: it starts its own count
    if (action == photo::Action::Retry) photo_tries = tries;
    else photo_path.clear(), photo_tries = 0;
  }

  // The last presented picture, as shown (through the display table the present applies), to `path`. `stretch` (lane 201): the picture is
  // the squeezed source and is stretched to the shown shape first (options.shown_wide / shown_tall).
  // Lane 302: the PNG encode + write + rename of a picture whose pixels are already read back. `part_file` is the temp file opened early by the
  // synchronous path, or null (the worker opens it here). True when the picture is on disk under `path`.
  bool FinishPng(FILE* part_file, const std::filesystem::path& path, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
    std::filesystem::path part = path;
    part += ".part";
    if (!part_file) part_file = rdr::PngOpen(part);
    if (!part_file) return false;
    bool written = rdr::WritePngFile(part_file, pixels.data(), width, height);
    if (written) {
      std::error_code ec;
      std::filesystem::rename(part, path, ec);
      if (ec) {  // (rename over an existing file can fail on some systems: remove it and try once more)
        std::filesystem::remove(path, ec);
        std::filesystem::rename(part, path, ec);
      }
      written = !ec;
    }
    if (!written) {
      std::error_code ec;
      std::filesystem::remove(part, ec);
    }
    if (written) {
      ++pngs;
      char line[512];
      std::snprintf(line, sizeof(line), "[rdr][live] picture: %s (%ux%u)", photo::Utf8(path).c_str(), width, height);
      Log(line);
    }
    return written;
  }

  void PicWorker() {
    std::unique_lock lock(pic_mu);
    for (;;) {
      pic_cv.wait(lock, [&] { return pic_stop || !pic_q.empty(); });
      if (pic_q.empty()) return;  // stopping, nothing left
      PicJob job = std::move(pic_q.front());
      pic_q.pop_front();
      lock.unlock();
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = FinishPng(nullptr, job.path, job.pixels, job.width, job.height);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      wwe2k14::perf_sys::NoteFileWrite(double(ms));  // the [perf][sys] line's file_write_ms
      if (!ok) Log(("[rdr][live] WARN picture not written: " + photo::Utf8(job.path)).c_str());
      else if (ms >= 500) Log(("[rdr][live] WARN slow picture write (" + std::to_string(ms) + " ms, off the render thread): " + photo::Utf8(job.path)).c_str());
      job.pixels = {};
      lock.lock();
      int run_done = 0;
      if (--pic_jobs == 0) run_done = std::exchange(pic_deferred_done, 0);
      lock.unlock();
      for (int i = 0; i < run_done; ++i) {
        if (options.picture_done) options.picture_done();
      }
      lock.lock();
    }
  }

  // Bounded (S101 lane 302 review S3): a 1440p picture is 14.7 MB of pixels, 4K 33 MB; with png_every=1 or a stalled disk the queue would grow
  // without limit. At kMaxPicJobs queued + running the render thread writes this one itself (the old behaviour: it waits on the disk).
  static constexpr int kMaxPicJobs = 4;
  void QueuePicture(const std::filesystem::path& path, std::vector<uint8_t>&& pixels, uint32_t width, uint32_t height) {
    {
      std::lock_guard lock(pic_mu);
      if (pic_jobs < kMaxPicJobs) {
        if (!pic_thread.joinable()) pic_thread = std::thread([this] { PicWorker(); });
        pic_q.push_back(PicJob{path, std::move(pixels), width, height});
        ++pic_jobs;
        pic_cv.notify_one();
        return;
      }
    }
    FinishPng(nullptr, path, pixels, width, height);  // synchronous: backpressure
  }

  // The render thread's end-of-frame "named picture handled": now, or when the queued writes are on disk.
  void NamedPictureDone() {
    if (!options.picture_done) return;
    {
      std::lock_guard lock(pic_mu);
      if (pic_jobs > 0) {
        ++pic_deferred_done;
        return;
      }
    }
    options.picture_done();
  }

  void StopPictureWriter() {
    {
      std::lock_guard lock(pic_mu);
      pic_stop = true;
    }
    pic_cv.notify_all();
    if (pic_thread.joinable()) pic_thread.join();
  }

  photo::Result WritePictureTo(Binder& binder, const std::filesystem::path& path, rdr::ImageId image = rdr::kNoImage, bool stretch = false, bool async = false) {
    rdr::ImageRef ref = binder.last_present().source;
    if (image != rdr::kNoImage) ref.image = image, ref.mip = 0, ref.layer = 0;  // S10b: DLSS's output
    if (ref.image == rdr::kNoImage) return photo::Result::NoPicture;
    // S101 lane 19 (case 7041 / the Bryan zip): written under a temp name and renamed when complete, so a reader (the case worker) never sees a half-written PNG.
    // Lane 201: opened BEFORE the read-back and the stretch (a path that cannot be written fails at once, not after a device wait), and by a
    // wide path (path.string() throws on names the ANSI code page cannot hold: a CJK user name, OneDrive).
    std::filesystem::path part = path;
    part += ".part";
    FILE* part_file = nullptr;  // (the async path opens its file on the worker)
    if (!async) {
      part_file = rdr::PngOpen(part);
      if (!part_file) return photo::Result::WriteFailed;
    }
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (!renderer->ReadImage(ref, pixels, width, height)) {
      if (part_file) {
        std::fclose(part_file);
        std::error_code ec;
        std::filesystem::remove(part, ec);
      }
      return photo::Result::ReadFailed;
    }
    if (shown_curve.size() == 1024) {
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        for (uint32_t c = 0; c < 3; ++c) pixels[i + c] = shown_curve[4 * pixels[i + c] + c];
      }
    }
    if (stretch && width && height) {
      uint32_t out_w = width, out_h = height;
      photo::ShownSize(width, height, options.shown_wide, options.shown_tall, out_w, out_h);
      if (out_w != width || out_h != height) {
        pixels = photo::Stretch(pixels.data(), width, height, out_w, out_h);
        width = out_w, height = out_h;
      }
    }
    if (async) {
      QueuePicture(path, std::move(pixels), width, height);
      return photo::Result::Ok;
    }
    return FinishPng(part_file, path, pixels, width, height) ? photo::Result::Ok : photo::Result::WriteFailed;
  }

  std::string Report() const {
    char line[640];
    std::snprintf(line, sizeof(line), "frames drawn %llu, skipped %llu (lasting work run in %llu; per-frame copies left to the drawn frame %llu; drawn by the 100 ms guard %llu); draws %llu, bound %llu, missing a shader %llu; pictures %llu; read-back parts %llu",
                  static_cast<unsigned long long>(drawn.load()), static_cast<unsigned long long>(skipped.load()), static_cast<unsigned long long>(persistent_only.load()),
                  static_cast<unsigned long long>(per_frame_copies_left.load()), static_cast<unsigned long long>(starved_draws.load()),
                  static_cast<unsigned long long>(draws.load()), static_cast<unsigned long long>(bound.load()),
                  static_cast<unsigned long long>(no_shader.load()), static_cast<unsigned long long>(pngs.load()),
                  static_cast<unsigned long long>(parts.load()));
    std::string out = line;
    if (streaks.by_image_left) { std::snprintf(line, sizeof(line), "; per-frame copies found by the image rule %llu", static_cast<unsigned long long>(streaks.by_image_left)); out += line; }
    if (streaks.by_image_guarded) { std::snprintf(line, sizeof(line), "; image rule held back by a guard %llu", static_cast<unsigned long long>(streaks.by_image_guarded)); out += line; }
    if (prune_frames.load()) {   // lane 307
      std::snprintf(line, sizeof(line), "; lasting binds %llu: kept %llu of %llu ops, %llu of %llu draws, %llu of %llu passes (%llu only by the model-part group), %llu lasting copies, %llu per-frame copies walked through, %llu binds without D3 (no object pass kept)",
                    static_cast<unsigned long long>(prune_frames.load()), static_cast<unsigned long long>(prune_ops_kept.load()), static_cast<unsigned long long>(prune_ops_in.load()),
                    static_cast<unsigned long long>(prune_draws_kept.load()), static_cast<unsigned long long>(prune_draws_in.load()), static_cast<unsigned long long>(prune_passes_kept.load()),
                    static_cast<unsigned long long>(prune_passes_in.load()), static_cast<unsigned long long>(prune_grouped.load()), static_cast<unsigned long long>(prune_seeds.load()),
                    static_cast<unsigned long long>(prune_promoted.load()), static_cast<unsigned long long>(prune_blind.load()));
      out += line;
    }
    if (catch_up_copies.load() || catch_up_not_state.load() || catch_up_too_big.load()) {  // A1b
      std::snprintf(line, sizeof(line), "; state steps caught up %llu (in %llu copies; per-frame copies not state %llu, too big %llu)",
                    static_cast<unsigned long long>(catch_up_steps.load()), static_cast<unsigned long long>(catch_up_copies.load()),
                    static_cast<unsigned long long>(catch_up_not_state.load()), static_cast<unsigned long long>(catch_up_too_big.load()));
      out += line;
    }
    return out;
  }
};

LiveSink::LiveSink() : impl_(new Impl) { impl_->geometry.reserve(size_t(1) << 20); }  // lane 273: no insert under store_mu rehashes ~400k ranges (12-80 ms "sink appends" in runs 7189 / 7190)
LiveSink::~LiveSink() {
  Stop();
  delete impl_;
}

namespace {
void (*g_inner_log)(const char*) = nullptr;  // S101 lane 19: the options' own log function, called after the case zip has read the line
void LogTee(const char* line) {
  rdr_case::info::NoteRendererLine(line);
  if (g_inner_log) g_inner_log(line);
  else std::fprintf(stderr, "%s\n", line);  // review fix 10: no log function given: the lines still go somewhere
}
}  // namespace

bool LiveSink::Start(const LiveOptions& options, std::string& error) {
  if (impl_->active) return true;
  thin::g_pins.Reset();  // lane 122 (review 8): a new session starts with no pins (generation 0)
  impl_->options = options;
  // S101 lane 19: the renderer's own "[rdr] device / swapchain / display timing" lines also feed the case zip's device block (no second query).
  g_inner_log = options.log;
  impl_->options.log = &LogTee;
  rdr::SetLogSink(&LogTee);
  rdr_case::info::SetRender(uint32_t(std::ceil(1280.0f * options.render_scale)), uint32_t(std::ceil(720.0f * options.render_scale)), options.render_scale);
  rdr::RendererOptions ro;
  ro.validation = options.validation;
  ro.format_caps_mask = options.format_caps_mask;  // lane 200
  ro.window.instance = options.window_instance;  // null window: headless (stage A, the mirror); a window: the renderer presents to it (stage B)
  ro.window.window = options.window;
  ro.window.current = options.window_source;  // lane 235
  ro.window.current_user = options.window_source_user;
  ro.vsync = options.vsync;
  ro.render_scale = options.render_scale;  // S100 settings parity S1: the launcher's RESOLUTION (images marked kUsageScaled)
  ro.present_fill = options.present_fill;  // settings parity S5
  ro.present_sgsr = options.present_sgsr;  // settings parity S10a
  ro.present_hz = options.present_hz;  // S100 phone 120 fix 2
  ro.gpu_timestamps = options.gpu_timestamps;  // lane 269
  ro.pipeline_store_dir = options.pipeline_store_dir, ro.pipeline_prebuild_slice_us = options.pipeline_prebuild_slice_us;  // lane 276
  ro.dlss_mode = options.dlss_mode, ro.streamline_dir = options.streamline_dir;  // settings parity S10b
  g_dlss = options.dlss_mode >= 0, g_dlss_phases = options.dlss_phases;
  g_velocity_legacy = options.dlss_jitter_legacy;  // lane 185
  g_crowd_density = options.crowd_density, g_shadows = options.shadows, g_depth_of_field = options.depth_of_field;  // S6 / S7 / S8
  g_crowd_rank.enabled = options.density_track;  // S101 lane 234
  g_shadow_half = options.shadow_half;  // S12
  g_film_grain = options.film_grain;  // settings parity S3 (read by the binder on the render thread, set before it starts)
  g_crowd_ours = options.crowd_ours;  // S101 lane 35
  g_post_ours = options.post_ours;    // S101 lane PO
  g_arena_ours = options.arena_ours, g_wrestlers_ours = options.wrestlers_ours;  // S101 lane 41
  g_caw_ours = options.caw_ours;  // lane 153
  g_caw_cache_root = options.caw_cache_root;
  g_ring_ours = options.ring_ours;  // S101 ring contract
  d3::g_arena_d3 = options.arena_d3 && options.arena_thin && !g_dlss;  // lane 138 (D3): needs the arena's thin draws; the guest hook's switch (d3::g_hook_on) is set with it
  d3::g_verify = d3::g_arena_d3 && options.arena_d3_verify;   // lane 152 (10)
  d3::g_hook_on.store(d3::g_arena_d3 && d3::kRecordRouteBuilt, std::memory_order_release);   // review A: the guest hook stays off until the record route exists (lane 152: built)
  d3::g_caster_d3 = options.caster_d3 && !g_dlss;   // lane 228 (the live seam, 17.12): the caster chain; options.caster_d3 already needs the ours hooks, scene records, light states and owner mode (reader.h)
  d3::g_caster_verify = d3::g_caster_d3 && options.caster_d3_verify;
  d3::g_caster_plane = d3::g_caster_d3 && options.caster_d3_plane;   // lane 268
  d3::g_caster_hook_on.store(d3::g_caster_d3 && d3::kRecordRouteBuilt, std::memory_order_release);   // the caster hooks stay off until the chain exists; the gate's latch clears it (binder.h D3CEndFrame)
  d3::g_ring_d3 = options.ring_d3 && !g_dlss;   // lane 246 (the live seam, 16.9): the ring chain; options.ring_d3 already needs the R2 record (--wwe2k14_rdr_ring_d3), ring thin, light states and owner mode (reader.h). NOT the cvar --wwe2k14_rdr_ring_d3 (16.8 N5)
  d3::g_ring_verify = d3::g_ring_d3 && options.ring_d3_verify;
  d3::g_ring_hook_on.store(d3::g_ring_d3 && d3::kRecordRouteBuilt, std::memory_order_release);   // the ring hook stays off until the chain exists; the gate's latch clears it (binder_ring_d3.inc D3REndFrame)
  d3::g_wrestlers_d3 = options.wrestlers_d3 && !g_dlss;   // lane 245 (the live seam, 18.9): the wrestler chain; options.wrestlers_d3 already needs the ours hooks + thin draws, scene records, light states and owner mode (reader.h)
  d3::g_wrestlers_verify = d3::g_wrestlers_d3 && options.wrestlers_d3_verify;
  d3::g_wrestlers_hook_on.store(d3::g_wrestlers_d3 && d3::kRecordRouteBuilt, std::memory_order_release);   // the wrestler scope stays off until the chain exists; the gate's latch clears it (binder.h D3WEndFrame)
  thin::g_arena_thin = options.arena_thin && !g_dlss, thin::g_ring_thin = options.ring_thin && !g_dlss, thin::g_wrestlers_thin = options.wrestlers_thin && !g_dlss;  // lane 129 (KC): the DLSS-off gate for all three classes  // lane 122 (D1 thin draws; not with DLSS: the velocity walk draws the stream's vertices)
  g_ui_ours = options.ui_ours;  // S101 UI contract
  rdr_scene::g_ui_quads = options.ui_quads;  // S101 lane 148: read by MakeUiOurs when the render thread builds the hook
  g_movie_ours = options.movie_ours;  // S101 lane 131
  g_ui_text_ours = options.ui_text_ours;  // S101 lane 89 (UT)
  origin::g_labels_wanted.store(options.ui_text_ours || wwe2k14::caw::g_on.load(std::memory_order_relaxed), std::memory_order_relaxed);  // S101 lane 118 review: never false over a `!caw on`; S101 lane 89: the run strings the text hook lays out are read only while labels are on
  g_flash_ours = options.flash_ours;  // S101 flash contract
  g_props_ours = options.props_ours;  // S101 lane 84
  g_screens_ours = options.screens_ours;  // S101 lane 115
  g_crowd_props_ours = options.crowd_props_ours;  // S101 lane 133
  g_shadow_casters_ours = options.shadow_casters_ours;  // S101 lane 99
  g_rim_ours = options.rim_ours;  // lane 124
  g_circles_ours = options.circles_ours;  // lane 136
  g_plane_shadow_ours = options.plane_shadow_ours;  // lane 139
  g_small_passes_ours = options.small_passes_ours;  // lane 135
  g_rim_chain_ours = options.rim_chain_ours;  // lane 151
  g_particles_ours = options.particles_ours;  // S101 lane 67
  copy_alias::g_mode = options.copy_alias, copy_alias::g_classes = options.copy_alias_classes;  // S101 lane 190
  g_shadows_ours = options.shadows_ours, g_shadow_map_size = options.shadow_map_size, g_shadow_plane_percent = options.shadow_plane_percent, g_shadow_render_scale = options.render_scale;  // S101 lane 123
  impl_->renderer = rdr::Renderer::Create(ro, error);
  if (!impl_->renderer) return false;
  if (options.arena_ours) impl_->Log("[rdr][arena] the arena is drawn by us in place from the scene record (switch on)");
  if (options.arena_thin) impl_->Log(thin::g_arena_thin ? "[rdr][thin] arena thin draws ON: pinned keys skip the reader's geometry build, the binder draws their templates (every 32nd frame of a key takes the full path as its proof); census [rdr][thin] every 600 frames"
                                                       : "[rdr][thin] arena thin draws requested but OFF (DLSS is on: the velocity walk draws the stream's vertices)");  // lane 122
  if (options.ring_thin) impl_->Log(thin::g_ring_thin ? "[rdr][thin] ring thin draws ON: the key's ordinal carries the instance; rope slot 0 and the skirt tie stay as the hook chose them per key (proved every 32nd frame); census [rdr][thin] every 600 frames"
                                                      : "[rdr][thin] ring thin draws requested but OFF (DLSS is on: the velocity walk draws the stream's vertices)");  // lane 129
  if (options.wrestlers_thin) impl_->Log(thin::g_wrestlers_thin ? "[rdr][thin] wrestlers thin draws ON: the template's geometry with the stream's own palette rows; cloth and refused parts stay on the full path; census [rdr][thin] every 600 frames"
                                                                : "[rdr][thin] wrestlers thin draws requested but OFF (DLSS is on: the velocity walk draws the stream's vertices)");  // lane 129
  if (options.post_ours) impl_->Log("[rdr][post] the post chain (depth of field, bloom, glare, the combine, the film) is recorded by our own pass from the last dof draw; the stream's chain draws and the copies they feed are dropped; a chain the plan cannot read stays the stream's (switch on)");  // S101 lane PO
  if (options.caw_ours) impl_->Log("[rdr][caw] the Create a Superstar editor's wrestler is drawn by us in place, part by part from the edit pacs (switch on; no scene record needed)");   // lane 153
  if (options.wrestlers_ours) impl_->Log("[rdr][wrestlers] the wrestlers are drawn by us in place from the scene record (switch on)");
  if (options.ring_ours) impl_->Log("[rdr][ring] the ring is drawn by us in place from the scene record (switch on)");
  if (options.flash_ours) impl_->Log("[rdr][flash] the crowd flashes are drawn by us in place from the arena file (switch on)");
  if (options.screens_ours) impl_->Log("[rdr][screens] the stage screens' geometry (ribbon band, wall, mini-tron) is drawn by us in place from the arena file; the video planes stay the stream's (switch on)");  // S101 lane 115
  if (options.crowd_props_ours) impl_->Log("[rdr][crowd_props] the crowd's sign / foam-hand draws are drawn by us in place from the arena file, each checked byte for byte (switch on)");  // S101 lane 133
  if (options.props_ours) impl_->Log("[rdr][props] the commentary table, the ring steps and the announcer's microphone are drawn by us in place from the game files (switch on)");  // S101 lane 84
  if (options.shadow_casters_ours) impl_->Log("[rdr][shadow_casters] our caster geometry is drawn in the shadow passes (plane mask, wrestler maps) in place of the game's, from the game files (switch on)");  // S101 lane 99
  if (options.shadows_ours) impl_->Log(("[rdr][shadows_ours] our shadow passes (switch on): maps " + std::to_string(shadows_ours::MapSize()) + " px, plane mask " + std::to_string(g_shadow_plane_percent) + "% of the 360 size").c_str());  // S101 lane 123
  if (options.circles_ours) impl_->Log("[rdr][circles] the circle blob shadows are drawn from the scene record in place of the game's (each checked against the game's own constants; switch on)");  // lane 136
  if (options.plane_shadow_ours) impl_->Log("[rdr][plane_shadow] the plane shadow chain's constants (PlaneInv, camera, EyePos, palette; the blur's matrix) come from the scene record in place of the stream's, each checked against the game's own (switch on)");  // lane 139
  if (options.small_passes_ours) impl_->Log("[rdr][small_passes] the wrestlers' sweat map and skin pre-light passes are drawn with meshes of our own, each draw checked against the game's (switch on)");  // lane 135
  if (options.rim_chain_ours) impl_->Log("[rdr][rim_chain] the rim light map's coverage reductions and blur are drawn by our own programs on the game's draws (switch on)");  // lane 151
  if (options.rim_ours) impl_->Log("[rdr][rim_map] our caster geometry is drawn in the rim light map in place of the game's, from the game files (switch on)");  // lane 124
  if (options.particles_ours) impl_->Log("[rdr][particles] runs of particle draws are drawn as one draw each by our vertex stage (switch on)");  // S101 lane 67
  if (options.movie_ours) impl_->Log("[rdr][movie] the movie planes (titantron, menu backdrop) are drawn by our own quad and YUV shader from the planes the decoder wrote this frame (switch on)");  // S101 lane 131
  if (options.ui_ours) impl_->Log("[rdr][ui] the menus' and HUD's pictures are bound from our own uploads (switch on)");
  if (options.ui_quads) impl_->Log("[rdr][ui] the sprites and flat rectangles are drawn with our own quad and 2D program (switch on, lane 148)");   // S101 lane 148
  if (options.ui_text_ours) impl_->Log("[rdr][ui_text] the game's text is laid out by us, each glyph quad checked against the game's (switch on); the run-string labels are ON for the whole session for it (origin::g_labels_wanted)");  // S101 lane 89
  if (options.crowd_ours || options.arena_ours || options.wrestlers_ours || options.ring_ours || options.flash_ours || options.props_ours || options.shadow_casters_ours || options.screens_ours || options.rim_ours || options.crowd_props_ours || options.circles_ours || options.plane_shadow_ours /* lane 139 */) {  /* lane 136: circles_ours */  /* lane 133 */  // lane 124: rim_ours  // S101 lane 115: screens  // S101 lane 99: shadow casters  // S101 lane 84: props; S101 lane 35 / 41: records to the render thread
    impl_->keeper = std::make_unique<SnapshotKeeper>();
    rdr_scene::live::AddSink(impl_->keeper.get());
    if (options.crowd_ours) impl_->Log(options.crowd_cards ? "[rdr][crowd] the crowd is drawn by us from the scene record (record j-1 for frame j); the stream's block draws are dropped, and the far cards too: one instanced draw per person type with the game's own pictures (any card ours cannot account for stays the game's)"
                                   : "[rdr][crowd] the crowd is drawn by us from the scene record (record j-1 for frame j); the stream's block draws are dropped (the far cards stay the stream's)");
  }
  impl_->stopping = false;
  {
    std::lock_guard lock(impl_->pic_mu);
    impl_->pic_stop = false;  // (lane 302: a Start after a Stop gets a working picture writer)
  }
  impl_->thread = std::thread([this] { impl_->Run(); });
  impl_->active = true;
  rex::RegisterQuitFlush(&impl_->quit_req, &impl_->quit_ack);  // S101 lane 216: the SDK's quit flush (before the hard exit) asks for the final census
  impl_->quit_registered = true;
  char line[256];
  std::snprintf(line, sizeof(line), "[rdr][live] started on %s (validation %d, %s)", impl_->renderer->device_name(), options.validation,
                options.window ? "OWNS THE WINDOW" : "headless mirror");
  impl_->Log(line);
  return true;
}

void LiveSink::Stop() {
  origin::g_labels_wanted.store(false, std::memory_order_relaxed);  // S101 lane 89 (review F7)
  if (!impl_->active) return;
  if (impl_->quit_registered) {
    rex::UnregisterQuitFlush(&impl_->quit_req);
    impl_->quit_registered = false;
  }
  {
    std::lock_guard lock(impl_->mail_mu);
    impl_->stopping = true;
  }
  impl_->mail_cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->StopPictureWriter();  // lane 302: the queued pictures are written before the sink goes
  impl_->renderer->WaitIdle();
  if (impl_->keeper) {  // S101 lane 35
    rdr_scene::live::RemoveSink(impl_->keeper.get());
    char crowd_line[400];
    std::snprintf(crowd_line, sizeof(crowd_line), "[rdr][records] frames submitted with no previous record: %llu, with no record of their own (a refused record): %llu; first parts (read-back splits) with a record %llu, without %llu", static_cast<unsigned long long>(impl_->keeper->no_previous), static_cast<unsigned long long>(impl_->keeper->wrong_frame), static_cast<unsigned long long>(impl_->keeper->part_records), static_cast<unsigned long long>(impl_->keeper->part_none));
    impl_->Log(crowd_line);
    impl_->keeper.reset();
  }
  const std::string stop_line = "[rdr][live] stopped: " + impl_->Report();   // lane 307: a std::string (the census is longer than the old 256 byte buffer)
  impl_->Log(stop_line.c_str());
  impl_->renderer.reset();
  impl_->active = false;
}

bool LiveSink::active() const { return impl_->active; }

uint64_t LiveSink::Append(Stream stream, std::vector<uint8_t>&& bytes) {
  const uint64_t key = impl_->next_key.fetch_add(1, std::memory_order_relaxed);  // lane 273: no lock for the key
  // S101 lane 11: the case ring copies the bytes OUTSIDE store_mu (the render thread takes it per draw). No frame names this key until Append
  // returns, so the key is safe to use before the store holds it.
  StoreTimer t(6);
  case_hook::OnContent(stream == Stream::Geometry, key, bytes.data(), bytes.size());
  t.Stop();  // [6] the case ring (the bug button's recorder: off for players)
  std::shared_ptr<const std::vector<uint8_t>> shared;
  t.holder = 5;
  if (stream == Stream::Geometry) shared = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));  // allocated before the lock
  t.Stop();  // [5] the allocation
  t.holder = 4;
  std::lock_guard lock(impl_->store_mu);
  t.Stop();  // [4] the wait for the lock
  t.holder = 1;
  if (stream == Stream::Geometry) impl_->geometry.emplace(key, std::move(shared));
  else impl_->textures.emplace(key, std::move(bytes));
  t.Stop();  // [1] the insert under the lock
  return key;
}

void LiveSink::DropGeometry(uint64_t key) {
  case_hook::DropGeometry(key);  // S101 lane 5
  std::shared_ptr<const std::vector<uint8_t>> freed;    // lane 273: the bytes are released after the lock, not under it (the game thread's Append waits on store_mu)
  {
    std::lock_guard lock(impl_->store_mu);
    struct Hold2 { StoreTimer t{2}; ~Hold2() { t.Stop(); } } hold2;
    if (auto it = impl_->geometry.find(key); it != impl_->geometry.end()) {
      freed = std::move(it->second);
      impl_->geometry.erase(it);
    }
  }
}

void LiveSink::SetImages(const ImageRecord* records, size_t count) {
  case_hook::SetImages(records, count);  // S101 lane 5
  impl_->images.assign(records, records + count);
  impl_->images_pending = true;
}

void LiveSink::ReleaseImage(uint32_t id) {
  case_hook::ReleaseImage(id);  // S101 lane 5
  impl_->released.push_back(id);
}

void LiveSink::SetHideHud(bool hide) { g_hide_hud.store(hide, std::memory_order_relaxed); }

bool LiveSink::SavePicture(const std::filesystem::path& path) {
  if (!impl_->active) return false;
  std::lock_guard lock(impl_->overlay_mu);
  if (!impl_->photo_path.empty() && impl_->photo_path != path)  // a request still waiting is replaced by this newer one (its own count)
    impl_->Log(("[rdr][live][photo] WARN " + photo::Utf8(impl_->photo_path) + ": replaced by a newer request before it was written").c_str());
  if (impl_->photo_path != path) impl_->photo_tries = 0;  // the same path again (a same-second double save) keeps the count
  impl_->photo_path = path;
  return true;
}

bool LiveSink::SaveLastPicture(const std::filesystem::path& path) {
  if (!impl_->active) return false;
  {
    std::lock_guard lock(impl_->mail_mu);
    impl_->last_picture_path = path;
  }
  impl_->mail_cv.notify_all();
  return true;
}

// S101 lane 216: asks the render thread for every class's final census line and waits (at most `timeout_ms`) until it is printed. Any thread; never blocks longer than that.
bool LiveSink::FlushFinalCensus(uint32_t timeout_ms) {
  if (!impl_->active) return false;
  impl_->quit_ack.store(false, std::memory_order_release);  // (SF1: cleared here, not by the render thread)
  impl_->quit_req.store(true, std::memory_order_release);
  impl_->mail_cv.notify_all();
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!impl_->quit_ack.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return impl_->quit_ack.load(std::memory_order_acquire);
}

void LiveSink::SetOverlay(std::shared_ptr<const rdr::OverlayList> list) {
  std::lock_guard lock(impl_->overlay_mu);
  impl_->overlay_next = std::move(list);
  impl_->overlay_changed = true;
}

void LiveSink::SetOverlayTexture(uint32_t id, uint32_t width, uint32_t height, std::vector<uint8_t>&& rgba) {
  if (rgba.size() < size_t(width) * height * 4) return;
  std::lock_guard lock(impl_->overlay_mu);
  impl_->overlay_textures.push_back({id, width, height, std::move(rgba)});
}

void LiveSink::SetDisplayCurve(const uint8_t* rgba256) { impl_->display_curve.assign(rgba256, rgba256 + 1024); }

// Strip-360 A1 (checks only, --wwe2k14_rdr_pace_game): the game waits until fewer than `max_waiting` frames are queued, so an outrun
// fast-forward draws every frame and its pictures compare exactly; bounded by `timeout_ms` (never a hang).
// S101 lane 302 (run 7202): the wait is the swap thread's, which holds the det baton. A render thread that is stuck (a stalled disk under a
// picture write, an outside load) turned every frame into a full timeout = 1 fps for the whole game. After kPaceGiveUp timeouts in a row
// the pacing is switched off (one log line) until the render thread has caught up (fewer than max_waiting queued), then it resumes.
void LiveSink::WaitForRoom(size_t max_waiting, uint32_t timeout_ms) {
  constexpr int kPaceGiveUp = 3;
  std::unique_lock lock(impl_->mail_mu);
  if (impl_->pace_off) {
    if (impl_->mail.size() >= max_waiting && !impl_->stopping) {  // still behind: no waiting
      ++impl_->pace_skipped_frames;
      return;
    }
    impl_->pace_off = false, impl_->pace_timeouts = 0;
    const uint64_t skipped = impl_->pace_skipped_frames;
    lock.unlock();
    char line[160];
    std::snprintf(line, sizeof(line), "[rdr][live] pace_game: the render thread caught up, pacing resumes (%llu frames went unpaced)", static_cast<unsigned long long>(skipped));
    impl_->Log(line);
    return;
  }
  const bool room = impl_->room_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return impl_->stopping || impl_->mail.size() < max_waiting; });
  if (room) {
    impl_->pace_timeouts = 0;
    return;
  }
  if (++impl_->pace_timeouts >= kPaceGiveUp) {
    impl_->pace_off = true;
    ++impl_->pace_give_ups;
    impl_->pace_skipped_frames = 0;
    lock.unlock();
    char line[200];
    std::snprintf(line, sizeof(line), "[rdr][live] WARN pace_game: the render thread did not make room for %d frames in a row (%u ms each); pacing is off until it catches up (episode %u)",
                  kPaceGiveUp, timeout_ms, impl_->pace_give_ups);
    impl_->Log(line);
  }
}

void LiveSink::SubmitFrame(std::vector<uint8_t>&& frame, uint64_t guest_frame, const std::string& picture) {
  case_hook::OnFrame(frame.data(), frame.size(), guest_frame);  // S101 lane 5
  Message message;
  message.frame = std::move(frame);
  message.guest_frame = guest_frame;
  message.seq = ++impl_->submit_seq;
  message.picture = picture;
  if (impl_->images_pending) {
    message.has_images = true;
    message.images = impl_->images;
    impl_->images_pending = false;
  }
  message.released.swap(impl_->released);
  message.display_curve.swap(impl_->display_curve);
  if (impl_->keeper) message.d3_record = d3::AnyD3Chain() ? impl_->keeper->ForD3(guest_frame) : nullptr;   // lane 152 (B1): taken BEFORE ForSubmit (it does not move `cur`)
  if (impl_->keeper) message.record = impl_->keeper->ForSubmit(guest_frame);  // S101 lane 35
  {
    std::lock_guard lock(impl_->mail_mu);
    impl_->mail.push_back(std::move(message));
  }
  impl_->mail_cv.notify_one();
}

bool LiveSink::DrawAndRead(std::vector<uint8_t>&& frame, uint64_t guest_frame, uint32_t image, uint32_t mip, uint32_t layer, std::vector<uint8_t>& rgba,
                           uint32_t& width, uint32_t& height, uint32_t timeout_ms, ReadTimes* times) {
  if (!impl_->active) return false;
  case_hook::OnFrame(frame.data(), frame.size(), guest_frame, true);  // S101 lane 5/11: the part before a read-back copy is a frame file too (flagged kFramePart)
  auto request = std::make_shared<ReadRequest>();
  request->image = image;
  request->mip = mip;
  request->layer = layer;
  request->posted = std::chrono::steady_clock::now();
  Message message;
  message.frame = std::move(frame);
  message.guest_frame = guest_frame;
  message.read = request;
  if (impl_->keeper) message.record = impl_->keeper->ForPart(guest_frame);   // lane 215: the first part's draws need the record the second part will get (circles / plane were blind for the whole frame)
  if (impl_->images_pending) {  // the part may name a new image; releases and the display table wait for the frame's end
    message.has_images = true;
    message.images = impl_->images;
    impl_->images_pending = false;
  }
  {
    std::lock_guard lock(impl_->mail_mu);
    if (impl_->stopping) return false;
    impl_->mail.push_back(std::move(message));
  }
  impl_->mail_cv.notify_one();
  std::unique_lock lock(request->mu);
  if (!request->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return request->done; })) {
    impl_->Log("[rdr][live] WARN a read-back copy was not drawn in time: the game reads what its memory held");
    return false;
  }
  if (times) {
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    times->wait_ms = ms(request->posted, request->taken), times->draw_ms = ms(request->taken, request->drawn), times->read_ms = ms(request->drawn, request->read);
  }
  if (!request->ok) return false;
  rgba = std::move(request->rgba);
  width = request->width;
  height = request->height;
  return true;
}

AsyncReadback& LiveSink::Async() { return impl_->async; }

uint64_t LiveSink::SubmittedCount() const { return impl_->submit_seq.load(std::memory_order_relaxed); }

std::string LiveSink::Report() const { return impl_->Report(); }

}  // namespace rdr_adapter
