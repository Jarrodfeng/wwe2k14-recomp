// D1 thin draws: the contract (Session 101, lane 122 = K0 of docs\renderer\SKIP_GAME_WORK_MAP.md 2.3 / 4.2). No game or SDK includes: the reader, the binder,
// fview and the unit test build it.
//
// What it is: for a draw of a class we replace in place (arena first), the game still records the stream draw. D1 keeps only what the replaced draw needs from it
// (its place in the frame: the anchor, and the key that names it) and stops the READER from building the rest: the geometry gather and the shader lookups. The binder
// draws a TEMPLATE (what a P2a replacement that passed every check chose: program pair, our geometry, layout, slots ours set) with the stream draw's own banks, slots and
// state. Proof frames re-prove every key on the full path; a pin handshake makes a hole impossible by construction.
//
//   key       = {pass site, object, part, ordinal of this (site, object, part) among the frame's draws in draw order, program = ProgramKey(vs hash, ps hash, ThinDrawFacts)}
//   template  = ThinTemplate (the binder's, thin_store.h)
//   pin set   = the keys the binder holds a template for AND publishes as usable, with a generation. The reader goes lite only for keys of the newest set it has read and
//               stamps that generation on the frame (names.h kSecThin / ThinFrameStamp). The binder releases a key's template only after the frame it is binding carries a
//               generation that no longer has it, and never sooner than kReleaseFrames after its last use. A lite draw with no template is a HOLE (counted; required 0).
//   proof     = a pinned key takes the full path (P2a, every check) in frames where hash(key) % K == frame % K; the binder compares the P2a bind with the template.
//
// Switches (all default off on PC and phone): g_arena_thin / g_ring_thin / g_wrestlers_thin; live --wwe2k14_rdr_arena_thin / _ring_thin / _wrestlers_thin (each needs its
// class's _ours switch), fview --thin CLASS. Arena: lane 122. Ring and wrestlers: lane 129 (KC), the same keys; a template's class says which hook's slot draws it.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../renderer/api.h"
#include "names.h"

namespace rdr_adapter::thin {

// ---- switches ---------------------------------------------------------------------------------------------------------------------
inline bool g_arena_thin = false, g_ring_thin = false, g_wrestlers_thin = false;

enum class Cls : uint8_t { Arena, Ring, Wrestlers, Casters, Count };   // lane 202: Casters (Caster D3 E0): a class NAME only today; it has no thin draws, no hook slot and no switch (ClsOn is false), so no loop over the classes does anything for it
inline const char* ClsName(Cls c) {
  static const char* const k[] = {"arena", "ring", "wrestlers", "casters"};
  return uint8_t(c) < uint8_t(Cls::Count) ? k[uint8_t(c)] : "?";
}
inline bool ClsOn(Cls c) { return c == Cls::Casters ? false : c == Cls::Arena ? g_arena_thin : c == Cls::Ring ? g_ring_thin : g_wrestlers_thin; }
inline bool AnyOn() { return g_arena_thin || g_ring_thin || g_wrestlers_thin; }  // lane 129 (KC): the reader's gate: any class's keys may be pinned
// Review F1: ONE predicate for "the reader fills and writes the identity facts table" (the reader's fill, its query and its write, and fview's emulation all call this). The planted
// `facts_table_gate` reproduces the bug (the table gated on the arena's switch only: a ring-only or wrestlers-only run then has keys that never match the reader's).
inline bool g_plant_facts_gate = false, g_plant_no_vb = false;
inline bool FactsTableOn() { return g_plant_facts_gate ? g_arena_thin : AnyOn(); }

inline constexpr uint32_t kProofK = 32;           // a key takes the full path in 1 frame of K (about 3 % of the class's draws, spread by the key's hash)
inline constexpr uint32_t kReleaseFrames = 300;   // a template is kept at least this long after its last use
inline constexpr uint64_t kBlankHash = 0xB1A4B1A4B1A4B1A4ull;  // what a lite draw's shader hash / offsets hold (fview's emulation, and a reader that did not fill them)
inline constexpr uint32_t kBlankCount = 0xFFFFFFFEu;  // (fview's emulation only; the live reader leaves a lite draw's offsets, bytes and counts 0 and its layout 0xFFFFFFFF: the binder reads none of them)
inline constexpr uint32_t kSiteColour = 0x82C0A950u, kSiteMirror = 0x82C0B94Cu;  // the model-part colour sites (draw_replace_hook.h kModelPartColourSite / MirrorSite): the only draws with a key

// ---- hashing ----------------------------------------------------------------------------------------------------------------------
inline uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 31;
  return h;
}
inline uint64_t TripleHash(uint32_t pass_site, uint32_t object, uint32_t part) { return Mix(Mix(Mix(0x7A1E, pass_site), object), part) | 1ull; }
// The program (vertex + pixel shader hashes) is part of the key: the same part can be drawn by a different program pair from one frame to the next (Check B: the prop AB0A0E20 switches
// yBG_Default_shadow -> yBG_Default_base), and the reader keeps its shader lookups (it needs the programs' facts for the banks and slots), so a program change is just another key.
inline uint64_t ProgramHash(uint64_t vs_hash, uint64_t ps_hash) { return Mix(Mix(0x9A11, vs_hash), ps_hash); }
inline uint64_t FactsHash(const ThinDrawFacts& f) {
  uint64_t h = Mix(0xFAC7, f.index_addr);
  for (uint32_t v : {f.index_count, f.base_vertex, f.start_index, f.img0, f.img4, f.img5}) h = Mix(h, v);
  if (!g_plant_no_vb) h = Mix(Mix(h, f.vb_addr), f.vb_dw1);   // review F4 (the planted `no_vb_fact` leaves the vertex identity out)
  return h;
}
// The key's program field: the shader pair and the draw's identity facts.
inline uint64_t ProgramKey(uint64_t vs_hash, uint64_t ps_hash, const ThinDrawFacts& f) { return Mix(ProgramHash(vs_hash, ps_hash), FactsHash(f)); }
inline uint64_t KeyHash(uint32_t pass_site, uint32_t object, uint32_t part, uint32_t ordinal, uint64_t program = 0) { return Mix(Mix(TripleHash(pass_site, object, part), ordinal), program) | 1ull; }
// Proof frame for a key: every kProofK-th frame, spread by the key's hash. k == 0: never (the planted `no_proof`).
inline bool IsProofFrame(uint64_t key_hash, uint64_t frame, uint32_t k = kProofK) { return k != 0 && (key_hash % k) == (frame % k); }
// lane 138 (D3): the hash an OBJECT's proof frame is taken from (all the object's parts prove in one frame; d3_arena.h ObjectProofFrame).
inline uint64_t ObjectProofHash(uint32_t object) { return Mix(0xD3D3D3D3ull, object) | 1ull; }

struct ThinKey {
  uint32_t pass_site = 0, object = 0, part = 0, ordinal = 0;
  uint64_t program = 0;  // ProgramHash of the draw's shader pair
  uint64_t Hash() const { return KeyHash(pass_site, object, part, ordinal, program); }
  uint64_t Triple() const { return TripleHash(pass_site, object, part); }
};

// ---- the template -----------------------------------------------------------------------------------------------------------------
// What the binder keeps per pinned key: exactly what a P2a replacement chose (draw_replace_hook.h ReplaceOut), plus the stream facts the proof compares.
struct ThinSlot {
  bool set = false;
  rdr::ImageId image = rdr::kNoImage;
  rdr::Format view_format = rdr::Format::Undefined;
  uint8_t swizzle[4] = {0, 0, 0, 0};
  uint8_t min_level = 0, max_level = 0;  // the STREAM slot's level window (a stream fact the proof compares; ours has the file's whole chain)
};

struct ThinTemplate {
  Cls cls = Cls::Arena;
  int vs = -1, ps = -1;                 // rdr::fx::kVariants indices the binder chose for the learned draw
  uint16_t layout = 0xFFFFu;            // ours (built by name)
  uint8_t topology = 0xFFu, index_32bit = 0xFFu;
  rdr::BufferRange vertex{}, index{};
  uint32_t index_count = 0;
  ThinSlot slots[rdr::kMaxTextures];
  uint32_t stream_vertex_count = 0, stream_index_count = 0;  // the stream draw's counts when learned (the proof compares them: a lite draw does not carry them)
  uint64_t blend_base = 0;              // the 120 Hz tween's key before its occurrence (binder.h BlendRanges), from the learned draw
  uint32_t learned_frame = 0;
  // lane 129 (KC), PLANT ONLY (the honest template holds none): `template_rows` keeps the wrestler palette rows of the learned draw and applies them instead of the stream's bank.
  uint32_t plant_first_register = 0, plant_rows = 0;
  std::vector<float> plant_row_data;
};

// ---- the pin set and its publisher ------------------------------------------------------------------------------------------------
struct PinSet {
  uint32_t generation = 0;
  std::vector<uint64_t> keys;      // sorted key hashes the reader may go lite for
  std::vector<uint64_t> triples;   // sorted (site, object, part) hashes of those keys: the reader counts ordinals only for these
  std::vector<uint64_t> arena_triples;   // lane 152 (D3-F item 13): the subset of `triples` whose keys are the ARENA's (D3's proof-by-object applies to these only)
  std::vector<uint64_t> wrestler_triples;   // lane 227 (Wrestlers D3 H2): the subset whose keys are the WRESTLERS' (the wrestler chain's proof-by-object applies to these only, behind its own switch)
  bool Has(uint64_t key) const { return std::binary_search(keys.begin(), keys.end(), key); }
  bool IsArenaTriple(uint64_t triple) const { return std::binary_search(arena_triples.begin(), arena_triples.end(), triple); }
  bool IsWrestlerTriple(uint64_t triple) const { return std::binary_search(wrestler_triples.begin(), wrestler_triples.end(), triple); }
  bool HasTriple(uint64_t triple) const { return std::binary_search(triples.begin(), triples.end(), triple); }
};

// Binder -> reader: the newest published set. Acquire() is one lock and a shared_ptr copy (once a frame).
class PinPublisher {
 public:
  uint32_t Publish(std::vector<uint64_t> keys, std::vector<uint64_t> triples, bool force_new_generation = false, std::vector<uint64_t> arena_triples = {}, std::vector<uint64_t> wrestler_triples = {}) {
    std::sort(keys.begin(), keys.end());
    std::sort(triples.begin(), triples.end());
    triples.erase(std::unique(triples.begin(), triples.end()), triples.end());
    std::sort(arena_triples.begin(), arena_triples.end());
    arena_triples.erase(std::unique(arena_triples.begin(), arena_triples.end()), arena_triples.end());
    std::sort(wrestler_triples.begin(), wrestler_triples.end());
    wrestler_triples.erase(std::unique(wrestler_triples.begin(), wrestler_triples.end()), wrestler_triples.end());
    std::lock_guard<std::mutex> lock(mu_);
    if (!force_new_generation && current_ && current_->keys == keys && current_->triples == triples && current_->arena_triples == arena_triples && current_->wrestler_triples == wrestler_triples) return current_->generation;
    auto set = std::make_shared<PinSet>();
    set->generation = ++generation_;
    set->keys = std::move(keys);
    set->triples = std::move(triples);
    set->arena_triples = std::move(arena_triples);
    set->wrestler_triples = std::move(wrestler_triples);
    current_ = std::move(set);
    return generation_;
  }
  std::shared_ptr<const PinSet> Acquire() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }
  uint32_t generation() const {
    std::lock_guard<std::mutex> lock(mu_);
    return generation_;
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    current_.reset();
    generation_ = 0;
  }

 private:
  mutable std::mutex mu_;
  std::shared_ptr<const PinSet> current_;
  uint32_t generation_ = 0;
};
// The live pair (game render thread reads, our render thread publishes). fview keeps its own so a test can run two.
inline PinPublisher g_pins;

// ---- counters ---------------------------------------------------------------------------------------------------------------------
enum class Differ : uint8_t { Program, Layout, SlotImage, SlotView, Levels, Geometry, Counts, Blend, Count };
inline const char* DifferName(Differ d) {
  static const char* const k[] = {"program", "layout", "slot image", "slot view", "levels", "geometry", "counts", "blend key"};
  return uint8_t(d) < uint8_t(Differ::Count) ? k[uint8_t(d)] : "?";
}

struct ThinCounters {
  uint64_t frames = 0;          // frames bound with the class on
  uint64_t thin = 0;            // draws drawn from a template
  uint64_t proof = 0;           // pinned keys that took the full path and were compared
  uint64_t proof_ok = 0;
  uint64_t proof_mismatch = 0;  // proof compares that differed (required 0 after the first frames); by reason below
  uint64_t differ[uint8_t(Differ::Count)] = {};
  uint64_t learned = 0;         // draws whose P2a replacement gave their key a template
  uint64_t full_other = 0;      // full-path replaced draws that were neither a proof nor a learn (an unpinned key waiting out the handshake)
  uint64_t epoch_drops = 0;     // lite draws dropped because the hook replaced its assets (an arena change): templates cleared, frames in flight lose these draws
  uint64_t unpinned = 0;        // keys dropped from the set (a failed check on the full path, or a changed template)
  uint64_t released = 0;        // templates freed after the handshake
  uint64_t holes = 0;           // lite draws with no template (required 0)
  uint64_t slot_missing = 0;    // thin draws whose stream binds no image where the template sets one (drawn without that slot: counted, required 0)
  uint64_t pinned = 0;          // keys in the newest set
  uint64_t blend_moved = 0;
  uint64_t buffer_unpins = 0;   // lane 129 review F2: keys erased because the hook destroyed a buffer their template names (a swept wrestler part)
  uint64_t object_unpins = 0;   // keys unpinned because the arena dropped their object's join
  uint64_t not_learned = 0;     // lite draws of a capture replayed without their keys learned (rplay / case zips): drawn by nothing, not a hole     // info: the proof saw a different tween key (offsets of the stream's geometry differ between frames)
  double ms_thin = 0, ms_proof = 0;
};

inline std::string CensusLine(Cls c, const ThinCounters& k) {
  char text[768];
  std::snprintf(text, sizeof text,
                "[rdr][thin] %s thin %llu, proof %llu (ok %llu), learned %llu, pinned %llu, holes %llu, proof mismatches %llu (%s %llu, %s %llu, %s %llu, %s %llu, %s %llu, %s %llu, %s %llu), unpinned %llu, released %llu, slot missing %llu, tween key moved %llu, full other %llu, epoch drops %llu, object unpins %llu, not learned %llu, frames %llu, ms thin %.2f proof %.2f",
                ClsName(c), (unsigned long long)k.thin, (unsigned long long)k.proof, (unsigned long long)k.proof_ok, (unsigned long long)k.learned, (unsigned long long)k.pinned,
                (unsigned long long)k.holes, (unsigned long long)k.proof_mismatch, DifferName(Differ::Program), (unsigned long long)k.differ[0], DifferName(Differ::Layout),
                (unsigned long long)k.differ[1], DifferName(Differ::SlotImage), (unsigned long long)k.differ[2], DifferName(Differ::SlotView), (unsigned long long)k.differ[3],
                DifferName(Differ::Levels), (unsigned long long)k.differ[4], DifferName(Differ::Geometry), (unsigned long long)k.differ[5], DifferName(Differ::Counts),
                (unsigned long long)k.differ[6], (unsigned long long)k.unpinned, (unsigned long long)k.released, (unsigned long long)k.slot_missing, (unsigned long long)k.blend_moved, (unsigned long long)k.full_other, (unsigned long long)k.epoch_drops, (unsigned long long)k.object_unpins, (unsigned long long)k.not_learned,
                (unsigned long long)k.frames, k.ms_thin, k.ms_proof);
  return text;
}

// Faults (planted; fview --thin-fault NAME, the unit test). Names are the gate's.
enum class Fault : uint8_t { None, ReleaseWithoutAck, FirstTemplate, WrongSiteTemplate, AllMips, ReadStream, NoProof, StaleGeneration, NoProgramKey, NoOrdinal, ObjectSibling, ForeignNoUnpin,
                             FactsTableGate, NoVbFact, RopeColourVisible,
                             TemplateRows, RopeColourInstance0, SkirtTie, ClothPinned /* lane 129 (KC) */,
                             EmuSharedTables /* lane 149 */, WrongLayout /* lane 162 */ };
inline bool ParseFault(const std::string& s, Fault& f) {
  static const struct { const char* n; Fault f; } k[] = {{"", Fault::None}, {"release_without_ack", Fault::ReleaseWithoutAck}, {"first_template", Fault::FirstTemplate},
                                                          {"wrong_site_template", Fault::WrongSiteTemplate}, {"all_mips", Fault::AllMips}, {"read_stream", Fault::ReadStream},
                                                          {"no_proof", Fault::NoProof}, {"stale_generation", Fault::StaleGeneration}, {"no_program_key", Fault::NoProgramKey},
                                                          {"no_ordinal", Fault::NoOrdinal}, {"object_sibling", Fault::ObjectSibling}, {"foreign_no_unpin", Fault::ForeignNoUnpin},
                                                          {"facts_table_gate", Fault::FactsTableGate}, {"no_vb_fact", Fault::NoVbFact}, {"rope_colour_visible", Fault::RopeColourVisible},
                                                          {"template_rows", Fault::TemplateRows}, {"rope_colour_instance0", Fault::RopeColourInstance0}, {"skirt_tie", Fault::SkirtTie},
                                                          {"cloth_pinned", Fault::ClothPinned}, {"emu_shared_tables", Fault::EmuSharedTables}, {"wrong_layout", Fault::WrongLayout}};   // lane 149: emu_shared_tables = the old fview emulation (one stamp and one facts table for every loaded frame, re-run on every bind)
  for (const auto& e : k)
    if (s == e.n) return f = e.f, true;
  return false;
}

}  // namespace rdr_adapter::thin
