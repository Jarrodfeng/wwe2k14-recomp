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
#include "../../src/renderer_scene/detached_piece.h"
#include "../../src/renderer_scene/bone_tag_rows.h"
#include "../../src/scene_live/model_hooks.h"
#include "../../src/scene_live/piece_link.h"        // S103 NRB
#include <set>
#include "../../src/renderer_scene/named_load_fail.h"   // S103 DLW
#include "../../src/scene_live/model_file_note.h"        // S110 SEATREL (was PACANS' pac_answer table)
#include "../../src/renderer_scene/detached_piece.h"    // S103 DTP

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
  f.guest_frame = frame ? frame + WrestlerContributor::kPaletteLead : 0;   // S103 W1: a palette tagged t is stream t + 1: record t + kPaletteLead takes it
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
      f.guest_frame = 7 + WrestlerContributor::kPaletteLead;   // S103 W1
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
      g[kDesc + 44] = 0, g[kDesc + 48] = 0, g[kDesc + 92] = 12345;   // S103 CMB: a stock attire (mode 0): the composite read says "base" (an unreadable descriptor would now be the refusal tag, a hole)
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
      {  // PZF2 A1: a link no latch has verified names nothing (fail closed); the verdict is per latch (Collect), so a pass must come before each Collect
        WrestlerContributor wu;
        wwe2k14::wrestlers::ModelLinkRead(wu, kReader, rw);
        submit_exit(wu, kBody, 0, 7);
        Counters cu;
        SceneTables tu = EmitAt(wu, cu, 7);
        Check(tu.objects.size() == 1 && tu.Str(tu.assets[tu.objects[0].asset].entry) != "model_key:12345" && !wu.BodyKeyFor(kBody).verified && wu.BodyKeyFor(kBody).known && wu.BodyKeyFor(kBody).desc == kDesc && wu.BodyKeyFor(kBody).key == 12345,
              "PZF2 A1: a body link no latch verified is NOT named in the record (twin: the old trust-the-link rule named it); B-4: BodyKeyFor gives desc + key only");
      }
      w.VerifyBodyLink(kBody, true);
      Counters c;
      SceneTables t = EmitAt(w, c, 7);
      const AssetRef* a = t.objects.size() == 1 ? &t.assets[t.objects[0].asset] : nullptr;
      Check(a && t.Str(a->pac) == "pac\\ch\\ch123.pac" && t.Str(a->entry) == "model_key:12345" && a->variant == 4 && t.engine_objects.size() == 1 && t.engine_objects[0] == kBody,
            "the record carries the key as entry, the pac named from the key (S103 B) and the attire as variant, and the body as the engine object");
      Check(c.unknown == 1 + 1 + 1, "a key with a derived pac name counts only the mask unknown: mask + light + derived palette = 3 (S103 B)");
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
      {  // PZF2 A2: the body's own node count is read at the build ([model+300], vtable 0x8206AF90, [model+296] != 0) and seeds the link: an unseen link whose address is reused by another object is dropped
        auto g2 = g;
        g2[kBody + 0] = scene_inputs::kModelVtable, g2[scene_inputs::kModelVtable + 8] = scene_inputs::kCountGetter, g2[kBody + 296] = 1, g2[kBody + 300] = 109;
        auto r2 = [&](uint32_t a2, uint32_t& v) { auto it = g2.find(a2); if (it == g2.end()) return false; v = it->second; return true; };
        WrestlerContributor wa;
        wwe2k14::wrestlers::ModelLinkRead(wa, kReader, r2);
        wa.SubmitSkin(kBody, 6, 70, pa.data(), 5);   // another object (70 nodes) at the address, the first thing the update hook ever reports
        Check(!wa.ModelKeyFor(kBody).known && wa.OwnerOfPiece(0x70000100) == 0 && wa.BodyLinkDroppedCount() == 1, "PZF2 A2: a link seeded with the build's node count 109 is dropped by the first update-hook report of another count (70), pieces with it");
        WrestlerContributor wb;
        wwe2k14::wrestlers::ModelLinkRead(wb, kReader, r2);
        wb.SubmitSkin(kBody, 6, kN, pa.data(), 5);
        Check(wb.ModelKeyFor(kBody).known && wb.BodyLinkDroppedCount() == 0, "PZF2 A2: the same count (109) keeps the link");
        WrestlerContributor wc;   // planted twin: no count readable at the build (the old rule): the first report is adopted, whatever it is
        wwe2k14::wrestlers::ModelLinkRead(wc, kReader, rw);
        wc.SubmitSkin(kBody, 6, 70, pa.data(), 5);
        Check(wc.ModelKeyFor(kBody).known && wc.BodyLinkDroppedCount() == 0, "PZF2 A2 planted twin (no game count at the build): the other object's count is adopted and the link stays: the hole A2 closes");
      }
      {  // S103 CMB: the composite attire's tag lives INSIDE the body's link, so every drop of the link drops the colours (a reused body address must NEVER get Heenan's colours)
        const std::string tag = "/c:" + std::string(96, '1');
        const uint32_t kHeenan = 0x60000000;
        {
          WrestlerContributor wt;
          wt.SubmitModelLink(kHeenan, 24902, 0, 0, 0x3, {}, 109, tag);
          Check(wt.ModelKeyFor(kHeenan).tag == tag && WrestlerContributor::KeyLink(24902, tag).entry == "model_key:24902" + tag && WrestlerContributor::KeyLink(24902).entry == "model_key:24902",
                "CMB: the link carries the tag; the composite entry differs from the stock attire's of the same loaded key; the plain KeyLink (the slot_key fallback) stays the plain base");
          wt.SubmitModelLink(kHeenan, 24902, 0, 0, 0x3, {}, 109);   // a relink at the address without a tag (the stock attire, or a base / refused read)
          Check(wt.ModelKeyFor(kHeenan).tag.empty(), "CMB drop path (relink): the old colours are gone");
        }
        {  // S110 SEATREL (was PACANS): the file the game's model was BUILT from, noted per slot descriptor (a seated slot: the game built slot 397's model from CH398's pac)
          const uint32_t kRd = 0x70001000, kDsc = 0x71000000;   // the reader of the read, the slot descriptor it built
          model_file_note::Clear();
          rdr_scene::namedfail::Clear();
          model_file_note::NoteRead(kRd, "/EMD/00039702", "DLC4:\\PAC\\CH\\CH398_DLC06.PAC");
          model_file_note::NoteBuild(kDsc, 39702, kRd);
          const WrestlerContributor::Link ls = WrestlerContributor::KeyLink(39702, std::string(), kDsc);
          Check(ls.pac == "pac\\ch\\ch398.pac" && ls.entry == "model_key:39802" && ls.variant == 0, "SEATREL: the descriptor the game built from CH398's pac: KeyLink(39702, desc) names pac\\ch\\ch398.pac / model_key:39802");
          Check(WrestlerContributor::KeyLink(39702).pac == "pac\\ch\\ch397.pac" && WrestlerContributor::KeyLink(39702, std::string(), kDsc + 192).pac == "pac\\ch\\ch397.pac", "SEATREL: no descriptor, or another one: the id rule (a probe lookup names nothing)");
          Check(WrestlerContributor::KeyLink(39802, std::string(), kDsc).pac == "pac\\ch\\ch398.pac" && WrestlerContributor::KeyLink(39802, std::string(), kDsc).entry == "model_key:39802", "SEATREL: another key (398's own) on that descriptor keeps the id rule");
          Check(WrestlerContributor::KeyLink(39712, std::string(), kDsc).pac == "pac\\ch\\ch397.pac", "SEATREL: another attire of 397 on that descriptor (the note is for key 39702 only): the id rule");
          rdr_scene::namedfail::Note("pac\\ch\\ch398.pac", "model_key:39802", 0);
          Check(WrestlerContributor::KeyFailed(39702, std::string(), kDsc) && !WrestlerContributor::KeyFailed(39702), "SEATREL: KeyFailed sees the failure of ch398 (the file the game built from) for the descriptor only");
          model_file_note::NoteRead(kRd, "/EMD/00039702", "DLC4:\\PAC\\CH\\CH397_DLC05.PAC");   // the next read took CH397 again, but the game has not built from it yet
          Check(WrestlerContributor::KeyLink(39702, std::string(), kDsc).pac == "pac\\ch\\ch398.pac", "SEATREL: a read that built nothing changes the descriptor's answer not at all");
          model_file_note::NoteBuild(kDsc, 39702, kRd);   // the rebuild
          const WrestlerContributor::Link lo = WrestlerContributor::KeyLink(39702, std::string(), kDsc);
          Check(lo.pac == "pac\\ch\\ch397.pac" && lo.entry == "model_key:39702", "SEATREL: after the rebuild from CH397's pac the answer is back to the id rule");
          model_file_note::Clear();
          rdr_scene::namedfail::Clear();
        }
        {
          WrestlerContributor wt;   // the node-count drop (another object built at the address)
          wt.SubmitModelLink(kHeenan, 24902, 0, 0, 0x3, {}, 109, tag);
          wt.SubmitSkin(kHeenan, 6, 70, pa.data(), 5);
          Check(!wt.ModelKeyFor(kHeenan).known && wt.ModelKeyFor(kHeenan).tag.empty(), "CMB drop path (node count): link and colours gone");
        }
        {
          WrestlerContributor wt;   // the slot key of another character
          wt.SubmitModelLink(kHeenan, 24902, 0, 0, 0x3, {}, 109, tag);
          wt.SubmitSlotKey(kHeenan, 10402);
          Check(wt.ModelKeyFor(kHeenan).tag.empty() && !wt.ModelKeyFor(kHeenan).known, "CMB drop path (another character's slot key): colours gone");
          wt.SubmitModelLink(kHeenan, 24902, 0, 0, 0x3, {}, 109, tag);
          wt.SubmitSlotKey(kHeenan, 24912);
          Check(wt.ModelKeyFor(kHeenan).tag == tag, "CMB: the same character's slot key keeps the link and its colours");
        }
        {
          WrestlerContributor wt;   // LRU eviction (bounded table)
          wt.SubmitModelLink(kHeenan, 24902, 0, 0, 0x3, {}, 109, tag);
          for (uint32_t i = 1; i <= 4 * WrestlerContributor::kMaxSkeletons + 2; ++i) wt.SubmitModelLink(0x61000000 + i * 0x10, 100 + i, 0, 0, 0, {});
          Check(!wt.ModelKeyFor(kHeenan).known && wt.ModelKeyFor(kHeenan).tag.empty(), "CMB drop path (LRU eviction): colours gone");
        }
        {
          WrestlerContributor wt;   // ModelLinkRead end to end: a composite read stores the tag; a base read at the same address replaces it with none; an early return (empty key) stores nothing
          std::map<uint32_t, uint32_t> m;
          m[0x1000 + 96] = 0x2000, m[0x1000 + 100] = 0x30000, m[0x30000 + 16] = 0x40000, m[0x2000 + 28] = 24902, m[0x2000 + 4] = kHeenan, m[0x2000 + 44] = 1, m[0x2000 + 48] = 1, m[0x2000 + 92] = 24912;
          m[0x1000 + 68] = 0x7000, m[0x1000 + 72] = 16, m[0x2000 + 12] = 0, m[0x2000 + 16] = 0;   // the PACH buffer / size, no piece array
          for (int i = 0; i < 12; ++i) m[0x40000 + 132 + 1320 + 4 * i] = 0x01010101u * uint32_t(i + 1);
          auto rd = [&](uint32_t a2, uint32_t& v) { auto it = m.find(a2); if (it == m.end()) return false; v = it->second; return true; };
          wwe2k14::wrestlers::ModelLinkRead(wt, 0x1000, rd);

          uint8_t cols[48];
          const std::string t = wt.ModelKeyFor(kHeenan).tag;
          Check(attire_compose::ParseEntryTag(t, cols) && cols[0] == 1 && cols[47] == 12, "CMB: ModelLinkRead stores the tag of the 48 bytes at block + 132 set + 1320 inside the link");
          m[0x2000 + 44] = 0;   // the stock attire loaded into the same address
          wwe2k14::wrestlers::ModelLinkRead(wt, 0x1000, rd);
          Check(wt.ModelKeyFor(kHeenan).known && wt.ModelKeyFor(kHeenan).tag.empty(), "CMB: a mode 0 relink at the address carries no colours (a reused body address does not inherit Heenan's)");
          m[0x2000 + 44] = 1;
          wwe2k14::wrestlers::ModelLinkRead(wt, 0x1000, rd, nullptr, false);   // the switch off / the stream build: nothing is read, the link is the plain base
          Check(wt.ModelKeyFor(kHeenan).known && wt.ModelKeyFor(kHeenan).tag.empty(), "CMB fix 1: compose off (switch / stream build): no tag");
          m[0x2000 + 48] = 99;   // a colour set out of range: the hook could not read the colours of a composite attire
          wwe2k14::wrestlers::ModelLinkRead(wt, 0x1000, rd);
          Check(wt.ModelKeyFor(kHeenan).tag == "/r", "CMB fix 3: a refusal is carried as the refusal tag (the loader fails it: a hole), not as the plain base");
        }
      }
      {  // PZF2 A1: the descriptor check. mgr = [0x837635A4], descriptors at [mgr + 8] + 192 * i (12), [desc + 4] = the body, [desc + 28] = the key
        std::map<uint32_t, uint32_t> m;
        const uint32_t mgr = 0x51000000, arr = 0x51100000;
        m[scene_inputs::kSlotMgrPtr] = mgr, m[mgr + 8] = arr;
        for (uint32_t i = 0; i < 12; ++i) m[arr + 192 * i + 4] = 0x30000 + i, m[arr + 192 * i + 28] = 51200;
        m[arr + 192 * 3 + 4] = kBody, m[arr + 192 * 3 + 28] = 12345;
        static std::map<uint32_t, uint32_t>* g_m = nullptr;
        g_m = &m;
        const scene_inputs::ReadFn rdm = [](uint32_t a, void* dst, size_t n) -> bool {
          for (size_t k = 0; k < n; k += 4) {
            auto it = g_m->find(a + uint32_t(k));
            if (it == g_m->end()) return false;   // an unmapped word is unreadable here: every read the check makes is checked
            const uint32_t v = it->second, be = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
            std::memcpy(static_cast<uint8_t*>(dst) + k, &be, 4);
          }
          return true;
        };
        uint32_t d = 0, k = 0;
        Check(scene_inputs::DescriptorOfBody(rdm, kBody, d, k) && d == arr + 192 * 3 && k == 12345 && scene_inputs::VerifyBodyLinkAgainstDescriptors(rdm, kBody, 12345), "PZF2 A1: exactly one descriptor names the body and its key matches: verified");
        Check(!scene_inputs::VerifyBodyLinkAgainstDescriptors(rdm, kBody, 12335), "PZF2 A1 twin: the descriptor says another key (a twin / another attire at the address) -> not verified");
        Check(!scene_inputs::VerifyBodyLinkAgainstDescriptors(rdm, 0x1234, 12345), "PZF2 A1 twin: no descriptor names this body -> not verified");
        m[arr + 192 * 7 + 4] = kBody;
        Check(!scene_inputs::VerifyBodyLinkAgainstDescriptors(rdm, kBody, 12345), "PZF2 A1 twin: two descriptors name the same body -> not verified (exactly one)");
        m[arr + 192 * 7 + 4] = 0x30007;
        m.erase(arr + 192 * 11 + 4);
        Check(!scene_inputs::VerifyBodyLinkAgainstDescriptors(rdm, kBody, 12345), "PZF2 A1: an unreadable descriptor word fails closed");
        m[arr + 192 * 11 + 4] = 0x3000B;
        m[mgr + 8] = 0;
        Check(!scene_inputs::VerifyBodyLinkAgainstDescriptors(rdm, kBody, 12345), "PZF2 A1: a null descriptor array fails closed");
        m[mgr + 8] = arr;
        // the contributor: verdict -> naming and pieces (A1 / A3)
        WrestlerContributor wv;
        wwe2k14::wrestlers::ModelLinkRead(wv, kReader, rw);
        wv.SubmitSkin(kBody, 6, kN, pa.data(), 5);
        Check(!wv.BodyKeyFor(kBody).verified && wv.VerifyBodyLink(kBody, false) == false && !wv.BodyKeyFor(kBody).verified && wv.BodyLinkUnverified() == 0 && wv.OwnerOfPiece(0x70000100) == 0,
              "PZF2 A1: a failing verdict leaves the link unverified and drops its listed pieces (counted on a change: 0 here, it was never verified)");
        const bool relist = wv.VerifyBodyLink(kBody, true);
        Check(relist && wv.BodyKeyFor(kBody).verified && wv.ModelKeyFor(kBody).pieces.empty(), "PZF2 A3: a passing verdict after a drop asks for the pieces to be re-read (true), and verifies the link");
        std::vector<scene_inputs::DescPiece> dp;
        const auto rwd = [&](uint32_t a2, uint32_t& v) { auto it = g.find(a2); if (it == g.end()) return false; v = it->second; return true; };
        Check(scene_inputs::ReadDescriptorPieces(rwd, kDesc, dp) && dp.size() == 3 && dp[0].id == 10000 && dp[2].id == 777, "PZF2 A3: the pieces re-read from [desc + 12] / [desc + 16] (a null model skipped)");
        std::vector<WrestlerContributor::PieceRef> pr;
        for (const auto& p : dp) pr.push_back({p.model, p.id});
        wv.RelistPieces(kBody, std::move(pr));
        Check(wv.ModelKeyFor(kBody).pieces.size() == 3 && wv.OwnerOfPiece(0x70000100) == kBody && wv.BodyLinkRelisted() == 1 && !wv.VerifyBodyLink(kBody, true), "PZF2 A3: relisted (3 pieces, owner map and the lock-free list); a second pass asks for nothing");
        Check(wv.VerifyBodyLink(kBody, false) == false && wv.BodyLinkUnverified() == 1 && wv.OwnerOfPiece(0x70000100) == 0 && !wv.BodyKeyFor(kBody).verified, "PZF2 A1: a verdict that now fails drops the pieces again and counts the change once");
        // a verdict is for ONE latch: the next Collect without a fresh verdict names nothing
        wv.VerifyBodyLink(kBody, true);
        Counters cv;
        SceneTables tv = EmitAt(wv, cv, 7);
        const bool named = tv.objects.size() == 1 && tv.Str(tv.assets[tv.objects[0].asset].entry) == "model_key:12345";
        SceneTables tv2 = EmitAt(wv, cv, 8);
        Check(named && tv2.objects.size() == 1 && tv2.Str(tv2.assets[tv2.objects[0].asset].entry) != "model_key:12345", "PZF2 A1: the verdict holds for the latch it was made at: named at that Collect, not at the next one without a verdict");
      }
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
      w4.VerifyBodyLink(0x1000, true);   // PZF2 A1: the latch's verdict
      t4 = EmitAt(w4, c4, 6);
      Check(t4.objects.size() == 1 && t4.Str(t4.objects[0].id) == "skel:00001000" && t4.Str(t4.assets[t4.objects[0].asset].pac) == "pac\\ch\\ch009.pac" && t4.Str(t4.assets[t4.objects[0].asset].entry) == "model_key:999",
            "after the rebuild the old slot (actor:slot:2) and the old declared pac link are gone; the new key is what the record carries");
      {  // S103 B: the participant slot key names the model of a slot-matched skeleton that has no declared link
        WrestlerContributor ws;
        ws.SubmitSkin(0x2000, 0, 4, pb.data(), 3);
        ws.SubmitSlotKey(0x2000, 34320);  // character 343, attire 2, kind 0
        Counters cs;
        SceneTables ts = EmitAt(ws, cs, 3);
        const AssetRef* as = ts.objects.size() == 1 ? &ts.assets[ts.objects[0].asset] : nullptr;
        Check(as && ts.Str(as->pac) == "pac\\ch\\ch343.pac" && ts.Str(as->entry) == "model_key:34320" && as->variant == 2, "S103 B: the slot key names pac, entry and attire");
      }
      {  // S103 DLW (COV2 GAP 2 part 3): "the record can name it" means the key LOADS. A DLC wrestler's pac failed to open: the record stops naming him, the GuestModel rows draw him (not a hole)
        rdr_scene::namedfail::Clear();
        auto build = [&](WrestlerContributor& wd, uint32_t faults, int route) {   // route 0 = the participant slot's key, 1 = the declared link, 2 = the body's model link
          wd.SetFaultsForTest(faults);
          if (route == 2) wd.SubmitModelLink(0x3000, 38510, 0x1, 0x2, 0x3, {});   // (the link resets the node count: skin after it)
          wd.SubmitSkin(0x3000, 0, 4, pb.data(), 3);
          if (route == 0) wd.SubmitSlotKey(0x3000, 38510);   // character 385 (Fandango), attire 1, kind 0
          if (route == 1) wd.SubmitAssetLink(0x3000, "pac\\ch\\ch385.pac", "model_key:38510", 1);
          if (route == 2) wd.VerifyBodyLink(0x3000, true);
        };
        for (int route = 0; route < 3; ++route) {
          rdr_scene::namedfail::Clear();
          WrestlerContributor wd;
          build(wd, 0, route);
          Counters cd;
          const bool can_d = wd.RecordCanName(0x3000);   // (before the Emit: a body link counts only for the latch it was verified in)
          SceneTables td = EmitAt(wd, cd, 3);
          const AssetRef* ad = td.objects.size() == 1 ? &td.assets[td.objects[0].asset] : nullptr;
          Check(can_d && ad && td.Str(ad->pac) == "pac\\ch\\ch385.pac", route == 0 ? "DLW: the slot key names ch385 while it loads" : route == 1 ? "DLW: the declared link names ch385 while it loads" : "DLW: the body's model link names ch385 while it loads");
          rdr_scene::namedfail::Note("pac\\ch\\ch385.pac", "model_key:38510", 1);   // the loader failed it (the DLC pac did not open)
          WrestlerContributor we;
          build(we, 0, route);
          Counters ce;
          const bool can_e = we.RecordCanName(0x3000);   // (before the Emit: a body link counts only for the latch it was verified in)
          SceneTables te = EmitAt(we, ce, 3);
          const AssetRef* ae = te.objects.size() == 1 ? &te.assets[te.objects[0].asset] : nullptr;
          const uint64_t fb0 = rdr_scene::namedfail::FellBack();
          Check(!can_e && ae && ae->pac == 0 && rdr_scene::namedfail::FellBack() >= fb0,
                route == 0 ? "DLW: a failed slot-key model is not named (RecordCanName false, pac 0 -> the GuestModel rows draw it)" : route == 1 ? "DLW: a failed declared link is not named (RecordCanName false, pac 0)" : "DLW: a failed body model link is not named (RecordCanName false, pac 0)");
          WrestlerContributor wf;
          build(wf, WrestlerContributor::kFaultNamedFailHole, route);   // PLANT: the old rule ("has a key"): the hole is caught
          Counters cf;
          const bool can_f = wf.RecordCanName(0x3000);   // (before the Emit: a body link counts only for the latch it was verified in)
          SceneTables tf = EmitAt(wf, cf, 3);
          const AssetRef* af = tf.objects.size() == 1 ? &tf.assets[tf.objects[0].asset] : nullptr;
          Check(can_f && af && tf.Str(af->pac) == "pac\\ch\\ch385.pac", route == 0 ? "DLW PLANT (the old RecordCanName): a failed slot key is still 'named' -> the hole the GuestModel reader is never asked about" : route == 1 ? "DLW PLANT: a failed declared link is still named (the hole)" : "DLW PLANT: a failed body link is still named (the hole)");
          // another character is untouched
          WrestlerContributor wg;
          wg.SubmitSkin(0x4000, 0, 4, pb.data(), 3);
          wg.SubmitSlotKey(0x4000, 34320);
          Check(wg.RecordCanName(0x4000), "DLW: a character whose pac loads is still named");
        }
        {  // S103 CMB fix 2 / 4: a composite attire's tagged entry is what the loader fails or loads; an unverified tagged link is a hole, never the plain slot key
          const std::string ctag = "/c:" + std::string(96, '2');
          auto buildt = [&](WrestlerContributor& wd, const std::string& t, bool verify) {
            wd.SubmitModelLink(0x3000, 38510, 0x1, 0x2, 0x3, {}, 0, t);
            wd.SubmitSkin(0x3000, 0, 4, pb.data(), 3);
            wd.SubmitSlotKey(0x3000, 38510);
            if (verify) wd.VerifyBodyLink(0x3000, true);
          };
          rdr_scene::namedfail::Clear();
          WrestlerContributor wa;
          buildt(wa, ctag, true);
          const bool can_a = wa.RecordCanName(0x3000);
          Counters ca;
          SceneTables ta = EmitAt(wa, ca, 3);
          const AssetRef* aa = ta.objects.size() == 1 ? &ta.assets[ta.objects[0].asset] : nullptr;
          Check(can_a && aa && ta.Str(aa->entry) == "model_key:38510" + ctag, "CMB: a verified composite link names the TAGGED entry");
          rdr_scene::namedfail::Note("pac\\ch\\ch385.pac", "model_key:38510", 1);   // the plain entry failed: the tagged one is another ref
          WrestlerContributor wb;
          buildt(wb, ctag, true);
          Check(wb.RecordCanName(0x3000), "CMB: the plain entry's failure does not fail the tagged entry");
          rdr_scene::namedfail::Clear();
          rdr_scene::namedfail::Note("pac\\ch\\ch385.pac", "model_key:38510" + ctag, 1);   // the tagged load failed (a refusal or a compose failure): a hole
          WrestlerContributor wc;
          buildt(wc, ctag, true);
          const bool can_c = wc.RecordCanName(0x3000);
          Counters cc;
          SceneTables tc = EmitAt(wc, cc, 3);
          const AssetRef* ac = tc.objects.size() == 1 ? &tc.assets[tc.objects[0].asset] : nullptr;
          Check(!can_c && ac && ac->pac == 0, "CMB fix 2: a failed TAGGED entry makes RecordCanName false and the record names nothing (the plain entry is not tried)");
          rdr_scene::namedfail::Clear();
          WrestlerContributor wd;
          buildt(wd, ctag, false);   // the link is not verified this latch: the old code fell back to the participant slot's key = the plain stock model
          const bool can_d2 = wd.RecordCanName(0x3000);
          Counters cd2;
          SceneTables td2 = EmitAt(wd, cd2, 3);
          const AssetRef* ad2 = td2.objects.size() == 1 ? &td2.assets[td2.objects[0].asset] : nullptr;
          Check(can_d2 && ad2 && ad2->pac == 0 && rdr_scene::namedfail::LinkHoles().load() >= 1, "CMB fix 4 / ARM F8: an unverified composite link never falls back to the plain slot key; it is a counted HOLE (can-name true: the guest rows are not asked; the body goes out unnamed)");
          rdr_scene::namedfail::HoleRule().store(false);
          Check(!wd.RecordCanName(0x3000), "ARM F8 planted twin (switch off): the old answer, the guest rows are asked");
          rdr_scene::namedfail::HoleRule().store(true);
          WrestlerContributor we2;
          buildt(we2, "", false);   // the control: a plain link, unverified: the slot key still names it
          Check(we2.RecordCanName(0x3000), "CMB fix 4 control: a plain unverified link still falls back to the slot key");
          rdr_scene::namedfail::Clear();
        }
        rdr_scene::namedfail::Clear();
      }
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
      w8.VerifyBodyLink(0x1000, true);
      EmitAt(w8, c8, 5);
      w8.VerifyBodyLink(0x1000, true);
      EmitAt(w8, c8, 6);
      Check(w8.LinkedSeenAsSkeleton() == 1, "a linked body that turns up as a skeleton is counted once");
      {  // S103 PZ F5 (run 36): the declared link belongs to the BODY, not the aged Known record. A referee body is linked at the build, leaves the record for 2000 Collects (120 of kPruneAfter, then kKnownAge 1800) (the entrance -> match load), returns in slot 6
        struct R { std::string id, entry; size_t pieces = 0; uint64_t erased = 0, revived = 0; };
        auto scenario = [&](uint32_t faults) {
          static uint32_t g_erase_lines = 0;
          WrestlerContributor w;
          w.SetFaultsForTest(faults);
          g_erase_lines = 0;
          w.SetKnownEraseLog([](uint32_t, uint32_t, uint32_t, uint64_t) { ++g_erase_lines; });
          w.SubmitModelLink(0x1000, 25500, 0, 0, 0x3, {{0x70000000, 10000}, {0x70000100, 20002}, {0x70000200, 21011}});
          w.SubmitSkin(0x1000, 6, kN, pa.data(), 5);
          Counters c;
          EmitAt(w, c, 5);
          for (uint32_t i = 0; i < 2000; ++i) { SceneTables tt; FrameInfo f; f.guest_frame = 10 + i; w.Emit(f, tt, c); }
          const bool had_pieces_while_away = w.OwnerOfPiece(0x70000100) == 0x1000;
          w.SubmitSkin(0x1000, 6, kN, pb.data(), 3000);
          if (w.VerifyBodyLink(0x1000, true))   // PZF2 A1 / A3: the latch's descriptor check passes; the pieces the Known's aging dropped are re-read from [desc + 12] and re-listed
            w.RelistPieces(0x1000, {{0x70000000, 10000}, {0x70000100, 20002}, {0x70000200, 21011}});
          SceneTables t = EmitAt(w, c, 3000);
          R r;
          for (const Object& o : t.objects)
            if (t.Str(o.id) == "actor:slot:6") r.id = "actor:slot:6", r.entry = t.Str(t.assets[o.asset].entry);
          r.pieces = w.ModelKeyFor(0x1000).pieces.size() + (had_pieces_while_away ? 1000 : 0);
          r.erased = g_erase_lines, r.revived = w.BodyLinkRevived();
          return r;
        };
        const R ok = scenario(0), twin = scenario(WrestlerContributor::kFaultLinkInKnown);
        Check(ok.id == "actor:slot:6" && ok.entry == "model_key:25500", "PZ F5: the body's declared link survives 2000 Collects without the body: slot 6 comes back named model_key:25500");
        Check(ok.pieces == 3, "PZF2 A3: its pieces are dropped when the Known ages out (the name stays), and re-listed from the descriptor when the latch's check passes: 3 after it returns, none while it is away");
        Check(ok.erased == 1 && ok.revived == 1, "PZ F5: one erase line for the model-linked Known, one revival counted (the address-reuse risk is counted)");
        Check(twin.id == "actor:slot:6" && twin.entry != "model_key:25500" && twin.pieces == 0, "PZ F5 planted twin (kFaultLinkInKnown, today's rule): unnamed, pieces gone: the run-36 hole; fails the check above");
        // dropped by another object: the update hook reports the address with another node count
        { WrestlerContributor w; w.SubmitModelLink(0x1000, 25500, 0, 0, 0x3, {{0x70000000, 10000}});
          w.SubmitSkin(0x1000, 6, kN, pa.data(), 5);
          Check(w.ModelKeyFor(0x1000).known, "PZ F5: linked and seen with 109 nodes");
          w.SubmitSkin(0x1000, 6, 70, pa.data(), 6);
          Check(!w.ModelKeyFor(0x1000).known && w.OwnerOfPiece(0x70000000) == 0 && w.BodyLinkDroppedCount() == 1, "PZ F5: the update hook reports the address with another node count: link and pieces dropped, counted"); }
        // dropped when the slot key names another character (slot < 6)
        { WrestlerContributor w; w.SubmitModelLink(0x1000, 34320, 0, 0, 0x3, {});
          w.SubmitSkin(0x1000, 0, kN, pa.data(), 5);
          w.SubmitSlotKey(0x1000, 34310);   // same character 343, another attire: the link stands
          Check(w.ModelKeyFor(0x1000).known, "PZ F5: a slot key of the same character keeps the link");
          w.SubmitSlotKey(0x1000, 32420);   // character 324: another body at this address
          Check(!w.ModelKeyFor(0x1000).known && w.BodyLinkDroppedKey() == 1, "PZ F5: a slot key naming another character drops the link, counted"); }
        // the next link at the address replaces it whole
        { WrestlerContributor w; w.SubmitModelLink(0x1000, 25500, 0, 0, 0x3, {{0x70000000, 10000}});
          w.SubmitModelLink(0x1000, 34320, 0, 0, 0x4, {});
          Check(w.ModelKeyFor(0x1000).key == 34320 && w.OwnerOfPiece(0x70000000) == 0 && w.BodyLinkCount() == 1, "PZ F5: the next link at the address replaces the old one (key and pieces)"); }
      }
    }
  }
  // ---- S103 W1: the palette leads the stream by one frame (run 26's case: record j held the palette of stream j + 1, 31464 of 31464 slots) ----
  {
    WrestlerContributor w;
    w.SubmitConsumer(0x1000, kN, pa.data(), 7);   // tag 7 = the walk that builds stream 8
    w.SubmitConsumer(0x1000, kN, pb.data(), 8);   // tag 8 = stream 9
    Counters c;
    SceneTables t;
    FrameInfo f;
    f.guest_frame = 8;                             // record 8 pairs with stream 8: it must carry the walk tagged 7 (pa), never the one tagged 8 (pb: stream 9's)
    w.Emit(f, t, c);
    Check(t.objects.size() == 1 && PaletteIs(t, 0, pa), "W1: record j carries the palette tagged j - 1 (stream j's), not the newer one (stream j + 1's)");
    Check(t.objects.size() == 1 && !PaletteIs(t, 0, pb), "W1 planted twin (the old rule: newest tag <= j) would put stream j + 1's palette here and fails this check");
    SceneTables t2;
    f.guest_frame = 9;
    w.Emit(f, t2, c);
    Check(t2.objects.size() == 1 && PaletteIs(t2, 0, pb), "W1: the next record carries the palette tagged 8");
  }
  // ---- S103 NRB / NRBF: own-skeleton pieces the owner's body draws (piece_link.h, slot_piece.h; docs\research\s103\NRB_pieces.md section 6) ----
  {
    using scene_inputs::PieceWhy;
    // (1) the guest read of the SUBM chain on a fake memory: char + 1152 = list head, node tagged 'SUBM', [node + 24] = comp, comp + 72 / + 80 = begin / end of 48-byte instances; per instance
    // +8 special object, +12 node map (one int32 per node), +24 copy flag, +28 deferred-build flag
    static std::map<uint32_t, uint32_t> mem;
    mem.clear();
    const uint32_t ch = 0x40000000, node = 0x40100000, comp = 0x40200000, arr = 0x40300000, map1 = 0x40400000, map2 = 0x40500000;
    auto put = [](uint32_t a, uint32_t v) { mem[a] = v; };
    put(ch + 1152 + 20, node), put(node + 4, scene_inputs::kSubmTag), put(node + 20, 0), put(node + 24, comp);
    put(comp + 72, arr), put(comp + 80, arr + 4 * 48);
    put(arr + 0, 0x70001000), put(arr + 16, 10000u << 16), put(arr + 12, map1), put(arr + 24, 1), put(arr + 28, 1);   // a clean piece
    put(arr + 48 + 0, 0), put(arr + 48 + 16, 26u << 16);                                                              // an instance with no model: skipped
    put(arr + 96 + 0, 0x70002000), put(arr + 96 + 16, 20002u << 16), put(arr + 96 + 12, map2), put(arr + 96 + 24, 1), put(arr + 96 + 28, 1);
    put(arr + 144 + 0, 0x70003000), put(arr + 144 + 16, 21011u << 16), put(arr + 144 + 12, map1), put(arr + 144 + 24, 1), put(arr + 144 + 28, 1), put(arr + 144 + 8, 0x40600000);   // the special-id path
    for (uint32_t i = 0; i < 11; ++i) put(map1 + 4 * i, i), put(map2 + 4 * i, i == 3 ? 0xFFFFFFFFu : i);   // map2: node 3 unmapped
    const scene_inputs::ReadFn rd = [](uint32_t a, void* dst, size_t n) -> bool {
      for (size_t k = 0; k < n; k += 4) {
        auto it = mem.find(a + uint32_t(k));
        const uint32_t v = it == mem.end() ? 0u : it->second;
        const uint32_t be = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);   // guest memory is big-endian
        std::memcpy(static_cast<uint8_t*>(dst) + k, &be, 4);
      }
      return true;
    };
    const scene_inputs::PieceCountFn counts = [](uint32_t m) -> uint32_t { return m == 0x70001000 ? 7u : m == 0x70002000 ? 11u : m == 0x70003000 ? 10u : 0u; };
    std::vector<scene_inputs::CharPiece> got;
    Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts) && got.size() == 3 && got[0].model == 0x70001000 && got[0].id == 10000 && got[1].model == 0x70002000 && got[1].id == 20002 &&
              got[2].model == 0x70003000, "NRB: the SUBM chain gives (model, id) per instance, a model-less instance skipped");
    Check(got.size() == 3 && got[0].why == PieceWhy::Ok && got[1].why == PieceWhy::Unmapped && got[2].why == PieceWhy::Special, "NRBF: proof: a complete map is Ok; a -1 node is Unmapped (a hole); a special-id instance is Special (a hole)");
    {  // entry: scene_inputs::ReadCharacterPieces, directly with a forged guest memory + count source (its production caller InputsContributor::ReadSlotSkeletons needs the whole latch / character table, which the other NRB checks of this unit also skip)
       // S105 A5a: the reader's copy probe (production caller: ReadCharacterPieces at the latch): inst +8 / +20 / +24 and the LAST mapped node pair, whatever the proof says; the proof's own verdict is unchanged
      Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts) && got.size() == 3 && got[0].probe.drawn == 0 && got[0].probe.special == 0 && got[0].probe.copy == 1, "A5a: the probe carries inst +20 (0 = the game does not draw it), +8 and +24 as read");
      put(arr + 20, 1);
      Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts) && got[0].why == PieceWhy::Ok && got[0].probe.drawn == 1 && got[0].probe.piece_node == 6 && got[0].probe.owner_node == 6,
            "A5a: a 7-node piece with a full map probes its LAST mapped node (piece node 6, owner node 6); inst +20 == 1 = drawn");
      Check(got[1].why == PieceWhy::Unmapped && got[1].probe.piece_node == 10 && got[1].probe.owner_node == 10 && got[2].why == PieceWhy::Special && got[2].probe.special == 0x40600000u,
            "A5a: a piece with a -1 node is still Unmapped (verdict unchanged) and still probes its last mapped node (10); a special-id instance carries its +8 word");
      Check(scene_inputs::ReadCharacterPieces(rd, ch, got, nullptr) && got[0].why == PieceWhy::NoCount && got[0].probe.piece_node == ~0u && got[0].probe.drawn == 0xFFFFFFFFu, "A5a: NoCount reads nothing: the probe stays unread (0xFFFFFFFF / ~0), and the own-rows rule refuses unread");
      {  // the count source the glue passes (WrestlerContributor::PieceNodeCount): the update hook's count when the wanted table (a stream-only table) has none: a CAW piece is then proven / probed, not NoCount
        static WrestlerContributor* cw;
        WrestlerContributor wc;
        cw = &wc;
        wc.SubmitSkin(0x70001000, -1, 7, Pose(7, 0.1f).data(), 5);   // the update hook sized the piece (7 nodes); nothing is in the wanted table (no WantSkeleton ran), and the game's own count word is unreadable here (a CAW piece)
        const scene_inputs::PieceCountFn hook = [](uint32_t m) -> uint32_t { return cw->WantCount(m); };
        Check(scene_inputs::ReadCharacterPieces(rd, ch, got, nullptr, nullptr, 0, 0, false, hook) && got[0].why == PieceWhy::Ok && got[0].probe.piece_node == 6 && got[0].nodes == 7, "A5a: with neither a wanted nor a game count, the update hook's count sizes the CAW piece: it reads its map / probe");
        Check(scene_inputs::ReadCharacterPieces(rd, ch, got, nullptr) && got[0].why == PieceWhy::NoCount && got[0].probe.piece_node == ~0u, "A5a planted twin (no hook count: the old source): NoCount, the probe stays unread (the check above fails it)");
        // review F1: a STOCK piece (the game's own count word is 7) whose update-hook Known holds ANOTHER skeleton's count (9, a reused address) must stay Ok: the hook count is never a second opinion
        put(0x70001000 + 0, scene_inputs::kModelVtable), put(scene_inputs::kModelVtable + 8, scene_inputs::kCountGetter), put(0x70001000 + 296, 1), put(0x70001000 + 300, 7);
        wc.SubmitSkin(0x70001000, -1, 9, Pose(9, 0.1f).data(), 6);
        Check(scene_inputs::ReadCharacterPieces(rd, ch, got, nullptr, nullptr, 0, 0, false, hook) && got[0].why == PieceWhy::Ok && got[0].nodes == 7, "A5a F1: a stock piece (game count 7) beside a stale Known count 9 stays Ok (the hook count is used only when neither the wanted table nor the game knows the model)");
        Check(scene_inputs::ReadCharacterPieces(rd, ch, got, hook) && got[0].why == PieceWhy::CountMismatch, "A5a F1 planted twin (the hook count wired as the wanted count: the old glue): the stock piece becomes CountMismatch (a hole): the check above fails it");
        put(0x70001000 + 0, 0), put(0x70001000 + 296, 0), put(0x70001000 + 300, 0);
      }
    }
    {  // S103 W2B (W2_ADV R4 / 3b): with the switch on, a -1 node no longer makes the piece Unmapped: its finished row [[model+200] + 64 * j] is read at the latch; mode byte inst+37 must be 1; an unreadable row = MapUnreadable (never a rest-pose row)
      const uint32_t pal = 0x40900000;
      put(0x70002000 + 200, pal), put(arr + 96 + 36, 0x00010000u);
      for (uint32_t k = 0; k < 16; ++k) { const float v = 7.f + float(k); uint32_t bits; std::memcpy(&bits, &v, 4); put(pal + 64 * 3 + 4 * k, bits); }
      Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts, nullptr, 0, 0, true) && got[1].why == PieceWhy::Ok && got[1].free_nodes.size() == 1 && got[1].free_nodes[0] == 3 && got[1].rows.size() == 16 && got[1].rows[0] == 7.f && got[1].rows[15] == 22.f,
            "W2B: a -1 node is Ok with its row read from [[model+200] + 64 * j] (the piece's own bytes)");
      Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts) && got[1].why == PieceWhy::Unmapped && got[1].free_nodes.empty(), "W2B planted twin: the switch off keeps the old rule (Unmapped, no rows)");
      put(arr + 96 + 36, 0);
      Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts, nullptr, 0, 0, true) && got[1].why == PieceWhy::Unmapped && got[1].free_nodes.empty(), "W2B (W2_ADV 3b): mode byte inst+37 != 1 (mode 0 does not compute a -1 node whose parent is -1) = not proved");
      put(arr + 96 + 36, 0x00010000u), put(0x70002000 + 200, 0);
      Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts, nullptr, 0, 0, true) && got[1].why == PieceWhy::MapUnreadable && got[1].rows_bad && got[1].rows.empty(), "W2B: a null palette pointer = MapUnreadable (a counted, absent piece), never filled");
      put(0x70002000 + 200, pal);
      Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts, nullptr, 0, scene_inputs::kFaultNoRows, true) && got[1].why == PieceWhy::MapUnreadable, "W2B planted twin: rest-pose fallback (rows not read) must not be Ok");
      {  // S103 HGF (HGP.md section 3): rows that all still equal the bind palette (identity) = the game's first spring / SUBM update has not run: the piece is absent for this latch (no rows), never drawn at the record origin
        for (uint32_t k = 0; k < 16; ++k) { const float v = (k % 5) == 0 ? 1.f : 0.f; uint32_t bits; std::memcpy(&bits, &v, 4); put(pal + 64 * 3 + 4 * k, bits); }
        Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts, nullptr, 0, 0, true) && got[1].why == PieceWhy::Ok && got[1].rows_unposed && got[1].free_nodes.empty() && got[1].rows.empty(),
              "HGF: a piece whose rows are all still the bind palette (identity) gives no rows (absent this latch), why stays Ok, rows_unposed set");
        Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts, nullptr, 0, scene_inputs::kFaultDrawUnposed, true) && got[1].why == PieceWhy::Ok && !got[1].rows_unposed && got[1].free_nodes.size() == 1 && got[1].rows.size() == 16 && got[1].rows[0] == 1.f,
              "HGF planted twin (kFaultDrawUnposed: the old rule) keeps the identity rows: the check above is what stops the hair drawing at the record origin");
        Check(!scene_inputs::PieceRowsAreBind(nullptr, 0), "HGF: no rows is not 'bind' (nothing to drop)");
        const float nearid[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0.001f, 1};
        Check(!scene_inputs::PieceRowsAreBind(nearid, 1), "HGF: a translation 0.001 off identity is a posed piece");
        float tiny[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1e-6f, 1};
        Check(scene_inputs::PieceRowsAreBind(tiny, 1), "HGF: a float-noise difference (1e-6) from identity is still the bind palette (tolerance 1e-5)");
        for (uint32_t k = 0; k < 16; ++k) { const float v = 7.f + float(k); uint32_t bits; std::memcpy(&bits, &v, 4); put(pal + 64 * 3 + 4 * k, bits); }
      }
      put(0x70002000 + 200, 0), put(arr + 96 + 36, 0);
    }
    auto why0 = [&]() { return scene_inputs::ReadCharacterPieces(rd, ch, got, counts) && !got.empty() ? got[0].why : PieceWhy::kCount; };
    put(arr + 28, 0);
    Check(why0() == PieceWhy::NotDeferred, "NRBF plant: not built from the deferred data (desc+144 / factory) -> NotDeferred");
    put(arr + 28, 1), put(arr + 24, 0);
    Check(why0() == PieceWhy::NoCopy, "NRBF plant: owner copy off -> NoCopy");
    put(arr + 24, 1), put(arr + 12, 0);
    Check(why0() == PieceWhy::MapNull, "NRBF plant: no node map -> MapNull");
    put(arr + 12, map1);
    Check(scene_inputs::ReadCharacterPieces(rd, ch, got, nullptr) && got[0].why == PieceWhy::NoCount, "NRBF plant: no recorded node count -> NoCount");
    put(node + 4, 0x434D4D44);   // plant: another component's tag
    Check(scene_inputs::ReadCharacterPieces(rd, ch, got, counts) && got.empty(), "NRB plant: no node tagged SUBM -> no pieces (never a guess)");
    put(node + 4, scene_inputs::kSubmTag), put(comp + 80, arr + 4 * 48 + 4);   // plant: an array length that is not whole instances
    Check(!scene_inputs::ReadCharacterPieces(rd, ch, got, counts), "NRB plant: a ragged instance array is refused");
    put(comp + 80, arr + 4 * 48);

    {  // S103 PZ F2: path B (the descriptor's own piece model, inst + 28 == 0) is proven from the descriptor; path A (desc + 144 != 0) is refused. Listings sub_8259E720 0x8259E78C-0x8259EA4C.
      static scene_inputs::BodyDesc g_bd;
      const scene_inputs::BodyDescFn bodyfn = [](uint32_t) -> scene_inputs::BodyDesc { return g_bd; };
      const uint32_t desc = 0x40700000, ents = 0x40800000;
      put(desc + 12, ents), put(desc + 16, 3), put(desc + 144, 0);
      put(arr + 4, 0xAAAA0001), put(arr + 16, (10000u << 16) | 0u);            // inst + 4 = [entry + 0]; inst + 16 = id << 16 | index (index 0)
      put(ents + 0, 0xAAAA0001), put(ents + 4, 0x70001000), put(ents + 16, 0);   // entry 0: the piece's model is the descriptor's own (B1), no deferred PACH data
      put(desc + 4, 0x1000), put(desc + 28, 25500);       // PZF2 B-2: the latch re-reads [desc + 4] == the owner skeleton and [desc + 28] == the link's key
      g_bd = scene_inputs::BodyDesc{true, desc, 25500};   // kind 0 (the referee)
      put(arr + 28, 0);                                    // inst + 28 == 0: not built from the deferred data
      auto why_b = [&](uint32_t faults = 0, bool with_fn = true) { return scene_inputs::ReadCharacterPieces(rd, ch, got, counts, with_fn ? bodyfn : nullptr, 0x1000, faults) && !got.empty() ? got[0].why : PieceWhy::kCount; };
      Check(why_b() == PieceWhy::Ok, "PZ F2: B1 (inst + 0 == [entry + 4], [entry + 16] == 0, [desc + 144] == 0, [entry + 0] == inst + 4) is proven Ok");
      Check(why_b(0, false) == PieceWhy::NotDeferred, "PZ F2: with no declared link source the piece stays NotDeferred (a hole)");
      put(ents + 4, 0);
      Check(why_b() == PieceWhy::B2Unproven && got[0].build == scene_inputs::PieceBuild::B2, "PZF2 B-3: B2 ([entry + 4] == 0 and [entry + 16] == 0: a fresh model from [entry + 0]) is refused B2Unproven until a run shows one, and the build is recorded as B2");
      Check(why_b(scene_inputs::kFaultAcceptB2) == PieceWhy::Ok, "PZF2 B-3 planted twin (kFaultAcceptB2: the old PZF rule) accepts B2: the check above is what stops it");
      put(ents + 4, 0x70001000);
      put(desc + 144, 0x40900000);
      Check(why_b() == PieceWhy::PathA, "PZ F2: path A ([desc + 144] != 0: a customised model) is refused PathA");
      Check(why_b(scene_inputs::kFaultAnyNotDeferred) == PieceWhy::Ok, "PZ F2 planted twin (kFaultAnyNotDeferred: accept any NotDeferred) wrongly accepts path A: the check above is what stops it");
      put(desc + 144, 0);
      g_bd.known = false;
      Check(why_b() == PieceWhy::WrongDesc, "PZ F2: no declared link for the owner: WrongDesc");
      g_bd.known = true, g_bd.desc = 0;
      Check(why_b() == PieceWhy::WrongDesc, "PZ F2: a null descriptor: WrongDesc");
      g_bd.desc = desc;
      put(desc + 16, 0);
      Check(why_b() == PieceWhy::WrongDesc, "PZ F2: an empty descriptor array: WrongDesc");
      put(desc + 16, 3), put(arr + 16, (10000u << 16) | 5u);   // index 5 of 3 entries
      Check(why_b() == PieceWhy::WrongDesc, "PZ F2: an index past the descriptor's entries: WrongDesc");
      put(arr + 16, (10000u << 16) | 0u);
      put(ents + 0, 0xBBBB0002);
      Check(why_b() == PieceWhy::EntryMismatch, "PZ F2: [entry + 0] != inst + 4 (another descriptor's instance): EntryMismatch");
      put(ents + 0, 0xAAAA0001), put(ents + 4, 0x70009999);
      Check(why_b() == PieceWhy::EntryMismatch, "PZ F2: inst + 0 is not the entry's model (and the entry has one): EntryMismatch");
      put(ents + 4, 0x70001000), put(ents + 16, 0x50000000);
      Check(why_b() == PieceWhy::EntryMismatch, "PZ F2: the entry holds deferred PACH data but inst + 28 == 0: EntryMismatch");
      put(ents + 16, 0);
      // PZF2 B-1 + EB: an ALLOW-list of rig-proven kinds (0 referee, 2 match body, 4 Bryan's entrance body); any other kind is KindUnproven. The kind comes from the latch's own read of [desc + 28] (B-2).
      for (uint32_t k : {0u, 2u, 4u}) {
        g_bd.key = 25500 + k, put(desc + 28, 25500 + k);
        Check(why_b() == PieceWhy::Ok && got[0].kind == k && got[0].build == scene_inputs::PieceBuild::B1, "PZF2 B-1: path B1 on a proven kind (0 / 2 / 4) is Ok, the kind and build recorded");
      }
      for (uint32_t k : {1u, 3u, 5u, 6u, 9u}) {
        g_bd.key = 25500 + k, put(desc + 28, 25500 + k);
        Check(why_b() == PieceWhy::KindUnproven, "PZF2 B-1: a kind no rig proved is refused KindUnproven (an allow-list, not a deny-list)");
      }
      Check(why_b(scene_inputs::kFaultAnyKind) == PieceWhy::Ok, "PZF2 B-1 planted twin (kFaultAnyKind: no kind test) accepts kind 9: the check above is what stops it");
      // PZF2 B-2: the latch re-reads the descriptor; a link whose descriptor now names another body or another key proves nothing
      g_bd.key = 25502, put(desc + 28, 25502);
      put(desc + 4, 0x2000);
      Check(why_b() == PieceWhy::WrongDesc && why_b(scene_inputs::kFaultNoDescReread) == PieceWhy::Ok, "PZF2 B-2: [desc + 4] != the owner skeleton -> WrongDesc; the planted twin (no re-read, the old rule) accepts it");
      put(desc + 4, 0x1000), put(desc + 28, 25512);
      Check(why_b() == PieceWhy::WrongDesc && why_b(scene_inputs::kFaultNoDescReread) == PieceWhy::Ok, "PZF2 B-2: [desc + 28] != the link's key (another character's descriptor now) -> WrongDesc; the twin accepts it");
      put(desc + 28, 25502);
      g_bd.key = 25500, put(desc + 28, 25500);
      put(map1 + 4 * 3, 0xFFFFFFFFu);
      Check(why_b() == PieceWhy::Unmapped, "PZ F2: a NotDeferred piece with an unmapped node is Unmapped (the map is checked before the deferred flag), never Ok");
      put(map1 + 4 * 3, 3);
      put(arr + 28, 1);
      Check(why_b() == PieceWhy::Ok, "PZ F2: the deferred (inst + 28 == 1) piece is Ok as before, with or without the descriptor");

      // PZ F3: the game's own node count at the latch (vtable 0x8206AF90, [model + 296] != 0 -> [model + 300]) when the scene core has no wanted count yet
      const scene_inputs::PieceCountFn none = [](uint32_t) -> uint32_t { return 0u; };
      auto why_c = [&](scene_inputs::PieceCountFn cf) { return scene_inputs::ReadCharacterPieces(rd, ch, got, cf, nullptr, 0, 0) && !got.empty() ? got[0].why : PieceWhy::kCount; };
      Check(why_c(none) == PieceWhy::NoCount, "PZ F3: wanted 0 and no game count readable: NoCount");
      put(0x70001000 + 0, scene_inputs::kModelVtable), put(scene_inputs::kModelVtable + 8, scene_inputs::kCountGetter), put(0x70001000 + 296, 1), put(0x70001000 + 300, 7);
      Check(why_c(none) == PieceWhy::Ok, "PZ F3: wanted 0 + the game's count 7: proven at the first latch (map of 7 nodes complete)");
      put(0x70001000 + 0, 0x8206AF94);
      Check(why_c(none) == PieceWhy::NoCount, "PZ F3 planted twin: a wrong vtable word gives no game count: NoCount");
      put(0x70001000 + 0, scene_inputs::kModelVtable), put(0x70001000 + 296, 0);
      Check(why_c(none) == PieceWhy::NoCount, "PZ F3 / S105 CAWP: [model + 296] == 0 and no delegate object at [model + 220]: NoCount");
      {  // S105 CAWP: the game's delegate branch (sub_82BD3AB8: [m+296] == 0 -> D = [m+220]; D's slot 2 = sub_82C27620 -> [[D+12]+24]); the dog tag of a created superstar (run 115: 'reader proof 1 no_count')
        put(0x70001000 + 220, 0x70009000), put(0x70009000, 0x8206C7A0), put(0x8206C7A0 + 8, scene_inputs::kDataCountGetter), put(0x70009000 + 12, 0x7000A000), put(0x7000A000 + 24, 7);
        Check(why_c(none) == PieceWhy::Ok, "S105 CAWP: the delegate count (D vtable 0x8206C7A0 slot 2 = sub_82C27620, [[D+12]+24] = 7) sizes the piece: proven at the first latch");
        // planted twin (the old single-vtable rule, no delegate branch = the delegate word never read): count 0, so the check above fails it
        Check(scene_inputs::GameNodeCountW([&](uint32_t a, uint32_t& v) { if (a == 0x70001000 + 220) return false; auto it = mem.find(a); if (it == mem.end()) return false; v = it->second; return true; }, 0x70001000) == 0,
              "S105 CAWP planted twin (the old rule: [m+296] == 0 is not followed): count 0");
        Check(scene_inputs::GameNodeCountW([&](uint32_t a, uint32_t& v) { auto it = mem.find(a); if (it == mem.end()) return false; v = it->second; return true; }, 0x70001000) == 7, "S105 CAWP: GameNodeCountW itself gives the delegate count 7");
        put(0x8206C7A0 + 8, 0x82C27624);
        Check(why_c(none) == PieceWhy::NoCount, "S105 CAWP: a delegate whose slot 2 is another function is not followed: NoCount (fail closed)");
        put(0x8206C7A0 + 8, scene_inputs::kDataCountGetter), put(0x70009000 + 12, 0);
        Check(why_c(none) == PieceWhy::NoCount, "S105 CAWP: sub_82C27620 with [D+12] == 0 returns 0: NoCount");
        put(0x70001000 + 220, 0);
      }
      put(0x70001000 + 296, 1), put(0x70001000 + 300, 9);
      Check(why_c(counts) == PieceWhy::CountMismatch, "PZ F3: wanted 7 and the game's count 9 differ: CountMismatch");
      put(0x70001000 + 300, 7);
      Check(why_c(counts) == PieceWhy::Ok, "PZ F3: wanted 7 == the game's 7: Ok");
      put(0x70001000 + 0, 0), put(0x70001000 + 296, 0), put(0x70001000 + 300, 0);
    }

    {  // S105 CAWP (docs/research/s105/CAWP.md): a SUBM-linked piece the update hook went quiet for is a latch target EVERY latch while its owner is in a slot: fresh, never stale, never pruned; dropped when its owner leaves
      struct Res { bool in_targets_each = true, fresh_end = false, drawn_by_owner_end = false, absent_after_leave = false; uint64_t stale = 0; };
      auto drive200 = [&](uint32_t faults) {
        Res r;
        WrestlerContributor w{};
        w.SetFaultsForTest(faults);
        w.SubmitSkin(0x1000, 0, kN, pa.data(), 5);   // the body: the update hook reports it once (then quiet: a slot target regardless)
        w.SubmitSkin(0x70001000, -1, 7, Pose(7, 0.1f).data(), 5);   // the hair: reported once, then quiet (match, run 115): only the link can keep it
        WrestlerContributor::SlotTable tbl{};
        tbl.skels.push_back({0x1000, 0});
        tbl.state[0] = WrestlerContributor::kSlotOccupied;
        scene_inputs::CharPiece cp;
        cp.model = 0x70001000, cp.id = 10000, cp.slot = 0, cp.why = PieceWhy::Ok, cp.nodes = 7, cp.copy = 1;
        tbl.pieces.push_back(cp);
        SceneTables t;
        for (uint64_t h = 6; h < 6 + 200; ++h) {
          std::vector<WrestlerContributor::PaletteTarget> targets;
          uint32_t miss = 0;
          w.SubmitSkin(0x1000, 0, kN, pa.data(), h);   // the body keeps being reported (a match); only the hair is quiet
          w.LatchTargets(h, &tbl, targets, miss);
          bool piece_in = false;
          for (const auto& x : targets) {
            if (x.skel == 0x70001000) piece_in = x.count == 7;
            w.SubmitLatchConsumer(x.skel, x.count, Pose(x.count, 0.2f + 0.001f * float(h)).data(), h);
          }
          if (h > 6 + 8) r.in_targets_each = r.in_targets_each && piece_in;   // past the update hook's fresh window
          Counters c;
          t = EmitAt(w, c, h);
          r.stale += c.stale;
        }
        const Object* po = nullptr;
        for (const Object& o : t.objects) if (t.Str(o.id) == "skel:70001000") po = &o;
        r.fresh_end = po && po->bones_count == 7;
        r.drawn_by_owner_end = po && (po->flags & kObjDrawnByOwner);
        WrestlerContributor::SlotTable empty;   // the owner leaves the character table: the piece is no latch target any more
        std::vector<WrestlerContributor::PaletteTarget> targets2;
        uint32_t miss2 = 0;
        w.LatchTargets(300, &empty, targets2, miss2);
        r.absent_after_leave = true;
        for (const auto& x : targets2) if (x.skel == 0x70001000) r.absent_after_leave = false;
        return r;
      };
      const Res good = drive200(0);
      Check(good.in_targets_each && good.fresh_end && good.drawn_by_owner_end && good.stale == 0, "S105 CAWP: a linked piece the update hook went quiet for is a target at every latch for 200 latches: fresh (no stale record), still drawn by its owner");
      Check(good.absent_after_leave, "S105 CAWP: when its owner leaves the character table the piece is no latch target any more");
      {  // the guest model reader's want (wwe2k14_app.h g_want): a current piece link with a count is read although the update hook never sized it (the dog tag, 10032)
        WrestlerContributor w;
        WrestlerContributor::SlotTable tbl;
        tbl.skels.push_back({0x1000, 0});
        tbl.state[0] = WrestlerContributor::kSlotOccupied;
        w.SubmitSkin(0x1000, 0, kN, pa.data(), 5);
        scene_inputs::CharPiece cp, cp0;
        cp.model = 0xA90F0B30, cp.id = 10032, cp.slot = 0, cp.why = PieceWhy::Ok, cp.nodes = 5, cp.copy = 1;
        cp0.model = 0xA90F0B40, cp0.id = 10033, cp0.slot = 0, cp0.why = PieceWhy::NoCount;   // no count: not wanted
        tbl.pieces.push_back(cp), tbl.pieces.push_back(cp0);
        std::vector<WrestlerContributor::PaletteTarget> targets;
        uint32_t miss = 0;
        w.LatchTargets(6, &tbl, targets, miss);
        Check(w.WantCount(0xA90F0B30) == 0 && w.GuestModelWanted(0xA90F0B30) && !w.GuestModelWanted(0xA90F0B40) && w.PieceLinkCount(0xA90F0B30) == 5, "S105 CAWP: a current piece link with a count is wanted by the guest model reader (update hook never sized it); a link with no count, and a stranger, are not");
        WrestlerContributor w2;
        w2.SetFaultsForTest(WrestlerContributor::kFaultReaderIgnoresLinks);
        w2.SubmitSkin(0x1000, 0, kN, pa.data(), 5);
        w2.LatchTargets(6, &tbl, targets, miss);
        Check(!w2.GuestModelWanted(0xA90F0B30), "S105 CAWP planted twin (kFaultReaderIgnoresLinks = the old reader want): the dog tag is skipped (the check above fails it)");
        tbl.skels.clear();   // the owner left the table: the link is gone with it (the table's links are replaced whole every latch)
        w.LatchTargets(7, &tbl, targets, miss);
        Check(w.PieceLinkCount(0xA90F0B30) == 0 && !w.GuestModelWanted(0xA90F0B30), "S105 CAWP: a piece whose owner left the table is no longer wanted by the guest model reader");
      }
      {  // S105 CAWP fix round (review F1 / F2 / F3) and CAWE (the editor's hair): what the latch pushes, with planted twins
        struct Pc { uint32_t model; PieceWhy why; uint32_t drawn; };
        auto targets_of = [&](uint32_t faults, bool named_owner, const std::vector<Pc>& pcs, bool off_latch, bool* offlatch_accepted = nullptr) {
          WrestlerContributor w;
          w.SetFaultsForTest(faults);
          w.SubmitSkin(0x1000, 0, kN, pa.data(), 5);
          if (named_owner) w.SubmitSlotKey(0x1000, 34302);
          WrestlerContributor::SlotTable tbl;
          tbl.skels.push_back({0x1000, 0});
          tbl.state[0] = WrestlerContributor::kSlotOccupied;
          for (const Pc& p : pcs) {
            scene_inputs::CharPiece cp;
            cp.model = p.model, cp.id = 10000 + (p.model & 0xFF), cp.slot = 0, cp.why = p.why, cp.nodes = 7, cp.copy = 1, cp.probe.drawn = p.drawn;
            tbl.pieces.push_back(cp);
          }
          std::vector<WrestlerContributor::PaletteTarget> targets;
          uint32_t miss = 0;
          w.LatchTargets(6, &tbl, targets, miss);
          std::set<uint32_t> got;
          for (const auto& x : targets) got.insert(x.skel);
          if (offlatch_accepted) {   // F3: the latch read of a piece target made off the latch thread
            WrestlerContributor::SetOffLatchThread(off_latch);
            const int r = w.SubmitLatchConsumer(pcs.empty() ? 0 : pcs[0].model, 7, Pose(7, 0.2f).data(), 6);
            WrestlerContributor::SetOffLatchThread(false);
            *offlatch_accepted = r == WrestlerContributor::kSubmitted;
          }
          return got;
        };
        const uint32_t kHid = 0x70002000, kStock = 0x70002100, kOk = 0x70002200, kNoCopy = 0x70002300;
        // F1: a piece the game hides (inst +20 == 0) is never a target
        const auto f1 = targets_of(0, false, {{kHid, PieceWhy::Ok, 0}, {kOk, PieceWhy::Ok, 1}}, false);
        Check(!f1.count(kHid) && f1.count(kOk), "S105 CAWP F1: a piece the game hides (probe.drawn 0) is not a latch target; a drawn one is");
        Check(targets_of(WrestlerContributor::kFaultPushEverything, false, {{kHid, PieceWhy::Ok, 0}}, false).count(kHid), "S105 CAWP F1 planted twin (kFaultPushEverything = the old push): the hidden piece IS pushed (the check above fails it)");
        // F2: a not-Ok stock piece of a NAMED owner is no target (no permanent hole); Ok and NoCopy pieces are
        const auto f2 = targets_of(0, true, {{kStock, PieceWhy::KindUnproven, 1}, {kOk, PieceWhy::Ok, 1}, {kNoCopy, PieceWhy::NoCopy, 1}}, false);
        Check(!f2.count(kStock) && f2.count(kOk) && f2.count(kNoCopy), "S105 CAWP F2: a not-Ok piece of a named owner is not pushed (it would be a permanent hole); an Ok piece and a NoCopy piece are");
        Check(targets_of(WrestlerContributor::kFaultPushEverything, true, {{kStock, PieceWhy::KindUnproven, 1}}, false).count(kStock), "S105 CAWP F2 planted twin (kFaultPushEverything): the not-Ok named-owner piece IS pushed (the check above fails it)");
        // CAWE: the editor's hair (unnamed owner in slot 0, reader proof WrongDesc, own-rows route) is still pushed
        Check(targets_of(0, false, {{kStock, PieceWhy::WrongDesc, 1}}, false).count(kStock), "S105 CAWE: the character editor's hair (WrongDesc, unnamed slot-0 owner) is a latch target: the F1 / F2 filters keep it");
        // F3: a piece target read off the latch thread is refused; on the latch thread it is accepted
        bool acc_off = true, acc_on = false, acc_twin = false;
        targets_of(0, false, {{kOk, PieceWhy::Ok, 1}}, true, &acc_off);
        targets_of(0, false, {{kOk, PieceWhy::Ok, 1}}, false, &acc_on);
        targets_of(WrestlerContributor::kFaultHoldOffLatch, false, {{kOk, PieceWhy::Ok, 1}}, true, &acc_twin);
        Check(!acc_off && acc_on, "S105 CAWP F3: an off-latch piece read (the game running: torn rows) is refused; the latch thread's is accepted");
        Check(acc_twin, "S105 CAWP F3 planted twin (kFaultHoldOffLatch = accept an off-latch read): the torn piece IS submitted (the check above fails it)");
        // F4 / F5: the reader wants only what the emitter cannot draw from the owner's model; an over-cap count stores 0
        {
          WrestlerContributor w;
          w.SubmitSkin(0x1000, 0, kN, pa.data(), 5);
          w.SubmitSlotKey(0x1000, 34302);
          WrestlerContributor::SlotTable tbl;
          tbl.skels.push_back({0x1000, 0});
          tbl.state[0] = WrestlerContributor::kSlotOccupied;
          scene_inputs::CharPiece a, b, c;
          a.model = kOk, a.id = 10001, a.slot = 0, a.why = PieceWhy::Ok, a.nodes = 7, a.copy = 1;
          b.model = kStock, b.id = 10002, b.slot = 0, b.why = PieceWhy::KindUnproven, b.nodes = 7, b.copy = 1;
          c.model = kHid, c.id = 10003, c.slot = 0, c.why = PieceWhy::NoCount, c.nodes = 5000, c.copy = 1;
          tbl.pieces.push_back(a), tbl.pieces.push_back(b), tbl.pieces.push_back(c);
          std::vector<WrestlerContributor::PaletteTarget> tg;
          uint32_t miss = 0;
          w.LatchTargets(6, &tbl, tg, miss);
          Check(!w.GuestModelWanted(kOk) && w.GuestModelWanted(kStock), "S105 CAWP F4: an Ok piece of a named owner is not read by the guest model reader (it draws from the owner's model); a not-Ok one is");
          Check(w.PieceLinkCount(kHid) == 0 && !w.GuestModelWanted(kHid), "S105 CAWP F5: a count over the cap (5000 > 1024) is stored as 0 and never wanted");
        }
      }
      const Res old = drive200(WrestlerContributor::kFaultHookOnlyPieces);
      Check(!old.in_targets_each && old.stale > 0, "S105 CAWP planted twin (kFaultHookOnlyPieces = the old hook-only latch): the piece stops being a target, goes stale and is pruned (the check above fails it)");
    }
    // (2) the contributor: the body (slot 0, model_key 34302) and two own-skeleton pieces fed by the palette hook; the latch's table links them with the reader's proof
    auto run = [&](scene_inputs::PieceWhy why_a, scene_inputs::PieceWhy why_b, uint32_t drawn_owner_skel, SceneTables& t, WrestlerContributor& w, std::vector<uint32_t> ring = {}) {
      w.SubmitSkin(drawn_owner_skel, 0, kN, pa.data(), 7);
      w.SubmitSlotKey(drawn_owner_skel, 34302);
      if (drawn_owner_skel != 0x1000) w.SubmitSkin(0x1000, -1, kN, pb.data(), 7);   // the table's [char+48] skeleton lives but is not the one drawn as slot 0
      w.SubmitConsumer(0x70001000, 7, Pose(7, 0.3f).data(), 7);
      w.SubmitConsumer(0x70002000, 11, Pose(11, 0.4f).data(), 7);
      WrestlerContributor::SlotTable st;
      st.skels.push_back({0x1000, 0});
      st.state[0] = WrestlerContributor::kSlotOccupied;
      st.pieces.push_back({0x70001000, 10000, 0, why_a});
      st.pieces.push_back({0x70002000, 20002, 0, why_b});
      for (auto& pc : st.pieces) pc.copy = pc.why == PieceWhy::NoCopy ? 0u : 1u;   // S103 DTP fix: the raw copy word the reader would have read
      st.ring_models = ring;
      std::vector<WrestlerContributor::PaletteTarget> tg;
      uint32_t missing = 0;
      w.LatchTargets(7, &st, tg, missing);
      Counters c;
      t = EmitAt(w, c, 7);
    };
    auto find = [](const SceneTables& t, const std::string& id) -> const Object* {
      for (const Object& o : t.objects) if (t.Str(o.id) == id) return &o;
      return nullptr;
    };
    {
      SceneTables t;
      WrestlerContributor w;
      run(PieceWhy::Ok, PieceWhy::Ok, 0x1000, t, w);
      const Object* body = find(t, "actor:slot:0");
      const Object* pa_ = find(t, "skel:70001000");
      const Object* pb_ = find(t, "skel:70002000");
      Check(body && (body->flags & kObjVisible) && t.Str(t.assets[body->asset].entry) == "model_key:34302", "NRBF: the owner's entry is untouched (no ;minus, no second body)");
      Check(pa_ && !(pa_->flags & kObjVisible) && (pa_->flags & kObjDrawnByOwner) && t.assets[pa_->asset].pac == 0, "NRBF: a proven piece is invisible, flagged drawn-by-owner, with no model of its own");
      Check(pb_ && (pb_->flags & kObjDrawnByOwner) && pb_->bones_count == 11 && w.PieceByOwner() == 2, "NRBF: both pieces drawn by their owner (11-node palette kept), counted");
      Check(pa_ && pb_ && pa_->pad == 10000 && pa_->layer == 1 && pb_->pad == 20002 && pb_->layer == 1 && body && body->pad == 0 && body->layer == 0,
            "PZF2 EB: a drawn-by-owner piece carries its piece id (pad) and its owner's slot + 1 (layer) for the draw-time mesh check; the body carries none");
    }
    {
      SceneTables t;
      WrestlerContributor w;
      run(PieceWhy::Ok, PieceWhy::Unmapped, 0x1000, t, w);   // 20002 has an unmapped node: refused, a hole
      const Object* pb_ = find(t, "skel:70002000");
      Check(pb_ && !(pb_->flags & kObjDrawnByOwner) && w.PieceByOwner() == 1 && w.PieceWhyCount(PieceWhy::Unmapped) == 1, "NRBF: an Unmapped piece is not flagged (a hole), counted by reason");
      Check(pb_ && pb_->pad == 0 && pb_->layer == 0, "PZF2 EB: a refused piece carries no piece id / owner slot");
    }
    {
      SceneTables t;
      WrestlerContributor w;
      run(PieceWhy::Ok, PieceWhy::Ok, 0x2000, t, w);   // the drawn slot-0 body is 0x2000, the link was read from [char+48] = 0x1000: review F1
      const Object* pa_ = find(t, "skel:70001000");
      Check(find(t, "actor:slot:0") && pa_ && !(pa_->flags & kObjDrawnByOwner) && w.PieceByOwner() == 0 && w.PieceWhyCount(PieceWhy::OwnerMismatch) == 2, "NRBF plant: owner skeleton != the drawn slot body -> the pieces stay holes (OwnerMismatch)");
    }
    {
      SceneTables t;
      WrestlerContributor w;
      run(PieceWhy::Ok, PieceWhy::Ok, 0x1000, t, w, {0x70002000});   // NRBF F1: 0x70002000 is a ring model (the ring class table): the ring family draws it, it is no wrestler object
      Check(!find(t, "skel:70002000") && find(t, "skel:70001000") && w.RingLeftOut() >= 1, "NRBF: a ring model (apron skirt) is left out of the wrestler objects, a wrestler's piece is not");
    }
    {  // S103 W2B (docs\agents\s103\W2.md, bone_tag_rows.h): the spring-hair rows of proven pieces ride the owner's palette after its body rows, canonical order (piece id, then node), each tagged (piece id, node)
      using scene_inputs::CharPiece;
      auto w2b = [&](bool piece_ok, bool switch_on, bool rows_match, SceneTables& t) {
        WrestlerContributor w;
        w.SubmitSkin(0x1000, 0, kN, pa.data(), 7);
        w.SubmitSlotKey(0x1000, 34302);
        w.SubmitConsumer(0x70001000, 7, Pose(7, 0.3f).data(), 7);
        w.SubmitConsumer(0x70002000, 11, Pose(11, 0.4f).data(), 7);
        WrestlerContributor::SlotTable st;
        st.skels.push_back({0x1000, 0});
        st.state[0] = WrestlerContributor::kSlotOccupied;
        CharPiece p20002{0x70002000, 20002, 0, PieceWhy::Ok};   // reader order: 20002 first, then 10000: the canonical order must still put 10000 first
        p20002.copy = 1, p20002.free_nodes = {2, 5};
        CharPiece p10000{0x70001000, 10000, 0, piece_ok ? PieceWhy::Ok : PieceWhy::Unmapped};
        p10000.copy = 1, p10000.free_nodes = {3};
        p20002.rows.resize(32), p10000.rows.resize(rows_match ? 16 : 7);
        for (size_t k = 0; k < p20002.rows.size(); ++k) p20002.rows[k] = 200.f + float(k);   // row 0 = node 2: 200..215; row 1 = node 5: 216..231
        for (size_t k = 0; k < p10000.rows.size(); ++k) p10000.rows[k] = 100.f + float(k);   // node 3: 100..115
        st.pieces.push_back(p20002), st.pieces.push_back(p10000);
        std::vector<WrestlerContributor::PaletteTarget> tg;
        uint32_t missing = 0;
        rdr_scene::g_spring_hair_rows.store(switch_on);
        w.LatchTargets(7, &st, tg, missing);
        Counters c;
        t = EmitAt(w, c, 7);
        rdr_scene::g_spring_hair_rows.store(true);
      };
      auto body_of = [](const SceneTables& t) -> const Object* { for (const Object& o : t.objects) if (t.Str(o.id) == "actor:slot:0") return &o; return nullptr; };
      SceneTables t;
      w2b(true, true, true, t);
      const Object* b = body_of(t);
      Check(b && b->bones_count == kN + 3 && t.bone_tags.size() == 3 && t.bone_tags[0].piece_id == 10000 && t.bone_tags[0].node == 3 && t.bone_tags[1].piece_id == 20002 && t.bone_tags[1].node == 2 && t.bone_tags[2].piece_id == 20002 && t.bone_tags[2].node == 5,
            "W2B: the body's palette is its own rows + 3 spring-hair rows, tagged in canonical order (10000:3, 20002:2, 20002:5), whatever order the reader listed the pieces");
      Check(b && t.bone_tags.size() == 3 && t.bone_tags[0].bone == b->bones_first + kN && t.bone_tags[1].bone == b->bones_first + kN + 1 && t.bone_tags[2].bone == b->bones_first + kN + 2,
            "W2B: each tag names its own palette row (consecutive, right after the body's)");
      Check(b && t.bones[b->bones_first + kN].m[0] == 100.f && t.bones[b->bones_first + kN].m[15] == 115.f && t.bones[b->bones_first + kN + 1].m[0] == 200.f && t.bones[b->bones_first + kN + 2].m[0] == 216.f,
            "W2B: the rows are the piece's own bytes (no maths, no rest-pose fill)");
      bool body_same = b != nullptr;
      for (uint32_t k = 0; b && k < kN; ++k) body_same = body_same && std::memcmp(t.bones[b->bones_first + k].m, pa[k].m, 64) == 0;
      Check(body_same, "W2B: the body's own rows are untouched");
      SceneTables t2;
      w2b(false, true, true, t2);   // planted twin 1: a piece that is not Ok (Unmapped) gives no rows, and the tag set differs from the model's: a hole downstream
      Check(body_of(t2) && body_of(t2)->bones_count == kN + 2 && t2.bone_tags.size() == 2 && t2.bone_tags[0].piece_id == 20002, "W2B planted twin: only a proven piece contributes rows (an Unmapped one is not read)");
      SceneTables t3;
      w2b(true, false, true, t3);   // planted twin 2: the switch off = the old picture (no rows, no tags)
      Check(body_of(t3) && body_of(t3)->bones_count == kN && t3.bone_tags.empty(), "W2B planted twin: --wwe2k14_rdr_spring_hair_rows=false writes no rows and no tags (the old picture)");
      SceneTables t4;
      w2b(true, true, false, t4);   // planted twin 3: a piece whose rows were not all read (7 floats for 1 node) is never padded from the rest pose
      Check(body_of(t4) && body_of(t4)->bones_count == kN + 2 && t4.bone_tags.size() == 2, "W2B planted twin: a short / missing row is skipped, never filled (the model's tags then differ: a hole)");
      {  // review E: a body whose palette was NOT refreshed this record (kept from an earlier one) gets no spring-hair rows (they are this latch's)
        WrestlerContributor w;
        w.SubmitSkin(0x1000, 0, kN, pa.data(), 7);
        w.SubmitSlotKey(0x1000, 34302);
        WrestlerContributor::SlotTable st;
        st.skels.push_back({0x1000, 0});
        st.state[0] = WrestlerContributor::kSlotOccupied;
        CharPiece hp{0x70001000, 10000, 0, PieceWhy::Ok};
        hp.copy = 1, hp.free_nodes = {3}, hp.rows.assign(16, 1.f), hp.nodes = 40;
        st.pieces.push_back(hp);
        std::vector<WrestlerContributor::PaletteTarget> tg;
        uint32_t missing = 0;
        w.LatchTargets(7, &st, tg, missing);
        Counters c;
        SceneTables fresh = EmitAt(w, c, 7);
        SceneTables later = EmitAt(w, c, 8);   // no new palette: the kept one is emitted stale
        const Object* bf = body_of(fresh);
        const Object* bl = body_of(later);
        Check(bf && bf->bones_count == kN + 1 && fresh.bone_tags.size() == 1 && fresh.bone_tags[0].piece_nodes == 40, "W2B: the tag carries the piece model's node count (review G)");
        Check(!bl || (bl->bones_count == kN && later.bone_tags.empty()), "W2B (review E): a stale body gets no spring-hair rows");
      }
    }
    {  // S103 HGF (HGP.md section 6, review 1): the hair rows attach to the body palette of THEIR latch. REAL production order: record R runs the pre-latch (LatchTargets(hook R-1) reads the rows, SubmitConsumer tags the palette
       // R-1 + kPaletteLead = R), then Collect(R) can only pick a tag <= R - kPaletteLead: the PREVIOUS latch's palette, so it needs the rows of the PREVIOUS latch (kept beside the current set).
      using scene_inputs::CharPiece;
      auto body_of = [](const SceneTables& t) -> const Object* { for (const Object& o : t.objects) if (t.Str(o.id) == "actor:slot:0") return &o; return nullptr; };
      auto drive = [&](uint32_t faults, uint64_t first, uint64_t last, SceneTables& at_last) {
        WrestlerContributor w;
        w.SetFaultsForTest(faults);
        w.SubmitSkin(0x1000, 0, kN, pa.data(), first - 5);   // the update hook's sight of the body (gives the slot)
        w.SubmitSlotKey(0x1000, 34302);
        for (uint64_t rec = first; rec <= last; ++rec) {
          WrestlerContributor::SlotTable st;
          st.skels.push_back({0x1000, 0});
          st.state[0] = WrestlerContributor::kSlotOccupied;
          CharPiece hp{0x70001000, 10000, 0, PieceWhy::Ok};
          hp.copy = 1, hp.free_nodes = {3}, hp.rows.assign(16, float(rec)), hp.nodes = 40;   // the rows read at this latch carry the latch's record number
          st.pieces.push_back(hp);
          std::vector<WrestlerContributor::PaletteTarget> tg;
          uint32_t missing = 0;
          w.LatchTargets(rec - 1, &st, tg, missing);                                  // the pre-latch: hook R-1
          w.SubmitConsumer(0x1000, kN, pa.data(), rec - 1 + WrestlerContributor::kPaletteLead);   // the latch consumer palette
          Counters c;
          SceneTables t = EmitAt(w, c, rec - WrestlerContributor::kPaletteLead);       // record R (EmitAt takes the palette tag R - kPaletteLead): Collect(R) picks tag <= R - kPaletteLead
          if (rec == last) at_last = std::move(t);
        }
      };
      SceneTables real, twin_cur, twin_any;
      drive(0, 100, 102, real);
      const Object* br = body_of(real);
      Check(br && br->bones_count == kN + 1 && real.bone_tags.size() == 1 && real.bones[br->bones_first + kN].m[0] == 101.f,
            "HGF: in production order the rows are attached, and they are the PREVIOUS latch's (read at the moment of the body palette Collect picked): record 102 carries the rows read for record 101");
      drive(WrestlerContributor::kFaultHairCurrentSet, 100, 102, twin_cur);
      Check(body_of(twin_cur) && body_of(twin_cur)->bones_count == kN && twin_cur.bone_tags.empty(), "HGF planted twin (kFaultHairCurrentSet: the first rule, the CURRENT latch's rows): never true in the real order, the hair vanishes: the check above is what stops it");
      drive(WrestlerContributor::kFaultHairAnyFrame, 100, 102, twin_any);
      Check(body_of(twin_any) && body_of(twin_any)->bones_count == kN + 1 && twin_any.bones[body_of(twin_any)->bones_first + kN].m[0] == 102.f, "HGF planted twin (kFaultHairAnyFrame: the old rule) pairs the body with the NEWER latch's rows (102): the first check above is what stops it");
      {  // a palette matching neither set (a missed latch) = no rows, counted
        const uint64_t skew0 = rdr_scene::g_spring_census.rows_frame_skew.load();
        WrestlerContributor w;
        w.SubmitSkin(0x1000, 0, kN, pa.data(), 90);
        w.SubmitSlotKey(0x1000, 34302);
        WrestlerContributor::SlotTable st;
        st.skels.push_back({0x1000, 0});
        st.state[0] = WrestlerContributor::kSlotOccupied;
        CharPiece hp{0x70001000, 10000, 0, PieceWhy::Ok};
        hp.copy = 1, hp.free_nodes = {3}, hp.rows.assign(16, 5.f), hp.nodes = 40;
        st.pieces.push_back(hp);
        std::vector<WrestlerContributor::PaletteTarget> tg;
        uint32_t missing = 0;
        w.LatchTargets(100, &st, tg, missing);
        w.SubmitConsumer(0x1000, kN, pa.data(), 150);   // a palette of some other latch
        Counters c;
        SceneTables t = EmitAt(w, c, 151);
        Check(body_of(t) && body_of(t)->bones_count == kN && t.bone_tags.empty() && rdr_scene::g_spring_census.rows_frame_skew.load() == skew0 + 1, "HGF: a body palette that matches neither latch's rows gets no rows (absent, never skewed), counted hair_rows_frame_skew");
      }
    }
    {  // S103 HGF: the per-window census. max / mean / n / gap_big are read and zeroed by the text; a big gap is counted; the cumulative fields stay
      using rdr_scene::g_spring_census;
      (void)rdr_scene::SpringHairCensusText();   // reset
      const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      float farrow[16];
      std::memcpy(farrow, id, sizeof farrow);
      farrow[12] = 849.f;
      const float b[3] = {0, 0, 0};
      const float g_small = rdr_scene::SpringRootGapNote(id, id, b);
      const float g_big = rdr_scene::SpringRootGapNote(farrow, id, b);
      Check(g_small == 0.f && g_big > 848.f && g_big < 850.f && g_spring_census.gap_big.load() == 1 && g_spring_census.gap_n.load() == 2, "HGF: a gap > 20 units is counted gap_big; a zero gap is not");
      const std::string a = rdr_scene::SpringHairCensusText();
      const std::string a2 = rdr_scene::SpringHairCensusText();
      Check(a.find("max 849.000") != std::string::npos && a.find("(n 2, gap_big 1)") != std::string::npos && a2.find("max 0.000000") != std::string::npos && a2.find("(n 0, gap_big 0)") != std::string::npos,
            "HGF: the census max / n / gap_big are THIS WINDOW (the next print starts from zero); the old cumulative max hid every later spike");
      const float nanrow[16] = {NAN};
      Check(rdr_scene::SpringRootGapNote(nanrow, id, b) < 0.f && g_spring_census.gap_n.load() == 0, "HGF: a non-finite gap is dropped, not counted");
    }
    {  // S103 DTP (docs\research\s103\PEP.md section 3): the game set a piece's owner-copy flag to 0 (the People's Elbow pad): NoCopy -> kObjDetachedPiece, pad / layer set, an alpha-0 owner row
      SceneTables t;
      WrestlerContributor w;
      run(PieceWhy::NoCopy, PieceWhy::Ok, 0x1000, t, w);
      const Object* pa_ = find(t, "skel:70001000");
      const Object* pb_ = find(t, "skel:70002000");
      Check(pa_ && !(pa_->flags & kObjVisible) && (pa_->flags & kObjDetachedPiece) && !(pa_->flags & kObjDrawnByOwner), "DTP: a NoCopy piece is invisible and flagged detached (not drawn-by-owner)");
      Check(pa_ && pa_->pad == 10000 && pa_->layer == 1 && pa_->bones_count == 7, "DTP: a detached piece carries its piece id (pad), its owner's slot + 1 (layer) and its own palette");
      Check(pb_ && (pb_->flags & kObjDrawnByOwner) && !(pb_->flags & kObjDetachedPiece) && w.PieceByOwner() == 1 && w.PieceDetached() == 1 && w.PieceWhyCount(PieceWhy::NoCopy) == 1 && w.PieceWhyText().find("detached 1") != std::string::npos,
            "DTP: the Ok piece beside it stays drawn-by-owner; the census counts `detached 1`");
      rf::DetachedOwnerRowOut row;
      Check(pa_ && rf::DetachedOwnerRow(pa_->flags, pa_->pad, pa_->layer, true, row) && row.slot == 0 && row.piece == 10000 && row.alpha == 0.f, "DTP: the detached piece gives the owner an alpha-0 row for (slot 0, piece 10000)");
      Check(pa_ && !rf::DetachedOwnerRow(pa_->flags, pa_->pad, pa_->layer, false, row), "DTP planted twin (the flag ignored: rf::Plants().ignore_detached_flag) gives no row, so the pad stays drawn on the arm: the check above fails it");
      Check(pb_ && !rf::DetachedOwnerRow(pb_->flags, pb_->pad, pb_->layer, true, row), "DTP: a piece the owner draws (Ok) asks for no detach row");
      Check(!rf::DetachedOwnerRow(kObjDetachedPiece, 0, 1, true, row) && !rf::DetachedOwnerRow(kObjDetachedPiece, 10000, 0, true, row), "DTP: an older record with no piece id / owner slot gives no row (a hole as before)");
    }
    {
      SceneTables t;
      WrestlerContributor w;
      run(PieceWhy::NoCopy, PieceWhy::Ok, 0x2000, t, w);   // the drawn slot-0 body is 0x2000, the link was read from 0x1000: no owner identity, so no detach (OwnerMismatch)
      Check(find(t, "skel:70001000") && !(find(t, "skel:70001000")->flags & kObjDetachedPiece) && w.PieceDetached() == 0, "DTP: NoCopy with a mismatched owner skeleton is not detached");
      SceneTables t2;
      WrestlerContributor w2;
      rdr_scene::g_detached_pieces.store(false);
      run(PieceWhy::NoCopy, PieceWhy::Ok, 0x1000, t2, w2);
      rdr_scene::g_detached_pieces.store(true);
      Check(find(t2, "skel:70001000") && !(find(t2, "skel:70001000")->flags & kObjDetachedPiece) && w2.PieceDetached() == 0, "DTP: --wwe2k14_rdr_detached_pieces=false: the piece stays a plain hole (the old picture)");
    }
    {  // one `piece detach` line per change of the copy flag (1 -> 0, then 0 -> 1); nothing while it stays
      static int calls = 0, last_copy = -1;
      calls = 0, last_copy = -1;
      WrestlerContributor w;
      w.SetPieceDetachLog([](uint32_t, int32_t, uint32_t id, uint32_t, uint32_t copy, uint64_t) { if (id == 10000) ++calls, last_copy = int(copy); });
      SceneTables t;
      run(PieceWhy::Ok, PieceWhy::Ok, 0x1000, t, w);
      Check(calls == 0, "DTP: a piece that is copied from the start logs no detach");
      run(PieceWhy::NoCopy, PieceWhy::Ok, 0x1000, t, w);
      Check(calls == 1 && last_copy == 0, "DTP: the copy flag going to 0 logs one detach line");
      run(PieceWhy::NoCopy, PieceWhy::Ok, 0x1000, t, w);
      Check(calls == 1, "DTP: the flag staying 0 logs nothing more");
      run(PieceWhy::Ok, PieceWhy::Ok, 0x1000, t, w);
      Check(calls == 2 && last_copy == 1, "DTP: the flag going back to 1 logs one line");
    }
    {  // S103 DTP fix (review): after the ELBW the game sets only +20 = 0 and never puts +24 back: the pad's palette stops, goes stale, is pruned. The owner row must stay (the pad never returns to the arm)
      SceneTables t;
      WrestlerContributor w;
      run(PieceWhy::NoCopy, PieceWhy::Ok, 0x1000, t, w);
      Counters c2;
      SceneTables ts = EmitAt(w, c2, 9);   // no new palette for the pieces: the kept entries are stale now
      const Object* pa_ = find(ts, "skel:70001000");
      rf::DetachedOwnerRowOut row;
      Check(pa_ && (pa_->flags & kObjDetachedPiece) && !(pa_->flags & kObjSkinned) && pa_->bones_count == 0 && pa_->pad == 10000 && pa_->layer == 1, "DTP fix: a STALE detached piece keeps the flag, pad and layer, and is written without bones");
      Check(pa_ && rf::DetachedOwnerRow(pa_->flags, pa_->pad, pa_->layer, true, row) && row.alpha == 0.f, "DTP fix: the stale-after-move case still gives the owner its alpha-0 row (the pad is not drawn on the arm)");
      SceneTables tt;
      WrestlerContributor wt;
      wt.SetFaultsForTest(WrestlerContributor::kFaultDetachNeedsFresh);
      run(PieceWhy::NoCopy, PieceWhy::Ok, 0x1000, tt, wt);
      Counters c3;
      SceneTables tts = EmitAt(wt, c3, 9);
      const Object* qa_ = find(tts, "skel:70001000");
      Check(!(qa_ && (qa_->flags & kObjDetachedPiece)), "DTP fix planted twin (the flag needs a fresh palette: the old rule) loses the flag once the palette is stale, so the pad returns to the arm: the checks above fail it");
    }
    {  // S105 A5a (docs\research\s105\A5a_ADV.md): a CAW's own-skeleton piece (hair / necklace / braces) of a GUEST-ROW owner. PRODUCTION ORDER, as the game runs it: the update hook's exit palettes (SubmitSkin, derived,
       // NOT what the draw reads) -> the latch (LatchTargets: links + the copy probe, then each target's [skel+200] read submitted through SubmitLatchConsumer) -> Collect + EmitItems (Emit). entry: SubmitLatchConsumer / EmitItems.
      using scene_inputs::OwnRowsWhy;
#if WWE2K14_STREAM()
      static int a5a_mode = 2;   // mode Ours: the update hook keeps the derived heads the latch's mismatch rule compares with (Put: `CurrentMode() != Off`); the stream-free build is pinned to Ours
      scene_frame::g_mode_cvar.store(&a5a_mode);
#endif
      constexpr uint32_t kOwner = 0x1000, kPiece = 0x70001000;
      static std::map<uint32_t, uint32_t> a5mem;
      static std::set<uint32_t> a5bad;
      static WrestlerContributor* a5w;
      constexpr uint64_t kHook = 20;
      struct Mut {
        uint32_t copy = 1, drawn = 1;
        bool break_rows = false, owner_old_tag = false, off_latch = false, stream = false;
        bool behind = false;     // review F5: the piece's copy ran BEFORE the owner's own update of the pass: it holds the owner's PREVIOUS latch row
        float shift = 0.f;       // review F5: sub_82BD3620 shifted the owner by this (x) after the copy
        float trans_off = 0.f;   // review F5: the piece's translation is off by this much beyond any recorded shift
      };
      std::vector<Mat4> own_rows = Pose(kN, 0.5f), own_prev = Pose(kN, 0.4f), derived_piece = Pose(7, 0.3f);
      auto drive = [&](uint32_t faults, const Mut& m, WrestlerContributor& w, std::vector<Mat4>* latch_piece_out = nullptr) {
        std::vector<Mat4> piece_rows = Pose(7, 0.9f);
        const Mat4& src_row = m.behind ? own_prev[9] : own_rows[9];
        std::memcpy(piece_rows[6].m, src_row.m, sizeof(float) * 12);       // sub_8259D230: the owner's row 9 copied into the piece's row 6 (rotation)
        piece_rows[6].m[12] = src_row.m[12] - m.shift + m.trans_off;       // (the owner's translation was shifted after the copy: the piece keeps the unshifted one)
        piece_rows[6].m[13] = src_row.m[13], piece_rows[6].m[14] = src_row.m[14];
        if (m.break_rows) piece_rows[6].m[0] += 0.25f;                      // a bind / unposed / other-frame palette: not the owner's rotation
        if (latch_piece_out) *latch_piece_out = piece_rows;
        w.SetNoWalk(!m.stream);
        w.SetFaultsForTest(faults);
        w.SubmitSkin(kOwner, 0, kN, own_rows.data(), kHook);                // the owner's update exit: equals the latch read (no mismatch for the owner)
        w.StampDrawForTest(kOwner, kHook);                                  // S107 CAM: the stream-free build shows a body only while the game asked to draw it
        w.SubmitSkin(kPiece, -1, 7, derived_piece.data(), kHook);           // the piece's update exit: the game rewrites +200 AFTER it, so the latch read differs (the refusal's cause)
        if (m.shift != 0.f) w.ApplyDelta(kOwner, m.shift, 0.f, 0.f);        // sub_82BD3620 (the wwe2k14_skel_shift hook)
        // (review F8 b) the piece is READ the way the latch reads it: ReadCharacterPieces over a forged SUBM chain, the count from the update hook only because neither the wanted table nor the game's own count knows the model
        a5mem.clear(), a5bad.clear();
        const uint32_t ach = 0x41000000, anode = 0x41100000, acomp = 0x41200000, aarr = 0x41300000, amap = 0x41400000;
        a5mem[ach + 1152 + 20] = anode, a5mem[anode + 4] = scene_inputs::kSubmTag, a5mem[anode + 24] = acomp, a5mem[acomp + 72] = aarr, a5mem[acomp + 80] = aarr + 48;
        a5mem[aarr + 0] = kPiece, a5mem[aarr + 16] = 10000u << 16, a5mem[aarr + 12] = amap, a5mem[aarr + 20] = m.drawn, a5mem[aarr + 24] = m.copy == 0xFFFFFFFFu ? 1u : m.copy;
        if (m.copy == 0xFFFFFFFFu) a5bad.insert(aarr + 24);
        for (uint32_t i = 0; i < 7; ++i) a5mem[amap + 4 * i] = i == 6 ? 9u : i;   // the last mapped node: piece row 6 = owner row 9
        a5w = &w;
        static const scene_inputs::ReadFn a5rd = [](uint32_t a, void* dst, size_t n) -> bool {
          for (size_t k = 0; k < n; k += 4) {
            if (a5bad.count(a + uint32_t(k))) return false;
            auto it = a5mem.find(a + uint32_t(k));
            const uint32_t v = it == a5mem.end() ? 0u : it->second, be = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
            std::memcpy(static_cast<uint8_t*>(dst) + k, &be, 4);
          }
          return true;
        };
        static const scene_inputs::PieceCountFn a5hook = [](uint32_t mm) -> uint32_t { return a5w->WantCount(mm); };
        // one latch: LatchTargets (links + probe), then InputsContributor::LatchPalettes' read + SubmitLatchConsumer of each target
        const auto latch = [&](uint64_t hook, const Mat4* owner_pal, const Mat4* piece_pal, uint64_t owner_hook) {
          WrestlerContributor::SlotTable st;
          st.skels.push_back({kOwner, 0});
          st.state[0] = WrestlerContributor::kSlotOccupied;
          std::vector<scene_inputs::CharPiece> a5got;
          scene_inputs::ReadCharacterPieces(a5rd, ach, a5got, nullptr, nullptr, 0, 0, false, a5hook);
          for (auto& gp : a5got) gp.slot = 0, st.pieces.push_back(gp);
          std::vector<WrestlerContributor::PaletteTarget> tg;
          uint32_t missing = 0;
          w.LatchTargets(hook, &st, tg, missing);
          for (const auto& t : tg) {
            const bool is_piece = t.skel == kPiece;
            if (is_piece && m.off_latch) WrestlerContributor::SetOffLatchThread(true);
            w.SubmitLatchConsumer(t.skel, t.count, is_piece ? piece_pal : owner_pal, is_piece ? hook : owner_hook);
            WrestlerContributor::SetOffLatchThread(false);
          }
        };
        if (m.behind) {   // the PREVIOUS latch (hook kHook - 1) read the owner's previous palette and is collected by its own record: the owner's Kept then holds it as `prev_skin`
          latch(kHook - 1, own_prev.data(), piece_rows.data(), kHook - 1);
          Counters c0;
          (void)EmitAt(w, c0, kHook);
        }
        latch(kHook, own_rows.data(), piece_rows.data(), (m.owner_old_tag) ? kHook - 1 : kHook);
        Counters c;
        return EmitAt(w, c, kHook + WrestlerContributor::kPaletteLead);
      };
      const auto own_flag = [&](const SceneTables& t) { const Object* o = find(t, "skel:70001000"); return o && (o->flags & kObjOwnRowsPiece) != 0; };
      std::vector<Mat4> latch_piece;
      SceneTables t_ok;
      WrestlerContributor w_ok;
      t_ok = drive(0, Mut{}, w_ok, &latch_piece);
      {
        const Object* po = find(t_ok, "skel:70001000");
        Check(po && own_flag(t_ok) && !(po->flags & (kObjVisible | kObjDrawnByOwner | kObjDetachedPiece)) && po->pad == 10000 && po->layer == 1, "A5a: the piece of a guest-row owner is written invisible with kObjOwnRowsPiece, its piece id (pad) and owner slot + 1 (layer), never drawn-by-owner / detached");
        Check(po && po->bones_count == 7 && PaletteIs(t_ok, size_t(po - t_ok.objects.data()), latch_piece) && !PaletteIs(t_ok, size_t(po - t_ok.objects.data()), derived_piece),
              "A5a: its palette is the LATCH read of [piece+200] (the game's copy + spring), not the update-exit one");
        Check(w_ok.PieceLatchSubmitted() == 1 && w_ok.OwnRowsCount(OwnRowsWhy::Ok) == 1, "A5a: the latch accepted the piece's read (piece_latch_submitted 1) and the copy proof passed (own_rows ok 1)");
        Check(w_ok.OwnRowsText().find("ok 1") != std::string::npos && w_ok.PieceWhyText().find("own_rows [ok 1") != std::string::npos, "A5a: the 600-frame wrestlers line carries the own_rows counts");
      }
      {  // the stream build takes the same latch acceptance (the game is parked at the latch in both builds)
        Mut m;
        m.stream = true;
        WrestlerContributor w;
        Check(own_flag(drive(0, m, w)), "A5a: the stream build (walk present) writes the same flagged piece");
      }
      {  // review F5: the owner's copy may run BEFORE the owner's own update of the pass (the piece holds the owner's previous latch row), and the game shifts the owner after the copy (sub_82BD3620)
        Mut mb;
        mb.behind = true;
        WrestlerContributor wb;
        Check(own_flag(drive(0, mb, wb)), "A5a F5: a piece one copy behind (it holds the owner's PREVIOUS latch row) is accepted (a moving frame no longer refuses the hair)");
        WrestlerContributor wb2;
        Check(!own_flag(drive(WrestlerContributor::kFaultNoPrevRows, mb, wb2)), "A5a F5 planted twin (this latch's owner row only: the first rule): the one-behind piece is refused and the hair goes missing (the check above fails it)");
        Mut ms;
        ms.shift = 2.f;
        WrestlerContributor wsft;
        Check(own_flag(drive(0, ms, wsft)), "A5a F5: the owner's recorded shift (ApplyDelta after the copy) is allowed in the translation compare");
        Mut mt;
        mt.trans_off = 5.f;
        WrestlerContributor wt;
        const SceneTables tt = drive(0, mt, wt);
        Check(!own_flag(tt) && wt.OwnRowsCount(OwnRowsWhy::RowsDiffer) == 1, "A5a F5: a translation off by more than the recorded shift (a palette one frame of root motion away) is NOT accepted");
        WrestlerContributor wt2;
        Check(own_flag(drive(WrestlerContributor::kFaultRotationOnly, mt, wt2)), "A5a F5 planted twin (rotation only: the first rule): the off-translation piece IS accepted (the check above fails it)");
      }
      {  // the unsafe directions, each refused and counted (the piece stays the plain hole it was: no flag)
        Mut m;
        m.copy = 0;
        WrestlerContributor w;
        const SceneTables t = drive(0, m, w);
        const Object* cz = find(t, "skel:70001000");
        Check(!own_flag(t) && cz && (cz->flags & kObjDetachedPiece) && w.PieceDetached() == 1, "A5a: a copy word of 0 (read by the reader: NoCopy) is not this rule's: it is DTP's detached piece (the People's Elbow route), never an owner-row piece");
      }
      {
        Mut m;
        m.copy = 0xFFFFFFFFu;   // the probe could not read the copy word: never guessed
        WrestlerContributor w;
        const SceneTables t = drive(0, m, w);
        Check(!own_flag(t) && w.OwnRowsCount(OwnRowsWhy::ProbeUnread) == 1, "A5a: an unread probe (the copy word unreadable) is refused, never guessed");
      }
      {
        Mut m;
        m.break_rows = true;
        WrestlerContributor w;
        const SceneTables t = drive(0, m, w);
        Check(!own_flag(t) && w.OwnRowsCount(OwnRowsWhy::RowsDiffer) == 1, "A5a: a piece whose mapped row is not the owner's row of the same latch (an unposed / bind / other-frame palette) is NOT accepted");
      }
      {
        Mut m;
        m.owner_old_tag = true;
        WrestlerContributor w;
        const SceneTables t = drive(0, m, w);
        Check(!own_flag(t) && w.OwnRowsCount(OwnRowsWhy::TagDiffers) == 1, "A5a: a piece and an owner of different latches (a previous latch's body palette beside this latch's piece) are NOT paired");
      }
      {  // entry: scene_inputs::JudgeOwnRows, called directly: the owner's NAME comes from the update hook's slot key (SubmitSlotKey, hook code this unit does not run), so an end-to-end named owner cannot be driven here
        scene_inputs::OwnRowsCase oc;
        oc.probe.drawn = 1, oc.probe.special = 0, oc.probe.copy = 1, oc.probe.piece_node = 3, oc.probe.owner_node = 5;
        oc.owner_found = oc.owner_skel_matches = oc.piece_consumer = oc.owner_consumer = true;
        oc.owner_named = true;
        std::vector<Mat4> orows = Pose(kN, 0.5f), prows = Pose(7, 0.9f);
        std::memcpy(prows[3].m, orows[5].m, sizeof(float) * 16);
        oc.owner_rows = orows[0].m, oc.owner_count = orows.size(), oc.piece_rows = prows[0].m, oc.piece_count = prows.size();
        Check(scene_inputs::JudgeOwnRows(oc) == OwnRowsWhy::OwnerNamed, "A5a: a piece of a NAMED (stock) owner is not this rule's (the merged-body route draws it), proof or not");
        oc.owner_named = false;
        Check(scene_inputs::JudgeOwnRows(oc) == OwnRowsWhy::Ok, "A5a: the same pair with an unnamed owner and a good proof passes");
        oc.owner_found = false;
        Check(scene_inputs::JudgeOwnRows(oc) == OwnRowsWhy::NoOwnerItem, "A5a: an owner that is not an item of this record gives no route");
      }
      {
        Mut m;
        m.drawn = 0;
        WrestlerContributor w;
        const SceneTables t = drive(0, m, w);
        Check(find(t, "skel:70001000") == nullptr && w.OwnRowsCount(OwnRowsWhy::Hidden) == 1, "A5a: inst +20 == 0 (the game's SUBM loop skips it) writes NO object: not a hole, not drawn");
      }
      {
        Mut m;
        m.off_latch = true;
        WrestlerContributor w;
        const SceneTables t = drive(0, m, w);
        Check(!own_flag(t) && w.PieceLatchSubmitted() == 0, "A5a: the off-latch fallback's read (game threads running) is never accepted for a piece: the latch thread only");
      }
      // planted twins in the UNSAFE direction: the old rule's hole must come back / the unsafe piece must be accepted, so the checks above fail them
      {
        WrestlerContributor w;
        const SceneTables t = drive(WrestlerContributor::kFaultNoPieceLatch, Mut{}, w);
        const Object* po = find(t, "skel:70001000");
        Check(!own_flag(t) && po && po->bones_count == 7 && PaletteIs(t, size_t(po - t.objects.data()), derived_piece) && w.PieceLatchSubmitted() == 0 && w.OwnRowsCount(OwnRowsWhy::NotConsumer) == 1,
              "A5a planted twin (kFaultNoPieceLatch = the old refusal): the piece keeps its update-exit palette, is not flagged, and the hole reappears (the first checks above fail it)");
      }
      {
        Mut m;
        m.copy = 0xFFFFFFFFu;
        WrestlerContributor w;
        Check(!own_flag(drive(0, m, w)), "A5a: an unreadable copy word is not accepted (counted probe_unread)");
        Mut m2;
        m2.break_rows = true;
        WrestlerContributor w2;
        Check(own_flag(drive(WrestlerContributor::kFaultNoCopyProof, m2, w2)), "A5a planted twin (no proof): an unposed / other-frame piece IS flagged (the RowsDiffer check above fails it)");
        Mut m3;
        m3.owner_old_tag = true;
        WrestlerContributor w3;
        Check(own_flag(drive(WrestlerContributor::kFaultNoCopyProof, m3, w3)), "A5a planted twin (no proof): a piece of another latch than its owner IS flagged (the TagDiffers check above fails it)");
        Mut m4;
        m4.drawn = 0;
        WrestlerContributor w4;
        Check(find(drive(WrestlerContributor::kFaultNoCopyProof, m4, w4), "skel:70001000") != nullptr, "A5a planted twin (no proof): a piece the game hides gets an object (the Hidden check above fails it)");
      }
      {
        Mut m;
        m.off_latch = true;
        WrestlerContributor w;
        Check(own_flag(drive(WrestlerContributor::kFaultHoldOffLatch, m, w)), "A5a planted twin (kFaultHoldOffLatch = accept an off-latch read): the piece IS flagged (the off-latch check above fails it)");
      }
#if WWE2K14_STREAM()
      scene_frame::g_mode_cvar.store(nullptr);   // back to the unit's default (Off)
#endif
    }
    {  // the piece object is gone (pruned): the latch's link proof alone keeps the flag: a link-only invisible object, no bones
      SceneTables t;
      WrestlerContributor w;
      w.SubmitSkin(0x1000, 0, kN, pa.data(), 7);
      w.SubmitSlotKey(0x1000, 34302);
      WrestlerContributor::SlotTable st;
      st.skels.push_back({0x1000, 0});
      st.state[0] = WrestlerContributor::kSlotOccupied;
      st.pieces.push_back({0x70001000, 10000, 0, PieceWhy::NoCopy});
      st.pieces.back().copy = 0;
      std::vector<WrestlerContributor::PaletteTarget> tg;
      uint32_t missing = 0;
      w.LatchTargets(7, &st, tg, missing);
      Counters c;
      t = EmitAt(w, c, 7);
      const Object* pa_ = find(t, "skel:70001000");
      rf::DetachedOwnerRowOut row;
      Check(pa_ && !(pa_->flags & kObjVisible) && (pa_->flags & kObjDetachedPiece) && pa_->bones_count == 0 && pa_->pad == 10000 && pa_->layer == 1 && rf::DetachedOwnerRow(pa_->flags, pa_->pad, pa_->layer, true, row),
            "DTP fix: a NoCopy link whose piece has no object at all is written link-only (flag, pad, layer, no bones) and gives the owner row");
      SceneTables t2;
      WrestlerContributor w2;
      w2.SetFaultsForTest(WrestlerContributor::kFaultDetachNeedsFresh);
      w2.SubmitSkin(0x1000, 0, kN, pa.data(), 7);
      w2.SubmitSlotKey(0x1000, 34302);
      uint32_t missing2 = 0;
      std::vector<WrestlerContributor::PaletteTarget> tg2;
      w2.LatchTargets(7, &st, tg2, missing2);
      Counters c2;
      t2 = EmitAt(w2, c2, 7);
      Check(!find(t2, "skel:70001000"), "DTP fix planted twin (no link-only entry) writes nothing for it: the pad would be drawn on the arm");
      st.skels.clear();   // the owner slot is not drawn: nothing is written for a link-only piece
      SceneTables t3;
      WrestlerContributor w3;
      std::vector<WrestlerContributor::PaletteTarget> tg3;
      uint32_t missing3 = 0;
      w3.LatchTargets(7, &st, tg3, missing3);
      Counters c3;
      t3 = EmitAt(w3, c3, 7);
      Check(!find(t3, "skel:70001000"), "DTP fix: a link-only piece with no drawn owner is not written");
    }
    {  // piece id 0 is never flagged
      SceneTables t;
      WrestlerContributor w;
      w.SubmitSkin(0x1000, 0, kN, pa.data(), 7);
      w.SubmitSlotKey(0x1000, 34302);
      w.SubmitConsumer(0x70001000, 7, Pose(7, 0.3f).data(), 7);
      WrestlerContributor::SlotTable st;
      st.skels.push_back({0x1000, 0});
      st.state[0] = WrestlerContributor::kSlotOccupied;
      st.pieces.push_back({0x70001000, 0, 0, PieceWhy::NoCopy});
      std::vector<WrestlerContributor::PaletteTarget> tg;
      uint32_t missing = 0;
      w.LatchTargets(7, &st, tg, missing);
      Counters c;
      t = EmitAt(w, c, 7);
      Check(find(t, "skel:70001000") && !(find(t, "skel:70001000")->flags & kObjDetachedPiece), "DTP fix: piece id 0 is never flagged detached (a flag with no pad is unusable)");
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
  f.guest_frame = kFrame + WrestlerContributor::kPaletteLead;   // S103 W1
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
