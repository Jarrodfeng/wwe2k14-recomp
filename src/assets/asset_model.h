// Session 96 (the asset runtime, Jarrod 2026-11-08: "From scratch", "Census speed", "PC only", "part of the launcher ...
// a button in advanced"): the CPU-side form of a wrestler or an arena, read from the player's own game files at run time
// (CLAUDE.md rules 1 and 2: nothing from the game is shipped or committed). This header is the CONTRACT between the
// loaders (src\assets\asset_load.h: pac -> JBOY -> these structs) and the viewer (src\assets\view\: D3D11, cameras,
// shots, the command pipe; the program wwe2k14_assets.exe, started by the launcher's Advanced button). Formats: docs\systems\assets.md (from out\runtime\research_*.md, Session 95-96).
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace assets {

// One vertex as the viewer draws it (little-endian, host floats). Filled from the JBOY's planar streams:
// S0 position + normal + colour, S1 skin (bone slot, weight) pairs, S2 UV (first set), a lightmap UV (arenas), S3 tangent.
struct Vertex {
  float pos[3];
  float normal[3];
  float uv0[2];       // the first texture coordinate set
  float uv1[2];       // the lightmap set (arenas); 0 when the mesh has none
  float tangent[3];   // 0 when the mesh has none
  uint32_t color;     // RGBA8 as stored (0xFFFFFFFF when none)
  // skeleton NODE indices (the mesh's palette slot already mapped through its palette); 0 when unused. Session 99 (scene objects): 16
  // bits - Fandango's DLC stage set 2591 is a 659-node rig whose lamps hang from nodes past 255 (8 bits clamped them all to node 255).
  uint16_t bones[4];
  float weights[4];   // sum 1 for skinned vertices; {1,0,0,0} for rigid ones
};

// A DDS texture as stored (linear, little-endian): the viewer uploads `data` (all mips) in `format`.
enum class TexFormat : uint8_t { kUnknown, kBC1, kBC2, kBC3, kB4G4R4A4, kB8G8R8A8, kR8G8B8A8 };
struct Texture {
  std::string name;  // the name in its texture directory (e.g. "color", "rin_mat00")
  TexFormat format = TexFormat::kUnknown;
  uint32_t width = 0, height = 0, mips = 1;
  std::vector<uint8_t> data;  // mip 0 first, tightly packed
  bool live = false;          // Session 98 (tron video): the viewer rewrites this texture every video frame (a stage slot)
};

// What a mesh's material needs for a plain look: texture slots are indices into Model::textures, -1 = none.
struct Material {
  std::string shader;  // the material class name as stored ("yCh_skin", "yBG_Default", "yProp_BumpMap", ...)
  int diffuse = -1, normal = -1, lightmap = -1, mask = -1, specular = -1;
  std::string diffuse_name;  // Session 97: the diffuse sampler's texture name as stored (kept when the name is in no texture directory: stage movies)
  float ambient[4] = {1, 1, 1, 1};  // g_f4MatAmbCol when present
  float diffuse_color[4] = {1, 1, 1, 1};  // g_f4MatDifCol when present
  // Session 97 (look spec out/runtime/s97/look.md): the rest of the lighting recipe's per-material parameters.
  float light_limit[4] = {10, 10, 10, 1};  // g_fvLightLimit (cap on the summed light; the CTAB default)
  float spec_col[4] = {1, 1, 1, 1};        // g_f4SpecularCol (arena) / g_f4SpecCol (wrestler)
  float spec_lev = 0;                      // g_fSpecularLev / g_fSpecLev (0 = highlight off)
  float spec_pow = 300;                    // g_iSpecularPow / g_fSpecPow
  // Session 98 (skin fix): the wrestler parameters the game packs into the skin draw's constants [C: value match, probe run 6816 /
  // ch161 JBOY]: c142 = (g_fSpecLev, g_fSpecPow, g_fSpecLvBase, g_fSpecPwBase); c204 = (g_f4Ref_COL.rgb, g_f4Ref_DOT);
  // c205.xy = (g_fFairy, g_fFairyEdge); c155 = g_f3SSSSinCol, c158 = g_f3SSSSinCol2 (the skin glow; only the yCh_skin family has it).
  float spec_lv_base = 0, spec_pw_base = 1;
  float ref_col[4] = {0, 0, 0, 1};
  float fairy = 0, fairy_edge = 1;
  float sss_col[4] = {0, 0, 0, 1}, sss_col2[4] = {0, 0, 0, 1};
  bool has_sss = false;
  // Session 98 (wrestler fix 3): the gear shader's tiled detail (probe run 6816, gear draw 1405: c206 = (50, g_f4NoiseTBump, g_f4NoiseTDiff,
  // g_f4NoiseTSpec) = (50, .08, .08, .5), c208 = (0, ..TBumpF, ..TDiffF, ..TSpecF) = (0, .3, .3, .8), vertex c188 = (g_fNoiseTNear,
  // g_fNoiseTFar) = (250, 1000): the two sets are mixed by the clip depth). `detail` = g_mNoiseTilePrm (texture `noise2`, chCommon.pac).
  int detail = -1;
  float noise_near[3] = {0, 0, 0}, noise_far[3] = {0, 0, 0};  // bump (normal gain), diffuse gain, light (specular) gain
  float noise_dist[2] = {250, 1000};                          // g_fNoiseTNear / g_fNoiseTFar
  bool has_lm_slot = false;                // the mesh carries a sampLightMapPrm slot (= the game's "LM" shader family),
                                           // even when no texture is set: then the stage-wide lightmap is used
  bool alpha_blend = false;  // translucent classes (yCh_trans, yCh_Aniso_trans, ...)
  bool alpha_test = false;   // cut-outs (hair cards, crowd cards)
  bool cull_back = false;   // Session 98 (stage fix): the game draws this material with back faces culled (the stage movie surfaces, capture 6818)
  bool blend_inline = false;  // Session 98 (spot / beard pass): alpha-blended in the opaque pass's order with depth write on (the stage movie
                              // surfaces: capture 6818 draws them SrcAlpha / InvSrcAlpha with depth write, before the later layers)
  bool additive = false;     // S97: drawn added on top (arena slots flagged `a` in entry 50001: glows, beams, fog)
  bool crowd = false;        // S97: an audience member (the game's crowd vertex shader lights with the UNNORMALISED world normal: its
                             // length is the member's scale, 0.84-1.01, so the diffuse sum and the hemisphere blend scale with it)
};

struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;  // TRIANGLE LIST (the file's strips are converted; degenerate triangles dropped)
  int material = -1;              // index into Model::materials
  std::string part;               // which JBOY / slot it came from (e.g. "body", "hair", "slot 12"), for the census report
  int placement = -1;             // Session 97: index into Model::placements; -1 = drawn once as stored
  bool skinned = false;           // Session 97: vertices follow Model::skin (wrestler pieces); false = drawn as stored
  int light_set = -1;             // Session 97: index into Model::light_sets (arenas); -1 = the viewer's default rig
  // Session 98 (step 16, click to inspect): where the mesh came from, for the viewer's INSPECT panel and the friendly names.
  std::string node;     // the first node name of the piece's model ("ar_fence01_a"); empty when the piece has none
  std::string palette;  // the skin-palette bone names joined by ';' (wrestler pieces)
  int src_kind = 0;     // 1 arena (stage slot, ring set, stage screen), 2 wrestler piece, 3 crowd body, 4 prop (bgEtc leaf)
  std::string src_pac;  // "bg05.pac" / "ch107.pac" / "bgEtc.pac"
  int src_key = 0;      // the pac entry: slot / key; wrestler entry id (0 body, 10000+ hair ...); 20000+N crowd body; prop key
};

// Session 97: one arena light set (pac entry 30002/N): four directions TO the light, four colours, sky / ground ambient.
// Layout (784-byte kind): dirs vec4 at +0x10, colours at +0x50, sky at +0x100, ground at +0x110 (out/runtime/s97/look.md
// section 5). The 896-byte kind: dirs +0x10, colours +0x60, no ambient read (unconfirmed where it sits: left 0).
struct LightSet {
  int id = 0;  // the set number as stored (the `l(N)` of entry 50001)
  int edit_id = -2;  // Session 98 (lighting editor): the id whose edits apply (-2 = id); -1 = a fallback copy (the mesh's own set was missing / black): never edited
  float dir[4][3] = {};
  float col[4][3] = {};
  float sky[3] = {};
  float ground[3] = {};
  // Session 98 (skin fix; probe run 6816, out\s98\skin_fix\_model.py): the w of each stored colour = that light's weight in the wrestlers' entrance
  // specular lobes (c0..c3.w of the skin draw; arena 5 set 10 stores 0.6275 / 0.3529 / 1.451 / 0.2745 = probe run 6816 exactly).
  float wt[4] = {};
  // The wrestlers' highlight light (the skin draw's c4 colour / c12 direction): run 6816's entrant takes set 10's light 4 here.
  // It is NOT a diffuse light (the ambient cube is built from lights 0-3 only); it only feeds the edge highlight.
  float hl_dir[3] = {};
  float hl_col[3] = {};
  // Session 98 (replay S5): the colours the ambient cube is built from = the previous frame's (the game's yPreLight pass is fed the
  // last frame's c0..c3; renderer.cpp plCol). Set by the entrance rig; without it the cube uses `col`.
  bool has_pcol = false;
  float pcol[4][3] = {};
  // Session 98 (match oracle, out\s98\match_oracle\notes.md): each light's cube falloff = the yPreLight pass's c16..c19.x (side_prelight.frag:
  // keep_k = 1 - share x (1 - fall_k)). Stored at +0x140 of the 896-byte kind (arenas 0 / 5: sets 10 / 15 / 16 / 200 / 201 hold (0, 0, 0.5, 0),
  // the others 0) [C: probe run 6816's match prelight draw (frame 15841) c16..c19.x = (0, 0, 0.5, 0) = set 10's +0x140; run 071's match
  // cube (capture match_a) fits the PreLight of set 10 with it to rms 0.010 (ratio 0.992) vs 0.045 (ratio 0.955) without]. The entrance rig's
  // cubes are built with 0 [C: every entrance prelight draw of runs 6816 / 6824 / 071], so an evaluated rig set clears it.
  float fall[4] = {};
};
// Our default rig for models with no light set of their own (the wrestlers): the numbers of arena 5 set 121 (the
// commonest set of the arenas), read with out/runtime/s97/lightset_dump.py. Numbers only.
inline LightSet DefaultRig() {
  LightSet s;
  s.id = 121;
  const float d[4][3] = {{-0.02149f, 0.6153f, -0.788f}, {-0.7456f, 0.5618f, 0.3584f}, {0.6938f, 0.6247f, 0.3584f}, {-0.03294f, 0.6285f, -0.7771f}};
  for (int i = 0; i < 4; ++i) {
    for (int k = 0; k < 3; ++k) s.dir[i][k] = d[i][k];
    s.col[i][0] = s.col[i][1] = 0.451f, s.col[i][2] = 0.4784f;
  }
  s.sky[0] = s.sky[1] = s.sky[2] = s.ground[0] = s.ground[1] = s.ground[2] = 0.1961f;
  return s;
}

// Session 97: a 4x4 matrix, row-major, ROW vectors (p' = p * M, translation in elements 12..14): the game's own CPU
// convention (src\nf\palette_transpose.h, src\native_skin.h).
using Matrix = std::array<float, 16>;
inline Matrix Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
inline Matrix Mul(const Matrix& a, const Matrix& b) {  // a then b (row vectors: p * a * b)
  Matrix r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
  return r;
}

// Local transform from a rotation quaternion (x y z w, column-vector meaning) and a translation, in row-vector form.
inline Matrix FromQuatT(const float q[4], const float t[3]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  // column-form rotation R; row form stores R^T (row i = column i of R)
  const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
  const float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
  const float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
  return {r00, r10, r20, 0, r01, r11, r21, 0, r02, r12, r22, 0, t[0], t[1], t[2], 1};
}
// Quaternion of R = Rz(z) * Ry(y) * Rx(x) (column vectors, right-handed, radians): the node record's euler angles
// (Session 97 skeleton reader: this order beat the other 23 variants on Kane, 4.2 vs 53.1 mean joint distance).
inline void EulerXYZToQuat(const float e[3], float q[4]) {
  const float cx = std::cos(e[0] * 0.5f), sx = std::sin(e[0] * 0.5f), cy = std::cos(e[1] * 0.5f), sy = std::sin(e[1] * 0.5f),
              cz = std::cos(e[2] * 0.5f), sz = std::sin(e[2] * 0.5f);
  // qz * qy * qx
  q[3] = cz * cy * cx + sz * sy * sx;
  q[0] = cz * cy * sx - sz * sy * cx;
  q[1] = cz * sy * cx + sz * cy * sx;
  q[2] = sz * cy * cx - cz * sy * sx;
}
// Inverse of a rigid (rotation + translation) row-vector matrix.
inline Matrix InverseRigid(const Matrix& m) {
  Matrix r = Identity();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i * 4 + j] = m[j * 4 + i];
  for (int j = 0; j < 3; ++j) r[12 + j] = -(m[12] * r[j] + m[13] * r[4 + j] + m[14] * r[8 + j]);
  return r;
}
// The files are -Y up with the other handedness; the viewer negates Y (S = diag(1,-1,1)). A file-space transform M
// becomes S * M * S in viewer space: flip the signs of the elements that mix Y with X / Z.
inline Matrix MirrorY(Matrix m) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      if ((i == 1) != (j == 1)) m[i * 4 + j] = -m[i * 4 + j];
  return m;
}

struct Bone {
  std::string name;
  int parent = -1;
  // FILE space (Session 97: no longer Y-negated), parent-relative, as in the node record.
  float local_translation[3] = {0, 0, 0};
  float local_rotation[4] = {0, 0, 0, 1};  // quaternion x y z w from the record's euler angles
  Matrix bind_world = Identity();          // the bind pose in model space, VIEWER space (MirrorY applied)
  Matrix inv_bind = Identity();            // its inverse, viewer space
};

// Session 97: extra copies of some meshes (the crowd's people, repeated ring pieces): each mesh whose `placement` names
// an entry here is drawn once per transform (model space -> model space), instead of once as stored.
struct Placement {
  std::string label;  // e.g. "crowd body 20003", "ring post"
  std::vector<Matrix> transforms;
};

// A loaded wrestler (all JBOY pieces of one attire) or an arena (all stage slots of one bg pac).
struct Model {
  std::string label;  // "wrestler 107 attire 0", "arena 5"
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
  std::vector<Texture> textures;
  std::vector<Bone> bones;
  std::vector<Placement> placements;
  // Session 97: the skinning palette, one matrix per bone (pose_world * inv_bind); empty = every bone identity (the
  // stored bind pose). The viewer can replace it per frame (motions).
  std::vector<Matrix> skin;
  // Session 97: the light sets the meshes use (Mesh::light_set indexes this) and the stage-wide top-down lightmap
  // (arena texture `fade_lightmap00`; index into textures, -1 = none).
  std::vector<LightSet> light_sets;
  int stage_lightmap = -1;
  // Session 97: g_f4LightMapParam of the stage: xy = top-down scale of the stage lightmap (uv = world.xz * xy * 0.5 + 0.5),
  // gain = c184.z. The CTAB default is (1/3500, 1/3500, 1); the live value of arena 5 is (1/5600, 1/5750, 2.5).
  float lm_scale[2] = {1.f / 3500.f, 1.f / 3500.f};
  float lm_gain = 2.5f;
  float bounds_min[3] = {0, 0, 0}, bounds_max[3] = {0, 0, 0};
  // Anything odd the loader saw (a missing texture name, a stream that did not tile, NaN positions, ...). The census
  // reports these per item; an empty list means the loader found nothing wrong.
  std::vector<std::string> warnings;
  // Time spent reading + decoding (ms), for the census.
  double load_ms = 0;
};

}  // namespace assets
