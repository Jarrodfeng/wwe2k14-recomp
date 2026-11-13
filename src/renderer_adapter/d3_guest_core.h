// D3 skip the object: the game-side decision core (Session 101, lane 138; docs\renderer\SKIP_GAME_WORK_MAP.md 2.4 / 8.3). No game, SDK or renderer includes: the hook glue
// (scene_live\d3_arena_hook.h, on the existing REX_HOOK_RAW(sub_82C04DF0) in draw_origin.h) and tools\d3\d3_unit_test.cpp both build THIS header, so what is tested is what ships.
//
// What it knows. sub_82C04DF0 is called once per object per pass-entry (four callers; 0x82C13D80 is one of them), and one call may draw only the keyed colour sites (0x82C0A950 /
// 0x82C0B94C) or other sites too. The hook cannot tell from the call; it LEARNS it: every call the game records in full is observed (the sub_82BDED40 hook counts its parts, keyed or not)
// and kept per (object, call index in the frame). A call may be skipped only if the object's same call index drew only keyed parts (> 0) in the last frame the object was recorded
// in full AND the binder's mark set holds (object, keyed ordinal). The keyed ordinal = how many earlier calls of the object, this frame, were keyed: it is the binder's run index
// (d3_binder.h counts runs of keyed draws per object), so the packet and the sequence join without a call id on the draw.
#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "d3_arena.h"

namespace rdr_adapter::d3 {

inline constexpr uint32_t kMaxCalls = 4;   // calls of one object tracked per frame; a fifth is never skipped
inline constexpr uint32_t kMaxRingCalls = 16;   // lane 258: the ring ledger (g_robjs, the ring chain's offline ledger) follows up to 16 calls an object (a rope / cover / turnbuckle model is called once per instance: 12 a frame); the arena, casters and wrestlers keep 4
inline constexpr uint32_t kMaxCasterPlaneCalls = 16;   // lane 290: the caster ledger follows up to 16 calls of a PLANE-pass object (the ring's plane proxy 0xAAF56230 makes 12 calls a frame; kMaxCalls = 4 left calls 4-11 without a marker); the map and rim passes keep kMaxCalls (CallInputs::max_calls)
inline constexpr uint32_t kCallSlots = 16;
inline constexpr uint32_t CasterCallCap(uint32_t pass) { return pass == 2u ? kMaxCasterPlaneCalls : kMaxCalls; }   // lane 290: pass 2 = the plane (CasterPassOfSite); map (1) and rim (3) keep 4      // storage per entry (>= every max)

struct CallInfo {
  uint16_t keyed = 0, other = 0;
  bool Keyed() const { return keyed > 0 && other == 0; }
};

class GuestObjects {
 public:
  GuestObjects() = default;
  explicit GuestObjects(uint32_t max_calls) : max_calls_(max_calls < 1 ? 1 : max_calls > kCallSlots ? kCallSlots : max_calls) {}
  void SetMaxCalls(uint32_t n) { std::lock_guard<std::mutex> lock(mu_); max_calls_ = n < 1 ? 1 : n > kCallSlots ? kCallSlots : n; map_.clear(); }   // lane 258 (before the first call)
  struct Verdict {
    uint32_t call = 0;         // this call's index in the frame
    uint32_t ordinal = 0;      // keyed ordinal (valid when eligible)
    bool eligible = false;     // last time this call index was recorded in full it drew only keyed parts
  };
  // A call of `object` begins in guest frame `frame`.
  Verdict Begin(uint32_t object, uint64_t frame, uint32_t cap = 0) {   // lane 290: cap (> 0) = this call's own limit, at most the ledger's (a pass that keeps fewer calls than the ledger holds)
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = map_[object];
    if (e.frame != frame) {   // a new frame for this object: what it drew last time is what it is expected to draw
      if (e.cur_n) {
        for (uint32_t i = 0; i < kCallSlots; ++i) e.prev[i] = e.cur[i];
        e.prev_n = e.cur_n;
      }
      for (uint32_t i = 0; i < kCallSlots; ++i) e.cur[i] = {};
      e.cur_n = 0;
      e.frame = frame;
    }
    Verdict v;
    v.call = e.cur_n;
    if (e.cur_n >= max_calls_ || (cap && e.cur_n >= cap)) return v;   // a fifth call: never eligible
    ++e.cur_n;
    if (v.call < e.prev_n && e.prev[v.call].Keyed()) {
      v.eligible = true;
      for (uint32_t i = 0; i < v.call; ++i) v.ordinal += e.prev[i].Keyed();
    }
    return v;
  }
  // A SKIPPED call ended: it is expected to draw what it drew the last time (the previous frame's entry for that call index). Without this the next frame finds an empty entry and the object is
  // skipped only every other frame (lane 138 review fix 2).
  void EndSkipped(uint32_t object, uint64_t frame, uint32_t call) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(object);
    if (it == map_.end() || it->second.frame != frame || call >= max_calls_) return;
    it->second.cur[call] = it->second.prev[call];
  }
  // The call ended: `info` = what it drew (recorded in full), or the expected (skipped).
  void End(uint32_t object, uint64_t frame, uint32_t call, CallInfo info) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(object);
    if (it == map_.end() || it->second.frame != frame || call >= max_calls_) return;
    it->second.cur[call] = info;
  }
  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    map_.clear();
  }

 private:
  struct Entry {
    uint64_t frame = ~0ull;
    CallInfo prev[kCallSlots], cur[kCallSlots];
    uint32_t prev_n = 0, cur_n = 0;
  };
  mutable std::mutex mu_;
  uint32_t max_calls_ = kMaxCalls;
  std::unordered_map<uint32_t, Entry> map_;
};

// What the hook needs to decide, gathered by the glue (guest reads) and the binder's publications. Every input is a plain value so the unit test can drive the decision.
// lane 152 (item 16): the light state the packet needs, read BEFORE the decision. The glue's probe says whether the call's state can be written (readable, room in the frame's tables) without writing it.
struct StateProbe {
  bool (*fn)(void* ctx, uint32_t object, bool& readable, uint32_t& table_states, uint32_t& ref_ordinal) = nullptr;   // ref_ordinal = the ordinal the light-state tracker WILL give this call's ref (review S6)
  void* ctx = nullptr;
};
struct CallInputs {
  uint32_t object = 0;
  uint64_t frame = 0;
  int64_t now_ms = 0;
  bool owner = true;
  CachePath path = CachePath::Fresh1;   // which sub_82BD7B50 path this call takes (PredictPath; the glue reads it BEFORE deciding)
  bool state_readable = true;      // R (the render camera record) could be read for this call
  uint32_t state_count = 0;        // how many light states the frame holds so far
  const StateProbe* probe = nullptr;   // lane 152 (16): when set, asked for state_readable / state_count at the packet step (only for a call that would otherwise be skipped); the two fields above are the unit test's / emulation's stand-ins
  bool ring = false;               // lane 229: the call is a ring model call (its class switch is g_ring_d3)
  bool caster = false;             // lane 217: the call is a caster pass entry (its class switch is g_caster_d3, not the arena's)
  bool wrestler = false;           // lane 227: the call belongs to the wrestler chain (its class switch is g_wrestlers_d3)
  uint32_t max_calls = 0;          // lane 290: 0 = the ledger's own limit; > 0 = at most this many calls of the object are tracked (the caster ledger holds 16 for the plane pass and 4 for map / rim)
  uint32_t proof_skew = 0;         // lane 152 (12): the hook-to-stream frame offset: an object is also not skipped in the frame the READER's proof schedule (frame + skew) calls its proof frame
};
struct CallOutcome {
  Why why = Why::SwitchOff;
  uint32_t ordinal = 0;
  uint32_t call = 0;
  uint32_t packet_call = 0;   // lane 246: the `call` the packet carries (the light-state ref's ordinal), valid when packet_written; the ring hook checks the ring-rows record's own `call` against it
  bool packet_written = false;
  bool eligible = false;      // lane 268 (review N1): the object was eligible for a marker this call (it had drawn keyed parts last time)
};

// The decision, made BEFORE the original runs. Writes the packet (g_packets) first, then decides; the caller records the object in full for anything but Why::Skip.
// `marks`: the binder's published set (null: nothing is markable). Counters go to g_tally.
inline CallOutcome DecideCall(GuestObjects& objs, const CallInputs& in, const MarkSet* marks, PacketTable& packets, GuestTally& tally, const Gate& gate, Fault fault = Fault::None) {
  CallOutcome out;
  const GuestObjects::Verdict v = objs.Begin(in.object, in.frame, in.max_calls);
  out.call = v.call, out.ordinal = v.ordinal, out.eligible = v.eligible;
  tally.NotePath(in.path);   // lane 237: the census's path histogram (fresh1 / fresh2 / record3 / cached4)
  if (in.path == CachePath::Cached4) tally.NoteCachedObject(in.frame, in.object);   // lane 301: a replayed list (path 4) records no part: calls = runs leaves this object's runs out
  SkipInputs si;
  si.path = in.path;
  si.switch_on = in.ring ? g_ring_d3 : in.caster ? g_caster_d3 : in.wrestler ? g_wrestlers_d3 : g_arena_d3, si.owner = in.owner;
  si.gate_proven = gate.Proven(in.now_ms);
  const uint64_t pframe = in.frame + (fault == Fault::ProofSkew ? 11 : 0);   // (plant: a skewed proof clock)
  si.proof_frame = ObjectProofFrame(in.object, pframe) || (in.proof_skew && ObjectProofFrame(in.object, pframe + in.proof_skew));   // lane 152 (12): both clocks' proof frames are full frames
  si.markable = v.eligible && marks && marks->Has(MarkKey(in.object, v.ordinal));
  // The packet is written before the decision so a skip can never lack it (8.3 item 2). Only for an object that would otherwise be skipped.
  const bool can_skip_path = in.path == CachePath::Fresh1 || fault == Fault::PacketOnCachePath;   // (review S3) no packet for a call that cache path 3 / 4 keeps: it would never be consumed
  if (si.markable && si.switch_on && si.owner && si.gate_proven && can_skip_path && (!si.proof_frame || fault == Fault::SkipInProofFrame)) {
    bool readable = in.state_readable;
    uint32_t count = in.state_count, ref_ordinal = v.call;
    if (in.probe && in.probe->fn) in.probe->fn(in.probe->ctx, in.object, readable, count, ref_ordinal);   // lane 152 (16): the state is READ first, the decision comes after
    if (fault == Fault::SkipUnreadable) readable = true, count = 0;                           // PLANT: the skip is decided before / without the state read
    if (!readable) si.packet_ok = false, si.packet_why = Why::Unreadable;
    else if (count > kMaxStates) si.packet_ok = false, si.packet_why = Why::StateOverflow;
    else {
      Packet p;
      p.frame = in.frame, p.object = in.object, p.ordinal = v.ordinal, p.call = fault == Fault::CallIndexFromHook ? v.call : ref_ordinal;   // (review S6) the light-state ref's ordinal, not the hook's own count
      out.packet_call = p.call;
      si.packet_ok = packets.Put(std::move(p));
      si.packet_why = Why::TableFull;
      out.packet_written = si.packet_ok;
    }
  }
  out.why = Decide(si, fault);
  if (fault == Fault::HoleNoPacket && out.why == Why::Skip && out.packet_written) {   // PLANT: the marker goes out without its packet (a producer that wrote the marker, then lost the packet)
    (void)packets.Take(in.frame, in.object, v.ordinal);
    out.packet_written = false;
  }
  if (out.why != Why::SwitchOff) tally.Note(out.why);
  return out;
}

// ---- which path of sub_82BD7B50 (the model's cached command-list manager) this call will take: pure reads, the same the function makes (listing 0x82BD7B6C-0x82BD7C44) -------------------------------
// rd(addr, word) reads a big-endian guest word (false: unreadable). model = r3, r13 = the guest thread register.
template <class Rd>
inline CachePath PredictPath(Rd rd, uint32_t model, uint32_t r13) {
  uint32_t w888 = 0, w600 = 0;
  if (!rd(model + 888, w888) || !rd(model + 600, w600)) return CachePath::Unreadable;
  if (w888 == 0 || (w600 & 1) == 0) return CachePath::Fresh1;
  uint32_t tls = 0, blk = 0, slot = 0;
  if (!rd(r13, tls)) return CachePath::Unreadable;
  if (tls) {
    if (!rd(tls + 1048, blk)) return CachePath::Unreadable;
    if (blk && !rd(blk + 32, slot)) return CachePath::Unreadable;
  }
  uint32_t list = 0, entry = 0;
  if (!rd(model + 896, list)) return CachePath::Unreadable;
  if (!list || !rd(list + (slot << 2), entry)) return CachePath::Unreadable;
  if (entry != 0) return CachePath::Fresh2;
  uint32_t flags = 0, handles = 0, handle = 0, head = 0;
  if (!rd(model + 900, handles) || !handles || !rd(handles + (slot << 2), flags)) return CachePath::Unreadable;
  if (!rd(model + (flags ? 852u : 848u), handle) || !handle || !rd(handle, head)) return CachePath::Unreadable;
  return head == 0 ? CachePath::Record3 : CachePath::Cached4;
}

// ---- lane 228: which path of a CASTER pass's cached-list twin this call takes (sub_82BD8680 map, sub_82BD8958 plane, sub_82BD8A60 rim): pure reads, the same the twins make [C listings 0x82BD8698-0x82BD8758,
// 0x82BD8964-0x82BD89FC, 0x82BD8A6C-0x82BD8B08, read 2026-11-13]. pass = the scene record's CasterPass (1 map, 2 plane, 3 rim).
//   * path 1 (Fresh1): [model+888] == 0 or ([model+600] & bit) == 0, bit = 2 (map) / 4 (plane) / 8 (rim): no cache, drawn fresh -> skippable. The MAP twin has a second way to the same result (0x82BD8710-0x82BD8720):
//     with no list in the slot and the collapse flag set (sub_82BA7808 = word 0x83929A50 + 4 * slot, map A only) it returns 0 with local untouched = drawn fresh -> Fresh1.
//   * path 2 (Fresh2, never skipped): [[model+896] + slot*4] != 0: local = 1 and the body hands local to sub_82BFBE30 as r5 (0x82C020A0 / 0x82BF9A88 / 0x82BF4D2C) -> NOT skipped.
//   * path 3 / 4: handle [model + (flags ? hi : lo)], flags = [[model+900] + slot*4]; map 860 / 856, plane 868 / 864, rim 876 / 872; [handle] == 0: the game begins its cache (Record3), else it replays it (Cached4).
inline constexpr uint32_t kCasterCollapseTable = 0x83929A50;   // = scene_live\caster_core.h kCollapseTable (the hook header static_asserts the two equal)
template <class Rd>
inline CachePath PredictPathCaster(Rd rd, uint32_t pass, uint32_t model, uint32_t r13) {
  const uint32_t bit = pass == 1 ? 2u : pass == 2 ? 4u : pass == 3 ? 8u : 0u;
  const uint32_t handle_hi = pass == 1 ? 860u : pass == 2 ? 868u : 876u;
  if (!bit) return CachePath::Unreadable;
  uint32_t w888 = 0, w600 = 0;
  if (!rd(model + 888, w888) || !rd(model + 600, w600)) return CachePath::Unreadable;
  if (w888 == 0 || (w600 & bit) == 0) return CachePath::Fresh1;
  uint32_t tls = 0, blk = 0, slot = 0;
  if (!rd(r13, tls)) return CachePath::Unreadable;
  if (tls) {
    if (!rd(tls + 1048, blk)) return CachePath::Unreadable;
    if (blk && !rd(blk + 32, slot)) return CachePath::Unreadable;
  }
  uint32_t list = 0, entry = 0;
  if (!rd(model + 896, list)) return CachePath::Unreadable;
  if (!list || !rd(list + (slot << 2), entry)) return CachePath::Unreadable;
  if (entry != 0) return CachePath::Fresh2;   // never skipped (Decide: Why::CachePath2); for a caster the body also hands local to sub_82BFBE30 (cache-constants branch)
  if (pass == 1) {   // the collapse test is AFTER the list test in the map twin
    uint32_t flag = 0;
    if (!rd(kCasterCollapseTable + 4 * slot, flag)) return CachePath::Unreadable;
    if (flag != 0) return CachePath::Fresh1;
  }
  uint32_t flags = 0, handles = 0, handle = 0, head = 0;
  if (!rd(model + 900, handles) || !handles || !rd(handles + (slot << 2), flags)) return CachePath::Unreadable;
  if (!rd(model + (flags ? handle_hi : handle_hi - 4u), handle) || !handle || !rd(handle, head)) return CachePath::Unreadable;
  return head == 0 ? CachePath::Record3 : CachePath::Cached4;
}

// Taking a Skip decision back (the original then records the pass in full): the packet leaves the table, the marker counts (frame tally and cumulative) move from skip to `why`.
inline void WithdrawSkip(CallOutcome& out, const CallInputs& in, PacketTable& packets, GuestTally& tally, Why why) {
  if (out.packet_written) (void)packets.Take(in.frame, in.object, out.ordinal);
  out.packet_written = false;
  out.why = why;
  tally.why[uint8_t(Why::Skip)].fetch_sub(1, std::memory_order_relaxed);
  if (tally.markers.load(std::memory_order_relaxed)) tally.markers.fetch_sub(1, std::memory_order_relaxed);
  tally.cum_why[uint8_t(Why::Skip)].fetch_sub(1, std::memory_order_relaxed);   // (lane 237's cumulative counters)
  if (tally.cum_markers.load(std::memory_order_relaxed)) tally.cum_markers.fetch_sub(1, std::memory_order_relaxed);
  tally.Note(why);
}

// A caster call whose tracker record cannot back the marker: after DecideCall said Skip, the hook checks that the pass-entry tracker (the record the binder joins by (frame, pass, model, ordinal)) wrote THIS
// call under the ordinal the hook's ledger gave the packet. `tracker_ordinal` = the tracker's ordinal for the call (-1: the tracker wrote no record: unregistered, table full, an old frame). A mismatch withdraws the packet:
// Why::Unreadable, counted as dropped. Returns true when the call was withdrawn. The plant (fault SkipUnreadable) leaves it alone.
inline bool WithdrawIfRecordUnbacked(CallOutcome& out, const CallInputs& in, int64_t tracker_ordinal, PacketTable& packets, GuestTally& tally, Fault fault = Fault::None) {
  if (out.why != Why::Skip) return false;
  if (tracker_ordinal == int64_t(out.ordinal) || fault == Fault::SkipUnreadable) return false;
  WithdrawSkip(out, in, packets, tally, Why::Unreadable);
  return true;
}

// lane 228 (lane 226's review): the plane pass (CasterPass 2) is NEVER skipped. Its c0-c16 are produced INSIDE sub_82BF22C8 (plane::Tracker::OnPartDraw at LR 0x82BF4D3C), so a skip would leave the plane
// registers unrecorded unless lane 226's follow-up snapshot at the pass entry exists AND is joined by the binder (kPlaneSkipBuilt flips then). A marked plane call is withdrawn (Why::NotMarkable: not counted as dropped, the refusal is by design).
inline constexpr bool kPlaneSkipBuilt = false;
inline bool WithdrawIfPassUnskippable(CallOutcome& out, const CallInputs& in, uint32_t pass, PacketTable& packets, GuestTally& tally, bool allow = false) {
  if (out.why != Why::Skip || pass != 2 || kPlaneSkipBuilt || allow) return false;
  WithdrawSkip(out, in, packets, tally, Why::NotMarkable);
  return true;
}

// lane 299 (the plane skip arm, SKIP_GAME_WORK_MAP.md 17.15): what a skipped plane call must still tell the plane shadow chain. plane_shadow_ours joins the stream's mask draws to the plane tracker's PlaneDraw entries
// (one per part draw INSIDE the pass, LR 0x82BF4D3C: plane_hook.h OnPartDraw). A skipped call never reaches that hook, so the hook REPLAYS the entries: the part list of (model, call index) as the last call the game recorded
// in full drew it (the verify arm proves the marker's sequence equals it: "length differs 0"). No list yet = no skip (the call is recorded in full and teaches it).
inline constexpr uint32_t kPlaneMaxParts = 32;   // the ring proxy draws 2 parts a call, the wrestler proxy 8
struct PlanePartList {
  uint16_t n = 0;
  uint16_t part[kPlaneMaxParts] = {};
};
class PlanePartLedger {
 public:
  // A plane call recorded in full drew these parts (in order); the newest recorded-in-full call wins (a changed part list is re-learned at the object's next proof frame, when every call of it is recorded in full). An overflowing call (more than kPlaneMaxParts) is forgotten: it is never replayed.
  void Store(uint32_t model, uint32_t call, const uint16_t* parts, uint32_t n, bool overflow, bool keep_first = false) {   // keep_first = the plant (a list never changes once learned)
    std::lock_guard<std::mutex> lock(mu_);
    if (keep_first && map_.count(Key(model, call))) return;
    if (overflow || n == 0) {
      map_.erase(Key(model, call));
      return;
    }
    PlanePartList& l = map_[Key(model, call)];
    l.n = uint16_t(n > kPlaneMaxParts ? kPlaneMaxParts : n);
    for (uint32_t i = 0; i < l.n; ++i) l.part[i] = parts[i];
  }
  void Clear() {   // every list is forgotten (skip decisions are refused until a call is recorded in full again); the unit case calls it to prove the refusal, production never needs it: a stale list is overwritten by the next recorded-in-full call of that (model, call), and a skip needs one first
    std::lock_guard<std::mutex> lock(mu_);
    map_.clear();
  }
  bool Lookup(uint32_t model, uint32_t call, PlanePartList& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = map_.find(Key(model, call));
    if (it == map_.end()) return false;
    out = it->second;
    return true;
  }
  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
  }

 private:
  static uint64_t Key(uint32_t model, uint32_t call) { return (uint64_t(model) << 8) | (call & 0xFFu); }
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, PlanePartList> map_;
};
// The refusal: a plane Skip decision that cannot be replayed (`replayable` = the caller found a part list for (model, call) AND the plane tracker is there to take the entries) is taken back (packet withdrawn, Why::NotMarkable)
// and the call is recorded in full. Returns true only when IT withdrew (the caller counts / logs exactly those). `allow_unproven` is the plant (the old behaviour: skip anyway).
inline bool WithdrawIfPlaneUnreplayable(CallOutcome& out, const CallInputs& in, uint32_t pass, bool replayable, PacketTable& packets, GuestTally& tally, bool allow_unproven = false) {
  if (out.why != Why::Skip || pass != 2 || allow_unproven || replayable) return false;
  WithdrawSkip(out, in, packets, tally, Why::NotMarkable);
  return true;
}
// The replay: `sink(part)` once per part of the skipped call, in the order the game drew them (production: Tracker::OnPartDraw(model, part, frame), true when it made the entry). Returns how many entries were made.
template <class Sink>
inline uint32_t ReplayPlaneParts(const PlanePartList& l, Sink sink) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < l.n; ++i)
    if (sink(uint32_t(l.part[i]))) ++n;
  return n;
}

// lane 228 (review S2): is a cache recording OPEN on this thread's slot? sub_82BB4AE0 (begin) sets [entry+36] = 1 (0x82BB4B8C), the seal sub_82BB4B98 clears it (0x82BB4C34); recorder = word [0x8392D9B8], entry =
// [rec+104] + 68 * [[rec+156] + 4*slot] (0x82BB4B04-0x82BB4B40, slot as the twins read it). A caster pass that runs between an outer begin and its seal would have its marker sealed into that cache: refused (marker AND skip).
// recorder null = no recording at all (closed). Any unreadable word = Unreadable (refused: the safe side).
enum class OuterCache : uint8_t { Closed, Open, Unreadable };
inline constexpr uint32_t kCacheRecorderWord = 0x8392D9B8;
template <class Rd>
inline OuterCache CasterOuterRecording(Rd rd, uint32_t r13) {
  uint32_t rec = 0;
  if (!rd(kCacheRecorderWord, rec)) return OuterCache::Unreadable;
  if (rec == 0) return OuterCache::Closed;
  uint32_t tls = 0, blk = 0, slot = 0, base = 0, tab = 0, idx = 0, flag = 0;
  if (!rd(r13, tls)) return OuterCache::Unreadable;
  if (tls) {
    if (!rd(tls + 1048, blk)) return OuterCache::Unreadable;
    if (blk && !rd(blk + 32, slot)) return OuterCache::Unreadable;
  }
  if (!rd(rec + 104, base) || !rd(rec + 156, tab) || !tab || !rd(tab + (slot << 2), idx)) return OuterCache::Unreadable;
  if (!rd(base + 68u * idx + 36u, flag)) return OuterCache::Unreadable;
  return flag != 0 ? OuterCache::Open : OuterCache::Closed;
}
// The refusal: any Open / Unreadable state turns a Skip decision into Why::OuterRecording (counted dropped, so the verify run measures it) and, for the caller, no marker is written at all.
inline bool WithdrawIfOuterRecording(CallOutcome& out, const CallInputs& in, OuterCache state, PacketTable& packets, GuestTally& tally, bool allow = false) {
  if (out.why != Why::Skip || state == OuterCache::Closed || allow) return false;
  WithdrawSkip(out, in, packets, tally, Why::OuterRecording);
  return true;
}

// lane 246 (Ring D3, 16.8 precondition 1-2 and the caster precedent of WithdrawIfRecordUnbacked): a ring call whose ring-rows record cannot back the marker. After DecideCall said Skip, the hook checks that the tracker
// (ring_rows_hook.h, run at the entry BEFORE the decision) wrote THIS call with its node rows, and under the `call` the packet carries (the join of the ring record is (hook frame, object, call)). `backed` = the entry was
// recorded with n_nodes > 0 and no unreadable / count-unknown flag; `tracker_call` = its call index. A mismatch withdraws the packet (Why::Unreadable, counted as dropped). Returns true when withdrawn.
inline bool WithdrawIfRingRowsUnbacked(CallOutcome& out, const CallInputs& in, bool backed, uint32_t tracker_call, PacketTable& packets, GuestTally& tally, bool allow = false) {
  if (out.why != Why::Skip || allow) return false;
  if (backed && out.packet_written && tracker_call == out.packet_call) return false;
  WithdrawSkip(out, in, packets, tally, Why::Unreadable);
  return true;
}

// What the ring hook saw (the game thread adds, the render thread reads): ring-class calls claimed, the cached-list path each took (PredictPath: the colour twin's four), calls whose ring-rows record was missing / empty / under another
// call, skips withdrawn for an open outer cache recording, calls that took the nested exit, and the two bars of lane 237 applied to the ring: `fresh2 skipped` (a Skip decided on a path-2 call) and `non-fresh1 skipped`: both must be 0.
struct RingHookStats {
  std::atomic<uint64_t> claimed{0}, no_rows{0}, call_differs{0}, outer_open{0}, nested{0}, fresh2_skipped{0}, nonfresh1_skipped{0}, table_reads{0}, table_missing{0};
  std::string Line() const {   // the path histogram is the ring TALLY's (g_rtally.cum_path, filled by DecideCall -> NotePath: lane 237), so the verify run reads the one the decision used
    std::string s = "[rdr][d3][ring] hook: ring calls " + std::to_string(claimed.load(std::memory_order_relaxed)) + " (paths";
    static const char* const pn[] = {"fresh1", "fresh2", "record3", "cached4", "unreadable"};
    for (uint8_t p = 0; p < 5; ++p) s += std::string(" ") + pn[p] + " " + std::to_string(g_rtally.cum_path[p].load(std::memory_order_relaxed));
    s += "; no rows " + std::to_string(no_rows.load(std::memory_order_relaxed)) + ", call differs " + std::to_string(call_differs.load(std::memory_order_relaxed)) + ", outer cache recording " +
         std::to_string(outer_open.load(std::memory_order_relaxed)) + "); nested " + std::to_string(nested.load(std::memory_order_relaxed)) + ", class table reads " + std::to_string(table_reads.load(std::memory_order_relaxed)) +
         " (missing " + std::to_string(table_missing.load(std::memory_order_relaxed)) + "); fresh2 skipped " + std::to_string(fresh2_skipped.load(std::memory_order_relaxed)) + ", non-fresh1 skipped " +
         std::to_string(nonfresh1_skipped.load(std::memory_order_relaxed)) + "; reader ordinal fills " + std::to_string(g_ring_fills.load(std::memory_order_relaxed)) + " (missing " + std::to_string(g_ring_fill_missing.load(std::memory_order_relaxed)) + ")";
    return s;
  }
};
inline RingHookStats g_rstats;

// What the caster hooks saw, for the log line the live binder prints (the game thread adds, the render thread reads): calls by pass, the cached-list path each took (PredictPathCaster), calls whose tracker record was
// missing or under another ordinal, calls withdrawn after a Skip decision, calls that took the nested/never-decided exit.
struct CasterHookStats {
  std::atomic<uint64_t> calls[4] = {}, path[4][5] = {}, no_record[4] = {}, ordinal_differs[4] = {}, outer_open[4] = {}, nested{0}, in_proof_frame[4] = {}, proof_markable[4] = {}, plane_skipped{0}, plane_replayed_parts{0}, plane_no_parts{0};   // lane 299: plane_skipped = plane calls the hook skipped, plane_replayed_parts = PlaneDraw entries it replayed for them, plane_no_parts = plane skips taken back for want of a part list; proof_markable (lane 268): calls of MARKED objects that met their proof frame
  static const char* PathName(uint8_t p) {
    static const char* const k[] = {"fresh1", "fresh2", "record3", "cached4", "unreadable"};
    return p < 5 ? k[p] : "?";
  }
  std::string Line() const {
    std::string s = "[rdr][d3][casters] hook:";
    static const char* const pn[] = {"", "map", "plane", "rim"};
    for (uint8_t ps = 1; ps <= 3; ++ps) {
      s += std::string(" ") + pn[ps] + " calls " + std::to_string(calls[ps].load(std::memory_order_relaxed)) + " (paths";
      for (uint8_t p = 0; p < 5; ++p) s += std::string(" ") + PathName(p) + " " + std::to_string(path[ps][p].load(std::memory_order_relaxed));
      s += "; no record " + std::to_string(no_record[ps].load(std::memory_order_relaxed)) + ", ordinal differs " + std::to_string(ordinal_differs[ps].load(std::memory_order_relaxed)) + ", outer cache recording " + std::to_string(outer_open[ps].load(std::memory_order_relaxed)) + ", marked objects in a proof frame " + std::to_string(proof_markable[ps].load(std::memory_order_relaxed)) + ");";
    }
    s += " nested " + std::to_string(nested.load(std::memory_order_relaxed));
    s += "; plane skip: calls skipped " + std::to_string(plane_skipped.load(std::memory_order_relaxed)) + ", PlaneDraw entries replayed " + std::to_string(plane_replayed_parts.load(std::memory_order_relaxed)) + ", taken back for want of a part list " + std::to_string(plane_no_parts.load(std::memory_order_relaxed));   // lane 299
    return s;
  }
};
inline CasterHookStats g_cstats;

// ---- one call, begin to end: what the hook's CallScope does, in the header the unit test compiles (lane 138 review fix 2) --------------------------------------------------------------------
struct GuestCall {
  CallInputs in;
  CallOutcome out;
  bool begun = false;
};
inline GuestCall BeginGuestCall(GuestObjects& objs, const CallInputs& in, const MarkSet* marks, PacketTable& packets, GuestTally& tally, const Gate& gate, Fault fault = Fault::None) {
  GuestCall c;
  c.in = in;
  c.out = DecideCall(objs, in, marks, packets, tally, gate, fault);
  c.begun = true;
  return c;
}
// `kept` = what the game recorded in full (ignored for a skipped call). Every begun call is ended: a skipped one by its expected entry, a kept one by what it drew.
inline void EndGuestCall(GuestObjects& objs, const GuestCall& c, CallInfo kept, Fault fault = Fault::None, GuestTally* tally = nullptr) {
  if (!c.begun) return;
  if (tally) tally->TouchFrame(c.in.frame);   // lane 301: the hook saw this frame (a frame with no keyed call is still compared with the binder's keyed runs, which must then be 0 too)
  if (c.out.why == Why::Skip) {
    objs.EndSkipped(c.in.object, c.in.frame, c.out.call);
    if (tally) tally->NoteKeyedCall(c.in.frame);   // lane 152 (17): a skipped call was eligible only because it drew keyed parts last time
  } else {
    objs.End(c.in.object, c.in.frame, c.out.call, kept);
    if (tally && kept.Keyed()) tally->NoteKeyedCall(c.in.frame);
  }
}

// ---- lane 245 (Wrestlers D3, the live seam; SKIP_GAME_WORK_MAP.md 18.9): the wrestler colour call's decision, built from the 18.7 preconditions. The hook (scene_live\d3_wrestler_hook.h) and tools\d3\d3_unit_test.cpp both call
// BeginWrestlerCall, so what is tested is what ships. The wrestler colour call is the same function as the arena's (sub_82C04DF0) and takes the same cached-list twin (sub_82BD7B50), so the path predictor is the arena's PredictPath;
// what is the wrestler class's own: the ledger / packets / marks / gate / tally the caller passes (the wrestler globals g_w*, never the arena's), the class switch (CallInputs::wrestler -> g_wrestlers_d3) and the refusals below.
//   1. CallInputs::path DEFAULTS to Unreadable for a wrestler call (WrestlerCallInputs) and is set by PredictPath BEFORE the decision (18.7 precondition 1);
//   2. only Fresh1 skips (DecideCall: no packet and Why::CachePath2/3/4/Unreadable for any other path; precondition 2); the verify arm writes a marker only for a Skip decision, so it follows the same rule;
//   3. a Skip decision while an OUTER cache recording is open on the thread's slot is taken back (no packet, no marker) in BOTH arms (lane 228 review S2: CasterOuterRecording);
//   4. a Skip decision needs the hook frame's light camera (a caster MAP entry with a palette) and rim depth (a rim entry) in the caster tracker (18.7 precondition 7): otherwise taken back (Why::Unreadable, counted dropped).
inline CallInputs WrestlerCallInputs() {
  CallInputs in;
  in.wrestler = true;
  in.path = CachePath::Unreadable;   // precondition 1: never Fresh1 by default
  return in;
}
// A wrestler call whose frame lacks the light camera or the rim depth cannot back its marker's TexProjLight / LightEye / RimLightDepth: the packet is taken back and the call is recorded in full.
inline bool WithdrawIfNoCasterRecords(CallOutcome& out, const CallInputs& in, bool have_camera, bool have_rim, PacketTable& packets, GuestTally& tally) {
  if (out.why != Why::Skip || (have_camera && have_rim)) return false;
  WithdrawSkip(out, in, packets, tally, Why::Unreadable);
  return true;
}
// lane 245 (reviewer SF2): an object that has sphere occluders (a skin body) needs lane 242's row of THIS hook frame behind the marker's c168-c171; `sphere_state` = Tracker::RowState (0 never a sphere skeleton, 1 row present,
// 2 known but no row yet). State 2 takes the skip back (Why::Unreadable, counted `no sphere row`).
inline bool WithdrawIfNoSphereRow(CallOutcome& out, const CallInputs& in, int sphere_state, PacketTable& packets, GuestTally& tally) {
  if (out.why != Why::Skip || sphere_state != 2) return false;
  WithdrawSkip(out, in, packets, tally, Why::Unreadable);
  return true;
}
// What the wrestler scope saw (game thread adds, render thread reads): calls by predicted path, the share that were marked (the calls a skip would cover), calls refused after a Skip decision, nested calls.
struct WrestlerHookStats {
  std::atomic<uint64_t> calls{0}, path[5] = {}, marked{0}, marked_path[5] = {}, outer_open{0}, no_camera{0}, no_rim{0}, no_sphere{0}, nested{0}, in_proof_frame{0};
  std::string Line() const {
    std::string s = "[rdr][d3][wrestlers] hook: calls " + std::to_string(calls.load(std::memory_order_relaxed)) + " (paths";
    for (uint8_t p = 0; p < 5; ++p) s += std::string(" ") + CasterHookStats::PathName(p) + " " + std::to_string(path[p].load(std::memory_order_relaxed));
    s += "); calls the mark set holds " + std::to_string(marked.load(std::memory_order_relaxed)) + " (paths";
    for (uint8_t p = 0; p < 5; ++p) s += std::string(" ") + CasterHookStats::PathName(p) + " " + std::to_string(marked_path[p].load(std::memory_order_relaxed));
    s += "); refused after a skip decision: outer cache recording " + std::to_string(outer_open.load(std::memory_order_relaxed)) + ", no light camera " + std::to_string(no_camera.load(std::memory_order_relaxed)) +
         ", no rim depth " + std::to_string(no_rim.load(std::memory_order_relaxed)) + ", no sphere row " + std::to_string(no_sphere.load(std::memory_order_relaxed)) + "; nested " + std::to_string(nested.load(std::memory_order_relaxed));
    return s;
  }
};
inline WrestlerHookStats g_wstats;
// The plants of tools\d3\d3_unit_test.cpp (the wrestler block); production passes the default (all false).
struct WrestlerFaults {
  bool path_unset = false;         // the path is never predicted: the old default (Fresh1)
  bool path2_as_fresh = false;     // a list in the slot (path 2) is taken for a fresh draw
  bool outer_ignored = false;      // the outer cache recording is not looked at
  bool camera_ignored = false;     // the light camera / rim depth are not required
  bool sphere_ignored = false;     // a known sphere skeleton without a row this frame is skipped all the same
};
// One wrestler call's decision. `in` carries object, frame, now_ms, owner, state_readable / state_count / probe, proof_skew (WrestlerCallInputs() as the base); `rd` reads guest words; `cams(frame, have_camera, have_rim)` asks the
// caster tracker; `spheres(frame, object)` asks the sphere tracker (0 / 1 / 2, see WithdrawIfNoSphereRow). Everything the call touches is the caller's: ledger `objs`, `packets`, `tally`, `gate`, `st`.
template <class Rd, class CamFn, class SphFn>
inline GuestCall BeginWrestlerCall(GuestObjects& objs, CallInputs in, uint32_t r13, Rd rd, CamFn cams, SphFn spheres, const MarkSet* marks, PacketTable& packets, GuestTally& tally, const Gate& gate, WrestlerHookStats& st,
                                   Fault fault = Fault::None, const WrestlerFaults& pl = {}) {
  in.wrestler = true;
  if (!pl.path_unset) {
    in.path = PredictPath(rd, in.object, r13);   // precondition 1: BEFORE the decision
    if (pl.path2_as_fresh && in.path == CachePath::Fresh2) in.path = CachePath::Fresh1;
  }
  st.calls.fetch_add(1, std::memory_order_relaxed);
  st.path[uint8_t(in.path) < 5 ? uint8_t(in.path) : 4].fetch_add(1, std::memory_order_relaxed);
  GuestCall call = BeginGuestCall(objs, in, marks, packets, tally, gate, fault);
  if (marks && marks->Has(MarkKey(in.object, call.out.ordinal))) {
    st.marked.fetch_add(1, std::memory_order_relaxed);
    st.marked_path[uint8_t(in.path) < 5 ? uint8_t(in.path) : 4].fetch_add(1, std::memory_order_relaxed);
  }
  if (call.out.why == Why::Skip && !pl.outer_ignored) {
    const OuterCache oc = CasterOuterRecording(rd, r13);
    if (WithdrawIfOuterRecording(call.out, in, oc, packets, tally)) st.outer_open.fetch_add(1, std::memory_order_relaxed);
  }
  if (call.out.why == Why::Skip && !pl.camera_ignored) {
    bool have_camera = false, have_rim = false;
    cams(in.frame, have_camera, have_rim);
    if (WithdrawIfNoCasterRecords(call.out, in, have_camera, have_rim, packets, tally)) (have_camera ? st.no_rim : st.no_camera).fetch_add(1, std::memory_order_relaxed);
  }
  if (call.out.why == Why::Skip && !pl.sphere_ignored) {   // reviewer SF2: the skin bodies' c168-c171 come from lane 242's row of this hook frame
    if (WithdrawIfNoSphereRow(call.out, in, spheres(in.frame, in.object), packets, tally)) st.no_sphere.fetch_add(1, std::memory_order_relaxed);
  }
  return call;
}

}  // namespace rdr_adapter::d3
