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
#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/body_scale.h"

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
  bool per_bone = false, expect_fail = false, plant_no_scale = false, print_scales = false;
  bool latch_mode = false, latch_wrong_slot = false;   // S103 W3: the scale comes from the record path (the latch, k/4096-quantised like the game's float), not the stream; the twin hands each object the PREVIOUS object's value (a wrong slot)
  float latch_prev = 1.0f;
  std::set<uint32_t> latch_done;
  std::set<uint32_t> scales_printed;
  std::map<std::string, std::pair<int, int>> told_links;
  std::map<uint32_t, int> row_map;
  std::map<uint32_t, float> told_scale;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--capture") capture = next();
    else if (a == "--game") game = next();
    else if (a == "--frames") std::sscanf(next(), "%d-%d", &first, &last);
    else if (a == "--step") step = std::max(1, std::atoi(next()));
    else if (a == "--pair") pair = std::atoi(next());
    else if (a == "--bones") per_bone = true;
    else if (a == "--link") { const std::string v = next(); const size_t eq = v.find('='); int ch = 0, at = 0; if (eq != std::string::npos && std::sscanf(v.c_str() + eq + 1, "%d:%d", &ch, &at) == 2) told_links[v.substr(0, eq)] = {ch, at}; else { std::fprintf(stderr, "bad --link %s (ID=CH:AT)\n", v.c_str()); return 3; } }   // S103 W2: TOLD model of a record object whose record names none
    else if (a == "--scales") print_scales = true;   // S103 W2: print `BODYSCALE 0xOBJECT s` per object the stream measures (the offline rig feeds them to BodyScaleTable: tools\scene\frame_rig.py)
    else if (a == "--hole-selftest" || a == "--hole-twin") {   // S103 W2: the hole decision (body_scale.h BodyScaleFor), no capture needed
      const bool twin = a == "--hole-twin";
      auto& T = rdr_scene::BodyScaleTable::Get();
      T.Clear();
      int bad = 0;
      auto expect = [&](const char* what, float got, bool want_hole) { const bool hole = !(got > 0.f); std::printf("%s %s: s %.4f %s\n", hole == want_hole ? "ok  " : "FAIL", what, got, hole ? "HOLE" : "drawn"); bad += hole != want_hole; };
      expect("live record, scale unknown", rdr_scene::BodyScaleFor(true, 0x1234, false, twin), true);
      expect("live record, plant 'unknown'", rdr_scene::BodyScaleFor(true, 0x1234, true, twin), true);
      T.Set(0x1234, 0.89453f);
      expect("live record, scale known", rdr_scene::BodyScaleFor(true, 0x1234, false, twin), false);
      expect("live record, other object still unknown", rdr_scene::BodyScaleFor(true, 0x9999, false, twin), true);
      expect("viewer record needs no game scale", rdr_scene::BodyScaleFor(false, 0x9999, false, twin), false);
      {  // the census (never used for drawing): latch vs the stream-learned scale
        T.Clear();
        T.Set(0xA1, 0.89453f); T.SetLatch(0xA1, 0, 0.89453f, true);   // equal
        T.Set(0xA2, 1.01050f); T.SetLatch(0xA2, 1, 0.92090f, true);   // differ
        T.SetLatch(0xA3, 2, 0.95776f, true);                          // the stream has not measured it: unjoined
        T.SetLatch(0xA4, 3, 0.f, false);                              // unreadable
        const auto r = T.Census();
        const std::string line = rdr_scene::BodyScaleTable::CensusText(r);
        std::printf("census: %s\n", line.c_str());
        const bool good = r.same == 1 && r.differ == 1 && r.unjoined == 1 && r.unreadable == 1 && r.have_first && r.first_addr == 0xA2 && line.find("latch==stream 1, differ 1, unjoined 1") != std::string::npos;
        std::printf("%s census counts and format\n", good ? "ok  " : "FAIL");
        bad += !good;
      }
      {  // S103 W3: the latch is the source; the stream is the compare; a disagreement is a hole; a cleared / unreadable latch leaves no value
        T.Clear();
        auto chk = [&](const char* what, float got, float want) { const bool ok = (want == 0.f) ? !(got > 0.f) : std::fabs(got - want) < 1e-6f; std::printf("%s %s: s %.6f (want %.6f)\n", ok ? "ok  " : "FAIL", what, got, want); bad += !ok; };
        T.SetLatch(0xB1, 6, 0.89453125f, true);
        chk("latch only: taken", rdr_scene::BodyScaleFor(true, 0xB1, false, twin), 0.89453125f);
        T.Set(0xB1, 0.89453f);
        chk("latch + stream agree: taken", rdr_scene::BodyScaleFor(true, 0xB1, false, twin), 0.89453125f);
        T.Set(0xB1, 0.9104f);
        bool conf = false;
        chk("latch vs stream disagree: hole", rdr_scene::BodyScaleFor(true, 0xB1, false, twin, nullptr, &conf), 0.f);
        std::printf("%s conflict flagged\n", conf ? "ok  " : "FAIL"); bad += !conf;
        T.SetLatch(0xB1, 6, 0.f, false);
        chk("unreadable record clears the old latch: stream only", rdr_scene::BodyScaleFor(true, 0xB1, false, twin), 0.9104f);
        T.SetLatch(0xB2, 3, 0.95776f, true); T.ClearLatch(0xB2);
        chk("slot lost (cleared latch): hole", rdr_scene::BodyScaleFor(true, 0xB2, false, twin), 0.f);
        T.SetLatch(0xB3, 3, 0.95776f, true);
        chk("forced unknown beats the latch (plant)", rdr_scene::BodyScaleFor(true, 0xB3, true, twin), 0.f);
      }
      std::printf("RESULT hole selftest %s\n", bad ? "FAIL" : "PASS");
      return expect_fail ? (bad ? 0 : 1) : (bad ? 1 : 0);   // (--expect-fail must come before: the planted twin must FAIL)
    }
    else if (a == "--scale-source-latch") latch_mode = true;
    else if (a == "--latch-wrong-slot") latch_mode = true, latch_wrong_slot = true;
    else if (a == "--no-body-scale") plant_no_scale = true;   // the planted twin: the old behaviour (no body scale on the palette)
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
  std::map<std::string, Acc> fe_group, fe_noscale, fe_scaled;
  std::set<uint32_t> ratio_seen; int ratio_shown = 0;
  std::set<std::string> multi_seen; uint64_t meshes_total = 0, meshes_undrawn = 0; std::map<std::string, uint64_t> undrawn_names;
  uint64_t pos_fits = 0, no_stream_skin = 0, chunk_fail = 0, no_node = 0, diag_shown = 0;
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
    std::map<uint32_t, std::set<uint32_t>> drawn_parts; std::map<uint32_t, const assets::Model*> obj_model;
    for (uint32_t d = 0; d < fr.draws.count; ++d) {
      const DrawOrigin* org = fr.OriginOf(d);
      if (!org || (org->pass_site != kModelPartColourSite && org->pass_site != kModelPartMirrorSite) || org->object == 0) continue;
      const Draw& dr = fr.draws[d];
      if (dr.vs_bank >= fr.constant_banks.count) continue;
      int oi = -1;
      for (size_t i = 0; i < t.engine_objects.size(); ++i) if (t.engine_objects[i] == org->object) { oi = int(i); break; }
      bool oi_by_rows = false;
      if (getenv("W2_DBG") && oi < 0) { static std::set<uint32_t> once; if (once.insert(org->object).second) std::fprintf(stderr, "dbg: stream object %08X part %u has no engine match (told %zu, engine_objects %zu)\n", org->object, org->part, told_links.size(), t.engine_objects.size()); }
      if (oi < 0 && !told_links.empty()) {   // S103 W2: a record with no engine addresses: the TOLD object whose palette rows (by node name, bit for bit) are this draw's
        auto cached = row_map.find(org->object);
        if (cached != row_map.end()) oi = cached->second;
        else {
          for (size_t i = 0; i < t.objects.size() && oi < 0; ++i) {
            const rdr_scene::Object& co = t.objects[i];
            auto tl = told_links.find(std::string(t.Str(co.id)));
            if (tl == told_links.end() || !(co.flags & rdr_scene::kObjSkinned) || co.bones_count < 60) continue;
            auto LL = load("pac\\ch\\ch" + std::to_string(tl->second.first) + ".pac", "", uint32_t(tl->second.second));
            if (!LL->ok || LL->m.bones.size() != co.bones_count) { if (getenv("W2_DBG")) std::fprintf(stderr, "prepass: %s load ok %d bones %zu vs %u (%s)\n", std::string(t.Str(co.id)).c_str(), int(LL->ok), LL->m.bones.size(), co.bones_count, LL->why.c_str()); continue; }
            // The join is the core's own: the stream draw's vertices are one of this model's body meshes (any LOD: the record index of the part is not the draw's part in a far shot) at ONE scale
            // (the rule WrestlerCore confirms a body by). The old captures' palettes are not the stream's, so a row match cannot join them.
            bool all = false;
            double sj = 1.0;
            if (getenv("W2_DBG")) { std::string lst; for (const assets::Mesh& me : LL->m.meshes) if (me.src_kind == 2) lst += " " + std::to_string(me.src_key) + ":" + std::to_string(me.vertices.size()); std::fprintf(stderr, "join try: object %08X part %u stream vertices %u; model meshes%s\n", org->object, org->part, dr.vertex_count, lst.c_str()); }
            {
              std::vector<uint8_t> vbj;
              const VertexLayout& lj = fr.layouts[dr.layout];
              if (lj.host_stride >= 12 && dr.vertex_count && cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vbj) && vbj.size() >= size_t(lj.host_stride) * dr.vertex_count) {
                for (const assets::Mesh& me : LL->m.meshes) {
                  if (me.src_kind != 2 || me.src_key != 0 || me.vertices.size() != dr.vertex_count) continue;
                  double sab = 0, saa = 0;
                  for (uint32_t v = 0; v < dr.vertex_count; ++v) {
                    float p[3];
                    std::memcpy(p, vbj.data() + size_t(v) * lj.host_stride, 12);
                    for (int c = 0; c < 3; ++c) { const double a = (c == 1 ? -1.0 : 1.0) * me.vertices[v].pos[c]; sab += double(p[c]) * a, saa += a * a; }
                  }
                  const double sc = saa > 0 ? sab / saa : 1.0;
                  double worst = 0;
                  for (uint32_t v = 0; v < dr.vertex_count; ++v) {
                    float p[3];
                    std::memcpy(p, vbj.data() + size_t(v) * lj.host_stride, 12);
                    for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(double(p[c]) - sc * (c == 1 ? -1.0 : 1.0) * me.vertices[v].pos[c]));
                  }
                  if (sc > 0.1 && sc < 10.0 && worst < 1e-3) { all = true, sj = sc; break; }
                }
              }
            }
            if (all) {
              oi = int(i), row_map[org->object] = int(i);
              told_scale[org->object] = float(sj);
              if (!plant_no_scale) rdr_scene::BodyScaleTable::Get().SetByName(std::string(t.Str(co.id)), float(sj));
              if (print_scales && scales_printed.insert(org->object).second) std::printf("BODYSCALE %s %.9g\n", std::string(t.Str(co.id)).c_str(), sj);
            }
          }
        }
        oi_by_rows = oi >= 0;   // (joined by the stream draw fitting the TOLD model)
      }
      if (oi < 0) continue;
      const rdr_scene::Object& ro = t.objects[size_t(oi)];
      if (!(ro.flags & rdr_scene::kObjSkinned) || ro.bones_count < 60) continue;
      ++draws;
      // S103 W2: a pre-lane-B capture's record names no model: --link ID=CH:AT (TOLD, as frame_rig.py's DEFAULT_LINKS) names it; otherwise the record's own asset link
      std::string a_pac, a_entry;
      uint32_t a_variant = 0;
      auto told_it = told_links.find(std::string(t.Str(ro.id)));
      if (told_it != told_links.end()) {
        a_pac = "pac\\ch\\ch" + std::to_string(told_it->second.first) + ".pac", a_entry = "", a_variant = uint32_t(told_it->second.second);
      } else if (ro.asset >= t.assets.size() || t.assets[ro.asset].pac == 0) {
        if (no_link++ < 3) std::fprintf(stderr, "no asset link: object %s, %u nodes\n", std::string(t.Str(ro.id)).c_str(), ro.bones_count);
        continue;
      } else {
        const auto& ar = t.assets[ro.asset];
        a_pac = std::string(t.Str(ar.pac)), a_entry = std::string(t.Str(ar.entry)), a_variant = ar.variant;
      }
      auto L = load(a_pac, a_entry, a_variant);
      if (!L->ok) { ++no_link; if (names_wrong_seen["load " + L->why]++ == 0) std::fprintf(stderr, "load failed: %s\n", L->why.c_str()); continue; }
      const assets::Model& m = L->m;
      drawn_parts[org->object].insert(org->part); obj_model[org->object] = &m;
      double draw_scale = 1.0;
      if (m.bones.size() != ro.bones_count) { if (!nodes_mismatch++) std::fprintf(stderr, "model bones %zu != record bones %u\n", m.bones.size(), ro.bones_count); continue; }
      const assets::Mesh* mesh = nullptr;
      for (const assets::Mesh& me : m.meshes) if (me.src_record == int(org->part) && me.src_kind == 2 && me.src_key == 0) { mesh = &me; break; }
      if (!mesh) { ++no_mesh; continue; }
      if (mesh->vertices.size() != dr.vertex_count) {
        if (vcount_diff++ < 3) std::fprintf(stderr, "frame %d part %u: mesh %zu vertices, stream draw %u (asset %s %s)\n", f, org->part, mesh->vertices.size(), dr.vertex_count, a_pac.c_str(), a_entry.c_str());
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
          const double s = saa > 0 ? sab / saa : 1.0; draw_scale = s; if (!plant_no_scale && latch_mode) { if (latch_done.insert(org->object).second) { const float q = float(std::floor(s * 4096.0 + 0.5) / 4096.0); rdr_scene::BodyScaleTable::Get().SetLatch(org->object, 0, latch_wrong_slot ? latch_prev : q, true); latch_prev = q; } } else if (!plant_no_scale) { rdr_scene::BodyScaleTable::Get().Set(org->object, float(s)); if (oi_by_rows) rdr_scene::BodyScaleTable::Get().SetByName(std::string(t.Str(ro.id)), float(s)); } if (print_scales && scales_printed.insert(org->object).second) { if (oi_by_rows) std::printf("BODYSCALE %s %.9g\n", std::string(t.Str(ro.id)).c_str(), s); else std::printf("BODYSCALE 0x%08X %.9g\n", org->object, s); }
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
      if (draw_scale != 1.0 && ratio_shown < 40 && ratio_seen.insert(org->object).second) {   // S103 W2: the body scale from the record's palette alone: pose bone-length / bind bone-length
        for (int nn = 0; nn < 3; ++nn) { const rdr_scene::Mat4& Mn = t.bones[ro.bones_first + size_t(nn)]; std::fprintf(stderr, "NODE%d %s measured %.5f:", nn, m.bones[size_t(nn)].name.c_str(), draw_scale); for (int q = 0; q < 16; ++q) std::fprintf(stderr, " %.4f", double(Mn.m[q])); std::fprintf(stderr, "\n"); }
        std::vector<double> rs;
        auto W = [&](size_t b, double* o) {
          const float* bw = m.bones[b].bind_world.data();
          const double px = bw[12], py = -bw[13], pz = bw[14];   // file-space origin (the bind world is viewer space, Y mirrored)
          const rdr_scene::Mat4& M = t.bones[ro.bones_first + b];
          for (int c = 0; c < 3; ++c) o[c] = px * M.m[0 * 4 + c] + py * M.m[1 * 4 + c] + pz * M.m[2 * 4 + c] + M.m[3 * 4 + c];
        };
        for (size_t b = 2; b < m.bones.size(); ++b) {
          const int p = m.bones[b].parent;
          if (p < 2) continue;
          const float* bb = m.bones[b].bind_world.data(); const float* pb = m.bones[size_t(p)].bind_world.data();
          const double dl = std::sqrt(double(bb[12] - pb[12]) * (bb[12] - pb[12]) + double(bb[13] - pb[13]) * (bb[13] - pb[13]) + double(bb[14] - pb[14]) * (bb[14] - pb[14]));
          if (dl < 2.0) continue;
          double a[3], c2[3];
          W(b, a); W(size_t(p), c2);
          rs.push_back(std::sqrt((a[0] - c2[0]) * (a[0] - c2[0]) + (a[1] - c2[1]) * (a[1] - c2[1]) + (a[2] - c2[2]) * (a[2] - c2[2])) / dl);
        }
        std::sort(rs.begin(), rs.end());
        if (!rs.empty()) { ++ratio_shown; std::fprintf(stderr, "SCALE-FROM-PALETTE f%d obj %08X: measured %.5f | bone-length ratio median %.5f (min %.5f max %.5f, %zu bones)\n", f, org->object, draw_scale, rs[rs.size() / 2], rs.front(), rs.back(), rs.size()); }
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
      // S103 W2: the stream's skin (its own vertex bytes: weights x3 + slot bytes, the draw's c76 rows) against OUR shader path (BuildSkinChunks, the record's palette by node, the shader's four-bone blend)
      if (mesh->vertices.size() != dr.vertex_count) continue;
      int el_w = -1, el_s = -1;
      { const VertexLayout& lay = fr.layouts[dr.layout]; for (uint32_t e = 0; e < lay.element_count; ++e) { const VertexElement& el = fr.elements[lay.element_first + e]; if (el.vk_format == 106) el_w = el.host_offset; if (el.vk_format == 41) el_s = el.host_offset; } }
      std::vector<uint8_t> vb2;
      const VertexLayout& lay2 = fr.layouts[dr.layout];
      if (el_s < 0 || !cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vb2) || vb2.size() < size_t(lay2.host_stride) * dr.vertex_count) { ++no_stream_skin; continue; }
      std::vector<rdr_scene::SkinChunk> chunks;
      if (!rdr_scene::BuildSkinChunks(*mesh, chunks)) { ++chunk_fail; continue; }
      if (chunks.size() > 1 && multi_seen.insert(mesh->part + "/" + std::to_string(chunks.size())).second) std::fprintf(stderr, "MULTI-CHUNK part %s: %zu chunks (nodes %zu / %zu)\n", mesh->part.c_str(), chunks.size(), chunks[0].nodes.size(), chunks[1].nodes.size());
      { std::string dn0 = m.bones[size_t(mesh->vertices[0].bones[0])].name; ++undrawn_names["USED " + mesh->part + " | " + (mesh->material >= 0 && size_t(mesh->material) < m.materials.size() ? m.materials[size_t(mesh->material)].shader : std::string("?")) + " | first bone " + dn0 + " | chunks " + std::to_string(chunks.size())]; }
      std::vector<char> done(mesh->vertices.size(), 0);
      for (const rdr_scene::SkinChunk& ch : chunks) {
        std::vector<float> rowsv(63 * 4, 0.f);
        {
          std::vector<assets::Matrix> slotsm;
          for (uint16_t node : ch.nodes) { assets::Matrix sk = assets::Identity(); if (node < ro.bones_count) std::memcpy(sk.data(), t.bones[ro.bones_first + node].m, 64); { const float bs = rdr_scene::BodyScaleFor(true, org->object); /* S103 W3: the production decision (latch first, stream as the compare) */ if (bs > 0.f) rdr_scene::ScalePaletteRows(sk.data(), bs); } slotsm.push_back(assets::Mul(assets::MirrorY(sk), assets::Identity())); }
          assets::Material mt; assets::LightSet ls; rdr_scene::CharCtx cc; cc.mat = &mt; cc.lights = &ls; cc.bones = slotsm.data(); cc.bone_count = uint32_t(slotsm.size()); cc.mirror = -1.f;
          rdr_scene::ResolveChar("WorldMatrixArray", 63, cc, rowsv.data());
        }
        for (size_t ci = 0; ci < ch.vertices.size(); ++ci) {
          const uint32_t vi = ch.src[ci];
          if (done[vi]) continue;
          done[vi] = 1;
          const uint8_t* vp = vb2.data() + size_t(vi) * lay2.host_stride;
          float P[3], W3[3] = {0, 0, 0};
          std::memcpy(P, vp, 12);
          if (el_w >= 0) std::memcpy(W3, vp + el_w, 12);
          const uint8_t* sb = vp + el_s;
          int valid[4], nv = 0;
          for (int k = 0; k < 4; ++k) if (sb[k] != 255 && sb[k] < pal.size()) valid[nv++] = k;
          if (!nv) continue;
          // the stream: explicit weights for all but the last valid, the last = 1 - sum
          double th[3] = {0, 0, 0}, th2[3] = {0, 0, 0}, sum = 0;
          int dom_node = -1; double dwt = -1;
          for (int q = 0; q < nv; ++q) {
            const int k = valid[q];
            double w = (q + 1 < nv) ? double(W3[std::min(k, 2)]) : 1.0 - sum;
            sum += w;
            const int slot = sb[k];
            const auto& R = rows[size_t(slot)];
            for (int c = 0; c < 3; ++c) th[c] += w * (R[size_t(c * 4 + 0)] * P[0] + R[size_t(c * 4 + 1)] * P[1] + R[size_t(c * 4 + 2)] * P[2] + R[size_t(c * 4 + 3)]);
            { const float Q[3] = {ch.vertices[ci].pos[0], -ch.vertices[ci].pos[1], ch.vertices[ci].pos[2]}; for (int c = 0; c < 3; ++c) th2[c] += w * (R[size_t(c * 4 + 0)] * Q[0] + R[size_t(c * 4 + 1)] * Q[1] + R[size_t(c * 4 + 2)] * Q[2] + R[size_t(c * 4 + 3)]); }
            if (w > dwt) dwt = w, dom_node = rec_node[size_t(slot)];
          }
          // ours: the shader's blend over the chunk's slots
          const rdr_scene::SkinVertex& sv = ch.skin[ci];
          auto Mof = [&](uint32_t id) -> const rdr_scene::Mat4* { const uint16_t node = id < ch.nodes.size() ? ch.nodes[id] : 0; return node < ro.bones_count ? &t.bones[ro.bones_first + node] : nullptr; };
          double wv[4] = {sv.w[0], sv.w[1], sv.w[2], 1.0 - sv.w[0] - sv.w[1] - sv.w[2]};
          double ou[3] = {0, 0, 0};
          for (int k = 0; k < 4; ++k) {
            const rdr_scene::Mat4* M = Mof(sv.ids[k]);
            if (!M) { ++no_node; continue; }
            for (int c = 0; c < 3; ++c) ou[c] += wv[k] * (P[0] * M->m[0 * 4 + c] + P[1] * M->m[1 * 4 + c] + P[2] * M->m[2 * 4 + c] + M->m[3 * 4 + c]);
          }
          if (dom_node < 0) continue;
          {  // the front-end's own constants: slots = skin[node] (MirrorY'd: a Live record in viewer space) x world, ResolveChar's rows, the loader's vertex position, the effect's four-bone blend
            const assets::Vertex& lv = ch.vertices[ci];
            auto row = [&](uint32_t id, int k, const float p[3]) { const float* r = &rowsv[4 * (3 * std::min<uint32_t>(id, 20) + uint32_t(k))]; return double(r[0]) * p[0] + double(r[1]) * p[1] + double(r[2]) * p[2] + double(r[3]); };
            double o3[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) for (int c = 0; c < 3; ++c) o3[c] += wv[k] * row(sv.ids[k], c, lv.pos);
            const double e3 = std::sqrt((o3[0] - th[0]) * (o3[0] - th[0]) + (o3[1] - th[1]) * (o3[1] - th[1]) + (o3[2] - th[2]) * (o3[2] - th[2]));
            const std::string& dn3 = m.bones[size_t(dom_node)].name;
            Acc& a3 = fe_group[GroupOf(dn3)];
            ++a3.verts; a3.sum += e3; a3.max = std::max(a3.max, e3); a3.bad += e3 > 0.05;
            {  // vs the game's real skinned position th (the stream's scaled vertex): the front-end as it is (no scale) and with the body's scale applied
              const float ps[3] = {float(lv.pos[0] * draw_scale), float(lv.pos[1] * draw_scale), float(lv.pos[2] * draw_scale)};
              double o5[3] = {0, 0, 0};
              for (int k = 0; k < 4; ++k) for (int c = 0; c < 3; ++c) o5[c] += wv[k] * row(sv.ids[k], c, ps);
              const double eu = std::sqrt((o3[0] - th[0]) * (o3[0] - th[0]) + (o3[1] - th[1]) * (o3[1] - th[1]) + (o3[2] - th[2]) * (o3[2] - th[2]));
              const double es = std::sqrt((o5[0] - th[0]) * (o5[0] - th[0]) + (o5[1] - th[1]) * (o5[1] - th[1]) + (o5[2] - th[2]) * (o5[2] - th[2]));
              Acc& au = fe_noscale[GroupOf(dn3)]; ++au.verts; au.sum += eu; au.max = std::max(au.max, eu); au.bad += eu > 0.05;
              Acc& as = fe_scaled[GroupOf(dn3)]; ++as.verts; as.sum += es; as.max = std::max(as.max, es); as.bad += es > 0.05;
            }
          }
          const double e =std::sqrt((ou[0] - th[0]) * (ou[0] - th[0]) + (ou[1] - th[1]) * (ou[1] - th[1]) + (ou[2] - th[2]) * (ou[2] - th[2]));
          const std::string& dn = m.bones[size_t(dom_node)].name;
          for (Acc* a : {&by_group[GroupOf(dn)], &by_bone[dn]}) { ++a->verts; a->sum += e; a->max = std::max(a->max, e); a->bad += e > 0.05; }
          if (e > 0.05 && diag_shown < 12) {
            ++diag_shown;
            std::fprintf(stderr, "BAD f%d part %u v%u (%s): err %.3f nv %d stream slots %u,%u,%u,%u w %.3f %.3f %.3f | loader bones %u,%u,%u,%u w %.3f %.3f %.3f %.3f | chunk ids %u,%u,%u,%u w %.3f %.3f %.3f\n", f, org->part, vi, dn.c_str(), e, nv, sb[0], sb[1], sb[2], sb[3], W3[0], W3[1], W3[2],
                         mesh->vertices[vi].bones[0], mesh->vertices[vi].bones[1], mesh->vertices[vi].bones[2], mesh->vertices[vi].bones[3], mesh->vertices[vi].weights[0], mesh->vertices[vi].weights[1], mesh->vertices[vi].weights[2], mesh->vertices[vi].weights[3], sv.ids[0], sv.ids[1], sv.ids[2], sv.ids[3], sv.w[0], sv.w[1], sv.w[2]);
          }
        }
      }
    }
    for (auto& kv : obj_model) {   // S103 W2: model meshes the stream never drew for this object this frame
      const assets::Model* mp = kv.second;
      for (const assets::Mesh& me : mp->meshes) {
        if (me.src_kind == 2 && me.src_key != 0) { ++undrawn_names["EXTRA key " + std::to_string(me.src_key) + " " + me.part + " verts " + std::to_string(me.vertices.size()) + " skinned " + std::to_string(int(me.skinned))]; continue; }
        if (me.src_kind != 2) continue;
        ++meshes_total;
        if (drawn_parts[kv.first].count(uint32_t(me.src_record))) continue;
        ++meshes_undrawn;
        const std::string key = me.part + " | " + (me.material >= 0 && size_t(me.material) < mp->materials.size() ? mp->materials[size_t(me.material)].shader : std::string("?")) + " | verts " + std::to_string(me.vertices.size());
        ++undrawn_names[key];
      }
    }
  }
  std::printf("W2: model meshes (body kind) seen %llu, never drawn by the stream %llu\n", (unsigned long long)meshes_total, (unsigned long long)meshes_undrawn);
  for (auto& kv : undrawn_names) std::printf("UNDRAWN %s x%llu\n", kv.first.c_str(), (unsigned long long)kv.second);
  for (auto& kv : fe_noscale) std::printf("FE_NOSCALE %s verts %llu bad(>0.05) %llu mean %.5f max %.5f\n", kv.first.c_str(), (unsigned long long)kv.second.verts, (unsigned long long)kv.second.bad, kv.second.sum / std::max<uint64_t>(1, kv.second.verts), kv.second.max);
  for (auto& kv : fe_scaled) std::printf("FE_SCALED %s verts %llu bad(>0.05) %llu mean %.5f max %.5f\n", kv.first.c_str(), (unsigned long long)kv.second.verts, (unsigned long long)kv.second.bad, kv.second.sum / std::max<uint64_t>(1, kv.second.verts), kv.second.max);
  for (auto& kv : fe_group)std::printf("FRONTEND %s verts %llu bad(>1.0) %llu mean %.5f max %.5f\n", kv.first.c_str(), (unsigned long long)kv.second.verts, (unsigned long long)kv.second.bad, kv.second.sum / std::max<uint64_t>(1, kv.second.verts), kv.second.max);
  std::printf("W2: no stream skin %llu, chunk build failed %llu, slot without node %llu\n", (unsigned long long)no_stream_skin, (unsigned long long)chunk_fail, (unsigned long long)no_node);
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
  for (auto& kv : fe_group) bad_total += kv.second.bad;   // S103 W2: the front-end path (production palette scaling) vs the stream
  const bool ok = bad_total == 0 && slots > 0;
  std::printf("W2 body scale %s, source %s\n", plant_no_scale ? "NOT applied (planted twin = the old behaviour)" : "applied (production BodyScaleFor + ScalePaletteRows)", latch_mode ? (latch_wrong_slot ? "latch, WRONG SLOT twin" : "latch (record path, k/4096)") : "stream");
  std::printf("RESULT pair %d %s (slots %llu, bad %llu)\n", pair, ok ? "EQUAL" : "DIFFER", (unsigned long long)slots, (unsigned long long)bad_total);
  return expect_fail ? (ok ? 1 : 0) : (ok ? 0 : 1);
}
