// S102 lane D1: breadcrumbs for a lost Vulkan device (docs\renderer\DEVICE_LOST.md).
//
// The Surface Pro 6 (Intel UHD 620) lost the device in about 1 in 5 Brock entrance runs: one `frame_submit_failed: VkResult -4`, then nothing drawn
// again. The log could not say what the GPU was doing. Two records name it next time:
//
//   1. The ring (CPU): the last kFrames frames, each a compact list of the steps it recorded (pass / copy / dispatch / present: the step's plan index,
//      its first target or destination image, its GPU class, the pass's draws, pipeline binds, vertices + indices, the first and last draw's program)
//      plus the frame's uploads, destroys and the pipelines built while it was recorded. Written by the render thread only, a few stores a step
//      (no per-draw work: a pass's counts are the counters RecordPass already keeps). Dumped once, on the first sign of a lost device.
//   2. The GPU marker: a host-visible buffer, kMarkerWords 32-bit words a frame slot (vkCmdUpdateBuffer, 4 bytes, outside render passes; each word
//      written once a frame, so no write-after-write in one command buffer). Word 0 = the frame's serial when its buffer starts. Marker k (k >= 1) has two
//      words: BEGIN (2k), written with no barrier when the front end reaches it, and END (2k+1), written when marker k's work is DONE: an execution
//      barrier (all commands -> transfer) before it, a transfer-write -> host-read barrier after it, so it is in memory before anything after it runs (S102
//      D1 review F1: a bare write can sit in a cache and be lost at an engine reset, and on the 4090 the newest one read one short). After a loss the CPU
//      reads the words (mapped memory of a lost device stays readable): the highest END holding the slot's serial is the last marked work the GPU FINISHED;
//      the fault is in the next marked step or after it, up to the highest BEGIN (the front end's reach). Marker 1 = the uploads, markers 2.. = the steps
//      (mode 1: the first step of each run of one GPU class, and the present; mode 2: every pass / dispatch / present and the first copy of a run of copies;
//      mode 3: every step: then the first unfinished marker is exactly one step), the last marker the frame's end (after the read-backs).
//      Cost (rplay, Bryan capture, RTX 4090, --timing, GPU ms a frame 0.69 without; docs\renderer\DEVICE_LOST.md): ANY marker word costs about +0.35 ms
//      a frame on the 4090, about the same for 2 words a frame as for 134 (mode 1 0.98-0.99, mode 2 0.99-1.03, two words only 1.04-1.09; vkCmdFillBuffer
//      worse, 1.17; a device-local host-visible buffer 1.03): a fixed cost of the transfer command between passes, not per word. Not "tiny", and
//      the Surface's cost is unmeasured, so the marker ships OFF: a diagnosis run turns it on (mode 3), the CPU ring stays on.
//
// Switches (read every frame, rdr_switches.h): wwe2k14_rdr_breadcrumbs (default 1) the ring and the dump; wwe2k14_rdr_gpu_marker (default 0) the
// marker words, 0 off / 1 class runs / 2 passes and copy runs / 3 every step.
// Test only (rplay --crumbs-selftest FRAME:STEP [--crumbs-plant N]): the renderer stalls the GPU at the first marked step >= STEP of frame serial
// FRAME (a VkEvent the host sets later: invalid usage under today's spec, so those arms run without validation), waits until the marker words say the
// GPU reached it, runs the device-lost handler as if the device were lost, and checks that the decode names that step and that the ring's steps
// match the frame's plan. The selftest records exactly the production marker stream (no flush of its own). Plants (each must FAIL the check): 1 the
// stalled step's crumb names the next marker, 2 one step's crumb is not recorded, 3 presents are still counted after the (simulated) loss: the bug this lane
// fixed, 4 the END words without their barriers (the first version's stream: on the 4090 the decode read one short).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rdr {

// Set by the renderer on the first sign of a lost device: the frame serial + 1 (0 = the device is fine). Read by the frame csv (device_lost column),
// the present accounting (no present counts after a loss) and the live sink (the player's message).
inline std::atomic<uint64_t> g_device_lost{0};
inline bool DeviceLost() { return g_device_lost.load(std::memory_order_relaxed) != 0; }

namespace crumbs {

inline std::atomic<bool> g_on{true};      // wwe2k14_rdr_breadcrumbs
inline std::atomic<bool> g_message{false};  // wwe2k14_rdr_device_lost_message (the game's frame_stats.h shows the player's message once)
inline std::atomic<int> g_marker{0};      // wwe2k14_rdr_gpu_marker: 0 off, 1 class runs + the present, 2 passes / dispatches / present / copy runs, 3 every step, 4 every step with BEGIN words only (no barriers)
// Test only (rplay): the frame serial and step to stall at (-1 = off), the plant (1 marker off by one, 2 a crumb left out, 3 presents counted after the loss)
inline std::atomic<int64_t> g_selftest_frame{-1};
inline std::atomic<int32_t> g_selftest_step{0};
inline std::atomic<int> g_plant{0};       // 1 a crumb names the next marker, 2 a crumb left out, 3 presents counted after the loss, 4 END words without barriers
inline std::atomic<uint64_t> g_presented_at_loss{0};
// Offline instrument (rplay --role-audit FILE, DEVICE_LOST.md section 10): one JSON line per recorded step with the images it touches and their state BEFORE the
// step (u = every subresource still VK_IMAGE_LAYOUT_UNDEFINED: never written by the GPU in this run), for tools\renderer\role_audit.py. Null in the game.
inline std::atomic<std::FILE*> g_role_audit{nullptr};
// S102 D1 round 6 (DEVICE_LOST.md section 13), wwe2k14_rdr_frame_top_barrier, read every frame: 0 off (the stream byte for byte as before); 1 one full memory barrier
// (all commands -> all commands, every write -> every read / write) at the top of each frame's command buffer; 2 also one before the present step (or at the end
// when the frame presents nothing). The cheap test of a cross-frame / timing hazard. Test only: g_frame_barrier_plant 1 = the switch is on but no barrier is recorded.
inline std::atomic<int> g_frame_top_barrier{0};
inline std::atomic<int> g_frame_barrier_plant{0};
inline std::atomic<uint64_t> g_frame_barrier_frames{0}, g_frame_barrier_count{0};  // rplay: rdr::g_presented when the (simulated) loss was handled

constexpr uint32_t kFrames = 8;          // frames kept (frames in flight are 2; the frame whose submit failed and the ones before it)
constexpr uint32_t kSteps = 320;         // steps kept a frame (a Brock frame records ~105; more are counted, not kept)
constexpr uint32_t kBuilt = 16;          // pipelines built, the last ones
constexpr uint32_t kMarkerWords = 512;   // marker words a frame slot (2 KB): word 0 + BEGIN / END of markers 1..kMaxMarker
constexpr uint32_t kMaxMarker = kMarkerWords / 2 - 1;
inline uint32_t BeginWord(uint32_t k) { return 2 * k; }
inline uint32_t EndWord(uint32_t k) { return 2 * k + 1; }
constexpr uint32_t kMarkUploads = 1;     // marker 1: the uploads (before ApplyUploads)
constexpr uint32_t kMarkFirstStep = 2;   // marker 2: the first marked step
constexpr uint16_t kNoMarker = 0xFFFF;

enum StepKind : uint8_t { kPass = 0, kCopy = 1, kPresent = 2, kDispatch = 5, kCopyBatch = 6 };
inline const char* StepKindName(uint8_t k) {
  switch (k) {
    case kPass: return "pass";
    case kCopy: return "copy";
    case kPresent: return "present";
    case kDispatch: return "dispatch";
    case kCopyBatch: return "copy batch";
    default: return "?";
  }
}

struct Step {
  uint8_t kind = 0;
  uint8_t cls = 0;            // GpuClass of the step (its first target / destination)
  uint16_t marker = kNoMarker;  // the marker word written before this step (kNoMarker: none, mode 1 inside a copy run)
  uint16_t first_prog = 0xFFFF, last_prog = 0xFFFF;  // a pass's first and last recorded draw's program (0xFFFF: none)
  uint32_t step = 0;          // the step's number in the frame's step list
  uint32_t index = 0;         // the pass / copy / dispatch / present number in its own list
  uint32_t image = 0;         // first colour target (else depth) of a pass, destination of a copy, first cube of a dispatch
  uint32_t draws = 0;         // a pass: draws recorded (indexed + plain); a copy batch: copies taken
  uint32_t binds = 0;         // a pass: pipeline binds
  uint32_t prims = 0;         // a pass: vertices + indices recorded (saturated)
};

struct Frame {
  uint64_t serial = ~0ull;
  uint32_t slot = 0;
  uint8_t mode = 0;           // RenderMode
  uint8_t marker_mode = 0;    // the marker mode this frame was recorded with
  uint16_t end_marker = kNoMarker;  // the marker after the last step and the read-backs (kNoMarker: no markers)
  uint32_t steps_total = 0;   // steps recorded (may exceed kSteps)
  uint32_t n = 0;             // steps kept
  uint32_t uploads = 0, destroys = 0, built = 0;  // the frame's image / buffer uploads, destroys processed, pipelines built while it was recorded
  bool submitted = false;     // vkQueueSubmit returned VK_SUCCESS
  Step steps[kSteps];
};

struct Built {
  uint64_t serial = 0;
  uint16_t program = 0xFFFF;
  uint8_t cls = 0;
};

// The ring. Render thread only (Render and the pipeline builds it calls), so no locks.
struct Ring {
  Frame frames[kFrames];
  Built built[kBuilt];
  uint64_t built_count = 0;

  Frame& Begin(uint64_t serial, uint32_t slot, uint8_t mode, uint8_t marker_mode) {
    Frame& f = frames[serial % kFrames];
    f.serial = serial, f.slot = slot, f.mode = mode, f.marker_mode = marker_mode;
    f.end_marker = kNoMarker, f.steps_total = 0, f.n = 0, f.uploads = f.destroys = f.built = 0, f.submitted = false;
    return f;
  }
  // The next step's record, or nullptr when the frame already holds kSteps (still counted).
  static Step* Add(Frame& f) {
    ++f.steps_total;
    if (f.n >= kSteps) return nullptr;
    Step& s = f.steps[f.n++];
    s = Step{};
    return &s;
  }
  void NoteBuilt(uint64_t serial, uint16_t program, uint8_t cls) {
    Built& b = built[built_count++ % kBuilt];
    b.serial = serial, b.program = program, b.cls = cls;
  }
  const Frame* Find(uint64_t serial) const {
    const Frame& f = frames[serial % kFrames];
    return f.serial == serial ? &f : nullptr;
  }
  const Frame* FindLow(uint32_t serial_lo) const {  // by the marker word (the serial's low 32 bits)
    for (const Frame& f : frames)
      if (f.serial != ~0ull && uint32_t(f.serial) == serial_lo) return &f;
    return nullptr;
  }
};

// The value a marker word holds: the frame's serial, low 32 bits, never 0 (a word never written reads 0).
inline uint32_t MarkValue(uint64_t serial) { return uint32_t(serial) | (uint32_t(serial) == 0 ? 0x80000000u : 0u); }

// One slot's marker words read back: which frame it holds and how far the GPU got.
struct SlotReach {
  bool any = false;           // word 0 was ever written
  uint32_t serial_lo = 0;     // word 0
  int32_t begun = -1;         // the highest marker whose BEGIN holds the serial (the front end's reach; -1 none)
  int32_t ended = -1;         // the highest marker whose END holds the serial (its work finished; -1 none)
  int32_t unfinished = 1;     // the first marker not finished: ended + 1 (the fault is in it or after it)
  bool done = false;          // the frame's end marker finished
};

// Reads one slot's words. `end_marker` is the frame's end marker (from the ring; kNoMarker when unknown).
inline SlotReach Reach(const uint32_t* words, uint16_t end_marker) {
  SlotReach r;
  if (!words || !words[0]) return r;
  r.any = true;
  r.serial_lo = words[0];
  for (uint32_t k = 1; k <= kMaxMarker; ++k) {
    if (words[BeginWord(k)] == r.serial_lo) r.begun = int32_t(k);
    if (words[EndWord(k)] == r.serial_lo) r.ended = int32_t(k);
  }
  r.unfinished = r.ended < 0 ? 1 : r.ended + 1;
  r.done = end_marker != kNoMarker && r.ended == int32_t(end_marker);
  return r;
}

// The step a marker names in a frame (nullptr: none, e.g. the uploads or the end).
inline const Step* StepOfMarker(const Frame& f, int32_t marker) {
  if (marker < int32_t(kMarkFirstStep)) return nullptr;
  for (uint32_t i = 0; i < f.n; ++i)
    if (f.steps[i].marker == uint16_t(marker)) return &f.steps[i];
  return nullptr;
}

}  // namespace crumbs
}  // namespace rdr
