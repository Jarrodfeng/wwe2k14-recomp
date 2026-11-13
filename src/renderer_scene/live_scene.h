// The live scene writer: the contract every Phase C class builds against (Session 101, lane 1; arc 3.3 track G, 5 "Phase C").
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game, at engine level, describes each frame as a scene record (docs\renderer\SCENE_RECORD.md, source = Game). Each CLASS (crowd,
// arena, wrestlers, ring / props / particles / post, UI) is one Contributor: its own hooks observe the game (read only: no change to what
// the game does or draws) and keep a private snapshot; at the frame boundary the writer asks every contributor to append its part to one
// SceneTables, then hands the record to its sinks (the capture folder's scene\frame_NNNNNN.rsc, the case ring, later our renderer).
//
// Rules (fixed for every class lane; hand back rather than change them):
//  - LOCKS (S101 lane 26 review): Emit runs while the writer's own mutex is held, on the thread that calls the frame boundary (the game's present
//    path in reader.h, which holds its own lock). A contributor must never take reader.h's lock, and a contributor's snapshot lock must never be
//    held by a game-thread hook while that hook calls anything in reader.h (two-lock deadlock). Hooks publish under their own short lock and return.
//  - Hooks run on game threads; Emit runs on the writer's thread at the frame boundary. A contributor owns its own synchronisation
//    (a double buffer swapped under a short lock, or atomics). Emit never blocks on game state and never reads guest memory itself:
//    it copies the snapshot its hooks took.
//  - Off by default and free when off: Enabled() is one relaxed load; hooks return at once when it is false.
//  - Unknown stays unknown (SCENE_RECORD.md "Rules"): 0xFFFFFFFF / string 0, never a guessed value. A class that could not read a value
//    this frame leaves it unknown and counts it (Counters).
//  - Object ids are stable across frames ("crowd:block:3:member:17", "actor:slot:0", "arena:slot:12"): SCENE_RECORD.md "Stable ids".
//  - Assets by name: AssetRef pac + entry from the files, never guest memory addresses.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "record_io.h"  // SceneTables (same folder)

namespace rdr_scene::live {

// What one contributor did this frame (the writer logs a census line per window: "[scene][live] frames A-B: crowd objects N, fields_unknown U").
struct Counters {
  uint32_t objects = 0;    // Objects appended
  uint32_t unknown = 0;    // fields left unknown because the snapshot lacked them
  uint32_t stale = 0;      // frames the snapshot was older than this frame (the hook did not run)
};

struct FrameInfo {
  uint64_t guest_frame = 0;   // the game's frame counter at the boundary (the same clock the capture's frames use)
  uint32_t frame_in_window = 0;
  uint32_t capture_frame = 0xFFFFFFFFu;  // S101 lane26: the capture folder's own frame number for this frame (frames\frame_NNNNNN.bin), or none (live / case ring)
};

class Contributor {
 public:
  virtual ~Contributor() = default;
  virtual const char* Name() const = 0;  // the class: "crowd", "arena", "wrestlers", "post", "ui", ...
  // Append this class's part of the frame to `t` (objects, bones, params, lights, sprites, ...). Uses t.AddString for names.
  virtual void Emit(const FrameInfo& f, SceneTables& t, Counters& c) = 0;
  // S101 lane 109 (F4): appends this class's own totals to the census line (default: nothing).
  virtual void Census(std::string& out) { (void)out; }
};

// Registration happens once, at startup, before Enabled() can become true. The writer does not own contributors (static lifetime).
void Register(Contributor* c);

// True while a sink wants records (a capture armed with scene records, the case ring, our renderer). One relaxed atomic load.
bool Enabled();

// Called once per frame by the frame boundary hook (where the capture's frame is submitted: src\renderer_adapter\live.cpp).
// Builds the record (header source = Game, guest_frame), asks each contributor in registration order, hands it to the sinks.
void EndFrame(uint64_t guest_frame);

// Sinks: where records go. The capture folder writes scene\frame_NNNNNN.rsc beside frames\ (SCENE_RECORD.md "Layout on disk").
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void OnRecord(const FrameInfo& f, const SceneTables& t) = 0;
  // S101 lane26: what EndFrame actually calls. The record is built once and shared (immutable), so a sink that queues work for its own thread
  // keeps the pointer instead of copying; the default forwards to OnRecord. Runs on the frame-boundary thread: return quickly, never block.
  virtual void OnRecordShared(const FrameInfo& f, std::shared_ptr<const SceneTables> t) { OnRecord(f, *t); }
  // S101 lane26: appends this sink's part of the per-window census line ("folder written 300, dropped 0"). Default: nothing.
  virtual void Census(std::string& line) { (void)line; }
};
void AddSink(Sink* s);     // Enabled() becomes true while at least one sink is attached
void RemoveSink(Sink* s);

// ---- S101 lane26: additions the writer's tests and the frame boundary need (the contract above is unchanged) ----
inline constexpr uint32_t kNoCaptureFrame = 0xFFFFFFFFu;
// EndFrame with the capture folder's frame number (reader.h: the one frame-boundary call). EndFrame(guest_frame) is this with kNoCaptureFrame.
void EndFrame(uint64_t guest_frame, uint32_t capture_frame);
// Where the census / refusal lines go (the game sets REXLOG_INFO; a test prints). Without one, nothing is logged.
using LogFn = void (*)(const char* line);
void SetLog(LogFn fn);
// Writes the `[scene][live] frames A-B: ...` line (if any frame was built in the window) and starts a new window. reader.h calls it with
// the `[rdr][names]` window.
void CensusLog(uint64_t from, uint64_t to);
// Totals since the start (the tests read these).
struct Totals {
  uint64_t frames = 0;       // EndFrame calls that did work (a sink was attached and a contributor registered)
  uint64_t records = 0;      // records handed to the sinks
  uint64_t refused = 0;      // frames whose record was dropped: a contributor threw, or the tables failed validation
  uint64_t sink_errors = 0;  // a sink threw (caught; the game path is unaffected)
};
Totals GetTotals();
// Same checks ParseRecord makes on a file (every index inside its table): false with the reason. EndFrame runs it before any sink sees a record.
bool Validate(const SceneTables& t, std::string& error);

}  // namespace rdr_scene::live
