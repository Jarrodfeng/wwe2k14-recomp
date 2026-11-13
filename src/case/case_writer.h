// Turns a ring snapshot into one case zip (Session 101, lane 5; docs\renderer\CASE_ZIP.md). Runs on a worker thread: the game never waits.
// No game or SDK includes.
#pragma once
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "case_manifest.h"
#include "case_ring.h"

namespace rdr_case {

// S101 lane 302 (review N1): the game's log file is written by a background thread; the app sets this to rex::DrainLogging so the worker
// reads a log whose queue is on disk. Null in the standalone tools (their log is not async).
inline void (*g_drain_log)() = nullptr;

struct CaseRequest {
  Manifest manifest;               // the game side fills trigger, note, ours_only, build, device, settings, where, way_back (builder, seed, replay_mode); the writer fills the rest
  std::string device_tag = "pc";   // the id's device part
  std::string log_text;            // log.txt as text, or
  std::filesystem::path log_path;  // S101 lane 11: the run log; the WORKER reads its last log_window_s seconds (the trigger thread never touches the disk)
  double log_window_s = 10.0;
  std::time_t log_now = 0;         // the trigger's wall clock (0: now)
  std::string inputs_rec;          // inputs.rec: src\replay.h format; empty = a stub that says nothing was recorded
  // S101 lane 19: run on the worker once the window is known (guest frames first..last): the recorder's rows cut to the window (empty = none
  // recorded), and a last chance to fill the manifest (the [rdr][names] counts of the window).
  std::function<std::string(uint64_t first, uint64_t last)> inputs_for_window;
  std::function<void(Manifest&, uint64_t first, uint64_t last)> finish_manifest;
  std::string states_text;         // states.txt
  std::filesystem::path picture_path;  // picture.png: read when the zip is written (a few retries: the picture may still be on its way). PNG
  bool picture_temp = false;           // picture_path is a temp file: deleted once read (or after the retries)
  std::vector<uint8_t> picture_png;   // or the bytes themselves
  std::filesystem::path out_dir;       // where the zip goes (out\cases on PC)
  std::chrono::steady_clock::time_point triggered = std::chrono::steady_clock::now();
};

struct CaseResult {
  bool ok = false;
  std::filesystem::path path;
  std::string error;
  double ms_from_trigger = 0;      // trigger to the zip renamed into place
  uint64_t zip_bytes = 0;
  uint32_t frames = 0;
  uint64_t capture_bytes = 0;
  uint32_t unresolved = 0;         // draw / content refs the ring did not have (the writer marks the draw no-geometry / drops the ref)
  uint32_t scene_records = 0;     // S101 lane26: scene records written under capture\scene\ (one per ring frame that had one)
  uint32_t scene_unmatched = 0;   // S101 lane26: records whose guest frame no ring frame carries (left out)
  uint32_t stale_contents = 0;     // S101 lane 11: contents left out because they do not fit the image record the zip carries (the image changed after)
  bool has_picture = false;       // a GAME-part zip got its picture.png
  std::string manifest_json;
};

// The last `window_s` seconds of a run log (lines "[YYYY-MM-DD HH:MM:SS..." or "[...HH:MM:SS"), read from the last 1 MB of the file.
std::string RecentLogText(const std::filesystem::path& path, double window_s, std::time_t now);

// Synchronous: call it from a worker thread.
CaseResult WriteCase(const Snapshot& snapshot, CaseRequest request);

// One worker thread, a bounded queue (3 waiting): a trigger when the queue is full gets `done` at once with ok=false. `done` runs on the worker
// thread. WaitIdle() blocks until the queue is empty and the worker idle.
void WriteCaseAsync(Snapshot snapshot, CaseRequest request, std::function<void(const CaseResult&)> done);
void WaitIdle();

}  // namespace rdr_case
