// S102 T4: unit test of the frame builder's fast de-dup (src/renderer/frame_builder.h, rdr::g_fb_fast). Offline, no GPU, no game.
//   fb_test.exe            -> exit 0 and "FB PASS" when every check holds
// Checks:
//   1 ids: legacy (byte-wise FNV + multimap) and fast (word-wise + open addressing) hand out the SAME id for every Add over 80 frames of a
//          workload with many near-duplicates (items that differ only in their last bytes), the frame's tables equal byte for byte, and flipping the
//          switch between frames on ONE builder changes nothing (the mode is latched per frame).
//   2 distribution: 120k distinct states / dynamics / samplers / texture sets: no full 64-bit hash collision, and the low-12-bit buckets are as even as a
//          random hash's (chi-square inside 6 sigma), for the word hash and, as a reference line, for the old byte hash.
//   3 storage: after warm-up the fast tables allocate nothing: slots / items / keys keep their data pointers and capacities across 40 frames.
//   4 cost: ns an Add, legacy vs fast, printed (not a bar here: the bar is equality; direction only).
// Planted twins (the build script compiles them; each must FAIL):
//   -DRDR_FB_PLANT_LASTWORD            a hash that ignores the last word: check 2 (full collisions) fails
//   -DRDR_FB_PLANT_LASTWORD -DRDR_FB_PLANT_NOCMP   plus equality trusting the key: check 1 (an id differs) fails
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

#define RDR_FB_UNIT_TEST_BUILD
#define private public
#include "../../src/renderer/frame_builder.h"
#undef private
#include "../../src/renderer_adapter/pack_once.h"
#include "../../src/renderer_adapter/capture_reader.h"  // S102 T1b: BankRef, the bank type the binder really passes to PackOnceInto

using namespace rdr;

static uint64_t g_rng = 0x1234567ull;
static uint32_t Rnd() {
  g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
  return uint32_t(g_rng >> 11);
}

static DrawState MakeState(uint32_t n, uint32_t last) {
  DrawState s;
  std::memset(&s, 0, sizeof s);
  s.program = uint16_t(n % 61), s.vertex_layout = uint16_t((n / 61) % 29), s.topology = uint8_t(n % 3), s.cull = uint8_t((n >> 3) % 3);
  s.depth_test = uint8_t(n & 1), s.depth_write = uint8_t((n >> 1) & 1), s.depth_compare = uint8_t((n >> 2) % 8);
  s.blend[0].enable = uint8_t((n >> 5) & 1), s.blend[0].src_color = uint8_t((n >> 6) % 19);
  s.blend[3].write_mask = uint8_t(last & 15), s.blend[3].alpha_op = uint8_t((last >> 4) & 7);  // the LAST bytes
  return s;
}
static DynamicState MakeDynamic(uint32_t n, uint32_t last) {
  DynamicState d;
  std::memset(&d, 0, sizeof d);
  d.viewport[2] = float(640 + n % 17), d.viewport[3] = float(360 + (n / 17) % 13), d.viewport[5] = 1.0f;
  d.scissor.width = 640, d.scissor.height = 360;
  d.stencil_reference = uint8_t(n % 5);
  d.system[15] = float(last);  // the last word
  d.system[0] = float(n % 7) * 0.25f;
  d.system[1] = float(n);  // makes every n distinct
  return d;
}
static SamplerDesc MakeSampler(uint32_t n, uint32_t last) {
  SamplerDesc s;
  std::memset(&s, 0, sizeof s);
  s.mag_filter = uint8_t(n & 1), s.min_filter = uint8_t((n >> 1) & 1), s.address_u = uint8_t((n >> 2) % 4), s.address_v = uint8_t((n >> 4) % 4), s.max_anisotropy = uint8_t(1 + (n >> 6) % 16);
  s.max_lod = float(last);  // the last word
  return s;
}
static TextureSet MakeTextures(uint32_t n, uint32_t last) {
  TextureSet t;
  std::memset(&t, 0, sizeof t);
  const uint32_t used = 1 + n % 9;
  for (uint32_t i = 0; i < used; ++i) t.slots[i].image = ImageId(1 + (n * 2654435761u + i * 40503u) % 3000), t.slots[i].sampler = (n + i) % 6;
  t.slots[kMaxTextures - 1].image = ImageId(last);  // the last slot
  return t;
}

struct Fail {
  int count = 0;
  void Check(bool ok, const char* what, long long a = 0, long long b = 0) {
    if (ok) return;
    ++count;
    std::printf("FB FAIL: %s (%lld, %lld)\n", what, a, b);
  }
} g_fail;

template <class T>
static bool Equal(const std::vector<T>& a, const std::vector<T>& b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

static void CheckIds() {
  FrameBuilder ref, test;  // ref stays legacy; test is flipped
  uint64_t adds = 0;
  for (int frame = 0; frame < 80; ++frame) {
    g_fb_fast = false;
    ref.Begin(frame);
    g_fb_fast = (frame % 5 != 4);  // frames 4, 9, ... run legacy on the same builder: the switch flips both ways
    test.Begin(frame);
    g_fb_fast = false;  // (the mode is latched: flipping it mid-frame must change nothing)
    const uint32_t n = 150 + Rnd() % 600;
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t base = Rnd() % 90, last = (Rnd() % 4 == 0) ? Rnd() % 6 : 0;  // many items differ only in the last word
      if (Rnd() & 1) g_fb_fast = (frame % 5 != 4);                                  // mid-frame flip (ignored by the latch)
      else g_fb_fast = false;
      const DrawState s = MakeState(base, last);
      const DynamicState d = MakeDynamic(base, last);
      const SamplerDesc sm = MakeSampler(base, last);
      const TextureSet t = MakeTextures(base, last);
      const uint32_t a0 = ref.State(s), b0 = test.State(s);
      const uint32_t a1 = ref.Dynamic(d), b1 = test.Dynamic(d);
      const uint32_t a2 = ref.Sampler(sm), b2 = test.Sampler(sm);
      const uint32_t a3 = ref.Textures(t), b3 = test.Textures(t);
      adds += 4;
      g_fail.Check(a0 == b0 && a1 == b1 && a2 == b2 && a3 == b3, "an id differs between legacy and fast", frame, k);
      if (g_fail.count > 3) return;
    }
    const Frame fa = ref.Finish(), fb = test.Finish();
    g_fail.Check(fa.states.count == fb.states.count && fa.dynamics.count == fb.dynamics.count && fa.samplers.count == fb.samplers.count && fa.texture_sets.count == fb.texture_sets.count, "table sizes differ", frame);
    g_fail.Check(std::memcmp(fa.states.data, fb.states.data, fa.states.count * sizeof(DrawState)) == 0, "states differ");
    g_fail.Check(std::memcmp(fa.dynamics.data, fb.dynamics.data, fa.dynamics.count * sizeof(DynamicState)) == 0, "dynamics differ");
    g_fail.Check(std::memcmp(fa.samplers.data, fb.samplers.data, fa.samplers.count * sizeof(SamplerDesc)) == 0, "samplers differ");
    g_fail.Check(std::memcmp(fa.texture_sets.data, fb.texture_sets.data, fa.texture_sets.count * sizeof(TextureSet)) == 0, "texture sets differ");
  }
  std::printf("[fb] ids: %llu adds over 80 frames, legacy == fast (mode flipped between frames, latched inside them)\n", (unsigned long long)adds);
}

template <class F>
static void Distribution(const char* name, uint32_t count, F make, bool word) {
  std::set<uint64_t> seen;
  std::vector<uint32_t> bucket(4096, 0);
  uint64_t full = 0;
  for (uint32_t i = 0; i < count; ++i) {
    auto v = make(i);
    const uint64_t h = word ? FrameBuilder::HashWords(&v, sizeof v) : FrameBuilder::Hash(&v, sizeof v);
    if (!seen.insert(h).second) ++full;
    ++bucket[h & 4095];
  }
  const double expect = double(count) / 4096.0;
  double chi = 0;
  for (uint32_t b : bucket) chi += (double(b) - expect) * (double(b) - expect) / expect;
  const double z = (chi - 4095.0) / std::sqrt(2.0 * 4095.0);  // ~N(0,1) for a random hash
  std::printf("[fb] distribution %-8s %-5s: %u distinct inputs, full-hash collisions %llu, bucket chi-square z %.2f\n", name, word ? "word" : "byte", count, (unsigned long long)full, z);
  if (word) {
    g_fail.Check(full == 0, "full 64-bit hash collisions", (long long)full);
    g_fail.Check(std::fabs(z) < 6.0, "low-bit buckets are uneven", (long long)(z * 100));
  }
}

static void CheckDistribution() {
  const uint32_t n = 120000;
  // distinct inputs: every one differs from the others, a third of them ONLY in the last word / last slot
  auto st = [](uint32_t i) { return MakeState(i % 9000, i / 9000); };
  auto dy = [](uint32_t i) { return MakeDynamic(i % 20000, i / 20000); };
  auto sm = [](uint32_t i) { return MakeSampler(i % 400, i / 400); };
  auto ts = [](uint32_t i) { return MakeTextures(i % 6000, 3001 + i / 6000); };
  // (MakeState's last-byte fields hold 7 bits, so 9000 x 13 distinct values stay distinct; the others are distinct by construction.)
  for (int word = 1; word >= 0; --word) {
    Distribution("states", 9000 * 13, st, word);
    Distribution("dynamics", n, dy, word);
    Distribution("samplers", 400 * 300, sm, word);
    Distribution("textures", n, ts, word);
  }
}

static void CheckStorage() {
  g_fb_fast = true;
  FrameBuilder b;
  for (int frame = 0; frame < 6; ++frame) {  // warm-up: the tables grow to their frame size
    b.Begin(frame);
    for (uint32_t k = 0; k < 900; ++k) b.Textures(MakeTextures(Rnd() % 700, 0)), b.State(MakeState(Rnd() % 300, 0)), b.Dynamic(MakeDynamic(Rnd() % 400, 0));
  }
  const void* slots = b.texture_sets_.slots.data();
  const size_t cap = b.texture_sets_.slots.capacity(), icap = b.texture_sets_.items.capacity(), kcap = b.texture_sets_.keys.capacity();
  const void* sslots = b.states_.slots.data();
  for (int frame = 6; frame < 46; ++frame) {
    b.Begin(frame);
    for (uint32_t k = 0; k < 900; ++k) b.Textures(MakeTextures(Rnd() % 700, 0)), b.State(MakeState(Rnd() % 300, 0)), b.Dynamic(MakeDynamic(Rnd() % 400, 0));
  }
  g_fail.Check(slots == b.texture_sets_.slots.data() && cap == b.texture_sets_.slots.capacity(), "texture set slots were reallocated after warm-up");
  g_fail.Check(icap == b.texture_sets_.items.capacity() && kcap == b.texture_sets_.keys.capacity(), "texture set items / keys were reallocated after warm-up");
  g_fail.Check(sslots == b.states_.slots.data(), "state slots were reallocated after warm-up");
  g_fail.Check(b.texture_sets_.index.empty() && b.states_.index.empty(), "the legacy multimap holds nodes in fast mode");
  std::printf("[fb] storage: texture-set slots %zu (%.1f KB), kept across 40 frames, no node tables in fast mode\n", cap, double(cap * sizeof(b.texture_sets_.slots[0])) / 1024.0);
  g_fb_fast = false;
}

static void CheckCost() {
  std::vector<TextureSet> pool;
  for (uint32_t i = 0; i < 900; ++i) pool.push_back(MakeTextures(i % 500, 0));
  double ns[2] = {0, 0};
  for (int mode = 0; mode < 2; ++mode) {
    g_fb_fast = mode == 1;
    FrameBuilder b;
    for (int warm = 0; warm < 5; ++warm) {
      b.Begin(warm);
      for (const TextureSet& t : pool) b.Textures(t);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int frames = 400;
    for (int f = 0; f < frames; ++f) {
      b.Begin(f);
      for (const TextureSet& t : pool) b.Textures(t);
    }
    ns[mode] = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / (double(frames) * pool.size());
  }
  g_fb_fast = false;
  std::printf("[fb] cost: Textures() legacy %.0f ns, fast %.0f ns an Add (%.2fx); one TextureSet is %zu bytes\n", ns[0], ns[1], ns[0] / ns[1], sizeof(TextureSet));
}

// T5 (review F1): the shipped PackOnceInto on fake types. A fresh builder, pack_once on: a 4-vec4 vertex block, then a no-fragment Variant{} (block_bytes 0,
// bools_offset 0). Built with -fsanitize=address the unclamped bools copy is a heap overflow; the clamp passes. Also: the bytes equal the stack-block path's.
struct FakeRange {
  uint16_t reg, count, offset;
  uint8_t blend;
};
struct FakeVariant {
  const FakeRange* constants;
  uint16_t constant_count, block_bytes, bools_offset;
};
struct FakeBank {
  uint32_t dwords[256 * 4];
};
struct FakeBools {
  uint32_t words[40];
};

static void CheckPackOnce() {
  FakeBank bank;
  for (uint32_t i = 0; i < 256 * 4; ++i) bank.dwords[i] = 0x01000000u + i;
  FakeBools bools;
  for (uint32_t i = 0; i < 40; ++i) bools.words[i] = 0xB0010000u + i;
  const FakeRange vr[] = {{0, 4, 0, 0}};
  const FakeRange mixed[] = {{8, 2, 16, 0}, {20, 3, 48, 0}};
  // the first: a 4-vec4 vertex block; the second: Variant{}; the others: ranges / bools cut by a short block
  const FakeVariant variants[] = {{vr, 1, 64, 0xFFFF}, {nullptr, 0, 0, 0}, {mixed, 2, 128, 96}, {mixed, 2, 70, 64}, {vr, 1, 0, 0}};
  g_pack_once = true;
  g_fb_fast = false;
  rdr_adapter::ConstantBank real_bank;   // S102 T1b: the same bytes as a real ConstantBank, packed through a BankRef (the binder's branch: CopyBytes), once into a third builder
  std::memcpy(real_bank.dwords, bank.dwords, sizeof real_bank.dwords);
  FrameBuilder viaref;
  viaref.Begin(0);
  FrameBuilder once, legacy;  // fresh: the first reserves are exactly as large as asked, so an overrun leaves the allocation
  once.Begin(0);
  legacy.Begin(0);
  for (int round = 0; round < 3; ++round) {
    for (const FakeVariant& v : variants) {
      const uint32_t a = rdr_adapter::PackOnceInto(v, bank, bools, once);
      // the stack-block path (binder.h's default PackConstants): the unclamped copies into 8 KB, the first `bytes` pushed
      const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(v.block_bytes, 16), 8192);
      static uint8_t block[8192];
      std::memset(block, 0, sizeof block);
      for (uint32_t r = 0; r < v.constant_count; ++r) std::memcpy(block + v.constants[r].offset, bank.dwords + uint32_t(v.constants[r].reg) * 4, uint32_t(v.constants[r].count) * 16);
      if (v.bools_offset != 0xFFFF) std::memcpy(block + v.bools_offset, bools.words, 32);
      const uint32_t b = legacy.PushConstants(block, bytes);
      const uint32_t c = rdr_adapter::PackOnceInto(v, rdr_adapter::BankRef(real_bank), bools, viaref);
      g_fail.Check(c == b, "pack once through a BankRef: offset differs from the stack path", c, b);
      g_fail.Check(a == b, "pack once offset differs from the stack path", a, b);
    }
  }
  g_fail.Check(Equal(once.constants_, legacy.constants_), "pack once bytes differ from the stack path");
  g_fail.Check(Equal(viaref.constants_, legacy.constants_), "pack once through a BankRef: bytes differ from the stack path");
  g_pack_once = false;
  std::printf("[fb] pack once: %zu constant bytes equal the stack-block path over 3 rounds of 5 variants (a no-fragment Variant{} included), clamped to the reserved size\n", once.constants_.size());
}

int main() {
  CheckPackOnce();
  CheckIds();
  CheckDistribution();
  CheckStorage();
  CheckCost();
  if (g_fail.count) {
    std::printf("FB FAIL: %d check(s)\n", g_fail.count);
    return 1;
  }
  std::printf("FB PASS\n");
  return 0;
}
