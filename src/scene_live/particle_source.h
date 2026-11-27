// The particle source (Session 103, lane F2): the game side of route 2, in a pure header. When the writer builds a record (at the latch, or in Emit when no latch ran) it reads the
// particle node's live entries / items from guest memory through a ReadFn (the real GuestMem path) and expands the billboards against the GAME-space camera, into a ParticleSnapshot
// stamped with the HOOK clock's frame (scene_frame::HookFrameOf). Read only: nothing is written to the guest. No game or SDK includes (the glue, particle_glue.h, gives it the guest
// reader and the node address); tools\scene\f_test.cpp drives it on a synthetic memory.
//
// Mode 0 (every player's default) costs one relaxed load: Take returns at once.
//
// THE PASS IS NOT WHOLE YET, ON PURPOSE (FRAME_CONTRACT 3, the main session's decision): a particle pass that ran but could not be placed is a MISSING INPUT. The snapshot carries a
// ParticleStatus (particle_tables.h) into the record so the render side can turn it into a hole. The kInParticles stamp stays WITHHELD (kStampParticles == false).
// Lane F3 (2026-11-16): the quad class, the 3x3 pivot grid, the handle tables, ZOffset and the sampler byte are now read from the listings; the stamp rule is written and behind the
// constant. What still blocks flipping it (each shows up in the pass's status, so a clean read cannot lie): (1) kPiTexture: no TextureNamer is installed (handle -> prtcls.pac
// entry needs the game side's content hash; SetNamer); (2) kPiBlend: an item's blend code no run proved (S103 F4: code 1 = additive is proven by run 22; 0 and 2 are not: BlendCodeProven); (3) the third
// method's items (0x82A0E3D8) are other-class (kPiOtherClass). Rule: stamp only when WholePass(status) (no reasons at all), then flip the constant and set `moved` for
// kInParticles in inputs_core.h.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "inputs_core.h"   // ReadFn, ReadSceneCameraOrDraw, Swap32, Capture
#include "draw_camera_core.h"   // S103 CAE: the draw thread's camera, the fallback for no scene manager (the CAW editor)
#include "particle_reader.h"
#include "presence_rules.h"   // S103 S2: the per-frame stamp rule (hold by presence)
#include "safe_guest_pages.h"   // S108 RDS: LineCopy (the pass's line copy)

namespace rdr_scene::live::particles {

inline constexpr bool kStampParticles = false;   // see the note above: NOT ready (texture namer, blend codes, the third method)
// Test only: 1 = the view handed to BasisFromView is the VIEWER-space S * view (S = diag(1, -1, 1)): the wiring fault the unit must catch. 2 (S103 F5) = the old pairing rule (noted == hook frame).
// S103 MP plants (tools\scene\mp_unit.cpp): 3 = TakeEmpty ignores a node call in the frame; 4 = the node's empty words are ignored; 5 = only the LAST node call of a screen frame is read; 6 = the scene camera is used on a screen frame
// (the screen-camera test is ignored); 7 = the 2D row position is not stamped on the batches; 8 = the last call alone decides "empty" (S103 MPBF); 9 = calls past the cap are no reason.
inline int g_source_plant = 0;
inline bool WholePass(const ParticleStatus& s) { return (s.reasons & ~uint32_t(rdr_scene::kPiInfoMask)) == 0 && s.unreadable == 0 && s.other_class == 0; }   // S103 MP: the info bits (the empty-pass marker, the pivot note) are information, not a reason

// ---- S103 MP: the node's own camera on a flat frame (docs\research\s103\MP_menu_particles.md sections 2-3; claims_MP MP6 / MP7)
// The node (sub_82254E30) takes its view-projection from the calling thread's current camera object (camera table 0x83929A40, slot = word +32 of [[r13]+1048], sub_82BA7748): sub_82BA8780 -> sub_82296630(out, cam+176, cam+240) = (+240) . (+176) [claims_MP MP6],
// NOT the scene manager's camera. On the title / menus that camera is the particle task's own render view made orthographic (S111 TITLESMOKE2, IsScreenViewProj below): diag(2/1280, -2/720, ~3.3e-6, 1)
// with a z translation m[14] of about 0.0033 [C listings; S103 MP's fview on c2d_title / menu / options / play / select read that translation as the z scale].
inline void MulRowMajor(const float a[16], const float b[16], float out[16]) {   // out = a * b, row-vector convention (p * a * b)
  float t[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
  std::memcpy(out, t, sizeof t);
}
// The screen camera test (S111 TITLESMOKE2, docs\research\s111\TITLESMOKE2.md). The game's menu particles are drawn with the particle task's own render view (sub_82939D20: eye at z -1000, no
// rotation), which the task's UI layer sub_82938FE8 makes orthographic (+480 = 1) and rebuilds (slot 11 sub_82939B08 -> sub_82BB3748 -> sub_82BB2DD0: 2 / w and 2 / h centred, row 1 negated,
// 1 / (far - near) and near / (near - far)) before it calls each bank's node. Its view . proj is diagonal in x / y with a Z TRANSLATION, (1000 - near) / (far - near) (about 0.0033 with the
// render view ctor's near 10 / far 3e5). So: no perspective column (m[3] = m[7] = m[11] = 0, m[15] = 1), no rotation and no x / y translation (every other off-diagonal 0), non-zero x / y / z
// scales; m[14] (depth only) may be any finite value. A 3D camera has a perspective column or a rotation and is refused. Plant 18 = the old strict rule (m[14] must be 0 too), which refused every
// real menu call (S111 run 001: screen_calls 0, a no_camera hole on every menu frame).
inline bool IsScreenViewProj(const float m[16]) {
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(m[i])) return false;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      if (r != c && !(r == 3 && c == 2 && g_source_plant != 18) && std::fabs(m[r * 4 + c]) > 1e-9f) return false;   // (r 3, c 2) = m[14], the depth translation
  return m[0] != 0.f && m[5] != 0.f && m[10] != 0.f && std::fabs(m[15] - 1.f) < 1e-6f;
}
// What the hook read at the node's entry (the glue fills it; tests build it). `rows` = the q2d tap's running row count of the frame (kNoRows = the tap is off; informational: ParticleScreen::row_count).
// S111 FLATSEQ: `seq2d` = the q2d store's last call-order ticket at the call (quads2d_tap.h SeqNow; kNoRows = the tap is off): a screen call's batches are stamped with it (flat_particles.h PosOf).
inline constexpr uint32_t kNoRows = 0xFFFFFFFFu;
struct NodeCall {
  uint32_t node = 0;
  uint32_t rows = kNoRows;
  uint32_t seq2d = kNoRows;
  bool have_camera = false;    // the camera table object was readable and non-null (a null camera: the node returns before drawing, 0x82254E68)
  bool camera_null = false;
  bool words_zero = false;     // +3084 or +3088 read 0 AT THE CALL (the entry hook): the node returns before drawing (0x82254E3C / 0x82254E4C). Read at the call, not at the latch: the words change in between. An unreadable word is not empty
  float view_proj[16] = {};
  float view[16] = {};         // S103 PRT fix 4: the node camera's own view (+176), for the scene-camera agreement check
  bool have_view = false;
  // S103 PRT (PNS.md 4, [C]): the global soft-particle switch [0x836EE38C] -> object, +32 on / off, +36 the range (float), read AT THE CALL with the words (the node reads it at 0x82254ED8 and draws T_SoftParticle with c211 = the range while it is non-zero).
  bool soft_on = false;
  float soft_range = 0.f;
};
inline constexpr size_t kMaxCallsPerFrame = 16;
inline constexpr uint32_t kEntryListGlobal = 0x83929A84u, kMaxEntryList = 1024u;   // S103 PRT fix 5 [C: sub_82254E30 +29C]
inline constexpr uint32_t kSoftSwitchGlobal = 0x836EE38Cu, kSoftSwitchOn = 32u, kSoftSwitchRange = 36u;
// The batch code of a call's soft switch: 0 = hard; else lround(range * 4) clamped to 1..255 (BatchKey::soft / ParticleBatch::soft; range = code / 4: Kane 30 -> 120, Undertaker 10 -> 40; no record layout change).
// S103 PRT fix 6: the range clamped at 63.75 (code 255).
inline bool SoftClamped(const NodeCall& c) { return c.soft_on && std::isfinite(c.soft_range) && c.soft_range * 4.f + 0.5f >= 256.f; }
// S103 PRT fix 4: a non-screen call whose own camera does not agree with the scene camera must not be placed with the scene camera. Compared: the rotation part of the two views (what the billboard basis uses),
// tolerance 0.05 per element (~3 degrees: the scene copy can be a frame older). A call with no camera read agrees (nothing to compare). Plant 13 = the check is skipped.
// S103 PRT fix2 (run 013: node_cam_mismatch 20799, nodes_read 0): on the real game the check rejected EVERY node, so it is COUNT-ONLY now (g_node_cam_skip false: a wrong skip hides real particles, worse than a wrong camera).
// The relation is split so the next log settles why: 0 = agrees, 1 = agrees with the TRANSPOSED scene rotation (a row / column layout difference), 2 = neither (counted as a mismatch; the first one is printed in the
// census). Only a relation 2 can skip, and only when g_node_cam_skip is set. S111 FLASHCAM review: until then the node's "view" was its PROJECTION (+176, the names in ReadNodeCamera were swapped), which is why
// every node counted as a mismatch (run 013); it now compares the node's view (+240).
inline bool g_node_cam_plant_off = false;
inline bool g_node_cam_skip = false;
inline int NodeCameraRelation(const NodeCall& c, const float scene_view[16]) {
  if (g_node_cam_plant_off || !c.have_view) return 0;
  bool same = true, tr = true;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) {
      same = same && std::fabs(c.view[r * 4 + k] - scene_view[r * 4 + k]) <= 0.05f;
      tr = tr && std::fabs(c.view[r * 4 + k] - scene_view[k * 4 + r]) <= 0.05f;
    }
  return same ? 0 : tr ? 1 : 2;
}
inline uint8_t SoftCodeOf(const NodeCall& c) {
  if (!c.soft_on || !std::isfinite(c.soft_range) || c.soft_range <= 0.f) return 0;
  const float q = std::floor(c.soft_range * 4.f + 0.5f);
  return uint8_t(q < 1.f ? 1.f : q > 255.f ? 255.f : q);
}

// The call is a screen-camera call (a flat frame's group).
inline bool IsScreenCall(const NodeCall& c) { return c.have_camera && IsScreenViewProj(c.view_proj); }
// The hook's read of the node's own camera (S103 MP, claims_MP MP6): r13 -> [r13] -> +1048 -> block, slot = block +32, camera object = [0x83929A40 + 4 * slot] (sub_82BA7748), VIEW +240, PROJECTION +176 (S111 FLASHCAM
// review: the names were the other way round; the flash test sub_823B2470 takes its view z from +240, and inputs_core.h kCamView / kCamProj agree). Read only; a failed
// read leaves have_camera false (the call is then not a screen call and the old scene-camera path applies); a null object sets camera_null (the node returns at 0x82254E68).
inline constexpr uint32_t kCameraTable = 0x83929A40u, kCamObjView = 240u, kCamObjProj = 176u, kMaxCameraSlot = 256u;
inline void ReadNodeCamera(scene_inputs::ReadFn read, uint32_t r13, NodeCall& c) {
  c.have_camera = false, c.camera_null = false, c.have_view = false;
  uint32_t tb = 0, blk = 0, slot = 0, cam = 0;
  if (!read || !scene_inputs::ReadWord(read, r13, tb) || !tb || !scene_inputs::ReadWord(read, tb + 1048u, blk)) return;
  if (blk && !scene_inputs::ReadWord(read, blk + 32u, slot)) return;
  if (slot >= kMaxCameraSlot || !scene_inputs::ReadWord(read, kCameraTable + 4u * slot, cam)) return;
  if (!cam) { c.camera_null = true; return; }
  rdr_scene::Mat4 v, p;
  if (!scene_inputs::ReadMat(read, cam + kCamObjView, v) || !scene_inputs::ReadMat(read, cam + kCamObjProj, p)) return;
  std::memcpy(c.view, v.m, sizeof c.view), c.have_view = true;
  MulRowMajor(v.m, p.m, c.view_proj);   // the game's own order: sub_82296630(out, cam+176, cam+240) computes +240 . +176 = view . projection (claims_MP MP6)
  c.have_camera = true;
}
// The node does not draw when its word +3084 or +3088 is 0 (0x82254E3C / 0x82254E4C) or the camera object is null (0x82254E68). The words are read AT THE CALL (ReadNodeWords, the entry hook) into the call; True only when
// they READ and one is 0 (an unreadable word is not "empty"). Plant 4: the words are ignored.
inline bool NodeDrawsNothing(const NodeCall& c) {
  if (c.camera_null) return true;
  if (g_source_plant == 4) return false;
  return c.words_zero;
}
inline void ReadNodeWords(scene_inputs::ReadFn read, uint32_t node, NodeCall& c) {
  c.words_zero = false;
  c.soft_on = false, c.soft_range = 0.f;   // S103 PRT: the soft switch, read with the words (an unreadable global = hard)
  {
    uint32_t obj = 0, on = 0, rb = 0;
    if (read && scene_inputs::ReadWord(read, kSoftSwitchGlobal, obj) && obj >= 0x40000000u && obj < 0xE0000000u && scene_inputs::ReadWord(read, obj + kSoftSwitchOn, on) && on != 0 && scene_inputs::ReadWord(read, obj + kSoftSwitchRange, rb)) {
      std::memcpy(&c.soft_range, &rb, 4);
      c.soft_on = true;
    }
  }
  uint32_t a = 0, b = 0;
  if (!read || !node || node > 0xFFFFFFFCu - 3088u || !scene_inputs::ReadWord(read, node + 3084u, a) || !scene_inputs::ReadWord(read, node + 3088u, b)) return;
  c.words_zero = a == 0 || b == 0;
}

// GuestMem over a ReadFn (copies n raw big-endian guest bytes, false when unreadable): the real path the game uses. The 4-byte range rule: a read whose range wraps mod 2^32 is refused here
// as well as in the reader.
// S108 RDS phase 2: `lines` = the pass's line copy (safe_guest_pages.h LineCopy: the 128-byte aligned line of a word is read once per pass, in the word's own page; a refused line is the word read). Owned by
// the caller for ONE pass (Take / TakeScreen); null = one checked read per word (the namer's long-lived instance must never carry lines across frames). ReadItem / ReadNode / ReadEntry read their words one
// by one (a key's 16-byte records word by word, a node's state byte four times per word): ~1,300 page checks a frame in Fandango's entrance (S108 RDS tally, thread 19F2).
class FnGuestMem final : public GuestMem {
 public:
  explicit FnGuestMem(scene_inputs::ReadFn read, wwe2k14::safe_pages::LineCopy<>* lines = nullptr) : read_(read), lines_(lines) {}
  bool Read32(uint32_t guest, uint32_t& out) const override {
    if (!read_ || guest > 0xFFFFFFFCu) return false;
    uint32_t raw = 0;
    const scene_inputs::ReadFn rd = read_;
    if (!(lines_ ? lines_->Get(guest, 4, &raw, [rd](uint32_t a, uint32_t n, void* d) { return rd(a, d, n); }) : rd(guest, &raw, 4))) return false;
    out = scene_inputs::Swap32(raw);
    return true;
  }
 private:
  scene_inputs::ReadFn read_;
  wwe2k14::safe_pages::LineCopy<>* lines_ = nullptr;
};


class ParticleSource {
 public:
  void SetReader(scene_inputs::ReadFn r) { read_ = r; }
  // F3: the game side's handle -> file namer for the slot-33 texture handles (null = every texture stays unresolved, counted, kPiTexture). Not owned.
  void SetNamer(const TextureNamer* n) { namer_ = n; }
  // Called by the node's render-entry hook with r3 (the pass object) and the hook clock's frame (the glue reads the clock only when the scene switch is on); relaxed stores.
  // B4: a second DIFFERENT r3 in the same hook frame is counted (nodes_extra): the source reads the last one. (Test / old path: the words are read here, at the call, with the source's reader.)
  void NoteNode(uint32_t node, uint64_t hook_frame) {
    NodeCall c;
    c.node = node;
    if (scene_frame::CurrentMode() != scene_frame::Mode::Off) ReadNodeWords(read_, node, c);   // mode 0: no guest read at all
    NoteCall(c, hook_frame);
  }
  // S103 MP (B): EVERY call of the frame is kept (menus draw two groups a frame), in call order, with the q2d tap's row count at the call and the node's own camera. A call of another frame starts a new list.
  // Everything the readers decide on (the list, the frame it belongs to, the last node) is stored INSIDE the lock, so a reader that holds it sees one consistent state.
  void NoteCall(const NodeCall& c, uint64_t hook_frame) {
    std::lock_guard<std::mutex> lock(calls_mu_);
    if (noted_.load(std::memory_order_relaxed) == hook_frame) {
      if (node_.load(std::memory_order_relaxed) != c.node && node_.load(std::memory_order_relaxed) != 0) nodes_extra_.fetch_add(1, std::memory_order_relaxed);
      for (const NodeCall& o : calls_)
        if (o.node == c.node) { same_node_twice_.fetch_add(1, std::memory_order_relaxed); break; }   // S103 MPBF: the same r3 called twice in one frame
    }
    if (calls_frame_ != hook_frame) calls_.clear(), calls_frame_ = hook_frame, calls_over_flag_ = false, (scene_frame::CurrentMode() != scene_frame::Mode::Off ? ReadEntryList() : void());   // the entry list: once a frame, at its first call (under the lock)
    if (calls_.size() < kMaxCallsPerFrame) calls_.push_back(c);
    else calls_over_.fetch_add(1, std::memory_order_relaxed), calls_over_flag_ = true;
    node_.store(c.node, std::memory_order_relaxed);
    noted_.store(hook_frame, std::memory_order_relaxed);
  }
  // S103 PRT fix 5: the game's entry list [[0x83929A84] + 24] (count + 36), sorted. Fail open: an unreadable object / pointer / an empty or oversized list = no filter (entry_list_open counts it).
  void ReadEntryList() {
    wwe2k14::safe_pages::PassLines pass_lines;   // S108 RDS: the list's words through this pass's line copy (the game's ReadGuest; ~256 checks a frame were one a word)
    list_.clear();
    uint32_t obj = 0, ptr = 0, n = 0;
    if (!read_ || !scene_inputs::ReadWord(read_, kEntryListGlobal, obj) || obj < 0x40000000u || obj >= 0xE0000000u || !scene_inputs::ReadWord(read_, obj + 24u, ptr) || !scene_inputs::ReadWord(read_, obj + 36u, n) ||
        !ptr || n == 0 || n > kMaxEntryList) {
      entry_list_open_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t w = 0;
      if (!scene_inputs::ReadWord(read_, ptr + 4u * i, w)) { list_.clear(), entry_list_open_.fetch_add(1, std::memory_order_relaxed); return; }
      list_.push_back(w);
    }
    std::sort(list_.begin(), list_.end());
  }
  // S103 MP (A): the paired frame's EMPTY-PASS verdict, for a frame Take took nothing from. True = `out` is an empty snapshot (kPiEmptyPass, stamped with the hook clock). The same pairing rule as Take (the writer's frame, F5).
  // Never stamps an input and never touches a census a match reads: it only lets the render side say Parts::Whole on a flat frame.
  // Decided under calls_mu_ (S103 MPBF 3): a call in the frame (calls_frame_ == want) is never empty. EMPTY only when the node has never been called, or the last call is at least TWO frames behind (noted + 1 < want):
  // the frame right after the particles stop (noted == want - 1) and a note from a later frame (the draw task is ahead: the frame's own list is gone) stay NotRecorded (fail closed).
  bool TakeEmpty(uint64_t writer_frame, ParticleSnapshot& out) {
    if (scene_frame::CurrentMode() == scene_frame::Mode::Off || !read_) return false;
    const uint64_t want = g_source_plant == 2 ? scene_frame::HookFrameOf(writer_frame) : writer_frame;
    if (g_source_plant != 3) {   // (plant 3: a node call in the frame is ignored)
      std::lock_guard<std::mutex> lock(calls_mu_);
      if (calls_frame_ == want) return false;   // a call ran in the frame: Take's business
      const uint64_t noted = noted_.load(std::memory_order_relaxed);
      if (node_.load(std::memory_order_relaxed) != 0 && !(noted + 1 < want)) return false;   // the first frame after the particles stop, or a later note: not recorded
    }
    out.Clear();
    out.guest_frame = scene_frame::HookFrameOf(writer_frame);
    out.status.reasons = rdr_scene::kPiEmptyPass;
    empty_frames_.fetch_add(1, std::memory_order_relaxed);
    presence::Particles().NoteEmptyPass();   // S103 MPBF 8: the stream-draws-on-empty column for TakeEmpty frames (never a verdict a match reads)
    return true;
  }

  // S103 CAE: the scene camera of a frame the pass drew in: the composer's copy, else (no scene manager at all: the CAW editor) the draw thread's camera on the record clock (inputs_core.h
  // ReadSceneCameraOrDraw, the same read the inputs latch uses). Plant 10 (unit only): the old read, no fallback.
  bool SceneCamera(uint64_t writer_frame, rdr_scene::Camera& cam, scene_inputs::CameraInfo& ci) const {
    if (g_source_plant == 10) return scene_inputs::ReadSceneCamera(read_, cam, ci);
    return scene_inputs::ReadSceneCameraOrDraw(read_, writer_frame, &drawcam::GetForLatch, cam, ci);
  }

  // The writer's call. True = `out` holds this frame's snapshot (possibly empty, possibly with a not-whole status), stamped HookFrameOf(writer_frame). False = nothing taken (mode 0, no node yet,
  // the pass did not run in the paired hook frame).
  bool Take(uint64_t writer_frame, ParticleSnapshot& out) {
    if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return false;   // one relaxed load
    wwe2k14::safe_pages::PassLines pass_lines;   // S108 RDS review: the texture namer's handle walks of this pass (WalkTextureHandle through its own long-lived FnGuestMem = the game's ReadGuest) share this pass's line copy; TakeScreen runs inside it
    out.Clear();
    const uint64_t hook = scene_frame::HookFrameOf(writer_frame);
    const uint64_t want_noted = g_source_plant == 2 ? hook : writer_frame;   // plant 2 = the old rule (see below)
    uint32_t node = 0;
    uint64_t noted = 0;
    bool over = false;
    {   // one consistent read of the note (the draw tasks keep noting): the last node, its frame, and a copy of the frame's calls into the member buffer (no allocation once grown)
      std::lock_guard<std::mutex> lock(calls_mu_);
      node = node_.load(std::memory_order_relaxed), noted = noted_.load(std::memory_order_relaxed);
      calls_buf_.clear();
      list_buf_.clear();
      if (noted == want_noted && calls_frame_ == noted) calls_buf_ = calls_, over = calls_over_flag_, list_buf_ = list_;
    }
    if (g_source_plant == 9) over = false;   // plant 9: calls past the cap are not a reason
    const std::vector<NodeCall>& calls = calls_buf_;
    bool any_screen = false;
    for (const NodeCall& c : calls) any_screen = any_screen || (IsScreenCall(c) && g_source_plant != 6);   // (plant 6: no screen test)
    if (!node || !read_) { no_node_.fetch_add(1, std::memory_order_relaxed); return false; }
    // B3: the pass must have run in EXACTLY the paired frame (S103 F5: the writer's frame, see below). The census buckets say how the noted frame and the record's hook frame pair up (the trace run proves it).
    if (noted == hook) pair_0_.fetch_add(1, std::memory_order_relaxed);
    else if (noted == hook + 1) pair_plus1_.fetch_add(1, std::memory_order_relaxed);
    else if (noted < hook) pair_minus_.fetch_add(1, std::memory_order_relaxed);
    else pair_other_.fetch_add(1, std::memory_order_relaxed);
    // S103 F5 (the cause of "our frame draws no particles"): the node is a DRAW-half task and reads g_guest_frame AFTER the swap published the next frame, so a pass that ran in the
    // walks this latch closes is noted at the WRITER's frame (= hook + 1): runs 28 / 29 [C census]: pair (noted-hook) +1 12930 / 12924 of ~13000 latches, 0 in bucket 0, taken 0, stale_node 12986.
    // The old rule (noted == hook) refused every one of them. Same clock as ring_recompute_contrib.h (run 14: every one of 2239 notes was +1). The snapshot keeps the HOOK stamp (the contributor's rule).
    if (noted != want_noted) { stale_node_.fetch_add(1, std::memory_order_relaxed); return false; }   // the pass did not run in that frame: nothing to draw, not a hole
    out.guest_frame = hook;
    if (any_screen) return TakeScreen(calls, over, hook, writer_frame, out);   // S103 MP: a frame with a screen-camera call: every call is its own group, drawn with the node's own camera
    rdr_scene::Camera cam;
    scene_inputs::CameraInfo ci;
    wwe2k14::safe_pages::LineCopy<> lines;   // S108 RDS: this pass's line copy (dropped on return)
    FnGuestMem mem(read_, &lines);
    // S103 MP (A) + MPBF 4: the pass draws nothing when EVERY call of the frame returned before drawing (its own early-outs: +3084 / +3088 == 0 read at the call, null camera object): a whole, empty pass needs no camera and
    // reads no item. S103 PRT (PNS.md): EVERY distinct drawing node of the frame is read (call order) into the same snapshot: a node = one loaded particle bank, each with its own entry table (an entrance holds several, slots 4-8 + the arena
    // banks); reading only the last dropped the other banks' lights. A node called twice is read once (counted same_node_read). Plant 11 = the old rule (the last drawing node only).
    uint32_t drawing = 0;
    uint32_t nodes[kMaxCallsPerFrame];
    uint8_t soft[kMaxCallsPerFrame] = {};
    const NodeCall* ncall[kMaxCallsPerFrame] = {};
    uint32_t n_nodes = 0;
    for (const NodeCall& c : calls) {
      if (g_source_plant == 8 && &c != &calls.back()) continue;   // plant 8: the LAST call decides (the old node_empty)
      if (NodeDrawsNothing(c)) continue;
      ++drawing;
      bool seen = false;
      for (uint32_t i = 0; i < n_nodes; ++i) seen = seen || nodes[i] == c.node;
      if (seen) { same_node_read_.fetch_add(1, std::memory_order_relaxed); continue; }
      if (g_source_plant == 11) n_nodes = 0;   // plant 11: only the last drawing node survives
      if (n_nodes < kMaxCallsPerFrame) soft[n_nodes] = SoftCodeOf(c), ncall[n_nodes] = &c, nodes[n_nodes++] = c.node;
      if (SoftClamped(c)) soft_clamped_.fetch_add(1, std::memory_order_relaxed);   // S103 PRT fix 6
    }
    // S103 PRT fix 3: a stable order. Two walk threads can call the nodes in either order; the banks are drawn in bank order (type, id at node+3092 / +3096; an unreadable key sorts last), ties by call order.
    if (g_source_plant != 14) {
      uint64_t key[kMaxCallsPerFrame];
      for (uint32_t i = 0; i < n_nodes; ++i) {
        uint32_t t = 0, id = 0;
        key[i] = mem.Read32(nodes[i] + 3092u, t) && mem.Read32(nodes[i] + 3096u, id) ? (uint64_t(t) << 32) | id : ~0ull;
      }
      for (uint32_t i = 1; i < n_nodes; ++i)
        for (uint32_t j = i; j > 0 && key[j - 1] > key[j]; --j) {
          std::swap(key[j - 1], key[j]), std::swap(nodes[j - 1], nodes[j]), std::swap(soft[j - 1], soft[j]), std::swap(ncall[j - 1], ncall[j]);
        }
    }
    const bool node_empty = !calls.empty() && drawing == 0;
    if (node_empty) out.status.reasons |= rdr_scene::kPiEmptyPass;
    if (!node_empty && !SceneCamera(writer_frame, cam, ci)) {   // the pass ran but we cannot place a billboard: not whole, a counted hole with its reason
      out.status.reasons |= rdr_scene::kPiNoCamera;
      hole_no_camera_.fetch_add(1, std::memory_order_relaxed);
      incomplete_.fetch_add(1, std::memory_order_relaxed);
      presence::Particles().Note(presence::PartWhy::NoCamera, false);   // S103 S2: counted by verdict, never stamped
      return true;   // an empty snapshot carrying the status
    }
    float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!node_empty) std::memcpy(view, cam.view.m, sizeof view);   // GAME space (Y down): the view the game's own camera has. Never S * view.
    if (g_source_plant == 1)
      for (int i = 4; i < 8; ++i) view[i] = -view[i];   // the plant: S * view negates row 1
    const CameraBasis basis = node_empty ? CameraBasis() : BasisFromView(view);
    ReaderStats rs;
    rs.list = list_buf_.data(), rs.list_n = uint32_t(list_buf_.size());
    if (!node_empty)
      for (uint32_t i = 0; i < n_nodes; ++i) {
        if (ncall[i] && !IsScreenCall(*ncall[i]) && NoteCamRelation(*ncall[i], view) == 2 && g_node_cam_skip) continue;   // S103 PRT fix 4 / fix2: counted; skips only when g_node_cam_skip (default off)
        NoteNodeId(mem, nodes[i]);
        rs.soft = soft[i];
        ReadNode(mem, nodes[i], basis, out, rs, namer_);
        nodes_read_.fetch_add(1, std::memory_order_relaxed);
      }
    Add(rs);
    // B2: what makes the pass not whole (node-level and partial unreadable reads, other-class items, the cap) + F3: a texture the namer could not name, a blend code whose meaning is
    // unconfirmed. The 3x3 pivot grid is decoded now: a shifted pivot is counted (info) but no longer a reason (kPiPivot stays in the enum for old records).
    ParticleStatus& ps = out.status;
    ps.unreadable = uint32_t(rs.unreadable), ps.other_class = uint32_t(rs.items_other_class), ps.pivot_not_centred = uint32_t(rs.pivot_not_centred);
    if (rs.unreadable) ps.reasons |= rdr_scene::kPiUnreadable;
    if (rs.items_other_class) ps.reasons |= rdr_scene::kPiOtherClass;
    if (rs.tex_unresolved) ps.reasons |= rdr_scene::kPiTexture;
    if (rs.blend_unproven) ps.reasons |= rdr_scene::kPiBlend;
    if (out.dropped || over) ps.reasons |= rdr_scene::kPiDropped, hole_dropped_.fetch_add(1, std::memory_order_relaxed);   // (S103 MPBF 5: calls past the per-frame cap are dropped too)
    if (rs.unreadable) hole_unreadable_.fetch_add(1, std::memory_order_relaxed);
    if (ps.reasons & ~uint32_t(rdr_scene::kPiInfoMask)) incomplete_.fetch_add(1, std::memory_order_relaxed);
    {   // S103 S2 (hold by presence, presence_rules.h): the stamp is per frame. A frame with no item of an unproven kind (WholePass) stamps when the class constant OR the presence switch
        // (--wwe2k14_rdr_scene_particles_by_presence, default off) allows it; a frame holding an unnamed texture / unproven blend code / other-class item holds that frame only.
      const presence::PartWhy pw = presence::ParticleVerdict(ps, out.entries.size());
      const bool stamp = presence::StampParticles(kStampParticles, pw) && WholePass(ps);
      if (stamp) scene_inputs::Capture(scene_inputs::kInParticles, hook);
      presence::Particles().Note(pw, stamp);
    }
    taken_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // S103 MP (B): the screen path. Every non-empty call is read with the node's OWN camera when it is a screen call (screen-aligned basis [U]: the billboard basis block the node builds at 0x82254EB0
  // from the camera's +608 / +624 is not ported), with the scene camera otherwise; each SCREEN call's batches carry the 2D store's last ticket at the call + 1 (BatchKey::seq_after_p1; S111 FLATSEQ:
  // was the row count) so the flat drawer draws them before the first 2D row with a larger seq.
  // A screen call always holds the pass (kPiScreenCam: the flat drawer for them is not built), and its camera goes into the record (ParticleSnapshot::screen). `over` = calls past the cap were dropped (kPiDropped).
  bool TakeScreen(const std::vector<NodeCall>& calls, bool over, uint64_t hook, uint64_t writer_frame, ParticleSnapshot& out) {
    wwe2k14::safe_pages::LineCopy<> lines;   // S108 RDS: this pass's line copy (dropped on return)
    FnGuestMem mem(read_, &lines);
    ReaderStats rs;
    rs.list = list_buf_.data(), rs.list_n = uint32_t(list_buf_.size());
    ParticleStatus& ps = out.status;
    rdr_scene::Camera cam;
    scene_inputs::CameraInfo ci;
    bool scene_tried = false, scene_ok = false;
    uint32_t drawn_calls = 0, last_rows = 0, screen_diff = 0;
    for (const NodeCall& c : calls) {
      if (g_source_plant == 5 && &c != &calls.back()) continue;   // plant 5: the last call only
      if (NodeDrawsNothing(c)) continue;
      ++drawn_calls;
      CameraBasis basis;   // identity = screen-aligned
      const bool screen_call = IsScreenCall(c);
      if (screen_call) {
        ps.reasons |= rdr_scene::kPiScreenCam;
        if (!out.has_screen) out.has_screen = true, std::memcpy(out.screen.view_proj, c.view_proj, sizeof c.view_proj);
        else if (std::memcmp(out.screen.view_proj, c.view_proj, sizeof c.view_proj) != 0) ++screen_diff;
      } else {
        if (!scene_tried) scene_tried = true, scene_ok = SceneCamera(writer_frame, cam, ci);
        if (!scene_ok) { ps.reasons |= rdr_scene::kPiNoCamera; continue; }
        float view[16];
        std::memcpy(view, cam.view.m, sizeof view);
        if (NoteCamRelation(c, view) == 2 && g_node_cam_skip) continue;   // S103 PRT fix 4 / fix2
        basis = BasisFromView(view);
      }
      last_rows = c.rows == kNoRows ? 0 : c.rows;
      const size_t e0 = out.entries.size(), k0 = out.keys.size();
      rs.soft = SoftCodeOf(c);
      if (SoftClamped(c)) soft_clamped_.fetch_add(1, std::memory_order_relaxed);   // S103 PRT fix 6
      ReadNode(mem, c.node, basis, out, rs, namer_);
      memo_.assign(k0 + 64, -1);   // keys made by this call are renumbered once each (a copy with the row stamp; the interned list never invalidates a reference we hold); a member buffer: no allocation once grown
      for (size_t i = e0; i < out.entries.size(); ++i) {
        const uint32_t k = out.entries[i].key;
        if (k >= memo_.size()) memo_.resize(k + 1, -1);
        if (memo_[k] < 0) {
          BatchKey kk = out.keys[k];
          // (plant 7: the row is not stamped). S111 TITLESMOKE review: only a SCREEN call's batches carry the row: they are the flat drawer's (flat_particles.h IsFlat); a scene-camera call of the same
          // frame keeps 0 and is never drawn flat with the screen camera (plant 17: the old rule, every call stamped)
          kk.seq_after_p1 = c.seq2d == kNoRows || g_source_plant == 7 || (!screen_call && g_source_plant != 17) ? 0u : c.seq2d + 1u;   // S111 FLATSEQ: the 2D ticket, not the row count
          memo_[k] = int64_t(out.Intern(kk));
        }
        out.entries[i].key = uint32_t(memo_[k]);
      }
    }
    Add(rs);
    screen_calls_.fetch_add(drawn_calls, std::memory_order_relaxed);
    if (screen_diff) screen_cam_differs_.fetch_add(screen_diff, std::memory_order_relaxed);
    if (out.has_screen) out.screen.calls = drawn_calls, out.screen.row_count = last_rows, out.screen.stamp = rdr_scene::kParticleStampSeq;   // S111 FLATSEQ: the batches carry 2D tickets (flat_particles.h SeqStamped)
    if (!drawn_calls) ps.reasons |= rdr_scene::kPiEmptyPass;   // every call returned before drawing
    ps.unreadable = uint32_t(rs.unreadable), ps.other_class = uint32_t(rs.items_other_class), ps.pivot_not_centred = uint32_t(rs.pivot_not_centred);
    if (rs.unreadable) ps.reasons |= rdr_scene::kPiUnreadable;
    if (rs.items_other_class) ps.reasons |= rdr_scene::kPiOtherClass;
    if (rs.tex_unresolved) ps.reasons |= rdr_scene::kPiTexture;
    if (rs.blend_unproven) ps.reasons |= rdr_scene::kPiBlend;
    if (screen_diff) ps.reasons |= rdr_scene::kPiScreenCam;
    if (out.dropped || over) ps.reasons |= rdr_scene::kPiDropped, hole_dropped_.fetch_add(1, std::memory_order_relaxed);   // (S103 MPBF 5)
    if (rs.unreadable) hole_unreadable_.fetch_add(1, std::memory_order_relaxed);
    if (ps.reasons & ~uint32_t(rdr_scene::kPiInfoMask)) incomplete_.fetch_add(1, std::memory_order_relaxed);
    presence::Particles().Note(presence::ParticleVerdict(ps, out.entries.size()), false);   // never stamped: the verdict counts the frame (kPiScreenCam holds it)
    taken_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // "[scene][particles] read: taken T, entries E, ..., holes H (no_camera C, unreadable U, dropped D), incomplete I, pair (0 / +1 / -N / other), nodes_extra X, no_node N, stale_node M, + S103 MPBF: empty_frames, calls_over,
  // screen_calls, screen_cam_differs, same_node_twice": counters only, built at print time.
  // [U, B5] an entry the game retired this frame (state 0 by the latch) is not drawn here although the game drew it once more.
  std::string Census() const {
    const auto L = [](const std::atomic<uint64_t>& a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    char b[1600];
    std::snprintf(b, sizeof b,
                  "[scene][particles] read: taken %llu, entries %llu, items %llu, sprites %llu, gated %llu, other_class %llu, items_skipped %llu, unresolved %llu, pivot_not_centred %llu, non_finite %llu, alpha_zero %llu, over_cap %llu, "
                  "unreadable %llu, holes %llu (no_camera %llu, unreadable %llu, dropped %llu), incomplete %llu, pair (noted-hook: 0 %llu, +1 %llu, <0 %llu, other %llu), nodes_extra %llu, no_node %llu, stale_node %llu, quads %llu, tex (named %llu, unresolved %llu), blend_read %llu (other %llu, confirmed %d), sampler_read %llu, zoffset_read %llu, "
                  "empty_frames %llu, calls_over %llu, screen_calls %llu, screen_cam_differs %llu, same_node_twice %llu",
                  L(taken_), L(entries_), L(items_), L(keys_), L(gated_), L(other_), L(skipped_), L(unresolved_), L(pivot_), L(non_finite_), L(alpha_zero_), L(over_cap_), L(unreadable_),
                  L(hole_no_camera_) + L(hole_unreadable_) + L(hole_dropped_), L(hole_no_camera_), L(hole_unreadable_), L(hole_dropped_), L(incomplete_), L(pair_0_), L(pair_plus1_), L(pair_minus_), L(pair_other_),
                  L(nodes_extra_), L(no_node_), L(stale_node_), L(quads_), L(tex_named_), L(tex_unres_), L(blend_read_), L(blend_other_), int(kBlendCodesConfirmed), L(sampler_read_), L(zoffset_read_),
                  L(empty_frames_), L(calls_over_), L(screen_calls_), L(screen_cam_differs_), L(same_node_twice_));
    std::string s = std::string(b) + ", blend_unproven " + std::to_string((unsigned long long)blend_unproven_.load(std::memory_order_relaxed)) + presence::Particles().Str();   // S103 S2
    char c2[700];   // S103 PRT: over_cap by site, the nodes read, the third class
    std::snprintf(c2, sizeof c2, ", over_cap_by (entries %llu, items %llu, keys %llu, trail %llu), nodes_read %llu, same_node_read %llu, third_items %llu, node_cam_mismatch %llu, node_cam_transposed %llu, entry_not_listed %llu, entry_list_open %llu, soft_clamped %llu, soft_flat_hard %llu", L(over_entries_), L(over_items_), L(over_keys_), L(over_trail_), L(nodes_read_), L(same_node_read_), L(third_items_), L(node_cam_mismatch_), L(node_cam_transposed_), L(entry_not_listed_), L(entry_list_open_), L(soft_clamped_), L(soft_flat_hard_));
    s += c2;
    {
      std::lock_guard<std::mutex> lock(node_mu_);
      if (cam_first_) {
        char l[400];
        std::snprintf(l, sizeof l, "\n[scene][particles] node_cam first mismatch: node rot [%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f] scene rot [%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]", cam_node_[0], cam_node_[1], cam_node_[2], cam_node_[3], cam_node_[4], cam_node_[5], cam_node_[6], cam_node_[7], cam_node_[8],
                      cam_scene_[0], cam_scene_[1], cam_scene_[2], cam_scene_[3], cam_scene_[4], cam_scene_[5], cam_scene_[6], cam_scene_[7], cam_scene_[8]);
        s += l;
      }
    }
    {   // one line per DISTINCT node (type / id / count = node+3092 / +3096 / +3088) seen by a scene read
      std::lock_guard<std::mutex> lock(node_mu_);
      for (const NodeSeen& n : nodes_seen_) {
        char l[160];
        std::snprintf(l, sizeof l, "\n[scene][particles] node: type %u id %u count %u frames %llu", n.type, n.id, n.count, (unsigned long long)n.frames);
        s += l;
      }
    }
    return s;
  }
  uint64_t holes() const { return hole_no_camera_.load() + hole_unreadable_.load() + hole_dropped_.load(); }
  uint64_t taken() const { return taken_.load(); }
  uint64_t incomplete() const { return incomplete_.load(); }

 private:
  // S103 PRT: the census of distinct nodes (a bank = type / id; count = its entry count): read once per node per frame in a scene read. Bounded (64 distinct); a repeat bumps `frames`.
  struct NodeSeen { uint32_t type = 0, id = 0, count = 0; uint64_t frames = 0; };
  void NoteNodeId(const GuestMem& mem, uint32_t node) {
    uint32_t type = 0, id = 0, count = 0;
    if (!mem.Read32(node + 3092, type) || !mem.Read32(node + 3096, id) || !mem.Read32(node + 3088, count)) return;
    std::lock_guard<std::mutex> lock(node_mu_);
    for (NodeSeen& n : nodes_seen_)
      if (n.type == type && n.id == id && n.count == count) { ++n.frames; return; }
    if (nodes_seen_.size() < 64) nodes_seen_.push_back(NodeSeen{type, id, count, 1});
  }
  // The relation of a node call's camera to the scene camera, counted (and the first mismatch kept for the census).
  int NoteCamRelation(const NodeCall& c, const float scene_view[16]) {
    const int rel = NodeCameraRelation(c, scene_view);
    if (rel == 1) node_cam_transposed_.fetch_add(1, std::memory_order_relaxed);
    if (rel == 2) {
      node_cam_mismatch_.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(node_mu_);
      if (!cam_first_) {
        cam_first_ = true;
        for (int r = 0; r < 3; ++r)
          for (int k = 0; k < 3; ++k) cam_node_[r * 3 + k] = c.view[r * 4 + k], cam_scene_[r * 3 + k] = scene_view[r * 4 + k];
      }
    }
    return rel;
  }
  void Add(const ReaderStats& r) {
    entry_not_listed_.fetch_add(r.entry_not_listed, std::memory_order_relaxed), soft_flat_hard_.fetch_add(r.soft_flat_hard, std::memory_order_relaxed);
    over_entries_.fetch_add(r.over_entries, std::memory_order_relaxed), over_items_.fetch_add(r.over_items, std::memory_order_relaxed), over_keys_.fetch_add(r.over_keys, std::memory_order_relaxed), over_trail_.fetch_add(r.over_trail, std::memory_order_relaxed), third_items_.fetch_add(r.items_third, std::memory_order_relaxed);
    entries_.fetch_add(r.entries, std::memory_order_relaxed), items_.fetch_add(r.items, std::memory_order_relaxed), keys_.fetch_add(r.keys, std::memory_order_relaxed);
    gated_.fetch_add(r.entries_gated, std::memory_order_relaxed), other_.fetch_add(r.items_other_class, std::memory_order_relaxed), skipped_.fetch_add(r.items_skipped, std::memory_order_relaxed);
    unresolved_.fetch_add(r.items_unresolved, std::memory_order_relaxed), pivot_.fetch_add(r.pivot_not_centred, std::memory_order_relaxed), non_finite_.fetch_add(r.non_finite, std::memory_order_relaxed);
    quads_.fetch_add(r.items_quad, std::memory_order_relaxed), tex_named_.fetch_add(r.tex_named, std::memory_order_relaxed), tex_unres_.fetch_add(r.tex_unresolved, std::memory_order_relaxed);
    blend_unproven_.fetch_add(r.blend_unproven, std::memory_order_relaxed), blend_read_.fetch_add(r.blend_read, std::memory_order_relaxed), blend_other_.fetch_add(r.blend_other, std::memory_order_relaxed), sampler_read_.fetch_add(r.sampler_read, std::memory_order_relaxed), zoffset_read_.fetch_add(r.zoffset_read, std::memory_order_relaxed);
    alpha_zero_.fetch_add(r.alpha_zero, std::memory_order_relaxed), over_cap_.fetch_add(r.over_cap, std::memory_order_relaxed), unreadable_.fetch_add(r.unreadable, std::memory_order_relaxed);
  }
  scene_inputs::ReadFn read_ = nullptr;
  const TextureNamer* namer_ = nullptr;
  std::atomic<uint64_t> quads_{0}, tex_named_{0}, tex_unres_{0}, blend_read_{0}, blend_unproven_{0}, blend_other_{0}, sampler_read_{0}, zoffset_read_{0};
  std::mutex calls_mu_;
  std::vector<NodeCall> calls_;
  uint64_t calls_frame_ = ~0ull;
  bool calls_over_flag_ = false;   // calls past kMaxCallsPerFrame were dropped in calls_frame_ (under calls_mu_)
  std::vector<uint32_t> list_, list_buf_;   // S103 PRT fix 5: the game's entry list (sorted), read at a frame's first call; Take's copy
  std::vector<NodeCall> calls_buf_;   // Take's copy of the frame's calls (single consumer: the writer's latch); a member so it keeps its capacity
  std::vector<int64_t> memo_;         // TakeScreen's key renumbering memo (same consumer)
  std::atomic<uint64_t> calls_over_{0}, empty_frames_{0}, screen_calls_{0}, screen_cam_differs_{0}, same_node_twice_{0};
  std::atomic<uint64_t> node_cam_mismatch_{0}, entry_not_listed_{0}, entry_list_open_{0}, soft_clamped_{0}, soft_flat_hard_{0};
  std::atomic<uint64_t> over_entries_{0}, over_items_{0}, over_keys_{0}, over_trail_{0}, third_items_{0}, nodes_read_{0}, same_node_read_{0};
  mutable std::mutex node_mu_;
  bool cam_first_ = false;
  float cam_node_[9] = {}, cam_scene_[9] = {};   // the first mismatching rotation pair (node view, scene view), row-major 3x3
  std::atomic<uint64_t> node_cam_transposed_{0};
  std::vector<NodeSeen> nodes_seen_;
  std::atomic<uint32_t> node_{0};
  std::atomic<uint64_t> noted_{0};
  std::atomic<uint64_t> taken_{0}, entries_{0}, items_{0}, keys_{0}, gated_{0}, other_{0}, skipped_{0}, unresolved_{0}, pivot_{0}, non_finite_{0}, alpha_zero_{0}, over_cap_{0}, unreadable_{0};
  std::atomic<uint64_t> hole_no_camera_{0}, hole_unreadable_{0}, hole_dropped_{0}, incomplete_{0}, no_node_{0}, stale_node_{0};
  std::atomic<uint64_t> pair_0_{0}, pair_plus1_{0}, pair_minus_{0}, pair_other_{0}, nodes_extra_{0};
};

}  // namespace rdr_scene::live::particles
