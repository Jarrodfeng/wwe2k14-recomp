// S103 CPU1 offline proof (docs\research\s103\RHP.md): three invisible CPU cuts of the rdr_host thread, each shown byte-identical to its old path and cheaper, with planted twins (the old
// behaviour) that must fail the "calls per frame" bars. No game, no GPU, no SDK, no game data: every picture / record here is forged by this file.
//   1. MemoView's light-rows key: SceneTables::LightsKey (hashed once per record, kept in the arena memo, reset by Recycle) vs the old per-call FNV-1a. Same hit / miss sequence over a scripted
//      run of recycled tables (same address, same sizes, changed bytes), and the hash count per frame.
//   2. ParseSlotAssetStrings / fast::ParseInt vs the sscanf forms they replace (a corpus + a fuzz), and sscanf calls per frame = 0.
//   3. YuvaToRgba (table form) vs YuvaToRgbaOld (the per-pixel formula): EVERY (Y, Cb, Cr) triple, odd sizes, pitch padding, an alpha plane; and the screen feed's conversions per new picture.
// Build: pwsh -NoProfile -File tools\scene\build_cpu1_unit.ps1   Run: out\tools\cpu1_unit.exe   (exit 0 = all pass; prints the before / after numbers)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rf_c.h"
#include "../../src/renderer_scene/screen_feed_yuv.h"
#include "../../src/renderer_scene/stage_census.h"
#include "../../src/renderer_scene/yuv_rgba.h"
#include "../../src/scene_live/arena_apply.h"

using namespace rdr_scene;
using rdr_scene::rf::c::YuvPlanes;
using rdr_scene::live::arena::ParseSlotAssetStrings;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* name, const std::string& detail = std::string()) {
  if (ok) ++g_pass;
  else ++g_fail;
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
}
static double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static std::string N(double v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

// ------------------------------------------------------------------------------------------------------------------------------ 1. the light key
static uint64_t OldFnv(const SceneTables& t) {   // the removed per-call hash, verbatim
  uint64_t lh = 1469598103934665603ull;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(t.lights.data());
  for (size_t i = 0; i < t.lights.size() * sizeof(Light); ++i) lh = (lh ^ p[i]) * 1099511628211ull;
  return lh;
}
struct MemoSim {   // the MemoView predicate, one thread's single entry
  const SceneTables* t = nullptr;
  uint64_t frame = ~0ull, hash = 0;
  size_t objects = 0, lights = 0, strings = 0;
  uint64_t builds = 0;
  bool Call(const SceneTables& tb, uint64_t key) {   // true = hit
    if (t == &tb && frame == tb.header.guest_frame && objects == tb.objects.size() && lights == tb.lights.size() && strings == tb.strings.size() && hash == key) return true;
    hash = key;
    t = &tb, frame = tb.header.guest_frame, objects = tb.objects.size(), lights = tb.lights.size(), strings = tb.strings.size();
    ++builds;
    return false;
  }
};
static Light MakeLight(uint32_t set, uint32_t seed) {
  Light l;
  l.set = set, l.kind = uint8_t(seed & 3), l.slot = uint8_t(seed % 7), l.pad = uint16_t(seed * 3);
  for (int k = 0; k < 3; ++k) l.dir_or_pos[k] = float((seed + k) % 17) * 0.25f, l.color[k] = float((seed * 5 + k) % 11) * 0.1f;
  l.range = float(seed % 5);
  return l;
}
static void FillTable(SceneTables& t, uint64_t frame, uint32_t n_lights, uint32_t content_seed) {
  t.header.guest_frame = frame;
  t.lights.clear();
  for (uint32_t i = 0; i < n_lights; ++i) t.lights.push_back(MakeLight(i / 5, content_seed + i));
  t.objects.resize(30), t.strings = "a";
}

static void TestLightKey() {
  // 1a. the equivalence classes: every single-byte change of the rows changes the old hash and the new key (a corpus: 1..40 rows, each byte flipped)
  bool all_differ = true, same_stable = true;
  for (uint32_t n : {1u, 2u, 3u, 7u, 40u}) {
    SceneTables t;
    FillTable(t, 1, n, 100);
    const uint64_t k0 = t.LightsKey(true), o0 = OldFnv(t);
    const size_t bytes = n * sizeof(Light);
    for (size_t b = 0; b < bytes; ++b) {
      uint8_t* p = reinterpret_cast<uint8_t*>(t.lights.data()) + b;
      const uint8_t keep = *p;
      *p ^= 0x5A;
      if (t.LightsKey(true) == k0 || OldFnv(t) == o0) all_differ = false;
      *p = keep;
    }
    if (t.LightsKey(true) != k0 || OldFnv(t) != o0) same_stable = false;
  }
  Check(all_differ, "lightkey.every_byte_flip_changes_old_and_new");
  Check(same_stable, "lightkey.restore_gives_the_same_key");

  // 1b. the memo: a scripted run of recycled tables (same address, same sizes, content changed / unchanged / frame changed), 25 MemoView calls per record: hit / miss sequences equal
  SceneTables tab;   // ONE table, refilled in place with Recycle (the swap-once pool shape)
  MemoSim old_m, new_m, plant_m;
  std::mt19937 rng(7);
  const int kFrames = 400, kCalls = 25;
  bool same_seq = true, plant_seq = true;
  StageCensus& cen = StageCensusRef();
  uint64_t old_hashes = 0;
  const uint64_t h0 = cen.lights_hashed.load();
  uint64_t plant_hashes = 0;
  uint32_t seed = 1;
  for (int f = 0; f < kFrames; ++f) {
    tab.Recycle();
    const int mode = int(rng() % 4);   // 0: same frame number, same bytes as before; 1: same frame, same size, changed bytes (the recycled-table hazard); 2: new frame; 3: new size
    static uint64_t frame_no = 10;
    static uint32_t n = 12;
    if (mode == 2) ++frame_no;
    if (mode == 3) n = 8 + uint32_t(rng() % 9), ++frame_no;
    if (mode == 1) seed += 3;
    FillTable(tab, frame_no, n, seed);
    for (int c = 0; c < kCalls; ++c) {
      ++old_hashes;
      const bool a = old_m.Call(tab, OldFnv(tab));
      const bool b = new_m.Call(tab, tab.LightsKey(false));
      ++plant_hashes;
      const bool p = plant_m.Call(tab, tab.LightsKey(true));
      if (a != b) same_seq = false;
      if (a != p) plant_seq = false;
    }
  }
  const uint64_t new_hashes = cen.lights_hashed.load() - h0 - plant_hashes;   // (the plant arm hashed every call; subtract it)
  Check(same_seq, "lightkey.same_hit_miss_sequence_as_the_old_per_call_fnv");
  Check(old_m.builds == new_m.builds, "lightkey.same_record_reads", std::to_string(old_m.builds) + " vs " + std::to_string(new_m.builds));
  Check(plant_seq, "lightkey.plant_rehash_is_functionally_identical (it only costs more)");
  // the bar: hashes per frame (record). Old = kCalls per record; new <= 1 per record.
  const double per_frame_new = double(new_hashes) / kFrames, per_frame_old = double(old_hashes) / kFrames, per_frame_plant = double(plant_hashes) / kFrames;
  Check(per_frame_new <= 1.0 + 1e-9, "lightkey.bar_hashes_per_frame_at_most_1", "new " + N(per_frame_new) + ", old " + N(per_frame_old));
  Check(!(per_frame_plant <= 1.0), "lightkey.plant_rehash_fails_the_bar", "plant " + N(per_frame_plant));

  // 1c. time: 40 light rows (a typical arena record), 25 calls: per-call cost old vs new-cached
  SceneTables big;
  FillTable(big, 5, 40, 9);
  const int reps = 200000;
  volatile uint64_t sink = 0;
  double t0 = NowMs();
  for (int i = 0; i < reps; ++i) sink = sink + OldFnv(big);
  const double t_old = NowMs() - t0;
  big.arena_memo.ResetLights();
  t0 = NowMs();
  for (int i = 0; i < reps; ++i) sink = sink + big.LightsKey(false);
  const double t_new = NowMs() - t0;
  std::printf("INFO lightkey cost per call (40 rows = %zu bytes): old %.1f ns, new (cached) %.1f ns; first hash of a record: %.1f ns (old 1 call %.1f ns)\n", 40 * sizeof(Light), t_old * 1e6 / reps, t_new * 1e6 / reps, [&] {
    double a = NowMs();
    for (int i = 0; i < reps; ++i) big.arena_memo.ResetLights(), sink = sink + big.LightsKey(false);
    return (NowMs() - a) * 1e6 / reps;
  }(), t_old * 1e6 / reps);
  Check(t_new < t_old, "lightkey.cached_call_is_cheaper_than_the_old_call");

  // 1d. a copy / a move-assigned table starts with no key (ParseRecord does `out = SceneTables()`)
  SceneTables c1;
  FillTable(c1, 3, 6, 1);
  (void)c1.LightsKey(false);
  SceneTables c2 = c1;
  Check(c2.arena_memo.lights_key.load() == 0, "lightkey.copy_starts_empty");
  c1 = SceneTables();
  Check(c1.arena_memo.lights_key.load() == 0, "lightkey.assign_resets");
}

// ------------------------------------------------------------------------------------------------------------------------------ 2. parsing
static int g_sscanf_calls = 0;
static bool OldParseSlotAsset(std::string_view pac_in, std::string_view entry_in, int& arena, int& slot) {   // the removed body, verbatim (strings instead of the table)
  std::string pac(pac_in);
  std::replace(pac.begin(), pac.end(), '/', '\\');
  int id = 0;
  ++g_sscanf_calls;
  if (std::sscanf(pac.c_str(), "pac\\bg\\bg%d.pac", &id) != 1) return false;
  const std::string entry(entry_in);
  int k = 0;
  ++g_sscanf_calls;
  if (std::sscanf(entry.c_str(), "slot %d", &k) != 1) return false;
  arena = id, slot = k;
  return true;
}
static bool OldCrowdLike(std::string_view entry) {
  int num = 0, ty = 0;
  return std::sscanf(std::string(entry).c_str(), "card:%d", &ty) == 1 || (std::sscanf(std::string(entry).c_str(), "%d", &num) == 1 && num >= 20000 && num < 20200);
}
static bool NewCrowdLike(std::string_view entry) {
  int num = 0, ty = 0;
  return (entry.size() >= 5 && entry.compare(0, 5, "card:") == 0 && fast::ScanInt(entry, 5, ty)) || (fast::ScanInt(entry, 0, num) && num >= 20000 && num < 20200);
}
static bool OldActorSlot(std::string_view sid, unsigned& n) {
  return sid.size() >= 11 && sid.compare(0, 11, "actor:slot:") == 0 && std::sscanf(std::string(sid.substr(11)).c_str(), "%u", &n) == 1;
}
static bool NewActorSlot(std::string_view sid, unsigned& n) {
  uint32_t v = 0;
  const bool ok = sid.size() >= 11 && sid.compare(0, 11, "actor:slot:") == 0 && fast::ScanUInt(sid, 11, v);
  if (ok) n = v;
  return ok;
}

static void TestParse() {
  const char* pacs[] = {"pac\\bg\\bg04.pac", "pac/bg/bg04.pac", "pac\\bg\\bg4.pac", "pac\\bg\\bg12", "pac\\bg\\bg12xyz", "pac\\bg\\bg", "pac\\bg\\bgx", "pac\\bg\\bg-3.pac", "pac\\bg\\bg+7.pac", "pac\\bg\\bg 9.pac", "pac\\bg\\bg\t9.pac",
                        "pac\\bg\\bg007.pac", "pac\\bg\\bg0x10.pac", "pac\\bg\\bg12.pac.bak", "PAC\\bg\\bg04.pac", "pac\\bg\\bf04.pac", "pac/bg\\bg04.pac", "pac\\chr\\chr01.pac", "", "pac", "pac\\bg\\b",
                        "pac\\bg\\bg2147483647.pac", "pac\\bg\\bg1.5.pac", "pac\\bg\\bg00.pac", "pac\\bg\\bg99.pac",
                        "pac\\bg\\bg2147483648.pac", "pac\\bg\\bg-2147483648.pac", "pac\\bg\\bg4294967296.pac", "pac\\bg\\bg4294987296.pac", "pac\\bg\\bg99999999999.pac"};   // (the last five: %d overflow)
  const char* entries[] = {"slot 0", "slot 12", "slot  3", "slot\t4", "slot", "slot x", "slots 4", "slot-5", "slot -5", "slot +6", "slot 7x", "Slot 1", "", "slot 0 ", "slot 000", "model", "slot 99999", "sl", "slot 2147483647", "slot 2147483648", "slot -2147483648", "slot 4294967296", "slot 4294987296", "slot 99999999999"};
  size_t cases = 0;
  bool same = true;
  std::string first_diff;
  for (const char* p : pacs)
    for (const char* e : entries) {
      int a0 = -7, s0 = -7, a1 = -7, s1 = -7;
      const bool o = OldParseSlotAsset(p, e, a0, s0), n = ParseSlotAssetStrings(p, e, a1, s1);
      ++cases;
      if (o != n || (o && (a0 != a1 || s0 != s1))) {
        same = false;
        if (first_diff.empty()) first_diff = std::string("pac '") + p + "' entry '" + e + "'";
      }
    }
  Check(same, "parse.slot_asset_equals_sscanf_on_the_corpus", std::to_string(cases) + " cases" + (first_diff.empty() ? "" : "; first difference " + first_diff));

  {  // ScanInt / ScanUInt against the real sscanf("%d") / ("%u"), overflow included (S103 CPU1 review item 3)
    const char* nums[] = {"0", "7", "2147483647", "2147483648", "-2147483648", "-2147483649", "4294967295", "4294967296", "4294987296", "99999999999", "-99999999999", "-4294967296", "-4294967297", "+5", "  12", "-0", "007"};
    bool ok = true;
    for (const char* s : nums) {
      int d = -7, d2 = -8;
      unsigned u = 7, u2 = 8;
      std::sscanf(s, "%d", &d), std::sscanf(s, "%u", &u);
      fast::ScanInt(s, 0, d2), fast::ScanUInt(s, 0, u2);
      if (d != d2 || u != u2) ok = false, std::printf("  scan differs '%s': %%d %d vs %d, %%u %u vs %u\n", s, d, d2, u, u2);
    }
    Check(ok, "parse.scan_int_uint_equal_ucrt_sscanf_with_overflow", std::to_string(sizeof nums / sizeof *nums) + " values");
  }

  // fuzz: record-shaped strings with random digits / junk
  std::mt19937 rng(11);
  const char alpha[] = "pac\\/bg0123456789 +-.xslot";
  bool fuzz_same = true;
  size_t fuzz_cases = 0;
  for (int i = 0; i < 200000 && fuzz_same; ++i) {
    std::string pac = (rng() & 1) ? "pac\\bg\\bg" : "", entry = (rng() & 1) ? "slot " : "";
    for (int k = int(rng() % 6); k > 0; --k) pac += alpha[rng() % (sizeof alpha - 1)];
    for (int k = int(rng() % 5); k > 0; --k) entry += alpha[rng() % (sizeof alpha - 1)];
    int a0 = -7, s0 = -7, a1 = -7, s1 = -7;
    const bool o = OldParseSlotAsset(pac, entry, a0, s0), n = ParseSlotAssetStrings(pac, entry, a1, s1);
    ++fuzz_cases;
    if (o != n || (o && (a0 != a1 || s0 != s1))) fuzz_same = false, std::printf("  fuzz difference: pac '%s' entry '%s' old %d (%d,%d) new %d (%d,%d)\n", pac.c_str(), entry.c_str(), int(o), a0, s0, int(n), a1, s1);
  }
  Check(fuzz_same, "parse.slot_asset_equals_sscanf_on_a_fuzz", std::to_string(fuzz_cases) + " cases");

  const char* crowd[] = {"card:12", "card:", "card:x", "card: 5", "card:-1", "20000", "20199", "20200", "19999", " 20005", "+20005", "20005x", "x20005", "", "card", "cardd:3", "20000.5", "0", "-20000", "slot 3",
                         "2147483648", "-2147483648", "4294967296", "4294987296", "99999999999", "card:2147483648", "card:-2147483648", "card:4294967296", "card:4294987296", "card:99999999999"};
  bool crowd_same = true;
  for (const char* e : crowd)
    if (OldCrowdLike(e) != NewCrowdLike(e)) crowd_same = false, std::printf("  crowd_like differs: '%s'\n", e);
  for (int v = 19990; v < 20210 && crowd_same; ++v) {
    const std::string s = std::to_string(v);
    if (OldCrowdLike(s) != NewCrowdLike(s)) crowd_same = false;
  }
  Check(crowd_same, "parse.crowd_like_equals_sscanf");

  const char* actors[] = {"actor:slot:0", "actor:slot:12", "actor:slot:", "actor:slot:x", "actor:slot: 3", "actor:slot:-1", "actor:slot:+4", "actor:slot:7y", "actor:slot:4294967295", "actor:slot:42", "actor:slot:2147483648", "actor:slot:-2147483648", "actor:slot:4294967296", "actor:slot:4294987296", "actor:slot:99999999999","actor:sl", "actor:slot", "actor:model:3", ""};
  bool actor_same = true;
  for (const char* a : actors) {
    unsigned n0 = 99, n1 = 99;
    const bool o = OldActorSlot(a, n0), n = NewActorSlot(a, n1);
    if (o != n || (o && n0 != n1)) actor_same = false, std::printf("  actor slot differs: '%s' old %d %u new %d %u\n", a, int(o), n0, int(n), n1);
  }
  Check(actor_same, "parse.actor_slot_equals_sscanf");

  // the bar: sscanf calls per frame. A record has ~1000 asset rows (the arena slots + wrestlers + crowd entries in the Bryan entrance window; here 600 slot assets). ReadArenaRecord parses each once per record read.
  const int assets = 600, reps = 2000;
  std::vector<std::string> pacv, entv;
  for (int i = 0; i < assets; ++i) pacv.push_back("pac\\bg\\bg04.pac"), entv.push_back("slot " + std::to_string(i % 60));
  g_sscanf_calls = 0;
  int a = 0, s = 0, sink = 0;
  double t0 = NowMs();
  for (int r = 0; r < reps; ++r)
    for (int i = 0; i < assets; ++i) sink += OldParseSlotAsset(pacv[size_t(i)], entv[size_t(i)], a, s) ? a + s : 0;
  const double t_old = NowMs() - t0;
  const int old_calls = g_sscanf_calls;
  g_sscanf_calls = 0;
  t0 = NowMs();
  for (int r = 0; r < reps; ++r)
    for (int i = 0; i < assets; ++i) sink += ParseSlotAssetStrings(pacv[size_t(i)], entv[size_t(i)], a, s) ? a + s : 0;
  const double t_new = NowMs() - t0;
  Check(g_sscanf_calls == 0 && old_calls == 2 * assets * reps, "parse.bar_sscanf_calls_per_frame_0", "new " + std::to_string(g_sscanf_calls / reps) + " per record, old " + std::to_string(old_calls / reps) + " per record (plant)");
  std::printf("INFO parse cost per record of %d slot assets: old %.3f ms, new %.3f ms (sink %d)\n", assets, t_old / reps, t_new / reps, sink);
  Check(t_new < t_old, "parse.faster_than_sscanf");
}

// ------------------------------------------------------------------------------------------------------------------------------ 3. YUV
static bool SameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0; }

static void TestYuv() {
  // 3a. EVERY (Y, Cb, Cr): image 256 wide (Y = x), 4096 rows; chroma row r (= pixel rows 2r, 2r+1) holds one constant (Cb, Cr) pair; 32 images cover all 65536 pairs.
  bool exhaustive = true;
  size_t triples = 0;
  std::vector<uint8_t> yp(256 * 4096), cbp(128 * 2048), crp(128 * 2048), o_old, o_new;
  for (size_t y = 0; y < 4096; ++y)
    for (size_t x = 0; x < 256; ++x) yp[y * 256 + x] = uint8_t(x);
  for (int img = 0; img < 32 && exhaustive; ++img) {
    for (int r = 0; r < 2048; ++r) {
      const uint32_t pair = uint32_t(img) * 2048u + uint32_t(r);   // 0..65535
      std::memset(&cbp[size_t(r) * 128], int(pair >> 8), 128);
      std::memset(&crp[size_t(r) * 128], int(pair & 255), 128);
    }
    YuvPlanes p;
    p.w = 256, p.h = 4096, p.y = yp.data(), p.y_pitch = 256, p.cb = cbp.data(), p.cb_pitch = 128, p.cr = crp.data(), p.cr_pitch = 128;
    const bool a = rf::c::YuvaToRgbaOld(p, o_old), b = rf::c::YuvaToRgba(p, o_new);
    triples += size_t(256) * 2048;   // 2048 chroma pairs x 256 Y values per image (each pair is also shown on a second pixel row)
    if (!a || !b || !SameBytes(o_old, o_new)) exhaustive = false;
  }
#if defined(__AVX2__)
  const char* build = "AVX2 path";
#else
  const char* build = "scalar path";
#endif
  Check(exhaustive, "yuv.every_Y_Cb_Cr_triple_byte_identical", std::to_string(triples) + " triples (all 2^24), " + build);

  // 3b. random pictures: odd sizes, padded pitches, with and without alpha
  std::mt19937 rng(3);
  bool rnd_same = true;
  size_t pics = 0;
  for (int i = 0; i < 300; ++i) {
    const uint32_t w = 1 + rng() % 70, h = 1 + rng() % 40, ypad = rng() % 5, cpad = rng() % 5;
    const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    std::vector<uint8_t> Y((w + ypad) * h), B((cw + cpad) * ch), R((cw + cpad) * ch), A((w + ypad) * h);
    for (auto* v : {&Y, &B, &R, &A})
      for (uint8_t& c : *v) c = uint8_t(rng());
    YuvPlanes p;
    p.w = w, p.h = h, p.y = Y.data(), p.y_pitch = w + ypad, p.cb = B.data(), p.cb_pitch = cw + cpad, p.cr = R.data(), p.cr_pitch = cw + cpad;
    if (i & 1) p.a = A.data(), p.a_pitch = w + ypad;
    ++pics;
    if (!rf::c::YuvaToRgbaOld(p, o_old) || !rf::c::YuvaToRgba(p, o_new) || !SameBytes(o_old, o_new)) rnd_same = false;
  }
  Check(rnd_same, "yuv.random_pictures_byte_identical", std::to_string(pics) + " pictures (odd sizes, padded pitches, alpha on every other)");
  {  // refusals are the same
    YuvPlanes bad;
    std::vector<uint8_t> o;
    Check(!rf::c::YuvaToRgba(bad, o) && !rf::c::YuvaToRgbaOld(bad, o), "yuv.refuses_missing_planes_like_before");
  }

  // 3c. time: the Bryan screen picture 1280x400 (chroma 640x200)
  const uint32_t W = 1280, H = 400;
  std::vector<uint8_t> Y(W * H), B(W / 2 * H / 2), R(W / 2 * H / 2);
  for (auto* v : {&Y, &B, &R})
    for (uint8_t& c : *v) c = uint8_t(rng());
  YuvPlanes p;
  p.w = W, p.h = H, p.y = Y.data(), p.y_pitch = W, p.cb = B.data(), p.cb_pitch = W / 2, p.cr = R.data(), p.cr_pitch = W / 2;
  const int reps = 200;
  rf::c::YuvaToRgba(p, o_new), rf::c::YuvaToRgbaOld(p, o_old);   // (warm up the allocation and the table)
  double t0 = NowMs();
  for (int i = 0; i < reps; ++i) rf::c::YuvaToRgbaOld(p, o_old);
  const double t_old = (NowMs() - t0) / reps;
  t0 = NowMs();
  for (int i = 0; i < reps; ++i) rf::c::YuvaToRgba(p, o_new);
  const double t_new = (NowMs() - t0) / reps;
  std::printf("INFO yuv 1280x400 convert: old %.3f ms, new %.3f ms (%.1fx)\n", t_old, t_new, t_old / t_new);
  Check(t_new < t_old, "yuv.conversion_is_cheaper");
#if defined(__AVX2__)
  // the bar (this build has the AVX2 path: the x86-64-v3 release exe): the conversion of one decoded picture costs under a quarter of the old formula. The old function IS the plant: it fails this.
  Check(t_new * 4 < t_old, "yuv.bar_conversion_under_a_quarter_of_the_old_cost", "new " + N(t_new) + " ms, old (plant) " + N(t_old) + " ms");
  Check(!(t_old * 4 < t_old), "yuv.plant_old_formula_fails_the_bar");
#endif
}

// the feed's conversions per decoded picture
class FakeSource final : public rf::c::PlaneSource {
 public:
  std::vector<uint8_t> y, cb, cr;
  uint32_t w = 64, h = 32;
  uint64_t id = 0;
  bool Planes(uint32_t, uint32_t, rf::c::YuvPlanes& out, uint64_t& content_id) override {
    out = rf::c::YuvPlanes{};
    out.w = w, out.h = h, out.y = y.data(), out.y_pitch = w, out.cb = cb.data(), out.cb_pitch = w / 2, out.cr = cr.data(), out.cr_pitch = w / 2;
    content_id = id;
    return true;
  }
  void Paint(uint8_t seed) {
    y.assign(size_t(w) * h, 0), cb.assign(size_t(w / 2) * (h / 2), 0), cr.assign(size_t(w / 2) * (h / 2), 0);
    for (size_t i = 0; i < y.size(); ++i) y[i] = uint8_t(seed + i * 7);
    for (size_t i = 0; i < cb.size(); ++i) cb[i] = uint8_t(seed * 3 + i), cr[i] = uint8_t(seed * 5 + i * 2);
  }
};

static void TestFeed() {
  // The feed already converts once per decoded picture (its cache is keyed by the decoder's content id; the row serial only asks when the id changed): 150 decoder flushes (a new id each time),
  // each shown on 3 screens in the frame (the arena has several screens of one movie): converted must equal the flushes, shared 2 per flush, the bytes equal the old formula's.
  const int flushes = 150, screens = 3;
  FakeSource src;
  rf::c::YuvScreenFeed feed(&src);
  bool bytes_same = true;
  std::vector<uint8_t> ref;
  for (int f = 0; f < flushes; ++f) {
    src.Paint(uint8_t(f + 1)), src.id = uint64_t(f + 1);
    for (int s = 0; s < screens; ++s) {
      uint32_t w = 0, h = 0;
      const uint8_t* rgba = nullptr;
      uint64_t cid = 0;
      if (!feed.Picture(801 + uint32_t(s), uint32_t(src.id), w, h, rgba, cid) || cid != src.id) { bytes_same = false; continue; }
      rf::c::YuvPlanes pl;
      uint64_t id2 = 0;
      src.Planes(0, 0, pl, id2);
      rf::c::YuvaToRgbaOld(pl, ref);
      if (ref.size() != size_t(w) * h * 4 || std::memcmp(ref.data(), rgba, ref.size()) != 0) bytes_same = false;
    }
  }
  Check(bytes_same, "feed.pictures_equal_the_old_formula_through_the_feed", std::to_string(flushes * screens) + " pictures");
  Check(feed.converted() == uint64_t(flushes) && feed.shared() == uint64_t(flushes * (screens - 1)), "feed.bar_one_conversion_per_decoded_picture",
        "converted " + std::to_string((unsigned long long)feed.converted()) + ", shared " + std::to_string((unsigned long long)feed.shared()) + " (RHP.md's 2.24 ms = this one conversion a presented frame, not a cache miss)");
}

static void TestCensus() {
  StageWindow a, b;
  b.build_ns = 18'000'000ull * 10, b.record_ns = 10'000'000ull * 10, b.render_ns = 3'500'000ull * 10, b.memo_calls = 300, b.memo_builds = 10, b.lights_hashed = 10;
  const std::string line = StageLine(b, a, 10, rf::c::YuvPathName());
  std::printf("INFO %s\n", line.c_str());
  Check(line.find("build 18.00 ms") != std::string::npos && line.find("record 10.00 ms") != std::string::npos && line.find("render+present 3.50 ms") != std::string::npos && line.find("memo calls 30.0") != std::string::npos &&
            line.find(std::string("yuv ") + rf::c::YuvPathName()) != std::string::npos,
        "census.line_format");
  Check(StageLine(b, a, 0).find("no frame presented") != std::string::npos, "census.no_frames");
  {
    StageCensus& c = StageCensusRef();
    const uint64_t n0 = c.build_n.load();
    { StageTimer t(c.build_ns, c.build_n); }
    Check(c.build_n.load() == n0 + 1, "census.timer_counts_one_call");
  }
}

int main() {
  TestLightKey();
  TestParse();
  TestYuv();
  TestFeed();
  TestCensus();
  std::printf("\n[cpu1_unit] %d passed, %d failed; sizeof(SceneTables) %zu\n", g_pass, g_fail, sizeof(SceneTables));
  return g_fail ? 1 : 0;
}
