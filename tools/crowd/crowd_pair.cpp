// crowd_pair: the crowd promotion's pairing and pose test (Session 101, lane 36; docs\renderer\CROWD_PROMOTION_MAP.md section 6). 0 game runs.
// For each captured frame i it poses OUR crowd body (assets::crowd_body, the shipped PoseBody) with the scene record's palette for the
// type and compares the result with the stream's own skinned vertices (the vertex blob of the block draws at call site 0x82518574) of the
// frames i-1, i, i+1. The offset with the smallest residual is the record-to-stream pairing; the residual at that offset says whether the
// record's 22-matrix blocks are in the body's node order (per-node residuals). It also compares the record's member worlds with the stream's
// c76-c78 at the block and card sites.
//   crowd_pair --capture DIR --game DIR [--frames A-B] [--step N] [--csv FILE] [--swap A,B] [--shift K] [--verbose]
// --swap A,B and --shift K are the planted faults (they act inside the same code path): swap two palette nodes; read record i+K for frame i.
// Exit 0 = measured, 2 = a loud failure (no scene records, unreadable input...), 3 = bad usage. The summary line starts with "RESULT".
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/crowd_body.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/record_io.h"

namespace {
using rdr_adapter::CaptureReader;
using rdr_adapter::LoadedFrame;
using rdr_scene::SceneTables;

constexpr uint32_t kBlockSite = 0x82518574, kCardSite = 0x825130C4;
constexpr int kW = 2, kN = 2 * kW + 1;  // stream frames i-2 .. i+2 are read; the brief's i-1, i, i+1 are the middle three

struct Options {
  std::string capture, game, csv;
  int first = 0, last = -1, step = 1, swap_a = -1, swap_b = -1, shift = 0;
  bool verbose = false;
  float body_scale = 10.f;
};

struct Body {                      // one person type's LOD 0 body, in FILE space (the loader's Y mirror undone), flattened
  assets::Model model;             // stored vertices (file space)
  std::vector<std::array<float, 3>> stored;
  size_t verts = 0;
  bool ok = false;
};

struct Posed {
  std::vector<std::array<float, 3>> pos;
  std::vector<uint16_t> dominant;  // the heaviest node of each vertex
};

// Constant register c(n) of a bank: 4 floats.
const float* Reg(const rdr_adapter::ConstantBank& b, int n) { return reinterpret_cast<const float*>(b.dwords) + 4 * n; }

// c76-c78 -> the 16-float row-vector matrix (the op-21 recorder's transposed 12 floats: crowd_live.md "The member matrix").
void WorldFromRegs(const rdr_adapter::ConstantBank& b, float m[16]) {
  std::memset(m, 0, 16 * sizeof(float));
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) m[k * 4 + r] = Reg(b, 76 + r)[k];
  m[15] = 1.f;
}

struct Key {
  int64_t x, y, z;
  bool operator<(const Key& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
};
Key KeyOf(const float t[3]) { return {int64_t(std::llround(t[0] * 20)), int64_t(std::llround(t[1] * 20)), int64_t(std::llround(t[2] * 20))}; }

struct Member {
  uint32_t object = 0;
  int type = -1, lod = 0;
  bool card = false;
};

// "crowd:block:B:member:M" / "crowd:card:B:member:M" -> type B
bool ParseId(std::string_view id, bool& card, int& type) {
  int b = 0;
  std::string s(id);
  if (std::sscanf(s.c_str(), "crowd:block:%d:", &b) == 1) { card = false; type = b; return true; }
  if (std::sscanf(s.c_str(), "crowd:card:%d:", &b) == 1) { card = true; type = b; return true; }
  return false;
}

int ArenaOf(const SceneTables& t) {
  for (const auto& a : t.assets) {
    const std::string pac(t.Str(a.pac));
    const size_t p = pac.rfind("bg");
    int n = 0;
    if (p != std::string::npos && std::sscanf(pac.c_str() + p, "bg%d", &n) == 1) return n;
  }
  return -1;
}

struct Stat {  // running sum
  double sum = 0, mx = 0;
  size_t n = 0;
  void Add(double v) { sum += v; mx = std::max(mx, v); ++n; }
  double Mean() const { return n ? sum / double(n) : 0; }
};

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
    else if (a == "--verbose") opt.verbose = true;
    else if (a == "--body-scale") opt.body_scale = float(std::atof(next()));
    else { std::fprintf(stderr, "crowd_pair: unknown argument %s\n", a.c_str()); return 3; }
  }
  if (opt.capture.empty() || opt.game.empty()) { std::fprintf(stderr, "usage: crowd_pair --capture DIR --game DIR [--frames A-B] [--step N] [--csv F] [--swap A,B] [--shift K]\n"); return 3; }

  const auto t0 = std::chrono::steady_clock::now();
  CaptureReader cap;
  if (!cap.Open(opt.capture)) { std::fprintf(stderr, "crowd_pair: FAIL cannot open the capture at %s\n", opt.capture.c_str()); return 2; }
  if (cap.scene_record_count() == 0) {
    std::fprintf(stderr, "crowd_pair: FAIL %s has no scene records (scene\\frame_N.rsc): take the capture with --wwe2k14_scene_records\n", opt.capture.c_str());
    return 2;
  }
  if (!assets::crowd_body::Init(opt.game)) { std::fprintf(stderr, "crowd_pair: FAIL no game files at %s\n", opt.game.c_str()); return 2; }
  const int nframes = int(cap.frame_count());
  if (opt.last < 0 || opt.last >= nframes) opt.last = nframes - 1;

  std::map<int, Body> bodies;  // by type, for the capture's arena
  int arena = -1;
  auto get_body = [&](int type) -> Body* {
    auto it = bodies.find(type);
    if (it != bodies.end()) return it->second.ok ? &it->second : nullptr;
    Body& b = bodies[type];
    std::string err;
    if (!assets::crowd_body::LoadBody(arena, type, b.model, err)) { std::fprintf(stderr, "crowd_pair: body %d: %s\n", type, err.c_str()); return nullptr; }
    for (auto& me : b.model.meshes)
      for (auto& v : me.vertices) {  // viewer space -> file space (undo the loader's Y mirror once)
        v.pos[1] = -v.pos[1];
        v.normal[1] = -v.normal[1];
        for (int c = 0; c < 3; ++c) v.pos[c] *= opt.body_scale;  // measured, lane 36: the game's blob = stored x 10 x palette (docs\systems\crowd_live.md)
        b.stored.push_back({v.pos[0], v.pos[1], v.pos[2]});
      }
    b.verts = b.stored.size();
    b.ok = true;
    return &b;
  };

  std::FILE* csv = nullptr;
  if (!opt.csv.empty()) {
    csv = std::fopen(opt.csv.c_str(), "w");
    if (!csv) { std::fprintf(stderr, "crowd_pair: FAIL cannot write %s\n", opt.csv.c_str()); return 2; }
    std::fprintf(csv, "frame,guest_frame,best_offset,res_m2,res_m1,res_0,res_p1,res_p2,margin,types,block_draws,block_matched,block_world_maxdiff,card_draws,card_matched,card_world_maxdiff\n");
  }

  // overall tallies
  int analysed = 0, skipped = 0;
  std::map<int, int> best_hist;
  std::map<int, Stat> node_res;  // node -> residual at each frame's best offset
  Stat world_block[kN], world_card[kN];  // by stream offset index (offset = index - kW)
  size_t block_draws[kN] = {}, block_matched[kN] = {}, card_draws[kN] = {}, card_matched[kN] = {}, vert_count_mismatch[kN] = {}, types_compared[kN] = {};
  std::map<int, std::array<size_t, kN>> lod_match;  // record layer -> block draws matched, by offset index
  Stat overall_best;
  std::map<int, int> world_vote;  // best offset by member-world agreement (every complete frame, also those with no LOD-0 member to pose)
  std::map<int, bool> types_seen;
  int world_ties = 0;
  size_t key_collisions = 0;
  double scale_lo[4] = {1e30, 1e30, 1e30, 1e30}, scale_hi[4] = {};  // the record worlds' uniform scale (row 0 length) by layer

  LoadedFrame fr[kN];
  for (int f = opt.first; f <= opt.last; f += opt.step) {
    const int rec_idx = f + opt.shift;
    if (rec_idx < 0 || rec_idx >= nframes || !cap.HasSceneRecord(uint32_t(rec_idx))) { ++skipped; continue; }
    SceneTables rec;
    std::string err;
    if (!rdr_scene::ReadRecord(cap.SceneRecordPath(uint32_t(rec_idx)).string(), rec, err)) {
      std::fprintf(stderr, "crowd_pair: FAIL scene record %d: %s\n", rec_idx, err.c_str());
      return 2;
    }
    if (arena < 0) {
      arena = ArenaOf(rec);
      if (arena < 0) { std::fprintf(stderr, "crowd_pair: FAIL record %d names no arena pac\n", rec_idx); return 2; }
    }
    // The record's crowd members by world translation.
    std::map<Key, Member> members;
    size_t rec_blocks = 0;
    for (uint32_t oi = 0; oi < rec.objects.size(); ++oi) {
      const auto& ob = rec.objects[oi];
      if (ob.klass != rdr_adapter::DrawClass::Crowd) continue;
      Member m;
      m.object = oi;
      m.lod = ob.layer;
      if (!ParseId(rec.Str(ob.id), m.card, m.type)) continue;
      if (m.lod >= 3) m.card = true;
      if (!members.emplace(KeyOf(&ob.world.m[12]), m).second) ++key_collisions;  // two record members at one translation: the first is kept
      if (!m.card) ++rec_blocks;
    }
    if (members.empty()) { ++skipped; continue; }

    // Pose each type's LOD 0 body with the record's palette (once per type).
    std::map<int, Posed> posed;
    auto pose_type = [&](int type, const rdr_scene::Object& ob) -> const Posed* {
      auto it = posed.find(type);
      if (it != posed.end()) return &it->second;
      Body* b = get_body(type);
      if (!b) return nullptr;
      std::vector<float> pal(size_t(ob.bones_count) * 16);
      if (ob.bones_first + ob.bones_count > rec.bones.size()) return nullptr;
      std::memcpy(pal.data(), &rec.bones[ob.bones_first], pal.size() * sizeof(float));
      if (opt.swap_a >= 0 && opt.swap_b >= 0 && size_t(std::max(opt.swap_a, opt.swap_b)) < ob.bones_count)
        for (int k = 0; k < 16; ++k) std::swap(pal[size_t(opt.swap_a) * 16 + k], pal[size_t(opt.swap_b) * 16 + k]);
      assets::Model out;
      assets::crowd_body::PoseBody(b->model, pal.data(), ob.bones_count, out);  // b->model is in FILE space already, the palette is file space
      Posed& p = posed[type];
      for (auto& me : out.meshes)
        for (size_t vi = 0; vi < me.vertices.size(); ++vi) {
          p.pos.push_back({me.vertices[vi].pos[0], me.vertices[vi].pos[1], me.vertices[vi].pos[2]});
          const auto& sv = b->model.meshes[size_t(&me - out.meshes.data())].vertices[vi];
          int best = 0;
          for (int j = 1; j < 4; ++j)
            if (sv.weights[j] > sv.weights[best]) best = j;
          p.dominant.push_back(sv.bones[best]);
        }
      return &p;
    };

    double res[kN];
    bool have[kN] = {};
    for (double& r : res) r = 1e30;
    bool complete = true;  // every stream frame of the window exists (the capture's first and last frames are edges)
    std::array<std::map<int, Stat>, kN> node_by_off;
    size_t f_block[kN] = {}, f_block_m[kN] = {}, f_card[kN] = {}, f_card_m[kN] = {}, types_this[kN] = {};
    double f_bw[kN] = {}, f_cw[kN] = {};
    size_t f_exact[kN] = {};  // block draws whose c76-c78 equal the record member's world to within 1e-3
    for (int oi = 0; oi < kN; ++oi) {
      const int sf = f + (oi - kW);
      if (sf < 0 || sf >= nframes) { complete = false; continue; }
      LoadedFrame& L = fr[oi];
      if (!cap.LoadFrame(uint32_t(sf), L)) { std::fprintf(stderr, "crowd_pair: FAIL cannot load frame %d\n", sf); return 2; }
      Stat resid;
      std::map<std::pair<int, uint64_t>, bool> seen;
      for (uint32_t d = 0; d < L.draws.count; ++d) {
        const rdr_adapter::DrawOrigin* o = L.OriginOf(d);
        if (!o) continue;
        const bool block = o->call_site == kBlockSite, card = o->call_site == kCardSite;
        if (!block && !card) continue;
        const auto& dr = L.draws[d];
        if (dr.vs_bank >= L.constant_banks.count) continue;
        float wm[16];
        WorldFromRegs(L.constant_banks[dr.vs_bank], wm);
        (block ? f_block : f_card)[oi]++;
        auto it = members.find(KeyOf(&wm[12]));
        if (it == members.end()) continue;
        const Member& m = it->second;
        const auto& ob = rec.objects[m.object];
        {
          double md = 0;
          for (int k = 0; k < 12; ++k) md = std::max(md, double(std::fabs(wm[k] - ob.world.m[k])));
          (block ? f_block_m : f_card_m)[oi]++;
          double& fw = (block ? f_bw : f_cw)[oi];
          fw = std::max(fw, md);
          if (block && md < 1e-3) ++f_exact[oi];
          (block ? world_block : world_card)[oi].Add(md);
          if (block) lod_match[m.lod][size_t(oi)]++;
          if (oi == kW && m.lod >= 0 && m.lod < 4) {
            const double sc = std::sqrt(double(ob.world.m[0]) * ob.world.m[0] + double(ob.world.m[1]) * ob.world.m[1] + double(ob.world.m[2]) * ob.world.m[2]);
            scale_lo[m.lod] = std::min(scale_lo[m.lod], sc), scale_hi[m.lod] = std::max(scale_hi[m.lod], sc);
          }
        }
        if (!block || m.card || m.lod != 0) continue;
        // pose test: only LOD 0 block members, one comparison per distinct (type, vertex blob)
        if (!seen.emplace(std::make_pair(m.type, dr.vertex_offset), true).second) continue;
        const Posed* ps = pose_type(m.type, ob);
        if (!ps) continue;
        std::vector<uint8_t> vb;
        if (dr.layout >= L.layouts.count || !cap.ReadGeometry(dr.vertex_offset, dr.vertex_bytes, vb)) continue;
        const auto& lay = L.layouts[dr.layout];
        if (lay.element_count == 0 || L.elements[lay.element_first].vk_format != 106) continue;  // position = R32G32B32_SFLOAT
        const size_t stride = lay.host_stride, nv = stride ? vb.size() / stride : 0;
        const size_t eoff = L.elements[lay.element_first].host_offset;
        if (nv != ps->pos.size()) { ++vert_count_mismatch[oi]; if (opt.verbose) std::fprintf(stderr, "frame %d type %d: stream %zu verts, body %zu\n", f, m.type, nv, ps->pos.size()); continue; }
        if (opt.verbose && oi == kW) {  // which palette of the record fits this blob best (any type, any LOD)?
          Body* bb = get_body(m.type);
          std::map<uint32_t, uint32_t> ranges;  // first -> count
          std::map<uint32_t, std::string> who;
          for (const auto& o2 : rec.objects)
            if (o2.klass == rdr_adapter::DrawClass::Crowd && o2.bones_count) { ranges[o2.bones_first] = o2.bones_count; who[o2.bones_first] = std::string(rec.Str(o2.id)); }
          double bestr = 1e30;
          uint32_t bestf = 0;
          for (auto& [pf, pc] : ranges) {
            if (size_t(pf) + pc > rec.bones.size()) continue;  // a palette range past the record's bones: skipped
            std::vector<float> pal(size_t(pc) * 16);
            std::memcpy(pal.data(), &rec.bones[pf], pal.size() * sizeof(float));
            assets::Model out2;
            assets::crowd_body::PoseBody(bb->model, pal.data(), pc, out2);
            double s2 = 0;
            size_t vi = 0;
            for (auto& me : out2.meshes)
              for (auto& vtx : me.vertices) {
                float p[3];
                std::memcpy(p, vb.data() + vi * stride + eoff, 12);
                s2 += std::sqrt(double(p[0] - vtx.pos[0]) * (p[0] - vtx.pos[0]) + double(p[1] - vtx.pos[1]) * (p[1] - vtx.pos[1]) + double(p[2] - vtx.pos[2]) * (p[2] - vtx.pos[2]));
                ++vi;
              }
            s2 /= double(nv);
            if (s2 < bestr) bestr = s2, bestf = pf;
          }
          std::fprintf(stderr, "  type %d ): best palette of the record = %s with %.2f\n", m.type, who[bestf].c_str(), bestr);
        }
        double sum = 0;
        for (size_t v = 0; v < nv; ++v) {
          float p[3];
          std::memcpy(p, vb.data() + v * stride + eoff, 12);
          double e = 0;
          for (int c = 0; c < 3; ++c) e += double(p[c] - ps->pos[v][c]) * double(p[c] - ps->pos[v][c]);
          e = std::sqrt(e);
          sum += e;
          node_by_off[size_t(oi)][ps->dominant[v]].Add(e);
        }
        resid.Add(sum / double(nv));
        ++types_this[oi];
        types_seen[m.type] = true;
      }
      if (resid.n) { res[oi] = resid.Mean(); have[oi] = true; }
    }
    ++analysed;
    for (int oi = 0; oi < kN; ++oi) {
      block_draws[oi] += f_block[oi], block_matched[oi] += f_block_m[oi], card_draws[oi] += f_card[oi], card_matched[oi] += f_card_m[oi];
      types_compared[oi] += types_this[oi];
    }
    if (complete) {
      int bw = -1;
      double bn = 0;
      bool tie = false;
      for (int oi = 0; oi < kN; ++oi) {  // the share of the stream frame's block draws that equal a record member's world
        const double share = f_block[oi] ? double(f_exact[oi]) / double(f_block[oi]) : 0.0;
        if (share > bn + 1e-9) bn = share, bw = oi, tie = false;
        else if (share > 0 && std::fabs(share - bn) <= 1e-9) tie = true;
      }
      if (bw >= 0 && !tie) world_vote[bw - kW]++;
      else ++world_ties;
    }
    int best = -2;
    double bestv = 1e30, second = 1e30;
    for (int oi = 0; oi < kN; ++oi)
      if (have[oi]) {
        if (res[oi] < bestv) { second = bestv; bestv = res[oi]; best = oi - kW; }
        else if (res[oi] < second) second = res[oi];
      }
    if (best != -2 && complete) {  // edge frames (a window cut short by the capture's ends) are not counted
      best_hist[best]++;
      overall_best.Add(bestv);
      for (auto& [n, s] : node_by_off[size_t(best + kW)]) { node_res[n].sum += s.sum; node_res[n].n += s.n; node_res[n].mx = std::max(node_res[n].mx, s.mx); }
    }
    if (csv) {
      const size_t bi = best == -2 ? size_t(kW) : size_t(best + kW);  // the columns after the margin are read at the frame's best offset
      std::fprintf(csv, "%d,%llu,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%zu,%zu,%zu,%.5f,%zu,%zu,%.5f\n", f, (unsigned long long)rec.header.guest_frame, best,
                   have[0] ? res[0] : -1.0, have[1] ? res[1] : -1.0, have[2] ? res[2] : -1.0, have[3] ? res[3] : -1.0, have[4] ? res[4] : -1.0, second < 1e29 ? second - bestv : -1.0, types_this[bi], f_block[bi],
                   f_block_m[bi], f_bw[bi], f_card[bi], f_card_m[bi], f_cw[bi]);
    }
    (void)rec_blocks;
  }

  if (analysed == 0) { std::fprintf(stderr, "crowd_pair: FAIL no frame could be analysed (%d skipped)\n", skipped); return 2; }
  if (csv) std::fclose(csv);

  // Node-order verdict: per-node residual at the best offset against the median of the node means.
  std::vector<double> means;
  for (auto& [n, s] : node_res) means.push_back(s.Mean());
  std::sort(means.begin(), means.end());
  const double med = means.empty() ? 0 : means[means.size() / 2];
  std::vector<int> bad;
  for (auto& [n, s] : node_res)
    if (s.Mean() > std::max(4.0 * med, 0.01)) bad.push_back(n);  // against the best residual (about 0), not a fixed unit
  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  int best_overall = 0, best_n = -1;
  for (auto& [o, n] : best_hist)
    if (n > best_n) best_n = n, best_overall = o;
  const size_t bi = size_t(best_overall + kW);
  std::printf("frames analysed %d (skipped %d), arena %d; at offset %+d: types compared %zu, LOD-0 members whose stream blob has another vertex count %zu\n", analysed, skipped, arena,
              best_overall, types_compared[bi], vert_count_mismatch[bi]);
  std::printf("record world scale (row 0 length) by layer:");
  for (int l = 0; l < 4; ++l)
    if (scale_hi[l] > 0) std::printf(" %d: %.4f..%.4f", l, scale_lo[l], scale_hi[l]);
  std::printf("\n");
  std::printf("person types compared:");
  for (auto& [t, _] : types_seen) std::printf(" %d", t);
  std::printf("\nworld vote (best offset by exact member worlds, every complete frame):");
  for (auto& [o, n] : world_vote) std::printf(" %+d:%d", o, n);
  std::printf(" (ties or none: %d)", world_ties);
  std::printf("\n");
  std::printf("best offset histogram (record i against stream frame i+offset):");
  for (auto& [o, n] : best_hist) std::printf(" %+d:%d", o, n);
  std::printf("\nmean best residual %.3e (max frame %.3e) game units\n", overall_best.Mean(), overall_best.mx);
  std::printf("per-node residual (mean/max at the best offset):\n");
  for (auto& [n, s] : node_res) std::printf("  node %2d: mean %.3e max %.3e (%zu verts)\n", n, s.Mean(), s.mx, s.n);
  for (int oi = 0; oi < kN; ++oi)
    std::printf("member worlds, stream offset %+d: block draws %zu matched %zu (c76-c78 max diff %.3e, mean %.3e); card draws %zu matched %zu (max diff %.3e, mean %.3e)\n", oi - kW,
                block_draws[oi], block_matched[oi], world_block[oi].mx, world_block[oi].Mean(), card_draws[oi], card_matched[oi], world_card[oi].mx, world_card[oi].Mean());
  for (auto& [l, v] : lod_match) std::printf("  record layer %d (%s): block draws matched by offset -2..+2: %zu / %zu / %zu / %zu / %zu\n", l, l == 0 ? "LOD 0" : l == 1 ? "LOD 1" : l == 2 ? "LOD 2" : "card", v[0], v[1], v[2], v[3], v[4]);
  // The verdict: the pose test needs LOD 0 comparisons (else it proves nothing: loud), the mean residual at the best offset must be small, and
  // no node may stand out (a swapped pair of palette slots shows as exactly those nodes).
  if (best_hist.empty() || node_res.empty()) {
    std::fprintf(stderr, "crowd_pair: FAIL no LOD-0 block member was compared on a complete window: the pose test proved nothing\n");
    return 2;
  }
  const char* verdict = !bad.empty() ? "MISMATCH" : overall_best.Mean() > 0.05 ? "POOR" : "ok";
  std::printf("RESULT offset=%+d (%d of %d frames) node_order=%s", best_overall, best_n, analysed, verdict);
  if (!bad.empty()) {
    std::printf(" nodes=");
    for (size_t i = 0; i < bad.size(); ++i) std::printf("%s%d", i ? "," : "", bad[i]);
  }
  std::printf(" residual=%.3e\n", overall_best.Mean());
  std::printf("record member translation collisions (first kept): %zu; LOD-0 vertex-count mismatches by offset -2..+2: %zu / %zu / %zu / %zu / %zu\n", key_collisions,
              vert_count_mismatch[0], vert_count_mismatch[1], vert_count_mismatch[2], vert_count_mismatch[3], vert_count_mismatch[4]);
  std::printf("run time %.1f s\n", dt);
  return 0;
}
