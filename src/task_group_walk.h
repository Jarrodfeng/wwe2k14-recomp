// S103 FDP: the ONE walk of the game's task runner groups (docs\systems\task_runner.md). Pure header (only <cstdint>): scene_freeze.h (RunnerEnter, live guest memory) and the scene-input
// reader (scene_live\inputs_core.h ReadStagePageClock, fake guest in tools\scene\fd_clock_unit.cpp) both use it, so a second copy of the layout cannot exist.
// Layout [C: sub_82C2FEB8 (registration), sub_82C302A0 (runner), scene_freeze.h RunnerEnter]: owner = [0x83930050]; [owner+32] = POINTER to the group array, [owner+40] = its end; each group is a
// 16-byte entry: +0 begin of the task-pointer words, +4 capacity end, +8 cursor end (registration stores the task pointer at [+8] and adds 4); group g = [owner+32] + 16*g (35 groups).
// `rd(addr, v)` reads one big-endian guest word as host order, false when unreadable.
#pragma once
#include <cstdint>

namespace task_walk {

inline constexpr uint32_t kOwnerGroupsPtr = 32, kOwnerGroupsEnd = 40, kGroupStride = 16, kGroupMaxCount = 64, kGroupMaxBytes = 4096, kAll = 0xFFFFFFFFu;
inline constexpr uint32_t kGuestLo = 0x40000000u, kGuestHi = 0xE0000000u;   // the bounds every guest pointer here is checked against (-1 = an empty slot fails them)
inline bool PtrOk(uint32_t p) { return p >= kGuestLo && p < kGuestHi; }

struct GroupInfo {
  uint32_t base = 0;    // [owner+32]
  uint32_t count = 0;   // ([owner+40] - base) / 16
  uint32_t begin = 0, end = 0, tasks = 0;   // the asked group: +0, +8 and (end-begin)/4 (0 when no group was asked or it is out of range)
};

// The group array: false when [owner+32] / [owner+40] are unreadable, out of bounds or not a whole 0..kGroupMaxCount 16-byte entries.
template <class Rd>
inline bool ReadGroups(Rd&& rd, uint32_t owner, GroupInfo& gi) {
  uint32_t g0 = 0, g1 = 0;
  gi = GroupInfo();
  if (!rd(owner + kOwnerGroupsPtr, g0) || !PtrOk(g0) || !rd(owner + kOwnerGroupsEnd, g1) || g1 < g0 || (g1 - g0) % kGroupStride != 0 || (g1 - g0) / kGroupStride > kGroupMaxCount) return false;
  gi.base = g0;
  gi.count = (g1 - g0) / kGroupStride;
  return true;
}

// One group's task words: false when the array or the group entry is unusable, or the group is empty / over kGroupMaxBytes (`gi` still holds what was read).
template <class Rd>
inline bool ReadGroup(Rd&& rd, uint32_t owner, uint32_t group, GroupInfo& gi) {
  if (!ReadGroups(rd, owner, gi) || group >= gi.count) return false;
  const uint32_t g = gi.base + kGroupStride * group;
  uint32_t b = 0, e = 0;
  if (!rd(g, b) || !rd(g + 8, e) || e < b) return false;
  gi.begin = b, gi.end = e, gi.tasks = (e - b) / 4;
  return e != b && e - b <= kGroupMaxBytes;
}

// fn(task, slot_addr) for every task pointer of `group` (kAll = every group) that is within the guest bounds; fn returns true to stop. Returns false when the array is unusable.
template <class Rd, class Fn>
inline bool ForEachGroupTask(Rd&& rd, uint32_t owner, uint32_t group, Fn&& fn) {
  GroupInfo gi;
  if (!ReadGroups(rd, owner, gi)) return false;
  for (uint32_t gx = 0; gx < gi.count; ++gx) {
    if (group != kAll && gx != group) continue;
    const uint32_t g = gi.base + kGroupStride * gx;
    uint32_t b = 0, e = 0;
    if (!rd(g, b) || !rd(g + 8, e) || e < b || e - b > kGroupMaxBytes) continue;
    for (uint32_t p = b; p < e; p += 4) {
      uint32_t t = 0;
      if (!rd(p, t) || !PtrOk(t)) continue;
      if (fn(t, p)) return true;
    }
  }
  return true;
}

}  // namespace task_walk
