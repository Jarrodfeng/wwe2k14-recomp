// The ring ropes' node rows from the scene record (Session 103, lane B2F; live_map_rope.h). Record side only: no renderer, no assets, so the offline units (build_e5_unit.ps1) link it with live_map_e2_input.cpp.
#include <cstdio>
#include <string>

#include "live_map_rope.h"
#include "record_io.h"
#include "rows_ring.h"   // S106 G4-2

namespace rdr_scene {

void BuildRopeInput(const SceneTables& t, RopeInput& out) {
  out = RopeInput();
  for (const AssetRef& a : t.assets) {   // the arena: the SAME rule as ArenaCore::ArenaOfRecord (arena_ours_core.cpp; that file cannot be linked into the offline units): the bgNN of the first asset whose pac names one
    const std::string pac(t.Str(a.pac));
    const size_t p = pac.rfind("bg");
    int n = 0;
    if (p != std::string::npos && std::sscanf(pac.c_str() + p, "bg%d", &n) == 1) {
      out.arena = n;
      break;
    }
  }
  // the rows to use: ours (kRrRecomputed) when the record has any rope entry of ours, else the game's own copy (rf_ring.cpp's per-class rule)
  for (const RingRows& e : t.ring_rows)
    if ((e.klass == kRcRopeHi || e.klass == kRcRopeLo) && (e.flags & kRrRecomputed)) out.recomputed = true;
  for (const RingRows& e : t.ring_rows) {
    if (e.klass != kRcRopeHi && e.klass != kRcRopeLo) continue;
    ++out.calls_seen;
    if (bool(e.flags & kRrRecomputed) != out.recomputed) continue;
    if (e.n_nodes == 0 || uint64_t(e.first_node) + e.n_nodes > t.ring_nodes.size() || out.count >= kRopeMax) {
      ++out.unusable;
      continue;
    }
    RopeInput::Rope& r = out.rope[out.count++];
    r.first = uint32_t(out.rows.size() / 12), r.n = e.n_nodes;
    double sx = 0, sz = 0;
    for (uint32_t k = 0; k < e.n_nodes; ++k) {
      const RingNodeRows& nr = t.ring_nodes[e.first_node + k];
      out.rows.insert(out.rows.end(), nr.rows, nr.rows + 12);
      sx += nr.rows[3], sz += nr.rows[11];   // the translation of the node (row k = (m[k], m[4 + k], m[8 + k], m[12 + k]))
    }
    r.cx = float(sx / e.n_nodes), r.cz = float(sz / e.n_nodes);
  }
}

}  // namespace rdr_scene
