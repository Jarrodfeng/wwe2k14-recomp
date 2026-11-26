// The particle tables of the scene record (Session 103, lane F; record section ids 54 and 55, FRAME_CONTRACT.md section 8; docs\renderer\SEE_THROUGH_ORDER.md
// section 5 for the layout and why). Pure data: no renderer, no game. Our StageHook (live_particles.h) draws them at Stage::AfterBlended; the contributor
// (src\scene_live\particle_contrib.h) writes them.
//
// A batch = one run of particles that share everything the draw state needs (the game's runs: PARTICLES_PROMOTION_MAP.md); a particle = what the game's
// particle draw methods hand the effect yParticle per sprite: four vertices (position, colour, two uv pairs: the layout of stride 32 the census measured,
// [C] tools\particles\census.py "shape") and the sprite's LocalWorld rows (VS c228-c230; c231 is (0, 0, 0, 1) in every draw measured). Positions are in the
// sprite's local space; the effect multiplies by LocalWorld and the frame's ViewProj. Everything is in the GAME's space (viewer_space = false).
#pragma once
#include <cstdint>
#include <vector>

namespace rdr_scene {

inline constexpr uint32_t kScParticleBatches = 54;  // S103 F: ParticleBatch[]. Optional; absent = no particles (an older record)
inline constexpr uint32_t kScParticles = 55;        // S103 F: Particle[] (the batches index it). 54-57 are lane F's (49-53 are lane E's); 56 and 57 are reserved, unused

inline constexpr uint32_t kScParticleStatus = 56;  // S103 F2: ParticleStatus[0 or 1]: why this frame's particle pass is not whole (B2). Absent = whole / none recorded

inline constexpr uint32_t kScParticleScreen = 57;  // S103 MP: ParticleScreen[0 or 1]: the node's own 2D screen camera on a flat frame (menus), and how many node calls the frame made. Absent = the scene camera (a match)

inline constexpr uint32_t kNoParticleTexture = 0xFFFFFFFFu;

enum ParticleBlend : uint8_t { kParticleAlpha = 0, kParticleAdditive = 1, kParticleReverseSubtract = 2 };  // S103 F5: 2 = SrcAlpha / One with the Xenos op 4 = Vulkan REVERSE_SUBTRACT 2 in rdr::BlendState (the blend setter's code 2, listing sub_82C23168)
enum ParticleSite : uint8_t { kParticleSprite = 0, kParticleQuad = 1 };  // the game's two draw methods: camera-facing sprites / oriented quads (0x82A0DA58 / 0x82A0E338)

struct ParticleVertex {
  float pos[3];      // local space
  uint32_t color;    // RGBA8, byte 0 = red (the vertex attribute is R8G8B8A8_UNORM, as the stream's)
  float uv[2];       // the sampled pair
  float uv2[2];      // the second pair (read by no effect of ours; kept so the row is the whole vertex)
};
static_assert(sizeof(ParticleVertex) == 32, "the particle vertex is the census's stride 32");

struct Particle {
  float local_world[12];  // VS c228, c229, c230: four floats each, as the constants hold them (row k = column k of the row-vector matrix)
  ParticleVertex v[4];    // a triangle strip of four
};
static_assert(sizeof(Particle) == 48 + 128, "Particle is a flat POD");

struct ParticleBatch {
  uint32_t first = 0, count = 0;  // into kScParticles
  uint32_t texture = kNoParticleTexture;  // index into kScAssets (an AssetRef naming the particle texture: pac + entry); kNoParticleTexture = a flat white one
  uint8_t blend = kParticleAlpha;  // ParticleBlend
  uint8_t soft = 0;                // 0 = hard; else the game drew it soft (ySoftParticle, reads scene depth) and the code is the range * 4 (S103 PRT: lround, 1..255: Kane 30 -> 120): live_particles.cpp draws the soft program with range = soft / 4
  uint8_t site = kParticleSprite;  // ParticleSite (informational: the vertices are already placed)
  uint8_t addr = 0;                // S103 F3: the sampler-address byte of the item's 28-byte record + 1 (0 = not set); what it means is [U], the draw still clamps (was pad)
  float zoffset = 0;               // VS c200.x: depth pulled towards the camera (0 in every draw measured)
  float use_normal = 0;            // VS c201.x: below 0.9 flat brightness; otherwise lit
  float light[4] = {0, 0, 0, 0};   // VS c202: the light vector
  uint32_t frame = 0;              // the guest frame (low 32 bits) the contributor captured this batch in: the hook draws a batch only for the record's own frame
  uint32_t row_after_p1 = 0;       // S103 MP (was reserved, always 0): the 2D row count when the node call that made this batch started, + 1 (0 = not noted: a match batch). A flat drawer draws the batch after that 2D row
  uint8_t depth_always = 0;        // S111 PARTZ: 1 = the game drew this entry's particles with depth compare ALWAYS (entry word +220 == 0, sub_82A0AED8 +84); 0 = its reversed GEQUAL
  uint8_t reserved_pz[3] = {0, 0, 0};
};
static_assert(sizeof(ParticleBatch) == 52, "ParticleBatch is a flat POD");

// Why the frame's particle pass is NOT WHOLE (FRAME_CONTRACT 3: a pass that ran but could not be placed is a missing input: the stamp is withheld, and an already-OURS frame is a hole
// the render side reports). `reasons` is a bitmask; the counts say how many. Whole = reasons == 0.
enum ParticleIncomplete : uint32_t {
  kPiNoCamera = 1,      // the scene camera could not be read: no sprite could be placed
  kPiUnreadable = 2,    // a node-level or partial guest read failed
  kPiOtherClass = 4,    // an item of a class the reader does not know (quads / the third method)
  kPiPivot = 8,         // an item whose pivot code is not a centred one (drawn centred, approximate)
  kPiDropped = 16,      // sprites dropped at the per-frame cap
  kPiMalformed = 32,    // the tables read back malformed and were dropped whole
  kPiTexture = 64,      // S103 F3: a key's texture could not be named (no namer, or it found no file for the handle): drawn with the white stand-in
  kPiBlend = 128,       // S103 F3: the item's blend code was read but what 0 / 1 / 2 mean is not confirmed by a run
  kPiEmptyPass = 256,   // S103 MP: INFO, not a reason: this frame's node did not draw (no call in the frame, or its +3084 / +3088 word is 0: the node returns at 0x82254E3C / 0x82254E4C). A whole, empty pass: Parts::Whole for a flat frame
  kPiScreenCam = 512,   // S103 MP: a reason: the batches were drawn by the node with its 2D screen camera (the basis is assumed screen-aligned [U]) and the flat drawer for them is not built: Held
};
inline constexpr uint32_t kPiMultiNode = 1024;  // S103 MPBF: a reason: more than one DISTINCT node object drew in the frame (the source reads only the last one)
inline constexpr uint32_t kPiInfoMask =kPiPivot | kPiEmptyPass;  // bits that never make a pass not whole
struct ParticleStatus {
  uint32_t reasons = 0;
  uint32_t unreadable = 0, other_class = 0, pivot_not_centred = 0;
};
static_assert(sizeof(ParticleStatus) == 16, "ParticleStatus is a flat POD");

// S103 MP: the camera the node drew a flat frame's particles with (its own inputs: the camera table's object for the calling thread, sub_82296630(out, cam+176, cam+240) = +240 . +176; read at the entry hook),
// read at the node's entry hook. `calls` = node calls the frame made; `row_count` = the frame's 2D rows at the last call.
struct ParticleScreen {
  float view_proj[16] = {};
  uint32_t calls = 0, row_count = 0, reserved[2] = {0, 0};
};
static_assert(sizeof(ParticleScreen) == 64 + 16, "ParticleScreen is a flat POD");

struct ParticleTables {
  std::vector<ParticleBatch> batches;
  std::vector<Particle> particles;
  std::vector<ParticleStatus> status;  // 0 or 1 rows (kScParticleStatus)
  std::vector<ParticleScreen> screen;  // 0 or 1 rows (kScParticleScreen, S103 MP)
  bool empty() const { return batches.empty(); }
  uint32_t Incomplete() const { return status.empty() ? 0u : (status[0].reasons & ~uint32_t(kPiInfoMask)); }
  bool EmptyPass() const { return !status.empty() && (status[0].reasons & kPiEmptyPass) != 0; }
  void Clear() { batches.clear(), particles.clear(), status.clear(), screen.clear(); }
  size_t Bytes() const { return batches.size() * sizeof(ParticleBatch) + particles.size() * sizeof(Particle) + status.size() * sizeof(ParticleStatus) + screen.size() * sizeof(ParticleScreen); }  // the case ring's size estimate
  // The tables are well formed: every batch indexes inside the particle list, batches do not overlap, and no batch is empty.
  bool Valid(const char** why = nullptr) const {
    uint64_t next = 0;
    for (const ParticleBatch& b : batches) {
      if (b.count == 0 || b.first != next || uint64_t(b.first) + b.count > particles.size()) { if (why) *why = "a batch is empty, out of order or past the particle list"; return false; }
      next = uint64_t(b.first) + b.count;
    }
    if (next != particles.size()) { if (why) *why = "particles not covered by a batch"; return false; }
    return true;
  }
};

}  // namespace rdr_scene
