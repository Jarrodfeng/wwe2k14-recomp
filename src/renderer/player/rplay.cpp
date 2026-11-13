// The player: runs the renderer with no game. Headless, writes a PNG, reports validation errors in its exit code.
//   rplay [--pipeline-store DIR [--prebuild] [--store-plant 1|2] [--slice-us N]]   (lane 276: the pipeline list + driver cache kept in DIR; --prebuild builds the listed pipelines before frame 0)
//   rplay [--validation N] [--gpu N] [--scale F] [--out file.png] [--frames N]
//   rplay --capture DIR ... [--post-ours [--post-fault LIST] [--post-limits no_r32f_linear]]   (S101 lane PO) the post chain recorded by our own pass (no --game needed)
//   rplay --capture DIR ... [--crowd-ours] [--arena-ours] [--wrestlers-ours] --game DIR   (S101 lane 47) the promotion switches fview has, for the replay farm (src/renderer_scene/promotion_rig.h)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <cstdio>
#include <filesystem>

#include "../../renderer_adapter/binder.h"
#include "../../renderer_adapter/lasting_prune.h"  // lane 307: --skip-every
#include "../../renderer_adapter/jitter_replay.h"  // lane 185: --jitter / --jitter-legacy / --jitter-census / --jitter-marker-late
#include "../../renderer_adapter/wide_replay.h"  // lane 171: --wide / --tall / --wide-legacy / --wide-census
#include "../../renderer_scene/promotion_rig.h"  // S101 lane 47
#include "../frame_builder.h"
#include "../frame_pump.h"
#include "../problem.h"
#include "../renderer.h"
#include "../shaders/spv/test_blit_frag.h"
#include "../shaders/spv/test_blit_vert.h"
#include "../shaders/spv/test_tri_frag.h"
#include "../shaders/spv/test_tri_vert.h"
#include "png.h"

using namespace rdr;

// player\window_win32.cpp: kept apart so windows.h and its macros never meet the renderer's names.
bool OpenTestWindow(int width, int height, rdr::NativeWindow& out);
bool PumpTestWindow();

namespace {

// S101 lane22 (the replay farm, tools\farm\farm.py): per-frame timing, fixed-frame pictures and a test-only per-draw delay. Off unless asked.
struct FarmOptions {
  std::string gpu_log_path;  // lane 269: --gpu-log FILE.csv: frame,ring_gpu_us,collected (the live renderer's no-stall GPU timestamps, frames pipelined, no WaitIdle); --gpu-plant 1|2 plants a wrong period / a too-early read
  uint32_t gpu_plant = 0;
  std::string timing_path;  // --timing FILE.csv: one row a frame: frame,draws,load_us,bind_us,render_us,gpu_us (gpu_us = WaitIdle after the frame: execution + submit/wake latency), gpu_ns = the GPU timestamps around the command buffer
  std::string shot_dir;     // --shot-dir DIR with --shots 5,50,299: the presented picture after those frames (frame_NNNN.png)
  std::vector<uint32_t> shots;
  uint32_t warm_frames = 0;  // --warm-frames N (S101 lane 47): render the first N frames untimed in this process before the timed loop (loads the hooks' meshes, builds pipelines, lifts the GPU clock)
  bool prebuild = false;      // lane 276: --prebuild: PrebuildPipelines() to the end before the first frame (with --pipeline-store DIR)
  bool scene_live = false;    // lane 276: --scene-live
  uint32_t delay_us = 0;  // --delay-us N: a busy wait of N us per draw counted in stats.draws (draws that reached the bind) (planted slowness for the farm's own check)
} g_farm;

// S101 lane 67: --particles-ours [--particles-fault NAME] (particle runs drawn as one draw each, src/renderer_adapter/particle_runs.h)
bool g_particles_arg = false;
int g_announce_arena = -1;   // S101 lane 271: --announce-arena N [--announce-wait-ms M]: the match card's arena is handed to the per-arena classes before the first record, then M ms pass (the load screen)
int g_announce_wait_ms = 0;
rdr_adapter::wide::Replay g_wide_replay;  // lane 171
rdr_adapter::jitter::Replay g_jitter_replay;  // lane 185
uint64_t g_velocity_sum = 0;  // lane 185: wrestler velocity draws collected, summed over the frames (--jitter-census)
float g_wide_factor = 1.0f, g_tall_factor = 1.0f;
bool g_wide_legacy = false, g_wide_no_full = false, g_wide_post_rect = false;  // lane 177: --wide-no-fullview
bool g_persist_plant = false;   // S101 lane 224: --persist-plant: the planted fault "the guard is missing" (the old behaviour: a persistent-only bind is planned like a drawn one)
uint32_t g_persist_every = 0;   // S101 lane 224: --persist-only-every N: every N-th bound frame is bound as a skipped frame's lasting work would be (binder.persist_only_bind), to prove the post hook plans nothing on it
// Lane 307: --skip-every N [--skip-prune old|on|plant] [--skip-census]: every N-th frame is handled as live.cpp handles a frame it skips (persist-only bind of its lasting work, RenderMode::PersistentOnly);
// old = every op but the per-frame copies (live.cpp before lane 307), on = lasting_prune.h's walk-back, plant = the walk-back stopped after one level (the planted fault).
uint32_t g_skip_every = 0;
std::string g_skip_prune = "on";
bool g_skip_census = false;
bool g_skip_by_image = true;
uint32_t g_skip_hash_frames = 0;   // --skip-hash N: after each skipped frame before frame N, the contents of every lasting copy's destination rectangle are hashed ([rplay][skiphash]): the lasting state itself, not a picture of it
bool g_thin_arg = false;  // lane 122: --thin arena (D1 thin draws; the reader is emulated on the capture, binder.h ThinEmulateReader)
rdr_adapter::thin::PinPublisher g_thin_pins;
rdr_adapter::particles::Fault g_particles_fault = rdr_adapter::particles::Fault::None;

double NowUs() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

constexpr uint8_t kTopologyTriangleList = 3;
constexpr uint8_t kCompareLess = 1, kCompareAlways = 7;
constexpr uint8_t kFormatR32G32Sfloat = 103 & 0xFF;

DrawState OpaqueState(uint16_t program, uint16_t layout, bool depth) {
  DrawState state{};
  state.program = program;
  state.vertex_layout = layout;
  state.topology = kTopologyTriangleList;
  state.depth_test = depth;
  state.depth_write = depth;
  state.depth_compare = depth ? kCompareLess : kCompareAlways;
  state.blend[0].write_mask = 0xF;
  return state;
}

DynamicState FullViewport(float width, float height) {
  DynamicState dynamic{};
  dynamic.viewport[2] = width;
  dynamic.viewport[3] = height;
  dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = uint32_t(width);
  dynamic.scissor.height = uint32_t(height);
  return dynamic;
}

// Plays a capture written by the adapter: the real game frames, through the real binder and renderer, with no game.
// Every colour surface the last frame wrote, as one PNG each, in the order the frame first used them. tools\renderer\gallery_sheet.py makes the picture sheet.
void WriteGallery(Renderer& renderer, const rdr_adapter::Binder& binder, const std::string& folder) {
  std::filesystem::create_directories(folder);
  uint32_t index = 0, wrote = 0;
  for (uint32_t id : binder.gallery()) {
    ++index;
    ImageRef ref;
    ref.image = binder.Mapped(id);
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (!renderer.ReadImage(ref, pixels, width, height)) continue;
    char name[512];
    std::snprintf(name, sizeof(name), "%s/%03u_img%u_%ux%u.png", folder.c_str(), index, id, width, height);
    if (WritePng(name, pixels.data(), width, height)) ++wrote;
  }
  Log("[rplay] gallery: %u of %zu surfaces written to %s", wrote, binder.gallery().size(), folder.c_str());
}

int RunCapture(Renderer& renderer, const std::string& folder, int frames, const std::string& out_path, bool list_bound, const std::string& gallery_dir, const std::string& isolate,
               const rdr_scene::PromotionOptions& promotion) {
  rdr_adapter::CaptureReader capture;
  if (!capture.Open(folder)) {
    Log("[rplay] could not open capture %s", folder.c_str());
    return 2;
  }
  rdr_scene::PromotionRig rig;  // S101 lane 47: declared before the binder, so the binder goes first and the hooks after it
  rdr_adapter::Binder binder(renderer);
  binder.CreateImages(capture);
  if (g_particles_arg) {  // S101 lane 67
    rdr_adapter::g_particles_ours = true;
    binder.SetParticleFault(g_particles_fault);
    Log("[rplay] particle runs on (fault %s)", rdr_adapter::particles::FaultName(g_particles_fault));
  }
  {  // S101 lane 47: the promotion switches (nothing is created, and nothing in the replay changes, when none is on)
    std::string error;
    if (!rig.Enable(renderer, binder, capture, promotion, error)) {
      Log("[rplay] promotion: %s", error.c_str());
      return 2;
    }
    std::string message;
    if (g_announce_arena >= 0) {   // S101 lane 271
      rig.Announce(g_announce_arena);
      std::this_thread::sleep_for(std::chrono::milliseconds(g_announce_wait_ms));
      Log("[rplay] announced arena %d, then waited %d ms (the loads ran meanwhile on their workers)", g_announce_arena, g_announce_wait_ms);
    }
    if (!rig.Prime(180.0, capture.frame_count(), message)) {
      Log("[rplay] promotion: %s", message.c_str());
      return 2;
    }
    if (g_thin_arg) {  // lane 122
      if (!promotion.arena) {
        Log("[rplay] --thin arena needs --arena-ours");
        return 2;
      }
      g_thin_pins.Reset();
      binder.SetThinPublisher(&g_thin_pins);
      rdr_adapter::thin::g_arena_thin = true;
    }
    if (rig.active()) Log("[rplay] promotion on: crowd %d, arena %d, wrestlers %d (%s)", promotion.crowd, promotion.arena, promotion.wrestlers, message.empty() ? "no crowd to prime" : message.c_str());
  }
  g_wide_replay.Configure(g_wide_factor, g_tall_factor, g_wide_legacy, g_wide_no_full, g_wide_post_rect);  // lane 171
  if (g_jitter_replay.on) {  // lane 185
    g_jitter_replay.namer = [](uint64_t hash) {
      const int v = rdr_adapter::Binder::VariantOfHash(hash);
      return v >= 0 ? std::string(rdr::fx::kVariants[v].name) : std::string();
    };
    rdr_adapter::g_velocity_census = true;  // the binder collects the wrestler velocity draws without DLSS, so their count can be read
    rdr_adapter::g_velocity_legacy = g_jitter_replay.legacy;
  }
  FrameBuilder builder;
  rdr_adapter::BindStats stats;
  stats.count_pipelines = true;  // (the report below prints them)
  rdr_adapter::LoadedFrame frame;
  const uint32_t count = std::min(uint32_t(frames), capture.frame_count());
  std::FILE* timing = nullptr;  // S101 lane22
  if (!g_farm.timing_path.empty()) {
    timing = std::fopen(g_farm.timing_path.c_str(), "w");
    if (!timing) {
      Log("[rplay] could not write %s", g_farm.timing_path.c_str());
      return 2;
    }
    std::fprintf(timing, "frame,draws,load_us,bind_us,render_us,gpu_us,gpu_ns\n");
  }
  std::FILE* gpu_log = nullptr;  // lane 269
  if (!g_farm.gpu_log_path.empty()) {
    gpu_log = std::fopen(g_farm.gpu_log_path.c_str(), "w");
    if (!gpu_log) {
      Log("[rplay] could not write %s", g_farm.gpu_log_path.c_str());
      return 2;
    }
    std::fprintf(gpu_log, "frame,wall_render_us,ring_gpu_us,collected\n");
  }
  if (g_farm.scene_live) renderer.SetPipelinePrebuildAllowed(false);  // lane 276
  if (g_farm.prebuild) {  // lane 276: the listed pipelines are built now, before frame 0 (the catalog was registered by the binder)
    const size_t left = renderer.PrebuildPipelines(0);
    Log("[rplay] prebuild: %zu listed pipelines left unbuilt (no program for them yet)", left);
  }
  if (!g_farm.shot_dir.empty()) std::filesystem::create_directories(g_farm.shot_dir);
  if (g_farm.warm_frames) {  // S101 lane 47: untimed frames first, the same frames through the same binder / rig / renderer (the arena and wrestlers load on their first record; pipelines build; the clock leaves idle)
    rdr_adapter::BindStats warm_stats;
    rdr_adapter::LoadedFrame warm;
    for (uint32_t w = 0; w < std::min(g_farm.warm_frames, count); ++w) {
      if (!capture.LoadFrame(w, warm)) {
        Log("[rplay] could not read warm-up frame %u", w);
        return 3;
      }
      binder.ApplyContents(warm, capture);
      rig.Feed(w);
      builder.Begin(w);
      if (g_thin_arg) binder.ThinEmulateReader(warm, 2);  // lane 122
      binder.Bind(warm, capture, builder, warm_stats);
      rdr::Frame done = builder.Finish();
      if (warm.header.flags & rdr_adapter::kFramePart) done.keep_contents = true;
      renderer.RenderFrame(done);
    }
    renderer.WaitIdle();
    Log("[rplay] warm-up: %u untimed frames", std::min(g_farm.warm_frames, count));
  }
  rdr_adapter::lasting::CopyStreaks skip_streaks;   // lane 307
  rdr_adapter::lasting::Pruner skip_pruner;
  std::vector<uint8_t> skip_seed;
  std::vector<rdr_adapter::Op> skip_ops, skip_dropped;
  std::unordered_map<uint32_t, const rdr_adapter::ImageRecord*> skip_images;
  for (const rdr_adapter::ImageRecord& r : capture.images()) skip_images[r.id] = &r;
  struct SkipTotals { uint32_t frames = 0, ops_in = 0, ops_kept = 0, draws_in = 0, draws_kept = 0, passes_in = 0, passes_kept = 0, seeds = 0, grouped = 0, bound = 0; double bind_us = 0; } skip_total;
  for (uint32_t f = 0; f < count; ++f) {
    const double t0 = NowUs();  // S101 lane22
    if (!capture.LoadFrame(f, frame)) {
      Log("[rplay] could not read frame %u", f);
      return 3;
    }
    if (g_wide_factor > 1.001f || g_tall_factor > 1.001f) g_wide_replay.Apply(frame, capture);  // lane 171
    if (g_jitter_replay.on) g_jitter_replay.Apply(frame, capture);  // lane 185 (after the wide rule, as the live reader: ApplyWide then ApplyJitter)
    binder.ApplyContents(frame, capture);
    rig.Feed(f);  // S101 lane 47
    const double t1 = NowUs();  // S101 lane22
    const uint32_t draws_before = stats.draws;
    builder.Begin(f);
    stats.list = f + 1 == count && list_bound;
    stats.isolate = isolate.empty() ? nullptr : isolate.c_str();
    stats.gallery = f + 1 == count && !gallery_dir.empty();
    if (g_thin_arg) binder.ThinEmulateReader(frame, 2);  // lane 122
    binder.persist_only_bind = !g_persist_plant && g_persist_every && f % g_persist_every == g_persist_every - 1;   // S101 lane 224
    bool skipped_frame = false;   // lane 307
    const rdr_adapter::Table<rdr_adapter::Op> all_ops = frame.ops;
    if (g_skip_every) {
      skip_streaks.by_image = g_skip_by_image;
      skip_streaks.images = &capture.images();
      skip_streaks.Note(frame);
      skipped_frame = f % g_skip_every == g_skip_every - 1;
      if (skipped_frame) {
        skip_streaks.Classify(frame, false, skip_seed, [](const rdr::CopyOp&) {});
        bool any = false;
        for (uint8_t v : skip_seed) any |= v != 0;
        if (g_skip_prune == "truth") {   // ground truth: the whole frame is bound and drawn; the lasting destinations are hashed as in the other arms
          skip_ops.assign(all_ops.data, all_ops.data + all_ops.count);
        } else if (!any) {   // no lasting work: live.cpp binds nothing (the frame costs its contents only); here the frame is simply not bound
          skipped_frame = true;
          skip_ops.clear();
          skip_dropped.assign(all_ops.data, all_ops.data + all_ops.count);
        } else if (g_skip_prune == "old") {
          skip_ops.clear();
          for (uint32_t i = 0; i < all_ops.count; ++i)
            if (all_ops[i].kind != rdr_adapter::OpKind::Copy || skip_seed[i]) skip_ops.push_back(all_ops[i]);
        } else {
          rdr_adapter::lasting::Stats ps;
          skip_pruner.plant_no_producers = g_skip_prune == "none";
          skip_pruner.no_promote = g_skip_prune == "nopromote";
          skip_pruner.Select(frame, skip_seed, skip_ops, skip_dropped, ps, g_skip_prune == "plant");
          binder.d3_skip_observe = ps.d3_blind();
          skip_total.ops_in += ps.ops, skip_total.ops_kept += ps.kept_ops, skip_total.draws_in += ps.draws, skip_total.draws_kept += ps.kept_draws, skip_total.passes_in += ps.passes, skip_total.passes_kept += ps.kept_passes, skip_total.seeds += ps.seeds, skip_total.grouped += ps.grouped_kept;
        }
        frame.ops = rdr_adapter::Table<rdr_adapter::Op>{skip_ops.data(), uint32_t(skip_ops.size())};
        binder.persist_only_bind = true;
        binder.frame_part_open = true;
        if (g_skip_census && any) {
          std::string line = "[rplay][skip] frame " + std::to_string(f) + " (" + g_skip_prune + "): ops " + std::to_string(skip_ops.size()) + " of " + std::to_string(all_ops.count) + ", lasting copies:";
          for (uint32_t i = 0; i < all_ops.count; ++i) {
            if (all_ops[i].kind != rdr_adapter::OpKind::Copy || !skip_seed[i]) continue;
            const rdr::CopyOp& c = frame.copies[all_ops[i].index];
            const auto sr = skip_images.find(c.source.image), dr = skip_images.find(c.dest.image);
            char buf[256];
            std::snprintf(buf, sizeof buf, " [img%u %ux%u role%u -> img%u %ux%u role%u rect %d,%d %ux%u]", unsigned(c.source.image), sr != skip_images.end() ? sr->second->width : 0u, sr != skip_images.end() ? sr->second->height : 0u,
                          sr != skip_images.end() ? unsigned(sr->second->role) : 0u, unsigned(c.dest.image), dr != skip_images.end() ? dr->second->width : 0u, dr != skip_images.end() ? dr->second->height : 0u,
                          dr != skip_images.end() ? unsigned(dr->second->role) : 0u, c.dest_rect.x, c.dest_rect.y, unsigned(c.dest_rect.width), unsigned(c.dest_rect.height));
            line += buf;
          }
          Log("%s", line.c_str());
        }
      }
    }
    const double bind_t0 = NowUs();
    const uint32_t bound_before = stats.bound;
    binder.Bind(frame, capture, builder, stats);
    if (skipped_frame) {
      skip_total.frames++;
      skip_total.bound += stats.bound - bound_before;
      skip_total.bind_us += NowUs() - bind_t0;
      frame.ops = all_ops;
      binder.frame_part_open = false;
    }
    binder.persist_only_bind = false;
    binder.d3_skip_observe = false;
    if (g_jitter_replay.on) {  // lane 185
      g_velocity_sum += binder.velocity_draw_count();
      if (g_jitter_replay.census) std::printf("[jitter] frame %u velocity draws %zu\n", f, binder.velocity_draw_count());
    }
    if (g_farm.delay_us) {  // S101 lane22: planted slowness, per bound draw
      const double end = NowUs() + double(g_farm.delay_us) * double(stats.draws - draws_before);
      while (NowUs() < end) {}
    }
    rdr::Frame finished = builder.Finish();
    const double t2 = NowUs();  // S101 lane22
    if (frame.header.flags & rdr_adapter::kFramePart) finished.keep_contents = true;  // S101 lane 11: a DrawAndRead part (live.cpp does the same)
    const double gpu_t0 = NowUs();
    renderer.RenderFrame(finished, skipped_frame && g_skip_prune != "truth" ? rdr::RenderMode::PersistentOnly : rdr::RenderMode::Full);
    if (skipped_frame && f < g_skip_hash_frames) {   // lane 307: the lasting state, read back
      renderer.WaitIdle();
      uint32_t ordinal = 0;
      for (uint32_t i = 0; i < all_ops.count; ++i) {
        if (all_ops[i].kind != rdr_adapter::OpKind::Copy || !skip_seed[i]) continue;
        const rdr::CopyOp& c = frame.copies[all_ops[i].index];
        rdr::ImageRef ref;
        ref.image = binder.Mapped(c.dest.image);
        ref.mip = c.dest.mip;
        ref.layer = c.dest.layer;
        std::vector<uint8_t> px;
        uint32_t w = 0, h = 0;
        uint64_t hash = 1469598103934665603ull;
        if (ref.image != rdr::kNoImage && renderer.ReadImage(ref, px, w, h)) {
          for (int32_t y = std::max(0, c.dest_rect.y); y < std::min<int64_t>(h, int64_t(c.dest_rect.y) + c.dest_rect.height); ++y)
            for (int32_t x = std::max(0, c.dest_rect.x); x < std::min<int64_t>(w, int64_t(c.dest_rect.x) + c.dest_rect.width); ++x)
              for (int k = 0; k < 4; ++k) hash = (hash ^ px[(size_t(y) * w + x) * 4 + k]) * 1099511628211ull;
        } else {
          hash = 0;
        }
        Log("[rplay][skiphash] f=%u n=%u dest=%u mip=%u rect=%d,%d,%u,%u hash=%016llx", f, ordinal++, unsigned(c.dest.image), unsigned(c.dest.mip), c.dest_rect.x, c.dest_rect.y, unsigned(c.dest_rect.width), unsigned(c.dest_rect.height), static_cast<unsigned long long>(hash));
      }
    }
    if (gpu_log) std::fprintf(gpu_log, "%u,%.1f,%u,%llu\n", f, NowUs() - gpu_t0, rdr::g_gpu_last_us.load(), static_cast<unsigned long long>(rdr::g_gpu_frames.load()));
    if (timing) {  // S101 lane22: wait for the GPU after every frame, so the frame's own cost is read (not the pipelined average)
      const double t3 = NowUs();
      renderer.WaitIdle();
      const double t4 = NowUs();
      std::fprintf(timing, "%u,%u,%.1f,%.1f,%.1f,%.1f,%llu\n", f, stats.draws - draws_before, t1 - t0, t2 - t1, t3 - t2, t4 - t3, static_cast<unsigned long long>(renderer.LastGpuNs()));
    }
    if (!skipped_frame && std::find(g_farm.shots.begin(), g_farm.shots.end(), f) != g_farm.shots.end() && binder.last_present().source.image != kNoImage) {
      std::vector<uint8_t> pixels;
      uint32_t width = 0, height = 0;
      char name[1024];
      std::snprintf(name, sizeof(name), "%s/frame_%04u.png", g_farm.shot_dir.c_str(), f);
      if (renderer.ReadImage(binder.last_present().source, pixels, width, height) && WritePng(name, pixels.data(), width, height)) Log("[rplay] shot %s (%ux%u)", name, width, height);
      else Log("[rplay] could not write shot %s", name);
    }
  }
  if (timing) std::fclose(timing);
  if (g_skip_every)   // lane 307
    Log("[rplay][skip] %s: %u skipped frames (every %u), lasting binds kept %u of %u ops, %u of %u draws, %u of %u passes (%u only by the object group), %u lasting copies (image rule found %llu per-frame); bound draws %u, bind %.1f us a skipped frame",
        g_skip_prune.c_str(), skip_total.frames, g_skip_every, skip_total.ops_kept, skip_total.ops_in, skip_total.draws_kept, skip_total.draws_in, skip_total.passes_kept, skip_total.passes_in, skip_total.grouped, skip_total.seeds, static_cast<unsigned long long>(skip_streaks.by_image_left), skip_total.bound,
        skip_total.frames ? skip_total.bind_us / skip_total.frames : 0.0);
  if (g_jitter_replay.on) {  // lane 185
    g_jitter_replay.Report();
    std::printf("[jitter] velocity_draws_ total %llu over %u frames (%s window)\n", (unsigned long long)g_velocity_sum, count, g_jitter_replay.legacy ? "legacy" : "marker");
  }
  if (g_wide_replay.state.on())
    Log("[rplay] wide: %u draws, %u squeezed (menu %u, hud %u, scene draws after the first depth copy %u), full-view quads left unsqueezed %u; the post-pass hash rule left alone: %u full-view quads, %u other rectangles", g_wide_replay.totals.draws, g_wide_replay.totals.squeezed, g_wide_replay.totals.menu, g_wide_replay.totals.hud, g_wide_replay.totals.scene_after_copy, g_wide_replay.totals.full_view, g_wide_replay.totals.post_after_full, g_wide_replay.totals.post_after_partial);  // lane 171
  renderer.WaitIdle();
  if (gpu_log) {  // lane 269: the sanity check. Every result is checked against (0, time since its submit] inside the renderer; a healthy run drops none and has all but the last frames_in_flight collected.
    std::fclose(gpu_log);
    renderer.LogGpuSummary();
    uint64_t implausible = 0, not_ready = 0;
    renderer.GpuTotals(implausible, not_ready);
    const uint64_t collected = rdr::g_gpu_frames.load();
    const bool pass = collected > 0 && implausible == 0 && not_ready == 0 && collected + 2 >= count;
    Log("[rplay][gpu] check %s: %u frames rendered, %llu collected, %llu implausible, %llu not ready (mean %.3f ms a frame)", pass ? "PASS" : "FAIL", count,
        static_cast<unsigned long long>(collected), static_cast<unsigned long long>(implausible), static_cast<unsigned long long>(not_ready),
        collected ? double(rdr::g_gpu_ns_total.load()) / 1e6 / double(collected) : 0.0);
    if (!pass && g_farm.gpu_plant == 0) return 5;
  }
  if (rig.active()) {  // S101 lane 47
    const std::string report = rig.Report(binder);
    size_t from = 0;  // (one Log per line: a long Log is cut, and three classes' lines together are long)
    while (from < report.size()) {
      size_t to = report.find('\n', from);
      if (to == std::string::npos) to = report.size();
      if (to > from) Log("[rplay] promotion census: %s", report.substr(from, to - from).c_str());
      from = to + 1;
    }
  }
  if (g_particles_arg) Log("[rplay] promotion census: %s", binder.ParticleCensus().c_str());  // S101 lane 67
  if (g_thin_arg) Log("%s", binder.ThinCensus().c_str());  // lane 122
  if (!gallery_dir.empty()) WriteGallery(renderer, binder, gallery_dir);
  Log("[rplay] capture: %u frames, %u draws, %u bound, %u missing a shader, %u no geometry, %u bad layout, %u high slot", count, stats.draws, stats.bound, stats.no_shader, stats.no_geometry, stats.bad_layout, stats.high_slot);
  if (capture.scene_record_count())  // S101 lane 26: the capture carries the live scene writer's records (scene folder, frame_NNNNNN.rsc)
    Log("[rplay] scene records: %u beside the %u frames (rsc_dump / rsc_draw FOLDER --frame N reads one)", capture.scene_record_count(), capture.frame_count());
  Log("[rplay] distinct shader pairs %zu, pipelines if each game shader were its own program %zu", stats.shader_pairs.size(), stats.pipelines.size());
  std::vector<std::pair<uint32_t, uint64_t>> missing;
  for (const auto& item : binder.missing()) missing.push_back({item.second, item.first});
  std::sort(missing.rbegin(), missing.rend());
  for (size_t m = 0; m < missing.size(); ++m) Log("[rplay] missing shader %016llX: %u draws", static_cast<unsigned long long>(missing[m].second), missing[m].first);
  {
    std::vector<std::pair<uint32_t, uint64_t>> empty;
    uint32_t total = 0;
    for (const auto& item : stats.empty_slots) {
      empty.push_back({item.second, item.first});
      total += item.second;
    }
    std::sort(empty.rbegin(), empty.rend());
    Log("[rplay] empty texture slots: %u slot reads in %zu variant slots", total, empty.size());
    for (size_t e = 0; e < empty.size() && e < 40; ++e)
      Log("[rplay]   %s slot %u: %u draws", rdr::fx::kVariants[empty[e].second >> 8].name, uint32_t(empty[e].second & 0xFF), empty[e].first);
  }
  for (const auto& item : stats.type_mismatches) {
    const uint32_t vs = uint32_t(item.first >> 32), location = uint32_t(item.first >> 16) & 0xFFFF, format = uint32_t(item.first) & 0xFFFF;
    Log("[rplay] attribute type: %s location %u gets VkFormat %u (%s) but declares %s: %u layouts", rdr::fx::kVariants[vs].name, location, format,
        rdr::VertexFormatIsInteger(uint16_t(format)) ? "integer" : "float", rdr::VertexFormatIsInteger(uint16_t(format)) ? "a float" : "an integer", item.second);
  }
  if (!out_path.empty() && binder.last_present().source.image != kNoImage) {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (renderer.ReadImage(binder.last_present().source, pixels, width, height) && WritePng(out_path.c_str(), pixels.data(), width, height)) Log("[rplay] wrote %s (%ux%u)", out_path.c_str(), width, height);
    else Log("[rplay] could not read the presented image");
  }
  return renderer.validation_errors() ? 4 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  RendererOptions options;
  std::string out_path;
  bool list_bound = false;
  std::string isolate;  // --isolate NAME: draw only draws whose program name contains NAME
  std::string gallery_dir;
  int frames = 3;
  bool windowed = false;
  std::string capture_dir;
  rdr_scene::PromotionOptions promotion;  // S101 lane 47
  double fps_cap = -1;  // --pump N: run through the frame pump at a cap of N (0 = uncapped)
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--validation" && i + 1 < argc) options.validation = std::atoi(argv[++i]);
    else if (arg == "--gpu" && i + 1 < argc) options.gpu_index = std::atoi(argv[++i]);
    else if (arg == "--format-caps-mask" && i + 1 < argc) options.format_caps_mask = uint32_t(std::strtoul(argv[++i], nullptr, 16));  // lane 200
    else if (arg == "--scale" && i + 1 < argc) options.render_scale = float(std::atof(argv[++i])), g_jitter_replay.scale = options.render_scale;
    else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
    else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (arg == "--window") windowed = true;
    else if (arg == "--wide" && i + 1 < argc) g_wide_factor = float(std::atof(argv[++i]));  // lane 171: the screen shape (21:9 = 1.3125), offline: wide_replay.h
    else if (arg == "--tall" && i + 1 < argc) g_tall_factor = float(std::atof(argv[++i]));
    else if (arg == "--wide-drop-fullview") g_wide_replay.drop_full_view = true;  // lane 177: measurement only (full-view quads write no colour)
    else if (arg == "--wide-no-fullview") g_wide_no_full = true;  // lane 177: the plant (full-view quads squeezed like any scene draw)
    else if (arg == "--wide-mute-draw" && i + 1 < argc) g_wide_replay.mute_draw = std::atoll(argv[++i]);  // lane 186: measurement only (draw N writes no colour: a render diff reads its footprint)
    else if (arg == "--wide-post-rect") g_wide_post_rect = true;  // lane 186: the plant (the old post list: every T_Rect after the HUD marker unsqueezed)
    else if (arg == "--wide-clipoff-squeezed") g_wide_replay.clip_off_squeezed_plant = true;  // lane 244: the plant (clip-off draws squeezed like any other: the 976 px shift of the CAW editor's combine)
    else if (arg == "--wide-from-squeezed") g_wide_replay.from_squeezed = true;  // lane 244: the capture was recorded with the wide rule on: rebuild the 16:9 dynamics from guest_scale / guest_offset, then apply the rule
    else if (arg == "--wide-offscreen-squeezed") g_wide_replay.offscreen_squeezed_plant = true;  // lane 244b: the plant (offscreen passes squeezed in the blanket modes: the ghost outline)
    else if (arg == "--wide-all") g_wide_replay.all_plant = true;  // lane 244: gpu_wide_all (the app sets it outside matches): every draw squeezed, as the CAW editor captures were recorded
    else if (arg == "--wide-legacy") g_wide_legacy = true;  // lane 171: the S5 rule (the plant)
    else if (arg == "--wide-census") g_wide_replay.census = true;
    else if (arg == "--jitter" && i + 1 < argc) g_jitter_replay.on = true, g_jitter_replay.phase = uint32_t(std::atoi(argv[++i]));  // lane 185: the DLSS jitter rule offline, a fixed phase
    else if (arg == "--jitter-phases" && i + 1 < argc) g_jitter_replay.phases = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--jitter-legacy") g_jitter_replay.legacy = true;  // lane 185: the S10b window (the plant)
    else if (arg == "--jitter-marker-late") g_jitter_replay.marker_late = true;  // lane 185: the window never closes at the marker (the HUD plant)
    else if (arg == "--jitter-census") g_jitter_replay.census = true;
    else if (arg == "--jitter-chain") g_jitter_replay.chain = true;  // lane 185: the armed state carries across frames, as live
    else if (arg == "--jitter-no-fullview") g_jitter_replay.no_full_view = true;  // lane 185: the plant for the full-view composite skip
    else if (arg == "--jitter-zero") g_jitter_replay.zero = true;  // lane 185: a jitter of 0 through the whole path (identity)
    else if (arg == "--no-vsync") options.vsync = false;
    else if (arg == "--pipeline-store" && i + 1 < argc) options.pipeline_store_dir = argv[++i];  // lane 276
    else if (arg == "--prebuild") g_farm.prebuild = true;
    else if (arg == "--scene-live") g_farm.scene_live = true;  // lane 276: SetPipelinePrebuildAllowed(false)
    else if (arg == "--save-ms" && i + 1 < argc) options.pipeline_save_interval_ms = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--store-plant" && i + 1 < argc) options.pipeline_store_plant = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--slice-us" && i + 1 < argc) options.pipeline_prebuild_slice_us = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--capture" && i + 1 < argc) capture_dir = argv[++i];
    else if (arg == "--list-bound") list_bound = true;
    else if (arg == "--crowd-ours") promotion.crowd = true;  // S101 lane 47
    else if (arg == "--crowd-cards") promotion.crowd_cards = true;  // S101 lane CD (with --crowd-ours)
    else if (arg == "--arena-ours") promotion.arena = true;
    else if (arg == "--wrestlers-ours") promotion.wrestlers = true;
    else if (arg == "--caw-ours") promotion.caw = true;   // lane 153 (review note: rplay sets PromotionOptions::caw)
    else if (arg == "--ring-ours") promotion.ring = true;  // S101 ring contract
    else if (arg == "--ui-ours") promotion.ui = true;  // S101 UI contract
    else if (arg == "--ui-quads") rdr_scene::g_ui_quads = true;  // S101 lane 148 (needs --ui-ours)
    else if (arg == "--ui-text-ours") promotion.ui_text = true;  // S101 lane 89 (UT)
    else if (arg == "--flash-ours") promotion.flash = true;  // S101 flash contract
    else if (arg == "--props-ours") promotion.props = true;  // S101 lane 84
    else if (arg == "--post-ours") promotion.post = true;  // S101 lane PO
    else if (arg == "--skip-every" && i + 1 < argc) g_skip_every = uint32_t(std::atoi(argv[++i]));  // lane 307
    else if (arg == "--skip-prune" && i + 1 < argc) g_skip_prune = argv[++i];
    else if (arg == "--skip-census") g_skip_census = true;
    else if (arg == "--skip-hash" && i + 1 < argc) g_skip_hash_frames = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--skip-by-image" && i + 1 < argc) g_skip_by_image = std::string(argv[++i]) != "off";
    else if (arg == "--persist-plant") g_persist_plant = true;  // S101 lane 224
    else if (arg == "--persist-only-every" && i + 1 < argc) g_persist_every = uint32_t(std::atoi(argv[++i]));  // S101 lane 224
    else if (arg == "--post-fault" && i + 1 < argc) promotion.post_fault = argv[++i];  // S101 lane PO: PostFaults::Parse's list
    else if (arg == "--post-limits" && i + 1 < argc) promotion.post_limits = argv[++i];  // S101 lane PO: no_r32f_linear
    else if (arg == "--screens-ours") promotion.screens = true;  // S101 lane 115
    else if (arg == "--movie-ours") promotion.movie = true;  // S101 lane 131
    else if (arg == "--shadow-casters-ours") promotion.shadow_casters = true;  // S101 lane 99
    else if (arg == "--rim-ours") promotion.rim = true;  // lane 124
    else if (arg == "--circles-ours") promotion.circles = true;  // lane 136
    else if (arg == "--plane-shadow-ours") promotion.plane_shadow = true;  // lane 139
    else if (arg == "--small-passes-ours") promotion.small_passes = true;  // lane 135
    else if (arg == "--rim-chain-ours") promotion.rim_chain = true;  // lane 151
    else if (arg == "--game" && i + 1 < argc) promotion.game_root = argv[++i];
    else if (arg == "--depth-of-field" && i + 1 < argc) rdr_adapter::g_depth_of_field = std::string(argv[++i]) != "off";  // S101 lane PO: the DEPTH OF FIELD setting, offline
    else if (arg == "--film-grain" && i + 1 < argc) rdr_adapter::g_film_grain = std::string(argv[++i]) != "off";  // S101 lane PO: the FILM GRAIN setting, offline
    else if (arg == "--shadows" && i + 1 < argc) rdr_adapter::g_shadows = std::string(argv[++i]) != "off";  // S101 lane 86: the SHADOWS setting, offline
    else if (arg == "--shadows-ours") rdr_adapter::g_shadows_ours = true;  // S101 lane 123 (SH2)
    else if (arg == "--copy-alias" && i + 1 < argc) rdr_adapter::copy_alias::g_mode = std::atoi(argv[++i]);  // S101 lane 190 (copy_alias.h): 0 off, 1 census, 2 drop
    else if (arg == "--copy-alias-classes" && i + 1 < argc) rdr_adapter::copy_alias::g_classes = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--shadow-map-size" && i + 1 < argc) rdr_adapter::g_shadow_map_size = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--shadow-plane-scale" && i + 1 < argc) rdr_adapter::g_shadow_plane_percent = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--shadows-ours-fault" && i + 1 < argc) {
      if (!rdr_adapter::shadows_ours::ParseFault(argv[++i], rdr_adapter::shadows_ours::g_fault)) {
        Log("[rplay] unknown --shadows-ours-fault %s", argv[i]);
        return 2;
      }
    }
    else if (arg == "--shadow-half") rdr_adapter::g_shadow_half = true;  // S101 lane 86: the QUALITY LOW half-rate maps, offline
    else if (arg == "--shadow-keying" && i + 1 < argc) {  // S101 lane 86: hash = the old shader-hash lists (the A/B twin); rim_as_shadow = the planted fault
      const std::string mode = argv[++i];
      rdr_adapter::shadow_roles::g_hash_keying = mode == "hash";
      rdr_adapter::shadow_roles::g_rim_as_shadow = mode == "rim_as_shadow";
    }
    else if (arg == "--particles-ours") g_particles_arg = true;  // S101 lane 67
    else if (arg == "--thin" && i + 1 < argc && std::string(argv[i + 1]) == "arena") ++i, g_thin_arg = true;  // lane 122
    else if (arg == "--particles-fault" && i + 1 < argc) {
      if (!rdr_adapter::particles::ParseFault(argv[++i], g_particles_fault)) {
        Log("[rplay] unknown --particles-fault %s", argv[i]);
        return 2;
      }
    }
    else if (arg == "--isolate" && i + 1 < argc) isolate = argv[++i];
    else if (arg == "--gallery" && i + 1 < argc) gallery_dir = argv[++i];
    else if (arg == "--pump" && i + 1 < argc) fps_cap = std::atof(argv[++i]);
    else if (arg == "--gpu-log" && i + 1 < argc) g_farm.gpu_log_path = argv[++i];  // lane 269
    else if (arg == "--gpu-plant" && i + 1 < argc) g_farm.gpu_plant = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--timing" && i + 1 < argc) g_farm.timing_path = argv[++i];  // S101 lane22
    else if (arg == "--shot-dir" && i + 1 < argc) g_farm.shot_dir = argv[++i];
    else if (arg == "--announce-arena" && i + 1 < argc) g_announce_arena = std::atoi(argv[++i]);   // S101 lane 271
    else if (arg == "--announce-wait-ms" && i + 1 < argc) g_announce_wait_ms = std::atoi(argv[++i]);
    else if (arg == "--warm-frames" && i + 1 < argc) g_farm.warm_frames = uint32_t(std::atoi(argv[++i]));  // S101 lane 47
    else if (arg == "--delay-us" && i + 1 < argc) g_farm.delay_us = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--shots" && i + 1 < argc) {
      const char* c = argv[++i];
      while (*c) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(c, &end, 10);
        if (end == c) break;
        g_farm.shots.push_back(uint32_t(value));
        c = (*end == ',') ? end + 1 : end;
      }
    }
  }
  rdr_adapter::g_shadow_render_scale = options.render_scale;  // S101 lane 123
  if (!g_farm.timing_path.empty() || !g_farm.gpu_log_path.empty()) options.gpu_timestamps = true;  // S101 lane22, lane 269
  options.gpu_timestamp_plant = g_farm.gpu_plant;
  if (!g_farm.shots.empty() && g_farm.shot_dir.empty()) {
    Log("[rplay] --shots needs --shot-dir");
    return 2;
  }
  if (windowed && !OpenTestWindow(1280, 720, options.window)) {
    Log("[rplay] could not open a window");
    return 2;
  }

  std::string error;
  std::unique_ptr<Renderer> renderer = Renderer::Create(options, error);
  if (!renderer) {
    Log("[rplay] could not start: %s", error.c_str());
    return 2;
  }

  promotion.render_scale = options.render_scale;  // S101 lane PO
  if (!capture_dir.empty()) return RunCapture(*renderer, capture_dir, frames, out_path, list_bound, gallery_dir, isolate, promotion);

  ProgramDesc tri_program;
  tri_program.name = "test_tri";
  tri_program.vertex_spirv = kSpv_test_tri_vert;
  tri_program.vertex_bytes = sizeof(kSpv_test_tri_vert);
  tri_program.fragment_spirv = kSpv_test_tri_frag;
  tri_program.fragment_bytes = sizeof(kSpv_test_tri_frag);
  const uint16_t tri = renderer->AddProgram(tri_program);
  ProgramDesc blit_program;
  blit_program.name = "test_blit";
  blit_program.vertex_spirv = kSpv_test_blit_vert;
  blit_program.vertex_bytes = sizeof(kSpv_test_blit_vert);
  blit_program.fragment_spirv = kSpv_test_blit_frag;
  blit_program.fragment_bytes = sizeof(kSpv_test_blit_frag);
  const uint16_t blit = renderer->AddProgram(blit_program);

  VertexLayoutDesc tri_layout{};
  tri_layout.attribute_count = 2;
  tri_layout.attributes[0] = {0, 0, kFormatR32G32Sfloat, 0};
  tri_layout.attributes[1] = {1, 0, 106 /* R32G32B32_SFLOAT */, 8};
  tri_layout.strides[0] = 20;
  const uint16_t tri_vertices = renderer->AddVertexLayout(tri_layout);
  VertexLayoutDesc no_vertices{};
  const uint16_t empty = renderer->AddVertexLayout(no_vertices);

  ImageDesc color_desc;
  color_desc.width = 640;
  color_desc.height = 360;
  color_desc.format = Format::RGBA8;
  color_desc.usage = kUsageTarget | kUsageSampled | kUsageScaled;
  const ImageId scene = renderer->CreateImage(color_desc);
  const ImageId output = renderer->CreateImage(color_desc);
  ImageDesc copy_desc = color_desc;
  copy_desc.usage = kUsageSampled | kUsageTarget;
  ImageDesc depth_desc = color_desc;
  depth_desc.format = Format::Depth;
  depth_desc.usage = kUsageTarget | kUsageScaled | kUsageTransient;
  const ImageId depth = renderer->CreateImage(depth_desc);

  // A small feedback chain, like the game's sweat maps: a pass reads last frame's copy of itself. It must run once per game frame, shown or not.
  ImageDesc feedback_desc;
  feedback_desc.width = feedback_desc.height = 64;
  feedback_desc.format = Format::RGBA8;
  feedback_desc.usage = kUsageTarget | kUsageSampled;
  const ImageId feedback_target = renderer->CreateImage(feedback_desc);
  const ImageId feedback_copy = renderer->CreateImage(feedback_desc);

  std::unique_ptr<FramePump> pump;
  if (fps_cap >= 0) {
    PumpOptions pump_options;
    pump_options.fps_cap = fps_cap;
    pump = std::make_unique<FramePump>(*renderer, pump_options);
  }

  FrameBuilder own_builder;
  const auto started = std::chrono::steady_clock::now();
  for (int f = 0; f < frames; ++f) {
    FrameBuilder& builder = pump ? pump->BeginFrame(uint64_t(f)) : own_builder;
    if (!pump) builder.Begin(uint64_t(f));
    const float vertices[] = {-0.7f, -0.7f, 1, 0, 0, 0.7f, -0.7f, 0, 1, 0, 0.0f, 0.7f, 0, 0, 1};
    const uint32_t geometry = builder.PushGeometry(vertices, sizeof(vertices));
    const float offset[4] = {float(f) * 0.05f, 0, 0.5f, 0};
    const float tint[4] = {1, 1, 1, 1};
    const uint32_t vs = builder.PushConstants(offset, sizeof(offset));
    const uint32_t fs = builder.PushConstants(tint, sizeof(tint));

    SamplerDesc feedback_sampler{};
    feedback_sampler.address_u = feedback_sampler.address_v = feedback_sampler.address_w = 2;
    feedback_sampler.max_anisotropy = 1;
    feedback_sampler.max_lod = 1000.0f;
    TargetsOp feedback_targets;
    feedback_targets.color[0].image = feedback_target;
    feedback_targets.color_count = 1;
    builder.SetTargets(feedback_targets);
    TextureSet feedback_textures;
    feedback_textures.slots[0].image = feedback_copy;
    feedback_textures.slots[0].sampler = builder.Sampler(feedback_sampler);
    DrawOp feedback_draw{};
    feedback_draw.state = builder.State(OpaqueState(blit, empty, false));
    feedback_draw.dynamic = builder.Dynamic(FullViewport(64, 64));
    feedback_draw.textures = builder.Textures(feedback_textures);
    feedback_draw.vs_constants = vs;
    feedback_draw.fs_constants = fs;
    feedback_draw.count = 3;
    feedback_draw.instances = 1;
    builder.Draw(feedback_draw);
    CopyOp feedback_step;
    feedback_step.source.image = feedback_target;
    feedback_step.dest.image = feedback_copy;
    feedback_step.source_rect.width = feedback_step.dest_rect.width = 64;
    feedback_step.source_rect.height = feedback_step.dest_rect.height = 64;
    builder.Copy(feedback_step);

    TargetsOp scene_targets;
    scene_targets.color[0].image = scene;
    scene_targets.color_count = 1;
    scene_targets.depth.image = depth;
    builder.SetTargets(scene_targets);
    ClearOp clear;
    clear.flags = kClearColor | kClearDepth | kClearStencil;
    clear.full = 1;
    clear.color[0] = 0.08f;
    clear.color[1] = 0.10f;
    clear.color[2] = 0.14f;
    clear.color[3] = 1.0f;
    clear.depth = 1.0f;
    builder.Clear(clear);
    DrawOp draw{};
    draw.state = builder.State(OpaqueState(tri, tri_vertices, true));
    draw.dynamic = builder.Dynamic(FullViewport(640, 360));
    draw.textures = builder.Textures(TextureSet{});
    draw.vs_constants = vs;
    draw.fs_constants = fs;
    draw.vertex[0] = {kFrameGeometry, geometry};
    draw.count = 3;
    draw.instances = 1;
    draw.blend_key_low = 1;
    const BlendRange moving{0, 16};
    draw.blend_first = builder.PushBlendRanges(&moving, 1);
    draw.blend_count = 1;
    builder.Draw(draw);

    TargetsOp output_targets;
    output_targets.color[0].image = output;
    output_targets.color_count = 1;
    builder.SetTargets(output_targets);
    SamplerDesc sampler{};
    sampler.mag_filter = sampler.min_filter = 1;
    sampler.address_u = sampler.address_v = sampler.address_w = 2;  // clamp to edge
    sampler.max_anisotropy = 1;
    sampler.max_lod = 1000.0f;
    TextureSet textures;
    textures.slots[0].image = scene;
    textures.slots[0].sampler = builder.Sampler(sampler);
    DrawOp full{};
    full.state = builder.State(OpaqueState(blit, empty, false));
    full.dynamic = builder.Dynamic(FullViewport(640, 360));
    full.textures = builder.Textures(textures);
    full.vs_constants = vs;
    full.fs_constants = fs;
    full.count = 3;
    full.instances = 1;
    builder.Draw(full);

    PresentOp present;
    present.source.image = output;
    present.source_rect.width = 640;
    present.source_rect.height = 360;
    builder.Present(present);
    if (pump) {
      pump->PublishFrame();
      // The game's fixed 60 steps, on its own clock.
      std::this_thread::sleep_until(started + std::chrono::microseconds(int64_t((f + 1) * 16667)));
    } else {
      renderer->RenderFrame(builder.Finish());
    }
    if (windowed && !PumpTestWindow()) break;
  }
  if (pump) {
    const PumpStats stats = pump->stats();
    Log("[rplay] pump: published %llu, drawn %llu, skipped %llu", static_cast<unsigned long long>(stats.published), static_cast<unsigned long long>(stats.drawn), static_cast<unsigned long long>(stats.skipped));
    pump.reset();
  }

  renderer->WaitIdle();
  int exit_code = 0;
  if (!out_path.empty()) {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    ImageRef ref;
    ref.image = output;
    if (renderer->ReadImage(ref, pixels, width, height) && WritePng(out_path.c_str(), pixels.data(), width, height)) {
      Log("[rplay] wrote %s (%ux%u)", out_path.c_str(), width, height);
    } else {
      Log("[rplay] could not write %s", out_path.c_str());
      exit_code = 3;
    }
  }
  const uint64_t errors = renderer->validation_errors();
  Log("[rplay] device %s, %d frames, validation errors %llu", renderer->device_name(), frames, static_cast<unsigned long long>(errors));
  if (errors) exit_code = 4;
  return exit_code;
}
