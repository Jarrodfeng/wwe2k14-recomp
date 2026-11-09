// Session 98 (plan step 13, viewer side): the lighting EDIT MODEL of the Arena Viewer. Portable C++ (std + stage_key.h + json_lite.h).
// Holds the arena's light key (pac entry 30000, read from the player's own game files at run time; nothing is kept on disk but our
// own patch numbers), applies edits as PATCHES to a private copy (the same patches the game hook applies, docs\systems\lighting_mod.md
// section "Arena"), evaluates the edited key into light sets for the live scene, and writes / reads the mod file
// `<mods>\lighting\arena_NN.json` (format 1: key_size, key_fnv, key_fnv_data and the patches).
//
// How an edit maps onto the key (the animated case; Jarrod's plan: "scale all keys of a track by the edit"):
//   - colour (light k = track 10+k, sky = track 0, ground = track 1): the caller gives the wanted colour AT the viewer's key time T.
//     Every key of the track is multiplied, channel by channel, by wanted / (value at T). A static set (all keys equal) therefore
//     becomes exactly the wanted colour; an animated set keeps its shape (flicker, sweep) at the new level. A channel whose value
//     at T is ~0 cannot be scaled: its keys all become the wanted value (absolute). Scales are int16 = colour * 4095, clamped 0..32767.
//   - direction (lights only): the caller gives yaw / pitch in degrees at T = the file angles rz and ry (direction to the light =
//     Rz Ry Rx (-1,0,0) = (-cos ry cos rz, -cos ry sin rz, sin ry), so yaw = rz, pitch = ry; ax, the roll, does not move it). Every
//     key gets the same OFFSET (wanted - value at T), so an animated direction keeps its motion.
//   - the position field is never edited.
// A patch is one key (act, track, chunk) with `angle` and / or `scale`; `chunk` is the ordinal among the track's chunks of at least
// 25 bytes (stage_key.h Chunks, tools\assets\stagekey.py light_keys). ApplyPatches below is the viewer's copy of the game hook's
// patcher (src\lighting_mod.h ApplyPatches); tools\assets\lighting_mod_apply.py is the Python one.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "asset_model.h"
#include "pac.h"
#include "stage_key.h"
#include "view/json_lite.h"

namespace assets {
namespace lighting_edit {

inline uint64_t Fnv1a64(const uint8_t* d, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ d[i]) * 0x100000001b3ull;
  return h;
}
inline std::string Hex64(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%016llX", (unsigned long long)v);
  return b;
}

// Entry 30000 of the first PACH of the raw bgNN.pac file that has one (the same entry as tools\assets\stagekey.py arena_entries
// 'p0/30000'), decoded, in the FILE layout.
inline bool ReadArenaKey(const std::vector<uint8_t>& raw, Bytes& key, std::string& err) {
  std::vector<uint8_t> b;
  std::string e2;
  if (!wwe2k14::xctd::Unpack(raw, b, e2)) return err = e2, false;
  if (b.size() < 16) return err = "pac too small", false;
  size_t pos = std::memcmp(b.data(), "EPAC", 4) == 0 ? 0x4000 : 0;
  while (pos + 8 <= b.size()) {
    pac::Pach p;
    if (std::memcmp(b.data() + pos, "PACH", 4) != 0 || !pac::ParsePach(b, pos, b.size(), p)) {
      pos += 0x800;
      continue;
    }
    pos = (p.end + 0x7FF) & ~size_t(0x7FF);
    key = pac::Get(b, p, 30000);
    if (!key.empty()) return true;
  }
  return err = "this arena has no light key (entry 30000)", false;
}

struct Patch {
  int act = 0, track = 0, chunk = 0;
  bool has_pos = false, has_angle = false, has_scale = false;
  float pos[3] = {};  // never edited by the viewer; carried through load / save
  int angle[3] = {}, scale[3] = {};  // int16 units: angle 65536 = 360 degrees, scale = colour * 4095
};

// The game hook's patcher: overwrite the angle / scale payload of each patched key in `b`. Returns how many were applied.
inline int ApplyPatches(Bytes& b, const stage_key::Index& ix, const std::vector<Patch>& patches, int& skipped) {
  int applied = 0;
  skipped = 0;
  for (const Patch& p : patches) {
    const stage_key::Rec* r = ix.Find(p.act, p.track);
    size_t at = 0;
    bool found = false;
    if (r && p.chunk >= 0) {
      int seen = 0;
      for (const stage_key::Chunk& c : stage_key::Chunks(ix, *r)) {
        if (c.len < 25) continue;
        if (seen++ == p.chunk) {
          at = c.at, found = true;
          break;
        }
      }
    }
    if (!found || at + 25 > b.size()) {
      ++skipped;
      continue;
    }
    if (p.has_pos)
      for (int i = 0; i < 3; ++i) {
        uint32_t u;
        std::memcpy(&u, &p.pos[i], 4);
        for (int q = 0; q < 4; ++q) b[at + 1 + 4 * i + q] = uint8_t((u >> (8 * q)) & 0xFF);
      }
    if (p.has_angle)
      for (int i = 0; i < 3; ++i) b[at + 13 + 2 * i] = uint8_t(p.angle[i] & 0xFF), b[at + 14 + 2 * i] = uint8_t((p.angle[i] >> 8) & 0xFF);
    if (p.has_scale)
      for (int i = 0; i < 3; ++i) b[at + 19 + 2 * i] = uint8_t(p.scale[i] & 0xFF), b[at + 20 + 2 * i] = uint8_t((p.scale[i] >> 8) & 0xFF);
    ++applied;
  }
  return applied;
}

// Names where docs\systems\assets.md / out\runtime\s97\stage_key.md / entrance_light.h give a use; "?" = from the entrance slot
// remap only, not confirmed on screen.
inline std::string SetName(int id) {
  switch (id) {
    case 20: case 21: return "crowd";
    case 30: case 31: case 32: return "ring";
    case 100: return "default (black placeholder)";
    case 502: return "ring placeholder (black; blends to 31)";
    case 121: return "default rig / main canvas";
    case 200: return "entrance zone";
    case 201: return "ramp zone";
    default: break;
  }
  if (id >= 450 && id <= 454) return "entrance rig";
  return "";
}

inline int I16Wrap(double v) {  // a signed 16-bit value from any number (angles wrap)
  const long long r = (long long)std::llround(std::clamp(v, -1e12, 1e12));
  return int(int16_t(uint16_t(r & 0xFFFF)));
}

class Editor {
 public:
  Editor() = default;
  Editor(const Editor&) = delete;  // (ix_ points at work_)
  Editor& operator=(const Editor&) = delete;
  struct Ident {
    size_t size = 0;
    uint64_t fnv = 0, fnv_data = 0;
  };
  bool Loaded() const { return loaded_; }
  const Ident& Id() const { return id_; }
  size_t BaseOffset() const { return ix_.base; }

  bool Load(const Bytes& key, std::string& err) {
    loaded_ = false;
    patches_.clear();
    orig_ = key;
    work_ = key;
    if (!ix_.Parse(work_)) return err = "the light key does not parse", false;
    id_.size = orig_.size();
    id_.fnv = Fnv1a64(orig_.data(), orig_.size());
    id_.fnv_data = Fnv1a64(orig_.data() + ix_.base, orig_.size() - ix_.base);
    loaded_ = true;
    return true;
  }

  std::vector<int> SetIds() const {
    std::vector<int> v;
    for (const stage_key::Rec& r : ix_.rec)
      if ((r.track == 0 || r.track == 1 || (r.track >= 10 && r.track <= 14)) && std::find(v.begin(), v.end(), int(r.act)) == v.end()) v.push_back(r.act);
    std::sort(v.begin(), v.end());
    return v;
  }
  bool HasLight(int id, int k) const { return ix_.Find(id, 10 + k) != nullptr; }

  // The (edited) set at key time `t` (game frames). `orig` = the unedited key.
  bool Eval(int id, double t, LightSet& out, bool orig = false) const {
    if (!loaded_) return false;
    if (!orig) return stage_key::EvalLightSet(ix_, id, t, out);
    stage_key::Index oi;
    return oi.Parse(orig_) && stage_key::EvalLightSet(oi, id, t, out);
  }
  // yaw / pitch (degrees) = the file angles rz / ry of light k at time t.
  bool Angles(int id, int k, double t, double& yaw, double& pitch) const {
    const stage_key::Rec* r = ix_.Find(id, 10 + k);
    if (!r) return false;
    const stage_key::Sample s = stage_key::SampleTrack(ix_, *r, t, true);
    if (!s.ok) return false;
    yaw = s.v[2] * 360.0 / 65536.0, pitch = s.v[1] * 360.0 / 65536.0;
    return true;
  }

  // ---- edits (all return false with err on an unknown set / light) ----
  // track: 0 sky, 1 ground, 10..13 light colour.
  bool SetColor(int id, int track, const float want[3], double t, std::string& err) {
    const stage_key::Rec* r = ix_.Find(id, track);
    if (!loaded_ || !r) return err = "set " + std::to_string(id) + " has no track " + std::to_string(track), false;
    const stage_key::Sample cur = stage_key::SampleTrack(ix_, *r, t, true);
    if (!cur.ok) return err = "track has no keys", false;
    int ordinal = 0;
    for (const stage_key::Chunk& c : stage_key::Chunks(ix_, *r)) {
      if (c.len < 25) continue;
      Patch& p = Slot(id, track, ordinal++);
      for (int ch = 0; ch < 3; ++ch) {
        const int key_v = stage_key::I16(work_, c.at + 19 + 2 * ch);
        const double have = cur.sc[ch], target = std::max(0.0, double(want[ch]) * 4095.0);
        double nv = std::fabs(have) < 4.0 ? target : key_v * (target / have);
        p.scale[ch] = int(std::lround(std::clamp(nv, 0.0, 32767.0)));
      }
      p.has_scale = true;
    }
    Rebuild();
    return true;
  }
  bool SetDir(int id, int k, double yaw_deg, double pitch_deg, double t, std::string& err) {
    const stage_key::Rec* r = ix_.Find(id, 10 + k);
    if (!loaded_ || !r) return err = "set " + std::to_string(id) + " has no light " + std::to_string(k), false;
    double cy, cp;
    if (!Angles(id, k, t, cy, cp)) return err = "track has no keys", false;
    const double dy = (yaw_deg - cy) * 65536.0 / 360.0, dp = (pitch_deg - cp) * 65536.0 / 360.0;
    // An offset result stays UNWRAPPED while it fits int16 (so a key and its neighbour keep the same short way round between
    // them). A key that would leave the range is wrapped; then the edit is flagged (LastSwing): that key may swing the long way.
    swing_ = false;
    auto put = [&](double v) {
      if (v >= -32768.0 && v <= 32767.0) return int(std::lround(v));
      swing_ = true;
      return I16Wrap(v);
    };
    int ordinal = 0;
    for (const stage_key::Chunk& c : stage_key::Chunks(ix_, *r)) {
      if (c.len < 25) continue;
      Patch& p = Slot(id, 10 + k, ordinal++);
      p.angle[0] = stage_key::I16(work_, c.at + 13);
      p.angle[1] = put(stage_key::I16(work_, c.at + 15) + dp);
      p.angle[2] = put(stage_key::I16(work_, c.at + 17) + dy);
      p.has_angle = true;
    }
    Rebuild();
    return true;
  }
  bool LastSwing() const { return swing_; }  // the last SetDir had to wrap a key past +-180 degrees: that key may swing the long way
  void ResetSet(int id) {
    for (auto it = patches_.begin(); it != patches_.end();) it = std::get<0>(it->first) == id ? patches_.erase(it) : std::next(it);
    Rebuild();
  }
  void ResetAll() {
    patches_.clear();
    Rebuild();
  }
  bool Edited(int id) const {
    for (const auto& kv : patches_)
      if (std::get<0>(kv.first) == id) return true;
    return false;
  }
  size_t PatchCount() const { return patches_.size(); }
  std::vector<Patch> Patches() const {
    std::vector<Patch> v;
    for (const auto& kv : patches_) v.push_back(kv.second);
    return v;
  }
  // Replace the edits (a loaded mod). Patches that do not fit the key are dropped; returns how many were kept.
  int SetPatches(const std::vector<Patch>& in) {
    patches_.clear();
    for (const Patch& p : in) {
      const stage_key::Rec* r = ix_.Find(p.act, p.track);
      if (!r || (!p.has_pos && !p.has_angle && !p.has_scale)) continue;
      int n = 0;
      for (const stage_key::Chunk& c : stage_key::Chunks(ix_, *r))
        if (c.len >= 25) ++n;
      if (p.chunk < 0 || p.chunk >= n) continue;
      patches_[{p.act, p.track, p.chunk}] = p;
    }
    Rebuild();
    return int(patches_.size());
  }

  // ---- the mod file ----
  std::string ToJson(int arena, const std::string& name) const {
    std::string s = "{\n \"format\": 1,\n \"kind\": \"arena\",\n \"arena\": " + std::to_string(arena) + ",\n \"name\": \"" + jl::Esc(name) +
                    "\",\n \"key_size\": " + std::to_string(id_.size) + ",\n \"key_fnv\": \"" + Hex64(id_.fnv) + "\",\n \"key_fnv_data\": \"" +
                    Hex64(id_.fnv_data) + "\",\n \"patches\": [";
    bool first = true;
    for (const auto& kv : patches_) {
      const Patch& p = kv.second;
      s += first ? "\n  " : ",\n  ";
      first = false;
      s += "{\"act\": " + std::to_string(p.act) + ", \"track\": " + std::to_string(p.track) + ", \"chunk\": " + std::to_string(p.chunk);
      if (p.has_pos) {
        char b[96];
        std::snprintf(b, sizeof b, "[%.9g, %.9g, %.9g]", double(p.pos[0]), double(p.pos[1]), double(p.pos[2]));
        s += std::string(", \"pos\": ") + b;
      }
      if (p.has_angle) s += ", \"angle\": [" + std::to_string(p.angle[0]) + ", " + std::to_string(p.angle[1]) + ", " + std::to_string(p.angle[2]) + "]";
      if (p.has_scale) s += ", \"scale\": [" + std::to_string(p.scale[0]) + ", " + std::to_string(p.scale[1]) + ", " + std::to_string(p.scale[2]) + "]";
      s += "}";
    }
    return s + "\n ]\n}\n";
  }
  // Reads a mod back into the editor. The file must belong to this key (key_fnv or key_fnv_data, and key_size when present).
  bool FromJson(const std::string& text, std::string& err) {
    jl::Value j;
    std::string pe;
    if (!jl::Parse(text, j, pe) || j.t != jl::Value::Obj) return err = "not a JSON object: " + pe, false;
    if (j.str("kind", "arena") != "arena") return err = "not an arena mod (kind)", false;
    auto hex = [](const std::string& s, uint64_t& v) {
      char* e = nullptr;
      v = std::strtoull(s.c_str(), &e, 16);
      return e && e != s.c_str();
    };
    uint64_t f = 0;
    bool match = false, any = false;
    if (hex(j.str("key_fnv"), f)) any = true, match |= f == id_.fnv;
    if (hex(j.str("key_fnv_data"), f)) any = true, match |= f == id_.fnv_data;
    if (!any) return err = "the mod has no key_fnv", false;
    if (!match) return err = "the mod is for a different light key (key_fnv does not match this arena)", false;
    if (j.has("key_size") && size_t(j.num("key_size", 0)) != id_.size) return err = "key_size does not match", false;
    const jl::Value* pl = j.get("patches");
    if (!pl || pl->t != jl::Value::Arr) return err = "no patches", false;
    std::vector<Patch> v;
    for (const jl::Value& e : pl->a) {
      // (as the game reads it, src\lighting_mod.h LoadArenaMod: act and track must be numbers, else the patch is dropped)
      if (e.t != jl::Value::Obj || !e.get("act") || !e.get("track") || e.get("act")->t != jl::Value::Num || e.get("track")->t != jl::Value::Num) continue;
      Patch p;
      p.act = int(e.num("act", 0)), p.track = int(e.num("track", 0)), p.chunk = int(e.num("chunk", 0));
      // Vec rule of the game: at least n entries, every one a number, else the WHOLE array is ignored. Clamp, then round.
      auto vec = [&](const char* k, double* o) {
        const jl::Value* a = e.get(k);
        if (!a || a->t != jl::Value::Arr || a->a.size() < 3) return false;
        for (int i = 0; i < 3; ++i) {
          if (a->a[i].t != jl::Value::Num) return false;
          o[i] = a->a[i].n;
        }
        return true;
      };
      double q3[3];
      if (vec("pos", q3)) {
        p.has_pos = true;
        for (int i = 0; i < 3; ++i) p.pos[i] = float(q3[i]);
      }
      if (vec("angle", q3)) {
        p.has_angle = true;
        for (int i = 0; i < 3; ++i) p.angle[i] = int(std::lround(std::clamp(q3[i], -32768.0, 32767.0)));
      }
      if (vec("scale", q3)) {
        p.has_scale = true;
        for (int i = 0; i < 3; ++i) p.scale[i] = int(std::lround(std::clamp(q3[i], -32768.0, 32767.0)));
      }
      v.push_back(p);
    }
    SetPatches(v);
    return true;
  }

 private:
  Patch& Slot(int act, int track, int chunk) {
    Patch& p = patches_[{act, track, chunk}];
    p.act = act, p.track = track, p.chunk = chunk;
    return p;
  }
  void Rebuild() {
    std::memcpy(work_.data(), orig_.data(), orig_.size());
    std::vector<Patch> v;
    for (const auto& kv : patches_) v.push_back(kv.second);
    int skipped = 0;
    ApplyPatches(work_, ix_, v, skipped);
  }

  bool swing_ = false;
  bool loaded_ = false;
  Ident id_;
  Bytes orig_, work_;
  stage_key::Index ix_;  // points at work_ (same size forever: Rebuild copies in place)
  std::map<std::tuple<int, int, int>, Patch> patches_;
};

}  // namespace lighting_edit
}  // namespace assets
