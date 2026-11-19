// S103 D1: the game's colour-pass CHOICE for a stage-slot mesh (effect alternative, technique, pass), the read rule of the decompilation experiment
// (docs\agents\s103\decomp\PLAN.md; claims out\s103\decomp\claims_R1..R12 + V). The same logic as tools\harvest\choice_predict.py (frozen: 1335 / 1337 on six sealed
// arenas); every branch names the predictor's branch id (BRANCHES there: claim + instruction range). Pure header: no game memory, no files.
//
//   alternative  group-0 slot: selector = (phase word 0x836F3940 == 1) ? 0 : (slot record +10 bit 0x40 clear ? 1 : 0)       [stage_sel: R7 LM rule, sub_824E9E10 0x824EA038-0x824EA080]
//                groups 1-4 (s a i j o): the outer selector (writer [U]; default 0)                                          [sel_group: R10 class1]
//                selector 0 -> the effect's own array (+384), 1 -> the '<name>LM' twin (+388) when it resolves               [sel_388 / arr388_twin: R3, V(3)]
//   technique    the mesh record word +0x90                                                                                   [PLAN.md skeleton R0: sub_82C04DF0 0x82C05AF8]
//   pass         base (model+540: 1 for slots 390..397, or 81..88 when the arena id is in 200..249) ...                      [pass_base: R9; skinned_slot: R10 class4]
//                + 2 when rec+4 and the slot's shadow task ran (own p bit for the phase AND the gate)                         [p5ee0_shadow / shadow_p: R9, R10 class3, R11]
//
// The effect technique table (EffectTable) is read from the PLAYER's pac\_startup.pac (key 0) at run time: technique -> passes -> the vertex / pixel container's
// microcode XXH3 (tools\shaders\effect_table.py's layout, docs\formats\effect_techniques.md). Nothing from the game is stored here or committed.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rdr_scene::read_rule {

// ---- the slot record and the stage pass rules ----------------------------------------------------------------------------------
// cycled: a slot of a slot-cycle group (pac entries 60010-60025); not a record byte: the game's builder sets rec+8 0x80 at run time, the file has no such bit.
struct SlotRec { uint8_t b6 = 0, b7 = 0, b8 = 0, b9 = 0, b10 = 0; bool cycled = false; };

// [draw_group: R8 slot draw groups] sub_824E7190 0x824E71C8-0x824E72A4: -1 = no pass draws the slot (rec+8 0x80, or rec+9 0x08; rec+9 0x80 assumed set: rec9_bit80);
// 0 none of s a i j o; 1 s; 2 a; 3 i; 4 j or o.
inline int DrawGroup(const SlotRec& r) {
  if ((r.b8 & 0x80) != 0 || (r.b9 & 0x08) != 0) return -1;
  if (r.b8 & 0x20) return 1;
  if (r.b8 & 0x40) return 2;
  if (r.b10 & 0x10) return 3;
  if (r.b10 & 0x0C) return 4;
  return 0;
}

// [sel_group / stage_sel] the selector word +1184 at the slot's draw.
// [sel_cycled: S103 UV] a cycled slot is drawn by the cycle task sub_83420EA0 (sub_824E82A0 0x824E8554-0x824E8578 gave it the no-pass bit), which never sets +1184: the outer selector.
constexpr int Selector(const SlotRec& r, int group, bool phase1, int outer_selector = 0) {
  if (r.cycled) return outer_selector;
  if (group != 0) return outer_selector;      // [sel_group: R10 class1] the per-slot setter is skipped for groups 1-4 (r6 = 0 from sub_825B9970 0x825B9AB4)
  if (phase1) return 0;                       // [stage_sel: R7 LM rule] li r4,0 at 0x824EA050 for every slot when the phase word is 1
  return (r.b10 & 0x40) ? 0 : 1;              // [stage_sel: R7 / R3 selector setters] (~byte[slot_rec+10] >> 6) & 1
}

// unit case (compiled with every TU that includes this header): a cycled group-0 slot takes the outer selector, an ordinary one the stage rule.
static_assert(Selector(SlotRec{0, 0, 0, 0, 0, true}, 0, false, 0) == 0 && Selector(SlotRec{0, 0, 0, 0, 0, true}, 0, false, 1) == 1 &&
              Selector(SlotRec{}, 0, false, 0) == 1 && Selector(SlotRec{0, 0, 0, 0, 0x40, false}, 0, false, 1) == 0,
              "read_rule::Selector: sel_cycled");

// [shadow_p / shadow_gate / shadow_reset: R10 class3, R11] the slot's own rec+9 bit for the phase (pg 0x04 phase 1, pe 0x02 otherwise) AND the extra gate
// (model+600 bit 0 / no vis object / sub_82699B50(obj,0) / (r and sub_82699B50(obj,1)): `gate_pass`, the predictor's default 1); per slot, never inherited.
inline bool ShadowSlot(const SlotRec& r, int group, bool phase1, bool gate_pass = true, bool module_active = true) {
  if (!module_active || group < 0) return false;
  const uint8_t bit = phase1 ? 0x04 : 0x02;
  return (r.b9 & bit) != 0 && gate_pass;
}

// [skinned_slot: R10 class4] sub_824ED698 0x824ED6F4-0x824ED724: model+540 = 1 iff the slot is in 390..397, or 81..88 when [stage+1216] (the arena id [L]) is in 200..249.
inline int Model540(int slot, int stage_1216) {
  const bool alt = stage_1216 >= 200 && stage_1216 <= 249;
  const int lo = alt ? 81 : 390, hi = alt ? 88 : 397;
  return (slot >= lo && slot <= hi) ? 1 : 0;
}

// [rec0 .. rec20: R1 rec+N / V] sub_82BDA038: the 24-byte record of the effect behind the +384 handle, by its lowercased name.
struct RecFlags { bool rec0 = true, rec4 = false, rec8 = false, rec12 = false, rec20 = false; };
inline RecFlags RecFlagsOf(const std::string& lower) {
  static const char* const kRec0[] = {"ybg_default", "ybg_defaultlm", "ybg_defaultrdc", "ybg_bumpmap", "ybg_bumpmaplm", "ybg_bumpmaprdc", "ybg_reflect", "ybg_reflectlm", "ybg_reflectrdc"};   // 0x82BDA678-0x82BDA7A0
  static const char* const kYch[] = {"ych_skin", "ych_skin_flex", "ych_trans", "ych_trans_tiling", "ych_grar", "ych_grar_flex", "ych_gear", "ych_gear_flex", "ych_cloth", "ych_cloth_flex",
                                     "ych_anisotropic", "ych_aniso_trans", "ych_reflect", "ych_ls"};                                                                                                // 0x82BDA7E8-0x82BDAD88
  RecFlags f;
  bool in0 = false, ych = false;
  for (const char* k : kRec0) in0 = in0 || lower == k;
  for (const char* k : kYch) ych = ych || lower == k;
  f.rec0 = !in0;
  f.rec4 = in0 || ych || lower == "yprop_psm" || lower == "yprop_psmlm";
  f.rec8 = lower == "ybg_psm" || lower == "yprop_psm" || lower == "yprop_psmlm";   // 0x82BDAAB4-0x82BDAB18
  f.rec12 = ych && lower != "ych_aniso_trans";                                      // 0x82BDAB1C-0x82BDACC4
  f.rec20 = lower == "ych_aniso_trans";                                             // 0x82BDAD10-0x82BDAD48
  return f;
}

// [pass_base / p4f88_* / p5ee0_*: R9 pass value, review 1.2] the pass index. specpow_word: rec+16's target word (-1 = absent); m616 / m620 / camera_1164 0 for stage models.
inline int PassIndex(const RecFlags& r, bool shadow_on, int m540, int64_t specpow_word = -1, int m616 = 0, int m620 = 0, int camera_1164 = 0) {
  int v = m540 == 1 ? 1 : 0;                                   // [pass_base] 0x82C0585C-0x82C058AC
  if (r.rec12) {                                               // [p4f88_rec12]
    if (m616 == 1) v += 4;                                     // [p4f88_616]
    else if (m616 == 0) { if (!(specpow_word >= 0 && specpow_word < 0x80000000LL)) v += 8; }
    else if (m616 == 2) v += 8;
  }
  if (!r.rec0 && m540 == 1) v &= ~1;                           // [p5ee0_bit0]
  if (r.rec4 && shadow_on) v += 2;                             // [p5ee0_shadow]
  if (r.rec8 && camera_1164 != 0) v += 4;                      // [p5ee0_camera]
  if (r.rec20 && m620 == 1) v += 16;                           // [p5ee0_aniso]
  return v;
}

inline std::string Lower(std::string s) {
  for (char& c : s) c = char((c >= 'A' && c <= 'Z') ? c + 32 : c);
  return s;
}
// [arr388_twin: R3 model+388 / V(3)] 'L','M' written at min(len, 13): the twin's lowercased name.
inline std::string TwinLower(const std::string& lower) { return lower.substr(0, 13) + "lm"; }

// ---- the effect technique table (the player's _startup.pac) ---------------------------------------------------------------------
struct PassShaders { uint64_t vs = 0, ps = 0; bool vs_ok = false, ps_ok = false; };
struct EffectTech {
  std::string name;                               // the effect's name as the file stores it
  std::vector<std::vector<PassShaders>> techs;    // [technique][pass]; a technique the parse refused is empty
};
using EffectTable = std::unordered_map<std::string, EffectTech>;   // key: the lowercased name (the registry's resolver lowercases: sub_82BAF4D0)

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The container at body+p (tools\shaders\shaderlib.py containers()): kind 1 VS, 2 PS, 0 not a valid container; `hash` = Hash(microcode).
template <class Hash>
inline int Container(const uint8_t* d, size_t n, size_t p, Hash hash, uint64_t& h) {
  if (p + 36 > n) return 0;
  const uint32_t magic = Be32(d + p);
  if (magic != 0x102A1100u && magic != 0x102A1101u) return 0;
  const uint32_t vsize = Be32(d + p + 4), psize = Be32(d + p + 8), desc = Be32(d + p + 24);
  if (!(desc >= 28 && desc < vsize && p + desc + 8 <= n && p + uint64_t(vsize) + psize <= n)) return 0;
  const uint32_t moff = Be32(d + p + desc), msize = Be32(d + p + desc + 4);
  if (!msize || (msize & 3) || uint64_t(moff) + msize > psize) return 0;
  h = hash(d + p + vsize + moff, msize);
  return magic == 0x102A1101u ? 1 : 2;
}

// One effect blob (3-word header + body) -> techniques (tools\shaders\effect_table.py parse_effect: body+0x200 table, +0x20C count; technique +4 pass count,
// +16 + 4*pass the pass; pass +8 the block; block +72 / +76 the vertex / pixel cells; the container is the first valid one at or after the cell, within 0x20).
template <class Hash>
inline void ParseEffect(const uint8_t* blob, size_t blob_n, Hash hash, EffectTech& out) {
  if (blob_n < 12 || uint64_t(Be32(blob + 4)) + 12 > blob_n) return;
  const uint8_t* b = blob + 12;
  const size_t n = Be32(blob + 4);
  if (n < 0x210) return;
  const uint32_t tab = Be32(b + 0x200), cnt = Be32(b + 0x20C);
  if (!(cnt > 0 && cnt < 4096 && uint64_t(tab) + 4ull * cnt <= n)) return;
  const auto shader = [&](uint32_t cell, int want, uint64_t& h) -> bool {
    for (size_t p = cell; p <= size_t(cell) + 0x20 && p + 36 <= n; ++p) {
      uint64_t hh = 0;
      const int k = Container(b, n, p, hash, hh);
      if (k == 0) continue;
      if (k != want) return false;   // the first valid container is of the other kind: a problem (the predictor's None)
      h = hh;
      return true;
    }
    return false;
  };
  out.techs.assign(cnt, {});
  for (uint32_t ti = 0; ti < cnt; ++ti) {
    const uint32_t t = Be32(b + tab + 4 * ti);
    if (uint64_t(t) + 20 > n) continue;
    const uint32_t npass = Be32(b + t + 4);
    if (!(npass > 0 && npass < 64) || uint64_t(t) + 16 + 4ull * npass > n) continue;
    std::vector<PassShaders> passes;
    for (uint32_t p = 0; p < npass; ++p) {
      const uint32_t ps = Be32(b + t + 16 + 4 * p);
      if (uint64_t(ps) + 12 > n) continue;
      const uint32_t pid = Be32(b + ps + 8);
      if (uint64_t(pid) + 80 > n) continue;   // the predictor's parse skips such a pass the same way (passes are then indexed by position, as there)
      PassShaders s;
      s.vs_ok = shader(Be32(b + pid + 72), 1, s.vs);
      s.ps_ok = shader(Be32(b + pid + 76), 2, s.ps);
      passes.push_back(s);
    }
    out.techs[ti] = std::move(passes);
  }
}

// pac\_startup.pac key 0 (decoded): the effect table (tools\shaders\shaderlib.py effect_table: LE32 count at +0, 16-byte header, 32-byte entries: name16, 'obj\0', LE32 size, LE32 offset).
template <class Hash>
inline bool ParseEffectLeaf(const uint8_t* d, size_t n, Hash hash, EffectTable& out) {
  if (n < 16) return false;
  const uint32_t count = Le32(d);
  if (count == 0 || count > 100000 || 16 + 32ull * count > n) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = d + 16 + 32 * size_t(i);
    if (std::memcmp(e + 16, "obj\0", 4) != 0) return false;
    const uint32_t size = Le32(e + 20), off = Le32(e + 24);
    if (uint64_t(off) + size > n) continue;
    size_t len = 0;
    while (len < 16 && e[len]) ++len;
    EffectTech et;
    et.name.assign(reinterpret_cast<const char*>(e), len);
    ParseEffect(d + off, size, hash, et);
    out.emplace(Lower(et.name), std::move(et));   // (first one wins, as the predictor's setdefault)
  }
  return !out.empty();
}

// The process's table (built once from the player's files by live_assets.cpp's GameFilesSource::SetRoot; null = not built: the old heuristics stay in force).
// (inline: one store per process; every tool that links front_end.cpp without live_assets.cpp simply has none and keeps the old heuristics)
inline std::mutex& TableMutex() { static std::mutex mu; return mu; }
inline std::shared_ptr<const EffectTable>& TableSlot() { static std::shared_ptr<const EffectTable> t; return t; }
inline void PublishEffectTable(std::shared_ptr<const EffectTable> t) { std::lock_guard<std::mutex> l(TableMutex()); TableSlot() = std::move(t); }
inline std::shared_ptr<const EffectTable> EffectTableNow() { std::lock_guard<std::mutex> l(TableMutex()); return TableSlot(); }

}  // namespace rdr_scene::read_rule
