// S103 W1: the palette census. For every wrestler draw of the stream (main-pass model parts), every row triple of the skinning constants c76.. is looked up in the scene
// records near that frame (the draw's object = the record object whose engine address equals the draw's DrawOrigin::object). Reports, per record node and per record
// offset (record index - (frame-1)), how many triples are bit-equal, so the node groups that are NOT equal at the frame the contract names show up.
//   w1_palette_census --capture DIR [--frames A-B] [--step N] [--names FILE] [--object HEX]
// --names FILE: one node name per line (node k = line k, 1-based palette index k+1 in the game's tables): prints group totals (body / fingers / face).
// Read only. 0 game runs.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_adapter/draw_replace_hook.h"
#include "../../src/renderer_scene/record_io.h"

using namespace rdr_adapter;
using rdr_scene::SceneTables;

static const float* Reg(const ConstantBank& b, int n) { return reinterpret_cast<const float*>(b.dwords) + 4 * n; }
static uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

int main(int argc, char** argv) {
  std::string capture, names_file; bool lines = false;
  int first = 0, last = -1, step = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--capture") capture = next();
    else if (a == "--frames") std::sscanf(next(), "%d-%d", &first, &last);
    else if (a == "--step") step = std::max(1, std::atoi(next()));
    else if (a == "--lines") lines = true;
    else if (a == "--names") names_file = next();
    else { std::fprintf(stderr, "unknown %s\n", a.c_str()); return 3; }
  }
  CaptureReader cap;
  if (!cap.Open(capture)) { std::fprintf(stderr, "cannot open capture\n"); return 2; }
  std::vector<std::string> names;
  if (!names_file.empty()) { std::ifstream in(names_file); std::string l; while (std::getline(in, l)) { while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back(); names.push_back(l); } }
  const int nframes = int(cap.frame_count());
  if (last < 0 || last >= nframes) last = nframes - 1;
  constexpr int kWin = 5;   // record indices f-3 .. f+1, offset o = idx - (f-1) in -2..+2
  // tally[node][offset+2], per object
  struct Obj { std::vector<std::array<uint64_t, kWin>> hit; uint64_t triples = 0, none = 0, any_off0 = 0; uint32_t count = 0; };
  std::map<uint32_t, Obj> objs;
  std::map<int, uint64_t> best_off;   // the offset that matched (first found, nearest 0 first) per triple
  uint64_t draws = 0, triples = 0, matched = 0, unmatched_nonzero = 0, zero_rows = 0;
  std::map<uint32_t, std::map<int, uint64_t>> unmatched_by_k;   // object -> palette slot k
  LoadedFrame fr;
  std::map<int, std::shared_ptr<SceneTables>> cache;
  auto get_rec = [&](int idx) -> std::shared_ptr<SceneTables> {
    if (idx < 0 || !cap.HasSceneRecord(uint32_t(idx))) return nullptr;
    auto it = cache.find(idx);
    if (it != cache.end()) return it->second;
    auto t = std::make_shared<SceneTables>();
    std::string err;
    if (!rdr_scene::ReadRecord(cap.SceneRecordPath(uint32_t(idx)).string(), *t, err)) t.reset();
    cache[idx] = t;
    if (cache.size() > 40) cache.erase(cache.begin());
    return t;
  };
  for (int f = first; f <= last; f += step) {
    if (!cap.LoadFrame(uint32_t(f), fr)) continue;
    std::shared_ptr<SceneTables> recs[kWin];
    bool any = false;
    for (int o = -2; o <= 2; ++o) { recs[o + 2] = get_rec(f - 1 + o); any |= recs[o + 2] != nullptr; }
    if (!recs[2]) continue;
    for (uint32_t d = 0; d < fr.draws.count; ++d) {
      const DrawOrigin* org = fr.OriginOf(d);
      if (!org || (org->pass_site != kModelPartColourSite && org->pass_site != kModelPartMirrorSite) || org->object == 0) continue;
      const Draw& dr = fr.draws[d];
      if (dr.vs_bank >= fr.constant_banks.count) continue;
      // find the object in the offset-0 record
      int oi = -1;
      for (size_t i = 0; i < recs[2]->engine_objects.size(); ++i) if (recs[2]->engine_objects[i] == org->object) { oi = int(i); break; }
      if (oi < 0) continue;
      const rdr_scene::Object& ro = recs[2]->objects[size_t(oi)];
      if (!(ro.flags & rdr_scene::kObjSkinned) || ro.bones_count < 60) continue;   // the body skeleton only
      Obj& ob = objs[org->object];
      if (ob.hit.empty()) ob.hit.assign(ro.bones_count, std::array<uint64_t, kWin>{});
      ob.count = ro.bones_count;
      ++draws;
      const ConstantBank& bank = fr.constant_banks[dr.vs_bank];
      // palette rows used: scan k while the triple is non-zero (rows after the part's own are stale, still valid palette rows of the same record or zero)
      for (int k = 0; k < 20; ++k) {
        const float* r0 = Reg(bank, 76 + 3 * k);
        const float* r1 = Reg(bank, 77 + 3 * k);
        const float* r2 = Reg(bank, 78 + 3 * k);
        bool zero = true;
        for (int c = 0; c < 4; ++c) zero = zero && Bits(r0[c]) == 0 && Bits(r1[c]) == 0 && Bits(r2[c]) == 0;
        if (zero) { ++zero_rows; continue; }
        ++triples; ++ob.triples;
        int found_off = 99; uint32_t found_node = 0; bool found = false;
        // record offsets nearest the contract's first: 0, then -1, +1, -2, +2
        for (int oo : {0, -1, 1, -2, 2}) {
          const auto& rc = recs[oo + 2];
          if (!rc) continue;
          int j2 = -1;
          for (size_t i = 0; i < rc->engine_objects.size(); ++i) if (rc->engine_objects[i] == org->object) { j2 = int(i); break; }
          if (j2 < 0) continue;
          const rdr_scene::Object& o2 = rc->objects[size_t(j2)];
          for (uint32_t n = 0; n < o2.bones_count && n < ob.hit.size(); ++n) {
            const rdr_scene::Mat4& M = rc->bones[o2.bones_first + n];
            bool same = true;
            const float* rr[3] = {r0, r1, r2};
            for (int j = 0; j < 3 && same; ++j)
              for (int c = 0; c < 4; ++c) if (Bits(rr[j][c]) != Bits(M.m[c * 4 + j])) { same = false; break; }
            if (same) { found = true; found_off = oo; found_node = n; ++ob.hit[n][size_t(oo + 2)]; break; }
          }
          if (found) break;
        }
        if (found) { ++matched; ++best_off[found_off]; (void)found_node; }
        else { ++unmatched_nonzero; ++ob.none; ++unmatched_by_k[org->object][k]; if (lines) std::printf("UNM frame %d draw %u obj 0x%08X part %u k %d vcount %u site 0x%X\n", f, d, org->object, org->part, k, dr.vertex_count, org->pass_site); }
      }
    }
  }
  std::printf("draws %llu, triples %llu, matched %llu, unmatched_nonzero %llu, zero_triples %llu\n", (unsigned long long)draws, (unsigned long long)triples, (unsigned long long)matched, (unsigned long long)unmatched_nonzero, (unsigned long long)zero_rows);
  for (auto& kv : best_off) std::printf("MATCH_OFFSET %d %llu\n", kv.first, (unsigned long long)kv.second);
  for (auto& kv : objs) {
    std::printf("OBJECT 0x%08X nodes %u triples %llu unmatched %llu\n", kv.first, kv.second.count, (unsigned long long)kv.second.triples, (unsigned long long)kv.second.none);
    for (uint32_t n = 0; n < kv.second.hit.size(); ++n) {
      const auto& h = kv.second.hit[n];
      uint64_t tot = 0; for (auto x : h) tot += x;
      if (!tot) continue;
      std::printf("NODE %u %s off(-2..+2)= %llu %llu %llu %llu %llu\n", n, n < names.size() ? names[n].c_str() : "?", (unsigned long long)h[0], (unsigned long long)h[1], (unsigned long long)h[2], (unsigned long long)h[3], (unsigned long long)h[4]);
    }
    for (auto& ku : unmatched_by_k[kv.first]) std::printf("UNMATCHED_SLOT k=%d %llu\n", ku.first, (unsigned long long)ku.second);
  }
  return 0;
}
