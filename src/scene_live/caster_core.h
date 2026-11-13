// The caster passes' per-entry record: the pure rules (Session 101, lane 202 = Caster D3 phase 1 pieces E0-E2, INERT; docs\renderer\SKIP_GAME_WORK_MAP.md 17.3 / 17.7).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; NO game, SDK or asset includes: the tracker (caster_hook.h), the contributor (caster_contrib.h) and the offline test
// (tools\caster\caster_rows_test.cpp) all run THESE functions.
//
// INERT: nothing here changes what the game does. kCasterSkipBuilt is false (no skipping is built); the switch --wwe2k14_rdr_caster_d3 only turns the record on (every mode is the verify / census arm).
// Marks: [C] read in a listing (cited), [L] a reading, [U] unknown.
//
//  * The three passes. sub_82C01FE0 (T_ShadowMap: the props map, wrestler map A, map B), sub_82BF22C8 (T_PlaneShadowMap), sub_82BF9878 (T_RimLightMap). Each is a model-class method with no direct
//    callers (vtable slots only, 17.2 item 1). The first two instructions after the prologue of the map and rim pass store r4 at 6044(r1) / 5980(r1) [C listing 0x82C01FF4, 0x82BF988C]; the map pass
//    keeps r5 in r30 (0x82C02008) and the model in r24 / r26. r3 = the model, r4 = the pass key, r5 = the palette pointer or 0 (0 = [model+200]); the plane twin (0x82BF22C8) stores no r4 and
//    reads no r5 in its first instructions [C listing 0x82BF22C8-0x82BF2308: only r3 is used until the cached-list twin returns]. [U]: whether the plane pass's later body reads r4 / r5.
//  * The camera. R, the calling thread's render camera record, read exactly as the colour pass's light state reads it (light_state_hook.h: G = word 0x8392DDA4, R = [G+4] + 2208 * slot, slot from
//    [[r13]+1048]+32 or 0). Whether R AT THE ENTRY carries the A / B camera (not only the colour pass's) is [U] (17.2 item 2): the census counts how many entry states equal a colour-call state of the same frame.
//  * The map-A collapse flag. sub_82BA7808 returns the word at 0x83929A50 + 4 * slot (listing 0x82BA7808-0x82BA7834: lwz r11,0(r13); lwz r11,1048(r11); lwz r10,32(r11) or 0; lis r11,-31853;
//    addi r9,r11,-26032; lwzx r3,r10*4,r9) [C]. It is set by sub_82BA77D8(1) around map A only (17.5) [C from the listing of sub_827E8448, not re-read here]: the entry records it.
//  * The map-A inputs. [model+408..416] (listing 0x82C03CE8-0x82C03DA8) are recorded for the map pass; their meaning is E4's.
//  * The palette. r5, or [model+200] when r5 == 0; the node count is the wrestler contributor's WantCount(model) (the largest count the update hook saw); a model it has not seen (props, pieces) has
//    no palette here (kCcNoPalette, counted apart, not a failure). The rows themselves are NOT copied: the record carries a hash of the node matrices as read at the entry, and the palette
//    source of E3 is the wrestler record's own bones for the object (the colour pass's copy); the census compares the two (`pal_equal`).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../renderer_scene/light_state_rows.h"   // BuildCasterRows (lane 221 CrossCheckExtras)
#include "../renderer_scene/scene_record.h"
#include "light_state_hook.h"   // kGlobalWord, kRecordSize, kUsed*, kKeyBytes, HashKey, DecodeRecord, SameState

namespace rdr_scene::live::casters {

inline constexpr bool kCasterSkipBuilt = false;          // lane 202: no skipping is built (SKIP_GAME_WORK_MAP.md 17.7); the record is observed only
inline constexpr uint32_t kCollapseTable = 0x83929A50;   // sub_82BA7808: the collapse flag of slot s is the word at kCollapseTable + 4 * s [C listing 0x82BA7808-0x82BA7834]
inline constexpr uint32_t kPlaneNodeWindow = 96;   // lane 226 (E5): palette nodes a plane entry copies from [model+200] (= kCasterMaxNodes)
inline constexpr uint32_t kPaletteOffset = 200;          // [model+200] = the model's own palette (r5 == 0 means this)
inline constexpr uint32_t kMapAOffset = 408;             // [model+408], [+412], [+416]: the map-A collapse inputs [C listing 0x82C03CE8-0x82C03DA8]

struct PassInfo {
  uint32_t fn;           // the pass method
  uint8_t pass;          // CasterPass
  const char* name;
};
inline constexpr PassInfo kPasses[] = {
    {0x82C01FE0, kCpMap, "map"},    // T_ShadowMap
    {0x82BF22C8, kCpPlane, "plane"},  // T_PlaneShadowMap
    {0x82BF9878, kCpRim, "rim"},    // T_RimLightMap
};
inline const char* PassName(uint8_t p) {
  static const char* const k[kCpCount] = {"none", "map", "plane", "rim"};
  return p < kCpCount ? k[p] : "?";
}
// The pass a stream draw's pass site belongs to (shadow_roles.h: 0x82C04634 inside sub_82C01FE0, 0x82BF4D38 inside sub_82BF22C8, 0x82BFBD58 inside sub_82BF9878).
inline uint8_t PassOfSite(uint32_t pass_site) {
  switch (pass_site) {
    case 0x82C04634: return kCpMap;
    case 0x82BF4D38: return kCpPlane;
    case 0x82BFBD58: return kCpRim;
    default: return kCpNone;
  }
}

inline constexpr uint32_t kMaxCalls = 1024;     // CasterCall entries a frame (~60-150 expected [L]); one that does not fit is counted `overflow`
inline constexpr uint32_t kMaxStates = 64;      // camera states a frame (the captures show <= 8 colour-call states [C]; entry states are [U])
inline constexpr uint32_t kRing = 4;            // closed frames kept for Emit

// R as read at an entry: the raw bytes of the used ranges, the dedupe key, the slot.
struct EntryR {
  uint8_t raw[lights::kRecordSize];
  uint8_t key[lights::kKeyBytes];
  uint32_t slot = 0, wbits = 0, hbits = 0;
  bool null_block = false, have_map = false;
};
enum class RStatus : uint8_t { Ok, Unreadable, BadSlot };
// The read exactly as light_state_hook.h OnModelCall performs it (kept in step on purpose: a caster entry's R and a colour call's R are the same record).
template <class ReadWordFn, class ReadBytesFn>
inline RStatus ReadEntryR(ReadWordFn read_word, ReadBytesFn read_bytes, uint32_t r13, EntryR& e) {
  uint32_t g = 0, arr = 0, tls = 0, blk = 0, idx = 0;
  e.null_block = false;
  if (!read_word(lights::kGlobalWord, g) || g == 0 || !read_word(g + 4, arr) || arr == 0 || !read_word(r13, tls) || tls == 0 || !read_word(tls + 1048, blk)) return RStatus::Unreadable;
  if (blk == 0) idx = 0, e.null_block = true;   // sub_82BCBFD8 / sub_82BA7808: no recording-list block: slot 0
  else if (!read_word(blk + 32, idx)) return RStatus::Unreadable;
  if (idx >= lights::kMaxSlot) return RStatus::BadSlot;
  if (!read_bytes(arr + idx * lights::kRecordSize, e.raw, lights::kRecordSize)) return RStatus::Unreadable;
  e.have_map = read_word(g + 208, e.wbits) && read_word(g + 212, e.hbits);
  if (!e.have_map) e.wbits = e.hbits = 0;
  e.slot = idx;
  std::memcpy(e.key, e.raw, lights::kUsedA);
  std::memcpy(e.key + lights::kUsedA, e.raw + lights::kUsedB0, lights::kUsedB);
  std::memcpy(e.key + lights::kUsedA + lights::kUsedB, &e.wbits, 4);
  std::memcpy(e.key + lights::kUsedA + lights::kUsedB + 4, &e.hbits, 4);
  return RStatus::Ok;
}
// lane 221 (SKIP_GAME_WORK_MAP.md 17.10): the thread slot of the calling thread, exactly as sub_82BA7718 / sub_82BA7748 / sub_82BA7808 compute it ([[r13]+1048] null: slot 0). False = unreadable.
template <class ReadWordFn>
inline bool ReadThreadSlot(ReadWordFn read_word, uint32_t r13, uint32_t& slot) {
  uint32_t tls = 0, blk = 0;
  if (!read_word(r13, tls) || tls == 0 || !read_word(tls + 1048, blk)) return false;
  slot = 0;
  if (blk != 0 && !read_word(blk + 32, slot)) return false;
  return slot < lights::kMaxSlot;
}
inline constexpr uint32_t kProductSlots = 2;   // 0x839299C0 + 64 * s for s < 2: the camera pointer table starts at 0x83929A40 (sub_82BA7748), so a slot of 2 or more is not a product [C listing 0x82BA7748-0x82BA7774]
inline constexpr uint32_t kDepthOffset0 = 300, kDepthOffset1 = 304;   // G + 300 / G + 304: RimLightDepth c162 = (float, float, 0, 0) [C listing 0x82BD8B68-0x82BD8B88, G = word 0x8392DDA4]
inline constexpr uint32_t kPoseArrayOffset = 192, kPoseRowOffset = 48;   // [model+192] = the pose array (64-byte matrices), row 3 at +48 [C listing 0x82C03D94-0x82C03DDC]
// A big-endian guest vec4 (raw bytes) to host floats.
inline void BeFloat4(const uint8_t* raw, float out[4]) {
  for (int i = 0; i < 4; ++i) {
    const uint32_t w = (uint32_t(raw[4 * i]) << 24) | (uint32_t(raw[4 * i + 1]) << 16) | (uint32_t(raw[4 * i + 2]) << 8) | raw[4 * i + 3];
    std::memcpy(&out[i], &w, 4);
  }
}
// The map-A collapse flag of a slot: false = the word is unreadable.
template <class ReadWordFn>
inline bool ReadCollapse(ReadWordFn read_word, uint32_t slot, uint32_t& flag) { return read_word(kCollapseTable + 4 * slot, flag); }

// FNV-1a 64 over the node matrices (host floats' bytes), n nodes; 0 for n == 0 (and never 0 for n > 0: a zero hash is mapped to 1).
inline uint64_t HashPalette(const Mat4* m, uint32_t n) {
  if (n == 0) return 0;
  const uint64_t h = lights::HashKey(reinterpret_cast<const uint8_t*>(m), size_t(n) * sizeof(Mat4));
  return h ? h : 1;
}

// lane 217 (SKIP_GAME_WORK_MAP.md 17.9): the per-node hash. FNV-1a 32 of one palette matrix's 64 bytes (host floats' bytes), the same bytes HashPalette hashes. A record carries one per node up to
// kCasterMaxNodes (kScCasterNodes), so ONE verify run says which nodes of the entry's palette differ from the record's bones (the whole-palette hash of run 7138 equalled no object's: cause [U]).
inline uint32_t HashNode(const Mat4& m) {
  uint32_t h = 2166136261u;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&m);
  for (size_t i = 0; i < sizeof(Mat4); ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}
inline void HashNodes(const Mat4* m, uint32_t n, std::vector<uint32_t>& out) {
  for (uint32_t i = 0; i < n; ++i) out.push_back(HashNode(m[i]));
}
inline constexpr uint32_t kNodeBlock = 8;                                    // the census groups differing nodes in blocks of this many
inline constexpr uint32_t kNodeBlocks = kCasterMaxNodes / kNodeBlock;        // 12
inline constexpr uint32_t kBonesHeaderForStamp = 0;                          // the bones of a call stamped S are in the record whose HEADER frame is S + this (17.8b SF1: the record g-1 for stream frame g)

// The bones of one record, hashed per node, by engine object: the stash the next records' calls are compared with (the bones are NOT in the record that carries the call, 17.8b).
struct BoneStash {
  uint64_t frame = 0;   // the record's header frame
  bool valid = false;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> objs;   // engine object -> node hashes (the first min(bones, kCasterMaxNodes) bones)
};
inline void StashBones(const SceneTables& t, uint64_t header_frame, BoneStash& s) {
  s.frame = header_frame, s.valid = true;
  size_t used = 0;   // the slots (and their vectors' capacity) are reused from record to record
  for (size_t k = 0; k < t.engine_objects.size() && k < t.objects.size(); ++k) {
    const Object& o = t.objects[k];
    if (o.bones_count == 0 || uint64_t(o.bones_first) + o.bones_count > t.bones.size()) continue;
    if (used == s.objs.size()) s.objs.emplace_back();
    auto& slot = s.objs[used++];
    slot.first = t.engine_objects[k];
    slot.second.clear();
    HashNodes(&t.bones[o.bones_first], std::min<uint32_t>(o.bones_count, kCasterMaxNodes), slot.second);
  }
  s.objs.resize(used);
}

// The census join between the caster calls of one frame and the record's other tables (called by the contributor after the frame's tables are in the record).
struct Cross {
  uint64_t calls = 0;
  uint64_t cam_eq_colour = 0;     // the entry's shadow matrices equal a colour-call light state of the same frame (R at the entry = the colour pass's R)
  uint64_t cam_differ = 0;        // ... equal none (the entry reads another camera: the A / B camera, [U] 17.2 item 2)
  uint64_t cam_no_refs = 0;       // the frame has no light refs to compare with
  uint64_t pal_equal = 0;         // the palette hash equals the hash of the wrestler record's bones of the same engine object (the palette source of E3)
  uint64_t pal_differ = 0;        // the object is in the record, the hashes differ
  uint64_t pal_not_in_record = 0; // the object is not a record object (props, pieces)
  uint64_t pal_other_frame = 0;   // the record's frame is not the call's frame + 1: not compared
  uint64_t pal_none = 0;          // the call has no palette hash (n_nodes 0)
  // lane 217: the per-node compare of the entry's palette with the record bones of the header frame == the call's stamp (17.8b), per node.
  uint64_t node_calls = 0;        // calls with per-node hashes
  uint64_t node_no_bones = 0;     // ... whose bones record (header == stamp) is not in the stash
  uint64_t node_not_in_record = 0;   // ... whose object is not a record object (props, pieces)
  uint64_t node_calls_equal = 0;  // ... every compared node equal
  uint64_t node_calls_differ = 0; // ... at least one node differs
  uint64_t node_short = 0;        // ... the record has fewer bones than the call has nodes: the common prefix is compared, the rest counted here per call
  uint64_t node_differ[kCasterMaxNodes] = {};   // per node index: calls in which that node's hash differs
  uint32_t node_first_differ_min = 0xFFFFFFFFu;  // the lowest node index that ever differed
  // lane 221: the entry's camera product (the thread slot's current camera) against the camera rows built from the entry's R, per pass: the live check that the product table IS what feeds c56-59 (maps: equal
  // expected; the rim: different expected, that is why it needs the product).
  uint64_t prod_eq_r[kCpCount] = {}, prod_differ_r[kCpCount] = {}, prod_missing = 0;
  uint64_t depth_calls = 0, pose_calls = 0;
};
inline std::string NodeBlocksText(const Cross& x) {   // "b0-7:0 b8-15:0 ..." for the census line (all blocks, so a zero is visible)
  std::string s;
  for (uint32_t b = 0; b < kNodeBlocks; ++b) {
    uint64_t sum = 0;
    for (uint32_t k = 0; k < kNodeBlock; ++k) sum += x.node_differ[b * kNodeBlock + k];
    s += (b ? " " : "") + std::to_string(b * kNodeBlock) + "-" + std::to_string(b * kNodeBlock + kNodeBlock - 1) + ":" + std::to_string(sum);
  }
  return s;
}
// The per-node compare. `stash` = the recent records' bone hashes (any order); calls stamped `hook_frame` look for the stash whose frame == hook_frame + kBonesHeaderForStamp.
inline void CrossCheckNodes(const SceneTables& t, size_t first_call, uint32_t hook_frame, const BoneStash* stash, size_t n_stash, Cross& x) {
  const BoneStash* bs = nullptr;
  for (size_t i = 0; i < n_stash; ++i)
    if (stash[i].valid && stash[i].frame == uint64_t(hook_frame) + kBonesHeaderForStamp) bs = &stash[i];
  for (size_t i = first_call; i < t.caster_calls.size(); ++i) {
    const CasterCall& c = t.caster_calls[i];
    if (c.frame != hook_frame || !(c.flags & kCcNodeHashes) || c.n_nodes == 0 || uint64_t(c.node_first) + c.n_nodes > t.caster_nodes.size()) continue;
    ++x.node_calls;
    if (!bs) { ++x.node_no_bones; continue; }
    const std::vector<uint32_t>* rec = nullptr;
    for (const auto& kv : bs->objs)
      if (kv.first == c.object) { rec = &kv.second; break; }
    if (!rec) { ++x.node_not_in_record; continue; }
    const uint32_t n = std::min<uint32_t>(c.n_nodes, uint32_t(rec->size()));
    if (n < c.n_nodes) ++x.node_short;
    bool any = false;
    for (uint32_t k = 0; k < n; ++k)
      if (t.caster_nodes[c.node_first + k] != (*rec)[k]) {
        ++x.node_differ[k];
        any = true;
        if (k < x.node_first_differ_min) x.node_first_differ_min = k;
      }
    ++(any ? x.node_calls_differ : x.node_calls_equal);
  }
}


// lane 221: the per-call census of the CasterExtra table (see Cross::prod_eq_r).
inline void CrossCheckExtras(const SceneTables& t, size_t first_call, uint32_t hook_frame, Cross& x) {
  for (size_t i = first_call; i < t.caster_calls.size() && i < t.caster_extras.size(); ++i) {
    const CasterCall& c = t.caster_calls[i];
    if (c.frame != hook_frame) continue;
    const CasterExtra& e = t.caster_extras[i];
    x.depth_calls += (e.flags & kCxDepth) != 0;
    x.pose_calls += (e.flags & kCxPose) != 0;
    if (!(e.flags & kCxProduct) || c.state == kCasterNoState || c.state >= t.caster_states.size()) { ++x.prod_missing; continue; }
    const CasterRows r = BuildCasterRows(t.caster_states[c.state]);
    bool eq = true;
    for (int a = 0; a < 4 && eq; ++a)
      for (int k = 0; k < 4; ++k)
        if (r.view_proj[a][k] != e.product[k * 4 + a]) { eq = false; break; }
    ++(eq ? x.prod_eq_r : x.prod_differ_r)[c.pass < kCpCount ? c.pass : 0];
  }
}

// `frame_offset` = how many frames the record's header runs ahead of the hook's stamp (the +1 of lane 152). Bones are compared as host floats' bytes.
inline void CrossCheckCalls(const SceneTables& t, size_t first_call, uint32_t hook_frame, uint64_t record_frame, uint32_t frame_offset, Cross& x) {
  std::vector<const LightState*> colour;   // the colour-call states of this hook frame
  for (const LightRef& r : t.light_refs)
    if (r.frame == hook_frame && r.state < t.light_states.size()) colour.push_back(&t.light_states[r.state]);
  for (size_t i = first_call; i < t.caster_calls.size(); ++i) {
    const CasterCall& c = t.caster_calls[i];
    if (c.frame != hook_frame) continue;
    ++x.calls;
    if (c.state != kCasterNoState && c.state < t.caster_states.size()) {
      if (colour.empty()) {
        ++x.cam_no_refs;
      } else {
        bool eq = false;
        for (const LightState* s : colour)
          if (std::memcmp(s->shadow_m, t.caster_states[c.state].shadow_m, sizeof s->shadow_m) == 0) { eq = true; break; }
        ++(eq ? x.cam_eq_colour : x.cam_differ);
      }
    }
    // INFORMATIONAL ONLY (lane 211, SKIP_GAME_WORK_MAP.md 17.8b): this pairing (the record that carries the call, header == stamp + 1) is the one run 7138 showed WRONG for the bones (they are in the record whose
    // header equals the call's stamp), and the entry hash also fails against that record's first 96 bones (0 of 30 calls), cause [U]. pal_equal 0 / pal_differ > 0 is therefore no failure; the palette is proven
    // by the binder's proof compare and tools\caster\caster_zip_probe.py until a per-node hash lands in CasterCall.
    if (c.pal_hash == 0 || (c.flags & kCcMats)) { ++x.pal_none; continue; }   // lane 226: a plane entry's matrices are in kScCasterMats, not a wrestler's palette
    if (record_frame != uint64_t(hook_frame) + frame_offset) { ++x.pal_other_frame; continue; }
    bool found = false;
    for (size_t k = 0; k < t.engine_objects.size() && k < t.objects.size(); ++k) {
      if (t.engine_objects[k] != c.object) continue;
      const Object& o = t.objects[k];
      if (o.bones_count == 0 || uint64_t(o.bones_first) + o.bones_count > t.bones.size()) continue;
      found = true;
      const uint32_t n = std::min<uint32_t>(o.bones_count, kCasterMaxNodes);
      const uint32_t cn = std::min<uint32_t>(c.n_nodes, kCasterMaxNodes);
      ++((n == cn && HashPalette(&t.bones[o.bones_first], n) == c.pal_hash) ? x.pal_equal : x.pal_differ);
      break;
    }
    if (!found) ++x.pal_not_in_record;
  }
}

}  // namespace rdr_scene::live::casters
