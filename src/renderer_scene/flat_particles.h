// S111 TITLESMOKE: where a FLAT frame's particle groups go among its 2D rows. Pure: no renderer, no game, no files (tools\scene\flat_particles_unit.cpp drives it; tools\scene\host_rig_flatseq.h
// drives it through the real SceneHost::DrawRows).
// The title / main menu / options / play menus draw their backdrop particles (the game's particle node with the thread's 2D screen camera; prtcls.pac section PCMN: Smoke0001 /
// Smoke0004 / Fire_01..04 / Rock) in two groups a frame BETWEEN the 2D rows (docs\research\s103\MP_menu_particles.md section 1).
// S111 FLATSEQ: the recorder stamps each batch with the 2D store's last call-order ticket at its node call + 1 (ParticleBatch::seq_after_p1, particle_source.h ParticleSource::TakeScreen,
// from quads2d_tap.h SeqNow = q2d::Store::LastTicket): the number every 2D row carries (Quad2D::seq). A batch goes BEFORE the first record row whose seq is larger, so a row the record lost
// (refused by Quad2DRowProblem at q2d::TakeFrame: a hole) or a row of another recording thread cannot shift it, as the old row COUNT did (docs\research\s111\TITLESMOKE.md review item 1).
// A record written before FLATSEQ (ParticleScreen::stamp 0) holds the row count there and is read by the old rule. The stream-free host (scene_host.cpp SceneHost::DrawRows) draws the
// batches of position p after the frame's first p rows through LiveParticles::RecordFlat, which draws only the batches whose PosOf is p.
#pragma once
#include <cstddef>
#include <cstdint>

#include "particle_tables.h"
#include "rows_quad2d.h"

namespace rdr_scene::flat_particles {

// Planted twins (tools\scene\flat_particles_unit.cpp --plant, tools\scene\host_rig_flatseq.h --plant): 1 = every batch after the last row (the row position ignored); 2 = the position off by one (the
// stamp taken as the bound itself); 3 = a batch with no stamp (0) is drawn flat too, after the last row (the old rule); 4 = the stamp read as the row COUNT whatever the record says (the rule before FLATSEQ).
inline int g_plant = 0;

// S111 TITLESMOKE review: the batch is the flat drawer's: the recorder stamps a row only on the batches of a node call made with the 2D screen camera (particle_source.h ParticleSource::TakeScreen);
// 0 = a scene-camera call of the same frame (or the tap off), which the screen camera must never place.
inline bool IsFlat(const ParticleBatch& b) { return b.seq_after_p1 != 0 || g_plant == 3; }

// The frame's particles were drawn by the node with its 2D screen camera (the record carries the ParticleScreen row, kScParticleScreen) and there is a screen call's batch to draw.
inline bool HasScreen(const ParticleTables& pt) {
  if (pt.screen.empty()) return false;
  for (const ParticleBatch& b : pt.batches)
    if (IsFlat(b)) return true;
  return false;
}

// S111 FLATSEQ: the record's batches carry the 2D store's ticket (ParticleScreen::stamp == kParticleStampSeq); a record written before it carries the row count.
inline bool SeqStamped(const ParticleTables& pt) { return !pt.screen.empty() && pt.screen[0].stamp == kParticleStampSeq && g_plant != 4; }

// How many of the frame's `n` 2D rows (`rows`, the record's, ascending by seq) come before flat batch `b`: the rows before the first one whose seq is past the ticket the node call saw
// (seq_after_p1 - 1); in an older record (SeqStamped false) the row count at the call (seq_after_p1 - 1), clamped to `n`.
inline size_t PosOf(const ParticleTables& pt, const ParticleBatch& b, const Quad2D* rows, size_t n) {
  if (g_plant == 1 || b.seq_after_p1 == 0) return n;
  if (!SeqStamped(pt)) {
    const size_t p = g_plant == 2 ? size_t(b.seq_after_p1) : size_t(b.seq_after_p1 - 1u);
    return p < n ? p : n;
  }
  const uint32_t last = g_plant == 2 ? b.seq_after_p1 : b.seq_after_p1 - 1u;   // the last ticket handed out before the node call
  size_t p = 0;
  while (p < n && rows && rows[p].seq <= last) ++p;
  return p;
}

// The distinct positions of the frame's batches, ascending. The node makes at most 16 calls a frame (particle_source.h kMaxCallsPerFrame), so 32 is never reached; a position past it is
// not kept (its batches are then never drawn and the host counts them particles_not_drawn).
inline constexpr size_t kMaxStops = 32;
struct Stops {
  size_t at[kMaxStops] = {};
  size_t n = 0;
};
inline Stops StopsOf(const ParticleTables& pt, const Quad2D* rows, size_t n) {
  Stops s;
  for (const ParticleBatch& b : pt.batches) {
    if (!IsFlat(b)) continue;
    const size_t p = PosOf(pt, b, rows, n);
    size_t k = 0;
    while (k < s.n && s.at[k] < p) ++k;
    if (k < s.n && s.at[k] == p) continue;
    if (s.n == kMaxStops) continue;
    for (size_t m = s.n; m > k; --m) s.at[m] = s.at[m - 1];
    s.at[k] = p, ++s.n;
  }
  return s;
}

// The flat batches at position `pos` (what one RecordFlat call draws).
inline size_t CountAt(const ParticleTables& pt, size_t pos, const Quad2D* rows, size_t n) {
  size_t c = 0;
  for (const ParticleBatch& b : pt.batches)
    if (IsFlat(b) && PosOf(pt, b, rows, n) == pos) ++c;
  return c;
}
// The frame's flat batches (what the host counts particles_not_drawn when it cannot draw them).
inline size_t FlatBatches(const ParticleTables& pt) {
  size_t c = 0;
  for (const ParticleBatch& b : pt.batches) c += IsFlat(b) ? 1 : 0;
  return c;
}

}  // namespace rdr_scene::flat_particles
