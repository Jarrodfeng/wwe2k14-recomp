// wwe2k14 - guest frame timing. The game calls VdSwap once per frame it presents (hooked in
// kernel_overrides.h), so the time between calls is the game's own frame time: what the player feels as
// slowdown, independent of how the host GPU presents. Logs a summary every few seconds plus every hitch,
// and writes every frame to logs/frames_NNN.csv (NNN = this run's wwe2k14_NNN.log).
// Session 73 (Jarrod: "maximal information" from testers' logs): the CSV also has each frame's game-thread CPU time,
// the GPU command thread's CPU time (Android / Linux), the GPU's own busy time (of the latest frame it finished, from
// the submissions' timestamps: a frame or two behind), the scene's draw height and a scene marker ("entrance start");
// every frame over wwe2k14_slow_frame_ms gets a "slow frame" line saying what happened in it (pipelines / images /
// shaders made on the spot, files opened, memory probes, GPU waits); Android logs the thermal status, headroom and
// the CPU clusters' clocks with every summary.
// Session 73, b77 (Jarrod's b76 log: each entrance's first ~2 s ran the GPU at 35-40 ms a frame, 6-13 ms after, with
// the CPU idle and no heat): the GPU's clock where the phone lets an app read it (kgsl / devfreq sysfs: CSV gpu_mhz,
// the slow-frame and thermal lines), texture loads per frame, and "keep warm": with --wwe2k14_gpu_keep_warm, GPU busy
// work in the in-game sequence (SDK KeepWarmWork: buffer fills, at most once per 1/60 s of wall time), so the phone's
// GPU governor does not drop the clock through a load and start the next scene at the bottom. The phone launcher
// turns it on.
// b78 (Jarrod's b77 log, report 118: the fixed 96 MB cost the GPU ~2 ms a frame, too little; the clock files are
// "Permission denied" on his phone; the entrance's GPU time rose in flat steps 8 -> 17.6 -> 37.7 ms while the GPU sat
// idle half of each frame - the governor seemingly lowering the clock): keep-warm now steers the GPU's busy share (its
// submissions' timestamp time per wall time, every 100 ms) towards wwe2k14_gpu_keep_warm_target_ms per 1/60 s: more
// fill while under it, less while over, none in a frame over 20 ms (a GPU-bound frame gets no extra work), up to
// wwe2k14_gpu_keep_warm_mb. The GPU waits are split by why (SDK WaitSite: the frame limit, copies / resolves, occlusion
// queries, readbacks, draws): the slow-frame lines and the CSV say which one held the frame.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <share.h>
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/perf/frame_events.h>  // Session 73: what a frame did on the GPU side

#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <ctime>
#include <map>
#endif

#include "guest_gpu_only.h"  // lane 256
#include "blackbox.h"     // Session 73: the first frame in the start-up black box
#include "dynamic_res.h"  // Session 73: each frame's length to the dynamic resolution
#include "perf_overlay.h"  // Session 80: each frame's length to the performance overlay
#include "renderer/overlay.h"  // lane 269: rdr::g_gpu_* (the new renderer's GPU timestamps)
#include "renderer/breadcrumbs.h"  // S102 D1: rdr::DeviceLost (the device_lost column, the player's message)
#include <thread>
#include "thread_name.h"  // S103: the lost-device message box thread names itself
#include "android_lifecycle.h"  // Session 95: a frame out of front is not a hitch
#include "hang_guard.h"  // Session 95: --wwe2k14_hang_guard and its test switch
#include "renderer_game/scene_frame.h"  // S103 H: SnapshotMode() in OnSwap (the scene mode is read once per guest frame)
#include "perf_mem.h"    // lane 249: [perf][mem] lines
#include "renderer_adapter/skips_auto.h"  // S102 K3: the D3 skips' frame-boundary latch (no game types)
#include "device_lost_message.h"  // S105 A9
#include "renderer_adapter/rdr_switches_cvar.h"  // S102 S0-4: the per-frame switch table (no game types)
#include "ab_flip.h"  // S102 M7: --wwe2k14_ab_flip, the in-run A/B flip schedule
#include "frame_split.h"  // S102 H3: --wwe2k14_frame_split (the swap thread's per-frame split, columns at the end of the csv)

#if defined(__ANDROID__)
#include "android_perf_hint.h"  // Session 64: each frame's length to the phone's CPU governor
#include "android_pace.h"       // Session 93: present pacing, thermal headroom, the M5c pace gate switch
#include "android_driver.h"     // Session 72: the custom driver's trial file goes after a few hundred frames
#include "android_frame_rate.h"  // Session 73: the screen's refresh rate set for a 60 fps source
#endif

REXCVAR_DECLARE(int32_t, wwe2k14_frame_summary_seconds);
REXCVAR_DECLARE(int32_t, wwe2k14_hitch_ms);
REXCVAR_DECLARE(bool, wwe2k14_frame_csv);
REXCVAR_DECLARE(int32_t, wwe2k14_slow_frame_ms);
REXCVAR_DECLARE(bool, wwe2k14_frame_gpu_time);
REXCVAR_DECLARE(bool, wwe2k14_gpu_keep_warm);
REXCVAR_DECLARE(bool, wwe2k14_frame_split);
REXCVAR_DECLARE(int32_t, wwe2k14_gpu_keep_warm_mb);
REXCVAR_DECLARE(double, wwe2k14_gpu_keep_warm_target_ms);

namespace wwe2k14 {

// This run's log number: the newest logs/wwe2k14_NNN.log, which ReXGlue creates at startup. Our own files
// use it (frames_NNN.csv, captures/NNN_*.bmp) but never the "wwe2k14_" prefix, which ReXGlue's sequential
// log naming scans (core/logging.cpp NextSequentialLogPath). -1 if none.
// Session 8 task 0: taken ONCE and kept (CurrentLogNumber, primed in OnPostInitLogging right after ReXGlue created
// the log). With the run lock's shared mode up to 3 instances run at once, and a later scan would see another
// instance's newer log (runs 384/385 saved their screenshots as 386_*). run_and_triage serializes launches until
// the new log exists, so at priming time the newest log is this run's. One instance: same number as before.
inline int ScanNewestLogNumber() {
  int newest = -1;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path() / "logs", ec)) {
    const std::string stem = entry.path().stem().string();
    if (entry.path().extension() != ".log" || stem.rfind("wwe2k14_", 0) != 0) continue;
    const std::string digits = stem.substr(8);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
    newest = std::max(newest, std::stoi(digits));
  }
  return newest;
}
inline int CurrentLogNumber() {
  static std::atomic<int> cached{-1};
  int v = cached.load(std::memory_order_acquire);
  if (v >= 0) return v;
  v = ScanNewestLogNumber();
  if (v < 0) return v;
  int expected = -1;
  cached.compare_exchange_strong(expected, v, std::memory_order_acq_rel);
  return cached.load(std::memory_order_acquire);
}

// S108 SHOT-A (docs\research\s108\SHOT_plan.md 5): this run's own folder for the subject-shot outputs: captures\NNN_pPID (NNN = the log number, PID = the process id the run logs as
// "run log number: NNN (pid P)"). A run is told apart by its folder, never by a bare number. Fixed on first use (the verb runs long after start). Not created here.
inline std::filesystem::path RunCaptureDir() {
  static const std::filesystem::path dir = [] {
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    char name[48];
    std::snprintf(name, sizeof name, "%03d_p%lu", std::max(CurrentLogNumber(), 0), pid);
    return std::filesystem::current_path() / "captures" / name;
  }();
  return dir;
}

// Guest frames presented so far (VdSwap calls). Also the clock for input replay later.
inline std::atomic<uint64_t> g_guest_frame{0};

// Session 73: what the game side did in the current frame (the slow-frame lines): files opened (kernel_overrides.h
// file hook), memory pool probes (MmAllocatePhysicalMemoryEx calls), and the scene marker for the CSV
// (session_log.h's scene changes, load_turbo.h's window ends).
inline std::atomic<uint32_t> g_frame_file_opens{0};
inline std::atomic<uint32_t> g_frame_mem_probes{0};
// Session 73: true while the game is in its in-game sequence (9: the entrances and the match; load_turbo.h sets it),
// the only place the keep-warm work runs.
inline std::atomic<bool> g_in_game_sequence{false};
inline std::mutex g_scene_marker_mutex;
inline std::string g_scene_marker;
inline void NoteScene(const std::string& what) {
  std::lock_guard<std::mutex> lock(g_scene_marker_mutex);
  if (!g_scene_marker.empty()) g_scene_marker += " / ";
  g_scene_marker += what;
}

// Session 29: set by harness steps that pause the game on purpose (a memory scan): the next frame's length is not a
// hitch (src/bookmark.h's automatic bookmarks skip it).
inline std::atomic<bool> g_frame_time_ignore_next{false};

// Windows thread id of the first thread that called VdSwap (the game's main thread, as the profiler's "main thread");
// 0 before the first guest frame. thread_placement.h places it under the pseudo-name "main".
inline std::atomic<uint32_t> g_swap_thread_id{0};

class FrameStats {
 public:
  void OnSwap() {
    if (android_lifecycle::AtFrameBoundary(g_guest_frame.load(std::memory_order_relaxed) + 1)) g_frame_time_ignore_next.store(true, std::memory_order_relaxed);  // Session 95
    hang_guard::AtFrameBoundary(g_guest_frame.load(std::memory_order_relaxed) + 1);  // Session 95: test only, before the lock below
    using clock = std::chrono::steady_clock;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = clock::now();
    const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed) + 1;   // S102 K3 review F6: the number is published AFTER the switch refresh and the skips' latch below, so a recording thread that reads frame N reads N's latch
    ab_flip::AtFrame(frame);  // S102 M7: --wwe2k14_ab_flip (default off): the in-run A/B flips its switches here, so the refresh below logs them at this frame
    rdr_sw::RefreshFromCvars(frame);  // S102 S0-4: the lane switches, read once per guest frame (a change logs "[switch] frame N NAME=VALUE" once)
    scene_frame::SnapshotMode();  // S103 H: the scene mode, once per guest frame, before the number below is published (a run-time cvar change never splits a frame)
    skips_auto::AtGuestFrame(rdr_sw::Bool(rdr_sw::kSkipsAuto), frame);  // S102 K3: the D3 skips' on / off is latched here, once per guest frame, from the render thread's decision (skips_auto.h)
    g_guest_frame.store(frame, std::memory_order_release);
    if (frame == 1) blackbox::Step("first frame: the game is running");  // Session 73
    if (frame == 1) perf_mem::Start(&g_guest_frame);  // lane 249: the memory line thread + the boot line
    if (frame == 600) blackbox::Step("600 frames in");
#ifdef _WIN32
    const uint32_t thread_id = GetCurrentThreadId();
    if (frame == 1) {
      g_swap_thread_id.store(thread_id, std::memory_order_relaxed);
    } else if (thread_id != g_swap_thread_id.load(std::memory_order_relaxed) && !other_swapper_logged_) {
      other_swapper_logged_ = true;  // evidence for whether "main" is one stable thread; logged once per run
      REXLOG_INFO("[wwe2k14][frames] VdSwap called from thread {} at frame {} (first caller: thread {}); "
                  "logged once", thread_id, frame, g_swap_thread_id.load(std::memory_order_relaxed));
    }
#endif
    if (frame == 1) {
      start_ = last_ = window_start_ = now;
      window_first_frame_ = 1;
      rex::perf::frame_events::Get().measure_gpu.store(REXCVAR_GET(wwe2k14_frame_gpu_time), std::memory_order_relaxed);
      last_cpu_ns_ = ThreadCpuNs();
      OpenCsv();
      REXLOG_INFO("[wwe2k14][frames] first guest frame{}", csv_ ? " (per-frame CSV: " + csv_path_ + ")" : "");
      return;
    }
    const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
    const double t = std::chrono::duration<double>(now - start_).count();
    last_ = now;
    perf_overlay::PushFrame(ms);  // Session 80 (--wwe2k14_perf_overlay)
#if defined(__ANDROID__)
    android_driver::OnFrame();
    android_frame_rate::g_frame_rate.OnFrame(frame);
#endif

    // Session 73: what this frame did.
    const uint64_t cpu_ns = ThreadCpuNs();
    const double game_cpu_ms = cpu_ns && last_cpu_ns_ ? double(cpu_ns - last_cpu_ns_) / 1e6 : -1.0;
    last_cpu_ns_ = cpu_ns;
    // S102 H3: this thread's split since the last row (only when the csv was opened with the columns; the counters are cumulative, Splitter diffs them).
    const frame_split::Row split_row = split_csv_ ? split_.Take(frame_split::Gather(), game_cpu_ms) : frame_split::Row{};
    if (split_csv_ && split_.backwards() && !split_backwards_logged_) {  // F2: a counter that went backwards (the row counted 0 for it): said once
      split_backwards_logged_ = true;
      REXLOG_WARN("[wwe2k14][frame_split] a split counter went backwards at frame {} (counted 0 for that part; {} so far)", frame, split_.backwards());
    }
    const uint64_t gpu_thread_ns = GpuThreadCpuNs();
    const double gpu_thread_ms =
        gpu_thread_ns && last_gpu_thread_ns_ ? double(gpu_thread_ns - last_gpu_thread_ns_) / 1e6 : -1.0;
    last_gpu_thread_ns_ = gpu_thread_ns;
    auto& events = rex::perf::frame_events::Get();
    double gpu_ms = -1.0;
    if (const uint64_t done = events.gpu_frames_done.load(std::memory_order_relaxed); done != last_gpu_frames_done_) {
      last_gpu_frames_done_ = done;
      gpu_ms = events.gpu_frame_us.load(std::memory_order_relaxed) / 1000.0;
      last_gpu_ms_ = gpu_ms;
    }
    // Lane 269: in owner mode the SDK measures nothing; the new renderer's own timestamps (renderer.cpp CollectGpuTime, read a few frames late, no stall)
    // fill the CSV gpu_ms and the slow-frame GPU field: the GPU time of the frames it submitted since the last guest frame (a decoupled 120's halfway
    // pictures included). NOT fed to dynamic resolution or the perf hint (last_gpu_ms_ stays the SDK's): their behaviour is unchanged.
    double shown_gpu_ms = gpu_ms;
    if (const uint64_t owner_frames = rdr::g_gpu_frames.load(std::memory_order_relaxed); owner_frames != last_owner_gpu_frames_) {
      const uint64_t owner_ns = rdr::g_gpu_ns_total.load(std::memory_order_relaxed);
      if (gpu_ms < 0 && last_owner_gpu_frames_ != 0) shown_gpu_ms = double(owner_ns - last_owner_gpu_ns_) / 1e6;
      last_owner_gpu_frames_ = owner_frames, last_owner_gpu_ns_ = owner_ns;
    }
    // S102 S0-3: what ran since the last guest frame. rdr_submit_ms = time inside Renderer::Render (fence wait and acquire left out); gpu_work_ms = the stamped GPU work (the start gap
    // left out); render_thread_ms = the live render THREAD's CPU time (bind + parse + Render, live.cpp's loop; thread CPU, so waits and the Present's blocking are not in it);
    // acquire_ms / present_call_ms = time in vkAcquireNextImage / the Present call. Empty = nothing counted yet.
    double rdr_submit_ms = -1.0, gpu_work_ms = -1.0, render_thread_ms = -1.0, acquire_ms = -1.0, present_call_ms = -1.0;
    double record_thread_ms = -1.0;  // S102 R21-B (review F2): rdr_record's CPU since the last guest frame (the two-stage render thread); empty until that stage has run
    {
      const auto take = [](const std::atomic<uint64_t>& total, uint64_t& last, double& out_ms) {
        const uint64_t v = total.load(std::memory_order_relaxed);  // (one load)
        if (last != 0 || v != 0) out_ms = double(v - last) / 1e6;
        last = v;
      };
      const uint64_t submit_calls = rdr::g_submit_busy_calls.load(std::memory_order_relaxed);
      if (submit_calls != last_submit_calls_ && last_submit_calls_ != 0) take(rdr::g_submit_busy_ns_total, last_submit_ns_, rdr_submit_ms);
      else last_submit_ns_ = rdr::g_submit_busy_ns_total.load(std::memory_order_relaxed);
      last_submit_calls_ = submit_calls;
      const uint64_t work_ns = rdr::g_gpu_work_ns_total.load(std::memory_order_relaxed);
      if (work_ns != last_gpu_work_ns_ && last_gpu_work_ns_ != 0) gpu_work_ms = double(work_ns - last_gpu_work_ns_) / 1e6;
      last_gpu_work_ns_ = work_ns;
      const uint64_t passes = rdr::g_render_thread_passes.load(std::memory_order_relaxed);
      if (passes != 0) take(rdr::g_render_thread_ns_total, last_render_thread_ns_, render_thread_ms);
      if (rdr::g_record_thread_ns_total.load(std::memory_order_relaxed) != 0) take(rdr::g_record_thread_ns_total, last_record_thread_ns_, record_thread_ms);  // S102 R21-B
      if (submit_calls != 0) {
        take(rdr::g_acquire_ns_total, last_acquire_ns_, acquire_ms);
        take(rdr::g_present_call_ns_total, last_present_call_ns_, present_call_ms);
      }
    }
    // Session 73 (moved below the GPU read in Session 95): dynamic resolution judges on the GPU time too.
    if (!g_frame_time_ignore_next.load(std::memory_order_relaxed)) dynres::g_controller.OnFrame(ms, last_gpu_ms_, game_cpu_ms);
#if defined(__ANDROID__)
    // The frame's length and the GPU's time (b77: the hint session reports GPU time too where Android has it, API 35).
    if (!g_frame_time_ignore_next.load(std::memory_order_relaxed)) perf_hint::g_perf_hint.OnFrame(ms, last_gpu_ms_);
    android_pace::OnFrame();  // Session 93: present pacing's frame rate, the thermal headroom (all off by default)
#endif
    KeepWarm(now, ms);
    const int gpu_mhz = GpuClockMhz();
    const uint32_t textures = events.textures.load(std::memory_order_relaxed);
    const uint64_t texture_kb = events.texture_kb.load(std::memory_order_relaxed);
    const uint64_t warm_kb = events.warm_kb_done.load(std::memory_order_relaxed);
    const uint32_t pipelines = events.pipelines.load(std::memory_order_relaxed);
    const uint32_t images = events.images.load(std::memory_order_relaxed);
    const uint32_t shaders = events.shaders.load(std::memory_order_relaxed);
    const uint64_t gpu_wait_us = events.gpu_wait_us.load(std::memory_order_relaxed);
    // b78: this frame's waits by why; the biggest for the CSV.
    constexpr uint32_t kSites = rex::perf::frame_events::kWaitSiteCount;
    double wait_ms_by[kSites];
    uint32_t wait_main = 0;
    for (uint32_t i = 0; i < kSites; ++i) {
      const uint64_t v = events.gpu_wait_us_by[i].load(std::memory_order_relaxed);
      wait_ms_by[i] = double(v - last_wait_us_by_[i]) / 1000.0;
      last_wait_us_by_[i] = v;
      if (wait_ms_by[i] > wait_ms_by[wait_main]) wait_main = i;
    }
    const double wait_ms = double(gpu_wait_us - last_gpu_wait_us_) / 1000.0;
    const uint32_t files = g_frame_file_opens.load(std::memory_order_relaxed);
    const uint32_t probes = g_frame_mem_probes.load(std::memory_order_relaxed);
    std::string scene;
    {
      std::lock_guard<std::mutex> scene_lock(g_scene_marker_mutex);
      scene.swap(g_scene_marker);
    }
    if (!scene.empty()) last_scene_ = scene, last_scene_t_ = t;
    const int rows = dynres::g_controller.Rows();
    // Session 95: frames that reached a present since the last row (a swap is a frame drawn, not a frame shown).
    // S102 D1: the renderer lost its Vulkan device (rdr::g_device_lost): every row from here says so (frames_*.csv device_lost = 1), one line here
    // names the guest frame, and the player's message shows once when --wwe2k14_rdr_device_lost_message is on (PC only; the game keeps running behind it).
    const bool lost_now = rdr::DeviceLost();
    if (lost_now && !device_lost_seen_) {
      device_lost_seen_ = true;
      REXLOG_ERROR("[wwe2k14][frames] DEVICE LOST at guest frame {} (t={:.1f} s): the GPU stopped drawing; frames from here are not presented (frames csv device_lost = 1)", frame, t);
      REXLOG_ERROR("{} (at the device loss: video memory use against DXGI's budget)", wwe2k14::perf_mem::Format(wwe2k14::perf_mem::Take(), t, frame, false));  // S102 D1 review: the residency hypothesis
      TellPlayerDeviceLost(frame);  // S105 A9: the switch is read from the rdr_sw table (both builds); before, only the stream build ever set rdr::crumbs::g_message
    }
    const uint64_t presents = events.presents.load(std::memory_order_acquire);
    const uint32_t presented = uint32_t(presents - last_presents_);
    last_presents_ = presents;
    window_presented_ += presented;
    if (csv_) {
      for (char& c : scene) c = (c == ',' || c == '\n') ? ';' : c;
      char gpu_text[24] = "", game_text[24] = "", gthread_text[24] = "";
      if (shown_gpu_ms >= 0) std::snprintf(gpu_text, sizeof(gpu_text), "%.3f", shown_gpu_ms);
      if (game_cpu_ms >= 0) std::snprintf(game_text, sizeof(game_text), "%.3f", game_cpu_ms);
      if (gpu_thread_ms >= 0) std::snprintf(gthread_text, sizeof(gthread_text), "%.3f", gpu_thread_ms);
      char mhz_text[16] = "";
      if (gpu_mhz > 0) std::snprintf(mhz_text, sizeof(mhz_text), "%d", gpu_mhz);
      char gap_text[24] = "", gapmax_text[24] = "";
      const uint32_t gap_max_us = rdr::g_present_gap_max_us.exchange(0, std::memory_order_relaxed);   // S102 R25-C2: take and clear every row (also a 0-present row, so a stale gap never leaks into the next)
      if (presented && gap_max_us) std::snprintf(gapmax_text, sizeof(gapmax_text), "%.3f", gap_max_us / 1000.0);
      if (presented) std::snprintf(gap_text, sizeof(gap_text), "%.3f", events.present_gap_us.load(std::memory_order_relaxed) / 1000.0);
      char submit_text[24] = "", work_text[24] = "", rthread_text[24] = "", acquire_text[24] = "", presentcall_text[24] = "";  // S102 S0-3
      if (rdr_submit_ms >= 0) std::snprintf(submit_text, sizeof(submit_text), "%.3f", rdr_submit_ms);
      if (gpu_work_ms >= 0) std::snprintf(work_text, sizeof(work_text), "%.3f", gpu_work_ms);
      if (render_thread_ms >= 0) std::snprintf(rthread_text, sizeof(rthread_text), "%.3f", render_thread_ms);
      if (acquire_ms >= 0) std::snprintf(acquire_text, sizeof(acquire_text), "%.3f", acquire_ms);
      if (present_call_ms >= 0) std::snprintf(presentcall_text, sizeof(presentcall_text), "%.3f", present_call_ms);
      char record_text[24] = "";  // S102 R21-B
      if (record_thread_ms >= 0) std::snprintf(record_text, sizeof(record_text), "%.3f", record_thread_ms);
      char split_text[frame_split::kColumns * 16 + 8] = "";  // S102 H3: ",a,b,..." (empty cells on the first row) or nothing when the switch is off
      if (split_csv_) {
        size_t at = 0;
        for (int i = 0; i < frame_split::kColumns && at + 16 < sizeof(split_text); ++i) {
          if (split_row.valid && !std::isnan(split_row.ms[i])) at += size_t(std::snprintf(split_text + at, sizeof(split_text) - at, ",%.3f", split_row.ms[i]));  // (NaN = unknown: an empty cell)
          else split_text[at++] = ',', split_text[at] = 0;
        }
      }
      std::fprintf(csv_, "%llu,%.3f,%.3f,%s,%s,%s,%d,%s,%u,%.1f,%.0f,%.2f,%s,%s,%u,%s,%s,%s,%s,%s,%s,%s,%d,%s%s\n",  // S102 D1: device_lost after record_thread_ms (the fs_ split columns stay last)
                   static_cast<unsigned long long>(frame), t, ms, game_text, gthread_text, gpu_text, rows, mhz_text,
                   textures - last_textures_, double(texture_kb - last_texture_kb_) / 1024.0,
                   double(warm_kb - last_warm_kb_) / 1024.0, wait_ms,
                   wait_ms > 0.05 ? rex::perf::frame_events::kWaitSiteNames[wait_main] : "", scene.c_str(), presented, gap_text, submit_text, work_text, rthread_text, acquire_text, presentcall_text, record_text, lost_now ? 1 : 0, gapmax_text, split_text);
    }
    const int32_t slow_ms = REXCVAR_GET(wwe2k14_slow_frame_ms);
    if (slow_ms > 0 && ms >= slow_ms && slow_logged_ < kMaxSlowLines) {
      ++slow_logged_;
      std::string gpu_part = shown_gpu_ms >= 0 ? fmt::format("GPU {:.1f} ms", shown_gpu_ms) : std::string("GPU -");
      if (gpu_mhz > 0) gpu_part += fmt::format(" at {} MHz", gpu_mhz);
      REXLOG_INFO("[wwe2k14][frames] slow frame {}: {:.0f} ms (t={:.1f} s): game CPU {}, GPU thread {}, {}; made: "
                  "{} pipelines, {} images, {} shaders, {} texture loads ({:.1f} MB); {} files opened, {} memory "
                  "probes; GPU waits {:.1f} ms{}{}{}{}",
                  frame, ms, t, game_cpu_ms >= 0 ? fmt::format("{:.1f} ms", game_cpu_ms) : std::string("-"),
                  gpu_thread_ms >= 0 ? fmt::format("{:.1f} ms", gpu_thread_ms) : std::string("-"), gpu_part,
                  pipelines - last_pipelines_, images - last_images_, shaders - last_shaders_,
                  textures - last_textures_, double(texture_kb - last_texture_kb_) / 1024.0, files - last_files_,
                  probes - last_probes_, wait_ms, WaitSplit(wait_ms_by),
                  warm_kb != last_warm_kb_ ? fmt::format("; keep-warm {:.0f} MB", (warm_kb - last_warm_kb_) / 1024.0)
                                           : std::string(),
                  last_scene_.empty() ? "" : fmt::format("; {:.1f} s after {}", t - last_scene_t_, last_scene_),
                  slow_logged_ == kMaxSlowLines ? " (further slow frames only in the CSV)" : "");
    }
    last_pipelines_ = pipelines, last_images_ = images, last_shaders_ = shaders;
    last_files_ = files, last_probes_ = probes, last_gpu_wait_us_ = gpu_wait_us;
    last_textures_ = textures, last_texture_kb_ = texture_kb, last_warm_kb_ = warm_kb;

    window_ms_.push_back(ms);
    const int32_t hitch_ms = REXCVAR_GET(wwe2k14_hitch_ms);
    if (hitch_ms > 0 && ms >= hitch_ms) {
      ++window_hitches_;
      if (++hitches_logged_ <= kMaxHitchLines) {
        REXLOG_INFO("[wwe2k14][frames] hitch: frame {} took {:.0f} ms (t={:.1f} s){}", frame, ms, t,
                    hitches_logged_ == kMaxHitchLines ? "; further hitches only counted in summaries" : "");
      }
    }

    const int32_t summary_s = REXCVAR_GET(wwe2k14_frame_summary_seconds);
    const double window_s = std::chrono::duration<double>(now - window_start_).count();
    if (summary_s > 0 && window_s >= summary_s) {
      LogSummary(frame, t, window_s);
      LogThermal(t);  // Session 73 (Android)
      window_start_ = now;
      window_first_frame_ = frame + 1;
      window_ms_.clear();
      window_hitches_ = 0;
      window_presented_ = 0;
      if (csv_) std::fflush(csv_);
    }
  }

 private:
  static constexpr uint32_t kMaxHitchLines = 2000;
  static constexpr uint32_t kMaxSlowLines = 4000;

  // Session 73: this thread's CPU time (the game's swapping thread), ns; 0 = unknown.
  static uint64_t ThreadCpuNs() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    const auto v = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(kernel) + v(user)) * 100;
#else
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
  }

  // Session 73: the GPU command thread's CPU time (Android / Linux: /proc/self/task/TID/schedstat, ns); 0 = unknown.
  // Found by name ("GPU Commands", cut to 15 characters) once, retried every 300 frames until found.
  uint64_t GpuThreadCpuNs() {
#if defined(_WIN32)
    return 0;
#else
    if (gpu_thread_fd_ < 0) {
      if (GuestGpuOnly()) return 0;  // lane 256: no 'GPU Commands' thread exists (no /proc scan every 300 frames for good)
      if (g_guest_frame.load(std::memory_order_relaxed) < gpu_thread_retry_frame_) return 0;
      gpu_thread_retry_frame_ = g_guest_frame.load(std::memory_order_relaxed) + 300;
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        char name[64] = {};
        if (FILE* f = std::fopen((e.path() / "comm").c_str(), "r")) {
          if (!std::fgets(name, sizeof(name), f)) name[0] = 0;
          std::fclose(f);
        }
        if (std::strncmp(name, "GPU Commands", 12) == 0) {
          gpu_thread_fd_ = ::open((e.path() / "schedstat").c_str(), O_RDONLY | O_CLOEXEC);
          break;
        }
      }
      if (gpu_thread_fd_ < 0) return 0;
    }
    char buf[96];
    const ssize_t n = ::pread(gpu_thread_fd_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return 0;
    buf[n] = 0;
    return std::strtoull(buf, nullptr, 10);
#endif
  }

  // Session 73: Android's thermal status and headroom (AThermal, API 30 / 31, by dlsym) and the CPU clusters' clocks
  // now, with every summary: "[wwe2k14][thermal] t=125 s: status 2 (moderate), headroom 0.83; CPU 0-5 2.21 GHz, ...".
  void LogThermal(double t) {
#if defined(__ANDROID__)
    struct AThermalManager;
    using AcquireFn = AThermalManager* (*)();
    using StatusFn = int (*)(AThermalManager*);
    using HeadroomFn = float (*)(AThermalManager*, int);
    static void* lib = dlopen("libandroid.so", RTLD_NOW);
    static auto acquire = lib ? reinterpret_cast<AcquireFn>(dlsym(lib, "AThermal_acquireManager")) : nullptr;
    static auto status_fn = lib ? reinterpret_cast<StatusFn>(dlsym(lib, "AThermal_getCurrentThermalStatus")) : nullptr;
    static auto headroom_fn = lib ? reinterpret_cast<HeadroomFn>(dlsym(lib, "AThermal_getThermalHeadroom")) : nullptr;
    static AThermalManager* manager = acquire ? acquire() : nullptr;
    static constexpr const char* kStatus[] = {"none", "light", "moderate", "severe", "critical", "emergency",
                                              "shutdown"};
    std::string text = fmt::format("[wwe2k14][thermal] t={:.0f} s:", t);
    if (manager && status_fn) {
      const int status = status_fn(manager);
      text += fmt::format(" status {} ({})", status, status >= 0 && status <= 6 ? kStatus[status] : "?");
    } else {
      text += " status -";
    }
    if (manager && headroom_fn) {
      const float headroom = headroom_fn(manager, 0);  // (now; NaN when not known yet)
      if (headroom == headroom) text += fmt::format(", headroom {:.2f}", headroom);
    }
    // The clusters: CPUs grouped by their top clock, each group's current clock.
    static std::map<unsigned, std::vector<int>> clusters = [] {
      std::map<unsigned, std::vector<int>> by_max;
      const long cpus = sysconf(_SC_NPROCESSORS_CONF);
      for (int cpu = 0; cpu < cpus && cpu < 64; ++cpu) {
        unsigned khz = 0;
        if (FILE* f = std::fopen(fmt::format("/sys/devices/system/cpu/cpu{}/cpufreq/cpuinfo_max_freq", cpu).c_str(),
                                 "r")) {
          if (std::fscanf(f, "%u", &khz) != 1) khz = 0;
          std::fclose(f);
        }
        by_max[khz].push_back(cpu);
      }
      return by_max;
    }();
    std::string cpu_text;
    for (const auto& [max_khz, list] : clusters) {
      unsigned khz = 0;
      if (FILE* f = std::fopen(
              fmt::format("/sys/devices/system/cpu/cpu{}/cpufreq/scaling_cur_freq", list.front()).c_str(), "r")) {
        if (std::fscanf(f, "%u", &khz) != 1) khz = 0;
        std::fclose(f);
      }
      cpu_text += fmt::format("{}{}-{} {:.2f} GHz", cpu_text.empty() ? "; CPU " : ", ", list.front(), list.back(),
                              khz / 1e6);
    }
    std::string gpu_text;
    if (const int mhz = GpuClockMhz(); mhz > 0) gpu_text = fmt::format("; GPU {} MHz", mhz);
    if (const int busy = GpuBusyPercent(); busy >= 0) {
      gpu_text += fmt::format("{}busy {}%", gpu_text.empty() ? "; GPU " : ", ", busy);
    }
    {
      const auto now = std::chrono::steady_clock::now();
      const uint64_t sdk_busy = rex::perf::frame_events::Get().gpu_busy_us_total.load(std::memory_order_relaxed);
      const uint64_t busy = sdk_busy ? sdk_busy : rdr::g_gpu_ns_total.load(std::memory_order_relaxed) / 1000;  // lane 269: owner mode has no SDK timestamps
      if (thermal_busy_time_ != std::chrono::steady_clock::time_point{}) {
        const double wall_us = std::chrono::duration<double, std::micro>(now - thermal_busy_time_).count();
        if (wall_us > 0) {
          gpu_text += fmt::format("{}GPU busy {:.0f}% (timestamps)", gpu_text.empty() ? "; " : ", ",
                                  100.0 * double(busy - thermal_busy_us_) / wall_us);
        }
      }
      thermal_busy_us_ = busy;
      thermal_busy_time_ = now;
    }
    const uint64_t warm_kb = rex::perf::frame_events::Get().warm_kb_done.load(std::memory_order_relaxed);
    if (warm_kb != thermal_warm_kb_) {
      gpu_text += fmt::format("; keep-warm {:.0f} MB since the last line", (warm_kb - thermal_warm_kb_) / 1024.0);
      thermal_warm_kb_ = warm_kb;
    }
    REXLOG_INFO("{}{}{}", text, cpu_text, gpu_text);
#else
    (void)t;
#endif
  }

  // b78: " (frame 40.1, copy 3.2)": the frame's GPU waits by why, the ones over 0.05 ms.
  static std::string WaitSplit(const double* ms_by) {
    std::string out;
    for (uint32_t i = 0; i < rex::perf::frame_events::kWaitSiteCount; ++i) {
      if (ms_by[i] < 0.05) continue;
      out += fmt::format("{}{} {:.1f}", out.empty() ? " (" : ", ", rex::perf::frame_events::kWaitSiteNames[i], ms_by[i]);
    }
    return out.empty() ? out : out + ")";
  }

  // b78: keep-warm work (see the top of the file). Every 100 ms of wall time the GPU's busy share (its submissions'
  // timestamp time) is compared with the target; the fill grows 25% while under it and shrinks 20% while over.
  void KeepWarm(std::chrono::steady_clock::time_point now, double frame_ms) {
    if (!REXCVAR_GET(wwe2k14_gpu_keep_warm)) return;
    auto& events = rex::perf::frame_events::Get();
    const bool in_game = g_in_game_sequence.load(std::memory_order_relaxed);
    const bool light_frame = frame_ms <= 20.0;  // no extra work in a GPU-bound (or otherwise slow) frame
    if (warm_window_start_ == std::chrono::steady_clock::time_point{}) {
      warm_window_start_ = now;
      warm_busy_start_ = events.gpu_busy_us_total.load(std::memory_order_relaxed);
    }
    const double wall_us = std::chrono::duration<double, std::micro>(now - warm_window_start_).count();
    if (wall_us >= 100000.0) {
      const uint64_t busy = events.gpu_busy_us_total.load(std::memory_order_relaxed);
      const double share = double(busy - warm_busy_start_) / wall_us;
      warm_window_start_ = now;
      warm_busy_start_ = busy;
      const double want = REXCVAR_GET(wwe2k14_gpu_keep_warm_target_ms) / (1000.0 / 60.0);
      const double most = double(REXCVAR_GET(wwe2k14_gpu_keep_warm_mb));
      if (!in_game) {
        warm_mb_ = 0.0;
      } else if (share < want * 0.9 && light_frame) {
        warm_mb_ = std::min(most, warm_mb_ < 16.0 ? 16.0 : warm_mb_ * 1.25);
      } else if (share > want * 1.1) {
        warm_mb_ *= 0.8;
        if (warm_mb_ < 8.0) warm_mb_ = 0.0;
      }
      const bool on = warm_mb_ > 0.0;
      if (on != warm_on_ && ++warm_lines_ <= 200) {
        REXLOG_INFO("[wwe2k14][warm] keep-warm {} at frame {} (GPU busy {:.0f}% of the last 0.1 s, aiming at {:.0f}%)",
                    on ? "on" : "off", g_guest_frame.load(std::memory_order_relaxed), share * 100.0, want * 100.0);
      }
      warm_on_ = on;
    }
    // At most once per 1/60 s of wall time: fast-forwarded loads run hundreds of frames a second.
    if (!warm_on_ || !in_game || !light_frame || now - last_warm_ < std::chrono::microseconds(15000)) return;
    last_warm_ = now;
    events.warm_kb.store(uint32_t(warm_mb_ * 1024.0), std::memory_order_relaxed);
  }

  // b77: the GPU's clock (MHz) and busy share (%), where the phone lets an app read them (Qualcomm kgsl, devfreq,
  // some vendors' /sys/kernel/gpu); -1 = unknown. The files are found once (the first call logs which, or why none).
  int GpuClockMhz() {
#if defined(_WIN32)
    return -1;
#else
    FindGpuFiles();
    const long long v = ReadNumber(gpu_clock_fd_);
    if (v <= 0) return -1;
    return int(v >= 1000000 ? v / 1000000 : v >= 10000 ? v / 1000 : v);  // Hz, kHz or MHz
#endif
  }
  int GpuBusyPercent() {
#if defined(_WIN32)
    return -1;
#else
    FindGpuFiles();
    const long long v = ReadNumber(gpu_busy_fd_);
    return v >= 0 && v <= 100 ? int(v) : -1;
#endif
  }
#if !defined(_WIN32)
  static long long ReadNumber(int fd) {
    if (fd < 0) return -1;
    char buf[64];
    const ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return -1;
    buf[n] = 0;
    char* end = nullptr;
    const long long v = std::strtoll(buf, &end, 10);
    return end == buf ? -1 : v;
  }
  void FindGpuFiles() {
    if (gpu_files_looked_) return;
    gpu_files_looked_ = true;
    std::vector<std::string> clocks = {"/sys/class/kgsl/kgsl-3d0/gpuclk", "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
                                       "/sys/kernel/gpu/gpu_clock"};
    std::vector<std::string> busies = {"/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "/sys/kernel/gpu/gpu_busy"};
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/devfreq", ec)) {
      const std::string name = e.path().filename().string();
      if (name.find("gpu") != std::string::npos || name.find("kgsl") != std::string::npos ||
          name.find("mali") != std::string::npos || name.find("g3d") != std::string::npos) {
        clocks.push_back((e.path() / "cur_freq").string());
      }
    }
    std::string tried;
    for (const auto& p : clocks) {
      const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd >= 0 && ReadNumber(fd) > 0) {
        gpu_clock_fd_ = fd;
        gpu_clock_path_ = p;
        break;
      }
      tried += fmt::format("{}{} ({})", tried.empty() ? "" : ", ", p, fd < 0 ? std::strerror(errno) : "no number");
      if (fd >= 0) ::close(fd);
    }
    for (const auto& p : busies) {
      const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd >= 0 && ReadNumber(fd) >= 0) {
        gpu_busy_fd_ = fd;
        break;
      }
      if (fd >= 0) ::close(fd);
    }
    if (gpu_clock_fd_ >= 0) {
      REXLOG_INFO("[wwe2k14][frames] GPU clock from {} ({} MHz now){}", gpu_clock_path_, GpuClockMhz(),
                  gpu_busy_fd_ >= 0 ? ", busy share readable" : "");
    } else {
      REXLOG_INFO("[wwe2k14][frames] the GPU clock is not readable on this device: {}",
                  tried.empty() ? std::string("no candidate files") : tried);
    }
  }
#endif

  void LogSummary(uint64_t frame, double t, double window_s) {
    if (window_ms_.empty()) return;
    std::vector<double> sorted = window_ms_;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : sorted) sum += v;
    const double avg = sum / sorted.size();
    const double p95 = sorted[std::min(sorted.size() - 1, size_t(sorted.size() * 0.95))];
    REXLOG_INFO("[wwe2k14][frames] t={:.0f} s, frames {}-{}: {:.1f} fps, avg {:.1f} ms, p95 {:.1f} ms, "
                "worst {:.0f} ms, hitches {}",
                t, window_first_frame_, frame, sorted.size() / window_s, avg, p95, sorted.back(),
                window_hitches_);
    REXLOG_INFO("[wwe2k14][frames] on screen: {} of {} frames reached a present ({:.0f}%), {:.1f} a second", window_presented_,
                sorted.size(), 100.0 * window_presented_ / sorted.size(), window_presented_ / window_s);
  }

  void OpenCsv() {
    if (!REXCVAR_GET(wwe2k14_frame_csv)) return;
    const int newest = CurrentLogNumber();
    if (newest < 0) return;
    char name[32];
    std::snprintf(name, sizeof(name), "frames_%03d.csv", newest);
    csv_path_ = (std::filesystem::current_path() / "logs" / name).string();
#ifdef _WIN32
    csv_ = _fsopen(csv_path_.c_str(), "w", _SH_DENYNO);  // shared: readable while the game runs
#else
    csv_ = std::fopen(csv_path_.c_str(), "w");
#endif
    if (csv_) {
      split_csv_ = REXCVAR_GET(wwe2k14_frame_split);  // S102 H3: the columns are decided here, once, so every row has as many cells as the header
      frame_split::g_on.store(split_csv_, std::memory_order_relaxed);  // F6: the hooks' switch is set here and nowhere else: a mid-run cvar flip changes neither
      std::fputs("frame,t_seconds,frame_ms,game_cpu_ms,gpu_thread_cpu_ms,gpu_ms,draw_rows,gpu_mhz,textures,texture_mb,"
                 "warm_mb,gpu_wait_ms,gpu_wait_main,scene,presented,present_gap_ms,rdr_submit_ms,gpu_work_ms,render_thread_ms,acquire_ms,present_call_ms,record_thread_ms,"
                 "device_lost,present_gap_max_ms",  // S102 R25-C2: present_gap_max_ms after device_lost (the largest present gap among the row's presents). S102 D1: 1 from the first frame the renderer saw its Vulkan device lost (before the fs_ split columns, which must stay last)
                 // S102 R21-B: record_thread_ms before the fs_ columns (they stay last)
                 csv_);
      if (split_csv_) {  // S102 H3: appended AFTER every existing column (the parsers read by name and refuse a split column in front)
        for (int i = 0; i < frame_split::kColumns; ++i) std::fprintf(csv_, ",%s", frame_split::kColumnNames[i]);
        REXLOG_INFO("[wwe2k14][frame_split] on: {} columns after present_call_ms (src/frame_split_math.h)", frame_split::kColumns);
      }
      std::fputc('\n', csv_);
    }
  }

  bool split_backwards_logged_ = false;
  bool device_lost_seen_ = false;  // S102 D1
  // S102 D1: the player's message on a lost device. A box on its own thread (the game's threads keep running: sound, the log); nothing on a phone (the log line).
  // S105 A9 (docs/research/s105/A9.md): called once at the first frame after the loss. The switch wwe2k14_rdr_device_lost_message (default on) comes from rdr_sw, which exists in both builds.
  // PC: a box on its own thread (the render thread never waits on it, the game's threads run on). Phone: a black-box line + an exit request; the launcher's LastRun shows the toast.
  static void TellPlayerDeviceLost(uint64_t frame) {
    const bool on = device_lost_message::ShouldShow(rdr_sw::Bool(rdr_sw::kDeviceLostMessage));
#if defined(__ANDROID__)
    REXLOG_ERROR("{}", device_lost_message::LogLine(on, true));
    if (on) {
      blackbox::Note("%s", device_lost_message::BlackboxLine(frame).c_str());
      rex::FlushLogging();  // (review F4) the process is killed ~400 ms after the request: the [rdr][DEVICE_LOST] lines go out first (hang_guard.h does the same)
      android_lifecycle::WriteExitRequest("the graphics driver stopped");
    }
#else
    (void)frame;
    REXLOG_ERROR("{}", device_lost_message::LogLine(on, false));
#if defined(_WIN32)
    if (on) {
      std::thread([] { wwe2k14::NameThread(L"device_lost_box");
        const std::string text = device_lost_message::kTextPc;  // ASCII: widened one to one
        MessageBoxW(nullptr, std::wstring(text.begin(), text.end()).c_str(), L"WWE 2K14", MB_ICONERROR | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
      }).detach();
    }
#endif
#endif
  }
  std::mutex mutex_;
  std::chrono::steady_clock::time_point start_, last_, window_start_;
  uint64_t window_first_frame_ = 0;
  std::vector<double> window_ms_;
  uint32_t window_hitches_ = 0;
  uint32_t hitches_logged_ = 0;
  bool other_swapper_logged_ = false;
  // Session 73
  uint64_t last_cpu_ns_ = 0, last_gpu_thread_ns_ = 0, last_gpu_frames_done_ = 0, last_gpu_wait_us_ = 0, last_owner_gpu_frames_ = 0, last_owner_gpu_ns_ = 0;  // lane 269
  uint32_t last_pipelines_ = 0, last_images_ = 0, last_shaders_ = 0, last_files_ = 0, last_probes_ = 0;
  uint32_t slow_logged_ = 0;
  std::string last_scene_;
  double last_scene_t_ = 0.0;
  int gpu_thread_fd_ = -1;
  uint64_t gpu_thread_retry_frame_ = 0;
  // b77
  double last_gpu_ms_ = -1.0;
  uint32_t last_textures_ = 0;
  uint64_t last_texture_kb_ = 0, last_warm_kb_ = 0, thermal_warm_kb_ = 0;
  uint64_t last_presents_ = 0, window_presented_ = 0;  // Session 95
  bool warm_on_ = false;
  double warm_mb_ = 0.0;
  std::chrono::steady_clock::time_point warm_window_start_{};
  uint64_t warm_busy_start_ = 0, thermal_busy_us_ = 0;
  std::chrono::steady_clock::time_point thermal_busy_time_{};
  uint64_t last_wait_us_by_[rex::perf::frame_events::kWaitSiteCount] = {};
  uint32_t warm_lines_ = 0;
  std::chrono::steady_clock::time_point last_warm_{};
  bool gpu_files_looked_ = false;
  int gpu_clock_fd_ = -1, gpu_busy_fd_ = -1;
  std::string gpu_clock_path_;
  frame_split::Splitter split_;  // S102 H3
  bool split_csv_ = false;
  uint64_t last_submit_calls_ = 0, last_submit_ns_ = 0, last_gpu_work_ns_ = 0, last_render_thread_ns_ = 0, last_acquire_ns_ = 0, last_present_call_ns_ = 0, last_record_thread_ns_ = 0;  // S102 S0-3; R21-B: last_record_thread_ns_
  std::FILE* csv_ = nullptr;
  std::string csv_path_;
};

inline FrameStats g_frame_stats;

}  // namespace wwe2k14
