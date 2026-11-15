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
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cstdio>
#include <filesystem>

#include "../../renderer_adapter/binder.h"
#include "../../renderer_adapter/lasting_prune.h"  // lane 307: --skip-every
#include "../../renderer_adapter/members_switch.h"  // S102 T6
#include "../../renderer_adapter/jitter_replay.h"  // lane 185: --jitter / --jitter-legacy / --jitter-census / --jitter-marker-late
#include "../../renderer_adapter/wide_replay.h"  // lane 171: --wide / --tall / --wide-legacy / --wide-census
#include "../../renderer_scene/promotion_rig.h"  // S101 lane 47
#include "../frame_builder.h"
#include "../frame_pump.h"
#include "../stage_plants.h"  // S102 R21-B
#include "stage_selftest.h"  // S102 R21-B: --stage-selftest (and the stage itself, renderer_adapter\stage_split.h)
#include "../output_switches.h"  // S102 G2
#include "../present_fetch_plants.h"  // S102 R24-A
#include "../copy_batch.h"  // S102 G12
#include "../render_lean.h"  // S102 RT1
#include "../depth_nostencil.h"  // S102 R24-C
#include "../breadcrumbs.h"  // S102 D1
#include "../overlay.h"  // S102 D1: rdr::g_presented
#include "../frame_facts.h"  // S102 R21-A
#include "../frame_blend.h"  // S102 R21-A review F2: the tween probe
#include "../../renderer_adapter/frame_shape.h"  // S102 R21-A step 5 (tool only)
#include "../../renderer_adapter/plan_memo.h"  // S102 R23-P: the planners' input census / the memo
#include "../combine_lite.h"  // S102 R18-B2
#include "../gpu_small.h"     // S102 R24-E
#include "../group_stamps.h"  // S102 R24-F
#include "../half_trans.h"  // S102 C4
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
  std::string counts_path;  // S102 M1: --counts OUT.json: the renderer's per-frame, per-pass call counts (src/renderer/counts.h), byte-identical on a second run of the same capture
  int counts_plant = -1;    // S102 M1: --counts-plant PASS: one extra draw recorded in render pass PASS of every frame (the fault tools\budget\budget_check.py must catch)
  // S102 lane KO (test only, tools\ko\ko_check.py): --ko-draw-none = every drawn frame goes to RenderMode::NoPasses (no pass, no copy; the present step stays);
  // --ko-render-drop = every frame is dropped right after LoadFrame (no contents, bind or render: the live render thread's drop); --ko-plant = the planted twin of
  // draw_none: every second frame is rendered in full anyway (the counts must catch it).
  bool ko_draw_none = false, ko_render_drop = false, ko_plant = false;
  uint32_t delay_us = 0;  // --delay-us N: a busy wait of N us per draw counted in stats.draws (draws that reached the bind) (planted slowness for the farm's own check)
} g_farm;

// S101 lane 67: --particles-ours [--particles-fault NAME] (particle runs drawn as one draw each, src/renderer_adapter/particle_runs.h)
bool g_particles_arg = false;
std::string g_plan_memo_census_name;   // S102 R23-P: --plan-memo-census NAME: the planner-input census (plan_memo.h), printed at the end
std::string g_frame_shape_name;   // S102 R21-A step 5: --frame-shape NAME: the planner-input census of the drawn frames
bool g_tween_probe = false;   // S102 R21-A review F2: --tween-probe: TickBlender Keep(frame f) / Blend(frame f + 1, 0.5): the draws mixed (0 = no blend key reached the frame)
bool g_ff_want_keys = false;   // S102 R21-A: --frame-facts-want-keys: a consumer of the blend keys is "on" (the proof of plant 3)
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
bool g_guard_decay = false;      // S102 K1 (piece B): --guard-decay on|off (wwe2k14_rdr_guard_decay)
uint32_t g_guard_window = 16;    // S102 K1: --guard-decay-frames N (the planted twin: 1)
bool g_skip_live_nothing = true; // S102 K1: --skip-live-nothing on|off: a skipped frame with no lasting work binds and records nothing, as live.cpp
uint32_t g_guard_rule = 0;       // S102 K1 TEST ONLY: --guard-rule old = the time-based draft rule (the planted twin)
bool g_run_end_keep = false, g_run_end_peek_ignored = false;   // S102 K2 round 2: --run-end-keep on|off (wwe2k14_rdr_run_end_keep); --run-end-peek-ignored = the planted twin
uint32_t g_splice_cad = 0;   // S102 K2: --splice-cadence IMG,N
uint32_t g_splice_img = 0, g_splice_a = 0, g_splice_b = 0;   // S102 K1: --splice-reader IMG,A,B: a reader gap (frames A..B-1) spliced into the capture's per-image reads of IMG
bool g_skip_lean = false;        // S102 K1 (piece A): --skip-lean on|off (wwe2k14_rdr_skip_lean)
std::vector<uint32_t> g_hash_imgs;   // S102 K2 (diagnosis): --hash-img A,B
int g_skip_stress = 0;   // 0 off, 1 any copy, 2 only copies into a kUsageLasting destination (a real lasting seed's shape);      // S102 K2 (TEST ONLY): --skip-stress on: every skipped frame is forced to carry one lasting copy (see the skip block)
bool g_lt_census = false;        // S102 K2 (review F4): --lt-census: the write census of the lasting render targets
uint32_t g_lt_hash_every = 0;    // S102 K2 (review F4): --lt-hash N: at every Nth DRAWN frame the whole content of every lasting render target is hashed ([rplay][lthash])
uint32_t g_describe_img = 0;    // S102 K2 (diagnosis, TEST ONLY): --describe-img N: who writes and reads image N, per frame (what is img388?)
bool g_lasting_targets = false;   // S102 K2 (review F4): --lasting-targets on (wwe2k14_rdr_lasting_nogroup_targets)
bool g_lasting_nogroup = false;  // S102 K2 (piece C): --lasting-nogroup on|off (wwe2k14_rdr_lasting_nogroup): a lasting bind keeps only its seeds' passes (no model-part group rule); the binder's ordinal view is the whole frame's ops
uint32_t g_skip_hash_frames = 0;   // --skip-hash N: after each skipped frame before frame N, the contents of every lasting copy's destination rectangle are hashed ([rplay][skiphash]): the lasting state itself, not a picture of it
int g_d3_chains = 0;   // S102 K3 rework: --d3-chains on|off-frames: arm the arena + caster D3 chains as live.cpp does (the offline "armed" arm); off-frames = bound with binder.d3_chain_frame false (the armed-off frame the rework makes free)
bool g_thin_arg = false;  // lane 122: --thin arena (D1 thin draws; the reader is emulated on the capture, binder.h ThinEmulateReader)
rdr_adapter::thin::PinPublisher g_thin_pins;
rdr_adapter::particles::Fault g_particles_fault = rdr_adapter::particles::Fault::None;

// S102 M1: one JSON line per frame (fixed key order, integers only: the file is a function of the capture and the renderer's recording, nothing else).
void WriteCallCounts(std::FILE* f, const rdr::CallCounts& c) {
  std::fprintf(f, "\"draws\":%u,\"indexed_draws\":%u,\"dispatches\":%u,\"copies\":%u,\"clears\":%u,\"barriers\":%u,\"barrier_images\":%u,\"pipeline_binds\":%u,\"descriptor_binds\":%u,"
               "\"push_constants\":%u,\"dynamic_sets\":%u,\"vertex_binds\":%u,\"index_binds\":%u,\"vertices\":%llu,\"indices\":%llu,\"area_px\":%llu",
               c.draws, c.indexed_draws, c.dispatches, c.copies, c.clears, c.barriers, c.barrier_images, c.pipeline_binds, c.descriptor_binds, c.push_constants, c.dynamic_sets, c.vertex_binds,
               c.index_binds, static_cast<unsigned long long>(c.vertices), static_cast<unsigned long long>(c.indices), static_cast<unsigned long long>(c.area_px));
}

void WriteFrameCounts(std::FILE* f, uint32_t index, const rdr::FrameCounts& fc) {
  std::fprintf(f, "%s{\"frame\":%u,\"dropped\":%u,\"passes\":%u,\"steps\":%u,\"texture_sets\":%u,\"ring_constants\":%u,\"ring_geometry\":%u,\"ring_prev\":%u,\"upload_bytes\":%llu,\"total\":{", index ? ",\n" : "", index, fc.dropped, fc.passes, fc.steps,
               fc.texture_sets, fc.ring_constants, fc.ring_geometry, fc.ring_prev, static_cast<unsigned long long>(fc.upload_bytes));
  WriteCallCounts(f, fc.total);
  std::fprintf(f, "},\"pass\":[");
  for (size_t p = 0; p < fc.pass.size(); ++p) {
    const rdr::PassCounts& pc = fc.pass[p];
    std::fprintf(f, "%s{\"index\":%u,\"width\":%u,\"height\":%u,\"color_count\":%u,\"first_op\":%u,\"target_image\":%u,\"target_mip\":%u,\"target_layer\":%u,\"target_format\":%u,", p ? "," : "", pc.index, pc.width, pc.height, pc.color_count, pc.first_op,
                 pc.target_image, pc.target_mip, pc.target_layer, pc.target_format);
    WriteCallCounts(f, pc.calls);
    std::fprintf(f, "}");
  }
  std::fprintf(f, "]}");
}

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

// S102 G2: --present-shot FILE (the last offscreen output as a PNG) and the output counters one line the check reads. Called at the end of both modes.
std::string g_present_shot;
void PresentReport(Renderer& renderer, uint32_t size_w, uint32_t size_h) {
  Log("[rplay][present] size %ux%u: presents %llu, drawn capped %llu (cap %u, plant %d), through SGSR %llu, bilinear switch %d, makecap tries %llu, blit ms %.4f", size_w, size_h,
      static_cast<unsigned long long>(rdr::g_out_frames.load()), static_cast<unsigned long long>(rdr::g_out_cap_frames.load()), rdr::g_out_cap_h.load(), rdr::g_out_cap_plant.load() ? 1 : 0,
      static_cast<unsigned long long>(rdr::g_out_sgsr_frames.load()), rdr::g_out_bilinear.load() ? 1 : 0,
      static_cast<unsigned long long>(rdr::g_out_makecap_tries.load()), renderer.GpuRoles().blit_ms);
  Log("%s", rdr::PresentFetchLine("[rplay]").c_str());  // S102 R24-A
  if (g_present_shot.empty()) return;
  std::vector<uint8_t> pixels;
  uint32_t width = 0, height = 0;
  if (renderer.ReadPresent(pixels, width, height) && WritePng(g_present_shot.c_str(), pixels.data(), width, height)) Log("[rplay] wrote %s (%ux%u, the presented output)", g_present_shot.c_str(), width, height);
  else Log("[rplay] could not read the presented output (--present-shot needs --present-size)");
}
uint32_t g_present_w = 0, g_present_h = 0;

// S102 R21-B (docs\renderer\STAGE_SPLIT.md): the two-stage render thread in the capture replay. --stage-split on: frame f+1 is bound on this thread while
// rdr_record records frame f (renderer_adapter\stage_split.h, the game's own code). Tool-only spins emulate the Surface's stage costs on a fast PC.
bool g_stage_split = false;     // --stage-split on|off
uint32_t g_bind_spin_us = [] { const char* e = std::getenv("RDR_TEST_BIND_SPIN_US"); return e ? uint32_t(std::atoi(e)) : 0u; }();  // --bind-spin-us N (or live.cpp's env): a busy spin before each Bind
uint32_t g_record_spin_us = 0;  // --record-spin-us N: a busy spin before each RenderFrame (both paths)
std::string g_frame_hashes;     // --frame-hashes CSV: per frame, an FNV-1a hash of the image the frame presents, read after the frame (both paths)
int g_stage_selftest = 0;       // --stage-selftest N: N synthetic frames (stage_selftest.h); with --stage-split on through the stage
void SpinUs(uint32_t us) {
  if (us) for (const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us); std::chrono::steady_clock::now() < until;) {}
}
// The hash line of one rendered frame (the thread that rendered it, right after its RenderFrame).
std::string PresentHash(Renderer& renderer, const rdr::Frame& frame) {
  if (!frame.presents.count) return "none";
  std::vector<uint8_t> px;
  uint32_t w = 0, h = 0;
  if (!renderer.ReadImage(frame.presents[frame.presents.count - 1].source, px, w, h)) return "unreadable";
  uint64_t hash = 1469598103934665603ull;
  for (uint8_t b : px) hash = (hash ^ b) * 1099511628211ull;
  char text[48];
  std::snprintf(text, sizeof text, "%ux%u:%016llx", w, h, static_cast<unsigned long long>(hash));
  return text;
}

// S102 G10: --arms "label=MODE:NAMES|label=MODE:NAMES|...": frame f is drawn under arm f % N (rdr::SetDrawMod; the first arm is usually "base=0:"), and the end of the run prints each arm's stamped GPU ms
// (colour, work, total, copies, shadows, the rest) as mean and median over its frames after the first --arm-skip. Interleaving the arms frame by frame makes a ranking out of one process (no per-arm start-up).
struct ArmSpec { std::string label; int mode = 0; std::string names; };
std::vector<ArmSpec> g_arms;
int g_arm_skip = 8;
int g_arm_period = 1;        // S102 R24-D: --arm-period N: the arm changes every N frames (arm = f / N % arms; default 1 = every frame, as before): a mid-run flip every N frames, as the Surface's in-run A/B does
bool g_arena_scan_follow = false;  // S102 R24-D: --arena-scan-follow: a mode 4 arm sets the texture-alpha scan flag per frame from its arena bit (as live.cpp does) instead of leaving it on for the whole replay
void ParseArms(const std::string& spec) {
  size_t at = 0;
  while (at <= spec.size()) {
    size_t end = spec.find('|', at);
    if (end == std::string::npos) end = spec.size();
    const std::string item = spec.substr(at, end - at);
    const size_t eq = item.find('='), colon = item.find(':');
    if (eq != std::string::npos && colon != std::string::npos && colon > eq) g_arms.push_back({item.substr(0, eq), std::atoi(item.substr(eq + 1, colon - eq - 1).c_str()), item.substr(colon + 1)});
    if (!g_arms.empty() && g_arms.back().mode == 6) rdr_adapter::prelight_compute::g_storage_always.store(true);   // S102 L1b: both arms' cubes are made with storage usage
    if (!g_arms.empty() && g_arms.back().mode == 4) rdr::colour_cuts::g_arena_overdraw_scan.store(true);  // S102 C4: the texture-alpha scan runs for the whole replay when an arm may switch the arena overdraw on
    at = end + 1;
  }
}
void ArmsReport() {
  std::lock_guard<std::mutex> lock(rdr::g_arm_mutex);
  auto stats = [](std::vector<float> v, size_t skip, double& mean, double& median) {
    if (v.size() > skip) v.erase(v.begin(), v.begin() + std::ptrdiff_t(skip)); else v.clear();
    mean = median = 0;
    if (v.empty()) return;
    double sum = 0;
    for (float x : v) sum += x;
    mean = sum / double(v.size());
    std::sort(v.begin(), v.end());
    median = v[v.size() / 2];
  };
  for (const auto& item : rdr::g_arm_samples) {
    const std::string label = item.first >= 0 && size_t(item.first) < g_arms.size() ? g_arms[size_t(item.first)].label : std::string("arm") + std::to_string(item.first);
    double m[6][2];
    const std::vector<float>* series[6] = {&item.second.colour, &item.second.work, &item.second.total, &item.second.copies, &item.second.shadow, &item.second.other};
    for (int k = 0; k < 6; ++k) stats(*series[k], size_t(g_arm_skip), m[k][0], m[k][1]);
    Log("[rplay][arm] %s: frames %zu, mean/median ms: colour %.4f/%.4f work %.4f/%.4f total %.4f/%.4f copies %.4f/%.4f shadows %.4f/%.4f rest %.4f/%.4f", label.c_str(), item.second.colour.size() > size_t(g_arm_skip) ? item.second.colour.size() - size_t(g_arm_skip) : size_t(0),
        m[0][0], m[0][1], m[1][0], m[1][1], m[2][0], m[2][1], m[3][0], m[3][1], m[4][0], m[4][1], m[5][0], m[5][1]);
  }
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
  rdr::TickBlender tween_probe;   // S102 R21-A review F2
  uint64_t tp_frames = 0, tp_matched = 0;
  rdr_adapter::FrameShape frame_shape;   // S102 R21-A step 5 (tool only)
  binder.CreateImages(capture);
  binder.SetBlendKeysWanted(g_ff_want_keys);   // S102 R21-A: only read with --frame-facts on
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
    if (g_d3_chains) {   // S102 K3 rework: the chains armed as live.cpp arms them (no hook, no records: every frame is a full one, the armed-off shape)
      if (g_thin_arg) {
        rdr_adapter::d3::g_arena_d3 = true;
        binder.D3SetRecordRoute(rdr_adapter::d3::kRecordRouteBuilt, false, -1);
      }
      rdr_adapter::d3::g_caster_d3 = true;
      binder.SetD3CasterLive(rdr_adapter::d3::kRecordRouteBuilt, false, false);
      Log("[rplay] D3 chains armed (arena %s, casters), frames bound %s the chains", g_thin_arg ? "yes" : "no (needs --thin arena)", g_d3_chains == 2 ? "WITHOUT" : "with");
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
  std::FILE* counts_file = nullptr;  // S102 M1
  if (!g_farm.counts_path.empty()) {
    counts_file = std::fopen(g_farm.counts_path.c_str(), "wb");  // "wb": no CRLF, so the bytes are the same on any host
    if (!counts_file) {
      Log("[rplay] could not write %s", g_farm.counts_path.c_str());
      return 2;
    }
    std::fprintf(counts_file, "{\"format\":\"rdr-counts\",\"version\":1,\"frames\":[\n");
    renderer.SetCountsPlant(g_farm.counts_plant);
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
  std::map<uint32_t, uint32_t> lt_writes, lt_last;   // S102 K2 (review F4): lasting render target -> frames that write it / the last such frame
  std::vector<uint8_t> skip_forced;   // S102 K2: --skip-stress: the op indices of the frame whose seed was forced (not the classifier's)
  std::vector<rdr_adapter::Op> skip_ops, skip_dropped;
  std::unordered_map<uint32_t, const rdr_adapter::ImageRecord*> skip_images;
  for (const rdr_adapter::ImageRecord& r : capture.images()) skip_images[r.id] = &r;
  struct SkipTotals { uint32_t frames = 0, ops_in = 0, ops_kept = 0, draws_in = 0, draws_kept = 0, passes_in = 0, passes_kept = 0, seeds = 0, grouped = 0, bound = 0; double bind_us = 0; } skip_total;
  double k2_lasting_us = 0, k2_bind_us = 0; uint32_t k2_lasting_n = 0, k2_obj_passes = 0, k2_obj_kept = 0, k2_obj_dropped = 0, k2_lasting_targets = 0;   // S102 K2: the render thread's cost of the skipped frames that carry a lasting bind (same measure as k1_skip_us), object passes of those frames
  uint32_t ko_dropped = 0;  // S102 lane KO
  double k1_part_us[2][3] = {};   // [skipped, drawn][contents, bind, render]
  double k1_note_us = 0, k1_skip_us = 0, k1_drawn_us = 0; uint32_t k1_skip_n = 0, k1_drawn_n = 0, k1_lasting_binds = 0;   // S102 K1: the render thread's own cost of a frame handled, by kind (load + contents + classify + bind + record submit; no GPU wait)
  // S102 R21-B: the stage (after the warm-up: those frames stay on this thread). The per-frame read-backs of the other tools would read the frame the
  // record stage is on, not this one: refused with the split, except --frame-hashes, which the record stage itself takes after each frame.
  std::unique_ptr<rdr_adapter::stage_split::RecordStage> stage;
  std::FILE* hashes = nullptr;
  if (!g_frame_hashes.empty() && !(hashes = std::fopen(g_frame_hashes.c_str(), "wb"))) {
    Log("[rplay] could not write %s", g_frame_hashes.c_str());
    return 2;
  }
  if (g_stage_split) {
    if (timing || counts_file || gpu_log || g_skip_every || g_lt_hash_every || !g_farm.shots.empty() || g_farm.ko_draw_none || g_farm.ko_render_drop || g_persist_every) {
      Log("[rplay] --stage-split on cannot run with --timing / --counts / --gpu-log / --skip-every / --lt-hash / --shots / --ko-* / --persist-only-every (they read the frame after its RenderFrame on this thread)");
      return 2;
    }
    stage = std::make_unique<rdr_adapter::stage_split::RecordStage>(renderer, nullptr, [&renderer, hashes](const rdr::Frame& done, const rdr_adapter::stage_split::Packet&) {
      if (hashes) std::fprintf(hashes, "%llu,%s\n", static_cast<unsigned long long>(done.tick), PresentHash(renderer, done).c_str());
    });
  }
  const double loop_t0 = NowUs();
  double st_load = 0, st_contents = 0, st_bind = 0, st_hand = 0;   // S102 R21-B: this thread's time a frame by part (the [rplay][stage] line)
  rdr::ThreadCpuMeter bind_cpu;   // S102 R21-B (review F2): this thread's CPU (the game's render_thread_ms) and rdr_record's (record_thread_ms), apart
  const uint64_t record_cpu0 = rdr::g_record_thread_ns_total.load();
  for (uint32_t f = 0; f < count; ++f) {
    const double t0 = NowUs();  // S101 lane22
    const bool k1_loaded = capture.LoadFrame(f, frame);
    const double k1_load = NowUs() - t0;   // S102 K1: the capture file read stands in for the live hand-off (not this thread's work in the game)
    if (!k1_loaded) {
      Log("[rplay] could not read frame %u", f);
      return 3;
    }
    if (g_lt_census) {   // S102 K2 (review F4): which frames write each lasting render target (kUsageLasting + target): every frame, or only now and then?
      std::unordered_set<uint32_t> seen;
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        if (frame.ops[i].kind != rdr_adapter::OpKind::SetTargets) continue;
        const rdr::TargetsOp& tt = frame.targets[frame.ops[i].index];
        for (uint32_t c = 0; c < tt.color_count; ++c) {
          const uint32_t id = uint32_t(tt.color[c].image);
          const auto it = skip_images.find(id);
          if (it != skip_images.end() && (it->second->usage & rdr::kUsageLasting) && seen.insert(id).second) { ++lt_writes[id]; lt_last[id] = f; }
        }
      }
    }
    if (g_describe_img) {   // S102 K2 (diagnosis, TEST ONLY): what one image is and who writes / reads it, per frame, from the capture alone (--describe-img N): ops in frame order, the pass target each read happens in
      const auto desc = [&](uint32_t id) -> std::string {
        char b[160];
        const auto it = skip_images.find(id);
        if (it == skip_images.end()) { std::snprintf(b, sizeof b, "img%u (no record)", id); return b; }
        const rdr_adapter::ImageRecord& r = *it->second;
        std::snprintf(b, sizeof b, "img%u %ux%u fmt%u kind%u usage%x src%u flags%x role%u roleflags%x", id, unsigned(r.width), unsigned(r.height), unsigned(r.format), unsigned(r.kind), unsigned(r.usage), unsigned(r.source), unsigned(r.flags), unsigned(r.role), unsigned(r.role_flags));
        return b;
      };
      if (f == 0) Log("[rplay][describe] %s", desc(g_describe_img).c_str());
      uint32_t tgt = 0, pass_no = 0;
      uint32_t reads = 0, writes_copy = 0, copies_from = 0, drawn_to = 0;
      std::string line;
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const rdr_adapter::Op& op = frame.ops[i];
        if (op.kind == rdr_adapter::OpKind::SetTargets) { tgt = uint32_t(frame.targets[op.index].color[0].image), ++pass_no; if (uint32_t(frame.targets[op.index].color[0].image) == g_describe_img) ++drawn_to; }
        else if (op.kind == rdr_adapter::OpKind::Draw) {
          const rdr_adapter::Draw& d = frame.draws[op.index];
          bool hit = false;
          for (uint32_t k = 0; k < d.texture_count; ++k) hit |= uint32_t(frame.texture_slots[d.texture_first + k].image) == g_describe_img;
          if (hit && (reads++ == 0 || f < 3)) { char b[200]; std::snprintf(b, sizeof b, " [read in pass %u (target %s)]", pass_no, desc(tgt).c_str()); line += b; }
        } else if (op.kind == rdr_adapter::OpKind::Copy) {
          const rdr::CopyOp& c = frame.copies[op.index];
          if (uint32_t(c.dest.image) == g_describe_img) { ++writes_copy; char b[260]; std::snprintf(b, sizeof b, " [copy INTO it from %s rect %d,%d %ux%u at op %u]", desc(uint32_t(c.source.image)).c_str(), c.dest_rect.x, c.dest_rect.y, unsigned(c.dest_rect.width), unsigned(c.dest_rect.height), i); line += b; }
          if (uint32_t(c.source.image) == g_describe_img) { ++copies_from; char b[200]; std::snprintf(b, sizeof b, " [copy FROM it into %s]", desc(uint32_t(c.dest.image)).c_str()); line += b; }
        }
      }
      if (drawn_to) {   // the passes that draw to it: their other targets, whether those are cleared first, and which earlier passes of the frame write the same targets
        for (uint32_t i = 0; i < frame.ops.count; ++i) {
          if (frame.ops[i].kind != rdr_adapter::OpKind::SetTargets) continue;
          const rdr::TargetsOp& t = frame.targets[frame.ops[i].index];
          if (uint32_t(t.color[0].image) != g_describe_img) continue;
          std::string l = " pass@op" + std::to_string(i) + " colour";
          for (uint32_t c = 0; c < t.color_count; ++c) l += " " + desc(uint32_t(t.color[c].image));
          l += "; depth " + (t.depth.image != rdr::kNoImage ? desc(uint32_t(t.depth.image)) : std::string("none"));
          uint32_t clears = 0, draws_in_pass = 0, obj_in_pass = 0;
          for (uint32_t k = i + 1; k < frame.ops.count && frame.ops[k].kind != rdr_adapter::OpKind::SetTargets; ++k) {
            if (frame.ops[k].kind == rdr_adapter::OpKind::Clear) {   // review F2a: what the pass clears (flags: 1 colour, 2 depth, 4 stencil as rdr::kClear*; full; colour mask; rect)
              const rdr::ClearOp& cl = frame.clears[frame.ops[k].index];
              char cb[160];
              std::snprintf(cb, sizeof cb, " [clear flags %x full %d colour mask %x rect %d,%d %ux%u]", unsigned(cl.flags), int(cl.full), unsigned(cl.color_mask), cl.rect.x, cl.rect.y, unsigned(cl.rect.width), unsigned(cl.rect.height));
              l += cb;
            }
            clears += frame.ops[k].kind == rdr_adapter::OpKind::Clear;
            if (frame.ops[k].kind == rdr_adapter::OpKind::Draw) {
              ++draws_in_pass;
              const rdr_adapter::DrawOrigin* o = frame.OriginOf(frame.ops[k].index);
              obj_in_pass += o && o->object;
              if (o) { char ob[160]; std::snprintf(ob, sizeof ob, " [draw origin class %u site %08X object %08X part %u call_site %08X pass_site-ordinal-free]", unsigned(o->klass), unsigned(o->pass_site), unsigned(o->object), unsigned(o->part), unsigned(o->call_site)); l += ob; }
            }
          }
          l += "; clears in pass " + std::to_string(clears) + ", draws " + std::to_string(draws_in_pass) + " (engine-object " + std::to_string(obj_in_pass) + ")";
          for (uint32_t k = i + 1; k < frame.ops.count && frame.ops[k].kind != rdr_adapter::OpKind::SetTargets; ++k) {   // what its draws sample, and who writes that earlier in the frame
            if (frame.ops[k].kind != rdr_adapter::OpKind::Draw) continue;
            const rdr_adapter::Draw& d = frame.draws[frame.ops[k].index];
            for (uint32_t q = 0; q < d.texture_count; ++q) {
              const uint32_t x = uint32_t(frame.texture_slots[d.texture_first + q].image);
              l += " {samples " + desc(x) + "; written earlier by:";
              uint32_t cur = 0;
              for (uint32_t m = 0; m < k; ++m) {
                const rdr_adapter::Op& w = frame.ops[m];
                if (w.kind == rdr_adapter::OpKind::SetTargets) cur = m;
                else if (w.kind == rdr_adapter::OpKind::Copy && uint32_t(frame.copies[w.index].dest.image) == x) l += " copy@op" + std::to_string(m);
                if (w.kind == rdr_adapter::OpKind::SetTargets && uint32_t(frame.targets[w.index].color[0].image) == x) l += " pass@op" + std::to_string(m);
              }
              (void)cur;
              l += "}";
            }
          }
          if (t.depth.image != rdr::kNoImage) {
            uint32_t earlier = 0, earlier_obj = 0;
            for (uint32_t k = 0; k < i; ++k) {
              if (frame.ops[k].kind != rdr_adapter::OpKind::SetTargets) continue;
              const rdr::TargetsOp& e = frame.targets[frame.ops[k].index];
              if (e.depth.image != t.depth.image) continue;
              ++earlier;
              for (uint32_t m = k + 1; m < frame.ops.count && frame.ops[m].kind != rdr_adapter::OpKind::SetTargets; ++m)
                if (frame.ops[m].kind == rdr_adapter::OpKind::Draw) { const rdr_adapter::DrawOrigin* o = frame.OriginOf(frame.ops[m].index); if (o && o->object) { ++earlier_obj; break; } }
            }
            l += "; earlier passes with the same depth image " + std::to_string(earlier) + " (with an engine-object draw " + std::to_string(earlier_obj) + ")";
          }
          line += l;
        }
      }
      if (reads || writes_copy || copies_from || drawn_to) Log("[rplay][describe] frame %u: draws reading it %u, copies into it %u, copies from it %u, passes drawing to it %u:%s", f, reads, writes_copy, copies_from, drawn_to, line.c_str());
    }
    if (g_farm.ko_render_drop) {  // S102 lane KO: dropped at once (the live sink's ko_render_drop): the load above is the hand-off's, nothing else runs
      ++ko_dropped;
      if (timing) std::fprintf(timing, "%u,0,%.1f,0,0,0,0\n", f, NowUs() - t0);
      continue;
    }
    if (g_wide_factor > 1.001f || g_tall_factor > 1.001f) g_wide_replay.Apply(frame, capture);  // lane 171
    if (g_jitter_replay.on) g_jitter_replay.Apply(frame, capture);  // lane 185 (after the wide rule, as the live reader: ApplyWide then ApplyJitter)
    if (!g_arms.empty()) {  // S102 G10: this frame's arm
      const size_t arm = (f / size_t(g_arm_period > 0 ? g_arm_period : 1)) % g_arms.size();   // S102 R24-D: --arm-period
      rdr::g_arm_tag.store(int(arm));
      if (g_arms[arm].mode == 3) {  // S102 G10: arm kind 3 = the colour-cut mask (NAMES is the number) for this frame, no draw change
        rdr::SetDrawMod(0, "");
        rdr::colour_cuts::g_colour_cuts.store(uint32_t(std::atoi(g_arms[arm].names.c_str())));
      } else if (g_arms[arm].mode == 4) {  // S102 C4: arm kind 4 = the lever bits (NAMES is the number) for this frame: 1 = the arena overdraw, 2 = the half-size transparents
        rdr::SetDrawMod(0, "");
        const int bits = std::atoi(g_arms[arm].names.c_str());
        rdr::colour_cuts::g_arena_overdraw.store((bits & 1) != 0);
        if (g_arena_scan_follow) rdr::colour_cuts::g_arena_overdraw_scan.store((bits & 1) != 0);  // S102 R24-D: live.cpp's rule (the scan runs only on frames the switch is on)
        rdr::colour_cuts::g_half_transparents.store((bits & 2) != 0);
      } else if (g_arms[arm].mode == 5) {  // S102 L1: arm kind 5 = the analytic pre-light switch (NAMES "1" on, anything else off) for this frame, no draw change
        rdr::SetDrawMod(0, "");
        rdr_adapter::prelight_analytic::g_fview.store(g_arms[arm].names == "1");
      } else if (g_arms[arm].mode == 7) {  // S102 R24-C: arm kind 7 = stencil_dead (NAMES "1" on, anything else off) for this frame, no draw change
        rdr::SetDrawMod(0, "");
        rdr::g_stencil_dead.store(g_arms[arm].names == "1");
      } else if (g_arms[arm].mode == 8) {  // S102 R24-E: arm kind 8 (main session: 7 was R24-C's) = the small-passes mask (NAMES is the number) for this frame, no draw change
        rdr::SetDrawMod(0, "");
        rdr::g_gpu_small.store(rdr::ClampGpuSmall(std::atoi(g_arms[arm].names.c_str())));
        const size_t colon = g_arms[arm].names.find(':');   // "MASK:X" also forces DOFParam2.x to X (a tool: the captures never cross x = 0)
        rdr::g_gpu_small_force.store(colon != std::string::npos);
        if (colon != std::string::npos) rdr::g_gpu_small_force_x.store(rdr::CombineLiteBits(float(std::atof(g_arms[arm].names.c_str() + colon + 1))));
      } else if (g_arms[arm].mode == 6) {  // S102 L1b: arm kind 6 = the compute pre-light switch (NAMES "1" on, anything else off) for this frame, no draw change (the cubes carry storage usage in both arms)
        rdr::SetDrawMod(0, "");
        rdr_adapter::prelight_compute::g_fview.store(g_arms[arm].names == "1");
      } else {
        rdr::SetDrawMod(g_arms[arm].mode, g_arms[arm].names);
      }
    }
    const double k1_c0 = NowUs();   // S102 K1: the split of a handled frame's cost: contents, bind (+ finish), record + submit
    binder.ApplyContents(frame, capture);
    rig.Feed(f);  // S101 lane 47
    const double t1 = NowUs();  // S101 lane22
    const uint32_t draws_before = stats.draws;
    FrameBuilder& fb = stage ? stage->Acquire() : builder;   // S102 R21-B: the stage's slot for this frame (waits while frame f-2 is still being recorded)
    fb.Begin(f);
    stats.list = f + 1 == count && list_bound;
    stats.isolate = isolate.empty() ? nullptr : isolate.c_str();
    stats.gallery = f + 1 == count && !gallery_dir.empty();
    if (g_thin_arg) binder.ThinEmulateReader(frame, 2);  // lane 122
    binder.d3_chain_frame = g_d3_chains != 2;   // S102 K3 rework
    if (g_d3_chains) ++(g_d3_chains == 2 ? rdr_adapter::d3t::g_frames_off : rdr_adapter::d3t::g_frames_chain);
    binder.persist_only_bind = !g_persist_plant && g_persist_every && f % g_persist_every == g_persist_every - 1;   // S101 lane 224
    bool skipped_frame = false;   // lane 307
    bool k2_lasting_frame = false;   // S102 K2: this skipped frame carries a lasting bind
    const rdr_adapter::Table<rdr_adapter::Op> all_ops = frame.ops;
    if (g_skip_every) {
      skip_streaks.by_image = g_skip_by_image;
      skip_streaks.guard_decay = g_guard_decay;
      skip_streaks.guard_window = g_guard_window;
      skip_streaks.lean = g_skip_lean;
      skip_streaks.guard_rule = g_guard_rule == 1 ? 1 : 0;
      skip_streaks.cadence_blind = g_guard_rule == 2;   // S102 K2: --guard-rule blind = the rule before the cadence fix (the planted twin)
      skip_streaks.images = &capture.images();
      // S102 K1 (review F6): the splice. In frames outside [A,B) a read of IMG BEFORE its copy is added (the state reader is present); inside, every read of IMG is removed (the reader is gone). The
      // classifier and the stale-read ledger see this frame; binding still uses the real one (its classification does not feed the ledger).
      std::vector<rdr_adapter::Op> sp_ops;
      std::vector<rdr_adapter::Draw> sp_draws;
      std::vector<rdr_adapter::TextureSlot> sp_slots;
      rdr_adapter::LoadedFrame sp_frame;
      const rdr_adapter::LoadedFrame* nf = &frame;
      if (g_splice_img) {
        const bool gap = f >= g_splice_a && f < g_splice_b;
        sp_slots.assign(frame.texture_slots.data, frame.texture_slots.data + frame.texture_slots.count);
        sp_draws.assign(frame.draws.data, frame.draws.data + frame.draws.count);
        if (gap || g_splice_cad) for (rdr_adapter::TextureSlot& t : sp_slots) if (t.image == g_splice_img) t.image = 0;
        // S102 K2 (state-read cadence, SKIP_COST.md K2 section "stale reads with the guard decay"): --splice-cadence IMG,N: every read of IMG is removed; every Nth frame gets a state reader BEFORE the frame's copy into IMG (a read
        // that carries content from the last frame: the guard's evidence), all the other frames a reader AFTER it (a per-frame use: the guard's positive evidence). A read cadence longer than the guard window.
        const bool cad_state = g_splice_cad && f % g_splice_cad == 0;
        if (!gap && (!g_splice_cad || cad_state)) {
          rdr_adapter::Draw d;
          d.texture_first = uint32_t(sp_slots.size());
          d.texture_count = 1;
          rdr_adapter::TextureSlot t;
          t.image = g_splice_img;
          sp_slots.push_back(t);
          sp_draws.push_back(d);
          sp_ops.push_back({rdr_adapter::OpKind::Draw, uint32_t(sp_draws.size() - 1)});
        }
        sp_ops.insert(sp_ops.end(), all_ops.data, all_ops.data + all_ops.count);
        if (g_splice_cad && !cad_state) {   // the per-frame reader, after everything (so after the copy into IMG)
          rdr_adapter::Draw d;
          d.texture_first = uint32_t(sp_slots.size());
          d.texture_count = 1;
          rdr_adapter::TextureSlot t;
          t.image = g_splice_img;
          sp_slots.push_back(t);
          sp_draws.push_back(d);
          sp_ops.push_back({rdr_adapter::OpKind::Draw, uint32_t(sp_draws.size() - 1)});
        }
        sp_frame.ops = {sp_ops.data(), uint32_t(sp_ops.size())};
        sp_frame.draws = {sp_draws.data(), uint32_t(sp_draws.size())};
        sp_frame.texture_slots = {sp_slots.data(), uint32_t(sp_slots.size())};
        sp_frame.copies = frame.copies;
        nf = &sp_frame;
      }
      { const double tn = NowUs(); skip_streaks.Note(*nf); k1_note_us += NowUs() - tn; }   // S102 K1: the per-frame bookkeeping a handled frame always pays (drawn or not)
      skipped_frame = f % g_skip_every == g_skip_every - 1;
      if (!skipped_frame) skip_streaks.NoteDrawn(*nf);   // S102 K1 (review F5): a drawn frame clears what skipped frames owed
      if (skipped_frame) {
        if (g_splice_img) {   // the ledger sees the spliced frame's classification; the binding below uses the real one
          std::vector<uint8_t> sp_seed;
          skip_streaks.Classify(*nf, false, sp_seed, [](const rdr::CopyOp&) {});
          skip_streaks.ledger = false;
        }
        skip_streaks.Classify(frame, false, skip_seed, [](const rdr::CopyOp&) {});
        skip_streaks.ledger = true;
        if (g_run_end_keep && skipped_frame) {   // S102 K2 round 2: the run-end recovery: the next frame's copy table (live: the queued message) decides which left-out copies end a run
          std::vector<rdr::CopyOp> next;
          rdr_adapter::LoadedFrame nxt;
          if (g_run_end_peek_ignored || f + 1 >= count || !capture.LoadFrame(f + 1, nxt)) {
            for (uint32_t i = 0; i < frame.copies.count; ++i) next.push_back(frame.copies[i]);   // no next frame / the planted twin: every destination counts as written next
          } else {
            for (uint32_t i = 0; i < nxt.copies.count; ++i) next.push_back(nxt.copies[i]);
          }
          skip_streaks.KeepRunEnds(frame, skip_seed, next);
          skip_streaks.CommitOwed();   // round 3 F1: the keeps clear the debt when committed (Classify has already committed the owing)
        }
        skip_forced.assign(all_ops.count, 0);
        if (g_skip_stress) {   // S102 K2 (TEST ONLY): every skipped frame carries one forced lasting copy (the f-th copy op of the frame): the lasting bind runs on every skipped frame, as the Surface's does when a guard holds a moving copy
          std::vector<uint32_t> cand;
          for (uint32_t i = 0; i < all_ops.count; ++i) {
            if (all_ops[i].kind != rdr_adapter::OpKind::Copy) continue;
            if (g_skip_stress == 2) {   // only a copy into a lasting-usage image (what a real lasting seed writes)
              const auto di = skip_images.find(uint32_t(frame.copies[all_ops[i].index].dest.image));
              if (di == skip_images.end() || !(di->second->usage & rdr::kUsageLasting)) continue;
            }
            cand.push_back(i);
          }
          if (!cand.empty()) {
            const uint32_t i = cand[f % cand.size()];
            if (!skip_seed[i]) skip_forced[i] = 1;
            skip_seed[i] = 1;
          }
        }
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
          skip_pruner.group = !g_lasting_nogroup;   // S102 K2
          skip_pruner.lasting_targets = g_lasting_targets;   // review F4: --lasting-targets on
          skip_pruner.images = &capture.images();
          skip_pruner.plant_no_lasting_target = g_skip_prune == "nolasttarget";
          skip_pruner.plant_no_attach = g_skip_prune == "noattach";   // S102 K2 (review F1)
          skip_pruner.group_plant = g_skip_prune == "sitegroup" ? 2 : g_skip_prune == "grpbefore" ? 3 : g_skip_prune == "grpafter" ? 4 : 0;   // S102 K2 (review F2c, diagnosis)
          skip_pruner.Select(frame, skip_seed, skip_ops, skip_dropped, ps, g_skip_prune == "plant");
          k2_obj_passes += ps.object_passes, k2_obj_kept += ps.object_passes_kept, k2_obj_dropped += ps.object_passes_dropped, k2_lasting_targets += ps.lasting_target_passes;
          if (g_describe_img)   // S102 K2 (diagnosis): which ops that touch the described image this lasting bind keeps
            for (uint32_t i = 0; i < all_ops.count; ++i) {
              const rdr_adapter::Op& op = all_ops[i];
              const bool kept = skip_pruner.kept_mask.size() == all_ops.count && skip_pruner.kept_mask[i];
              if (op.kind == rdr_adapter::OpKind::Copy && (uint32_t(frame.copies[op.index].dest.image) == g_describe_img || uint32_t(frame.copies[op.index].source.image) == g_describe_img))
                Log("[rplay][describe] skipped frame %u (nogroup %s): copy op %u %s img%u -> img%u: %s%s", f, g_lasting_nogroup ? "on" : "off", i, uint32_t(frame.copies[op.index].dest.image) == g_describe_img ? "INTO" : "FROM", unsigned(frame.copies[op.index].source.image), unsigned(frame.copies[op.index].dest.image), kept ? "KEPT" : "left out", skip_seed[i] ? " (lasting seed)" : "");
            }
          if (g_lasting_nogroup) binder.ord_full_ops = &all_ops, binder.ord_keep = &skip_pruner.kept_mask;   // S102 K2: the D3 / thin ordinals over the whole frame (a no-op while the chains are off)
          binder.d3_skip_observe = ps.d3_blind();
          skip_total.ops_in += ps.ops, skip_total.ops_kept += ps.kept_ops, skip_total.draws_in += ps.draws, skip_total.draws_kept += ps.kept_draws, skip_total.passes_in += ps.passes, skip_total.passes_kept += ps.kept_passes, skip_total.seeds += ps.seeds, skip_total.grouped += ps.grouped_kept;
        }
        if (any && g_skip_prune != "truth") ++k1_lasting_binds, k2_lasting_frame = true;
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
    if (!g_frame_shape_name.empty() && !skipped_frame) frame_shape.Note(frame);   // S102 R21-A step 5
    const double bind_t0 = NowUs();
    const uint32_t bound_before = stats.bound;
    if (!skipped_frame) binder.frame_part_open = (frame.header.flags & rdr_adapter::kFramePart) != 0;   // S102 G3 (review F1): a captured DrawAndRead part is bound as live binds it
    // S102 K1: live.cpp binds and records NOTHING for a skipped frame with no lasting work (it is dropped after its contents); rplay did, so a skipped frame paid a full empty RenderFrame (slot fence + compile) the live thread never pays.
    // --skip-live-nothing on (default) models the live path; off = the old rplay behaviour. Only frames whose lasting bind is empty are affected.
    const bool k1_nothing = g_skip_live_nothing && skipped_frame && g_skip_prune != "truth" && skip_ops.empty();
    SpinUs(g_bind_spin_us);   // S102 R21-B: tool only (live.cpp's RDR_TEST_BIND_SPIN_US): emulates the Surface's bind
    if (!k1_nothing) binder.Bind(frame, capture, fb, stats);
    if (!skipped_frame) binder.frame_part_open = false;
    if (skipped_frame) {
      skip_total.frames++;
      skip_total.bound += stats.bound - bound_before;
      skip_total.bind_us += NowUs() - bind_t0;
      if (k2_lasting_frame) k2_bind_us += NowUs() - bind_t0;   // S102 K2: the CPU-only cost of the lasting binds (no GPU wait in it)
      frame.ops = all_ops;
      binder.frame_part_open = false;
    }
    binder.persist_only_bind = false;
    binder.d3_skip_observe = false;
    binder.ord_full_ops = nullptr, binder.ord_keep = nullptr;   // S102 K2
    if (g_jitter_replay.on) {  // lane 185
      g_velocity_sum += binder.velocity_draw_count();
      if (g_jitter_replay.census) std::printf("[jitter] frame %u velocity draws %zu\n", f, binder.velocity_draw_count());
    }
    if (g_farm.delay_us) {  // S101 lane22: planted slowness, per bound draw
      const double end = NowUs() + double(g_farm.delay_us) * double(stats.draws - draws_before);
      while (NowUs() < end) {}
    }
    rdr::Frame finished = stage ? rdr::Frame{} : fb.Finish();   // S102 R21-B: the stage finishes its slot itself (Submit)
    if (g_tween_probe && !skipped_frame && !stage) {   // S102 merge: the probe needs the finished frame on this thread (not with --stage-split)
      // S102 R21-A review F2: the halfway picture of live.cpp, as a count
      if (tween_probe.valid()) {
        uint32_t matched = 0;
        tween_probe.Blend(finished, 0.5f, matched);
        tp_matched += matched, ++tp_frames;
      }
      tween_probe.Keep(finished);
    }
    const double t2 = NowUs();  // S101 lane22
    if (frame.header.flags & rdr_adapter::kFramePart) finished.keep_contents = true;  // S101 lane 11: a DrawAndRead part (live.cpp does the same)
    const double gpu_t0 = NowUs();
    const bool ko_none_now = g_farm.ko_draw_none && !(g_farm.ko_plant && (f & 1) == 0);  // S102 lane KO (the plant leaves every second frame fully recorded)
    if (stage) {   // S102 R21-B: handed over; rdr_record records it while the next frame is bound here
      rdr_adapter::stage_split::Packet packet;
      packet.guest_frame = f;
      packet.keep_contents = (frame.header.flags & rdr_adapter::kFramePart) != 0;
      packet.record_spin_us = g_record_spin_us;
      stage->Submit(packet);
    } else if (!k1_nothing) {
      SpinUs(g_record_spin_us);   // S102 R21-B: tool only: emulates the Surface's record
      renderer.RenderFrame(finished, ko_none_now ? rdr::RenderMode::NoPasses : skipped_frame && g_skip_prune != "truth" ? rdr::RenderMode::PersistentOnly : rdr::RenderMode::Full);
      if (hashes) std::fprintf(hashes, "%llu,%s\n", static_cast<unsigned long long>(finished.tick), PresentHash(renderer, finished).c_str());
    }
    st_load += k1_c0 - t0, st_contents += t1 - k1_c0, st_bind += t2 - t1, st_hand += NowUs() - t2;   // S102 R21-B
    if (g_skip_every && k2_lasting_frame) k2_lasting_us += NowUs() - t0 - k1_load, ++k2_lasting_n;   // S102 K2: the cost of a skipped frame that carries a lasting bind (the same stopwatch as k1_skip_us)
    if (g_skip_every) { (skipped_frame ? k1_skip_us : k1_drawn_us) += NowUs() - t0 - k1_load; ++(skipped_frame ? k1_skip_n : k1_drawn_n); k1_part_us[skipped_frame ? 0 : 1][0] += t1 - k1_c0; k1_part_us[skipped_frame ? 0 : 1][1] += t2 - t1; k1_part_us[skipped_frame ? 0 : 1][2] += NowUs() - gpu_t0; }   // S102 K1
    if (!skipped_frame && g_lt_hash_every && f > 0 && f % g_lt_hash_every == 0) {   // S102 K2 (review F4): the lasting render targets at a drawn frame: truth vs ON vs ON + sub-switch
      renderer.WaitIdle();
      for (const rdr_adapter::ImageRecord& rec : capture.images()) {
        if (!(rec.usage & rdr::kUsageLasting) || !(rec.usage & rdr::kUsageTarget)) continue;
        rdr::ImageRef r4;
        r4.image = binder.Mapped(rdr::ImageId(rec.id));
        r4.mip = 0, r4.layer = 0;
        std::vector<uint8_t> p4;
        uint32_t w4 = 0, h4 = 0;
        uint64_t hh = 1469598103934665603ull;
        if (r4.image != rdr::kNoImage && renderer.ReadImage(r4, p4, w4, h4)) for (uint8_t b4 : p4) hh = (hh ^ b4) * 1099511628211ull;
        Log("[rplay][lthash] f=%u img=%u hash=%016llx", f, unsigned(rec.id), static_cast<unsigned long long>(hh));
      }
    }
    if (skipped_frame && f < g_skip_hash_frames) {   // lane 307: the lasting state, read back
      renderer.WaitIdle();
      for (uint32_t hid : g_hash_imgs) {   // S102 K2 (review F2b, diagnosis): whole-image hashes after each skipped frame (--hash-img A,B,...)
        rdr::ImageRef r3;
        r3.image = binder.Mapped(rdr::ImageId(hid));
        r3.mip = 0, r3.layer = 0;
        std::vector<uint8_t> p3;
        uint32_t w3 = 0, h3 = 0;
        uint64_t hh = 1469598103934665603ull;
        if (r3.image != rdr::kNoImage && renderer.ReadImage(r3, p3, w3, h3)) for (uint8_t b3 : p3) hh = (hh ^ b3) * 1099511628211ull;
        Log("[rplay][imghash] f=%u img=%u hash=%016llx", f, hid, static_cast<unsigned long long>(hh));
      }
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
        uint64_t src_hash = 0, dsrc_hash = 0;   // S102 K2 (review F2b): the whole SOURCE image of the copy, and its depth attachment image when a pass of the frame drew to it with one, read after the frame
        {
          const auto whole = [&](uint32_t id) -> uint64_t {
            rdr::ImageRef r2;
            r2.image = binder.Mapped(rdr::ImageId(id));
            r2.mip = 0, r2.layer = 0;
            std::vector<uint8_t> p2;
            uint32_t w2 = 0, h2 = 0;
            uint64_t h = 1469598103934665603ull;
            if (r2.image == rdr::kNoImage || !renderer.ReadImage(r2, p2, w2, h2)) return 0;
            for (uint8_t b : p2) h = (h ^ b) * 1099511628211ull;
            return h;
          };
          src_hash = whole(uint32_t(c.source.image));
          for (uint32_t k = 0; k < all_ops.count && !dsrc_hash; ++k)   // the depth image of the pass that draws to the copy's source
            if (all_ops[k].kind == rdr_adapter::OpKind::SetTargets && frame.targets[all_ops[k].index].color[0].image == c.source.image && frame.targets[all_ops[k].index].depth.image != rdr::kNoImage)
              dsrc_hash = whole(uint32_t(frame.targets[all_ops[k].index].depth.image));
        }
        Log("[rplay][skiphash] f=%u n=%u dest=%u mip=%u rect=%d,%d,%u,%u hash=%016llx forced=%d src=%016llx dsrc=%016llx", f, ordinal++, unsigned(c.dest.image), unsigned(c.dest.mip), c.dest_rect.x, c.dest_rect.y, unsigned(c.dest_rect.width), unsigned(c.dest_rect.height), static_cast<unsigned long long>(hash), i < skip_forced.size() && skip_forced[i] ? 1 : 0, static_cast<unsigned long long>(src_hash), static_cast<unsigned long long>(dsrc_hash));
      }
    }
    if (counts_file) WriteFrameCounts(counts_file, f, renderer.LastFrameCounts());  // S102 M1
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
  {  // S102 R21-B: the loop's wall time (bind + record, overlapped or not), the throughput the gate compares; the stage is drained first
    std::string stage_line;
    if (stage) {
      stage->EndOfInput();
      stage->Stop();
      stage_line = stage->Line();
    }
    renderer.WaitIdle();
    const double secs = (NowUs() - loop_t0) / 1e6;
    Log("[rplay][stage] split %s: %u frames in %.3f s = %.2f frames a second (bind spin %u us, record spin %u us); validation errors %llu", stage ? "on" : "off", count, secs,
        secs > 0 ? double(count) / secs : 0.0, g_bind_spin_us, g_record_spin_us, static_cast<unsigned long long>(renderer.validation_errors()));
    const double n = count ? double(count) * 1000.0 : 1.0;
    Log("[rplay][stage] this thread, ms a frame: load %.3f, contents %.3f, bind %.3f (spin included), %s %.3f", st_load / n, st_contents / n, st_bind / n, stage ? "hand-off" : "record + submit", st_hand / n);
    const double nf = count ? double(count) * 1e6 : 1.0;   // ns -> ms a frame
    Log("[rplay][stage] CPU ms a frame: render thread (this one) %.3f, record thread %.3f", double(bind_cpu.TakeNs()) / nf, double(rdr::g_record_thread_ns_total.load() - record_cpu0) / nf);
    if (!stage_line.empty()) Log("%s", stage_line.c_str());
    if (hashes) std::fclose(hashes);
  }
  if (g_farm.ko_draw_none || g_farm.ko_render_drop) Log("[rplay][ko] draw_none=%d render_drop=%d plant=%d: %u of %u frames dropped before any bind", g_farm.ko_draw_none ? 1 : 0, g_farm.ko_render_drop ? 1 : 0, g_farm.ko_plant ? 1 : 0, ko_dropped, count);
  if (timing) std::fclose(timing);
  if (std::FILE* ft = rdr::g_frame_trace.exchange(nullptr)) std::fclose(ft);  // S102 G11
  if (rdr_adapter::frame_cuts::g_fview_blit || rdr_adapter::frame_cuts::g_fview_restore) Log("%s", binder.FrameCutsReport().c_str());  // S102 G11
  if (rdr_adapter::frame_roundtrip::g_fview_level > 0) Log("%s", binder.FrameRoundTripReport().c_str());  // S102 R18-B1
  {   // S102 T1: bytes zeroed + copied building constant banks, over the run (warm-up frames included)
    const double nf = rdr_adapter::g_bank_frames.load() ? double(rdr_adapter::g_bank_frames.load()) : 1.0;
    Log("[rplay][time] banks: lazy %s, blocks %s; %.0f KB zeroed + copied a frame (%.1f banks built a frame, %llu frames)", rdr_adapter::g_lazy_banks.load() ? "on" : "off", rdr_adapter::g_bank_blocks.load() ? "on" : "off",
        double(rdr_adapter::g_bank_bytes.load()) / nf / 1024.0, double(rdr_adapter::g_bank_resolved.load()) / nf, static_cast<unsigned long long>(rdr_adapter::g_bank_frames.load()));
  }
  if (counts_file) {  // S102 M1
    std::fprintf(counts_file, "\n]}\n");
    std::fclose(counts_file);
    Log("[rplay] counts: %u frames written to %s", count, g_farm.counts_path.c_str());
  }
  if (g_skip_every)   // lane 307
    Log("[rplay][skip] %s: %u skipped frames (every %u), lasting binds kept %u of %u ops, %u of %u draws, %u of %u passes (%u only by the object group), %u lasting copies (image rule found %llu per-frame); bound draws %u, bind %.1f us a skipped frame",
        g_skip_prune.c_str(), skip_total.frames, g_skip_every, skip_total.ops_kept, skip_total.ops_in, skip_total.draws_kept, skip_total.draws_in, skip_total.passes_kept, skip_total.passes_in, skip_total.grouped, skip_total.seeds, static_cast<unsigned long long>(skip_streaks.by_image_left), skip_total.bound,
        skip_total.frames ? skip_total.bind_us / skip_total.frames : 0.0);
  if (g_lt_census) {   // S102 K2 (review F4)
    std::string lt;
    for (const auto& kv : lt_writes) { char b[96]; std::snprintf(b, sizeof b, " img%u:%u/%u(last f%u)", kv.first, kv.second, count, lt_last[kv.first]); lt += b; }
    Log("[rplay][lt_census] lasting render targets written in N of %u frames:%s", count, lt.c_str());
  }
  if (g_skip_every)   // S102 K2 (R16 piece C): what a lasting bind costs and keeps, with the group rule or without (the cost includes the record + submit; the capture file read is left out)
    Log("[rplay][k2] nogroup %s: lasting binds %u of %u skipped frames, ms per lasting bind %.3f (Bind() alone %.3f), kept %u of %u draws, %u of %u passes (%u only by the model-part group); object passes %u, kept %u, left out although another was kept %u, kept for a lasting render target %u",
        g_lasting_nogroup ? "on" : "off", k2_lasting_n, k1_skip_n, k2_lasting_n ? k2_lasting_us / k2_lasting_n / 1000.0 : 0.0, k2_lasting_n ? k2_bind_us / k2_lasting_n / 1000.0 : 0.0, skip_total.draws_kept, skip_total.draws_in, skip_total.passes_kept, skip_total.passes_in, skip_total.grouped,
        k2_obj_passes, k2_obj_kept, k2_obj_dropped, k2_lasting_targets);
  if (g_skip_every) {   // S102 K1: the census line (the live line is [rdr][skip_cost]); the state hash lets two arms be compared (the flat guard walk must equal the map walk)
    uint64_t sh = 1469598103934665603ull;
    for (uint32_t id : skip_streaks.state_images) sh += uint64_t(id) * 0x9E3779B97F4A7C15ull;
    uint32_t top_img = 0, top_n = 0, stale_top = 0, stale_top_n = 0;
    for (const auto& kv : skip_streaks.stale_by_image) if (kv.second > stale_top_n) stale_top = kv.first, stale_top_n = kv.second;
    for (const auto& kv : skip_streaks.by_image_dest) if (kv.second > top_n || (kv.second == top_n && kv.first < top_img)) top_img = kv.first, top_n = kv.second;
    Log("[rplay][skip_cost] frames %u, skipped %u, lasting binds %u, skipped-frame ms mean %.3f, drawn-frame ms mean %.3f (the capture file read left out); per 600 frames: lasting binds %.1f; guard decay %s (window %u), lean %s: guard held %llu, would have been held by the old guard %llu, per-frame by image %llu; state guard images %zu, state hash %016llx; streaks.Note %.1f us a frame handled; split a skipped frame: contents %.0f us, bind %.0f us, record + submit %.0f us; a drawn frame: %.0f / %.0f / %.0f us; stale reads %llu (img%u %u, spliced img %u: %u), would decay (switch off) %llu, rule %s, top image-rule dest img %u (%u copies left out), gaps 1:%llu 2:%llu 3-4:%llu 5-8:%llu 9-16:%llu 17-32:%llu 33-64:%llu 65+:%llu",
        k1_skip_n + k1_drawn_n, k1_skip_n, k1_lasting_binds, k1_skip_n ? k1_skip_us / k1_skip_n / 1000.0 : 0.0, k1_drawn_n ? k1_drawn_us / k1_drawn_n / 1000.0 : 0.0,
        (k1_skip_n + k1_drawn_n) ? double(k1_lasting_binds) * 600.0 / double(k1_skip_n + k1_drawn_n) : 0.0, g_guard_decay ? "on" : "off", g_guard_window, g_skip_lean ? "on" : "off",
        static_cast<unsigned long long>(skip_streaks.by_image_guarded), static_cast<unsigned long long>(skip_streaks.guard_decayed), static_cast<unsigned long long>(skip_streaks.by_image_left), skip_streaks.state_images.size(), static_cast<unsigned long long>(sh), (k1_skip_n + k1_drawn_n) ? k1_note_us / double(k1_skip_n + k1_drawn_n) : 0.0,
        k1_skip_n ? k1_part_us[0][0] / k1_skip_n : 0.0, k1_skip_n ? k1_part_us[0][1] / k1_skip_n : 0.0, k1_skip_n ? k1_part_us[0][2] / k1_skip_n : 0.0,
        k1_drawn_n ? k1_part_us[1][0] / k1_drawn_n : 0.0, k1_drawn_n ? k1_part_us[1][1] / k1_drawn_n : 0.0, k1_drawn_n ? k1_part_us[1][2] / k1_drawn_n : 0.0,
        static_cast<unsigned long long>(skip_streaks.stale_read), stale_top, stale_top_n, g_splice_img, g_splice_img && skip_streaks.stale_by_image.count(g_splice_img) ? skip_streaks.stale_by_image[g_splice_img] : 0u, static_cast<unsigned long long>(skip_streaks.would_decay), g_guard_rule == 1 ? "old" : g_guard_rule == 2 ? "blind" : "positive", top_img, top_n,
        static_cast<unsigned long long>(skip_streaks.gap_hist[0]), static_cast<unsigned long long>(skip_streaks.gap_hist[1]), static_cast<unsigned long long>(skip_streaks.gap_hist[2]), static_cast<unsigned long long>(skip_streaks.gap_hist[3]),
        static_cast<unsigned long long>(skip_streaks.gap_hist[4]), static_cast<unsigned long long>(skip_streaks.gap_hist[5]), static_cast<unsigned long long>(skip_streaks.gap_hist[6]), static_cast<unsigned long long>(skip_streaks.gap_hist[7]));
  }
  if (g_skip_every)   // S102 K2 (review F1 / a): the stale counter split (once per owed episode)
    Log("[rplay][stale] run-end keep %s (copies kept %llu), stale frames (per read) %llu, owed and read at the end %zu; total %llu: by path exact-rect %llu / image rule %llu / decayed guard %llu (must be 0 with the decay on); gap <=16 %llu / >16 or none %llu; designed lag %llu, copy run ended on a skipped frame %llu",
        g_run_end_keep ? (g_run_end_peek_ignored ? "on (peek ignored)" : "on") : "off", static_cast<unsigned long long>(skip_streaks.run_end_kept), static_cast<unsigned long long>(skip_streaks.stale_frames), skip_streaks.StaleOpen(),
        static_cast<unsigned long long>(skip_streaks.stale_read), static_cast<unsigned long long>(skip_streaks.stale_by_path[1]), static_cast<unsigned long long>(skip_streaks.stale_by_path[2]), static_cast<unsigned long long>(skip_streaks.stale_by_path[3]),
        static_cast<unsigned long long>(skip_streaks.stale_gap_le16), static_cast<unsigned long long>(skip_streaks.stale_gap_gt16), static_cast<unsigned long long>(skip_streaks.stale_designed), static_cast<unsigned long long>(skip_streaks.stale_run_end));
  if (rdr_adapter::map_turns::AnyOn()) Log("%s", binder.TurnsReport().c_str());   // S102 G3
  if (g_jitter_replay.on) {  // lane 185
    g_jitter_replay.Report();
    std::printf("[jitter] velocity_draws_ total %llu over %u frames (%s window)\n", (unsigned long long)g_velocity_sum, count, g_jitter_replay.legacy ? "legacy" : "marker");
  }
  if (g_wide_replay.state.on())
    Log("[rplay] wide: %u draws, %u squeezed (menu %u, hud %u, scene draws after the first depth copy %u), full-view quads left unsqueezed %u; the post-pass hash rule left alone: %u full-view quads, %u other rectangles", g_wide_replay.totals.draws, g_wide_replay.totals.squeezed, g_wide_replay.totals.menu, g_wide_replay.totals.hud, g_wide_replay.totals.scene_after_copy, g_wide_replay.totals.full_view, g_wide_replay.totals.post_after_full, g_wide_replay.totals.post_after_partial);  // lane 171
  renderer.WaitIdle();
  if (rdr::crumbs::g_selftest_frame.load() >= 0)  // S102 D1: the accounting after the (simulated) loss: no present may count once the device is lost
    Log("[rplay][crumbs] presents: %llu at the loss, %llu at the end; device lost at serial %llu", static_cast<unsigned long long>(rdr::crumbs::g_presented_at_loss.load()),
        static_cast<unsigned long long>(rdr::g_presented.load()), static_cast<unsigned long long>(rdr::g_device_lost.load()));
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
    {  // S102 S0-3: the per-role means over the whole run (one line the gate reads)
      const rdr::Renderer::GpuRoleTotals roles = renderer.GpuRoles();
      double sum = 0;
      for (int c = 0; c < rdr::kGpuClassCount; ++c) sum += roles.cls_ms[c];
      Log("[rplay][gpu] roles: frames %llu, mean ms a frame: colour %.4f shadow %.4f rim %.4f plane %.4f cards %.4f post %.4f output %.4f ui %.4f frame %.4f copies %.4f prelight %.4f feedback %.4f other %.4f; work %.4f gap %.4f total %.4f; roles sum %.4f; no pass %llu, no present %llu, overflow %llu; stamps a frame %.1f; passes a frame: colour %.2f shadow %.2f rim %.2f plane %.2f cards %.2f post %.2f output %.2f ui %.2f frame %.2f copies %.2f prelight %.2f feedback %.2f other %.2f; acquire %.4f ms, Present call %.4f ms a render; rdr_submit busy %.4f ms a render over %llu",
          static_cast<unsigned long long>(roles.frames), roles.cls_ms[rdr::kGpuColor], roles.cls_ms[rdr::kGpuShadow], roles.cls_ms[rdr::kGpuRim], roles.cls_ms[rdr::kGpuPlane], roles.cls_ms[rdr::kGpuCards],
          roles.cls_ms[rdr::kGpuPost], roles.cls_ms[rdr::kGpuOutput], roles.cls_ms[rdr::kGpuUi], roles.cls_ms[rdr::kGpuFrame], roles.cls_ms[rdr::kGpuCopies], roles.cls_ms[rdr::kGpuPrelight], roles.cls_ms[rdr::kGpuFeedback], roles.cls_ms[rdr::kGpuOther], roles.work_ms, roles.gap_ms, roles.total_ms, sum, static_cast<unsigned long long>(roles.no_pass),
          static_cast<unsigned long long>(roles.no_present), static_cast<unsigned long long>(roles.overflow), roles.stamps_a_frame,
          roles.cls_passes_a_frame[rdr::kGpuColor], roles.cls_passes_a_frame[rdr::kGpuShadow], roles.cls_passes_a_frame[rdr::kGpuRim], roles.cls_passes_a_frame[rdr::kGpuPlane],
          roles.cls_passes_a_frame[rdr::kGpuCards], roles.cls_passes_a_frame[rdr::kGpuPost], roles.cls_passes_a_frame[rdr::kGpuOutput], roles.cls_passes_a_frame[rdr::kGpuUi], roles.cls_passes_a_frame[rdr::kGpuFrame], roles.cls_passes_a_frame[rdr::kGpuCopies],
          roles.cls_passes_a_frame[rdr::kGpuPrelight], roles.cls_passes_a_frame[rdr::kGpuFeedback], roles.cls_passes_a_frame[rdr::kGpuOther],
          roles.acquire_ms, roles.present_call_ms,
          rdr::g_submit_busy_calls.load() ? double(rdr::g_submit_busy_ns_total.load()) / 1e6 / double(rdr::g_submit_busy_calls.load()) : 0.0, static_cast<unsigned long long>(rdr::g_submit_busy_calls.load()));
    }
    {  // S102 G5: the points outside our command buffers over the whole run (one line the gate reads); the GPU side is there only with --gpu-outside 1
      const rdr::OutsideSums& o = renderer.OutsideRun();
      const std::string line = rdr::OutsideLine("[rplay][outside]", o);
      if (!line.empty()) Log("%s", line.c_str());
    }
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
  if (g_d3_chains) Log("%s", rdr_adapter::d3t::Line().c_str());   // S102 K3 rework: the D3 bookkeeping per frame
  if (rdr_adapter::own_group::g_on[rdr_adapter::own_group::kModule] || rdr_adapter::own_group::g_on[rdr_adapter::own_group::kRim]) Log("%s", binder.OwnGroupCensus().c_str());   // S102 P4P5
  if (!gallery_dir.empty()) WriteGallery(renderer, binder, gallery_dir);
  Log("[rplay] capture: %u frames, %u draws, %u bound, %u missing a shader, %u no geometry, %u bad layout, %u high slot", count, stats.draws, stats.bound, stats.no_shader, stats.no_geometry, stats.bad_layout, stats.high_slot);
  std::printf("[fb] fast frames %llu, pack-once frames %llu\n", static_cast<unsigned long long>(rdr::g_fb_fast_frames.load()), static_cast<unsigned long long>(rdr::g_pack_once_frames.load()));  // S102 T4 / T5: the flag reached the builder
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
  if (!g_arms.empty()) ArmsReport();  // S102 G10
  Log("[rplay][arms] kept samples %llu (arms %zu)", (unsigned long long)rdr::g_arm_kept.load(), g_arms.size());  // review F1: 0 without --arms
  if (rdr::colour_cuts::g_half_transparents.load())  // S102 C4: what the half-size tail took (counts; the check reads these)
    Log("[rplay][half] colour passes seen %llu, taken %llu, declined (short tail) %llu, (target) %llu, draws halved %llu; draws by reason: taken %llu, colour targets %llu, not additive %llu, alpha changes %llu, depth state %llu, stencil %llu, shader %llu plant %d",
        (unsigned long long)rdr::half_trans::g_frames.load(), (unsigned long long)rdr::half_trans::g_passes_taken.load(), (unsigned long long)rdr::half_trans::g_declined_short.load(), (unsigned long long)rdr::half_trans::g_declined_target.load(),
        (unsigned long long)rdr::half_trans::g_draws_halved.load(), (unsigned long long)rdr::half_trans::g_reasons[0].load(), (unsigned long long)rdr::half_trans::g_reasons[1].load(), (unsigned long long)rdr::half_trans::g_reasons[2].load(),
        (unsigned long long)rdr::half_trans::g_reasons[3].load(), (unsigned long long)rdr::half_trans::g_reasons[4].load(), (unsigned long long)rdr::half_trans::g_reasons[5].load(), (unsigned long long)rdr::half_trans::g_reasons[6].load(), rdr::half_trans::g_plant.load());
  if (rdr::colour_cuts::g_colour_cuts.load() || rdr::colour_cuts::g_arena_overdraw.load() || rdr::arena_fragments::g_mask.load() || !g_arms.empty())  // S102 G10: what the colour cuts took (counts; the check reads these)
    Log("[rplay][colour_cuts] mask %u plant %d: colour passes with a prepass %llu, draws pre-drawn %llu, declined (depth clear) %llu, (overlap) %llu, (nothing eligible) %llu", rdr::colour_cuts::g_colour_cuts.load(), rdr::colour_cuts::g_colour_cuts_plant.load(),
        (unsigned long long)rdr::colour_cuts::g_frames_taken.load(), (unsigned long long)rdr::colour_cuts::g_draws_prepassed.load(), (unsigned long long)rdr::colour_cuts::g_declined_clear.load(),
        (unsigned long long)rdr::colour_cuts::g_declined_overlap.load(), (unsigned long long)rdr::colour_cuts::g_declined_none.load());
  if (rdr::colour_cuts::g_colour_cuts.load() || rdr::colour_cuts::g_arena_overdraw.load() || rdr::arena_fragments::g_mask.load() || !g_arms.empty()) {  // review F2: the rule's census, per program pair: taken / not alpha-simple / product / per-frame / no colour / vertex alpha
    std::lock_guard<std::mutex> lock(rdr::colour_cuts::g_alpha_rows_mutex);
    uint64_t total[rdr::colour_cuts::kAlphaReasonCount] = {};
    for (const auto& row : rdr::colour_cuts::g_alpha_rows) {  // S102 C4 (review F5): names are built here, from the registry, never per draw
      uint64_t n[rdr::colour_cuts::kAlphaReasonCount];
      for (int r = 0; r < rdr::colour_cuts::kAlphaReasonCount; ++r) total[r] += (n[r] = row.n[r].load());
      Log("[rplay][alpha] %llu taken, %llu not alpha-simple, %llu product<1, %llu per-frame, %llu no colour, %llu vertex alpha<255: %s + %s (%llu refused for the texture; %llu with a zero constants product)", (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)n[2],
          (unsigned long long)n[3], (unsigned long long)n[4], (unsigned long long)n[5], rdr::fx::kVariants[row.vs].name, row.ps >= 0 ? rdr::fx::kVariants[row.ps].name : "(no fragment)", (unsigned long long)n[6], (unsigned long long)row.zero.load());
    }
    Log("[rplay][alpha] total: %llu taken, %llu not alpha-simple, %llu product<1, %llu per-frame, %llu no colour, %llu vertex alpha<255", (unsigned long long)total[0], (unsigned long long)total[1],
        (unsigned long long)total[2], (unsigned long long)total[3], (unsigned long long)total[4], (unsigned long long)total[5]);
    Log("[rplay][overdraw] arena overdraw %d: texture-alpha proof refused %llu draws (reason 6 in the census: %llu), admitted %llu", int(rdr::colour_cuts::g_arena_overdraw.load()), (unsigned long long)rdr::colour_cuts::g_overdraw_texture_refused.load(),
        (unsigned long long)total[6], (unsigned long long)rdr::colour_cuts::g_overdraw_texture_taken.load());
    Log("[rplay][overdraw] texture scans: %llu images, %.2f MB read, %.2f ms (%.2f ms per MB); proven slots that clamp to a border: %llu (refused for a transparent border: %llu)", (unsigned long long)rdr::colour_cuts::g_scan_images.load(),
        double(rdr::colour_cuts::g_scan_bytes.load()) / 1048576.0, double(rdr::colour_cuts::g_scan_ns.load()) / 1e6, rdr::colour_cuts::g_scan_bytes.load() ? double(rdr::colour_cuts::g_scan_ns.load()) / 1e6 / (double(rdr::colour_cuts::g_scan_bytes.load()) / 1048576.0) : 0.0,
        (unsigned long long)rdr::colour_cuts::g_border_proven.load(), (unsigned long long)rdr::colour_cuts::g_border_refused.load());
  }
  if (rdr::arena_fragments::g_mask.load())  // S102 R18-B3: what the arena fragments switch took (counts; the check reads these)
    Log("[rplay][arena_fragments] mask %u plant %d: colour passes taken %llu, draws blend-off %llu, draws widened %llu, declined states (not a plain alpha blend) %llu, (other target blended) %llu, (float target) %llu, exact proofs %llu, inexact %llu",
        rdr::arena_fragments::g_mask.load(), rdr::arena_fragments::g_plant.load(), (unsigned long long)rdr::arena_fragments::g_passes_taken.load(), (unsigned long long)rdr::arena_fragments::g_blend_off.load(),
        (unsigned long long)rdr::arena_fragments::g_widened.load(), (unsigned long long)rdr::arena_fragments::g_declined[0].load(), (unsigned long long)rdr::arena_fragments::g_declined[1].load(), (unsigned long long)rdr::arena_fragments::g_declined[2].load(),
        (unsigned long long)rdr::arena_fragments::g_proof_exact.load(), (unsigned long long)rdr::arena_fragments::g_proof_inexact.load());
  if (rdr::colour_cuts::g_colour_cuts.load() || rdr::colour_cuts::g_arena_overdraw.load() || rdr::arena_fragments::g_mask.load() || !g_arms.empty())
    Log("[rplay][colour_cuts] taken draws by shader of the normal draw: plain %llu, alpha-test %llu", (unsigned long long)rdr::colour_cuts::g_taken_plain.load(), (unsigned long long)rdr::colour_cuts::g_taken_alphakill.load());
  if (rdr::colour_cuts::g_colour_cuts.load() || rdr::colour_cuts::g_arena_overdraw.load() || rdr::arena_fragments::g_mask.load() || !g_arms.empty())
    Log("[rplay][colour_cuts] passes declined: mixed depth direction %llu, depth-dependent stencil writer %llu", (unsigned long long)rdr::colour_cuts::g_declined_direction.load(), (unsigned long long)rdr::colour_cuts::g_declined_stencil.load());
  if (rdr::colour_cuts::g_colour_cuts.load() || rdr::colour_cuts::g_arena_overdraw.load() || rdr::arena_fragments::g_mask.load() || !g_arms.empty())
    Log("[rplay][colour_cuts] named draws %llu: no depth test/write %llu, stencil %llu, blended %llu, no colour written %llu, other compare %llu, discarding shader %llu, eligible %llu (of which on a plain alpha blend %llu)",
        (unsigned long long)rdr::colour_cuts::g_reasons[0].load(), (unsigned long long)rdr::colour_cuts::g_reasons[1].load(), (unsigned long long)rdr::colour_cuts::g_reasons[2].load(), (unsigned long long)rdr::colour_cuts::g_reasons[3].load(),
        (unsigned long long)rdr::colour_cuts::g_reasons[4].load(), (unsigned long long)rdr::colour_cuts::g_reasons[5].load(), (unsigned long long)rdr::colour_cuts::g_reasons[6].load(), (unsigned long long)rdr::colour_cuts::g_reasons[7].load(), (unsigned long long)rdr::colour_cuts::g_alpha_blend_taken.load());
  if (rdr::g_draw_census.load()) {  // S102 G10: what the scene colour passes drew, per program name, a frame (counts: draws, triangles, pixels written (area bound))
    std::lock_guard<std::mutex> lock(rdr::g_draw_census_mutex);
    const double n = count ? double(count) : 1.0;
    for (const auto& row : rdr::g_draw_census_rows)
      Log("[rplay][draws] %.2f draws %.0f tris %.0f px %.0f vs %.0f fs a frame: %s", double(row.second.draws) / n, double(row.second.indices) / 3.0 / n, double(row.second.area_px) / n, double(row.second.vs_invocations) / n, double(row.second.fs_invocations) / n, row.first.c_str());
  }
  if (g_tween_probe) Log("[rplay][tween_probe] blended frames %llu, draws matched %llu", static_cast<unsigned long long>(tp_frames), static_cast<unsigned long long>(tp_matched));
  if (!g_frame_shape_name.empty()) frame_shape.Report(g_frame_shape_name.c_str());   // S102 R21-A step 5
  if (rdr_adapter::plan_memo::g_fallback.load() || rdr_adapter::plan_memo::g_mask.load()) Log("%s", rdr_adapter::plan_memo::Line().c_str());   // S102 R23-P
  if (g_plan_memo_census_name.empty() && rdr_adapter::plan_memo::g_time_on.load()) rdr_adapter::plan_memo::ReportTimes("rplay", uint64_t(rdr_adapter::plan_memo::g_tm_calls[rdr_adapter::plan_memo::kTmAlias]));
  if (!g_plan_memo_census_name.empty()) rdr_adapter::plan_memo::g_census.Report(g_plan_memo_census_name.c_str());   // S102 R23-P
  { char lite_line[200]; rdr::CombineLiteCensusLine(0, lite_line, sizeof lite_line); Log("%s", lite_line); }   // S102 R18-B2
  { char small_line[420]; rdr::GpuSmallCensusLine(0, small_line, sizeof small_line); Log("%s", small_line); }   // S102 R24-E
  Log("[rdr][frame_facts] %s: records %llu, record objects %llu (arena-prefixed %llu), blend skipped %llu draws (kept %llu), flat key tables %llu, verified records %llu (bad %llu)", rdr::g_frame_facts.load() ? "on" : "off",
      static_cast<unsigned long long>(rdr::g_ff_records.load()), static_cast<unsigned long long>(rdr::g_ff_objects.load()), static_cast<unsigned long long>(rdr::g_ff_arena.load()),
      static_cast<unsigned long long>(rdr::g_ff_blend_skipped.load()), static_cast<unsigned long long>(rdr::g_ff_blend_kept.load()), static_cast<unsigned long long>(rdr::g_ff_keys_flat.load()), static_cast<unsigned long long>(rdr::g_ff_verified.load()), static_cast<unsigned long long>(rdr::g_ff_verify_bad.load()));   // S102 R21-A
  Log("[rplay][lean] mask %d: sets kept %llu (earlier frames %llu) built %llu fallback %llu collide %llu evicted %llu; staging pool hit %llu miss %llu; churn %llu, audits %llu, kept sets naming a dead view %llu", rdr::g_render_lean.load(), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetHit)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetHitOld)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetMiss)),
      static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetFull)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetCollide)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcSetEvict)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcStageHit)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcStageMiss)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcChurn)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcAudit)), static_cast<unsigned long long>(rdr::LeanGet(rdr::kLcAuditBad)));   // S102 RT1
  Log("[rdr][copy_batch] %s: batches %llu, copies in them %llu, most regions in one %llu", rdr::g_copy_batch.load() ? "on" : "off", static_cast<unsigned long long>(rdr::g_copy_batches.load()),
      static_cast<unsigned long long>(rdr::g_copy_batched.load()), static_cast<unsigned long long>(rdr::g_copy_batch_max.load()));   // S102 G12
  if (rdr_adapter::prelight_layered::g_fview) Log("%s", binder.PreLightReport().c_str());   // S102 C1
  if (rdr_adapter::prelight_analytic::g_fview) Log("%s", binder.PreLightAnalyticReport().c_str());   // S102 L1
  if (rdr_adapter::prelight_compute::g_fview) Log("%s", binder.PreLightComputeReport().c_str());   // S102 L1b
  if (rdr_adapter::copy_alias::g_mode > 0 || rdr_adapter::copy_alias::g_fullsize > 0 || rdr_adapter::copy_alias::g_small > 0 || rdr_adapter::copy_alias::g_shadow_direct || rdr_adapter::copy_alias::g_frame_direct || rdr_adapter::copy_alias::g_direct > 0) Log("%s", binder.CopyAliasReport().c_str());   // S102 G8: the census line (copies elided a frame)
  if (!out_path.empty() && binder.last_present().source.image != kNoImage) {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (renderer.ReadImage(binder.last_present().source, pixels, width, height) && WritePng(out_path.c_str(), pixels.data(), width, height)) Log("[rplay] wrote %s (%ux%u)", out_path.c_str(), width, height);
    else Log("[rplay] could not read the presented image");
  }
  PresentReport(renderer, g_present_w, g_present_h);  // S102 G2
  return renderer.validation_errors() ? 4 : 0;
}

}  // namespace

bool g_lc_selftest = false;
bool g_stencil_selftest = false;   // S102 R24-C
static uint64_t ProblemTotal(const char* name) {
  ProblemCount counts[kMaxProblemNames];
  const int n = ProblemCounts(counts, kMaxProblemNames);
  for (int i = 0; i < n && i < kMaxProblemNames; ++i)
    if (std::strcmp(counts[i].name, name) == 0) return counts[i].count;
  return 0;
}

// S102 L1b review F1 / F5: a synthetic frame (no capture): ONE dispatch into a cube that has the lasting usage and that nothing reads. The rule of a copy into a lasting destination: the step is persistent, so a
// redraw (DisplayOnly) records NO dispatch, a PersistentOnly frame records it, a Full frame records it. A dispatch naming an image that is not there (kNoImage) is dropped whole with a Problem (never indexed).
// Prints PASS / FAIL lines; exit 0 only when every line passes. `--light-cube-plant 4` is the first version's rule: the DisplayOnly line must FAIL.
static int LightCubeSelfTest(Renderer& renderer) {
  int fails = 0;
  const auto say = [&](bool ok, const char* text, unsigned a, unsigned b) { std::printf("%s  %s (%u, expected %u)\n", ok ? "PASS" : "FAIL", text, a, b), fails += ok ? 0 : 1; };
  if (!renderer.compute_supported()) {
    std::printf("FAIL  the renderer cannot dispatch (compute_supported false)\n");
    return 1;
  }
  ImageDesc desc;
  desc.width = desc.height = 64, desc.format = Format::RGBA8, desc.kind = ImageKind::Cube, desc.usage = uint8_t(kUsageSampled | kUsageStorage | kUsageLasting);
  const ImageId cube = renderer.CreateImage(desc);
  FrameBuilder builder;
  const auto frame_of = [&](uint32_t count, ImageId second) {
    builder.Begin(1);
    float zeros[kMaxLightCubes * kLightCubeVec4 * 4] = {};
    DispatchOp d{};
    d.cube[0] = cube, d.cube[1] = second, d.cube_count = count;
    d.constants = builder.PushConstants(zeros, sizeof(zeros));
    builder.Dispatch(d);
    return builder.Finish();
  };
  const auto dispatches = [&](const Frame& f, RenderMode mode) {
    renderer.RenderFrame(f, mode);
    renderer.WaitIdle();
    return renderer.LastFrameCounts().total.dispatches;
  };
  const Frame one = frame_of(1, kNoImage);
  dispatches(one, RenderMode::Full);   // the first frame also makes the direction table
  say(dispatches(one, RenderMode::Full) == 1, "Full: one dispatch", dispatches(one, RenderMode::Full), 1);
  const uint32_t display = dispatches(one, RenderMode::DisplayOnly);
  say(display == 0, "DisplayOnly: no dispatch (the cube is lasting: its writer is persistent, a redraw does not repeat it)", display, 0);
  const uint32_t persistent = dispatches(one, RenderMode::PersistentOnly);
  say(persistent == 1, "PersistentOnly: the dispatch runs (a frame that is not shown still writes the cube)", persistent, 1);
  g_problem_nonfatal.store(true);
  const uint64_t before = ProblemTotal("light_cube_bad");
  const Frame bad = frame_of(2, kNoImage);
  const uint32_t refused = dispatches(bad, RenderMode::Full);
  g_problem_nonfatal.store(false);
  say(refused == 0, "a dispatch naming kNoImage is dropped whole (no dispatch, no out-of-range index)", refused, 0);
  say(ProblemTotal("light_cube_bad") == before + 1, "the refusal is a counted Problem (light_cube_bad)", unsigned(ProblemTotal("light_cube_bad") - before), 1);
  std::printf("[rplay][lc_selftest] %s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}

#include "stencil_selftest.inc"   // S102 R24-C review F1: StencilSelfTest (synthetic stencil scenes)

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
    else if (arg == "--staging-cap-mb" && i + 1 < argc) options.staging_cap_mb = uint32_t(std::atoi(argv[++i]));  // S102 U1 (dev builds)
    else if (arg == "--staging-wait-plant") options.staging_wait_plant = true;
    else if (arg == "--gpu-name" && i + 1 < argc) options.gpu_name = argv[++i];  // S102 U1: the launcher's card choice (part of a name)
    else if (arg == "--bc-decode" && i + 1 < argc) options.bc_decode = uint32_t(std::atoi(argv[++i]));  // S102 U1: 0 off, 1 auto, 2 force
    else if (arg == "--scale" && i + 1 < argc) options.render_scale = float(std::atof(argv[++i])), g_jitter_replay.scale = options.render_scale;
    else if (arg == "--out" && i + 1 < argc) out_path = argv[++i];
    else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (arg == "--window") windowed = true;
    else if (arg == "--present-size" && i + 1 < argc) {  // S102 G2: the present into offscreen images of WxH (no window): the output pass timed at any panel size
      unsigned pw = 0, ph = 0;
      if (std::sscanf(argv[++i], "%ux%u", &pw, &ph) != 2 || pw < 16 || ph < 16 || pw > 8192 || ph > 8192) {
        Log("[rplay] --present-size wants WxH (16..8192)");
        return 2;
      }
      options.present_offscreen_w = g_present_w = pw, options.present_offscreen_h = g_present_h = ph;
    }
    else if (arg == "--present-effect" && i + 1 < argc) {  // S102 G2: sgsr (the upscaling present through SGSR 1) or bilinear (the plain sample)
      const std::string effect = argv[++i];
      if (effect != "sgsr" && effect != "bilinear") {
        Log("[rplay] --present-effect is sgsr or bilinear");
        return 2;
      }
      options.present_sgsr = effect == "sgsr";
    }
    else if (arg == "--output-cap" && i + 1 < argc) rdr::g_out_cap_h.store(uint32_t(std::atoi(argv[++i])));  // S102 G2 (b): the present drawn at most this tall, blitted up
    else if (arg == "--output-cap-plant") rdr::g_out_cap_plant.store(true);  // S102 G2 TEST ONLY: the cap is asked for and never applied
    else if (arg == "--output-blit-plant") rdr::g_out_blit_plant.store(true);  // S102 G2 TEST ONLY: the blit writes only the capped-size corner
    else if (arg == "--output-makecap-fail-plant") rdr::g_out_makecap_fail_plant.store(true);  // S102 G2 TEST ONLY: the capped target cannot be made
    else if (arg == "--present-bilinear") rdr::g_out_bilinear.store(true);  // S102 G2 (c): the switch itself (SGSR off even with --present-effect sgsr)
    else if (arg == "--present-shot" && i + 1 < argc) g_present_shot = argv[++i];  // S102 G2
    else if (arg == "--present-fetch") rdr::g_present_fetch.store(true);  // S102 R24-A (wwe2k14_rdr_present_fetch)
    else if (arg == "--present-fetch-plant" && i + 1 < argc) rdr::g_present_fetch_plant.store(std::atoi(argv[++i]));  // S102 R24-A TEST ONLY: 2 = the fit test skipped
    else if (arg == "--display-curve-s100") rdr::g_display_curve_s100.store(true);  // S102 R24-A: the game's real display curve (five documented points)
    else if (arg == "--display-curve-file" && i + 1 < argc) rdr::g_display_curve_file = argv[++i];  // S102 R24-A: 256 x RGBA8
    else if (arg == "--overlay-plant") rdr::g_overlay_plant.store(true);  // S102 R24-A TEST ONLY: an overlay over the picture
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
    else if (arg == "--robust-buffers") options.robust_buffers = true;  // S102 D1: the device with robustBufferAccess
    else if (arg == "--exclusive-fullscreen") options.exclusive_fullscreen = true;  // S102 G2b: with --window only (a window on the screen: a lab run, never an agent run)
    else if (arg == "--pipeline-store" && i + 1 < argc) options.pipeline_store_dir = argv[++i];  // lane 276
    else if (arg == "--prebuild") g_farm.prebuild = true;
    else if (arg == "--scene-live") g_farm.scene_live = true;  // lane 276: SetPipelinePrebuildAllowed(false)
    else if (arg == "--save-ms" && i + 1 < argc) options.pipeline_save_interval_ms = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--store-plant" && i + 1 < argc) options.pipeline_store_plant = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--slice-us" && i + 1 < argc) options.pipeline_prebuild_slice_us = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--capture" && i + 1 < argc) capture_dir = argv[++i];
    else if (arg == "--list-bound") list_bound = true;
    else if (arg == "--crowd-ours") promotion.crowd = true;  // S101 lane 47
    else if (arg == "--crowd-fast" && i + 1 < argc) { const std::string v = argv[++i]; rdr_adapter::g_crowd_fast = v == "on" || v == "1"; }  // S102 R21-C (binder.h)
    else if (arg == "--crowd-fast-steps" && i + 1 < argc) rdr_adapter::g_crowd_fast_steps = std::atoi(argv[++i]);  // S102 R21-C
    else if (arg == "--crowd-fast-fault" && i + 1 < argc) rdr_adapter::g_crowd_fast_fault = argv[++i];  // S102 R21-C TEST ONLY
    else if (arg == "--crowd-workers" && i + 1 < argc) rdr_adapter::g_crowd_workers = std::atoi(argv[++i]);  // S102 R21-C step 3 (members_switch.h)
    else if (arg == "--crowd-workers-fault" && i + 1 < argc) rdr_adapter::g_crowd_workers_unjoined = std::string(argv[++i]) == "unjoined";  // S102 R21-C TEST ONLY
    else if (arg == "--crowd-density" && i + 1 < argc) rdr_adapter::g_crowd_density = std::atoi(argv[++i]);  // S102 R21-C (as fview)
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
    else if (arg == "--lazy-banks" && i + 1 < argc) rdr_adapter::g_lazy_banks = std::string(argv[++i]) != "off";   // S102 T1: banks resolved on use (default off)
    else if (arg == "--bank-blocks" && i + 1 < argc) rdr_adapter::g_bank_blocks = std::string(argv[++i]) != "off";   // S102 T1b: constants by block map (default off)
    else if (arg == "--bank-block-plant" && i + 1 < argc) rdr_adapter::g_bank_plant = std::atoi(argv[++i]);   // S102 T1b: 4 = a block map skips its patches, 5 = base off by one
    else if (arg == "--members-instanced" && i + 1 < argc) rdr_adapter::g_members_instanced = std::string(argv[++i]) != "off";   // S102 T6: the crowd's members as instanced draws (default off)
    else if (arg == "--bank-plant") rdr_adapter::g_bank_plant = 1;   // S102 T1: the planted twin (a resolved bank skips its patches)
    else if (arg == "--skip-hash" && i + 1 < argc) g_skip_hash_frames = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--skip-by-image" && i + 1 < argc) g_skip_by_image = std::string(argv[++i]) != "off";
    else if (arg == "--guard-decay" && i + 1 < argc) g_guard_decay = std::string(argv[++i]) != "off";   // S102 K1
    else if (arg == "--guard-decay-frames" && i + 1 < argc) g_guard_window = uint32_t(std::atoi(argv[++i]));   // S102 K1: 1 = the planted twin (decays too fast)
    else if (arg == "--skip-live-nothing" && i + 1 < argc) g_skip_live_nothing = std::string(argv[++i]) != "off";   // S102 K1
    else if (arg == "--guard-rule" && i + 1 < argc) { const std::string v = argv[++i]; g_guard_rule = v == "old" ? 1 : v == "blind" ? 2 : 0; }   // S102 K1 TEST ONLY
    else if (arg == "--run-end-keep" && i + 1 < argc) g_run_end_keep = std::string(argv[++i]) != "off";   // S102 K2 round 2
    else if (arg == "--run-end-peek-ignored") g_run_end_peek_ignored = true;
    else if (arg == "--splice-cadence" && i + 1 < argc) { unsigned a = 0, n = 0; if (std::sscanf(argv[++i], "%u,%u", &a, &n) == 2) g_splice_img = a, g_splice_cad = n, g_splice_a = g_splice_b = 0; }   // S102 K2: IMG,N
    else if (arg == "--splice-reader" && i + 1 < argc) { unsigned a = 0, b = 0, c = 0; if (std::sscanf(argv[++i], "%u,%u,%u", &a, &b, &c) == 3) g_splice_img = a, g_splice_a = b, g_splice_b = c; }   // S102 K1: IMG,A,B
    else if (arg == "--skip-lean" && i + 1 < argc) g_skip_lean = std::string(argv[++i]) != "off";   // S102 K1
    else if (arg == "--hash-img" && i + 1 < argc) { std::string v = argv[++i]; size_t pos = 0; while (pos < v.size()) { size_t e = v.find(',', pos); if (e == std::string::npos) e = v.size(); g_hash_imgs.push_back(uint32_t(std::strtoul(v.substr(pos, e - pos).c_str(), nullptr, 10))); pos = e + 1; } }   // S102 K2
    else if (arg == "--skip-stress" && i + 1 < argc) { const std::string v = argv[++i]; g_skip_stress = v == "off" ? 0 : v == "lasting" ? 2 : 1; }   // S102 K2
    else if (arg == "--describe-img" && i + 1 < argc) g_describe_img = uint32_t(std::strtoul(argv[++i], nullptr, 10));   // S102 K2
    else if (arg == "--lt-census") g_lt_census = true;   // S102 K2
    else if (arg == "--lt-hash" && i + 1 < argc) g_lt_hash_every = uint32_t(std::strtoul(argv[++i], nullptr, 10));   // S102 K2
    else if (arg == "--lasting-targets" && i + 1 < argc) g_lasting_targets = std::string(argv[++i]) != "off";   // S102 K2
    else if (arg == "--lasting-nogroup" && i + 1 < argc) g_lasting_nogroup = std::string(argv[++i]) != "off";   // S102 K2
    else if (arg == "--fb-fast") rdr::g_fb_fast = true;       // S102 T4 (wwe2k14_rdr_fb_fast)
    else if (arg == "--pack-once") rdr::g_pack_once = true;   // S102 T5 (wwe2k14_rdr_pack_once)
    else if (arg == "--fb-fast-plant") rdr::g_fb_fast_plant = true;  // S102 T4 TEST ONLY: the planted twin (with --fb-fast)
    else if (arg == "--pack-once-plant") rdr::g_pack_once_plant = true;  // S102 T5 TEST ONLY: the planted twin
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
    else if (arg == "--own-card-bake") promotion.own_card_bake = true;   // S102 P1
    else if (arg == "--own-card-bake=none") promotion.own_card_bake = promotion.bake_none = true;
    else if (arg == "--own-shadow-module" || arg == "--own-shadow-module=none") {   // S102 P4P5 (own_group.h): the shadow module's passes as one unit; =none: left out (the cut)
      rdr_adapter::own_group::g_on[rdr_adapter::own_group::kModule] = true;
      rdr_adapter::own_group::g_none[rdr_adapter::own_group::kModule] = arg.size() > 20;
    } else if (arg == "--own-rim-map" || arg == "--own-rim-map=none") {
      rdr_adapter::own_group::g_on[rdr_adapter::own_group::kRim] = true;
      rdr_adapter::own_group::g_none[rdr_adapter::own_group::kRim] = arg.size() > 14;
    }
    else if (arg == "--own-plane") rdr_adapter::g_own_plane = true, promotion.plane_shadow = true;   // S102 P3: the plane chain as one group (the plane hook makes its draws); needs scene records with the plane tables
    else if (arg == "--own-plane=none") rdr_adapter::g_own_plane = true, rdr_adapter::g_own_plane_none = true;   // S102 P3
    else if (arg == "--own-plane-fault" && i + 1 < argc) {   // S102 P3 PLANTED FAULTS
      std::string why;
      if (!rdr_adapter::ParseOwnPlaneFault(argv[++i], why)) return std::fprintf(stderr, "%s\n", why.c_str()), 2;
    }
    else if (arg == "--bake-fault" && i + 1 < argc) promotion.bake_fault = argv[++i];
    else if (arg == "--bake-record-offset" && i + 1 < argc) promotion.bake_record_offset = std::atoi(argv[++i]);
    else if (arg == "--rim-chain-ours") promotion.rim_chain = true;  // lane 151
    else if (arg == "--game" && i + 1 < argc) promotion.game_root = argv[++i];
    else if (arg == "--depth-of-field" && i + 1 < argc) rdr_adapter::g_depth_of_field = std::string(argv[++i]) != "off";  // S101 lane PO: the DEPTH OF FIELD setting, offline
    else if (arg == "--film-grain" && i + 1 < argc) rdr_adapter::g_film_grain = std::string(argv[++i]) != "off";  // S101 lane PO: the FILM GRAIN setting, offline
    else if (arg == "--shadows" && i + 1 < argc) rdr_adapter::g_shadows = std::string(argv[++i]) != "off";  // S101 lane 86: the SHADOWS setting, offline
    else if (arg == "--shadows-ours") rdr_adapter::g_shadows_ours = true;  // S101 lane 123 (SH2)
    else if (arg == "--prelight-analytic") rdr_adapter::prelight_analytic::g_fview = true;   // S102 L1: the switch's offline fallback (prelight_analytic.h)
    else if (arg == "--prelight-analytic-fault" && i + 1 < argc) {   // S102 L1: planted twins (comma list)
      std::string list = argv[++i];
      for (size_t at = 0; at <= list.size();) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        const std::string w = list.substr(at, comma - at);
        at = comma + 1;
        if (!w.empty() && !rdr_adapter::prelight_analytic::AddFault(w.c_str())) {
          Log("[rplay] unknown --prelight-analytic-fault %s", w.c_str());
          return 2;
        }
      }
    }
    else if (arg == "--prelight-compute") rdr_adapter::prelight_compute::g_fview = true;   // S102 L1b: the switch's offline fallback (prelight_compute.h)
    else if (arg == "--stage-split" && i + 1 < argc) g_stage_split = std::string(argv[++i]) == "on";   // S102 R21-B
    else if (arg == "--bind-spin-us" && i + 1 < argc) g_bind_spin_us = uint32_t(std::atoi(argv[++i]));   // S102 R21-B (tool only)
    else if (arg == "--record-spin-us" && i + 1 < argc) g_record_spin_us = uint32_t(std::atoi(argv[++i]));   // S102 R21-B (tool only)
    else if (arg == "--frame-hashes" && i + 1 < argc) g_frame_hashes = argv[++i];   // S102 R21-B
    else if (arg == "--stage-selftest" && i + 1 < argc) g_stage_selftest = std::atoi(argv[++i]);   // S102 R21-B
    else if (arg == "--stage-latch-us" && i + 1 < argc) rdr_adapter::stage_split::g_latch_us.store(uint32_t(std::atoi(argv[++i])));   // S102 R21-B (test only): the record stage waits for the next frame's hand-over
    else if (arg == "--stage-plant" && i + 1 < argc) {   // S102 R21-B: the planted twins (STAGE_SPLIT.md), each must be caught
      const std::string plant = argv[++i];
      namespace ss = rdr_adapter::stage_split;
      if (plant == "no_seal") ss::g_plant.store(ss::kPlantNoSeal);
      else if (plant == "shared_builder") ss::g_plant.store(ss::kPlantSharedBuilder);
      else if (plant == "queue_unbounded") ss::g_plant.store(ss::kPlantUnbounded);
      else if (plant == "overlay_in_hook") ss::g_plant.store(ss::kPlantOverlayInHook);   // review F1 (stage_selftest only)
      else if (plant == "no_release_defer") rdr::g_plant_destroy_now.store(true);
      else if (plant == "no_producer") rdr::g_plant_no_producer.store(true);
      else {
        Log("[rplay] unknown --stage-plant %s (no_seal, shared_builder, queue_unbounded, no_release_defer, no_producer)", plant.c_str());
        return 2;
      }
    }
    else if (arg == "--stencil-selftest") g_stencil_selftest = true;   // S102 R24-C review F1: synthetic stencil scenes, Full / DisplayOnly / PersistentOnly, judged by pixel
    else if (arg == "--light-cube-selftest") g_lc_selftest = true;   // S102 L1b review F1 / F5: a synthetic frame with a dispatch, rendered Full / DisplayOnly / PersistentOnly
    else if (arg == "--no-compute") rdr::g_force_no_compute.store(true);   // S102 L1b: the renderer says it cannot dispatch
    else if (arg == "--light-cube-plant" && i + 1 < argc) rdr::g_light_cube_plant.store(std::atoi(argv[++i]));   // S102 L1b plants: 1 the dispatch -> reader barrier left out, 2 the face-swapped shader
    else if (arg == "--prelight-compute-fault" && i + 1 < argc) {   // S102 L1b: planted twins (comma list)
      std::string list = argv[++i];
      for (size_t at = 0; at <= list.size();) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        const std::string w = list.substr(at, comma - at);
        at = comma + 1;
        if (!w.empty() && !rdr_adapter::prelight_compute::AddFault(w.c_str())) {
          Log("[rplay] unknown --prelight-compute-fault %s", w.c_str());
          return 2;
        }
      }
    }
    else if (arg == "--prelight-layered") rdr_adapter::prelight_layered::g_fview = true;   // S102 C1: the switch's offline fallback (prelight_layered.h)
    else if (arg == "--no-multiview") rdr::g_force_no_multiview.store(true);   // S102 C1: the renderer says it has no multiview
    else if (arg == "--layered-plant" && i + 1 < argc) rdr::g_layered_plant.store(std::atoi(argv[++i]));   // S102 C1 plants: 1 view mask (1 << N) - 2, 2 a one-layer transition
    else if (arg == "--layered-clear-sentinel") rdr::g_layered_clear_sentinel.store(true);   // S102 C1 review F3
    else if (arg == "--prelight-layered-fault" && i + 1 < argc) {   // S102 C1: planted twins (comma list)
      std::string list = argv[++i];
      for (size_t at = 0; at <= list.size();) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        const std::string w = list.substr(at, comma - at);
        at = comma + 1;
        if (!w.empty() && !rdr_adapter::prelight_layered::AddFault(w.c_str())) {
          Log("[rplay] unknown --prelight-layered-fault %s", w.c_str());
          return 2;
        }
      }
    }
    else if (arg == "--copy-alias" && i + 1 < argc) rdr_adapter::copy_alias::g_mode = std::atoi(argv[++i]);  // S101 lane 190 (copy_alias.h): 0 off, 1 census, 2 drop
    else if (arg == "--shadow-direct" && i + 1 < argc) rdr_adapter::copy_alias::g_shadow_direct = std::atoi(argv[++i]) != 0;   // S102 C3 (copy_alias.h)
    else if (arg == "--frame-direct" && i + 1 < argc) rdr_adapter::copy_alias::g_frame_direct = std::atoi(argv[++i]) != 0;   // S102 C3 (copy_alias.h)
    else if (arg == "--alias-small" && i + 1 < argc) rdr_adapter::copy_alias::g_small = rdr_adapter::copy_alias::ClampSmall(std::atoi(argv[++i]));  // S102 G12 (copy_alias.h): the class-mask override, 0 off, 1 bake / shadow / rim / post, 2 + scene colour / other
    else if (arg == "--copy-direct" && i + 1 < argc) rdr_adapter::copy_alias::g_direct = rdr_adapter::copy_alias::ClampDirect(std::atoi(argv[++i]));  // S102 C5 (copy_alias.h)
    else if (arg == "--combine-lite" && i + 1 < argc) rdr::g_combine_lite.store(rdr::ClampCombineLite(std::atoi(argv[++i])));   // S102 R18-B2 (combine_lite.h): 0 off, 1 exact early-outs, 2 + the flat-pixel exit
    else if (arg == "--gpu-small" && i + 1 < argc) rdr::g_gpu_small.store(rdr::ClampGpuSmall(std::atoi(argv[++i])));   // S102 R24-E (gpu_small.h): a mask, 1 = the dead quarter-size DoF chain is not recorded
    else if (arg == "--gpu-small-plant" && i + 1 < argc) rdr::g_gpu_small_plant.store(std::atoi(argv[++i]));   // S102 R24-E: tool-only planted faults
    else if (arg == "--combine-lite-plant" && i + 1 < argc) rdr::g_combine_lite_plant.store(std::atoi(argv[++i]));   // S102 R18-B2: tool-only planted faults / the mark instrument
    else if (arg == "--render-lean" && i + 1 < argc) rdr::g_render_lean.store(std::atoi(argv[++i]));   // S102 RT1: bit mask 1 texture sets kept, 4 staging pool (2 = the hooks side: the game only)
    else if (arg == "--lean-capacity" && i + 1 < argc) rdr::g_lean_capacity.store(std::atoi(argv[++i]));   // S102 RT1: the kept-set cache holds only N sets (so the sweep and the free run)
    else if (arg == "--lean-sweep-age" && i + 1 < argc) rdr::g_lean_sweep_age.store(std::atoi(argv[++i]));   // S102 RT1: a full cache frees sets idle for more than N frames (default 600)
    else if (arg == "--lean-churn" && i + 1 < argc) rdr::g_lean_churn_every.store(std::atoi(argv[++i]));   // S102 RT1: every N frames one live image's sampled view is replaced (a view change under the cache; also with the lever off)
    else if (arg == "--lean-audit" && i + 1 < argc) rdr::g_lean_audit_every.store(std::atoi(argv[++i]));   // S102 RT1: every N frames the kept sets are checked against the live views
    else if (arg == "--lean-plant" && i + 1 < argc) rdr::g_lean_plant.store(std::atoi(argv[++i]));   // S102 RT1: 1 sets not evicted, 3 staging handed back early
    else if (arg == "--stencil-dead" && i + 1 < argc) { const std::string v = argv[++i]; rdr::g_stencil_dead.store(!(v == "off" || v == "0" || v == "false")); }  // S102 R24-C (depth_nostencil.h)
    else if (arg == "--stencil-dead-poison") rdr::g_stencil_dead_poison.store(true);  // S102 R24-C: tools only
    else if (arg == "--stencil-dead-plant" && i + 1 < argc) rdr::g_stencil_dead_plant.store(std::atoi(argv[++i]));  // S102 R24-C: tests only
    else if (arg == "--depth-census") rdr::g_depth_census_on.store(true);  // S102 R24-C
    else if (arg == "--copy-batch" && i + 1 < argc) { const std::string v = argv[++i]; rdr::g_copy_batch.store(!(v == "off" || v == "0" || v == "false")); }  // S102 G12 (copy_batch.h): adjacent copies of one source into one destination as one command
    else if (arg == "--crumbs" && i + 1 < argc) rdr::crumbs::g_on.store(std::atoi(argv[++i]) != 0);  // S102 D1 (breadcrumbs.h): the lost-device ring, default 1 as in the game
    else if (arg == "--gpu-marker" && i + 1 < argc) rdr::crumbs::g_marker.store(std::atoi(argv[++i]));  // S102 D1: the GPU marker words, 0 off (default, as in the game), 1 class runs, 2 passes + copy runs, 3 every step
    else if (arg == "--crumbs-selftest" && i + 1 < argc) {  // S102 D1: FRAME:STEP: stall the GPU at that step of that frame serial, handle it as a lost device, check the decode
      long long sf = 0, ss = 0;
      if (std::sscanf(argv[++i], "%lld:%lld", &sf, &ss) != 2 || sf < 2 || ss < 0) { Log("[rplay] --crumbs-selftest wants FRAME:STEP (frame >= 2)"); return 2; }
      rdr::crumbs::g_selftest_frame.store(sf), rdr::crumbs::g_selftest_step.store(int32_t(ss));
      bool fatal = false;  // --crumbs-fatal: leave the dev build's stop on a Problem on (review F4: the dump must be out before it)
      for (int j = 1; j < argc; ++j) fatal |= std::string(argv[j]) == "--crumbs-fatal";
      if (!fatal) g_problem_nonfatal.store(true);  // the simulated loss logs a Problem; a dev build must not stop on it
    }
    else if (arg == "--crumbs-fatal") {}  // (read by --crumbs-selftest)
    else if (arg == "--frame-top-barrier" && i + 1 < argc) rdr::crumbs::g_frame_top_barrier.store(std::atoi(argv[++i]));  // S102 D1 round 6: 0 off, 1 top of every frame, 2 + before the present
    else if (arg == "--frame-barrier-plant" && i + 1 < argc) rdr::crumbs::g_frame_barrier_plant.store(std::atoi(argv[++i]));  // S102 D1 round 6: 1 the switch on, the barrier not recorded
    else if (arg == "--role-audit" && i + 1 < argc) rdr::crumbs::g_role_audit.store(std::fopen(argv[++i], "wb"));  // S102 D1: one JSON line per step (tools\renderer\role_audit.py)
    else if (arg == "--crumbs-plant" && i + 1 < argc) rdr::crumbs::g_plant.store(std::atoi(argv[++i]));  // S102 D1: 1 a crumb names the wrong marker, 2 a crumb left out, 3 presents counted after the loss, 4 END words without barriers
    else if (arg == "--frame-facts" && i + 1 < argc) { const std::string v = argv[++i]; rdr::g_frame_facts.store(!(v == "off" || v == "0" || v == "false")); }   // S102 R21-A (frame_facts.h)
    else if (arg == "--frame-facts-plant" && i + 1 < argc) rdr::g_frame_facts_plant.store(std::atoi(argv[++i]));   // S102 R21-A: 1 prefix accepts "arena:slo", 2 twin tail ignored, 3 blend skipped while a consumer is on
    else if (arg == "--frame-shape" && i + 1 < argc) g_frame_shape_name = argv[++i];   // S102 R21-A step 5
    else if (arg == "--plan-memo" && i + 1 < argc) rdr_adapter::plan_memo::g_fallback.store(uint32_t(std::atoi(argv[++i])));   // S102 R23-P: the switch's fallback here (a bit mask, plan_memo.h)
    else if (arg == "--plan-memo-verify" && i + 1 < argc) rdr_adapter::plan_memo::g_verify.store(uint32_t(std::atoi(argv[++i])));   // S102 R23-P
    else if (arg == "--plan-memo-plant" && i + 1 < argc) rdr_adapter::plan_memo::g_plant.store(std::atoi(argv[++i]));   // S102 R23-P TEST ONLY
    else if (arg == "--plan-memo-times") rdr_adapter::plan_memo::g_time_on.store(true);   // S102 R23-P
    else if (arg == "--plan-memo-census" && i + 1 < argc) { g_plan_memo_census_name = argv[++i]; rdr_adapter::plan_memo::g_census_on.store(true); rdr_adapter::plan_memo::g_time_on.store(true); }   // S102 R23-P
    else if (arg == "--tween-probe") g_tween_probe = true;   // S102 R21-A review F2
    else if (arg == "--frame-facts-want-keys") g_ff_want_keys = true;
    else if (arg == "--frame-facts-verify") rdr::g_ff_verify.store(true);   // S102 R21-A   // S102 R21-A
    else if (arg == "--copy-batch-plant" && i + 1 < argc) rdr::g_copy_batch_plant.store(std::atoi(argv[++i]));  // S102 G12: 1 the last region is not copied, 2 the barrier is left out, 3 the control
    else if (arg == "--alias-fullsize" && i + 1 < argc) rdr_adapter::copy_alias::g_fullsize = rdr_adapter::copy_alias::ClampLevel(std::atoi(argv[++i]));  // S102 G8 (copy_alias.h): the full-size alias rule, 0 off, 1 segments + depth, 2 + the Present
    else if (arg == "--copy-alias-fault" && i + 1 < argc) {  // S102 G8: planted faults (copy_alias.h Fault), as fview's
      if (!rdr_adapter::copy_alias::ParseFault(argv[++i], rdr_adapter::copy_alias::g_fault)) {
        Log("[rplay] unknown --copy-alias-fault %s", argv[i]);
        return 2;
      }
    }
    else if (arg == "--copy-alias-classes" && i + 1 < argc) rdr_adapter::copy_alias::g_classes = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--shadow-map-size" && i + 1 < argc) rdr_adapter::g_shadow_map_size = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--shadow-plane-scale" && i + 1 < argc) rdr_adapter::g_shadow_plane_percent = uint32_t(std::atoi(argv[++i]));
    else if (arg == "--shadows-ours-fault" && i + 1 < argc) {
      if (!rdr_adapter::shadows_ours::ParseFault(argv[++i], rdr_adapter::shadows_ours::g_fault)) {
        Log("[rplay] unknown --shadows-ours-fault %s", argv[i]);
        return 2;
      }
    }
    else if (arg == "--turns" && i + 1 < argc) {  // S102 lane G3: bit 1 wrestler chain, 2 rim, 4 props, 8 pre-light (map_turns.h)
      const int m = std::atoi(argv[++i]);
      for (int g = 0; g < 4; ++g) rdr_adapter::map_turns::g_on[g] = (m & (1 << g)) != 0;
    } else if (arg == "--half-drawn-clock" && i + 1 < argc) rdr_adapter::map_turns::g_half_rate_drawn_clock = std::string(argv[++i]) != "off";   // S102 G3 step 1
    else if (arg == "--skip-blank-plane") rdr_adapter::plane_blank::g_fview.store(true);  // S102 G4c: the blank plane chain left out (plane_blank.h), as fview
    else if (arg == "--shadow-half") rdr_adapter::g_shadow_half = true;  // S101 lane 86: the QUALITY LOW half-rate maps, offline
    else if (arg == "--shadow-keying" && i + 1 < argc) {  // S101 lane 86: hash = the old shader-hash lists (the A/B twin); rim_as_shadow = the planted fault
      const std::string mode = argv[++i];
      rdr_adapter::shadow_roles::g_hash_keying = mode == "hash";
      rdr_adapter::shadow_roles::g_rim_as_shadow = mode == "rim_as_shadow";
    }
    else if (arg == "--particles-ours") g_particles_arg = true;  // S101 lane 67
    else if (arg == "--thin" && i + 1 < argc && std::string(argv[i + 1]) == "arena") ++i, g_thin_arg = true;  // lane 122
    else if (arg == "--d3-chains") g_d3_chains = 1;   // S102 K3 rework: arm the D3 chains (bound with them)
    else if (arg == "--d3-chains-off-frames") g_d3_chains = 2;   // S102 K3 rework: armed, every frame bound WITHOUT the chains (what armed-off now costs)
    else if (arg == "--particles-fault" && i + 1 < argc) {
      if (!rdr_adapter::particles::ParseFault(argv[++i], g_particles_fault)) {
        Log("[rplay] unknown --particles-fault %s", argv[i]);
        return 2;
      }
    }
    else if (arg == "--isolate" && i + 1 < argc) isolate = argv[++i];
    else if (arg == "--gpu-class-plant" && i + 1 < argc) rdr_adapter::g_gpu_class_plant = [](const std::string& v) { return v == "scene_to_post" ? 1 : v == "drop_copy_dest" ? 2 : v == "drop_frame_buffer" ? 3 : 0; }(argv[++i]);  // S102 S0-3 test plant
    else if (arg == "--gallery" && i + 1 < argc) gallery_dir = argv[++i];
    else if (arg == "--pump" && i + 1 < argc) fps_cap = std::atof(argv[++i]);
    else if (arg == "--gpu-log" && i + 1 < argc) g_farm.gpu_log_path = argv[++i];  // lane 269
    else if (arg == "--gpu-outside" && i + 1 < argc) rdr::g_gpu_outside.store(std::atoi(argv[++i]) != 0);  // S102 G5: the outside meter (default 0, like the game); 1 = wait on each frame's fence and log [rplay][outside]
    else if (arg == "--upload-batch-plant" && i + 1 < argc) rdr::g_upload_batch_plant.store(std::atoi(argv[++i]));  // S102 G5 test plant (see outside.h)
    else if (arg == "--upload-batch" && i + 1 < argc) rdr::g_upload_batch.store(std::atoi(argv[++i]) != 0);  // S102 G5: the uploads in three groups
    else if (arg == "--gpu-plant" && i + 1 < argc) g_farm.gpu_plant = uint32_t(std::atoi(argv[++i]));  // 1 / 2 lane 269's; 3 / 4 / 5 S102 S0-3's (an empty pass in the plane role / a slow one in the rim role / a slow copy loop at the end of the frame)
    else if (arg == "--draw-census" && i + 1 < argc) {
      const int v = std::atoi(argv[++i]);
      rdr::g_draw_census.store(v != 0), rdr::g_draw_census_stats.store(v >= 2), options.instrument_stats = v >= 2;  // 1 = draws + area bound, 2 = also vertex / fragment shader invocations (waits for the GPU each frame)
    }  // S102 G10: print the colour passes' draws per program at the end
    else if (arg == "--colour-cuts" && i + 1 < argc) rdr::colour_cuts::g_colour_cuts.store(uint32_t(std::atoi(argv[++i])));  // S102 G10: the colour-pass cuts bit mask (colour_cuts.h), as wwe2k14_rdr_low_colour_cuts
    else if (arg == "--alpha-plant" && i + 1 < argc) rdr::colour_cuts::g_plant_mdl_alpha.store(std::atoi(argv[++i]));  // S102 G10 TEST ONLY: 5 = MdlAlpha 0.5 on one yBG_DefaultLM_base draw a frame
    else if (arg == "--alpha-rule-skip") rdr::colour_cuts::g_skip_alpha_rule.store(true);  // S102 G10 TEST ONLY: the alpha rule is skipped (the planted translucent draw is pre-drawn)
    else if (arg == "--colour-cuts-plant" && i + 1 < argc) rdr::colour_cuts::g_colour_cuts_plant.store(std::atoi(argv[++i]));  // S102 G10 TEST ONLY: 1 LESS kept, 2 discard let through, 3 prepass records nothing
    else if (arg == "--arena-overdraw" && i + 1 < argc) {  // S102 C4: wwe2k14_rdr_arena_overdraw (1 = on): the prepass also takes the arena draws whose diffuse texture is proven alpha-free
      const bool on = std::atoi(argv[++i]) != 0;
      rdr::colour_cuts::g_arena_overdraw.store(on), rdr::colour_cuts::g_arena_overdraw_scan.store(on || (rdr::arena_fragments::g_mask.load() & 1) != 0);  // S102 R18-B3 review F4
    }
    else if (arg == "--arena-fragments" && i + 1 < argc) {  // S102 R18-B3: wwe2k14_rdr_arena_fragments (bit mask, arena_fragments.h): 1 blend off for the exactly-opaque draws, 2 the widened prepass families
      const uint32_t mask = uint32_t(std::atoi(argv[++i]));
      rdr::arena_fragments::g_mask.store(mask);
      rdr::colour_cuts::g_arena_overdraw_scan.store(rdr::colour_cuts::g_arena_overdraw.load() || (rdr::arena_fragments::g_mask.load() & 1) != 0);  // review F4: the scan flag is recomputed from both switches, whatever their order
    }
    else if (arg == "--arena-fragments-plant" && i + 1 < argc) rdr::arena_fragments::g_plant.store(std::atoi(argv[++i]));  // S102 R18-B3 TEST ONLY (arena_fragments.h g_plant)
    else if (arg == "--arena-overdraw-plant" && i + 1 < argc) rdr::colour_cuts::g_arena_overdraw_plant.store(std::atoi(argv[++i]));  // S102 C4 TEST ONLY: 1 = the texture-alpha proof admits every texture
    else if (arg == "--half-plant" && i + 1 < argc) rdr::half_trans::g_plant.store(std::atoi(argv[++i]));  // S102 C4 TEST ONLY: 1 plain bilinear composite, 2 every tail draw halved whatever its blend, 3 the half-size depth pass left out
    else if (arg == "--half-transparents" && i + 1 < argc) rdr::colour_cuts::g_half_transparents.store(std::atoi(argv[++i]) != 0);  // S102 C4: wwe2k14_rdr_low_half_transparents
    else if (arg == "--draw-log" && i + 1 < argc) rdr::g_draw_log.store(std::fopen(argv[++i], "wb"));  // S102 C4: the per-draw census lines (needs --draw-census 2)
    else if (arg == "--arms" && i + 1 < argc) ParseArms(argv[++i]);  // S102 G10
    else if (arg == "--arm-skip" && i + 1 < argc) g_arm_skip = std::atoi(argv[++i]);
    else if (arg == "--arm-period" && i + 1 < argc) g_arm_period = std::atoi(argv[++i]);  // S102 R24-D
    else if (arg == "--arena-scan-follow") g_arena_scan_follow = true;  // S102 R24-D
    else if (arg == "--draw-mod" && i + 2 < argc) {  // S102 G10: MODE (1 leave out, 2 1x1 scissor) NAMES (comma list of program-name substrings, "*" = all): the colour-pass ranking instrument (overlay.h)
      const int mode = std::atoi(argv[++i]);
      rdr::SetDrawMod(mode, argv[++i]);
    }
    else if (arg == "--gpu-pass-stamps" && i + 1 < argc) { const bool on = std::atoi(argv[++i]) != 0; rdr::g_gpu_pass_stamps.store(on); if (on) rdr::g_gpu_stamps.store(true); }  // S102 R24-B: the live per-step table ([rdr][gpu][passes]); also turns the stamps on
    else if (arg == "--gpu-group-stamps" && i + 1 < argc) rdr::g_gpu_group_stamps.store(std::clamp(std::atoi(argv[++i]), 0, 255));   // S102 R24-F (wwe2k14_rdr_gpu_group_stamps): N = the least draws a segment holds before a group stamp; also turns the per-step stamps on (the frame carries it)
    else if (arg == "--gpu-group-plant" && i + 1 < argc) rdr::g_gpu_group_plant.store(std::atoi(argv[++i]));   // S102 R24-F TEST ONLY: 1 = a stamp whose segment is not recorded (chain broken), 2 = a draw left out of the segment counts (coverage short)
    else if (arg == "--gpu-census" && i + 1 < argc) rdr::g_gpu_census.store(std::atoi(argv[++i]) != 0);  // S102 G6: 1 = also a stamp per step + the per-(class, image) census lines (needs --gpu-stamps 1)
    else if (arg == "--gpu-stamps" && i + 1 < argc) rdr::g_gpu_stamps.store(std::atoi(argv[++i]) != 0);  // S102 S0-3: default 0 (like the game); 1 = the per-run stamps
    else if (arg == "--timing" && i + 1 < argc) g_farm.timing_path = argv[++i];  // S101 lane22
    else if (arg == "--counts" && i + 1 < argc) g_farm.counts_path = argv[++i];  // S102 M1
    else if (arg == "--frame-selfblit") rdr_adapter::frame_cuts::g_fview_blit = true;     // S102 G11: the switches' fallbacks here (frame_cuts.h)
    else if (arg == "--frame-restore") rdr_adapter::frame_cuts::g_fview_restore = true;
    else if (arg == "--frame-roundtrip" && i + 1 < argc) rdr_adapter::frame_roundtrip::g_fview_level = std::atoi(argv[++i]);   // S102 R18-B1: the switch's fallback here (frame_roundtrip.h)
    else if (arg == "--frame-roundtrip-trace") rdr_adapter::frame_roundtrip::g_trace = true;
    else if (arg == "--frame-roundtrip-fault" && i + 1 < argc) { if (!rdr_adapter::frame_roundtrip::ParseFault(argv[++i], rdr_adapter::frame_roundtrip::g_fault)) { std::fprintf(stderr, "unknown --frame-roundtrip-fault\n"); return 2; } }
    else if (arg == "--frame-cuts-fault" && i + 1 < argc) { if (!rdr_adapter::frame_cuts::ParseFault(argv[++i], rdr_adapter::frame_cuts::g_fault)) { std::fprintf(stderr, "unknown --frame-cuts-fault\n"); return 2; } }
    else if (arg == "--frame-trace" && i + 1 < argc)rdr::g_frame_trace.store(std::fopen(argv[++i], "wb"));  // S102 G11: json lines per frame-class pass / draw / clear (+ the pass GPU ms with --gpu-census 1); closed at exit
    else if (arg == "--ko-draw-none") g_farm.ko_draw_none = true;      // S102 lane KO
    else if (arg == "--ko-render-drop") g_farm.ko_render_drop = true;  // S102 lane KO
    else if (arg == "--ko-plant") g_farm.ko_plant = true;              // S102 lane KO
    else if (arg == "--counts-plant" && i + 1 < argc) g_farm.counts_plant = std::atoi(argv[++i]);  // S102 M1
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
  options.light_cube_compute = rdr_adapter::prelight_compute::g_fview.load() || rdr_adapter::prelight_compute::g_storage_always.load() || g_lc_selftest;   // S102 L1b review F2
  options.multiview = rdr_adapter::prelight_layered::g_fview.load();   // S102 C1 review F2
  options.occlusion_precise = rdr::g_frame_trace.load() != nullptr;  // S102 G11 (F1): the precise queries only when a trace file is open
  if (!g_arms.empty()) rdr::g_arms_on.store(true);  // S102 G10 (review F1)
  options.counts = true;  // S102 M1: counting is always on in rplay (integer increments); --counts FILE writes it out
  if (!g_farm.timing_path.empty() || !g_farm.gpu_log_path.empty() || rdr::g_gpu_group_stamps.load() > 0) options.gpu_timestamps = true;  // S101 lane22, lane 269; S102 R24-F: the group stamps need the timestamp pool
  options.gpu_timestamp_plant = g_farm.gpu_plant < 3 ? g_farm.gpu_plant : 0;
  options.gpu_stamp_plant = g_farm.gpu_plant >= 3 ? g_farm.gpu_plant : 0;  // S102 S0-3
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
  rdr::PresentFetchPlants(*renderer);  // S102 R24-A (TEST ONLY: a plant flag or nothing)
  if (g_lc_selftest) return LightCubeSelfTest(*renderer);   // S102 L1b
  if (g_stencil_selftest) return StencilSelfTest(*renderer);   // S102 R24-C
  if (g_stage_selftest > 0) return rdr::StageSelfTest(*renderer, g_stage_split, g_stage_selftest);   // S102 R21-B
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
  PresentReport(*renderer, g_present_w, g_present_h);  // S102 G2
  const uint64_t errors = renderer->validation_errors();
  Log("[rplay] device %s, %d frames, validation errors %llu", renderer->device_name(), frames, static_cast<unsigned long long>(errors));
  if (errors) exit_code = 4;
  return exit_code;
}
