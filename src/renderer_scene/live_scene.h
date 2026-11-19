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
#include <atomic>
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
  // S102 U1 (docs\renderer\SWAP_TAIL.md): take EVERY input Emit reads from game threads (snapshots, tracker frames) into the contributor's own
  // storage, for frame f, while the game threads are parked at the frame barrier. True = taken: the next Emit(f) reads only what Latch took, so it
  // may run after the update thread was released. False (the default) = this contributor cannot: the writer then runs the whole frame at the latch.
  // A contributor that latches must also report (NoteViolation) any shared-state read its Emit makes while InDeferredEmit() is true.
  virtual bool Latch(const FrameInfo& f) { (void)f; return false; }
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
// S103 lane S (docs\renderer\OURS_INPUT_AUDIT.md item 7): the record's kScInputStamps snapshot is taken ONCE, at the END of the latch (after every
// contributor's Latch, game threads parked), or at the record's build when no latch ran (EndFrame: the same moment). It replaces whatever a
// contributor wrote, so the render side can check that record j's own inputs are fresh for its hook frame. A deferred emit with no snapshot of
// its own frame writes none (the render side then refuses OURS for it: fail safe). nullptr (the default) = the contributors' rows, as before.
using StampFillFn = void (*)(std::vector<InputStampRow>& out);
void SetStampFill(StampFillFn fn);
// S103 L1C review B1: the stamps a contributor makes at EMIT (after the latch snapshot): the bits this fill names overwrite their rows in the record's input_stamps. Keep this list to the bits that CANNOT be decided at the latch (bit 14: it needs the record's own movie rows).
void SetEmitStampFill(StampFillFn fn);
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

// ---- S102 U1 (docs\renderer\SWAP_TAIL.md): the frame boundary in two halves (the switch wwe2k14_rdr_tail_release, reader.h) ----
// LatchFrame runs where EndFrame ran (the game threads parked at the frame barrier): it counts the frame, asks every contributor to Latch, and
//  - when every one latched and `allow_defer`: returns with out.emit_deferred = true; EmitLatched(out) builds the record later (after the release);
//  - otherwise: runs the whole frame now (the contributors that latched use what they took), out.emit_deferred = false.
// EmitLatched is a no-op unless out.emit_deferred; it clears it. Same record, same sinks, same order as EndFrame.
struct Latched {
  bool emit_deferred = false;
  FrameInfo info;
  uint32_t contributors = 0;   // how many were registered at the latch (a registration in between refuses the frame: counted)
};
void LatchFrame(uint64_t guest_frame, uint32_t capture_frame, bool allow_defer, Latched& out);
void EmitLatched(Latched& l);
// True on the thread running a deferred EmitLatched: a contributor's shared-state path reached then is a violation (it claimed to latch and did not).
// Inline (header-only): tools that link a contributor without live_scene.cpp (crowd_make, wrestler_emit) still link.
namespace tail_detail {
inline thread_local bool t_deferred_emit = false;
inline std::atomic<uint64_t> g_violations{0};
inline std::atomic<const char*> g_first_violation{nullptr};
}  // namespace tail_detail
inline bool InDeferredEmit() { return tail_detail::t_deferred_emit; }
inline void NoteViolation(const char* contributor) {
  tail_detail::g_violations.fetch_add(1, std::memory_order_relaxed);
  const char* none = nullptr;
  tail_detail::g_first_violation.compare_exchange_strong(none, contributor ? contributor : "?", std::memory_order_relaxed);
}
struct LatchTotals {
  uint64_t latched = 0;          // LatchFrame calls that did work
  uint64_t deferred = 0;         // of them, the emit was deferred
  uint64_t not_latchable = 0;    // a contributor could not latch (the frame ran whole at the latch)
  uint64_t registry_changed = 0; // EmitLatched found another contributor count: the frame was refused
  uint64_t violations = 0;       // NoteViolation calls (must stay 0)
  std::string first_violation;   // the first contributor that reported one
  std::string not_latchable_name;  // the last contributor that could not latch
};
LatchTotals GetLatchTotals();
#ifdef RDR_TAIL_PLANTS
// Test only (tools\tail\tail_order_test.cpp): 1 = LatchFrame claims every contributor latched without asking it (the emit then reads shared state late);
// 2 + i = the same for contributor i (registration order) alone.
inline int g_tail_plant = 0;
#endif

}  // namespace rdr_scene::live
