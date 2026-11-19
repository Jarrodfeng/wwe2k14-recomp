// S103 W1: the skin replay. For every wrestler body draw of the stream (main pass model parts), the game's skinned vertex positions (the draw's own c76 rows, the slot of each
// bone in the part's palette) are compared with OUR skinned positions: the model the front-end draws (assets::LoadWrestler through the viewer seam, the record's own link) skinned
// with the SCENE RECORD's palette by the model's node index, exactly as front_end.cpp does (slots = skin[node]). Reported per node group (body / hands+fingers / face) and per
// bone name, before / after. Read only. 0 game runs.
//   w1_skin_check --capture DIR --game DIR [--frames A-B] [--step N] [--pair K] [--bones] [--fault NAME]
// --pair K: the record index used for stream frame f is f - 1 + K (default 0, the pairing measured by w1_palette_census: stream f == record f-1).
// Faults (planted twins of the old behaviours): none yet beyond --pair.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_adapter/draw_replace_hook.h"
#include "../../src/renderer_scene/record_io.h"

using namespace rdr_adapter;
using rdr_scene::SceneTables;

static const float* Reg(const ConstantBank& b, int n) { return reinterpret_cast<const float*>(b.dwords) + 4 * n; }
static uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

static std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; }
// The node groups of a wrestler skeleton by NAME (the names are the game's: "Bip L Finger1", "l_mabuta", ...).
static const char* GroupOf(const std::string& name) {
  const std::string n = Lower(name);
  if (n.find("finger") != std::string::npos || n.find("thumb") != std::string::npos || n.find("fin") == 0 || n.find("hand") != std::string::npos) return "fingers";
  for (const char* k : {"eye", "mabuta", "meziri", "lip", "jaw", "mouth", "tongue", "cheek", "brow", "face", "mayu", "kuchi", "me_", "hoho", "nose", "ear"})
    if (n.find(k) != std::string::npos) return "face";
  return "body";
}

int main(int argc, char** argv) {
  std::string capture, game;
  int first = 0, last = -1, step = 1, pair = 0;
  bool per_bone = false, expect_fail = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--capture") capture = next();
    else if (a == "--game") game = next();
    else if (a == "--frames") std::sscanf(next(), "%d-%d", &first, &last);
    else if (a == "--step") step = std::max(1, std::atoi(next()));
    else if (a == "--pair") pair = std::atoi(next());
    else if (a == "--bones") per_bone = true;
    else if (a == "--expect-fail") expect_fail = true;   // the planted twin: the old pairing (--pair 1) must DIFFER
    else { std::fprintf(stderr, "unknown %s\n", a.c_str()); return 3; }
  }
  CaptureReader cap;
  if (!cap.Open(capture)) { std::fprintf(stderr, "cannot open capture\n"); return 2; }
  ViewerInit(game);
  const int nframes = int(cap.frame_count());
  if (last < 0 || last >= nframes) last = nframes - 1;

  struct Loaded { assets::Model m; bool ok = false; std::string why; };
  std::map<std::string, std::shared_ptr<Loaded>> models;
  auto load = [&](const std::string& pac, const std::string& entry, uint32_t variant) -> std::shared_ptr<Loaded> {
    const std::string key = pac + "|" + entry + "|" + std::to_string(variant);
    auto it = models.find(key);
    if (it != models.end()) return it->second;
    auto L = std::make_shared<Loaded>();
    int id = 0, k = 0, ord = int(variant);
    std::string norm = pac;
    std::replace(norm.begin(), norm.end(), '/', '\\');
    if (std::sscanf(norm.c_str(), "pac\\ch\\ch%d.pac", &id) == 1 || std::sscanf(norm.c_str(), "pac\\ch\\CH%d.PAC", &id) == 1) {
      if (std::sscanf(entry.c_str(), "model_key:%d", &k) == 1) {
        ord = ViewerOrdinalOfModelKey(id, k);
        if (ord < 0) ord = ViewerOrdinalOfModelKey(id, k - k % 10 + 2);
      }
      if (ord >= 0) L->ok = ViewerLoad("wrestler", id, ord, L->m, L->why);
      else L->why = "no model key";
    } else L->why = "not a ch pac: " + pac;
    models[key] = L;
    return L;
  };

  struct Acc { uint64_t verts = 0, bad = 0; double sum = 0, max = 0; };
  std::map<std::string, Acc> by_group, by_bone, pos_group;
  uint64_t pos_fits = 0;
  double pos_scale_min = 1e9, pos_scale_max = -1e9;
  uint64_t vcount_diff = 0, vcount_same = 0, draws = 0, no_link = 0, no_mesh = 0, nodes_mismatch = 0, name_mismatch = 0, slot_rows_differ = 0, slots = 0, slots_unmatched = 0, ambiguous = 0, idx_mismatch_slots = 0;
  std::map<std::string, int> names_wrong_seen;
  LoadedFrame fr;
  for (int f = first; f <= last; f += step) {
    const int ridx = f - 1 + pair;
    if (!cap.LoadFrame(uint32_t(f), fr) || ridx < 0 || !cap.HasSceneRecord(uint32_t(ridx))) continue;
    SceneTables t;
    std::string err;
    if (!rdr_scene::ReadRecord(cap.SceneRecordPath(uint32_t(ridx)).string(), t, err)) continue;
    for (uint32_t d = 0; d < fr.draws.count; ++d) {
      const DrawOrigin* org = fr.OriginOf(d);
      if (!org || (org->pass_site != kModelPartColourSite && org->pass_site != kModelPartMirrorSite) || org->object == 0) continue;
      const Draw& dr = fr.draws[d];
      if (dr.vs_bank >= fr.constant_banks.count) continue;
      int oi = -1;
      for (size_t i = 0; i < t.engine_objects.size(); ++i) if (t.engine_objects[i] == org->object) { oi = int(i); break; }
      if (oi < 0) continue;
      const rdr_scene::Object& ro = t.objects[size_t(oi)];
      if (!(ro.flags & rdr_scene::kObjSkinned) || ro.bones_count < 60) continue;
      ++draws;
      if (ro.asset >= t.assets.size() || t.assets[ro.asset].pac == 0) { if (no_link++ < 3) std::fprintf(stderr, "no asset link: object %s, %u nodes\n", std::string(t.Str(ro.id)).c_str(), ro.bones_count); continue; }
      const auto& ar = t.assets[ro.asset];
      auto L = load(std::string(t.Str(ar.pac)), std::string(t.Str(ar.entry)), ar.variant);
      if (!L->ok) { ++no_link; if (names_wrong_seen["load " + L->why]++ == 0) std::fprintf(stderr, "load failed: %s\n", L->why.c_str()); continue; }
      const assets::Model& m = L->m;
      if (m.bones.size() != ro.bones_count) { if (!nodes_mismatch++) std::fprintf(stderr, "model bones %zu != record bones %u\n", m.bones.size(), ro.bones_count); continue; }
      const assets::Mesh* mesh = nullptr;
      for (const assets::Mesh& me : m.meshes) if (me.src_record == int(org->part) && me.src_kind == 2 && me.src_key == 0) { mesh = &me; break; }
      if (!mesh) { ++no_mesh; continue; }
      if (mesh->vertices.size() != dr.vertex_count) {
        if (vcount_diff++ < 3) std::fprintf(stderr, "frame %d part %u: mesh %zu vertices, stream draw %u (asset %s %s)\n", f, org->part, mesh->vertices.size(), dr.vertex_count, std::string(t.Str(ar.pac)).c_str(), std::string(t.Str(ar.entry)).c_str());
      } else {
        ++vcount_same;
        // the stream's vertex positions (host offset 0, 4 x f32) against the loader's file positions: one scale per draw (the character size scale), then the residual per node group
        std::vector<uint8_t> vb;
        const VertexLayout& lay = fr.layouts[dr.layout];
        if (lay.host_stride >= 12 && cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vb) && vb.size() >= size_t(lay.host_stride) * dr.vertex_count) {
          double sab = 0, saa = 0;
          for (uint32_t v = 0; v < dr.vertex_count; ++v) {
            float p[3];
            std::memcpy(p, vb.data() + size_t(v) * lay.host_stride, 12);
            for (int c = 0; c < 3; ++c) { const double a = (c == 1 ? -1.0 : 1.0) * mesh->vertices[v].pos[c]; sab += double(p[c]) * a, saa += a * a; }
          }
          const double s = saa > 0 ? sab / saa : 1.0;
          if (getenv("W1_DEBUG") && pos_fits < 2) for (int v = 0; v < 4; ++v) { float p[3]; std::memcpy(p, vb.data() + size_t(v) * lay.host_stride, 12); std::fprintf(stderr, "stream %.4f %.4f %.4f  file %.4f %.4f %.4f (stride %u)\n", p[0], p[1], p[2], mesh->vertices[size_t(v)].pos[0], mesh->vertices[size_t(v)].pos[1], mesh->vertices[size_t(v)].pos[2], lay.host_stride); }
          ++pos_fits;
          pos_scale_min = std::min(pos_scale_min, s), pos_scale_max = std::max(pos_scale_max, s);
          for (uint32_t v = 0; v < dr.vertex_count; ++v) {
            float p[3];
            std::memcpy(p, vb.data() + size_t(v) * lay.host_stride, 12);
            double e = 0;
            for (int c = 0; c < 3; ++c) e = std::max(e, std::fabs(double(p[c]) - s * (c == 1 ? -1.0 : 1.0) * mesh->vertices[v].pos[c]));
            int dom = mesh->vertices[v].bones[0];
            float dw = -1.f;
            for (int k = 0; k < 4; ++k) if (mesh->vertices[v].weights[k] > dw) dw = mesh->vertices[v].weights[k], dom = mesh->vertices[v].bones[k];
            Acc& a = pos_group[GroupOf(m.bones[size_t(dom)].name)];
            ++a.verts, a.sum += e, a.max = std::max(a.max, e), a.bad += e > 1e-3;
          }
        }
      }
      // palette names of the slots
      std::vector<std::string> pal;
      { std::string cur; for (char c : mesh->palette) { if (c == ';') { pal.push_back(cur); cur.clear(); } else cur += c; } pal.push_back(cur); }
      const ConstantBank& bank = fr.constant_banks[dr.vs_bank];
      // slot -> record node by bits (rows of the draw vs the record's palette)
      std::vector<int> rec_node(pal.size(), -1);
      std::vector<std::array<float, 12>> rows(pal.size());
      for (size_t k = 0; k < pal.size(); ++k) {
        for (int j = 0; j < 3; ++j) for (int c = 0; c < 4; ++c) rows[k][size_t(j * 4 + c)] = Reg(bank, 76 + 3 * int(k) + j)[c];
        ++slots;
        for (uint32_t n = 0; n < ro.bones_count && rec_node[k] < 0; ++n) {
          const rdr_scene::Mat4& M = t.bones[ro.bones_first + n];
          bool same = true;
          for (int j = 0; j < 3 && same; ++j) for (int c = 0; c < 4; ++c) if (Bits(rows[k][size_t(j * 4 + c)]) != Bits(M.m[c * 4 + j])) { same = false; break; }
          if (same) rec_node[k] = int(n);
        }
        if (rec_node[k] < 0) ++slots_unmatched;
        else if (m.bones[size_t(rec_node[k])].name != pal[k]) {
          ++name_mismatch;
          if (names_wrong_seen["n " + pal[k] + " -> " + m.bones[size_t(rec_node[k])].name]++ == 0) std::fprintf(stderr, "slot %zu palette name '%s' but the record node %d is '%s'\n", k, pal[k].c_str(), rec_node[k], m.bones[size_t(rec_node[k])].name.c_str());
        }
      }
      // slot of a model bone: by name in the palette
      std::map<std::string, int> slot_of;
      for (size_t k = 0; k < pal.size(); ++k) { if (slot_of.count(pal[k])) ++ambiguous; slot_of[pal[k]] = int(k); }
      // replay
      auto skin_point = [&](const float p[3], const std::function<void(int, float*)>&) { (void)p; };
      (void)skin_point;
      for (const assets::Vertex& v : mesh->vertices) {
        float ours[3] = {0, 0, 0}, theirs[3] = {0, 0, 0}, ws = 0;
        int dom = 0; float dw = -1.f;
        bool usable = true;
        for (int k = 0; k < 4; ++k) {
          const float w = v.weights[k];
          if (!(w > 0.f)) continue;
          const int node = v.bones[k];
          if (node >= int(ro.bones_count)) { usable = false; break; }
          const rdr_scene::Mat4& M = t.bones[ro.bones_first + size_t(node)];
          const std::string& nm = m.bones[size_t(node)].name;
          auto sl = slot_of.find(nm);
          if (sl == slot_of.end()) { usable = false; break; }
          const auto& R = rows[size_t(sl->second)];
          for (int c = 0; c < 3; ++c) {
            ours[c] += w * (v.pos[0] * M.m[0 * 4 + c] + v.pos[1] * M.m[1 * 4 + c] + v.pos[2] * M.m[2 * 4 + c] + M.m[3 * 4 + c]);
            theirs[c] += w * (R[size_t(c * 4 + 0)] * v.pos[0] + R[size_t(c * 4 + 1)] * v.pos[1] + R[size_t(c * 4 + 2)] * v.pos[2] + R[size_t(c * 4 + 3)]);
          }
          ws += w;
          if (w > dw) dw = w, dom = node;
        }
        if (!usable || !(ws > 0.f)) continue;
        const double e = std::sqrt(double(ours[0] - theirs[0]) * (ours[0] - theirs[0]) + double(ours[1] - theirs[1]) * (ours[1] - theirs[1]) + double(ours[2] - theirs[2]) * (ours[2] - theirs[2]));
        const std::string& dn = m.bones[size_t(dom)].name;
        for (Acc* a : {&by_group[GroupOf(dn)], &by_bone[dn]}) { ++a->verts; a->sum += e; a->max = std::max(a->max, e); a->bad += e > 0.05; }   // 0.05 game units
      }
    }
  }
  std::printf("vertex counts: mesh == stream draw %llu, differ %llu\n", (unsigned long long)vcount_same, (unsigned long long)vcount_diff);
  std::printf("pair %d: wrestler body draws %llu, no link/model %llu, no mesh %llu, bone-count mismatch %llu, slots %llu (unmatched %llu, name mismatch %llu, ambiguous names %llu)\n", pair, (unsigned long long)draws, (unsigned long long)no_link,
              (unsigned long long)no_mesh, (unsigned long long)nodes_mismatch, (unsigned long long)slots, (unsigned long long)slots_unmatched, (unsigned long long)name_mismatch, (unsigned long long)ambiguous);
  std::printf("vertex positions: %llu draws fitted, scale %.5f .. %.5f\n", (unsigned long long)pos_fits, pos_scale_min, pos_scale_max);
  for (auto& kv : pos_group) std::printf("POSITIONS %s verts %llu off(>1e-3) %llu mean %.2e max %.2e\n", kv.first.c_str(), (unsigned long long)kv.second.verts, (unsigned long long)kv.second.bad, kv.second.sum / std::max<uint64_t>(1, kv.second.verts), kv.second.max);
  for (auto& kv : by_group) std::printf("GROUP %s verts %llu bad(>0.05) %llu mean %.5f max %.5f\n", kv.first.c_str(), (unsigned long long)kv.second.verts, (unsigned long long)kv.second.bad, kv.second.sum / std::max<uint64_t>(1, kv.second.verts), kv.second.max);
  if (per_bone) for (auto& kv : by_bone) std::printf("BONE %-20s verts %llu bad %llu mean %.5f max %.5f\n", kv.first.c_str(), (unsigned long long)kv.second.verts, (unsigned long long)kv.second.bad, kv.second.sum / std::max<uint64_t>(1, kv.second.verts), kv.second.max);
  // verdict: every palette slot of every wrestler body draw bit-equal to a record node of the same name, and no skinned vertex off by > 0.05 units, in every node group
  uint64_t bad_total = slots_unmatched + name_mismatch + (draws > no_link ? 0 : 1);
  for (auto& kv : by_group) bad_total += kv.second.bad;
  const bool ok = bad_total == 0 && slots > 0;
  std::printf("RESULT pair %d %s (slots %llu, bad %llu)\n", pair, ok ? "EQUAL" : "DIFFER", (unsigned long long)slots, (unsigned long long)bad_total);
  return expect_fail ? (ok ? 1 : 0) : (ok ? 0 : 1);
}
