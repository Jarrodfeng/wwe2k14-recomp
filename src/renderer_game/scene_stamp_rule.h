// S103 lane FLAT: which scene-hook asks may make a frame a 3D frame (the Scene3D stamp), stream-free (OFF) build only. Pure, header-only: no includes.
// Cause [C: run 004 log, docs\research\s103\CV_stream_free_coverage.md]: the menu backdrop's particle node (sub_82254E30) runs every menu frame and asked scene_skip::Ours() first, so the first ask
// of the frame took the Scene3D decision (scene_frame::Decide) and stamped the frame NotReady. A stamped frame is "not flat" (scene_kind.h IsFlatFrame), so no menu / load / results frame was ever flat
// and, after a match, each one held the last arena picture (host_core.h R3 Hold3D) under its 2D rows.
// The game's own terms [C: claims_MP MP6, particle_source.h]: the node takes its view-projection from the calling thread's current camera object (camera table 0x83929A40, sub_82BA7748 ->
// sub_82BA8780 -> sub_82296630). On a menu that camera is the 2D screen camera; a 3D scene walk calls it with the scene camera. The node also returns before drawing when [node+3084] or
// [node+3088] is 0 or the camera object is null (0x82254E3C / 0x82254E4C / 0x82254E68): a call that draws nothing is no evidence of a 3D walk either.
// So only a call that DRAWS with a NON-screen camera may ask the scene decision. Any other call is answered "the game walks" without deciding (the same answer a not-READY menu frame got before
// the first match: the original runs). The stream build never uses this (the hook asks as before).
#pragma once

namespace scene_stamp_rule {

#ifdef WWE2K14_STAMP_PLANTS
inline int g_plant = 0;   // 1 = menu_hook_is_3d: the old rule (every particle call asks, so a menu stamps its frame as 3D)
#define STAMP_PLANT(n) (::scene_stamp_rule::g_plant == (n))
#else
#define STAMP_PLANT(n) false
#endif

// have_camera = the node's camera object was readable and non-null; screen_camera = its view-projection is the 2D screen camera; draws_nothing = the node returns before drawing (zero word / null camera).
// An unreadable camera is NOT proof of a menu: it keeps asking (as before).
inline bool ParticleCallAsksScene(bool have_camera, bool screen_camera, bool draws_nothing) {
  if (STAMP_PLANT(1)) return true;
  if (draws_nothing) return false;
  if (have_camera && screen_camera) return false;
  return true;
}

}  // namespace scene_stamp_rule
