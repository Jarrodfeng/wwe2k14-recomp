// S103 PMS (docs\research\s103\PMF.md section 4, slice 1): the rows the shadowed FLOOR (yBG_* FX_SHADOW, slot 12) reads from the game's props map, from OUR inputs. Pure: no renderer, no game, no SDK.
//
// The props map is the game's first shadow pass of a frame (sub_827E7F00): a light straight down, then the circle blobs in the BLUE channel of the same 512 RGBA8 target. The floor shaders read it with
//   TexProjLight (VS c68-c71) = bias(w, h) x (Trapezoidal x ViewProj of the props light)   [C: shadow_census.py 'bias' check, rows 0 / 1 / 2 / 3 of the stream's floor draws, tools\shadow\shadow_census.py:709-735]
//   ShadowPrm    (PS c168-c170) = the props light's shadow floats                            [C: the same state the props casters read, SKIP_GAME_WORK_MAP 17.2 item 2]
// and the circle blobs' own c4-c7 ARE Trapezoidal x ViewProj (4e-8, SHADOW_PROMOTION_MAP 2.3), so the floor matrix is the circle entry's rows with the bias applied. In REGISTER layout (register i = four floats
// of constant c(base + i)), the bias takes the rows: row 0 = 0.5 tv0 + (0.5 + 0.5 / w) tv3, row 1 = -0.5 tv1 + (0.5 - 0.5 / h) tv3 (the y flip takes the half texel with it), rows 2 and 3 = tv2, tv3.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "light_state_rows.h"
#include "props_map_switch.h"   // g_on / On()
#include "record_io.h"   // SceneTables
#include "scene_record.h"

namespace rdr_scene {
namespace props_map {

inline bool g_skip_pairing = false;   // test only: the planted twin that trusts any direct row
inline bool g_prefer_direct_off = false;   // test only (tools\scene\ks1_unit.cpp): the search path alone

// tv = the circle entry's c4-c7 (4 registers of 4 floats). reversed = the planted fault 'props_bias_order' (the bias applied to the components of each register instead of to the registers: the other order of the product).
inline void FloorMatrix(const float tv[16], uint32_t w, uint32_t h, bool reversed, float out[16]) {
  const double bx = 0.5 + 0.5 / double(w ? w : 512), by = 0.5 - 0.5 / double(h ? h : 512);
  float r[16];
  if (!reversed) {
    for (int k = 0; k < 4; ++k) {
      r[k] = float(0.5 * double(tv[k]) + bx * double(tv[12 + k]));
      r[4 + k] = float(-0.5 * double(tv[4 + k]) + by * double(tv[12 + k]));
      r[8 + k] = tv[8 + k];
      r[12 + k] = tv[12 + k];
    }
  } else {
    for (int i = 0; i < 4; ++i) {
      r[4 * i + 0] = float(0.5 * double(tv[4 * i + 0]) + bx * double(tv[4 * i + 3]));
      r[4 * i + 1] = float(-0.5 * double(tv[4 * i + 1]) + by * double(tv[4 * i + 3]));
      r[4 * i + 2] = tv[4 * i + 2];
      r[4 * i + 3] = tv[4 * i + 3];
    }
  }
  std::memcpy(out, r, sizeof r);
}

// Trapezoidal x ViewProj of a light state, register layout (numpy `TR @ VP` of the census): the product the circle pass uploads as c4-c7 when the state is the props light.
inline void StateProduct(const LightState& st, double tv[16]) {
  const CasterRows cr = BuildCasterRows(st);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += double(cr.trapezoid[i][k]) * double(cr.view_proj[k][j]);
      tv[4 * i + j] = s;
    }
}

// The colour-call light state that holds the props camera: the (non-synthetic) state whose ViewProj ROW 3 (the light's position row: straight down, y 50,000) equals the circle entry's row 3 to `tol` relative.
// Row 3 of Trapezoidal x ViewProj IS ViewProj's row 3 (the Trapezoidal's rows 2 / 3 are the identity's: shadow_census.py 'trap_rows23_identity'), so the circle rows name the state without its crop. The WHOLE product is NOT
// compared: a colour call's light state is the guest frame BEFORE the circle entry's (refs stamped 3965, circle 3966: measured on z7147 f100, rel err 9.4e-4 of the product on every props state, 0 on row 3), and the crop moves with the camera.
// Its ShadowPrm rows are the floor's c168-c170 (BuildLightRows: the readers' rows, not the casters' c168 alone). False = no such state in the record (a counted hole; never a constant).
inline bool PrmFromRecord(const SceneTables& t, const float circle_vp[16], float prm[3][4], const char*& why, double tol = 1e-5) {
  // S103 KS1: the direct read first (kScPropsLight: the shadow module's light-1 record, read at the latch): it exists under OURS, where no model call ever sees the props light. An unusable row falls through to the search.
  for (size_t i = t.props_light.size(); i-- > 0;) {
    const PropsLight& pl = t.props_light[i];
    if ((pl.flags & (kPlRead | kPlHaveVp)) != (kPlRead | kPlHaveVp) || pl.version != kPropsLightLayout || g_prefer_direct_off) continue;
    bool same = true;   // paired with the circles by the light, as the search does: light 1's ViewProj row 3 must equal the circle entry's row 3 (a stale or other light's row falls back to the search)
    for (int k = 0; k < 4; ++k) {
      const double a = double(pl.vp_row3[k]), b = double(circle_vp[12 + k]);
      if (!(std::fabs(a - b) <= tol * std::fmax(1.0, std::fabs(b)))) same = false;
    }
    if (!same && !g_skip_pairing) continue;
    bool fin = true;
    for (int k = 0; k < 12; ++k) fin = fin && std::isfinite(pl.prm[k / 4][k % 4]);
    if (!fin || !(pl.prm[0][1] > pl.prm[0][0])) continue;
    std::memcpy(prm, pl.prm, sizeof pl.prm);
    why = "props light read directly from the shadow module's light-1 record (kScPropsLight)";
    return true;
  }
  if (t.light_states.empty()) {
    why = "props ShadowPrm: the record has no light states (the colour-call state that holds the props camera)";
    return false;
  }
  for (const LightState& st : t.light_states) {
    if (st.flags & kLsSynthetic) continue;   // a latch-written state: its shadow fields are zero
    const CasterRows cr = BuildCasterRows(st);
    bool same = true;   // each component on its own: |a - b| <= tol * max(1, |b|) (a joint scale of 50,000 would let the direction (0, 1, 0) drift by ~0.5)
    for (int k = 0; k < 4; ++k) {
      const double a = double(cr.view_proj[3][k]), b = double(circle_vp[12 + k]);
      if (!(std::fabs(a - b) <= tol * std::fmax(1.0, std::fabs(b)))) same = false;
    }
    if (!same) continue;
    const LightRows lr = BuildLightRows(st);
    std::memcpy(prm, lr.shadow_prm, sizeof lr.shadow_prm);
    for (int i = 0; i < 12; ++i)
      if (!std::isfinite(prm[i / 4][i % 4])) {
        why = "props ShadowPrm: the matching light state holds a non-finite value";
        return false;
      }
    if (!(prm[0][1] > prm[0][0])) {   // the receiver divides by (far - near): an empty or reversed range would put NaN on the floor
      why = "props ShadowPrm: the matching light state has no depth range (c168.y <= c168.x)";
      return false;
    }
    why = "colour-call light state holding the props camera";
    return true;
  }
  why = "props ShadowPrm: no colour light state holds the props camera (no light state's ViewProj row 3 equals the circle rows' row 3)";
  return false;
}

}  // namespace props_map
}  // namespace rdr_scene
