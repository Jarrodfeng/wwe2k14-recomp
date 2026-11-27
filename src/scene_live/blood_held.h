// S111 BLOODMOD3 (docs\research\s111\BLOODMOD3.md): the permanent blood mod's rows for a floor splat the game fades without drawing its own record. The pool update sub_826983B8 steps every live particle's life
// (sub_82697EB8) BEFORE it tests the replay word [Q + 3888], and only the particle -> record copy waits on that word: during an instant replay a splat can fade and free while its record shows the playback.
// Header only, std + the row contract: the tracker (weapon_rows_core.h Tracker EmitHeldSplats / NoteBloodRef) and tools\weapons\floor_blood_test.cpp run THIS code. Read only: nothing here changes the game.
// The rule (the draw sub_826979F8): a splat's node rows are  inverse-bind x local x world  (row vectors) with local = diag(scale, 1, scale) (record +16) and world = the y turn sub_82C330D8 makes from the
// angles (0, record +28, 0) (m[2] = -sin, m[8] = sin) with the record's place (+0..+11) as row 3 (sub_82C334A8). A y turn commutes with diag(s, 1, s), so ANY drawn splat's rows and its record fields give
// another splat's rows without the inverse bind. In the rows' column form (arena_world.h RowsFromMatrix): G' = T(p') Rc(a' - a) D(s' / s) T(-p) G, Rc(d) = [[cos d, 0, sin d], [0, 1, 0], [-sin d, 0, cos d]].
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../renderer_scene/weapon_rows.h"

namespace rdr_scene::live::bloodheld {

namespace bw = rdr_scene::weapons;

// Particle k at Q + 48 + 80 k (weapon_rows.h kBloodPartOff / kBloodPartStride); the pool update copies +32..+47 -> record +0..+15, +56 -> +16, +64 -> +20, +68 -> +24, +72 -> +28, +76 -> +32 (sub_826983B8).
inline constexpr uint32_t kPartOffX = 32, kPartOffY = 36, kPartOffZ = 40, kPartOffScale = 56, kPartOffColour = 68, kPartOffAngle = 72, kPartOffTex = 76;
inline constexpr uint32_t kRecOffX = 0, kRecOffY = 4, kRecOffZ = 8, kRecOffScale = 16, kRecOffAngle = 28;
inline constexpr uint32_t kStateFade = 3;   // sub_82697EB8: state 3 lowers the alpha by 0.003333 x dt each update and frees the slot (state 4) when it drops below 0

struct Fields { float x = 0.f, y = 0.f, z = 0.f, scale = 0.f, angle = 0.f; };

// Planted twins (the unit sets them; the game never does).
struct Plants {
  bool flip_angle = false;   // twin: the turn applied the other way round (the made splat turned by -(a' - a))
};

template <class ReadWordFn>
inline bool ReadFloat(ReadWordFn& read_word, uint32_t a, float& out) {
  uint32_t v = 0;
  if (!read_word(a, v)) return false;
  std::memcpy(&out, &v, 4);
  return true;
}

// The fields the draw builds a record's rows from (record at `rec` = Q + 2448 + 48 k).
template <class ReadWordFn>
inline bool ReadRecordFields(ReadWordFn& read_word, uint32_t rec, Fields& f) {
  return ReadFloat(read_word, rec + kRecOffX, f.x) && ReadFloat(read_word, rec + kRecOffY, f.y) && ReadFloat(read_word, rec + kRecOffZ, f.z) && ReadFloat(read_word, rec + kRecOffScale, f.scale) &&
         ReadFloat(read_word, rec + kRecOffAngle, f.angle);
}

struct Live {
  Fields f;
  float alpha = 0.f, colour = 1.f;
  uint32_t state = bw::kBloodStateFree, tex = 0, alpha_bits = 0;
};
// Particle k's own state, alpha, colour, texture and fields (what its record would hold after the copy).
template <class ReadWordFn>
inline bool ReadParticle(ReadWordFn& read_word, uint32_t q, uint32_t k, Live& p) {
  const uint32_t at = q + bw::kBloodPartOff + bw::kBloodPartStride * k;
  uint32_t tw = 0;
  if (!read_word(at + bw::kBloodPartOffState, p.state) || !read_word(at + bw::kBloodPartOffAlpha, p.alpha_bits) || !read_word(at + kPartOffTex, tw)) return false;
  std::memcpy(&p.alpha, &p.alpha_bits, 4);
  p.tex = tw < 16u ? tw : 0u;   // as the tracker's PullBloodRecs takes a record's texture
  return ReadFloat(read_word, at + kPartOffColour, p.colour) && ReadFloat(read_word, at + kPartOffX, p.f.x) && ReadFloat(read_word, at + kPartOffY, p.f.y) && ReadFloat(read_word, at + kPartOffZ, p.f.z) &&
         ReadFloat(read_word, at + kPartOffScale, p.f.scale) && ReadFloat(read_word, at + kPartOffAngle, p.f.angle);
}

// The rows of the splat with fields `to`, made from a drawn splat's rows `ref_rows` (12 floats, column form) and the record fields `ref` they were drawn from. false when ref.scale is not > 0.
inline bool SynthRows(const float ref_rows[12], const Fields& ref, const Fields& to, float out[12], const Plants& p = Plants()) {
  if (!(ref.scale > 0.f)) return false;
  float h[12];
  std::memcpy(h, ref_rows, sizeof h);
  h[3] -= ref.x, h[7] -= ref.y, h[11] -= ref.z;   // T(-p)
  const float k = to.scale / ref.scale;
  const float d = p.flip_angle ? ref.angle - to.angle : to.angle - ref.angle;
  const float c = std::cos(d), s = std::sin(d);
  for (int j = 0; j < 4; ++j) {   // Rc(d) D(k): rows 0 and 2 scaled, then turned about y
    const float r0 = h[j] * k, r2 = h[8 + j] * k;
    out[j] = c * r0 + s * r2;
    out[4 + j] = h[4 + j];
    out[8 + j] = -s * r0 + c * r2;
  }
  out[3] += to.x, out[7] += to.y, out[11] += to.z;   // T(p')
  return true;
}

}  // namespace rdr_scene::live::bloodheld
