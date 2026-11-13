// Lane 132 (S101): reads the particle-site draws of a capture and says what the soft variant looks like (shape, which VS registers change between neighbours).
//   soft_probe CAPTURE_DIR FIRST_FRAME LAST_FRAME      (0 game runs; reads the capture only; counts and register numbers, nothing from the game is printed as bytes)
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/particle_runs.h"

using namespace rdr_adapter;
using namespace rdr_adapter::particles;

int main(int argc, char** argv) {
  if (argc < 4) return std::printf("FAIL  usage: soft_probe CAPTURE FIRST LAST\n"), 1;
  CaptureReader capture;
  if (!capture.Open(argv[1])) return std::printf("FAIL  cannot open %s\n", argv[1]), 1;
  const uint32_t first = uint32_t(std::atoi(argv[2])), last = uint32_t(std::atoi(argv[3]));
  std::map<std::string, uint64_t> shapes;
  std::map<int, uint64_t> diffregs;  // registers that differ between two adjacent soft draws
  std::map<uint32_t, uint64_t> runlen;
  uint64_t draws = 0, pairs = 0, same_but_world = 0;
  for (uint32_t f = first; f <= last; ++f) {
    LoadedFrame frame;
    if (!capture.LoadFrame(f, frame)) continue;
    int prev = -1;
    uint32_t len = 0;
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind != OpKind::Draw) { if (len) ++runlen[len]; len = 0; prev = -1; continue; }
      const Draw& d = frame.draws[op.index];
      const DrawOrigin* o = frame.OriginOf(op.index);
      const bool soft = o && d.vs_hash == kSoftVs && d.ps_hash == kSoftPs;
      if (!soft) { if (len) ++runlen[len]; len = 0; prev = -1; continue; }
      ++draws;
      char buf[400];
      const VertexLayout& l = frame.layouts[d.layout];
      std::string el;
      for (uint32_t e = 0; e < l.element_count; ++e) {
        const VertexElement& v = frame.elements[l.element_first + e];
        el += std::to_string(v.host_offset) + "/" + std::to_string(v.vk_format) + " ";
      }
      std::snprintf(buf, sizeof buf, "site %08X op %u vcount %u vbytes %u ibytes %u prim %u topo %u inline %d stride %u elements %u [%s] textures %u", o->call_site, o->op, d.vertex_count,
                    d.vertex_bytes, d.index_bytes, d.source_prim, frame.fixeds[d.fixed].state.topology, (d.flags & kDrawInline) ? 1 : 0, l.host_stride, l.element_count, el.c_str(), d.texture_count);
      ++shapes[buf];
      if (prev >= 0) {
        const Draw& p = frame.draws[prev];
        ++pairs;
        const uint32_t* x = frame.constant_banks[p.vs_bank].dwords;
        const uint32_t* y = frame.constant_banks[d.vs_bank].dwords;
        std::set<int> regs;
        for (int r = 0; r < 256; ++r)
          for (int k = 0; k < 4; ++k)
            if (x[r * 4 + k] != y[r * 4 + k]) regs.insert(r);
        for (int r : regs) ++diffregs[r];
        if (p.ps_bank != d.ps_bank && std::memcmp(&frame.constant_banks[p.ps_bank], &frame.constant_banks[d.ps_bank], sizeof(ConstantBank)) != 0) ++diffregs[1000];
        if (frame.origins[prev].call_site != o->call_site) ++diffregs[1001];
        for (uint32_t t = 0; t < d.texture_count && t < p.texture_count; ++t)
          if (frame.texture_slots[p.texture_first + t].image != frame.texture_slots[d.texture_first + t].image) ++diffregs[2000 + t];
        (void)same_but_world;
      }
      prev = int(op.index);
      ++len;
    }
    if (len) ++runlen[len];
  }
  std::printf("soft draws %llu, adjacent pairs %llu\n", (unsigned long long)draws, (unsigned long long)pairs);
  for (auto& s : shapes) std::printf("shape x%llu: %s\n", (unsigned long long)s.second, s.first.c_str());
  for (auto& r : diffregs) std::printf("differs between adjacent soft draws: %s %d x%llu\n", r.first == 1000 ? "ps_bank" : r.first == 1001 ? "site" : r.first >= 2000 ? "texture slot" : "vs reg c", r.first >= 2000 ? r.first - 2000 : r.first, (unsigned long long)r.second);
  for (auto& r : runlen) std::printf("adjacent soft run length %u x%llu\n", r.first, (unsigned long long)r.second);
  return 0;
}
