// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {


// lane 139 (SH3 slice 2, docs/renderer/SHADOW_PLANE.md): the plane shadow chain's constants as the game uploads them (ids 18 / 19 are in the SceneSectionId enum). Two optional sections:
// kScPlaneFrames = PlaneFrame[] (one per guest frame the plane module drew: the module's own constants, shared by every draw of the frame) and kScPlaneDraws = PlaneDraw[] (one per plane-chain draw:
// the per-draw part, and for the blur the whole blur block). Both versioned (kPlaneLayoutVersion), both stamped with the hook's guest frame; a consumer joins by (stamp, kind, object, part, nth) and
// never by the record's index. Fields are NAMED by what they are (rule 6); the hook maps them to the 360's registers (plane_shadow_core.h). A value the writer could not see is not "present".
inline constexpr uint32_t kPlaneLayoutVersion = 1;
enum PlaneKind : uint32_t { kPlaneMask = 0, kPlaneBlur = 1, kPlaneProxy = 2 };   // kPlaneProxy (S103 A3, S8): one row per entry of the plane node's proxy list copy (object only; kind decode / plane position [U]); consumers of the walk's mask / blur rows never see it
enum PlaneFrameHave : uint32_t {   // PlaneFrame::have: bit set = the value was uploaded THIS frame (cleared per frame; never "the last frame's")
  kPlaneHaveFade = 1, kPlaneHaveNormal = 2, kPlaneHaveFit = 4, kPlaneHaveCamera = 8, kPlaneHaveAll = 15,
};
enum PlaneFlags : uint32_t {
  kPlaneFromLatch = 0x100,    // S103 A3 (S8): a PlaneDraw row built at the latch from the proxy list copy (plane_proxy.h), not recorded by the walk; plane_contrib.h drops it when the walk recorded rows of the same frame
  kPlaneNoDraw = 0x200,       // S103 PB: sub_827ED960's x pair collapsed (the game's own 2/0): the plane covers no screen pixel; fit_matrix holds the game's raw inf and kPlaneHaveFit is clear
  kPlaneCameraIsColour = 1,   // set by a reader that checked the camera bit-equal to the colour pass's (the writer cannot see the colour pass; the record's Camera has no bit-equal combined matrix)
};
struct PlaneFrame {
  uint32_t version = kPlaneLayoutVersion;
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits)
  uint32_t flags = 0;            // PlaneFlags
  uint32_t have = 0;             // PlaneFrameHave
  float fade[10] = {};           // the module's distance / fade array: 500, 300, 40, 1.5, 0.5, 0.35, 0.21, 1.0, 0.0, then the screen-fit weight (module+68..+107; the weight is module+104)
  float plane_normal[4] = {};    // the plane's normal (0, -1, 0, 0)
  float fit_matrix[16] = {};     // the per-frame screen fit of the plane (module+1312), row-major as uploaded
  float camera[16] = {};         // the main camera's view-projection (module+1072), row-major as uploaded
};
static_assert(sizeof(PlaneFrame) == 16 + 40 + 16 + 64 + 64, "PlaneFrame is a flat POD table");
enum PlaneDrawHave : uint32_t { kPlaneDrawHavePos = 1, kPlaneDrawHaveBlur = 2 };   // blur: scale + weight + matrix + point all uploaded this frame
struct PlaneDraw {
  uint32_t version = kPlaneLayoutVersion;
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) when the draw was issued
  uint32_t kind = 0;             // PlaneKind
  uint32_t flags = 0;
  uint32_t object = 0;           // mask: the model object (DrawOrigin::object); blur: 0
  uint32_t part = 0;             // mask: the part index; blur: 0
  uint32_t have = 0;             // PlaneDrawHave
  uint32_t ordinal = 0;          // the draw's sequence number in its frame (both kinds): the identity a record delivered twice keeps
  float plane_pos[4] = {};       // mask: the plane's position (0, -120, 0, 0), uploaded per proxy before its draws
  float blur_scale[4] = {};      // blur: the first blur row (x = module+96)
  float blur_weight[4] = {};     // blur: the weight row (x = module+104)
  float blur_matrix[16] = {};    // blur: camera x fit, transposed, as uploaded
  float blur_point[4] = {};      // blur: the fit-point row from the table at 0x83625ADC (640, 0, 640, 1)
};
static_assert(sizeof(PlaneDraw) == 32 + 16 + 16 + 16 + 64 + 16, "PlaneDraw is a flat POD table");
// S103 E5 (lane E's range 49-53; docs\renderer\LIVE_MAPS.md "Lane E5"): two optional latch sections.
//  49 kScPlaneEyes = PlaneEyes[] (one per frame the latch could read the plane module's per-arena tables): the plane mask's EyePos (VS c72) PER PROXY KIND, recomputed from the module's inputs
//     exactly as sub_827ED6D0 does (vector at module+768+16 kind, angle at module+896+4 kind, rotated about y by angle * pi/180 - pi, the result stored at module+992..1000), and PlanePos (c7) = (0, [module+1440], 0, 0).
//     Kinds: 1 = the wrestler proxy, 4..7 = the ring proxy's four points. `have` bit k = eye[k] was recomputed (a kind whose table was not loaded stays 0 and its bit clear: never a guess).
//  50 kScPartClocks = PartClock[] (one per RUN of parts of a stage model with the same clock): the UV-scroll clock VS c186 `Time` = the part's time word [[model+484] + 24 part + 12], which sub_82BD5A98
//     advances by [model+248] every update and wraps at the part's period word (+16). A consumer asks for (object, part) and takes the row with the largest `part` <= the asked one.
inline constexpr uint32_t kScPlaneEyes = 49;
inline constexpr uint32_t kPlaneEyesLayout = 1;
enum PlaneEyeKind : uint32_t { kPeWrestler = 1, kPeRing0 = 4, kPeRing3 = 7, kPeKinds = 8 };
enum PlaneEyesFlags : uint32_t { kPeHavePos = 1 };   // plane_pos was read
struct PlaneEyes {
  uint32_t version = kPlaneEyesLayout;
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) the latch read the module at
  uint32_t have = 0;             // bit k = eye[k] valid
  uint32_t flags = 0;            // PlaneEyesFlags
  float eye[kPeKinds][4] = {};   // EyePos per module kind (x, y, z, 1)
  float plane_pos[4] = {};       // PlanePos (c7): (0, [module+1440], 0, 0)
};
static_assert(sizeof(PlaneEyes) == 16 + 128 + 16, "PlaneEyes is a flat POD table");

}  // namespace rdr_scene
