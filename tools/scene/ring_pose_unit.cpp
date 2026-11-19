// ring_pose_unit: the offline proof of src\renderer_scene\ring_pose.h (S103 lane C4): the file's slot -> node map applied to a stored mesh, and the CPU skin with a call's rows. 0 game runs. Hand-worked oracles
// with literal numbers (never the code's constants). A plant is a fault in the INPUT, each must fail its NAMED check:
//   ring_pose_unit [--plant NAME]
//   slot_identity   the palette's slot -> node map is replaced by the identity (the game: palette[slot] - 1)  -> "pose.slot_to_node"
//   rigid_node_off  the rigid mesh's node is 1 more                                                           -> "pose.rigid"
//   mirror_off      the palette is not mirrored to the viewer's space (the game's rows used as they are)      -> "pose.mirror_space"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>
#include <string>

#include "../../src/renderer_scene/ring_gpu_skin.h"
#include "../../src/renderer_scene/ring_pose.h"

using namespace rdr_scene::rf::c;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-24s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}
static bool Near(float a, float b, float t = 1e-4f) { return std::fabs(a - b) <= t; }
static std::string Sci(double v) { char b[32]; std::snprintf(b, sizeof b, "%.3e", v); return b; }

static assets::Matrix Trans(float x, float y, float z) {
  assets::Matrix m = assets::Identity();
  m[12] = x, m[13] = y, m[14] = z;
  return m;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];

  // a 3-vertex mesh with skin pairs: palette (slot -> node) = {5, 2, 7} (1-based file numbers 6, 3, 8): vertex 0 = slot 0 (w 1), vertex 1 = slots 1 + 2 (w 0.25 + 0.25: normalised 0.5 / 0.5), vertex 2 = no pair.
  assets::Mesh m;
  m.vertices.resize(3);
  for (assets::Vertex& v : m.vertices) std::memset(&v, 0, sizeof v);
  m.vertices[0].pos[0] = 10, m.vertices[1].pos[0] = 20, m.vertices[2].pos[0] = 30;
  m.src_skin_pairs = 2;
  m.src_skin_slots = {0, 255, 255, 255, 1, 2, 255, 255, 255, 255, 255, 255};
  m.src_skin_weights = {1.f, 0.f, 0.f, 0.f, 0.25f, 0.25f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  m.palette_nodes = Plant("slot_identity") ? std::vector<uint16_t>{0, 1, 2} : std::vector<uint16_t>{5, 2, 7};
  const bool bound = BindMesh(m);
  const assets::Vertex& v0 = m.vertices[0];
  const assets::Vertex& v1 = m.vertices[1];
  Check(bound && v0.bones[0] == 5 && Near(v0.weights[0], 1.f) && v1.bones[0] == 2 && v1.bones[1] == 7 && Near(v1.weights[0], 0.5f) && Near(v1.weights[1], 0.5f) && m.vertices[2].bones[0] == 5, "pose.slot_to_node",
        "v0 node " + std::to_string(v0.bones[0]) + " v1 nodes " + std::to_string(v1.bones[0]) + "," + std::to_string(v1.bones[1]) + " w " + std::to_string(v1.weights[0]));
  Check(Near(v1.weights[0] + v1.weights[1], 1.f), "pose.weights", "sum " + std::to_string(v1.weights[0] + v1.weights[1]));   // 0.25 + 0.25 in the file: normalised to 1

  // the pose: node 5 moves +100 in x, node 2 moves +10 in y, node 7 moves +30 in y (viewer-space palette)
  std::vector<assets::Matrix> pal(8, assets::Identity());
  pal[5] = Trans(100, 0, 0), pal[2] = Trans(0, 10, 0), pal[7] = Trans(0, 30, 0);
  assets::Mesh out = m;
  PoseMesh(m, pal.data(), pal.size(), out);
  // v0 = (10, 0, 0) on node 5 -> (110, 0, 0); v1 = (20, 0, 0) on nodes 2 + 7 half each -> (20, 20, 0); v2 has no pair: the fallback node 5 -> (130, 0, 0)
  Check(Near(out.vertices[0].pos[0], 110.f) && Near(out.vertices[1].pos[1], 20.f) && Near(out.vertices[1].pos[0], 20.f) && Near(out.vertices[2].pos[0], 130.f), "pose.skin_values",
        "v0 " + std::to_string(out.vertices[0].pos[0]) + " v1 (" + std::to_string(out.vertices[1].pos[0]) + ", " + std::to_string(out.vertices[1].pos[1]) + ") v2 " + std::to_string(out.vertices[2].pos[0]));

  // a rigid mesh (no skin pairs) is bound to rigid_node: all vertices on node 3
  assets::Mesh r;
  r.vertices.resize(2);
  for (assets::Vertex& v : r.vertices) std::memset(&v, 0, sizeof v);
  r.vertices[0].pos[2] = 1, r.vertices[1].pos[2] = 2;
  r.rigid_node = Plant("rigid_node_off") ? 4 : 3;
  const bool rb = BindMesh(r);
  pal[3] = Trans(0, 0, 50);
  pal[4] = Trans(0, 0, 999);
  assets::Mesh ro = r;
  PoseMesh(r, pal.data(), pal.size(), ro);
  Check(rb && Near(ro.vertices[0].pos[2], 51.f) && Near(ro.vertices[1].pos[2], 52.f), "pose.rigid", "z " + std::to_string(ro.vertices[0].pos[2]) + " " + std::to_string(ro.vertices[1].pos[2]));

  // a node outside the palette keeps the stored place (no influence): vertex on node 9 of an 8-node palette
  assets::Mesh x;
  x.vertices.resize(1);
  std::memset(&x.vertices[0], 0, sizeof(assets::Vertex));
  x.vertices[0].pos[0] = 7, x.vertices[0].bones[0] = 9, x.vertices[0].weights[0] = 1.f;
  assets::Mesh xo = x;
  PoseMesh(x, pal.data(), pal.size(), xo);
  Check(Near(xo.vertices[0].pos[0], 7.f), "pose.outside_palette");

  // the viewer space: the game's row (translation y = +154 in the game's -Y-up space) becomes -154 in the viewer's (S M S); a vertex at the origin lands at the mirrored place. The record's 12 rows are VS c76..c78:
  // rows[3] = x, rows[7] = y, rows[11] = z translation (rows[4 r + c] = m[4 c + r]); the applier's WorldFromRows is rf_c's; here the same mirror by hand.
  {
    assets::Matrix g = Trans(0, 154, 0);
    assets::Matrix mv = Plant("mirror_off") ? g : assets::MirrorY(g);
    std::vector<assets::Matrix> p1(1, mv);
    assets::Mesh o1;
    o1.vertices.resize(1);
    std::memset(&o1.vertices[0], 0, sizeof(assets::Vertex));
    o1.vertices[0].weights[0] = 1.f;
    assets::Mesh o2 = o1;
    PoseMesh(o1, p1.data(), 1, o2);
    Check(Near(o2.vertices[0].pos[1], -154.f), "pose.mirror_space", "y " + std::to_string(o2.vertices[0].pos[1]));
  }
  // ---- S103 C5: the unchanged-pose skip and the pool names
  {
    float a[36], b[36];
    for (int i = 0; i < 36; ++i) a[i] = b[i] = float(i) * 0.5f;
    b[35] += 1.f;   // only the LAST float of the last node differs
    const size_t n = Plant("hash_short") ? 35 : 36;
    const uint64_t ha = HashRows(a, n, 7), hb = HashRows(b, n, 7);
    std::map<std::string, uint64_t> memo;
    const bool first = PoseNeeded(memo, "k#0", ha, true);    // nothing remembered: pose
    const bool same = PoseNeeded(memo, "k#0", ha, true);     // same rows, slot live: skip
    const bool dead = PoseNeeded(memo, "k#0", ha, false);    // slot not live (retired): pose again
    const bool moved = PoseNeeded(memo, "k#0", hb, true);    // rows changed: pose
    Check(ha != hb && first && !same && dead && moved, "pose.skip_unchanged", "first " + std::to_string(first) + " same " + std::to_string(same) + " dead " + std::to_string(dead) + " moved " + std::to_string(moved));
    // the names: the ASKED key (a 980 -> 960 fallback never shares a pool with a real 960), all under the set's prefix; dropping the old set's prefix leaves the new set's pools alone
    const int asked = 980, resolved = 960;
    const std::string n980 = PoolName("c:arena:5:aa", Plant("pool_by_resolved") ? resolved : asked), n960 = PoolName("c:arena:5:aa", 960), other = PoolName("c:arena:5:bb", 980);
    std::map<std::string, int> pools{{n980, 1}, {n960, 2}, {other, 3}};
    const std::string pre = PoolPrefix("c:arena:5:aa");
    for (auto it = pools.begin(); it != pools.end();) it = it->first.rfind(pre, 0) == 0 ? pools.erase(it) : std::next(it);
    Check(n980 != n960 && pools.size() == 1 && pools.count(other) == 1, "pose.pool_names", n980 + " vs " + n960 + ", left " + std::to_string(pools.size()));
    // S103 RGP review F2: the game's own rows have a high call k (asked 960) AND a low call k (asked 980) in one arena whose set lacks 980: Resolve maps both to model 960, idx is per ASKED model so both are k. The draw key
    // the kept lists and the blend use is (resolved model, Object::pad): the two must differ for every k, and no two instances of a model may share one (the old formula 1 + idx collided: low call k blended toward high call k).
    {
      std::set<std::pair<int, uint16_t>> keys;
      bool distinct = true;
      for (uint32_t k = 0; k < 40; ++k) {
        const int resolved = 960;
        const uint16_t hi = Plant("pad_old") ? uint16_t(1u + k) : RingInstancePad(k, false);   // asked 960 == resolved
        const uint16_t lo = Plant("pad_old") ? uint16_t(1u + k) : RingInstancePad(k, true);    // asked 980, resolved to 960
        distinct &= keys.insert({resolved, hi}).second;
        distinct &= keys.insert({resolved, lo}).second;
      }
      Check(distinct && RingInstancePad(32766u, true) != 0 && RingInstancePad(100000u, true) == RingInstancePad(32766u, true), "pose.instance_keys", "keys " + std::to_string(keys.size()) + " of 80");
    }
  }
  // ---- S103 RGP: the GPU-skinned ring (docs\research\s103\RGS.md option 1): the front end's chunk path + the game's skinned prop vertex stage (ring_gpu_skin.h repeats ch_skin in the shader's order) must put every vertex
  // where the CPU pose does, on meshes shaped like the ring's: rigid-per-vertex (one pair, weight 1: 1331 of 1429 ring meshes), the two-pair rope (palette 20 / 6), a rigid mesh with no pairs, a vertex whose node is past the
  // palette. The call's palette is built by RingPalette (the function rf_ring.cpp's GPU branch calls) from a node pool at a non-zero first_node. Plants (each must fail its NAMED check):
  //   gpu_slot_off     the chunk's slot -> node map is off by one slot                                    -> "gpu.matches_cpu"
  //   gpu_first_node   the GPU palette is read one node too late in the pool (the S103 cover base slip: STAGE_LIVE_WRITERS.md "Cause 1, covers": a base 65536 bytes off) -> "gpu.matches_cpu"
  //   gpu_weights_off  the 4th-bone weight rule is dropped (the residual 1 - w.x - w.y - w.z lost)        -> "gpu.two_pair_rope"
  {
    struct Node { float rows[12]; };
    uint32_t rs = 12345u;
    const auto rnd = [&rs]() { rs = rs * 1664525u + 1013904223u; return float(rs >> 8) / float(1u << 24); };   // [0, 1)
    std::vector<Node> pool(64);
    for (Node& nd : pool) {   // a random rotation (three axis turns) and a translation up to 300, as the VS c76..c78 rows: rows[4 k + 0..2] = the matrix's column k, rows[4 k + 3] = the translation
      const float ax = rnd() * 6.28f, ay = rnd() * 6.28f, az = rnd() * 6.28f;
      const float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
      const float R[3][3] = {{cy * cz, -cy * sz, sy}, {sx * sy * cz + cx * sz, -sx * sy * sz + cx * cz, -sx * cy}, {-cx * sy * cz + sx * sz, cx * sy * sz + sx * cz, cx * cy}};
      for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 3; ++c) nd.rows[4 * k + c] = R[k][c];
        nd.rows[4 * k + 3] = (rnd() - 0.5f) * 600.f;
      }
    }
    const uint32_t first_node = 7, n_nodes = 41;   // a call: nodes 0..40 of ITS range = pool 7..47
    std::vector<assets::Matrix> cpu_pal, gpu_pal;
    RingPalette(pool.data(), first_node, n_nodes, false, false, cpu_pal);
    RingPalette(pool.data(), first_node + (Plant("gpu_first_node") ? 1u : 0u), n_nodes, false, false, gpu_pal);

    const auto vertex_at = [&](assets::Vertex& v) { std::memset(&v, 0, sizeof v); for (int k = 0; k < 3; ++k) v.pos[k] = (rnd() - 0.5f) * 400.f, v.normal[k] = rnd() - 0.5f; v.tangent[0] = 1.f; };
    // rope: 600 vertices along a strip, 20 slots (the file's palette: a permutation of nodes), each vertex two neighbouring slots, weights summing to 1 (the file's own); triangles between neighbouring vertices
    assets::Mesh rope;
    rope.vertices.resize(600);
    rope.src_skin_pairs = 2;
    rope.src_skin_slots.assign(600 * 4, 255);
    rope.src_skin_weights.assign(600 * 4, 0.f);
    for (size_t s = 0; s < 20; ++s) rope.palette_nodes.push_back(uint16_t((s * 7 + 3) % n_nodes));
    for (size_t i = 0; i < 600; ++i) {
      vertex_at(rope.vertices[i]);
      const size_t s = std::min<size_t>(18, i * 19 / 600);
      const float w = 0.1f + 0.8f * rnd();
      rope.src_skin_slots[i * 4] = uint8_t(s), rope.src_skin_slots[i * 4 + 1] = uint8_t(s + 1);
      rope.src_skin_weights[i * 4] = w, rope.src_skin_weights[i * 4 + 1] = 1.f - w;
    }
    for (uint32_t i = 0; i + 2 < 600; ++i) rope.indices.insert(rope.indices.end(), {i, i + 1, i + 2});
    // rigid-per-vertex: 300 vertices, 6 slots, one pair at weight 1; triangles inside a slot group
    assets::Mesh rig;
    rig.vertices.resize(300);
    rig.src_skin_pairs = 1;
    rig.src_skin_slots.assign(300 * 4, 255);
    rig.src_skin_weights.assign(300 * 4, 0.f);
    for (size_t s = 0; s < 6; ++s) rig.palette_nodes.push_back(uint16_t(30 - 5 * s));
    for (size_t i = 0; i < 300; ++i) {
      vertex_at(rig.vertices[i]);
      rig.src_skin_slots[i * 4] = uint8_t(i / 50), rig.src_skin_weights[i * 4] = 1.f;
    }
    for (uint32_t g = 0; g < 6; ++g)
      for (uint32_t i = 0; i < 48; ++i) rig.indices.insert(rig.indices.end(), {g * 50 + i, g * 50 + i + 1, g * 50 + i + 2});
    // rigid: no pairs, the mesh's node
    assets::Mesh rg;
    rg.vertices.resize(40);
    rg.rigid_node = 12;
    for (assets::Vertex& v : rg.vertices) vertex_at(v);
    for (uint32_t i = 0; i < 38; ++i) rg.indices.insert(rg.indices.end(), {i, i + 1, i + 2});
    // a vertex whose node is past the call's palette (node 50 of a 41-node palette): both paths keep the stored place
    assets::Mesh out_of;
    out_of.vertices.resize(3);
    for (assets::Vertex& v : out_of.vertices) vertex_at(v), v.bones[0] = 50, v.weights[0] = 1.f;
    out_of.indices = {0, 1, 2};
    const bool bound = BindMesh(rope) && BindMesh(rig) && BindMesh(rg);
    for (assets::Vertex& v : out_of.vertices) v.bones[0] = 50, v.weights[0] = 1.f;   // (no bind: the node is the test input)
    if (Plant("gpu_weights_off")) for (assets::Vertex& v : rope.vertices) v.weights[1] = v.weights[1] * 0.5f;   // the second weight no longer completes 1: the residual must land on the first bone (the shader's rule)
    double worst = 0.0, worst_one = 0.0, worst_rope = 0.0;
    bool cut_ok = true;
    for (const assets::Mesh* mp : {&rope, &rig, &rg, &out_of}) {
      assets::Mesh cpu = *mp;
      PoseMesh(*mp, cpu_pal.data(), cpu_pal.size(), cpu);
      std::vector<float> gpu;
      std::vector<char> done;
      const bool ok = GpuSkinMesh(*mp, gpu_pal.data(), gpu_pal.size(), gpu, done, Plant("gpu_slot_off"));
      cut_ok = cut_ok && ok;
      double w = 0.0;
      for (size_t v = 0; ok && v < mp->vertices.size(); ++v) {
        if (!done[v]) continue;
        for (int k = 0; k < 3; ++k) w = std::max(w, std::fabs(double(gpu[3 * v + size_t(k)]) - double(cpu.vertices[v].pos[k])));
      }
      if (mp == &rope) worst_rope = w;
      if (mp == &rig) worst_one = w;
      worst = std::max(worst, w);
    }
    // PoseMesh normalises a rope vertex by its weight sum; the plant breaks the file's sum, so the rope compare is where it shows (the shader puts the residual on bone 0, the CPU divides)
    Check(bound && cut_ok && worst < 1e-3, "gpu.matches_cpu", "max |p_gpu - p_cpu| " + Sci(worst) + " over rope / rigid-per-vertex / rigid / outside-palette meshes");
    Check(worst_one < 1e-3, "gpu.one_pair", "rigid-per-vertex max " + Sci(worst_one));
    Check(worst_rope < 1e-3, "gpu.two_pair_rope", "rope max " + Sci(worst_rope));
    // the chunk cut: a mesh with a 20-slot palette fits one chunk (kMaxPaletteBones 21), every vertex keeps its pairs
    std::vector<rdr_scene::SkinChunk> chunks;
    const bool cut = rdr_scene::BuildSkinChunks(rope, chunks);
    Check(cut && chunks.size() >= 1 && chunks.front().nodes.size() <= size_t(rdr_scene::kMaxPaletteBones), "gpu.chunk_cut", "rope: " + std::to_string(chunks.size()) + " chunk(s), first with " + std::to_string(chunks.empty() ? 0 : chunks.front().nodes.size()) + " slots");
  }
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
