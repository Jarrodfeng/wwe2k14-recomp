// Props of other pacs, the part that needs no GPU: the join of a stream draw to a file mesh of the commentary table / the ring announcer's microphone / the ring steps, and the per-draw checks
// (S101 lane 84 = PO; docs\renderer\UNCLAIMED_MODEL_PARTS.md section 3.2; as built: docs\renderer\PROPS_OURS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// A copy of RingCore's design on a different file side (assets::prop_gs::PropSet: sources instead of ring entries; no arena: the set is loaded once). PropsOurs (props_ours.h) is the
// DrawReplaceHook; everything that can be wrong about WHICH file mesh a stream draw is lives here, so tools\props\props_ours_test.cpp runs the same code on the captures without a GPU.
// No game or SDK includes.
//
// The join (once per object, at the colour site 0x82C0A950): the candidates are the sources whose mesh `part` has the draw's vertex and index counts for EVERY part the object draws in this
// frame and whose element-0 positions equal the stream's bit for bit (the microphone's counts also fit PACH 301: only the positions refuse it). A tie (the 17 table variants, the 10 step
// leaves, the mics 4 / 135) is broken by the slot-0 image content over every gathered part (the candidate whose textures the stream holds on the most parts); without a content feed the first
// candidate's geometry is ours (the byte compare below decides) and every slot stays the stream's. Every draw then builds OUR vertex bytes in the stream's layout and compares them with the
// stream's bytes (every draw in fview, the first use of each blob key live). Slots 0 / 4 / 5 are ours only where the content check passed; the mip count is the stream record's
// (min(record mips, max_level + 1) rule: never more than the stream image holds).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/prop_game_space.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../renderer_adapter/image_content_table.h"
#include "record_io.h"
#include "ring_ours_core.h"   // RingPrepared (the same prepared buffer)

namespace rdr_scene {

// Planted faults (each must fail its exact check).
struct PropsFaults {
  bool colour_order = false, uv_halves_swapped = false, uv_sign = false, bone_zero = false, weight_word = false;
  bool counts_only_join = false;     // R: the join by counts alone (no position check), the last candidate taken: the microphone becomes PACH 301
  bool wrong_table_variant = false;  // the table joined to the last variant and its texture forced into the slots (no content check)
  bool steps_position_swap = false;  // the steps' file positions written with x and z exchanged
  bool lookalike = false;            // the microphone's first vertex byte written wrong: an object that joins by positions but whose bytes differ (WM30 run 7084): the join must be given up, not retried every draw
  bool scene_lookalike_p1 = false, scene_lookalike_both = false;   // lane 137: the cage twin PACH 1 (and PACH 2) written with a wrong vertex byte: re-join to the twin / give up
  bool zero_cleared = false;         // lane 220 follow-up PLANT: the stream's u half at the first sign-bit vertex has its sign bit cleared (a -0.0 vertex whose bit a game path cleared)
  bool teach_abs = false;            // lane 220 follow-up PLANT (the past bug): any clear bit teaches |u|, as before the strict-vertex rule
  bool neg_zero_u = false;           // lane 220 PLANT (the real re-find of run 7133): a file u of -0.0 does not count as negative (the loaders' old `< 0` test): the u-sign rule is not consulted for such a mesh and it is written |u|
  bool scene_off = false;            // lane 137: no scene sources joined at all (the state before the promotion): the real re-find
  bool scene_desk_105 = false;      // lane 137: the desk joined to bgEtc p9/105 with its picture forced into slot 0 (the file picture differs from the stream's)
  bool scene_wrong = false;         // lane 137: a scene prop (cage, desk, monitors) joined to the wrong source (the first step leaf): the byte / count checks must refuse it
  bool all_mips = false;             // our image carries the file's whole mip chain, not the stream image's count
  bool no_verify = false;            // the byte compare of our buffers against the stream's is skipped (a writer fault then reaches the picture)
  bool special_ids = false;          // lane 254 PLANT: the first draft: people are required only for arenas 0 and 23 (an editor-like record of any other arena then loads)
  bool editor_as_match = false;      // lane 254 PLANT: the pre-254 rule: any record that names an arena is "a match" (the Create-a-Superstar editor then loads the whole props set)
  bool frozen_clock = false;         // lane 115 F2: NoClaimFrame does not tick the core's clock (the 300-frame foreign verdict then never expires on frames where nothing is claimed)
  bool Any() const { return special_ids || editor_as_match || frozen_clock || colour_order || uv_halves_swapped || uv_sign || bone_zero || weight_word || counts_only_join || wrong_table_variant || steps_position_swap || lookalike || all_mips || no_verify || scene_wrong || scene_lookalike_p1 || scene_lookalike_both || scene_desk_105 || scene_off || neg_zero_u || zero_cleared || teach_abs; }
  assets::ring_gs::WriteFaults Writer() const {
    assets::ring_gs::WriteFaults w;
    w.colour_order = colour_order, w.uv_halves_swapped = uv_halves_swapped, w.uv_sign = uv_sign, w.bone_zero = bone_zero, w.weight_word = weight_word;
    return w;
  }
  static bool Parse(const std::string& list, PropsFaults& f, std::string& error);
};

struct PropsOursOptions {
  std::string game_root;
  PropsFaults faults;
  bool verify_all = false;   // fview / the unit test: compare the vertex and index bytes on EVERY draw; live: the first use of each blob key
  std::function<bool(uint32_t record_id, uint32_t& w, uint32_t& h)> image_info;
  std::function<bool(uint32_t record_id, rdr_adapter::ImageContent& out)> image_content;   // the content check for slots 0 / 4 / 5 and the ties; none = geometry only
};

using PropsPrepared = RingPrepared;

class PropsCore {
 public:
  explicit PropsCore(const PropsOursOptions& options) : options_(options) {}

  void SetProps(std::shared_ptr<const assets::prop_gs::PropSet> props);   // null clears; resets every join
  const assets::prop_gs::PropSet* props() const { return props_.get(); }
  void SetReleaseHook(std::function<void(PropsPrepared&)> f) { release_ = std::move(f); }
  static int ArenaOfRecord(const SceneTables& record) { return RingCore::ArenaOfRecord(record); }   // the record names an arena (necessary for "in a match", not sufficient: InMatch)
  // Lane 254 (docs/renderer/PROPS_OURS.md "Lane 254"): which records are "a match" for the props. The arena alone is not enough: the Create-a-Superstar editor presents bg00 (also a real
  // match arena) with ONE actor and no crowd, its clothing pages bg23 with neither; the props hook loaded ~160 MB of evtobj for them and joined nothing. One rule for EVERY arena id:
  //   - no arena named: not a match;  - the menu / story backgrounds (db/ids/arenas.json: 66, 67, 68, 76-80, "not a match arena"): never a match (a fast path; the next rule alone also holds);
  //   - otherwise a match only with people: >= 2 "actor:" objects or any "crowd:" member (every match / entrance / victory / WM30 record has both).
  struct MatchEvidence { int arena = -1; int actors = 0; int crowd = 0; };
  static MatchEvidence EvidenceOf(const SceneTables& record) {
    MatchEvidence e;
    e.arena = ArenaOfRecord(record);
    if (e.arena < 0) return e;
    for (const Object& o : record.objects) {
      const std::string_view id = record.Str(o.id);
      if (id.compare(0, 6, "actor:") == 0) ++e.actors;
      else if (id.compare(0, 6, "crowd:") == 0) ++e.crowd;
    }
    return e;
  }
  static bool MenuOnlyArena(int arena) { return arena == 66 || arena == 67 || arena == 68 || (arena >= 76 && arena <= 80); }
  // old_rule_plant: the pre-254 rule (any named arena); special_ids_plant: the first lane-254 draft (people required only for arenas 0 and 23).
  static bool InMatch(const MatchEvidence& e, bool old_rule_plant = false, bool special_ids_plant = false) {
    if (e.arena < 0) return false;
    if (old_rule_plant) return true;
    if (MenuOnlyArena(e.arena)) return false;
    if (special_ids_plant && e.arena != 0 && e.arena != 23) return true;
    return e.actors >= 2 || e.crowd > 0;
  }
  static bool InMatch(const SceneTables& record, bool old_rule_plant = false, bool special_ids_plant = false) { return InMatch(EvidenceOf(record), old_rule_plant, special_ids_plant); }
  void ResetFrame() { ++frame_no_; }
  void NoClaimFrame() {   // lane 115 F2: a frame where nothing was claimed ticks the clock too (PropsOurs::NoClaimFrame calls this)
    if (!options_.faults.frozen_clock) ++frame_no_;
  }

  bool Claims(const rdr_adapter::DrawOrigin& o) const;

  struct Decision {
    bool ok = false;
    rdr_adapter::KeepWhy why = rdr_adapter::KeepWhy::NotReady;
    const char* reason = "";
    int mesh = -1;                    // index into props()->meshes
    PropsPrepared* prepared = nullptr;
    int diffuse = -1, normal_map = -1, specular = -1;
    uint32_t diffuse_mips = 0, normal_mips = 0, specular_mips = 0;
  };
  void UploadFailed(PropsPrepared* p) {
    if (p) p->failed = true;
    if (stats_.replaced) --stats_.replaced;
    ++stats_.kept[uint8_t(rdr_adapter::KeepWhy::Mismatch)];
  }
  Decision Decide(const rdr_adapter::ReplaceIn& in, const uint8_t* ps_slots, uint32_t ps_slot_count);

  struct Stats {
    uint64_t decisions = 0, replaced = 0;
    uint64_t kept[uint8_t(rdr_adapter::KeepWhy::Count)] = {};
    uint64_t joined_content = 0, joined_tie_texture = 0, joined_tie_open = 0;   // objects
    uint64_t foreign = 0, unjoined = 0;
    uint64_t bad_part = 0, bad_vertices = 0, bad_indices = 0, bad_bytes = 0, bad_view = 0;
    uint64_t layout_refused = 0, joins_dropped = 0;
    uint64_t slot_kept_tie = 0, slot_kept_no_file = 0, slot_kept_levels = 0;
    uint64_t content_checks = 0, content_ok = 0, content_differs = 0, content_unknown = 0;
    uint64_t verify_runs = 0, prepared_built = 0, lookalike = 0;   // lookalike: objects given up as NotOurs after failed byte compares
  };
  const Stats& stats() const { return stats_; }
  std::string TakeFirstFail() {
    std::string s;
    s.swap(first_fail_);
    return s;
  }
  size_t prepared_count() const { return prepared_.size(); }
  int uv_mode() const { return uv_mode_; }
  void ReleaseAll();
  std::string Report() const;
  // The source an object is joined to (-1 none), for the unit test.
  int SourceOfObject(uint32_t object) const {
    const auto it = joins_.find(object);
    return it == joins_.end() || it->second.foreign ? -1 : it->second.source;
  }

 private:
  struct ObjJoin {
    int source = -1;
    bool foreign = false, slot0_open = false, tie_texture = false, force_slot0 = false;
    uint64_t blocked_until = 0;
    bool blocked_unjoined = false;
    bool tied = false;   // the join chose among several candidates (a byte mismatch then excludes that source and the object re-joins at once)
  };
  struct VerKey {
    uint64_t vertex = 0, index = 0;
    int mesh = -1;
  };
  bool ContentMatches(uint32_t image_id, int texture, bool force, uint32_t& mips);
  uint64_t FileHash(int texture, uint32_t mips);
  std::string last_reason_;
  std::set<std::string> differ_notes_;
  int MeshIndex(int source, int part) const;
  const assets::ring_gs::RingMesh* MeshOf(int source, int part) const {
    const int i = MeshIndex(source, part);
    return i < 0 ? nullptr : &props_->meshes[size_t(i)];
  }
  bool JoinObject(const rdr_adapter::ReplaceIn& in, ObjJoin& out, const char*& reason);
  int uv_mode_ = 0;   // 0 unknown, 1 |u|, 2 signed: learned from the stream's own u half (the ring's rule)
  int StrictNegU(const assets::ring_gs::RingMesh& m) const;   // lane 220 follow-up: the first strictly negative u (the only vertex whose clear bit may teach |u|)
  int FirstNegU(const assets::ring_gs::RingMesh& m) const;   // lane 220: the mesh's first vertex whose file u has the sign bit set (the plant neg_zero_u restores the old `< 0` rule)
  bool DetectUvSign(const rdr_adapter::ReplaceIn& in, const rdr_adapter::VertexLayout& lay, const assets::ring_gs::RingMesh& m);
  bool IsForeign(uint32_t object) const {
    const auto it = foreign_.find(object);
    return it != foreign_.end() && frame_no_ < it->second;
  }

  PropsOursOptions options_;
  std::shared_ptr<const assets::prop_gs::PropSet> props_;
  std::unordered_map<uint32_t, int> mesh_by_key_;   // source << 16 | part -> mesh index
  std::unordered_map<uint32_t, ObjJoin> joins_;
  std::unordered_map<uint32_t, std::vector<int>> bad_sources_;   // object -> sources whose bytes did not equal the stream's (cleared with the foreign verdict's expiry)
  std::unordered_map<uint32_t, uint32_t> byte_fails_;            // object -> byte-compare failures since its last good join: a second one makes it foreign
  std::unordered_map<uint32_t, uint64_t> foreign_;   // object -> the frame its "not a prop of ours" verdict expires (addresses are reused)
  std::unordered_map<uint64_t, bool> content_ok_;
  std::unordered_map<uint64_t, uint64_t> file_hash_;
  std::unordered_map<uint64_t, std::unique_ptr<PropsPrepared>> prepared_;
  std::unordered_map<uint64_t, VerKey> verified_key_;
  std::function<void(PropsPrepared&)> release_;
  std::string first_fail_, detail_;   // detail_: where the last failed byte compare differed
  uint64_t frame_no_ = 0;
  uint32_t redecide_ = 0;   // S101 lane 220: nesting of the same-call re-decision after a tied candidate's bytes failed
  Stats stats_;
};

}  // namespace rdr_scene
