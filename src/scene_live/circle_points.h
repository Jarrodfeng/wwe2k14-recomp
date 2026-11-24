// S6 (S103 lane A3): the circle blob shadows' body points, read at the latch from the update-side state instead of inside the skipped walk (docs\renderer\INPUTS_A2.md 5c, 5d).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; includes only inputs_core.h (our pure header): the contributor (inputs_contrib.h) and the offline unit (tools\scene\inputs_unit.cpp) run THIS code.
//
// The rule is sub_82812E90's own, read from its listing [C generated\default, 0x82812E90-0x82813224; run 10 / T6b trace wwe2k14_078.log for the getter]:
//   * the 9 character slots at 0x836F38D0 (word per slot = the character, 0 = empty);
//   * a character is drawn only when [char+1208] != 0, [char+224] != 0 and [char+232] != 0 (0x82812FA8-0x82812FC8);
//   * its component = sub_823A6450(char) = [char+3112] when non-zero, else the node tagged 0x434D4D44 in the list at char+1152 (sub_822515B0 -> sub_82C31CB0: walk [node+20] up to 255 links comparing [node+4]
//     with the tag, then the head itself, then the [node+16] chain), the component object = [node+24];
//   * the point indices are 21 consecutive int16 at component+40 (7 steps x 3, `addi r7,r31,42` with reads at -2, 0, +2 per step, r7 += 6); an index < 0 leaves the point unset (the stack slot keeps whatever it held);
//   * point k = the 16 bytes at +48 (the translation row: x, y, z, w) of the world matrix `[[char+48]+192] + 64 * index`; [char+48] == 0 skips the point (unset).
// NOT covered, so the S6 input is NOT stamped: the same function builds ShadowParam (c0) = ([circle+16], ground height sub_82812600(char), [circle+20], [circle+24]). The circle object's address (r3 of the
// call, constant per run in T6) has no global found yet [U], and the ground height is a function of the character's position and match globals (sub_82612700 / sub_826088A8 / sub_824E6FC8) [U]. ViewProj (c4-c7)
// is the main camera's, which the record already carries. See INPUTS_A2.md 5d.
#pragma once
#include <cstdint>
#include <vector>

#include "inputs_core.h"
#include "../renderer_scene/rows_circle.h"   // S106 G4-2

namespace scene_inputs {

inline constexpr uint32_t kCircleSlotTable = 0x836F38D0, kCircleSlots = 9;
inline constexpr uint32_t kCharWorld = 48, kCharGate0 = 1208, kCharGate1 = 224, kCharGate2 = 232, kCharCompCache = 3112, kCharCompList = 1152;
inline constexpr uint32_t kCircleDefaultVec = 0x8206F2D0;   // sub_82812E90 r28 (lis -32249 / addi -3376): 16 bytes copied when the matrix address is 0
inline constexpr uint32_t kCompTag = 0x434D4D44, kCompIndices = 40, kSkelWorldArray = 192, kTranslationRow = 48;

struct CirclePoints {
  uint32_t slot = 0, character = 0, component = 0;
  uint32_t set_mask = 0;   // bit k = point k was read; the others are unset (the game leaves them stale)
  float points[rdr_scene::kCirclePoints][4] = {};
};
// S103 PMC2: the branch of ReadCircleModule that answered (PMH.md: 598 "holes" were one of these, unnamed). Index = the counter slot in the inputs census `[circle_module ...]`.
enum class CircleModuleWhy : uint32_t { Ok = 0, ModulePtr, CircleWord, ModeWords, HandleWords, ViewProjRead, ParamRead, Degenerate, Count };   // Degenerate = a non-finite module+2240 (the game's light is degenerate: its draws land nowhere), NOT unreadable
struct CircleReadInfo {
  uint32_t chars = 0, points = 0, unset = 0, empty_slots = 0, gated = 0, no_component = 0, unreadable = 0;
  CircleModuleWhy module_why = CircleModuleWhy::Ok;   // S103 PMC2: which ReadCircleModule branch answered (set by ReadCircleShadows)
  bool light_degenerate = false;                      // S103 PMC2: the module's ViewProj (module+2240) is non-finite: the game's own state at a scene start; no rows, stamp kept
};

// sub_82C31CB0: the node tagged `tag` in the list headed at `head` (0 = none). Bounded like the game's (255 links).
inline uint32_t FindByTag(ReadFn read, uint32_t head, uint32_t tag, bool& unreadable) {
  uint32_t n = head, w = 0;
  for (int i = 0; i < 255; ++i) {
    if (!ReadWord(read, n + 20, w)) return unreadable = true, 0;
    if (w == 0) break;   // end of the [node+20] chain: the head / parent chain test follows
    n = w;
    if (!ReadWord(read, n + 4, w)) return unreadable = true, 0;
    if (w == tag) return n;
    if (i == 254) return 0;   // 255 links without a hit: the game returns 0
  }
  uint32_t p = head;
  if (!ReadWord(read, p + 4, w)) return unreadable = true, 0;
  if (w == tag) return p;
  for (int i = 0; i < 255; ++i) {
    if (!ReadWord(read, p + 16, p)) return unreadable = true, 0;
    if (p == 0) return 0;
    if (!ReadWord(read, p + 4, w)) return unreadable = true, 0;
    if (w == tag) return p;
  }
  return 0;
}

// The component of a character (sub_823A6450). 0 = none.
inline uint32_t CharacterComponent(ReadFn read, uint32_t ch, bool& unreadable) {
  uint32_t cache = 0;
  if (!ReadWord(read, ch + kCharCompCache, cache)) return unreadable = true, 0;
  if (cache) return cache;   // sub_823A6450 returns [char+3112] itself when non-zero
  const uint32_t node = FindByTag(read, ch + kCharCompList, kCompTag, unreadable);
  if (!node) return 0;
  uint32_t comp = 0;
  if (!ReadWord(read, node + 24, comp)) return unreadable = true, 0;
  return comp;
}

// All characters' points for one latch. `out` is cleared first. false = the slot table itself is unreadable.
inline bool ReadCirclePoints(ReadFn read, std::vector<CirclePoints>& out, CircleReadInfo& info) {
  out.clear();
  info = CircleReadInfo{};
  for (uint32_t s = 0; s < kCircleSlots; ++s) {
    uint32_t ch = 0;
    if (!read || !ReadWord(read, kCircleSlotTable + 4 * s, ch)) return false;
    if (ch < 0x10000) {
      ++info.empty_slots;
      continue;
    }
    uint32_t g0 = 0, g1 = 0, g2 = 0, skel = 0;
    if (!ReadWord(read, ch + kCharGate0, g0) || !ReadWord(read, ch + kCharGate1, g1) || !ReadWord(read, ch + kCharGate2, g2)) {
      ++info.unreadable;
      continue;
    }
    if (!g0 || !g1 || !g2) {
      ++info.gated;
      continue;
    }
    if (!ReadWord(read, ch + kCharWorld, skel)) {   // (read after the gates: a gated character is skipped before its skeleton matters)
      ++info.unreadable;
      continue;
    }
    bool unread = false;
    const uint32_t comp = CharacterComponent(read, ch, unread);
    if (unread) {
      ++info.unreadable;
      continue;
    }
    if (!comp) {
      ++info.no_component;
      continue;
    }
    int16_t idx[rdr_scene::kCirclePoints];
    uint32_t raw[(rdr_scene::kCirclePoints * 2 + 3) / 4];   // 11 words of room, 42 bytes read (21 int16)
    if (!read(comp + kCompIndices, raw, rdr_scene::kCirclePoints * 2)) {
      ++info.unreadable;
      continue;
    }
    {   // the int16 array is big-endian in guest memory: the word read left it as raw bytes
      const uint8_t* b = reinterpret_cast<const uint8_t*>(raw);
      for (uint32_t k = 0; k < rdr_scene::kCirclePoints; ++k) idx[k] = int16_t((uint16_t(b[2 * k]) << 8) | b[2 * k + 1]);
    }
    uint32_t world = 0;
    bool have_skel = skel != 0;   // 0x82813034-0x82813038: [char+48] == 0 skips the point (unset)
    if (have_skel && !ReadWord(read, skel + kSkelWorldArray, world)) {
      ++info.unreadable;
      continue;
    }
    CirclePoints cp;
    cp.slot = s, cp.character = ch, cp.component = comp;
    for (uint32_t k = 0; k < rdr_scene::kCirclePoints; ++k) {
      if (idx[k] < 0 || !have_skel) {
        ++info.unset;   // the game leaves the stack slot as it was
        continue;
      }
      // 0x82813048-0x8281306C: the matrix address = array + 64 * index; when that sum is 0 the game copies the DEFAULT vector at 0x8206F2D0 instead (a valid value, not an error)
      const uint32_t at = world + 64u * uint32_t(idx[k]);
      uint32_t row[4];
      if (!read(at == 0 ? kCircleDefaultVec : at + kTranslationRow, row, sizeof row)) {
        ++info.unset;
        ++info.unreadable;
        continue;
      }
      for (int c = 0; c < 4; ++c) {
        const uint32_t w = Swap32(row[c]);
        std::memcpy(&cp.points[k][c], &w, 4);
      }
      cp.set_mask |= 1u << k;
      ++info.points;
    }
    ++info.chars;
    out.push_back(cp);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// ShadowParam (c0) and ViewProj (c4-c7): the rest of what sub_82812E90 uploads (S103 lane A3, bit 8; docs\renderer\INPUTS_A3.md 4). Everything below is read from the update-side state the game reads it from, at the latch.
// Sources [C listings read this session]:
//   * the shadow module: word 0x83924748 (a pointer; the destructor sub_822A7608 calls it through [obj+0] slot 0, and the constructor sub_827E93D8 stores the circle object at this+22692: so it is the constructor's `this`);
//   * the circle object = [module+22692] (sub_82812DD0 constructs it: +16, +20, +24 floats). The caller (sub_827E7338 0x827E74BC-0x827E74E8, sub_827E7F00 0x827E7FF8-0x827E8024) stores [module+264] -> [circle+20] and
//     [module+268] -> [circle+24] right before `bl sub_82813228` (the wrapper that calls sub_82812E90), and passes r4 = module+2240 = the matrix sub_82812E90 uploads TRANSPOSED as c4-c7 (lvx128 of its four rows, vmrghw/vmrglw
//     merges, 0x82812F1C-0x82812F7C). So: z = [module+264], w = [module+268], x = [circle+16] (written by the constructor; any other writer is read as it stands), ViewProj row i = column i of module+2240.
//   * y = sub_82812600(char), the ground height: GroundHeight below.
// No hook is needed: every input is a field of the module / circle object / character, read lock-free while the barrier clients are parked (the latch precondition).
inline constexpr uint32_t kShadowModulePtr = 0x83924748, kModCircle = 22692, kModZ = 264, kModW = 268, kModViewProj = 2240, kCircleX = 16;
// Static data the ground-height code reads, at the addresses the code computes (lis rX,hi ; lfs / lwz lo(rX): e.g. 0x82190000 - 18228 = 0x8218B8CC). The VALUES stay in the game's memory: they are read at the latch.
inline constexpr uint32_t kGhZero = 0x8218B8CC, kGhOne = 0x8218B8C0, kGhRingHalf = 0x8201CAE0;   // sub_82812600: f31 = 0.0, |x| and |z| < f13 selects the in-ring value; sub_82612700's (1 - t) constant
inline constexpr uint32_t kGhRingFlag = 0x83767CF0, kGhRingTable = 0x83622470;                   // sub_826088A8: f1 = float at table + (flag == 0 ? 4 : 0)
inline constexpr uint32_t kGhGateObj = 0x83763544, kGhGateOff = 23156;                           // sub_824E6FC8: [0x83763544] ? [[0x83763544] + 23156] : 1
inline constexpr uint32_t kGhModeObj = 0x8376AAF4, kGhModeOff = 64;                              // sub_827D7578 returns 0x8376AAF4; the mode id is [that + 64]
inline constexpr uint32_t kGhTabPtr = 0x836F2C34, kGhTabCount = 0x836F2C38, kGhTabStride = 108, kGhTabIdOff = 4, kGhTabClassOff = 31;   // sub_822C3440: 108-byte entries, byte +4 = id, byte +31 = the class sub_822C3838 returns
inline constexpr uint32_t kGhC0 = 0x8201D560, kGhC1 = 0x82012D90, kGhC2 = 0x82004F58, kGhC3 = 0x82020D04, kGhC4 = 0x82020D00, kGhC5 = 0x8201CF0C, kGhC6 = 0x8201CD60;   // sub_82612700's constants

inline bool ReadFloat(ReadFn read, uint32_t a, float& f) {
  uint32_t w = 0;
  if (!ReadWord(read, a, w)) return false;
  std::memcpy(&f, &w, 4);
  return std::isfinite(f);
}
// sub_822C3838(mode): the class byte of the match-type table's entry for `mode` (ids 200..249 count as 200: sub_822A7240 = sub_822AA2F8); 0 when there is no entry. false = unreadable.
inline bool MatchClass(ReadFn read, uint32_t mode, uint32_t& cls) {
  cls = 0;
  const uint32_t id = (mode >= 200 && mode <= 249) ? 200u : mode;
  if (id > 255) return true;   // the table compares a BYTE (lbz r9,4(r3); cmpw r9,r31): no entry can equal it
  uint32_t tab = 0, n = 0;
  if (!ReadWord(read, kGhTabPtr, tab) || !ReadWord(read, kGhTabCount, n) || n > 512) return false;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t w = 0;
    if (!ReadWord(read, tab + kGhTabStride * i + kGhTabIdOff, w)) return false;
    if ((w >> 24) == id) {
      uint32_t c = 0;
      if (!ReadWord(read, tab + kGhTabStride * i + (kGhTabClassOff & ~3u), c)) return false;
      cls = c & 0xFF;   // byte +31 = the low byte of the big-endian word at +28
      return true;
    }
  }
  return true;
}
// sub_82612700 (x, z): the ramp / floor height outside the ring square.
inline bool RampHeight(ReadFn read, float x, float z, float& out) {
  float zero = 0, one = 0;
  if (!ReadFloat(read, kGhZero, zero) || !ReadFloat(read, kGhOne, one)) return false;
  uint32_t gate = 1, hold = 0, mode = 0, cls = 0;
  if (!ReadWord(read, kGhGateObj, hold)) return false;
  if (hold && !ReadWord(read, hold + kGhGateOff, gate)) return false;
  if (gate == 0) return out = zero, true;   // 0x82612720: sub_824E6FC8() == 0 -> 0
  if (!ReadWord(read, kGhModeObj + kGhModeOff, mode) || !MatchClass(read, mode, cls)) return false;
  float f31, f30, f29, c6;
  if (cls == 2) {
    if (!ReadFloat(read, kGhC3, f31) || !ReadFloat(read, kGhC4, f30) || !ReadFloat(read, kGhC5, f29)) return false;
  } else {
    if (!ReadFloat(read, kGhC0, f31) || !ReadFloat(read, kGhC1, f30) || !ReadFloat(read, kGhC2, f29)) return false;
  }
  if (!(z < f31)) return out = f29, true;    // 0x8261276C: z >= f31 -> f29
  if (!(z > f30)) return out = zero, true;   // 0x8261277C: z <= f30 -> 0
  if (!ReadFloat(read, kGhC6, c6)) return false;
  const float ax = x < zero ? -x : x;
  bool interp = !(ax > c6);                  // 0x826127A8 ble -> the ramp formula
  if (!interp) {
    uint32_t m2 = 0;
    if (!ReadWord(read, kGhModeObj + kGhModeOff, m2)) return false;
    interp = (m2 == 54 || m2 == 55 || m2 == 15 || m2 == 56);   // 0x826127B8-0x826127D4
  }
  if (!interp) return out = zero, true;      // 0x826127D8
  out = (one - (z - f31) / (f30 - f31)) * f29;   // 0x826127E0-0x826127F8
  return std::isfinite(out);
}
// sub_82812600(char): the ground height the circle blob sits on.
inline bool GroundHeight(ReadFn read, uint32_t ch, float& out) {
  float x = 0, z = 0, zero = 0, half = 0;
  if (!ReadFloat(read, ch + 160, x) || !ReadFloat(read, ch + 168, z) || !ReadFloat(read, kGhZero, zero) || !ReadFloat(read, kGhRingHalf, half)) return false;
  const float ax = x < zero ? -x : x, az = z >= zero ? z : -z;
  if (ax < half && az < half) {   // 0x82812654-0x82812660: inside the ring square -> sub_826088A8
    uint32_t flag = 0;
    if (!ReadWord(read, kGhRingFlag, flag)) return false;
    return ReadFloat(read, kGhRingTable + (flag == 0 ? 4u : 0u), out);
  }
  uint32_t hold = 0, gate = 1;
  if (!ReadWord(read, kGhGateObj, hold)) return false;
  if (hold && !ReadWord(read, hold + kGhGateOff, gate)) return false;
  if (gate == 0) return out = zero, true;   // 0x82812684: f1 = f31
  return RampHeight(read, x, z, out);
}

struct CircleModuleRead {
  float view_proj[16] = {};
  float xzw[3] = {};   // [circle+16], [module+264], [module+268]
  bool active = false; // true only when the game WOULD draw: both render-entry off tests pass and both of the wrapper's handle lookups resolve (audit item 3, bit-8 review 3)
};
enum class CircleModuleState { Ok, NothingToDraw, Unreadable };
// sub_82756710 / sub_82756780, read-only: a handle (the word at circle+8 / circle+28) resolves through the table at [global]: begin = [P+8], end = [P+16], 8-byte entries, index = handle & 0xFFFF,
// valid when the entry's first word equals the handle, result = the entry's second word; anything else gives 0 (the game then stores 0 at circle+12 / circle+32 and sub_82813228 draws nothing).
// Globals: 0x83930000 - 25960 = 0x83929A98 (sub_82756710) and 0x83930000 - 5740 = 0x8392E994 (sub_82756780).
inline constexpr uint32_t kHandleTab1 = 0x83929A98, kHandleTab2 = 0x8392E994;
inline bool ResolveHandle(ReadFn read, uint32_t global, uint32_t handle, uint32_t& out) {
  out = 0;
  uint32_t p = 0, begin = 0, end = 0;
  if (!ReadWord(read, global, p) || p < 0x10000 || !ReadWord(read, p + 8, begin) || !ReadWord(read, p + 16, end)) return false;
  const int32_t count = int32_t(end - begin) >> 3;
  const uint32_t idx = handle & 0xFFFFu;
  if (!(idx < uint32_t(count))) return true;   // cmplw idx, count; bge -> 0
  uint32_t first = 0, val = 0;
  if (!ReadWord(read, begin + 8 * idx, first) || !ReadWord(read, begin + 8 * idx + 4, val)) return false;
  if (first == handle) out = val;
  return true;
}
// The module-level part (the same for every character of a call). NothingToDraw = the game draws no circle shadow this frame (a legitimate state, stamped with no rows); Unreadable = we could not tell.
inline CircleModuleState ReadCircleModule(ReadFn read, CircleModuleRead& out, CircleModuleWhy* why = nullptr) {
  out = CircleModuleRead{};
  CircleModuleWhy dummy;
  CircleModuleWhy& wy = why ? *why : dummy;   // S103 PMC2: the branch that answered
  wy = CircleModuleWhy::Ok;
  uint32_t mod = 0, circle = 0;
  if (!read || !ReadWord(read, kShadowModulePtr, mod)) return wy = CircleModuleWhy::ModulePtr, CircleModuleState::Unreadable;
  if (mod < 0x10000) return CircleModuleState::NothingToDraw;   // no shadow module (a scene without one)
  if (!ReadWord(read, mod + kModCircle, circle)) return wy = CircleModuleWhy::CircleWord, CircleModuleState::Unreadable;
  if (circle < 0x10000) return CircleModuleState::NothingToDraw;   // no circle object
  uint32_t mode = 0, node_word = 0;
  if (!ReadWord(read, mod + 288, mode) || !ReadWord(read, mod + 22696, node_word)) return wy = CircleModuleWhy::ModeWords, CircleModuleState::Unreadable;
  // the render entry's two off tests (own_node_hook.h: sub_827E9740, r3 = module + 4): the word at module+22696 equal to 0, or the word at module+288 equal to 2 -> it returns without drawing, and the fit
  // task then does not refresh module+2240 either (it would be stale)
  if (mode == 2 || node_word == 0) return CircleModuleState::NothingToDraw;
  // sub_82813228 (0x82813240-0x82813280): it draws only when both handles resolve to non-zero
  uint32_t h1 = 0, h2 = 0, r1 = 0, r2 = 0;
  if (!ReadWord(read, circle + 8, h1) || !ReadWord(read, circle + 28, h2) || !ResolveHandle(read, kHandleTab1, h1, r1) || !ResolveHandle(read, kHandleTab2, h2, r2)) return wy = CircleModuleWhy::HandleWords, CircleModuleState::Unreadable;
  if (r1 == 0 || r2 == 0) return CircleModuleState::NothingToDraw;
  uint32_t m[16];
  if (!read(mod + kModViewProj, m, sizeof m)) return wy = CircleModuleWhy::ViewProjRead, CircleModuleState::Unreadable;
  float f[16];
  for (int i = 0; i < 16; ++i) {
    const uint32_t w = Swap32(m[i]);
    std::memcpy(&f[i], &w, 4);
    if (!std::isfinite(f[i])) return wy = CircleModuleWhy::Degenerate, CircleModuleState::NothingToDraw;   // S103 PMC2: module+2240 (Trapezoidal x ViewProj of light 1) non-finite = the game's light is degenerate; its props pass draws land nowhere (PMH.md): the game's state, not 'we could not tell'
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out.view_proj[r * 4 + c] = f[c * 4 + r];   // uploaded row r = memory column r
  if (!ReadFloat(read, circle + kCircleX, out.xzw[0]) || !ReadFloat(read, mod + kModZ, out.xzw[1]) || !ReadFloat(read, mod + kModW, out.xzw[2])) return wy = CircleModuleWhy::ParamRead, CircleModuleState::Unreadable;
  out.active = true;
  return CircleModuleState::Ok;
}
// Full CircleShadow rows (what the walk's tracker records) for every drawn character: points + c0 + c4-c7. A character whose ground height cannot be computed is counted `no_height` and gets no row.
// Returns false when the slot table is unreadable; `module_ok` false = the module part could not be read (no rows).
inline bool ReadCircleShadows(ReadFn read, uint64_t hook_frame, std::vector<rdr_scene::CircleShadow>& out, CircleReadInfo& info, uint32_t& no_height, bool& module_ok) {
  std::vector<CirclePoints> cps;
  out.clear();
  no_height = 0;
  module_ok = false;
  if (!ReadCirclePoints(read, cps, info)) return false;
  CircleModuleRead mod;
  const CircleModuleState st = ReadCircleModule(read, mod, &info.module_why);
  info.light_degenerate = info.module_why == CircleModuleWhy::Degenerate;   // S103 PMC2
  if (st == CircleModuleState::Unreadable) return true;   // we could not tell: no rows, no stamp (module_ok false)
  module_ok = true;
  if (st == CircleModuleState::NothingToDraw) return true;   // the game draws no circle shadow this frame: no rows, and the input is still valid
  for (const CirclePoints& cp : cps) {
    float y = 0;
    if (!GroundHeight(read, cp.character, y)) {
      ++no_height;
      continue;
    }
    rdr_scene::CircleShadow e;
    e.frame = uint32_t(hook_frame);
    e.character = cp.character;
    e.slot = cp.slot;
    e.flags = rdr_scene::kCircleHaveViewProj | rdr_scene::kCircleHaveParam | rdr_scene::kCircleFromLatch;
    e.shadow_param[0] = mod.xzw[0], e.shadow_param[1] = y, e.shadow_param[2] = mod.xzw[1], e.shadow_param[3] = mod.xzw[2];
    std::memcpy(e.view_proj, mod.view_proj, sizeof e.view_proj);
    for (uint32_t k = 0; k < rdr_scene::kCirclePoints; ++k)
      for (int c = 0; c < 4; ++c) e.points[k][c] = (cp.set_mask & (1u << k)) ? cp.points[k][c] : 0.f;
    out.push_back(e);
  }
  return true;
}

}  // namespace scene_inputs
