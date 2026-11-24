// The record -> the E2 maps' inputs (Session 103, lane E2; live_maps_e2.h). Reads the scene record's tables only. An input the record does not carry is NAMED in the `*_missing` lists (lane A moves it).
// Every table is read at ONE stamp chosen by the shadow map's rule (ChooseStamp, live_maps_internal.h); nothing is allocated per frame.
#include <cstring>

#include "live_maps_e2.h"
#include "live_maps_internal.h"
#include "record_io.h"
#include "rows_caster.h"   // S106 G4-2
#include "rows_lights.h"
#include "rows_liquid.h"
#include "rows_pictures.h"
#include "rows_plane.h"

namespace rdr_scene {

bool E2Faults::Parse(const std::string& list, E2Faults& f, std::string& error) {
  size_t at = 0;
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string w = list.substr(at, comma - at);
    at = comma + 1;
    if (w.empty()) continue;
    if (w == "rim_shadow_camera") f.rim_shadow_camera = true;
    else if (w == "rim_clear_black") f.rim_clear_black = true;
    else if (w == "rim_no_depth") f.rim_no_depth = true;
    else if (w == "plane_identity_fit") f.plane_identity_fit = true;
    else if (w == "plane_stale_camera") f.plane_stale_camera = true;
    else if (w == "prelight_wrong_light") f.prelight_wrong_light = true;
    else if (w == "prelight_no_faces") f.prelight_no_faces = true;
    else if (w == "sweat_no_items") f.sweat_no_items = true;
    else if (w == "sweat_flip") f.sweat_flip = true;
    else if (w == "sweat_unbound") f.sweat_unbound = true;   // S103 SWF
    else if (w == "sweat_no_prev") f.sweat_no_prev = true;   // S103 SWF
    else if (w == "plane_eye_ring") f.plane_eye_ring = true;   // S103 E4
    else if (w == "plane_rope_drop_side") f.plane_rope_drop_side = true;   // S103 B2F
    else if (w == "plane_rope_swap_eyes") f.plane_rope_swap_eyes = true;   // S103 B2F
    else if (w == "plane_rope_none") f.plane_rope_none = true;             // S103 B2F
    else {
      error = "unknown E2 maps fault '" + w + "' (rim_shadow_camera, rim_no_depth, rim_clear_black, plane_identity_fit, plane_stale_camera, prelight_wrong_light, prelight_no_faces, sweat_no_items, sweat_flip, plane_eye_ring, plane_rope_drop_side, plane_rope_swap_eyes, plane_rope_none)";
      return false;
    }
  }
  return true;
}

namespace {
bool Finite(float x) { return std::isfinite(x); }
// An iterator over indices (so ChooseStamp can run over a filtered view of a table without a copy).
struct Idx {
  size_t i;
  size_t operator*() const { return i; }
  Idx& operator++() { ++i; return *this; }
  bool operator!=(const Idx& o) const { return i != o.i; }
};
}  // namespace

void BuildE2Input(const SceneTables& t, const E2Faults& faults, E2Input& out) {
  (void)faults;
  out = E2Input();
  const uint32_t frame32 = uint32_t(t.header.guest_frame);

  // ---- rim: the entries that carry a camera state and the extras the entry read (the product and the depth range), at ONE stamp.
  const auto rim_ok = [&](size_t i) {
    const CasterCall& c = t.caster_calls[i];
    return c.pass == kCpRim && c.state < t.caster_states.size() && i < t.caster_extras.size() && (t.caster_extras[i].flags & (kCxProduct | kCxDepth)) == (kCxProduct | kCxDepth);
  };
  for (const CasterCall& c : t.caster_calls)
    if (c.pass == kCpRim) ++out.rim_calls;
  uint32_t rstamp = 0;
  if (ChooseStamp(Idx{0}, Idx{t.caster_calls.size()}, [&](size_t i) { return rim_ok(i) ? t.caster_calls[i].frame : frame32 + 1000u; }, frame32, rstamp)) {
    for (size_t i = 0; i < t.caster_calls.size(); ++i) {
      if (!rim_ok(i) || t.caster_calls[i].frame != rstamp) continue;
      CasterRows rows = BuildCasterRows(t.caster_states[t.caster_calls[i].state]);
      ApplyCasterExtra(rows, t.caster_extras[i], true);
      out.rim_stamp = rstamp;
      if (!rows.rim_cam_valid || !rows.rim_light_depth_valid) out.rim_why = "the rim entry has no camera or depth range";
      else if (!Finite16(&rows.view_proj[0][0])) out.rim_why = "the rim camera is not finite";
      else if (!Finite(rows.rim_light_depth[0]) || !Finite(rows.rim_light_depth[1]) || !(rows.rim_light_depth[1] > rows.rim_light_depth[0])) out.rim_why = "the rim depth range is not finite or far <= near";
      else {
        out.rim_rows = rows;
        out.have_rim = true;
        out.rim_why = "rim pass entry (main camera product, RimLightDepth)";
      }
      break;
    }
  }
  if (!out.have_rim && !*out.rim_why) out.rim_why = out.rim_calls ? "no rim pass entry of one stamp carries the camera product and the depth range" : "the record has no rim pass entry";

  // ---- plane: the plane tables at ONE stamp (kScPlaneFrames); the previous block = the newest stamp BELOW it; PlanePos from the plane calls' pose_t (kCxPlanePos)
  for (const CasterCall& c : t.caster_calls)
    if (c.pass == kCpPlane) ++out.plane_calls;
  uint32_t pstamp = 0;
  if (ChooseStamp(t.plane_frames.begin(), t.plane_frames.end(), [](const PlaneFrame& f) { return f.frame; }, frame32, pstamp)) {
    out.plane_stamp = pstamp;
    for (const PlaneFrame& f : t.plane_frames) {
      if (f.frame == pstamp) {
        out.plane_frame = f;
        out.have_plane = f.have == kPlaneHaveAll && Finite16(f.fit_matrix) && Finite16(f.camera);
        out.plane_nodraw = (f.flags & kPlaneNoDraw) != 0;   // S103 PB
      } else if (int32_t(pstamp - f.frame) > 0 && (!out.have_plane_prev || int32_t(f.frame - out.plane_prev.frame) > 0)) {
        out.plane_prev = f, out.have_plane_prev = true;
      }
    }
  }
  for (size_t i = 0; i < t.caster_calls.size() && i < t.caster_extras.size(); ++i) {
    if (t.caster_calls[i].pass != kCpPlane || !(t.caster_extras[i].flags & kCxPlanePos)) continue;
    const float* p = t.caster_extras[i].pose_t;
    if (!Finite(p[0]) || !Finite(p[1]) || !Finite(p[2])) continue;
    std::memcpy(out.plane_pos, p, 16);
    out.have_plane_pos = out.plane_pos_from_record = true;
    break;
  }
  {   // S103 E5: EyePos per kind + PlanePos from the record's PlaneEyes (recomputed at the latch from the plane module's tables: plane_eye_read.h), at ONE stamp as the plane frame
    uint32_t estamp = 0;
    // the eyes and the plane frame are read at the same latch: when the record has a plane frame the eyes must carry ITS stamp (a mismatch is a hole, never a mix of two frames)
    if (ChooseStamp(t.plane_eyes.begin(), t.plane_eyes.end(), [](const PlaneEyes& e) { return e.frame; }, frame32, estamp) && (t.plane_frames.empty() || estamp == pstamp)) {
      for (const PlaneEyes& e : t.plane_eyes) {
        if (e.frame != estamp) continue;
        out.plane_eyes_stamp = estamp;
        for (uint32_t k = 0; k < kPeKinds; ++k)
          if ((e.have & (1u << k)) && Finite(e.eye[k][0]) && Finite(e.eye[k][1]) && Finite(e.eye[k][2])) out.plane_eye_have |= 1u << k, std::memcpy(out.plane_eye_kind[k], e.eye[k], 16);
        if (out.plane_eye_have & (1u << kPeWrestler)) std::memcpy(out.plane_eye, out.plane_eye_kind[kPeWrestler], 16), out.plane_eye_record = true;
        if (!out.have_plane_pos && (e.flags & kPeHavePos) && Finite(e.plane_pos[1])) std::memcpy(out.plane_pos, e.plane_pos, 16), out.have_plane_pos = out.plane_pos_from_record = true;
        break;
      }
    }
  }
  if (t.plane_frames.empty()) out.plane_missing.Add("kScPlaneFrames at the chosen stamp: fade, screen fit (PlaneInv), camera, normal");
  else if (!out.have_plane && !out.plane_nodraw) out.plane_missing.Add("kScPlaneFrames: no complete finite block at the chosen stamp");
  // S103 E4 review F2: EyePos (VS c72) is PER-ARENA data, not a game constant: the review reads it as module+768+16*kind (kind 1 = the wrestler proxy, ring kinds 4-7 from sub_827F19C0's table
  // [[0x83924768]+48]+24[i], i = arena id), rotated about y by the angle at module+896+4*kind (sub_827ED6D0), stored at module+992..1000 [U: not verified here; written outside the walk]. brock_losswin's arena
  // gives (-450, -1400, 0) for the wrestler proxy in 32 of 32 mask draws and every other capture on disk has no plane chain, so there is ONE arena of evidence. PlanePos (0, -120, 0) is the same case.
  // Until a latch reader supplies both (lane A), they stay missing; the rig may use that arena's values as a FIXTURE (maps_draw --const-eye).
  if (!out.plane_eye_record) out.plane_missing.Add("EyePos (VS c72) for the wrestler proxy: kScPlaneEyes (S103 E5: recomputed at the latch from module+768+16 kind / +896+4 kind by sub_827ED6D0's rotation) has no entry with the wrestler kind loaded at the chosen stamp");
  if (!out.have_plane_pos) out.plane_missing.Add("PlanePos (c7): no kCxPlanePos entry and kScPlaneEyes carries none ([module+1440] unread)");
  BuildRopeInput(t, out.rope);   // S103 B2F: the ropes' proxy palettes are the rope node rows of the record's ring rows (live_map_rope.h)
  if (!out.rope.count) out.plane_missing.Add("the ring's rope rows (kScRingRows rope entries with nodes): the ropes' shadow proxy (entry 956) has no palette, the map holds the wrestlers' shadow only");
  if (out.plane_calls) out.plane_missing.Add("which objects are in the mask (kCpPlane calls' object -> record object) and the proxy palette (kScCasterMats is empty)");
  out.plane_why = out.have_plane ? "the record's plane frame" : "the record carries no plane frame for this frame";

  // ---- pre-light and sweat: the record has no section for their inputs yet (OWN_SMALL_PASSES.md section 4: produced inside the nodes)
  out.prelight_missing.Add("the five light rows of each slot: the slot -> light set pick (10010, the first character's set, 10, the pairs (11,1) (14,2) (12,3) (13,4)) is in the manager row's set ids, the main camera's rotation is the record's camera, but the row FORMULA inside sub_827EBA00 is undecoded [U] (S103 E4: LIVE_MAPS.md)");
  // S103 SWF: the sweat / blood liquid maps (kScLiquidMaps / Items / Pictures): the rows of the record in order, every row with the items its range names and the pictures those items name. A record with none is a DRY
  // record (nothing live, or an older record), not a missing input; a record whose liquid tables were refused (bad layout) says so.
  if (t.liquid_errors) out.sweat_missing.Add("the record's liquid tables were refused (layout / range / picture): every skin draws dry");
  for (const LiquidMapRow& r : t.liquid_maps) {
    if (uint64_t(r.item_first) + r.item_count > t.liquid_items.size()) continue;   // (ParseRecord already refuses such a record: a hand-built SceneTables must not index past the table)
    LiquidMapIn m;
    m.row = r;
    m.items.assign(t.liquid_items.begin() + r.item_first, t.liquid_items.begin() + r.item_first + r.item_count);
    for (const LiquidItemRow& it : m.items) {
      if (!it.picture) continue;
      bool have = false;
      for (const LiquidPictureRow* p : out.liquid_pictures) have = have || p->key == it.picture;
      if (have) continue;
      for (const LiquidPictureRow& p : t.liquid_pictures)
        if (p.key == it.picture) { out.liquid_pictures.push_back(&p); break; }
    }
    out.liquid.push_back(std::move(m));
  }
  for (const LiquidImageRow& r : t.liquid_images) {   // S108 SHOT-A: a saved record's maps (section 69): ParseRecord already checked the range; a hand-built table must not index past the bytes
    if (uint64_t(r.offset) + r.nbytes > t.liquid_image_bytes.size()) continue;
    LiquidImageIn im;
    im.row = r, im.bytes = t.liquid_image_bytes.data() + r.offset;
    out.liquid_images.push_back(im);
  }
}

}  // namespace rdr_scene
