// S103 lane W2B (docs\agents\s103\W2.md, W2_ADV.md): offline proof of the spring-hair rows of a wrestler's SUBM pieces. 0 game runs, no game files: synthetic JBOY blobs, a fake record, the pure headers.
//   1. the loader: forced names (kami_b / spring) are appended even when the body has them (the body keeps its copy), root -> Bip by the alias table (the Rock's pad), l_elbow is an extra with its tag, a piece never maps to
//      another piece's appended bone, the first owner match wins;
//   2. the alias table: 42 pairs, the pairs the Rock pad needs;
//   3. the palette plan (bone_tag_rows.h): a full match, an absent piece (its rows zero, the body kept), the count-only twin, malformed records, rows no model node takes (entry 29), the planted twin that refuses the body;
//   4. the record: section 66 round trip, and an old record without it.
// Build: tools\scene\build_w2b_unit.ps1   Run: out\tools\w2b_unit.exe [--plant NAME]   (plants make exactly their named check fail)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/assets/jboy.h"
#include "../../src/assets/piece_keys.h"
#include "../../src/renderer_scene/bone_tag_rows.h"
#include "../../src/renderer_scene/record_io.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

namespace {

void PutBe32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + size_t(i)] = uint8_t(v >> (24 - 8 * i));
}
void PutBeF(std::vector<uint8_t>& b, size_t off, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  PutBe32(b, off, v);
}

struct Node { std::string name; int parent; float tx; };

// A JBOY blob with no meshes and the given node table (80-byte records: name 16, translation +16, euler +32, parent +48).
std::vector<uint8_t> MakeJboy(const std::vector<Node>& nodes) {
  const uint32_t node_tab = 0x40, body_len = node_tab + 80u * uint32_t(nodes.size());
  std::vector<uint8_t> b(8 + body_len, 0);
  std::memcpy(b.data(), "JBOY", 4);
  PutBe32(b, 4, body_len);
  PutBe32(b, 8 + 0x18, uint32_t(nodes.size()));
  PutBe32(b, 8 + 0x20, node_tab);
  for (size_t i = 0; i < nodes.size(); ++i) {
    const size_t o = 8 + node_tab + 80 * i;
    std::memcpy(&b[o], nodes[i].name.c_str(), std::min<size_t>(15, nodes[i].name.size()));
    PutBeF(b, o + 16, nodes[i].tx);
    PutBe32(b, o + 48, nodes[i].parent < 0 ? 0xFFFFFFFFu : uint32_t(nodes[i].parent));
  }
  return b;
}

bool Load(assets::Model& m, const std::vector<uint8_t>& blob, int key) {
  assets::JboyOptions o;
  o.part = key == 0 ? "body" : "piece " + std::to_string(key);
  o.kind = 2, o.key = key;
  std::string err;
  const bool ok = assets::LoadJboy(blob, o, m, err);
  if (!ok) std::printf("  load error: %s\n", err.c_str());
  return ok;
}

int IndexOf(const assets::Model& m, const std::string& name, int piece_id) {
  for (size_t i = 0; i < m.bones.size(); ++i)
    if (m.bones[i].name == name && m.bones[i].piece_id == piece_id) return int(i);
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") plant = argv[i + 1];
  using namespace rdr_scene;

  // ---- 0. the entry filter (ENT): the game's own key list (sub_8271EF80) ----
  {
    const auto wanted = [&](uint32_t id) { return plant == "old_filter" ? assets::WrestlerEntryIsModelOld(id, false) : assets::WrestlerEntryIsModel(id, false); };
    Check(wanted(0) && wanted(26) && wanted(27) && wanted(10000) && wanted(10010) && wanted(20002) && wanted(21011) && wanted(30001), "entry filter: body, pads, hair, eyes and the game's other keys load");
    Check(wanted(28) && wanted(29), "entry filter: entries 28 / 29 (Mr. Socko, glasses, neck pieces; the game builds them) load: ch144 p0 has 23 meshes, 22 before");
    Check(wanted(32) && wanted(33) && wanted(1), "entry filter: 32 / 33 / 1 (built when present; no shipped attire has them)");
    Check(!wanted(60) && !wanted(100) && !wanted(110) && !wanted(40000) && !wanted(9000) && !wanted(27000), "entry filter: spring-hair text (60), AI data (100), motions (110), dummies (40000), 9000s and keys outside the game's list are not models");
    Check(assets::WrestlerEntryIsModel(3, true) && assets::WrestlerEntryIsModel(26, true), "entry filter: a prop pac keeps its slots 1..9");
  }
  // ---- 2. the alias table ----
  Check(assets::kNodeAliasCount == 42, "alias table: 42 (from, to) pairs (sub_82BD24C8, table 0x8392DDC0)");
  Check(std::strcmp(assets::NodeAliasOf("root"), "Bip") == 0 && std::strcmp(assets::NodeAliasOf("l_kote"), "Bip L Forearm") == 0 && std::strcmp(assets::NodeAliasOf("r_kote"), "Bip R Forearm") == 0 && std::strcmp(assets::NodeAliasOf("r_tsumasaki"), "Bip R Toe0") == 0 && std::strcmp(assets::NodeAliasOf("bone03"), "null") == 0, "alias: root -> Bip, l_kote -> Bip L Forearm, r_kote -> Bip R Forearm, r_tsumasaki -> Bip R Toe0, bone03 -> null (-1)");
  Check(assets::NodeAliasOf("Root") == nullptr && assets::NodeAliasOf("l_elbow") == nullptr && assets::NodeAliasOf("kami_b00") == nullptr, "alias: only a-z names, l_elbow has none");

  // ---- 1. the loader ----
  {
    // body: 6 nodes, including a chain name the piece also has (the 612 case)
    std::vector<Node> body = {{"Bip", -1, 0}, {"Bip Head", 0, 1}, {"Bip L UpperArm", 0, 1}, {"Bip L Forearm", 2, 1}, {"kami_b09", 1, 1}, {"kami_b09_00", 4, 1}};
    assets::Model m;
    Check(Load(m, MakeJboy(body), 0) && m.main_bones == 6 && m.bones.size() == 6 && m.bones[4].piece_id == -1, "loader: the body's own nodes are main_bones, tagged -1");
    // piece 10000: the hair chain (forced names, one of them also in the body), plus a body node by name
    std::vector<Node> hair = {{"Bip Head", -1, 0}, {"kami_b00", 0, 5}, {"kami_b00_00", 1, 5}, {"kami_b09", 0, 5}, {"kami_b09_00", 3, 5}, {"spring01", 0, 1}};
    Check(Load(m, MakeJboy(hair), 10000), "loader: a hair piece loads");
    Check(m.bones.size() == 6 + 5 && m.bones[0].name == "Bip" && m.bones[4].name == "kami_b09" && m.bones[4].piece_id == -1, "loader: five forced nodes appended, the body keeps its own kami_b09 (W2_ADV row 5)");
    const int a = IndexOf(m, "kami_b09", 10000), b0 = IndexOf(m, "kami_b00", 10000), sp = IndexOf(m, "spring01", 10000);
    Check(a >= 6 && b0 >= 6 && sp >= 6 && m.bones[size_t(a)].piece_node == 3 && m.bones[size_t(b0)].piece_node == 1, "loader: a forced name is appended with its (piece id, node index) tag even when the body has the name");
    Check(m.bones[size_t(IndexOf(m, "kami_b09_00", 10000))].parent == a, "loader: the piece's chain parent is the piece's own kami_b09, not the body's (the piece copy is simulated)");
    // piece 10010: the same forced name again: never maps to piece 10000's bone (R5)
    std::vector<Node> hair2 = {{"Bip Head", -1, 0}, {"kami_b00", 0, 5}};
    Check(Load(m, MakeJboy(hair2), 10010) && m.bones.size() == 6 + 5 + 1 && IndexOf(m, "kami_b00", 10010) == 11, "loader: a second piece's forced name gets its own bone (R5: never merged into another piece's)");
    // the extras are exactly the bones after main, in loader order: piece id ascending, node ascending
    bool canon = true;
    for (size_t i = m.main_bones + 1; i < m.bones.size(); ++i)
      canon = canon && (m.bones[i].piece_id > m.bones[i - 1].piece_id || (m.bones[i].piece_id == m.bones[i - 1].piece_id && m.bones[i].piece_node > m.bones[i - 1].piece_node));
    Check(canon, "loader: the extras are in canonical order (piece id ascending, then node)");
  }
  {
    // the Rock's pad (W2_ADV R1): piece 26 nodes `vector root Bip L UpperArm ... l_Elbow_j Bip L Forearm ... l_elbow`; root -> Bip by the alias, l_elbow has no alias and no owner node
    std::vector<Node> body = {{"Bip", -1, 0}, {"Bip L UpperArm", 0, 1}, {"Bip L Forearm", 1, 1}};
    std::vector<Node> pad = {{"vector", -1, 0}, {"root", 0, 0}, {"Bip L UpperArm", 1, 1}, {"l_Elbow_j", 2, 1}, {"Bip L Forearm", 3, 1}, {"l_elbow", 4, 1}};
    assets::Model m;
    const bool ok = Load(m, MakeJboy(body), 0) && Load(m, MakeJboy(pad), 26);
    Check(ok && IndexOf(m, "root", 26) < 0, "Rock pad: `root` is mapped to Bip by the alias table, not appended");
    Check(ok && IndexOf(m, "l_elbow", 26) >= 0 && IndexOf(m, "l_Elbow_j", 26) >= 0 && IndexOf(m, "vector", 26) >= 0, "Rock pad: l_elbow / l_Elbow_j / vector (no alias, no owner node) are the pad's extras, tagged (26, node)");
    Check(ok && IndexOf(m, "Bip L UpperArm", 26) < 0 && IndexOf(m, "Bip L Forearm", 26) < 0, "Rock pad: the body names map by name");
    Check(ok && m.bones.size() == 3 + 3, "Rock pad: three extras, in the tags the record side collects from the game's -1 words (copy flag 1)");
  }

  // ---- 3. the palette plan ----
  {
    using Tags = std::vector<std::pair<uint32_t, uint32_t>>;
    const Tags model = {{10000, 3}, {20002, 2}, {20002, 5}};
    const std::vector<uint32_t> mnodes = {40, 30, 30};   // the node count of each extra's piece entry (review G)   // canonical order; main = 4 body nodes
    auto tag = [](uint32_t bone, uint32_t p, uint32_t n) { BoneTag t; t.bone = bone, t.piece_id = p, t.node = n, t.piece_nodes = p == 10000 ? 40u : 30u; return t; };
    PalettePlan plan;
    const std::vector<BoneTag> full = {tag(104, 10000, 3), tag(105, 20002, 2), tag(106, 20002, 5)};   // object palette at 100: 4 body rows + 3
    Check(PlanPalette(100, 7, 7, 4, model, mnodes, full.data(), full.size(), plan) == PaletteVerdict::Match && plan.absent.empty() && plan.present_rows == 3 && plan.row_of[4] == 4 && plan.row_of[6] == 6 && plan.unmodelled == 0,
          "plan: every tag present, in order: all three extras drawn from their rows");
    // reader order differs from canonical: the plan follows the tags, not the position
    const std::vector<BoneTag> swapped = {tag(104, 20002, 2), tag(105, 20002, 5), tag(106, 10000, 3)};
    Check(PlanPalette(100, 7, 7, 4, model, mnodes, swapped.data(), swapped.size(), plan) == PaletteVerdict::Match && plan.row_of[4] == 6 && plan.row_of[5] == 4 && plan.row_of[6] == 5, "plan: rows are found by their tag, whatever the record's order");
    // an absent piece: the record has only the 20002 rows (10000 was not proved)
    const std::vector<BoneTag> no10000 = {tag(104, 20002, 2), tag(105, 20002, 5)};
    const bool plan_ok = PlanPalette(100, 6, 7, 4, model, mnodes, no10000.data(), no10000.size(), plan) == PaletteVerdict::Match && plan.absent.size() == 1 && plan.absent[0] == 10000 && plan.row_of[4] == -1 && plan.row_of[5] == 4 && plan.row_of[6] == 5;
    const bool body_drawn = !(plant == "refuse_body" && !plan.absent.empty());   // the planted twin: the rule W2_ADV R2 rejected (one unproved piece refuses the whole body) = rf_wrestlers' refuse_body_on_absent_piece
    Check(plan_ok && body_drawn, "plan: a piece with no rows is ABSENT (its node has no row: zeros, its meshes skipped), the body and the other piece still draw");
    // no extras at all in the record: every piece absent, body drawn (the old count-only check would call 4 != 7 a hole)
    Check(PlanPalette(100, 4, 7, 4, model, mnodes, nullptr, 0, plan) == PaletteVerdict::Match && plan.absent.size() == 2 && plan.present_rows == 0, "plan: a record with the body rows only: both pieces absent, the body draws (never a whole-wrestler hole for an unproved piece)");
    // a piece with a different node set (one node missing / one extra) is absent, never partially drawn
    const std::vector<BoneTag> partial = {tag(104, 10000, 3), tag(105, 20002, 2)};
    Check(PlanPalette(100, 6, 7, 4, model, mnodes, partial.data(), partial.size(), plan) == PaletteVerdict::Match && plan.absent.size() == 1 && plan.absent[0] == 20002 && plan.row_of[4] == 4 && plan.row_of[5] == -1 && plan.row_of[6] == -1, "plan: a piece with only some of its nodes is absent as a whole");
    // the count-only twin: a palette that merely has the model's size but wrong tags is NOT accepted as the model's rows
    const std::vector<BoneTag> wrong = {tag(104, 10000, 9), tag(105, 20002, 2), tag(106, 20002, 5)};
    Check(PlanPalette(100, 7, 7, 4, model, mnodes, wrong.data(), wrong.size(), plan) == PaletteVerdict::Match && plan.absent.size() == 1 && plan.absent[0] == 10000 && plan.row_of[4] == -1 && plan.unmodelled == 1,
          "plan: right size, a wrong tag: the piece is absent (a count-only accept would draw row 104 as node 3), the stray row is counted unmodelled");
    // review G: the same (piece, node) but another piece entry (node count): an attire swapped under the latch must not pair the wrong chains
    std::vector<BoneTag> other_entry = full;
    other_entry[1].piece_nodes = 31, other_entry[2].piece_nodes = 31;
    Check(PlanPalette(100, 7, 7, 4, model, mnodes, other_entry.data(), other_entry.size(), plan) == PaletteVerdict::Match && plan.absent.size() == 1 && plan.absent[0] == 20002 && plan.row_of[4] == 4, "plan (review G): a piece whose node count differs from the loader's entry is absent");
    // review A: the body prefix (what WrestlerCore links by) is the palette minus the tagged rows
    Check(BodyRowCount(100, 7, full.data(), full.size()) == 4 && BodyRowCount(100, 4, nullptr, 0) == 4 && BodyRowCount(100, 4, full.data(), full.size()) == 4 && BodyRowCount(0, 11, full.data(), full.size()) == 11 - 0, "BodyRowCount: 7 rows with 3 tags = a 4-node body; no tags / tags of another object change nothing");
    // review C: absent rows are zero matrices; the casters skip a chunk that skins to one
    {
      std::vector<Mat4> skin(6);
      for (Mat4& m : skin) { std::memset(m.m, 0, sizeof m.m); m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.f; }
      std::memset(skin[5].m, 0, sizeof skin[5].m);
      const std::vector<uint16_t> body_only = {0, 1, 2}, hair = {1, 5};
      Check(!SkinChunkAbsent(body_only, skin.data(), 6) && SkinChunkAbsent(hair, skin.data(), 6) && !SkinChunkAbsent(hair, static_cast<const Mat4*>(nullptr), 0), "casters (review C): a chunk that skins to an absent piece's zero row is skipped by SkinChunkAbsent, a body chunk is not");
    }
    // malformed
    const std::vector<BoneTag> on_body = {tag(103, 10000, 3)};
    Check(PlanPalette(100, 5, 7, 4, model, mnodes, on_body.data(), on_body.size(), plan) == PaletteVerdict::Tags, "plan: a tag on a body row is a malformed record (hole)");
    Check(PlanPalette(100, 6, 7, 4, model, mnodes, no10000.data(), 1, plan) == PaletteVerdict::Tags, "plan: an untagged extra row is a malformed record (hole)");
    Check(PlanPalette(100, 3, 7, 4, model, mnodes, nullptr, 0, plan) == PaletteVerdict::Count, "plan: fewer rows than the body is a hole");
    const std::vector<BoneTag> dup = {tag(104, 20002, 2), tag(105, 20002, 2)};
    Check(PlanPalette(100, 6, 7, 4, model, mnodes, dup.data(), dup.size(), plan) == PaletteVerdict::Match && plan.absent.size() == 2 && plan.row_of[5] == -1, "plan (review H): the same (piece, node) twice (two instances of one piece id) makes THAT piece absent, never a refusal of the body");
    // entry 29: a piece the loader does not hold; rows exist, no node takes them
    const std::vector<BoneTag> e29 = {tag(104, 29, 1), tag(105, 29, 2)};
    Check(PlanPalette(100, 6, 4, 4, Tags{}, std::vector<uint32_t>{}, e29.data(), e29.size(), plan) == PaletteVerdict::Match && plan.unmodelled == 2 && plan.absent.empty(), "plan: rows of a piece the model does not hold (entry 29) are counted unmodelled, the body draws");
    // another object's tags are not ours
    const std::vector<BoneTag> other = {tag(204, 10000, 3)};
    Check(PlanPalette(100, 4, 4, 4, Tags{}, std::vector<uint32_t>{}, other.data(), other.size(), plan) == PaletteVerdict::Match && plan.unmodelled == 0, "plan: tags of another object's palette are ignored");
  }

  // ---- 4. the record ----
  {
    SceneTables t;
    t.bones.resize(10);
    t.bone_tags.push_back(BoneTag{kBoneTagLayout, 8, 10000, 3});
    t.bone_tags.push_back(BoneTag{kBoneTagLayout, 9, 20002, 5});
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables u;
    std::string err;
    const bool ok = ParseRecord(bytes.data(), bytes.size(), u, err);
    Check(ok && u.bone_tags.size() == 2 && u.bone_tags[1].piece_id == 20002 && u.bone_tags[1].node == 5 && u.bone_tags[0].bone == 8, "record: section 66 round trips (and the header version stays 1: additive)");
    SceneTables v;
    v.bones.resize(2);
    const std::vector<uint8_t> old = SerializeRecord(v);
    SceneTables w;
    Check(ParseRecord(old.data(), old.size(), w, err) && w.bone_tags.empty(), "record: an old record has no section 66: no tags, so a model with piece nodes finds every piece absent (the body draws, hair skipped)");
  }

  std::printf("w2b_unit: %s\n", g_fail ? "FAILED" : "all checks passed");
  return g_fail ? 1 : 0;
}
