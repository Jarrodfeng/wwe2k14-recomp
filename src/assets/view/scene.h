// Session 98 (scenes): an "actor" = one wrestler placed in the loaded arena with its own transform (position + yaw), attire,
// motion and frame, and its own skin palette. The viewer (viewer_main.cpp) owns the list; the renderer holds one model per
// actor (Renderer::SetTransform turns and moves it). Space: the viewer's (Y up, +Z toward the entrance stage).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../asset_model.h"
#include "../entrance.h"
#include "../motion.h"

namespace view {

// A skinned vertex low on the body (the feet and boots): kept to find where the sole of the posed wrestler is.
struct FootVert {
  float p[3];
  uint16_t b[4];  // (Session 99: Vertex::bones is 16-bit)
  float w[4];
};

// The skinned vertices whose bind height is below `ymax` (viewer space, feet at about 0).
inline std::vector<FootVert> CollectFeet(const assets::Model& m, float ymax = 25.f) {
  std::vector<FootVert> out;
  for (const assets::Mesh& me : m.meshes) {
    if (!me.skinned) continue;
    for (const assets::Vertex& v : me.vertices) {
      if (!(v.pos[1] < ymax) || !std::isfinite(v.pos[1])) continue;
      FootVert f;
      for (int i = 0; i < 3; ++i) f.p[i] = v.pos[i];
      for (int i = 0; i < 4; ++i) f.b[i] = v.bones[i], f.w[i] = v.weights[i];
      out.push_back(f);
      if (out.size() >= 60000) return out;
    }
  }
  return out;
}

// Lowest posed height among the feet vertices (skin empty = the bind pose); the posed centroid of them goes to `cxz`.
inline float PosedFeet(const std::vector<FootVert>& feet, const std::vector<assets::Matrix>& skin, float cxz[2]) {
  float lo = 1e30f, sx = 0, sz = 0;
  size_t n = 0;
  for (const FootVert& f : feet) {
    float x = 0, y = 0, z = 0, wsum = 0;
    if (skin.empty()) {
      x = f.p[0], y = f.p[1], z = f.p[2], wsum = 1;
    } else {
      for (int k = 0; k < 4; ++k) {
        if (f.w[k] <= 0 || f.b[k] >= skin.size()) continue;
        const assets::Matrix& M = skin[f.b[k]];
        x += f.w[k] * (f.p[0] * M[0] + f.p[1] * M[4] + f.p[2] * M[8] + M[12]);
        y += f.w[k] * (f.p[0] * M[1] + f.p[1] * M[5] + f.p[2] * M[9] + M[13]);
        z += f.w[k] * (f.p[0] * M[2] + f.p[1] * M[6] + f.p[2] * M[10] + M[14]);
        wsum += f.w[k];
      }
      if (wsum <= 0) continue;
      x /= wsum, y /= wsum, z /= wsum;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    lo = std::min(lo, y);
    sx += x, sz += z;
    ++n;
  }
  cxz[0] = n ? sx / (float)n : 0.f;
  cxz[1] = n ? sz / (float)n : 0.f;
  return n ? lo : 0.f;
}

struct Actor {
  std::string id;
  int wrestler = 0, attire = 0;
  std::string spot;            // the spot name, "pos" for a free position
  double t = 0;                // ramp parameter
  float pos[3] = {0, 0, 0};    // world position of the feet (Y includes the grounding)
  float floor_y = 0;           // the surface height at the spot
  float yaw = 0;               // degrees, atan2(dx, dz) of the facing direction (0 faces +Z, 180 faces -Z)
  size_t model = 0;            // index into the renderer's models
  std::string light;           // which light set the wrestler uses
  // motion state (per actor)
  assets::motion::Motion motion;
  bool have_motion = false, pose_motion = false;
  double frame = 0;
  std::string motion_label;
  // grounding
  std::vector<FootVert> feet;
  std::vector<assets::Matrix> skin;
  float feet_y = 0, feet_cxz[2] = {0, 0};  // posed lowest foot height (model space) and the posed foot centroid
  bool ground = true;   // feet on the surface (the lowest posed foot vertex)
  bool anchor = true;   // the posed feet's centroid on the spot (not the model origin)
  // The anchor is frozen at placement / when the motion or frame is set (model-space centroid of the posed feet then): playing
  // moves the root freely from there. ground_play re-grounds the feet every played frame (root height motions); default off.
  float anchor_dx = 0, anchor_dz = 0;
  bool ground_play = false;
  int light_mode = -3;  // place "light": -3 auto (the spot's zone set), -1 default rig, N an arena set number (kept for re-placing)
  // Session 98 (shadow / match light): "auto" off the stage / ramp in the MATCH look = the game's match wrestler light: arena set 10,
  // camera-attached (match_base holds its view-space directions; viewer_main MatchLightsFollowCamera turns them every frame).
  bool match_light = false;
  assets::LightSet match_base;
  bool want_bind = false;  // place "motion":"bind": keep the bind pose (otherwise a wrestler without a motion stands in the idle)
  float origin[3] = {0, 0, 0};  // where the model origin ends up (after grounding and anchoring)
  // Session 98 (entrance): an entrance actor is driven by the EntranceState timeline, not by `frame` / the global play.
  bool entrance = false;
  bool has_opt = false;               // own pose options (the entrance poses at translation scale 1.0)
  assets::motion::PoseOpts opt;
  int ent_cue = -1;                   // the cue index whose body motion `motion` currently holds
};

// Session 98 (entrance): what the viewer's entrance command keeps. The timeline is in motion frames (1/60 s).
struct EntranceState {
  bool active = false, playing = false;
  std::shared_ptr<assets::entrance::Show> show;
  assets::entrance::Timeline tl;
  std::string actor = "ent";
  double t = 0, speed = 1, intro_hold = 0;   // scene time (motion frames); intro_hold seconds added to cue 0
  size_t cue = 0;
  double local = 0;
  std::string root_y = "canvas";               // "raw" | "ground" | "scale" | "canvas" (Session 98 replay S5: canvas = the game's rule)
  bool cam_entrance = false, show_cue0 = false, light_keyed = true, lights_on = true;
  double cam_dy = 120;  // [U] added to the entrance camera's eye and aim heights: the file values put the eye under the floor (see entrance.md)
  int take = 1;
  bool l4 = true;                              // set 10's white light 4 in the entrant's 4th slot (A/B switch "l4")
  bool prop_light = true;                      // Session 98 (sweep look): belt / hand props lit by the entrant's lights (A/B "prop_light")
  bool rig_weights = true;                     // Session 98 (sweep look): lobe weights 1.0 unless the driving cue has slot 4 (A/B "rig_weights")
  bool rig_loop = true;                        // Session 98 (generalisation): rig motions loop / keep running in rig-less cues (A/B switch "rig_loop")
  bool stage_ground = true;                  // Session 98 (generalisation): lift the entrant by the stage surface under him (A/B switch "ground")
  float ground_dy = 0;                         // the lift applied at the current frame (viewer units)
  bool rig_on = true;                        // the act-25011 rig's colours replace the arena's sets (A/B switch "rig", entrance_time)
  int light_variant = 0;                       // the camera-rotation convention for the light directions (probe: see entrance_info)
  std::vector<assets::LightSet> base_actor;  // the wrestler's own light sets as placed (the arena's are Viewer::arena_orig)
  double y_ref = 123, scene_base_t = 0;     // the root height of the first shown cue (the -Y-up reference); ent.t at a camera-path render base
  size_t arena_model = 0;
  double root[3] = {0, 0, 0};                  // the last applied root (motion space) and the ground it stands on
  float floor_y = 0;
  std::map<std::pair<int, int>, float> floor_cache;
  bool chain = false;                                      // "join":"absolute" (default, gen check 6) | "chain": see entrance.h ChainOffsets
  std::vector<std::array<double, 3>> off;                  // per cue root shift (zeros when absolute)
  // Session 98 (gen check 4): the title belt in the entrant's left hand ("belt":N = evtobj.pac PACH N, 0 = none). belt_model = its model.
  int belt = 0;
  int belt_model = -1;
  bool belt_waist = false;  // Session 98 (gen check 5): "title":"waist" = worn on Bip Spine (run 6821), else held in Bip L Hand (run 6820)
  std::string title = "none";  // the "title" asked for ("auto" kept as asked) and the bank played (entrance_title re-runs with them)
  int bank = 0, wrestler = 0;
  // the belt plays the entrance's own prop motion in its hand (entrance.h PropAttach; gen check 6: default on, "belt_motion":false = the
  // measured static pose of belt_pose.h)
  bool belt_motion = true;
  bool comp = false;      // at least one entrance companion actor is placed (Show::comps)
  // Session 98 (props general): the placed companions: actor id ("comp", "comp2", ..) and the index into Show::comps
  std::vector<std::pair<std::string, size_t>> comps;
  // Session 98 (gen check 6 / props general): the props of the entrance's prop acts (Show::props with an evtobj model). One model each.
  // Hand props: carried by the entrant for the title (item 9), else by the first companion when there is one [C: bank 416, run 6821
  // Ricardo] else the entrant [U]. World props: in the entrance's motion space (Show::PropAct::mode).
  struct Prop {
    size_t act = 0;        // index into Show::props
    int model = -1;        // viewer model index
    int pach = -1;
    bool belt = false;     // a title act (item 9) played by the chosen belt's model (BeltTwin)
    bool worn = false;     // Session 98 (cap / particles): a cap / hat model (asset_load.h PropWorn): a still-zero cue = on the head
    bool shown = false;   // drawn at the current frame (entrance_info)
    float at[3] = {0, 0, 0};  // a world prop's root at the current frame (viewer space, entrance_info)
  };
  std::vector<Prop> props;
  // Session 98 (attire props): the game's prop-id rewrite (evt_props.h): the keys used and every change, by act (entrance_info)
  int prop_key = -1;             // table 101's key (the performer's model id), -1 none
  std::string prop_key_src;      // "model directory" | "arg" | "inferred" | why none
  int title_key = -1;            // table 103's key (the held title's row), -1 none
  std::string title_key_src;
  struct Rewrite {
    int leaf = 0;
    uint32_t from = 0, to = 0;
    std::string by;              // "table 103" | "fixed" | "table 101" | "prop_swap"
  };
  std::vector<Rewrite> rewrites;
  std::map<size_t, uint32_t> played;  // Show::props index -> the id played (after the rewrites)
  std::string tables_err;             // evtdata.pac could not be read: no rewrites
  int title_props = 0;   // how many of `props` are title acts with a loaded belt model (UpdateBelt's static belt steps aside for them)
  bool props_on = true;   // "props":false leaves them out
  // Session 99 (announcer): the ring announcer (entrance.h Show::Announcer), actor id `ann_actor` ("" = none placed), his motion
  // segments on the scene clock (each from `from` on, until the next; `mo` = -1: not there), his microphone's model.
  struct AnnSeg {
    size_t cue = 0;              // the segment begins `off` motion frames into cue `cue` (resolved against `tl` every frame, so an
    double off = 0;              // intro_hold change in entrance_time moves it too); its motion's frame 0 = the same moment
    int mo = -1;                 // index into ann_mo
    bool loop = true;
  };
  std::string ann_actor;
  const void* ann_ptr = nullptr;  // the Actor this entrance placed (by identity: a user's "place" with the same id replaces it)
  int ann_id = 0;
  std::vector<assets::motion::Motion> ann_mo;  // placed in the entrance's motion space
  std::vector<AnnSeg> ann_seg;
  int ann_seg_cur = -2;                        // the segment whose motion the actor holds
  int ann_mic_model = -1;
  bool ann_mic_right = false;
};

// The model turn (radians, Renderer::SetTransform) that makes a model facing -Z face `yaw_deg`.
inline float TurnFor(float yaw_deg) { return (yaw_deg + 180.f) * 3.14159265358979f / 180.f; }

}  // namespace view
