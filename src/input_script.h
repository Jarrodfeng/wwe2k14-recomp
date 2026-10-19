// wwe2k14 - controller record / scripted replay, screenshots, and scripted quit (test harness).
//
// Record:  --wwe2k14_input_record=<file>  (Session 30: plus "# frame F state NAME = V" lines when a route state
//           changes, for tools/input_scripts/recording_to_route.py) writes every change of controller 1's state as a playable
//          script (`wait_frames` + `set` lines, frames = guest frames from VdSwap), with a comment for
//          each file the game opens, so sync points can be added by hand afterwards.
// Replay:  --wwe2k14_input_script=<file>  drives controller 1 from a script instead of the real pad.
//          When the script ends, the real pad takes over again. Besides the pad state
//          (XamInputGetState), scripted presses also produce key-down/key-up events for
//          XamInputGetKeystrokeEx, which the game's menus and "Press Start" read.
//
// Script commands (one per line, '#' starts a comment, frames are guest frames, ~60 per second):
//   wait_frames N                 do nothing for N frames
//   wait_file TEXT [TIMEOUT] [optional]
//                                 wait until the game opens a file whose path contains TEXT
//                                 (case-insensitive; counts opens since the last input or the last
//                                 wait_file, so a load triggered by the press before it is seen;
//                                 default timeout 1800). With `optional` (after TIMEOUT; Session 10) a timeout
//                                 is logged as a warning and the script goes on; the sync point stays at the
//                                 last file seen, so the next wait_file also counts files opened meanwhile
//   press TARGETS [HOLD] [GAP]    hold for HOLD frames (6), release for GAP frames (12); TARGETS are
//                                 BUTTONS plus LT, RT (full pull) and L_UP/L_DOWN/L_LEFT/L_RIGHT (left stick)
//   press_until_file TARGETS TEXT [TRIES] [INTERVAL] [HOLD]
//                                 press TARGETS (as `press`, held HOLD frames, default 5), then wait up to INTERVAL
//                                 frames (default 90) for a file containing TEXT to open; if none did, press again,
//                                 up to TRIES presses (default 3); then a script failure. For menu presses the game
//                                 sometimes drops (Session 7b: the arena pick when the random pre-selected arena is
//                                 already the target)
//   press_until_state TARGETS NAME COND [TRIES] [INTERVAL] [HOLD] [unreadable]
//                                 (Session 15 extension, E3) as press_until_file, but done when the named state
//                                 (states.toml or an inline 0x chain, as wait_state) meets COND (=, !=, <, <=, >,
//                                 >= VALUE) after the press is released; with 'unreadable', an unreadable state
//                                 counts as met too. Not met within INTERVAL frames (default 30): press again, up to
//                                 TRIES (default 10); then a script failure. For "the game accepted the press" signals
//                                 that come much sooner than a file open (an entrance skip: the step word resets).
//                                 Session 30: with 'if_needed' (after the numbers, like 'unreadable') nothing is
//                                 pressed when the state already meets COND (e.g. a menu cursor the game remembers)
//   set BUTTONS LT RT LX LY RX RY set the full pad state until the next set/press (what recordings use);
//                                 settings are changed with `cvar`, not `set`
//   screenshot LABEL              save the current guest output to captures/NNN_LABEL.png
//   screenshot_every N [LABEL]    (Session 10) from now on, a screenshot every N frames in the background while
//                                 the script runs, captures/NNN_LABEL_01.png, _02, ...; `screenshot_every 0` stops
//                                 it. Runs alongside the other steps (e.g. a long wait_state for a match's end)
//   clip_start NAME [FPS] [SCALE] / clip_stop [NAME]
//                                 record the guest output (internal resolution) to
//                                 <--wwe2k14_clip_dir>/NNN_clip_NAME.mp4 (NNN = run log number; FPS default 60,
//                                 SCALE 1/0.5/0.333/0.25, checked at load; one clip_start per NAME); ignored
//                                 (logged) while that setting is empty; any script end stops the clip and a quit
//                                 (also on_fail quit) waits for clips to finish (src/clip_capture.h)
//   log TEXT                      write TEXT to the log
//   mark ID [TEXT]                log a numbered marker (ID decimal) and record timeline event "mark"
//                                 (a = ID, b = guest frame) so analysis tools can cut runs at it
//   cvar NAME VALUE               set a ReXGlue/app setting now (rex::cvar::SetFlagByName), e.g. to toggle a
//                                 change in-run for an A/B. VALUE is the rest of the line ("" = empty string).
//                                 Logged, and on success recorded as timeline event "cvar" (a = FNV-1a 32 of
//                                 NAME, b = VALUE as a number: true/false -> 1/0, decimal or 0x-hex integers,
//                                 anything else 0xFFFFFFFF; c = guest frame). An unknown setting or a rejected
//                                 value is a script failure (on_fail)
//   fail_if_file TEXT             arm a forbidden file: if the game opens a file whose path contains TEXT
//                                 (case-insensitive) while it's armed, the script fails (on_fail). Up to 8
//                                 armed at once; they stay armed until clear_fail_if or the script ends
//   clear_fail_if [TEXT]          disarm that pattern, or all of them
//   repeat N                      run the lines up to the matching end_repeat N times (expanded at load,
//   ...                           each pass logs "repeat i/N"; no nesting). E.g. an in-run A/B:
//   end_repeat                    repeat 20 / cvar X true / wait_frames 120 / cvar X false / wait_frames 120 /
//                                 end_repeat
//   log_text [N]                  Session 30: log the text on screen now (the strings the game fetched through its
//                                 text manager: src/screen_text.h), and with N the N newest distinct strings
//   wait_text TEXT [TIMEOUT] [optional]
//                                 wait (default 1800 frames) until a string containing TEXT (any case) is on screen;
//                                 then as wait_state (failure screenshot text_fail_l<line>, or `optional`). TEXT may
//                                 hold spaces ("ONE ON ONE"); quote it when it ends in a number
//   assert_text TEXT              fail now (screenshot) unless a string containing TEXT is on screen
//   plugin load [PATH] | reload [PATH] | unload
//                                 Session 30 (coverage build only): hot-reload probe plugins (src/probe_plugin.h,
//                                 probes/probe_api.h; default PATH = wwe2k14_probes.dll beside the exe)
//   dump_struct NAME [ADDR]       Session 30: log every field of a known object (tools/input_scripts/structs.toml:
//                                 SmartMenu, MessageManager, SequenceManager, LogoEditor, ...) at its base chain, or
//                                 at ADDR (hex); checked reads (src/structs.h)
//   include FILE                  Session 30: the lines of FILE in place (expanded at load). FILE is looked up beside
//                                 this script, then in tools/input_scripts/nav/ and tools/input_scripts/ (walking up
//                                 from the script; ".txt" may be left out), so `include boot_to_menu` takes the
//                                 navigation snippet nav/boot_to_menu.txt. Up to 8 levels deep, no loops; a repeat
//                                 can't start in one file and end in another. Its steps run under the include line's
//                                 number; messages add their own file and line: "line 5 (boot_to_menu.txt:12)".
//   watch_zero 0xADDRESS          hardware-watch a guest word; log every write that zeroes it
//                                 (src/guest_watch.h; up to 4 words)
//   watch ADDR [LEN] [any|change|VALUE] / unwatch [ADDR]
//                                 hardware-watch guest bytes (LEN 1/2/4/8, default 4): log each write (rate-limited)
//                                 with the guest frame, writing function and call stacks (src/guest_watch.h)
//   snap ADDR:LEN [NAME] / unsnap [NAME]
//                                 record guest bytes at every VdSwap to out/harness/watch/<run>/ (src/guest_watch.h;
//                                 report: tools/watch/snap_report.py)
//   wait_state NAME VALUE [TIMEOUT] [HOLD] [optional]
//                                 wait until guest state NAME compares true with VALUE on HOLD consecutive frames
//                                 (default 2: reads aren't synchronised with the guest), up to TIMEOUT frames
//                                 (default 1800); then a script failure (on_fail) with a screenshot, or with
//                                 `optional` (after TIMEOUT and HOLD; Session 10) a warning, and the script goes on
//                                 captures/NNN_state_fail_l<line>.png. NAME is an entry of states.toml (next to
//                                 the script, else tools/input_scripts/states.toml above it; format:
//                                 src/game_state.h) or an inline chain 0xADDR[>OFF...]. VALUE: N, !=N (or !N),
//                                 >N, <N, >=N, <=N; N decimal (may be negative) or 0x hex. An unreadable state
//                                 (null pointer) never matches. Doesn't move wait_file's sync point. Takes HOLD-1
//                                 frames even when the value is already there (HOLD 1: no frames), so later
//                                 inputs move by that much. Logs the first value, every change in whether it
//                                 matches, and other value changes at most every 30 frames
//   assert_state NAME VALUE       check NAME against VALUE now (one read, no frames); a mismatch or an unreadable
//                                 state is a script failure (on_fail) with the same screenshot as wait_state
//   log_state NAME                log NAME's current value (never fails)
//   on_fail quit|continue         what a failure (wait_file / wait_state timeout, forbidden file, cvar error,
//                                 assert_state mismatch, watch/snap refused at run time) does: quit the game (default; after a failure screenshot
//                                 the quit waits until it is written), or end the script and leave the game
//                                 running
//   quit                          close the game like Alt+F4
//   scan_start [1|2|4] / scan_eq V / scan_ne V / scan_gt V / scan_lt V / scan_changed / scan_unchanged /
//   scan_inc [N] / scan_dec [N] / scan_show [N] / scan_end
//                                 (Session 29, src/mem_scan.h) Cheat-Engine-style memory scanner: narrow the writable
//                                 guest memory down to the variable that behaves as expected; runs on this thread
//   check TEXT                    (Session 29, src/check_prompt.h) show TEXT on screen and wait for Jarrod's answer key
//                                 (F1 pass, F2 fail, F5 skip; --wwe2k14_check_timeout_s, default 300 s wall clock);
//                                 logs "[wwe2k14][check] PASS|FAIL|SKIP|TIMEOUT line N ...: TEXT" and goes on
// BUTTONS: names joined with '+' (A+B), or a hex mask, or 'none'. Names: A B X Y START BACK LB RB LS RS
// UP DOWN LEFT RIGHT GUIDE.
//
// Session 29: live batches (src/live_control.h, --wwe2k14_live=NAME): the same commands arrive over a named pipe while
// the game runs; each batch runs as its own little script once no other script runs (StartLive), with on_fail
// continue unless the batch says otherwise. A clip or screenshot_every ends with its batch.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/perf/timeline.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

#include "frame_stats.h"
#include "guest_watch.h"
#include "game_state.h"
#include "screen_text.h"  // Session 30: log_text / wait_text / assert_text
#include "structs.h"      // Session 30: dump_struct  // Session 8 task 9a: wait_state / assert_state / log_state
#include "coverage.h"  // Session 8 task 6: no-op macros unless built with the coverage preset
#include "flight_recorder.h"  // Session 8 task 11: script steps into the flight recorder (default off)
#include "clip_capture.h"  // Session 8 phase 2 task t1: clip_start / clip_stop (--wwe2k14_clip_dir, default off)
#include "replay.h"  // Session 16 item 1: --wwe2k14_replay_mode / _record / _play (default off)
#include "check_prompt.h"  // Session 29: `check TEXT` (on-screen question, F1 / F2 / F5)
#include "mem_scan.h"      // Session 29: scan_* (live memory scanner)

namespace rex::kernel::xam {
// ReXGlue's implementations (exported by rexruntime.dll, not declared in public headers).
uint32_t XamInputGetState_entry(uint32_t user_index, uint32_t flags,
                                ppc_ptr_t<rex::input::X_INPUT_STATE> input_state);
uint32_t XamInputGetCapabilities_entry(uint32_t user_index, uint32_t flags,
                                       ppc_ptr_t<rex::input::X_INPUT_CAPABILITIES> caps);
uint32_t XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, uint32_t flags,
                                      ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke);
}  // namespace rex::kernel::xam

REXCVAR_DEFINE_STRING(wwe2k14_screenshot_format, "png", "wwe2k14",
                      "Screenshot files: png (default since Session 17, Jarrod: lossless, ~4x smaller) or bmp (before)");
REXCVAR_DEFINE_BOOL(wwe2k14_input_record_scripted, false, "wwe2k14",
                    "Session 30: --wwe2k14_input_record also records what an input script presses (tests of "
                    "tools/input_scripts/recording_to_route.py); default: the real controller only");
REXCVAR_DEFINE_STRING(wwe2k14_input_record, "", "wwe2k14",
                      "Record controller 1 to this script file (relative paths: the exe folder)");
REXCVAR_DEFINE_STRING(wwe2k14_input_result, "", "wwe2k14",
                      "Session 27: write one line to this file when the input script ends: ok or failed (line, the "
                      "last log step). For the launcher's scripted runs");
REXCVAR_DEFINE_STRING(wwe2k14_input_script, "", "wwe2k14",
                      "Drive controller 1 from this input script (see src/input_script.h)");

namespace wwe2k14 {

// XAM result codes (Win32 error values): success and ERROR_DEVICE_NOT_CONNECTED.
constexpr uint32_t kXamSuccess = 0;
constexpr uint32_t kXamDeviceNotConnected = 0x48F;
constexpr uint32_t kXamEmpty = 0x10D2;  // ERROR_EMPTY: no keystroke pending

// Set by the app: closes the window the same way as Alt+F4.
inline std::function<void()> g_request_quit;

struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const PadState&) const = default;
};

// Screenshots whose worker thread hasn't finished yet (a failure that quits waits for them: the close path
// hard-exits the process, which would cut a BMP write short).
inline std::atomic<int> g_screenshots_in_flight{0};

// Saves the guest output as a PNG (or a 24-bit BMP) on a worker thread (the capture waits on its own GPU copy).
// Session 16 (src/matte.h): called on the game thread at every screenshot before the capture starts (null = nothing).
inline void (*g_screenshot_hook)(const std::string& label) = nullptr;

// PNG through Windows Imaging Component (part of Windows: no extra library). RawImage is R8 G8 B8 X8 -> 24-bit BGR;
// with `alpha` (Session 18: title reveal thumbnails) R8 G8 B8 A8 -> 32-bit BGRA.
inline bool SavePngWic(const std::filesystem::path& path, const rex::ui::RawImage& image, bool alpha = false) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // this worker thread
  bool ok = false;
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* props = nullptr;
  do {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
      break;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE)))
      break;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
      break;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props))) break;
    if (FAILED(frame->SetSize(image.width, image.height))) break;
    const WICPixelFormatGUID want = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID fmt = want;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != want) break;
    const uint32_t bpp = alpha ? 4 : 3, row = image.width * bpp;
    std::vector<uint8_t> buf(size_t(row) * image.height);
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* src = image.data.data() + size_t(y) * image.stride;
      uint8_t* dst = buf.data() + size_t(y) * row;
      for (uint32_t x = 0; x < image.width; ++x) {
        dst[x * bpp + 0] = src[x * 4 + 2], dst[x * bpp + 1] = src[x * 4 + 1], dst[x * bpp + 2] = src[x * 4 + 0];
        if (alpha) dst[x * 4 + 3] = src[x * 4 + 3];
      }
    }
    if (FAILED(frame->WritePixels(image.height, row, UINT(buf.size()), buf.data()))) break;
    ok = SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
  } while (false);
  if (props) props->Release();
  if (frame) frame->Release();
  if (encoder) encoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

inline void SaveScreenshot(const std::string& label) {
  if (g_screenshot_hook) g_screenshot_hook(label);
  g_screenshots_in_flight.fetch_add(1);
  std::thread([label] {
    struct InFlight {
      ~InFlight() { g_screenshots_in_flight.fetch_sub(1); }
    } in_flight;
    auto* runtime = REX_KERNEL_STATE()->emulator();
    auto* graphics = runtime ? runtime->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
      REXLOG_WARN("[wwe2k14][input] screenshot '{}': no guest output to capture", label);
      return;
    }
    const auto dir = std::filesystem::current_path() / "captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%03d_", std::max(CurrentLogNumber(), 0));
    // Session 17 (Jarrod): PNG by default (lossless, ~4x smaller than the 14 MB 1440p BMPs). Written to NAME.png.part
    // and renamed when complete, so a file named .png is always whole. --wwe2k14_screenshot_format=bmp = the old way.
    if (REXCVAR_GET(wwe2k14_screenshot_format) != "bmp") {
      const auto path = dir / (prefix + label + ".png");
      auto part = path;
      part += ".part";
      if (SavePngWic(part, image) && (std::filesystem::rename(part, path, ec), !ec)) {
        REXLOG_INFO("[wwe2k14][input] screenshot saved: {} ({}x{})", path.string(), image.width, image.height);
      } else {
        REXLOG_WARN("[wwe2k14][input] screenshot '{}': PNG write failed ({})", label, ec ? ec.message() : "WIC");
      }
      return;
    }
    const auto path = dir / (prefix + label + ".bmp");
    std::ofstream out(path, std::ios::binary);
    const uint32_t row = (image.width * 3 + 3) & ~3u, size = 54 + row * image.height;
    auto put32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("BM", 2);
    put32(size), put32(0), put32(54), put32(40), put32(image.width), put32(image.height);
    put16(1), put16(24), put32(0), put32(row * image.height), put32(2835), put32(2835), put32(0), put32(0);
    std::vector<char> line(row, 0);
    for (uint32_t y = image.height; y-- > 0;) {  // BMP rows go bottom-up; RawImage is R8 G8 B8 X8
      const uint8_t* src = image.data.data() + size_t(y) * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) {
        line[x * 3 + 0] = char(src[x * 4 + 2]);
        line[x * 3 + 1] = char(src[x * 4 + 1]);
        line[x * 3 + 2] = char(src[x * 4 + 0]);
      }
      out.write(line.data(), row);
    }
    REXLOG_INFO("[wwe2k14][input] screenshot saved: {} ({}x{})", path.string(), image.width, image.height);
  }).detach();
}

class InputScript {
 public:
  // Called once at startup (after settings are final).
  void Init() {
    const std::string record = REXCVAR_GET(wwe2k14_input_record);
    const std::string script = REXCVAR_GET(wwe2k14_input_script);
    WWE2K14_COVERAGE_INIT(script);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!script.empty()) {
      if (!Load(script)) {
        // Session 27 (review): a program waiting on --wwe2k14_input_result must not wait on a running game.
        if (!REXCVAR_GET(wwe2k14_input_result).empty()) {
          last_log_ = "script load";
          failed_ = true;
          WriteResult();
          if (quit_on_fail_ && g_request_quit) g_request_quit();
        }
        return;
      }
      active_ = true;
      REXLOG_INFO("[wwe2k14][input] replaying script {} ({} steps); the real controller is ignored until it ends",
                  script, steps_.size());
    }
    if (!record.empty()) {
      record_file_ = _fsopen(record.c_str(), "w", _SH_DENYNO);  // shared: readable while recording
      REXLOG_INFO("[wwe2k14][input] {} controller 1 to {}", record_file_ ? "recording" : "FAILED to open",
                  record);
      if (record_file_) std::fputs("# recorded by wwe2k14 (frames = guest frames)\n", record_file_);
    }
  }

  // Once per guest frame (VdSwap).
  void OnFrame(uint64_t frame) {
    GuestWatchOnFrame(frame);  // watch hits / snap ranges (no-op unless a script armed one)
    screen_text::OnFrame(frame);  // Session 30 (no-op outside harness runs)
    std::unique_lock<std::mutex> lock(mutex_);
    frame_ = frame;
    while (active_) {
      Advance();
      if (!cvar_pending_) break;
      // Advance stopped at a `cvar` step. Apply it without holding mutex_ (a setting's change callbacks can take
      // a while, and the game's input calls shouldn't wait on them), then go on with the next step this frame.
      const Step step = steps_[index_];
      lock.unlock();
      const bool applied = ApplyCvar(step, frame);
      lock.lock();
      cvar_pending_ = false;
      if (!applied) {
        REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: cvar {} was not set", L(step), step.text);
        FailAndFinish();
        break;
      }
      ++index_;
      step_started_ = false;
    }
    // Session 10: screenshot_every (periodic_every_ is 0 unless a script turned it on; Finish turns it off).
    if (active_ && periodic_every_ && frame_ >= periodic_next_) {
      ++periodic_count_;
      const std::string label = fmt::format("{}_{:02d}", periodic_label_, periodic_count_);
      REXLOG_INFO("[wwe2k14][input] periodic screenshot '{}' (frame {})", label, frame_);
      SaveScreenshot(label);
      periodic_next_ = frame_ + periodic_every_;
    }
    ClipOnFrame(frame);  // in-game clip trigger (one atomic load unless a clip_start is running)
    // A failure that saved a screenshot quits once the screenshot is written (at least kFailQuitMinFrames later),
    // or after kFailQuitMaxFrames if the capture never finishes.
    // A quit waiting for in-game clips (Finish) uses the same path; g_clips_in_flight is always 0 with clips off.
    if (fail_quit_pending_) {
      const uint64_t waited = frame_ - fail_quit_frame_;
      if ((waited >= kFailQuitMinFrames && g_screenshots_in_flight.load() == 0 && g_clips_in_flight.load() == 0) ||
          waited >= fail_quit_max_) {
        fail_quit_pending_ = false;
        REXLOG_INFO("[wwe2k14][input] quitting after the failure or quit ({} frames later; screenshots in flight: "
                    "{}, clips in flight: {})",
                    waited, g_screenshots_in_flight.load(), g_clips_in_flight.load());
        if (g_request_quit) g_request_quit();
      }
    }
    if (record_file_) RecordStates();
    if (record_file_ && !(frame % 300)) std::fflush(record_file_);
  }

  // Session 30 (#1): while recording, a "# frame F state NAME = V" line whenever one of the route states changes, so
  // tools/input_scripts/recording_to_route.py can turn each press into a press_until_state on what it changed.
  // Only readable values are written (a state outside its own screen may read unrelated memory: the converter only
  // looks at changes just after a press). Under mutex_ (OnFrame).
  void RecordStates() {
    if (!record_states_loaded_) {
      record_states_loaded_ = true;
      const std::string file = FindStatesFile((std::filesystem::current_path() / "record").string());
      std::vector<GuestState> all;
      std::string error;
      if (!file.empty() && LoadGuestStates(file, all, error)) {
        static constexpr const char* kNames[] = {"boot_step", "seq", "menu", "cursor", "target", "logo_screen",
                                                 "logo_phase", "logo_grid_cursor", "logo_size_cursor",
                                                 "logo_tool_cursor", "logo_category_cursor"};
        for (GuestState& g : all) {
          if (std::find_if(std::begin(kNames), std::end(kNames), [&](const char* n) { return g.name == n; }) !=
              std::end(kNames)) {
            record_states_.push_back(std::move(g));
          }
        }
      }
      record_values_.assign(record_states_.size(), UINT64_MAX);
      std::fprintf(record_file_, "# states recorded on change: %zu from %s\n", record_states_.size(),
                   file.empty() ? "(no states.toml)" : file.c_str());
    }
    for (size_t i = 0; i < record_states_.size(); ++i) {
      uint32_t raw = 0;
      if (!ReadGuestState(record_states_[i], &raw)) continue;
      if (record_values_[i] == raw) continue;
      record_values_[i] = raw;
      std::fprintf(record_file_, "# frame %llu state %s = %u\n", static_cast<unsigned long long>(frame_),
                   record_states_[i].name.c_str(), raw);
    }
  }

  void OnFileOpened(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
      std::string lower = path;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
      if (!forbidden_hit_) {
        auto it = std::find_if(forbidden_.begin(), forbidden_.end(),
                               [&](const std::string& pattern) { return lower.find(pattern) != std::string::npos; });
        if (it != forbidden_.end()) {
          REXLOG_ERROR("[wwe2k14][input] script FAILED: forbidden file {} (pattern {}) at frame {}", path, *it,
                       frame_);
          forbidden_hit_ = true;  // on_fail is applied on the script's own thread at the next frame (Advance)
        }
      }
      opens_.emplace_back(frame_, std::move(lower));
      if (opens_.size() > 512) opens_.erase(opens_.begin(), opens_.begin() + 256);
    }
    if (record_file_) {
      FlushRecordWait();
      std::fprintf(record_file_, "# frame %llu: opened %s\n", static_cast<unsigned long long>(frame_),
                   path.c_str());
      std::fflush(record_file_);  // recordings often end in a crash
    }
  }

  // First call per (function, user index, flags), with our answer: shows how the game polls input.
  void TraceCall(const char* function, uint32_t user, uint32_t flags, bool null_ptr, bool scripted, uint32_t result) {
    const std::string key = fmt::format("{}/{:X}/{:X}/{}", function, user, flags, null_ptr);
    std::lock_guard<std::mutex> lock(trace_mutex_);
    if (std::find(traced_.begin(), traced_.end(), key) != traced_.end() || traced_.size() > 64) return;
    traced_.push_back(key);
    REXLOG_INFO("[wwe2k14][input] first {}(user=0x{:X}, flags=0x{:X}{}) -> 0x{:X}{}", function, user, flags,
                null_ptr ? ", null ptr" : "", result, scripted ? " (scripted)" : " (real)");
  }

  uint32_t GetState(uint32_t user_index, uint32_t flags, ppc_ptr_t<rex::input::X_INPUT_STATE> state) {
    const bool gamepad_query = !(flags & 0xFF) || (flags & 0x01);
    const bool user1 = (user_index & 0xFF) == 0 || (user_index & 0xFF) == 0xFF || (flags & (1u << 30));
    bool scripted = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_ && user1 && gamepad_query) {
        if (state) {
          state->packet_number = packet_;
          state->gamepad.buttons = pad_.buttons;
          state->gamepad.left_trigger = pad_.lt;
          state->gamepad.right_trigger = pad_.rt;
          state->gamepad.thumb_lx = pad_.lx;
          state->gamepad.thumb_ly = pad_.ly;
          state->gamepad.thumb_rx = pad_.rx;
          state->gamepad.thumb_ry = pad_.ry;
        }
        scripted = true;
      }
    }
    if (scripted) {
      TraceCall("XamInputGetState", user_index, flags, !state, true, kXamSuccess);
      NoteButtonsRead(state ? uint32_t(uint16_t(state->gamepad.buttons)) : 0, user1 && gamepad_query && state, true);
      if (state && user1 && gamepad_query && REXCVAR_GET(wwe2k14_input_record_scripted)) {  // Session 30 (tests)
        std::lock_guard<std::mutex> lock(mutex_);
        if (record_file_) {
          const auto& g = state->gamepad;
          Record({uint16_t(g.buttons), g.left_trigger, g.right_trigger, int16_t(g.thumb_lx), int16_t(g.thumb_ly),
                  int16_t(g.thumb_rx), int16_t(g.thumb_ry)});
        }
      }
      return kXamSuccess;
    }
    const uint32_t result = rex::kernel::xam::XamInputGetState_entry(user_index, flags, state);
    TraceCall("XamInputGetState", user_index, flags, !state, false, result);
    NoteButtonsRead(state ? uint32_t(uint16_t(state->gamepad.buttons)) : 0,
                    result == kXamSuccess && user1 && gamepad_query && state, false);
    if (result == kXamSuccess && state && user1 && gamepad_query) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (record_file_) {
        const auto& g = state->gamepad;
        Record({uint16_t(g.buttons), g.left_trigger, g.right_trigger, int16_t(g.thumb_lx), int16_t(g.thumb_ly),
                int16_t(g.thumb_rx), int16_t(g.thumb_ry)});
      }
    }
    return result;
  }

  // Session 19 item 9 (latency): timeline event "inread" (a = buttons, b = guest frame, c = 1 scripted / 0 real)
  // the first time a controller read of user 1 returns different buttons than the previous read: the moment the
  // game sees a press. Only with --wwe2k14_timeline (Record is a no-op otherwise).
  void NoteButtonsRead(uint32_t buttons, bool valid, bool scripted) {
    if (!valid) return;
    if (last_read_buttons_.exchange(buttons, std::memory_order_relaxed) != buttons) {
      rex::perf::timeline::Record("inread", buttons, uint32_t(g_guest_frame.load(std::memory_order_relaxed)),
                                  scripted ? 1u : 0u);
    }
  }
  std::atomic<uint32_t> last_read_buttons_{0};

  // While a script runs, report a standard pad on user 1 even if no real controller is connected.
  uint32_t GetCapabilities(uint32_t user_index, uint32_t flags, ppc_ptr_t<rex::input::X_INPUT_CAPABILITIES> caps) {
    const uint32_t result = rex::kernel::xam::XamInputGetCapabilities_entry(user_index, flags, caps);
    TraceCall("XamInputGetCapabilities", user_index, flags, !caps, false, result);
    const bool user1 = (user_index & 0xFF) == 0 || (user_index & 0xFF) == 0xFF || (flags & (1u << 30));
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && user1 && caps && result == kXamDeviceNotConnected) {
      caps->type = 1;      // XINPUT_DEVTYPE_GAMEPAD
      caps->sub_type = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
      caps->flags = 0;
      caps->gamepad.buttons = 0xF7FF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = int16_t(0xFFC0);
      caps->gamepad.thumb_ly = int16_t(0xFFC0);
      caps->gamepad.thumb_rx = int16_t(0xFFC0);
      caps->gamepad.thumb_ry = int16_t(0xFFC0);
      caps->vibration.left_motor_speed = 0xFFFF;
      caps->vibration.right_motor_speed = 0xFFFF;
      return kXamSuccess;
    }
    return result;
  }

  uint32_t GetKeystroke(mapped_u32 user_index_ptr, uint32_t flags, ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke) {
    const bool gamepad_query = !(flags & 0xFF) || (flags & 0x01);
    const uint32_t user = user_index_ptr ? uint32_t(*user_index_ptr) : 0;
    const bool user1 = (user & 0xFF) == 0 || (user & 0xFF) == 0xFF || (flags & (1u << 30));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_ && user1 && gamepad_query) {
        TraceCall("XamInputGetKeystrokeEx", user, flags, !keystroke, true, 0);
        if (!keystroke) return kXamEmpty;
        uint16_t virtual_key = 0, key_flags = 0;
        if (!NextKeystroke(virtual_key, key_flags)) return kXamEmpty;
        keystroke->virtual_key = virtual_key;
        keystroke->unicode = uint16_t(0);
        keystroke->flags = key_flags;
        keystroke->user_index = 0;
        keystroke->hid_code = 0;
        if (user_index_ptr) *user_index_ptr = 0u;
        return kXamSuccess;
      }
    }
    const uint32_t result = rex::kernel::xam::XamInputGetKeystrokeEx_entry(user_index_ptr, flags, keystroke);
    TraceCall("XamInputGetKeystrokeEx", user, flags, !keystroke, false, result);
    return result;
  }

  // ---- Session 29: live batches (src/live_control.h) ----
  struct LiveResult {
    bool running = false, failed = false;
    uint64_t start_frame = 0, end_frame = 0, frame = 0;
    int line = 0;  // script line of the current step while running
  };

  // Parses `text` (input-script lines) and runs it as the current script. Only when no script runs; returns the batch
  // number (> 0), or 0 with `error` set (busy, or the parse error).
  uint64_t StartLive(const std::string& text, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
      error = fmt::format("busy: a script is running (line {})", index_ < steps_.size() ? steps_[index_].line : 0);
      return 0;
    }
    std::vector<Step> previous;
    previous.swap(steps_);
    std::istringstream in(text);
    if (!Parse(in, "live_batch")) {
      error = "parse error: " + (parse_error_.empty() ? std::string("see the log") : parse_error_);
      steps_.swap(previous);
      return 0;
    }
    index_ = 0;
    step_started_ = false;
    quit_on_fail_ = false;  // a failed wait ends the batch, not the game (a batch can still say `on_fail quit`)
    batch_failed_ = false;
    sync_mark_ = frame_;
    opens_.clear();
    pad_ = {};
    ++packet_;
    ++live_batch_;
    live_running_ = true;
    live_start_frame_ = frame_;
    live_end_frame_ = 0;
    live_failed_ = false;
    active_ = true;
    REXLOG_INFO("[wwe2k14][live] batch {}: {} steps from frame {}", live_batch_, steps_.size(), frame_);
    return live_batch_;
  }

  // State of live batch `batch` (false: not the latest batch).
  bool LiveStatus(uint64_t batch, LiveResult& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (batch != live_batch_) return false;
    out.running = live_running_;
    out.failed = live_failed_;
    out.start_frame = live_start_frame_;
    out.end_frame = live_end_frame_;
    out.frame = frame_;
    out.line = index_ < steps_.size() ? steps_[index_].line : 0;
    return true;
  }

  // Ends whatever script runs now (a live `!stop`); the batch counts as failed. False when none runs.
  bool StopLive() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return false;
    REXLOG_WARN("[wwe2k14][live] stop: script ended at line {} (frame {})",
                index_ < steps_.size() ? steps_[index_].line : 0, frame_);
    batch_failed_ = true;
    Finish(false);
    return true;
  }

  bool Busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
  }

  uint64_t Frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_;
  }

 private:
  enum class Op {
    kWaitFrames, kWaitFile, kSet, kScreenshot, kLog, kOnFail, kQuit, kWatchZero,
    kCvar, kMark, kFailIfFile, kClearFailIf, kPressUntilFile, kPressUntilState,
    kWaitState, kAssertState, kLogState,
    kWatchCmd,  // watch / unwatch / snap / unsnap (text = command, value = its arguments; src/guest_watch.h)
    kClipStart, kClipStop,  // src/clip_capture.h (text = NAME; value = "FPS SCALE")
    kScreenshotEvery,       // Session 10: frames = interval (0 = stop), text = label
    kCheck,                 // Session 29: text = the question (src/check_prompt.h)
    kScan,                  // Session 29: text = scan command, value = its arguments (src/mem_scan.h)
    kLogText, kWaitText, kAssertText,  // Session 30: screen text (src/screen_text.h); text = TEXT, frames = N / timeout
    kDumpStruct,                       // Session 30: text = struct name (structs.toml), frames = address (0 = its base)
    kPlugin                            // Session 30: text = load / reload / unload, value = PATH (src/probe_plugin.h)
  };
  struct Step {
    Op op;
    int line = 0;
    // wait_frames length / wait_file timeout / watch_zero address / mark ID / cvar value as a timeline number /
    // press_until_file interval
    uint64_t frames = 0;
    PadState pad;
    std::string text;   // file pattern (lowercase) / label / log or mark text / on_fail policy / cvar name
    std::string value;  // cvar value
    uint32_t tries = 0;  // press_until_file
    uint32_t hold = 0;   // press_until_file / wait_state frames to hold
    size_t state = 0;        // wait_state / assert_state / log_state: index into states_
    StateCompare compare;    // wait_state / assert_state
    bool optional = false;   // Session 10: wait_file / wait_state ... optional (a timeout warns and goes on)
    bool unreadable_ok = false;  // press_until_state ... unreadable
    bool if_needed = false;      // Session 30: press_until_state ... if_needed (no press when already met)
    int sub_line = 0;        // Session 30: a step from an `include`d file: its line there (`line` = the include line)
    std::string origin;      // ... and "file.txt:N" (nested: "a.txt:3 > b.txt:7"); empty for the script's own lines
  };

  // Session 30: a step's line for messages ("5", or "5 (boot_to_menu.txt:12)" for an included step) and for file
  // labels ("5", or "5_12").
  static std::string L(const Step& s) {
    return s.origin.empty() ? std::to_string(s.line) : fmt::format("{} ({})", s.line, s.origin);
  }
  static std::string Tag(const Step& s) {
    return s.sub_line ? fmt::format("{}_{}", s.line, s.sub_line) : std::to_string(s.line);
  }

  static constexpr size_t kMaxFailIfFile = 8;      // fail_if_file patterns armed at once
  static constexpr uint64_t kFailQuitMinFrames = 2;    // a failure screenshot: earliest quit (frames after the fail)
  static constexpr uint64_t kFailQuitMaxFrames = 600;  // ... latest, even if the capture thread hasn't finished
  static constexpr uint64_t kClipQuitMaxFrames = 1800;  // `quit`: most frames it waits for clips to finish
  static constexpr uint64_t kStateLogInterval = 30;    // wait_state: value changes logged at most this often
  static constexpr int64_t kMaxRepeat = 100000;    // repeat count
  static constexpr size_t kMaxSteps = 1000000;     // steps after repeat expansion

  static void ToLower(std::string& text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  }

  // The rest of the line, without surrounding whitespace.
  static std::string RestOfLine(std::stringstream& words) {
    std::string rest;
    std::getline(words >> std::ws, rest);
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
    return rest;
  }

  // FNV-1a (32-bit) of a setting name: the "cvar" timeline event's first value. Same in Python:
  //   h = 0x811C9DC5; for b in name.encode(): h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
  static constexpr uint32_t Fnv1a32(std::string_view text) {
    uint32_t hash = 0x811C9DC5u;
    for (char c : text) hash = (hash ^ static_cast<unsigned char>(c)) * 0x01000193u;
    return hash;
  }

  // A setting value as a timeline number: true/false -> 1/0, decimal (negative ones wrap) or 0x-hex integers
  // that fit in 32 bits, anything else 0xFFFFFFFF.
  static uint32_t TimelineValue(std::string value) {
    constexpr uint32_t kNotANumber = 0xFFFFFFFFu;
    ToLower(value);
    if (value == "true") return 1;
    if (value == "false") return 0;
    const bool hex = value.rfind("0x", 0) == 0;
    const char* begin = value.data() + (hex ? 2 : 0);
    const char* end = value.data() + value.size();
    if (begin == end) return kNotANumber;
    if (hex) {
      uint64_t number = 0;
      auto [ptr, ec] = std::from_chars(begin, end, number, 16);
      return ec == std::errc() && ptr == end && number <= 0xFFFFFFFFull ? uint32_t(number) : kNotANumber;
    }
    int64_t number = 0;
    auto [ptr, ec] = std::from_chars(begin, end, number, 10);
    const bool fits = number >= INT32_MIN && number <= int64_t{0xFFFFFFFF};
    return ec == std::errc() && ptr == end && fits ? uint32_t(number) : kNotANumber;
  }

  // Applies a `cvar` step (called without mutex_). False, after logging why, if there's no such setting or it
  // rejected the value.
  static bool ApplyCvar(const Step& step, uint64_t frame) {
    const std::string shown = step.value.empty() ? "\"\"" : step.value;
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(step.text);
    if (!info) {
      REXLOG_WARN("[wwe2k14][input] cvar {} = {} (frame {}) -> FAILED: no such setting", step.text, shown, frame);
      return false;
    }
    // Read now: the pointer is only valid until the next registry call.
    const bool needs_restart = info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart;
    if (!rex::cvar::SetFlagByName(step.text, step.value)) {
      REXLOG_WARN("[wwe2k14][input] cvar {} = {} (frame {}) -> FAILED: value rejected (wrong type, out of range, "
                  "or a setting that can't change after startup)", step.text, shown, frame);
      return false;
    }
    rex::perf::timeline::Record("cvar", Fnv1a32(step.text), uint32_t(step.frames), uint32_t(frame));
    if (needs_restart) {
      REXLOG_WARN("[wwe2k14][input] cvar {} = {} (frame {}) -> ok, but this setting only takes effect after a "
                  "restart", step.text, shown, frame);
    } else {
      REXLOG_INFO("[wwe2k14][input] cvar {} = {} (frame {}) -> ok", step.text, shown, frame);
    }
    return true;
  }

  static bool ParseButtons(std::string token, uint16_t& out) {
    static const std::pair<const char*, uint16_t> kNames[] = {
        {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
        {"BACK", 0x0020},  {"LS", 0x0040},   {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
        {"GUIDE", 0x0400}, {"A", 0x1000},    {"B", 0x2000},    {"X", 0x4000},     {"Y", 0x8000}};
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return std::toupper(c); });
    out = 0;
    if (token == "NONE") return true;
    if (token.rfind("0X", 0) == 0) {
      out = uint16_t(std::stoul(token, nullptr, 16));
      return true;
    }
    std::stringstream parts(token);
    for (std::string name; std::getline(parts, name, '+');) {
      auto it = std::find_if(std::begin(kNames), std::end(kNames), [&](auto& n) { return name == n.first; });
      if (it == std::end(kNames)) return false;
      out |= it->second;
    }
    return true;
  }

  // `press` targets: buttons as above, plus LT / RT (full pull) and L_UP / L_DOWN / L_LEFT / L_RIGHT
  // (left stick fully pushed), joined with '+'.
  static bool ParsePress(std::string token, PadState& pad) {
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return std::toupper(c); });
    pad = {};
    std::stringstream parts(token);
    for (std::string name; std::getline(parts, name, '+');) {
      if (name == "LT") pad.lt = 255;
      else if (name == "RT") pad.rt = 255;
      else if (name == "L_UP") pad.ly = 32767;
      else if (name == "L_DOWN") pad.ly = -32768;
      else if (name == "L_LEFT") pad.lx = -32768;
      else if (name == "L_RIGHT") pad.lx = 32767;
      else {
        uint16_t buttons = 0;
        if (!ParseButtons(name, buttons)) return false;
        pad.buttons |= buttons;
      }
    }
    return true;
  }

  // Next word as a number, or `fallback` if the line has no more words. False if the word isn't a number.
  static bool OptionalNumber(std::stringstream& words, int64_t& value, int64_t fallback) {
    std::string word;
    if (!(words >> word)) {
      value = fallback;
      return true;
    }
    char* end = nullptr;
    value = std::strtoll(word.c_str(), &end, 10);
    return end == word.c_str() + word.size();  // the whole word was a number
  }

  static std::string DescribeState(const GuestState& state, bool readable, uint32_t raw) {
    if (!readable) return "unreadable";
    return fmt::format("0x{:0{}X} ({})", raw, state.size * 2, StateNumber(state, raw));
  }

  // wait_state / assert_state / log_state NAME: an inline chain (0x...), or an entry of states.toml in the script's
  // folder (loaded once, the first time a script line names a state). Called while loading the script.
  bool ResolveState(const std::string& script_path, const std::string& name, size_t& index, std::string& why) {
    for (size_t i = 0; i < states_.size(); ++i) {
      if (states_[i].name == name) {
        index = i;
        return true;
      }
    }
    if (name.rfind("0x", 0) == 0 || name.rfind("0X", 0) == 0) {
      GuestState inline_state;
      inline_state.name = name;
      if (!ParseStateChain(name, inline_state)) {
        why = "bad inline chain";
        return false;
      }
      states_.push_back(std::move(inline_state));
      index = states_.size() - 1;
      return true;
    }
    if (!states_loaded_) {
      states_loaded_ = true;
      const std::string file = FindStatesFile(script_path);
      if (file.empty()) {
        why = "no states.toml next to the script or in a tools/input_scripts folder above it";
        return false;
      }
      std::string error;
      std::vector<GuestState> loaded;
      if (!LoadGuestStates(file, loaded, error)) {
        why = "states file: " + error;
        return false;
      }
      REXLOG_INFO("[wwe2k14][input] loaded {} named states from {}", loaded.size(), file);
      for (GuestState& s : loaded) {
        REXLOG_INFO("[wwe2k14][input]   state {} = {} (size {}{}){}{}", s.name, s.chain, s.size,
                    s.is_signed ? ", signed" : "", s.meaning.empty() ? "" : ": ", s.meaning);
        states_.push_back(std::move(s));
      }
      return ResolveState(script_path, name, index, why);
    }
    why = "no state named '" + name + "' in states.toml (or use an inline 0xADDR[>OFF...] chain)";
    return false;
  }

  bool Load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
      REXLOG_ERROR("[wwe2k14][input] can't open script {}", path);
      return false;
    }
    return Parse(in, path);
  }

  // Parses a script into steps_ (cleared on an error; parse_error_ says why). `path` names it in messages and finds
  // states.toml (a live batch passes a name in the exe folder: tools/input_scripts/states.toml above it is found).
  bool Parse(std::istream& in, const std::string& path) {
    parse_error_.clear();
    std::vector<std::string> include_stack{NormalIncludePath(path)};
    if (!ParseLines(in, path, 0, 0, {}, include_stack)) return false;
    return CheckParsedSteps(path);
  }

  // Session 30: where `include NAME` finds its file (see the header): beside the including script, then
  // tools/input_scripts/nav/ and tools/input_scripts/ walking up from it (12 levels); NAME or NAME.txt. Empty = none.
  static std::string FindIncludeFile(const std::string& from, const std::string& name) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path want(name);
    std::vector<fs::path> names{want};
    if (!want.has_extension()) names.push_back(fs::path(name + ".txt"));
    for (const fs::path& n : names) {
      if (n.is_absolute() && fs::is_regular_file(n, ec)) return n.lexically_normal().string();
    }
    fs::path dir = fs::absolute(fs::path(from), ec).parent_path();
    if (ec) dir = fs::path(from).parent_path();
    for (const fs::path& n : names) {
      if (fs::is_regular_file(dir / n, ec)) return (dir / n).lexically_normal().string();
    }
    for (int level = 0; level < 12 && !dir.empty(); ++level) {
      for (const char* sub : {"nav", ""}) {
        const fs::path base = dir / "tools" / "input_scripts" / sub;
        for (const fs::path& n : names) {
          if (fs::is_regular_file(base / n, ec)) return (base / n).lexically_normal().string();
        }
      }
      const fs::path parent = dir.parent_path();
      if (parent == dir) break;
      dir = parent;
    }
    return {};
  }

  static std::string NormalIncludePath(const std::string& path) {
    std::error_code ec;
    std::string p = std::filesystem::weakly_canonical(std::filesystem::path(path), ec).string();
    if (ec || p.empty()) p = path;
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return p;
  }

  // The lines of one file into steps_. depth 0 = the script itself; an included file's steps get line = top_line (the
  // include line in the script) and origin = `origin` + "file:N".
  bool ParseLines(std::istream& in, const std::string& path, int depth, int top_line, const std::string& origin,
                  std::vector<std::string>& include_stack) {
    static constexpr int kMaxIncludeDepth = 8;
    const std::string file_name = std::filesystem::path(path).filename().string();
    int line_number = 0;
    int repeat_line = 0;  // line of the open `repeat` (0 = none)
    int64_t repeat_count = 0;
    size_t repeat_start = 0;  // first step of the open repeat's body
    for (std::string line; std::getline(in, line);) {
      ++line_number;
      if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
      std::stringstream words(line);
      std::string cmd;
      if (!(words >> cmd)) continue;
      std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return std::tolower(c); });
      Step step{Op::kWaitFrames, depth ? top_line : line_number};
      if (depth) {
        step.sub_line = line_number;
        step.origin = (origin.empty() ? "" : origin + " > ") + fmt::format("{}:{}", file_name, line_number);
      }
      bool ok = true;
      bool emit = true;  // false: the command added its steps itself (or none)
      std::string why;   // detail for the parse error
      int64_t n = 0;
      if (cmd == "wait_frames") {
        ok = OptionalNumber(words, n, -1) && n >= 0;
        step.frames = uint64_t(n);
      } else if (cmd == "wait_file") {
        step.op = Op::kWaitFile;
        ok = bool(words >> step.text) && OptionalNumber(words, n, 1800) && n > 0;
        step.frames = uint64_t(n);
        std::transform(step.text.begin(), step.text.end(), step.text.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (std::string extra; ok && (words >> extra)) {  // Session 10: wait_file TEXT TIMEOUT optional
          ok = extra == "optional";
          step.optional = ok;
          if (!ok) why = "expected 'optional' after the timeout, got '" + extra + "'";
        }
      } else if (cmd == "press") {
        std::string buttons;
        int64_t hold = 0, gap = 0;
        ok = bool(words >> buttons) && ParsePress(buttons, step.pad) &&
             OptionalNumber(words, hold, 6) && OptionalNumber(words, gap, 12) && hold > 0 && gap >= 0;
        step.op = Op::kSet;
        steps_.push_back(step);
        steps_.push_back({Op::kWaitFrames, line_number, uint64_t(hold)});
        steps_.push_back({Op::kSet, line_number});
        step = {Op::kWaitFrames, line_number, uint64_t(gap)};
      } else if (cmd == "press_until_file") {
        std::string buttons;
        int64_t tries = 0, interval = 0, hold = 0;
        ok = bool(words >> buttons) && ParsePress(buttons, step.pad) && bool(words >> step.text) &&
             OptionalNumber(words, tries, 3) && OptionalNumber(words, interval, 90) &&
             OptionalNumber(words, hold, 5) && tries > 0 && tries <= 100 && hold > 0 && interval > hold;
        step.op = Op::kPressUntilFile;
        step.frames = uint64_t(interval);
        step.tries = uint32_t(tries);
        step.hold = uint32_t(hold);
        ToLower(step.text);
      } else if (cmd == "press_until_state") {
        std::string buttons, name, value;
        int64_t tries = 0, interval = 0, hold = 0;
        ok = bool(words >> buttons) && ParsePress(buttons, step.pad) && bool(words >> name) &&
             bool(words >> value) && ParseStateCompare(value, step.compare);
        if (ok) {
          std::vector<std::string> rest;
          for (std::string word; words >> word;) rest.push_back(word);
          // Session 30: the keywords `unreadable` and `if_needed` may follow the numbers, in either order.
          while (!rest.empty() && (rest.back() == "unreadable" || rest.back() == "if_needed")) {
            (rest.back() == "unreadable" ? step.unreadable_ok : step.if_needed) = true;
            rest.pop_back();
          }
          std::stringstream numbers;
          for (const std::string& word : rest) numbers << word << ' ';
          ok = OptionalNumber(numbers, tries, 10) && OptionalNumber(numbers, interval, 30) &&
               OptionalNumber(numbers, hold, 5) && tries > 0 && tries <= 100 && hold > 0 && interval > hold;
          if (std::string extra; ok && (numbers >> extra)) {
            ok = false;
            why = "unexpected '" + extra + "' at the end";
          }
        }
        step.op = Op::kPressUntilState;
        step.frames = uint64_t(interval);
        step.tries = uint32_t(tries);
        step.hold = uint32_t(hold);
        if (ok) ok = ResolveState(path, name, step.state, why);
      } else if (cmd == "set") {
        std::string buttons;
        int64_t lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
        ok = bool(words >> buttons) && ParseButtons(buttons, step.pad.buttons) && OptionalNumber(words, lt, 0) &&
             OptionalNumber(words, rt, 0) && OptionalNumber(words, lx, 0) && OptionalNumber(words, ly, 0) &&
             OptionalNumber(words, rx, 0) && OptionalNumber(words, ry, 0);
        step.op = Op::kSet;
        step.pad.lt = uint8_t(lt), step.pad.rt = uint8_t(rt);
        step.pad.lx = int16_t(lx), step.pad.ly = int16_t(ly), step.pad.rx = int16_t(rx), step.pad.ry = int16_t(ry);
      } else if (cmd == "screenshot") {
        step.op = Op::kScreenshot;
        ok = bool(words >> step.text);
      } else if (cmd == "screenshot_every") {  // Session 10: screenshot_every N [LABEL] (N = 0 stops)
        step.op = Op::kScreenshotEvery;
        ok = OptionalNumber(words, n, -1) && n >= 0;
        step.frames = uint64_t(n);
        if (ok && n > 0) {
          ok = bool(words >> step.text);
          if (!ok) why = "screenshot_every N LABEL";
        }
      } else if (cmd == "clip_start" || cmd == "clip_stop") {
        step.op = cmd == "clip_start" ? Op::kClipStart : Op::kClipStop;
        words >> step.text;  // clip_stop: optional
        double fps = 60, scale = 1;
        std::string extra;
        if (step.op == Op::kClipStart) {
          ok = !step.text.empty();
          if (ok && (words >> extra)) ok = std::sscanf(extra.c_str(), "%lf", &fps) == 1;
          if (ok && (words >> extra)) ok = std::sscanf(extra.c_str(), "%lf", &scale) == 1;
          // Same checks as the run-time ones (clip::ParseClipArgs), here so a bad route fails at load whatever
          // --wwe2k14_clip_dir is (not after a long boot, and only with clips on).
          if (ok && !clip::ParseClipArgs(step.text, fps, scale, nullptr, &why)) ok = false;
          if (!ok && why.empty()) why = "clip_start NAME [FPS] [SCALE]";
          step.value = fmt::format("{} {}", fps, scale);
        }
      } else if (cmd == "log") {
        step.op = Op::kLog;
        std::getline(words >> std::ws, step.text);
      } else if (scan::IsScanCommand(cmd)) {  // Session 29: scan_* (src/mem_scan.h)
        step.op = Op::kScan;
        step.text = cmd;
        step.value = RestOfLine(words);
        ok = scan::Check(cmd, step.value, why);
      } else if (cmd == "check") {  // Session 29: check TEXT (src/check_prompt.h)
        step.op = Op::kCheck;
        step.text = RestOfLine(words);
        ok = !step.text.empty();
        if (!ok) why = "check TEXT";
      } else if (cmd == "on_fail") {
        step.op = Op::kOnFail;
        ok = bool(words >> step.text) && (step.text == "quit" || step.text == "continue");
      } else if (cmd == "quit") {
        step.op = Op::kQuit;
      } else if (cmd == "watch_zero") {
        step.op = Op::kWatchZero;
        std::string address;
        ok = bool(words >> address);
        if (ok) {
          char* end = nullptr;
          step.frames = std::strtoull(address.c_str(), &end, 16);
          ok = end == address.c_str() + address.size() && step.frames && step.frames <= 0xFFFFFFFFull;
        }
      } else if (IsGuestWatchCommand(cmd)) {
        step.op = Op::kWatchCmd;
        step.text = cmd;
        step.value = RestOfLine(words);
        GuestWatchArgs parsed;
        ok = ParseGuestWatchCommand(cmd, step.value, parsed, why);
      } else if (cmd == "mark") {
        step.op = Op::kMark;
        ok = OptionalNumber(words, n, -1) && n >= 0 && n <= int64_t{0xFFFFFFFF};
        step.frames = uint64_t(n);
        step.text = RestOfLine(words);
      } else if (cmd == "cvar") {
        step.op = Op::kCvar;
        ok = bool(words >> step.text);
        step.value = RestOfLine(words);
        ok = ok && !step.value.empty();
        if (step.value == "\"\"") step.value.clear();
        step.frames = TimelineValue(step.value);
        // Settings are registered at startup (the GPU plugin's in OnPreSetup), so this is normally final. The step
        // checks again when it runs.
        if (ok && !rex::cvar::GetFlagInfo(step.text)) {
          REXLOG_WARN("[wwe2k14][input] script {} line {}: no setting named '{}' (yet); that step will fail if it "
                      "still doesn't exist when it runs", path, line_number, step.text);
        }
      } else if (cmd == "fail_if_file") {
        step.op = Op::kFailIfFile;
        ok = bool(words >> step.text);
        ToLower(step.text);
      } else if (cmd == "clear_fail_if") {
        step.op = Op::kClearFailIf;
        words >> step.text;  // optional: none = all
        ToLower(step.text);
      } else if (cmd == "wait_state" || cmd == "assert_state" || cmd == "log_state") {
        step.op = cmd == "wait_state" ? Op::kWaitState : cmd == "assert_state" ? Op::kAssertState : Op::kLogState;
        std::string name, value;
        int64_t timeout = 0, hold = 0;
        ok = bool(words >> name);
        if (ok && step.op != Op::kLogState) ok = bool(words >> value) && ParseStateCompare(value, step.compare);
        if (ok && step.op == Op::kWaitState) {
          // Session 10: a trailing 'optional' (after TIMEOUT / HOLD, which keep their defaults when left out): a
          // timeout warns and the script goes on. Taken off first so it is never read as a number.
          std::stringstream numbers;
          {
            std::vector<std::string> rest;
            for (std::string word; words >> word;) rest.push_back(word);
            if (!rest.empty() && rest.back() == "optional") {
              step.optional = true;
              rest.pop_back();
            }
            for (const std::string& word : rest) numbers << word << ' ';
          }
          ok = OptionalNumber(numbers, timeout, 1800) && OptionalNumber(numbers, hold, 2) && timeout > 0 &&
               hold > 0 && hold <= timeout;
          if (std::string extra; ok && (numbers >> extra)) {
            ok = false;
            why = "unexpected '" + extra + "' at the end";
          }
        }
        step.frames = uint64_t(timeout);
        step.hold = uint32_t(hold);
        if (std::string extra; ok && (words >> extra)) {  // e.g. 'assert_state menu 1 600' (assert doesn't wait)
          ok = false;
          why = "unexpected '" + extra + "' at the end";
        }
        if (ok) ok = ResolveState(path, name, step.state, why);
      } else if (cmd == "log_text" || cmd == "wait_text" || cmd == "assert_text") {  // Session 30 (src/screen_text.h)
        step.op = cmd == "log_text" ? Op::kLogText : cmd == "wait_text" ? Op::kWaitText : Op::kAssertText;
        std::string rest;
        std::getline(words >> std::ws, rest);
        while (!rest.empty() && std::isspace((unsigned char)rest.back())) rest.pop_back();
        if (step.op == Op::kLogText) {
          std::stringstream numbers(rest);
          ok = OptionalNumber(numbers, n, 0) && n >= 0 && n <= 200;
          step.frames = uint64_t(n);
        } else {
          // TEXT may hold spaces; wait_text takes [TIMEOUT] [optional] after it. Quotes keep a trailing number in TEXT.
          std::vector<std::string> toks;
          std::stringstream ts(rest);
          for (std::string t; ts >> t;) toks.push_back(t);
          int64_t timeout = 1800;
          if (step.op == Op::kWaitText) {
            if (!toks.empty() && toks.back() == "optional") {
              step.optional = true;
              toks.pop_back();
            }
            if (toks.size() > 1 && !toks.back().empty() &&
                std::all_of(toks.back().begin(), toks.back().end(), [](unsigned char c) { return std::isdigit(c); })) {
              timeout = std::stoll(toks.back());
              toks.pop_back();
            }
          }
          std::string text;
          for (const std::string& t : toks) text += (text.empty() ? "" : " ") + t;
          if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
          step.text = text;
          step.frames = uint64_t(timeout);
          ok = !text.empty() && timeout > 0;
          if (text.empty()) why = "needs a TEXT";
        }
      } else if (cmd == "plugin") {  // Session 30 (#4): probe plugins, coverage build (src/probe_plugin.h)
        step.op = Op::kPlugin;
        ok = bool(words >> step.text) && (step.text == "load" || step.text == "reload" || step.text == "unload");
        std::getline(words >> std::ws, step.value);
        while (!step.value.empty() && std::isspace((unsigned char)step.value.back())) step.value.pop_back();
        if (!ok) why = "plugin load [PATH] | reload [PATH] | unload";
      } else if (cmd == "dump_struct") {  // Session 30 (src/structs.h, tools/input_scripts/structs.toml)
        step.op = Op::kDumpStruct;
        std::string name, addr;
        ok = bool(words >> name);
        if (ok && (words >> addr)) {
          uint32_t a = 0;
          ok = game_state_detail::ParseUnsigned(addr.rfind("0x", 0) == 0 || addr.rfind("0X", 0) == 0 ? addr : "0x" + addr, &a) && a;
          step.frames = a;
          if (!ok) why = "ADDR must be a guest address (hex)";
        }
        if (ok && !structs_loaded_) {
          structs_loaded_ = true;
          const std::string file = structs::FindStructsFile(path);
          std::string error;
          if (file.empty() || !structs::Load(file, structs_, error)) {
            REXLOG_WARN("[wwe2k14][input] structs.toml: {}", file.empty() ? "not found" : error);
          }
        }
        if (ok && std::none_of(structs_.begin(), structs_.end(), [&](const structs::Def& d) { return d.name == name; })) {
          ok = false;
          why = "no struct named '" + name + "' in tools/input_scripts/structs.toml";
        }
        step.text = name;
      } else if (cmd == "include") {  // Session 30
        emit = false;
        std::string name;
        std::getline(words >> std::ws, name);
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
        const std::string file = name.empty() ? std::string() : FindIncludeFile(path, name);
        if (name.empty()) {
          ok = false;
          why = "include needs a file name";
        } else if (file.empty()) {
          ok = false;
          why = "no such file beside the script or in tools/input_scripts/nav or tools/input_scripts";
        } else if (depth + 1 > kMaxIncludeDepth) {
          ok = false;
          why = fmt::format("includes nested deeper than {}", kMaxIncludeDepth);
        } else {
          const std::string norm = NormalIncludePath(file);
          if (std::find(include_stack.begin(), include_stack.end(), norm) != include_stack.end()) {
            ok = false;
            why = "include loop: " + file + " is already being included";
          } else {
            std::ifstream sub(file);
            if (!sub) {
              ok = false;
              why = "can't open " + file;
            } else {
              const size_t before = steps_.size();
              include_stack.push_back(norm);
              const int sub_top = depth ? top_line : line_number;
              const std::string sub_origin =
                  depth ? (origin.empty() ? "" : origin + " > ") + fmt::format("{}:{}", file_name, line_number) : "";
              if (!ParseLines(sub, file, depth + 1, sub_top, sub_origin, include_stack)) return false;
              include_stack.pop_back();
              REXLOG_INFO("[wwe2k14][input] script {} line {}: include {} ({} steps)", file_name, line_number, file,
                          steps_.size() - before);
            }
          }
        }
      } else if (cmd == "repeat") {
        emit = false;
        ok = OptionalNumber(words, n, -1) && n >= 0 && n <= kMaxRepeat;
        if (repeat_line) {
          ok = false;
          why = fmt::format("repeat blocks can't be nested (the one at line {} is still open)", repeat_line);
        } else if (ok) {
          repeat_line = line_number;
          repeat_count = n;
          repeat_start = steps_.size();
        }
      } else if (cmd == "end_repeat") {
        emit = false;
        ok = repeat_line != 0;
        if (!ok) why = "no open repeat";
        const size_t body_size = ok ? steps_.size() - repeat_start : 0;
        if (ok && repeat_start + (body_size + 1) * size_t(repeat_count) > kMaxSteps) {
          ok = false;
          why = fmt::format("expands to more than {} steps", kMaxSteps);
        }
        if (ok) {
          std::vector<Step> body(steps_.begin() + repeat_start, steps_.end());
          steps_.erase(steps_.begin() + repeat_start, steps_.end());
          for (int64_t pass = 1; pass <= repeat_count; ++pass) {
            Step log{Op::kLog, depth ? top_line : repeat_line, 0, {}, fmt::format("repeat {}/{}", pass, repeat_count)};
            if (depth) {
              log.sub_line = repeat_line;
              log.origin = (origin.empty() ? "" : origin + " > ") + fmt::format("{}:{}", file_name, repeat_line);
            }
            steps_.push_back(log);
            steps_.insert(steps_.end(), body.begin(), body.end());
          }
          repeat_line = 0;
        }
      } else {
        ok = false;
      }
      if (!ok) {
        parse_error_ = fmt::format("{}line {}: can't parse '{}'{}", depth ? file_name + " " : "", line_number, line,
                                   why.empty() ? "" : " (" + why + ")");
        REXLOG_ERROR("[wwe2k14][input] script {} {}", path, parse_error_);
        steps_.clear();
        return false;
      }
      if (emit) steps_.push_back(step);
    }
    if (repeat_line) {
      parse_error_ = fmt::format("{}line {}: repeat without end_repeat", depth ? file_name + " " : "", repeat_line);
      REXLOG_ERROR("[wwe2k14][input] script {} {}", path, parse_error_);
      steps_.clear();
      return false;
    }
    return true;
  }

  // After the whole script (with its includes) is in steps_: the checks that need all of it.
  bool CheckParsedSteps(const std::string& path) {
    // fail_if_file: at most kMaxFailIfFile patterns armed at once. Steps run strictly in order, so checking here
    // is exact.
    std::vector<std::string> armed;
    for (const Step& step : steps_) {
      if (step.op == Op::kFailIfFile) {
        if (std::find(armed.begin(), armed.end(), step.text) == armed.end()) armed.push_back(step.text);
        if (armed.size() > kMaxFailIfFile) {
          REXLOG_ERROR("[wwe2k14][input] script {} line {}: more than {} fail_if_file patterns armed at once", path,
                       L(step), kMaxFailIfFile);
          steps_.clear();
          return false;
        }
      } else if (step.op == Op::kClearFailIf) {
        if (step.text.empty()) armed.clear();
        else armed.erase(std::remove(armed.begin(), armed.end(), step.text), armed.end());
      }
    }
    // clip_start: one per NAME (after repeat expansion), since a second clip of that name would overwrite the
    // first one's files (or share them with it while it is still finishing).
    std::vector<std::string> clip_names;
    for (const Step& step : steps_) {
      if (step.op != Op::kClipStart) continue;
      if (std::find(clip_names.begin(), clip_names.end(), step.text) != clip_names.end()) {
        REXLOG_ERROR("[wwe2k14][input] script {} line {}: clip_start {}: that NAME is already used by an earlier "
                     "clip_start (one clip per NAME; inside a repeat, put the clip outside it)",
                     path, L(step), step.text);
        steps_.clear();
        return false;
      }
      clip_names.push_back(step.text);
    }
    return true;
  }

  // Runs steps until one has to wait for later frames, or stops at a `cvar` step (cvar_pending_) for OnFrame to
  // apply. Called with mutex_ held.
  void Advance() {
    if (forbidden_hit_) {  // OnFileOpened saw a fail_if_file pattern (and logged it)
      FailAndFinish();
      return;
    }
    while (index_ < steps_.size()) {
      Step& step = steps_[index_];
      if (!step_started_) {
        step_started_ = true;
        FlightOnScriptStep(step.line, int(step.op), step.text);  // no-op unless --wwe2k14_flight_recorder
        step_start_frame_ = frame_;
        press_attempt_ = 0;
        state_hold_ = 0;
        state_frame_valid_ = false;
        if (step.op == Op::kWaitFile) {
          REXLOG_INFO("[wwe2k14][input] line {}: waiting for a file containing '{}' (frame {})", L(step),
                      step.text, frame_);
        }
      }
      switch (step.op) {
        case Op::kWaitFrames:
          if (frame_ - step_start_frame_ < step.frames) return;
          break;
        case Op::kWaitFile: {
          const bool seen = std::any_of(opens_.begin(), opens_.end(), [&](const auto& open) {
            return open.first >= sync_mark_ && open.second.find(step.text) != std::string::npos;
          });
          if (!seen) {
            if (frame_ - step_start_frame_ < step.frames) return;
            if (step.optional) {  // Session 10: wait_file ... optional
              // The sync point stays where the last file was seen, so a later wait_file still counts files that
              // opened during this wait (e.g. M3.PAC when a match skips the entrances this waited for).
              REXLOG_WARN("[wwe2k14][input] line {}: no file containing '{}' opened within {} frames (optional: "
                          "going on, frame {})", L(step), step.text, step.frames, frame_);
              break;
            }
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: no file containing '{}' opened within {} frames",
                         L(step), step.text, step.frames);
            FailAndFinish();
            return;
          }
          REXLOG_INFO("[wwe2k14][input] line {}: saw '{}' after {} frames", L(step), step.text,
                      frame_ - step_start_frame_);
          sync_mark_ = frame_;
          break;
        }
        case Op::kPressUntilFile: {
          // press_attempt_ is reset when the step starts (above); press_t0_ = frame of the current press.
          auto start_press = [&] {
            ++press_attempt_;
            press_t0_ = frame_;
            press_released_ = false;
            pad_ = step.pad;
            ++packet_;
            sync_mark_ = frame_;
          };
          if (press_attempt_ == 0) {
            start_press();
            return;
          }
          if (!press_released_) {
            if (frame_ - press_t0_ < step.hold) return;
            pad_ = {};
            ++packet_;
            press_released_ = true;
          }
          const bool seen = std::any_of(opens_.begin(), opens_.end(), [&](const auto& open) {
            return open.first >= press_t0_ && open.second.find(step.text) != std::string::npos;
          });
          if (!seen) {
            if (frame_ - press_t0_ < step.frames) return;
            if (press_attempt_ < step.tries) {
              REXLOG_WARN("[wwe2k14][input] line {}: no file containing '{}' within {} frames of press {}; "
                          "pressing again (frame {})", L(step), step.text, step.frames, press_attempt_, frame_);
              SaveScreenshot(fmt::format("retry_l{}_{}", Tag(step), press_attempt_));
              start_press();
              return;
            }
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: no file containing '{}' opened after {} presses",
                         L(step), step.text, press_attempt_);
            FailAndFinish();
            return;
          }
          REXLOG_INFO("[wwe2k14][input] line {}: saw '{}' {} frames after press {}", L(step), step.text,
                      frame_ - press_t0_, press_attempt_);
          sync_mark_ = frame_;
          break;
        }
        case Op::kPressUntilState: {
          // Session 15 extension (E3): press, then after the release read the state once per frame.
          auto start_press = [&] {
            ++press_attempt_;
            press_t0_ = frame_;
            press_released_ = false;
            pad_ = step.pad;
            ++packet_;
            sync_mark_ = frame_;
          };
          if (press_attempt_ == 0) {
            // Session 30: `if_needed`: already there (a remembered cursor on its target) -> no press at all.
            if (step.if_needed) {
              const GuestState& state0 = states_[step.state];
              uint32_t raw0 = 0;
              if (ReadGuestState(state0, &raw0) && StateMatches(state0, raw0, step.compare)) {
                REXLOG_INFO("[wwe2k14][input] line {}: {} {} already met, no press ({})", L(step), state0.name,
                            step.compare.text, DescribeState(state0, true, raw0));
                sync_mark_ = frame_;
                break;
              }
            }
            start_press();
            return;
          }
          if (!press_released_) {
            if (frame_ - press_t0_ < step.hold) return;
            pad_ = {};
            ++packet_;
            press_released_ = true;
          }
          const GuestState& state = states_[step.state];
          uint32_t raw = 0;
          const bool readable = ReadGuestState(state, &raw);
          const bool met = readable ? StateMatches(state, raw, step.compare) : step.unreadable_ok;
          if (!met) {
            if (frame_ - press_t0_ < step.frames) return;
            if (press_attempt_ < step.tries) {
              REXLOG_WARN("[wwe2k14][input] line {}: {} {} not met within {} frames of press {} ({}); pressing "
                          "again (frame {})", L(step), state.name, step.compare.text, step.frames, press_attempt_,
                          DescribeState(state, readable, raw), frame_);
              start_press();
              return;
            }
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: {} {} not met after {} presses ({})", L(step),
                         state.name, step.compare.text, press_attempt_, DescribeState(state, readable, raw));
            FailAndFinish();
            return;
          }
          REXLOG_INFO("[wwe2k14][input] line {}: {} {} met {} frames after press {} ({})", L(step), state.name,
                      step.compare.text, frame_ - press_t0_, press_attempt_, DescribeState(state, readable, raw));
          sync_mark_ = frame_;
          break;
        }
        case Op::kSet:
          if (!(pad_ == step.pad)) {
            if (!(step.pad == PadState{})) sync_mark_ = frame_;  // a new input, not a release
            pad_ = step.pad;
            ++packet_;
          }
          break;
        case Op::kScreenshot:
          REXLOG_INFO("[wwe2k14][input] line {}: screenshot '{}' (frame {})", L(step), step.text, frame_);
          SaveScreenshot(step.text);
          break;
        case Op::kScreenshotEvery:  // Session 10: OnFrame takes them while the script runs
          periodic_every_ = step.frames;
          periodic_label_ = step.text;
          periodic_next_ = frame_ + step.frames;
          periodic_count_ = 0;
          if (step.frames) {
            REXLOG_INFO("[wwe2k14][input] line {}: screenshot_every {} '{}' (frame {})", L(step), step.frames,
                        step.text, frame_);
          } else {
            REXLOG_INFO("[wwe2k14][input] line {}: screenshot_every 0 (stopped, frame {})", L(step), frame_);
          }
          break;
        case Op::kLog:
          REXLOG_INFO("[wwe2k14][input] line {}: {} (frame {})", L(step), step.text, frame_);
          last_log_ = step.text;  // Session 27: for --wwe2k14_input_result
          break;
        case Op::kScan: {  // Session 29 (src/mem_scan.h): runs here, on the game thread (the frame waits for it)
          std::string error;
          g_frame_time_ignore_next.store(true, std::memory_order_relaxed);  // this pause is not a hitch (bookmark.h)
          if (!scan::Run(step.text, step.value, error)) {
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: {} {}: {}", L(step), step.text, step.value,
                         error);
            FailAndFinish();
            return;
          }
          break;
        }
        case Op::kCheck: {
          // Session 29 (src/check_prompt.h): ask once (press_attempt_ is reset when a step starts), then read the answer
          // once per frame. The timeout is wall clock: fast-forwarded frames would shorten a frame count.
          if (press_attempt_ == 0) {
            press_attempt_ = 1;
            check_t0_ = std::chrono::steady_clock::now();
            if (!check::Ask(step.text, step.line)) {
              REXLOG_WARN("[wwe2k14][check] SKIP line {} (no on-screen overlay in this run, frame {}): {}", L(step),
                          frame_, step.text);
              break;
            }
            REXLOG_INFO("[wwe2k14][check] asking, line {} (frame {}): {}", L(step), frame_, step.text);
            return;
          }
          const double waited =
              std::chrono::duration<double>(std::chrono::steady_clock::now() - check_t0_).count();
          const int verdict = check::Poll();
          if (verdict == check::kNone && waited < double(std::max(1, int(REXCVAR_GET(wwe2k14_check_timeout_s))))) {
            return;
          }
          check::Done();
          if (verdict == check::kFail || verdict == check::kNone) {
            REXLOG_WARN("[wwe2k14][check] {} line {} after {:.1f} s (frame {}): {}", check::VerdictName(verdict),
                        L(step), waited, frame_, step.text);
          } else {
            REXLOG_INFO("[wwe2k14][check] {} line {} after {:.1f} s (frame {}): {}", check::VerdictName(verdict),
                        L(step), waited, frame_, step.text);
          }
          break;
        }
        case Op::kOnFail:
          quit_on_fail_ = step.text == "quit";
          break;
        case Op::kClipStart: {
          REXLOG_INFO("[wwe2k14][input] line {}: clip_start {} {} (frame {})", L(step), step.text, step.value,
                      frame_);
          double fps = 60, scale = 1;
          std::sscanf(step.value.c_str(), "%lf %lf", &fps, &scale);
          if (!clip::g_manager.Start(step.text, fps, scale, frame_)) {
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: clip_start refused", L(step));
            FailAndFinish();
            return;
          }
          break;
        }
        case Op::kClipStop:
          REXLOG_INFO("[wwe2k14][input] line {}: clip_stop {} (frame {})", L(step), step.text, frame_);
          clip::g_manager.StopFromScript(step.text, frame_);
          break;
        case Op::kQuit:
          // In-game clips (src/clip_capture.h): Finish stops a running clip and defers the quit until ffmpeg has
          // finished every clip (up to kClipQuitMaxFrames), as it does for every script end that quits.
          REXLOG_INFO("[wwe2k14][input] line {}: quit (frame {})", L(step), frame_);
          WWE2K14_COVERAGE_MARK(0u, std::string("quit"), frame_);
          Finish(true);
          return;
        case Op::kWatchZero:
          REXLOG_INFO("[wwe2k14][input] line {}: watch_zero 0x{:08X} (frame {})", L(step), uint32_t(step.frames),
                      frame_);
          if (!GuestWatchZero(uint32_t(step.frames))) {
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: watch_zero refused (reason in the [watch] line "
                         "above)", L(step));
            FailAndFinish();
            return;
          }
          break;
        case Op::kWatchCmd:
          REXLOG_INFO("[wwe2k14][input] line {}: {} {} (frame {})", L(step), step.text, step.value, frame_);
          if (!RunGuestWatchCommand(step.text, step.value)) {  // watch / snap refused at run time
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: {} refused (reason in the [watch]/[snap] line "
                         "above)", L(step), step.text);
            FailAndFinish();
            return;
          }
          break;
        case Op::kCvar:
          cvar_pending_ = true;
          return;
        case Op::kMark:
          REXLOG_INFO("[wwe2k14][input] mark {}{}{} (frame {})", step.frames, step.text.empty() ? "" : " ",
                      step.text, frame_);
          rex::perf::timeline::Record("mark", uint32_t(step.frames), uint32_t(frame_));
          WWE2K14_COVERAGE_MARK(uint32_t(step.frames), step.text, frame_);
          GuestWatchOnMark(uint32_t(step.frames), step.text, frame_);  // marks.tsv while a watch/snap runs
          break;
        case Op::kWaitState: {
          if (state_frame_valid_ && state_frame_ == frame_) return;  // one read per frame (Advance can rerun)
          const GuestState& state = states_[step.state];
          const bool first = !state_frame_valid_;
          state_frame_ = frame_;
          state_frame_valid_ = true;
          uint32_t raw = 0;
          const bool readable = ReadGuestState(state, &raw);
          const bool matches = readable && StateMatches(state, raw, step.compare);
          // Log the first value and every change in whether it matches at once; other value changes at most every
          // kStateLogInterval frames (a running counter such as match_clock would otherwise log every frame).
          const bool differs = readable != state_logged_readable_ || (readable && raw != state_logged_value_);
          if (first || matches != state_logged_match_ || (differs && frame_ - state_log_frame_ >= kStateLogInterval)) {
            REXLOG_INFO("[wwe2k14][input] line {}: wait_state {} {}: {} (frame {})", L(step), state.name,
                        step.compare.text, DescribeState(state, readable, raw), frame_);
            state_logged_readable_ = readable;
            state_logged_value_ = raw;
            state_logged_match_ = matches;
            state_log_frame_ = frame_;
          }
          if (matches) {
            if (++state_hold_ < step.hold) return;
          } else {
            state_hold_ = 0;
            if (frame_ - step_start_frame_ < step.frames) return;
            if (step.optional) {  // Session 10: wait_state ... optional
              REXLOG_WARN("[wwe2k14][input] line {}: wait_state {} {} not reached within {} frames (last: {}; "
                          "optional: going on, frame {})", L(step), state.name, step.compare.text, step.frames,
                          DescribeState(state, readable, raw), frame_);
              break;
            }
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: wait_state {} {} not reached within {} frames "
                         "(last: {})", L(step), state.name, step.compare.text, step.frames,
                         DescribeState(state, readable, raw));
            FailWithScreenshot(fmt::format("state_fail_l{}", Tag(step)));
            return;
          }
          REXLOG_INFO("[wwe2k14][input] line {}: state {} {} reached after {} frames ({}, held {} frames)",
                      L(step), state.name, step.compare.text, frame_ - step_start_frame_,
                      DescribeState(state, true, raw), step.hold);
          break;
        }
        case Op::kLogText: {  // Session 30
          REXLOG_INFO("[wwe2k14][input] line {}: screen text: {} (frame {})", L(step), screen_text::Joined(2000),
                      frame_);
          if (step.frames) {
            std::string recent;
            for (const auto& [f, t] : screen_text::Recent(size_t(step.frames))) {
              recent += fmt::format("{}{} @{}", recent.empty() ? "" : " | ", t, f);
            }
            REXLOG_INFO("[wwe2k14][input] line {}: recent text (newest first): {}", L(step), recent);
          }
          break;
        }
        case Op::kPlugin: {  // Session 30 (#4)
          std::string msg;
          if (!WWE2K14_PROBE_COMMAND(step.text, step.value, msg)) {
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: plugin {}: {}", L(step), step.text, msg);
            FailAndFinish();
            return;
          }
          REXLOG_INFO("[wwe2k14][input] line {}: plugin {}: {}", L(step), step.text, msg);
          break;
        }
        case Op::kDumpStruct: {  // Session 30
          for (const structs::Def& d : structs_) {
            if (d.name != step.text) continue;
            const uint32_t addr = step.frames ? uint32_t(step.frames) : structs::BaseAddress(d);
            for (const std::string& line : structs::Dump(d, addr)) {
              REXLOG_INFO("[wwe2k14][input] line {}: {}", L(step), line);
            }
          }
          break;
        }
        case Op::kWaitText:
        case Op::kAssertText: {  // Session 30
          const std::string found = screen_text::Find(step.text);
          if (step.op == Op::kAssertText) {
            if (found.empty()) {
              REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: assert_text '{}' not on screen (screen text: "
                           "{}, frame {})", L(step), step.text, screen_text::Joined(), frame_);
              FailWithScreenshot(fmt::format("text_fail_l{}", Tag(step)));
              return;
            }
            REXLOG_INFO("[wwe2k14][input] line {}: assert_text '{}' ok ('{}', frame {})", L(step), step.text, found,
                        frame_);
            break;
          }
          if (found.empty()) {
            if (frame_ - step_start_frame_ < step.frames) return;
            if (step.optional) {
              REXLOG_WARN("[wwe2k14][input] line {}: wait_text '{}' not on screen within {} frames (screen text: {}; "
                          "optional: going on, frame {})", L(step), step.text, step.frames, screen_text::Joined(),
                          frame_);
              break;
            }
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: wait_text '{}' not on screen within {} frames "
                         "(screen text: {})", L(step), step.text, step.frames, screen_text::Joined());
            FailWithScreenshot(fmt::format("text_fail_l{}", Tag(step)));
            return;
          }
          REXLOG_INFO("[wwe2k14][input] line {}: text '{}' on screen after {} frames ('{}')", L(step), step.text,
                      frame_ - step_start_frame_, found);
          sync_mark_ = frame_;
          break;
        }
        case Op::kAssertState:
        case Op::kLogState: {
          const GuestState& state = states_[step.state];
          uint32_t raw = 0;
          const bool readable = ReadGuestState(state, &raw);
          if (step.op == Op::kLogState) {
            REXLOG_INFO("[wwe2k14][input] line {}: state {} = {} (frame {})", L(step), state.name,
                        DescribeState(state, readable, raw), frame_);
            break;
          }
          if (!readable || !StateMatches(state, raw, step.compare)) {
            REXLOG_ERROR("[wwe2k14][input] script FAILED at line {}: assert_state {} {} (is {}, frame {})", L(step),
                         state.name, step.compare.text, DescribeState(state, readable, raw), frame_);
            FailWithScreenshot(fmt::format("state_fail_l{}", Tag(step)));
            return;
          }
          REXLOG_INFO("[wwe2k14][input] line {}: assert_state {} {} ok ({}, frame {})", L(step), state.name,
                      step.compare.text, DescribeState(state, true, raw), frame_);
          break;
        }
        case Op::kFailIfFile:
          if (std::find(forbidden_.begin(), forbidden_.end(), step.text) == forbidden_.end()) {
            forbidden_.push_back(step.text);
          }
          REXLOG_INFO("[wwe2k14][input] line {}: fail_if_file '{}' armed (frame {})", L(step), step.text, frame_);
          break;
        case Op::kClearFailIf:
          if (step.text.empty()) {
            forbidden_.clear();
          } else {
            forbidden_.erase(std::remove(forbidden_.begin(), forbidden_.end(), step.text), forbidden_.end());
          }
          REXLOG_INFO("[wwe2k14][input] line {}: clear_fail_if {} (frame {})", L(step),
                      step.text.empty() ? std::string("(all)") : "'" + step.text + "'", frame_);
          break;
      }
      ++index_;
      step_started_ = false;
    }
    REXLOG_INFO("[wwe2k14][input] script finished at frame {}; real controller active again", frame_);
    Finish(false);
  }

  // One key-down / key-up event (XINPUT_KEYSTROKE flags 1 / 2) for the first button or trigger whose state
  // differs from what the keystroke reader has been told so far. Called with mutex_ held.
  bool NextKeystroke(uint16_t& virtual_key, uint16_t& key_flags) {
    static constexpr std::pair<uint16_t, uint16_t> kButtonKeys[] = {
        {0x1000, 0x5800}, {0x2000, 0x5801}, {0x4000, 0x5802}, {0x8000, 0x5803},  // A B X Y
        {0x0200, 0x5804}, {0x0100, 0x5805},                                      // RB LB
        {0x0001, 0x5810}, {0x0002, 0x5811}, {0x0004, 0x5812}, {0x0008, 0x5813},  // dpad up down left right
        {0x0010, 0x5814}, {0x0020, 0x5815}, {0x0040, 0x5816}, {0x0080, 0x5817}};  // start back LS RS
    for (const auto& [mask, key] : kButtonKeys) {
      const bool is = pad_.buttons & mask;
      if (bool(keystroke_view_.buttons & mask) != is) {
        keystroke_view_.buttons ^= mask;
        virtual_key = key;
        key_flags = is ? 1 : 2;
        return true;
      }
    }
    constexpr uint8_t kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    for (auto [now, seen, key] : {std::tuple{pad_.lt, &keystroke_view_.lt, uint16_t(0x5806)},
                                  std::tuple{pad_.rt, &keystroke_view_.rt, uint16_t(0x5807)}}) {
      if ((now > kTriggerThreshold) != (*seen > kTriggerThreshold)) {
        *seen = now;
        virtual_key = key;
        key_flags = now > kTriggerThreshold ? 1 : 2;
        return true;
      }
    }
    return false;
  }

  // Session 27: a script failure (every "script FAILED" path) = remember it for --wwe2k14_input_result, then end.
  void FailAndFinish() {
    failed_ = true;
    batch_failed_ = true;  // Session 29: live batches
    Finish(quit_on_fail_);
  }

  // Session 27: --wwe2k14_input_result=FILE: one line when the script ends, for a program that started the game with
  // a script (the launcher's logo import): "ok<TAB>LAST LOG TEXT" or "failed<TAB>line N<TAB>LAST LOG TEXT" (the last
  // `log` step before the end names the stage). Written once per run.
  void WriteResult() {
    const std::string path = REXCVAR_GET(wwe2k14_input_result);
    if (path.empty() || result_written_) return;
    result_written_ = true;
    const int line = index_ < steps_.size() ? steps_[index_].line : (steps_.empty() ? 0 : steps_.back().line);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
      const std::string text = failed_ ? fmt::format("failed\tline {}\t{}\n", line, last_log_)
                                       : fmt::format("ok\t{}\n", last_log_);
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
    REXLOG_INFO("[wwe2k14][input] result {} (line {}, last log '{}') -> {}", failed_ ? "failed" : "ok", line,
                last_log_, path);
  }

  void Finish(bool quit) {
    WriteResult();
    active_ = false;
    check::Done();  // Session 29: a script that ends (or fails) takes its question off the screen
    if (live_running_) {  // Session 29: the live batch has ended (src/live_control.h reads it through LiveStatus)
      live_running_ = false;
      live_end_frame_ = frame_;
      live_failed_ = batch_failed_;
      REXLOG_INFO("[wwe2k14][live] batch {} {} at frame {} ({} frames)", live_batch_, batch_failed_ ? "FAILED" : "done",
                  frame_, frame_ - live_start_frame_);
    }
    periodic_every_ = 0;  // Session 10: screenshot_every ends with the script
    pad_ = {};
    keystroke_view_ = {};
    forbidden_.clear();
    forbidden_hit_ = false;
    ++packet_;
    // In-game clips (src/clip_capture.h; nothing runs while --wwe2k14_clip_dir is empty): every script end stops a
    // running clip, and a quit (`quit` or an on_fail quit) waits in OnFrame until each clip has finished (its json
    // written, ffmpeg done), at most kClipQuitMaxFrames. Without clips in flight the quit is immediate, as before.
    clip::g_manager.Stop(frame_, quit ? "script quits" : "script ended");
    if (quit && g_clips_in_flight.load() > 0) {
      REXLOG_INFO("[wwe2k14][input] quit waits for {} clip(s) to finish (frame {})", g_clips_in_flight.load(),
                  frame_);
      fail_quit_pending_ = true;
      fail_quit_frame_ = frame_;
      fail_quit_max_ = kClipQuitMaxFrames;
      return;
    }
    // Session 18: also for screenshots still being written (PNG encoding is slower than BMP; fast-forwarded runs
    // reach the quit a few ms after the last screenshot: run 1387 left two .png.part files). At most
    // kFailQuitMaxFrames; none in flight = quit at once, as before.
    if (quit && g_screenshots_in_flight.load() > 0) {
      REXLOG_INFO("[wwe2k14][input] quit waits for {} screenshot(s) to be written (frame {})",
                  g_screenshots_in_flight.load(), frame_);
      fail_quit_pending_ = true;
      fail_quit_frame_ = frame_;
      fail_quit_max_ = kFailQuitMaxFrames;
      return;
    }
    if (quit && g_request_quit) g_request_quit();
  }

  // A script failure that saves a screenshot first (wait_state / assert_state). With on_fail quit the quit is
  // deferred to OnFrame until the screenshot is written: quitting at once races the BMP write against the
  // process's hard exit (run 247 won by 8 ms at 720p; a 4K BMP is 9x larger).
  void FailWithScreenshot(const std::string& label) {
    failed_ = true;  // Session 27: --wwe2k14_input_result
    batch_failed_ = true;  // Session 29: live batches
    SaveScreenshot(label);
    const bool quit = quit_on_fail_;
    Finish(false);  // also stops a running clip; OnFrame then waits for it too
    if (quit) {
      fail_quit_pending_ = true;
      fail_quit_frame_ = frame_;
      fail_quit_max_ = g_clips_in_flight.load() > 0 ? kClipQuitMaxFrames : kFailQuitMaxFrames;
    }
  }

  // Recording: a `wait_frames` for the time since the last change, then a `set` with the new state.
  void Record(const PadState& pad) {
    // Ignore tiny stick jitter so recordings stay readable.
    auto close_to = [](int16_t a, int16_t b) { return std::abs(int(a) - int(b)) < 2048; };
    if (pad.buttons == recorded_.buttons && pad.lt == recorded_.lt && pad.rt == recorded_.rt &&
        close_to(pad.lx, recorded_.lx) && close_to(pad.ly, recorded_.ly) && close_to(pad.rx, recorded_.rx) &&
        close_to(pad.ry, recorded_.ry)) {
      return;
    }
    FlushRecordWait();
    recorded_ = pad;
    std::fprintf(record_file_, "set 0x%04X %u %u %d %d %d %d\n", pad.buttons, pad.lt, pad.rt, pad.lx, pad.ly,
                 pad.rx, pad.ry);
    std::fflush(record_file_);
  }

  void FlushRecordWait() {
    if (frame_ > record_frame_) std::fprintf(record_file_, "wait_frames %llu\n",
                                             static_cast<unsigned long long>(frame_ - record_frame_));
    record_frame_ = frame_;
  }

  std::mutex mutex_;
  std::mutex trace_mutex_;
  std::vector<std::string> traced_;
  uint64_t frame_ = 0;
  // Replay.
  bool active_ = false;
  std::vector<Step> steps_;
  size_t index_ = 0;
  bool step_started_ = false;
  uint64_t step_start_frame_ = 0;
  bool quit_on_fail_ = true;
  bool failed_ = false, result_written_ = false;  // Session 27: --wwe2k14_input_result
  std::string last_log_;
  PadState pad_;
  uint32_t packet_ = 1;
  PadState keystroke_view_;                                // pad state as last reported via keystrokes
  std::vector<std::pair<uint64_t, std::string>> opens_;    // (frame, lowercase path) while a script runs
  uint64_t sync_mark_ = 0;  // wait_file counts opens from here: the last input or the last wait_file
  uint32_t press_attempt_ = 0;   // press_until_file: presses made so far in the current step
  uint64_t press_t0_ = 0;        // press_until_file: frame of the current press
  bool press_released_ = false;  // press_until_file: the current press's HOLD has ended
  bool cvar_pending_ = false;            // Advance stopped at a `cvar` step for OnFrame to apply
  uint64_t periodic_every_ = 0;          // Session 10: screenshot_every interval (0 = off)
  uint64_t periodic_next_ = 0;           // ... frame of the next one
  uint32_t periodic_count_ = 0;          // ... taken so far (label suffix)
  std::string periodic_label_;           // ... label
  std::vector<std::string> forbidden_;   // armed fail_if_file patterns (lowercase)
  bool forbidden_hit_ = false;           // a forbidden file was opened; the next frame applies on_fail
  std::vector<GuestState> states_;       // states the script uses (states.toml entries + inline chains)
  bool states_loaded_ = false;           // states.toml has been read (only when a script names a state)
  uint32_t state_hold_ = 0;              // wait_state: consecutive matching frames so far
  uint64_t state_frame_ = 0;             // wait_state: frame of the last read
  bool state_frame_valid_ = false;       // wait_state: a read has happened in the current step
  bool state_logged_readable_ = false;   // wait_state: what the last value log line showed (change logging)
  uint32_t state_logged_value_ = 0;
  bool state_logged_match_ = false;
  uint64_t state_log_frame_ = 0;         // wait_state: frame of the last value log line
  bool fail_quit_pending_ = false;       // FailWithScreenshot under on_fail quit: OnFrame quits when it's written
  uint64_t fail_quit_frame_ = 0;         // (also a quit waiting for in-game clips to finish: Finish)
  uint64_t fail_quit_max_ = kFailQuitMaxFrames;  // most frames that quit waits
  std::string parse_error_;              // Session 29: why the last Parse failed (for a live batch's reply)
  std::chrono::steady_clock::time_point check_t0_;  // Session 29: `check` asked at
  bool batch_failed_ = false;            // Session 29: the current script (or live batch) failed
  uint64_t live_batch_ = 0;              // Session 29: number of the latest live batch (0 = none yet)
  bool live_running_ = false;            // ... it has not ended yet
  bool live_failed_ = false;             // ... it ended with a failure (or a stop)
  uint64_t live_start_frame_ = 0, live_end_frame_ = 0;
  // Recording.
  std::FILE* record_file_ = nullptr;
  bool structs_loaded_ = false;                // Session 30: dump_struct (structs.toml, loaded at the first use)
  std::vector<structs::Def> structs_;
  bool record_states_loaded_ = false;          // Session 30: RecordStates
  std::vector<GuestState> record_states_;
  std::vector<uint64_t> record_values_;
  PadState recorded_;
  uint64_t record_frame_ = 0;
};

inline InputScript g_input_script;

// Session 33 (photo mode, src\photo_mode.h): sees player 1's pad after everything above and may change it (an idle
// pad for the game while the pad drives the camera). Null = nothing.
inline void (*g_pad_filter)(rex::input::X_INPUT_STATE* state) = nullptr;
// Session 33: photo mode's per-frame step (VdSwap, kernel_overrides.h). Null = nothing.
inline void (*g_photo_frame_hook)(uint8_t* base) = nullptr;
// Session 34: the 30 Years of WrestleMania jump's per-frame step (src/wm30_jump.h, VdSwap). Null = nothing.
inline void (*g_wm30_frame_hook)(uint64_t frame) = nullptr;

inline uint32_t XamInputGetStateOverride(uint32_t user_index, uint32_t flags,
                                         ppc_ptr_t<rex::input::X_INPUT_STATE> state) {
  const uint32_t result = replay::g_replay.State(user_index, flags, state,
                                                 [&] { return g_input_script.GetState(user_index, flags, state); });
  if (g_pad_filter && result == 0 && state && ((user_index & 0xFF) == 0 || (user_index & 0xFF) == 0xFF) &&
      (!(flags & 0xFF) || (flags & 0x01)))
    g_pad_filter(&*state);
  return result;
}

inline uint32_t XamInputGetCapabilitiesOverride(uint32_t user_index, uint32_t flags,
                                                ppc_ptr_t<rex::input::X_INPUT_CAPABILITIES> caps) {
  return replay::g_replay.Caps(user_index, flags, caps,
                               [&] { return g_input_script.GetCapabilities(user_index, flags, caps); });
}

inline uint32_t XamInputGetKeystrokeExOverride(mapped_u32 user_index_ptr, uint32_t flags,
                                               ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke) {
  return replay::g_replay.Keystroke(user_index_ptr, flags, keystroke,
                                    [&] { return g_input_script.GetKeystroke(user_index_ptr, flags, keystroke); });
}

}  // namespace wwe2k14

REX_HOOK(__imp__XamInputGetState, wwe2k14::XamInputGetStateOverride)
REX_HOOK(__imp__XamInputGetKeystrokeEx, wwe2k14::XamInputGetKeystrokeExOverride)
REX_HOOK(__imp__XamInputGetCapabilities, wwe2k14::XamInputGetCapabilitiesOverride)
