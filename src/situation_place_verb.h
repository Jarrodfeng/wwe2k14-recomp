// wwe2k14 - the situation director's `place SLOT X Z [FACING_DEG]` verb (S110 PLACE, docs/research/s110/PLACE.md sections 1-3 and "Sites the build will touch").
//
//   place SLOT X Z [FACING_DEG]   DEV BUILDS ONLY. Puts the LIVE wrestler SLOT at (X, Z) with the game's own functions and leaves him in the state he is in
//                                 (nothing queued, no motion changed). FACING_DEG (-360..360) is converted to radians; omitted = the facing is kept.
//                                 The facing convention is the game's own (sub_822FB578 stores char+148): the angle the game's readers (sub_82C32810) compute from a
//                                 point A toward a point B is atan2(-(Bz - Az), Bx - Ax) - pi/2, so facing from (520, 525) toward (636, 640.5) is 225 degrees.
//
// A wrestler's position lives in TWO copies the game keeps equal every frame: char+160 / +164 / +168 (+148 facing) and the motion controller [char+3096]
// (+48 / +56 position, +32 angle). A bare char+160 write is replaced by the controller's old position on the next frame (sub_823EA6E0 +0x178..+0x180), so the
// write goes through the game's own all-mirror warp sub_829C0EE8(char, f1 = x, f2 = y, f3 = z) (char+160/164/168, the old position +192/+200, the skeleton root
// [char+176], the controller +48/+56, the CMMD component, the CHPH reset word, +1016 / +1017). The place byte and the floor height come from the game's own
// sub_82612E88(char+160) and sub_82612BF8(char, 255), exactly as the match-start placement sub_8273B790 takes them.
//
// NOT a stand-alone header: situation_director_core.h includes it INSIDE namespace wwe2k14::situation, after SlotChar / Hex / Fn are declared, so it opens
// no namespace of its own. Pure: no game include (the unit test compiles it with the core).
#pragma once

constexpr uint32_t kPosOff = 160;          // char+160 / +164 / +168 = x, y, z (+172 = w); the Lua natives and sub_8277CA90 read and write exactly these
constexpr uint32_t kYOff = 164;
constexpr uint32_t kZOff = 168;
constexpr uint32_t kYawOff = 148;          // char+148 = the facing (radians); char+144..+159 is the rotation vector
constexpr uint32_t kRotOff = 144;
constexpr uint32_t kOldRotOff = 208;       // char+208..+223 = the previous rotation (sub_8264FAB0 copies +144 -> +208 after a placement)
constexpr uint32_t kPlaceOff = 70;         // char+70 (u8) = the place byte (yChar.GetPlace = sub_8277CA30)
constexpr uint32_t kKindOff = 24;          // [char+24] == 4: a wrestler (the controller at +3096 is the wrestler kind's, sub_823B45C8)
constexpr uint32_t kWrestlerKind = 4;
constexpr uint32_t kCtrlOff = 3096;        // char+3096 = the motion controller
constexpr uint32_t kCtrlAngOff = 32;       // ctrl+32 = the angle (sub_8273B790 +0x298 stores char+148 there; sub_82410AB8 repeats it every frame)
constexpr uint32_t kCtrlPosOff = 48;       // ctrl+48 / +56 = x / z (written by the warp)
constexpr uint32_t kPartnerOff = 836;      // char+836 = the partner of a two-object motion (the root applier and sub_823B77F8 sync the pair through it)
constexpr uint32_t kActorTable = 0x836F4B40;  // word 0x836F4B40 + 4 * [char+1340] != 0: a cutscene actor (OMGROUTE G5, sub_823CB3F0)
constexpr uint32_t kActorIdxOff = 1340;
constexpr int32_t kActorCount = 72;        // sub_823CB3F0: index > -1 and < 72, else 0 (no actor)
constexpr uint32_t kSpecialOff = 1920;     // char+1920 = 1 on both wrestlers of a two-wrestler special in progress (no +836 is set)
constexpr uint32_t kRootOff = 176;         // [char+176] -> the skeleton root node's translation (x at +0, z at +8)

inline uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

template <class E>
std::string PlaceWrestler(E& e, int slot, float x, float z, bool has_facing, float deg) {
  char tb[128];
  if (has_facing) std::snprintf(tb, sizeof tb, "place %d %.2f %.2f %.2f", slot, double(x), double(z), double(deg));
  else std::snprintf(tb, sizeof tb, "place %d %.2f %.2f", slot, double(x), double(z));
  const std::string tag = tb;
  auto refuse = [&](const std::string& why) {
    e.Log("director: " + tag + ": refused (" + why + ") clock " + e.Clock());
    return "refused: " + tag + " (" + why + ")";
  };
  // 1. the refusals: nothing is called or stored before all of them have passed
  uint32_t ch = 0;
  const std::string r = SlotChar(e, slot, ch);
  if (!r.empty()) return refuse(r);
  if (e.U32(ch + kKindOff) != kWrestlerKind) return refuse("[char+24] is not 4: not a wrestler kind, his controller is not the wrestler's");
  const uint32_t ctrl = e.U32(ch + kCtrlOff);
  if (!e.Ok(ctrl)) return refuse("the wrestler has no motion controller, [char+3096] is empty");
  if (e.U32(ch + kPartnerOff) != 0) return refuse("he is in a two-object motion, [char+836] holds his partner");
  for (int s = 0; s < kSlots; ++s) {
    if (s == slot) continue;
    const uint32_t other = e.U32(kCharTable + 4u * uint32_t(s));
    if (e.Ok(other) && e.U32(other + kPartnerOff) == ch) return refuse("slot " + std::to_string(s) + " holds him in a two-object motion ([+836] is him)");
  }
  // the game's own test (sub_823CB3F0): the 72-word table is read only for 0 <= [char+1340] < 72; -1 (every wrestler in runs 28 / 29) or more = no actor
  const int32_t actor_idx = int32_t(e.U32(ch + kActorIdxOff));
  if (actor_idx >= 0 && actor_idx < kActorCount && e.U32(kActorTable + 4u * uint32_t(actor_idx)) != 0) return refuse("he is a cutscene actor (the actor table word for [char+1340] is set)");
  if (e.U32(ch + kSpecialOff) != 0) return refuse("a two-wrestler special is in progress, [char+1920] is not 0 (it is 1 on both wrestlers and sets no +836)");
  // 2. the facing, through the game's own yaw function (it stores char+148 wrapped to pi)
  if (has_facing) e.CallF(Fn::SetYaw, ch, double(deg) * (3.14159265358979323846 / 180.0));
  // 3. x and z, then the place byte and the floor height from the game's own two functions (the floor height reads the place byte just stored)
  e.Put32(ch + kPosOff, FloatBits(x));
  e.Put32(ch + kZOff, FloatBits(z));
  const uint32_t root = e.U32(ch + kRootOff);  // the skeleton root node the height function reads on one branch (the warp writes it too)
  if (e.Ok(root)) {
    e.Put32(root + 0, FloatBits(x));
    e.Put32(root + 8, FloatBits(z));
  }
  const uint8_t place = uint8_t(e.Call(Fn::PlaceOf, ch + kPosOff, 0, 0.0) & 255);
  e.Put8(ch + kPlaceOff, place);
  const double y = e.CallRetF(Fn::FloorY, ch, 255);
  // 4. the game's all-copies warp (every mirror of the position)
  e.CallF3(Fn::Warp, ch, double(x), y, double(z));
  // 5. THE ONE NAMED FIX-UP: sub_829C0EE8 also writes char+70 by its own rule, a story-scene one (its callers sub_829D9C08 / sub_829DB748 place the cutscene actors
  //    of acts 34000-34599); that is not the match's place code for this point. The only all-mirror warp is the scene variant, so the byte the game's own
  //    sub_82612E88 gave for this point is put back. Nothing else is corrected after the warp.
  e.Put8(ch + kPlaceOff, place);
  // 6. the controller's angle (the warp writes none; sub_8273B790 +0x298 does this store) and the old rotation (sub_8264FAB0 copies +144..+159 to +208 after a placement)
  e.Put32(ctrl + kCtrlAngOff, e.U32(ch + kYawOff));
  for (uint32_t i = 0; i < 16; i += 4) e.Put32(ch + kOldRotOff + i, e.U32(ch + kRotOff + i));
  // 7. read back and log one line
  const float rx = e.F32(ch + kPosOff), ry = e.F32(ch + kYOff), rz = e.F32(ch + kZOff), rf = e.F32(ch + kYawOff);
  const unsigned rp = e.U8(ch + kPlaceOff);
  char b[256];
  std::snprintf(b, sizeof b, "asked x %.3f z %.3f facing %s%.3f deg; read back x %.3f y %.3f z %.3f facing %.5f rad (%.2f deg) place %u (floor y %.3f, place byte from the game)",
                double(x), double(z), has_facing ? "" : "(kept) ", has_facing ? double(deg) : 0.0, double(rx), double(ry), double(rz), double(rf),
                double(rf) * (180.0 / 3.14159265358979323846), rp, y);
  e.Log("director: " + tag + ": " + b + "; char " + Hex(ch) + " clock " + e.Clock());
  if (std::fabs(double(rx) - double(x)) > 0.01 || std::fabs(double(rz) - double(z)) > 0.01)
    return "failed: " + tag + " read-back x " + std::to_string(double(rx)) + " z " + std::to_string(double(rz)) + " is not the point asked";
  return "ok: " + tag + " (read back x " + std::to_string(double(rx)) + " y " + std::to_string(double(ry)) + " z " + std::to_string(double(rz)) + " place " + std::to_string(rp) + ")";
}
