// The wrestlers' light object computed at the latch (Session 103, lane A8; docs\renderer\INPUTS_A8.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Pure: scene_record.h only, header only, no game, no SDK.
//
// Bit 7 / E8: a wrestler's light STATE (R+0..895) is what the game's draw node installs into R before the model call. The node installs the wrestler's EMBEDDED light object (char + 3936, block char + 3984,
// sub_8259FAE8 0x8259FBE0-0x8259FBE8 -> sub_82620488 mode 0 -> sub_8261FE90 0x82620528) which the character's update task sub_8259F630 (0x8259F654-0x8259F680) fills by copying the blend object (mgr + 376) when
// mgr + 380 == 1, else the set at [char + 3920] (id 10). The embedded object is PERSISTENT data: the latch reads it as it is (no source rule, no mgr + 380), and the walk's R equals it, byte for byte, at the
// latch of the SAME frame (docs\renderer\INPUTS_A8.md: 3,480 of 3,480 refs; 920 of 920 on changed frames; the blend / set 10 rules fail on 148 model-latches because the set objects are lerped in place by
// sub_827F8E08 after the copy).
// Under OURS the walk is skipped and no state exists. This module (1) keeps the latch's wrestler blocks per hook frame, (2) at record j's Emit, for each actor "actor:slot:K": compares the walk's state when there
// is one (the census `wrestler_light latch==walk`), and (3) only when asked (the switch), writes a LightState + LightRef from the block where the walk recorded none. The ref frame is header - 1 (E8's stamp), the
// block is the latch of hook header - 2: the same frame (a LightRef carries the draw-half record frame j - 1, the latch of record j - 1 carries hook j - 2: A6 delta +1).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "scene_record.h"
#include "rows_circle.h"   // S106 G4-2
#include "rows_inputs.h"
#include "rows_lights.h"

namespace rdr_scene::wlight {

inline constexpr uint32_t kSlots = 9;
inline constexpr uint32_t kKeep = 8;   // hook frames kept (the consumer needs hook - 1 and its changed-baseline hook - 2)

struct Entry {
  uint32_t slot = 0;        // the character slot 0..8 (the word at 0x836F38D0 + 4 * slot)
  uint32_t character = 0;   // the word
  uint32_t source = 0;      // the block's guest address (char + 3984 on the embedded path)
  uint32_t skel = 0;        // S111 DARKDIP: the character's skeleton [char + 48] (the body its draw node draws and lights); 0 = not read (an offline replay row): joined by slot
  uint32_t block[kLsLightFloats] = {};
};

// Test-only faults (the planted twins). Each must change the verdict of tools\scene\a8_proof.cpp / a8_unit.cpp.
struct Faults {
  bool stale = false;          // use the latch of one hook EARLIER than the same frame
  bool early = false;          // use the latch of the NEXT hook (the frame after)
  bool wrong_slot = false;     // join the actor to slot + 1
  bool always_equal = false;   // the census counts everything equal
  bool no_changed_gate = false;  // the census counts static frames too (the "changed frames only" rule)
  bool no_walk_guard = false;  // overwrite a walked state (the synthesis ignores the walk)
  bool ignore_ours = false;    // synthesize on a walked frame too
};

struct Census {
  uint64_t frames = 0, no_latch = 0;          // records seen; records with no latch stored for the same-frame hook
  uint64_t walked = 0, eq = 0, differ = 0;    // walked refs compared (all)
  uint64_t changed = 0, eq_changed = 0, differ_changed = 0;   // the bar: a CHANGED block cannot be matched by a stale read
  uint64_t synth = 0, synth_refused = 0, no_entry = 0, no_actor_slot = 0, fallback = 0;
  uint32_t last_actors = 0, last_with_entry = 0;   // S103 L1C: the LAST Apply: wrestler actors with an engine object, and how many of them have a latch entry (bit 7's stamp: caster_stamp.h StampFromLatch)
  char first[200] = {};
};

// "actor:slot:K" (the record's wrestler actors, ids written by lane B / the wrestler contributor) -> K. False for any other id.
inline bool ActorSlot(std::string_view id, uint32_t& slot) {
  static constexpr std::string_view kP = "actor:slot:";
  if (id.size() <= kP.size() || id.substr(0, kP.size()) != kP) return false;
  uint32_t v = 0;
  size_t i = kP.size();
  if (i >= id.size() || id[i] < '0' || id[i] > '9') return false;
  for (; i < id.size() && id[i] >= '0' && id[i] <= '9'; ++i) v = v * 10 + uint32_t(id[i] - '0');
  if (i < id.size() && id[i] != ':') return false;
  slot = v;
  return v < kSlots;
}

struct Latch {
  uint32_t hook = 0;
  bool valid = false;
  std::vector<Entry> entries;
  const Entry* Find(uint32_t slot) const {
    for (const Entry& e : entries)
      if (e.slot == slot) return &e;
    return nullptr;
  }
  // S111 DARKDIP (docs\research\s111\DARKDIP.md): the block of the body the slot DRAWS. The game's draw node lights a body with its own character's object (sub_825A3908 +160 / sub_8259FAE8 +100:
  // sub_82620488(char + 3936, 0)), and a slot can draw a character the table no longer names (the editor's old one until its flip, drawn_slots.h). Entries without a skeleton join by slot as before.
  const Entry* FindBody(uint32_t slot, uint32_t body) const {
    if (body)
      for (const Entry& e : entries)
        if (e.slot == slot && e.skel == body) return &e;
    return Find(slot);
  }
};

// The latch's wrestler blocks of the last kKeep hook frames. Thread-safe (the latch thread Puts, the writer thread reads).
class Store {
 public:
  void Put(uint32_t hook, std::vector<Entry>&& entries) {
    std::lock_guard<std::mutex> lock(mu_);
    for (Latch& l : ring_)
      if (l.valid && l.hook == hook) {   // the same hook twice (Emit's own Take after a missed latch): keep the first
        return;
      }
    Latch& l = ring_[next_];
    next_ = (next_ + 1) % kKeep;
    l.hook = hook, l.valid = true, l.entries = std::move(entries);
  }
  bool Get(uint32_t hook, Latch& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const Latch& l : ring_)
      if (l.valid && l.hook == hook) {
        out = l;
        return true;
      }
    return false;
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    for (Latch& l : ring_) l = Latch{};
    next_ = 0;
  }

 private:
  mutable std::mutex mu_;
  Latch ring_[kKeep];
  uint32_t next_ = 0;
};

// Record j (header frame): the LightRef frame of its draw half is j - 1 (E8's stamp, shadow_fit_record.h WrestlerLightStamp), and the latch of the SAME frame is the hook that carries record j - 1 = hook j - 2.
inline uint32_t RefFrameOfRecord(const SceneTables& t) { return t.header.guest_frame >= 1 ? uint32_t(t.header.guest_frame) - 1u : 0u; }   // S103 CLK2: header 0 / 1 never wrap to 0xFFFFFFFF
inline uint32_t HookOfRecord(const SceneTables& t, const Faults* ft = nullptr) {
  const uint32_t ref = RefFrameOfRecord(t);
  uint32_t h = ref >= 1u ? ref - 1u : 0u;
  if (ft && ft->stale && h >= 1u) h -= 1u;   // S103 CLK2: never wraps below 0
  if (ft && ft->early) h += 1u;
  return h;
}

// Synthesize only for an OURS frame (the walk was skipped): on a walked / fallback frame whose lights tracker closes late, no ref exists YET and a synthetic one would shadow the walk's. `ours` = scene_frame::Ours(header - 1)
// (null = unknown = no). Fault `ignore_ours` is the planted twin.
inline bool ShouldSynthesize(bool switch_on, bool (*ours)(uint64_t), const SceneTables& t, const Faults* ft = nullptr) {
  if (!switch_on) return false;
  if (ft && ft->ignore_ours) return true;
  return t.header.guest_frame != 0 && ours && ours(uint64_t(RefFrameOfRecord(t)));   // S103 CLK: header 0 would wrap to 0xFFFFFFFF
}

// Apply the module to one record. `synthesize` false = the mirror census only (nothing in the record changes). Returns the number of states written.
//  * per actor "actor:slot:K" of the record with an engine object: the walked ref of THIS frame (r.object == the actor, r.frame == ref frame), if any;
//  * walked + entry: compare the 224 words; counted on all refs and, apart, on CHANGED ones (the entry's block differs from the previous hook's block of the same slot);
//  * not walked + entry + synthesize: append one LightState (light = the block) and one LightRef; never over a walked ref.
inline uint32_t Apply(SceneTables& t, const Store& store, bool synthesize, Census& c, const Faults* ft = nullptr) {
  ++c.frames;
  const uint32_t ref_frame = RefFrameOfRecord(t);
  const uint32_t hook = HookOfRecord(t, ft);
  Latch cur, prev;
  const bool have = store.Get(hook, cur);
  const bool have_prev = have && store.Get(hook - 1u, prev);
  if (!have) ++c.no_latch;
  uint32_t written = 0, actors = 0, with_entry = 0;
  for (size_t i = 0; i < t.objects.size() && i < t.engine_objects.size(); ++i) {
    uint32_t slot = 0;
    if (!t.engine_objects[i] || !ActorSlot(t.Str(t.objects[i].id), slot)) continue;
    if (ft && ft->wrong_slot) slot = (slot + 1) % kSlots;
    const Entry* e = have ? cur.FindBody(slot, t.engine_objects[i]) : nullptr;   // S111 DARKDIP: the drawn body's own block
    ++actors;
    if (e) ++with_entry;
    const LightRef* walked = nullptr;
    for (const LightRef& r : t.light_refs)
      if (r.object == t.engine_objects[i] && r.frame == ref_frame && r.state < t.light_states.size()) {
        walked = &r;
        break;
      }
    if (!e) {
      ++c.no_entry;
      continue;
    }
    if (walked && !(synthesize && ft && ft->no_walk_guard)) {
      const uint32_t* w = reinterpret_cast<const uint32_t*>(t.light_states[walked->state].light);
      const bool same = std::memcmp(w, e->block, sizeof e->block) == 0 || (ft && ft->always_equal);
      const Entry* pe = have_prev ? prev.FindBody(slot, t.engine_objects[i]) : nullptr;
      const bool is_changed = pe && std::memcmp(pe->block, e->block, sizeof e->block) != 0;
      ++c.walked;
      (same ? c.eq : c.differ)++;
      if (is_changed || (ft && ft->no_changed_gate)) {
        ++c.changed;
        (same ? c.eq_changed : c.differ_changed)++;
      }
      if (!same && !c.first[0]) {
        uint32_t k = 0;
        while (k < kLsLightFloats && w[k] == e->block[k]) ++k;
        std::snprintf(c.first, sizeof c.first, "slot %u frame %u word %u walk %08X latch %08X", slot, ref_frame, k, k < kLsLightFloats ? w[k] : 0u, k < kLsLightFloats ? e->block[k] : 0u);
      }
      continue;
    }
    if (!synthesize) continue;
    if (t.light_states.size() >= 0x7FFFFFF0u) {
      ++c.synth_refused;
      continue;
    }
    LightState s = {};
    std::memcpy(s.light, e->block, sizeof s.light);
    s.flags = kLsSynthetic;   // the shadow fields stay zero: d3::RecordIndex::Add skips it
    LightRef r;
    r.object = t.engine_objects[i], r.ordinal = 0, r.state = uint32_t(t.light_states.size()), r.frame = ref_frame;
    t.light_states.push_back(s);
    t.light_refs.push_back(r);
    ++c.synth, ++written;
  }
  c.last_actors = actors, c.last_with_entry = with_entry;
  return written;
}

// A record's own latch rows as a Latch: for offline replay. The character of slot K = the record's circle row (CircleShadow::character); the embedded block = the kLoNodeObject row at char + 3984.
// Slots whose block is not a node-object row (an override-path or manager object) are skipped.
inline void LatchFromRecord(const SceneTables& t, Latch& out) {
  out = Latch{};
  out.valid = true;
  // S111 CUTREACH review: the label is the hook of the latch the rows were READ at (every row carries that latch's frame f; latch f stamps hook f - 1), never one derived from the header. Record r carries latch r - 1's
  // rows (hook r - 2, the hook its own Apply asks: HookOfRecord), at the boundary (the first latch, after a skipped one) the latch's own (hook r - 1), and a record written before CUTREACH its own latch's (hook r - 1).
  const uint64_t read_at = t.light_objects.empty() ? t.header.guest_frame : t.light_objects[0].frame;
  out.hook = read_at >= 1 ? uint32_t(read_at) - 1u : 0u;
  std::map<uint32_t, uint32_t> seen;
  for (const CircleShadow& cs : t.circle_shadows) {
    if (cs.slot >= kSlots || !cs.character || seen.count(cs.slot)) continue;
    seen[cs.slot] = cs.character;
    const uint32_t src = cs.character + 3936u + 48u;
    for (const LightObjectRow& r : t.light_objects)
      if (r.kind == kLoNodeObject && r.object == src && (r.flags & kLoBlockRead)) {
        Entry e;
        e.slot = cs.slot, e.character = cs.character, e.source = src;
        std::memcpy(e.block, r.words, sizeof e.block);
        out.entries.push_back(e);
        break;
      }
  }
}

inline std::string CensusText(const Census& c, bool on) {
  char b[640];
  std::snprintf(b, sizeof b,
                "[wrestler_light %s, frames %llu, no_latch %llu, no_entry %llu, wrestler_light latch==walk %llu, differ %llu (changed frames only: latch==walk %llu, differ %llu, of %llu changed), synth %llu, refused %llu, first [%s]]",
                on ? "on" : "off", (unsigned long long)c.frames, (unsigned long long)c.no_latch, (unsigned long long)c.no_entry, (unsigned long long)c.eq, (unsigned long long)c.differ, (unsigned long long)c.eq_changed,
                (unsigned long long)c.differ_changed, (unsigned long long)c.changed, (unsigned long long)c.synth, (unsigned long long)c.synth_refused, c.first);
  return b;
}

}  // namespace rdr_scene::wlight
