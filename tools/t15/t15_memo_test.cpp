// S102 lane T15: the offline proof of the four draw memos in src\renderer_adapter\slot_geo_memo.h (0 game runs, 0 Surface runs).
// Built by tools\t15\t15_check.py:   clang++ -std=c++20 -O2 -DNOMINMAX -DT15_PLANTS -I <xxHash dir> -I src\renderer_adapter tools\t15\t15_memo_test.cpp
//
// What it drives: the REAL slot_geo_memo.h pieces (SlotKey / SlotWayTable, SamplerKey + SamplerMemo, StreamHint, ContentDone, the census)
// inside small models of the reader's functions (AddTextureSlot, BuildSampler + Dedupe, EnsureContent, BuildGeometry's hash and GeoKey), written line for line
// from reader.h (the check script's wiring part pins the reader's text to the same shape). Every arm runs the same scripted input twice, switches off and
// on, and requires the RECORDED outputs (the slot tables, the sampler tables, the content reads, the content keys) to be equal. Inputs: synthetic scripts
// with the hazards the memos could miss (same pointer / same words with new header bytes, registry epoch moves, image flags moving mid frame, a copy
// destination written by the CPU, the player's filtering changing, a stream rewritten in place, a range that is not the whole stream, a stream that is not
// small, inline bytes that differ only at the tail), with the real xxh3 as the hash.
// Planted twins (-DT15_PLANTS): each memo with one hole, which the same arms must catch.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "slot_geo_memo.h"

namespace dm = rdr_adapter::slot_geo_memo;

namespace {
int g_fail = 0, g_pass = 0;
void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  ok ? ++g_pass : ++g_fail;
}
uint64_t Xxh3(const void* p, size_t n) { return XXH3_64bits(p, n); }

// ======================================================================================================================================
// Part 1: texture slots (AddTextureSlot, EnsureContent, BuildSampler + Dedupe)
// ======================================================================================================================================
enum Plant { kNone, kSkipIgnoresCopyDest, kKeyIgnoresHeader, kKeyIgnoresGeneration, kSamplerIgnoresAniso, kSamplerIgnoresGeneration, kWaysNoFlagCheck };

struct Img {
  bool packed = false, copy_dest = false, swap = false, target16 = false, cpu_written = false;
  uint8_t pack_host[4] = {0, 1, 2, 3};
  uint64_t checked_frame = ~0ull;
};
struct Obj {
  uint32_t id = 0;
  uint8_t header_raw[24] = {};
};
struct SlotOut {
  uint32_t slot, image, sampler, swz;
  bool operator==(const SlotOut& o) const { return slot == o.slot && image == o.image && sampler == o.sampler && swz == o.swz; }
};
struct SlotRecord {
  uint8_t kind = 0;
  dm::SlotKey key;
  uint32_t id = 0;
  bool packed = false, copy_dest = false, swap = false, target16 = false;
  uint8_t pack_host[4] = {};
  SlotOut out{};
};
struct Switches {
  bool content_skip = false, ways = false, sampler_memo = false;
  Plant plant = kNone;
};
struct World {
  std::vector<Img> imgs;
  std::map<uint32_t, Obj> objs;
  uint64_t record_frame = 0, generation = 0, epoch = 1;
  int aniso_override = -1;
  float lod_bias = 0;
  std::vector<std::array<uint8_t, 32>> samplers;  // the frame tables
  std::map<std::array<uint8_t, 32>, uint32_t> sampler_map;
  std::vector<SlotOut> slots;
  std::vector<std::pair<uint64_t, uint32_t>> reads;  // (frame, image): EnsureContent's reads
  uint64_t slot_calls = 0, hits = 0, content_calls = 0, sampler_builds = 0;
  dm::SlotWayTable<SlotRecord, 32> table;
  dm::SamplerMemo smemo;
  void NewFrame() {
    ++record_frame;
    ++generation;  // FrameTables::Reset
    samplers.clear();
    sampler_map.clear();
  }
};

// reader.h BuildSampler, reduced to the inputs it reads: the descriptor bytes are a pure function of them (the real one fills rdr::SamplerDesc the same way)
std::array<uint8_t, 32> SamplerDescOf(const dm::SamplerKey& k) {
  std::array<uint8_t, 32> d{};
  std::memcpy(d.data(), &k, 32);
  d[1] = uint8_t(k.w[0] >> 10 & 7);
  return d;
}
uint32_t BuildSampler(World& w, const uint32_t words[6], uint8_t mag, uint8_t min, uint8_t mip, uint8_t aniso) {
  ++w.sampler_builds;
  const dm::SamplerKey k = dm::MakeSamplerKey(words, mag, min, mip, aniso, w.aniso_override, w.lod_bias, false, false);
  const auto d = SamplerDescOf(k);
  auto it = w.sampler_map.find(d);  // Dedupe
  if (it != w.sampler_map.end()) return it->second;
  const uint32_t at = uint32_t(w.samplers.size());
  w.samplers.push_back(d);
  w.sampler_map[d] = at;
  return at;
}
uint32_t BuildSamplerMemo(World& w, const uint32_t words[6], uint8_t mag, uint8_t min, uint8_t mip, uint8_t aniso, const Switches& sw) {
  if (!sw.sampler_memo) return BuildSampler(w, words, mag, min, mip, aniso);
  int32_t ovr = w.aniso_override;
  if (sw.plant == kSamplerIgnoresAniso) ovr = -1;
  const dm::SamplerKey k = dm::MakeSamplerKey(words, mag, min, mip, aniso, ovr, w.lod_bias, false, false);
  const uint64_t gen = sw.plant == kSamplerIgnoresGeneration ? 0 : w.generation;
  uint32_t index = 0;
  if (w.smemo.Find(k, gen, index)) return index;
  index = BuildSampler(w, words, mag, min, mip, aniso);
  w.smemo.Put(k, gen, index);
  return index;
}
// reader.h EnsureContent (the parts that decide a read)
void EnsureContent(World& w, uint32_t id) {
  ++w.content_calls;
  Img& img = w.imgs[id];
  if (img.copy_dest) {
    if (!img.cpu_written) return;
    img.copy_dest = false;  // "copy_destination_rewritten_by_cpu"
  }
  if (img.checked_frame == w.record_frame) return;
  img.checked_frame = w.record_frame;
  w.reads.push_back({w.record_frame, id});
}
void EnsureContentChecked(World& w, uint32_t id, const Switches& sw) {
  if (sw.content_skip) {
    const Img& img = w.imgs[id];
    const bool copy_dest = sw.plant == kSkipIgnoresCopyDest ? false : img.copy_dest;
    if (dm::ContentDone(copy_dest, img.checked_frame, w.record_frame)) return;
  }
  EnsureContent(w, id);
}
// reader.h AddTextureSlot: the hit path, the miss path (the image lookup + flags + sampler), remember()
void AddSlot(World& w, const Switches& sw, uint32_t slot, const void* fetch, uint8_t mag, uint8_t aniso, const uint32_t words[6], uint32_t object) {
  ++w.slot_calls;
  const uint8_t* header_raw = object ? w.objs[object].header_raw : nullptr;
  dm::SlotKey now;
  now.generation = sw.plant == kKeyIgnoresGeneration ? 0 : w.generation;
  now.epoch = w.epoch;
  now.fetch = fetch;
  now.object = object;
  std::memcpy(now.w, words, 24);
  now.aniso_override = w.aniso_override;
  std::memcpy(&now.lod_bias_bits, &w.lod_bias, 4);
  if (object && sw.plant != kKeyIgnoresHeader) std::memcpy(now.header_raw, header_raw, 24);
  int way = 0;
  SlotRecord* matched = w.table.Find(slot, sw.ways ? dm::kSlotWays : 1, way, [&](const SlotRecord& c) { return c.kind && c.key.Equal(now); });
  if (matched) {
    SlotRecord& sc = *matched;
    if (sc.kind == 2) {
      w.slots.push_back(sc.out);
      ++w.hits;
      return;
    }
    const Img& img = w.imgs[sc.id];
    if (sw.plant == kWaysNoFlagCheck || (img.packed == sc.packed && img.copy_dest == sc.copy_dest && img.swap == sc.swap && img.target16 == sc.target16 && std::memcmp(img.pack_host, sc.pack_host, 4) == 0)) {
      w.slots.push_back(sc.out);
      ++w.hits;
      EnsureContentChecked(w, sc.id, sw);
      return;
    }
  }
  SlotRecord& sc = matched ? *matched : w.table.Victim(slot, sw.ways ? dm::kSlotWays : 1);
  sc.kind = 0;
  SlotOut out{slot, 0, 0, 0};
  if ((words[0] & 3) == 1) {  // a vertex type in a texture slot: "no texture", kind 2
    w.slots.push_back(out);
    sc.kind = 2, sc.key = now, sc.key.epoch = w.epoch, sc.out = out, sc.id = 0;
    return;
  }
  uint32_t id = 0;
  if (object) id = w.objs[object].id;
  if (object && ((w.objs[object].header_raw[0] ^ words[1]) & 0x40)) id = 0;  // "slot words differ from the object": a pure function of the same inputs
  if (!id) id = 1 + (words[1] >> 12) % 3;                                  // TextureOfWords
  const Img& img = w.imgs[id];
  out.image = id;
  out.swz = (words[3] >> 1) * 7 + (img.packed ? 1 : 0) + (img.copy_dest ? 2 : 0) + (img.swap ? 4 : 0) + (img.target16 ? 8 : 0) + (object ? w.objs[object].header_raw[1] : 0) * 16;
  out.sampler = BuildSamplerMemo(w, words, mag, 3, 3, aniso, sw);
  w.slots.push_back(out);
  sc.kind = 1, sc.key = now, sc.key.epoch = w.epoch, sc.id = id;
  sc.packed = img.packed, sc.copy_dest = img.copy_dest, sc.swap = img.swap, sc.target16 = img.target16;
  std::memcpy(sc.pack_host, img.pack_host, 4);
  sc.out = out;
  EnsureContentChecked(w, id, sw);
}

struct SlotRun {
  std::vector<SlotOut> slots;
  std::vector<std::pair<uint64_t, uint32_t>> reads;
  std::vector<std::array<uint8_t, 32>> sampler_tables_flat;  // every frame's table, concatenated
  uint64_t calls = 0, hits = 0, content_calls = 0, sampler_builds = 0;
};
// The script: `frames` frames of `draws` draws; each draw adds 1-4 slots from a pool of textures; events at random.
SlotRun RunSlotScript(uint32_t seed, const Switches& sw, int frames, int draws, bool alternate) {
  World w;
  w.imgs.resize(8);
  for (uint32_t o = 1; o <= 6; ++o) {
    Obj& ob = w.objs[o];
    ob.id = 1 + (o % 3);
    for (int b = 0; b < 24; ++b) ob.header_raw[b] = uint8_t(o * 31 + b);
  }
  w.imgs[2].copy_dest = true;
  w.imgs[3].packed = true;
  std::mt19937 rng(seed);
  static const int fetch_tag[3] = {0, 0, 0};
  SlotRun run;
  for (int f = 0; f < frames; ++f) {
    w.NewFrame();
    for (int d = 0; d < draws; ++d) {
      const uint32_t ev = rng() % 400;  // events: the same sequence in every arm (the rng is not consumed by the arms)
      if (ev == 0) w.epoch++;                                                    // the registry moved
      else if (ev == 1) w.objs[1 + rng() % 6].header_raw[rng() % 24] ^= 0x5A;    // same pointer, new header bytes
      else if (ev == 2) w.imgs[1 + rng() % 4].swap ^= true;                      // image flags move mid frame
      else if (ev == 3) w.imgs[2].cpu_written = true;                            // a CPU write to the copy destination
      else if (ev == 4) w.aniso_override = int(rng() % 7) - 1;                   // the player's filtering
      else if (ev == 5) w.lod_bias = float(rng() % 3) * 0.25f;
      else if (ev == 6) w.imgs[2].copy_dest = true, w.imgs[2].cpu_written = false;
      const int n = 1 + int(rng() % 4);
      for (int s = 0; s < n; ++s) {
        const uint32_t pick = alternate ? uint32_t((d + s) % 3) : uint32_t(rng() % 5);  // alternating = two shaders / textures taking turns on a slot
        const uint32_t words[6] = {2u | (pick << 10), 0x1000u * (pick + 1) + 0x40u, 0x55u, (0xAAu << 1) | (pick << 19), 0x3u << 2, 0x2u};
        const uint32_t object = (pick % 2) ? 1 + pick : 0;
        AddSlot(w, sw, uint32_t(s), &fetch_tag[pick % 3], uint8_t(pick % 3), uint8_t(pick == 4 ? 7 : 3), words, object);
      }
    }
    for (const auto& d : w.samplers) run.sampler_tables_flat.push_back(d);
    run.sampler_tables_flat.push_back(std::array<uint8_t, 32>{});  // a frame separator
    if (f % 7 == 3) w.imgs[2].cpu_written = false;
  }
  run.slots = w.slots;
  run.reads = w.reads;
  run.calls = w.slot_calls, run.hits = w.hits, run.content_calls = w.content_calls, run.sampler_builds = w.sampler_builds;
  return run;
}
bool SameRun(const SlotRun& a, const SlotRun& b) { return a.slots == b.slots && a.reads == b.reads && a.sampler_tables_flat == b.sampler_tables_flat; }

void Part1() {
  std::printf("-- part 1: texture slots (content skip, 4 ways, sampler memo)\n");
  const Switches off;
  struct Arm {
    const char* name;
    Switches sw;
  };
  Arm arms[4];
  arms[0] = {"slot_content_skip", {true, false, false, kNone}};
  arms[1] = {"slot_ways", {false, true, false, kNone}};
  arms[2] = {"sampler_memo", {false, false, true, kNone}};
  arms[3] = {"all three", {true, true, true, kNone}};
  for (bool alt : {false, true}) {
    for (const Arm& arm : arms) {
      bool equal = true;
      uint64_t calls = 0, hits_off = 0, hits_on = 0, cc_off = 0, cc_on = 0, sb_off = 0, sb_on = 0;
      for (uint32_t seed = 1; seed <= 12; ++seed) {
        const SlotRun a = RunSlotScript(seed, off, 60, 300, alt), b = RunSlotScript(seed, arm.sw, 60, 300, alt);
        equal &= SameRun(a, b);
        calls += a.calls, hits_off += a.hits, hits_on += b.hits, cc_off += a.content_calls, cc_on += b.content_calls, sb_off += a.sampler_builds, sb_on += b.sampler_builds;
      }
      char msg[400];
      std::snprintf(msg, sizeof(msg),
                    "%s (%s pattern): slot tables, sampler tables and content reads equal off / on over 12 seeds; slot hits %llu -> %llu of %llu, EnsureContent calls %llu -> %llu, sampler builds %llu -> %llu",
                    arm.name, alt ? "alternating" : "random", (unsigned long long)hits_off, (unsigned long long)hits_on, (unsigned long long)calls, (unsigned long long)cc_off,
                    (unsigned long long)cc_on, (unsigned long long)sb_off, (unsigned long long)sb_on);
      Check(equal, msg);
      if (arm.sw.content_skip && !arm.sw.ways) Check(cc_on < cc_off, "  content skip: it removes EnsureContent calls (the lever does something)");
      if (!arm.sw.content_skip && arm.sw.ways && alt) Check(hits_on > hits_off, "  slot ways: more slot hits on an alternating pattern (the lever does something)");
      if (arm.sw.sampler_memo && !arm.sw.ways) Check(sb_on < sb_off, "  sampler memo: fewer sampler builds");
    }
  }
#ifdef T15_PLANTS
  struct P {
    const char* name;
    Plant p;
    Switches sw;
  };
  P plants[6];
  plants[0] = {"content skip that ignores copy_dest (a CPU-written copy destination is never re-read)", kSkipIgnoresCopyDest, {true, false, false, kNone}};
  plants[1] = {"slot record without the header bytes (same pointer, new header)", kKeyIgnoresHeader, {false, true, false, kNone}};
  plants[2] = {"slot record without the generation (a stale sampler index)", kKeyIgnoresGeneration, {false, true, false, kNone}};
  plants[3] = {"sampler memo blind to the player's filtering override", kSamplerIgnoresAniso, {false, false, true, kNone}};
  plants[4] = {"sampler memo blind to the frame tables' generation", kSamplerIgnoresGeneration, {false, false, true, kNone}};
  plants[5] = {"slot records that skip the image flag check (a swap flag that moved mid frame is not seen)", kWaysNoFlagCheck, {false, true, false, kNone}};
  for (P& p : plants) {
    p.sw.plant = p.p;
    bool caught = false;
    for (bool alt : {false, true}) {
      for (uint32_t seed = 1; seed <= 12 && !caught; ++seed) {
        const SlotRun a = RunSlotScript(seed, off, 60, 300, alt), b = RunSlotScript(seed, p.sw, 60, 300, alt);
        caught |= !SameRun(a, b);
      }
    }
    Check(caught, (std::string("planted twin ") + (caught ? "caught: " : "NOT caught: ") + p.name).c_str());
  }
#endif
}

// ======================================================================================================================================
// Part 2: geometry (the small-stream hash once)
// ======================================================================================================================================
// reader.h GeoResolve1 / GeoKey, with the same mixing
uint32_t Resolve1(uint32_t base_vertex, uint32_t vmin, uint32_t vmax, uint32_t idx) {
  const uint32_t v = (idx + base_vertex) & 0xFFFFFFu;
  const uint32_t lo = vmin < vmax ? vmin : vmax;
  return v < lo ? lo : (v > vmax ? vmax : v);
}
uint64_t ContentKey(uint64_t vhash, uint64_t layout_hash, uint32_t prim, const std::vector<uint32_t>& indices) {
  uint64_t key = vhash;
  key ^= layout_hash * 0x9E3779B97F4A7C15ull;
  key ^= uint64_t(prim) * 0xC2B2AE3D27D4EB4Full;
  key ^= Xxh3(indices.data(), indices.size() * 4) * 0xFF51AFD7ED558CCDull;
  return key;
}
struct Draw {
  const uint8_t* stream;  // the guest stream (stable address)
  uint32_t stream_bytes, lo_vertex, v_count, stride;
  bool inline_draw;
  uint32_t count, base_vertex, vmin, vmax, prim;
  std::vector<uint32_t> raw_indices;  // indexed draws
};
std::vector<uint32_t> IndicesOf(const Draw& d) {
  std::vector<uint32_t> out(d.count);
  for (uint32_t i = 0; i < d.count; ++i) out[i] = Resolve1(d.base_vertex, d.vmin, d.vmax, d.inline_draw ? i : d.raw_indices[i]);
  return out;
}

enum GPlant { kGNone, kHintNeverCleared, kHintIgnoresLen, kHintFromOtherStream };
struct GeoSwitches {
  bool hash_once = false;
  GPlant plant = kGNone;
};
struct GeoRun {
  std::vector<uint64_t> keys;
  uint64_t hashes = 0, hint_taken = 0;
  bool operator==(const GeoRun& o) const { return keys == o.keys; }
};
// reader.h BuildGeometry's small-stream hashing + RunGeometryBuild's GeoKey, per draw.
GeoRun RunGeoScript(uint32_t seed, const GeoSwitches& sw, int draws) {
  std::mt19937 rng(seed);
  constexpr int kStreams = 6;
  std::vector<uint8_t> store[kStreams];  // streams at stable addresses, rewritten in place at random (same pointer, new content)
  for (int s = 0; s < kStreams; ++s) {
    store[s].resize(1024 + 640 * size_t(s));
    for (auto& b : store[s]) b = uint8_t(rng());
  }
  std::vector<uint8_t> inl_store[3];
  for (auto& v : inl_store) {
    v.resize(96);
    for (auto& b : v) b = uint8_t(rng());
  }
  GeoRun run;
  dm::StreamHint hint;  // the thread-local t_stream_hint
  dm::StreamHint other;  // (the plant "from another stream": the hint of the previous draw whatever stream this one reads)
  const uint64_t layout = 0x1234567ull;
  for (int i = 0; i < draws; ++i) {
    if (rng() % 7 == 0) store[rng() % kStreams][rng() % 400] ^= uint8_t(1 + rng() % 255);  // a stream rewritten in place
    if (rng() % 5 == 0) inl_store[rng() % 3][rng() % 96] ^= uint8_t(1 + rng() % 255);      // inline bytes change (also at the tail)
    Draw d;
    d.prim = 4;
    d.base_vertex = rng() % 3;
    d.vmin = 0;
    d.vmax = 3 + rng() % 22;
    bool small = true;
    if (rng() % 2 == 0) {
      const int s = int(rng() % kStreams);
      d.inline_draw = false;
      d.stream = store[s].data();
      d.stream_bytes = uint32_t(store[s].size());
      d.stride = 16;
      d.count = 12 + rng() % 4;
      d.raw_indices.resize(d.count);
      for (auto& x : d.raw_indices) x = rng() % 24;
      const bool whole = rng() % 3 != 0;  // the draw's vertex range: the whole stream, or a sub range (what GeoRange would find)
      d.lo_vertex = whole ? 0 : rng() % 3;  // (a sub range that starts at 0: same pointer, shorter length)
      d.v_count = whole ? d.stream_bytes / d.stride : (d.stream_bytes / d.stride) / 2;
      small = rng() % 4 != 0;  // a stream over the small limit: no hash is made, no hint is set
    } else {
      const int s = int(rng() % 3);
      d.inline_draw = true;
      d.stream = inl_store[s].data();
      d.stream_bytes = 96;
      d.stride = 16;
      d.count = 6;
      d.lo_vertex = 0;
      d.v_count = 6;
    }
    const uint32_t rest = d.stream_bytes - d.lo_vertex * d.stride;
    const uint32_t hash_len = d.v_count * d.stride < rest ? d.v_count * d.stride : rest;
    const uint8_t* vsrc = d.stream + d.lo_vertex * d.stride;
    // ---- BuildGeometry's memo check hash (small streams only), kept when the switch is on
    if (!d.inline_draw && small) {
      const uint64_t sh = Xxh3(d.stream, d.stream_bytes);  // the check's hash of the whole stream
      ++run.hashes;
      if (sw.hash_once) {
        hint = {d.stream, d.stream_bytes, sh, true};
        other = hint;
      } else {
        ++run.hashes;  // the old code hashed again for the memo store
      }
    }
    // ---- GeoKey
    const std::vector<uint32_t> indices = IndicesOf(d);
    dm::StreamHint use = sw.plant == kHintFromOtherStream ? other : hint;
    if (sw.plant == kHintIgnoresLen) use.len = hash_len;  // accepted whatever the range
    const bool match = !d.inline_draw && sw.hash_once && dm::HintMatches(use, vsrc, hash_len);
    uint64_t vhash;
    if (match) {
      vhash = use.hash;
      ++run.hint_taken;
    } else {
      vhash = Xxh3(vsrc, hash_len);
      ++run.hashes;
    }
    run.keys.push_back(ContentKey(vhash, layout, d.prim, indices));
    if (sw.plant != kHintNeverCleared) hint.valid = false;  // reader.h: t_stream_hint.valid = false after the build
  }
  return run;
}

void Part2() {
  std::printf("-- part 2: geometry (small-stream hash once)\n");
  const GeoSwitches off;
  GeoSwitches on;
  on.hash_once = true;
  bool equal = true;
  GeoRun ta{}, tb{};
  for (uint32_t seed = 1; seed <= 20; ++seed) {
    const GeoRun a = RunGeoScript(seed, off, 4000), b = RunGeoScript(seed, on, 4000);
    equal &= a == b;
    ta.hashes += a.hashes, tb.hashes += b.hashes, tb.hint_taken += b.hint_taken;
  }
  char msg[400];
  std::snprintf(msg, sizeof(msg), "geo_hash_once: every content key equal off / on over 20 seeds x 4000 draws (whole and partial ranges, streams rewritten in place, streams over the small limit, inline draws); hashes %llu -> %llu, hint taken %llu",
                (unsigned long long)ta.hashes, (unsigned long long)tb.hashes, (unsigned long long)tb.hint_taken);
  Check(equal, msg);
  {  // F6: CombineSmall is the old combined hash (BuildGeometry's full_hash_of: h = H(stream); if an index range, h ^= H(index) * golden)
    bool same = true;
    std::mt19937 r(7);
    for (int i = 0; i < 2000; ++i) {
      uint8_t sb[40], ib[24];
      for (auto& b : sb) b = uint8_t(r());
      for (auto& b : ib) b = uint8_t(r());
      const uint64_t hs = Xxh3(sb, sizeof(sb)), hi = Xxh3(ib, sizeof(ib));
      same &= dm::CombineSmall(hs, true, hi) == (hs ^ hi * 0x9E3779B97F4A7C15ull);
      same &= dm::CombineSmall(hs, false, 0) == hs;
      same &= dm::CombineSmall(hs, false, hi) == hs;
    }
    Check(same, "CombineSmall(H(s), true, H(i)) == H(s) ^ H(i) * 0x9E3779B97F4A7C15, and the no-index case == H(s) (2000 random pairs)");
  }
  Check(tb.hashes < ta.hashes && tb.hint_taken > 0, "  geo_hash_once: fewer hashes, and the hint was used (the lever does something)");
#ifdef T15_PLANTS
  struct P {
    const char* name;
    GPlant p;
  };
  const P plants[3] = {{"hint never cleared (a stale stream hash reaches the next draw)", kHintNeverCleared},
                       {"hint accepted whatever the range length (a sub range from the stream's start)", kHintIgnoresLen},
                       {"hint kept from the previous draw's stream (a stream hint for another stream)", kHintFromOtherStream}};
  for (const P& p : plants) {
    GeoSwitches sw = on;
    sw.plant = p.p;
    bool caught = false;
    for (uint32_t seed = 1; seed <= 20 && !caught; ++seed) caught |= !(RunGeoScript(seed, off, 4000) == RunGeoScript(seed, sw, 4000));
    Check(caught, (std::string("planted twin ") + (caught ? "caught: " : "NOT caught: ") + p.name).c_str());
  }
#endif
}

// ======================================================================================================================================
// Part 3: the census line (the format the live path logs) and the microbench
// ======================================================================================================================================
std::vector<std::string> g_lines;
void Collect(const char* l) { g_lines.push_back(l); }
void Part3() {
  std::printf("-- part 3: census line\n");
  dm::g_content_skip_on = true, dm::g_slot_ways_on = true, dm::g_sampler_memo_on = true, dm::g_geo_hash_once_on = true;
  dm::g_census = dm::Census{};
  dm::g_census.content.taken = 90, dm::g_census.content.declined = 10, dm::g_census.content.by[0] = 7, dm::g_census.content.by[1] = 3;
  g_lines.clear();
  dm::LogCensus(100, &Collect);  // opens the window (counts frame 1)
  for (uint64_t f = 101; f <= 700; ++f) dm::LogCensus(f, &Collect);
  bool shape = false;
  for (auto& l : g_lines) {
    std::printf("  %s\n", l.c_str());
    shape |= l.find("[rdr][slot_content_skip] frames 601, taken 90, declined 10 (") == 0;
  }
  Check(shape && g_lines.size() == 4, "census: one '[rdr][<lever>] frames N, taken T, declined D (by reason)' line per switched-on lever every 600 frames");
  dm::g_content_skip_on = false, dm::g_slot_ways_on = false, dm::g_sampler_memo_on = false, dm::g_geo_hash_once_on = false;
  g_lines.clear();
  for (uint64_t f = 701; f <= 1400; ++f) dm::LogCensus(f, &Collect);
  Check(g_lines.empty(), "census: no line while every switch is off");
}

double NsPer(const std::chrono::steady_clock::time_point& t0, uint64_t n) {
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / double(n);
}
uint64_t g_sink = 0;
// db::Hash64 of src\d3d_shader_bind.h (the Dedupe hash), copied for the model
uint64_t Hash64(const uint8_t* p, size_t n) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(n) * 0xC2B2AE3D27D4EB4Full);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
  }
  uint64_t tail = 0;
  for (size_t k = 0; i + k < n; ++k) tail |= uint64_t(p[i + k]) << (8 * k);
  h = (h ^ tail) * 0xC4CEB9FE1A85EC53ull;
  h ^= h >> 29;
  return h;
}

// The dropped experiment (a ring of 8 remembering inline builds' content keys; see slot_geo_memo.h): kept here only for its microbench.
struct ExpInlineKey {
  const void* vs;
  uint32_t prim, count, base_vertex, vmin, vmax, hash_len, lo, v_count, guest_stride, endian, restart_on, reset_value;
};
class ExpInlineMemo {
 public:
  bool Find(const ExpInlineKey& k, const uint8_t* bytes, uint32_t len, uint64_t& content_key) const {
    for (const Entry& x : e_) {
      if (x.used && x.len == len && std::memcmp(&x.key, &k, sizeof(k)) == 0 && std::memcmp(x.bytes.data(), bytes, len) == 0) {
        content_key = x.content_key;
        return true;
      }
    }
    return false;
  }
  void Put(const ExpInlineKey& k, const uint8_t* bytes, uint32_t len, uint64_t content_key) {
    Entry& x = e_[next_];
    next_ = (next_ + 1) % 8;
    x.used = true, x.key = k, x.len = len, x.content_key = content_key;
    x.bytes.assign(bytes, bytes + len);
  }

 private:
  struct Entry {
    bool used = false;
    ExpInlineKey key{};
    uint32_t len = 0;
    uint64_t content_key = 0;
    std::vector<uint8_t> bytes;
  };
  Entry e_[8];
  int next_ = 0;
};
void Bench() {
  std::printf("-- microbench (this PC, ns per call; models of the reader's code with the real hashes; the Surface is slower)\n");
  {  // 1. EnsureContent call vs the inline early-out
    constexpr uint64_t N = 20000000;
    std::vector<Img> imgs(64);
    for (auto& i : imgs) i.checked_frame = 5;
    uint64_t acc = 0, ticks = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
      const uint64_t a = __rdtsc();  // ScopedNs
      Img& img = imgs[i & 63];
      if (img.copy_dest || img.checked_frame != 5) acc++;
      ticks += __rdtsc() - a;
    }
    const double call = NsPer(t0, N);
    t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
      const Img& img = imgs[i & 63];
      if (!dm::ContentDone(img.copy_dest, img.checked_frame, 5)) acc++;
    }
    const double skip = NsPer(t0, N);
    g_sink += acc + ticks;
    std::printf("  slot_content_skip: the EnsureContent call (timer + image lookup + test) %.1f ns -> the inline test %.1f ns per repeated slot (the model leaves out the call's frame and the debug cvar read)\n", call, skip);
  }
  {  // 2. sampler: build + Dedupe (Hash64 + unordered_map find + memcmp) vs memo find
    constexpr uint64_t N = 4000000;
    std::unordered_map<uint64_t, uint32_t> map;
    std::vector<std::array<uint8_t, 32>> table;
    uint32_t w[6] = {2, 0, 0, 0xAA << 1, 3 << 2, 2};
    dm::SamplerMemo memo;
    std::vector<dm::SamplerKey> keys;
    for (int i = 0; i < 6; ++i) {
      w[3] = uint32_t(0xAA + i) << 1;
      keys.push_back(dm::MakeSamplerKey(w, 3, 3, 3, 7, -1, 0.0f, false, false));
    }
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
      const dm::SamplerKey& k = keys[i % 6];
      const auto d = SamplerDescOf(k);  // BuildSampler's arithmetic (modelled as the copy)
      const uint64_t h = Hash64(d.data(), 32);
      auto it = map.find(h);
      if (it != map.end() && std::memcmp(table[it->second].data(), d.data(), 32) == 0) {
        g_sink += it->second;
      } else {
        map[h] = uint32_t(table.size());
        table.push_back(d);
      }
    }
    const double built = NsPer(t0, N);
    for (int i = 0; i < 6; ++i) memo.Put(keys[size_t(i)], 1, uint32_t(i));
    t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
      uint32_t idx = 0;
      if (memo.Find(keys[i % 6], 1, idx)) g_sink += idx;
    }
    const double memoed = NsPer(t0, N);
    std::printf("  sampler_memo: descriptor + Hash64 + map find %.1f ns -> memo find %.1f ns per slot miss (the real BuildSampler fills a larger descriptor: more than the model's copy)\n", built, memoed);
  }
  {  // 3. geo hash once: a changed 3 KB small stream: three xxh3 passes (check, store, GeoKey) vs one
    constexpr uint64_t N = 300000;
    std::vector<uint8_t> stream(3072);
    for (size_t i = 0; i < stream.size(); ++i) stream[i] = uint8_t(i * 31);
    uint64_t acc = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
      stream[i & 255] ^= 1;
      acc += Xxh3(stream.data(), stream.size()) + Xxh3(stream.data(), stream.size()) + Xxh3(stream.data(), stream.size());
    }
    const double three = NsPer(t0, N);
    t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; ++i) {
      stream[i & 255] ^= 1;
      acc += Xxh3(stream.data(), stream.size());
    }
    const double one = NsPer(t0, N);
    g_sink += acc;
    std::printf("  geo_hash_once: a changed 3 KB small stream: 3 hashes %.0f ns -> 1 hash %.0f ns per changed draw (an unchanged draw already hashed once)\n", three, one);
  }
  {  // 4. (dropped) inline memo: GeoKey (resolve the indices, hash the bytes and the indices) vs find (key compare + memcmp), by draw size
    for (uint32_t n : {96u, 384u, 1024u, 3072u}) {
      const uint64_t N = 400000000ull / (n + 200);
      std::vector<uint8_t> bytes(n);
      for (uint32_t i = 0; i < n; ++i) bytes[i] = uint8_t(i * 7);
      const uint32_t verts = n / 16;
      std::vector<uint32_t> idx(verts);
      uint64_t acc = 0;
      auto t0 = std::chrono::steady_clock::now();
      for (uint64_t i = 0; i < N; ++i) {
        bytes[0] = uint8_t(i & 7);
        for (uint32_t j = 0; j < verts; ++j) idx[j] = Resolve1(1, 0, verts, j);
        acc += ContentKey(Xxh3(bytes.data(), n), 77, 4, idx);
      }
      const double full = NsPer(t0, N);
      ExpInlineMemo memo;
      ExpInlineKey k{};
      k.prim = 4, k.count = verts, k.base_vertex = 1, k.vmax = verts, k.hash_len = n, k.v_count = verts, k.guest_stride = 16, k.endian = 2;
      for (int v = 0; v < 8; ++v) {
        bytes[0] = uint8_t(v);
        memo.Put(k, bytes.data(), n, uint64_t(1000 + v));
      }
      t0 = std::chrono::steady_clock::now();
      for (uint64_t i = 0; i < N; ++i) {
        bytes[0] = uint8_t(i & 7);
        uint64_t key = 0;
        if (memo.Find(k, bytes.data(), n, key)) acc += key;
      }
      const double hit = NsPer(t0, N);
      g_sink += acc;
      std::printf("  dropped inline-memo experiment, %u-byte inline draw (%u vertices): key from scratch %.0f ns -> memo hit %.0f ns (ring of 8, the hit is the last-but-varied entry)%c", n, verts, full, hit, 10);
    }
  }
}
}  // namespace

int main() {
  Part1();
  Part2();
  Part3();
  Bench();
  std::printf("%d checks passed, %d failed (sink %llu)\n", g_pass, g_fail, (unsigned long long)(g_sink & 1));
  if (g_fail) return 1;
  std::printf("all checks passed\n");
  return 0;
}
