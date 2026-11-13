// Lane 29 (class C4, wrestlers) offline test tool. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game, no SDK, no run lock.
//   wrestler_emit selftest
//       Unit checks of the contributor (last update of the frame wins, frame pairing, skin = inv_bind x world checked against an INDEPENDENT
//       multiply and a hard-coded literal, ids, visibility, persistence, delta, counters, size filter), each with a planted-fault twin that
//       must be caught. Exit 0 = all pass.
//   wrestler_emit emit IN.rsc OUT.rsc [--fault bind|first|exit]
//       Reads a record the Arena Viewer wrote (wrestlers posed from the files), feeds every skinned object's palette through the contributor
//       the way the hook would (updates in the frame: the bind pose, a pose one step off, the real pose LAST; plus one update tagged with the NEXT
//       frame that must not be used), calls Emit at that frame, and writes OUT: the input record with each skinned object replaced by the object
//       Emit produced (id, flags, palette, light set), only the unknown link (asset, world, mesh mask, params) patched in from the input.
//       Also writes OUT.live.rsc: Emit's own tables unpatched (placeholder asset, source Live) with the input's camera, which rsc_draw must
//       accept (it skips objects whose asset has no pac).
//       --fault bind : only the bind pose is submitted (the picture must differ).   --fault first : the first update of the frame wins.   --fault exit : no consumer read, the exit (derived) palette stands.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/scene_live/wrestler_contrib.h"
#include "../../src/scene_live/model_hooks.h"

using namespace rdr_scene;
using namespace rdr_scene::live;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// Stand-ins for the writer's (lane 26) registry, which the contributor itself does not need.
namespace rdr_scene::live {
void Register(Contributor*) {}
bool Enabled() { return true; }
void EndFrame(uint64_t) {}
void AddSink(Sink*) {}
void RemoveSink(Sink*) {}
}  // namespace rdr_scene::live

static Mat4 Trans(float x, float y, float z) {
  Mat4 m = IdentityMat4();
  m.m[12] = x, m.m[13] = y, m.m[14] = z;
  return m;
}
static Mat4 Rot(float a, float x, float y, float z) {  // a rotation about Z plus a translation
  Mat4 m = IdentityMat4();
  m.m[0] = std::cos(a), m.m[1] = std::sin(a), m.m[4] = -std::sin(a), m.m[5] = std::cos(a);
  m.m[12] = x, m.m[13] = y, m.m[14] = z;
  return m;
}
// INDEPENDENT of the code under test: C = A x B with row vectors, written as the textbook element formula, column by column.
static Mat4 RefMul(const Mat4& A, const Mat4& B) {
  Mat4 C;
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) {
      double acc = 0;
      for (int k = 0; k < 4; ++k) acc += double(A.m[row * 4 + k]) * double(B.m[k * 4 + col]);
      C.m[row * 4 + col] = float(acc);
    }
  return C;
}
static Mat4 SwappedSkin(const Mat4& world, const Mat4& inv_bind) { return RefMul(world, inv_bind); }  // the planted wrong order
static bool Near(const Mat4& a, const Mat4& b, float eps) {
  for (int i = 0; i < 16; ++i)
    if (std::fabs(a.m[i] - b.m[i]) > eps * (1.f + std::fabs(b.m[i]))) return false;
  return true;
}

static std::vector<Mat4> Pose(int n, float seed) {
  std::vector<Mat4> v(n);
  for (int i = 0; i < n; ++i) v[i] = Rot(seed + 0.01f * i, seed * 3 + i, 2.f * i - seed, 0.5f * i);
  return v;
}

static SceneTables EmitAt(WrestlerContributor& w, Counters& c, uint64_t frame = 0) {
  SceneTables t;
  FrameInfo f;
  f.guest_frame = frame;
  w.Emit(f, t, c);
  return t;
}

static bool PaletteIs(const SceneTables& t, size_t obj, const std::vector<Mat4>& want, float eps = 1e-4f) {
  if (obj >= t.objects.size() || t.objects[obj].bones_count != want.size()) return false;
  for (size_t i = 0; i < want.size(); ++i)
    if (!Near(t.bones[t.objects[obj].bones_first + i], want[i], eps)) return false;
  return true;
}

static int SelfTest() {
  const int kN = 109;
  std::vector<Mat4> inv(kN), pa = Pose(kN, 0.1f), pb = Pose(kN, 0.2f), pc = Pose(kN, 0.3f);
  for (int i = 0; i < kN; ++i) inv[i] = Trans(-1.f * i, -0.5f * i, -2.f);
  auto world_of = [&](const std::vector<Mat4>& skin) {  // world = T(+t) x skin, by the independent multiply
    std::vector<Mat4> w(kN);
    for (int i = 0; i < kN; ++i) w[i] = RefMul(Trans(1.f * i, 0.5f * i, 2.f), skin[i]);
    return w;
  };
  // ---- the multiply order, against a hard-coded literal and the independent multiply ----
  {
    Mat4 W = IdentityMat4();  // 90 degrees about Z, translation (10,20,30)
    W.m[0] = 0, W.m[1] = 1, W.m[4] = -1, W.m[5] = 0, W.m[12] = 10, W.m[13] = 20, W.m[14] = 30;
    const Mat4 I = Trans(1, 2, 3);
    // inv_bind x world, row 3 by hand: 1*row0 + 2*row1 + 3*row2 + 1*row3 of W = (0,1,0) + (-2,0,0) + (0,0,3) + (10,20,30) = (8, 21, 33)
    const Mat4 got = SkinFromWorld(W, I);
    Check(got.m[12] == 8.f && got.m[13] == 21.f && got.m[14] == 33.f && got.m[15] == 1.f, "skin = inv_bind x world: the literal translation (8,21,33)");
    Mat4 sw = SwappedSkin(W, I);  // world x inv_bind would give (11,22,33)
    Check(!(sw.m[12] == 8.f && sw.m[13] == 21.f && sw.m[14] == 33.f), "planted twin (world x inv_bind) fails the literal");
    Check(Near(got, RefMul(I, W), 1e-6f) && !Near(got, RefMul(W, I), 1e-3f), "SkinFromWorld agrees with the independent inv x world, not world x inv");
  }
  // ---- last update wins, ids, visibility, persistence ----
  {
    WrestlerContributor w;
    for (auto* p : {&pa, &pb, &pc}) w.SubmitWorld(0x1000, 0, kN, world_of(*p).data(), inv.data(), 7);
    w.SubmitSkin(0x2000, -1, kN, pb.data(), 7);
    w.SubmitSkin(0x2000, -1, kN, pc.data(), 7);
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    Check(t.objects.size() == 2 && c.objects == 2, "two skeletons updated -> two objects");
    Check(t.objects.size() == 2 && t.Str(t.objects[0].id) == "actor:slot:0" && t.Str(t.objects[1].id) == "skel:00002000", "ids: actor:slot:0 for the matched skeleton, skel:<hex> for the other");
    Check(t.objects.size() == 2 && PaletteIs(t, 0, pc), "the palette is the LAST update's skin (inv_bind x world, independent multiply), within 1e-4");
    Check(t.objects.size() == 2 && !PaletteIs(t, 0, pa), "positive control: the first update's palette is NOT what was emitted");
    Check(t.objects.size() == 2 && t.objects[0].flags == (kObjVisible | kObjSkinned), "slot-matched: visible | skinned");
    Check(t.objects.size() == 2 && t.objects[1].flags == kObjSkinned, "no slot match: not visible (managers / extras), still recorded");
    Check(t.objects[0].asset < t.assets.size() && t.assets[t.objects[0].asset].pac == 0 && t.assets[t.objects[0].asset].variant == 0xFFFFFFFFu, "asset unknown = placeholder AssetRef (pac 0), in range");
    Check(c.unknown == 2u * 2 + 1 + 2 + 2, "unknown counted: (asset, mask) x2 + 1 unlinked + light set x2 + palette-not-at-consumer x2 = 9");
    std::string err;
    SceneTables back;
    auto bytes = SerializeRecord(t);
    Check(ParseRecord(bytes.data(), bytes.size(), back, err), "the emitted record parses back");
    // No update since: the wrestlers stay (persistent snapshot), stale counted per skeleton.
    Counters c2;
    SceneTables t2 = EmitAt(w, c2, 8);
    Check(t2.objects.size() == 2 && c2.stale == 2 && PaletteIs(t2, 0, pc), "a frame with no update keeps both skeletons (last palette), stale 2");
    // Pruned after kPruneAfter Emits without an update.
    Counters c3;
    SceneTables t3;
    for (uint32_t i = 0; i < WrestlerContributor::kPruneAfter + 1; ++i) { c3 = Counters(); t3 = EmitAt(w, c3, 9 + i); }
    Check(t3.objects.empty(), "pruned after kPruneAfter frames without an update (bounded memory)");
  }
  {  // planted fault: the FIRST update of the frame wins: the same comparison must fail
    WrestlerContributor w;
    w.SetFirstWinsForTest(true);
    for (auto* p : {&pa, &pb, &pc}) w.SubmitWorld(0x1000, 0, kN, world_of(*p).data(), inv.data(), 7);
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    Check(!PaletteIs(t, 0, pc), "planted fault (first update wins) is caught by the last-update comparison");
  }
  {  // frame pairing: an update tagged with a later frame is not used for an earlier record, and is used by the later one
    WrestlerContributor w;
    w.SubmitSkin(0x1000, 0, kN, pb.data(), 7);
    w.SubmitSkin(0x1000, 0, kN, pc.data(), 8);  // thread 7 already ran ahead
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    Check(PaletteIs(t, 0, pb), "record of frame 7 uses the frame-7 update, not the frame-8 one");
    Counters c2;
    SceneTables t2 = EmitAt(w, c2, 8);
    Check(PaletteIs(t2, 0, pc) && c2.stale == 0, "record of frame 8 uses the frame-8 update");
  }
  {  // the delta of sub_82BD3620 reaches the stored palette (pending one, then persistent one)
    WrestlerContributor w;
    w.SubmitSkin(0x1000, 0, kN, pa.data(), 7);
    w.ApplyDelta(0x1000, 1.f, 2.f, 3.f);
    std::vector<Mat4> want = pa;
    for (Mat4& m : want) m.m[12] += 1.f, m.m[13] += 2.f, m.m[14] += 3.f;
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    Check(PaletteIs(t, 0, want), "delta added to every node's translation of the pending palette");
    w.ApplyDelta(0x1000, 1.f, 0.f, 0.f);
    for (Mat4& m : want) m.m[12] += 1.f;
    Counters c2;
    SceneTables t2 = EmitAt(w, c2, 8);
    Check(PaletteIs(t2, 0, want), "delta also reaches the persistent palette");
    WrestlerContributor w0;
    w0.SubmitSkin(0x1000, 0, kN, pa.data(), 7);
    Counters c3;
    SceneTables t3 = EmitAt(w0, c3, 7);
    Check(!PaletteIs(t3, 0, want), "positive control: without the delta the palette differs");
  }
  {  // a slot claimed twice in one record: the second becomes a skel: id and is not visible
    WrestlerContributor w;
    w.SubmitSkin(0x1000, 3, 12, Pose(12, 0.5f).data(), 7);
    w.SubmitSkin(0x2000, 3, 12, Pose(12, 0.6f).data(), 7);
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    Check(t.objects.size() == 2 && t.Str(t.objects[0].id) == "actor:slot:3" && t.Str(t.objects[1].id) == "skel:00002000" && t.objects[1].flags == kObjSkinned, "second claim on slot 3 -> skel: id, not visible");
  }
  {  // size filter default, light set, unreadable
    WrestlerContributor w;
    Check(w.MinNodes() == WrestlerContributor::kDefaultMinNodes, "default size filter is the documented one");
    w.SetLightSet(10);
    w.SubmitSkin(0x3000, 1, 12, Pose(12, 0.5f).data(), 7);
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    Check(t.objects.size() == 1 && t.objects[0].light_set == 10 && c.unknown == 3, "light set 10 set by the wiring is emitted, not counted unknown (asset, mask, derived palette = 3)");
    w.SubmitWorld(0x3000, 1, 0, nullptr, nullptr);
    Counters c3;
    SceneTables t3 = EmitAt(w, c3, 8);
    Check(c3.unknown >= 1 && PaletteIs(t3, 0, Pose(12, 0.5f)), "an unreadable submit is counted unknown; no guessed palette replaces the kept one");
  }
  {  // lane 206: a skeleton under the size filter is recorded only when the game's own descriptor lists it as a wrestler piece (Diesel's hair: 48 nodes, own skeleton; K1 run 7093)
    WrestlerContributor w;
    const uint32_t body = 0x4000, hair = 0x5000, other = 0x6000;
    Check(!w.KeepSkeleton(hair, 48) && !w.KeepSkeleton(other, 48), "before the descriptor is read, a 48-node skeleton is dropped (the old rule)");
    Check(w.KeepSkeleton(body, 109) && !w.KeepSkeleton(body, 0), "a body-sized skeleton is kept; an empty one never");
    std::vector<WrestlerContributor::PieceRef> pieces(1);
    pieces[0].model = hair;
    w.SubmitModelLink(body, 33510, 0x1000, 16, 0x2000, std::move(pieces));
    Check(w.KeepSkeleton(hair, 48), "a 48-node skeleton the descriptor lists is kept");
    Check(!w.KeepSkeleton(other, 48), "planted twin: a 48-node skeleton the descriptor does NOT list is still dropped");
    Check(w.IsListedPiece(hair) && !w.IsListedPiece(other) && w.OwnerOfPiece(hair) == body, "the lock-free table agrees with the owner table");
    w.SubmitSkin(hair, -1, 48, Pose(48, 0.2f).data(), 7);
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    Check(t.objects.size() == 1 && t.objects[0].bones_count == 48, "the listed small skeleton is recorded with its own 48 nodes");
    w.SubmitModelLink(body, 33510, 0x1000, 16, 0x2000, {});   // the relink drops the old pieces
    Check(!w.KeepSkeleton(hair, 48), "a body dropped from the table takes its pieces out of the lock-free table");
  }
  {  // lane 207: own-skeleton pieces (a wrestler's hair, elbow pads) are never in the descriptor's list; the scene core WANTS (object, node count) once it has joined the draw object to a small model
    WrestlerContributor w;
    const uint32_t hair = 0xA90FE430, pads = 0xA8D42D70, other = 0x6000;
    Check(!w.KeepSkeleton(hair, 48), "nothing wanted: a 48-node own skeleton is dropped (the old rule)");
    w.WantSkeleton(hair, 48);
    w.WantSkeleton(pads, 10);
    Check(w.KeepSkeleton(hair, 48) && w.KeepSkeleton(pads, 10) && w.Wanted() == 2, "a wanted (address, count) is kept");
    Check(!w.KeepSkeleton(hair, 47) && !w.KeepSkeleton(hair, 49) && !w.KeepSkeleton(pads, 48), "planted: a wrong count for a wanted address is not kept");
    Check(!w.KeepSkeleton(other, 48) && !w.KeepSkeleton(other, 10), "an unwanted small skeleton is not kept");
    Check(!w.KeepSkeleton(hair, 0), "an empty skeleton is never kept");
    w.WantSkeleton(hair, 48);
    Check(w.Wanted() == 2, "wanting the same object again does not count twice");
    w.SubmitSkin(hair, -1, 48, Pose(48, 0.2f).data(), 7);
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    Check(t.objects.size() == 1 && t.objects[0].bones_count == 48, "the wanted small skeleton is recorded with its own 48 nodes");
    w.SubmitModelLink(0x4000, 33510, 0x1000, 16, 0x2000, {});   // a descriptor relink empties listed_ ...
    Check(w.KeepSkeleton(hair, 48), "... but the separate wanted table keeps the want");
    w.UnwantSkeleton(hair);
    Check(!w.KeepSkeleton(hair, 48) && w.KeepSkeleton(pads, 10) && w.Wanted() == 1, "unwant removes it; the other want stays");
    w.WantSkeleton(hair, 48);
    Check(w.KeepSkeleton(hair, 48) && w.Wanted() == 2, "a removed slot is reused by a new want");
    w.ClearWants();
    Check(!w.KeepSkeleton(hair, 48) && !w.KeepSkeleton(pads, 10) && w.Wanted() == 0, "ClearWants empties the table");
    for (uint32_t i = 0; i < 4096 + 5; ++i) w.WantSkeleton(0x10000 + i * 8, 10);
    Check(w.WantedFull() == 5 && w.Wanted() == 4096, "overflow: wants past the table are counted, not stored");
  }
  {  // bounded memory: more distinct skeletons than the cap are dropped and counted
    WrestlerContributor w;
    for (uint32_t i = 0; i < 100; ++i) w.SubmitSkin(0x10000 + i * 0x100, -1, 4, Pose(4, 0.1f).data(), 7);
    Counters c;
    SceneTables t = EmitAt(w, c, 7);
    const size_t cap = WrestlerContributor::kMaxSkeletons, room = WrestlerContributor::kBodyHeadroom;
    Check(t.objects.size() == cap - room && c.unknown >= 100 - (cap - room) && w.CapRefused() == 100 - (cap - room), "small skeletons stop at the cap minus the body headroom; the rest are counted (CapRefused)");
    for (uint32_t i = 0; i < 20; ++i) w.SubmitSkin(0x80000 + i * 0x100, -1, 109, Pose(109, 0.1f).data(), 7);
    Counters c2;
    SceneTables t2 = EmitAt(w, c2, 7);
    Check(t2.objects.size() == cap && w.CapRefused() == 100 - (cap - room) + 4, "body-sized skeletons still take the headroom (16 of 20), the cap holds");
  }
  // ===================== lane 43 (W1): the palette at the consumer, engine objects, the asset link =====================
  // Brock's real bug (WRESTLER_PROMOTION_MAP.md section 2): nodes 0-58 equal at the exit of sub_82BD30E8, nodes 59-108 differ. `exit_pose` is that
  // palette (derived at the exit); `draw_pose` is what the draw reads (+200 at sub_82C04DF0's entry).
  {
    std::vector<Mat4> exit_pose = pa, draw_pose = pa;
    for (int i = 59; i < kN; ++i) draw_pose[i].m[12] += 0.3f + 0.01f * (i - 59);  // 0.3-1.3 units, as measured on record 173
    auto bit_equal = [&](const SceneTables& t, size_t obj, const std::vector<Mat4>& want) {  // BIT exact: memcmp, no tolerance
      if (obj >= t.objects.size() || t.objects[obj].bones_count != want.size()) return false;
      return std::memcmp(&t.bones[t.objects[obj].bones_first], want.data(), want.size() * sizeof(Mat4)) == 0;
    };
    // The derived palette is what the exit hook produces from world + inv bind; make world so that skin == exit_pose exactly.
    auto submit_exit = [&](WrestlerContributor& w, uint32_t skel, int slot, uint64_t frame) {
      std::vector<Mat4> world(kN);
      for (int i = 0; i < kN; ++i) world[i] = RefMul(Trans(1.f * i, 0.5f * i, 2.f), exit_pose[i]);
      w.SubmitWorld(skel, slot, kN, world.data(), inv.data(), frame);
    };
    {  // 1. the consumer palette is what is emitted, bit for bit, in either hook order; the exit palette is NOT
      for (int order = 0; order < 2; ++order) {
        WrestlerContributor w;
        if (order == 0) { submit_exit(w, 0x1000, 0, 7); w.SubmitConsumer(0x1000, kN, draw_pose.data(), 7); }
        else { w.SubmitConsumer(0x1000, kN, draw_pose.data(), 7); submit_exit(w, 0x1000, 0, 7); }
        Counters c;
        SceneTables t = EmitAt(w, c, 7);
        char what[160];
        std::snprintf(what, sizeof what, "Brock plant: the emitted palette is the consumer's, bit-exact on nodes 0-108 (%s hook first)", order == 0 ? "exit" : "consumer");
        Check(t.objects.size() == 1 && bit_equal(t, 0, draw_pose), what);
        Check(t.objects.size() == 1 && !bit_equal(t, 0, exit_pose), "positive control: the exit palette (the old source) is NOT what was emitted");
        Check(t.objects.size() == 1 && t.Str(t.objects[0].id) == "actor:slot:0", "the consumer palette keeps the slot the update hook matched");
        Check(c.unknown == 2 + 1, "consumer source: unknown = asset, mask, light set only (no derived-palette count)");
      }
      WrestlerContributor w;  // the real bug re-found: with the consumer hook silent, the emitted palette differs from the draw's on nodes 59-108
      submit_exit(w, 0x1000, 0, 7);
      Counters c;
      SceneTables t = EmitAt(w, c, 7);
      Check(t.objects.size() == 1 && !bit_equal(t, 0, draw_pose) && c.unknown == 4 && w.DerivedFallbacks() == 1,
            "planted (today's source = the exit of sub_82BD30E8): differs from the draw's palette and is counted unknown + derived fallback");
      bool low_equal = true, high_differs = true;  // derived values come from a float multiply: nodes 0-58 compare with a tolerance, 59-108 must be off by > 0.1
      for (int i = 0; i < kN; ++i) {
        const Mat4& d = t.bones[t.objects[0].bones_first + i];
        if (i < 59 && !Near(d, draw_pose[i], 1e-4f)) low_equal = false;
        if (i >= 59 && std::fabs(d.m[12] - draw_pose[i].m[12]) < 0.1f) high_differs = false;
      }
      Check(low_equal && high_differs, "the plant fails exactly where the map measured it: nodes 0-58 equal, 59-108 differ");
    }
    {  // 2. the delta of sub_82BD3620 is not added to a consumer palette (the game added it to +200 before the draw read it), and still is to a derived one
      WrestlerContributor w;
      w.SubmitConsumer(0x1000, kN, draw_pose.data(), 7);
      w.ApplyDelta(0x1000, 1.f, 2.f, 3.f);
      Counters c;
      SceneTables t = EmitAt(w, c, 7);
      Check(t.objects.size() == 1 && bit_equal(t, 0, draw_pose), "ApplyDelta leaves a consumer palette alone (pending)");
      w.ApplyDelta(0x1000, 1.f, 2.f, 3.f);
      Counters c2;
      SceneTables t2 = EmitAt(w, c2, 8);
      Check(t2.objects.size() == 1 && bit_equal(t2, 0, draw_pose), "ApplyDelta leaves a consumer palette alone (persistent)");
    }
    {  // 3. frame pairing for the consumer palette: a frame-8 read does not leak into the record of frame 7
      WrestlerContributor w;
      w.SubmitConsumer(0x1000, kN, pa.data(), 7);
      w.SubmitConsumer(0x1000, kN, pb.data(), 8);
      Counters c;
      SceneTables t = EmitAt(w, c, 7);
      Check(PaletteIs(t, 0, pa, 0.f) && !PaletteIs(t, 0, pb, 0.f), "record of frame 7 uses the frame-7 consumer read");
    }
    {  // 4. engine objects: parallel to objects, the skeleton's address, padded for classes before this one
      WrestlerContributor w;
      submit_exit(w, 0x1000, 0, 7);
      w.SubmitConsumer(0x2000, 12, Pose(12, 0.5f).data(), 7);
      SceneTables t;
      t.objects.push_back(Object{});  // an earlier class's object: its row must be 0
      t.engine_objects.assign(1, 0u);
      FrameInfo f;
      f.guest_frame = 7;
      Counters c;
      w.Emit(f, t, c);
      Check(t.objects.size() == 3 && t.engine_objects.size() == 3 && t.engine_objects[0] == 0 && t.engine_objects[1] == 0x1000 && t.engine_objects[2] == 0x2000,
            "engine objects: earlier class 0, then each skeleton's guest address, parallel to objects");
      SceneTables t2;  // no earlier class and an empty engine column: still parallel after Emit
      WrestlerContributor w2;
      submit_exit(w2, 0x1000, 0, 7);
      Counters c2;
      w2.Emit(f, t2, c2);
      Check(t2.objects.size() == 1 && t2.engine_objects.size() == 1 && t2.engine_objects[0] == 0x1000, "engine objects on an empty table");
      std::string err;
      SceneTables back;
      auto bytes = SerializeRecord(t);
      Check(ParseRecord(bytes.data(), bytes.size(), back, err) && back.engine_objects == t.engine_objects, "the engine-object section round-trips through the record");
      SceneTables bad = t;
      bad.engine_objects.pop_back();  // planted: not parallel
      auto bbytes = SerializeRecord(bad);
      SceneTables bback;
      Check(!ParseRecord(bbytes.data(), bbytes.size(), bback, err), "planted twin: a non-parallel engine column is refused by the reader");
      SceneTables none;  // nothing emitted: the column stays empty (no section written)
      WrestlerContributor w3;
      Counters c3;
      w3.Emit(f, none, c3);
      Check(none.objects.empty() && none.engine_objects.empty(), "no skeleton: no objects, no engine column");
    }
    {  // 5. the hook's decision (ModelEntry) against a fake guest
      struct Guest { std::map<uint32_t, uint32_t> words; std::map<uint32_t, std::vector<Mat4>> mats; };
      Guest g;
      const uint32_t kModel = 0x40000000, kPal = 0x40010000, kPieceModel = 0x40020000, kOther = 0x40030000;
      g.words[kModel + 200] = kPal;
      g.words[kPieceModel + 200] = kPal;  // a hair / pad piece shares its owner's +200
      g.words[kOther + 200] = kPal + 0x100;
      // The array holds the skeleton's nodes, then ANOTHER skeleton's nodes right after (the map's Caution): the copy must stop at the node count.
      std::vector<Mat4> arr = draw_pose;
      arr.insert(arr.end(), pb.begin(), pb.end());
      g.mats[kPal] = arr;
      auto rw = [&](uint32_t a, uint32_t& v) { auto it = g.words.find(a); if (it == g.words.end()) return false; v = it->second; return true; };
      auto rm = [&](uint32_t a, uint32_t n, std::vector<Mat4>& out) {
        auto it = g.mats.find(a);
        if (it == g.mats.end() || it->second.size() < n) return false;
        out.assign(it->second.begin(), it->second.begin() + n);
        return true;
      };
      std::vector<Mat4> scratch;
      WrestlerContributor w;
      wwe2k14::wrestlers::ModelEntry(w, kModel, kPal, 7, rw, rm, scratch);  // before the update hook ever saw it: unknown model, no copy
      Check(w.ConsumerSubmits() == 0 && w.EntryUnknownModel() == 1, "a model the update hook has not seen makes no copy and is counted");
      submit_exit(w, kModel, 0, 7);  // the update hook now knows the node count (109)
      Check(w.WantCount(kModel) == uint32_t(kN), "the node count comes from the update hook (109)");
      wwe2k14::wrestlers::ModelEntry(w, kModel, kPal, 7, rw, rm, scratch);
      Counters c;
      SceneTables t = EmitAt(w, c, 7);
      Check(w.ConsumerSubmits() == 1 && t.objects.size() == 1 && bit_equal(t, 0, draw_pose), "ModelEntry copies the palette the draw reads, bit-exact");
      Check(t.objects[0].bones_count == uint32_t(kN), "the palette length is the skeleton's node count, not where the rows stop matching (rows after it are another skeleton's)");
      wwe2k14::wrestlers::ModelEntry(w, kPieceModel, kPal, 7, rw, rm, scratch);
      Check(w.EntryPieces() == 1 && w.EntryUnknownModel() == 1 && w.ConsumerSubmits() == 1, "a piece (unknown model, r5 = a known skeleton's palette) is counted apart, no copy");
      submit_exit(w, kOther, 1, 7);
      wwe2k14::wrestlers::ModelEntry(w, kOther, kPal, 7, rw, rm, scratch);  // known now, but [kOther+200] = kPal+0x100 != r5
      Check(w.EntryPtrMismatch() == 1 && w.ConsumerSubmits() == 1, "planted: r5 != [model+200] is refused and counted (positive control: the equal pointer was accepted above)");
      g.mats.erase(kPal);
      wwe2k14::wrestlers::ModelEntry(w, kModel, kPal, 8, rw, rm, scratch);
      Check(w.EntryUnreadable() == 1 && w.ConsumerSubmits() == 1, "an unreadable array is counted, nothing is submitted");
    }
    {  // 5b. lane 209: a WANTED own-skeleton piece the update hook never reports (Rock Retro's pads 10 nodes, Diesel's hair 48) is recorded at the palette hook
      struct Guest { std::map<uint32_t, uint32_t> words; std::map<uint32_t, std::vector<Mat4>> mats; };
      Guest g;
      const uint32_t kBody = 0x40000000, kBodyPal = 0x40010000, kPads = 0x50000000, kPadsPal = 0x50010000, kHair = 0x50020000, kHairPal = 0x50030000, kHairOther = 0x50040000,
                     kUnwanted = 0x50050000, kUnwantedPal = 0x50060000, kBorrower = 0x50070000;
      g.words[kBody + 200] = kBodyPal;
      g.words[kPads + 200] = kPadsPal;
      g.words[kHair + 200] = kHairPal;
      g.words[kUnwanted + 200] = kUnwantedPal;
      g.words[kBorrower + 200] = kBodyPal;
      const std::vector<Mat4> pads_pose = Pose(10, 0.31f), hair_pose = Pose(48, 0.47f), body_pose = Pose(109, 0.11f);
      g.mats[kBodyPal] = body_pose;
      g.mats[kPadsPal] = pads_pose;
      g.mats[kHairOther] = hair_pose;   // r5 != [kHair+200]: the draw reads another array
      g.mats[kUnwantedPal] = pads_pose;
      auto rw = [&](uint32_t a, uint32_t& v) { auto it = g.words.find(a); if (it == g.words.end()) return false; v = it->second; return true; };
      auto rm = [&](uint32_t a, uint32_t n, std::vector<Mat4>& out) {
        auto it = g.mats.find(a);
        if (it == g.mats.end() || it->second.size() < n) return false;
        out.assign(it->second.begin(), it->second.begin() + n);
        return true;
      };
      std::vector<Mat4> scratch;
      WrestlerContributor w;
      submit_exit(w, kBody, 0, 7);   // the update hook knows the body
      wwe2k14::wrestlers::ModelEntry(w, kBody, kBodyPal, 7, rw, rm, scratch);   // its palette pointer is now a known skeleton's
      w.WantSkeleton(kPads, 10);
      w.WantSkeleton(kHair, 48);
      Check(w.WantedCount(kPads) == 10 && w.WantedCount(kHair) == 48 && w.WantedCount(kUnwanted) == 0, "WantedCount returns the wanted count, 0 for an unwanted model");
      wwe2k14::wrestlers::ModelEntry(w, kPads, kPadsPal, 7, rw, rm, scratch);
      Check(w.OwnSkelConsumer() == 1 && w.OwnSkelPtrMismatch() == 0 && w.ConsumerSubmits() == 2, "a wanted model the update hook never saw is submitted at the palette hook");
      Counters c;
      SceneTables t = EmitAt(w, c, 7);
      int pads_idx = -1;
      for (size_t i = 0; i < t.objects.size(); ++i) if (t.objects[i].bones_count == 10) pads_idx = int(i);
      Check(pads_idx >= 0 && bit_equal(t, size_t(pads_idx), pads_pose), "... with the wanted count (10 nodes) and r5's rows, bit-exact");
      Check(w.WantCount(kPads) == 0, "... and the model stays unknown to the update hook, so the branch keeps applying");
      wwe2k14::wrestlers::ModelEntry(w, kPads, kPadsPal, 8, rw, rm, scratch);
      Check(w.OwnSkelConsumer() == 2, "the next draw is recorded again (the branch keeps applying)");
      wwe2k14::wrestlers::ModelEntry(w, kUnwanted, kUnwantedPal, 7, rw, rm, scratch);
      Check(w.OwnSkelConsumer() == 2 && w.ConsumerSubmits() == 3 && w.EntryUnknownModel() == 1, "an unwanted unknown model is not recorded (counted as unknown)");
      w.WantSkeleton(kBorrower, 10);
      wwe2k14::wrestlers::ModelEntry(w, kBorrower, kBodyPal, 7, rw, rm, scratch);   // wanted, but r5 is the body's palette: a name-mapped borrower stays on the piece route
      Check(w.OwnSkelConsumer() == 2 && w.EntryPieces() == 1 && w.ConsumerSubmits() == 3, "a borrower (r5 = a known skeleton's palette pointer) is not recorded, even when wanted");
      wwe2k14::wrestlers::ModelEntry(w, kHair, kHairOther, 7, rw, rm, scratch);   // r5 != [kHair+200]
      Check(w.OwnSkelConsumer() == 3 && w.OwnSkelPtrMismatch() == 1, "r5 != [model+200] is recorded (r5 is what the draw reads) and counted apart");
      Counters c2;
      SceneTables t2 = EmitAt(w, c2, 7);
      int hair_idx = -1;
      for (size_t i = 0; i < t2.objects.size(); ++i) if (t2.objects[i].bones_count == 48) hair_idx = int(i);
      Check(hair_idx >= 0 && bit_equal(t2, size_t(hair_idx), hair_pose), "... with r5's rows and the wanted 48 nodes");
      g.mats.erase(kPadsPal);
      wwe2k14::wrestlers::ModelEntry(w, kPads, kPadsPal, 9, rw, rm, scratch);
      Check(w.EntryUnreadable() == 1 && w.OwnSkelConsumer() == 3, "an unreadable array on the new branch is counted, nothing is submitted");
      Check(w.FirstOwnSkelEntry(0x70000) && !w.FirstOwnSkelEntry(0x70000), "the one-time diagnostic fires once per model");
      Check(w.NoteWantedHookSaw(kHair, 50, 48) && !w.NoteWantedHookSaw(kHair, 50, 48) && w.WantedHookSaw(kHair) && !w.WantedHookSaw(kPads), "the update-hook diagnostic fires once per address when the count differs");
    }
    {  // 6. the asset link: declared goes into the record (one AssetRef per distinct link); a guess never does
      WrestlerContributor w;
      submit_exit(w, 0x1000, 0, 7);
      submit_exit(w, 0x2000, 1, 7);
      submit_exit(w, 0x3000, -1, 7);
      w.SubmitAssetLink(0x1000, "pac\\ch\\CH001.PAC", "", 2);
      w.SubmitAssetLink(0x2000, "pac\\ch\\CH001.PAC", "", 2);  // same link: one AssetRef
      w.SubmitAssetGuess(0x3000, "pac\\ch\\CH009.PAC", "", 0);   // a guess: kept, counted, not in the record
      Counters c;
      SceneTables t = EmitAt(w, c, 7);
      const AssetRef* a0 = t.objects.size() == 3 && t.objects[0].asset < t.assets.size() ? &t.assets[t.objects[0].asset] : nullptr;
      Check(a0 && t.Str(a0->pac) == "pac\\ch\\CH001.PAC" && a0->variant == 2 && a0->kind == uint8_t(AssetKind::Model), "a declared link gives the object its real AssetRef");
      Check(a0 && t.objects[1].asset == t.objects[0].asset, "two objects with the same declared link share one AssetRef");
      Check(t.objects.size() == 3 && t.assets[t.objects[2].asset].pac == 0 && t.assets[t.objects[2].asset].variant == 0xFFFFFFFFu,
            "a guessed link NEVER enters the record: the object keeps the placeholder (unknown stays unknown)");
      Check(w.GuessedLinks() == 1 && w.GuessFor(0x3000).known && w.GuessFor(0x3000).pac == "pac\\ch\\CH009.PAC" && !w.GuessFor(0x1000).known,
            "the guess is counted and kept for the draw side");
      // 3 objects x (mask) + 1 unlinked skeleton + 1 object with a placeholder asset (0x3000) + light x3 + derived palettes x3
      Check(c.unknown == 3 + 1 + 1 + 3 + 3, "unknown counts: declared links do not count an asset unknown, the guessed one does");
      std::string err;
      SceneTables back;
      auto bytes = SerializeRecord(t);
      Check(ParseRecord(bytes.data(), bytes.size(), back, err), "a record with declared asset links parses back");
    }
    {  // 7. the declared route: ModelLinkRead against a fake reader / descriptor / piece array (R1 sections 1-3)
      std::map<uint32_t, uint32_t> g;
      auto rw = [&](uint32_t a, uint32_t& v) { auto it = g.find(a); if (it == g.end()) return false; v = it->second; return true; };
      const uint32_t kReader = 0x50000000, kDesc = 0x50001000, kBody = 0x1000, kPieces = 0x50002000;
      g[kReader + 68] = 0x60000000, g[kReader + 72] = 4096, g[kReader + 96] = kDesc;
      g[kDesc + 28] = 12345;  // character 123, attire 4, kind 5? -> key 12345: (12345 % 100) / 10 = 4, kind 5, character 123
      g[kDesc + 4] = kBody, g[kDesc + 12] = kPieces;
      // desc+16 = 4 entries (sub_823EDE50 stores the count there): hair 10000, a NULL hole (the constructor zeroes models), pad 26, an id outside R1's list 777
      g[kDesc + 16] = 4;
      const uint32_t ids[4] = {10000, 0, 26, 777}, models[4] = {0x70000000, 0, 0x70000100, 0x70000200};
      for (int i = 0; i < 4; ++i) g[kPieces + 24 * i + 4] = models[i], g[kPieces + 24 * i + 8] = ids[i] << 16;
      g[kPieces + 96 + 4] = 0x70000999, g[kPieces + 96 + 8] = 10003u << 16;  // beyond the count: must NOT be read
      WrestlerContributor w;
      wwe2k14::wrestlers::ModelLinkRead(w, kReader, rw);
      auto info = w.ModelKeyFor(kBody);
      Check(info.known && info.key == 12345 && info.pach_buf == 0x60000000 && info.pach_size == 4096 && info.desc == kDesc, "link: key, PACH buffer, size and descriptor read from the reader");
      Check(WrestlerContributor::AttireOfKey(info.key) == 4 && WrestlerContributor::KindOfKey(info.key) == 5 && WrestlerContributor::CharacterOfKey(info.key) == 123,
            "key arithmetic: attire = (key % 100) / 10, kind = key % 10, character = key / 100");
      Check(info.pieces.size() == 3 && info.pieces[0].id == 10000 && info.pieces[1].model == 0x70000100 && info.pieces[2].id == 777, "three pieces read, ids from the high half of the word at +8");
      Check(w.LinkBodies() == 1 && w.LinkPieces() == 3 && w.LinkKind(5) == 1 && w.LinkBadPiece() == 1, "counts: one body, three pieces, kind 5 once, one id outside R1's list");
      Check(w.OwnerOfPiece(0x70000100) == kBody && w.OwnerOfPiece(0x7FFFFFFF) == 0, "a piece model maps to its body through the descriptor, an unknown model to none");
      // the model shows up in a record: a pac-less AssetRef (pac unknown) whose entry names the key and whose variant is the attire
      submit_exit(w, kBody, 0, 7);
      Counters c;
      SceneTables t = EmitAt(w, c, 7);
      const AssetRef* a = t.objects.size() == 1 ? &t.assets[t.objects[0].asset] : nullptr;
      Check(a && a->pac == 0 && t.Str(a->entry) == "model_key:12345" && a->variant == 4 && t.engine_objects.size() == 1 && t.engine_objects[0] == kBody,
            "the record carries the key as entry and the attire as variant with the pac name left unknown, and the body as the engine object");
      Check(c.unknown == 2 + 1 + 1, "a key without a pac name still counts the asset unknown: (asset, mask) + light + derived palette = 4");
      // a piece entry (unknown model to the pose hooks) is counted as a piece BY THE DESCRIPTOR
      std::vector<Mat4> scratch;
      auto rm = [&](uint32_t, uint32_t, std::vector<Mat4>&) { return false; };
      wwe2k14::wrestlers::ModelEntry(w, 0x70000100, 0x40000000, 7, rw, rm, scratch);
      Check(w.EntryPieces() == 1 && w.EntryPiecesByDescriptor() == 1 && w.EntryUnknownModel() == 0, "sub_82C04DF0 on a piece model: counted a piece through the descriptor");
      // refusals, each counted and none submitted
      { WrestlerContributor w2; auto g2 = g; g2[kDesc + 28] = 51200; auto r2 = [&](uint32_t a2, uint32_t& v) { auto it = g2.find(a2); if (it == g2.end()) return false; v = it->second; return true; };
        wwe2k14::wrestlers::ModelLinkRead(w2, kReader, r2);
        Check(w2.LinkEmptyKey() == 1 && w2.LinkBodies() == 0, "an empty key (51200) is counted, nothing linked"); }
      { WrestlerContributor w2; auto g2 = g; g2[kDesc + 4] = 0; auto r2 = [&](uint32_t a2, uint32_t& v) { auto it = g2.find(a2); if (it == g2.end()) return false; v = it->second; return true; };
        wwe2k14::wrestlers::ModelLinkRead(w2, kReader, r2);
        Check(w2.LinkNoBody() == 1 && w2.LinkBodies() == 0, "a descriptor without a body model is counted, nothing linked"); }
      { WrestlerContributor w2; auto g2 = g; g2.erase(kReader + 96); auto r2 = [&](uint32_t a2, uint32_t& v) { auto it = g2.find(a2); if (it == g2.end()) return false; v = it->second; return true; };
        wwe2k14::wrestlers::ModelLinkRead(w2, kReader, r2);
        Check(w2.LinkUnreadable() == 1 && w2.LinkBodies() == 0, "an unreadable reader is counted, nothing linked"); }
      { WrestlerContributor w2; auto g2 = g; g2[kDesc + 16] = 200;  // planted: a count that is not a piece array
        for (uint32_t i = 4; i < 200; ++i) g2[kPieces + 24 * i + 4] = 0x70001000 + i, g2[kPieces + 24 * i + 8] = 10002u << 16;
        auto r2 = [&](uint32_t a2, uint32_t& v) { auto it = g2.find(a2); if (it == g2.end()) return false; v = it->second; return true; };
        wwe2k14::wrestlers::ModelLinkRead(w2, kReader, r2);
        Check(w2.ModelKeyFor(kBody).pieces.empty() && w2.LinkCountRefused() == 1 && w2.LinkBodies() == 1, "planted: n > kMaxPieces is refused and counted, the body is still linked"); }
      Check(info.pieces.size() == 3 && w.OwnerOfPiece(0x70000999) == 0, "the walk follows desc+16 (4 entries, one null hole skipped): the entry beyond the count is not read");
    }
    {  // 8. review fixes: source priority independent of the tags, state reset on a rebuilt model, bookkeeping bounded
      WrestlerContributor w;
      std::vector<Mat4> exit_a = pa, draw_a = pb;
      auto world_for = [&](const std::vector<Mat4>& skin) { std::vector<Mat4> wv(kN); for (int i = 0; i < kN; ++i) wv[i] = RefMul(Trans(1.f * i, 0.5f * i, 2.f), skin[i]); return wv; };
      w.SubmitConsumer(0x1000, kN, draw_a.data(), 10);                                                  // the draw read, tag 10
      w.SubmitWorld(0x1000, 0, kN, world_for(exit_a).data(), inv.data(), 11);                           // the exit hook, tag 11 (different thread)
      Counters c;
      SceneTables t = EmitAt(w, c, 11);
      Check(t.objects.size() == 1 && PaletteIs(t, 0, draw_a, 0.f) && c.unknown == 3 && t.Str(t.objects[0].id) == "actor:slot:0",
            "consumer at N, derived at N+1, record N+1: the consumer palette stands (not the newer-tagged derived one), slot from the update hook");
      WrestlerContributor w2;  // the derived palette raced ahead and was stored first: the consumer read still wins
      w2.SubmitWorld(0x1000, 0, kN, world_for(exit_a).data(), inv.data(), 11);
      w2.SubmitConsumer(0x1000, kN, draw_a.data(), 10);
      Counters c2;
      SceneTables t2 = EmitAt(w2, c2, 11);
      Check(t2.objects.size() == 1 && PaletteIs(t2, 0, draw_a, 0.f), "derived N+1 stored before consumer N: purged by the consumer read");
      WrestlerContributor w3;  // the consumer hook goes silent: after kConsumerFresh frames the derived palette is used again (and counted unknown)
      w3.SubmitConsumer(0x1000, kN, draw_a.data(), 10);
      w3.SubmitWorld(0x1000, 0, kN, world_for(exit_a).data(), inv.data(), 10 + WrestlerContributor::kConsumerFresh + 1);
      Counters c3;
      SceneTables t3 = EmitAt(w3, c3, 20);
      Check(t3.objects.size() == 1 && PaletteIs(t3, 0, exit_a, 1e-4f) && c3.unknown == 4, "a consumer that went quiet more than kConsumerFresh frames ago no longer outranks the derived palette");
      // a rebuilt model at the same address starts clean: the old count, slot and link are gone
      WrestlerContributor w4;
      w4.SubmitSkin(0x1000, 2, kN, pa.data(), 5);
      w4.SubmitAssetLink(0x1000, "pac\\ch\\OLD.PAC", "", 1);
      Check(w4.WantCount(0x1000) == uint32_t(kN), "count known before the rebuild");
      w4.SubmitModelLink(0x1000, 999, 0x1, 0x2, 0x3, {});
      Check(w4.WantCount(0x1000) == 0 && w4.ModelKeyFor(0x1000).key == 999, "SubmitModelLink resets the node count and takes the new key");
      Counters c4;
      SceneTables t4 = EmitAt(w4, c4, 5);
      Check(t4.objects.empty(), "the dead object's pending palette is not emitted for the rebuilt model");
      w4.SubmitSkin(0x1000, -1, kN, pb.data(), 6);
      t4 = EmitAt(w4, c4, 6);
      Check(t4.objects.size() == 1 && t4.Str(t4.objects[0].id) == "skel:00001000" && t4.assets[t4.objects[0].asset].pac == 0 && t4.Str(t4.assets[t4.objects[0].asset].entry) == "model_key:999",
            "after the rebuild the old slot (actor:slot:2) and the old declared pac link are gone; the new key is what the record carries");
      WrestlerContributor w5;  // latest count, not a high-water mark
      w5.SubmitSkin(0x1000, 0, kN, pa.data(), 5);
      w5.SubmitSkin(0x1000, 0, 70, pa.data(), 6);
      Check(w5.WantCount(0x1000) == 70u, "the node count is the latest, not the largest");
      // the bookkeeping table is bounded and evicts instead of refusing when full; Emit ages out what is not in the record
      WrestlerContributor w6;
      for (uint32_t i = 0; i < 200; ++i) w6.SubmitModelLink(0x20000 + i * 0x10, 100 + i, 0, 0, 0, {});
      Check(w6.KnownCount() <= 2 * WrestlerContributor::kMaxSkeletons && w6.ModelKeyFor(0x20000 + 199 * 0x10).key == 299 && w6.LinkBodies() == 200 && w6.LinkUnreadable() == 0,
            "links made while no record is written (no Emit) fill the table to its bound, then evict the least recently used: none is lost");
      Counters c6;
      for (uint32_t i = 0; i < WrestlerContributor::kKnownAge + 2; ++i) { SceneTables tt; FrameInfo f; f.guest_frame = i; w6.Emit(f, tt, c6); }
      Check(w6.KnownCount() == 0, "bookkeeping of models that never reach the record ages out in Emit");
      // O(1) piece owner map: rebuilt link drops the old pieces
      WrestlerContributor w7;
      w7.SubmitModelLink(0x1000, 123, 0, 0, 0, {{0x70000000, 10000}, {0x70000100, 26}});
      Check(w7.OwnerOfPiece(0x70000100) == 0x1000, "piece owner map filled by the link");
      w7.SubmitModelLink(0x1000, 124, 0, 0, 0, {{0x70000200, 10000}});
      Check(w7.OwnerOfPiece(0x70000100) == 0 && w7.OwnerOfPiece(0x70000200) == 0x1000, "a rebuilt body drops its old pieces from the owner map");
      // linked body seen as a skeleton
      WrestlerContributor w8;
      w8.SubmitModelLink(0x1000, 123, 0, 0, 0, {});
      w8.SubmitSkin(0x1000, 0, kN, pa.data(), 5);
      Counters c8;
      EmitAt(w8, c8, 5);
      EmitAt(w8, c8, 6);
      Check(w8.LinkedSeenAsSkeleton() == 1, "a linked body that turns up as a skeleton is counted once");
    }
  }
  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  wrestler_emit selftest\n");
  return g_fail ? 1 : 0;
}

static int EmitMode(const char* in, const char* out, const std::string& fault) {
  SceneTables t;
  std::string err;
  if (!ReadRecord(in, t, err)) { std::printf("FAIL  cannot read %s: %s\n", in, err.c_str()); return 2; }
  WrestlerContributor w;
  w.SetFirstWinsForTest(fault == "first");
  w.SetLightSet(0xFFFFFFFFu);
  std::vector<size_t> skinned;
  for (size_t i = 0; i < t.objects.size(); ++i)
    if ((t.objects[i].flags & kObjSkinned) && t.objects[i].bones_count > 0) skinned.push_back(i);
  if (skinned.empty()) { std::printf("FAIL  %s holds no skinned object (the viewer placed no posed wrestler?)\n", in); return 2; }
  const uint64_t kFrame = 7;
  for (size_t k = 0; k < skinned.size(); ++k) {
    const Object& o = t.objects[skinned[k]];
    const uint32_t n = o.bones_count;
    std::vector<Mat4> real(t.bones.begin() + o.bones_first, t.bones.begin() + o.bones_first + n), inv(n), bind(n), world(n), off(n);
    for (uint32_t i = 0; i < n; ++i) {
      inv[i] = Trans(-0.25f * i, -0.5f, -0.125f);  // exactly representable
      bind[i] = IdentityMat4();
      off[i] = real[i];
      off[i].m[12] += 3.f;  // a pose one step off
    }
    auto submit = [&](const std::vector<Mat4>& skin, uint64_t frame) {
      for (uint32_t i = 0; i < n; ++i) world[i] = RefMul(Trans(0.25f * i, 0.5f, 0.125f), skin[i]);  // world = T(+t) x skin (independent multiply)
      w.SubmitWorld(0x1000 + uint32_t(k), int32_t(k), n, world.data(), inv.data(), frame);
    };
    submit(bind, kFrame);
    if (fault != "bind") {
      // the exit of sub_82BD30E8 (derived): a pose that is not what the draw reads. The --fault exit arm uses a larger miss (12 units on x and z) so a small figure on screen still shows it.
      if (fault == "exit") for (uint32_t i = 0; i < n; ++i) off[i].m[12] += 9.f, off[i].m[14] += 12.f;
      submit(off, kFrame);
      // The draw's own read (sub_82C04DF0's entry) delivers the real pose. --fault exit: that hook is silent, the exit palette stands (Brock's bug).
      if (fault != "exit") w.SubmitConsumer(0x1000 + uint32_t(k), n, real.data(), kFrame);
    }
    submit(bind, kFrame + 1);  // thread 7 already in the next frame: must not leak into this record
  }
  Counters c;
  FrameInfo f;
  f.guest_frame = kFrame;
  SceneTables e;
  w.Emit(f, e, c);
  if (e.objects.size() != skinned.size()) { std::printf("FAIL  emitted %zu objects for %zu skinned objects\n", e.objects.size(), skinned.size()); return 2; }

  // The live record as Emit made it (no patching), with the input's camera: rsc_draw must take it (asset unknown objects are skipped).
  {
    SceneTables live = e;
    live.header.source = uint8_t(Source::Live);
    live.has_camera = t.has_camera;
    live.camera = t.camera;
    if (!WriteRecord(std::string(out) + ".live.rsc", live, err)) { std::printf("FAIL  cannot write live record: %s\n", err.c_str()); return 2; }
  }
  // The viewer record with each skinned object replaced by Emit's object; slot k = the k-th skinned object, so ids are actor:slot:k and in input order.
  for (size_t k = 0; k < skinned.size(); ++k) {
    Object& o = t.objects[skinned[k]];
    const Object eo = e.objects[k];
    if (eo.bones_count != o.bones_count) { std::printf("FAIL  object %zu: palette %u nodes, emitted %u\n", k, o.bones_count, eo.bones_count); return 2; }
    Object patched = eo;                       // id, flags, palette, light set: Emit's own
    patched.asset = o.asset, patched.world = o.world;  // the unknown link (asset, placement) from the viewer's record
    patched.mesh_mask_lo = o.mesh_mask_lo, patched.mesh_mask_hi = o.mesh_mask_hi;
    patched.params_first = o.params_first, patched.params_count = o.params_count;
    patched.light_set = o.light_set;           // the viewer's evaluated set (the record's light sets are not part of Emit's output yet)
    patched.id = o.id;                          // keep the viewer's id string (it indexes the input's strings)
    patched.bones_first = o.bones_first;
    for (uint32_t i = 0; i < o.bones_count; ++i) t.bones[o.bones_first + i] = e.bones[eo.bones_first + i];
    o = patched;
  }
  if (!WriteRecord(out, t, err)) { std::printf("FAIL  cannot write %s: %s\n", out, err.c_str()); return 2; }
  std::printf("emitted %zu wrestler palettes through the contributor (fault: %s); objects %u unknown %u stale %u -> %s\n", skinned.size(),
              fault.empty() ? "none" : fault.c_str(), c.objects, c.unknown, c.stale, out);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "selftest")) return SelfTest();
  if (argc >= 4 && !std::strcmp(argv[1], "emit")) {
    std::string fault;
    for (int i = 4; i + 1 < argc; ++i)
      if (!std::strcmp(argv[i], "--fault")) fault = argv[i + 1];
    return EmitMode(argv[2], argv[3], fault);
  }
  std::fprintf(stderr, "usage: wrestler_emit selftest | emit IN.rsc OUT.rsc [--fault bind|first|exit]\n");
  return 2;
}
