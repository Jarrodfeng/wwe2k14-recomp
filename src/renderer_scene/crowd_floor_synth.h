// TEST ONLY, never live (Session 101, lane 82; docs/renderer/CROWD_FAMILY_8276ED60.md section 4B): the seated floor people added to a COPY of a scene record, so
// the real crowd seam (CrowdOurs with the floor switch) can be proven offline on Check B, whose records were written before the floor observer existed and hold
// no floor members. fview --crowd-floor-synth FILE reads FILE (written by tools/crowd/floor_join.py --emit) and, for the stream frame f the session replays, adds
// that frame's floor members to the record the pairing names (j-1): one Object "crowd:floor:TT:member:I" per floor draw, the world = the draw's own c76-c78 (bit for bit, the
// hex words in the file), the type from the seat join with the player's own seat text (floor_join.py), the type's LOD 0 body asset and its LOD 0 palette taken from the
// record the way the live contributor would give it (a pose object / a card / a stands LOD 0 member of that type that already carries it; none = the member has no palette).
// The live path does not use this file: crowd_hooks.h + crowd_floor.h record the real members.
//
// File format (text, one member per line, '#' lines ignored): `frame type index w0 w1 ... w11` with the frame decimal and w0..w11 the 12 floats of c76-c78 as 8 hex digits (their bits).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "record_io.h"

namespace rdr_scene {

class FloorSynth {
 public:
  bool Load(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
      error = "cannot open " + path;
      return false;
    }
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      Row r;
      uint32_t frame = 0;
      if (!(ss >> frame >> r.type >> r.index)) {  // (S101 lane 82 follow-up: a bad number fails cleanly)
        error = path + ": bad line '" + line.substr(0, 60) + "'";
        return false;
      }
      for (int k = 0; k < 12; ++k) {
        std::string w;
        if (!(ss >> w)) {
          error = path + ": a line has fewer than 12 words";
          return false;
        }
        try {
          r.bits[k] = uint32_t(std::stoul(w, nullptr, 16));
        } catch (const std::exception&) {
          error = path + ": bad hex word '" + w.substr(0, 20) + "'";
          return false;
        }
      }
      if (r.type >= 20) {
        error = path + ": type out of range";
        return false;
      }
      rows_[frame].push_back(r);
      ++n;
    }
    if (n == 0) {
      error = path + ": no floor members in the file (a check that reads nothing must fail)";
      return false;
    }
    total_ = n;
    return true;
  }
  size_t total() const { return total_; }
  size_t RowsFor(uint32_t frame) const {
    const auto it = rows_.find(frame);
    return it == rows_.end() ? 0 : it->second.size();
  }

  // Adds the floor members of stream frame `frame` to `t`. Returns how many were added.
  uint32_t Apply(uint32_t frame, SceneTables& t) const {
    const auto it = rows_.find(frame);
    if (it == rows_.end()) return 0;
    // The arena's pac string, from any crowd asset; the LOD 0 body asset per type (made when the record has none); the LOD 0 palette per type from the record's own objects.
    uint32_t pac = 0;
    for (const AssetRef& a : t.assets)
      if (a.kind == uint8_t(AssetKind::Model) && std::string_view(t.Str(a.pac)).rfind("pac\\bg\\bg", 0) == 0) {
        pac = a.pac;
        break;
      }
    uint32_t asset_of[20];
    for (uint32_t& a : asset_of) a = 0xFFFFFFFFu;
    int score[20];
    uint32_t bones_first[20] = {}, bones_count[20] = {};
    for (int& s : score) s = -1;
    for (uint32_t i = 0; i < t.assets.size(); ++i) {
      unsigned n = 0;
      const std::string e(t.Str(t.assets[i].entry));
      if (e.rfind("card:", 0) != 0 && std::sscanf(e.c_str(), "%u", &n) == 1 && n >= 20000 && n < 20020) asset_of[n - 20000] = i;
    }
    for (const Object& o : t.objects) {
      if (o.klass != rdr_adapter::DrawClass::Crowd || !(o.flags & kObjSkinned) || !o.bones_count || o.asset >= t.assets.size()) continue;
      const std::string e(t.Str(t.assets[o.asset].entry));
      unsigned type = 0, n = 0;
      int s = -1;
      if (e.rfind("card:", 0) == 0 && std::sscanf(e.c_str(), "card:%u", &type) == 1) s = 8;
      else if (std::sscanf(e.c_str(), "%u", &n) == 1 && n >= 20000 && n < 20020) type = n - 20000, s = (o.flags & kObjVisible) ? 7 : 10;  // (a LOD 0 member / the pose object)
      if (s < 0 || type >= 20 || s <= score[type]) continue;
      score[type] = s, bones_first[type] = o.bones_first, bones_count[type] = o.bones_count;
    }
    uint32_t added = 0;
    for (const Row& r : it->second) {
      if (asset_of[r.type] == 0xFFFFFFFFu) {
        AssetRef a;
        a.kind = uint8_t(AssetKind::Model);
        a.pac = pac;
        a.entry = t.AddString(std::to_string(20000u + r.type));
        t.assets.push_back(a);
        asset_of[r.type] = uint32_t(t.assets.size() - 1);
      }
      Object o;
      o.asset = asset_of[r.type];
      o.klass = rdr_adapter::DrawClass::Crowd;
      o.layer = 0;
      o.flags = kObjVisible;
      float c[12];
      std::memcpy(c, r.bits, sizeof c);
      o.world = IdentityMat4();
      for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 4; ++k) o.world.m[k * 4 + row] = c[row * 4 + k];  // the same layout as crowd_contrib.h WorldFromConstants
      o.world.m[15] = 1.f;
      char id[64];
      std::snprintf(id, sizeof id, "crowd:floor:%u:member:%u", r.type, r.index);
      o.id = t.AddString(id);
      if (score[r.type] >= 0) o.flags |= kObjSkinned, o.bones_first = bones_first[r.type], o.bones_count = bones_count[r.type];
      t.objects.push_back(o);
      if (!t.engine_objects.empty()) t.engine_objects.push_back(0);  // (the parallel array: no engine object)
      ++added;
    }
    return added;
  }

 private:
  struct Row {
    uint32_t type = 0, index = 0;
    uint32_t bits[12] = {};
  };
  std::map<uint32_t, std::vector<Row>> rows_;
  size_t total_ = 0;
};

}  // namespace rdr_scene
