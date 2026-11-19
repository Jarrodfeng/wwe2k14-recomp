// S103 W2B (docs\agents\s103\W2.md, W2_ADV.md): the spring-hair rows of a wrestler's SUBM pieces. A piece node the game's node map leaves -1 (a name `kami_b*` / `spring*`, or one the owner lacks after the alias
// table, node_alias.h) has its palette row written every frame by the spring-hair simulation ([[inst+0]+200] + 64 * j), already in the body's bind space (no new maths). The wrestlers contributor appends the rows of
// every piece the reader PROVED to the body's palette after its own nodes, in the CANONICAL order (piece id ascending, then node index ascending), one BoneTag per appended row. The front end knows the same nodes from the
// files (Model::bones[main..], Bone::piece_id / piece_node). rf_wrestlers plans the palette per PIECE: a piece whose tags equal the model's is drawn from its rows; a piece the record cannot prove (no rows, a different
// node set, a piece the loader does not hold) is ABSENT: only the meshes that skin to its extra nodes are skipped (counted), the body always draws; its palette rows are zero and never read. A row is never filled from the
// rest pose (the hair would freeze to the head). Pure: no renderer, no game, no SDK (tools\scene\w2b_unit.cpp runs THIS code). Record section 66 (kScBoneTags), additive.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rdr_scene {

inline constexpr uint32_t kScBoneTags = 66;   // BoneTag[]: one row per palette row after a wrestler object's body rows. Optional (65 = PropsLight)
inline constexpr uint32_t kBoneTagLayout = 2;   // 2: + piece_nodes (review G)
struct BoneTag {
  uint32_t layout = kBoneTagLayout;
  uint32_t bone = 0;       // the absolute index into kScBones of the palette row this tags (rows of one object are consecutive, so an object's tags are the ones inside its range)
  uint32_t piece_id = 0;   // the SUBM piece's pac entry id
  uint32_t node = 0;       // the node's index in the PIECE's own node table
  uint32_t piece_nodes = 0;   // the piece model's node count as the latch read it (GameNodeCount): compared with the loader's piece entry, so a swapped attire cannot pair the wrong chains (review G)
};
static_assert(sizeof(BoneTag) == 20, "BoneTag is a flat POD table");

// --wwe2k14_rdr_spring_hair_rows (default ON, start_settings.cpp stores it). Off = the SUBM reader treats a -1 map word as Unmapped again, no rows are written, and an attire with piece nodes is a hole (the old picture).
inline std::atomic<bool> g_spring_hair_rows{true};

// Canonical order of the extra rows: by piece id, then node index. Stable: two instances of one piece id keep their reader order, which then never equals the model's tags (the piece is absent, fail closed).
struct ExtraKey {
  uint32_t piece_id = 0, node = 0;
};
inline bool ExtraKeyLess(const ExtraKey& a, const ExtraKey& b) { return a.piece_id != b.piece_id ? a.piece_id < b.piece_id : a.node < b.node; }

// The BODY's own row count of a record object's palette: bones_count minus the spring-hair rows appended after it (the tags inside [bones_first, bones_first + bones_count)). Tags are sorted by `bone` (ParseRecord
// requires it; the contributor writes them in order), so two lower_bounds find the range. Every user that needs the skeleton's node count (WrestlerCore link, the caster hashes, the CAW palette probe) uses this.
inline uint32_t BodyRowCount(uint32_t bones_first, uint32_t bones_count, const BoneTag* tags, size_t ntags) {
  if (!ntags) return bones_count;
  const auto less = [](const BoneTag& t, uint32_t v) { return t.bone < v; };
  const BoneTag* lo = std::lower_bound(tags, tags + ntags, bones_first, less);
  const BoneTag* hi = std::lower_bound(tags, tags + ntags, bones_first + bones_count, less);
  const uint32_t extra = uint32_t(hi - lo);
  return extra <= bones_count ? bones_count - extra : bones_count;
}

// An ABSENT piece's palette rows are written as zero matrices (rf_wrestlers: never a rest-pose row). A real palette row is a rigid transform with w = 1, so a row with a zero w and a zero diagonal can only be one of those. A chunk
// whose slots reference such a row is not drawn, and the casters (shadow map fit, rim depth, plane mask, chain) must skip it too: skinned with zeros its vertices collapse to the origin and stretch the fit (review C).
inline bool SkinRowAbsent(const float* m) { return m[15] == 0.f && m[0] == 0.f && m[5] == 0.f && m[10] == 0.f; }
template <class Nodes, class Row>
inline bool SkinChunkAbsent(const Nodes& nodes, const Row* skin, uint32_t skin_count) {
  if (!skin) return false;
  for (auto node : nodes)
    if (uint32_t(node) < skin_count && SkinRowAbsent(skin[node].m)) return true;
  return false;
}

enum class PaletteVerdict : uint8_t { Match = 0, Count = 1, Tags = 2 };   // Count: fewer rows than the body has; Tags: the extra rows are not tagged one for one, in order, right after the body's

// What rf_wrestlers draws from a record object's palette. row_of[k] (k < nodes) = the row of the record object's palette (relative to bones_first) that feeds skeleton node k, or -1 = no row (the node's piece is
// absent: the front end gets a zero matrix and skips the meshes that skin to it). absent = the piece ids (ascending) of the model's extra nodes the record cannot supply completely.
struct PalettePlan {
  std::vector<int32_t> row_of;
  std::vector<uint32_t> absent;
  uint32_t unmodelled = 0;   // record extra rows that no model node takes (a piece the loader does not hold: entries 28 / 29 / 32 / 33 / 1, or a node set that differs)
  uint32_t present_rows = 0; // extra rows used
};

// `bones_count` / `bones_first`: the record object's palette; `nodes` / `main_nodes` / `model_tags`: the model's (FrontEnd::ModelNodeCount / ModelMainNodeCount / ModelExtraTags; a model with no piece nodes has
// main_nodes == nodes and no tags); `tags` / `ntags`: the record's section. Match = a plan was made (possibly with absent pieces); Count / Tags = the record is malformed (the body is refused: a hole).
inline PaletteVerdict PlanPalette(uint32_t bones_first, uint32_t bones_count, uint32_t nodes, uint32_t main_nodes, const std::vector<std::pair<uint32_t, uint32_t>>& model_tags, const std::vector<uint32_t>& model_piece_nodes, const BoneTag* tags, size_t ntags, PalettePlan& out) {
  out = PalettePlan();
  if (main_nodes > nodes || model_tags.size() != size_t(nodes - main_nodes) || model_piece_nodes.size() != model_tags.size()) return PaletteVerdict::Tags;   // the model's own tags must cover exactly its extra bones
  if (bones_count < main_nodes) return PaletteVerdict::Count;
  const uint64_t lo = uint64_t(bones_first), body_end = lo + main_nodes, hi = lo + bones_count;
  uint32_t in_object = 0;
  struct RowInfo { uint32_t row = 0, piece_nodes = 0; };
  std::map<std::pair<uint32_t, uint32_t>, RowInfo> row_of_tag;   // (piece, node) -> row (relative) and the piece's node count the latch read
  std::map<uint32_t, uint32_t> piece_nodes_of;                    // piece id -> the node count its tags carry
  std::vector<uint32_t> bad_pieces;                               // a piece id twice (two instances of one id) or with two node counts: that piece is absent (review H), never a refusal of the body
  for (size_t i = 0; i < ntags; ++i) {
    if (tags[i].layout != kBoneTagLayout) return PaletteVerdict::Tags;
    if (tags[i].bone < lo || tags[i].bone >= hi) continue;   // another object's
    if (tags[i].bone < body_end) return PaletteVerdict::Tags;   // a tag on a body row
    if (tags[i].bone != body_end + in_object) return PaletteVerdict::Tags;   // not consecutive from the end of the body's rows
    if (!row_of_tag.emplace(std::make_pair(tags[i].piece_id, tags[i].node), RowInfo{uint32_t(tags[i].bone - lo), tags[i].piece_nodes}).second) bad_pieces.push_back(tags[i].piece_id);
    const auto pn = piece_nodes_of.emplace(tags[i].piece_id, tags[i].piece_nodes);
    if (!pn.second && pn.first->second != tags[i].piece_nodes) bad_pieces.push_back(tags[i].piece_id);
    ++in_object;
  }
  if (in_object != bones_count - main_nodes) return PaletteVerdict::Tags;   // an untagged extra row
  std::sort(bad_pieces.begin(), bad_pieces.end());
  out.row_of.assign(nodes, -1);
  for (uint32_t k = 0; k < main_nodes; ++k) out.row_of[k] = int32_t(k);
  std::map<uint32_t, uint32_t> model_n, record_n, matched_n;   // per piece id
  for (const auto& mt : model_tags) ++model_n[mt.first];
  for (const auto& kv : row_of_tag) ++record_n[kv.first.first];
  for (size_t e = 0; e < model_tags.size(); ++e) {
    const auto it = row_of_tag.find(model_tags[e]);
    if (it != row_of_tag.end() && it->second.piece_nodes == model_piece_nodes[e]) ++matched_n[model_tags[e].first];   // review G: the same piece entry (node count) as the loader's
  }
  for (const auto& kv : model_n)
    if (!(matched_n[kv.first] == kv.second && record_n[kv.first] == kv.second) || std::binary_search(bad_pieces.begin(), bad_pieces.end(), kv.first)) out.absent.push_back(kv.first);   // complete and exact, or the piece is absent
  for (size_t e = 0; e < model_tags.size(); ++e) {
    if (std::binary_search(out.absent.begin(), out.absent.end(), model_tags[e].first)) continue;
    out.row_of[main_nodes + e] = int32_t(row_of_tag[model_tags[e]].row);
    ++out.present_rows;
  }
  out.unmodelled = in_object - out.present_rows;
  return PaletteVerdict::Match;
}

// The census (`[scene][wrestlers] hair rows ...`, printed with the wrestler counters every 600 guest frames). rows = spring-hair rows the latch wrote into records. The rest are draw-side: drawn = bodies of a model with
// piece nodes drawn (with every piece present or not), refused = a malformed record (tags / count), unreadable = a proven piece whose rows could not be read at the latch (it is absent), hair_piece_skipped = (body, piece)
// pairs whose extra nodes the record could not supply (the piece's rows stay out and its meshes that skin to them are skipped: why = the latch's piece proof census, `piece_why` in the wrestlers line),
// hair_mesh_skipped = those meshes, rows_unmodelled = record rows no loaded node takes (pieces 28 / 29 / 32 / 33 / 1 are not loaded by our asset loader).
struct SpringHairCensus {
  std::atomic<uint64_t> rows{0}, drawn{0}, refused_tags{0}, refused_count{0}, refused_unreadable{0}, piece_skipped{0}, mesh_skipped{0}, rows_unmodelled{0};
  std::atomic<uint64_t> ent_drawn{0}, ent_hidden{0};   // S103 ENT R2: pieces 28 / 29 (Socko, glasses, neck pieces) drawn by their owner / hidden by the game while the owner is drawn, summed over frames ([U] whether the game hides them)
  std::atomic<uint32_t> gap_max_e3{0};   // W2_ADV 2c [U] / review D: for a hair ROOT j with body parent p and bind position b: |b x row_j - b x row_p| x 1000 (file units). At rest the root follows its parent, so a gap that grows with |b| shows a translation-scale mismatch. S103 HGF: THIS WINDOW (SpringHairCensusText reads and zeroes max / sum / n / big); row_j is the row the front end draws (after corr^-1)
  std::atomic<uint64_t> gap_sum_e3{0}, gap_n{0}, gap_big{0};   // gap_big = samples with a gap > kSpringGapBig (HGP: ~10x the steady 1-2 units; run 030's single 849 sample hid every later spike in a cumulative max)
  std::atomic<uint64_t> rows_unposed{0};       // S103 HGF: pieces whose rows at a latch all still equalled the bind palette (identity: the game's first spring / SUBM update had not run): absent for that latch (hair_rows_unposed)
  std::atomic<uint64_t> rows_frame_skew{0};    // S103 HGF: bodies whose hair rows were read in another latch than the body's palette: the rows are not attached that record (hair_rows_frame_skew)
};
inline SpringHairCensus g_spring_census;
inline constexpr float kSpringGapBig = 20.f;   // file units
// The hair root's position through a row (row vector x row, translation in [12..14]): b x row.
inline void SpringRootWorld(const float* row, const float* b, float* out) {
  for (int k = 0; k < 3; ++k) out[k] = b[0] * row[k] + b[1] * row[4 + k] + b[2] * row[8 + k] + row[12 + k];
}
// Census only: no row is changed. Returns the gap (file units) or -1 when it is not a finite number < 1e6 (then nothing is counted). `row_j` must be the row the front end will DRAW (after corr^-1); `wj` / `wp`
// (optional) receive b x row_j and b x row_p for the log line.
inline float SpringRootGapNote(const float* row_j, const float* row_p, const float* b, float* wj = nullptr, float* wp = nullptr) {
  float pj[3], pp[3], d2 = 0.f;
  SpringRootWorld(row_j, b, pj);
  SpringRootWorld(row_p, b, pp);
  for (int k = 0; k < 3; ++k) d2 += (pj[k] - pp[k]) * (pj[k] - pp[k]);
  if (wj) for (int k = 0; k < 3; ++k) wj[k] = pj[k];
  if (wp) for (int k = 0; k < 3; ++k) wp[k] = pp[k];
  const float g = std::sqrt(d2);
  if (!(g >= 0.f && g < 1.0e6f)) return -1.f;
  const uint32_t e3 = uint32_t(g * 1000.f);
  uint32_t cur = g_spring_census.gap_max_e3.load(std::memory_order_relaxed);
  while (e3 > cur && !g_spring_census.gap_max_e3.compare_exchange_weak(cur, e3, std::memory_order_relaxed)) {}
  g_spring_census.gap_sum_e3.fetch_add(e3, std::memory_order_relaxed);
  g_spring_census.gap_n.fetch_add(1, std::memory_order_relaxed);
  if (g > kSpringGapBig) g_spring_census.gap_big.fetch_add(1, std::memory_order_relaxed);
  return g;
}
inline std::string SpringHairCensusText() {
  const auto u = [](const std::atomic<uint64_t>& a) { return std::to_string((unsigned long long)a.load(std::memory_order_relaxed)); };
  // S103 HGF: the gap fields are THIS WINDOW (read and zeroed here); the other fields stay cumulative
  const double gmax = double(g_spring_census.gap_max_e3.exchange(0)) / 1000.0;
  const uint64_t gsum = g_spring_census.gap_sum_e3.exchange(0), gn = g_spring_census.gap_n.exchange(0), gbig = g_spring_census.gap_big.exchange(0);
  return "[scene][wrestlers] hair rows " + u(g_spring_census.rows) + ", bodies with extras drawn " + u(g_spring_census.drawn) + " / refused " +
         std::to_string((unsigned long long)(g_spring_census.refused_tags.load() + g_spring_census.refused_count.load())) + " (tags " + u(g_spring_census.refused_tags) +
         " / count " + u(g_spring_census.refused_count) + " / unreadable pieces " + u(g_spring_census.refused_unreadable) + "), hair_piece_skipped " + u(g_spring_census.piece_skipped) + " (meshes " + u(g_spring_census.mesh_skipped) +
         "), hair_rows_unmodelled " + u(g_spring_census.rows_unmodelled) + ", hair_rows_unposed " + u(g_spring_census.rows_unposed) + ", hair_rows_frame_skew " + u(g_spring_census.rows_frame_skew) + ", pieces 28/29 drawn " + u(g_spring_census.ent_drawn) + " hidden " + u(g_spring_census.ent_hidden) +
         ", hair root gap |b x row_j - b x row_parent| (file units, this window) max " + std::to_string(gmax) + " mean " + std::to_string(gn ? double(gsum) / 1000.0 / double(gn) : 0.0) + " (n " + std::to_string((unsigned long long)gn) + ", gap_big " + std::to_string((unsigned long long)gbig) + ")";
}

}  // namespace rdr_scene
