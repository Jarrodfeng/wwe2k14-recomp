// Lane C (Session 103): the arena from the live record: stage slots (the record's twins and absent slots), light sets by id every frame, and the moving pieces at the world
// the record carries (kFxSlotWorld rows). Replaces today's single whole-arena object of rsc_draw (lane R moves that to rf_arena.cpp; this file's row takes its place).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "../assets/stage_pools.h"  // S103 L
#include "rows_stage_fx.h"   // S106 G4-2
#include "rows_stage_look.h"
#include "rf_c.h"
#include "rf_crowd.h"  // S103 B4F
#include "ring_pose.h"   // S110 BARRICADE: RingInstancePad, WorldFromRowsCore
#include "stage_posed.h"   // S110 BARRICADE
#include "stage_slot_vis.h"   // VisFromRows (the marker's stage generation)
#include "stage_look.h"  // S103 L
#include "stage_lmpage.h"  // S103 L8

namespace rdr_scene::rf::c {

namespace {
inline bool Want(const BuildCtx& ctx, Family f) { return (ctx.families & (1u << uint32_t(f))) != 0; }
}  // namespace

// S103 L / L2 (STAGE_LIGHTING.md class B): the stage light-map page of every family model of the arena. The models are built with the match page (fade_lightmap01, settled); while an entrance plays the
// game's page is the entrance's: fade_lightmap00 + the 8 floor pools + the follow spot, which the record's kSlPool / kSlSpot rows describe (stage_look.h Pools); the page is composed HERE from the arena's own
// cache (ArenaSet::pcache, copied at registration: never the loader's global cache) and uploaded when its key changes (0 match, 1 base page = no rows, else the rows' hash).
// WWE_RDR_L2_PAGE=<raw RGBA8 file> (dev diagnosis only) uploads a page read by the rig from the game's own frame instead.
static void UpdatePages(BuildCtx& ctx, ArenaSet& as, bool entrance, const std::vector<assets::stage_pools::Pool>& pools) {
  if (as.page_base.empty() || as.page_w == 0 || as.page_h == 0) return;
  static const std::vector<uint8_t> dev_page = [] {
    std::vector<uint8_t> v;
    if (const char* p = std::getenv("WWE_RDR_L2_PAGE")) {
      if (FILE* f = std::fopen(p, "rb")) {
        v.resize(512 * 512 * 4);
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
        std::fclose(f);
      }
    }
    return v;
  }();
  // S103 L8 (arena_lightmap.md "S103 L8"): with --wwe2k14_rdr_stage_lmpage the match page is the game's page pass: page 00, page 01 faded over it as the page clock runs (600 frames), then page 01.
  // S103 FD (CF2): the clock is the GAME's own: the record's kSlPageClock row carries t (the float at the page object + 108) and its run / entrance words, read at the latch; the alpha is the game's
  // (assets::stage_pools::PageAlpha255, sub_825BAA28). No clock of ours: a second match, a no-entrance match, a replay rewind and a pause follow the game's t. No row (the object not found) = the
  // settled page, as before. Key kFadeKey + alpha while the fade runs.
  int fade_a = 255;
  if (!entrance && stage_lmpage::On()) {
    for (const StageLookRow& r : ctx.t.stage_look)
      if (r.kind == kSlPageClock) fade_a = assets::stage_pools::PageAlpha255(true, (r.flags & 2) != 0, r.dir[0]);
  }
  constexpr uint64_t kFadeKey = 0x4C384641444500ull;  // "L8FADE"
  const uint64_t key = !entrance ? (fade_a < 255 ? kFadeKey + uint64_t(fade_a) : 0) : (!dev_page.empty() && dev_page.size() == as.page_base.size()) ? 3 : pools.empty() ? 1 : stage_look::PoolsHash(pools);
  // B1 (review): compose only when some family model does not hold this key yet (a steady entrance composes nothing)
  bool stale = false;
  ForEachFamilyModel(as, [&](const FamilyModel& fm) { stale = stale || (fm.model >= 0 && fm.page_tex >= 0 && fm.page_key != key); });   // S103 SOF: part, cuts, moving (rf_c.h)
  const bool crowd_stale = crowd::PageStale(ctx.models.front_end(), key);   // S103 B4F: the crowd's models take the same page (the game's crowd binds the stage page too)
  if (!stale && !crowd_stale) return;
  std::vector<uint8_t> composed;
  if (!entrance && fade_a < 255 && as.pcache.ok) assets::stage_pools::FadeFrom(as.pcache, fade_a, composed);  // S103 L8
  else if (key > 3 && as.pcache.ok) assets::stage_pools::ComposeFrom(as.pcache, pools, composed);
  const char* const dump_path = std::getenv("WWE_RDR_L2_DUMP");   // S103 PS1: read per call (this runs only when a page is composed), so one rig process can give each job its own dump file; "" = none
  if (const char* dump = dump_path; dump && *dump && !composed.empty()) {  // S103 L2 (dev): the composed page as raw RGBA8, to compare with the game's own
    if (FILE* f = std::fopen(dump, "wb")) { std::fwrite(composed.data(), 1, composed.size(), f); std::fclose(f); }
    for (const assets::stage_pools::Pool& p : pools) std::fprintf(stderr, "[L2] pool x %.0f z %.0f half %.1f rgb %.3f %.3f %.3f disk %d\n", p.x, p.z, p.half, p.rgb[0], p.rgb[1], p.rgb[2], int(p.disk));
  }
  const auto one = [&](FamilyModel& fm) {
    if (fm.model < 0 || fm.page_tex < 0 || fm.page_key == key) return;
    const std::vector<uint8_t>& px = key == 0 ? (as.page_match.size() == as.page_base.size() ? as.page_match : as.page_base) : key == 3 ? dev_page : (key > 3 && !composed.empty()) ? composed : as.page_base;
    const bool ok = ctx.models.front_end().UpdatePage(fm.model, size_t(fm.page_tex), key, as.page_w, as.page_h, px.data());   // S103 UP: models of the set share ONE image: the first call sends, the rest see the key held
    if (ok) fm.page_key = key;
    if (std::getenv("WWE_RDR_L2_LOG")) std::fprintf(stderr, "[L2] page upload model %d tex %d key %llx pools %zu composed %zu -> %d\n", fm.model, fm.page_tex, (unsigned long long)key, pools.size(), composed.size(), int(ok));
  };
  ForEachFamilyModel(as, one);
  if (crowd_stale) {   // S103 B4F: the same pixels the family models just took
    const std::vector<uint8_t>& px = key == 0 ? (as.page_match.size() == as.page_base.size() ? as.page_match : as.page_base) : key == 3 ? dev_page : (key > 3 && !composed.empty()) ? composed : as.page_base;
    crowd::SetStagePage(ctx.models.front_end(), key, as.page_w, as.page_h, px.data());
  }
}

bool ApplyArenaLive(BuildCtx& ctx) {
  if (!Want(ctx, Family::Arena)) return true;
  std::string why;
  bool miss = false;
  ArenaSet* as = EnsureArena(ctx, why, miss);
  if (!as) {  // not resident = a MISS: the frame stays the game's (return false); any other reason skips the class with it
    Report(ctx, Family::Arena, 0, 0, 1, why);
    return !miss;
  }
  ctx.models.front_end().SetCurrentPageGroup(!as->page_base.empty() && as->page_w && as->page_h ? as->page_group : 0);   // S103 UP (review F2): the live set's group, set before the warm check (a warm build and a pageless arena included): crowd models made now join ITS image, never a retired set's
  std::vector<assets::stage_pools::Pool> pools;  // S103 L2
  bool pools_incomplete = false;
  stage_look::Pools(ctx.t, pools, &pools_incomplete);
  if (!ctx.warm()) UpdatePages(ctx, *as, stage_look::PageEntrance(ctx.t), pools);  // S103 L (FRAME_CONTRACT 7 item 6: a warm build only asks for assets); S111 PAGEPOOLS: the page's own latch (walk-time)
  // S103 RGF: the game's order = the loader's stage array order: the lead static meshes, then for each moving slot (in the order of its first mesh) the slot's piece and the static meshes that follow it
  // (rf_c_core.cpp, as->cuts). With no cut (no slot moves, or the planted twin) the old shape: the whole static model, then the moving slots.
  const FamilyModel& lead = as->part[size_t(Part::Arena)];
  const FamilyModel* fm_ptr = &lead;   // the set / light bookkeeping source: the first segment that has a model
  for (const auto& c : as->cuts)
    if (fm_ptr->model < 0 && c.seg.model >= 0) fm_ptr = &c.seg;
  const FamilyModel& fm = *fm_ptr;
  uint32_t drawn = 0, skipped = 0, static_meshes = lead.meshes;
  std::string reason;
  const auto push_static = [&](const FamilyModel& s) {
    if (s.model < 0) return;
    Object o;
    o.asset = uint32_t(s.model);
    o.world = IdentityMat4();
    LogOrder("arena");   // S103 FOB
    ctx.frame.objects.push_back(o);
    ctx.frame.model_lights.push_back(LightsFor(ctx.t, s, uint32_t(s.model)));
    drawn += s.meshes;
  };
  for (const auto& c : as->cuts) static_meshes += c.seg.meshes;
  // the moving pieces: the last row of each slot wins (a record carries one per slot per frame)
  std::map<int, const StageFxRow*> rows;
  for (const StageFxRow& r : ctx.t.stage_fx)
    if (r.kind == kFxSlotWorld && !PlantsRef().ignore_slot_worlds) rows[int(r.slot)] = &r;
  const auto push_moving = [&](int slot, const StageFxRow* row) {
    auto it = as->moving.find(slot);
    if (it == as->moving.end() || it->second.model < 0) {
      ++skipped;
      if (reason.empty()) reason = PlantsRef().ignore_slot_worlds ? "slot worlds ignored (planted)" : "slot " + std::to_string(slot) + " has no meshes in the arena";
      return;
    }
    const assets::Matrix w = WorldFromRows(row->world);
    Object o;
    o.asset = uint32_t(it->second.model);
    std::memcpy(o.world.m, w.data(), sizeof o.world.m);
    LogOrder("arena");   // S103 FOB
    ctx.frame.objects.push_back(o);
    ctx.frame.model_lights.push_back(LightsFor(ctx.t, it->second, uint32_t(it->second.model)));
    drawn += it->second.meshes;
  };
  // S110 BARRICADE (docs\research\s110\BARRICADE.md): a slot whose model has more than one node is drawn with the palette the game holds for it (the record's kFxSlotPalette rows, stage_posed.h): ITS one-slot model
  // (rf_c_core.cpp, built skinned) at identity world with the node rows made viewer-space matrices as the frame's bones, node index = pool index = the bound vertices' node (as the ring's GPU pieces, rf_ring.cpp).
  // false = the slot has no skinned model of its own (nothing drawn, nothing counted: the static arena still holds it).
  PosedAdoptRows(ctx.t.stage_fx, as->arena, as->posed_keep);   // the rows of this frame under THIS arena's stage marker, then the LAST rows of this stage generation (a frame without rows never makes a rig vanish)
  std::map<int, PosedSlot> posed = as->posed_keep.last;
  const auto push_posed = [&](const PosedSlot& ps) {
    auto it = as->moving.find(int(ps.slot));
    if (it == as->moving.end() || it->second.model < 0 || !it->second.posed) return false;
    Object o;
    o.asset = uint32_t(it->second.model);
    o.world = IdentityMat4();
    o.pad = RingInstancePad(0, false);
    o.flags |= kObjSkinned;
    o.bones_first = uint32_t(ctx.frame.bones.size());
    o.bones_count = ps.Nodes();
    for (uint32_t k = 0; k < ps.Nodes(); ++k) {
      const assets::Matrix m = WorldFromRowsCore(&ps.rows[size_t(k) * 12u]);
      Mat4 b;
      std::memcpy(b.m, m.data(), sizeof b.m);
      ctx.frame.bones.push_back(b);
    }
    LogOrder("arena");
    ctx.frame.objects.push_back(o);
    ctx.frame.model_lights.push_back(LightsFor(ctx.t, it->second, uint32_t(it->second.model)));
    drawn += it->second.meshes;
    return true;
  };
  push_static(lead);
  for (const auto& c : as->cuts) {
    if (auto p = posed.find(c.slot); p != posed.end() && push_posed(p->second)) posed.erase(p);   // S110 BARRICADE: a slot built skinned is drawn posed here (one not built skinned stands in the static arena: its rows are unused)
    auto r = rows.find(c.slot);
    if (r != rows.end()) push_moving(c.slot, r->second), rows.erase(r);   // S103 SOF: a pushed row leaves `rows` (no per-frame set of placed slots)
    push_static(c.seg);
  }
  for (const auto& kv : rows)   // what is left: a row whose slot has no cut (no meshes / the planted order): after the static arena
    push_moving(kv.first, kv.second);
  for (const auto& kv : posed) push_posed(kv.second);   // S110 BARRICADE: the posed slots with no cut
  Report(ctx, Family::Arena, static_meshes, drawn, skipped, reason);
  // S103 NR (3): the hole rules below run on a WARM build too (they are pure record tests, no upload): a frame READY in Warm must not then be refused by DrawLive for the same rule.
  {  // S103 L (review 7): an entrance whose rig values are missing for a set the game remaps is a MISSING INPUT (a hole: the live caller keeps the game's frame), counted
    const uint32_t missing = stage_look::MissingFor(ctx.t, fm.sets);
    if (missing) ctx.reports.back().holes += missing, ctx.reports.back().why += (ctx.reports.back().why.empty() ? "" : "; ") + std::string("entrance: ") + std::to_string(missing) + " remapped light set(s) without the rig's values (no stage look)";
  }
  if (stage_look::PageEntrance(ctx.t)) {  // S103 L2: the entrance page's inputs (S111 PAGEPOOLS: the page's own latch). A spot row that wants the entrant's root with no entrant in the record is a MISSING input (a hole). An entrance with NO pool rows at all is
    // not a render-side hole: "no pool rows on an entrance" is the DECISION's missing input (scene_inputs::kInStagePools has no stamp then, so the frame walks); here it is only said.
    if (pools_incomplete) ctx.reports.back().holes += 1, ctx.reports.back().why += (ctx.reports.back().why.empty() ? "" : "; ") + std::string("entrance page: the follow spot wants the entrant's root and the record has no actor:slot:0");
    else if (pools.empty()) {
      ctx.reports.back().why += (ctx.reports.back().why.empty() ? "" : "; ") + std::string("entrance page: no pool / spot rows in the record (the floor is the base page)");
    }
    if (!pools.empty() && as->pcache.ok && as->pcache.sw <= 0)  // B1: counted, not skipped silently: no lightmap00 sprite = the pools are not drawn (the spot disk needs none)
      ctx.reports.back().why += (ctx.reports.back().why.empty() ? "" : "; ") + std::string("entrance page: this arena has no lightmap00 pool sprite (pools not drawn)");
  }
  return true;
}

// S103 FOB: the GAME's order (docs\research\s103\XFO.md G1-G5): the canvas and skirts, the arena's slots, the screens, the ring's posts / turnbuckles / covers / ropes, the props (the wrestlers follow in Classes()), the flashes.
const ClassEntry kCRows[6] = {
    {"ring_early", Family::Ring, &ApplyRingEarly},
    {"arena", Family::Arena, &ApplyArenaLive},
    {"screens", Family::Screens, &ApplyScreens},
    {"ring_late", Family::Ring, &ApplyRingLate},
    {"props", Family::Props, &ApplyProps},
    {"flashes", Family::Flashes, &ApplyFlashes},
};

}  // namespace rdr_scene::rf::c
