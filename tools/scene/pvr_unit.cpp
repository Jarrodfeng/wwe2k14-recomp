// body-scale: none (front-end unit, no wrestler is drawn)  [S103 RGF: body_scale_feed_lint.py]
// S103 PVR unit: the per-frame posed vertices (a re-posed model: FrontEnd::UpdateModelVertices, the path of ModelCache::PosedModel = ring pieces and posed weapons; a CPU-posed cloth / skinned piece:
// FrontEnd::Record's cloth block) reach the GPU through the frame's geometry ring (rdr_scene::g_posed_ring) instead of a staged vkCmdCopyBuffer per mesh. Synthetic models drawn in the debug colour
// (no game files, no effect), re-posed every frame, rendered with the ring and with the old path. Needs a renderer (Vulkan); 0 game runs.
// S103 PF2 adds the DIRECT path (a unified-memory device writes the pose straight into a mapped copy of the model's vertex buffer, front_end.h g_posed_direct): this card is discrete, so the unit forces it
// (g_posed_direct_force: the copies live in host-visible memory the GPU reads over PCIe). Checks: the picture of every frame identical to the old path AND the ring path, 0 GPU buffer copies, 0 ring pushes, one mapped write per
// posed mesh, no fallback and no write into a copy a frame in flight may read (the audit), a model at rest writes nothing, two objects of one model.
//   pvr_unit.exe [--plant N]   N: 1 = every ring offset one vertex off, 2 = a model reads the previous frame's ring offset, 3 = a cloth piece reuses the previous Record's offset (each must make the picture differ);
//                              PF2: 8 = the tween loop renders each recorded frame 3 times (more than rdr::kRendersPerRecord: the renderer must count an overrun); 5 = ONE copy per model (every re-pose finds it in flight: detected, counted as fallbacks), 6 = one copy and the in-flight check left out (the audit counts the write), 7 = the draws bind the wrong copy
// Prints `pvr_unit ...` lines; exit 0 = every check passed (with --plant: exit 0 only when the fault was caught).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer/problem.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/record_io.h"

using namespace rdr_scene;

namespace {

constexpr uint32_t W = 96, H = 96;
constexpr size_t kBigVerts = 200000;    // 200k x 80 bytes = 16 MB: over kPosedRingCap (12 MB) on its own
constexpr size_t kMidVerts = 50000;     // 4 MB: fits the cap, not the room a 14 MB + 6 MB frame leaves

assets::Mesh Tri(float cx, float cy, size_t extra, bool skinned = false) {
  assets::Mesh me;
  me.part = "tri";
  me.vertices.assign(3 + extra, assets::Vertex{});
  const float px[3] = {cx - 0.2f, cx + 0.2f, cx}, py[3] = {cy - 0.2f, cy - 0.2f, cy + 0.25f};
  for (size_t i = 0; i < me.vertices.size(); ++i) {
    assets::Vertex& v = me.vertices[i];
    v.pos[0] = i < 3 ? px[i] : 0.f, v.pos[1] = i < 3 ? py[i] : 0.f, v.pos[2] = 0.5f;
    v.normal[2] = 1.f;
    v.color = 0xFFFFFFFFu;
    v.weights[0] = 1.f;
    (void)skinned;
  }
  me.indices = {0, 1, 2};
  return me;
}

// frame k of the re-posed model: three meshes that move every frame; `extra` pads a 4th mesh's vertex list (never indexed) so its block can be made big
assets::Model Pose(int k, size_t extra) {
  assets::Model m;
  m.label = "pvr";
  m.meshes.push_back(Tri(-0.5f + 0.05f * float(k), 0.4f, 0));
  m.meshes.push_back(Tri(0.1f, -0.4f + 0.04f * float(k), 0));
  m.meshes.push_back(Tri(0.5f - 0.06f * float(k), 0.3f - 0.03f * float(k), 0));
  if (extra) m.meshes.push_back(Tri(-0.1f + 0.02f * float(k), -0.1f, extra));
  return m;
}

// a skinned piece of a "yProp_" material no effect exists for: drawn in the debug colour, posed on the CPU by the bone palette each Record (the cloth block of Record). One bone, weight 1.
assets::Model ClothModel() {
  assets::Model m;
  m.label = "pvr_cloth";
  assets::Material mat;
  mat.shader = "yProp_pvr_none";
  m.materials.push_back(mat);
  assets::Mesh me = Tri(0.f, 0.f, 0, true);
  me.skinned = true;
  me.material = 0;
  m.meshes.push_back(me);
  return m;
}

uint64_t Hash(const std::vector<uint8_t>& b) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
  return h;
}

// pushes zeros into the frame's geometry blob: the other producers (crowd, particles, map plane ...) of a heavy frame
struct PushHook : StageHook {
  size_t before = 0, after = 0;
  const char* Name() const override { return "pvr_push"; }
  void Record(Stage stage, const StageCtx&, rdr::FrameBuilder& b) override {
    const size_t n = stage == Stage::BeforeOpaque ? before : stage == Stage::AfterBlended ? after : 0;
    if (!n) return;
    std::vector<uint8_t> junk(n, 0);
    b.PushGeometry(junk.data(), uint32_t(n), 16);
  }
};

struct Opt {
  bool ring = true;
  int plant = 0;
  int frames = 6;       // re-posed frames
  int rest = 0;         // frames after them with no re-pose (a model at rest)
  size_t extra = 0;     // the re-posed model's 4th mesh (vertices)
  bool two = false;     // two objects of the model in every frame
  bool cloth = false;   // a cloth piece instead of the re-posed model
  size_t hook_before = 0, hook_after = 0;
  bool direct = false;  // S103 PF2: the direct path (forced on this discrete card)
};

struct Result {
  std::vector<uint64_t> hashes;
  std::vector<uint64_t> pushes_by_frame;
  uint64_t posed_copies = 0, cloth_copies = 0, pushes = 0, overflow = 0, dropped = 0, validation = 0;
  uint64_t dwrites = 0, dbytes = 0, dfall = 0, dinfl = 0, dmade = 0, drel = 0;   // S103 PF2: mapped writes (meshes), their bytes, fallbacks to the staged copy, audit hits, copies made
  bool ok = true;
};

Result Run(const Opt& o) {
  Result r;
  rdr_scene::g_posed_ring.store(o.ring), rdr_scene::g_plant_posed_ring.store(o.plant < 5 ? o.plant : 0);
  rdr_scene::g_posed_direct.store(true), rdr_scene::g_posed_direct_force.store(o.direct), rdr_scene::g_plant_posed_direct.store(o.plant >= 5 ? o.plant : 0);   // S103 PF2
  const uint64_t push0 = g_posed_ring_pushes.load(), over0 = g_posed_ring_overflow.load();
  const uint64_t dw0 = g_posed_direct_writes.load(), db0 = g_posed_direct_bytes.load(), df0 = g_posed_direct_fallbacks.load(), di0 = g_posed_direct_inflight.load(), dm0 = g_posed_direct_made.load(), dr0 = g_posed_direct_released.load();
  std::string err;
  rdr::RendererOptions ro;
  ro.validation = 1;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); r.ok = false; return r; }
  {
    FrontEnd fe(*renderer);
    std::vector<std::string> warnings;
    const int id = o.cloth ? fe.AddModel(ClothModel(), warnings) : fe.AddModel(Pose(0, o.extra), warnings);
    PushHook hook;
    hook.before = o.hook_before, hook.after = o.hook_after;
    const int total = o.frames + o.rest;
    for (int k = -1; k < total; ++k) {
      // frame -1 draws the model as added (its first upload lands alone, like a live model made a frame before its first re-pose); frames 0.. re-pose it first (cloth: its bone moves instead)
      if (!o.cloth && k >= 0 && k < o.frames && !fe.UpdateModelVertices(id, Pose(k, o.extra))) { std::printf("FAIL UpdateModelVertices refused frame %d\n", k); r.ok = false; break; }
      SceneFrame sf;
      sf.width = W, sf.height = H;
      sf.viewer_space = false;
      sf.clear[0] = 0.1f, sf.clear[1] = 0.2f, sf.clear[2] = 0.3f, sf.clear[3] = 1.f;
      sf.camera.view = IdentityMat4();
      sf.camera.proj = IdentityMat4();
      if (o.hook_before || o.hook_after) sf.stage_hooks.push_back(&hook);
      for (int copy = 0; copy < (o.two ? 2 : 1); ++copy) {
        Object ob;
        ob.asset = uint32_t(id);
        ob.world = IdentityMat4();
        if (copy) ob.world.m[12] = 0.1f;   // (the second object is drawn a little to the side)
        if (o.cloth) {
          Mat4 bone = IdentityMat4();
          bone.m[12] = 0.04f * float(k + 1) + 0.3f * float(copy);   // the cloth piece's palette moves every frame
          sf.bones.push_back(bone);
          ob.flags |= kObjSkinned;
          ob.bones_first = uint32_t(sf.bones.size() - 1), ob.bones_count = 1;
        }
        sf.objects.push_back(ob);
      }
      std::vector<uint8_t> rgba;
      const uint64_t p0 = g_posed_ring_pushes.load();
      if (!fe.RenderToPixels(sf, rgba, err)) { std::printf("FAIL render frame %d: %s\n", k, err.c_str()); r.ok = false; break; }
      if (k >= 0) {
        r.hashes.push_back(Hash(rgba));
        r.pushes_by_frame.push_back(g_posed_ring_pushes.load() - p0);
        const rdr::Renderer::UploadStats us = renderer->LastUploadStats();
        r.posed_copies += us.by_producer[1], r.cloth_copies += us.by_producer[2];
      }
    }
    r.validation = fe.validation_errors();
    renderer->WaitIdle();
  }
  r.dropped = renderer->FramesDropped();
  r.pushes = g_posed_ring_pushes.load() - push0, r.overflow = g_posed_ring_overflow.load() - over0;
  r.dwrites = g_posed_direct_writes.load() - dw0, r.dbytes = g_posed_direct_bytes.load() - db0, r.dfall = g_posed_direct_fallbacks.load() - df0, r.dinfl = g_posed_direct_inflight.load() - di0, r.dmade = g_posed_direct_made.load() - dm0, r.drel = g_posed_direct_released.load() - dr0;
  rdr_scene::g_posed_direct_force.store(false), rdr_scene::g_plant_posed_direct.store(0);
  return r;
}

// S103 PF2: the host's tween shape WITHOUT any wait: every tick re-poses the model, records it once and renders the frame `renders` times back to back (frames in flight run behind). The copies are rewritten while the GPU is
// still reading older ones: the front end's free rule and the renderer's own reader record must keep them apart. fall = re-poses that found no copy free, infl = the independent audit (a write into a copy the renderer's
// last_read says may be in flight), over = frames rendered later than their Record promised. The last frame is read back after WaitIdle.
struct TweenResult { bool ok = true; uint64_t hash = 0, fall = 0, infl = 0, over = 0, writes = 0, validation = 0, dropped = 0; };
TweenResult RunTween(bool direct, int renders, int ticks, int plant) {
  TweenResult t;
  rdr_scene::g_posed_ring.store(true), rdr_scene::g_plant_posed_ring.store(0);
  rdr_scene::g_posed_direct.store(direct), rdr_scene::g_posed_direct_force.store(direct), rdr_scene::g_plant_posed_direct.store(plant >= 5 && plant <= 7 ? plant : 0);
  rdr::g_problem_nonfatal.store(true);   // (plant 8: the renderer's overrun Problem is counted, not fatal)
  const uint64_t dw0 = g_posed_direct_writes.load(), df0 = g_posed_direct_fallbacks.load(), di0 = g_posed_direct_inflight.load();
  std::string err;
  rdr::RendererOptions ro;
  ro.validation = 1;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); t.ok = false; return t; }
  {
    FrontEnd fe(*renderer);
    std::vector<std::string> warnings;
    const int id = fe.AddModel(Pose(0, 0), warnings);
    for (int k = 0; k < ticks && t.ok; ++k) {
      if (!fe.UpdateModelVertices(id, Pose(1 + k % 24, 0))) { std::printf("FAIL UpdateModelVertices refused tick %d\n", k); t.ok = false; break; }
      SceneFrame sf;
      sf.width = W, sf.height = H;
      sf.viewer_space = false;
      sf.clear[0] = 0.1f, sf.clear[1] = 0.2f, sf.clear[2] = 0.3f, sf.clear[3] = 1.f;
      sf.camera.view = IdentityMat4();
      sf.camera.proj = IdentityMat4();
      Object ob;
      ob.asset = uint32_t(id);
      ob.world = IdentityMat4();
      sf.objects.push_back(ob);
      if (!fe.RenderNoWait(sf, uint32_t(renders), err)) { std::printf("FAIL RenderNoWait tick %d: %s\n", k, err.c_str()); t.ok = false; break; }
    }
    renderer->WaitIdle();
    SceneFrame sf;   // the last pose, read back
    sf.width = W, sf.height = H;
    sf.viewer_space = false;
    sf.clear[0] = 0.1f, sf.clear[1] = 0.2f, sf.clear[2] = 0.3f, sf.clear[3] = 1.f;
    sf.camera.view = IdentityMat4();
    sf.camera.proj = IdentityMat4();
    Object ob;
    ob.asset = uint32_t(id);
    ob.world = IdentityMat4();
    sf.objects.push_back(ob);
    std::vector<uint8_t> rgba;
    if (!fe.RenderToPixels(sf, rgba, err)) { std::printf("FAIL final render: %s\n", err.c_str()); t.ok = false; }
    t.hash = Hash(rgba);
    t.validation = fe.validation_errors();
    renderer->WaitIdle();
  }
  t.over = renderer->DirectOverruns(), t.dropped = renderer->FramesDropped();
  t.writes = g_posed_direct_writes.load() - dw0, t.fall = g_posed_direct_fallbacks.load() - df0, t.infl = g_posed_direct_inflight.load() - di0;
  rdr_scene::g_posed_direct_force.store(false), rdr_scene::g_plant_posed_direct.store(0);
  return t;
}

bool Same(const Result& a, const Result& b) {
  if (a.hashes.size() != b.hashes.size() || a.hashes.empty()) return false;
  for (size_t i = 0; i < a.hashes.size(); ++i)
    if (a.hashes[i] != b.hashes[i]) return false;
  return true;
}

bool Moves(const Result& a, int frames) {
  for (int i = 1; i < frames && size_t(i) < a.hashes.size(); ++i)
    if (a.hashes[size_t(i)] == a.hashes[size_t(i - 1)]) return false;
  return a.hashes.size() > 1;
}

}  // namespace

int main(int argc, char** argv) {
  const int plant = (argc > 2 && !std::strcmp(argv[1], "--plant")) ? std::atoi(argv[2]) : 0;
  int bad = 0;
  const auto check = [&](bool ok, const std::string& what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) ++bad; };
  const auto n = [](uint64_t v) { return std::to_string(v); };
  constexpr int kFrames = 6, kRest = 8;

  // ---- 1: a re-posed model, then at rest (plants 1 and 2 run this one)
  if (plant == 0 || plant == 1 || plant == 2) {   // (plants 5..7 run section 6 only)
    Opt o;
    o.frames = kFrames, o.rest = kRest, o.plant = plant;
    const Result ring = Run(o);
    Opt ob = o;
    ob.ring = false, ob.plant = 0;
    const Result old = Run(ob);
    std::printf("pvr_unit model: ring pushes %llu posed_copies %llu overflow %llu | old: posed_copies %llu\n", (unsigned long long)ring.pushes, (unsigned long long)ring.posed_copies, (unsigned long long)ring.overflow, (unsigned long long)old.posed_copies);
    check(ring.ok && old.ok, "both arms rendered every frame");
    check(old.posed_copies >= uint64_t(3 * kFrames), "old path: one staged buffer copy per mesh per re-posed frame (" + n(old.posed_copies) + " over " + n(kFrames) + " frames x 3 meshes)");
    check(Moves(old, kFrames), "the pose reaches the picture: every re-posed frame differs from the one before (old path)");
    if (!plant) {
      check(ring.posed_copies <= 3 && ring.pushes >= uint64_t(kFrames), "ring: " + n(ring.pushes) + " pushes into the geometry ring, " + n(ring.posed_copies) + " posed buffer copies (the one write of the model at rest: at most 1 per mesh)");
      uint64_t late = 0;   // pushes in the last 3 frames of the rest phase
      for (size_t i = ring.pushes_by_frame.size() - 3; i < ring.pushes_by_frame.size(); ++i) late += ring.pushes_by_frame[i];
      check(late == 0, "at rest: ring pushes stop (" + n(late) + " in the last 3 of " + n(kRest) + " rest frames; the model is drawn from its own buffer)");
      check(ring.validation == 0 && old.validation == 0 && ring.dropped == 0, "0 validation errors, 0 dropped frames");
    }
    check(Same(ring, old), "the picture of every frame (moving and at rest) is identical, ring vs old path");
  }

  // ---- 2: two objects of one model in one frame: one push a frame
  if (!plant) {
    Opt o;
    o.frames = 4, o.two = true;
    const Result ring = Run(o);
    Opt ob = o;
    ob.ring = false;
    const Result old = Run(ob);
    bool one = !ring.pushes_by_frame.empty();
    for (uint64_t p : ring.pushes_by_frame) one = one && p == 1;
    check(ring.ok && old.ok && one, "two objects of one model: ONE push per frame (" + n(ring.pushes) + " over 4 frames)");
    check(Same(ring, old), "two objects: the picture is identical, ring vs old path");
  }

  // ---- 3: a cloth piece (the cloth block of Record; plants 1 and 3 run this one)
  if (plant == 0 || plant == 1 || plant == 3) {
    Opt o;
    o.cloth = true, o.frames = 5, o.plant = plant;
    const Result ring = Run(o);
    Opt ob = o;
    ob.ring = false, ob.plant = 0;
    const Result old = Run(ob);
    std::printf("pvr_unit cloth: ring pushes %llu cloth_copies %llu overflow %llu | old: cloth_copies %llu\n", (unsigned long long)ring.pushes, (unsigned long long)ring.cloth_copies, (unsigned long long)ring.overflow, (unsigned long long)old.cloth_copies);
    check(ring.ok && old.ok, "cloth: both arms rendered every frame");
    check(old.cloth_copies >= 4, "cloth old path: one staged buffer copy per piece per frame (" + n(old.cloth_copies) + ")");
    check(Moves(old, 5), "cloth: the palette reaches the picture (old path)");
    if (!plant) {
      check(ring.cloth_copies == 0 && ring.posed_copies == 0, "cloth ring: 0 cloth / posed buffer copies");
      check(ring.pushes >= 5 && ring.overflow == 0, "cloth ring: " + n(ring.pushes) + " pushes");
    }
    check(Same(ring, old), "cloth: the picture of every frame is identical, ring vs old path");
  }

  if (plant == 0 || plant == 4) {
    // ---- 4: (not under plant 4) a block over the posed cap falls back to the model's own buffer, counted, nothing dropped
    if (!plant) {
      Opt o;
      o.frames = 3, o.extra = kBigVerts;
      const Result rb = Run(o);
      Opt ob = o;
      ob.ring = false;
      const Result obr = Run(ob);
      check(rb.ok && obr.ok && rb.overflow >= 3 && rb.dropped == 0, "overflow: a 16 MB posed block does not fit the cap: " + n(rb.overflow) + " fallbacks to the buffer path, counted; dropped frames " + n(rb.dropped));
      check(Same(rb, obr) && rb.posed_copies > 0, "overflow: the fallback draws the same picture as the old path (nothing dropped), through " + n(rb.posed_copies) + " counted buffer copies");
    }
    // ---- 5: the renderer's REAL room: other producers push 14 MB before and 6 MB after the posed model (4 MB): the posed push must leave their room (no dropped frame), falling back per mesh
    {
      Opt o;
      o.frames = 4, o.extra = kMidVerts, o.hook_before = 14u << 20, o.hook_after = 6u << 20, o.plant = plant;
      const Result rb = Run(o);
      Opt ob = o;
      ob.ring = false, ob.plant = 0;
      const Result obr = Run(ob);
      std::printf("pvr_unit room: ring pushes %llu overflow %llu dropped %llu | old dropped %llu\n", (unsigned long long)rb.pushes, (unsigned long long)rb.overflow, (unsigned long long)rb.dropped, (unsigned long long)obr.dropped);
      check(rb.ok && obr.ok && rb.dropped == obr.dropped, "room: no frame dropped that the old path keeps (dropped " + n(rb.dropped) + " vs " + n(obr.dropped) + ")");
      check(rb.overflow >= 1, "room: the posed block gave way to the other producers' room, counted fallbacks " + n(rb.overflow));
      check(Same(rb, obr), "room: the picture of every frame is identical, ring vs old path");
    }
  }
  // ---- 6 (S103 PF2): the direct path: a re-posed model written straight into mapped copies (forced on this discrete card); plants 5, 6, 7 run this one
  if (plant == 0 || plant >= 5) {
    Opt o;
    o.frames = kFrames + 4, o.rest = kRest, o.direct = true, o.plant = plant;
    const Result d = Run(o);
    Opt ob = o;
    ob.direct = false, ob.ring = false, ob.plant = 0;
    const Result old = Run(ob);
    Opt orr = o;
    orr.direct = false, orr.plant = 0;
    const Result rg = Run(orr);
    const uint64_t meshes = 3ull * uint64_t(o.frames);
    std::printf("pvr_unit direct: mapped writes %llu (%llu bytes) copies made %llu fallbacks %llu in-flight writes %llu ring pushes %llu posed_copies %llu | old: posed_copies %llu | ring: pushes %llu posed_copies %llu\n", (unsigned long long)d.dwrites, (unsigned long long)d.dbytes,
                (unsigned long long)d.dmade, (unsigned long long)d.dfall, (unsigned long long)d.dinfl, (unsigned long long)d.pushes, (unsigned long long)d.posed_copies, (unsigned long long)old.posed_copies, (unsigned long long)rg.pushes, (unsigned long long)rg.posed_copies);
    check(d.ok && old.ok && rg.ok, "direct: all three arms rendered every frame");
    check(Moves(old, o.frames), "direct: the pose reaches the picture (old path moves every frame)");
    check(Same(d, old), "direct: the picture of every frame (moving and at rest) is identical to the old path (max diff 0: the hash of the whole RGBA frame)");
    check(Same(d, rg), "direct: the picture of every frame is identical to the ring path");
    check(d.posed_copies == 0, "direct: 0 GPU buffer copies of posed vertices (the old path made " + n(old.posed_copies) + ", the ring path " + n(rg.posed_copies) + " for the model at rest)");
    check(d.pushes == 0, "direct: 0 ring pushes (" + n(d.pushes) + "; the ring path pushed " + n(rg.pushes) + ")");
    check(d.dwrites == meshes, "direct: mapped writes = posed meshes (" + n(d.dwrites) + " of " + n(meshes) + ": 3 meshes x " + n(o.frames) + " re-posed frames; none for the " + n(kRest) + " rest frames)");
    check(d.dfall == 0 && d.dmade == 4, "direct: no copy was in flight when a re-pose came (" + n(d.dfall) + " fallbacks), 4 copies made once (" + n(d.dmade) + ")");
    check(d.dinfl == 0, "direct: no write into a copy a frame in flight may still read (audit " + n(d.dinfl) + ")");
    check(d.validation == 0 && old.validation == 0 && d.dropped == 0, "direct: 0 validation errors, 0 dropped frames");
    {   // S103 PF2 (review): no WaitIdle, the tween shape (2 Renders per Record), a re-pose every tick: the copies cycle while the GPU is behind (plant 8: 3 Renders per Record)
      constexpr int kTicks = 64;
      const int renders = plant == 8 ? 3 : int(rdr::kRendersPerRecord);
      const TweenResult dt = RunTween(true, renders, kTicks, plant);
      const TweenResult ot = RunTween(false, renders, kTicks, 0);
      std::printf("pvr_unit tween: %d ticks x %d renders, no wait: mapped writes %llu fallbacks %llu audit %llu overruns %llu | old: overruns %llu\n", kTicks, renders, (unsigned long long)dt.writes, (unsigned long long)dt.fall, (unsigned long long)dt.infl, (unsigned long long)dt.over, (unsigned long long)ot.over);
      check(dt.ok && ot.ok && dt.hash == ot.hash, "tween shape, no wait: the last frame is identical to the old path's");
      check(dt.writes == 3ull * kTicks && dt.fall == 0, "tween shape, no wait: every re-pose found a free copy (" + n(dt.writes) + " mesh writes over " + n(kTicks) + " ticks, " + n(dt.fall) + " fallbacks)");
      check(dt.infl == 0, "tween shape, no wait: the independent audit (the renderer's own last_read) saw 0 writes into a copy a frame in flight may read (" + n(dt.infl) + ")");
      check(dt.over == 0, "tween shape, no wait: no frame was rendered later than its Record promised (" + n(rdr::kRendersPerRecord) + " Renders per Record; overruns " + n(dt.over) + ")");
      check(dt.validation == 0 && dt.dropped == 0, "tween shape, no wait: 0 validation errors, 0 dropped frames");
    }
    if (!plant) {   // S103 PF2 (review): a model at rest frees its copies after kPosedRestRenders: one staged write into `vb`, the picture unchanged
      Opt q;
      q.frames = 3, q.rest = int(rdr_scene::kPosedRestRenders) + 120, q.direct = true;
      const Result rd = Run(q);
      Opt qb = q;
      qb.direct = false, qb.ring = false;
      const Result ro = Run(qb);
      check(rd.ok && ro.ok && Same(rd, ro), "at rest " + n(q.rest) + " frames: the picture of every frame is identical to the old path");
      check(rd.drel == 1 && rd.dmade == 4, "at rest: the 4 copies were freed once after " + n(rdr_scene::kPosedRestRenders) + " Renders (freed " + n(rd.drel) + ", made " + n(rd.dmade) + ")");
    }
    if (!plant) {
      Opt t = o;
      t.frames = 4, t.rest = 0, t.two = true;
      const Result dt = Run(t);
      Opt tb = t;
      tb.direct = false, tb.ring = false;
      const Result tobd = Run(tb);
      check(dt.ok && tobd.ok && Same(dt, tobd) && dt.dwrites == 12 && dt.pushes == 0, "direct: two objects of one model: ONE write a frame (" + n(dt.dwrites) + " mesh writes over 4 frames), the picture identical to the old path");
    }
  }
  std::printf("%s pvr_unit\n", bad ? "FAIL" : "PASS");
  if (plant) return bad ? 0 : 1;   // a planted fault must be caught: exit 0 only when a check failed
  return bad ? 1 : 0;
}
