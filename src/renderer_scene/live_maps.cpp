// See live_maps.h. Session 103, lane E.
#include "live_maps.h"
#include "rows_caster.h"   // S106 G4-2
#include "rows_circle.h"
#include "rows_lights.h"
#include "rows_pictures.h"
#include "props_light_rows.h"
#include "liquid_switch.h"   // S103 SWF

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "char_effects.h"       // S103 L7: KeyLightCensus
#include "live_maps_chain.h"   // S103 L7
#include "props_map_rows.h"     // S103 PMS
#include "live_maps_internal.h"
#include "maps_census.h"   // S103 PMC2
#include "props_map_switch.h"   // S103 MBC
#include "prop_cache_sweep.h"   // S103 MBC review 3
#include "shadow_fit_record.h"   // S103 E3: KeyLightFromRecord

namespace rdr_scene {

bool MapsFaults::Parse(const std::string& list, MapsFaults& f, std::string& error) {
  size_t at = 0;
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string w = list.substr(at, comma - at);
    at = comma + 1;
    if (w.empty()) continue;
    if (w == "transpose_light") f.transpose_light = true;
    else if (w == "no_casters") f.no_casters = true;
    else if (w == "circles_wrong_frame") f.circles_wrong_frame = true;
    else if (w == "scale_prm") f.scale_prm = true;
    else if (w == "circles_all_stamps") f.circles_all_stamps = true;
    else if (w == "fit_view_not_light") f.fit_view_not_light = true;     // S103 E3
    else if (w == "fit_key_not_negated") f.fit_key_not_negated = true;
    else if (w == "fit_no_bounds") f.fit_no_bounds = true;
    else if (w == "circles_no_mesh") f.circles_no_mesh = true;   // S103 E4
    else if (w == "props_reader_mapb") f.props_reader_mapb = true;   // S103 PMS
    else if (w == "props_bias_order") f.props_bias_order = true;
    else if (w == "props_no_blobs") f.props_no_blobs = true;
    else if (w == "mapb_wrestlers_only") f.mapb_wrestlers_only = true;   // S103 MBC
    else if (w == "mapb_casts_gm") f.mapb_casts_gm = true, mapb_props::g_plant_cast_gm.store(true, std::memory_order_relaxed);
    else if (w == "props_no_props") f.props_no_props = true;   // S103 PM2
    else if (w == "props_viewdepth_row2") f.props_viewdepth_row2 = true;
    else if (w == "props_casts_gm") f.props_casts_gm = true, props_casters::g_plant_cast_gm.store(true, std::memory_order_relaxed);
    else {
      error = "unknown maps fault '" + w + "' (transpose_light, no_casters, circles_wrong_frame, scale_prm, circles_all_stamps, fit_view_not_light, fit_key_not_negated, fit_no_bounds, circles_no_mesh, props_reader_mapb, props_bias_order, props_no_blobs, mapb_wrestlers_only, mapb_casts_gm, props_no_props, props_casts_gm, props_viewdepth_row2)";
      return false;
    }
  }
  return true;
}

// ---- the record -> the maps' inputs ---------------------------------------------------------------------------------------------------------------------------------


void BuildMapsInput(const SceneTables& t, const MapsFaults& faults, const SceneTables* other, MapsInput& out) {
  BuildMapsInput(t, faults, other, out, MapsFitOptions());
}

void BuildMapsInput(const SceneTables& t, const MapsFaults& faults, const SceneTables* other, MapsInput& out, const MapsFitOptions& fit) {
  out = MapsInput();
  out.force_fit = fit.force_fit || faults.fit_view_not_light || faults.fit_key_not_negated || faults.fit_no_bounds;
  out.fit_main_forward[0] = 0.3031f, out.fit_main_forward[1] = 0.0822f, out.fit_main_forward[2] = 0.9494f;   // the view_not_light plant's "main camera forward" (brock_losswin f110's, stream c59 row)
  out.guest_frame = t.header.guest_frame;
  const uint32_t frame32 = uint32_t(t.header.guest_frame);
  out.has_caster_records = !t.caster_calls.empty();

  // The wrestlers' shadow light: the render camera record R as a MAP pass entry of a wrestler (a call with a palette) read it, from the entries of ONE stamp. Map B's entry (collapse 0) first: it is
  // the full map; map A's (collapse 1) has the same light in every capture seen (SHADOW_PROMOTION_MAP.md 1.2). No fallback to a colour call's light state: that is the props map's camera
  // (SKIP_GAME_WORK_MAP 1253), not this map's.
  const LightState* st = nullptr;
  uint32_t stamp = 0;
  const auto wrestler_call = [&](const CasterCall& c) { return c.pass == kCpMap && c.n_nodes > 0 && c.state < t.caster_states.size(); };
  std::vector<const CasterCall*> calls;
  for (const CasterCall& c : t.caster_calls)
    if (wrestler_call(c)) calls.push_back(&c);
  if (ChooseStamp(calls.begin(), calls.end(), [](const CasterCall* c) { return c->frame; }, frame32, stamp)) {
    for (int pass = 0; pass < 2 && !st; ++pass)
      for (const CasterCall* c : calls) {
        if (c->frame != stamp || (pass == 0 && c->collapse != 0)) continue;
        st = &t.caster_states[c->state];
        break;
      }
    out.light_stamp = stamp;
  }
  if (!st) {
    out.light_why = out.has_caster_records ? "no caster MAP entry in the record" : "inputs missing (pass skipped?)";
  } else if (!((st->flags & kLsMapKnown) && st->map_wh[0] >= 64.f && st->map_wh[1] >= 64.f && st->map_wh[0] <= 4096.f && st->map_wh[1] <= 4096.f)) {
    out.light_why = "the light's map size is unknown or out of range";   // the readers' bias needs the size: not published
  } else {
    out.rows = BuildCasterRows(*st);
    out.cam = BuildWrestlerCameraRows(*st);   // uses the same (checked) size
    if (!Finite16(&out.rows.view_proj[0][0]) || !Finite16(&out.rows.trapezoid[0][0])) {
      out.light_why = "the light matrices are not finite (a NaN light: victory scene)";
    } else if (!out.cam.valid) {
      out.light_why = "no readers' projection (map size unknown)";
    } else {
      out.have_light = true;
      out.light_why = "caster map pass entry";
      out.map_w = uint32_t(st->map_wh[0]), out.map_h = uint32_t(st->map_wh[1]);
    }
  }

  // S103 E3: the fitted light camera's key light, only when it will be used (no game caster state, or forced). TOLD (the rig) wins over the record.
  if (out.force_fit || !out.have_light) {
    if (fit.have_told_key) {
      std::memcpy(out.fit_key, fit.told_key, sizeof out.fit_key);
      out.fit_key_valid = true, out.fit_key_told = true;
      out.fit_key_why = "TOLD to the rig (not in the record)";
    } else {
      out.fit_key_valid = KeyLightFromRecord(t, out.fit_key, out.fit_key_why);
    }
  }

  out.casters_expected = RecordHasWrestler(t);
  out.shadow_hole = out.casters_expected && (out.force_fit || (!out.have_light && !out.has_caster_records)) && !out.fit_key_valid;   // review 2: decided here, before Record

  // The circle blobs: ONE stamp's entries, at most one per slot (a catch-up record repeats a character under older stamps).
  const SceneTables& cs = other ? *other : t;
  out.has_circle_records = !cs.circle_shadows.empty();
  out.light_degenerate = false;   // S103 PMC2: a kPlDegenerate row in THIS record's props-light table (the latch's: the circle read found module+2240 non-finite, or the props light had no usable value)
  for (const PropsLight& pl : t.props_light) out.light_degenerate = out.light_degenerate || ((pl.flags & kPlDegenerate) && pl.version == kPropsLightLayout);
  std::vector<CircleShadow> entries = cs.circle_shadows;
  if (faults.circles_all_stamps)   // plant: catch-up entries (every entry again under stamp - 1) and no one-stamp rule
    for (size_t i = 0, n = cs.circle_shadows.size(); i < n; ++i) {
      CircleShadow d = cs.circle_shadows[i];
      --d.frame;
      entries.push_back(d);
    }
  uint32_t cstamp = 0;
  const bool have_stamp = ChooseStamp(entries.begin(), entries.end(), [](const CircleShadow& e) { return e.frame; }, frame32, cstamp);
  out.circle_stamp = cstamp;
  uint32_t slots_seen = 0;
  for (const CircleShadow& e : entries) {
    const uint32_t below = frame32 - e.frame;
    const bool in_window = below <= kStampWindow;
    if (faults.circles_all_stamps ? !in_window : !(have_stamp && e.frame == cstamp)) {
      ++out.circles_rejected_stamp;
      continue;
    }
    if (!faults.circles_all_stamps && (slots_seen >> (e.slot & 31)) & 1u) {   // a second entry of a slot under the same stamp: the first stays
      ++out.circles_rejected_stamp;
      continue;
    }
    slots_seen |= 1u << (e.slot & 31);
    if ((e.flags & (kCircleHaveViewProj | kCircleHaveParam)) != (kCircleHaveViewProj | kCircleHaveParam) || !Finite16(e.view_proj)) {
      ++out.circles_incomplete;
      continue;
    }
    out.circles.push_back(e);
  }
  if (!out.circles.empty()) {   // S103 PMS: the props map's floor ShadowPrm, from the colour-call state whose Trapezoidal x ViewProj is these circles' rows (one light a frame: every entry shares it)
    out.props_prm_valid = props_map::PrmFromRecord(t, out.circles[0].view_proj, out.props_prm, out.props_prm_why);
  } else {
    out.props_prm_why = "props ShadowPrm: no circle entry to take the props light from";
  }
  const SceneTables& ms = !cs.circle_mesh.empty() ? cs : t;
  if (!ms.circle_mesh.empty() && !faults.circles_no_mesh) out.have_circle_mesh = true, out.circle_mesh = ms.circle_mesh[0];
  BuildE2Input(t, E2Faults(), out.e2);   // S103 E2: the plane, rim, pre-light and sweat inputs (a rig with E2 faults calls it again)
}

// ---- the hook --------------------------------------------------------------------------------------------------------------------------------------------------------

LiveMapsHook::LiveMapsHook(rdr::Renderer& renderer, ModelProvider provider) : renderer_(renderer), provider_(std::move(provider)) {
  e2_ = std::make_unique<E2State>();   // S103 E2
  chain_ = std::make_unique<ChainState>();   // S103 L7
  e2_faults_.sweat_unbound = liquid_map::Plants().unbound;   // S103 SWF planted twins (the rig's --liquid-plant sets them before the hooks are made; no environment read)
  e2_faults_.sweat_no_prev = liquid_map::Plants().no_prev;
}

LiveMapsHook::~LiveMapsHook() {
  renderer_.WaitIdle();   // (the owner destroys the hook with the renderer's GPU work finished)
  ReleaseCasters();
  for (const Grave& g : graveyard_) {
    if (g.buffer != rdr::kNoBuffer) renderer_.DestroyBuffer(g.buffer);
    if (g.image != rdr::kNoImage) renderer_.DestroyImage(g.image);
  }
  graveyard_.clear();
  // S103 E2: the E2 maps' images and buffers
  for (rdr::ImageId id : {e2_->rim_color, e2_->rim_depth, e2_->plane_color, e2_->plane_depth, e2_->plane_half, e2_->plane_blur_a, e2_->plane_blur_b})   // S103 L9: the plane chain's images
    if (id != rdr::kNoImage) renderer_.DestroyImage(id);
  for (auto& kv : e2_->liquid)   // S103 SWF: the liquid maps, the snapshot, the disc and the item pictures
    if (kv.second.image != rdr::kNoImage) renderer_.DestroyImage(kv.second.image);
  e2_->liquid.clear();
  if (e2_->liquid_prev != rdr::kNoImage) renderer_.DestroyImage(e2_->liquid_prev);
  for (rdr::BufferId b : {e2_->liquid_disc_vb, e2_->liquid_disc_ib, e2_->liquid_quad_vb})
    if (b != rdr::kNoBuffer) renderer_.DestroyBuffer(b);
  if (e2_->liquid_pics) {
    struct B { rdr::Renderer& r; uint32_t Grey() { return 0; } uint32_t Create(const LiquidPictureRow&) { return 0; } void Update(uint32_t, const LiquidPictureRow&) {} void Destroy(uint32_t id) { r.DestroyImage(rdr::ImageId(id)); } } b{renderer_};
    e2_->liquid_pics->Clear(b);
  }
  if (e2_->pl_vb != rdr::kNoBuffer) renderer_.DestroyBuffer(e2_->pl_vb);
  if (e2_->blur_vb != rdr::kNoBuffer) renderer_.DestroyBuffer(e2_->blur_vb);   // S103 L9
  if (e2_->pl_ib != rdr::kNoBuffer) renderer_.DestroyBuffer(e2_->pl_ib);
  if (e2_->rope) {   // S103 B2F: join the proxy's load worker, then its three buffers (the GPU is idle, WaitIdle above)
    if (e2_->rope->worker.joinable()) e2_->rope->worker.join();
    for (rdr::BufferId b : {e2_->rope->vb, e2_->rope->sv, e2_->rope->ib})
      if (b != rdr::kNoBuffer) renderer_.DestroyBuffer(b);
    e2_->rope->vb = e2_->rope->sv = e2_->rope->ib = rdr::kNoBuffer;
  }
  for (rdr::ImageId id : e2_->prelight_strips)
    if (id != rdr::kNoImage) renderer_.DestroyImage(id);
  for (rdr::ImageId id : {chain_->sh_mid, chain_->sh_out, chain_->rim_out})   // S103 L7 (review 1): the chain's images
    if (id != rdr::kNoImage) renderer_.DestroyImage(id);
  if (shadow_color_ != rdr::kNoImage) renderer_.DestroyImage(shadow_color_);
  if (shadow_depth_ != rdr::kNoImage) renderer_.DestroyImage(shadow_depth_);
  if (circles_color_ != rdr::kNoImage) renderer_.DestroyImage(circles_color_);
  if (props_depth_ != rdr::kNoImage) renderer_.DestroyImage(props_depth_);   // S103 PM2
  if (circle_vb_ != rdr::kNoBuffer) renderer_.DestroyBuffer(circle_vb_);
  if (circle_ib_ != rdr::kNoBuffer) renderer_.DestroyBuffer(circle_ib_);
}

void LiveMapsHook::Retire(rdr::BufferId b) {
  if (b != rdr::kNoBuffer) graveyard_.push_back({frames_, b, rdr::kNoImage});
}
void LiveMapsHook::Retire(rdr::ImageId i) {
  if (i != rdr::kNoImage) graveyard_.push_back({frames_, rdr::kNoBuffer, i});
}
void LiveMapsHook::RetireModel(CasterModel& m) {
  Retire(m.vb), Retire(m.ib), Retire(m.svb);
  m.vb = m.ib = m.svb = rdr::kNoBuffer;
}
void LiveMapsHook::SweepGraveyard() {
  SweepStaleProps(prop_casters_, frames_, kPropCasterMaxAge, [&](CasterModel& m) { RetireModel(m); });   // S103 MBC review 3: before the graveyard pass (the buffers wait kGraveFrames there)
  // S103 CS1B review F2 / F3: the wrestler / guest casters are bounded by LIVENESS, not age: an entry whose model the provider no longer holds (the id was retired; ids are never reused: every editor slider drag makes a new guest id) goes.
  // A live wrestler keeps its buffers whatever the switches say.
  if (provider_)
    for (auto it = casters_.begin(); it != casters_.end();) {
      if (provider_(it->first) == nullptr) RetireModel(*it->second), it = casters_.erase(it);
      else ++it;
    }
  size_t keep = 0;
  for (size_t i = 0; i < graveyard_.size(); ++i) {
    const Grave& g = graveyard_[i];
    if (frames_ - g.frame >= kGraveFrames) {   // frames in flight are 2: four frames later nothing draws from it
      if (g.buffer != rdr::kNoBuffer) renderer_.DestroyBuffer(g.buffer);
      if (g.image != rdr::kNoImage) renderer_.DestroyImage(g.image);
    } else {
      graveyard_[keep++] = g;
    }
  }
  graveyard_.resize(keep);
}

void LiveMapsHook::ForgetAsset(uint32_t asset) {
  if (const auto pit = prop_casters_.find(asset); pit != prop_casters_.end()) RetireModel(*pit->second), prop_casters_.erase(pit);   // S103 MBC
  const auto it = casters_.find(asset);
  if (it == casters_.end()) return;
  RetireModel(*it->second);
  casters_.erase(it);
}

void LiveMapsHook::ReleaseCasters() {
  for (auto& kv : casters_) {
    if (kv.second->vb != rdr::kNoBuffer) renderer_.DestroyBuffer(kv.second->vb);
    if (kv.second->ib != rdr::kNoBuffer) renderer_.DestroyBuffer(kv.second->ib);
    if (kv.second->svb != rdr::kNoBuffer) renderer_.DestroyBuffer(kv.second->svb);
  }
  casters_.clear();
  for (auto& kv : prop_casters_) {   // S103 MBC
    if (kv.second->vb != rdr::kNoBuffer) renderer_.DestroyBuffer(kv.second->vb);
    if (kv.second->ib != rdr::kNoBuffer) renderer_.DestroyBuffer(kv.second->ib);
  }
  prop_casters_.clear();
  shadow_drawn_w_ = shadow_drawn_h_ = 0;   // S103 SPBF L4: a scene / arena change: the old map is not shown again
}

void LiveMapsHook::Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& fb) {
  if (stage != Stage::Maps) return;
  counts_ = MapsCounts();
  meas_ = Measured();   // S103 E3 (review 6)
  maps_.maps.clear();
  ++frames_;
  SweepGraveyard();
  const MapsInput* in = input_;
  input_ = repeatable_ ? input_ : nullptr;   // read once: a frame that does not set its input draws no maps (never last frame's). S103 E3: unless the owner said the input is repeatable (the offline rig's RenderToPixels draws a seed frame first: two Records per attach)
  if (!in) {
    counts_.shadow_skip = counts_.circles_skip = "no input (the frame has no record tables)";
    e2_->rim_skip = e2_->plane_skip = e2_->prelight_skip = e2_->sweat_skip = "no input (the frame has no record tables)";   // S103 E2
    e2_->rim_objects = e2_->rim_meshes = e2_->plane_draws = e2_->prelight_slots = e2_->sweat_items = 0;
    return;
  }
  input_ = in;   // (the per-map functions read input_; cleared again below)
  e2_->rim_skip = e2_->plane_skip = e2_->prelight_skip = e2_->sweat_skip = "";   // S103 E2: per-frame reset
  if (RecordShadow(ctx, fb)) ++shadow_made_;
  const uint32_t holes_before_circles = counts_.holes;
  if (RecordCircles(ctx, fb)) ++circles_made_;
  {   // S103 PMS: never a silent "ok": no PropsMap with the switch on = a counted hole with its reason. S103 PMC2: a degenerate game light is `props_light_degenerate`, not a hole; one missing image is one hole (maps_census.h)
    const bool props_on = props_map::On();
    const maps_census::PropsOutcome po = maps_census::ClassifyProps(counts_.props_degenerate, props_on, props_on && !maps_.Find(MapRole::PropsMap) && !counts_.props_skip[0]);
    if (po == maps_census::PropsOutcome::Degenerate) {
      ++props_degenerate_;
      counts_.props_skip = counts_.circles_skip;
    } else if (po == maps_census::PropsOutcome::Hole) {
      counts_.props_skip = counts_.circles_skip[0] ? counts_.circles_skip : "no circle map";
      ++props_holes_;
      counts_.holes = maps_census::HolesWithPropsHole(holes_before_circles, counts_.holes);
    }
  }
  // S103 E2: the plane, rim, pre-light and sweat maps (inside the input_ bracket: they read input_->e2)
  if (RecordRim(ctx, fb)) ++e2_->rim_made;
  if (RecordPlane(ctx, fb)) ++e2_->plane_made;
  if (RecordPreLight(ctx, fb)) ++e2_->prelight_made;
  if (RecordSweat(ctx, fb)) ++e2_->sweat_made;
  ChainMaps(fb);   // S103 L7: the game's blur / min-pair chains over the raw shadow and rim maps just made
  // S103 E3: a map that was not made carries its reason in its skip string; each is one counted hole (never a silent empty map on an OURS frame)
  counts_.holes += (e2_->rim_skip[0] != 0) + (e2_->plane_skip[0] != 0 && !e2_->plane_not_read) + (e2_->prelight_skip[0] != 0) + (e2_->sweat_skip[0] != 0);
  input_ = repeatable_ ? in : nullptr;
  counts_.maps_made = uint32_t(maps_.maps.size());
  if (!in->has_caster_records && !in->has_circle_records && !in->light_degenerate) ++inputs_missing_frames_;   // S103 PMC2: a degenerate light is the game's state, not a missing input
}

std::string LiveMapsHook::CensusLine() const {
  char b[800];   // S103 PMS: 480 -> 640 (the props clause); S103 CS1B: 720 (guest casters); S103 PMC2: 800 (props_light_degenerate)
  if (counts_.maps_made == 0 && inputs_missing_frames_ && inputs_missing_frames_ == frames_)
    return "[scene][maps] inputs missing (pass skipped?): the record has no kScCasterCalls and no kScCircleShadows (their writers run inside the game's walks)";
  std::snprintf(b, sizeof b,
                "[scene][maps] frames %llu (inputs missing in %llu), shadow made %llu (casters %u objects / %u meshes; skipped no model %u, hidden %u, not a character %u; %s), circles made %llu (drawn %u, rejected %u, incomplete %u; %s), props made %llu (holes %llu, props_light_degenerate %llu; %s), maps handed over %u, guest casters %u; "
                "E2: see the second line",
                (unsigned long long)frames_, (unsigned long long)inputs_missing_frames_, (unsigned long long)shadow_made_, counts_.casters_objects, counts_.casters_meshes, counts_.casters_skipped_no_model,
                counts_.casters_skipped_hidden, counts_.casters_skipped_not_char, counts_.shadow_skip[0] ? counts_.shadow_skip : "ok", (unsigned long long)circles_made_, counts_.circles_drawn,
                counts_.circles_rejected_stamp, counts_.circles_incomplete, counts_.circles_skip[0] ? counts_.circles_skip : "ok", (unsigned long long)props_made_, (unsigned long long)props_holes_, (unsigned long long)props_degenerate_, counts_.props_skip[0] ? counts_.props_skip : props_map::On() ? "ok" : "switch off", counts_.maps_made, guest_casters_last_);
  // S103 E2: counts per E2 map (relaxed integers; the strings are built here, never per draw)
  char e[1300];
  const E2State& s = *e2_;
  std::snprintf(e, sizeof e,
                "\n[scene][maps][e2] rim made %llu (objects %u, meshes %u; %s), plane made %llu (draws %u; %s; no-draw frames %llu; blank-plane frames %llu, passes skipped %llu; no-caster frames %llu; chain-failed frames %llu; mask passes %llu, of them empty %llu; seen %llu), pre-light made %llu (slots %u; %s), sweat made %llu (items %u; %s)",
                (unsigned long long)s.rim_made, s.rim_objects, s.rim_meshes, s.rim_skip[0] ? s.rim_skip : "ok", (unsigned long long)s.plane_made, s.plane_draws, s.plane_skip[0] ? s.plane_skip : "ok", (unsigned long long)s.plane_nodraw_frames, (unsigned long long)s.plane_blank_frames, (unsigned long long)s.plane_blank_passes, (unsigned long long)s.plane_no_caster_frames, (unsigned long long)s.plane_chain_failed_frames, (unsigned long long)s.plane_mask_passes, (unsigned long long)s.plane_mask_empty, (unsigned long long)s.plane_seen,
                (unsigned long long)s.prelight_made, s.prelight_slots, s.prelight_skip[0] ? s.prelight_skip : "ok", (unsigned long long)s.sweat_made, s.sweat_items, s.sweat_skip[0] ? s.sweat_skip : "ok");
  // S103 E3: where the shadow camera came from this frame, and how many maps were holes (each hole has its reason in the shadow / circles / e2 strings above)
  char f[360];
  std::snprintf(f, sizeof f, "\n[scene][maps][e3] shadow camera %s (casters sampled %u, radius %.0f, squeeze %.0f), holes this frame %u",
                counts_.shadow_fitted ? "FITTED by shadow_fit.h" : (counts_.shadow_skip[0] ? "none" : "from the game's caster state"), counts_.fit_samples, counts_.fit_radius, counts_.fit_scale, counts_.holes);
  char l7[360];   // S103 L7
  uint64_t key_n = 0, key_x = 0;
  KeyLightCensus(key_n, key_x);
  std::snprintf(l7, sizeof l7, "\n[scene][maps][l7] chains: shadow filtered %llu (passes %u), rim filtered %llu (passes %u)%s%s; key light rotated %llu times, with |hl_dir.x| > 1e-3: %llu (right-axis sign [U] until one is seen)",
                (unsigned long long)chain_->shadow_made, chain_->shadow_passes, (unsigned long long)chain_->rim_made, chain_->rim_passes, chain_->skip[0] ? "; skipped: " : "", chain_->skip,
                (unsigned long long)key_n, (unsigned long long)key_x);
  char rp[300];   // S103 B2F: the ropes' shadow proxy (ring entry 956) in the plane mask
  if (s.rope)
    std::snprintf(rp, sizeof rp, "\n[scene][maps][b2f] rope proxy: ropes drawn %u (dropped %u), draws %u%s%s", s.rope->ropes_drawn, s.rope->ropes_dropped, s.rope->draws, s.rope->note[0] ? "; not drawn: " : "", s.rope->note);
  else
    std::snprintf(rp, sizeof rp, "\n[scene][maps][b2f] rope proxy: not reached (no plane mask this frame)");
  // S103 SWF: the wrestlers' liquid maps (live_map_sweat.cpp): frames with rows, maps painted, items drawn, what was skipped and why, maps kept / handed to the skin / not matched to a body in the frame
  char lq[1200];
  std::snprintf(lq, sizeof lq,
                "\n[scene][maps][liquid] frames %llu, maps %llu (this frame %u), items drawn %llu (this frame %u; skipped: state %llu, type %llu, rect %llu, no_picture %llu, resources %llu, size %llu, repeat %llu), clears %llu, kept %zu, handed to the skin %u (unmatched %llu, no_object %llu, pruned %llu); "
                "rebuild rows %llu (empty %llu, on a later row %llu): sweat discs %llu, regions %llu (mirrored: regions %llu, discs %llu; live items held back %llu), no picture %llu%s%s",   // S108 BLD2: the game's rebuild draw (liquid_plan.h); BLD3: packages applied, of them re-sent on a later row
                (unsigned long long)s.liq_frames, (unsigned long long)s.liq_maps_total, s.liq_maps_frame, (unsigned long long)s.liq_items_total, s.liq_items_frame, (unsigned long long)s.liq_skip_state, (unsigned long long)s.liq_skip_type,
                (unsigned long long)s.liq_skip_rect, (unsigned long long)s.liq_skip_no_pic, (unsigned long long)s.liq_skip_resources, (unsigned long long)s.liq_skip_size, (unsigned long long)s.liq_skip_repeat, (unsigned long long)s.liq_clears, s.liquid.size(), s.liq_published,
                (unsigned long long)s.liq_unmatched, (unsigned long long)s.liq_no_object, (unsigned long long)s.liq_pruned, (unsigned long long)s.liq_alt_rows, (unsigned long long)s.liq_alt_empty, (unsigned long long)s.liq_alt_late, (unsigned long long)s.liq_rebuild_drawn,
                (unsigned long long)s.liq_regions_drawn, (unsigned long long)s.liq_regions_mirrored, (unsigned long long)s.liq_discs_mirrored, (unsigned long long)s.liq_items_mirrored, (unsigned long long)s.liq_alt_no_pic, s.sweat_skip[0] ? "; hole: " : "", s.sweat_skip);
  char pc[640];   // S103 PM2: the props map's prop casters (live_map_props.cpp)
  std::snprintf(pc, sizeof pc, "\n[scene][maps][props] casters %u objects / %u meshes (weapons %u / event %u / stage %u, ids 59/68/71/96 %u), skipped: no model %u, categories 33 / 43 left to map B %u (the props pass never draws them), no props light %u; prop min light-depth < near %u; %s",
                counts_.props_cast, counts_.props_meshes, counts_.props_fam_weapon, counts_.props_fam_event, counts_.props_fam_stage, counts_.props_fam_unsure, counts_.props_skipped_no_model, counts_.props_skipped_piece43,
                counts_.props_skipped_no_light, counts_.props_near_clip, counts_.props_cast_why[0] ? counts_.props_cast_why : props_casters::On() ? "drawn" : "switch off");
  char mb[640];   // S103 MBC: map B's prop casters (RMP.md 2.3 site 6)
  std::snprintf(mb, sizeof mb, "\n[scene][maps][mapb] mode %s, props cast %u objects / %u meshes (weapons %u, event props %u, structure pieces %u, ids 59/68/71/96 %u), skipped: mode 1 %u, gate unknown %u, no model %u, categories 33 / 43 at scene word 1 %u; out of the fit %u, prop min light-depth < near %u; ring (category 24) gate %s: not built; categories 33 / 43: cast per draw node (CWDT / CWDS entries, ids 2064 / 2556)",
                counts_.mapb_mode[0] ? counts_.mapb_mode : "-", counts_.mapb_props_cast, counts_.mapb_props_meshes, counts_.mapb_fam_weapon, counts_.mapb_fam_event, counts_.mapb_fam_stage, counts_.mapb_fam_unsure,
                counts_.mapb_skipped_mode1, counts_.mapb_skipped_hole, counts_.mapb_skipped_no_model, counts_.mapb_skipped_piece43, counts_.mapb_out_of_fit, counts_.mapb_near_clip, counts_.mapb_ring_gate ? "SET" : "0");
  return std::string(b) + e + f + l7 + rp + lq + mb + pc;
}

// An image is made without a wait; a different size retires the old one (destroyed kGraveFrames frames later).
bool LiveMapsHook::EnsureImages(uint32_t w, uint32_t h, bool circles) {
  if (!circles) {
    if (shadow_color_ != rdr::kNoImage && shadow_w_ == w && shadow_h_ == h) return true;
    Retire(shadow_color_), Retire(shadow_depth_);
    rdr::ImageDesc c;
    c.width = uint16_t(w), c.height = uint16_t(h);
    c.format = rdr::Format::RGBA16F;
    c.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    shadow_color_ = renderer_.CreateImage(c);
    rdr::ImageDesc d = c;
    d.format = rdr::Format::Depth;
    d.usage = rdr::kUsageTarget | rdr::kUsageTransient;
    shadow_depth_ = renderer_.CreateImage(d);
    shadow_w_ = w, shadow_h_ = h;
    shadow_drawn_w_ = shadow_drawn_h_ = 0;   // S103 SPBF L4: a new image holds no drawn map
    if (shadow_color_ != rdr::kNoImage) renderer_.SetImageClass(shadow_color_, rdr::kGpuShadow);
    return shadow_color_ != rdr::kNoImage && shadow_depth_ != rdr::kNoImage;
  }
  if (circles_color_ != rdr::kNoImage && circles_w_ == w && circles_h_ == h) return true;
  Retire(circles_color_), Retire(props_depth_);   // S103 PM2: the props pass's depth (made with the first prop caster below)
  props_depth_ = rdr::kNoImage;
  rdr::ImageDesc c;
  c.width = uint16_t(w), c.height = uint16_t(h);
  c.format = rdr::Format::RGBA8;
  c.usage = rdr::kUsageTarget | rdr::kUsageSampled;
  circles_color_ = renderer_.CreateImage(c);
  circles_w_ = w, circles_h_ = h;
  if (circles_color_ != rdr::kNoImage) renderer_.SetImageClass(circles_color_, rdr::kGpuShadow);
  return circles_color_ != rdr::kNoImage;
}

}  // namespace rdr_scene
