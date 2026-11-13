// D3 skip the object, the BINDER side (Session 101, lane 138; docs\renderer\SKIP_GAME_WORK_MAP.md 8.3). No game or SDK includes: the binder, fview and tools\d3\ build THIS header.
//
// What it does.
//   * LEARN: every object call the game recorded in full (its keyed draws, lite or not) teaches a SEQUENCE: per part, the draw as the stream gave it (state, slots, samplers, bools, banks) and
//     the D1 template (program pair, geometry, layout, slots ours set). Sequences are per (object, keyed ordinal); the guest hook's mark key is the same pair.
//   * PROVE: in the object's proof frame (every K-th by hash; the reader makes all its parts full together) the marker path is composed from the OLD sequence and compared with the stream's own
//     banks over the variant's constant ranges, bit for bit, and with its fixed state, dynamic state, slots and bools. The object is "ledger complete" when nothing differs; the mark set
//     (d3_arena.h MarkPublisher) holds exactly the complete objects whose every key is still pinned.
//   * DRAW: a marker (a draw whose origin call_site is kMarkerCall) takes its packet from the side table (a missing one is a HOLE) and its object's sequence, builds a MINI frame of synthesized
//     part draws (flagged kDrawThin, banks composed) and the binder draws them through BindDraw, exactly as thin draws.
// Where each register comes from (d3_arena.h Group): pass = the frame's pass bank (the 90 % majority of the site-A draws, SKIP_LEDGER's rule; the mirrored copy = the same with x negated), template =
// the sequence, the record groups (light / shadow / world / clock) = the packet. Offline Mode::Stream the packet carries the stream's own banks ("emulated record fields"); Mode::Record (lane 152, d3_record.h)
// builds them from the frame's scene records (light states, arena worlds) joined by the hook's frame stamp, and a register with NO source (clock c186, drift, a bone array) is named and keeps its object unmarked.
//
// lane 211 (Caster D3 E2 / E3; SKIP_GAME_WORK_MAP.md 17.8): the SAME core serves the caster passes (map / plane / rim) when `route` is thin::Cls::Casters: runs are the consecutive caster-site draws of one model in one pass
// (ScanCasterRuns), the key object is the composite (model, pass) (d3_arena.h CasterObjectId) with the ordinal in the pass, a part needs NO thin-store entry (casters have no D1 form: the geometry comes from the
// casters hook's join, `Hooks::geometry`), the camera run and the palette rows are the record groups (d3_record.h CasterRecordRegister), and the slot -> node map of a part is learned from a recorded-in-full
// call against the wrestler record's bones. The arena's behaviour is unchanged (route == Arena).
// lane 229 (Ring D3 R3; SKIP_GAME_WORK_MAP.md 16.8): the SAME core serves the ring when `route` is thin::Cls::Ring: the arena's path (the thin store pins the parts, the pass bank is the colour draws' camera) with the gate on the Ring
// entries, its own marker call (kRingMarkerCall), a part's palette facts in `cpi` (slot -> node, 1-based, npal; slots past npal are DON'T CARE in the record route), the c76.. rows from the ring-rows record (d3_record.h
// RingRecordRegister) and a per-object call-count guard (GuardCalls). Its whole state is RingChain (below), self-contained like CasterChain; the Binder's members are binder_ring_d3.inc.
#pragma once
#include <array>
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../renderer/shaders/spv/fx_registry.h"
#include "capture_reader.h"
#include "d3_guest_core.h"
#include "d3_record.h"   // lane 152 (D3-F): the record route
#include "thin_store.h"

namespace rdr_adapter::d3 {

enum class Mode : uint8_t { Stream, Sourced, Record };   // the record groups' registers: the packet's emulated fields / withheld (readiness) / the scene record (not built: withheld)
inline const char* ModeName(Mode m) { return m == Mode::Stream ? "stream" : m == Mode::Sourced ? "sourced" : "record"; }

using Bank = std::array<uint32_t, 1024>;

struct PartDesc {
  thin::ThinKey tk;
  uint64_t key = 0;
  Draw draw;
  DrawFixed fixed;
  DrawDynamic dyn;
  std::vector<TextureSlot> slots;
  std::vector<rdr::SamplerDesc> samplers;   // parallel to slots
  BoolLoop bools;
  Bank vs_bank{}, ps_bank{};
  DrawOrigin origin;
  std::string technique, asset;
  int vs_variant = -1, ps_variant = -1;
  bool mirror = false, programmed = false, world_varies = false;
  std::bitset<256> drift_vs, drift_ps;   // registers a recorded-in-full frame showed unequal to their pass / template source (Group::Drift)
  bool ring = false;                     // lane 229 (Ring D3 R3): a part of the ring class (route Ring): its palette facts are `cpi` (slot -> node 1-based, npal), its World rows come from the ring-rows record (RingRecordRegister)
  bool caster = false;                   // lane 211: a caster part (map / plane / rim): its groups are GroupOfCaster, its record registers CasterRecordRegister
  bool wrestler = false;                 // lane 227: a wrestler colour part: its groups are GroupOfWrestler, its record registers WrestlerRecordRegister (the palette facts are `cpi`, learned like the casters')
  uint8_t pass = 0;                      // CasterPass
  CasterPartInfo cpi;                    // lane 211: the part's palette facts (slot -> record bone, bone count)
  bool has_layout = false;               // lane 217 (E2b): a caster part keeps the learned draw's geometry facts, so its vertex layout (and elements) travel with it: the mini frame is a different frame
  VertexLayout layout{};
  std::vector<VertexElement> elems;
  thin::ThinTemplate tpl;
  bool world_dynamic() const { return programmed || world_varies; }
};

struct Sequence {
  std::vector<PartDesc> parts;
  bool complete = false;
  uint64_t learned_frame = 0, proof_frame = 0;
  uint32_t object = 0, ordinal = 0;
  std::map<uint32_t, uint64_t> unsourced;   // lane 152 (Mode::Record): the registers of the last proof compare that had NO source (UnsourcedKey -> parts); non-empty = never marked
  uint32_t why = 1;                        // lane 152: UnmarkedWhy of the last proof compare (1 = no proof compare yet); the census names it
  uint64_t last_seen = 0, streak_start = 0;   // lane 152 (S7): the last stream frame a run or a marker of this object was met, and the first frame of its current unbroken run of frames
  uint64_t proof_mismatch = 0;             // parts that mismatched in the last proof compare
  size_t proven_drift = 0;                 // lane 301: the drift bits (all parts) the sequence carried when it was last certified complete; Publish compares the current parts with it (marked with an unsourced register, refreshed every frame)
  size_t DriftBits() const { size_t n = 0; for (const PartDesc& d : parts) n += d.drift_vs.count() + d.drift_ps.count(); return n; }
  std::map<uint32_t, uint64_t> mismatch_regs;   // lane 152: stage << 16 | reg -> parts that differed at it in the last proof compare (the census names the top ones)
};
enum class UnmarkedWhy : uint8_t { Marked, NoProofYet, PartsDiffer, Unsourced, Mismatch, StateDiffers, Unstable, NotLearnable, ProofOverdue, DriftAfterMark, Count };
inline const char* UnmarkedName(UnmarkedWhy w) {
  static const char* const k[] = {"marked", "no proof compare yet", "part list differs from the last frame", "unsourced register", "proof mismatch", "state differs", "part list changes frame to frame", "not learnable (a part not pinned / no variant)", "proof overdue (not re-proved within 2 x K frames while drawn)", "drift learned after the mark (frozen; re-proof needed)"};
  return uint8_t(w) < uint8_t(UnmarkedWhy::Count) ? k[uint8_t(w)] : "?";
}

struct Run {
  uint32_t object = 0, ordinal = 0, first_op = 0;
  uint32_t call = 0;   // lane 152: this run's index among ALL the object's runs in the frame (the hook's call index; a light-state ref's ordinal); `ordinal` is the keyed one
  uint8_t pass = 0;    // lane 211: the caster pass of the run (0 = the arena's); for a caster run `ordinal` and `call` are the index in that pass
  std::vector<uint32_t> draws, ops;   // the draws of the call and their positions in the op list
  uint32_t keyed = 0, other = 0;
  bool Keyed() const { return keyed > 0 && other == 0; }
};

struct ComposeCtx {
  Mode mode = Mode::Stream;
  Fault fault = Fault::None;
  const Bank* pass_vs = nullptr;    // the frame's pass bank
  const Bank* pass_ps = nullptr;
  const std::vector<uint8_t>* vs_has = nullptr;   // which pass registers the bank has a value for (the others fall back to the template)
  const std::vector<uint8_t>* ps_has = nullptr;
  const Bank* marker_vs = nullptr;  // the marker's own bank (the library's shadow before the object): Fault::PassMarker takes the pass registers from it
  const Bank* marker_ps = nullptr;
  const PartFields* pf = nullptr;   // the packet's emulated fields for this part (null: live)
  const RecordSrc* rec = nullptr;   // lane 152: Mode::Record: the call's records (light state, world, clock); null in the other modes
};

struct CompareStats {
  uint64_t reads = 0, withheld = 0, parts = 0, mismatching = 0, would_differ = 0;
  uint64_t by_group[uint8_t(Group::Count)] = {};
  std::map<uint32_t, uint64_t> by_reg;   // stage << 16 | reg -> parts that differ at it
  std::map<uint32_t, uint64_t> masks;   // bit per Group of the registers that differ in a part -> parts (the readiness projection: which groups would have to be sourced for a part to be right)
  uint64_t pass_fallback = 0;   // pass registers the frame's pass bank had no value for: taken from the template and COUNTED (review fix 14)
  uint64_t unsourced_regs = 0, unsourced_parts = 0;   // lane 152 (Mode::Record): registers / parts with NO source (never compared: an unsourced register keeps its object unmarked)
  std::map<uint32_t, uint64_t> unsourced;             // UnsourcedKey(why, stage, reg) -> count
  std::vector<uint32_t>* diffs = nullptr;   // when set: stage << 16 | reg of every pass / template register that differs (drift detection)
  uint64_t sphere_reads = 0, sphere_norow = 0, sphere_eq[3] = {};   // lane 245: the sphere rows' hook-frame offset census (NoteSphereOffsets): registers with a candidate row / with none / equal to the stream's at each candidate stamp
};

// lane 221 (E4): the compare of a caster WORLD register (palette rows) with the stream's. Map A's collapsed rows are W x M1 of two matrices whose zero elements carry the SIGN of the W elements they multiply
// (the stream holds -0.0 in some of them and +0.0 in others, 0x82C03DEC-0x82C03E7C), and W = [model+224] is not in any record: a zero is a zero. Every other float must be bit-equal.
inline bool SameVec4ZeroSigned(const uint32_t* a, const uint32_t* b) {
  for (int i = 0; i < 4; ++i)
    if (a[i] != b[i] && !((a[i] | 0x80000000u) == 0x80000000u && (b[i] | 0x80000000u) == 0x80000000u)) return false;
  return true;
}

inline bool DebugOn() {
  static const bool on = std::getenv("D3_DEBUG") != nullptr;
  return on;
}

inline bool PassRegister(uint16_t reg) { return GroupOf(reg, false) == Group::Pass; }

// Composes one part's two banks (zero outside the variant's ranges) from the sources. `truth` (when given) is the stream's bank: the withheld registers' values that are NOT blank are counted.
inline void ComposePart(const PartDesc& d, const ComposeCtx& c, Bank& out_vs, Bank& out_ps, CompareStats* st, const Bank* truth_vs, const Bank* truth_ps) {
  out_vs.fill(0), out_ps.fill(0);
  bool part_differs = false, part_unsourced = false;
  uint32_t part_mask = 0;
  const auto stage = [&](int variant, const Bank& tmpl, const Bank* pass, const std::vector<uint8_t>* has, const Bank* marker, const std::vector<uint32_t>* emulated, const std::bitset<256>& drift, Bank& out, const Bank* truth, uint32_t stage_id) {
    if (variant < 0) return;
    const rdr::fx::Variant& v = rdr::fx::kVariants[variant];
    for (uint32_t r = 0; r < v.constant_count; ++r) {
      const rdr::fx::ConstantRange& range = v.constants[r];
      for (uint32_t reg = range.reg; reg < uint32_t(range.reg) + range.count; ++reg) {
        const Group base_group = d.caster ? GroupOfCaster(uint16_t(reg), d.programmed, d.pass) : d.wrestler ? GroupOfWrestler(uint16_t(reg), stage_id, d.world_dynamic()) : GroupOf(uint16_t(reg), d.world_dynamic());
        const Group g = (drift.test(reg) && (base_group == Group::Pass || base_group == Group::Template)) ? Group::Drift : base_group;   // lane 152: a World register whose value moves is the World group's (the record sources it), not drift
        const uint32_t* t = &tmpl[reg * 4];
        uint32_t* o = &out[reg * 4];
        bool withheld = false, unsourced = false, dontcare = false;
        // lane 229 (Ring D3 R3, SKIP_GAME_WORK_MAP.md 16.3): a ring part's bone slots past its palette (c76 + 3 npal .. c138) are DON'T CARE in the record route: neither composed nor compared (the stale-slot bug cannot come back).
        const bool ring_dc = d.ring && c.mode == Mode::Record && stage_id == 0 && RingDontCare(d.cpi.npal, reg) && c.fault != Fault::StaleSlots;
        // plants (Record route): canvas_marked = the canvas' own registers c230-c237 (no source) taken from the template and COMPARED (the proof compare must see them move); alpha_zero = c185 taken as zero instead of the template
        const bool plant_canvas = d.ring && c.mode == Mode::Record && c.fault == Fault::RingCanvasMarked && reg >= 230 && reg <= 237;
        const bool plant_alpha = d.ring && c.mode == Mode::Record && c.fault == Fault::RingAlphaZero && reg == 185 && g == Group::Drift;
        if (ring_dc) {
          std::memcpy(o, t, 16);
          dontcare = true;
        } else if (plant_canvas) {
          std::memcpy(o, t, 16);
        } else if (plant_alpha) {
          std::memset(o, 0, 16);
        } else
        switch (g) {
          case Group::Pass: {
            const bool have_pass = pass && has && (*has)[reg];
            if (!have_pass && st) ++st->pass_fallback;
            const uint32_t* p = (have_pass && c.fault != Fault::NoPass) ? &(*pass)[reg * 4] : t;
            if (c.fault == Fault::PassMarker && marker) p = &(*marker)[reg * 4];
            std::memcpy(o, p, 16);
            if (d.mirror && reg >= 56 && reg <= 59 && c.fault != Fault::NoMirror && c.fault != Fault::NoPass) o[0] ^= 0x80000000u;   // the mirror rule: the camera with x negated
            break;
          }
          case Group::Template: std::memcpy(o, t, 16); break;
          default: {   // the record groups
            if (c.fault == Fault::RecordSets && g == Group::Light) { std::memcpy(o, t, 16); break; }
            if (c.fault == Fault::MovingStatic && g == Group::World) { std::memcpy(o, t, 16); break; }
            if (c.fault == Fault::PlacedIdentity && g == Group::World && d.programmed && reg >= 76 && reg <= 78) {
              static const float rows[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
              std::memcpy(o, rows[reg - 76], 16);
              break;
            }
            if (c.fault == Fault::SphereFromPrevFrame && d.wrestler && stage_id == 0 && reg >= 168 && reg <= 171) { std::memcpy(o, t, 16); break; }   // lane 227 PLANT: the spheres of the part's last learned frame
            if (st && truth && d.wrestler && stage_id == 0 && reg >= 168 && reg <= 171 && c.mode == Mode::Record && c.rec) NoteSphereOffsets(st->sphere_reads, st->sphere_norow, st->sphere_eq, *c.rec, reg, &(*truth)[reg * 4]);   // lane 245: the census of the [U] stamp offset
            if (c.mode == Mode::Stream && emulated && emulated->size() >= 1024) std::memcpy(o, &(*emulated)[reg * 4], 16);
            else if (c.mode == Mode::Record && c.rec) {   // lane 152: the register from the frame's records, or NO source (named, never guessed)
              uint32_t v4[4];
              const NoSrc why = RecordRegister(stage_id, reg, g, d.mirror, *c.rec, c.fault, v4, (d.caster || d.ring || d.wrestler) ? &d.cpi : nullptr);
              if (why == NoSrc::None) std::memcpy(o, v4, 16);
              else if (why == NoSrc::DontCare) dontcare = true;   // lane 211: past the part's own bone count: zero in the composed bank, never compared
              else {
                unsourced = part_unsourced = true;
                if (st) ++st->unsourced_regs, ++st->unsourced[UnsourcedKey(why, stage_id, reg)];
              }
            } else withheld = true;   // Sourced, or no emulated fields: nothing sources this register today
            break;
          }
        }
        if (st) {
          ++st->reads;
          if (withheld) ++st->withheld;
          if (truth && !unsourced && !dontcare && !(d.caster && g == Group::World ? SameVec4ZeroSigned(o, &(*truth)[reg * 4]) : std::memcmp(o, &(*truth)[reg * 4], 16) == 0)) {
            ++st->by_group[uint8_t(g)];
            part_mask |= 1u << uint8_t(g);
            if (st->diffs && (g == Group::Pass || g == Group::Template)) st->diffs->push_back(stage_id << 16 | reg);
            ++st->by_reg[stage_id << 16 | reg];
            if (withheld) ++st->would_differ;
            part_differs = true;
          }
        }
      }
    }
  };
  stage(d.vs_variant, d.vs_bank, c.pass_vs, c.vs_has, c.marker_vs, c.pf ? &c.pf->vs : nullptr, d.drift_vs, out_vs, truth_vs, 0);
  stage(d.ps_variant, d.ps_bank, c.pass_ps, c.ps_has, c.marker_ps, c.pf ? &c.pf->ps : nullptr, d.drift_ps, out_ps, truth_ps, 1);
  if (st) {
    ++st->parts;
    st->unsourced_parts += part_unsourced;
    st->mismatching += part_differs;
    if (part_differs) ++st->masks[part_mask];
  }
}

// ---- the callbacks the binder hands in (this header knows nothing of its internals) -----------------------------------------------------------------------------------------------------
struct Hooks {
  std::function<int(uint64_t)> variant;                       // shader hash -> rdr::fx::kVariants index (or -1)
  std::function<const thin::ThinStore::Entry*(uint64_t)> pinned;   // key hash -> the store's pinned entry (null: not pinned)
  // lane 211 (casters, which have no thin store): the part's own bone count (ResolvePartSource's list length; 0 = unknown, the palette then has no source), and whether OUR geometry for (model, part, pass) is
  // ready (the casters hook's join; unset = ready). The analogue of `pinned`: a part whose geometry is gone leaves the mark set.
  std::function<uint32_t(uint32_t object, uint32_t part, uint32_t pass)> palette_count;
  std::function<bool(uint32_t object, uint32_t part, uint32_t pass)> geometry;
  // lane 221 (17.10 goal 3): the palette slots the PART'S VERTICES can address, read from the stream's own geometry (the bone-id element, R8G8B8A8_UINT: the highest id + 1): registers c76 + 3 n .. past it are never
  // read by the shader, so they are exact don't-cares (the stale-slot bug can not come back). Used when palette_count gives 0; 0 = no id element / unreadable (the palette then has no source).
  std::function<uint32_t(const LoadedFrame& frame, const Draw& dr)> slots_used;
};

class D3Core {
 public:
  thin::Cls route = thin::Cls::Arena;   // lane 211: which class this core serves (Arena; Casters: no thin store, the pass sites decide). lane 229 (Ring D3 R3): Ring = the arena's path with the gate on the thin store's Ring entries, its own marker call (kRingMarkerCall), its c76.. rows from the ring-rows record and a call-count guard
  bool caster() const { return route == thin::Cls::Casters; }
  bool ring() const { return route == thin::Cls::Ring; }
  std::unordered_set<uint32_t> instanced;   // lane 274 (S2): objects the arena scan ever saw as more than one run (call >= 1) in a frame = called once per instance; measured: no arena object proper has two calls
  std::function<int(uint64_t)> variant_of = [](uint64_t) { return -1; };   // lane 274 (N2): never empty; SetD3Offline / the end-of-frame scans install FindVariant. lane 274: shader hash -> variant (the binder sets it for the arena route's scan: a wrestler's draws are exempt from the instance split)
  bool arena() const { return route == thin::Cls::Arena; }   // lane 274
  bool ArenaSplits() const { return arena() && fault != Fault::ArenaMergeInstances; }   // lane 274: the arena route's scan splits instance calls (the plant arena_merge_instances is the old merge)
  bool wrestlers() const { return route == thin::Cls::Wrestlers; }   // lane 227: the wrestler class's chain (pinned thin entries of Cls::Wrestlers, its own marker, the colour-call record route)
  // The key object of a run / marker: the model, or for a caster the composite (model, pass) (the plant key_by_pass_wrong drops the pass: the passes' sequences merge).
  uint32_t KeyObject(uint32_t object, uint32_t pass) const { return caster() ? CasterObjectId(object, fault == Fault::KeyByPassWrong ? 0u : pass) : object; }
  uint32_t KeyObject(const Run& r) const { return KeyObject(r.object, r.pass); }
  Mode mode = Mode::Record;     // live default (lane 152: the record route, d3_record.h); fview sets Stream / Sourced / Record
  Fault fault = Fault::None;
  // lane 226 follow-up (S1): a plane call may be marked only by an OFFLINE core. Its c0-c16 / c7 exist in the record only if the plane tracker snapshots them (at the pass entry, plane_hook.h OnPassEntry: built, inert,
  // [U] live) and EyePos / the palette are re-proved per proof frame; until a live verify run proves the entry snapshot, a live core (this stays false; SetD3CasterOffline turns it on for fview) never marks kCpPlane.
  bool plane_marks_allowed = false;
  bool plane_join_fallback = true;       // lane 268 (review S1): false on a LIVE core: a plane call whose record has no entry c7 (kCxPlanePos) gets NO PlaneDraw join (a skipped call has none, so verify composes exactly as skip would)
  uint64_t plane_no_entry_pos = 0;       // lane 268: plane CasterCalls (live core) whose record carries no kCxPlanePos
  bool force_mark = false;       // fview --d3-mode sourced: mark despite an incomplete ledger (the readiness measurement)
  int lag = 2;                   // the emulated hook sees the mark set published this many binds ago
  PacketTable* packets = &g_packets;
  MarkPublisher* marks = &g_marks;
  Gate* gate = &g_gate;
  // ---- lane 152 (D3-F) ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
  RecordIndex records;                       // the frame's scene records by the hook's frame stamp (fview: every record of the capture; live: the frames' own, evicted)
  uint32_t record_offset = kHookToStreamOffset;   // stream guest frame - the hook's frame (J1: +1). The emulated hook runs at stream frame - this; the plant record_offset0 sets 0
  bool record_route = kRecordRouteBuilt;     // Publish marks nothing in Mode::Record while the route is not built (live, flipped by the gate); fview's record arms turn it on
  bool verify = false;                       // (10) the verify arm: markers are written, nothing is dropped; the stream's draws are compared with the marker's sequence (its own switch, default off)
  GuestTally* hook_tally = &g_tally;         // (17) where the hook's keyed-call count lives: live g_tally, fview its own
  std::unordered_set<uint64_t> marked_prev;  // (11) the keys the last Publish marked: only their proof mismatches reset the gate
  std::map<std::string, uint64_t> reasons;   // objects not marked, by named reason (recomputed at every Publish)
  struct VerifyNote { uint32_t object = 0, ordinal = 0, call = 0; uint64_t hook_frame = 0; };
  std::vector<VerifyNote> verify_notes;      // the markers of the frame in progress (verify arm)
  uint32_t prev_obj_ = 0, prev_call_ = 0;    // the plant state_prev_object
  bool have_prev_ = false;

  D3Counters c;
  CompareStats readiness;                    // sourced-mode: marker draws composed vs the stream truth, every frame
  std::unordered_map<uint64_t, Sequence> seqs;
  std::unordered_map<uint64_t, std::pair<std::bitset<256>, std::bitset<256>>> ledger;   // key -> drift registers (learned online, or fed from an earlier pass: fview --d3-ledger-in)
  uint64_t skippable = 0, pinned_objects = 0;
  uint64_t caster_runs[4] = {}, caster_unlearnable[4] = {};   // lane 217: keyed caster runs seen / not learnable, by pass
  uint64_t wr_runs = 0, wr_unlearnable[8] = {};              // lane 227 (H4): keyed runs seen by the wrestler chain / those whose first failing part was refused, by CapWhy (NotPinned = the hook kept it: cloth, piece, unknown model, not learned yet; OtherClass = an arena / ring object)
  std::map<std::string, uint64_t> caster_unlearnable_why;
  std::unordered_set<uint64_t> unstable;     // MarkKeys whose part list changed between frames (offline ledger: the object is not markable; online the proof frame catches it)
  std::unordered_map<uint64_t, std::pair<Sequence, uint64_t>> retired;   // sequences that left the mark set: kept kRetireFrames more frames for the markers in flight
  static constexpr uint64_t kRetireFrames = 8;
  // lane 301 (SKIP_GAME_WORK_MAP.md 11.10): a marked sequence is a frozen, certified version. When a full frame changes a COMPLETE sequence (drift learned, part list changed, or its proof failed) the sequence keeps the last PROVEN parts here
  // for the markers whose mark-set generation (<= max_gen: the newest set published at the freeze) still held the object, exactly as `retired` does for a pin loss; the live sequence leaves the mark set at once (complete = false).
  struct Frozen { Sequence seq; uint64_t frame = 0; uint32_t max_gen = 0; };
  std::unordered_map<uint64_t, Frozen> frozen;
  static constexpr uint64_t kFreezeFrames = 16;   // longer than kRetireFrames: the live queue is 4-8 binds deep and a bind can repeat (CatchUp)
  bool freeze_on = std::getenv("D3_NO_FREEZE") == nullptr;   // the plant `no_freeze` (D3_NO_FREEZE=1) is the code before lane 301: drift learned replaces the marked parts at once (the 7118 / 7202 latch)
  std::vector<uint32_t> cached_objs_;       // lane 301: this frame's objects the hook's call took cache path 4 for (LoadCachedObjects)
  // lane 301 (10.12 5(b)): the hook counts a keyed call only when it recorded parts; a path-4 call (a replayed cached list) records none while the binder sees the list's runs. Loads the hook frame's list; false = it overflowed (the frame cannot be compared).
  bool LoadCachedObjects(uint64_t hook_frame) {
    cached_objs_.clear();
    bool overflow = false;
    if (hook_tally) hook_tally->CachedObjects(hook_frame, cached_objs_, &overflow);
    return !overflow;
  }
  bool IsCachedObject(uint32_t object) const { return std::find(cached_objs_.begin(), cached_objs_.end(), object) != cached_objs_.end(); }
  // lane 301 (step 5): the offline arm `template_step` (D3_TEMPLATE_STEP=N[:count]): from the Nth frame of the run the VS c185 (MdlAlpha) of the stream draws of `count` objects (default 3) that are MARKED by then steps every frame, as the game's fade does
  // (7118 / 7202). An env/capture edit, NOT a --d3-fault (drift detection is off under a planted fault). The objects are the first `count` marked keys (sorted) at frame N; the edit lives in binder-owned banks.
  uint64_t step_from = ~0ull;
  uint32_t step_count = 3;
  std::vector<uint32_t> step_objects;
  std::vector<ConstantBank> step_banks;
  uint64_t step_edits = 0;
  uint64_t drop_gen = 0;                     // the mark generation published when a sequence last had to go (a marker older than this finds no sequence: an epoch drop, not a hole)
  uint64_t epoch_drops = 0, skipped_in_proof = 0;
  uint64_t pass_marker_equal = 0, pass_marker_total = 0;   // info: how often the marker's own bank (the previous draw's) holds the pass bank's values
  uint64_t frames_with_markers = 0;
  uint64_t real_calls_total = 0;   // lane 274: the real-calls arm's hook total (info)
  GuestObjects guest;                        // fview's emulated hook state
  bool hook_from_record = false;             // lane 258 (ring, fview --d3-hook-from-record): the hook tally is fed from the records' ring-rows entries (one keyed call per entry, at its stamped frame), NOT from the emulated calls: calls = runs is then checked against the real hook's count
  // lane 289: how a cut call's draws keep their place in the thin ordinals (ring route). 0 = the LIVE form (default): the binder fills the marker's part triples from the published mark set (ThinBeginFrame), exactly what the live binder and reader do
  // (the draws are absent from the stream as live: nothing is listed in `reserved`); 1 = the ORACLE (D3_RESERVE_ORACLE=1, lane 258's offline form: the cut draws are listed in `reserved` and counted in place); 2 = NOTHING (D3_NO_RESERVE=1, the
  // planted fault `ordinal_shift` = today's live code before lane 289: a cut instance shifts the ordinals of the instances after it).
  int ordinal_fill_mode = std::getenv("D3_NO_RESERVE") ? 2 : std::getenv("D3_RESERVE_ORACLE") ? 1 : 0;
  bool fill_check = std::getenv("D3_FILL_CHECK") != nullptr;   // lane 289 (offline): ALSO list the cut draws (the oracle) and compare the keys of the live-form fill with the oracle's, frame by frame (fill_checked / fill_differ)
  uint64_t fills = 0, fill_missing = 0, fill_checked = 0, fill_differ = 0, fill_missing_frame = 0, reader_missing_seen = 0;   // (fill_missing_frame: this frame's misses, fed to the gate by D3REndFrame; reader_missing_seen: the reader's g_ring_fill_missing already reported)
    // lane 289: markers whose part triples the binder added to the ordinal count / found no mark set or key for (required 0) / frames compared with the oracle / frames whose keys differ (required 0)
  bool reserve_ordinals = ordinal_fill_mode == 1 || (fill_check && ordinal_fill_mode == 0);   // lane 289: the RING's oracle list only (the ARENA lists its cut draws in every mode but D3_NO_RESERVE, lane 274); lane 258 (ring route): the draws an emulated skip cuts are listed in `reserved` so the binder's thin ordinal count (ComputeKeys) still includes them: the instances AFTER a cut keep their thin keys (the plant: D3_NO_RESERVE=1)
  std::vector<std::pair<uint32_t, DrawOrigin>> reserved;   // (draw index, its original origin) of the draws cut in this frame
  std::unordered_set<uint32_t> hook_ring_objects;   // lane 258: the ring models the hook called in this frame (the records' ring-rows entries): the runs of these models are what the hook's count is compared with
  GuestTally tally;
  Gate local_gate;
  std::vector<std::shared_ptr<const MarkSet>> hist;

  void Reset() {
    seqs.clear();
    frozen.clear();
    ring_npal.clear();
    c = {};
    readiness = {};
    skippable = pinned_objects = 0;
    guest.Reset();
    hist.clear();
  }

  // ---- runs --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  // A run = the draws of one call of one object: consecutive model-part draws of the same object (non-draw ops between them end the run). ordinal = keyed runs of the object before it.
  // A MARKER of object X counts as one keyed run of X (review fix 3): its ordinal is the binder's keyed-run count, and call_site & 0xFF must equal it (`ordinal_mismatch` counts the others).
  static void ScanRuns(const LoadedFrame& frame, std::vector<Run>& runs, uint64_t* ordinal_mismatch = nullptr, bool count_markers = true, bool wrestler_markers = false, bool split_on_site_restart = false, const std::function<int(uint64_t)>* variant_of = nullptr) {   // lane 152: count_markers false = the verify arm (a marker is only a note: the object's real run follows); lane 227: wrestler_markers = this scan counts the WRESTLER chain's markers (not the arena's)
    runs.clear();
    Run cur;
    bool open = false;
    int64_t last_part[2] = {-1, -1};   // lane 274: the last KEYED part of the open run per pass site (0 = colour 0x82C0A950, 1 = the mirrored copy 0x82C0B94C)
    std::unordered_map<uint32_t, uint32_t> keyed_before, calls_before;
    const auto close = [&]() {
      last_part[0] = last_part[1] = -1;
      if (open && !cur.draws.empty()) {
        cur.call = calls_before[cur.object]++;   // lane 152
        cur.ordinal = keyed_before[cur.object];
        if (cur.Keyed()) ++keyed_before[cur.object];
        runs.push_back(cur);
      }
      cur = Run{};
      open = false;
    };
    for (uint32_t op = 0; op < frame.ops.count; ++op) {
      const Op& o = frame.ops[op];
      if (o.kind != OpKind::Draw) {
        close();
        continue;
      }
      const DrawOrigin* origin = frame.OriginOf(o.index);
      if (origin && (wrestler_markers ? IsWrestlerMarkerCall(origin->call_site) : (IsMarkerCall(origin->call_site) || IsRingMarkerCall(origin->call_site)))) {   // a marker: one keyed run of its object (lane 229: the ring class's markers too, so two classes' chains active on one frame count each other's markers as the runs they replaced; lane 227: the wrestler scan counts only its own, and the arena / ring scans never count a wrestler marker)
        close();
        const uint32_t expected = count_markers ? keyed_before[origin->object]++ : keyed_before[origin->object];
        if (count_markers) ++calls_before[origin->object];
        if (ordinal_mismatch && (origin->call_site & 0xFFu) != (expected & 0xFFu)) ++*ordinal_mismatch;
        continue;
      }
      if (!origin || origin->klass != DrawClass::ModelPart || !origin->object) {
        close();
        continue;
      }
      if (wrestler_markers && CasterPassOfSite(origin->pass_site)) {   // lane 227: a wrestler's RIM pass draws are ModelPart-class too; they are not a call of sub_82C04DF0 (the colour call: the light-state ref's ordinal counts those only), so they end the run and are not counted as one
        close();
        continue;
      }
      if (open && cur.object != origin->object) close();
      const bool keyed_site = origin->pass_site == thin::kSiteColour || origin->pass_site == thin::kSiteMirror;
      const int site = origin->pass_site == thin::kSiteMirror ? 1 : 0;
      // lane 274 (SKIP_GAME_WORK_MAP.md 10.12): a model called once per INSTANCE draws its parts again from part 0 for each call (rope x12, post x4 ...), and the instances are adjacent in the stream. With split_on_site_restart a keyed draw
      // whose part index is not above the last keyed part of the open run AT THE SAME SITE starts a new run (= a new call). Per site, because the arena draws every part twice in one call (site A then its mirrored copy at site B: parts 0 0 1 1 2 2).
      // A wrestler's colour draws (VS yCh_*) are exempt: a few of them draw part 0 twice at the same site in ONE call (A910B850 and two more in run 7147: 1 call, 2 part-0 draws), so the rule would over-split them (measured: S 3,750 vs 2,625 calls).
      if (split_on_site_restart && open && keyed_site && int64_t(origin->part) <= last_part[site] && !(variant_of && *variant_of && IsWrestlerVariant((*variant_of)(frame.draws[o.index].vs_hash)))) close();
      if (!open) cur.object = origin->object, cur.first_op = op, open = true;
      cur.draws.push_back(o.index);
      cur.ops.push_back(op);
      if (keyed_site) ++cur.keyed, last_part[site] = int64_t(origin->part);
      else ++cur.other;
    }
    close();
  }

  static bool IsWrestlerVariant(int v) { return v >= 0 && std::strncmp(rdr::fx::kVariants[v].name, "yCh_", 4) == 0; }   // lane 274
  // lane 211: the caster runs. A run = consecutive draws of one model at the caster pass sites of ONE pass whose part indices increase (a repeated part restarts a run: map A then map B of one model are two
  // runs; [L] the grouping, the census's `calls = runs` guard checks it live). The ordinal and call are counted per (model, pass); a caster marker counts as one run of its (model, pass).
  static void ScanCasterRuns(const LoadedFrame& frame, std::vector<Run>& runs, uint64_t* ordinal_mismatch = nullptr, bool count_markers = true, Fault fault = Fault::None) {
    runs.clear();
    Run cur;
    bool open = false;
    int64_t last_part = -1;
    std::unordered_map<uint32_t, uint32_t> before;   // composite (model, pass) -> runs before
    const auto keyobj = [&](uint32_t object, uint32_t pass) { return CasterObjectId(object, fault == Fault::KeyByPassWrong ? 0u : pass); };
    const auto close = [&]() {
      if (open && !cur.draws.empty()) {
        uint32_t& n = before[keyobj(cur.object, cur.pass)];
        cur.call = cur.ordinal = fault == Fault::OrdinalMerge && cur.pass == 1 ? 0u : n;   // plant ordinal_merge: map A and map B keyed alike
        ++n;
        runs.push_back(cur);
      }
      cur = Run{};
      open = false;
      last_part = -1;
    };
    for (uint32_t op = 0; op < frame.ops.count; ++op) {
      const Op& o = frame.ops[op];
      if (o.kind != OpKind::Draw) {
        close();
        continue;
      }
      const DrawOrigin* origin = frame.OriginOf(o.index);
      if (origin && IsCasterMarkerCall(origin->call_site)) {
        close();
        const uint32_t pass = CasterPassOfMarker(origin->call_site);
        uint32_t& n = before[keyobj(origin->object, pass)];
        if (ordinal_mismatch && (origin->call_site & 0xFFu) != (n & 0xFFu)) ++*ordinal_mismatch;
        if (count_markers) ++n;
        continue;
      }
      const uint32_t pass = origin ? CasterPassOfSite(origin->pass_site) : 0u;
      if (!origin || !IsCasterClass(origin->klass) || !origin->object || !pass) {
        close();
        continue;
      }
      if (open && (cur.object != origin->object || cur.pass != pass || int64_t(origin->part) <= last_part)) close();
      if (!open) cur.object = origin->object, cur.pass = uint8_t(pass), cur.first_op = op, open = true;
      cur.draws.push_back(o.index);
      cur.ops.push_back(op);
      ++cur.keyed;
      last_part = int64_t(origin->part);
    }
    close();
  }
  // The caster draws' keys: (model, part, pass) mixed (no thin store key exists for them). 0 = not a caster draw.
  static void CasterKeys(const LoadedFrame& frame, std::vector<uint64_t>& keys) {
    keys.assign(frame.draws.count, 0);
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const DrawOrigin* o = frame.OriginOf(i);
      if (!o || !IsCasterClass(o->klass) || !o->object) continue;
      const uint32_t pass = CasterPassOfSite(o->pass_site);
      if (pass) keys[i] = thin::Mix(thin::Mix(thin::Mix(0xCA5E, o->object), o->part), pass) | 1ull;
    }
  }
  // lane 258: the ring route splits a run at a part restart (one run per instance); the plant ring_merge_instances brings the old rule (instances merged into one run) back.
  bool RingSplits() const { return ring() && fault != Fault::RingMergeInstances; }
  // The runs of the frame for this core's class.
  void ScanFor(const LoadedFrame& frame, std::vector<Run>& runs, uint64_t* ordinal_mismatch, bool count_markers) const {
    if (caster()) ScanCasterRuns(frame, runs, ordinal_mismatch, count_markers, fault);
    else ScanRuns(frame, runs, ordinal_mismatch, count_markers, wrestlers(), RingSplits() || ArenaSplits(), &variant_of);
  }

  // ---- the pass bank: per register the value most of the site-A draws share (>= 90 %), as the ledger's `pass` source ------------------------------------------------------------------
  static void PassBank(const LoadedFrame& frame, Bank& vs, Bank& ps, std::vector<uint8_t>& vs_has, std::vector<uint8_t>& ps_has, bool wide = false) {
    vs.fill(0), ps.fill(0);
    vs_has.assign(256, 0), ps_has.assign(256, 0);
    static const uint16_t regs[] = {56, 57, 58, 59, 72, 74, 180, 184};
    static const uint16_t wide_regs[] = {56, 57, 58, 59, 72, 74, 180, 184, 52, 53, 54};   // lane 227: + View c52-c54 (the wrestlers' programs read it; every main-pass draw of the frame shares it)
    for (int stage = 0; stage < 2; ++stage) {
      const uint16_t* rp = wide ? wide_regs : regs;
      const size_t rn = wide ? sizeof wide_regs / sizeof wide_regs[0] : sizeof regs / sizeof regs[0];
      for (size_t ri = 0; ri < rn; ++ri) {
        const uint16_t reg = rp[ri];
        std::map<std::array<uint32_t, 4>, uint32_t> votes;
        uint32_t total = 0;
        for (uint32_t i = 0; i < frame.draws.count; ++i) {
          const DrawOrigin* o = frame.OriginOf(i);
          if (!o || o->klass != DrawClass::ModelPart || o->pass_site != thin::kSiteColour) continue;
          const Draw& d = frame.draws[i];
          if (stage == 1 && !d.ps_hash) continue;
          const ConstantBank& b = frame.constant_banks[stage ? d.ps_bank : d.vs_bank];
          std::array<uint32_t, 4> v;
          std::memcpy(v.data(), &b.dwords[reg * 4], 16);
          ++votes[v];
          ++total;
        }
        if (!total) continue;
        auto best = votes.begin();
        for (auto it = votes.begin(); it != votes.end(); ++it)
          if (it->second > best->second) best = it;
        if (DebugOn() && best->second * 10 < total * 9) std::fprintf(stderr, "[d3dbg] frame %llu stage %d reg %u: %zu values, best %u of %u\n", (unsigned long long)frame.header.guest_frame, stage, reg, votes.size(), best->second, total);
        if (best->second * 10 >= total * 9 && best->second >= 2) {
          std::memcpy(&(stage ? ps : vs)[reg * 4], best->first.data(), 16);
          (stage ? ps_has : vs_has)[reg] = 1;
        }
      }
    }
  }

  // ---- capture a part descriptor from the stream's draw i ----------------------------------------------------------------------------------------------------------------------------
  bool Capture(const LoadedFrame& frame, uint32_t i, uint64_t key, const thin::ThinKey& tk, const Hooks& h, PartDesc& d, const PartDesc* previous) const {
    const DrawOrigin* o = frame.OriginOf(i);
    const bool caster_route = route == thin::Cls::Casters;
    const thin::ThinStore::Entry* en = (!caster_route && h.pinned) ? h.pinned(key) : nullptr;
    cap_why = CapWhy::Ok;
    if (!o) return cap_why = CapWhy::Other, false;
    if (caster_route) {   // lane 211: a caster part is any model-part draw at a caster pass site (no thin entry exists for it)
      if (CasterPassOfSite(o->pass_site) == 0 || !o->object) return false;
    } else if (!en || en->t.cls != route) {   // the gate: the class this core serves (it was `Cls::Arena` only)
      return cap_why = !en ? CapWhy::NotPinned : CapWhy::OtherClass, false;
    }
    const Draw& dr = frame.draws[i];
    d.tk = tk, d.key = key, d.draw = dr;
    d.fixed = frame.fixeds[dr.fixed];
    d.dyn = frame.dynamics[dr.dynamic];
    d.slots.assign(frame.texture_slots.data + dr.texture_first, frame.texture_slots.data + dr.texture_first + dr.texture_count);
    d.samplers.clear();
    for (const TextureSlot& s : d.slots) d.samplers.push_back(frame.samplers[s.sampler]);
    d.bools = frame.bool_loops[dr.bool_loop];
    std::memcpy(d.vs_bank.data(), frame.constant_banks[dr.vs_bank].dwords, sizeof(Bank));
    std::memcpy(d.ps_bank.data(), frame.constant_banks[dr.ps_bank].dwords, sizeof(Bank));
    d.origin = *o;
    d.technique = frame.String(o->technique);
    d.asset = frame.String(o->asset);
    d.vs_variant = h.variant ? h.variant(dr.vs_hash) : -1;
    d.ps_variant = dr.ps_hash && h.variant ? h.variant(dr.ps_hash) : -1;
    d.mirror = !caster_route && o->pass_site == thin::kSiteMirror;
    d.programmed = d.vs_variant >= 0 && std::strstr(rdr::fx::kVariants[d.vs_variant].name, "skinned") != nullptr;
    if (ring()) {   // lane 229 (Ring D3 R3): the part's palette facts. A variant whose constant ranges stop at c78 reads slot 0 only (npal 1); one that reaches the bone slots (> c78) needs the part's own palette size: the vertices' highest bone id + 1 (Hooks::slots_used), 0 = unknown (NoPalette, named)
      d.ring = true;
      if (previous) d.cpi = previous->cpi;
      bool reads_slots = false;
      if (d.vs_variant >= 0) {
        const rdr::fx::Variant& v = rdr::fx::kVariants[d.vs_variant];
        for (uint32_t r = 0; r < v.constant_count; ++r)
          if (uint32_t(v.constants[r].reg) + v.constants[r].count > 79 && v.constants[r].reg <= 138) reads_slots = true;
      }
      uint32_t np = h.palette_count ? h.palette_count(o->object, o->part, 0) : 0;
      if (np == 0 && reads_slots && h.slots_used) np = h.slots_used(frame, dr);
      if (np == 0 && !reads_slots) np = 1;
      if (np == 0 && !previous) {   // lane 258: a part key first met as a lite draw in this instance: another instance's full draw may have given the size
        const uint64_t nk = thin::KeyHash(tk.pass_site, tk.object, tk.part, 0, tk.program);   // per PART (ordinal 0), not per instance
        const auto kn = ring_npal.find(nk);
        if (kn != ring_npal.end()) d.cpi.npal = kn->second;
      }
      if (np) ring_npal[thin::KeyHash(tk.pass_site, tk.object, tk.part, 0, tk.program)] = d.cpi.npal = uint8_t(std::min<uint32_t>(np, kCasterMaxSlots));   // lane 258: a lite draw (97 % of the ring's) gives none: the part keeps the count of its last full frame (as the wrestlers); it was overwritten with 0 = NoPalette for every c76-c138 read
    }
    if (wrestlers()) {   // lane 227: a wrestler part reads the palette rows when the variant has a c76 range (skinned, single bone, rigid); the slots the part's vertices can address are don't-care past them
      d.wrestler = true;
      if (d.vs_variant >= 0) {
        const rdr::fx::Variant& v = rdr::fx::kVariants[d.vs_variant];
        for (uint32_t r = 0; r < v.constant_count; ++r)
          if (v.constants[r].reg == 76) d.programmed = true;
      }
      if (previous) d.cpi = previous->cpi;
      uint32_t np = h.palette_count ? h.palette_count(o->object, o->part, 0) : 0;   // the file's palette length (the wrestler core's joined model): known for a lite draw too
      if (np == 0 && h.slots_used) np = h.slots_used(frame, dr);   // else the highest bone id of the draw's vertices + 1 (0 for a lite draw: the part then keeps the count of its last full frame)
      if (np) d.cpi.npal = uint8_t(std::min<uint32_t>(np, kCasterMaxSlots));
      if (DebugOn() && (dr.flags & kDrawThin) == 0) std::fprintf(stderr, "[d3dbg] wrestler capture frame %llu object %08X part %u: slots_used %u npal %u (vertex bytes %llu layout %u)\n", (unsigned long long)frame.header.guest_frame, o->object, o->part, np, unsigned(d.cpi.npal), (unsigned long long)dr.vertex_bytes, dr.layout);
    }
    if (caster_route) {   // lane 211: the variant reads the palette rows when it has a c76 range (skinned, single_bone, rigid)
      d.caster = true, d.pass = uint8_t(CasterPassOfSite(o->pass_site)), d.programmed = false;
      if (d.vs_variant >= 0) {
        const rdr::fx::Variant& v = rdr::fx::kVariants[d.vs_variant];
        for (uint32_t r = 0; r < v.constant_count; ++r)
          if (v.constants[r].reg == 76) d.programmed = true;
      }
      if (dr.layout != 0xFFFFFFFFu && dr.layout < frame.layouts.count) {   // lane 217 (E2b)
        const VertexLayout& vl = frame.layouts[dr.layout];
        if (uint64_t(vl.element_first) + vl.element_count <= frame.elements.count) {
          d.has_layout = true, d.layout = vl;
          d.elems.assign(frame.elements.data + vl.element_first, frame.elements.data + vl.element_first + vl.element_count);
        }
      }
      if (previous) d.cpi = previous->cpi;
      d.cpi.part = uint16_t(o->part);   // lane 226 (E5)
      uint32_t np = h.palette_count ? h.palette_count(o->object, o->part, d.pass) : 0;
      if (np == 0 && h.slots_used) np = h.slots_used(frame, dr);   // lane 221
      d.cpi.npal = uint8_t(std::min<uint32_t>(np, kCasterMaxSlots));
    }
    d.world_varies = previous && previous->world_varies;
    if (previous) d.drift_vs = previous->drift_vs, d.drift_ps = previous->drift_ps;
    else if (auto lg = ledger.find(key); lg != ledger.end()) d.drift_vs = lg->second.first, d.drift_ps = lg->second.second;
    if (previous && std::memcmp(&d.vs_bank[76 * 4], &previous->vs_bank[76 * 4], 12 * 4) != 0) d.world_varies = true;
    if (en) d.tpl = en->t;
    if (!(d.vs_variant >= 0 && (dr.ps_hash == 0 || d.ps_variant >= 0))) return cap_why = CapWhy::NoVariant, false;
    return true;
  }
  // lane 227 (H4): why the last Capture refused a draw (the wrestler chain's census of the objects the hook keeps recorded in full)
  enum class CapWhy : uint8_t { Ok, NotPinned, OtherClass, NoVariant, Other };
  mutable CapWhy cap_why = CapWhy::Ok;
  mutable std::unordered_map<uint64_t, uint8_t> ring_npal;   // lane 258 (ring): the last palette size a full draw gave per part key: an instance of a part first learned from a lite draw takes it (the size belongs to the part, not the instance)

  // ---- lane 152: the record route's pieces ------------------------------------------------------------------------------------------------------------------------------------------------
  // The hook's frame for a stream frame: the emulated hook ran `record_offset` frames earlier (Mode::Record only; the plant record_offset0 joins at 0).
  uint64_t HookFrame(uint64_t fno) const {
    const uint32_t off = mode == Mode::Record && fault != Fault::RecordOffset0 ? record_offset : 0;
    return fno >= off ? fno - off : fno;
  }
  // One call's records (light state by call index, arena world by keyed ordinal, the clock). The plant state_prev_object hands the PREVIOUS lookup's light state to this call.
  bool RecordFor(uint64_t hook_frame, uint32_t object, uint32_t call, uint32_t ordinal, RecordSrc& rs, uint32_t pass = 0) {
    if (caster()) {   // lane 211: a caster entry's camera state and palette (the light-state plants do not apply)
      const bool ok = records.FindCaster(hook_frame, pass, object, ordinal, fault, rs);
      if (ok && pass == rdr_scene::kCpPlane && !plane_join_fallback) {   // lane 268 (review S1)
        if (!rs.have_plane_pos) ++plane_no_entry_pos;
        for (auto& p : rs.plane_draw) p = nullptr;
      }
      return ok;
    }
    if (wrestlers()) {   // lane 227: the colour call's light state + the hook frame's wrestler light camera + the object's bones
      bool ok = records.FindWrestler(hook_frame, object, call, ordinal, fault, rs);
      if (fault == Fault::StatePrev && have_prev_) {
        RecordSrc other;
        records.Find(hook_frame, prev_obj_, prev_call_, ordinal, other);
        rs.rows = other.rows, rs.have_light = other.have_light;
        ok = other.have_light;
      }
      prev_obj_ = object, prev_call_ = call, have_prev_ = true;
      return ok;
    }
    bool ok = records.Find(hook_frame, object, call, ordinal, rs);
    if (ring()) {   // lane 229: + the call's node rows (the ring-rows record, joined by the same stamp / object / call as the call's light state); a missing entry leaves the World registers without a source (NoWorldRecord), counted by the caller
      if (!records.FindRing(hook_frame, object, call, rs)) ring_rows_missing++;
    }
    if (fault == Fault::StatePrev && have_prev_) {
      RecordSrc other;
      records.Find(hook_frame, prev_obj_, prev_call_, ordinal, other);
      rs.rows = other.rows, rs.have_light = other.have_light;
      ok = other.have_light;
    }
    prev_obj_ = object, prev_call_ = call, have_prev_ = true;
    return ok;
  }
  // One part of the OLD sequence composed as a marker would be and compared with the stream's part `k` of the run (banks over the variant's ranges, then the fixed / dynamic state, slots, samplers, bools).
  // false = the state differs. The record route's unsourced registers are counted in `st`, never compared.
  bool CompareRunPart(const LoadedFrame& frame, const Run& r, size_t k, const PartDesc& d, const PartDesc& fresh, const Bank& pass_vs, const Bank& pass_ps, const std::vector<uint8_t>& hv, const std::vector<uint8_t>& hp,
                      const Bank& mk_vs, const Bank& mk_ps, const RecordSrc& rs, CompareStats& st) {
    const Draw& dr = frame.draws[r.draws[k]];
    PartFields pf;
    if (mode == Mode::Stream) {   // the emulated record fields: the stream's own banks (never in Mode::Record: there the sources are the records)
      pf.vs.assign(frame.constant_banks[dr.vs_bank].dwords, frame.constant_banks[dr.vs_bank].dwords + 1024);
      pf.ps.assign(frame.constant_banks[dr.ps_bank].dwords, frame.constant_banks[dr.ps_bank].dwords + 1024);
    }
    Bank cv, cp, tv, tp;
    std::memcpy(tv.data(), frame.constant_banks[dr.vs_bank].dwords, sizeof(Bank));
    std::memcpy(tp.data(), frame.constant_banks[dr.ps_bank].dwords, sizeof(Bank));
    ComposeCtx cc;
    cc.mode = mode, cc.fault = fault, cc.pass_vs = &pass_vs, cc.pass_ps = &pass_ps, cc.vs_has = &hv, cc.ps_has = &hp, cc.marker_vs = &mk_vs, cc.marker_ps = &mk_ps, cc.pf = mode == Mode::Stream ? &pf : nullptr;
    cc.rec = mode == Mode::Record ? &rs : nullptr;
    const uint64_t before = st.mismatching;
    ComposePart(d, cc, cv, cp, &st, &tv, &tp);
    if (DebugOn() && st.mismatching != before) {   // diagnosis (D3_DEBUG=1): the composed and the stream's rows of the differing registers
      std::fprintf(stderr, "[d3dbg] proof frame %llu object %08X pass %u ordinal %u call %u part %zu site %c light %d: ", (unsigned long long)frame.header.guest_frame, r.object, unsigned(r.pass), r.ordinal, r.call, k, d.mirror ? 'B' : 'A', rs.have_light);
      for (uint32_t reg : {56u, 57u, 58u, 59u, 64u, 65u, 66u, 67u, 68u, 69u, 70u, 71u, 76u, 77u, 78u, 162u, 166u, 181u, 182u})   // lane 217: + the caster camera registers
        if (std::memcmp(&cv[reg * 4], &tv[reg * 4], 16) != 0 && d.vs_variant >= 0) {
          float a[4], b[4];
          std::memcpy(a, &cv[reg * 4], 16), std::memcpy(b, &tv[reg * 4], 16);
          std::fprintf(stderr, "c%u composed (%g %g %g %g) stream (%g %g %g %g); ", reg, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        }
      std::fprintf(stderr, "\n");
    }
    if (!StateEqual(d, fresh)) {
      ++c.state_differ;
      return false;
    }
    return true;
  }
  uint64_t verify_by_group[uint8_t(Group::Count)] = {};
  std::unordered_set<uint64_t> verify_keys;   // MarkKeys of the frame's verify markers (a marker was written ahead of that object's real run)

  // lane 211: the slot -> record-bone map of each caster part of a recorded-in-full run: slot k's three rows (c76 + 3k + j) equal row j of exactly the bone n (bit for bit) -> slot_node[k] = n. Costly (21 slots x the
  // bones x 3 rows), so only a part with no map yet or in the object's proof frame is (re)learned. A slot matches exactly ONE bone or none: no match (a stale slot past the part's list) and an ambiguous match (two bones with equal rows) both stay kNoNode, counted apart; the previous node only survives while it is still the unique match.
  uint64_t slots_learned = 0, slots_unmatched = 0, slots_ambiguous = 0;
  void LearnCasterSlots(const LoadedFrame& frame, const Run& r, const RecordSrc& rs, bool proof_now, std::vector<PartDesc>& fresh) {
    for (size_t k = 0; k < fresh.size() && k < r.draws.size(); ++k) {
      CasterPartInfo& ci = fresh[k].cpi;
      const Draw& dr = frame.draws[r.draws[k]];
      if (dr.vs_bank >= frame.constant_banks.count) continue;
      const uint32_t* bank = frame.constant_banks[dr.vs_bank].dwords;
      const uint32_t slots = std::max<uint32_t>(ci.npal, 0);
      for (uint32_t slot = 0; slot < kCasterMaxSlots && (slot < slots || ci.npal == 0); ++slot) {
        // lane 221: a learned slot is kept until the object's proof frame; a slot still unresolved (an AMBIGUOUS match at learn time: two bones with equal rows in the first frames, or no bone yet) is tried again at
        // every recorded-in-full call, so the map completes as soon as the pose tells the bones apart.
        if (ci.learned && !proof_now && (ci.slot_node[slot] != kNoNode || ci.slot_collapsed[slot])) continue;
        uint8_t found = kNoNode;
        const auto matches = [&](uint32_t node) {
          const float* m = rs.bones[node].m;
          for (int j = 0; j < 3; ++j) {
            float row[4];
            for (int c = 0; c < 4; ++c) row[c] = m[c * 4 + j];
            if (std::memcmp(row, &bank[(76 + 3 * slot + j) * 4], 16) != 0) return false;
          }
          return true;
        };
        // (lane 211 SF2) exactly one bone may match: two bones with equal rows are AMBIGUOUS (kNoNode, never a guess); the previous node is kept only while it is still the unique match
        uint32_t n_match = 0, first = kNoNode;
        for (uint32_t node = 0; node < rs.bone_count && node < kNoNode; ++node)
          if (matches(node)) {
            if (!n_match) first = node;
            ++n_match;
          }
        if (n_match == 1 || (n_match > 1 && fault == Fault::FirstMatch)) found = uint8_t(first);
        ci.slot_node[slot] = found;
        // lane 221 (E4): in a map-A call (collapse flag 1) a slot that matches no bone and has a zero rotation in all three rows IS the collapse branch's (0, 0, 0, t) [C listing 0x82C03D7C-0x82C03EB0 +
        // the stream: every such row of run 7147's map-A draws has the one translation of the pose matrix]. Learned once per part and re-learned in the object's proof frame, where the OLD set is what is proved.
        bool zero_rot = n_match == 0 && rs.collapse != 0;
        for (int j = 0; j < 3 && zero_rot; ++j)
          for (int c = 0; c < 3; ++c)
            if ((bank[(76 + 3 * slot + j) * 4 + c] | 0x80000000u) != 0x80000000u) zero_rot = false;
        ci.slot_collapsed[slot] = zero_rot;
        ++slots_learned;
        slots_unmatched += found == kNoNode && n_match == 0;
        slots_ambiguous += n_match > 1 && found == kNoNode;
      }
      ci.learned = true;
    }
  }

  // ---- lane 229 (Ring D3 R3): the ring's slot -> node map and the call-count guard -------------------------------------------------------------------------------------------------------------------
  // A ring part's palette slot k (c76 + 3k + j) equals row j of exactly ONE node n (1-based) of the call's ring-rows entry, bit for bit (the packer cut node n from r5 + 64 (n - 1)); learned from a recorded-in-full call, as
  // LearnCasterSlots does for the casters. No match (a slot the vertices never address) and an ambiguous match (two nodes with equal rows) both stay kNoNode, counted apart; a learned slot is kept until the object's proof frame.
  uint64_t ring_slots_learned = 0, ring_slots_unmatched = 0, ring_slots_ambiguous = 0, ring_rows_missing = 0;
  void LearnRingSlots(const LoadedFrame& frame, const Run& r, const RecordSrc& rs, bool proof_now, std::vector<PartDesc>& fresh) {
    if (!rs.ring_e || !rs.ring_pool) return;
    const rdr_scene::RingRows& e = *rs.ring_e;
    for (size_t k = 0; k < fresh.size() && k < r.draws.size(); ++k) {
      CasterPartInfo& ci = fresh[k].cpi;
      const Draw& dr = frame.draws[r.draws[k]];
      if (dr.vs_bank >= frame.constant_banks.count) continue;
      const uint32_t* bank = frame.constant_banks[dr.vs_bank].dwords;
      for (uint32_t slot = 0; slot < kCasterMaxSlots && slot < ci.npal; ++slot) {
        if (ci.learned && !proof_now && ci.slot_node[slot] != kNoNode) continue;
        uint32_t n_match = 0, first = kNoNode;
        for (uint32_t node = 1; node <= e.n_nodes && node < kNoNode; ++node) {
          const float* rows = rdr_adapter::d3::NodeRowsOf1(*rs.ring_pool, e, node);
          if (rows && std::memcmp(rows, &bank[(76 + 3 * slot) * 4], 48) == 0) {
            if (!n_match) first = node;
            ++n_match;
          }
        }
        ci.slot_node[slot] = n_match == 1 ? uint8_t(first) : kNoNode;
        ++ring_slots_learned;
        ring_slots_unmatched += n_match == 0;
        ring_slots_ambiguous += n_match > 1;
      }
      ci.learned = true;
    }
  }
  // The call-count guard (SKIP_GAME_WORK_MAP.md 16.3 R4): a ring object whose number of keyed calls differs from the previous frame's (a rope LOD flip, a skirt quadrant change, an object appearing or leaving) is unmarked for 2 x K frames:
  // its ordinal-keyed sequences shift, and which instance a learned ordinal stands for is [U] across a flip. Markers count as calls (they stand for runs the game did not record) except in the verify arm (extra draws).
  std::unordered_map<uint32_t, uint32_t> guard_last_;
  bool guard_started_ = false;   // N3: the first frame has nothing to compare with (an empty last map is a real state: a frame with no ring object)
  std::unordered_map<uint32_t, uint64_t> guard_until_;
  uint64_t guard_trips = 0;
  void GuardCalls(const LoadedFrame& frame, const std::vector<Run>& runs) {
    std::unordered_map<uint32_t, uint32_t> now;
    const bool cg = caster();   // lane 299 (review SF2): the caster route guards the PLANE pass's objects (keyed by the composite (model, pass)): the ring proxy is a 12-call object whose EyePos c72 is held per ordinal
    for (const Run& r : runs)
      if (r.Keyed() && (!cg || r.pass == rdr_scene::kCpPlane)) ++now[cg ? KeyObject(r) : r.object];
    if (!verify)
      for (uint32_t op = 0; op < frame.ops.count; ++op)
        if (frame.ops[op].kind == OpKind::Draw) {
          const DrawOrigin* o = frame.OriginOf(frame.ops[op].index);
          if (!o) continue;
          if (cg) {
            if (IsCasterMarkerCall(o->call_site) && CasterPassOfMarker(o->call_site) == rdr_scene::kCpPlane) ++now[CasterObjectId(o->object, rdr_scene::kCpPlane)];
          } else if (IsRingMarkerCall(o->call_site) || IsMarkerCall(o->call_site)) ++now[o->object];
        }
    const uint64_t fno = frame.header.guest_frame;
    if (guard_started_) {   // (the first frame has nothing to compare with)
      for (const auto& kv : now) {
        const auto it = guard_last_.find(kv.first);
        if (it == guard_last_.end() || it->second != kv.second) guard_until_[kv.first] = fno + 2ull * kProofK, ++guard_trips;
      }
      for (const auto& kv : guard_last_)
        if (!now.count(kv.first)) guard_until_[kv.first] = fno + 2ull * kProofK, ++guard_trips;
    }
    guard_last_ = std::move(now);
    guard_started_ = true;
  }
  bool Guarded(uint32_t object, uint64_t fno) const {
    if (!(ring() || caster()) || fault == Fault::RingLodFlipOrdinal || (caster() && fault == Fault::CasterPlaneNoGuard)) return false;
    const auto it = guard_until_.find(object);
    return it != guard_until_.end() && it->second > fno;
  }

  // ---- observe a bound frame: learn the sequences of the runs recorded in full, prove them in their proof frame, publish the mark set ---------------------------------------------------------------
  // keys / fulls: the binder's thin keys for this frame's draws (thin_keys_ / thin_full_).
  void Observe(const LoadedFrame& frame, const std::vector<Run>& runs, const std::vector<uint64_t>& keys, const std::vector<thin::ThinKey>& fulls, const Hooks& h) {
    const uint64_t fno = frame.header.guest_frame;
    if (arena())
      for (const Run& r : runs)
        if (r.call >= 1 && r.Keyed()) instanced.insert(r.object);
    Bank pass_vs, pass_ps;
    std::vector<uint8_t> hv, hp;
    std::vector<uint64_t> caster_keys;   // lane 211: the caster draws' own keys (no thin store)
    const thin::ThinKey no_thin_key{};
    if (caster()) {
      pass_vs.fill(0), pass_ps.fill(0), hv.assign(256, 0), hp.assign(256, 0);   // a caster pass has no pass bank: every camera register is a record register
      CasterKeys(frame, caster_keys);
    } else {
      PassBank(frame, pass_vs, pass_ps, hv, hp, wrestlers());
    }
    const std::vector<uint64_t>& K = caster() ? caster_keys : keys;
    if (ring() || caster()) GuardCalls(frame, runs);   // lane 229; lane 299: + the caster route's plane objects
    for (const Run& r : runs) {
      if (!r.Keyed()) continue;
      const uint64_t mk = MarkKey(KeyObject(r), r.ordinal);
      std::vector<PartDesc> fresh(r.draws.size());
      bool ok = true;
      size_t failed_k = 0;
      auto old = seqs.find(mk);
      for (size_t k = 0; k < r.draws.size() && ok; ++k) {
        const uint32_t i = r.draws[k];
        cap_why = CapWhy::Other;   // (review S3: a draw with no key is "other", never the previous draw's reason)
        const PartDesc* prev = (old != seqs.end() && k < old->second.parts.size() && old->second.parts[k].key == K[i]) ? &old->second.parts[k] : nullptr;
        ok = i < K.size() && K[i] && Capture(frame, i, K[i], caster() ? no_thin_key : fulls[i], h, fresh[k], prev);
        if (!ok) failed_k = k;
        else if (ring() && fresh[k].cpi.npal == 0) ++c.palette_unknown_parts;   // lane 258: a ring part whose palette size no full frame gave yet (named in the census)
      }
      if (wrestlers()) {   // lane 227 (H4): the wrestler chain's runs: pinned wrestler runs, and why the others stay recorded in full (never marked, counted by reason)
        ++wr_runs;
        if (!ok) {
          CapWhy w = cap_why;
          if (w == CapWhy::NotPinned) {   // a draw the hook KEPT (never a lite draw: lite means pinned): a wrestler program (yCh_*) = cloth, a piece, an unknown model, a part not learned yet; any other program is another class's object (arena, props, ring)
            const Draw& fd = frame.draws[r.draws[failed_k]];
            const int vv = h.variant ? h.variant(fd.vs_hash) : -1;
            if (vv < 0 || std::strncmp(rdr::fx::kVariants[vv].name, "yCh_", 4) != 0) w = CapWhy::OtherClass;
          }
          ++wr_unlearnable[uint8_t(w)];
        }
      }
      if (caster()) {   // lane 217 (E2b): the census of the caster runs by pass: how many were seen, how many could not be learned and the first failing part's shader hashes (hashes only)
        ++caster_runs[r.pass < 4 ? r.pass : 0];
        if (!ok) {
          ++caster_unlearnable[r.pass < 4 ? r.pass : 0];
          const Draw& fd = frame.draws[r.draws[failed_k]];
          char key[96];
          std::snprintf(key, sizeof key, "pass %u vs %016llX ps %016llX variant vs %d", unsigned(r.pass), (unsigned long long)fd.vs_hash, (unsigned long long)fd.ps_hash, h.variant ? h.variant(fd.vs_hash) : -1);
          ++caster_unlearnable_why[key];
        }
      }
      if (!ok) {   // a part not pinned (yet) / of another class / without a variant: the object cannot be marked from this frame
        if (old != seqs.end()) old->second.complete = false, old->second.why = uint32_t(UnmarkedWhy::NotLearnable);
        continue;
      }
      if (old == seqs.end()) {
        Sequence s;
        s.object = KeyObject(r), s.ordinal = r.ordinal;
        old = seqs.emplace(mk, std::move(s)).first;
      }
      Sequence& s = old->second;
      const bool was_complete = s.complete;   // lane 301: a COMPLETE sequence is a certified version; what this frame learns must not replace it for the markers already in flight
      bool drift_learned = false;
      if (fno > s.last_seen + 2) s.streak_start = fno;
      s.last_seen = fno;
      bool same = s.parts.size() == fresh.size();
      for (size_t k = 0; same && k < fresh.size(); ++k) same = s.parts[k].key == fresh[k].key;
      Bank mk_vs{}, mk_ps{};   // the bank the marker would have had: the draw before the run's first op
      {
        uint32_t prev = r.draws[0];
        for (uint32_t o = r.first_op; o-- > 0;)
          if (frame.ops[o].kind == OpKind::Draw) {
            prev = frame.ops[o].index;
            break;
          }
        std::memcpy(mk_vs.data(), frame.constant_banks[frame.draws[prev].vs_bank].dwords, sizeof(Bank));
        std::memcpy(mk_ps.data(), frame.constant_banks[frame.draws[prev].ps_bank].dwords, sizeof(Bank));
      }
      if (same && !s.parts.empty() && fault == Fault::None) {   // drift detection (off under a planted fault: it would heal the plant): the OLD sequence composed with the record groups taken from the stream, against the stream: what still differs is a register the pass / template source cannot give
        for (size_t k = 0; k < fresh.size(); ++k) {
          const Draw& dr = frame.draws[r.draws[k]];
          PartFields pf;
          pf.vs.assign(frame.constant_banks[dr.vs_bank].dwords, frame.constant_banks[dr.vs_bank].dwords + 1024);
          pf.ps.assign(frame.constant_banks[dr.ps_bank].dwords, frame.constant_banks[dr.ps_bank].dwords + 1024);
          Bank tv, tp, cv, cp;
          std::memcpy(tv.data(), pf.vs.data(), sizeof(Bank));
          std::memcpy(tp.data(), pf.ps.data(), sizeof(Bank));
          ComposeCtx cc;
          cc.mode = Mode::Stream, cc.fault = fault, cc.pass_vs = &pass_vs, cc.pass_ps = &pass_ps, cc.vs_has = &hv, cc.ps_has = &hp, cc.marker_vs = &mk_vs, cc.marker_ps = &mk_ps, cc.pf = &pf;
          CompareStats dt;
          std::vector<uint32_t> diffs;
          dt.diffs = &diffs;
          ComposePart(s.parts[k], cc, cv, cp, &dt, &tv, &tp);
          for (uint32_t x : diffs) {
            if (!(x >> 16 ? s.parts[k].drift_ps : s.parts[k].drift_vs).test(x & 0xFF)) drift_learned = true;   // lane 301: a bit the certified parts did not carry
            (x >> 16 ? fresh[k].drift_ps : fresh[k].drift_vs).set(x & 0xFF);
            auto& lg = ledger[fresh[k].key];
            (x >> 16 ? lg.second : lg.first).set(x & 0xFF);
          }
        }
      }
      RecordSrc rs;   // lane 152 (Mode::Record): the call's records, looked up once for the proof compare and the verify compare
      const bool proof_now = ObjectProofFrame(KeyObject(r), fno) && !s.parts.empty();
      const bool verify_now = verify && !s.parts.empty() && verify_keys.count(mk);
      const bool learn_slots = (caster() || ring() || wrestlers()) && mode == Mode::Record && !fresh.empty();
      if ((proof_now || verify_now || learn_slots) && mode == Mode::Record) {
        if (!RecordFor(HookFrame(fno), r.object, r.call, r.ordinal, rs, r.pass)) ++c.record_missing;
      }
      if (learn_slots && (caster() || wrestlers()) && rs.have_pal) LearnCasterSlots(frame, r, rs, proof_now, fresh);
      if (learn_slots && ring()) LearnRingSlots(frame, r, rs, proof_now, fresh);   // lane 229
      if (proof_now) {   // the proof: the OLD sequence composed as a marker would, against this frame's own draws (a caster part uses its OLD slot map: the map learned in an earlier frame is what a marker would use; the new one is learned into `fresh`)
        ++c.proof_objects;
        s.proof_frame = fno;
        CompareStats st;
        bool state_ok = same;
        if (same) {
          for (size_t k = 0; k < fresh.size(); ++k) {
            if (!CompareRunPart(frame, r, k, s.parts[k], fresh[k], pass_vs, pass_ps, hv, hp, mk_vs, mk_ps, rs, st)) state_ok = false;
            c.proof_parts += 1;
          }
        }
        c.proof_mismatch_parts += st.mismatching;
        if (marked_prev.count(mk)) c.proof_mismatch_marked += st.mismatching;   // (11) only a marked object's mismatch resets the gate
        for (int g = 0; g < int(Group::Count); ++g) c.by_group[g] += st.by_group[g];
        for (const auto& kv : st.by_reg) proof_by_reg[kv.first] += kv.second;
        sphere_reads += st.sphere_reads, sphere_norow += st.sphere_norow;   // lane 245
        for (int k = 0; k < 3; ++k) sphere_eq[k] += st.sphere_eq[k];
        s.unsourced = st.unsourced;
        s.proof_mismatch = st.mismatching;
        s.mismatch_regs = st.by_reg;
        s.complete = same && st.mismatching == 0 && state_ok && (st.unsourced.empty() || fault == Fault::MarkUnsourced || fault == Fault::DrawUnsourced);
        if (s.complete) s.proven_drift = s.DriftBits();   // review S4: the certification is the ONLY place it is set (from the OLD parts the proof just composed): a later learn that adds drift makes Publish's refresh fire
        s.why = uint32_t(!same ? UnmarkedWhy::PartsDiffer : !state_ok ? UnmarkedWhy::StateDiffers : st.mismatching ? UnmarkedWhy::Mismatch : !st.unsourced.empty() ? UnmarkedWhy::Unsourced : UnmarkedWhy::Marked);
      } else if (!same) {
        s.complete = false;
        s.why = uint32_t(UnmarkedWhy::PartsDiffer);
      }
      if (verify_now) {   // (10) the verify arm: a marker was written and the game's own draws came too; the sequence composed as the marker would be is compared with the stream's draws, every frame
        ++c.verify_markers;
        if (!same) {
          ++c.verify_len_differs;
        } else {
          CompareStats vt;
          for (size_t k = 0; k < fresh.size(); ++k) CompareRunPart(frame, r, k, s.parts[k], fresh[k], pass_vs, pass_ps, hv, hp, mk_vs, mk_ps, rs, vt);
          c.verify_parts += vt.parts, c.verify_mismatch_parts += vt.mismatching, c.verify_unsourced_parts += vt.unsourced_parts;
          for (int g = 0; g < int(Group::Count); ++g) verify_by_group[g] += vt.by_group[g];
        }
      }
      if (!(fault == Fault::StaleTemplate && !s.parts.empty()) && !(fault == Fault::SkipInProofFrame && false)) {
        // lane 301 (11.10 fix 1): the sequence was COMPLETE (marked, or marked a moment ago) and this full frame changes it (drift learned, the part list changed, or the proof just failed): the certified parts are FROZEN for the
        // markers already in flight (generations <= the newest published set), the sequence leaves the mark set now (complete = false, counted `drift after mark`), and `fresh` becomes the live learning copy. Before this, `s.parts = fresh`
        // put the new drift bit into the parts the in-flight markers composed from -> an unsourced register -> a hole (nothing drawn while the game had skipped the call) and, via the holes, the gate's latch.
        // review S1: a PURE proof failure (same list, no new drift) behaves as before - fresh (with the re-learned slot map) serves the markers in flight; only drift, a part-list change or a WORSE palette fact freezes.
        // S2: a PLANE pass's part-list change is not frozen (lane 299's PlaneDraw replay uses the new list). S3: a slot / palette fact that was resolved in the certified parts and is lost in `fresh` (two bones with equal rows,
        // npal changed) is treated like drift: the certified map is frozen, the object leaves the mark set.
        const bool plane_pass = caster() && !s.parts.empty() && s.parts[0].pass == rdr_scene::kCpPlane;
        const bool cpi_worse = same && (caster() || ring() || wrestlers()) && PaletteFactsWorse(s.parts, fresh);
        const bool list_changed = !same && !plane_pass;
        if (freeze_on && was_complete && !s.parts.empty() && (drift_learned || list_changed || cpi_worse)) {
          Frozen f;
          f.seq = s;   // the OLD parts (still s.parts: the replacement is below)
          f.frame = fno, f.max_gen = marks->generation();
          f.seq.complete = true;
          frozen[mk] = std::move(f);
          s.complete = false;
          if (!proof_now || s.why == uint32_t(UnmarkedWhy::Marked)) s.why = uint32_t(drift_learned ? UnmarkedWhy::DriftAfterMark : UnmarkedWhy::PartsDiffer);
          if ((drift_learned || cpi_worse) && marked_prev.count(mk)) ++c.drift_after_mark;   // N1: only an object that was in the mark set (drift bits and lost palette facts)
          if (DebugOn()) std::fprintf(stderr, "[d3dbg] freeze frame %llu object %08X ordinal %u: %s (max gen %u)\n", (unsigned long long)fno, s.object, s.ordinal, drift_learned ? "drift learned after the mark" : cpi_worse ? "palette facts lost" : "part list changed", frozen[mk].max_gen);
        }
        s.parts = std::move(fresh);
        s.learned_frame = fno;
        ++c.learned;
      }
    }
  }

  // review S3: a palette fact (slot -> node, collapse flag) resolved in the certified part and lost in the re-learned one, or a changed palette size (npal 0 = unknown, not counted)
  static bool PaletteFactsWorse(const std::vector<PartDesc>& old, const std::vector<PartDesc>& fresh) {
    for (size_t k = 0; k < old.size() && k < fresh.size(); ++k) {
      const CasterPartInfo& a = old[k].cpi;
      const CasterPartInfo& b = fresh[k].cpi;
      if (a.npal != 0 && a.npal != b.npal) return true;
      for (uint32_t slot = 0; slot < kCasterMaxSlots; ++slot)
        if ((a.slot_node[slot] != kNoNode || a.slot_collapsed[slot]) && b.slot_node[slot] == kNoNode && !b.slot_collapsed[slot]) return true;
    }
    return false;
  }

  static bool StateEqual(const PartDesc& a, const PartDesc& b) {
    if (std::memcmp(&a.fixed, &b.fixed, sizeof(DrawFixed)) != 0) return false;
    if (std::memcmp(&a.dyn, &b.dyn, sizeof(DrawDynamic)) != 0) return false;
    if (std::memcmp(&a.bools, &b.bools, sizeof(BoolLoop)) != 0) return false;
    if (a.slots.size() != b.slots.size()) return false;
    for (size_t i = 0; i < a.slots.size(); ++i) {
      if (std::memcmp(&a.slots[i].image, &b.slots[i].image, sizeof(uint32_t)) != 0 || a.slots[i].slot != b.slots[i].slot || a.slots[i].view_format != b.slots[i].view_format ||
          std::memcmp(a.slots[i].swizzle, b.slots[i].swizzle, 4) != 0 || a.slots[i].min_level != b.slots[i].min_level || a.slots[i].max_level != b.slots[i].max_level)
        return false;
      if (std::memcmp(&a.samplers[i], &b.samplers[i], sizeof(rdr::SamplerDesc)) != 0) return false;
    }
    return true;
  }

  // Publish the mark set: complete sequences (every part's key still pinned). `lag` handled by the reader emulation.
  // lane 152: the objects NOT marked are counted by named reason (`reasons`), the sequences that never got a proof compare are counted (item 12), and a marked sequence that carries an unsourced register is counted
  // (required 0: only the plant mark_unsourced makes one).
  void Publish(const Hooks& h, uint64_t fno = 0) {
    std::vector<uint64_t> keys;
    std::vector<std::pair<uint64_t, std::vector<uint64_t>>> entries;   // lane 289 (ring route): each marked key with the part triples of its sequence (published with the set: the READER's and the BINDER's ordinal fill at a marker)
    uint64_t seqs_alive = 0;
    reasons.clear();
    proof_never = 0;
    c.marked_unsourced = 0;
    c.ring_refused = 0;
    for (auto it = seqs.begin(); it != seqs.end();) {
      Sequence& s = it->second;
      bool all_pinned = !s.parts.empty();
      for (const PartDesc& d : s.parts) {
        if (d.caster) all_pinned = all_pinned && (!h.geometry || h.geometry(d.origin.object, d.origin.part, d.pass));   // lane 211: our geometry for the part is ready (no thin store)
        else all_pinned = all_pinned && h.pinned && h.pinned(d.key);
      }
      if (!all_pinned) {   // a key left the pin set (a failed check / the arena's assets went): the object leaves the mark set and the sequence goes
        drop_gen = marks->generation() + 1;
        retired[it->first] = {std::move(it->second), fno};
        it = seqs.erase(it);
        continue;
      }
      ++seqs_alive;
      const bool is_unstable = unstable.count(it->first) != 0;
      // (S7) a mark expires when the last proof compare is older than 2 x K frames while the object is being drawn (it reappeared, or the proof clocks drifted apart). `proof_overdue` COUNTS only an object drawn without a gap for
      // that long: its proof frame never came although it was there = the hook-to-reader offset is skewed (an object that was off screen and came back is unmarked too, but is not the detector's case).
      const bool stale = !force_mark && s.complete && fno && s.proof_frame && fno > s.proof_frame + 2 * kProofK && fno <= s.last_seen + 2;
      const bool overdue = stale;
      // lane 268 (17.13): an object that is not marked AND has not been drawn for 64+ frames is a STALE sequence (menus / entrance / an earlier scene; seqs_alive keeps it until a key leaves the pin set): its reason says so, so "marked 16 of 44" can be read against the objects that are actually drawn
      const std::string gone = (fno && fno > s.last_seen + 64) ? " [not drawn for 64+ frames]" : "";
      if (stale && fno >= s.streak_start + 2 * kProofK) ++c.proof_overdue;
      if (overdue) {
        if (DebugOn()) std::fprintf(stderr, "[d3dbg] overdue frame %llu object %08X ordinal %u proof_frame %llu last_seen %llu streak %llu\n", (unsigned long long)fno, s.object, s.ordinal, (unsigned long long)s.proof_frame, (unsigned long long)s.last_seen, (unsigned long long)s.streak_start);
        ++reasons[UnmarkedName(UnmarkedWhy::ProofOverdue)];
      } else if (caster() && !s.parts.empty() && s.parts[0].pass == rdr_scene::kCpPlane && (!plane_marks_allowed || fault == Fault::LiveGuard) && fault != Fault::LiveGuardOff) {   // lane 226 follow-up (S1): the live guard
        ++reasons["plane: not marked live (its sources are made inside the call; the entry snapshot is not proven live; --wwe2k14_rdr_caster_d3_plane lifts this)" + gone];
      } else if (arena() && (s.complete || force_mark) && !is_unstable && (records.IsRingObject(s.object) || IsLiveRingModel(s.object) || instanced.count(s.object))) {   // lane 274 (S2): a RING model is the ring chain's: the arena chain never marks it (instances, kMaxCalls, per-call rows the arena records do not carry), whatever the ring hook's state
        ++c.ring_refused;
        ++reasons["ring model (instanced; the ring chain's, never marked by the arena chain)" + gone];
      } else if (Guarded(s.object, fno) && (s.complete || force_mark) && !is_unstable) {   // lane 229: the call-count guard (R4): the object's number of calls changed within 2 x K frames
        ++reasons["call count changed (rope LOD / skirt quadrant / object appeared or left)" + gone];
      } else if ((s.complete || force_mark) && !is_unstable) {
        keys.push_back(it->first);
        if (ring()) {   // lane 289: the sequence's candidate draws, as ComputeKeys counts them (a ModelPart draw with an object at one of the two colour sites); a doubled part is listed twice
          std::vector<uint64_t> triples;
          triples.reserve(s.parts.size());
          for (const PartDesc& pd : s.parts)
            if (pd.origin.klass == DrawClass::ModelPart && pd.origin.object && (pd.origin.pass_site == thin::kSiteColour || pd.origin.pass_site == thin::kSiteMirror)) triples.push_back(thin::TripleHash(pd.origin.pass_site, pd.origin.object, pd.origin.part));
          entries.push_back({it->first, std::move(triples)});
        }
        if (!s.unsourced.empty() || (s.complete && s.DriftBits() != s.proven_drift)) ++c.marked_unsourced;   // lane 301: from the CURRENT parts every frame (drift learned since the certification counts), not only the proof's map
        if (caster() && !s.parts.empty()) {   // lane 221: the census names WHICH caster passes are marked (map ordinal 0 = map A when the model has a collapse flag, 1 = map B; props have one map)
          static const char* const pn[] = {"?", "map", "plane", "rim"};
          ++reasons["marked: " + std::string(pn[s.parts[0].pass < 4 ? s.parts[0].pass : 0]) + " ordinal " + std::to_string(s.ordinal) + (s.parts[0].cpi.npal ? "" : " (no palette)")];
        } else ++reasons["marked"];
      } else {
        if (s.proof_frame == 0) ++proof_never;
        if (is_unstable) ++reasons[std::string(UnmarkedName(UnmarkedWhy::Unstable)) + gone];
        else if (s.why == uint32_t(UnmarkedWhy::Unsourced) && !s.unsourced.empty()) {   // name the registers (the first three by count)
          std::vector<std::pair<uint64_t, uint32_t>> v;
          for (const auto& kv : s.unsourced) v.push_back({kv.second, kv.first});
          std::sort(v.rbegin(), v.rend());
          std::string name = "unsourced: ";
          for (size_t i = 0; i < v.size() && i < 3; ++i) name += (i ? ", " : "") + UnsourcedName(v[i].second);
          if (v.size() > 3) name += ", +" + std::to_string(v.size() - 3) + " more";
          ++reasons[name + gone];
        } else if (s.why == uint32_t(UnmarkedWhy::Mismatch)) {
          CompareStats top;
          top.by_reg = s.mismatch_regs;
          ++reasons[std::string(UnmarkedName(UnmarkedWhy::Mismatch)) + ": " + TopRegisters(top, 4) + " (the stream's value is no recorded state's)" + gone];
        } else {
          ++reasons[std::string(UnmarkedName(UnmarkedWhy(s.why < uint32_t(UnmarkedWhy::Count) ? s.why : 0))) + gone];
        }
      }
      ++it;
    }
    for (auto rt = retired.begin(); rt != retired.end();) rt = (fno > rt->second.second + kRetireFrames) ? retired.erase(rt) : std::next(rt);
    for (auto fz = frozen.begin(); fz != frozen.end();) fz = (fno > fz->second.frame + kFreezeFrames) ? frozen.erase(fz) : std::next(fz);   // lane 301
    c.objects_complete = keys.size();
    if (c.marked_unsourced > c.marked_unsourced_peak) c.marked_unsourced_peak = c.marked_unsourced;
    marked_prev.clear();
    marked_prev.insert(keys.begin(), keys.end());   // (11): the keys whose proof mismatches reset the gate are the ones the hook may skip
    if (mode == Mode::Record && !record_route) keys.clear(), entries.clear();   // review A: the record route is not switched on, so live NOTHING is published (the hook finds no markable object): inert by construction
    skippable = keys.size(), pinned_objects = seqs_alive;
    if (ring()) marks->PublishParts(std::move(entries));   // lane 289: the ring's set carries the part triples
    else marks->Publish(std::move(keys));
  }
  std::map<uint32_t, uint64_t> proof_by_reg;   // lane 152: register mismatches of the proof compares (stage << 16 | reg -> parts), for the report
  // lane 245 (H3): the sphere rows' hook-frame offset, measured: VS c168-c171 registers of the proof compares against the recorder's row at each candidate stamp (d3_record.h NoteSphereOffsets). The bar of the live verify run:
  // one candidate equal on (nearly) every register with a row; RecordIndex::sphere_pick is the one the marks are composed from.
  uint64_t sphere_reads = 0, sphere_norow = 0, sphere_eq[3] = {};
  std::string SphereOffsetLine() const {
    std::string s;
    for (int k = 0; k < 3; ++k) s += std::string(k ? "; " : "") + "hook frame " + (kSphereDelta[k] > 0 ? "+" : "") + std::to_string(kSphereDelta[k]) + ": equal " + std::to_string(sphere_eq[k]) + " of " + std::to_string(sphere_reads);
    return s + "; no row at any of the three " + std::to_string(sphere_norow) + "; pick " + std::to_string(kSphereDelta[records.sphere_pick >= 0 && records.sphere_pick < 3 ? records.sphere_pick : 0]);
  }
  uint64_t proof_never = 0;   // (12) sequences alive that never got a proof compare (the reader's proof schedule never reached them, or the hook's skipped them)

  // ---- the emulated hook (fview): decide each object call of the frame as the game's hook would (the SHIPPED DecideCall), and turn the skipped runs into markers -------------------------------------------
  // `frame` is rewritten in place (ops / draws / origins are binder-owned copies). The skipped run's first draw becomes the marker (a 0-count draw whose origin is {kMarkerCall | ordinal, object, generation},
  // its state and banks the PREVIOUS draw's: the device's shadow at that point), its other draws leave the op list, and the packet carries the stream's own banks as the emulated record fields.
  struct Emu {
    std::vector<Op> ops;
    std::vector<Draw> draws;
    std::vector<DrawOrigin> origins;
    std::vector<ThinDrawFacts> facts;   // lane 152: the thin facts table stays parallel to the draws (the verify arm appends marker draws)
  } emu;
  bool step_parsed = false;
  uint64_t step_base = ~0ull;
  void StepTemplates(LoadedFrame& frame) {   // lane 301 (step 5): see `step_from`
    if (!step_parsed) {
      step_parsed = true;
      if (const char* e = std::getenv("D3_TEMPLATE_STEP")) {
        unsigned long long n = 0;
        unsigned cnt = 3;
        if (std::sscanf(e, "%llu:%u", &n, &cnt) >= 1) step_from = n, step_count = cnt ? cnt : 3;
      }
    }
    const uint64_t fno = frame.header.guest_frame;
    if (step_from == ~0ull) return;   // the arm is off (no D3_TEMPLATE_STEP)
    if (step_base == ~0ull) step_base = fno;   // N counts from the first frame of the run
    if (fno < step_base + step_from) return;
    if (step_objects.empty()) {   // the first `step_count` objects (lowest keys) the last Publish marked: "marked by then"
      std::vector<uint64_t> ks(marked_prev.begin(), marked_prev.end());
      std::sort(ks.begin(), ks.end());
      for (uint64_t k : ks) {
        const auto it = seqs.find(k);
        if (it == seqs.end() || std::find(step_objects.begin(), step_objects.end(), it->second.object) != step_objects.end()) continue;
        step_objects.push_back(it->second.object);
        if (step_objects.size() >= step_count) break;
      }
      if (step_objects.empty()) return;
      std::fprintf(stderr, "[d3] template_step: from frame %llu VS c185 of %zu marked objects steps every frame:", (unsigned long long)fno, step_objects.size());
      for (uint32_t o : step_objects) std::fprintf(stderr, " %08X", o);
      std::fprintf(stderr, "\n");
    }
    std::vector<ConstantBank> added;
    std::unordered_map<uint32_t, uint32_t> remap;   // original vs bank -> the stepped copy (draws of the frame that share a bank share the copy)
    const uint32_t base = frame.constant_banks.count;
    const float delta = 0.01f * float(fno - (step_base + step_from) + 1);
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const DrawOrigin* o = frame.OriginOf(i);
      if (!o || std::find(step_objects.begin(), step_objects.end(), o->object) == step_objects.end()) continue;
      Draw& d = const_cast<Draw&>(frame.draws[i]);
      if (d.vs_bank >= base) continue;
      auto rm = remap.find(d.vs_bank);
      if (rm == remap.end()) {
        ConstantBank nb = frame.constant_banks[d.vs_bank];
        float x;
        std::memcpy(&x, &nb.dwords[185 * 4], 4);
        x += delta;
        std::memcpy(&nb.dwords[185 * 4], &x, 4);
        rm = remap.emplace(d.vs_bank, base + uint32_t(added.size())).first;
        added.push_back(nb);
      }
      d.vs_bank = rm->second;
      ++step_edits;
    }
    if (added.empty()) return;
    step_banks.assign(frame.constant_banks.data, frame.constant_banks.data + base);
    step_banks.insert(step_banks.end(), added.begin(), added.end());
    frame.constant_banks = {step_banks.data(), uint32_t(step_banks.size())};
  }
  uint32_t EmulateHook(LoadedFrame& frame) {
    StepTemplates(frame);
    const uint64_t fno = frame.header.guest_frame;
    const uint64_t hfno = HookFrame(fno);   // lane 152: the frame the real hook would have stamped (Mode::Record: stream frame - the measured offset); Stream / Sourced: the stream's own
    hist.push_back(marks->Acquire());
    while (hist.size() > size_t(lag) + 1) hist.erase(hist.begin());
    std::shared_ptr<const MarkSet> use = hist.size() == size_t(lag) + 1 ? hist.front() : nullptr;
    std::vector<Run> runs;
    ScanFor(frame, runs, nullptr, !verify);
    if (DebugOn() && std::getenv("D3_DEBUG_RUN")) for (const Run& r : runs) { std::fprintf(stderr, "[d3dbg] run frame %llu object %08X call %u ordinal %u keyed %u other %u first op %u parts", (unsigned long long)fno, r.object, r.call, r.ordinal, unsigned(r.keyed), unsigned(r.other), r.first_op); for (uint32_t di : r.draws) { const DrawOrigin* oo = frame.OriginOf(di); std::fprintf(stderr, " %u@%X", oo ? oo->part : 99u, oo ? oo->pass_site : 0u); } { const Draw& d0 = frame.draws[r.draws[0]]; float w[3]; std::memcpy(w, &frame.constant_banks[d0.vs_bank].dwords[76 * 4 + 3], 4); std::memcpy(w + 1, &frame.constant_banks[d0.vs_bank].dwords[77 * 4 + 3], 4); std::memcpy(w + 2, &frame.constant_banks[d0.vs_bank].dwords[78 * 4 + 3], 4); std::fprintf(stderr, " pos %g %g %g slot0 img %u", w[0], w[1], w[2], d0.texture_count ? frame.texture_slots[d0.texture_first].image : 0u); } std::fprintf(stderr, "\n"); }
    verify_keys.clear();
    std::vector<std::pair<uint32_t, uint32_t>> inserts;   // (verify arm) (op position, new draw index): a marker draw put AHEAD of the run, nothing dropped
    reserved.clear();
    emu.ops.assign(frame.ops.data, frame.ops.data + frame.ops.count);
    emu.draws.assign(frame.draws.data, frame.draws.data + frame.draws.count);
    emu.origins.assign(frame.origins.data, frame.origins.data + frame.origins.count);
    std::vector<uint8_t> drop(emu.ops.size(), 0);
    uint32_t markers = 0;
    const int64_t now_ms = int64_t(fno) * 16;
    packets->Evict(fno);
    if (hook_from_record && ring() && hook_tally) {   // lane 258: the real hook's call count of this hook frame = the ring-rows entries stamped with it (0 entries: nothing noted, the frame is not checked)
      hook_ring_objects.clear();
      const uint32_t n = records.RingCallsOfFrame(hfno, &hook_ring_objects);
      for (uint32_t i = 0; i < n; ++i) hook_tally->NoteKeyedCall(hfno);
    }
    GuestTally* const note_tally = hook_from_record && ring() ? nullptr : &tally;   // (EndGuestCall's per-call note is the emulation's own count: off then)
    for (const Run& r : runs) {
      CallInputs in;
      in.object = KeyObject(r), in.frame = hfno, in.now_ms = now_ms, in.owner = true, in.state_readable = true, in.state_count = 1, in.caster = caster(), in.ring = ring(), in.wrestler = wrestlers();
      if (caster()) in.max_calls = fault == Fault::CasterPlaneCap4 ? kMaxCalls : CasterCallCap(r.pass);   // lane 290: the plane pass's ledger holds 16 calls an object (the plant caster_plane_cap4 is the old 4)
      in.proof_skew = uint32_t(fno - hfno);   // (12) the reader's proof schedule runs on the stream's frame number
      const GuestCall gc = BeginGuestCall(guest, in, use.get(), *packets, tally, *gate, fault);
      const CallOutcome& out = gc.out;
      EndGuestCall(guest, gc, CallInfo{uint16_t(r.keyed), uint16_t(r.other)}, fault, note_tally);
      if (out.why != Why::Skip) continue;
      static const char* const only_env = std::getenv("D3_ONLY");
      static const char* const mod_env = std::getenv("D3_MOD");
      if (const char* only = only_env) {   // diagnosis: D3_ONLY=<hex object> skips only that object; D3_MOD=n/r skips objects with (object >> 4) % n == r
        if (std::strtoul(only, nullptr, 16) != r.object) {
          packets->Take(hfno, KeyObject(r), r.ordinal);
          continue;
        }
      }
      if (const char* md = mod_env) {
        unsigned n = 0, rem = 0;
        std::sscanf(md, "%u/%u", &n, &rem);
        if (n && ((r.object >> 4) % n) != rem) {
          packets->Take(hfno, KeyObject(r), r.ordinal);
          continue;
        }
      }
      ++markers;
      if (DebugOn()) std::fprintf(stderr, "[d3dbg] frame %llu marks object %08X ordinal %u parts %zu first op %u\n", (unsigned long long)fno, r.object, r.ordinal, r.draws.size(), r.first_op);
      if (DebugOn() && std::getenv("D3_DEBUG_RUN")) { for (size_t k = 0; k < r.draws.size(); ++k) { const DrawOrigin* oo = frame.OriginOf(r.draws[k]); std::fprintf(stderr, "[d3dbg]   run part %u site %08X ops %u draw %u\n", oo ? oo->part : 99u, oo ? oo->pass_site : 0u, r.ops[k], r.draws[k]); } }
      if (ObjectProofFrame(KeyObject(r), fno)) ++skipped_in_proof;   // only the plant skip_in_proof_frame gets here
      // the emulated record fields: the stream's banks of this run's draws, in order
      if (out.packet_written) {
        (void)packets->Take(hfno, KeyObject(r), r.ordinal);   // the guest's placeholder; the full packet replaces it
        Packet p;
        p.frame = hfno, p.object = KeyObject(r), p.ordinal = r.ordinal, p.call = out.call;
        for (uint32_t di : r.draws) {
          PartFields pf;
          const Draw& dr = frame.draws[di];
          pf.vs.assign(frame.constant_banks[dr.vs_bank].dwords, frame.constant_banks[dr.vs_bank].dwords + 1024);
          pf.ps.assign(frame.constant_banks[dr.ps_bank].dwords, frame.constant_banks[dr.ps_bank].dwords + 1024);
          p.parts.push_back(std::move(pf));
        }
        packets->Put(std::move(p));
      }
      // the marker: the first draw, with the state of the draw before it
      const uint32_t fd = r.draws[0];
      uint32_t prev = fd;
      for (uint32_t o = r.first_op; o-- > 0;)
        if (frame.ops[o].kind == OpKind::Draw) {
          prev = frame.ops[o].index;
          break;
        }
      Draw m = frame.draws[prev];
      m.flags = kDrawNoGeometry;
      m.vs_hash = m.ps_hash = m.vs_patched_hash = 0;
      m.texture_count = 0;
      m.vertex_offset = m.index_offset = 0;
      m.vertex_bytes = m.index_bytes = m.vertex_count = m.index_count = 0;
      m.layout = 0xFFFFFFFFu;
      m.vertex_offset = hfno;                                // the marker record's +20 word (base vertex): the hook's frame; the reader hands it to the binder here (contract 10.7)
      m.index_offset = (use ? use->generation : 0) + (fault == Fault::RingFillGenerationOff ? 1000u : 0u);            // the +24 word: the mark generation the hook read (plant ring_fill_generation_off: a generation no set has)
      // lane 204 (Check K1): the emulated hook goes through the SAME claim decision as the live reader (reader.h TryD3Marker -> IsMarkerReplay), with the arguments the replay delivers for the hook's record
      // (prim 6, recorded count 0 through the op-15 arithmetic, no inline vertices). An unclaimed marker is what live did: the draw goes through DrawCommon and its frame field is a geometry-store offset (a hole).
      if (!IsMarkerReplay(kMarkerPrim, ReplayIndexCount(kMarkerPrimMul, 0, kMarkerPrimAdd), false, fault)) {
        ++c.marker_unclaimed;
        m.vertex_offset = 0xD3D3D3D3ull;
      }
      DrawOrigin mo;
      const uint32_t mord = (r.ordinal + (fault == Fault::OrdinalOff ? 1 : 0)) & 0xFF;
      mo.op = 15, mo.flags = kOriginFromSubmit, mo.call_site = caster() ? CasterMarkerCall(r.pass, mord) : ring() ? (kRingMarkerCall | mord) : wrestlers() ? (kWrMarkerCall | mord) : (kMarkerCall | mord), mo.object = r.object, mo.part = 0, mo.pass_site = kMarkerPass;
      if (verify) {   // (10) the verify arm: the marker is an EXTRA draw ahead of the run; the game's own draws stay (nothing is dropped)
        inserts.push_back({r.first_op, uint32_t(emu.draws.size())});
        emu.draws.push_back(m);
        emu.origins.push_back(mo);
        verify_keys.insert(MarkKey(KeyObject(r), r.ordinal));
        continue;
      }
      if ((arena() && ordinal_fill_mode != 2) || (ring() && reserve_ordinals)) reserved.push_back({fd, emu.origins[fd]});   // lane 258: the cut draws still count in the thin ordinals (ComputeKeys), so the instances after a cut keep their keys
      emu.draws[fd] = m;
      emu.origins[fd] = mo;
      for (size_t k = 1; k < r.draws.size(); ++k) {
        drop[r.ops[k]] = 1;
        if ((arena() && ordinal_fill_mode != 2) || (ring() && reserve_ordinals)) reserved.push_back({r.draws[k], emu.origins[r.draws[k]]});
        emu.origins[r.draws[k]] = DrawOrigin{};
      }
    }
    // lane 274 (the real-calls arm, D3_REAL_CALLS=1, Mode::Record): the emulated hook makes ONE call per binder run, so `calls = runs` is true by construction and hid the ring's merged instances (RL1, run 7171). Here the hook's keyed-call
    // count of the frame is the CAPTURE's: the LightRefs (one per real colour call) of every object that has a keyed run in the stream. A merged run then counts 4 real calls against 1 run.
    if (mode == Mode::Record && arena()) {
      static const bool real_calls = std::getenv("D3_REAL_CALLS") != nullptr;
      if (real_calls && records.records()) {
        std::unordered_set<uint32_t> seen;
        uint32_t total = 0;
        for (const Run& r : runs)
          if (r.Keyed() && seen.insert(r.object).second) total += records.RealCalls(uint32_t(hfno), r.object);
        tally.SetKeyedCalls(hfno, total);
        real_calls_total += total;
      }
    }
    if (markers) {
      std::vector<Op> kept;
      kept.reserve(emu.ops.size() + inserts.size());
      size_t ins = 0;
      std::sort(inserts.begin(), inserts.end());
      for (size_t i = 0; i < emu.ops.size(); ++i) {
        while (ins < inserts.size() && inserts[ins].first == i) kept.push_back({OpKind::Draw, inserts[ins++].second});
        if (!drop[i]) kept.push_back(emu.ops[i]);
      }
      emu.ops.swap(kept);
      ++frames_with_markers;
    }
    frame.ops = {emu.ops.data(), uint32_t(emu.ops.size())};
    frame.draws = {emu.draws.data(), uint32_t(emu.draws.size())};
    frame.origins = {emu.origins.data(), uint32_t(emu.origins.size())};
    if (frame.thin_facts.count && frame.thin_facts.count != emu.draws.size()) {   // lane 152: a table shorter than the draws reads as "no facts" and moves every key (the verify arm appends draws)
      emu.facts.assign(frame.thin_facts.data, frame.thin_facts.data + frame.thin_facts.count);
      emu.facts.resize(emu.draws.size());
      frame.thin_facts = {emu.facts.data(), uint32_t(emu.facts.size())};
    }
    return markers;
  }

  // ---- the marker's draw (live and fview alike): the packet, the sequence, a mini frame of synthesized part draws ------------------------------------------------------------------------------------
  struct Mini {
    LoadedFrame frame;
    std::vector<Draw> draws;
    std::vector<DrawFixed> fixeds;
    std::vector<DrawDynamic> dynamics;
    std::vector<rdr::SamplerDesc> samplers;
    std::vector<TextureSlot> slots;
    std::vector<ConstantBank> banks;
    std::vector<BoolLoop> bools;
    std::vector<DrawOrigin> origins;
    std::vector<VertexLayout> layouts;     // lane 217 (E2b): a caster part's own layout, remapped
    std::vector<VertexElement> elements;
    std::string strings;
    std::vector<uint64_t> keys;
    std::vector<thin::ThinKey> fulls;
    std::vector<thin::ThinTemplate> tpls;
    std::unordered_map<uint64_t, const thin::ThinTemplate*> overlay;
  };
  std::map<std::tuple<uint64_t, uint32_t, uint32_t>, std::shared_ptr<const Packet>> bind_packets;   // lane 204 (P): the packets this bind has taken (frame, object, ordinal)
  void BeginBind() { bind_packets.clear(); }
  enum class MarkerResult : uint8_t { Ok, Hole, NoSequence, EpochDrop };
  // Takes the marker's packet and sequence and builds the mini frame (the join_by_index / offset0 plants change which packet is asked for).
  MarkerResult BuildMini(const LoadedFrame& frame, uint32_t marker_draw, const Bank* pass_vs, const Bank* pass_ps, const std::vector<uint8_t>& vs_has, const std::vector<uint8_t>& ps_has, Mini& m) {
    const DrawOrigin* o = frame.OriginOf(marker_draw);
    ++c.markers;
    const Draw& marker_draw_rec = frame.draws[marker_draw];
    const uint32_t real_object = o->object, ordinal = o->call_site & 0xFF, generation = uint32_t(marker_draw_rec.index_offset);
    const uint32_t mpass = caster() ? CasterPassOfMarker(o->call_site) : 0u;
    const uint32_t object = KeyObject(real_object, mpass);   // lane 211: the key object (the composite for a caster; the model itself for the arena)
    uint64_t pf = marker_draw_rec.vertex_offset;   // the hook's own guest frame, carried by the marker record (+20 word): the join needs no offset between the hook's frame and the frame header's
    if (fault == Fault::JoinByIndex) pf = frame.header.capture_frame;   // the plant: joined by the record index, not the guest frame
    if (fault == Fault::Offset0) pf = marker_draw_rec.vertex_offset + 1;     // the plant: another frame's packet
    Bank marker_vs, marker_ps;   // the marker draw's own banks: the shadow before the object
    {
      const Draw& md = frame.draws[marker_draw];
      marker_vs.fill(0), marker_ps.fill(0);   // (N4) an empty frame / a stale bank index reads as zeros, never out of range
      if (md.vs_bank < frame.constant_banks.count) std::memcpy(marker_vs.data(), frame.constant_banks[md.vs_bank].dwords, sizeof(Bank));
      if (md.ps_bank < frame.constant_banks.count) std::memcpy(marker_ps.data(), frame.constant_banks[md.ps_bank].dwords, sizeof(Bank));
    }
    // lane 204 (P): a packet is taken once, but one bind can repeat a pass's ops (live.cpp CatchUp) and so the same marker: the packets this bind already took are looked up first (cleared in BeginBind).
    const auto bind_key = std::make_tuple(pf, object, ordinal);
    std::shared_ptr<const Packet> pkt;
    if (auto bt = bind_packets.find(bind_key); bt != bind_packets.end()) pkt = bt->second;
    else if ((pkt = packets->Take(pf, object, ordinal))) bind_packets.emplace(bind_key, pkt);
    if (!pkt) {
      ++c.holes;
      return MarkerResult::Hole;
    }
    RecordSrc rs;   // lane 152 (Mode::Record): the call's records by the hook's frame stamp the marker carries; a missing light state is a hole (the hook's probe said it could be written)
    if (mode == Mode::Record) RecordFor(pf, real_object, pkt->call, ordinal, rs, mpass);   // (review S1) whether the records are enough is decided PER REGISTER below, not by a light state the object may not read
    auto it = seqs.find(MarkKey(object, ordinal));
    const Sequence* retired_seq = nullptr;
    if (auto fz = frozen.find(MarkKey(object, ordinal)); fz != frozen.end() && generation <= fz->second.max_gen) {   // lane 301: the marker's mark set still held the object when its sequence was frozen: it draws exactly what the last proof certified
      retired_seq = &fz->second.seq;
      ++c.frozen_markers;
    } else if (it == seqs.end()) {   // a sequence retired in the last frames still serves the markers already in flight (review fix 15: alive lag + 1 frames)
      auto rt = retired.find(MarkKey(object, ordinal));
      if (rt != retired.end()) retired_seq = &rt->second.first;
    }
    if (!retired_seq && (it == seqs.end() || it->second.parts.empty())) {
      if (generation < drop_gen) ++epoch_drops;
      else ++c.no_sequence;
      return generation < drop_gen ? MarkerResult::EpochDrop : MarkerResult::NoSequence;
    }
    const Sequence& s = retired_seq ? *retired_seq : it->second;
    if (!retired_seq) {
      Sequence& sq = it->second;
      if (frame.header.guest_frame > sq.last_seen + 2) sq.streak_start = frame.header.guest_frame;
      sq.last_seen = frame.header.guest_frame;
    }
    if (mode == Mode::Record && !verify && fault != Fault::DrawUnsourced) {   // (review S1) every register the parts read must have a source NOW: a record missing at marker time is a hole, never a zero fill that still draws
      CompareStats lc;
      Bank pv, pp;
      for (size_t k = 0; k < s.parts.size(); ++k) {
        ComposeCtx cc;
        cc.mode = mode, cc.fault = fault, cc.pass_vs = pass_vs, cc.pass_ps = pass_ps, cc.vs_has = &vs_has, cc.ps_has = &ps_has, cc.rec = &rs;
        ComposePart(s.parts[k], cc, pv, pp, &lc, nullptr, nullptr);
      }
      if (lc.unsourced_regs) {
        if (DebugOn()) { std::fprintf(stderr, "[d3dbg] caster hole frame %llu object %08X ordinal %u:", (unsigned long long)frame.header.guest_frame, object, ordinal); for (const auto& kv : lc.unsourced) std::fprintf(stderr, " %s x%llu;", UnsourcedName(kv.first).c_str(), (unsigned long long)kv.second); std::fprintf(stderr, "\n"); }
        // lane 301 (11.10 fix 2): the two causes are counted apart. A register of the SEQUENCE that no record can ever give (drift learned, not mapped, a palette no bone matched) is `unsourced after mark` (must stay 0: the freeze keeps
        // the proven parts for the markers in flight); `record missing` is only a light / world / clock / caster record that is really absent at marker time.
        bool record_side = false;
        for (const auto& kv : lc.unsourced) {
          const NoSrc why = NoSrc(kv.first >> 24);
          const bool sequence_side = why == NoSrc::Drift || why == NoSrc::NotMapped || why == NoSrc::PaletteSlot || (why == NoSrc::NoPalette && rs.have_pal);   // review S5: MapACollapse (the record lacks the pose) and NoPalette with no palette record are RECORD-side
          if (!sequence_side) record_side = true;
        }
        if (record_side || !freeze_on) ++c.record_missing;   // (the plant no_freeze = the code before lane 301: every unsourced register was "record missing" -> the 7118 / 7202 signature)
        if (!record_side) ++c.unsourced_after_mark;
        ++c.holes;
        return MarkerResult::Hole;
      }
    }
    if (verify) {   // (10) nothing is drawn in the verify arm: the marker only proves its packet and records arrived; the sequence is compared with the stream's real draws in Observe
      verify_keys.insert(MarkKey(object, ordinal));   // lane 204 (Q): live too (fview's EmulateHook alone filled it, so the live verify arm compared nothing); cleared at the end of D3EndFrame
      m.draws.clear();
      m.frame.draws = {m.draws.data(), 0};
      return MarkerResult::Ok;
    }
    if (!pkt->parts.empty() && pkt->parts.size() != s.parts.size()) {   // the proof bar's "draw count and order per object": the stream had another number of parts this frame
      ++c.seq_len_differs;
      unstable.insert(MarkKey(object, ordinal));
    }
    m.strings.clear();
    m.draws.clear(), m.fixeds.clear(), m.dynamics.clear(), m.samplers.clear(), m.slots.clear(), m.banks.clear(), m.bools.clear(), m.origins.clear(), m.layouts.clear(), m.elements.clear(), m.keys.clear(), m.fulls.clear(), m.tpls.clear(), m.overlay.clear();
    m.tpls.reserve(s.parts.size());
    m.strings.push_back('\0');   // (a name field is the byte offset + 1: offset 0 is "no name")
    m.draws.reserve(s.parts.size());
    m.slots.reserve(s.parts.size() * 8);
    for (size_t k = 0; k < s.parts.size(); ++k) {
      const PartDesc& d = s.parts[k];
      ComposeCtx cc;
      cc.mode = mode, cc.fault = fault, cc.pass_vs = pass_vs, cc.pass_ps = pass_ps, cc.vs_has = &vs_has, cc.ps_has = &ps_has, cc.marker_vs = &marker_vs, cc.marker_ps = &marker_ps;
      cc.pf = (k < pkt->parts.size() && mode != Mode::Record) ? &pkt->parts[k] : nullptr;   // lane 152: in Mode::Record the packet's banks are the TRUTH only, never a source
      cc.rec = mode == Mode::Record ? &rs : nullptr;
      ConstantBank cv, cp;
      Bank bv, bp;
      Bank tv{}, tp{};
      const PartFields* truth = (k < pkt->parts.size()) ? &pkt->parts[k] : nullptr;   // the stream's own banks (offline): the readiness measurement compares against them
      if (truth && truth->vs.size() >= 1024) std::memcpy(tv.data(), truth->vs.data(), sizeof(Bank));
      if (truth && truth->ps.size() >= 1024) std::memcpy(tp.data(), truth->ps.data(), sizeof(Bank));
      const uint64_t before = readiness.mismatching;
      std::vector<uint32_t> dv;   // the offline ledger: every frame's marker parts are compared with the stream's truth, and a pass / template register that differs is a drift register (SKIP_LEDGER's classification over the capture)
      readiness.diffs = &dv;
      ComposePart(d, cc, bv, bp, &readiness, truth ? &tv : nullptr, truth ? &tp : nullptr);
      readiness.diffs = nullptr;
      for (uint32_t x : dv) {
        auto& lg = ledger[d.key];
        (x >> 16 ? lg.second : lg.first).set(x & 0xFF);
      }
      if (DebugOn() && readiness.mismatching != before) {
        std::fprintf(stderr, "[d3dbg] frame %llu object %08X part %u site %c variant %s: ", (unsigned long long)frame.header.guest_frame, object, d.tk.part, d.mirror ? 'B' : 'A', d.vs_variant >= 0 ? rdr::fx::kVariants[d.vs_variant].name : "?");
        for (uint32_t r : {56u, 57u, 185u, 189u, 190u})
          if (std::memcmp(&bv[r * 4], &tv[r * 4], 16) != 0) {
            float a[4], b[4];
            std::memcpy(a, &bv[r * 4], 16), std::memcpy(b, &tv[r * 4], 16);
            std::fprintf(stderr, "c%u composed (%g %g %g %g) stream (%g %g %g %g); ", r, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
          }
        std::fprintf(stderr, "\n");
      }
      ++c.parts_total;
      c.pass_fallback = readiness.pass_fallback;
      std::memcpy(cv.dwords, bv.data(), sizeof(Bank));
      std::memcpy(cp.dwords, bp.data(), sizeof(Bank));
      m.banks.push_back(cv);
      m.banks.push_back(cp);
      Draw dr = d.draw;
      if (!d.caster) {   // the arena's thin form: geometry blank, the D1 template supplies it. A caster part keeps the learned draw's geometry facts: OUR geometry is the casters hook's join (lane 211, 17.8 [U])
        dr.flags = uint8_t((dr.flags | kDrawThin) & ~kDrawNoGeometry);
        dr.vertex_offset = dr.index_offset = thin::kBlankHash;
        dr.vertex_bytes = dr.index_bytes = dr.vertex_count = dr.index_count = thin::kBlankCount;
        if (d.wrestler && fault != Fault::PartCountsMissing) dr.vertex_count = d.tpl.stream_vertex_count, dr.index_count = d.tpl.stream_index_count;   // lane 227: the part's counts travel with it (the wrestler core joins by them)
        dr.layout = 0xFFFFFFFFu;
      }
      if (d.caster) {   // lane 217 (E2b): the learned draw's layout index belongs to the learned frame: the mini frame carries the part's own copy
        if (d.has_layout) {
          VertexLayout vl = d.layout;
          vl.element_first = uint32_t(m.elements.size());
          m.elements.insert(m.elements.end(), d.elems.begin(), d.elems.end());
          dr.layout = uint32_t(m.layouts.size());
          m.layouts.push_back(vl);
        } else {
          dr.layout = 0xFFFFFFFFu;
        }
      }
      dr.fixed = dr.dynamic = dr.bool_loop = uint32_t(k);
      dr.vs_bank = uint32_t(2 * k), dr.ps_bank = uint32_t(2 * k + 1);
      dr.texture_first = uint32_t(m.slots.size()), dr.texture_count = uint32_t(d.slots.size());
      for (size_t t = 0; t < d.slots.size(); ++t) {
        TextureSlot ts = d.slots[t];
        ts.sampler = uint32_t(m.samplers.size());
        m.samplers.push_back(d.samplers[t]);
        m.slots.push_back(ts);
      }
      m.draws.push_back(dr);
      m.fixeds.push_back(d.fixed);
      m.dynamics.push_back(d.dyn);
      m.bools.push_back(d.bools);
      DrawOrigin dor = d.origin;
      dor.technique = dor.asset = dor.widget = 0;
      if (!d.technique.empty()) dor.technique = uint32_t(m.strings.size()) + 1, m.strings += d.technique, m.strings.push_back('\0');
      if (!d.asset.empty()) dor.asset = uint32_t(m.strings.size()) + 1, m.strings += d.asset, m.strings.push_back('\0');
      m.origins.push_back(dor);
      m.keys.push_back(d.key);
      m.fulls.push_back(d.tk);
      thin::ThinTemplate tpl = d.tpl;
      if (fault == Fault::TemplateFirst && k > 0)   // lane 35's class: one material for every part
        for (int sl = 0; sl < rdr::kMaxTextures; ++sl) tpl.slots[sl] = s.parts[0].tpl.slots[sl];
      m.tpls.push_back(tpl);
    }
    for (size_t k = 0; k < s.parts.size(); ++k) m.overlay[s.parts[k].key] = &m.tpls[k];
    m.frame.header = frame.header;
    m.frame.draws = {m.draws.data(), uint32_t(m.draws.size())};
    m.frame.fixeds = {m.fixeds.data(), uint32_t(m.fixeds.size())};
    m.frame.dynamics = {m.dynamics.data(), uint32_t(m.dynamics.size())};
    m.frame.samplers = {m.samplers.data(), uint32_t(m.samplers.size())};
    m.frame.texture_slots = {m.slots.data(), uint32_t(m.slots.size())};
    m.frame.constant_banks = {m.banks.data(), uint32_t(m.banks.size())};
    m.frame.bool_loops = {m.bools.data(), uint32_t(m.bools.size())};
    m.frame.origins = {m.origins.data(), uint32_t(m.origins.size())};
    m.frame.layouts = {m.layouts.data(), uint32_t(m.layouts.size())};
    m.frame.elements = {m.elements.data(), uint32_t(m.elements.size())};
    m.frame.strings = {m.strings.data(), uint32_t(m.strings.size())};
    c.drawn += s.parts.size();
    return MarkerResult::Ok;
  }

  // ---- the offline ledger: the drift registers per key (hashes and register numbers only) ------------------------------------------------------------------------------------------------
  bool LoadLedger(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    static char buf[1 << 15];
    while (std::fgets(buf, sizeof buf, f)) {
      char* p = buf;
      if (*p == 'U') {
        unstable.insert(std::strtoull(p + 1, nullptr, 16));
        continue;
      }
      char* end = nullptr;
      const unsigned long long key = std::strtoull(p, &end, 16);
      if (end == p) continue;
      auto& lg = ledger[key];
      int stage = 0;
      for (p = end; *p; ++p) {
        if (*p == 'v') stage = 0;
        else if (*p == 'p') stage = 1;
        else if (*p >= '0' && *p <= '9') {
          const int v = std::atoi(p);
          while (p[1] >= '0' && p[1] <= '9') ++p;
          (stage ? lg.second : lg.first).set(size_t(v) & 255);
        }
      }
    }
    std::fclose(f);
    return true;
  }
  bool SaveLedger(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    for (uint64_t u : unstable) std::fprintf(f, "U %llx\n", (unsigned long long)u);
    for (const auto& kv : ledger) {
      if (kv.second.first.none() && kv.second.second.none()) continue;
      std::fprintf(f, "%llx v", (unsigned long long)kv.first);
      for (size_t r = 0; r < 256; ++r)
        if (kv.second.first.test(r)) std::fprintf(f, " %zu", r);
      std::fprintf(f, " p");
      for (size_t r = 0; r < 256; ++r)
        if (kv.second.second.test(r)) std::fprintf(f, " %zu", r);
      std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
  }

  // The census/readiness helpers live with the binder (it owns the counters it prints).
  static std::string TopRegisters(const CompareStats& st, size_t n) {
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (const auto& kv : st.by_reg) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    std::string out;
    char buf[64];
    for (size_t i = 0; i < v.size() && i < n; ++i) {
      std::snprintf(buf, sizeof buf, "%s c%u x%llu", (v[i].second >> 16) ? "PS" : "VS", v[i].second & 0xFFFF, (unsigned long long)v[i].first);
      out += (i ? ", " : "") + std::string(buf);
    }
    return out;
  }
};

// lane 217 (review S1 / N1): the caster class's whole state, SELF-CONTAINED from construction: route Casters and its own packets, marks, gate and tally, so nothing it does can reach the arena's live globals
// (g_packets / g_marks / g_gate / g_tally), whichever way a future lane switches it on. Created lazily (binder.h, SetD3CasterOffline): a Binder that never serves casters carries one null pointer.
struct CasterChain {
  D3Core d3c_;
  D3Core::Mini d3c_mini_;
  PacketTable d3c_own_packets_{4096};
  MarkPublisher d3c_own_marks_;
  Bank d3c_pass_vs_{}, d3c_pass_ps_{};
  std::vector<uint8_t> d3c_has_vs_, d3c_has_ps_;
  D3Counters d3c_base_;
  std::function<uint32_t(uint32_t, uint32_t, uint32_t)> d3c_palette_count_;
  std::unordered_map<uint64_t, uint32_t> d3c_slots_cache_;   // lane 221: (vertex range, layout) -> highest bone id + 1 (Binder::D3CSlotsUsed)
  std::function<bool(uint32_t, uint32_t, uint32_t)> d3c_geometry_;
  uint64_t d3c_evict_seen_ = 0;
  bool d3c_pass_ready_ = false, d3c_offline_ = false;
  CasterChain() {
    d3c_.route = thin::Cls::Casters;
    d3c_.packets = &d3c_own_packets_, d3c_.marks = &d3c_own_marks_, d3c_.gate = &d3c_.local_gate, d3c_.hook_tally = &d3c_.tally;
  }
  CasterChain(const CasterChain&) = delete;
  CasterChain& operator=(const CasterChain&) = delete;
};

// lane 229 (Ring D3 R3, SKIP_GAME_WORK_MAP.md 16.8): the ring class's whole state, SELF-CONTAINED from construction like CasterChain (lane 217 review S1 / N1): route Ring and its own packets, marks, gate and tally, so nothing it does can
// reach the arena's live globals (g_packets / g_marks / g_gate / g_tally), whichever way a future lane switches it on. Created lazily (binder_ring_d3.inc, SetD3RingOffline): a Binder that never serves the ring carries one null pointer.
struct RingChain {
  D3Core d3r_;
  std::vector<std::shared_ptr<const MarkSet>> d3r_fill_sets_;   // lane 289 review N3: the mark sets the fill lists point into (held for the bind, not copied per marker)
  std::unordered_map<uint32_t, const std::vector<uint64_t>*> d3r_fill_;   // lane 289: marker draw index -> the part triples of its skipped call (ThinBeginFrame fills it from the published mark set)
  std::unordered_map<uint32_t, thin::ThinKey> d3r_cut_;   // lane 258: the draws the offline emulation cut (ThinBeginFrame fills it only when there are any)
  D3Core::Mini d3r_mini_;
  PacketTable d3r_own_packets_{4096};
  MarkPublisher d3r_own_marks_;
  Bank d3r_pass_vs_{}, d3r_pass_ps_{};
  std::vector<uint8_t> d3r_has_vs_, d3r_has_ps_;
  D3Counters d3r_base_;
  std::function<uint32_t(uint32_t, uint32_t, uint32_t)> d3r_palette_count_;
  std::unordered_map<uint64_t, uint32_t> d3r_slots_cache_;   // (vertex range, layout) -> highest bone id + 1
  uint64_t d3r_evict_seen_ = 0;
  uint64_t d3r_skipped_frames_ = 0;
  std::string d3r_latch_msg_;
  bool d3r_pass_ready_ = false, d3r_offline_ = false;
  RingChain() {
    d3r_.route = thin::Cls::Ring;
    d3r_.records.index_rings = true;
    d3r_.packets = &d3r_own_packets_, d3r_.marks = &d3r_own_marks_, d3r_.gate = &d3r_.local_gate, d3r_.hook_tally = &d3r_.tally;
  }
  RingChain(const RingChain&) = delete;
  RingChain& operator=(const RingChain&) = delete;
};

// lane 227 (Wrestlers D3 H2): the wrestler class's whole state, SELF-CONTAINED from construction like the casters' (route Wrestlers and its own packets, marks, gate and tally): nothing it does can reach the arena's
// live globals (g_packets / g_marks / g_gate / g_tally). Created lazily (binder.h, SetD3WrestlersOffline): a Binder that never serves wrestlers carries one null pointer.
struct WrestlerChain {
  D3Core w_;
  D3Core::Mini w_mini_;
  PacketTable w_own_packets_{4096};
  MarkPublisher w_own_marks_;
  Bank w_pass_vs_{}, w_pass_ps_{};
  std::vector<uint8_t> w_has_vs_, w_has_ps_;
  D3Counters w_base_;
  std::unordered_map<uint64_t, uint32_t> w_slots_cache_;   // (vertex range, layout) -> highest bone id + 1 (Binder::D3WSlotsUsed)
  uint64_t w_evict_seen_ = 0;
  bool w_pass_ready_ = false, w_offline_ = false;
  WrestlerChain() {
    w_.route = thin::Cls::Wrestlers;
    w_.packets = &w_own_packets_, w_.marks = &w_own_marks_, w_.gate = &w_.local_gate, w_.hook_tally = &w_.tally;
  }
  WrestlerChain(const WrestlerChain&) = delete;
  WrestlerChain& operator=(const WrestlerChain&) = delete;
};

}  // namespace rdr_adapter::d3
