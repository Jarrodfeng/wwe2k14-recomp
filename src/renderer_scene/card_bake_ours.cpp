// The crowd card bake, owned: the pose from the scene record (S102 lane P1; docs\renderer\CARD_BAKE_OURS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "card_bake_ours.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace rdr_scene {

bool CardBakeFaults::Parse(const std::string& list, CardBakeFaults& out, std::string& error) {
  out = CardBakeFaults{};
  size_t at = 0;
  while (at < list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string name = list.substr(at, comma - at);
    at = comma + 1;
    if (name.empty()) continue;
    if (name == "wrong_slot") out.wrong_slot = true;
    else if (name == "wrong_pose") out.wrong_pose = true;
    else if (name == "no_pose") out.no_pose = true;
    else if (name == "stale") out.stale = true;
    else {
      error = "unknown bake fault '" + name + "' (wrong_slot, wrong_pose, no_pose, stale)";
      return false;
    }
  }
  return true;
}

void CardBakeOurs::SetRecord(std::shared_ptr<const SceneTables> record) {
  prev_record_ = std::move(record_), prev_palette_ = palette_;
  record_ = std::move(record);
  palette_ = {};
  if (!record_) return;
  // The type's palette: the bones range of any "crowd:*:TT:..." object that has one (every LOD group carries the same array: bake_pose_check "layers").
  for (const Object& o : record_->objects) {
    if (!o.bones_count) continue;
    const std::string_view id = record_->Str(o.id);
    if (id.substr(0, 6) != "crowd:") continue;
    const size_t a = id.find(':', 6);
    if (a == std::string_view::npos) continue;
    const size_t b = id.find(':', a + 1);
    const std::string t(id.substr(a + 1, b == std::string_view::npos ? std::string_view::npos : b - a - 1));
    char* end = nullptr;
    const long type = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || type < 0 || type >= long(kTypes)) continue;
    Palette& p = palette_[size_t(type)];
    if (p.have || o.bones_count > record_->bones.size() || o.bones_first > record_->bones.size() - o.bones_count) continue;
    p.first = o.bones_first, p.count = o.bones_count, p.have = true;
  }
}

bool CardBakeOurs::FrameReady(const rdr_adapter::LoadedFrame&, uint32_t) {
  ++stats_.frames;
  prev_dest_ = 0;
  bool any = false;
  for (const Palette& p : palette_) any = any || p.have;
  if (!record_ || !any) {
    ++stats_.frames_no_record;
    return false;
  }
  return true;
}

namespace {
// The bank's bone b as the record's Mat4 words (row-major, translation in [12..14]): register 76 + 3b + r holds COLUMN r of the matrix.
void BankBone(const rdr_adapter::ConstantBank& bank, uint32_t b, uint32_t m[16]) {
  for (uint32_t r = 0; r < 3; ++r) {
    const uint32_t* reg = &bank.dwords[4 * (rdr_adapter::kBakeBoneReg + 3 * b + r)];
    for (uint32_t row = 0; row < 4; ++row) m[row * 4 + r] = reg[row];
  }
  m[3] = 0, m[7] = 0, m[11] = 0, m[15] = 0x3F800000u;   // the fourth column is not in the registers (0, 0, 0, 1)
}
bool SameWords(const uint32_t* a, const uint32_t* b) {   // the three register columns only (the fourth is implied)
  for (uint32_t row = 0; row < 4; ++row)
    for (uint32_t col = 0; col < 3; ++col)
      if (a[row * 4 + col] != b[row * 4 + col]) return false;
  return true;
}
}  // namespace

uint32_t CardBakeOurs::Pose(const rdr_adapter::BakePassIn& in, const rdr_adapter::ConstantBank& stream, rdr_adapter::ConstantBank& out) {
  ++stats_.passes;
  out = stream;
  const bool use_prev = faults_.stale && stats_.frames > 30 && prev_record_;
  const SceneTables* rec = use_prev ? prev_record_.get() : record_.get();
  const std::array<Palette, kTypes>& palette = use_prev ? prev_palette_ : palette_;
  if (faults_.no_pose || !rec) return 0;
  // Which type is this pass? Pass k is type k; the 15-type arenas' null slots are not known here, so a pass whose bones match nothing of type k takes the type whose palette they match (learned from the stream).
  uint32_t masks[rdr_adapter::kBakeMaxBones];
  auto mask_of = [&](uint32_t type, uint32_t b, uint32_t words[16]) {
    const Palette& p = palette[type];
    uint32_t mask = 0;
    for (uint32_t s = 0; s < p.count && s < kSlots; ++s) {
      const float* pm = rec->bones[p.first + s].m;
      uint32_t pw[16];
      std::memcpy(pw, pm, sizeof pw);
      if (SameWords(words, pw)) mask |= 1u << s;
    }
    return mask;
  };
  uint32_t words[rdr_adapter::kBakeMaxBones][16];
  for (uint32_t b = 0; b < rdr_adapter::kBakeMaxBones; ++b) BankBone(stream, b, words[b]);
  int type = -1;
  const auto known = pass_type_.find(in.copy_dest);
  if (known != pass_type_.end()) {
    type = known->second;   // learned on first sight: from then on the pass's type does not depend on the stream
  } else {
    uint32_t best = 0;
    for (uint32_t t = 0; t < kTypes; ++t) {
      const uint32_t cand = (in.pass + t) % kTypes;   // try the pass's own number first
      if (!palette[cand].have) continue;
      uint32_t hits = 0;
      for (uint32_t b = 0; b < rdr_adapter::kBakeMaxBones; ++b) hits += mask_of(cand, b, words[b]) != 0;
      if (hits > best) best = hits, type = int(cand);
      if (t == 0 && hits >= 3) break;
    }
    if (type >= 0 && best >= 3) pass_type_[in.copy_dest] = type;
  }
  if (type < 0) {
    ++stats_.passes_no_type;
    return 0;
  }
  const Palette& pal = palette[size_t(type)];
  uint32_t written = 0;
  for (uint32_t b = 0; b < rdr_adapter::kBakeMaxBones; ++b) {
    Bone& bone = bone_[size_t(type)][b];
    masks[b] = mask_of(uint32_t(type), b, words[b]);
    const bool resolved = (bone.cand & (bone.cand - 1)) == 0 && bone.seen >= 2;
    if (!resolved) {   // learn: the slots this bone's matrix equals, intersected over frames (bake_pose_check's rule); a bone that matches nothing is unused or not posed from the record
      if (masks[b]) {
        const uint32_t next = bone.cand & masks[b];
        if (!next) ++stats_.perm_conflicts, bone.cand = masks[b], bone.seen = 1;
        else bone.cand = next, ++bone.seen;
      } else {
        ++stats_.bones_unused;
      }
      if (!((bone.cand & (bone.cand - 1)) == 0 && bone.seen >= 2)) {
        ++stats_.bones_unresolved;
        continue;
      }
    }
    uint32_t slot = 0;
    while (!((bone.cand >> slot) & 1u)) ++slot;
    if (faults_.wrong_pose) slot = (slot + 1) % pal.count;
    if (slot >= pal.count) continue;
    uint32_t src[16];
    std::memcpy(src, rec->bones[pal.first + slot].m, sizeof src);
    const bool equal = SameWords(words[b], src);
    ++stats_.bones_written, (equal ? stats_.bones_equal : stats_.bones_differ) += 1;
    for (uint32_t r = 0; r < 3; ++r) {
      uint32_t* reg = &out.dwords[4 * (rdr_adapter::kBakeBoneReg + 3 * b + r)];
      for (uint32_t row = 0; row < 4; ++row) reg[row] = src[row * 4 + r];
    }
    ++written;
  }
  return written;
}

uint32_t CardBakeOurs::CopyDest(const rdr_adapter::BakePassIn& in) {
  const uint32_t dest = (faults_.wrong_slot && in.pass > 0 && prev_dest_) ? prev_dest_ : in.copy_dest;
  prev_dest_ = in.copy_dest;
  return dest;
}

std::string CardBakeOurs::Census() const {
  char line[400];
  std::snprintf(line, sizeof line, "pose from the record: frames %llu (no record %llu), passes %llu (no type %llu), bones written %llu (equal to the stream's %llu, differ %llu), unresolved %llu, unused %llu, perm conflicts %llu",
                (unsigned long long)stats_.frames, (unsigned long long)stats_.frames_no_record, (unsigned long long)stats_.passes, (unsigned long long)stats_.passes_no_type, (unsigned long long)stats_.bones_written,
                (unsigned long long)stats_.bones_equal, (unsigned long long)stats_.bones_differ, (unsigned long long)stats_.bones_unresolved, (unsigned long long)stats_.bones_unused, (unsigned long long)stats_.perm_conflicts);
  return line;
}

}  // namespace rdr_scene
