// wrestler_pair: the wrestlers' pairing test (Session 101, lane 42 = W0; docs\renderer\WRESTLER_PROMOTION_MAP.md sections 2, 3 and 6). 0 game runs.
// Method extended from tools\crowd\crowd_pair.cpp (lane 36): measure the pairing, never assume it.
// For every captured frame it takes the stream's MAIN-PASS model draws (pass_site 0x82C0A950 / 0x82C0B94C, grouped by DrawOrigin::object) and:
//   1. resolves each stream object to a model of the player's own files by CONTENT: the object's drawn parts (part, vertex count, index count)
//      against every JBOY entry of every pac\ch\chNNN.pac attire (assets/jboy_parts.h; ambiguity is counted, never hidden);
//   2. links the object to a record skeleton (`actor:` / `skel:` objects of the scene record, bones_count == the model's node count), by the
//      share of palette rows that are bit-equal (or by the record's engine address when the record carries one);
//   3. for record i against stream frame i+off (off = -2..+2): the palette rows c76 + 3k .. +2 of every draw against the record's node
//      (palette[k] - 1) of the linked skeleton, word for word, with the palette length taken from the model's bone list (never from where
//      matching stops), per part and per node;
//   4. the part-index = mesh-index claim: the draw's part p against the model's mesh record p: vertex count, index count, every index value,
//      and the vertex positions (a per-axis least-squares scale and the residual).
//   wrestler_pair --capture DIR --game DIR [--frames A-B] [--step N] [--csv FILE] [--shift K] [--swap A,B] [--part-off K] [--verbose]
// DLC pacs (chNNN_dlcPP.pac) are NOT searched: only the disc's pac\ch\chNNN.pac. --perturb-index and --perturb-pos plant one wrong index value / position on the file side of the checks.
// --shift K, --swap A,B and --part-off K are planted faults (inside the same code path): read record i+K for frame i; swap two nodes of every
// record palette; compare the stream's part p with the model's mesh p+K in the part checks only (the palette link stays).
// Exit 0 = measured, 2 = a loud failure (no scene records, unreadable input...), 3 = bad usage. Lines starting with "SKELETON" and "RESULT" are
// the machine-readable output (wrestler_pair_selftest.py reads them).
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "stb_image.h"  // declarations only (pac.h's zlib); the implementation is loader_seam_game.cpp's
#include "../../src/assets/jboy_parts.h"
#include "../../src/assets/pac.h"
#include "../../src/assets/wrestler_geom.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/record_io.h"

namespace {
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;
using rdr_scene::SceneTables;
namespace jp = assets::jboy_parts;

constexpr uint32_t kPassSites[2] = {0x82C0A950, 0x82C0B94C};
constexpr int kN = 5, kMid = 2;  // record offsets -2..+2

struct Options {
  std::string capture, game, csv;
  int first = 0, last = -1, step = 1, swap_a = -1, swap_b = -1, shift = 0, part_off = 0;
  bool perturb_index = false, perturb_pos = false;
  bool verbose = false;
};

// ---- the roster: every JBOY entry of every character pac, counts only --------------------------------------------------------------------
struct PartInfo {
  uint32_t vertices = 0, index_count = 0, palette_count = 0;
  uint32_t palette[20] = {};
  bool ok = false;
};
struct Cand {
  int pac = 0, attire = 0;
  uint32_t entry = 0;
  uint32_t node_count = 0;
  std::vector<PartInfo> parts;
};
struct Roster {
  std::vector<Cand> cands;
  std::map<int, double> load_s;
  size_t pacs = 0;
};

std::string PacRel(int id) {
  char b[64];
  std::snprintf(b, sizeof b, "pac\\ch\\ch%03d.pac", id);
  return b;
}

bool ParseChName(const std::string& n, int& id) {
  if (n.size() != 9) return false;
  std::string l = n;
  for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (l.compare(0, 2, "ch") != 0 || l.compare(5, 4, ".pac") != 0) return false;
  int v = 0;
  for (int i = 2; i < 5; ++i) {
    if (l[size_t(i)] < '0' || l[size_t(i)] > '9') return false;
    v = v * 10 + (l[size_t(i)] - '0');
  }
  id = v;
  return true;
}

bool IsEntryWanted(uint32_t id) { return id == 0 || id == 26 || id == 27 || (id >= 10000 && id < 40000); }

bool BuildRoster(launcher::GameFiles& files, Roster& r, std::string& err) {
  std::vector<int> ids;
  for (const std::string& n : files.List("pac\\ch")) {
    int id;
    if (ParseChName(n, id)) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  if (ids.empty()) return err = "no pac\\ch\\chNNN.pac in the game folder", false;
  for (int id : ids) {
    const auto t0 = std::chrono::steady_clock::now();
    assets::pac::Pac p;
    std::string e;
    if (!assets::pac::Open(files, PacRel(id), p, e)) { std::fprintf(stderr, "wrestler_pair: note: %s skipped (%s)\n", PacRel(id).c_str(), e.c_str()); continue; }
    ++r.pacs;
    int attire = 0;
    for (const assets::pac::Pach& h : p.pachs) {
      if (!assets::pac::Find(h, 0)) continue;
      for (const assets::pac::Ent& en : h.ents) {
        if (!IsEntryWanted(en.id)) continue;
        assets::Bytes blob = assets::pac::Decode(p.bytes.data() + en.off, en.size);
        if (blob.size() < 8 || std::memcmp(blob.data(), "JBOY", 4) != 0) continue;
        jp::Parts jb;
        std::string e2;
        if (!jp::Parse(blob, jb, e2)) continue;
        Cand c;
        c.pac = id, c.attire = attire, c.entry = en.id, c.node_count = jb.node_count;
        for (const jp::Part& pt : jb.parts) {
          PartInfo pi;
          pi.vertices = pt.vertices, pi.index_count = assets::wrestler_geom::DrawIndexCount(pt), pi.palette_count = pt.palette_count, pi.ok = pt.ok;
          std::memcpy(pi.palette, pt.palette, sizeof pi.palette);
          c.parts.push_back(pi);
        }
        r.cands.push_back(std::move(c));
      }
      ++attire;
    }
    r.load_s[id] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  }
  return !r.cands.empty() || (err = "the roster is empty", false);
}

// ---- the stream's objects ---------------------------------------------------------------------------------------------------------------
struct StreamDraw {
  uint32_t draw = 0, part = 0, vc = 0, ic = 0;
};
using Sig = std::vector<std::array<uint32_t, 3>>;  // (part, vc, ic) of the first draw of each part, by part

struct Resolved {
  Sig sig;
  std::vector<int> cands;  // roster indices matching the signature (ambiguity = more than one with different content)
  bool ambiguous = false;
};

// A candidate fits an object when every drawn part has a mesh record with the same vertex and index counts.
bool Fits(const Cand& c, const Sig& s, int part_off) {
  for (const auto& e : s) {
    const int64_t p = int64_t(e[0]) + part_off;
    if (p < 0 || p >= int64_t(c.parts.size())) return false;
    const PartInfo& pi = c.parts[size_t(p)];
    if (pi.vertices != e[1] || pi.index_count != e[2]) return false;
  }
  return true;
}

struct Sk {  // a record skeleton
  uint32_t obj = 0;
  std::string id;
  uint32_t first = 0, count = 0;
};

// c76-c78-style row block of a record node: reg(base + j)[c] = m[c*4 + j]
struct NodeRows {
  const float* m;
};

struct Stat {
  double sum = 0, mx = 0;
  uint64_t n = 0;
  void Add(double v) { sum += v; mx = std::max(mx, v); ++n; }
  double Mean() const { return n ? sum / double(n) : 0; }
};

// per (offset, skeleton) accumulation
struct SkAcc {
  uint64_t frames = 0, draws = 0, rows = 0, exact = 0;
  std::map<uint32_t, uint64_t> node_cmp, node_bad;  // by node
  std::map<uint32_t, double> node_maxdiff;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> part_rows;  // part -> (rows, exact)
  std::set<uint32_t> objects;                                    // stream object addresses seen
  std::set<int> cand_ids;
  uint64_t bad_node_index = 0;                                   // palette node beyond the skeleton
};

// per stream object checks (part = mesh claim), independent of the record offset
struct ObjAcc {
  std::string label;
  uint64_t draws = 0, count_ok = 0, count_bad = 0, idx_ok = 0, idx_bad = 0, idx_skipped = 0;
  Stat pos_rms;
  double sx = 0, sy = 0, sz = 0;
  double smin[3] = {1e30, 1e30, 1e30}, smax[3] = {-1e30, -1e30, -1e30};  // per-axis scale over draws
  uint64_t fits = 0;
  std::set<uint32_t> parts_seen;
  std::map<std::string, uint64_t> linked[5];  // by offset index: skeleton id -> frames
  std::map<int, uint64_t> cand_votes;         // roster index -> frames it was the best fit
  uint64_t stale_eq = 0, stale_n = 0;         // the first register block past the palette: how often it equals a record node
  uint64_t frames_present = 0, frames_resolved = 0;
};

// A model of the roster, re-read for the part checks (the roster keeps counts only).
struct Model {
  assets::Bytes blob;
  jp::Parts parts;
};

std::string Ranges(const std::set<uint32_t>& s) {
  std::string out;
  for (auto it = s.begin(); it != s.end();) {
    auto j = it;
    uint32_t last = *it;
    ++j;
    while (j != s.end() && *j == last + 1) last = *j, ++j;
    char b[48];
    if (last == *it) std::snprintf(b, sizeof b, "%u", *it);
    else std::snprintf(b, sizeof b, "%u-%u", *it, last);
    out += (out.empty() ? "" : ",") + std::string(b);
    it = j;
  }
  return out;
}

const float* Reg(const rdr_adapter::ConstantBank& b, int n) { return reinterpret_cast<const float*>(b.dwords) + 4 * n; }
uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--capture") opt.capture = next();
    else if (a == "--game") opt.game = next();
    else if (a == "--csv") opt.csv = next();
    else if (a == "--frames") { if (std::sscanf(next(), "%d-%d", &opt.first, &opt.last) != 2) { std::fprintf(stderr, "bad --frames\n"); return 3; } }
    else if (a == "--step") opt.step = std::max(1, std::atoi(next()));
    else if (a == "--swap") { if (std::sscanf(next(), "%d,%d", &opt.swap_a, &opt.swap_b) != 2) { std::fprintf(stderr, "bad --swap\n"); return 3; } }
    else if (a == "--shift") opt.shift = std::atoi(next());
    else if (a == "--part-off") opt.part_off = std::atoi(next());
    else if (a == "--perturb-index") opt.perturb_index = true;
    else if (a == "--perturb-pos") opt.perturb_pos = true;
    else if (a == "--verbose") opt.verbose = true;
    else { std::fprintf(stderr, "wrestler_pair: unknown argument %s\n", a.c_str()); return 3; }
  }
  if (opt.capture.empty() || opt.game.empty()) {
    std::fprintf(stderr, "usage: wrestler_pair --capture DIR --game DIR [--frames A-B] [--step N] [--csv F] [--shift K] [--swap A,B] [--part-off K]\n");
    return 3;
  }
  const auto t0 = std::chrono::steady_clock::now();
  CaptureReader cap;
  if (!cap.Open(opt.capture)) { std::fprintf(stderr, "wrestler_pair: FAIL cannot open the capture at %s\n", opt.capture.c_str()); return 2; }
  if (cap.scene_record_count() == 0) {
    std::fprintf(stderr, "wrestler_pair: FAIL %s has no scene records (scene\\frame_N.rsc): take the capture with --wwe2k14_scene_records\n", opt.capture.c_str());
    return 2;
  }
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(opt.game));
  if (!files.Exists("pac\\ch")) { std::fprintf(stderr, "wrestler_pair: FAIL no game files at %s\n", opt.game.c_str()); return 2; }
  Roster roster;
  {
    std::string err;
    if (!BuildRoster(files, roster, err)) { std::fprintf(stderr, "wrestler_pair: FAIL %s\n", err.c_str()); return 2; }
  }
  const double roster_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr, "wrestler_pair: roster %zu pacs, %zu model entries, %.1f s\n", roster.pacs, roster.cands.size(), roster_s);

  const int nframes = int(cap.frame_count());
  if (opt.last < 0 || opt.last >= nframes) opt.last = nframes - 1;

  std::FILE* csv = nullptr;
  if (!opt.csv.empty()) {
    csv = std::fopen(opt.csv.c_str(), "w");
    if (!csv) { std::fprintf(stderr, "wrestler_pair: FAIL cannot write %s\n", opt.csv.c_str()); return 2; }
    std::fprintf(csv, "frame,guest_frame,best_offset,share_m2,share_m1,share_0,share_p1,share_p2,objects_linked,rows\n");
  }

  std::map<uint32_t, Resolved> resolved;           // by stream object address
  std::map<std::string, SkAcc> acc[kN];            // by offset index, skeleton id
  std::map<uint32_t, ObjAcc> objacc;               // by stream object address
  std::map<int, int> best_hist;
  int analysed = 0, skipped = 0, edge = 0, ties = 0;
  std::set<uint32_t> all_objs;
  uint64_t engine_declared = 0;
  uint64_t guest_mismatch = 0;  // record i whose guest frame differs from frame i's header

  // The roster keeps counts only; a model is re-read when a part check needs its streams.
  std::map<int, std::unique_ptr<Model>> models;
  assets::pac::Pac open_pac;
  int open_pac_id = -1;
  auto get_model = [&](int ci) -> Model* {
    auto it = models.find(ci);
    if (it != models.end()) return it->second.get();
    const Cand& C = roster.cands[size_t(ci)];
    if (open_pac_id != C.pac) {
      std::string e;
      if (!assets::pac::Open(files, PacRel(C.pac), open_pac, e)) return nullptr;
      open_pac_id = C.pac;
    }
    int attire = 0;
    for (const assets::pac::Pach& h : open_pac.pachs) {
      if (!assets::pac::Find(h, 0)) continue;
      if (attire++ != C.attire) continue;
      const assets::pac::Ent* en = assets::pac::Find(h, C.entry);
      if (!en) return nullptr;
      auto m = std::make_unique<Model>();
      m->blob = assets::pac::Decode(open_pac.bytes.data() + en->off, en->size);
      std::string e;
      if (!jp::Parse(m->blob, m->parts, e)) return nullptr;
      return (models[ci] = std::move(m)).get();
    }
    return nullptr;
  };

  // Palette rows of one object's draws against one record skeleton, word for word. `A` (optional) collects the per-node and per-part detail.
  auto compare = [&](const LoadedFrame& L, const SceneTables& R, const Sk& S, const Cand& C, const std::vector<StreamDraw>& ds, SkAcc* A, uint64_t& rows, uint64_t& exact) {
    rows = exact = 0;
    for (const auto& sd : ds) {
      const int64_t pi = int64_t(sd.part);
      if (pi < 0 || pi >= int64_t(C.parts.size())) continue;
      const PartInfo& P = C.parts[size_t(pi)];
      const auto& dr = L.draws[sd.draw];
      if (dr.vs_bank >= L.constant_banks.count) continue;
      const auto& bank = L.constant_banks[dr.vs_bank];
      if (A) ++A->draws;
      for (uint32_t k = 0; k < P.palette_count; ++k) {
        if (P.palette[k] < 1 || P.palette[k] > S.count) { if (A) ++A->bad_node_index; continue; }
        const uint32_t node = P.palette[k] - 1;
        const rdr_scene::Mat4& M = R.bones[S.first + node];
        bool all = true;
        double md = 0;
        for (int j = 0; j < 3; ++j) {
          const float* g = Reg(bank, 76 + 3 * int(k) + j);
          for (int c = 0; c < 4; ++c)
            if (Bits(g[c]) != Bits(M.m[c * 4 + j])) all = false, md = std::max(md, double(std::fabs(g[c] - M.m[c * 4 + j])));
        }
        rows += 3, exact += all ? 3 : 0;
        if (A) {
          A->rows += 3, A->exact += all ? 3 : 0;
          A->node_cmp[node]++;
          if (!all) A->node_bad[node]++, A->node_maxdiff[node] = std::max(A->node_maxdiff[node], md);
          auto& pr = A->part_rows[sd.part];
          pr.first += 3, pr.second += all ? 3 : 0;
        }
      }
    }
  };

  LoadedFrame fr;
  std::vector<uint8_t> vbuf, ibuf;
  for (int f = opt.first; f <= opt.last; f += opt.step) {
    if (!cap.LoadFrame(uint32_t(f), fr)) { std::fprintf(stderr, "wrestler_pair: FAIL cannot load frame %d\n", f); return 2; }
    std::map<uint32_t, std::vector<StreamDraw>> objs;  // the stream's main-pass model draws by object
    for (uint32_t d = 0; d < fr.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = fr.OriginOf(d);
      if (!o || (o->pass_site != kPassSites[0] && o->pass_site != kPassSites[1]) || o->object == 0) continue;
      const auto& dr = fr.draws[d];
      objs[o->object].push_back({d, o->part, dr.vertex_count, dr.index_count});
    }
    if (objs.empty()) { ++skipped; continue; }
    for (auto& [addr, ds] : objs) {  // resolve each object by content, once per signature
      all_objs.insert(addr);
      Sig sig;
      std::set<uint32_t> seen_part;
      for (const auto& s : ds)
        if (seen_part.insert(s.part).second) sig.push_back({s.part, s.vc, s.ic});
      std::sort(sig.begin(), sig.end());
      auto it = resolved.find(addr);
      if (it == resolved.end() || it->second.sig != sig) {
        Resolved r;
        r.sig = sig;
        for (size_t ci = 0; ci < roster.cands.size(); ++ci)
          if (Fits(roster.cands[ci], sig, 0)) r.cands.push_back(int(ci));
        resolved[addr] = std::move(r);
      }
    }

    SceneTables rec[kN];
    bool have_rec[kN] = {};
    bool complete = true;
    for (int oi = 0; oi < kN; ++oi) {
      const int ri = f - (oi - kMid) + opt.shift;  // record i against stream frame i+off: stream f uses record f - off
      if (ri < 0 || ri >= nframes || !cap.HasSceneRecord(uint32_t(ri))) { complete = false; continue; }
      std::string err;
      if (!rdr_scene::ReadRecord(cap.SceneRecordPath(uint32_t(ri)).string(), rec[oi], err)) {
        std::fprintf(stderr, "wrestler_pair: FAIL scene record %d: %s\n", ri, err.c_str());
        return 2;
      }
      have_rec[oi] = true;
      if (opt.swap_a >= 0 && opt.swap_b >= 0)  // planted: two palette nodes swapped in every skeleton
        for (const auto& ob : rec[oi].objects) {
          if (!(ob.flags & rdr_scene::kObjSkinned) || ob.bones_count == 0 || ob.bones_first + ob.bones_count > rec[oi].bones.size()) continue;
          if (uint32_t(std::max(opt.swap_a, opt.swap_b)) >= ob.bones_count) continue;
          std::swap(rec[oi].bones[ob.bones_first + uint32_t(opt.swap_a)], rec[oi].bones[ob.bones_first + uint32_t(opt.swap_b)]);
        }
    }
    if (have_rec[kMid] && rec[kMid].header.guest_frame != fr.header.guest_frame) ++guest_mismatch;
    if (!have_rec[kMid + 1]) { ++skipped; continue; }  // (the +1 record is the one every later lane needs; an edge frame has none)

    std::vector<Sk> sks[kN];
    for (int oi = 0; oi < kN; ++oi) {
      if (!have_rec[oi]) continue;
      const SceneTables& R = rec[oi];
      for (uint32_t k = 0; k < R.objects.size(); ++k) {
        const auto& ob = R.objects[k];
        const std::string id(R.Str(ob.id));
        if (!(id.rfind("actor:", 0) == 0 || id.rfind("skel:", 0) == 0) || ob.bones_count == 0 || ob.bones_first + ob.bones_count > R.bones.size()) continue;
        sks[oi].push_back({k, id, ob.bones_first, ob.bones_count});
      }
    }

    double share_rows[kN] = {}, share_exact[kN] = {};
    int linked_n[kN] = {};
    for (auto& [addr, ds] : objs) {
      const Resolved& rs = resolved[addr];
      ObjAcc& oa = objacc[addr];
      ++oa.frames_present;
      if (rs.cands.empty()) continue;
      ++oa.frames_resolved;
      int best_cand_any = -1;
      // identity: the skeleton (and model) whose palette rows fit best over ALL offsets; then every offset is measured against that same id
      double id_score = -1;
      std::string id_sk;
      int id_ci = -1;
      for (int oi = 0; oi < kN; ++oi) {
        if (!have_rec[oi]) continue;
        const SceneTables& R = rec[oi];
        int declared = -1;
        if (!R.engine_objects.empty())
          for (size_t si = 0; si < sks[oi].size(); ++si)
            if (R.engine_objects[sks[oi][si].obj] == addr) declared = int(si);
        for (size_t si = 0; si < sks[oi].size(); ++si) {
          if (declared >= 0 && int(si) != declared) continue;
          const Sk& S = sks[oi][si];
          for (int ci : rs.cands) {
            const Cand& C = roster.cands[size_t(ci)];
            if (C.node_count != S.count) continue;
            uint64_t rows, exact;
            compare(fr, R, S, C, ds, nullptr, rows, exact);
            if (rows == 0) continue;
            const double score = double(exact) / double(rows);
            if (score > id_score) id_score = score, id_sk = S.id, id_ci = ci;
          }
        }
        if (declared >= 0) ++engine_declared;
      }
      if (id_ci >= 0 && id_score >= 0.3) {
        for (int oi = 0; oi < kN; ++oi) {
          if (!have_rec[oi]) continue;
          const Sk* S = nullptr;
          for (const Sk& c : sks[oi])
            if (c.id == id_sk) S = &c;
          if (!S) continue;
          SkAcc& A = acc[oi][S->id];
          ++A.frames;
          A.objects.insert(addr);
          A.cand_ids.insert(id_ci);
          uint64_t r2, e2;
          compare(fr, rec[oi], *S, roster.cands[size_t(id_ci)], ds, &A, r2, e2);
          oa.linked[oi][S->id]++;
          share_rows[oi] += double(r2), share_exact[oi] += double(e2);
          ++linked_n[oi];
        }
        best_cand_any = id_ci;
      }
      // part checks: the draw's part p against the model's mesh record p (counts, every index value, the vertex positions)
      const int ci = best_cand_any >= 0 ? best_cand_any : rs.cands.front();
      oa.cand_votes[ci]++;
      Model* mdl = get_model(ci);
      const Cand& C = roster.cands[size_t(ci)];
      for (const auto& sd : ds) {
        ++oa.draws;
        oa.parts_seen.insert(sd.part);
        const int64_t pi = int64_t(sd.part) + opt.part_off;
        const auto& dr = fr.draws[sd.draw];
        if (pi < 0 || pi >= int64_t(C.parts.size()) || C.parts[size_t(pi)].vertices != sd.vc || C.parts[size_t(pi)].index_count != sd.ic) { ++oa.count_bad; continue; }
        ++oa.count_ok;
        if (!mdl || size_t(pi) >= mdl->parts.parts.size() || !mdl->parts.parts[size_t(pi)].ok) { ++oa.idx_skipped; continue; }
        const jp::Part& pt = mdl->parts.parts[size_t(pi)];
        // stale registers: the block just past the palette (the previous draw's rows) against every record node
        if (pt.palette_count < 20 && dr.vs_bank < fr.constant_banks.count) {
          const auto& bank = fr.constant_banks[dr.vs_bank];
          const SceneTables& R = rec[kMid + 1];
          bool any = false;
          for (const Sk& S : sks[kMid + 1]) {
            for (uint32_t n = 0; n < S.count && !any; ++n) {
              const rdr_scene::Mat4& M = R.bones[S.first + n];
              bool eq = true;
              for (int j = 0; j < 3 && eq; ++j) {
                const float* g = Reg(bank, 76 + 3 * int(pt.palette_count) + j);
                for (int c = 0; c < 4; ++c) eq = eq && Bits(g[c]) == Bits(M.m[c * 4 + j]);
              }
              any = eq;
            }
            if (any) break;
          }
          ++oa.stale_n, oa.stale_eq += any;
        }
        // indices
        if (dr.index_bytes == sd.ic * 2 && !dr.index_32bit && cap.ReadGeometry(dr.index_offset, dr.index_bytes, ibuf)) {
          std::vector<uint16_t> want = assets::wrestler_geom::BuildIndices(pt);   // lane 192: groups joined by 0xFFFF (the draw count)
          if (opt.perturb_index && !want.empty()) want[want.size() / 2] ^= 1;  // planted: one index value of the file's strip off by one bit
          bool same = want.size() == sd.ic;
          for (size_t i = 0; same && i < want.size(); ++i) {
            uint16_t g;
            std::memcpy(&g, ibuf.data() + 2 * i, 2);
            same = g == want[i];
          }
          (same ? oa.idx_ok : oa.idx_bad)++;
        } else {
          ++oa.idx_skipped;
        }
        // positions: the first R32G32B32(A32)_SFLOAT element against the file's S0 positions (per-axis least-squares scale and residual)
        if (dr.layout < fr.layouts.count && cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vbuf)) {
          const auto& lay = fr.layouts[dr.layout];
          const rdr_adapter::VertexElement* pe = nullptr;
          for (uint32_t e = 0; e < lay.element_count && !pe; ++e)
            if (fr.elements[lay.element_first + e].vk_format == 106 || fr.elements[lay.element_first + e].vk_format == 109) pe = &fr.elements[lay.element_first + e];
          if (pe && lay.host_stride && vbuf.size() == size_t(lay.host_stride) * sd.vc && sd.vc == pt.vertices) {
            double ab[3] = {}, bb[3] = {};
            for (uint32_t v = 0; v < sd.vc; ++v) {
              float g[3];
              std::memcpy(g, vbuf.data() + size_t(v) * lay.host_stride + pe->host_offset, 12);
              for (int c = 0; c < 3; ++c) {
                const double b = assets::BeF(pt.s[0] + 28ull * v + 4 * c) + (opt.perturb_pos && v == 0 && c == 0 ? 1.0 : 0.0);  // (planted: one position 1 unit off)
                ab[c] += double(g[c]) * b, bb[c] += b * b;
              }
            }
            double sc[3];
            for (int c = 0; c < 3; ++c) sc[c] = bb[c] > 0 ? ab[c] / bb[c] : 0;
            double ss = 0;
            for (uint32_t v = 0; v < sd.vc; ++v) {
              float g[3];
              std::memcpy(g, vbuf.data() + size_t(v) * lay.host_stride + pe->host_offset, 12);
              for (int c = 0; c < 3; ++c) {
                const double b = assets::BeF(pt.s[0] + 28ull * v + 4 * c) + (opt.perturb_pos && v == 0 && c == 0 ? 1.0 : 0.0);
                ss += (double(g[c]) - sc[c] * b) * (double(g[c]) - sc[c] * b);
              }
            }
            oa.pos_rms.Add(std::sqrt(ss / (3.0 * sd.vc)));
            oa.sx += sc[0], oa.sy += sc[1], oa.sz += sc[2], ++oa.fits;
            for (int c = 0; c < 3; ++c) oa.smin[c] = std::min(oa.smin[c], sc[c]), oa.smax[c] = std::max(oa.smax[c], sc[c]);
          }
        }
      }
    }
    ++analysed;
    double sh[kN];
    int best = -9;
    double bv = -1;
    bool tie = false;
    for (int oi = 0; oi < kN; ++oi) {
      sh[oi] = share_rows[oi] > 0 ? share_exact[oi] / share_rows[oi] : -1.0;
      if (sh[oi] > bv + 1e-12) bv = sh[oi], best = oi - kMid, tie = false;
      else if (sh[oi] >= 0 && std::fabs(sh[oi] - bv) <= 1e-12) tie = true;
    }
    if (!complete) ++edge;
    else if (best == -9 || tie) ++ties;
    else best_hist[best]++;
    if (csv) std::fprintf(csv, "%d,%llu,%d,%.5f,%.5f,%.5f,%.5f,%.5f,%d,%.0f\n", f, (unsigned long long)rec[kMid + 1].header.guest_frame, tie ? -9 : best, sh[0], sh[1], sh[2], sh[3], sh[4],
                          linked_n[kMid + 1], share_rows[kMid + 1]);
  }
  if (csv) std::fclose(csv);
  if (analysed == 0) { std::fprintf(stderr, "wrestler_pair: FAIL no frame could be analysed (%d skipped): no main-pass model draws or no record\n", skipped); return 2; }
  if (best_hist.empty()) { std::fprintf(stderr, "wrestler_pair: FAIL no frame voted for an offset: no skeleton linked to a stream object (%d ties, %d edge)\n", ties, edge); return 2; }

  int G = 0, gn = -1;
  for (auto& [o, n] : best_hist)
    if (n > gn) gn = n, G = o;
  const int gi = G + kMid;
  int hist_total = 0;
  for (auto& [o, n] : best_hist) hist_total += n;
  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("frames analysed %d (skipped %d, edge %d, ties %d); roster %zu pacs, %zu model entries; stream objects seen %zu\n", analysed, skipped, edge, ties, roster.pacs, roster.cands.size(),
              all_objs.size());
  std::printf("best offset histogram (record i against stream frame i+offset):");
  for (auto& [o, n] : best_hist) std::printf(" %+d:%d", o, n);
  std::printf("\n");

  // per skeleton, at the winning offset
  auto cand_label = [&](int ci) {
    const Cand& c = roster.cands[size_t(ci)];
    char b[96];
    std::snprintf(b, sizeof b, "ch%03d/attire%d/entry%u", c.pac, c.attire, c.entry);
    return std::string(b);
  };
  int n_sk = 0, n_exact = 0, n_mismatch = 0;
  bool counts_ok_all = true, idx_ok_all = true;
  for (auto& [id, A] : acc[gi]) {
    ++n_sk;
    std::set<uint32_t> bad_nodes, cmp_nodes;
    double maxd = 0;
    for (auto& [n, c] : A.node_cmp) cmp_nodes.insert(n);
    for (auto& [n, c] : A.node_bad) bad_nodes.insert(n), maxd = std::max(maxd, A.node_maxdiff[n]);
    const bool exact = A.rows > 0 && A.exact == A.rows && A.bad_node_index == 0;
    (exact ? n_exact : n_mismatch)++;
    uint64_t draws = 0, cok = 0, cbad = 0, iok = 0, ibad = 0, fits = 0;
    double rms = 0, sx = 0, sy = 0, sz = 0, spread = 0, lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
    size_t ncand = 0, ndist = 0, nsame = 0;
    std::set<uint32_t> parts;
    uint64_t stale_n = 0, stale_eq = 0;
    for (uint32_t addr : A.objects) {
      const ObjAcc& oa = objacc[addr];
      draws += oa.draws, cok += oa.count_ok, cbad += oa.count_bad, iok += oa.idx_ok, ibad += oa.idx_bad, fits += oa.fits;
      rms = std::max(rms, oa.pos_rms.mx), sx += oa.sx, sy += oa.sy, sz += oa.sz, stale_n += oa.stale_n, stale_eq += oa.stale_eq;
      parts.insert(oa.parts_seen.begin(), oa.parts_seen.end());
      for (int c = 0; c < 3; ++c) lo[c] = std::min(lo[c], oa.smin[c]), hi[c] = std::max(hi[c], oa.smax[c]);
      const Resolved& rr = resolved[addr];
      ncand = std::max(ncand, rr.cands.size());
      std::set<std::vector<uint32_t>> dist;
      for (int ci : rr.cands) {
        std::vector<uint32_t> sg;
        for (const auto& pp : roster.cands[size_t(ci)].parts) sg.insert(sg.end(), pp.palette, pp.palette + pp.palette_count), sg.push_back(0xFFFFFFFFu);
        dist.insert(sg);
      }
      ndist = std::max(ndist, dist.size());
      nsame = std::max(nsame, rr.cands.size() - dist.size());
    }
    if (fits)
      for (int c = 0; c < 3; ++c) spread = std::max(spread, hi[c] - lo[c]);
    std::string cl;
    for (int ci : A.cand_ids) cl += (cl.empty() ? "" : "|") + cand_label(ci);
    if (cbad) counts_ok_all = false;
    if (ibad) idx_ok_all = false;
    std::printf("SKELETON id=%s offset=%+d asset=%s nodes_compared=%zu frames=%llu objects=%zu candidates=%zu distinct_palettes=%zu same_palettes=%zu parts=%zu draws=%llu check_draws=%llu rows=%llu exact=%llu palette=%s", id.c_str(), G, cl.c_str(), cmp_nodes.size(),
                (unsigned long long)A.frames, A.objects.size(), ncand, ndist, nsame, parts.size(), (unsigned long long)A.draws, (unsigned long long)draws, (unsigned long long)A.rows, (unsigned long long)A.exact, exact ? "EXACT" : "MISMATCH");
    if (!exact) std::printf(" bad_nodes=%s max_diff=%.3e", Ranges(bad_nodes).c_str(), maxd);
    std::printf(" count_ok=%llu count_bad=%llu index_ok=%llu index_bad=%llu", (unsigned long long)cok, (unsigned long long)cbad, (unsigned long long)iok, (unsigned long long)ibad);
    if (fits) std::printf(" pos_scale=(%.4f,%.4f,%.4f) pos_scale_spread=%.3e pos_rms_max=%.3e", sx / double(fits), sy / double(fits), sz / double(fits), spread, rms);
    std::printf(" stale_block_equals_a_node=%llu/%llu\n", (unsigned long long)stale_eq, (unsigned long long)stale_n);
    (void)draws;
    if (opt.verbose)
      for (auto& [p, pr] : A.part_rows) std::printf("  part %u: rows %llu exact %llu\n", p, (unsigned long long)pr.first, (unsigned long long)pr.second);
  }
  // objects that did not link to a record skeleton
  size_t unresolved = 0, ambiguous = 0, unlinked = 0;
  for (uint32_t addr : all_objs) {
    const Resolved& rs = resolved[addr];
    const ObjAcc& oa = objacc[addr];
    if (rs.cands.empty()) { ++unresolved; continue; }
    std::set<std::vector<uint32_t>> distinct;  // candidates that differ in their palette lists
    for (int ci : rs.cands) {
      std::vector<uint32_t> sg;
      for (const auto& p : roster.cands[size_t(ci)].parts) sg.insert(sg.end(), p.palette, p.palette + p.palette_count), sg.push_back(0xFFFFFFFFu);
      distinct.insert(sg);
    }
    if (distinct.size() > 1) ++ambiguous;
    if (oa.linked[gi].empty()) ++unlinked;
    if (opt.verbose)
      std::printf("OBJECT %08X frames=%llu resolved=%llu candidates=%zu distinct=%zu linked=%s\n", addr, (unsigned long long)oa.frames_present, (unsigned long long)oa.frames_resolved, rs.cands.size(),
                  distinct.size(), oa.linked[gi].empty() ? "no" : oa.linked[gi].begin()->first.c_str());
  }
  std::printf("stream objects %zu: resolved to a roster model by content %zu (palette-list ambiguous %zu), no model fits %zu, resolved but no record skeleton %zu\n", all_objs.size(),
              all_objs.size() - unresolved, ambiguous, unresolved, unlinked);
  std::printf("offset table (rows exact / rows, linked objects pooled), all frames:\n");
  for (int oi = 0; oi < kN; ++oi) {
    uint64_t r = 0, e = 0;
    for (auto& [id, A] : acc[oi]) r += A.rows, e += A.exact;
    std::printf("  offset %+d: rows %llu exact %llu (%.4f)\n", oi - kMid, (unsigned long long)r, (unsigned long long)e, r ? double(e) / double(r) : 0.0);
  }
  std::printf("guest frame of record i vs frame i header: %llu mismatches\n", (unsigned long long)guest_mismatch);
  if (engine_declared) std::printf("links made through the record's declared engine address: %llu\n", (unsigned long long)engine_declared);
  std::printf("RESULT offset=%+d (%d of %d frames) skeletons=%d palette_exact=%d palette_mismatch=%d counts=%s indices=%s\n", G, gn, hist_total, n_sk, n_exact, n_mismatch,
              counts_ok_all ? "ok" : "MISMATCH", idx_ok_all ? "ok" : "MISMATCH");
  std::printf("run time %.1f s\n", dt);
  return 0;
}
