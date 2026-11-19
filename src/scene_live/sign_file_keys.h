// S103 CSF (docs\agents\s103\CS.md; docs\systems\crowd.md "Sign pictures"): which crowd-prop picture keys have a FILE picture in the player's own game files, so the latch reader never reads them back from the guest texture
// every frame (360 habit H-CS1, docs\renderer\360_HABITS.md; the coordinator's rule, 2026-11-19: "a key that has a file picture is always served from the file; guest reads only for keys >= 65000 and keys missing from the file").
//   signs: pac\audience.pac PACH 0 entry 0 (525 pictures, all 64x64 DXT1, keys 10001-52xxx and the 16 generic 60001 ... 60151; scanned 2026-11-19), keyed by the slot's file key (the row's file_key).
//   hands: the arena's audience set entries 42000 + h - 1 (S103 FHB), keyed by the hand's mesh key + 1000 (model_key 41000..41018).
// The two key spaces overlap numerically (sign ids reach 42xxx), so each has its own set. The live crowd (live_crowd.cpp) publishes a set once its loader has read the file (with the FNV of each picture's blocks); a key
// the file does not hold, a key of kSignFileKeyLimit or more (the game's runtime D3DX texture, branch 3 of sub_825E59F0) or a frame before the set is published keeps the guest read.
// One-time check (review 2): the FIRST time a key would be file-served the reader still reads the guest picture once and compares its FNV with the file's; a mismatch keeps that key on guest reads for good (counted).
// Lock-free fast path: one bit per key and plane (published / verified / bad), set by the loader's or the reader's thread. The FNV table is under a mutex, touched once per key.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace rdr_scene::live::crowd {

inline constexpr uint32_t kSignFileKeyLimit = 65000;   // sub_825E59F0: a key below 65000 is a file picture; 65000 and above is a runtime texture (index key - 65000)

// The ONE switch of the sign / foam-hand file pictures (stream-free default ON; the cvar wwe2k14_rdr_sign_file_pictures sets it once a guest frame, live.cpp): the reader and the applier both follow it.
// false = the pre-CSF rule (every sign / hand picture is read back from the guest, and the applier never serves a sign from the file).
inline std::atomic<bool> g_sign_file_pictures{true};
#ifdef RDR_CROWD_SNAP_PLANTS
inline int g_sign_file_plant = 0;   // test only: 1 = the reader reads the guest picture although the file holds the key (the pre-CSF rule); 2 = the file key is taken from the key table by the slot's own index (the pre-review rule: stale after the phase-4 slot re-map); 3 = the one-time FNV check passes every key
#endif

inline uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

struct SignFileKeys {
  static constexpr uint32_t kWords = (kSignFileKeyLimit + 63u) / 64u;
  enum Plane : uint32_t { kPublished = 0, kVerified = 1, kBad = 2 };
  static std::atomic<uint64_t>* Bits(uint32_t set, uint32_t plane) {   // set 0 = the signs' keys, 1 = the hands' keys
    static std::atomic<uint64_t> bits[2][3][kWords];
    return bits[set & 1u][plane];
  }
  static bool Bit(uint32_t key, uint32_t set, uint32_t plane) {
    if (key == 0 || key >= kSignFileKeyLimit) return false;
    return (Bits(set, plane)[key >> 6].load(std::memory_order_relaxed) >> (key & 63u)) & 1u;
  }
  static void SetBit(uint32_t key, uint32_t set, uint32_t plane) { Bits(set, plane)[key >> 6].fetch_or(uint64_t(1) << (key & 63u), std::memory_order_relaxed); }
  struct Table {
    std::mutex mu;
    std::unordered_map<uint64_t, uint64_t> fnv;   // (set << 32 | key) -> FNV-1a of the file picture's blocks
  };
  static Table& T() {
    static Table t;
    return t;
  }
  static std::atomic<uint64_t>& Verifications() { static std::atomic<uint64_t> n{0}; return n; }   // keys whose guest picture matched the file's
  static std::atomic<uint64_t>& Mismatches() { static std::atomic<uint64_t> n{0}; return n; }       // keys whose guest picture did NOT: kept on guest reads
  static void Publish(uint32_t key, uint32_t set = 0, uint64_t fnv = 0) {
    if (key == 0 || key >= kSignFileKeyLimit) return;
    {
      std::lock_guard<std::mutex> lock(T().mu);
      T().fnv[(uint64_t(set & 1u) << 32) | key] = fnv;
    }
    SetBit(key, set, kPublished);
  }
  // The file holds this key and the one-time check has not failed.
  static bool Has(uint32_t key, uint32_t set = 0) { return Bit(key, set, kPublished) && !Bit(key, set, kBad); }
  static bool Verified(uint32_t key, uint32_t set = 0) { return Has(key, set) && Bit(key, set, kVerified); }
  // The one-time check: the guest picture's blocks `n` bytes at `p` against the file's FNV. True = verified (served from the file from now on); false = mismatch (this key stays on guest reads).
  static bool Check(uint32_t key, uint32_t set, const uint8_t* p, size_t n) {
    uint64_t want = 0;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(T().mu);
      const auto it = T().fnv.find((uint64_t(set & 1u) << 32) | key);
      if (it != T().fnv.end()) want = it->second, found = true;
    }
    if (!found) return false;
#ifdef RDR_CROWD_SNAP_PLANTS
    if (g_sign_file_plant == 3) want = Fnv1a(p, n);   // planted: no real check (every key passes)
#endif
    if (Fnv1a(p, n) == want) return SetBit(key, set, kVerified), Verifications().fetch_add(1, std::memory_order_relaxed), true;
    SetBit(key, set, kBad), Mismatches().fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  static void Clear() {
    for (uint32_t s = 0; s < 2; ++s)
      for (uint32_t pl = 0; pl < 3; ++pl)
        for (uint32_t i = 0; i < kWords; ++i) Bits(s, pl)[i].store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(T().mu);
    T().fnv.clear();
  }
};

inline bool FilePicturesOn() {
  if (!g_sign_file_pictures.load(std::memory_order_relaxed)) return false;
#ifdef RDR_CROWD_SNAP_PLANTS
  if (g_sign_file_plant == 1) return false;
#endif
  return true;
}
// The file key / set a sign (site 0 / 1: its file_key, set 0) or a foam hand (site 2 / 3: model key 41000..41018 + 1000, set 1) would be served under; false = no file picture candidate (the switch is off, the key is
// not in the file set, or the one-time check failed for it).
inline bool FileCandidate(uint32_t site, uint32_t file_key, uint32_t model_key, uint32_t& key, uint32_t& set) {
  if (!FilePicturesOn()) return false;
  if (site < 2) key = file_key, set = 0;
  else if (model_key >= 41000 && model_key <= 41018) key = model_key + 1000u, set = 1;
  else return false;
  return SignFileKeys::Has(key, set);
}

}  // namespace rdr_scene::live::crowd
