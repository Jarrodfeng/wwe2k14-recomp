// The circle blob shadows, the game-side half (Session 101, lane 136 = SH3 slice 1; docs/renderer/SHADOW_CIRCLES.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only core: no game or SDK includes. The game plumbing (the guest reads, the register arguments, the call from the op-21 recorder hook) is circle_glue.h;
// tools/shadow/circles_unit.cpp drives this same code with a fake guest memory.
//
// Where the values are built. sub_82812E90 (T_CircleShadow, string 0x82035954; listing 0x82812E90-0x82813224) loops over the 9 character slots at 0x836F38D0 and, for each character with a
// model and a component, uploads three constant runs through sub_82BB27D8 (the op-21 recorder, which the crowd already hooks and which is game-wide: the return address is tested first):
//   bl 0x82BB27D8 at 0x82812F80  (LR 0x82812F84): r4 = 4,  r6 = 4,  r5 = r1+256   c4-c7  ViewProj (the matrix [r31], transposed), once per call of sub_82812E90
//   bl 0x82BB27D8 at 0x82813190  (LR 0x82813194): r4 = 0,  r6 = 1,  r5 = r1+80    c0     ShadowParam = ([circle+16], ground y from sub_82812600(char), [circle+20], [circle+24])
//   bl 0x82BB27D8 at 0x828131A4  (LR 0x828131A8): r4 = 8,  r6 = 21, r5 = r1+320   c8-c28 WorldPosArray: the translation row (+48, 16 bytes) of up to 21 of the character's skeleton world
//                                                  matrices [[char+48]+192][index], the indices (7 steps x 3, int16) from the character's component (tag 0x434D4D44, sub_823A6450)
// At the third call r30 = the slot's address (so [r30] = the character), r29 = the circle object (this) whose [+36] / [+40] / [+44] are the disc mesh's vertex pointer, index pointer and
// index count (the arguments of the next two ops, sub_82BB0F30 / sub_82BB1000 / sub_82BB10C0, lines 0x828131B0-0x828131E0). Read only; never writes guest memory or a register.
//
// Frames. Every entry is stamped with the guest frame the hook ran in; the join to the stream's frame is by that stamp. Threads: the hook runs on the thread that records the draws,
// Emit (circle_contrib.h) on the writer's thread; one short mutex guards the pending list (a handful of entries a frame).
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

#include "../renderer_scene/circle_shadow_core.h"
#include "../renderer_scene/scene_record.h"

namespace rdr_scene::live::circles {

inline constexpr uint32_t kViewProjReturn = 0x82812F84, kParamReturn = 0x82813194, kPointsReturn = 0x828131A8;
inline constexpr uint32_t kSlotTable = 0x836F38D0, kSlots = 9;
inline constexpr uint32_t kMeshVertexPtr = 36, kMeshIndexPtr = 40, kMeshIndexCount = 44;   // circle object fields (listing 0x828131B0, 0x828131C4, 0x828131CC)
inline constexpr uint32_t kMaxPending = 64;     // entries held between two Emits (4 a frame in the captures)

struct Stats {
  uint64_t calls = 0, viewproj = 0, params = 0, entries = 0, orphans = 0, unreadable = 0, overflow = 0, bad_slot = 0;
  uint64_t mesh_reads = 0, mesh_ok = 0, mesh_bad = 0;
};

// The guest reads the tracker needs: n bytes at an address (big-endian bytes, as in memory). False when unreadable.
using ReadBytes = std::function<bool(uint32_t addr, void* dst, size_t n)>;

inline bool ReadWordBE(const ReadBytes& rd, uint32_t addr, uint32_t& v) {
  uint8_t b[4];
  if (!rd(addr, b, 4)) return false;
  v = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
  return true;
}
inline bool ReadFloatsBE(const ReadBytes& rd, uint32_t addr, float* dst, size_t count) {
  uint8_t buf[16 * kCirclePoints];
  if (count * 4 > sizeof buf || !rd(addr, buf, count * 4)) return false;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t v = (uint32_t(buf[4 * i]) << 24) | (uint32_t(buf[4 * i + 1]) << 16) | (uint32_t(buf[4 * i + 2]) << 8) | buf[4 * i + 3];
    std::memcpy(&dst[i], &v, 4);
  }
  return true;
}

// Tests only: planted faults (each must fail its named check; one branch each in the shipped code).
enum class Plant : uint32_t { None = 0, MeshHeaderAsData, StaleViewProj, IdWordKept };   // IdWordKept: lane 205, the old mesh rule (the u8x4 id word left in memory order)

class Tracker {
 public:
  void SetPlant(Plant p) { plant_ = p; }
  // Is this return address one of ours (cheap: three compares; the recorder is game-wide)?
  static bool IsOurs(uint32_t lr) { return lr == kViewProjReturn || lr == kParamReturn || lr == kPointsReturn; }

  // One call of sub_82BB27D8 whose return address is ours. r29 / r30 are the caller's registers at the call (see the header comment).
  void OnConstants(uint32_t lr, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r29, uint32_t r30, uint32_t frame, const ReadBytes& rd) {
    Bump(&Stats::calls);
    if (lr == kViewProjReturn) {
      if (r4 != 4 || r6 != 4) return Bump(&Stats::orphans);
      if (plant_ != Plant::StaleViewProj) {   // F7: a ViewProj that cannot be read must not leave the previous call's matrix behind
        cur_.flags &= ~uint32_t(kCircleHaveViewProj);
        std::memset(cur_.view_proj, 0, sizeof cur_.view_proj);
      }
      if (!ReadFloatsBE(rd, r5, cur_.view_proj, 16)) return Bump(&Stats::unreadable);
      cur_.flags |= kCircleHaveViewProj;
      Bump(&Stats::viewproj);
    } else if (lr == kParamReturn) {
      if (r4 != 0 || r6 != 1) return Bump(&Stats::orphans);
      if (!ReadFloatsBE(rd, r5, param_, 4)) return Bump(&Stats::unreadable);
      have_param_ = true;
      Bump(&Stats::params);
    } else if (lr == kPointsReturn) {
      if (r4 != 8 || r6 != kCirclePoints) return Bump(&Stats::orphans);
      if (!have_param_) return Bump(&Stats::orphans);   // the points without their parameters: not the pattern, nothing recorded
      CircleShadow e = cur_;
      e.frame = frame;
      if (!ReadFloatsBE(rd, r5, &e.points[0][0], 4 * kCirclePoints)) return Bump(&Stats::unreadable);
      e.flags |= kCircleHaveParam;
      std::memcpy(e.shadow_param, param_, sizeof param_);
      uint32_t slot = (r30 - kSlotTable) / 4, ch = 0;
      if (r30 < kSlotTable || (r30 - kSlotTable) % 4 || slot >= kSlots) Bump(&Stats::bad_slot), slot = 0xFFFFFFFFu;
      else if (!ReadWordBE(rd, r30, ch)) ch = 0;
      e.slot = slot, e.character = ch;
      have_param_ = false;
      MaybeReadMesh(r29, rd);
      std::lock_guard<std::mutex> lock(mu_);
      if (pending_.size() >= kMaxPending) {
        ++stats_.overflow;
        return;
      }
      pending_.push_back(e);
      ++stats_.entries;
    }
  }

  // The entries gathered since the last call (oldest first), and the mesh when it was read since (or always, once known, when `always_mesh`).
  void Take(std::vector<CircleShadow>& out, CircleMesh* mesh, bool& mesh_present, bool always_mesh) {
    std::lock_guard<std::mutex> lock(mu_);
    out.swap(pending_);
    pending_.clear();
    mesh_present = false;
    if (mesh && have_mesh_ && (mesh_fresh_ || always_mesh)) *mesh = mesh_, mesh_present = true;
    mesh_fresh_ = false;
  }
  Stats GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
  }
  bool has_mesh() const {
    std::lock_guard<std::mutex> lock(mu_);
    return have_mesh_;
  }

 private:
  // The disc mesh (constant for the run): read the first time, again when the object's pointers change, and again while it has not read cleanly (4 tries at once, then one try every
  // kMeshRetryCalls constant uploads). F1 (the lane 136 review, from the listings): [circle+36] and [circle+40] are D3D RESOURCE OBJECTS, not data. [circle+36] is op 14's r5 (SetStreamSource):
  // the playback sub_82CAB3C8 reads the fetch word at +24 (the address, the type in the low two bits; 0x82CAB3EC) and the size word at +28 (0x82CAB3F4). [circle+40] is op 12's index buffer
  // object: sub_82CB19E0 reads the data address at +24 (0x82CB1CAC) and, from the word at +0, the top bit (0x82CB1CEC rlwinm r6,r6,0,0,0; r6 != 0 takes the 32-bit branch 0x82CB1D08-0x82CB1D24:
  // the index scaled by 4, format bit 2048): 32-bit indices are refused. [circle+44] is the plain index count.
  void MaybeReadMesh(uint32_t obj, const ReadBytes& rd) {
    uint32_t vobj = 0, iobj = 0, ic = 0;
    if (!obj || !ReadWordBE(rd, obj + kMeshVertexPtr, vobj) || !ReadWordBE(rd, obj + kMeshIndexPtr, iobj) || !ReadWordBE(rd, obj + kMeshIndexCount, ic)) return;
    if (have_mesh_ && vobj == mesh_vp_ && iobj == mesh_ip_ && ic == mesh_ic_) return;
    if (!have_mesh_ && mesh_tries_ >= kMeshTries && vobj == mesh_vp_ && iobj == mesh_ip_) {
      if (++mesh_skipped_ < kMeshRetryCalls) return;
      mesh_skipped_ = 0;
    }
    mesh_vp_ = vobj, mesh_ip_ = iobj, mesh_ic_ = ic;
    ++mesh_tries_;
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.mesh_reads;
    if (!vobj || !iobj || !ic || ic > kCircleMeshIndices || ic % 3) { ++stats_.mesh_bad; return; }
    uint32_t vdata = vobj, vsize = kCircleMeshVertices * kCircleVertexStride, idata = iobj, iflags = 0;
    if (plant_ != Plant::MeshHeaderAsData) {   // the shipped path: the objects' fields (the plant reads the header as the data, today's wrong code)
      uint32_t fetch = 0, size = 0;
      if (!ReadWordBE(rd, vobj + 24, fetch) || !ReadWordBE(rd, vobj + 28, size) || !ReadWordBE(rd, iobj + 24, idata) || !ReadWordBE(rd, iobj + 0, iflags)) { ++stats_.mesh_bad; return; }
      vdata = fetch & ~3u, vsize = size & ~3u;
      if (iflags & 0x80000000u) { ++stats_.mesh_bad; return; }   // 32-bit indices: not the mesh we know
      if (!vdata || !idata || (idata & 1) || !vsize) { ++stats_.mesh_bad; return; }
    }
    uint8_t ib[2 * kCircleMeshIndices];
    if (!rd(idata, ib, size_t(ic) * 2)) { ++stats_.mesh_bad; return; }
    uint32_t max_index = 0;
    for (uint32_t i = 0; i < ic; ++i) {
      const uint32_t v = (uint32_t(ib[2 * i]) << 8) | ib[2 * i + 1];
      if (v > max_index) max_index = v;
    }
    const uint32_t vcount = max_index + 1;
    uint8_t vb[kCircleMeshVertices * kCircleVertexStride];
    if (vcount > kCircleMeshVertices || size_t(vcount) * kCircleVertexStride > vsize || !rd(vdata, vb, size_t(vcount) * kCircleVertexStride)) { ++stats_.mesh_bad; return; }
    CircleMesh m;
    if (!rdr_scene::circles::MeshFromGuest(vb, vcount, ib, ic, m, plant_ == Plant::IdWordKept)) { ++stats_.mesh_bad; return; }
    mesh_ = m, have_mesh_ = true, mesh_fresh_ = true;
    ++stats_.mesh_ok;
  }

  void Bump(uint64_t Stats::*field) {
    std::lock_guard<std::mutex> lock(mu_);
    ++(stats_.*field);
  }
  static constexpr uint32_t kMeshTries = 4, kMeshRetryCalls = 600;
  mutable std::mutex mu_;
  std::vector<CircleShadow> pending_;
  CircleShadow cur_;            // the call's ViewProj (kept across the call's characters)
  float param_[4] = {};         // the call's ShadowParam, waiting for its points
  bool have_param_ = false;     // hook thread only (one drawing thread; see the header)
  CircleMesh mesh_;
  bool have_mesh_ = false, mesh_fresh_ = false;
  uint32_t mesh_vp_ = 0, mesh_ip_ = 0, mesh_ic_ = 0, mesh_tries_ = 0, mesh_skipped_ = 0;
  Plant plant_ = Plant::None;
  Stats stats_;
};

}  // namespace rdr_scene::live::circles
