// Session 98 (cap / particles): the ENTRANCE PARTICLE EFFECTS of pac\prtcls.pac, read from the player's own files, and a small
// deterministic simulation of them for the Arena Viewer (our code; the game's particle shaders and update code are not copied).
// Session 99 (particles 2): the commands are now DECODED FROM THE GAME'S CODE, not fitted. Evidence and the open items:
// out\s98\particles2\notes.md (local). [C] = read in the game's code / data, [U] = not confirmed.
//
// FILE LAYOUT (tools\assets\prtc.py prints it)
//   prtcls.pac: EPAC; 12-byte directory records from +0x80C (char[4] name, LE32 start in 2 KB sectors from +0x4000); sections PCE6
//   (entrance effects), PCCH, PCMN; the game's paths "/PCE6/%04d" .. (0x82000B78) [C]. A PCE6 record is named by the ENTRANCE BANK
//   (0107 Kane, 0139 / 0602 Cena, 0600 Kane_Champ) [C]. Its PACH holds effect N as a nested PACH (N/0 = 'PRTC', N/1 = the sprite texture
//   directory, the ch-pac layout) and entry 16000 = an alias table (u16 count, 36-byte records: u32 0x20000, f32 effect, f32 the effect
//   whose textures it uses) [C].
//   PRTC (little-endian): per emitter a 'rootchk' block whose 48-byte header follows the tag (the game reads it into the emitter at
//   +192, sub_82A10760): +8 u32 MODE (0 = camera-facing sprites, 2 = oriented quads with their own matrix, 1 = no rotation commands),
//   +16 char[16] name, +32 u16 MAX particles (Kane 40), +34 u16 command count [C: the counts match the cmd_chk records]. Then 'cmd_chk'
//   records of 60 bytes: char[8], u32 COMMAND, u32 A, s32 B, u32 bytes, 8 x f32 (command 22: a file name) [C].
//   A command runs on the effect frames A <= f < B (u32 compare, B = -1 = for ever) [C: sub_82A10198], in four phases per frame (table
//   0x820573D8: phase 1 = 3, 29, 30, 32, 34; 2 = 0; 4 = 16; 3 = the rest), then every live particle moves (sub_82A11CB0:
//   position += velocity x 10, roll += spin, age + 1). Command handlers: table 0x82057938 (update) / 0x820579C8 (load).
//
// THE COMMANDS (game coordinates: x, y DOWN, z; a length in the file x 10 = arena units) [C: the handler named per command]
//   0  EMIT     sub_82A12F18 / 82A122C8  every (interval + 1) frames: v0 + round(v1 r) particles into free slots (MAX per emitter);
//                                        life = round(v2 + v3 r) frames; velocity = (v4 + v5 r) along the emitter's x axis, turned by
//                                        +-v6 / 2 degrees about the axes in mask v7 (0 = all)            (r = uniform -1..1)
//   30 INTERVAL sub_82A17948             the emit interval = v0 + v1 r frames (Kane 5: one particle every 6 frames per emitter)
//   29 SHAPE    sub_82A17820 / 82A12610  spawn volume (type 7 = a box): half sizes x (v0 inner, v1 outer), y (v2, v3), z (v4, v5) x 10
//   13 SIZE     sub_82A14C28             size v0 at age v3 -> v1 + v2 r at age v4 (linear, then held); mode 2: width = v5 x height
//   2  ALPHA    sub_82A13658 / 82A13518  fade in over v0 frames, out over the last v1; level v3 / v4 (a v2-frame triangle wave between
//                                        them) + v5 r, 0-255
//   5  SPIN     sub_82A13A18             from age v6 (to v7): spin v0 + v1 r degrees per frame at v6, + v2 + v3 r per frame after
//   21 ORIENT   sub_82A163C8             at birth: angles (v0, v1, v2) + (v3, v4, v5) r degrees; mode 0 = the roll (v2 only), mode 2 =
//                                        the quad's own rotation (x then y then z [U order])
//   20 PUSH     sub_82A160F8             at birth: velocity += (v0, v1, v2) + a random direction x r x (v3, v4, v5), in the emitter's
//                                        frame (v6 = 1: in the world's)
//   8  DRAG     sub_82A13FA8             ages v0 .. v1: velocity x (v2 + v5 r, v3 + v6 r, v4 + v7 r) / 100 per frame
//   7  MOVE     sub_82A13E30 / 82A13C78  the EMITTER moves (v1, -v2, v3) x 10 and turns (v4, v5, v6) degrees over v0 + 1 frames
//                                        (Kane's Smoke_UP: z -90 = its x axis points up, so its particles rise; all four Kane 5
//                                        emitters move +100 z and 5 / 15 up on frame 0 = the game's smoke centre z 2629.5)
//   25 COLOUR   sub_82A16E88             at age v0: rgb = (v1, v2, v3) + (v4, v5, v6) r, 0-255 (Kane: (180, 0, 0) for particles born
//                                        before effect frame 370, (140, 50, 10) after)
//   1  ACCEL    sub_82A13138             ages v4 .. v5 (0 = for ever): velocity += (v0, -v1, v2) per frame (v3 = 0: emitter frame)
//   3  SPHERE   sub_82A138D0 / 82A12610  spawn shape: type v1 (0 = a random direction x a radius between v2 and v0, x 10)
//   19 RAMP     sub_82A15EF0 / 82A15DD0  colour (v2, v3, v4) at age v0 -> (v5, v6, v7) at age v1 (set only over the default white)
//   27 SIZEKEY  sub_82A173F0 / 82A172C0  at age v0 a target size (v1 + v3 r, v2 + v4 r) + v6 r, reached linearly over v5 frames
//   33 BIRTHV   sub_82A18158             newborn velocity x (v2 -> v3) / 100 over effect frames v0 .. v1
//   22 sprite texture ("Smoke0002.tga" -> directory entry "Smoke0002")
//   not drawn: 6 (sub_82A13BC8: at age v0 the sprite's cell byte = v1, an atlas frame [U]); handlers not read: 4, 9-12, 14-18, 23,
//   24, 26, 28, 31, 32, 34, 35 (spawn-shape types 1-3 of command 3 are drawn as type 0).
// Session 99 (particles 3), measured on the game's own draws (RenderDoc out\s98\combo\kane_stage_frame5451.rdc + the draw probe of
// run 071, Kane cue 5 f ~402, 109 smoke sprites; notes out\s98\particles3\notes.md, local) [C]:
//   SIZE   a sprite's half size = size x 10 (full edge = 20 x the cmd 13 / 27 size, both kinds; the viewer had x 5 by eye).
//   ALPHA  the vertex colour = (rgb, alpha) bytes / 255 (Kane's level 200 -> 0.784); the pixel shader multiplies it by the texture
//          and a soft-particle depth fade, no x 2 (the viewer's byte / 255 stands; the fade is drawn since particles 4:
//          min(|scene - particle|, 30) / 30 view units, viewer_hlsl.h kSprite).
//   PLACE  cmd 7's translation is applied (it was not: the cause of the ~100-unit offset of Kane's smoke).
// Session 99 (particles 4), read in the game's code [C] (the viewer already matched; no simulation change):
//   ORDER  sub_82A10198 per frame: free the particles with age >= life, phases 1 -> 4 over the commands (phase table 0 = phase 3),
//          then sub_82A11CB0 moves them, then frame + 1. So EMIT (phase 2) runs before cmd 7's MOVE (phase 3) on the same frame: a
//          particle born on a move frame spawns at the emitter's place before that frame's step (the loop below does the same).
//   BORN   sub_82A12610 stores a newborn's position in world space: object rotation x (emitter rotation (+64) x the shape point +
//          the emitter's translation (+128)) + the object's translation (sub_82A11268). Nothing later re-applies the emitter: born
//          particles stay where born (sub_82A11CB0 only adds velocity x 10; sub_82A11B58 = mode-2 quads turned along their motion).
//          The capture agrees: the centre is +100 z, not +200. So cmd 7's step IS turned by the object's angles too (the viewer's
//          erot starts at them: the same composition).
//   WINDOW the dispatcher calls a command's handler only on A <= f < B (0x82A102EC..0x82A10300), cmd 7 included: B cuts a move
//          (0668-0670: 49 of 51 steps), as Cmd::On does here.
// Session 99 (smoke density) [C: run 071 capture, pixel history on Kane's shirt + a CPU replay, tools\assets\smoke_compare.py]: the
//   game's per-pixel rule is exactly vertex alpha x texture alpha x min(|scene w - sprite w|, 30) / 30, sprites behind the scene
//   discarded, SrcAlpha / InvSrcAlpha into the 8-bit pre-post target (predicted = measured on all 47 draws at 2 pixels; the replay
//   of the game's 109 draws reproduces its frame within 0.5 / 255) - the viewer's pass does the same. Per emitter the viewer's
//   sprites match the game's (count, alpha, edge, centre within ~10 units). The game's frame is ONE random draw: its +43 red on
//   Kane's shirt lies inside the viewer's spread over 13 draws (+34 +- 22); the viewer's default draw is a dense one (+69: one
//   alpha-200 sprite 44 units in front of his torso). Not a rule difference; nothing changed in the simulation.
// [U] still: cmd 29's inner (hollow) box, the 0x8xx-mask cone sampling order.
// WHEN / WHERE [C]: the bank's GMEV scene objects (Placement below): each names an effect, a place + angles, a start and a stop on the
// entrance's cue clock. Banks without GMEV objects keep the old rule [U]: effect id == the cue's sub, at the entrant's feet.
// Kane's stage smoke: object 5 is placed at (0, deck, 2529.2) with no extra offset (run 071 probe: the matrix handed to the effect);
// the +100 z to the game's sprite centre 2629.5 is the emitters' own cmd 7 move (Session 99, particles 3) [C].
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "asset_model.h"

namespace assets {
namespace particles {

struct Cmd {
  int id = -1;
  uint32_t a = 0, b = 0xFFFFFFFFu;  // the effect frames A <= f < B the command runs on
  float v[8] = {};
  std::string text;  // command 22: the file name
  bool On(uint32_t f) const { return f >= a && f < b; }
};
struct Emitter {
  std::string name;
  int mode = 0;          // rootchk header +8: 0 camera-facing, 2 oriented quads (1: no rotation)
  int max_parts = 40;    // rootchk header +32
  std::vector<Cmd> cmds;
  const Cmd* Find(int id) const {
    for (const Cmd& c : cmds)
      if (c.id == id) return &c;
    return nullptr;
  }
};
struct Effect {
  int id = -1;
  std::vector<Emitter> emitters;
  int tex_from = -1;  // the effect whose texture directory it uses (itself or the 16000 alias)
};
// Session 99 (particles 2): WHERE and WHEN an effect plays = the bank's scene objects in pac\gm.pac section GMEV (named by the
// entrance bank, like PCE6) [C: the game's scene-object loader sub_827CAB70 reads entry 3/10000 of the scene pac (16-byte header,
// LE u32 count, then 32-byte records: f32 x, y, z, u32 id (low 17 bits), f32 rx, ry, rz (radians), u32); sub_827CA9C0 builds an object
// per record (factory sub_827D1408 by the type of entry 0's row: 1, 2, 8, 9, 10 = the particle object sub_827C8420, x 10 = arena
// units, sub_827D1DE0); entry 0 = 8-byte rows (u16 id, u16 type, u16 float count, u16 offset into entry 2's floats); the object's
// per-frame sub_827D2A00 posts "play effect p[1]" when the entrance reaches cue p[2] frame p[3] and stops it at cue p[4] frame p[5]
// (9999 = never), and hands the effect its world matrix (position + angles, sub_827D2778 / 82A0B8F0)]. Kane 107: the stage
// lights / pyro 0-2 (cue 0 f10 .. cue 5), the stage smoke 5 (cue 0 f200 .. cue 15), the ring-post fire 10-13 and 20-23 at
// (+-322, 225 up, +-322) from cue 20 f746, the shared truss lights 110-133 (a 941-unit circle 1099 above the ring) and 210-223
// (a row 1313 above the stage).
struct Placement {
  int effect = -1, type = 0;
  float pos[3] = {};       // game units (x 10 applied), game space (y down)
  float rot[3] = {};       // radians
  bool floor_rel = false;  // p[9] = 1: y counted from the floor under it [U: the 210-223 row and Kane's 5 have it; meaning inferred]
  float floor = 0;         // (viewer) that floor's height at the object's own x / z, looked up once when the entrance loads
  int start_sub = 0, start_f = 0, end_sub = 0, end_f = 9999;
};
struct Bank {
  bool ok = false;
  int bank = -1;
  std::vector<Placement> placed;  // gm.pac GMEV (empty: no scene objects for this bank -> the old rule, effect == the cue's sub)
  std::map<int, Effect> effects;
  std::map<int, std::vector<uint8_t>> tex_dirs;  // effect id -> its texture directory (entry N/1)
  std::map<std::pair<int, size_t>, Texture> sprites;  // (effect, emitter) -> its decoded sprite texture (command 22)
  std::map<int, int> alias;  // entry 16000: effect -> the effect whose textures it uses
  // An effect of a group (several copies sharing one texture set: Kane 0-2, 10-13, 20-23, 110-223). Without GMEV objects such an
  // effect has no place and is not drawn; a lone one is drawn at the entrant when its id equals the cue's sub (the pre-GMEV rule).
  bool Grouped(int id) const {
    if (alias.count(id)) return true;
    for (const auto& [k, v] : alias) if (v == id) return true;
    return false;
  }
  bool Drawn(int id) const {
    if (placed.empty()) return !Grouped(id);
    for (const Placement& p : placed) if (p.effect == id) return true;
    return false;
  }
};

// ---- Session 99 (scene objects): the GMEV objects that are not particle effects ----------------------------------------------------
// The factory sub_827D1408 switches on the row's type (u16 at row +2; jump table at 0x827D144C, 35 cases) [C]. Census over the 434
// GMEV records (placed objects): type 1 3611, 2 2, 6 643 (118 banks), 8 8994, 9 5116, 10 60, 11 14 (14 banks); no other type is placed.
// Every object keeps p = &row floats[1] at +84 (base constructor sub_827D1C30: +80 = the row, +84 = floats + 4, +88 = int(floats[0])).
// The two kinds below are the "500+" ids of the entrance banks (Kane 107: 500, 501 type 6, 502 type 11).
//
// TYPE 6 = a SCREEN FLASH (sub_827C7D38, 176 bytes, vtable 0x82032EC4) [C]:
//   floats: [0] -, [1] 1 (the 2D quad's blend / state value [U which]), [2] cue, [3] start frame, [4] end cue, [5] end frame,
//   [6..8] r g b (0-255), [9] strength (0-255), [10] curve (read only when the row has more than 10 floats, else 0).
//   per frame (sub_827C7F18): off once the entrance has passed (cue, end frame) or before (cue, start frame) (sub_823102F0: cue below
//   the current one, or the same cue and the current frame >= f - both tests use floats[2], the start cue); between them
//   t = (camera-motion frame (sub_823FAF90) - start) / (end - start), clamped to 0 outside 0..1, and
//   curve 0 = 1 - |1 - 2t| (up and down), 1 = |1 - 2t|, 2 = t, 3 = 1 - t (Kane's: full at the start, gone at the end), 4 = 1, other = t;
//   alpha byte = int(strength x curve x (1/255) x 128) clamped 0..128 (0x820034FC = 1/255, 0x82003760 = 128: Yuke's 0x80 = opaque).
//   draw (sub_827C8130, when the alpha byte is not 0): a 2D sprite centred at (screen w, h) x 0.5 (0x8202B6D4) over the whole screen,
//   colour (r, g, b, alpha byte) - a full-screen colour flash. [U] the blend (state value floats[1] goes to sub_82BB7630 as r4) and
//   where it sits in the frame (the viewer composes it with the entrance film filter: the same full-screen colour-lerp form).
// TYPE 11 = the SOFT-PARTICLE SWITCH (sub_827D5298, 112 bytes, vtable 0x820336B8) [C]:
//   per frame (sub_827D52E8): when the entrance is exactly at cue floats[1] frame floats[2] it sets the global particle-draw object
//   ([0x836EE38C]) +32 = 1 and +36 = floats[5]; at cue floats[3] frame floats[4] it sets +32 = 0 (9999 = never). The particle pass
//   sub_82254E30 draws with technique "T_SoftParticle" and pixel constant c211 = +36 while +32 is set, else "T_Particle"; the object's
//   constructor (sub_82255198) starts it off with +36 = 5.0. Kane's 502: on at cue 0 f2 with 30 (= the fade range the RenderDoc capture
//   read, particles 3). The only other writer, sub_833B1CB8 (on, 40), ran in the CAW scenario only. A row with 5 floats (0533) reads
//   its range from the next float of the array, as the game does.
struct ScreenFlash {
  int cue = 0, start_f = 0, end_f = 0, mode = 0, id = 0;
  float rgb[3] = {1, 1, 1};  // 0-1
  float strength = 0;        // 0-255
};
struct SoftSwitch {
  int on_cue = 0, on_f = 0, off_cue = 0, off_f = 9999, id = 0;
  float range = 5.f;
};
struct SceneLights {
  std::vector<ScreenFlash> flashes;
  std::vector<SoftSwitch> soft;
  bool Empty() const { return flashes.empty() && soft.empty(); }
};
// sub_823102F0: has the entrance passed (cue, f)? cue numbers compare by value (0 / 5 / 10 / ..), frames as integers.
inline bool CuePassed(int cur_cue, double cur_f, int cue, int f) { return cue < cur_cue || (cue == cur_cue && int(cur_f) >= f); }
// The flash's opacity (0..1, in steps of 1/128) at cue `cur_cue`, cue-local frame `f`.
inline float FlashAlpha(const ScreenFlash& s, int cur_cue, double f) {
  if (CuePassed(cur_cue, f, s.cue, s.end_f) || !CuePassed(cur_cue, f, s.cue, s.start_f) || s.end_f == s.start_f) return 0.f;
  double t = (f - s.start_f) / double(s.end_f - s.start_f);
  if (t < 0 || t > 1) t = 0;
  double c = t;
  switch (s.mode) {
    case 0: c = 1 - std::fabs(1 - 2 * t); break;
    case 1: c = std::fabs(1 - 2 * t); break;
    case 3: c = 1 - t; break;
    case 4: c = 1; break;
    default: break;  // 2 and any other: t
  }
  const int a = std::max(0, std::min(128, int(s.strength * c * (1.0 / 255.0) * 128.0)));
  return float(a) / 128.f;
}
// The flashes of this moment composed into one colour lerp (rgb, a): applying each in object order = one lerp toward `rgb` by `a`.
inline void FlashNow(const SceneLights& sl, int cur_cue, double f, float out[4]) {
  float keep = 1.f, acc[3] = {0, 0, 0};
  for (const ScreenFlash& s : sl.flashes) {
    const float a = FlashAlpha(s, cur_cue, f);
    if (a <= 0.f) continue;
    for (int k = 0; k < 3; ++k) acc[k] = acc[k] * (1.f - a) + s.rgb[k] * a;
    keep *= 1.f - a;
  }
  const float tot = 1.f - keep;
  for (int k = 0; k < 3; ++k) out[k] = tot > 0.f ? acc[k] / tot : 0.f;
  out[3] = tot;
}
// The soft-particle range in force at (cur_cue, f): the latest switch event passed decides; none = off (0). (review) best starts below
// any key (a cue -1 event counts); on a tie the OFF event wins, as in the game's update, which tests the off moment after the on one
// (sub_827D52E8: the +32 = 0 store comes last).
// [U] the switch writes a GLOBAL object ([0x836EE38C], built by sub_82252D98, reset only by sub_833B1C30 / a rebuild): the game may keep
// one entrance's value (Kane's 30) for a later entrance that has no switch of its own; the viewer starts every entrance off.
inline float SoftNow(const SceneLights& sl, int cur_cue, double f) {
  float range = 0.f;
  long long best = LLONG_MIN;
  auto key = [](int cue, int fr) { return (long long)cue * 1000000LL + std::min(fr, 999999); };
  for (const SoftSwitch& s : sl.soft)
    if (CuePassed(cur_cue, f, s.on_cue, s.on_f) && key(s.on_cue, s.on_f) > best) best = key(s.on_cue, s.on_f), range = s.range;
  for (const SoftSwitch& s : sl.soft)
    if (s.off_f != 9999 && CuePassed(cur_cue, f, s.off_cue, s.off_f) && key(s.off_cue, s.off_f) >= best) best = key(s.off_cue, s.off_f), range = 0.f;
  return range;
}

// One drawn sprite: centre, its two half-axes (viewer space; billboard: in the camera's right / up basis, the renderer turns them),
// colour (0-1, alpha) and which texture.
struct Sprite {
  float c[3], ax[3], ay[3], col[4];
  int tex = 0;
  bool billboard = false;
  int emitter = -1, age = 0, life = 0;  // (Session 99 smoke density: for the particles_dump command)
};

namespace detail {
// A deterministic stream per (emitter, effect): scrubbing gives the same picture (the game's own generator is one global LCG shared by
// every system, so its exact draws cannot be reproduced anyway).
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed * 0x9E3779B1u + 0x7F4A7C15u) {}
  uint32_t Next() {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return s;
  }
  float U() { return float(Next() & 0xFFFFFF) / float(0x1000000); }  // 0..1
  float S() { return U() * 2.f - 1.f; }                              // -1..1 (the game's rand: x * 2 - 1)
};
inline float Fin(float v, float d, float lo, float hi) { return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : d; }
constexpr float kDeg = 0.01745329252f;
// 3x3 rotation (rows), applied as v' = M v; x, then y, then z (R = Rz Ry Rx) [U order: only one-axis turns are used by the effects read]
struct M3 {
  float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  static M3 Euler(float ax, float ay, float az) {
    const float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
    M3 r;
    r.m[0][0] = cz * cy, r.m[0][1] = cz * sy * sx - sz * cx, r.m[0][2] = cz * sy * cx + sz * sx;
    r.m[1][0] = sz * cy, r.m[1][1] = sz * sy * sx + cz * cx, r.m[1][2] = sz * sy * cx - cz * sx;
    r.m[2][0] = -sy, r.m[2][1] = cy * sx, r.m[2][2] = cy * cx;
    return r;
  }
  M3 operator*(const M3& b) const {
    M3 r;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
    return r;
  }
  void Apply(const float v[3], float o[3]) const {
    for (int i = 0; i < 3; ++i) o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
  }
};
struct Part {
  int life = 0, age = 0;  // free when life == 0 or age > life (the game's test)
  float pos[3] = {}, vel[3] = {};
  float roll = 0, spin = 0;
  float w = 0, h = 0, size_end = 0, key_w = 0, key_h = 0;
  float alpha = 0;          // 0-255
  float rgb[3] = {255, 255, 255};
  M3 rot;                   // mode 2: the quad's own rotation
};
}  // namespace detail

// One emitter's running simulation (kept between calls: playing forward only steps the new frames; going back starts again from frame 0,
// so a scrub gives the same picture - deterministic in the frame).
struct EmitterState {
  int last = -1;  // the last frame whose commands ran (its move not yet applied); -1 = fresh
  std::vector<detail::Part> ps;
  detail::Rng rng{0};
  detail::M3 erot;
  int interval = 0;
  float etr[3] = {0, 0, 0};  // the emitter's own offset from the object (cmd 7's translation, game space), where new particles spawn
};
constexpr int kSnapEvery = 300;  // frames between kept snapshots (a scrub back re-simulates at most this many frames per emitter)
struct SimState {
  const void* fx = nullptr;  // the effect it belongs to (a different one resets it)
  std::vector<EmitterState> em;
  std::vector<std::vector<EmitterState>> snaps;  // per emitter: the state at frames 0, 300, 600, .. (before that frame's move)
};
constexpr size_t kSlotBudget = 65536;  // (review) particle slots simulated per viewer frame across every placed copy

// The sprites of effect `fx` `frame` frames (60 Hz) after it started, emitted around `org` (viewer space, Y up; org[1] = the floor),
// camera-facing ones as billboards (Sprite::billboard). `tex_of(emitter)` = the texture slot of an emitter's sprite (-1: none).
// `st` = the kept state (nullptr: simulated from frame 0 in a local one; at most kMaxFrames steps).
constexpr size_t kMaxSprites = 8192;
constexpr int kMaxFrames = 36000;  // 10 minutes
// Session 99: `rot` = the placed object's angles (radians, game space; nullptr = none), `stop` = the effect frame its emission stops
// at (the object's end; < 0 = never), `seed` = which copy (placed copies of one effect differ).
template <class TexOf>
inline void Simulate(const Effect& fx, double frame, const float org[3], TexOf tex_of, std::vector<Sprite>& out,
                     const float* rot = nullptr, double stop = -1, uint32_t seed = 0, SimState* st = nullptr,
                     size_t* budget = nullptr) {
  using namespace detail;
  if (!std::isfinite(frame) || frame < 0) return;
  const int nf = int(std::min<double>(frame, kMaxFrames));
  const M3 base = rot ? M3::Euler(Fin(rot[0], 0, -10, 10), Fin(rot[1], 0, -10, 10), Fin(rot[2], 0, -10, 10)) : M3();
  SimState local;
  SimState& S = st ? *st : local;
  if (S.fx != &fx || S.em.size() != fx.emitters.size())
    S.fx = &fx, S.em.assign(fx.emitters.size(), EmitterState()), S.snaps.assign(fx.emitters.size(), {});
  for (size_t ei = 0; ei < fx.emitters.size(); ++ei) {
    const Emitter& em = fx.emitters[ei];
    const int tex = tex_of(em);
    if (tex < 0) continue;
    const int maxp = std::max(1, std::min(em.max_parts, 1024));
    if (budget) {  // (review) a cap on the slots simulated across every copy in one call
      if (*budget < size_t(maxp)) continue;
      *budget -= size_t(maxp);
    }
    EmitterState& es = S.em[ei];
    std::vector<EmitterState>& snaps = S.snaps[ei];
    if (es.last >= 0 && nf < es.last) {  // going back: from the newest snapshot at or before nf (the same forward path: deterministic)
      size_t k = std::min(snaps.size(), size_t(nf / kSnapEvery) + 1);
      while (k > 0 && snaps[k - 1].last > nf) --k;
      if (k > 0) es = snaps[k - 1];
      else es.last = -1;
    }
    if (es.last < 0) {  // fresh: from frame 0
      es = EmitterState();
      es.ps.assign(static_cast<size_t>(maxp), Part());
      es.rng = Rng(uint32_t(fx.id) * 131u + uint32_t(ei) * 7919u + seed * 104729u + 1u);
      es.erot = base;  // the emitter's frame: the object's angles, then its own turn (cmd 7)
    }
    std::vector<Part>& ps = es.ps;
    Rng& rng = es.rng;
    M3& erot = es.erot;
    int& interval = es.interval;  // cmd 30
    for (int f = es.last + 1; f <= nf; ++f) {
      // (review) a snapshot every kSnapEvery frames for a cheap scrub back: the state after frame f - 1's commands, before its move
      // (last = f - 1; restoring it resumes here exactly)
      if (f % kSnapEvery == 0 && size_t(f / kSnapEvery) == snaps.size()) snaps.push_back(es);
      if (es.last >= 0)  // the move of the previous frame (sub_82A11CB0): position += velocity x 10, roll += spin, age + 1
        for (Part& p : ps) {
          if (p.life == 0) continue;
          for (int k = 0; k < 3; ++k) {
            p.vel[k] = Fin(p.vel[k], 0, -1000, 1000);  // (review) drag / accel cannot run away to inf
            p.pos[k] = Fin(p.pos[k] + p.vel[k] * 10.f, 0, -1e6f, 1e6f);
          }
          p.roll += p.spin, ++p.age;
        }
      es.last = f;
      const uint32_t uf = uint32_t(f);
      // phase 1: the interval (30); shape (29) is read at spawn
      for (const Cmd& c : em.cmds)
        if (c.id == 30 && c.On(uf)) interval = int(std::max(0.f, Fin(c.v[0] + c.v[1] * rng.S(), 0, 0, 3600)));
      // phase 2: emit (0)
      for (const Cmd& c : em.cmds) {
        if (c.id != 0 || !c.On(uf)) continue;
        if (stop >= 0 && double(f) >= stop) continue;  // the object stopped the effect: no new particles
        if ((f - int(c.a)) % (interval + 1) != 0) continue;
        int n = int(Fin(c.v[0], 0, 0, 64)) + int(Fin(c.v[1], 0, -64, 64) * rng.S() + 0.5f);
        const Cmd* shape = em.Find(29);
        for (Part& p : ps) {
          if (n <= 0) break;
          if (p.life != 0 && p.age <= p.life) continue;
          p = Part();
          p.life = std::max(1, int(Fin(c.v[2] + c.v[3] * rng.S() + 0.5f, 1, 1, 3600)));
          // velocity: the emitter's x axis, turned in a cone (v6 / 2 degrees) about the axes of mask v7 (0 = 15)
          const float spd = Fin(c.v[4] + c.v[5] * rng.S(), 0, -100, 100), cone = Fin(c.v[6], 0, 0, 360) * 0.5f * kDeg;
          int mask = int(Fin(c.v[7], 0, 0, 15));
          if (mask == 0) mask = 15;
          const float ax = (mask & 1) ? rng.S() * cone : 0.f, ay = (mask & 2) ? rng.S() * cone : 0.f, az = (mask & 4) ? rng.S() * cone : 0.f;
          const float d0[3] = {1, 0, 0};
          float d1[3], d2[3];
          M3::Euler(ax, ay, az).Apply(d0, d1);
          erot.Apply(d1, d2);
          for (int k = 0; k < 3; ++k) p.vel[k] = d2[k] * spd;
          // position: the shape box (29), outer half sizes x 10, in the emitter's frame [U: the inner (hollow) part]; or the
          // sphere (3: type v1 = 0: a random direction x a radius between v2 and v0, x 10) [U: types 1-3 drawn as that sphere]
          float lp[3] = {0, 0, 0};
          const Cmd* ball = em.Find(3);
          if (shape) {
            for (int k = 0; k < 3; ++k)
              lp[k] = rng.S() * 10.f * std::max(std::fabs(Fin(shape->v[2 * k], 0, -1e4f, 1e4f)), std::fabs(Fin(shape->v[2 * k + 1], 0, -1e4f, 1e4f)));
          } else if (ball) {
            float dir[3] = {rng.S(), rng.S(), rng.S()};
            const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            const float r0 = Fin(ball->v[2], 0, 0, 1e4f) * 10.f, r1 = Fin(ball->v[0], 0, 0, 1e4f) * 10.f;
            const float rad = r0 + rng.U() * (r1 - r0);
            for (int k = 0; k < 3; ++k) lp[k] = l > 1e-6f ? dir[k] / l * rad : 0.f;
          }
          erot.Apply(lp, p.pos);
          for (int k = 0; k < 3; ++k) p.pos[k] += es.etr[k];  // around the emitter's moved origin (cmd 7)
          --n;
        }
      }
      // phase 3: the per-particle commands, then the emitter's move
      for (const Cmd& c : em.cmds) {
        if (!c.On(uf)) continue;
        switch (c.id) {
          case 20:  // push at birth
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life || p.age != 0) continue;
              float dir[3] = {rng.S(), rng.S(), rng.S()};
              const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
              const float u = rng.S();
              float add[3];
              for (int k = 0; k < 3; ++k) add[k] = Fin(c.v[k], 0, -100, 100) + (l > 1e-6f ? dir[k] / l : 0.f) * u * Fin(c.v[3 + k], 0, -100, 100);
              float w[3];
              if (c.v[6] == 0) erot.Apply(add, w);
              else std::memcpy(w, add, sizeof w);
              for (int k = 0; k < 3; ++k) p.vel[k] += w[k];
            }
            break;
          case 21:  // orientation at birth
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life || p.age != 0) continue;
              float a[3];
              for (int k = 0; k < 3; ++k) a[k] = Fin(c.v[k], 0, -3600, 3600) + (c.v[3 + k] != 0 ? rng.S() * Fin(c.v[3 + k], 0, -3600, 3600) : 0.f);
              if (em.mode == 0) p.roll += a[2] * kDeg;
              else if (em.mode == 2) p.rot = M3::Euler(a[0] * kDeg, a[1] * kDeg, a[2] * kDeg);
            }
            break;
          case 25:  // colour at age v0
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life || p.age != int(Fin(c.v[0], -1, -1, 3600))) continue;
              for (int k = 0; k < 3; ++k) p.rgb[k] = Fin(c.v[1 + k] + rng.S() * c.v[4 + k], 0, 0, 255);
            }
            break;
          case 13: {  // size: v0 at age v3 -> v1 + v2 r at age v4
            const int a0 = int(Fin(c.v[3], 0, 0, 3600)), a1 = int(Fin(c.v[4], 0, 0, 3600));
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life || p.age < a0 || p.age > a1) continue;
              if (p.age == a0) p.size_end = Fin(c.v[1] + c.v[2] * rng.S(), 0, -1e4f, 1e4f);
              float s = p.size_end;
              if (a1 - a0 > 1) s = (p.size_end - c.v[0]) / float(a1 - a0) * float(p.age - a0) + c.v[0];
              s = std::max(0.f, Fin(s, 0, 0, 1e4f));
              p.h = s;
              p.w = (em.mode == 2 && c.v[5] > 0) ? Fin(c.v[5], 1, 0, 100) * s : s;
            }
            break;
          }
          case 2: {  // alpha
            const float fin = Fin(c.v[0], 0, 0, 3600), fout = Fin(c.v[1], 0, 0, 3600);
            const float k0 = fin > 0 ? 255.f / fin : 255.f, k1 = fout > 0 ? 255.f / fout : 255.f;
            const int per = int(Fin(c.v[2], 0, 0, 3600));
            float k2 = per ? (c.v[3] - c.v[4]) / float(per) : 0.f;
            if (k2 < 0) k2 = -k2;
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life) continue;
              const int rem = p.life - p.age;
              float env;
              if (p.age < int(fin)) env = std::min(255.f, std::max(0.f, float(p.age) * k0));
              else if (rem < int(fout)) env = rem > 0 ? std::min(255.f, std::max(0.f, 255.f - k1 * float(int(fout) - rem))) : 0.f;
              else env = 255.f;
              float a = env;
              if (per) {
                const int m = p.age % (2 * per);
                const float lvl = m < per ? float(m) * k2 + c.v[4] : c.v[3] - float(m - per) * k2;
                a = (lvl + rng.S() * c.v[5]) / 255.f * env;
              }
              p.alpha = std::min(255.f, std::max(0.f, Fin(a, 0, 0, 255)));
            }
            break;
          }
          case 5: {  // spin (mode 1 has none)
            if (em.mode == 1) break;
            const int a0 = int(Fin(c.v[6], 0, 0, 3600)), a1 = int(Fin(c.v[7], 0, 0, 3600));
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life || p.age < a0 || (a1 && p.age >= a1)) continue;
              if (p.age == a0) p.spin = (c.v[0] + c.v[1] * rng.S()) * kDeg;
              p.spin += (c.v[2] + (c.v[3] != 0 ? c.v[3] * rng.S() : 0.f)) * kDeg;
              p.spin = Fin(p.spin, 0, -6.3f, 6.3f);
            }
            break;
          }
          case 8: {  // drag over ages v0 .. v1
            const int a0 = int(Fin(c.v[0], 0, 0, 3600)), a1 = int(Fin(c.v[1], 0, 0, 3600));
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life || p.age < a0 || p.age >= a1) continue;
              for (int k = 0; k < 3; ++k) p.vel[k] *= Fin((float(int(Fin(c.v[2 + k], 100, -1000, 1000))) + c.v[5 + k] * rng.S()) * 0.01f, 1, -2, 2);
            }
            break;
          }
          case 7: {  // the emitter moves (v1, -v2, v3) x 10 and turns (v4, v5, v6) degrees over v0 + 1 frames
            const int dur = int(Fin(c.v[0], 0, 0, 3600));
            if (c.v[7] == 0 && uint32_t(f) > c.a + uint32_t(dur)) break;
            const float n = float(dur + 1);
            // Session 99 (particles 3) [C: load sub_82A13C78 stores the step (v1, -v2, v3) x 10.0 (0x82192140) / (v0 + 1); update
            // sub_82A13E30 turns that step by the emitter's current rotation, adds it to the emitter's translation, THEN applies the
            // turn (sub_82A10550)]. Kane 5: (0, -5 / -15, +100) = the game's flat quads at y -185 (deck -180) and its sprites centred
            // at z 2629.5 for the object at z 2529.2 (RenderDoc kane_stage_frame5451 + the draw probe, 109 sprites).
            const float step[3] = {Fin(c.v[1], 0, -1e4f, 1e4f) * 10.f / n, -Fin(c.v[2], 0, -1e4f, 1e4f) * 10.f / n,
                                   Fin(c.v[3], 0, -1e4f, 1e4f) * 10.f / n};
            float ws[3];
            erot.Apply(step, ws);
            for (int k = 0; k < 3; ++k) es.etr[k] = Fin(es.etr[k] + ws[k], 0, -1e6f, 1e6f);
            erot = erot * M3::Euler(Fin(c.v[4], 0, -3600, 3600) / n * kDeg, Fin(c.v[5], 0, -3600, 3600) / n * kDeg, Fin(c.v[6], 0, -3600, 3600) / n * kDeg);
            break;
          }
          case 1: {  // acceleration (v0, -v1, v2) per frame for ages v4 .. v5 (0 = for ever); v3 = 0: in the emitter's frame
            const int a0 = int(Fin(c.v[4], 0, 0, 3600)), a1 = int(Fin(c.v[5], 0, 0, 3600));
            float acc[3] = {Fin(c.v[0], 0, -100, 100), -Fin(c.v[1], 0, -100, 100), Fin(c.v[2], 0, -100, 100)}, w[3];
            if (c.v[3] == 0) erot.Apply(acc, w);
            else std::memcpy(w, acc, sizeof w);
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life || p.age < a0 || (a1 && p.age >= a1)) continue;
              for (int k = 0; k < 3; ++k) p.vel[k] += w[k];
            }
            break;
          }
          case 19: {  // colour ramp (v2, v3, v4) at age v0 -> (v5, v6, v7) at age v1
            const int a0 = int(Fin(c.v[0], 0, 0, 3600)), a1 = int(Fin(c.v[1], 0, 0, 3600));
            if (a1 < a0) break;
            float slope[3] = {0, 0, 0};
            if (a1 > a0)
              for (int k = 0; k < 3; ++k) slope[k] = (Fin(c.v[5 + k], 0, 0, 255) - Fin(c.v[2 + k], 0, 0, 255)) / float(a1 - a0);
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life || p.age < a0 || (a1 > a0 ? p.age >= a1 : p.age > a1)) continue;
              if (p.age == a0 && p.rgb[0] == 255 && p.rgb[1] == 255 && p.rgb[2] == 255)  // (the game: only a colour still at the default)
                for (int k = 0; k < 3; ++k) p.rgb[k] = Fin(c.v[2 + k], 0, 0, 255);
              if (p.age > a0)
                for (int k = 0; k < 3; ++k) p.rgb[k] = std::min(255.f, std::max(0.f, p.rgb[k] + slope[k]));
            }
            break;
          }
          case 27: {  // size key: at age v0 a target (v1 + v3 r, v2 + v4 r) + v6 r, reached over v5 frames (0 = at once)
            const int a0 = int(Fin(c.v[0], 0, 0, 3600)), dur = int(Fin(c.v[5], 0, 0, 3600));
            for (Part& p : ps) {
              if (p.life == 0 || p.age > p.life) continue;
              const int rel = p.age - a0;
              if (rel < 0 || rel > dur) continue;
              if (rel == 0) {
                const float u1 = rng.S(), u2 = rng.S(), e = c.v[6] != 0 ? rng.S() * c.v[6] : 0.f;
                p.key_w = Fin(c.v[1] + u2 * c.v[3] + e, 0, -1e4f, 1e4f), p.key_h = Fin(c.v[2] + u1 * c.v[4] + e, 0, -1e4f, 1e4f);
              }
              if (dur == 0) {
                if (rel == 0) p.w = std::max(0.f, p.key_w), p.h = std::max(0.f, p.key_h);
              } else {
                const float t = 1.f / float(dur - rel + 1);
                p.w = std::max(0.f, p.w + (p.key_w - p.w) * t), p.h = std::max(0.f, p.h + (p.key_h - p.h) * t);
              }
            }
            break;
          }
          case 33: {  // the birth speed x (v2 -> v3) / 100 over effect frames v0 .. v1
            const float span = c.v[1] - c.v[0], at = float(f) - c.v[0];
            if (at < 0) break;
            const float t = (at > span || span <= 0) ? 1.f : at / span;
            const float s = Fin((c.v[2] + (c.v[3] - c.v[2]) * t) * 0.01f, 1, -10, 10);
            for (Part& p : ps)
              if (p.life != 0 && p.age == 0)
                for (int k = 0; k < 3; ++k) p.vel[k] *= s;
            break;
          }
          default:
            break;
        }
      }
    }  // (the state of frame nf; its move is applied in the draw below, as the game draws after the update)
    // draw: game space (y down) -> viewer (Y up); full size = size x 10 (a unit quad of +-0.5 in the file-unit scale) [U: the game's
    // corners are not read; the game shot 6824 s5_stage's dark puff (Smoke_UP_Dark, size ~8-12) spans ~130 units at Kane's depth]
    for (const Part& p : ps) {
      if (p.life == 0 || p.age > p.life || p.alpha <= 0 || p.h <= 0) continue;
      Sprite s;
      // Session 99 (particles 4) [C]: the game draws the state after the whole update, i.e. after sub_82A11CB0's move (position +=
      // velocity x 10, roll += spin); the loop keeps frame nf's move for the next step, so it is applied here for the picture.
      float dp[3];
      for (int k = 0; k < 3; ++k) dp[k] = Fin(p.pos[k] + Fin(p.vel[k], 0, -1000, 1000) * 10.f, 0, -1e6f, 1e6f);
      const float roll = p.roll + p.spin;
      s.c[0] = org[0] + dp[0], s.c[1] = org[1] - dp[1], s.c[2] = org[2] + dp[2];
      if (!std::isfinite(s.c[0]) || !std::isfinite(s.c[1]) || !std::isfinite(s.c[2]) || !std::isfinite(p.w) || !std::isfinite(p.h) ||
          !std::isfinite(roll))
        continue;  // (review) nothing non-finite reaches the renderer
      // Session 99 (particles 3) [C: measured] half size = size x 10: in the game capture kane_stage_frame5451 the flat quads' world
      // matrix scale is the size itself (10.2 .. 16.1 = cmd 13's 10 -> 14 +- 4) and their unprojected edges are 20.00 x that scale on all
      // 29; the 80 camera-facing sprites (matrix scale 1, corners in the vertices) measure 143 .. 262 units = 20 x 7.2 .. 13.1, the range
      // of their cmd 13 sizes. (The viewer had x 5, judged by eye: half the game's size.)
      const float hw = p.w * 10.f, hh = p.h * 10.f;
      if (em.mode == 2) {  // the quad's own frame: local x / y axes
        float ex[3], ey[3];
        const float x0[3] = {1, 0, 0}, y0[3] = {0, 1, 0};
        p.rot.Apply(x0, ex), p.rot.Apply(y0, ey);  // (cmd 21 v6 = 0: not turned with the emitter, sub_82A163C8)
        for (int k = 0; k < 3; ++k) s.ax[k] = ex[k] * hw, s.ay[k] = ey[k] * hh;
        s.ax[1] = -s.ax[1], s.ay[1] = -s.ay[1];  // (the height above the floor comes from cmd 7: Kane's Smoke_Ground 5 units, as the game)
      } else {             // facing the camera, rolled
        const float ca = std::cos(roll), sa = std::sin(roll);
        s.ax[0] = hw * ca, s.ax[1] = hw * sa, s.ax[2] = 0, s.ay[0] = -hh * sa, s.ay[1] = hh * ca, s.ay[2] = 0;
        s.billboard = true;
      }
      for (int k = 0; k < 3; ++k) s.col[k] = p.rgb[k] / 255.f;
      s.col[3] = p.alpha / 255.f;
      s.tex = tex;
      s.emitter = int(ei), s.age = p.age, s.life = p.life;
      if (out.size() >= kMaxSprites) return;  // (review) a hard cap whatever the file says
      out.push_back(s);
    }
  }
}

}  // namespace particles
}  // namespace assets
