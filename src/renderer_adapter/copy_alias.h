// S101 lane 190 (docs\renderer\SKIP_GAME_WORK_MAP.md section 14.6): the resolve-copy elision by alias. No game or SDK includes.
//
// The 360 resolves a render target into a texture (43-94 copies a frame). Where every reader of the texture goes through the binder's Map(), the copy can be dropped and the
// texture's id aliased to the target it would have been copied from, for the rest of the frame: the readers sample the target directly. This header decides WHICH copies
// (the elide set), from the frame's own ops and slot tables, never from guest memory. The binder (binder.h) skips the copies in the set and answers Map(dest) with the source.
//
// The rule (all must hold for a copy S -> D at op position p; the first that fails is the copy's "why"):
//   - a whole copy: both rects cover the whole of both images (mip 0, layer 0), same size, same format, 2D single mip, a colour format, same scaling; no 16-bit-target
//     conversion, no red/blue swap; S is not transient (a transient image has no sampled view);
//   - D is written by this copy only: no target switch, no other copy and no CPU content upload writes D anywhere in the frame; no op reads D before p (so the next frame, which
//     sees a D the alias never wrote, reads it only after its own copy: checked per frame, and a D that breaks it is banned for good, see Planner::Violations);
//   - D is read after p by draws only, at least one (a copy that nobody reads is candidate D, not this), reading it in its own format (no view reinterpretation); never by a
//     copy, the Present, or through a draw's image-0 slot later in the frame (that draw could read anything: nothing is judged);
//   - S is not written (target switch or copy) after p up to D's last reader, and no reader draw has S among its targets (sampled while attached);
//   - S is not itself an aliased destination of this frame (no chains);
//   - the image is none of the caller's exclusions (shadows_ours images, a copy another hook drops).
// The alias never outlives the frame (the binder clears it at the end of Bind), and is never applied to a part of a frame split at a readback, nor to an isolate or velocity bind.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "plan_memo.h"  // S102 R23-P: kAliasEvents

#include "capture_reader.h"
#include "copy_direct.h"   // S102 C5: the direct switch's rule (pure) and its reasons

namespace rdr_adapter::copy_alias {

// 0 off (default), 1 census (the plan is made and counted, nothing is elided), 2 elide. Live: --wwe2k14_rdr_copy_alias; fview / rplay: --copy-alias N.
inline int g_mode = 0;
// Which classes may be elided (mode 2): bit per Class. Default: the crowd bake only (the 14.4 contract); the 14.5 classes are added by this mask once proven.
// Live: --wwe2k14_rdr_copy_alias_classes; fview / rplay: --copy-alias-classes N.
enum Class : uint8_t { kBake = 0, kShadow, kRim, kPost, kScene, kOther, kClassCount };
inline constexpr uint32_t kClassBake = 1u << kBake;
inline constexpr uint32_t kClassAll = (1u << kClassCount) - 1;
inline uint32_t g_classes = kClassBake;
inline const char* ClassName(int c) {
  static const char* const k[] = {"crowd bake", "shadow", "rim", "post", "scene colour", "other"};
  return c >= 0 && c < kClassCount ? k[c] : "?";
}

// Planted faults (tools only; a player build never sets them): each is a bypass of one rule, or a wrong timing. The checks must catch each (docs 14.6).
enum class Fault : uint8_t {
  None, Empty,      // Empty: the switch is on and the set is empty (the null self-test: byte-equal)
  SrcDirty,         // the source-rewritten rule is skipped
  SubRect,          // the whole-copy rule is skipped
  Format,           // the same-format / same-size rule is skipped
  ReadFirst,        // the "no read before the copy" rule is skipped
  Late,             // the set is the previous frame's, by copy ordinal (lane 152's late-record class)
  Outlive,          // the alias is not cleared at the end of the frame
  NoDrawWrites,     // draws and clears into the source after the copy are not write events (lane 190 round 2, B1)
  FsNoClear,        // S102 G8 (binder): a kept copy into an aliased destination is swallowed and the alias stays (the next segment's readers read the old source)
  IgnoreReadback,   // S102 G8: the "destination was read back by the CPU" rule is skipped (the census must count the read-back of an aliased destination)
  FsNoExcl,         // S102 G8 F2: an op another hook drops is booked as a normal copy again (read of S, fresh write of D): the review's two planted cases must fail
  NoPresentGuard,   // S102 G8 F1 (binder): a persist-only / part bind updates the last picture again
  PostClaimLate,    // S102 G12 review F1 (binder): the plan-time post claim test reverts to `post_state_ == 2` (only set during the walk): the claim is not excluded from the planners again
  FsNoReader,       // S102 G8: a full-size copy nobody reads this frame is elided too (its destination is read by the NEXT frame before any write: the stale-read ledger must fire)
  SdNoBlock,        // S102 C3: the shadow-direct rule ignores the half-rate / turn / plane-blank block (a map not drawn this frame is aliased anyway: the readers sample a stale or empty map)
  SdNoAlias,        // S102 C3 (binder): a shadow-direct copy is dropped but its destination is not aliased (the readers still read the never-written texture)
  FdNoAlias,        // S102 C3 (binder): a frame-direct copy (not the Present's) is dropped but its destination is not aliased (the readers of the scene copies read a texture nobody wrote)
  SdSizeBlind,      // S102 C3 F10: shadow direct also takes a copy between two of our images of DIFFERENT size (unit test plant)
  FdNoPresentTail,  // S102 C3 F4: a destination only the Present reads: writes to the source AFTER the Present are not looked at (unit test plant)
  FdNoLastPicGuard, // S102 C3 F3 (binder): a part / persist bind that draws into the last picture's source image does not save the picture first (the fview part probe must see it change)
  FdPresentStale,   // S102 C3 (binder): the Present's copy is dropped but the Present is not re-pointed (it shows the destination, which nobody wrote this frame: a stale picture)
  DirectNoRetarget, // S102 C5 (copy_direct): the copy is dropped but its pass keeps drawing into the source (the destination is never written: the readers see last frame's picture / garbage)
  DirectStaleSrc,   // S102 C5 F1: from the third frame the copy is KEPT although its pass already renders into the destination (the copy reads a source nobody writes: a stale source)
  DirectReadSrc,    // S102 C5 F1: the stale-read ledger is shown a read of every taken source before its pass (every source must be banned and the lever must stop taking)
  DirectWrongDest,  // S102 C5: the pass is retargeted to the NEXT direct pass's destination (a bake written to the wrong atlas cell)
  Count
};
inline Fault g_fault = Fault::None;
// S102 lane G8 (docs/renderer/ALIAS_FULLSIZE.md): the full-size alias rule, switch wwe2k14_rdr_alias_fullsize (PerFrame, default off; fview / rplay --alias-fullsize N).
// LEVELS (0-3, clamped; S102 G8 review R1): 0 off. 1 (safest): only a copy that a LATER whole copy into the same destination follows in the frame (the real D then ends the frame as it did before G8).
// 2: any copy of the segment rule below, including the last copy into D of the frame. 3: also the Present. The text that follows describes the rule these levels share.
// A full-size copy (a screen-sized source, scaled, of at least half of 1280x720) that lane 190's rule does not already take is judged by a relaxed rule: (a) a destination written by SEVERAL whole copies
// a frame is judged per segment (a copy up to the next copy into the same destination), (b) a depth copy whose readers sample the depth, (c) level 3: a destination only the Present reads.
// It takes its copies WHATEVER THE CLASS MASK AND THE COPY ALIAS MODE SAY, including copies of lane 190's classes "scene colour" and "other" (which the mask keeps off until NoteReadback is
// counted live): its own counters (Census::fs_*, g_fs_readback_hits) are the live measurement that lifts that caveat. A destination the CPU ever read back is refused (NoteReadback).
// Read by the binder at every Bind (live: live.cpp copies the table value in before each frame; the render thread is the only reader).
inline int g_fullsize = 0;
inline int ClampLevel(int v) { return v < 0 ? 0 : v > 3 ? 3 : v; }
// S102 lane G12 (docs/renderer/COPIES_LEFT.md): the class-mask override, switch wwe2k14_rdr_alias_small (PerFrame, default off; fview / rplay --alias-small N). Lane 190's strict rule
// is unchanged; what this switch lifts is the CLASS MASK and the mode that keep its verdicts from being acted on: 1 = a copy the strict rule admits is elided in the classes crowd bake,
// shadow, rim and post (lane 190's 14.5 classes) whatever --wwe2k14_rdr_copy_alias / _classes say; 2 = also the classes scene colour and other (the ones lane 190 held back until
// read-backs are counted live: its own counter `read-backs of aliased destinations (small)` is that live measurement; a destination the CPU ever locked is still refused).
// Read by the binder at every Bind (live.cpp copies the table value in before each frame; fview / rplay set it from the flag).
inline int g_small = 0;
// S102 lane C5 (docs/renderer/COPIES_LEFT.md section 6): the direct switch, wwe2k14_rdr_copy_direct (PerFrame, default off; fview / rplay --copy-direct N). 0 off; 1 = a copy the strict rule admits
// (class crowd bake, or `other` of at most kDirectMaxEdge pixels a side: the sprite / post stand-ins) is REMOVED, not aliased: its pass renders straight into the copy's destination (the pass's colour
// target is the destination, a texture made with target usage), so the readers sample the destination itself and there is no copy, no alias and no layout change on the source.
// Declined (counted by reason) unless the binder proves the pass shape (direct_check, binder.h).
inline int g_direct = 0;
inline constexpr uint32_t kDirectMaxEdge = 128;
inline bool DirectFaultOk(Fault f) { return f == Fault::None || f == Fault::DirectNoRetarget || f == Fault::DirectWrongDest || f == Fault::DirectStaleSrc || f == Fault::DirectReadSrc; }
inline int ClampDirect(int v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline int ClampSmall(int v) { return v < 0 ? 0 : v > 2 ? 2 : v; }   // S102 G12: the alias_small switch's value is 0-2 (ClampLevel, above, is G8's 0-3)
// S102 G12 review F2: the crowd bake class is exclusive with --wwe2k14_rdr_crowd_bake_skip=2 (a skipped bake's cards read the last bake's destination: an elided copy never wrote it), exactly as
// reader.h clears bit 0 of copy_alias_classes. Written every frame by reader.h LiveSendImages (the same S1 test), read by the binder into Facts::small_mask.
inline std::atomic<bool> g_small_bake_off{false};
// The class decision of the small-class override, pure (tools/g12/copy_batch_test.cpp): level 1 = bake, shadow, rim, post; level 2 adds scene colour and other; `mask` = the classes still allowed.
inline bool SmallClassOk(int level, uint32_t mask, int cls) {
  if (cls < 0 || cls >= kClassCount || !(mask >> cls & 1u)) return false;
  return level >= 1 && (cls <= kPost || (level >= 2 && (cls == kScene || cls == kOther)));
}
// S102 lane C3 (docs/renderer/COPIES_LEFT.md section 5): switch wwe2k14_rdr_shadow_direct (PerFrame, default off; fview / rplay --shadow-direct 1). With shadows_ours on, a whole copy of one of OUR shadow images into
// another of ours (the map into the game's map texture: three segments of one destination a frame) is not made: the destination is aliased to the map for its segment and the readers sample the map. It is
// judged by the full-size rule's segment semantics (level 2: the last copy goes too) whatever the class mask and the copy-alias mode say; it declines (counted) on a half-rate / turn / plane-blank frame.
inline bool g_shadow_direct = false;
// switch wwe2k14_rdr_frame_direct (PerFrame, default off; --frame-direct 1): G8's full-size alias at level 3 (segments, depth, the Present) under its own switch and census; counted for what the player's own alias_fullsize does not already take.
inline bool g_frame_direct = false;
// dev (fview): --hash-mapped hashes Map(surface) after the bind (the capture's own Present image) instead of the image the binder presented; --lasting-probe binds the NEXT frame once more as a
// persist-only part after each frame and requires the last picture to stay the drawn frame's (review F1). Counters printed by fview.
inline bool g_hash_mapped = false, g_lasting_probe = false;
inline int g_part_probe = 0;   // S102 C3 F3 (fview --part-probe N): the NEXT frame's first N ops bound once more as a Full-mode part (a read-back split); the last picture must stay the drawn frame's
inline uint64_t g_probe_count = 0, g_probe_bad = 0;
inline constexpr uint32_t kFullsizePixels = 1280u * 720u / 2;   // a copy whose source has at least this many pixels (capture size) is "full-size"
inline bool g_list_all = false;   // S102 G12 dev: the timeline of EVERY copy, not only full-size ones (fview --copy-alias-timeline-all)
inline bool g_list_fullsize = false;   // S102 G8 dev: with --copy-alias-list F also print the event timeline of each full-size copy's source and destination (fview --copy-alias-timeline)
inline int g_list_frame = -1;   // dev: list every copy of this frame with its verdict (fview --copy-alias-list F)
// S102 C5 (review F3): each name is PAIRED with its enumerator and the table must list the enumerators in enum order, checked at compile time: two names swapped, a fault added in the
// middle of the enum or a name appended in the wrong place no longer compiles (before, a names list shifted by one silently ran the wrong fault: the merge hazard of two lanes both adding faults).
struct FaultName { Fault fault; const char* name; };
inline constexpr FaultName kFaultNames[] = {
    {Fault::None, "none"},
    {Fault::Empty, "empty"},
    {Fault::SrcDirty, "src_dirty"},
    {Fault::SubRect, "sub_rect"},
    {Fault::Format, "format"},
    {Fault::ReadFirst, "read_first"},
    {Fault::Late, "late"},
    {Fault::Outlive, "outlive"},
    {Fault::NoDrawWrites, "no_draw_writes"},
    {Fault::FsNoClear, "fs_no_clear"},
    {Fault::IgnoreReadback, "ignore_readback"},
    {Fault::FsNoExcl, "fs_no_exclusion"},
    {Fault::NoPresentGuard, "no_present_guard"},
    {Fault::PostClaimLate, "post_claim_late"},
    {Fault::FsNoReader, "fs_no_reader"},
    {Fault::SdNoBlock, "sd_no_block"},
    {Fault::SdNoAlias, "sd_no_alias"},
    {Fault::FdNoAlias, "fd_no_alias"},
    {Fault::SdSizeBlind, "sd_size_blind"},
    {Fault::FdNoPresentTail, "fd_no_present_tail"},
    {Fault::FdNoLastPicGuard, "fd_no_last_pic_guard"},
    {Fault::FdPresentStale, "fd_present_stale"},
    {Fault::DirectNoRetarget, "direct_no_retarget"},
    {Fault::DirectStaleSrc, "direct_stale_src"},
    {Fault::DirectReadSrc, "direct_read_src"},
    {Fault::DirectWrongDest, "direct_wrong_dest"}
};
constexpr bool FaultNamesInOrder() {
  for (size_t i = 0; i < sizeof(kFaultNames) / sizeof(kFaultNames[0]); ++i)
    if (size_t(kFaultNames[i].fault) != i) return false;
  return true;
}
static_assert(sizeof(kFaultNames) / sizeof(kFaultNames[0]) == size_t(Fault::Count), "a fault needs its name");
static_assert(FaultNamesInOrder(), "kFaultNames must list the faults in enum order, each paired with its own enumerator");
inline bool ParseFault(const char* name, Fault& out) {
  for (const FaultName& f : kFaultNames)
    if (std::strcmp(name, f.name) == 0) return out = f.fault, true;
  return false;
}

// Why a copy was kept (the first rule that failed). Order = the order the rules are tried.
enum Why : uint8_t {
  kElide = 0, kWhyRef, kWhyRecord, kWhyFormat, kWhyUsage, kWhyRect, kWhyExtra, kWhyExcluded, kWhyBanned, kWhyChain,
  kWhyWriters, kWhyContent, kWhyReadBefore, kWhyReaderKind, kWhyNoReader, kWhyReaderView, kWhyBlind, kWhySrcDirty, kWhyFeedback, kWhyLasting, kWhyReadback, kWhyClassOff, kWhyLastWrite, kWhyCount
};
inline const char* WhyName(int w) {
  static const char* const k[] = {"elided", "refs (mip / layer / same image)", "no image record", "format / size / kind differ", "source transient", "sub-rect copy", "16-bit target or swap",
                                  "excluded image (shadows_ours / hook-dropped)", "banned (broke the rule once)", "source is an aliased destination", "dest written elsewhere", "dest has CPU content",
                                  "dest read before the copy", "dest read by a copy / the Present", "no reader (dead)", "reader views it in another format", "blind (image-0 slot later)",
                                  "source rewritten before the last reader", "reader renders into the source", "destination is a lasting surface", "destination was read back by the CPU", "class not enabled", "last copy into the destination this frame (level 1 keeps it)"};
  return w >= 0 && w < kWhyCount ? k[w] : "?";
}

// S2 (lane 190 round 2): a destination the game ever locked for a CPU read (reader.h OnTextureLock, the GPU-newer readback) is never elided: a later lock of an elided D would read the
// never-written image. Written by the guest thread at the lock, read by the binder's planner. Once seen, always.
inline std::mutex g_readback_mu;
inline std::unordered_set<uint32_t> g_readback_ids;
inline std::unordered_set<uint32_t> g_fs_aliased_ids;   // S102 G8: destinations the full-size rule has ever aliased (guarded by g_readback_mu)
inline std::unordered_set<uint32_t> g_sm_aliased_ids;   // S102 G12: destinations the small-class rule has ever aliased (guarded by g_readback_mu)
inline std::unordered_set<uint32_t> g_sd_aliased_ids;   // S102 C3 F2: destinations shadow direct has ever aliased (guarded by g_readback_mu)
inline uint64_t g_sd_readback_hits = 0;                 // S102 C3 F2: read-backs (OnTextureLock) of such a destination: must stay 0 live before shadow direct can be a default
inline uint64_t g_sm_readback_hits = 0;                 // S102 G12: read-backs (OnTextureLock) of such a destination: must stay 0 live before level 2 can be a default
inline uint64_t g_fs_readback_hits = 0;                 // S102 G8: read-backs (OnTextureLock) of a destination the full-size rule had aliased: the one-frame blip, counted (the ban holds afterwards)
inline std::unordered_set<uint32_t> g_rt_dropped_ids;   // S102 R18-B1: snapshot images whose copy the frame_roundtrip span left out (guarded by g_readback_mu)
inline uint64_t g_rt_readback_hits = 0;                 // S102 R18-B1: read-backs (OnTextureLock) of such an image: must stay 0 live
inline void NoteReadback(uint32_t image) {
  std::lock_guard lock(g_readback_mu);
  g_readback_ids.insert(image);
  if (g_rt_dropped_ids.count(image)) ++g_rt_readback_hits;   // S102 R18-B1
  if (g_fs_aliased_ids.count(image)) ++g_fs_readback_hits;
  if (g_sm_aliased_ids.count(image)) ++g_sm_readback_hits;   // S102 G12
  if (g_sd_aliased_ids.count(image)) ++g_sd_readback_hits;   // S102 C3 F2
}
inline void NoteRtDropped(uint32_t image) {   // S102 R18-B1
  std::lock_guard lock(g_readback_mu);
  g_rt_dropped_ids.insert(image);
}
inline uint64_t RtReadbackHits() {   // S102 R18-B1
  std::lock_guard lock(g_readback_mu);
  return g_rt_readback_hits;
}
inline void NoteFullsizeAliased(uint32_t image) {
  std::lock_guard lock(g_readback_mu);
  g_fs_aliased_ids.insert(image);
}
inline void NoteSdAliased(uint32_t image) {
  std::lock_guard lock(g_readback_mu);
  g_sd_aliased_ids.insert(image);
}
inline uint64_t SdReadbackHits() {
  std::lock_guard lock(g_readback_mu);
  return g_sd_readback_hits;
}
inline void NoteSmallAliased(uint32_t image) {
  std::lock_guard lock(g_readback_mu);
  g_sm_aliased_ids.insert(image);
}
inline uint64_t SmallReadbackHits() {
  std::lock_guard lock(g_readback_mu);
  return g_sm_readback_hits;
}
inline uint64_t FullsizeReadbackHits() {
  std::lock_guard lock(g_readback_mu);
  return g_fs_readback_hits;
}
// dev (fview --copy-alias-readback FRAME:ID,ID): at the start of that Bind the CPU "reads back" those capture image ids (a planted lock), so the census and the ban can be seen offline
inline int g_plant_readback_frame = -1;
inline std::vector<uint32_t> g_plant_readback_ids;
inline bool WasReadBack(uint32_t image) {
  std::lock_guard lock(g_readback_mu);
  return g_readback_ids.count(image) != 0;
}

struct Census {
  uint64_t frames = 0, frames_planned = 0, frames_skipped = 0;
  uint64_t copies[kClassCount] = {};      // copies seen (planned frames only)
  uint64_t eligible[kClassCount] = {};    // copies the rule admits (class mask ignored): what census mode reports
  uint64_t elided[kClassCount] = {};      // copies actually skipped (mode 2)
  uint64_t eligible_pixels[kClassCount] = {};
  uint64_t why[kWhyCount] = {};
  uint64_t violations = 0;                // a banned-for-good destination (see Planner::Violations)
  double ms = 0;                          // wall ms spent planning (Build, all planned frames)
  uint64_t frames_observed = 0;           // frames bound as parts of a split frame (violations checked, nothing planned)
  // S102 G8 (the full-size rule, g_fullsize > 0): copies of full-size sources seen / admitted, by the relaxed sub-rule that admitted them, pixels, and why the rest were kept
  uint64_t fs_copies = 0, fs_eligible = 0, fs_elided = 0, fs_pixels = 0;
  uint64_t fs_kind[4] = {};               // elided by: 0 the strict rule alone (only the class mask kept it), 1 per-segment judgement (several copies into the destination), 2 a depth copy, 3 the Present reads it (one copy can count in 1-3 together)
  uint64_t fs_why[kWhyCount] = {};
  uint64_t fs_readback_refused = 0;       // full-size copies kept because their destination was read back by the CPU
  uint64_t sm_elided[kClassCount] = {};   // S102 G12: copies the small-class override took, by class (counted here only, never in `elided`)
  uint64_t sm_pixels = 0;                 // S102 G12: their pixels
  uint64_t dr_taken[kClassCount] = {};    // S102 C5: copies taken by the direct switch, by class
  uint64_t dr_pixels = 0, dr_violations = 0;                 // S102 C5: their pixels
  uint64_t dr_declined[copy_direct::kDirectWhyCount] = {};           // S102 C5: copies the strict rule admitted but the direct switch kept, by reason (kDirectWhy names)
  uint64_t fs_readback_elided = 0;        // (planted fault only) full-size copies admitted although their destination was read back: must stay 0
  // S102 C3 (shadow_direct): copies of ours -> ours seen, taken, their pixels, and why the rest were kept
  uint64_t sd_copies = 0, sd_taken = 0, sd_pixels = 0, sd_frames = 0, sd_frames_taken = 0;
  uint64_t sd_why[kWhyCount] = {};
  uint64_t d3_blind = 0;   // S102 C3 F7: frames on which both direct switches were off because a D3 marker class was active
  uint64_t fd_by_base = 0;   // S102 C3 F5: copies frame_direct's level would take that the player's own alias_fullsize already takes (not counted as frame_direct's)
  uint64_t sd_other = 0, fd_other = 0;   // S102 C3: copies another planner's drop set already holds (declined on overlap)
  // S102 C3 (frame_direct): full-size copies of a frame seen, taken, taken as the Present's (copy whose destination only the Present reads), pixels, frames with a take, and why the rest were kept
  uint64_t fd_copies = 0, fd_taken = 0, fd_present = 0, fd_pixels = 0, fd_frames = 0, fd_frames_taken = 0;
  uint64_t fd_why[kWhyCount] = {};
};

struct Plan {
  std::vector<uint8_t> elide;   // by op index: 1 = the binder skips this copy and aliases its destination to its source
  std::vector<uint8_t> sd;      // S102 C3: by op index: 1 = the shadow-direct rule took it (the planted sd_no_alias fault acts on these only)
  std::vector<uint8_t> fd;      // S102 C3: by op index: 1 = the frame-direct switch's full-size rule took it, 2 = a copy whose destination only the Present reads
  uint32_t count = 0;
  std::vector<uint8_t> direct;  // S102 C5: by op index: 1 = the binder drops this copy and its pass renders into the destination (copy_direct)
  uint32_t direct_count = 0;
  void Reset(uint32_t ops) { elide.assign(ops, 0), sd.assign(ops, 0), fd.assign(ops, 0), count = 0, direct.assign(ops, 0), direct_count = 0; }
};

// What the caller knows that the frame does not.
struct Facts {
  std::function<const ImageRecord*(uint32_t)> record;   // an image id's record (null: unknown)
  std::function<bool(uint32_t)> excluded;               // an image another hook owns (shadows_ours images)
  std::function<bool(uint32_t)> op_excluded;            // an op position another hook handles (a copy post_ours drops, a half-rate shadow copy)
  std::function<uint8_t(uint32_t)> role;                // names.h SurfaceRole of an image (0 unknown)
  int fullsize = 0;                                     // S102 G8: the full-size rule's level for this frame (g_fullsize, or 0 where the binder cannot honour it: DLSS / velocity passes)
  int small_level = 0;                                   // S102 G12: the class-mask override level for this frame (g_small)
  uint32_t small_mask = kClassAll;                      // S102 G12 review F2: the classes the small-class override may take (the bake bit is cleared while crowd_bake_skip >= 2)
  int direct_level = 0;                                  // S102 C5: copy_direct for this frame (g_direct)
  std::function<int(uint32_t, const rdr::CopyOp&)> direct_check;   // S102 C5: the binder proves the pass shape of copy op p (0 = ok, else a DirectWhy)
  bool present_ok = false;                              // S102 G8: the Present may read an aliased destination (the binder's Present path takes the source: no stage view, no DLSS copy); only g_fullsize >= 2 uses it
  bool extend_to_present = false;                       // S4: a velocity pass (DLSS) redraws wrestler draws at the Present: every D is read until the frame's last Present
  bool frame_direct = false;                            // S102 C3: the frame-direct switch for this frame (it also raised `fullsize` to level 3: segments, depth and the Present; counted in the fd_ fields)
  bool shadow_direct = false;                           // S102 C3: the shadow-direct switch for this frame (g_shadow_direct, with shadows_ours active)
  std::function<uint32_t(uint32_t)> shadow_dims;        // S102 C3 F10: the id's OUR shadow image's size, width << 16 | height (0: not one of ours this frame); shadow direct needs both ends ours AND equal
  int base_fullsize = 0;                                // S102 C3 F5: g_fullsize as set by the player (before frame_direct raised `fullsize` to 3): what a copy needs frame_direct for is what this level does not take
  std::function<bool(uint32_t)> shadow_blocked;         // S102 C3: the id is owned by the half rate / a turn / the plane blank skip: its maps are not drawn every frame, so its copies stay
};

class Planner {
 public:
  Census census;

  // Builds the elide set of one frame into `plan` (plan.elide sized to the frame's ops). `mode` 1: counts only (plan stays empty); 2: fills it for the enabled classes.
  // A part of a frame split at a readback (or a persistent-only pass): nothing is planned or elided, but the "written before read" rule is still checked on it, so a D elided in frame N
  // and read by the first part of frame N+1 is counted. `first` = the first part of its guest frame (the later parts inherit what the earlier ones wrote).
  void Observe(const LoadedFrame& f, const Facts& facts, bool part_open) {
    ++census.frames_observed;
    Events(f, facts);
    Violations(split_open_);
    CarryFresh(part_open);
  }

  void Build(const LoadedFrame& f, const Facts& facts, int mode, Plan& plan) {
    const auto t0 = std::chrono::steady_clock::now();
    BuildImpl(f, facts, mode, plan);
    census.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }

 private:
  uint8_t relax_used_ = 0;                // S102 G8: which relaxed sub-rules the last relaxed Judge leaned on (bit 0 segments, 1 depth, 2 present)
  // S102 G12: the class-mask override admits the strict verdict of these classes (level 1: crowd bake, shadow, rim, post; level 2: also scene colour and other)
  static bool SmallTakes(const Facts& facts, int cls) { return SmallClassOk(facts.small_level, facts.small_mask, cls); }
  // S102 C3 F10: both ends are OUR shadow images and the same size (a copy between two of ours of different size is a scaled copy, never an alias)
  static bool SdPair(const Facts& facts, const rdr::CopyOp& c) {
    if (!facts.shadow_dims) return false;
    const uint32_t a = facts.shadow_dims(c.source.image), b = facts.shadow_dims(c.dest.image);
    return a && b && (a == b || g_fault == Fault::SdSizeBlind);
  }
  static bool IsFullsize(const Facts& facts, const rdr::CopyOp& c) {
    const ImageRecord* rs = facts.record ? facts.record(c.source.image) : nullptr;
    return rs && (rs->usage & rdr::kUsageScaled) && uint64_t(rs->width) * rs->height >= kFullsizePixels;   // S102 G8 R2: screen-sized = follows the render scale (the Present's size), not just many pixels
  }
  bool split_open_ = false;               // the previous bind was a part with more of its guest frame to come
  std::unordered_set<uint32_t> carry_;    // images written (freshly) by the earlier parts of this guest frame

  void CarryFresh(bool part_open) {
    if (part_open) {
      for (const Ev& e : ev_)
        if (e.kind == kCopyWrite || e.kind == kClearFull || e.kind == kContent) carry_.insert(e.image);
    } else {
      carry_.clear();
    }
    split_open_ = part_open;
  }

  void BuildImpl(const LoadedFrame& f, const Facts& facts, int mode, Plan& plan) {
    plan.Reset(f.ops.count);
    ++census.frames_planned;
    Events(f, facts);
    // Rule "banned for good": a destination an earlier frame's alias left unwritten must be written before it is read in every later frame.
    Violations(split_open_);
    std::unordered_set<uint32_t> elided_dests;
    uint32_t ordinal = 0;
    std::vector<uint8_t> now;   // by copy ordinal: this frame's decisions (for the Late fault)
    std::vector<uint32_t> fs_taken;   // S102 G8: op positions the full-size rule took (not counted in lane 190's class lines)
    std::vector<uint32_t> sm_taken;   // S102 G12: op positions the class-mask override took (not counted in lane 190's class lines either)
    std::vector<uint32_t> sd_taken;   // S102 C3: op positions the shadow-direct rule took (counted in the sd_ fields only)
    bool sd_seen = false, fd_seen = false, fd_took = false;
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      if (op.kind != OpKind::Copy) continue;
      const rdr::CopyOp& copy = f.copies[op.index];
      if (facts.op_excluded && facts.op_excluded(p)) {   // another planner drops it (post claim, bake, dead pass, turns, plane blank, the half rate): not judged; S102 C3 counts the ones its switches would have judged
        if (facts.shadow_direct && SdPair(facts, copy)) ++census.sd_other;
        else if (facts.frame_direct && facts.fullsize > 0 && IsFullsize(facts, copy)) ++census.fd_other;
        if (facts.direct_level > 0) {   // S102 C5 F7: a copy another planner took that the direct switch would have judged
          int ow = copy_direct::kDirectClass;
          if (DirectClassOk(f, facts, copy, ClassOf(f, facts, copy), ow)) ++census.dr_declined[copy_direct::kDirectOtherPlanner];
        }
        now.push_back(0), ++ordinal;
        continue;
      }
      const int cls = ClassOf(f, facts, copy);
      ++census.copies[cls];
      const bool sd = facts.shadow_direct && SdPair(facts, copy);   // S102 C3: ours -> ours, equal size (F10)
      uint32_t why = Judge(f, facts, p, copy, op.index, elided_dests, 0, sd);
      uint8_t take = 0;
      // S102 G8: a full-size copy lane 190's rule does not take for its class is judged by the relaxed rule too (facts.fullsize); the relaxed verdict is the one reported for it. A copy lane 190 takes
      // (strict rule, class on, mode 2) stays lane 190's: G8 counts only what it alone elides, in the fs_ fields (review F3 c).
      const bool strict_takes = !sd && why == kElide && mode >= 2 && (g_classes >> cls & 1u) && g_fault != Fault::Empty;
      const bool full = !sd && facts.fullsize > 0 && !strict_takes && IsFullsize(facts, copy);
      bool fs_admitted = false;
      uint8_t used = 0;
      if (full) {
        ++census.fs_copies;
        if (facts.frame_direct) ++census.fd_copies, fd_seen = true;
        relax_used_ = 0;
        const uint32_t rwhy = Judge(f, facts, p, copy, op.index, elided_dests, facts.fullsize);
        used = relax_used_;
        if (rwhy == kWhyReadback) ++census.fs_readback_refused;
        if (rwhy == kElide) {
          ++census.fs_eligible;
          fs_admitted = true;
          why = kElide;
        } else {
          why = rwhy;
          ++census.fs_why[why];
          if (facts.frame_direct) ++census.fd_why[why];
        }
      }
      if (sd) {   // S102 C3: the shadow-direct rule decides alone (the class mask, the copy-alias mode and the full-size rule do not apply to a copy between two of our shadow images)
        sd_seen = true;
        ++census.sd_copies;
        if (why == kElide) {
          if (g_fault != Fault::Empty) {
            take = 1;
            sd_taken.push_back(p);
            plan.sd[p] = 1;
            NoteSdAliased(copy.dest.image);
            ++census.sd_taken;
            census.sd_pixels += uint64_t(copy.dest_rect.width) * copy.dest_rect.height;
          }
        } else {
          ++census.sd_why[why];
        }
      } else if (fs_admitted) {   // (review F3 b: the full-size switch is its own: it takes the copy in census mode 1 too; it is not a class of lane 190's mask)
        if (g_fault != Fault::Empty) {
          take = 1;
          fs_taken.push_back(p);
          ++census.fs_elided;
          census.fs_pixels += uint64_t(copy.dest_rect.width) * copy.dest_rect.height;
          if (!used) ++census.fs_kind[0];
          for (int b = 0; b < 3; ++b)
            if (used >> b & 1u) ++census.fs_kind[b + 1];
          if (facts.frame_direct) {   // S102 C3 F5: counted for frame_direct only when the player's own level (alias_fullsize) would NOT have taken it
            relax_used_ = 0;
            const bool base_takes = facts.base_fullsize > 0 && Judge(f, facts, p, copy, op.index, elided_dests, facts.base_fullsize) == kElide;
            if (!base_takes) {
              ++census.fd_taken, fd_took = true;
              census.fd_pixels += uint64_t(copy.dest_rect.width) * copy.dest_rect.height;
              plan.fd[p] = (used & 4) ? 2 : 1;
              if (used & 4) ++census.fd_present;
            } else {
              ++census.fd_by_base;
            }
          }
          if (WasReadBack(copy.dest.image)) ++census.fs_readback_elided;   // only reachable with the ignore_readback fault
          NoteFullsizeAliased(copy.dest.image);
        }
      } else if (why == kElide) {
        ++census.eligible[cls];
        census.eligible_pixels[cls] += uint64_t(copy.dest_rect.width) * copy.dest_rect.height;
        int dwhy = copy_direct::kDirectClass;
        if (facts.direct_level > 0 && g_fault != Fault::Empty && DirectClassOk(f, facts, copy, cls, dwhy) && facts.direct_check)
          dwhy = DirectFaultOk(g_fault) ? facts.direct_check(p, copy) : int(copy_direct::kDirectFault);   // (F2: a planted alias fault admits copies the strict rule would not: the direct switch declines them)
        if (facts.direct_level > 0 && g_fault != Fault::Empty && dwhy == copy_direct::kDirectOk) {   // S102 C5: removed by a direct pass (not aliased: no SetAlias, the destination is written by the pass)
          plan.direct[p] = 1, ++plan.direct_count;
          ++census.dr_taken[cls];
          census.dr_pixels += uint64_t(copy.dest_rect.width) * copy.dest_rect.height;
        } else if (facts.direct_level > 0 && dwhy != copy_direct::kDirectClass) {
          ++census.dr_declined[dwhy];
        }
        if (plan.direct[p]) {
        } else if (mode >= 2 && (g_classes >> cls & 1u) && g_fault != Fault::Empty) take = 1;
        else if (SmallTakes(facts, cls) && g_fault != Fault::Empty) {   // S102 G12: the class-mask override (lane 190's class and mode do not apply)
          take = 1;
          sm_taken.push_back(p);
          ++census.sm_elided[cls];
          census.sm_pixels += uint64_t(copy.dest_rect.width) * copy.dest_rect.height;
          NoteSmallAliased(copy.dest.image);
        } else if (mode >= 2 && !(g_classes >> cls & 1u)) why = kWhyClassOff;
      }
      if (g_list_frame >= 0 && uint64_t(g_list_frame) + 1 == census.frames_planned)   // dev: one line per copy of the listed frame (fview --copy-alias-list F)
        std::printf("[copy_alias] f%d op %u copy %u -> %u class %s why '%s' rect %ux%u->%ux%u refs m%u l%u / m%u l%u extra t16 %d swap %d taken %d\n", g_list_frame, p, copy.source.image, copy.dest.image, ClassName(cls),
                    plan.direct[p] ? "direct (copy_direct: the pass draws into the destination)" : WhyName(why), copy.source_rect.width, copy.source_rect.height, copy.dest_rect.width, copy.dest_rect.height, copy.source.mip, copy.source.layer, copy.dest.mip, copy.dest.layer,
                    op.index < f.copy_extras.count ? f.copy_extras[op.index].target16 : -1, op.index < f.copy_extras.count ? f.copy_extras[op.index].swap : -1, int(take));   // S102 C3: taken (any rule)
      if (g_list_frame >= 0 && uint64_t(g_list_frame) + 1 == census.frames_planned && (g_list_fullsize || g_list_all)) ListTimeline(f, facts, p, copy);   // S102 G8: the events of a full-size copy's source and destination
      if (!full && !sd) ++census.why[why];
      now.push_back(take);
      if (take) {
        plan.elide[p] = 1, ++plan.count;
        elided_dests.insert(copy.dest.image);
      }
      ++ordinal;
    }
    if (g_fault == Fault::Late && mode >= 2) {   // planted: the previous frame's decisions, applied by copy ordinal
      ordinal = 0;
      plan.count = 0;
      std::fill(plan.elide.begin(), plan.elide.end(), 0);
      for (uint32_t p = 0; p < f.ops.count; ++p) {
        if (f.ops[p].kind != OpKind::Copy) continue;
        if (ordinal < late_.size() && late_[ordinal] && !(facts.op_excluded && facts.op_excluded(p))) plan.elide[p] = 1, ++plan.count;
        ++ordinal;
      }
      late_ = now;
    }
    if (fd_seen) {
      ++census.fd_frames;
      if (fd_took) ++census.fd_frames_taken;
    }
    if (sd_seen) {
      ++census.sd_frames;
      if (!sd_taken.empty()) ++census.sd_frames_taken;
    }
    for (uint32_t p = 0; p < f.ops.count; ++p)
      if (plan.elide[p]) {
        const rdr::CopyOp& c = f.copies[f.ops[p].index];
        if (std::find(fs_taken.begin(), fs_taken.end(), p) == fs_taken.end() && std::find(sm_taken.begin(), sm_taken.end(), p) == sm_taken.end() && std::find(sd_taken.begin(), sd_taken.end(), p) == sd_taken.end()) ++census.elided[ClassOf(f, facts, c)];   // (G8's drops are counted in fs_elided only)
        ever_elided_.insert(c.dest.image);
      }
    CarryFresh(false);
  }

 public:

  // Imports the destinations an earlier plan banned (a violation was seen).
  bool Banned(uint32_t image) const { return banned_.count(image) != 0; }
  const std::unordered_set<uint32_t>& ever_elided() const { return ever_elided_; }

  std::string Report() const {
    char b[512];
    std::string out;
    std::snprintf(b, sizeof b, "[rdr][copy_alias] mode %d classes 0x%X%s frames planned %llu (skipped %llu), violations %llu, observed parts %llu, plan ms %.2f (%.4f a planned frame)\n", g_mode, g_classes,
                  g_fault == Fault::None ? "" : " FAULT", static_cast<unsigned long long>(census.frames_planned), static_cast<unsigned long long>(census.frames_skipped),
                  static_cast<unsigned long long>(census.violations), static_cast<unsigned long long>(census.frames_observed), census.ms,
                  census.frames_planned ? census.ms / double(census.frames_planned) : 0.0);
    out += b;
    for (int c = 0; c < kClassCount; ++c) {
      if (!census.copies[c]) continue;
      std::snprintf(b, sizeof b, "[rdr][copy_alias]   %-12s copies %llu, eligible %llu (%.3f MPix), elided %llu\n", ClassName(c), static_cast<unsigned long long>(census.copies[c]),
                    static_cast<unsigned long long>(census.eligible[c]), double(census.eligible_pixels[c]) / 1e6, static_cast<unsigned long long>(census.elided[c]));
      out += b;
    }
    if (g_direct > 0) {   // S102 C5
      uint64_t total = 0, declined = 0;
      for (int c = 0; c < kClassCount; ++c) total += census.dr_taken[c];
      for (int w = 1; w < copy_direct::kDirectWhyCount; ++w) declined += census.dr_declined[w];
      std::snprintf(b, sizeof b, "[rdr][copy_direct] frames %llu, taken %llu (%.3f MPix; bake %llu, other %llu), declined %llu, stale-read violations %llu (", static_cast<unsigned long long>(census.frames_planned), static_cast<unsigned long long>(total),
                    double(census.dr_pixels) / 1e6, static_cast<unsigned long long>(census.dr_taken[kBake]), static_cast<unsigned long long>(census.dr_taken[kOther]), static_cast<unsigned long long>(declined), static_cast<unsigned long long>(census.dr_violations));
      out += b;
      for (int w = 1; w < copy_direct::kDirectWhyCount; ++w)
        if (census.dr_declined[w]) {
          std::snprintf(b, sizeof b, "%s %llu; ", copy_direct::DirectWhyName(w), static_cast<unsigned long long>(census.dr_declined[w]));
          out += b;
        }
      out += ")\n";
    }
    if (g_small > 0) {   // S102 G12
      uint64_t total = 0;
      for (int c = 0; c < kClassCount; ++c) total += census.sm_elided[c];
      std::snprintf(b, sizeof b, "[rdr][copy_alias]   small-class override (level %d): elided %llu (%.3f MPix; bake %llu, shadow %llu, rim %llu, post %llu, scene %llu, other %llu), read-backs of aliased destinations (small) %llu\n", g_small,
                    static_cast<unsigned long long>(total), double(census.sm_pixels) / 1e6, static_cast<unsigned long long>(census.sm_elided[kBake]), static_cast<unsigned long long>(census.sm_elided[kShadow]),
                    static_cast<unsigned long long>(census.sm_elided[kRim]), static_cast<unsigned long long>(census.sm_elided[kPost]), static_cast<unsigned long long>(census.sm_elided[kScene]),
                    static_cast<unsigned long long>(census.sm_elided[kOther]), static_cast<unsigned long long>(SmallReadbackHits()));
      out += b;
    }
    if (g_frame_direct || census.fd_copies) {   // S102 C3: the live census line of frame_direct
      std::snprintf(b, sizeof b, "[rdr][frame_direct] effective level 3 (G8 alias_fullsize %d), frames %llu, taken %llu (in %llu frames, %.3f MPix; the Present's %llu; %llu more already taken by alias_fullsize), read-backs of aliased destinations %llu, declined %llu (by reason:",
                    g_fullsize,
                    static_cast<unsigned long long>(census.fd_frames), static_cast<unsigned long long>(census.fd_taken), static_cast<unsigned long long>(census.fd_frames_taken),
                    double(census.fd_pixels) / 1e6, static_cast<unsigned long long>(census.fd_present), static_cast<unsigned long long>(census.fd_by_base), static_cast<unsigned long long>(FullsizeReadbackHits()),
                    static_cast<unsigned long long>(census.fd_copies - census.fd_taken - census.fd_by_base));
      out += b;
      for (int w = 1; w < kWhyCount; ++w)
        if (census.fd_why[w]) {
          std::snprintf(b, sizeof b, " [%s] %llu;", WhyName(w), static_cast<unsigned long long>(census.fd_why[w]));
          out += b;
        }
      out += ")";
      std::snprintf(b, sizeof b, ", dropped by another planner %llu\n", static_cast<unsigned long long>(census.fd_other));
      out += b;
    }
    if (g_shadow_direct || census.sd_copies) {   // S102 C3: the live census line (every 600 frames while the switch is on)
      std::snprintf(b, sizeof b, "[rdr][shadow_direct] frames %llu, taken %llu (in %llu frames, %.3f MPix), read-backs of aliased destinations %llu, D3 frames left alone %llu, declined %llu (by reason:",
                    static_cast<unsigned long long>(census.sd_frames), static_cast<unsigned long long>(census.sd_taken), static_cast<unsigned long long>(census.sd_frames_taken),
                    double(census.sd_pixels) / 1e6, static_cast<unsigned long long>(SdReadbackHits()), static_cast<unsigned long long>(census.d3_blind), static_cast<unsigned long long>(census.sd_copies - census.sd_taken));
      out += b;
      for (int w = 1; w < kWhyCount; ++w)
        if (census.sd_why[w]) {
          std::snprintf(b, sizeof b, " [%s] %llu;", WhyName(w), static_cast<unsigned long long>(census.sd_why[w]));
          out += b;
        }
      out += ")";
      std::snprintf(b, sizeof b, ", dropped by another planner %llu\n", static_cast<unsigned long long>(census.sd_other));
      out += b;
    }
    if (g_fullsize > 0 || census.fs_copies) {
      std::snprintf(b, sizeof b, "[rdr][copy_alias]   full-size rule (level %d): copies %llu, admitted %llu, elided %llu (%.3f MPix; strict alone %llu, segments %llu, depth %llu, present %llu), refused for a CPU read-back %llu, elided although read back %llu (must be 0), read-backs of aliased destinations %llu\n",
                    g_fullsize, static_cast<unsigned long long>(census.fs_copies), static_cast<unsigned long long>(census.fs_eligible), static_cast<unsigned long long>(census.fs_elided),
                    double(census.fs_pixels) / 1e6, static_cast<unsigned long long>(census.fs_kind[0]), static_cast<unsigned long long>(census.fs_kind[1]),
                    static_cast<unsigned long long>(census.fs_kind[2]), static_cast<unsigned long long>(census.fs_kind[3]), static_cast<unsigned long long>(census.fs_readback_refused),
                    static_cast<unsigned long long>(census.fs_readback_elided), static_cast<unsigned long long>(FullsizeReadbackHits()));
      out += b;
      out += "[rdr][copy_alias]   full-size kept because:";
      for (int w = 1; w < kWhyCount; ++w)
        if (census.fs_why[w]) {
          std::snprintf(b, sizeof b, " [%s] %llu;", WhyName(w), static_cast<unsigned long long>(census.fs_why[w]));
          out += b;
        }
      out += "\n";
    }
    out += "[rdr][copy_alias]   kept because:";
    for (int w = 1; w < kWhyCount; ++w)
      if (census.why[w]) {
        std::snprintf(b, sizeof b, " [%s] %llu;", WhyName(w), static_cast<unsigned long long>(census.why[w]));
        out += b;
      }
    return out;
  }

 private:
  // S102 G8 dev: one line per event of S and D around a full-size copy (kinds as the enum below).
  void ListTimeline(const LoadedFrame& f, const Facts& facts, uint32_t p, const rdr::CopyOp& c) const {
    static const char* const kn[] = {"read", "copyread", "PRESENT", "tgt", "COPYW", "CONTENT", "draw", "clrfull", "clrpart", "copypart"};
    const ImageRecord* rs = facts.record ? facts.record(c.source.image) : nullptr;
    if (!rs || (!g_list_all && rs->width * rs->height < 1280u * 720u / 2)) return;   // (S102 G12: g_list_all)
    for (const uint32_t id : {c.source.image, c.dest.image}) {
      const ImageRecord* r = facts.record ? facts.record(id) : nullptr;
      std::printf("[copy_alias]   op %u image %u (%s) fmt %d kind %d %ux%u mips %u layers %u usage 0x%X role %d:", p, id, id == c.source.image ? "src" : "dst", r ? r->format : -1, r ? r->kind : -1,
                  r ? r->width : 0, r ? r->height : 0, r ? r->mips : 0, r ? r->layers : 0, r ? r->usage : 0, facts.role ? facts.role(id) : -1);
      const auto [lo, hi] = Range(id);
      uint32_t n = 0;
      for (const Ev* e = lo; e != hi; ++e) {
        if (++n > 40) { std::printf(" ..."); break; }
        std::printf(" %s@%u", kn[e->kind], e->pos);
        if (e->view) std::printf("(v%u)", e->view);
        if (e->kind == kDrawRead) {
          const DrawOrigin* o = f.OriginOf(f.ops[e->pos].index);
          if (o && o->pass_site) std::printf("[%X]", o->pass_site);
        }
      }
      std::printf("\n");
    }
  }

  // Event kinds. Reads: kDrawRead, kCopyRead, kPresentRead. Writes: kTgtWrite (a target switch: the image is a target from here, NOT a refresh), kDrawWrite (a draw into a bound target), kClearPart,
  // kCopyPart (a copy that does not cover the whole image / mip / layer 0); and the FRESH writes that give an image all its pixels: kCopyWrite (a whole copy), kClearFull, kContent (a CPU upload).
  enum Kind : uint8_t { kDrawRead = 0, kCopyRead, kPresentRead, kTgtWrite, kCopyWrite, kContent, kDrawWrite, kClearFull, kClearPart, kCopyPart, kExcluded };
  // S102 G8 F2: kExcluded = a copy another hook drops (post_ours, a bake pass, a dead pass, the half-rate shadows): it never happens, so it is neither a read of S nor a write of D nor a refresh;
  // every rule skips it (it is kept as an event only so a timeline can show it).
  static bool IsWrite(uint8_t k) { return k >= kTgtWrite && k != kExcluded; }
  struct Ev {
    uint32_t image, pos;
    uint8_t kind, view;   // view: the slot's view format (0 = the image's own)
    bool operator<(const Ev& o) const { return image != o.image ? image < o.image : pos != o.pos ? pos < o.pos : kind < o.kind; }
  };
  std::vector<Ev> ev_;
  std::vector<uint32_t> tgt_of_;               // by op position: the index of the frame's targets entry in force (0xFFFFFFFF none)
  std::vector<uint32_t> last_blind_;           // size 1: the position of the last draw with an image-0 slot (0xFFFFFFFF none)
  std::unordered_map<uint32_t, uint32_t> site_of_, call_of_;   // image -> pass_site / call_site of the last named draw into it
  std::unordered_set<uint32_t> banned_, ever_elided_;
  std::vector<uint8_t> late_;
  // S102 R23-P (plan_memo.h kAliasEvents): the scratch of the counting sort and the last site / call marks made
  std::vector<uint32_t> sort_count_;
  std::vector<Ev> sort_tmp_;
  uint32_t site_img_ = 0, site_val_ = 0, call_img_ = 0, call_val_ = 0;
  bool site_valid_ = false, call_valid_ = false;
  uint64_t ev_site_skipped_ = 0, ev_site_made_ = 0;
  uint32_t last_present_ = 0;   // op position of the frame's last Present (0 none)

  std::pair<const Ev*, const Ev*> Range(uint32_t image) const {
    const Ev key{image, 0, 0, 0};
    const auto lo = std::lower_bound(ev_.begin(), ev_.end(), key);
    auto hi = lo;
    while (hi != ev_.end() && hi->image == image) ++hi;
    return {ev_.data() + (lo - ev_.begin()), ev_.data() + (hi - ev_.begin())};
  }

  void Events(const LoadedFrame& f, const Facts& facts) {
    ev_.clear();
    last_present_ = 0;
    site_of_.clear(), call_of_.clear();
    const bool fast = plan_memo::Has(plan_memo::kAliasEvents);   // S102 R23-P
    const bool plant_site = fast && plan_memo::Plant(4);
    site_valid_ = call_valid_ = false;
    ev_site_skipped_ = ev_site_made_ = 0;
    tgt_of_.assign(f.ops.count, 0xFFFFFFFFu);
    last_blind_.assign(1, 0xFFFFFFFFu);
    for (const ContentRef& ref : f.contents) ev_.push_back({ref.image, 0, kContent, 0});
    uint32_t cur = 0xFFFFFFFFu;
    bool wrote = false;   // a draw event was pushed for the targets in force since the last target switch / copy (one per run is enough: the rule asks "written after p")
    const auto push_target_writes = [&](uint32_t p, uint8_t kind, bool colour, bool depth) {
      if (cur == 0xFFFFFFFFu) return;
      const rdr::TargetsOp& t = f.targets[cur];
      if (colour)
        for (uint32_t c = 0; c < t.color_count; ++c)
          if (t.color[c].image) ev_.push_back({t.color[c].image, p, kind, 0});
      if (depth && t.depth.image) ev_.push_back({t.depth.image, p, kind, 0});
    };
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      switch (op.kind) {
        case OpKind::Clear: {   // B1: a clear of the targets in force writes them without a target switch (C25 resolve-with-clear: reader.h pushes it on the SOURCE right after the copy)
          if (g_fault == Fault::NoDrawWrites) break;
          const rdr::ClearOp& c = f.clears[op.index];
          push_target_writes(p, c.full ? kClearFull : kClearPart, (c.flags & rdr::kClearColor) != 0, (c.flags & (rdr::kClearDepth | rdr::kClearStencil)) != 0);
          break;
        }
        case OpKind::SetTargets: {
          wrote = false;
          cur = op.index;
          const rdr::TargetsOp& t = f.targets[op.index];
          for (uint32_t c = 0; c < t.color_count; ++c)
            if (t.color[c].image) ev_.push_back({t.color[c].image, p, kTgtWrite, 0});
          if (t.depth.image) ev_.push_back({t.depth.image, p, kTgtWrite, 0});
          break;
        }
        case OpKind::Draw: {
          tgt_of_[p] = cur;
          if (!wrote && g_fault != Fault::NoDrawWrites) {   // B1: the reader drops a repeated SetTargets and the renderer reopens a pass on the same targets, so draws after the copy still write the source
            push_target_writes(p, kDrawWrite, true, true);
            wrote = true;
          }
          const Draw& d = f.draws[op.index];
          for (uint32_t s = 0; s < d.texture_count; ++s) {
            const TextureSlot& slot = f.texture_slots[d.texture_first + s];
            if (!slot.image) last_blind_[0] = p;
            else ev_.push_back({slot.image, p, kDrawRead, slot.view_format});
          }
          if (cur != 0xFFFFFFFFu) {
            const rdr::TargetsOp& t = f.targets[cur];
            const uint32_t img = t.color_count ? t.color[0].image : t.depth.image;
            if (const DrawOrigin* o = f.OriginOf(op.index)) {
              if (!fast) {
                if (img && o->pass_site) site_of_[img] = o->pass_site;
                if (img && o->call_site) call_of_[img] = o->call_site;
              } else {   // the same two marks, written when the (image, site) pair is not the one just written (every draw of a pass names the same pair)
                if (img && o->pass_site) {
                  if (site_valid_ && site_img_ == img && (plant_site || site_val_ == o->pass_site)) ++ev_site_skipped_;
                  else site_of_[img] = o->pass_site, site_img_ = img, site_val_ = o->pass_site, site_valid_ = true, ++ev_site_made_;
                }
                if (img && o->call_site) {
                  if (call_valid_ && call_img_ == img && (plant_site || call_val_ == o->call_site)) ++ev_site_skipped_;
                  else call_of_[img] = o->call_site, call_img_ = img, call_val_ = o->call_site, call_valid_ = true, ++ev_site_made_;
                }
              }
            }
          }
          break;
        }
        case OpKind::Copy: {
          const rdr::CopyOp& c = f.copies[op.index];
          if (g_fault != Fault::FsNoExcl && facts.op_excluded && facts.op_excluded(p)) {   // S102 G8 F2: not performed: no read, no write, no refresh
            ev_.push_back({c.dest.image, p, kExcluded, 0});
            break;
          }
          wrote = false;   // draws after the copy are new writes
          ev_.push_back({c.source.image, p, kCopyRead, 0});
          // S3: only a WHOLE copy (mip 0, layer 0, covering a single-mip single-layer image) gives the destination all its pixels; any other copy bans the destination for good
          const ImageRecord* rd = facts.record ? facts.record(c.dest.image) : nullptr;
          const bool whole = rd && !c.dest.mip && !c.dest.layer && rd->mips == 1 && rd->layers == 1 && Full(c.dest_rect, rd->width, rd->height);
          ev_.push_back({c.dest.image, p, whole ? kCopyWrite : kCopyPart, 0});
          if (!whole) banned_.insert(c.dest.image);
          break;
        }
        case OpKind::Present: {
          ev_.push_back({f.presents[op.index].source.image, p, kPresentRead, 0});
          last_present_ = p;
          break;
        }
        default: break;
      }
    }
    if (!fast) {
      std::sort(ev_.begin(), ev_.end());
      return;
    }
    // S102 R23-P: the events were generated in op order, so within one image they are already in position order: a counting sort by image (stable) and an insertion pass for the equal positions' kind order
    // replaces the comparison sort of ~4000 events. Anything that breaks that (a position going backwards, an image id past a million) takes the old sort.
    const bool verify = plan_memo::VerifyDue(1);
    std::vector<Ev> raw;
    if (verify) raw = ev_;
    SortEventsFast();
    plan_memo::Count(plan_memo::kCtAliasFast);
    plan_memo::Count(plan_memo::kCtAliasSiteSkip, ev_site_skipped_), plan_memo::Count(plan_memo::kCtAliasSiteDo, ev_site_made_);
    if (verify) VerifyEvents(f, raw);
  }

  void SortEventsFast() {
    const size_t n = ev_.size();
    uint32_t max_image = 0, prev_pos = 0;
    bool mono = true;
    for (const Ev& e : ev_) {
      max_image = e.image > max_image ? e.image : max_image;
      if (e.pos < prev_pos) mono = false;
      prev_pos = e.pos;
    }
    if (!mono || max_image > (1u << 20)) {
      std::sort(ev_.begin(), ev_.end());
      plan_memo::Count(plan_memo::kCtAliasSortFallback);
      return;
    }
    if (max_image > plan_memo::g_max_image.load(std::memory_order_relaxed)) plan_memo::g_max_image.store(max_image, std::memory_order_relaxed);
    sort_count_.assign(size_t(max_image) + 2, 0);
    for (const Ev& e : ev_) ++sort_count_[size_t(e.image) + 1];
    for (size_t i = 1; i < sort_count_.size(); ++i) sort_count_[i] += sort_count_[i - 1];
    sort_tmp_.resize(n);
    for (const Ev& e : ev_) sort_tmp_[sort_count_[e.image]++] = e;
    ev_.swap(sort_tmp_);
    for (size_t i = 1; i < n; ++i) {
      const Ev v = ev_[i];
      size_t j = i;
      while (j > 0 && ev_[j - 1].image == v.image && v < ev_[j - 1]) ev_[j] = ev_[j - 1], --j;
      ev_[j] = v;
    }
    if (plan_memo::Plant(3)) {   // (planted twin 3: a sort that ignores the position: each image's events come out in reverse)
      for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && ev_[j].image == ev_[i].image) ++j;
        std::reverse(ev_.begin() + i, ev_.begin() + j);
        i = j;
      }
    }
    plan_memo::Count(plan_memo::kCtAliasSortCounting);
  }

  // Tool only (plan_memo verify): the old sort of the same generated events, and the old per-draw marks, against what the fast path made.
  void VerifyEvents(const LoadedFrame& f, std::vector<Ev> raw) {
    plan_memo::Count(plan_memo::kCtVerified);
    std::sort(raw.begin(), raw.end());   // (the old order; ties of one image / position / kind differ only in the view)
    const auto full = [](const Ev& a, const Ev& b) { return a.image != b.image ? a.image < b.image : a.pos != b.pos ? a.pos < b.pos : a.kind != b.kind ? a.kind < b.kind : a.view < b.view; };
    std::vector<Ev> mine = ev_;
    std::sort(raw.begin(), raw.end(), full);
    std::sort(mine.begin(), mine.end(), full);
    char b[160];
    if (raw.size() != mine.size()) { std::snprintf(b, sizeof b, "%zu events against %zu", mine.size(), raw.size()); plan_memo::Mismatch("alias events", b); return; }
    for (size_t i = 0; i < raw.size(); ++i)
      if (raw[i].image != mine[i].image || raw[i].pos != mine[i].pos || raw[i].kind != mine[i].kind || raw[i].view != mine[i].view) { std::snprintf(b, sizeof b, "event %zu differs (image %u pos %u kind %u)", i, raw[i].image, raw[i].pos, raw[i].kind); plan_memo::Mismatch("alias events", b); return; }
    for (size_t i = 1; i < ev_.size(); ++i)   // the order the readers' range search relies on
      if (ev_[i] < ev_[i - 1]) { std::snprintf(b, sizeof b, "events %zu / %zu out of order", i - 1, i); plan_memo::Mismatch("alias events", b); return; }
    std::unordered_map<uint32_t, uint32_t> site, call;   // the old marks: one per draw, in op order
    uint32_t cur = 0xFFFFFFFFu;
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      if (op.kind == OpKind::SetTargets) cur = op.index;
      else if (op.kind == OpKind::Draw && cur != 0xFFFFFFFFu) {
        const rdr::TargetsOp& t = f.targets[cur];
        const uint32_t img = t.color_count ? t.color[0].image : t.depth.image;
        if (const DrawOrigin* o = f.OriginOf(op.index)) {
          if (img && o->pass_site) site[img] = o->pass_site;
          if (img && o->call_site) call[img] = o->call_site;
        }
      }
    }
    if (site != site_of_ || call != call_of_) plan_memo::Mismatch("alias events", "the site / call marks differ from the per-draw marks");
  }

  // A destination an earlier frame's alias left unwritten must, in every later frame, be written (a copy, a target switch, an upload) before anything reads it. One that is not
  // (the structure changed under us) is banned for good, counted, and the check reports it.
  void Violations(bool inherit) {
    for (const uint32_t image : ever_elided_) {
      if (banned_.count(image)) continue;
      const auto [lo, hi] = Range(image);
      bool fresh = inherit && carry_.count(image) != 0, bad = false;   // (a later part of a split frame inherits what its earlier parts wrote)
      for (const Ev* e = lo; e != hi && !bad; ++e) {
        if (e->kind == kExcluded) continue;   // S102 G8 F2
        if (e->kind == kCopyWrite || e->kind == kClearFull || e->kind == kContent) fresh = true;   // S3: a target switch alone is not a refresh
        else if (!fresh && !IsWrite(e->kind)) bad = true;   // a read before the image was freshly written
      }
      if (bad) banned_.insert(image), ++census.violations;
    }
  }

  // S102 C5: the classes the direct switch removes: the crowd bake (unless crowd_bake_skip >= 2 cleared the bake bit: reason copy_direct::kDirectBakeSkip) and `other` copies of a small picture
  bool DirectClassOk(const LoadedFrame& f, const Facts& facts, const rdr::CopyOp& c, int cls, int& why) const {
    (void)f;
    if (cls == kBake) {
      if (!(facts.small_mask >> kBake & 1u)) return why = copy_direct::kDirectBakeSkip, false;
      return true;
    }
    if (cls == kOther && c.dest_rect.width <= kDirectMaxEdge && c.dest_rect.height <= kDirectMaxEdge) return true;
    why = copy_direct::kDirectClass;
    return false;
  }
  int ClassOf(const LoadedFrame& f, const Facts& facts, const rdr::CopyOp& c) const {
    (void)f;
    const auto site = site_of_.find(c.source.image);
    const uint32_t s = site == site_of_.end() ? 0 : site->second;
    const uint8_t role = facts.role ? facts.role(c.source.image) : 0;
    if (s == 0x82C01F60u) return kBake;
    if (s == 0x82C04634u || s == 0x82BF4D38u || role == uint8_t(SurfaceRole::ShadowMap)) return kShadow;
    if (s == 0x82BFBD58u || role == uint8_t(SurfaceRole::RimMap)) return kRim;
    if (role == uint8_t(SurfaceRole::PostStep)) return kPost;
    if (role == uint8_t(SurfaceRole::SceneColor)) return kScene;
    return kOther;
  }

  // The planted faults: the rules (and the ones that would catch the same copy next) a fault skips, so that the copies it admits reach the picture.
  static bool Bypass(uint32_t why) {
    switch (g_fault) {
      case Fault::NoDrawWrites: return false;
      case Fault::SrcDirty: return why == kWhySrcDirty || why == kWhyFeedback || why == kWhyWriters;
      case Fault::SubRect: return why == kWhyRect || why == kWhyExtra;
      case Fault::Format: return why == kWhyFormat || why == kWhyRect || why == kWhyExtra || why == kWhyUsage;
      case Fault::ReadFirst: return why == kWhyReadBefore || why == kWhyLasting;   // (a history texture is a lasting surface: S1 refuses it first)
      default: return false;
    }
  }
  static bool Full(const rdr::Rect& r, uint32_t w, uint32_t h) { return r.x == 0 && r.y == 0 && r.width >= w && r.height >= h; }

  uint32_t Judge(const LoadedFrame& f, const Facts& facts, uint32_t p, const rdr::CopyOp& copy, uint32_t copy_index, const std::unordered_set<uint32_t>& elided_dests, int relax, bool sd = false) {
    const uint32_t S = copy.source.image, D = copy.dest.image;
    if (sd) relax = 2;   // S102 C3: a shadow-direct copy is judged by the segment rule, the last copy into the destination included
    if (!S || !D || S == D || copy.source.mip || copy.source.layer || copy.dest.mip || copy.dest.layer) return kWhyRef;
    const ImageRecord* rs = facts.record ? facts.record(S) : nullptr;
    const ImageRecord* rd = facts.record ? facts.record(D) : nullptr;
    if (!rs || !rd) return kWhyRecord;
    {
      if (rs->width != rd->width || rs->height != rd->height || rs->format != rd->format || rs->kind != rd->kind || rs->kind != uint8_t(rdr::ImageKind::Image2D) || rs->mips != 1 || rd->mips != 1 ||
          rs->layers != 1 || rd->layers != 1 || rs->depth != 1 || rd->depth != 1 || ((rs->usage ^ rd->usage) & rdr::kUsageScaled))
        if (!Bypass(kWhyFormat)) return kWhyFormat;
    }
    if (rs->format == uint8_t(rdr::Format::Undefined)) return kWhyFormat;
    if (rs->format == uint8_t(rdr::Format::Depth)) {   // S102 G8: a depth copy is judged only by the relaxed rule (its readers sample the depth through the image's own sampled view); never colour <-> depth
      if (!relax || rd->format != rs->format) return kWhyFormat;
      relax_used_ |= 2;
    }
    if ((rs->usage & rdr::kUsageTransient) && !Bypass(kWhyUsage)) return kWhyUsage;
    if ((!Full(copy.source_rect, rs->width, rs->height) || !Full(copy.dest_rect, rd->width, rd->height)) && !Bypass(kWhyRect)) return kWhyRect;
    if (copy_index < f.copy_extras.count) {
      const CopyExtra& e = f.copy_extras[copy_index];
      if (e.target16 && !Bypass(kWhyExtra)) return kWhyExtra;   // (the red/blue swap is folded into the reading slots' swizzles; the copy itself moves the texels as they are)
    }
    if (sd) {   // S102 C3: our images are the point here; what still blocks is the half rate, a turn or the plane blank skip (their maps are not drawn every frame: the texture would be read stale)
      if (g_fault != Fault::SdNoBlock && facts.shadow_blocked && (facts.shadow_blocked(S) || facts.shadow_blocked(D))) return kWhyExcluded;
    } else if (facts.excluded && (facts.excluded(S) || facts.excluded(D))) {
      return kWhyExcluded;
    }
    if (banned_.count(D)) return kWhyBanned;
    if ((rd->usage & rdr::kUsageLasting) && !Bypass(kWhyLasting)) return kWhyLasting;   // S1: a later frame reads a lasting surface (a skipped bake's cards read the last bake's destination)
    if (g_fault != Fault::IgnoreReadback && WasReadBack(D)) return kWhyReadback;   // S2
    if (elided_dests.count(S)) return kWhyChain;
    // D's events.
    uint32_t last_reader = 0, readers = 0;
    bool present_used = false;   // S102 C3 F4
    // S102 G8 (relax >= 1): D may be written by several WHOLE copies a frame; each copy defines a segment, from itself up to the next copy into D. A read before p is fine when an earlier whole
    // copy of this frame wrote D; reads at or after the next copy belong to the next segment and are judged with that copy. Anything else that writes D (a target switch, a draw into it, a
    // clear, a part copy, an upload) still keeps the copy.
    uint32_t next_write = 0xFFFFFFFFu;
    {
      const auto [lo, hi] = Range(D);
      bool fresh = false;
      for (const Ev* e = lo; e != hi; ++e) {
        if (e->kind == kExcluded) continue;   // S102 G8 F2
        if (e->kind == kContent) return kWhyContent;
        if (e->kind == kCopyWrite) {
          if (e->pos == p) continue;
          if (relax >= 1) {
            if (e->pos < p) fresh = true;
            else next_write = std::min(next_write, e->pos);
            relax_used_ |= 1;
            continue;
          }
          if (!Bypass(kWhyWriters)) return kWhyWriters;
          continue;
        }
        if (IsWrite(e->kind)) { if (!Bypass(kWhyWriters)) return kWhyWriters; continue; }
        if (e->pos < p) { if (relax >= 1 && fresh) continue; if (!Bypass(kWhyReadBefore)) return kWhyReadBefore; continue; }
        if (e->pos == p) continue;   // (the copy's own read of D cannot happen: S != D)
        if (e->pos >= next_write) continue;   // a later segment's reader
        if (e->kind == kPresentRead && relax >= 3 && facts.present_ok) { relax_used_ |= 4; present_used = true; ++readers, last_reader = std::max(last_reader, e->pos); continue; }
        if (e->kind != kDrawRead) return kWhyReaderKind;
        if (e->view && e->view != rd->format) return kWhyReaderView;
        ++readers, last_reader = std::max(last_reader, e->pos);
      }
    }
    if (!readers && !(relax && g_fault == Fault::FsNoReader)) return kWhyNoReader;
    if (relax == 1 && next_write == 0xFFFFFFFFu) return kWhyLastWrite;   // S102 G8 R1: level 1 keeps the last copy into D of the frame: the real D ends the frame as before G8
    if (facts.extend_to_present && last_present_ > last_reader && next_write == 0xFFFFFFFFu) last_reader = last_present_;   // S4: the velocity pass redraws at the Present
    if (last_blind_[0] != 0xFFFFFFFFu && last_blind_[0] > p) return kWhyBlind;
    // S after the copy.
    {
      const auto [lo, hi] = Range(S);
      for (const Ev* e = lo; e != hi; ++e) {
        if (IsWrite(e->kind) && e->pos > p && e->pos <= last_reader && !Bypass(kWhySrcDirty)) return kWhySrcDirty;
        // S102 C3 F4: the Present shows the source image itself: whatever writes it before the frame ends (a clear after the Present) is the picture the Present, a photo and the next frame see
        if (present_used && g_fault != Fault::FdNoPresentTail && IsWrite(e->kind) && e->pos > last_reader && e->pos > p) return kWhySrcDirty;
      }
    }
    // A reader that renders into S (the pass was opened before the copy and stays open): sampled while attached.
    {
      const auto [lo, hi] = Range(D);
      for (const Ev* e = lo; e != hi; ++e) {
        if (e->kind != kDrawRead || e->pos <= p || e->pos >= next_write) continue;
        const uint32_t t = tgt_of_[e->pos];
        if (t == 0xFFFFFFFFu) continue;
        const rdr::TargetsOp& to = f.targets[t];
        for (uint32_t c = 0; c < to.color_count; ++c)
          if (to.color[c].image == S && !Bypass(kWhyFeedback)) return kWhyFeedback;
        if (to.depth.image == S && !Bypass(kWhyFeedback)) return kWhyFeedback;
      }
    }
    return kElide;
  }
};

}  // namespace rdr_adapter::copy_alias
