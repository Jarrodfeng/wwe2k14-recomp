// stage_posed_unit: the offline proof of S110 BARRICADE (docs\research\s110\BARRICADE.md): the stage slots the game draws POSED (src\renderer_scene\stage_posed.h). 0 game runs, no SDK, no game memory.
//   stage_posed_unit [--plant NAME]
// It runs the SHIPPED code against a FAKE GUEST: ReadPosedSlots (every slot 0..398 whose model has more than one node: n = [model+300] when [model+296] != 0, the palette [model+200], n x 64 bytes),
// PosedToRows / PosedFromRows, the record round trip (record_io) and the stage-fx contributor's own take (the hook line after BuildSlotRows). The ORACLE is written by hand from the note: node k of
// the palette is the 4x4 at palette + 64 k, the row is the packer's cut (row k = column k of the matrix: m[k], m[4+k], m[8+k], m[12+k]).
// A plant is a FAULT in the code under test; each must make a NAMED check fail (exit 1 when any check failed):
//   node1_only      only node 1 of the palette is read (the slot's world as one node)         -> "posed.five_nodes"
//   ignore_296      [model + 296] is not looked at (a model with no node table of its own)   -> "posed.no_node_table_none"
//   stride48        the palette is walked with a 48-byte stride                              -> "posed.five_nodes"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/ring_pose.h"
#include "../../src/renderer_scene/stage_posed.h"
#include "../../src/scene_live/stage_fx_contrib.h"

using namespace rdr_scene;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

struct Guest {
  std::map<uint32_t, uint8_t> b;
  void W(uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[a + uint32_t(i)] = uint8_t(v >> (24 - 8 * i));
  }
  void F(uint32_t a, float f) {
    uint32_t w;
    std::memcpy(&w, &f, 4);
    W(a, w);
  }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; ++i) {
      auto it = b.find(a + uint32_t(i));
      if (it == b.end()) return false;
      p[i] = it->second;
    }
    return true;
  }
};

constexpr uint32_t kStage = 0x40000000u, kVt = 0x70000000u;
// matrix k element i of a rig = a value that tells k and i apart (so a wrong node / stride / transpose changes the answer)
static float Elem(uint32_t rig, uint32_t k, uint32_t i) { return float(rig * 1000u + k * 20u + i) + 0.25f; }

// a rig model: node table flag, count, palette of n matrices
static void Model(Guest& g, uint32_t slot, uint32_t model, uint32_t flag, uint32_t n, uint32_t pal, uint32_t rig, bool nan_in_node = false) {
  g.W(kStage + kPosedStageModels + 4 * slot, model);
  g.W(model, kVt), g.W(kVt + 8, live::ringrows::kNodeCountFn);   // the skinned model's vtable: slot 2 = sub_82BD3AB8 (NodeCount's rule)
  g.W(model + kPosedModelCountFlag, flag);
  g.W(model + kPosedModelCount, n);
  g.W(model + kPosedModelPalette, pal);
  for (uint32_t k = 0; k < n; ++k)
    for (uint32_t i = 0; i < 16; ++i) g.F(pal + 64 * k + 4 * i, Elem(rig, k, i));
  if (nan_in_node && n > 2) g.F(pal + 64 * 2 + 4 * 5, std::nanf(""));
}

static Guest MakeGuest() {
  Guest g;
  for (uint32_t s = 0; s < kPosedSlots; ++s) g.W(kStage + kPosedStageModels + 4 * s, 0);
  Model(g, 390, 0x60000000u, 1, 5, 0x61000000u, 390);   // a corner rig: 5 nodes
  Model(g, 394, 0x60001000u, 1, 5, 0x61001000u, 394);   // its sign: 5 nodes
  Model(g, 25, 0x60002000u, 1, 1, 0x61002000u, 25);     // an ordinary barricade slot: ONE node (no rows)
  Model(g, 30, 0x60003000u, 0, 5, 0x61003000u, 30);     // [model + 296] == 0 and [+220] == 0: no skeleton at all (the game's count is -1): no rows
  g.W(0x60003000u + 220, 0);
  Model(g, 40, 0x60006000u, 0, 0, 0x61006000u, 40);     // [model + 296] == 0, [+220] = a delegate that holds 4 nodes: the game's count follows it (4), the palette is the model's own
  for (uint32_t k = 0; k < 4; ++k)
    for (uint32_t i = 0; i < 16; ++i) g.F(0x61006000u + 64 * k + 4 * i, Elem(40, k, i));
  g.W(0x60006000u + 220, 0x60007000u);
  g.W(0x60007000u, kVt), g.W(0x60007000u + kPosedModelCountFlag, 1), g.W(0x60007000u + kPosedModelCount, 4), g.W(0x60007000u + 220, 0);
  Model(g, 60, 0x60004000u, 1, 5, 0x61004000u, 60, true);   // a non-finite float in node 2: refused whole
  g.W(kStage + kPosedStageModels + 4 * 50, 0x62000000u);   // slot 50: a model object nobody can read
  Model(g, 398, 0x60005000u, 1, 3, 0x61005000u, 398);   // the last slot, 3 nodes
  return g;
}

static bool Read(const Guest& g, std::vector<PosedSlot>& out, uint32_t* bad = nullptr) {
  return ReadPosedSlots([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, kStage, out, bad);
}
static const PosedSlot* Find(const std::vector<PosedSlot>& v, uint32_t slot) {
  for (const PosedSlot& p : v)
    if (p.slot == slot) return &p;
  return nullptr;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) {
      const std::string p = argv[++i];
      g_posed_plant = p == "node1_only" ? 1 : p == "ignore_296" ? 2 : p == "stride48" ? 3 : 0;
    }
  const Guest g = MakeGuest();
  std::vector<PosedSlot> ps;
  uint32_t bad = 0;
  const bool ok = Read(g, ps, &bad);

  // ---- the rule: every slot with more than one node, from the game's values
  {
    std::string got;
    for (const PosedSlot& p : ps) got += " " + std::to_string(p.slot) + "x" + std::to_string(p.Nodes());
    Check(ok && Find(ps, 390) && Find(ps, 394) && Find(ps, 398), "posed.slots_found", "posed slots:" + got);
    Check(Find(ps, 40) && Find(ps, 40)->Nodes() == 4 && Find(ps, 40)->rows[0] != 0.f, "posed.count_follows_forward", "[+296] == 0 with a [+220] delegate: the game's count (4), as ringrows::NodeCount follows it");
    // oracle: node k's 12 floats = the packer's cut of matrix k
    bool five = false;
    if (const PosedSlot* p = Find(ps, 390)) {
      five = p->Nodes() == 5;
      for (uint32_t k = 0; k < 5 && five; ++k)
        for (uint32_t c = 0; c < 3 && five; ++c)
          for (uint32_t r = 0; r < 4 && five; ++r) five = p->rows[12 * k + 4 * c + r] == Elem(390, k, 4 * r + c);
    }
    Check(five, "posed.five_nodes", "slot 390: 5 nodes, node k = the 4x4 at palette + 64 k, rows cut as the packer cuts them");
    Check(Find(ps, 25) == nullptr, "posed.one_node_none", "a one-node slot gives no rows");
    Check(Find(ps, 30) == nullptr, "posed.no_node_table_none", "[model + 296] == 0: not followed, no rows");
    Check(Find(ps, 60) == nullptr && Find(ps, 50) == nullptr && bad >= 2, "posed.refused_isolated", "a non-finite palette and an unreadable model give no rows (bad " + std::to_string(bad) + ")");
    Check(Find(ps, 394) && Find(ps, 398) && Find(ps, 398)->Nodes() == 3, "posed.others_still_read", "the slots around a refused one are read");
    Guest g2;
    std::vector<PosedSlot> none;
    Check(!Read(g2, none) && none.empty() && !ReadPosedSlots([&g](uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }, 0, none), "posed.no_stage_array", "no stage / an unreadable model array = nothing");
  }

  // ---- the rows and the applier's read of them
  {
    std::vector<StageFxRow> rows;
    PosedToRows(ps, 77, rows);
    size_t want = 0;
    for (const PosedSlot& p : ps) want += p.Nodes();
    bool kinds = rows.size() == want && !rows.empty();
    for (const StageFxRow& r : rows) kinds = kinds && r.kind == kFxSlotPalette && r.seq == 77 && r.flags >= 2;
    std::map<int, PosedSlot> back;
    PosedFromRows(rows, back);
    bool same = back.size() == ps.size();
    for (const PosedSlot& p : ps) same = same && back.count(int(p.slot)) && back[int(p.slot)].rows == p.rows && back[int(p.slot)].model == p.model;
    Check(kinds && same, "posed.rows_roundtrip", "one row per node, the applier gets the same slots, nodes and floats back");
    std::vector<StageFxRow> torn = rows;
    for (size_t i = 0; i < torn.size(); ++i)
      if (torn[i].slot == 390 && torn[i].movie == 3) { torn.erase(torn.begin() + long(i)); break; }
    PosedFromRows(torn, back);
    Check(!back.count(390) && back.count(394), "posed.torn_set_dropped", "a slot missing a node is left out whole, the others stay");
    std::vector<StageFxRow> capped;
    PosedToRows(ps, 1, capped, 7);   // 5 + 5 + 3 nodes: the cap takes whole slots only
    std::map<int, uint32_t> per;
    for (const StageFxRow& r : capped) ++per[int(r.slot)];
    bool whole = capped.size() <= 7;
    for (const auto& kv : per) whole = whole && kv.second == Find(ps, uint32_t(kv.first))->Nodes();
    Check(whole && !capped.empty(), "posed.cap_whole_slots", "rows past the cap drop whole slots");

    // the record round trip (record_io accepts the new kind)
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    t.stage_fx = rows;
    const std::string path = "stage_posed_unit_roundtrip.rsc";
    std::string err;
    SceneTables rt;
    const bool rok = WriteRecord(path, t, err) && ReadRecord(path, rt, err);
    std::map<int, PosedSlot> rb;
    PosedFromRows(rt.stage_fx, rb);
    Check(rok && rb.size() == ps.size() && rb[390].rows == Find(ps, 390)->rows, "record.roundtrip", err);
    std::remove(path.c_str());
  }

  // ---- the draw's bones: changing ONLY palette row 2 moves ONLY the vertices bound to bone02 (the applier's path: rows -> WorldFromRowsCore -> the bound mesh's bones)
  {
    auto pose_of = [&](float node2_dy) {
      Guest rg;
      for (uint32_t s = 0; s < kPosedSlots; ++s) rg.W(kStage + kPosedStageModels + 4 * s, 0);
      Model(rg, 390, 0x60000000u, 1, 5, 0x61000000u, 390);
      for (uint32_t k = 0; k < 5; ++k)   // identity + a translation per node (row-vector 4x4: translation at [12..14])
        for (uint32_t i = 0; i < 16; ++i) rg.F(0x61000000u + 64 * k + 4 * i, i % 5 == 0 ? 1.f : i == 12 ? 10.f * float(k) : i == 13 && k == 2 ? node2_dy : 0.f);
      std::vector<PosedSlot> v;
      Read(rg, v);
      std::vector<StageFxRow> rows;
      PosedToRows(v, 1, rows);
      std::map<int, PosedSlot> m;
      PosedFromRows(rows, m);
      std::vector<assets::Matrix> pal;
      for (uint32_t k = 0; k < m[390].Nodes(); ++k) pal.push_back(rf::c::WorldFromRowsCore(&m[390].rows[size_t(k) * 12u]));
      assets::Mesh mesh;   // three vertices bound to bone01 / bone02 / bone03 (nodes 1 / 2 / 3: the file's palette slot -> node map)
      mesh.vertices.resize(3);
      for (assets::Vertex& vx : mesh.vertices) std::memset(&vx, 0, sizeof vx);
      mesh.src_skin_pairs = 1;
      mesh.src_skin_slots = {0, 255, 255, 255, 1, 255, 255, 255, 2, 255, 255, 255};
      mesh.src_skin_weights = {1.f, 0, 0, 0, 1.f, 0, 0, 0, 1.f, 0, 0, 0};
      mesh.palette_nodes = {1, 2, 3};
      const bool bound = rf::c::BindMesh(mesh);
      assets::Mesh out = mesh;
      rf::c::PoseMesh(mesh, pal.data(), pal.size(), out);
      std::vector<float> res;
      for (const assets::Vertex& vx : out.vertices) res.insert(res.end(), vx.pos, vx.pos + 3);
      if (!bound) res.clear();
      return res;
    };
    const std::vector<float> a = pose_of(0.f), b = pose_of(5.f);
    bool iso = a.size() == 9 && b.size() == 9;
    for (size_t i = 0; iso && i < 9; ++i) {
      const bool moved = std::fabs(a[i] - b[i]) > 1e-4f;
      iso = (i / 3 == 1) ? (moved == (i % 3 == 1)) : !moved;   // only vertex 1 (bone02), and only its y
    }
    Check(iso, "bones.row2_moves_only_bone02", "palette row 2 +5 in y: only the bone02 vertex moves (and only in y); bone01 / bone03 vertices stay");
  }

  // ---- C: a frame without rows keeps the last rows of the same stage generation; a new generation drops them. D: one of world / palette rows per slot
  {
    auto mk = [&](uint32_t slot) {
      std::map<int, PosedSlot> m;
      m[int(slot)] = *Find(ps, slot);
      return m;
    };
    PosedKeep keep;
    PosedAdopt(keep, true, 7, mk(390));
    PosedAdopt(keep, true, 7, {});            // an unread frame: no rows
    const bool kept = keep.last.count(390) == 1;
    PosedAdopt(keep, false, 0, {});           // a frame with no marker: nothing changes
    const bool kept2 = keep.last.count(390) == 1;
    PosedAdopt(keep, true, 8, mk(394));       // a new stage generation: the old rig is gone
    Check(kept && kept2 && !keep.last.count(390) && keep.last.count(394), "keep.last_rows_same_generation", "no rows this frame = the last rows; a new generation starts empty");
    std::vector<PosedSlot> two = {*Find(ps, 390), *Find(ps, 394)};
    std::vector<StageFxRow> have(1);
    have[0].kind = kFxSlotWorld, have[0].slot = 394;
    PosedWithoutWorldRows(two, have);
    Check(two.size() == 1 && two[0].slot == 390, "rows.one_kind_per_slot", "a slot that has a kFxSlotWorld row gets no palette rows");
    // S110 review: the applier adopts rows only from a marker of ITS arena (VisIsForArena, as VisAdoptRows): a stage of another arena (the editor's, one being freed) neither clears nor replaces them
    auto frame_rows = [&](uint32_t gen, uint32_t arena, uint32_t slot) {
      std::vector<StageFxRow> r;
      StageVis v;
      v.marker = true, v.known = true, v.gen = gen, v.arena = arena;
      VisToRows(v, r);
      std::vector<PosedSlot> one = {*Find(ps, slot)};
      PosedToRows(one, 1, r);
      return r;
    };
    PosedKeep k2;
    PosedAdoptRows(frame_rows(7, 4, 390), 4, k2);
    const std::vector<float> mine = k2.last.count(390) ? k2.last[390].rows : std::vector<float>();
    PosedAdoptRows(frame_rows(9, 50, 394), 4, k2);   // another arena's stage, another generation
    Check(k2.last.count(390) && k2.last[390].rows == mine && !k2.last.count(394) && k2.gen == 7, "keep.other_arena_ignored", "rows under a marker of another arena are not adopted and do not clear this arena's rows");
    PosedAdoptRows(frame_rows(8, 4, 394), 4, k2);    // this arena, a new generation
    Check(!k2.last.count(390) && k2.last.count(394), "keep.new_generation_same_arena", "a new generation of this arena's stage drops the old rows");
  }

  // ---- S110 review: the row cap holds the worst shipped arena (bg36: slots 0-14 x 15 nodes + slot 18 x 7 + the corners 390-393 x 5 = 252 rows, asset index census); the corners are never dropped
  {
    Guest g36;
    for (uint32_t s = 0; s < kPosedSlots; ++s) g36.W(kStage + kPosedStageModels + 4 * s, 0);
    for (uint32_t s = 0; s <= 14; ++s) Model(g36, s, 0x63000000u + 0x1000u * s, 1, 15, 0x64000000u + 0x1000u * s, s);
    Model(g36, 18, 0x63000000u + 0x1000u * 18, 1, 7, 0x64000000u + 0x1000u * 18, 18);
    for (uint32_t s = 390; s <= 393; ++s) Model(g36, s, 0x63000000u + 0x1000u * s, 1, 5, 0x64000000u + 0x1000u * s, s);
    std::vector<PosedSlot> v;
    Read(g36, v);
    std::vector<StageFxRow> rows;
    const size_t dropped = PosedToRows(v, 1, rows);
    std::map<int, PosedSlot> m;
    PosedFromRows(rows, m);
    Check(m.count(390) && m.count(393) && dropped == 0 && rows.size() == 252, "posed.cap_holds_worst_arena", "bg36's 252 palette rows fit; the corners are not dropped behind the 15-node ground slots (rows " + std::to_string(rows.size()) + ", dropped " + std::to_string(dropped) + ")");
    std::vector<StageFxRow> small;
    Check(PosedToRows(v, 1, small, 128) > 0, "posed.cap_counts_drops", "a cap that is hit reports the slots it dropped (the census counts them)");
  }

  // ---- the production hook: the contributor's own take writes the rows
  {
    static Guest* gp;
    Guest gg = MakeGuest();
    gp = &gg;
    static int mode = 2;
    scene_frame::g_mode_cvar.store(&mode);
    rdr_scene::live::stagefx::StageFxContributor con;
    con.SetReader(+[](uint32_t a, void* d, size_t n) { return gp->Read(a, d, n); });
    con.SetStage(kStage);
    rdr_scene::live::FrameInfo fi;
    fi.guest_frame = 10;
    con.Latch(fi);
    SceneTables t;
    rdr_scene::live::Counters cn;
    con.Emit(fi, t, cn);
    std::map<int, PosedSlot> got;
    PosedFromRows(t.stage_fx, got);
    Check(got.count(390) && got.count(394) && got[390].Nodes() == 5 && got[390].rows == Find(ps, 390)->rows && !got.count(25), "take.writes_posed_rows", "the contributor's take carries the rig's palette rows");
  }

  std::printf("%s\n", g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
