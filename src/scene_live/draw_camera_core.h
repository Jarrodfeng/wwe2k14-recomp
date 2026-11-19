// The draw thread's camera (clocks: Note is stamped on the DRAW-HALF clock = the record frame j; Get compares on the record clock, S103 CCF) (Session 103, lane WC, R5; docs\research\s103\WNCC_adversary.md section 3 and R5, the SOURCE OF TRUTH). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// Pure: no game, SDK or renderer includes. The body draw sub_82C04DF0 takes its camera from sub_82BA7748() = [0x83929A40 + 4 * slot], slot = [[[r13]+1048]+32] (0 when [[r13]+1048] is 0) [C listing sub_82BA7748; the same helper
// caster_core.h ReadThreadSlot computes]. The raw hook on sub_82C04DF0 (draw_origin.h) calls NoteDraw ONCE per call, BEFORE its Ours return, with the caller's r13: that is the very camera the game's draw reads. The camera object holds
// the VIEW at +240 and the PROJECTION at +176 (sub_82BB3748 +13C / sub_829352D8 +EC [C]); the HUD's ortho lands in +176 too and fails IsPerspective (inputs_core.h), so the 2D camera is never cached.
// Why: the scene manager's copy (ReadSceneCamera, mgr [0x836F3AA8]) is null in the CAW editor (reason 1), so that frame had no camera at all; this is the fallback the latch (inputs_contrib.h Take) uses for reason 1 only.
// Read only; one cheap atomic test per call once the frame has its camera; at most kMaxAttempts reads per frame while it has none (the HUD camera is installed for part of a frame).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <mutex>

#include "caster_core.h"   // ReadThreadSlot
#include "inputs_core.h"   // ReadCamera, kCamTable

namespace rdr_scene::live::drawcam {

inline constexpr uint32_t kMaxAttempts = 8, kSampleEvery = 300;   // reads per hook frame while no camera was accepted (a frame draws hundreds of models; the HUD camera fails the perspective test)
// The shadow module (S103 CCF; sub_827E93D8 stores its `this` at 0x83924748; sub_827E7DF0(this, desc, record) installs record = this + 304 or this + 1312 as the CURRENT camera of the thread slot through sub_82BA7DE0,
// so during the shadow-map render the table slot holds a LIGHT camera [C q decomp sub_827E7DF0 +DC, sub_827E93D8 +8C/+94/+1EC, sub_827E95D8 order in docs\systems\shadows.md]. this + 2320 is the module's own main-camera copy (never installed).
inline constexpr uint32_t kShadowModulePtr = 0x83924748, kLightRecordA = 304, kLightRecordB = 1312, kModuleCamera = 2320, kFlagA = 0x83929A48;   // kFlagA: per-slot "inside a shadow-map render" word (sub_82BA7778 sets it in the props pass sub_827E7F00, map A and map B sub_827E8080 [PMW.md]: map B also draws categories 26 + 41 in mode 0)
inline constexpr uint32_t kSlots = 4, kMainSlot = 0;   // BHF1 B1: thread slots tracked (>= 3 share [3]); in INLINE mode the main walk (sub_82C3B4B8) runs as job 0: its [[r13]+1048] is job 0 and slot = [job0+32] = 0; at most TWO walks run at once (job 0 and CParallelDraw1, sub_82C3B640) [L: from the render-job layout; the census `slots a/b/c/d` and `fallback_slot` show it]
inline constexpr uint64_t kMaxAge = 1;        // a cached camera, noted in guest-clock period j (the record frame j), answers the latch of record j or j + 1

struct DrawCameraStats {
  uint64_t calls = 0, accepted = 0, rejected = 0, slot_unread = 0, used = 0, used_age1 = 0, none = 0, ahead = 0, old = 0, light_skipped = 0, fallback_slot = 0, flag_a_accepted = 0, module_cam_accepted = 0;   // ahead: noted after the latch's record (frame_ > record); old: record - frame_ > kMaxAge
  uint32_t last_slot = 0, last_object = 0, last_reason = 0;
  // The census sample (every kSampleEvery-th accepted camera): which object the draw thread's camera was, and what the shadow module / flag A said at that moment.
  struct Sample { uint64_t frame = 0; uint32_t object = 0, module = 0, flag_a = 0, slot = 0; float eye[3] = {}, near_z = 0, far_z = 0; bool valid = false; } sample;
  uint32_t slot_seen[4] = {};   // thread slot histogram of accepted cameras (slot >= 3 counted in [3])
};

// Test only (tools\scene\inputs_unit.cpp): 1 = the thread slot is ignored (slot 0 always); 2 = the cache ignores its frame (any age answers); 3 = Get compares against record - 1 (the old hook-clock bug, S103 CCF); 4 = the shadow light cameras are not skipped.
inline int g_drawcam_plant = 0;

class DrawCamera {
 public:
  void Reset() {   // tests
    std::lock_guard<std::mutex> lk(mu_);
    for (uint32_t i = 0; i < kSlots; ++i) have_[i] = false, frame_[i] = 0, attempt_frame_[i] = ~0ull, attempts_[i] = 0, done_frame_[i].store(~0ull, std::memory_order_release);
    st_ = DrawCameraStats{};
    calls_.store(0, std::memory_order_relaxed);
    t_slot_cache() = SlotCache{};
  }
  // The hook side (a game draw thread). `read` copies raw big-endian guest bytes (scene_inputs::ReadFn). Returns true when this call accepted a camera.
  bool Note(scene_inputs::ReadFn read, uint32_t r13, uint64_t frame) {
    calls_.fetch_add(1, std::memory_order_relaxed);
    auto rw = [read](uint32_t a, uint32_t& v) { return read && scene_inputs::ReadWord(read, a, v); };
    // BHF1 B1: one camera per thread slot. The slot of the calling thread is read once per (thread, r13, frame) (a thread keeps its slot), so the per-call cost stays one atomic test.
    SlotCache& sc = t_slot_cache();
    if (sc.r13 != r13 || sc.frame != frame) {
      uint32_t sl = 0;
      sc.ok = casters::ReadThreadSlot(rw, r13, sl), sc.slot = sl, sc.r13 = r13, sc.frame = frame;
    }
    if (!sc.ok) {
      std::lock_guard<std::mutex> lk(mu_);
      return ++st_.slot_unread, false;
    }
    uint32_t slot = g_drawcam_plant == 1 ? 0 : sc.slot;
    const uint32_t si = slot < kSlots ? slot : kSlots - 1;
    if (done_frame_[si].load(std::memory_order_acquire) == frame) return false;   // this thread slot already has its camera for the frame
    std::lock_guard<std::mutex> lk(mu_);
    if (attempt_frame_[si] != frame) attempt_frame_[si] = frame, attempts_[si] = 0;
    if (attempts_[si] >= kMaxAttempts) return done_frame_[si].store(frame, std::memory_order_release), false;   // BHF1 fix: reads used up for this slot and frame: later calls take the lock-free early exit
    ++attempts_[si];
    uint32_t obj = 0, mod = 0, flag_a = 0;
    rw(scene_inputs::kCamTable + 4u * slot, obj), rw(kShadowModulePtr, mod), rw(kFlagA + 4u * slot, flag_a);
    if (g_drawcam_plant != 4 && mod && obj && (obj == mod + kLightRecordA || obj == mod + kLightRecordB)) return ++st_.light_skipped, --attempts_[si], false;   // a shadow-map light camera: not the frame's camera; it does not use up the frame's reads
    rdr_scene::Camera c;
    scene_inputs::CameraInfo info;
    if (!scene_inputs::ReadCamera(read, c, info, scene_inputs::kCamTable + 4u * slot)) return ++st_.rejected, st_.last_reason = info.reason, false;
    cam_[si] = c, frame_[si] = frame, have_[si] = true;
    st_.last_slot = slot, st_.last_object = info.object;
    ++st_.accepted, ++st_.slot_seen[slot < 3 ? slot : 3];
    if (flag_a) ++st_.flag_a_accepted;
    if (mod && obj == mod + kModuleCamera) ++st_.module_cam_accepted;
    if (st_.accepted % kSampleEvery == 1) st_.sample = {frame, info.object, mod, flag_a, slot, {c.eye[0], c.eye[1], c.eye[2]}, c.near_z, c.far_z, true};
    done_frame_[si].store(frame, std::memory_order_release);
    return true;
  }
  // The latch side: the camera the draw thread noted in guest-clock period `record_frame` (the record's frame; Note stamps on that same clock) or the one before. False = none.
  bool Get(uint64_t record_frame, rdr_scene::Camera& out) {   // BHF1 B1: answers from the MAIN walk's slot (kMainSlot) when it has a camera inside the window; only when it has none does the lowest other slot with one answer (counted: fallback_slot), never "first come"
    std::lock_guard<std::mutex> lk(mu_);
    bool any = false, ahead = false, old = false;
    const uint64_t rec = (g_drawcam_plant == 3 && record_frame) ? record_frame - 1 : record_frame;   // plant 3: the old bug (compared on the hook clock)
    for (uint32_t k = 0; k < kSlots; ++k) {
      const uint32_t i = (kMainSlot + k) % kSlots;   // the main slot first, then ascending
      if (!have_[i]) continue;
      if (i != kMainSlot && have_[kMainSlot]) break;   // BHF1 fix: a worker's camera answers ONLY when the main slot never had one; a main camera that is ahead / old is a miss (counted), as before
      any = true;
      if (g_drawcam_plant != 2) {
        if (frame_[i] > rec) { ahead = true; continue; }
        if (rec - frame_[i] > kMaxAge) { old = true; continue; }
      }
      out = cam_[i];
      ++st_.used;
      if (frame_[i] != record_frame) ++st_.used_age1;
      if (i != kMainSlot) ++st_.fallback_slot;
      return true;
    }
    if (!any) return ++st_.none, false;
    if (ahead) ++st_.ahead; else if (old) ++st_.old;
    return false;
  }
  static std::string SampleText(const DrawCameraStats& s) {   // the sample every ~300 accepted cameras: object, eye, near / far, the shadow module and what it says of this object, flag A
    if (!s.sample.valid) return "";
    const auto& m = s.sample;
    char b[320];
    std::snprintf(b, sizeof b, " sample{frame %llu slot %u object 0x%08X eye %.4g %.4g %.4g near %.4g far %.4g module 0x%08X = light A %d light B %d module_cam %d flagA %u}", (unsigned long long)m.frame, m.slot, m.object, m.eye[0], m.eye[1], m.eye[2], m.near_z, m.far_z, m.module,
                  int(m.module && m.object == m.module + kLightRecordA), int(m.module && m.object == m.module + kLightRecordB), int(m.module && m.object == m.module + kModuleCamera), m.flag_a);
    return b;
  }
  // S103 CAWB (CAWV Q3): the guest frame of the last actor drawn from a guest model (the CAW editor's body): the draw-driven census bits end here, not at the last record (the saving screen has no 3D).
  void NoteBodyDraw(uint64_t guest_frame) { last_body_draw_.store(guest_frame, std::memory_order_relaxed); }
  uint64_t LastBodyDraw() const { return last_body_draw_.load(std::memory_order_relaxed); }
  DrawCameraStats Stats() {
    std::lock_guard<std::mutex> lk(mu_);
    DrawCameraStats s = st_;
    s.calls = calls_.load(std::memory_order_relaxed);
    return s;
  }
  std::string CensusText() {
    const DrawCameraStats s = Stats();
    return " draw_camera source thread-slot: calls " + std::to_string((unsigned long long)s.calls) + " accepted " + std::to_string((unsigned long long)s.accepted) + " rejected " + std::to_string((unsigned long long)s.rejected) + " (last reason " +
           std::to_string(s.last_reason) + ") slot_unread " + std::to_string((unsigned long long)s.slot_unread) + " used " + std::to_string((unsigned long long)s.used) + " (age 1: " + std::to_string((unsigned long long)s.used_age1) +
           ") none " + std::to_string((unsigned long long)s.none) + " ahead " + std::to_string((unsigned long long)s.ahead) + " old " + std::to_string((unsigned long long)s.old) + " light_skipped " + std::to_string((unsigned long long)s.light_skipped) + " fallback_slot " + std::to_string((unsigned long long)s.fallback_slot) + " flag_a_accepted " + std::to_string((unsigned long long)s.flag_a_accepted) +
           " module_cam_accepted " + std::to_string((unsigned long long)s.module_cam_accepted) + SampleText(s) + " slots " + std::to_string(s.slot_seen[0]) + "/" + std::to_string(s.slot_seen[1]) + "/" + std::to_string(s.slot_seen[2]) + "/" + std::to_string(s.slot_seen[3]) + " last_body_draw " + std::to_string((unsigned long long)LastBodyDraw());
  }

 private:
  std::atomic<uint64_t> calls_{0};
  struct SlotCache { uint32_t r13 = 0; uint64_t frame = ~0ull; uint32_t slot = 0; bool ok = false; };
  static SlotCache& t_slot_cache() {
    static thread_local SlotCache c;
    return c;
  }
  std::atomic<uint64_t> done_frame_[kSlots] = {~0ull, ~0ull, ~0ull, ~0ull};
  std::atomic<uint64_t> last_body_draw_{0};   // S103 CAWB
  std::mutex mu_;
  rdr_scene::Camera cam_[kSlots];
  uint64_t frame_[kSlots] = {}, attempt_frame_[kSlots] = {~0ull, ~0ull, ~0ull, ~0ull};
  uint32_t attempts_[kSlots] = {};
  bool have_[kSlots] = {};
  DrawCameraStats st_;
};

inline DrawCamera& Cam() {
  static DrawCamera c;
  return c;
}

// S103 CAE: the callable the shared camera read takes (inputs_core.h ReadSceneCameraOrDraw): the draw thread's camera on the record clock.
inline bool GetForLatch(uint64_t record_frame, rdr_scene::Camera& out) { return Cam().Get(record_frame, out); }

}  // namespace rdr_scene::live::drawcam
