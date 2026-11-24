// Stage hooks and live targets: how a class plugs its own draws into our frame graph (Session 103, lane S;
// docs\renderer\FRAME_CONTRACT.md sections 5 and 8). The scene front-end calls every hook of SceneFrame::stage_hooks at each stage
// boundary, in list order, with the frame's builder. A hook records only what the front-end's generic material path cannot draw
// (maps, instancing, particles, screens); it never begins, finishes or waits on a frame.
#pragma once
#include <cstdint>
#include <vector>

#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"

namespace rdr_scene {

struct SceneFrame;

enum class Stage : uint8_t {
  Maps = 0,          // before the colour targets are set: shadow maps, plane, circles, rim, pre-light, sweat (lane E)
  BeforeOpaque = 1,  // colour targets set and cleared; nothing drawn yet
  AfterOpaque = 2,   // every opaque mesh drawn
  AfterBlended = 3,  // the see-through list drawn (far to near): particles, flashes, beams go here (lanes F, C)
  BeforePost = 4,    // the scene's colour + depth are final; the post pass follows
  AfterPost = 5,     // S110 CUTGREY: the post pass is recorded; `StageCtx::color` is its OUTPUT (the post output, or the scene colour of a no-post frame). The crowd flashes are drawn onto it just before this stage
};

// Where FrontEnd::Record draws. Images owned by the caller (live: the render thread's scene images; offline: the front-end's own).
struct LiveTargets {
  rdr::ImageId color = rdr::kNoImage;  // RGBA8 (or the caller's scene format), width x height
  rdr::ImageId depth = rdr::kNoImage;
  uint32_t width = 0, height = 0;
  bool clear = true;                   // clear colour + depth before the first draw
};

struct StageCtx {
  const SceneFrame* frame = nullptr;
  const float* view_proj = nullptr;    // 16 floats, row vectors (clip = p * W * VP)
  const float* eye = nullptr;          // 3 floats
  rdr::ImageId color = rdr::kNoImage, depth = rdr::kNoImage;
  uint32_t width = 0, height = 0;
  rdr::Renderer* renderer = nullptr;
};

class StageHook {
 public:
  virtual ~StageHook() = default;
  virtual const char* Name() const = 0;
  virtual void Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& builder) = 0;
};

// Lane E's outputs, handed to the colour pass by ROLE (never through the stream's copies). The effects that read a map (FX_SHADOW*, the
// rim map slot 15, the pre-light cube, the sweat map) take it from here; a role with no entry keeps today's stand-in.
enum class MapRole : uint32_t {
  None = 0,
  ShadowMap = 1,     // the wrestlers' / casters' shadow map (matrix = its light view-projection)
  PlaneShadow = 2,   // the plane shadow mask
  Circles = 3,       // the circle blob shadows
  RimMap = 4,        // the screen-space rim light map
  PreLightCube = 5,  // a pre-light cube (params[0] = the light set id it was baked for)
  SweatMap = 6,      // the per-wrestler sweat map (params[0] = the object id hash)
  PropsMap = 7,      // S103 PMS: the game's props map as the shadowed FLOOR reads it (yBG_* FX_SHADOW, slot 12): the circle blobs in the blue channel (512 RGBA8, cleared white); matrix / prm below
};
// S103 E: matrix by role, both 16 floats in REGISTER layout (4 vec4 rows as the 360's constant file holds them): ShadowMap = TexProjLight (VS c68-c71, bias and half texel INCLUDED; params = width,
// height, range start, range end); Circles = the RAW light ViewProj (VS c4-c7, no bias; params = width, height).
// S103 PMS: PropsMap = the floor's TexProjLight in REGISTER layout (VS c68-c71: bias(w, h) applied to the circle entry's c4-c7 = Trapezoidal x ViewProj of the props light, props_map_rows.h), params = (width, height),
// prm = PS c168-c170 from the colour-call light state that holds the props camera (a frame with no such state publishes no PropsMap: a counted hole), raw = false (the blobs have no chain).
// S103 E2: RimMap: matrix = the c56-c59 rows (register layout) of the rim pass's main camera = the TRANSPOSED CasterExtra::product, params = (width, height, rim near, rim far), the image 512x256 RGBA8
// (red 0, green = the depth fade). PlaneShadow: matrix = PlaneInv x CameraViewProj (the screen-fitted mask space), params = (512, 512, plane y), RGBA8, red = darkness. PreLightCube: the image is the
// 384x64 STRIP of one slot (six 64x64 faces side by side, tile j = cube layer j), params = (light set id, 384, 64). All three are RAW (`raw` = true): rim without the coverage reductions and blur, plane
// without the downscale and the two blurs, pre-light as a strip not a cube. A reader that wants the game's final texture must check `raw` and keep its stand-in until the chain exists; nothing may bind a raw map as the final one.
struct MapImage {
  MapRole role = MapRole::None;
  rdr::ImageId image = rdr::kNoImage;
  float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float params[4] = {0, 0, 0, 0};
  bool raw = false;   // S103 E2: see above. S103 L6: the ShadowMap is raw too (no blur / min-pair chain yet): true for this producer
  float prm[3][4] = {};   // S103 L6: ShadowMap only: PS c168-c170 (ShadowPrm) from the light state the map was made with (CasterRows::shadow_prm); a reader copies all three rows
};
struct LiveMaps {
  std::vector<MapImage> maps;
  const MapImage* Find(MapRole role, float param0 = 0.f, bool match_param = false) const {
    for (const MapImage& m : maps)
      if (m.role == role && (!match_param || m.params[0] == param0)) return &m;
    return nullptr;
  }
};

}  // namespace rdr_scene
