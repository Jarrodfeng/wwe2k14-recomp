// Which light object a model's draw installs, resolved from the record (Session 103, lane E4; docs\renderer\INPUTS_A3.md 4b, INPUTS_A2.md 3.1). Pure: scene_record.h only; header only; no allocation.
//
// The game installs, before a node's model calls, the object sub_826203E8(index, mode) picks: (1) with the gate object (word 0x836F53D0) null or its +152 flag zero, the mode's override word
// (0x83622668 mode 0, 0x8362266C mode 1) replaces the index when it is >= 0; (2) an index >= 0 goes to sub_827F6828: 10010 with the manager's mgr+372 != 0 -> the override object; an index in 0..n-1
// whose pool remap ((mgr+120)[index]) is not -1 -> the pool object; else the set (mgr+112)[index]; anything else none [C listings, A2 3.1]. The record has the objects as kLoSet / kLoPool / kLoOverride rows and
// the manager row's words map each to its row (kLmRefBase.., kLmSetInfoBase..); A3's kLoModelMap rows give (model, light object, raw index, mode) per model, read through GetModelMapEntry. This header applies the game's rule to those rows.
// [C] only as far as the listings and the unit's synthetic rows go: no capture on disk carries these rows (A3's reader is not run yet) [U].
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "scene_record.h"

namespace rdr_scene {

inline constexpr uint32_t kInstallOverrideId = 10010;
// The key light from the light OBJECTS is OFF (review F1 of lane E4): the wrestler draw nodes (sub_8259FAE8 at 0x8259FBE8, sub_825A3908 at 0x825A3A68) install through sub_82620488(node+3936 / node+2736, mode 0),
// which calls sub_826203E8 only when UNGATED with an override word >= 0; otherwise (0x82620520) it installs the node's own EMBEDDED object through sub_8261FE90, and that object is in none of A2's rows.
// Stays OFF (S103 probe KEY, docs/agents/s103/KEY.md): the A8 / L1C wrestler-light latch already reads that embedded object every latch and supplies the key on OURS frames, CAW editor included;
// flipping this would only duplicate it on the override path and bring back review F1's wrong-object key on transient frames.
inline constexpr bool kKeyFromLightObjects = false;

// S103 E8: the ONE key-light test (shadow_fit_record.h, KeyLightOfModel, KeyLightOfWrestler). A key is a finite UNIT direction with a horizontal part >= 0.2 (an unset light is (0, 0, 0), a vertical one has no azimuth);
// the w lane is NOT a key marker (run 28's wrestler states store w = 0 on every light; the game's KeyLightVec carries 0 and 1). WWE_RDR_E8_OLD_KEY_RULE=1 (the dev twin of tools\scene\e8_check.py) = the old `w == 1` as well.
inline bool IsKeyDirection(const float k[4]) {
  static const bool old_rule = [] {   // _dupenv_s on Windows: std::getenv is deprecated there and the -Werror tools (tail_order) failed on it
#ifdef _WIN32
    char* v = nullptr; size_t n = 0;
    const bool set = _dupenv_s(&v, &n, "WWE_RDR_E8_OLD_KEY_RULE") == 0 && v != nullptr;
    std::free(v);
    return set;
#else
    return std::getenv("WWE_RDR_E8_OLD_KEY_RULE") != nullptr;
#endif
  }();
  if (!(std::isfinite(k[0]) && std::isfinite(k[1]) && std::isfinite(k[2]))) return false;
  const float len2 = k[0] * k[0] + k[1] * k[1] + k[2] * k[2];
  if (old_rule && std::fabs(k[3] - 1.f) > 1e-3f) return false;
  return std::fabs(len2 - 1.f) <= 0.1f && std::sqrt(k[0] * k[0] + k[2] * k[2]) >= 0.2f;
}

// S103 CE (bit 7): the ONE usable-light-state test, shared by the draw (rf_wrestlers.cpp WrestlerLightState) and the latch's stamp (scene_live\wrestler_light_latch.h EntryUsable). `light` = a kScLightStates block (kLsDir / kLsCol rows).
// Usable = finite directions and colours of lights 0-4, colour sum of lights 0-3 > 0, and the key (light 4) is a direction OR the game switched the key light OFF (key colour r = g = b = 0: the entrances' slots 7 / 8, role-4
// characters, carry direction (0, 0, 0) and colour (0, 0, 0); a zero-colour key adds nothing, its direction is irrelevant). A zero direction with a NON-zero key colour stays unusable. docs\research\s103\CE_entrance_inputs.md 1.
// `key_off_counts` = false is the planted twin (the old rule).
inline bool KeyLightOff(const float* light, bool by_direction = false) {   // by_direction = the planted wrong rule (a zero DIRECTION, whatever the colour)
  if (by_direction) return light[kLsDir + 16] == 0.f && light[kLsDir + 17] == 0.f && light[kLsDir + 18] == 0.f;
  return light[kLsCol + 16] == 0.f && light[kLsCol + 17] == 0.f && light[kLsCol + 18] == 0.f;
}
inline bool LightStateUsable(const float* light, bool key_off_counts = true, bool key_off_by_direction = false) {
  for (uint32_t k = 0; k < 20; ++k)
    if (!std::isfinite(light[kLsDir + k]) || !std::isfinite(light[kLsCol + k])) return false;
  float csum = 0.f;
  for (uint32_t k = 0; k < 4; ++k)
    for (uint32_t c = 0; c < 3; ++c) csum += light[kLsCol + 4 * k + c];
  if (!(csum > 0.f)) return false;
  return IsKeyDirection(light + kLsDir + 16) || (key_off_counts && KeyLightOff(light, key_off_by_direction));
}
// The state has a key the shadow fit can use (KeyLightFromRecord's own rule: a key direction), and the key is lit.
inline bool HasKey(const float* light) {
  return IsKeyDirection(light + kLsDir + 16) && (light[kLsCol + 16] + light[kLsCol + 17] + light[kLsCol + 18]) > 0.f;
}

// Planted faults of the unit (each must fail its named check).
struct InstallFaults {
  bool ignore_remap = false;     // the pool remap of a set is ignored: the set's own object is used
  bool ignore_gate = false;      // the gate is ignored: the override words always apply
  bool ignore_override = false;  // the mode override words are never applied
};

// The manager row (kLoManager) of a record's light objects, or null.
inline const LightObjectRow* LightManagerRow(const std::vector<LightObjectRow>& rows) {
  for (const LightObjectRow& r : rows)
    if (r.kind == kLoManager && (r.flags & kLoBlockRead)) return &r;
  return nullptr;
}

// The block row the install (index, mode) selects, or false with a reason.
inline bool ResolveInstallRow(const std::vector<LightObjectRow>& rows, uint32_t index, uint32_t mode, const LightObjectRow*& out, const char*& why, const InstallFaults* ft = nullptr) {
  out = nullptr;
  const LightObjectRow* m = LightManagerRow(rows);
  if (!m) {
    why = "no light manager row in the record";
    return false;
  }
  const bool gated = (m->words[kLmGateObject] != 0 && m->words[kLmGateFlag] != 0) && !(ft && ft->ignore_gate);   // the override words are skipped under the gate
  if (!gated && mode <= 1 && !(ft && ft->ignore_override)) {   // review F3: only modes 0 and 1 have an override word (0x82620410-0x82620428)
    const uint32_t ov = m->words[mode == 0 ? kLmOverrideMode0 : kLmOverrideMode1];
    if (ov != 0xFFFFFFFFu && int32_t(ov) >= 0) index = ov;   // 0xFFFFFFFF = unread (0 is a valid index)
  }
  if (int32_t(index) < 0) {
    why = "install index < 0: the game installs nothing";
    return false;
  }
  uint32_t ref = kLightNoRow;
  const uint32_t n = m->aux0 < kLightMaxSets ? m->aux0 : kLightMaxSets;
  if (index == kInstallOverrideId && m->remap != 0) {
    ref = m->words[kLmRefBase + kLmRefOverride];
  } else if (index < n) {
    const uint32_t r1 = m->words[kLmSetInfoBase + index] >> 16;   // pool remap + 1; 0 = no remap
    if (r1 > kLightMaxPool && !(ft && ft->ignore_remap)) {   // review F4: a remap out of range is a hole, not the set's own object
      why = "the set's pool remap is out of range";
      return false;
    }
    if (r1 != 0 && r1 <= kLightMaxPool && !(ft && ft->ignore_remap)) ref = m->words[kLmRefBase + kLightMaxSets + (r1 - 1)];
    else ref = m->words[kLmRefBase + index];
  } else {
    why = "install index selects no object (sub_827F6828 returns 0)";
    return false;
  }
  if (ref == kLightNoRow || ref >= rows.size() || rows[ref].kind == kLoManager || rows[ref].kind == kLoModelMap) {
    why = "the selected object has no readable block in the record";
    return false;
  }
  out = &rows[ref];
  why = "";
  return true;
}

// The (raw index, mode) the learned table holds for `model` (an engine object address), or false.
inline bool ModelInstall(const std::vector<LightObjectRow>& rows, uint32_t model, uint32_t& index, uint32_t& mode) {
  for (const LightObjectRow& r : rows) {
    if (r.kind != kLoModelMap) continue;
    const uint32_t n = r.aux0 < kLightModelsPerRow ? r.aux0 : kLightModelsPerRow;
    for (uint32_t k = 0; k < n; ++k)
    {
      const ModelMapEntry e = GetModelMapEntry(r.words, k);   // the ONE accessor (scene_record.h): entries are (model, object, index, mode), 56 a row (round 5); E4 still resolves by (index, mode)
      if (e.model == model) {
        index = e.index, mode = e.mode;
        return true;
      }
    }
  }
  return false;
}

// The wrestler's KeyLightVec (R dir 4 of the object its model's install selects): a unit direction with a horizontal part >= 0.2 (IsKeyDirection, shared with KeyLightFromRecord).
inline bool KeyLightOfModel(const std::vector<LightObjectRow>& rows, uint32_t model, float key[3], const char*& why, const InstallFaults* ft = nullptr) {
  uint32_t index = 0, mode = 0;
  if (!ModelInstall(rows, model, index, mode)) {
    why = "no install learned for this wrestler's model (kLoModelMap): a model created after OURS began has none";
    return false;
  }
  const LightObjectRow* obj = nullptr;
  if (!ResolveInstallRow(rows, index, mode, obj, why, ft)) return false;
  float k[4];
  std::memcpy(k, &obj->words[kLsDir + 4 * 4], sizeof k);
  if (!IsKeyDirection(k)) {
    why = "the installed object has no key light (not a unit direction, or no azimuth)";
    return false;
  }
  key[0] = k[0], key[1] = k[1], key[2] = k[2];
  why = "light object installed on the wrestler's model (kLoModelMap + the game's selection rule)";
  return true;
}

// The WRESTLER's key light: its draw nodes install through sub_82620488(embedded object, mode 0): the override word of mode 0 (0x83622668) selects the object only when UNGATED and >= 0; otherwise the node's own
// embedded object is installed, which is in no record: a hole. Every wrestler shares the one override word, so the answer does not depend on the model.
inline bool KeyLightOfWrestler(const std::vector<LightObjectRow>& rows, float key[3], const char*& why, const InstallFaults* ft = nullptr) {
  const LightObjectRow* m = LightManagerRow(rows);
  if (!m) {
    why = "no light manager row in the record";
    return false;
  }
  const bool gated = (m->words[kLmGateObject] != 0 && m->words[kLmGateFlag] != 0) && !(ft && ft->ignore_gate);
  const uint32_t ov = m->words[kLmOverrideMode0];
  if (gated || ov == 0xFFFFFFFFu || int32_t(ov) < 0 || (ft && ft->ignore_override)) {
    why = "the wrestler's node installs its own EMBEDDED object (sub_82620488 0x82620520): not in the record";
    return false;
  }
  const LightObjectRow* obj = nullptr;
  if (!ResolveInstallRow(rows, ov, 0, obj, why, ft)) return false;
  float k[4];
  std::memcpy(k, &obj->words[kLsDir + 4 * 4], sizeof k);
  if (!IsKeyDirection(k)) {
    why = "the installed object has no key light (not a unit direction, or no azimuth)";
    return false;
  }
  key[0] = k[0], key[1] = k[1], key[2] = k[2];
  why = "light object selected by the mode-0 override word (the wrestler node's sub_826203E8 path)";
  return true;
}

// Bit 7 (caster_entries) is ready when there is at least one wrestler model and EVERY one has a key light from the objects; `first_key` = the first model's key (they agree in every capture seen).
inline bool CasterInputsReady(const std::vector<LightObjectRow>& rows, const uint32_t* wrestler_models, size_t count, float first_key[3], const char*& why) {
  if (count == 0) {
    why = "no wrestler model known";
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    float k[3];
    (void)wrestler_models[i];   // every wrestler shares the mode-0 override word
    if (!KeyLightOfWrestler(rows, k, why)) return false;
    if (i == 0) first_key[0] = k[0], first_key[1] = k[1], first_key[2] = k[2];
  }
  return true;
}

}  // namespace rdr_scene
