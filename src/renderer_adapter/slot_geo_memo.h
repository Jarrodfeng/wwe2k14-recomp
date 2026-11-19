// S102 lane T15 (R13 section 7: the reader's per-draw texture-slot capture and geometry build on the game's render-command thread).
//
// Four cuts, each behind its own PerFrame switch (default off; rdr_switches.h X-macro table, cvars in reader.h, read once a frame at the present
// with BoolSeen and published here as relaxed atomics, the origin_intern.h way). THIS HEADER HAS NO GAME OR SDK INCLUDES: the logic of every cut that
// can be pure is here so tools\t15\t15_memo_test.cpp drives the real code on synthetic and recorded inputs, switch off against switch on.
//
//   wwe2k14_rdr_slot_content_skip  AddTextureSlot's hit path called EnsureContent (a timer, an image lookup, the per-frame check) for every slot of
//                                  every draw although the image was already checked this record frame: the same early return, taken before the call.
//   wwe2k14_rdr_slot_ways          the per-slot record of the last add was ONE entry: draws that alternate two textures (or two shaders' fetch
//                                  facts) on a slot missed every time. 4 entries per slot, round-robin; the validity rule is the old one's, per entry.
//   wwe2k14_rdr_sampler_memo       BuildSampler + Dedupe (a Hash64 of the descriptor and a map find) for every slot MISS: the descriptor's inputs
//                                  (the four fetch words it reads, the shader's four overrides, the player's filtering, the jitter bias) -> the table
//                                  index within the frame tables' generation.
//   wwe2k14_rdr_geo_hash_once      a small stream's full hash was computed up to three times per changed draw (the memo hit check, the memo store,
//                                  GeoKey's content key): once, handed down.
//
// Tried and dropped (measured by tools	15	15_memo_test.cpp, microbench part): a ring that remembers inline-vertex builds' content keys. The key from scratch
// costs 11 ns at 96 bytes, 56 ns at 1 KB, 144 ns at 3 KB; the ring's compare costs 31 / 52 / 108 ns: a loss under ~1 KB, 25 % at 3 KB. Not a lever.
//
// None of them changes a byte the reader records, except that the sampler memo, like the Dedupe it stands in front of, shares a 64-bit Hash64 collision (the descriptor is still identical): the memos only answer a question the old code answered by recomputing it.
#pragma once
#include <atomic>
#include "census_flush.h"  // S102 CEN1
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace rdr_adapter::slot_geo_memo {

inline std::atomic<bool> g_content_skip_on{false}, g_slot_ways_on{false}, g_sampler_memo_on{false}, g_geo_hash_once_on{false};

// ---- the live census (game thread only: the reader's draw hooks all run under its lock) ------------------------------------------------
struct Lever {
  uint64_t frames = 0, taken = 0, declined = 0;
  uint64_t by[4] = {};  // declined by reason (named per lever in LogCensus)
};
struct Census {
  Lever content, ways, sampler, hash;
  uint64_t ways_hit_extra = 0;  // hits found in a way other than the first
  uint64_t window_start = 0;
};
inline Census g_census;

using LogFn = void (*)(const char*);
// Once per guest frame at the present: counts the frames each lever was on, and every 600 frames logs one line per lever that was on in the window:
//   [rdr][<lever>] frames N, taken T, declined D (reason a X, reason b Y)
inline census_flush::Site g_cf_slot_geo;   // S102 CEN1
inline void LogCensus(uint64_t frame, LogFn log) {
  Census& c = g_census;
  if (g_content_skip_on.load(std::memory_order_relaxed)) ++c.content.frames;
  if (g_slot_ways_on.load(std::memory_order_relaxed)) ++c.ways.frames;
  if (g_sampler_memo_on.load(std::memory_order_relaxed)) ++c.sampler.frames;
  if (g_geo_hash_once_on.load(std::memory_order_relaxed)) ++c.hash.frames;
  if (!c.window_start) c.window_start = frame;
  if (frame < c.window_start + 600 && !(census_flush::Due(g_cf_slot_geo) && frame > c.window_start)) return;   // S102 CEN1: a flip phase end closes the window now
  c.window_start = frame;
  char line[320];
  auto emit = [&](const char* name, Lever& l, const char* r0, const char* r1, const char* r2, const char* extra) {
    if (l.frames) {
      std::snprintf(line, sizeof(line), "[rdr][%s] frames %llu, taken %llu, declined %llu (%s %llu, %s %llu, %s %llu)%s", name, (unsigned long long)l.frames,
                    (unsigned long long)l.taken, (unsigned long long)l.declined, r0, (unsigned long long)l.by[0], r1, (unsigned long long)l.by[1], r2,
                    (unsigned long long)l.by[2], extra);
      if (log) log(line);
    }
    l = Lever{};
  };
  char extra[96] = "";
  std::snprintf(extra, sizeof(extra), "; hits in a later way %llu", (unsigned long long)c.ways_hit_extra);
  emit("slot_content_skip", c.content, "image not yet read this frame", "copy destination", "-", "");
  emit("slot_ways", c.ways, "no entry matched", "key matched, image flags moved", "-", extra);
  emit("sampler_memo", c.sampler, "no entry (new descriptor inputs)", "border colour note", "-", "");
  emit("geo_hash_once", c.hash, "range is not the whole stream", "no hash made (not small)", "-", "");
  c.ways_hit_extra = 0;
}

// ---- cut 1: the per-frame content check already done --------------------------------------------------------------------------------------
// EnsureContent's own first lines: `copy_dest` images go on to the CPU-write test, anything else returns at once when its checked_frame is this
// record frame ("read once per frame at most"). This is that test without the call.
inline bool ContentDone(bool copy_dest, uint64_t checked_frame, uint64_t record_frame) { return !copy_dest && checked_frame == record_frame; }

// The key of a slot record: what AddTextureSlot's old one-entry check compared (the frame tables' generation, the registry epoch, the shader's fetch
// facts (a stable pointer), the object, the six fetch words, the 24 header bytes at object + 28 when there is an object).
struct SlotKey {
  uint64_t generation = ~0ull, epoch = ~0ull;
  const void* fetch = nullptr;
  uint32_t object = 0;
  uint32_t w[6] = {};
  uint8_t header_raw[24] = {};
  int32_t aniso_override = -1;  // the sampler's other inputs (set once at a start today; in the key so no record outlives a change)
  uint32_t lod_bias_bits = 0;
  bool Equal(const SlotKey& o) const {
    return generation == o.generation && epoch == o.epoch && fetch == o.fetch && object == o.object && std::memcmp(w, o.w, sizeof(w)) == 0 &&
           aniso_override == o.aniso_override && lod_bias_bits == o.lod_bias_bits &&
           (!object || std::memcmp(header_raw, o.header_raw, sizeof(header_raw)) == 0);
  }
};

// ---- cut 2: slot records, kWays per slot -------------------------------------------------------------------------------------------------
inline constexpr int kSlotWays = 4;
// Entry is the reader's SlotCache. Way 0 is the old single entry. With ways = 1 only way 0 is ever used: the old behaviour.
template <class Entry, int Slots>
struct SlotWayTable {
  Entry e[kSlotWays][Slots];
  uint8_t next[Slots] = {};
  // The first entry for which pred(entry) holds among the first `ways`; sets `way`.
  template <class Pred>
  Entry* Find(uint32_t slot, int ways, int& way, Pred pred) {
    for (int w = 0; w < ways; ++w) {
      Entry& x = e[w][slot % Slots];
      if (pred(x)) {
        way = w;
        return &x;
      }
    }
    return nullptr;
  }
  // The entry a miss writes: way 0 with one way; else round-robin over the ways.
  Entry& Victim(uint32_t slot, int ways) {
    if (ways <= 1) return e[0][slot % Slots];
    uint8_t& n = next[slot % Slots];
    Entry& r = e[n % ways][slot % Slots];
    n = uint8_t((n + 1) % ways);
    return r;
  }
};

// ---- cut 3: the sampler descriptor memo ------------------------------------------------------------------------------------------------------
// Everything BuildSampler reads: fetch words 0, 3, 4, 5 (address modes, filters, min lod, border), the shader's overrides (mag / min / mip / aniso, 3
// and 7 meaning "not named"), the player's anisotropy override, the jitter's lod bias and the two diagnosis flags.
struct SamplerKey {
  uint32_t w[4];
  uint8_t mag, min, mip, aniso;
  int32_t aniso_override;
  uint32_t lod_bias_bits;
  uint8_t dbg_lod0, dbg_no_aniso, pad[2];
};
static_assert(sizeof(SamplerKey) == 32, "SamplerKey must have no padding (it is compared with memcmp)");
// `w` the slot's six fetch words; mag / min / mip / aniso the shader's overrides (3 / 3 / 3 / 7 = not named).
inline SamplerKey MakeSamplerKey(const uint32_t w[6], uint8_t mag, uint8_t min, uint8_t mip, uint8_t aniso, int32_t aniso_override, float lod_bias, bool dbg_lod0,
                                 bool dbg_no_aniso) {
  SamplerKey k{};
  k.w[0] = w[0], k.w[1] = w[3], k.w[2] = w[4], k.w[3] = w[5];
  k.mag = mag, k.min = min, k.mip = mip, k.aniso = aniso;
  k.aniso_override = aniso_override;
  std::memcpy(&k.lod_bias_bits, &lod_bias, 4);
  k.dbg_lod0 = dbg_lod0 ? 1 : 0, k.dbg_no_aniso = dbg_no_aniso ? 1 : 0;
  return k;
}
class SamplerMemo {
 public:
  static constexpr int kEntries = 32;
  bool Find(const SamplerKey& k, uint64_t generation, uint32_t& index) const {
    const Entry& x = e_[Slot(k)];
    if (x.generation != generation || std::memcmp(&x.key, &k, sizeof(k)) != 0) return false;
    index = x.index;
    return true;
  }
  void Put(const SamplerKey& k, uint64_t generation, uint32_t index) {
    Entry& x = e_[Slot(k)];
    x.key = k;
    x.generation = generation;
    x.index = index;
  }
  void Reset() { *this = SamplerMemo{}; }

 private:
  struct Entry {
    SamplerKey key{};
    uint64_t generation = ~0ull;
    uint32_t index = 0;
  };
  static uint32_t Slot(const SamplerKey& k) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    uint64_t words[4];
    std::memcpy(words, &k, sizeof(words));
    for (uint64_t w : words) {
      h = (h ^ w) * 0xFF51AFD7ED558CCDull;
      h ^= h >> 32;
    }
    return uint32_t(h % kEntries);
  }
  Entry e_[kEntries];
};

// ---- cut 4: the small stream's hash, once ---------------------------------------------------------------------------------------------------
// What BuildGeometry hashed for its memo check (the whole vertex stream), handed to GeoKey, which hashes the draw's vertex range: the same bytes when
// the range is the whole stream (pointer and length equal). HashFn is any (const void*, size_t) -> uint64_t; the reader passes cap::Xxh3.
struct StreamHint {
  const uint8_t* ptr = nullptr;
  uint32_t len = 0;
  uint64_t hash = 0;
  bool valid = false;
};
inline bool HintMatches(const StreamHint& hint, const uint8_t* p, uint32_t len) { return hint.valid && hint.ptr == p && hint.len == len; }
template <class HashFn>
inline uint64_t VertexRangeHash(const StreamHint& hint, const uint8_t* p, uint32_t len, HashFn fn, bool& reused) {
  reused = HintMatches(hint, p, len);
  return reused ? hint.hash : fn(p, len);
}
// The memo check's combined hash of a stream and its index range, built from the parts (so the stream part can be kept).
inline uint64_t CombineSmall(uint64_t stream_hash, bool has_index, uint64_t index_hash) {
  return has_index ? (stream_hash ^ (index_hash * 0x9E3779B97F4A7C15ull)) : stream_hash;
}

}  // namespace rdr_adapter::slot_geo_memo
