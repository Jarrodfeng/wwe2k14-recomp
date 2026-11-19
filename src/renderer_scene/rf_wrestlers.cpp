// Wrestlers from the live record (Session 103, lane B). See rf_wrestlers.h.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "rf_wrestlers.h"
#include "../renderer/problem.h"   // rdr::Log: the game run log (stderr never reaches it)
#include "body_scale.h"   // S103 W2
#include "crowd_fast.h"   // S103 CPU1: fast::ParseUInt
#include "shadow_fit_record.h"   // S103 E8: the shared key test and the frame's stamp
#include "guest_model_draw.h"   // S103 CW-2: the GuestModel rows -> a model (an actor the record cannot name)
#include "guest_piece_fade.h"
#include "piece_alpha.h"   // S103 EYF: the piece's [model+252] row
#include "detached_piece.h"   // S103 DTP: the owner-mesh row of a piece the game detached

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>   // S103 L7
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace rdr_scene::rf {

namespace {

WrestlerStats g_stats;
WrestlerPlants g_plants;

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

// S103 HGF (HGP.md section 5): the line that settles the root-gap cause. A hair root whose drawn row sits > kSpringGapBig file units from its body parent. Rate limited: the first 8 of a run plus the first per object id.
// body_age = records since this skeleton first carried a hair row (0-1 = the spawn). row_j_identity / row_j_zero describe the row as the latch read it (before corr^-1).
void LogHairGapBig(const SceneTables& t, uint32_t obj_index, const std::string& obj_id, uint32_t piece, uint32_t node, int32_t parent, float gap, const float* wj, const float* wp, const float* raw_row, bool has_corr) {
  static std::mutex mu;
  static std::map<std::pair<uint32_t, std::string>, std::pair<uint64_t, uint64_t>> age;   // (skeleton, object id) -> (last record frame counted, records); review 4: a body absent for > 8 records starts again at 0
  static std::set<std::string> logged_ids;
  static int logged = 0;
  const uint32_t skel = obj_index < t.engine_objects.size() ? t.engine_objects[obj_index] : 0u;
  std::lock_guard<std::mutex> lock(mu);
  auto& a = age[{skel, obj_id}];
  if (a.second == 0 || t.header.guest_frame > a.first + 8) a.second = 1, a.first = t.header.guest_frame;
  else if (a.first != t.header.guest_frame) ++a.second, a.first = t.header.guest_frame;
  const uint64_t body_age = a.second - 1;
  if (!(gap > kSpringGapBig)) return;
  if (!(logged < 8 || logged_ids.insert(obj_id).second)) return;
  ++logged;
  float dev = 0.f;   // review 5: the largest distance of the latched row from identity (the guard's tolerance is 1e-5)
  bool zero = true;
  for (int k = 0; k < 16; ++k) dev = std::max(dev, std::fabs(raw_row[k] - ((k % 5) == 0 ? 1.f : 0.f))), zero = zero && raw_row[k] == 0.f;
  rdr::Log("[scene][wrestlers] hair gap BIG frame %llu obj %s skel %08X piece %u node %u parent %d gap %.3f root_world (%.2f %.2f %.2f) parent_world (%.2f %.2f %.2f) row_j_max_dev_from_identity %.6g row_j_zero %d has_corr %d body_age %llu",
         (unsigned long long)t.header.guest_frame, obj_id.c_str(), unsigned(skel), unsigned(piece), unsigned(node), int(parent), double(gap), double(wj[0]), double(wj[1]), double(wj[2]), double(wp[0]), double(wp[1]), double(wp[2]),
         double(dev), int(zero), int(has_corr), (unsigned long long)body_age);
}

// S X S for a row-vector matrix (S = diag(1,-1,1)): the signs of the elements that mix Y with X / Z flip (assets::MirrorY, kept local so this file
// stays free of the loaders). The same conversion tools\scene\rsc_draw.cpp ConvertLiveToViewer applies to a Live record's worlds and palettes.
Mat4 MirrorY(const Mat4& in) {
  Mat4 m = in;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      if ((i == 1) != (j == 1)) m.m[i * 4 + j] = -m.m[i * 4 + j];
  return m;
}

// The record's evaluated light sets by game set id (the same decode as src\scene_live\arena_apply.h ReadArenaRecord: slots 0-3 directional, 4 the highlight
// light, 5 / 6 sky / ground; SCENE_RECORD.md "Light set ids").
std::map<uint32_t, assets::LightSet> LightSetsById(const SceneTables& t) {
  std::map<uint32_t, assets::LightSet> sets;
  for (const Light& l : t.lights) {
    const bool fresh = sets.find(l.set) == sets.end();
    assets::LightSet& ls = sets[l.set];
    if (fresh)
      for (float& f : ls.fall) f = -1.f;  // unknown until a directional light of the set says otherwise
    ls.id = int(l.set);
    if (l.kind == uint8_t(LightKind::Directional) && l.slot < 4) {
      std::memcpy(ls.dir[l.slot], l.dir_or_pos, sizeof ls.dir[0]);
      std::memcpy(ls.col[l.slot], l.color, sizeof ls.col[0]);
      ls.fall[l.slot] = l.range;
    } else if (l.kind == uint8_t(LightKind::Directional) && l.slot == 4) {
      std::memcpy(ls.hl_dir, l.dir_or_pos, sizeof ls.hl_dir);
      std::memcpy(ls.hl_col, l.color, sizeof ls.hl_col);
      ls.hl_view = true;   // S103 L7: the record's key light is authored in view space (char_effects.cpp KeyLightWorld)
      ++g_stats.key_sets, g_stats.key_sets_x += std::fabs(ls.hl_dir[0]) > 1e-3f;
    } else if (l.kind == uint8_t(LightKind::Ambient) && l.slot == 5) {
      std::memcpy(ls.sky, l.color, sizeof ls.sky);
    } else if (l.kind == uint8_t(LightKind::Ambient) && l.slot == 6) {
      std::memcpy(ls.ground, l.color, sizeof ls.ground);
    }
  }
  for (auto& kv : sets)
    for (float& f : kv.second.fall)
      if (f < 0.f) f = 0.f;  // the character path does not read the falloff; 0 = none
  return sets;
}

// S103 E8: the light state the game's colour draws of ONE wrestler read. The record's kScLightRefs name, per model call (engine object), the kScLightStates row R it read; the game's draws take DifLghVec / DifLghCol [0..4],
// KeyLightVec / KeyLightColor, SkyCol (R+352) and GroundCol (R+368) from that row verbatim [C: run 28 f500, tools\scene\e8_game_key.py: the stream's constants equal state 17's dir / col / block A / block B, the lobe weights
// are the colours' w (1.0 on the draws), the key (-0.615, 0.6721, 0.4124) is WORLD space, no camera rotation]. Same stamp rule as shadow_fit_record.h: header - 1 when the object has it, else the newest within the window.
// Null = no state for this object (an OURS frame: the model-entry hook is skipped; the record's light sets stay).
enum class LightStateUse { None, Missing, Rejected, Ok };
// `have_stamp` / `stamp`: the frame's ONE stamp (shadow_fit_record.h WrestlerLightStamp, the same the key is read at). None = the old feed on purpose (no states in the record and none expected; the dev twin).
const LightState* WrestlerLightState(const SceneTables& t, uint32_t engine_object, bool have_stamp, uint32_t stamp, LightStateUse& use) {
  static const bool off = std::getenv("WWE_RDR_NO_WRESTLER_LIGHT_STATE") != nullptr;   // dev twin: the old feed (the record's arena light set)
  static const bool plant_missing = std::getenv("WWE_RDR_E8_PLANT_NO_STATE") != nullptr;   // dev twin: a wrestler whose state is missing while the record has states
  static const bool plant_zero = std::getenv("WWE_RDR_E8_PLANT_ZERO_STATE") != nullptr;   // dev twin: the state is all zero
  use = LightStateUse::None;
  if (off) return nullptr;
  if (t.light_states.empty() && !Plants().light_states_expected) return nullptr;
  const LightState* s = nullptr;
  if (!plant_missing && engine_object && have_stamp)
    for (const LightRef& r : t.light_refs)   // synthetic ok: a wrestler's light state may come from the latch (E8); this is not a hole exemption
      if (r.object == engine_object && r.frame == stamp && r.state < t.light_states.size()) { s = &t.light_states[r.state]; break; }
  if (!s) {
    use = LightStateUse::Missing;
    return nullptr;
  }
  static const LightState zero = {};
  if (plant_zero) s = &zero;
  if (!LightStateUsable(s->light)) {   // S103 CE: the shared rule (light_install.h): a key-off state (key colour 0) is the game's own and is used as is, not replaced by the arena set
    use = LightStateUse::Rejected;
    return nullptr;
  }
  use = LightStateUse::Ok;
  return s;
}

// S103 NR (fix A) + NRBF (review F1): is an INVISIBLE skinned `skel:` object (no character slot, not linked to an owner) a hole? Every one used to be (run 35: 11 holes on every match frame, READY never true).
// It is a hole unless the record PROVES the game did not draw it:
//  (b) a ring row with that object address: the ring family draws it (the apron skirts: RingRows klass kRcSkirt, the same address): not a hole; else
//  (a) a WALKED wrestler light ref exists at the frame's stamp (the walk saw the game's draws; WalkedStamp) and none of them names this object: the game did not draw it: not a hole.
// Fail closed: no engine address, no walked evidence (an OURS frame carries only the latch's SYNTHETIC refs, one per actor:slot, so "no ref for the piece" proves nothing), or a walked ref for it = a hole.
// The game draws 3 of the 4 skirts by camera quadrant (ring_rows_core.h kExpectedCalls): the 4th has no light ref this frame and is not a hole.
// A ref is walked when its state is not kLsSynthetic. Lint: nrbf_hole_lint.py (every t.light_refs read in rf_*.cpp filters it or says why).
bool WalkedRef(const SceneTables& t, const LightRef& r) { return r.state < t.light_states.size() && !(t.light_states[r.state].flags & kLsSynthetic); }

// Is the frame's stamp backed by at least one WALKED wrestler ref (the game's own draws)? false on an OURS frame (synthetic refs only) and on a record with no wrestler ref.
bool WalkedStamp(const SceneTables& t, bool have_stamp, uint32_t stamp) {
  if (!have_stamp) return false;
  for (const LightRef& r : t.light_refs)
    if (r.frame == stamp && WalkedRef(t, r) && WrestlerRef(t, r)) return true;
  return false;
}

bool SkelPieceIsHole(const SceneTables& t, size_t obj_index, bool walked, uint32_t stamp) {
  const uint32_t addr = obj_index < t.engine_objects.size() ? t.engine_objects[obj_index] : 0u;
  if (Plants().old_hole_rule || !addr) return true;   // old_hole_rule: dev twin, every such piece is a hole (the rule before NR)
  for (const RingRows& e : t.ring_rows)
    if (e.object == addr) return false;
  if (!walked) return true;
  for (const LightRef& r : t.light_refs)
    if (r.object == addr && r.frame == stamp && WalkedRef(t, r)) return true;
  return false;
}

// S103 NRBF (review F5): did the game draw this LINKED piece this frame? Drawn = a walked light ref for its engine address at the stamp. Hidden = the walk has evidence for the frame (a walked wrestler
// ref at the stamp) and none names the piece: the game hid it while its owner shows, and the owner's body would draw it: a HOLE. Unobserved = no walked evidence at all (an OURS frame): hiding cannot be
// seen; the contributor's proof (map, owner identity, fresh palette) is all there is: not a hole, counted apart so the landing check can read it.
enum class PieceSeen { Drawn, Hidden, Unobserved };
PieceSeen PieceSeenThisFrame(const SceneTables& t, size_t obj_index, bool walked, uint32_t stamp) {
  const uint32_t addr = obj_index < t.engine_objects.size() ? t.engine_objects[obj_index] : 0u;
  if (!walked) return PieceSeen::Unobserved;
  if (addr)
    for (const LightRef& r : t.light_refs)
      if (r.object == addr && r.frame == stamp && WalkedRef(t, r)) return PieceSeen::Drawn;
  return PieceSeen::Hidden;
}

}  // namespace

bool IsWrestlerObject(const SceneTables& t, const Object& o) {
  const std::string_view id = t.Str(o.id);
  return StartsWith(id, "actor:") || StartsWith(id, "skel:");
}

const WrestlerStats& LastWrestlerStats() { return g_stats; }
WrestlerPlants& Plants() { return g_plants; }

std::string WrestlerStatsText() {
  const WrestlerStats& s = g_stats;
  char b[1024];
  std::snprintf(b, sizeof b,
                "[rf][wrestlers] objects %u, drawn %u, skipped %u (invisible: no character slot %u, model not named in the record %u, not resident %u), palette size != model nodes %u (body scale unknown, a hole: %u), own-skeleton pieces not drawn %u, bind pose (no palette) %u, light set by id %u / default %u (set %d), mesh mask hides pieces on %u, body scale applied %u / unknown %u%s%s",
                s.objects, s.drawn, s.skipped_invisible + s.skipped_unnamed + s.skipped_not_resident + s.skipped_palette_mismatch + s.skipped_body_scale, s.skipped_invisible, s.skipped_unnamed, s.skipped_not_resident, s.skipped_palette_mismatch, s.body_scale_unknown, s.own_skeleton_pieces, s.no_palette, s.light_by_id,
                s.light_default, s.default_set, s.mesh_masked, s.body_scaled, s.body_scale_unknown, s.first_skip_why.empty() ? "" : "; first skip: ", s.first_skip_why.c_str());
  std::string out = b;
  out += "; own-skeleton pieces that are not holes " + std::to_string(s.skel_not_hole);   // S103 NR
  out += "; piece alphas ([model+252]) carried to the owner " + std::to_string(s.pieces_alpha_carried) + ", missing (carried as 0) " + std::to_string(s.pieces_alpha_missing);   // S103 EYF
  out += "; pieces drawn by their owner's body " + std::to_string(s.pieces_by_owner) + " (of which on a frame with no walked evidence " + std::to_string(s.pieces_unobserved) + "; linked but not drawn by the game this frame = hole " + std::to_string(s.pieces_hidden_owner_drawn) + ")";   // S103 NRBF
  out += "; spring-hair pieces absent (rows not supplied: own meshes skipped, body drawn) " + std::to_string(s.hair_pieces_absent);   // S103 W2B (review I)
  out += "; pieces detached by the game (copy flag 0: owner mesh skipped, drawn from own rows) " + std::to_string(s.pieces_detached);   // S103 DTP
  out += "; own-skeleton pieces drawn from their own guest rows " + std::to_string(s.pieces_guest_drawn);   // S103 DLF
  out += "; linked pieces refused at draw (the owner's model has no mesh of the piece = hole) " + std::to_string(s.pieces_no_owner_mesh);   // S103 PZF2
  out += "; key light sets " + std::to_string(s.key_sets) + " (|hl_dir.x| > 1e-3: " + std::to_string(s.key_sets_x) + ")";   // S103 L7
  out += "; lit by the wrestler's own light state " + std::to_string(s.light_by_state) + " (state missing = hole " + std::to_string(s.light_state_missing) + ", rejected " + std::to_string(s.light_state_rejected) + ")";   // S103 E8
  if (!s.at_file_value.empty()) out += "; at the file's value (not in the record yet): " + s.at_file_value;
  return out;
}

// S103 CW-2: the front-end model of the guest model whose model object is the actor's engine object (the pairing key kScEngineObjects holds), or -1 (CacheState::why says why). Bytes come from the cache's own
// Store (by content hash, absorbed at the top of every BuildFrame: record_frame.cpp; CW-2b: per ModelCache, no function-local statics); the model is built once per content hash and kept under "guest:<hash>"
// (a changed body = a new hash = a new model; the old ones are retired, at most 16 kept).
static int GuestModelFor(BuildCtx& ctx, size_t obj_index, std::string_view id) {
  guestmodel::CacheState& cs = guestmodel::StateIn(ctx.models.GuestStateSlot());
  guestmodel::Store& store = cs.store;
  std::unordered_map<std::string, uint64_t>& hashes = cs.hashes;
  std::string& why_text = cs.why;
  const SceneTables& t = ctx.t;
  why_text.clear();
  const uint32_t addr = obj_index < t.engine_objects.size() ? t.engine_objects[obj_index] : 0u;
  if (t.guest_models.Empty() && store.bytes() == 0) return why_text = "the record carries no guest-model rows", -1;
  if (!addr) return why_text = "the object has no engine address (" + std::string(id) + ")", -1;
  const bool flat = Plants().guest_flat;   // CAWD debug plant (WrestlerPlants::guest_flat): a part with no diffuse is drawn flat magenta (drawn-black vs not-drawn in one shot); "0" = off
  uint64_t mh = 0;
  uint32_t nearest = 0, distinct = 0, last_seen = 0;
  for (const GuestModelPart& r : t.guest_models.parts) {
    if (r.model == addr) { mh = r.model_hash; break; }
    if (r.model != last_seen) ++distinct, last_seen = r.model;   // (rows of one model are adjacent)
    const uint32_t dn = nearest > addr ? nearest - addr : addr - nearest, dr = r.model > addr ? r.model - addr : addr - r.model;
    if (!nearest || dr < dn) nearest = r.model;
  }
  if (!mh) {
    // CAWD Q1 site 3: the engine address against the nearest row model address, once per distinct pair (the 74 hole frames of run 012: the join fails, nobody knows by how much)
    char jk[64], jl[200];
    std::snprintf(jk, sizeof jk, "guest-join:%08X:%08X", addr, nearest);
    std::snprintf(jl, sizeof jl, "[rdr][caw] guest join miss: engine address %08X, nearest guest-model row model %08X (%d bytes apart; %u row models in the record)", addr, nearest, int(int64_t(nearest) - int64_t(addr)), distinct);
    if (cs.join_seen.size() < guestmodel::CacheState::kMaxJoinLines && cs.join_seen.insert(jk).second) ctx.models.LogOnce(jk, jl);   // CTX2: at most 16 distinct pairs a run
    return why_text = "no guest-model rows for the object's engine address", -1;
  }
  char key[40];
  std::snprintf(key, sizeof key, "guest:%016llx", (unsigned long long)mh);
  const int have = ctx.models.Find(key);
  if (have >= 0) return have;
  assets::Model m;
  guestmodel::BuildStats bs;
  guestmodel::DrawCounters& cnt = guestmodel::Counters();
  if (!guestmodel::BuildModel(t, store, addr, m, bs, flat)) {
    ++cnt.models_holes;
    ctx.models.LogOnce("guest-hole:" + bs.why, "[rdr][caw] guest model hole: " + bs.why + " (the editor frame stays the game's while it lasts)");
    return why_text = bs.why, -1;
  }
  cnt.models_built++, cnt.tex_bound += bs.tex_bound, cnt.tex_override += bs.tex_override, cnt.tex_missing += bs.tex_missing, cnt.no_diffuse += bs.no_diffuse, cnt.extra_streams += bs.extra_streams,
      cnt.bad_nodes += bs.bad_nodes, cnt.effect_fallback += bs.effect_fallback, cnt.engine_default_cube += bs.tex_default_cube, cnt.bad_slots += bs.bad_slots, cnt.unskinned += bs.unskinned;
  ctx.models.LogOnce("guest-palettes:" + bs.part_palettes, "[rdr][caw] part palettes (palette slot -> skeleton node, CAWG row 6): " + bs.part_palettes + "; model max node + 1 = " + std::to_string(bs.max_node));   // S103 CAWS: per distinct content
  ctx.models.LogOnce("guest-census", guestmodel::CensusLine());   // ONE fixed key (a slider drag rebuilds the model many times: the line prints once per cache); the writer's `[rdr][caw] textures:` line is the per-content one
  const bool retired = hashes.size() >= 16;
  if (retired) {
    ctx.models.RetireOther("guest:", key);
    hashes.clear();
  }
  hashes[key] = mh;
  // CTX2: pictures are shared by content across builds (a slider drag rebuilds the model with the same 6.5 MB of pictures: one upload, not one a build); the ones no model still uses are released when the old models retire
  const int model_id = ctx.models.PutGuest(key, m, rdr::kGrpWrestler);   // S103 GSTP group + CTX2 shared pictures
  if (model_id >= 0) cs.max_node_of[model_id] = bs.max_node;   // S103 CAWS: the model's highest node + 1, read per actor in ApplyWrestlers
  if (retired) ctx.models.TrimGuestImages(m);
  return model_id;
}

bool ApplyWrestlers(BuildCtx& ctx) {
  const SceneTables& t = ctx.t;
  SceneFrame& f = ctx.frame;
  g_stats = WrestlerStats{};
  WrestlerStats& st = g_stats;
  // Runtime values (sweat, damage, fades of the shader, the rim map, hair highlights, team colours) are not in the record until lane A's sections land: they stay at the
  // file's value. Named here so the rig's report lists them (never silent). The object's own tint IS in the record and is applied.
  st.at_file_value = "sweat, blood, damage, rim map, hair highlights, reflection cube (front-end rules: SCENE_FRONTEND.md Wrestlers)";

  ClassReport rep;
  rep.family = Family::Wrestlers;
  const Object* cur_obj = nullptr;   // S103 AUSF: the object being decided (the skip lines name it)
  auto log_skip = [&](const std::string& why, bool is_hole) {   // S103 AUSF (AUS2 log 4): one line per distinct (object, reason), at most 64 a run: a skipped body is named with its frame
    static std::set<std::string> seen;
    if (!is_hole || !cur_obj || seen.size() >= 64) return;   // review 5: holes only (the cap is checked before any string is built)
    const std::string obj(t.Str(cur_obj->id));
    const std::string key = "wskip:" + obj + ":" + why.substr(0, 48);
    if (seen.insert(key).second) ctx.models.LogOnce(key, "[scene][wrestlers] skip frame " + std::to_string((unsigned long long)t.header.guest_frame) + " obj " + obj + " reason " + why + " hole " + (is_hole ? "yes" : "no"));
  };
  auto skip = [&](uint32_t& counter, const std::string& why, bool is_hole = false) {   // is_hole: this skip also counts a hole (the lint nrbf_hole_lint.py reads the ++rep.holes beside it); the first one names hole_why (review F5)
    ++counter;
    ++rep.skipped;
    log_skip(why, is_hole);
    if (st.first_skip_why.empty()) st.first_skip_why = why;
    if (rep.why.empty()) rep.why = why;
    if (is_hole && rep.hole_why.empty()) rep.hole_why = why;
  };

  // A Live record is the game's space; the loaders build viewer space (Y mirrored), so the objects' worlds and palettes take S X S, as the camera takes S on its world
  // side (SCENE_RECORD.md "One space rule"; tools\scene\rsc_draw.cpp ConvertLiveToViewer). Only when the caller drew in viewer space and the record is still the game's
  // (a caller that converted the whole record already marked it a Viewer record).
  const bool mirror = t.header.source == uint8_t(Source::Live) && f.viewer_space;

  // The record's light sets by id -> this frame's dense table, appended on first use (other classes keep their own entries).
  const std::map<uint32_t, assets::LightSet> sets = LightSetsById(t);
  std::map<uint32_t, uint32_t> dense;  // game set id -> index in f.light_sets
  uint32_t stamp = 0;
  const bool have_stamp = WrestlerLightStamp(t, stamp);   // S103 E8: one stamp per frame, the key light's
  const bool walked = WalkedStamp(t, have_stamp, stamp);   // S103 NRBF (review F1): the stamp is backed by the game's own (walked) draws, not only the latch's synthetic refs
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> state_sets;   // S103 E8: (set index, light state) -> index in f.light_sets
  auto dense_of = [&](uint32_t id) -> uint32_t {
    auto it = dense.find(id);
    if (it != dense.end()) return it->second;
    const uint32_t idx = uint32_t(f.light_sets.size());
    f.light_sets.push_back(sets.at(id));
    dense.emplace(id, idx);
    return idx;
  };
  // The default for an object whose set is unknown: the match / entrance set (id 10, "run 6816's entrant takes set 10", LightSet comments), else the lowest id the record
  // holds that has a light. LISTED in the stats.
  int default_id = -1;
  if (sets.count(10)) default_id = 10;
  else if (!sets.empty()) default_id = int(sets.begin()->first);
  st.default_set = default_id;

  if (!(ctx.families & (1u << uint32_t(Family::Wrestlers)))) return true;  // per-class pictures: this family is left out
  // S103 PZF2 (EB): the visible owner body of each character slot ("actor:slot:N"), for the draw-time check that its resident model really holds the meshes of a piece it is said to draw.
  std::map<uint32_t, size_t> owner_obj;
  for (size_t k = 0; k < t.objects.size(); ++k) {
    const std::string_view sid = t.Str(t.objects[k].id);
    unsigned n = 0;
    if (StartsWith(sid, "actor:slot:") && (t.objects[k].flags & kObjVisible) && fast::ScanUInt(sid, 11, n)) owner_obj.emplace(n, k);   // S103 CPU1: ScanUInt = what sscanf("%u") read, no copy (RHP.md)
  }
  struct PendingPieceAlpha { unsigned slot, piece; float alpha; };   // S103 EYF: a piece's [model+252] row, waiting for its owner's Object (the piece may come before or after it in the record)
  std::vector<PendingPieceAlpha> pending_piece_alpha;
  for (const Object& o : t.objects) {   // (a pre-pass: the owner's Object is built in the main loop, before or after its pieces)
    if ((o.flags & kObjVisible) || !(o.flags & (kObjDrawnByOwner | kObjDetachedPiece)) || !IsWrestlerObject(t, o) || o.pad == 0 || o.layer == 0) continue;
    if (DetachedOwnerRowOut dr; DetachedOwnerRow(o.flags, o.pad, o.layer, !g_plants.ignore_detached_flag, dr)) {   // S103 DTP (PEP.md section 3): the game stopped copying the owner's rows into this piece and poses it itself: the owner's merged mesh must not draw it (alpha 0 row: front_end skips it, counted as skipped_alpha0). Counted apart from an eyelid fade.
      pending_piece_alpha.push_back({dr.slot, dr.piece, dr.alpha});
      ++st.pieces_detached;
      continue;
    }
    if (!(o.flags & kObjSkinned) || !o.bones_count) continue;   // S103 DTP fix: a detached piece needs no palette for its owner row (above); a drawn-by-owner piece still does
    const PieceAlphaValue pv = PieceModelAlpha(t, size_t(&o - t.objects.data()), g_plants.piece_alpha_one);   // FAIL CLOSED: no readable row = alpha 0
    pending_piece_alpha.push_back({unsigned(o.layer) - 1u, unsigned(o.pad), pv.alpha});
    ++(pv.found ? st.pieces_alpha_carried : st.pieces_alpha_missing);
  }
  for (const Object& o : t.objects) {
    const size_t obj_index = size_t(&o - t.objects.data());   // S103 W2
    if (!IsWrestlerObject(t, o)) continue;
    cur_obj = &o;
    ++st.objects;
    ++rep.objects;
    bool piece_from_guest = false;
    float guest_alpha = 1.f;   // S103 SMF: the guest-routed piece's own [model+252] fade (EYF's rule on the guest route: the eyelid / lash layer is alpha 0 in entrances)
    // S103 DLW: an own-skeleton piece drawn from its own guest rows (it then takes the model path below, unnamed)
    if (!(o.flags & kObjVisible)) {  // a skeleton that matches no character slot (managers, waiting entrance models, own-skeleton pieces): the game draws nothing for it
      if ((o.flags & kObjDetachedPiece) && !((o.flags & kObjSkinned) && o.bones_count)) {   // S103 DTP fix: a detached piece with no palette of its own (link-only: the piece object is gone, or stale). The owner row was pushed in the pre-pass; it is never guest-routed
        if (PieceSeenThisFrame(t, obj_index, walked, stamp) == PieceSeen::Drawn) {   // the game still draws it and we have no fresh palette: a HOLE (the frame falls back), never a missing pad
          ++guestmodel::Counters().skel_holes;
          ++rep.holes;
          skip(st.skipped_invisible, "detached piece " + std::string(t.Str(o.id)) + ": the game draws it but the record has no fresh palette for it", true);
        } else {
          log_skip("detached piece not drawn by the game this frame (no palette)", false);
          ++st.skipped_invisible, ++rep.skipped;   // not drawn by the game this frame (the ELBW is over: it sets only +20 = 0): nothing to draw, not a hole
        }
        continue;
      }
      if ((o.flags & kObjSkinned) && o.bones_count) {
        ++st.own_skeleton_pieces;
        bool hole = false;
        bool guest_route = false;
        // S103 SMF: the guest route is for pieces of an owner that is itself drawn from guest rows ({visible, unnamed}); a resident named owner's merged mesh already draws its pieces (no double draw).
        auto owner_is_guest_body = [&](size_t k) { const Object& ob = t.objects[k]; return ob.asset >= t.assets.size() || t.assets[ob.asset].pac == 0; };
        bool owner_guest = false;
        // S103 DLF: only a hole the owner does NOT draw may take the guest route (no double draw, no resurrecting a hidden piece)
        std::string hole_text = "own-skeleton piece " + std::string(t.Str(o.id)) + " is a hole (the game draws it and no model of ours does)";
        if (o.flags & kObjDetachedPiece) {   // S103 DTP: the game poses this piece from its OWN palette now (the owner's copy is off), and the owner's merged mesh was told to skip it (pre-pass): it is drawn from its own guest rows, whatever the owner kind
          if (PieceSeenThisFrame(t, obj_index, walked, stamp) == PieceSeen::Hidden) ++st.skel_not_hole;   // the game did not draw it this frame: a skip, not a hole
          else hole = true, guest_route = o.pad != 0 && o.layer != 0;   // S103 DTP fix: a flag with no piece id / owner slot has no owner row (the owner would draw it too): a hole, never a guest draw
          hole_text = "detached piece " + std::string(t.Str(o.id)) + " (the game poses it from its own palette): no guest model of ours draws it";
        } else if (o.flags & kObjDrawnByOwner) {   // S103 NRBF: a linked SUBM piece the contributor proved (map complete, owner identity, fresh palette): the owner's body model draws it
          // S103 PZF2 (EB, kind-independent): "the owner's body draws it" is true only when the owner's RESIDENT model holds a mesh of this piece id (src_kind 2, src_key == id). The record carries the piece id in
          // Object::pad and the owner's slot + 1 in Object::layer; 0 in either (an older record) = unknown = a hole. This also closes ids the game builds but our loader never merges (28 / 29).
          const unsigned pid = o.pad, oslot = o.layer;
          bool mesh_ok = false;
          if (oslot != 0) {
            auto ow = owner_obj.find(oslot - 1);
            if (ow != owner_obj.end()) owner_guest = owner_is_guest_body(ow->second);   // S103 SMF: the owner is visible (owner_obj holds only visible actors) and unnamed
          }
          if (pid != 0 && oslot != 0) {
            auto ow = owner_obj.find(oslot - 1);
            std::string why_m;
            const int om = ow == owner_obj.end() ? -1 : ctx.models.Get(t, t.objects[ow->second].asset, &why_m);
            mesh_ok = om >= 0 && ctx.models.front_end().ModelHasPieceMesh(om, pid);
          }
          if (!mesh_ok) {
            ++st.pieces_no_owner_mesh;   // the owner's model has no mesh of it: nobody draws it
            if (PieceSeenThisFrame(t, obj_index, walked, stamp) == PieceSeen::Hidden) ++st.skel_not_hole;   // S103 SMF (review A3): the game hid it this frame: a skip (not a hole, never resurrected), counted like skel_not_hole
            else {
              hole = true;
              guest_route = owner_guest;   // S103 SMF: only an unnamed visible owner
            }
            hole_text = "own-skeleton piece " + std::string(t.Str(o.id)) + (pid ? ": the owner model has no mesh of piece " + std::to_string(pid) : ": the record does not name its piece id / owner slot");
          } else {
            switch (PieceSeenThisFrame(t, obj_index, walked, stamp)) {
              case PieceSeen::Drawn: ++st.pieces_by_owner; if (pid == 28 || pid == 29) rdr_scene::g_spring_census.ent_drawn.fetch_add(1, std::memory_order_relaxed); break;   // the game drew it too: not a hole   // S103 ENT R2 census: entries 28 / 29 drawn by the owner
              case PieceSeen::Unobserved: ++st.pieces_by_owner, ++st.pieces_unobserved; break;   // no walked evidence (OURS frame): the contributor's proof stands, counted apart
              case PieceSeen::Hidden: ++st.pieces_hidden_owner_drawn, hole = true; if (pid == 28 || pid == 29) rdr_scene::g_spring_census.ent_hidden.fetch_add(1, std::memory_order_relaxed); break;   // (S103 ENT R2: the game hid a 28 / 29 piece while its owner is drawn)   // F5: the game hid it while the owner body shows it: a hole, fail closed
            }
            if (!hole && !PieceModelAlpha(t, obj_index, g_plants.piece_alpha_one).found) {   // S103 EYF (review 2): no readable [model+252] row = the piece's fade is unknown: a HOLE (named, counted as pieces_alpha_missing), not a silent hide
              hole = true;
              hole_text = "own-skeleton piece " + std::string(t.Str(o.id)) + ": the record has no readable [model+252] (MdlAlpha) row for it";
            }
          }
        } else if (!SkelPieceIsHole(t, obj_index, walked, stamp)) ++st.skel_not_hole;   // S103 NR (A): not drawn by the game this frame, or drawn by the ring family: not a hole
        else {
          hole = true;   // B6: a piece with its own skeleton (hair 48 nodes, caps, glasses): the game draws it, we do not yet: a HOLE (S103 S: never ours while it is missing)
          // S103 SMF (review A4): the record names no owner for an unlinked piece (pad / layer = 0), so the guest route is taken only when no visible body could own it from its resident model: every body is a guest-row
          // body or a named body whose model holds no piece mesh (the referee, a manager); guest_piece_fade.h UnlinkedPieceMayGuestRoute
          std::vector<BodyKind> kinds;
          for (const auto& kv : owner_obj) {
            if (owner_is_guest_body(kv.second)) { kinds.push_back(BodyKind::Guest); continue; }
            std::string why_b;
            const int bm = ctx.models.Get(t, t.objects[kv.second].asset, &why_b);
            kinds.push_back(bm < 0 ? BodyKind::NamedUnknown : ctx.models.front_end().ModelHasAnyPieceMesh(bm) ? BodyKind::NamedWithPieces : BodyKind::NamedNoPieces);
          }
          guest_route = UnlinkedPieceMayGuestRoute(kinds.data(), kinds.size()) && PieceSeenThisFrame(t, obj_index, walked, stamp) != PieceSeen::Hidden;
        }
        // S103 DLW (COV2 GAP 2, review item 2): a piece the owner's model cannot draw (the owner is a guest-row body, or its mesh lacks the piece) is drawn from its OWN guest rows when the reader has them
        // (the piece skeleton's model entry is unnamed, so the GuestModel reader reads it like a CAW's hair): not a hole. Only where the hole would have been; the model path below draws it.
        if (hole && guest_route) {   // S103 SMF: the piece's own fade first (EYF's rule): no readable row = a hole, alpha 0 = not drawn (not a hole); guest_piece_fade.h
          const GuestPieceFade gf = DecideGuestPieceFade(true, PieceModelAlpha(t, obj_index, g_plants.piece_alpha_one), g_plants.guest_piece_no_alpha);
          guest_route = gf.route, hole = gf.hole, guest_alpha = gf.alpha;
        }
        if (hole && guest_route && GuestModelFor(ctx, obj_index, t.Str(o.id)) >= 0) {
          ++st.pieces_guest_drawn;
          piece_from_guest = true;
        }
        if (!piece_from_guest) {
          if (hole) {
            ++guestmodel::Counters().skel_holes;   // CAWD Q1 site 2: the skel hole has a number (AAB142F0: a model with no character slot)
            ++rep.holes;
            skip(st.skipped_invisible, hole_text, true);   // the hole is NAMED (the rig's check reads it)
          } else {
            log_skip("no character slot, not a hole", false);
            ++st.skipped_invisible, ++rep.skipped;   // not a hole: counted, never the "first skip" reason
          }
          continue;
        }
      }
      if (!piece_from_guest) {
        skip(st.skipped_invisible, "object without a character slot is not shown (" + std::string(t.Str(o.id)) + ")");
        continue;
      }
    }
    const bool unnamed = piece_from_guest || o.asset >= t.assets.size() || t.assets[o.asset].pac == 0;
    // S103 CW-2: a visible actor whose model no asset name reaches (the CAW editor's body) is drawn from the game's own finished geometry the record carries (GuestModel rows, joined by the engine object address).
    const int guest = unnamed ? GuestModelFor(ctx, obj_index, t.Str(o.id)) : -1;
    if (unnamed && guest < 0) {
      const std::string& guest_why = guestmodel::StateIn(ctx.models.GuestStateSlot()).why;
      skip(st.skipped_unnamed, "the record does not name the model of " + std::string(t.Str(o.id)) + " (asset pac 0: the declared link is missing for this skeleton)" + (guest_why.empty() ? std::string() : "; guest model: " + guest_why), true);
      ++rep.holes;   // S103 S (R2 review): a visible wrestler we cannot draw is a hole, never an ours frame without him
      continue;
    }
    std::string why;
    const int model = unnamed ? guest : ctx.models.Get(t, o.asset, &why);
    if (model < 0) {
      skip(st.skipped_not_resident, "model of " + std::string(t.Str(o.id)) + ": " + why, why.find("not resident") == std::string::npos);
      if (why.find("not resident") == std::string::npos) ++rep.holes;   // S103 S: a failed load is a hole; still loading is a miss (the cache counted it)
      continue;
    }

    // B1: a palette of another size than the model's skeleton means the record's link names the wrong model (a slot / row mix-up): refuse, never draw the wrong pose.
    const uint32_t nodes = ctx.models.front_end().ModelNodeCount(model);
    const uint32_t body_rows = rdr_scene::BodyRowCount(o.bones_first, o.bones_count, t.bone_tags.data(), t.bone_tags.size());   // S103 W2B (review A): the skeleton's rows without the spring-hair rows appended after them
    if (unnamed && (o.flags & kObjSkinned) && o.bones_count) {   // S103 CAWS (CAWG rows 3, 4, 8): `nodes` is 0 for a guest model (it has no bones list), so the check below never fires for one; this one compares the highest node its vertices use with the palette
      const auto mn = guestmodel::StateIn(ctx.models.GuestStateSlot()).max_node_of.find(model);
      if (mn != guestmodel::StateIn(ctx.models.GuestStateSlot()).max_node_of.end()) {
        const bool bad = guestmodel::NoteActorPalette(mn->second, body_rows);
        char pk[64], pl[320];   // one line per distinct (max node + 1, palette) pair (LogOnce dedups by key; a plant or a model change makes a new pair)
        std::snprintf(pk, sizeof pk, "caw-palette:%u:%u", mn->second, body_rows);
        std::snprintf(pl, sizeof pl, "[rdr][caw] palette: %u matrices, the model's vertices use nodes 0..%d (max node + 1 = %u)%s", body_rows, int(mn->second) - 1, mn->second, bad ? " -> NODE OUTSIDE THE PALETTE (the front end draws those with an identity matrix)" : "");
        ctx.models.LogOnce(pk, pl);
      }
    }
    // S103 W2B (docs\agents\s103\W2.md, W2_ADV.md R2, bone_tag_rows.h): the palette is the body's rows, then the rows of the pieces whose spring-hair nodes the latch proved, each tagged (piece id, node). The plan
    // feeds every skeleton node of the model from a record row, or from no row when its piece is ABSENT (no rows, another node set, a piece the loader does not hold): the body is still drawn and only the meshes that skin
    // to an absent piece's nodes are skipped (the front end, counted as hair_piece_skipped). A row is never filled from the rest pose. A malformed record (fewer rows than the body, an untagged extra row) is a hole.
    rdr_scene::PaletteVerdict pal_verdict = rdr_scene::PaletteVerdict::Match;
    rdr_scene::PalettePlan plan;
    if ((o.flags & kObjSkinned) && o.bones_count && nodes) {
      const uint32_t main_nodes = ctx.models.front_end().ModelMainNodeCount(model);
      if (!rdr_scene::g_spring_hair_rows.load(std::memory_order_relaxed)) {   // switched off: the old rule (the palette is exactly the model's skeleton; an attire with piece nodes is a hole)
        pal_verdict = o.bones_count == nodes ? rdr_scene::PaletteVerdict::Match : rdr_scene::PaletteVerdict::Count;
      } else {
        pal_verdict = rdr_scene::PlanPalette(o.bones_first, o.bones_count, nodes, main_nodes, ctx.models.front_end().ModelExtraTags(model), ctx.models.front_end().ModelExtraPieceNodes(model), t.bone_tags.data(), t.bone_tags.size(), plan);
        if (pal_verdict == rdr_scene::PaletteVerdict::Match && g_plants.refuse_body_on_absent_piece && !plan.absent.empty()) pal_verdict = rdr_scene::PaletteVerdict::Tags;   // planted twin: the rule W2_ADV R2 rejected (one unproven piece costs the whole wrestler)
      }
      auto& sc = rdr_scene::g_spring_census;
      if (pal_verdict == rdr_scene::PaletteVerdict::Match) {
        if (nodes > main_nodes) sc.drawn.fetch_add(1, std::memory_order_relaxed);
        sc.piece_skipped.fetch_add(plan.absent.size(), std::memory_order_relaxed);
        sc.rows_unmodelled.fetch_add(plan.unmodelled, std::memory_order_relaxed);
      } else if (nodes > main_nodes || pal_verdict == rdr_scene::PaletteVerdict::Tags) {
        (pal_verdict == rdr_scene::PaletteVerdict::Count ? sc.refused_count : sc.refused_tags).fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (pal_verdict != rdr_scene::PaletteVerdict::Match) {
      skip(st.skipped_palette_mismatch, "palette of " + std::string(t.Str(o.id)) + " has " + std::to_string(o.bones_count) + " nodes, its model " + std::to_string(nodes) + (pal_verdict == rdr_scene::PaletteVerdict::Tags ? " (the piece-node tags of the record differ from the model's)" : ""), true);
      ++rep.holes;   // S103 NRBF (review F2): a visible wrestler skipped for a wrong link is a hole (fail open before: the frame went ours without him); lint: nr_symmetry_lint.py
      if (unnamed) ++guestmodel::Counters().actors_skipped_palette;   // CAWD Q1 site 2 (after the hole: nrbf_hole_lint's plant expects the hole right below the skip)
      continue;
    }

    Object c = o;
    c.asset = uint32_t(model);
    c.flags |= kObjVisible;
    if (piece_from_guest) c.tint[3] *= guest_alpha;   // S103 SMF

    // Palette: one matrix per skeleton node (game space for a Live record), appended to the frame's table.
    const bool have_palette = (o.flags & kObjSkinned) && o.bones_count && size_t(o.bones_first) + o.bones_count <= t.bones.size() && !g_plants.no_palette;
    if (have_palette) {
      // S103 W2: the game's vertex is the file's position times the body's scale and its palette does not carry it: scale the palette's linear rows (p * diag(s,s,s,1) * M).
      // An unknown scale is a HOLE: the body is never drawn unscaled (the live caller treats a hole as a miss: the frame falls back whole to the game).
      uint32_t addr = obj_index < t.engine_objects.size() ? t.engine_objects[obj_index] : 0u;
      // (A Viewer record's palette is the viewer's own pose on its own unscaled mesh: no game scale applies, so only a Live record needs s.)
      const bool live_record = t.header.source == uint8_t(Source::Live);
      std::string obj_id(t.Str(o.id));
      bool scale_conflict = false;   // S103 W3: the record's latch value and the stream's measured one disagree: unknown, a hole
      // S103 CW-2: a guest model's vertices ARE the game's own bytes (the body scale is already in them): its palette is never scaled again, and no scale is looked up (unscaled = right for it).
      const float s = unnamed ? 1.0f : BodyScaleFor(live_record, addr, g_plants.no_body_scale, false, &obj_id, &scale_conflict);
      if (!(s > 0.f)) {
        ++st.body_scale_unknown;
        ++rep.holes;
        skip(st.skipped_body_scale, "body scale of " + std::string(t.Str(o.id)) + (scale_conflict ? " is not known (the record's latch value and the stream's measurement disagree): never drawn unscaled" : " is not known (no latch value, the stream has not measured it): never drawn unscaled"), true);
        continue;
      }
      c.bones_first = uint32_t(f.bones.size());
      const bool use_plan = plan.row_of.size() == size_t(nodes) && nodes;   // S103 W2B: the model's skeleton fed row by row (a guest model has no node count: the record's rows as they are)
      const uint32_t main_nodes_c = ctx.models.front_end().ModelMainNodeCount(model);
      c.bones_count = use_plan ? nodes : body_rows;
      for (uint32_t k = 0; k < c.bones_count; ++k) {
        const int32_t row = use_plan ? plan.row_of[k] : int32_t(k);
        if (row < 0) {   // an absent piece's node: zeros, never read (its meshes are skipped); never a rest-pose row
          f.bones.push_back(Mat4{});
          continue;
        }
        Mat4 src = t.bones[o.bones_first + uint32_t(row)];
        if (use_plan && k >= main_nodes_c) {
          const auto* xi = ctx.models.front_end().ModelExtraInfoOf(model, size_t(k - main_nodes_c));
          Mat4 raw = src;   // S103 HGF: the row as the latch read it (the log line's identity / zero fields)
          if (xi && xi->has_corr) {   // S103 W2B (review F): the piece's bind differs from the body's chain (the Rock's pads): row' = corr^-1 x P_j (apply corr^-1 first)
            Mat4 r;
            for (int i = 0; i < 4; ++i)
              for (int j = 0; j < 4; ++j) r.m[i * 4 + j] = xi->corr_inv[i * 4] * src.m[j] + xi->corr_inv[i * 4 + 1] * src.m[4 + j] + xi->corr_inv[i * 4 + 2] * src.m[8 + j] + xi->corr_inv[i * 4 + 3] * src.m[12 + j];
            src = r;
          }
          if (xi && xi->parent >= 0 && uint32_t(xi->parent) < main_nodes_c && live_record) {   // S103 W2B [U] (W2_ADV 2c, review D): hair root vs its body parent, in the record's own space; census only. S103 HGF: measured on the row the front end draws (AFTER corr^-1), and a gap > 20 units is logged
            float wj[3], wp[3];
            const float gap = rdr_scene::SpringRootGapNote(src.m, t.bones[o.bones_first + uint32_t(xi->parent)].m, xi->pos, wj, wp);
            if (gap >= 0.f) {   // every hair root: body_age counts records, the line itself only prints for a big gap
              const auto& tags = ctx.models.front_end().ModelExtraTags(model);
              const size_t e = size_t(k - main_nodes_c);
              LogHairGapBig(t, obj_index, obj_id, e < tags.size() ? tags[e].first : 0u, e < tags.size() ? tags[e].second : 0u, xi->parent, gap, wj, wp, raw.m, xi->has_corr);
            }
          }
        }
        f.bones.push_back(mirror ? MirrorY(src) : src);
      }
      c.flags |= kObjSkinned;
      for (uint32_t k = 0; k < c.bones_count; ++k) ScalePaletteRows(f.bones[c.bones_first + k].m, s);
      if (live_record) ++st.body_scaled;
    } else {
      c.bones_first = 0, c.bones_count = 0;
      c.flags &= ~uint32_t(kObjSkinned);  // the bind pose
      ++st.no_palette;
    }
    if (mirror) c.world = MirrorY(o.world);

    // Light set by game id -> the frame's dense table. Unknown (0xFFFFFFFF) or not among the record's lights: the default set, counted.
    const bool known = !g_plants.ignore_light && o.light_set != 0xFFFFFFFFu && sets.count(o.light_set);
    if (known) {
      c.light_set = dense_of(o.light_set);
      ++st.light_by_id;
    } else if (default_id >= 0) {
      c.light_set = dense_of(uint32_t(default_id));
      ++st.light_default;
    } else {
      c.light_set = 0xFFFFFFFFu;  // the front-end's own default rig
      ++st.light_default;
    }

    // S103 E8: a wrestler whose model call read a light state is lit by THAT state (the game's own constants), not by the arena's set: a copy of the chosen set with the state's lights. One stamp per frame (the key's).
    {
      LightStateUse use = LightStateUse::None;
      const LightState* ls_state = WrestlerLightState(t, obj_index < t.engine_objects.size() ? t.engine_objects[obj_index] : 0u, have_stamp, stamp, use);
      if (use == LightStateUse::Missing) {   // light states are registered (this record has some, or the caller says so) and this visible wrestler has none: the arena's set would light it differently (OURS frames carry no state): a HOLE, the live frame falls back whole
        ++st.light_state_missing;
        ++rep.holes;
        if (rep.hole_why.empty()) rep.hole_why = "the wrestler " + std::string(t.Str(o.id)) + " has no light state at the frame's stamp";
      } else if (use == LightStateUse::Rejected) {   // a zero / non-finite state or a key that is no direction: the old set stays, counted
        ++st.light_state_rejected;
      }
      if (ls_state) {
        const uint32_t skey = uint32_t(ls_state - t.light_states.data());
        auto it = state_sets.find(std::make_pair(c.light_set, skey));
        if (it == state_sets.end()) {
          assets::LightSet n = c.light_set < f.light_sets.size() ? f.light_sets[c.light_set] : assets::LightSet();
          const float* l = ls_state->light;
          for (int k = 0; k < 4; ++k) {
            std::memcpy(n.dir[k], l + kLsDir + 4 * k, 12);
            std::memcpy(n.col[k], l + kLsCol + 4 * k, 12);
            n.wt[k] = l[kLsCol + 4 * k + 3];
            n.fall[k] = 0.f;   // the arena set's falloff (sets 10 / 15: 0.5 on slot 2) is not this state's: the state has none
          }
          std::memcpy(n.hl_dir, l + kLsDir + 16, 12);
          std::memcpy(n.hl_col, l + kLsCol + 16, 12);
          std::memcpy(n.sky, l + kLsBlockA, 12);
          std::memcpy(n.ground, l + kLsBlockB, 12);
          n.hl_view = false;   // the state's key light is already world space
          n.edit_id = -1;      // a copy: the lighting editor's edits never apply to it
          n.has_pcol = false;
          it = state_sets.emplace(std::make_pair(c.light_set, skey), uint32_t(f.light_sets.size())).first;
          f.light_sets.push_back(n);
        }
        c.light_set = it->second;
        ++st.light_by_state;
      }
    }

    // Mesh mask: the attire's pieces the record shows (all ones = every piece of the loaded attire).
    if (g_plants.ignore_mask) c.mesh_mask_lo = c.mesh_mask_hi = 0xFFFFFFFFu;
    else if (o.mesh_mask_lo != 0xFFFFFFFFu || o.mesh_mask_hi != 0xFFFFFFFFu) ++st.mesh_masked;

    if (!pending_piece_alpha.empty()) {   // S103 EYF: this owner's pieces' alphas (owner = "actor:slot:N", the slot the pieces name as layer - 1)
      unsigned owner_slot = 0;
      const std::string_view oid = t.Str(o.id);
      if (StartsWith(oid, "actor:slot:") && fast::ScanUInt(oid, 11, owner_slot))
        for (const PendingPieceAlpha& pa : pending_piece_alpha)
          if (pa.slot == owner_slot) f.piece_alphas.push_back({uint32_t(f.objects.size()), pa.piece, pa.alpha});
    }
    rep.absent_pieces += uint32_t(plan.absent.size());   // S103 W2B (review I)
    st.hair_pieces_absent += uint32_t(plan.absent.size());
    for (uint32_t ap : plan.absent) f.absent_extras.push_back({uint32_t(f.objects.size()), ap});   // S103 W2B: the front end skips the meshes of this object that skin to the piece's extra nodes
    if (obj_index < t.engine_objects.size() && t.engine_objects[obj_index]) wrk::CarryBody(t, f, uint32_t(f.objects.size()), t.engine_objects[obj_index]);   // S103 WRK: the engine join + the pass mode + the live levels (wrk_join.h)   // S103 SWF: the join key of the body's liquid map
    f.objects.push_back(c);
    ++st.drawn;
    ++rep.drawn;
    if (unnamed && have_palette && c.bones_count) {   // S103 CAWB (CAWV Q1, census): the body probe, once per 600 guest frames; FrontEnd adds the additive count and prints it
      static uint64_t last_window = ~0ull;
      const uint64_t win = t.header.guest_frame / 600;
      if (win != last_window) {
        last_window = win;
        SceneFrame::BodyProbe& bp = f.body_probe;
        const float* r = f.bones[c.bones_first].m;
        bp.valid = true, bp.guest_frame = t.header.guest_frame;
        for (int k = 0; k < 3; ++k) bp.pos[k] = r[12 + k];
        const float dx = r[12] - f.camera.eye[0], dy = r[13] - f.camera.eye[1], dz = r[14] - f.camera.eye[2];
        bp.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float* v = f.camera.view.m;   // row vectors; flag 1 = view holds the combined world -> clip matrix
        const bool combined = (f.camera.flags & kCamViewIsViewProj) != 0;
        float cv[4] = {};
        for (int k = 0; k < 4; ++k) cv[k] = r[12] * v[k] + r[13] * v[4 + k] + r[14] * v[8 + k] + v[12 + k];
        float clip[4] = {cv[0], cv[1], cv[2], cv[3]};
        if (!combined) {
          const float* p = f.camera.proj.m;
          for (int k = 0; k < 4; ++k) clip[k] = cv[0] * p[k] + cv[1] * p[4 + k] + cv[2] * p[8 + k] + cv[3] * p[12 + k];
          bp.view_z = cv[2];
        } else {
          bp.view_z = clip[3];   // the combined matrix's w = the view depth
        }
        bp.ndc[0] = clip[3] != 0.f ? clip[0] / clip[3] : 0.f, bp.ndc[1] = clip[3] != 0.f ? clip[1] / clip[3] : 0.f;
        if (!ctx.models.front_end().ModelBounds(model, bp.bmin, bp.bmax)) std::memset(bp.bmin, 0, sizeof bp.bmin), std::memset(bp.bmax, 0, sizeof bp.bmax);
      }
    }
    if (unnamed) guestmodel::NoteActorDrawn(t.header.guest_frame);   // S103 CW-2 (CAWD: the one counting place; the last-draw frame rides with it)
  }
  ctx.reports.push_back(rep);
  return true;
}

}  // namespace rdr_scene::rf
