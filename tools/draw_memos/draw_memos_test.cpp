// S102 lane T14: the offline proof of the three per-draw memos (wwe2k14_rdr_note_memo, wwe2k14_rdr_shader_memo, wwe2k14_rdr_pin_hash), 0 game runs.
// Built by tools\draw_memos\draw_memos_check.py against the SHIPPED headers src\renderer_adapter\{draw_memos,thin_draw,reader_thin}.h with -DT14_PLANTS.
//   draw_memos_test.exe              every check on the honest build: exit 0 or 1, one PASS / FAIL line each
//   draw_memos_test.exe plant N      the same checks with planted fault N (1..6): must exit 1 (draw_memos.h T14_PLANT)
// What is compared is what the reader records: the census cells and the "first seen" lines (notes), the shader info each draw is given (shaders), and the
// answers + ordinals the reader's thin-draw query gives for a draw stream (pin set). Each arm runs the plain path (switch off), the memo path (on) and
// a run that flips the switch every block, and requires the same results. Each arm prints ns per operation off and on.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/draw_memos.h"
#include "../../src/renderer_adapter/reader_thin.h"

using namespace rdr_adapter;

namespace {

int g_failed = 0, g_checks = 0;
bool g_plant_run = false;
void Check(bool ok, const char* what, const std::string& detail = "") {
  ++g_checks;
  if (!ok) ++g_failed;
  std::printf("%s  %s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
}
double NowNs() { return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- 1. notes -------------------------------------------------------------------------------------------------------------------------------
struct Census {   // what reader.h's Census + NoteCounted keep
  std::map<std::string, uint64_t> window, seen;
  std::vector<std::string> first_seen;   // the "[rdr][census] first seen" lines, in order
  draw_memos::NoteMemo memo;
  uint64_t guard = 0;
  void Count(const char* name, uint64_t n, bool on) {   // = NoteCounted
    draw_memos::CountNote(memo, window, seen, name, n, on, [&] { first_seen.push_back(std::string(name) + "@" + std::to_string(guard)); });   // the body reader.h's NoteCounted calls
  }
  void Clear(std::vector<std::string>& dump) {   // = CensusMaybe's window clear
    std::string d;
    for (const auto& [k, v] : window) d += k + "=" + std::to_string(v) + ";";
    dump.push_back(d);
    memo.Invalidate();
    window.clear();
  }
};

static const char* kLits[] = {"clip_space_not_d3d", "tiling_active", "slot_gpu_copy_unknown", "viewport_z_inverted", "line_draw", "texture_header_changed",
                              "fetch_sign_biased", "slot_without_object", "vs_variant2_needed", "texture_slot_read_by_vs_and_ps", "copy_dest_8bit_signed_fetch",
                              "a_name_that_is_longer_than_forty_seven_characters_for_sure_yes", "x", "y", "slot_null_gpu_keeps_previous", "draw_no_vertex_shader",
                              "indexed_draw_no_index_buffer", "point_draw_dropped", "prim_unknown_7", "border_colour_ycbcr", "fetch_word4_exp_adjust_nonzero",
                              "slot_invalid_type_read_as_texture", "target16_read_signed", "signed_fetch_no_signed_host_format"};
constexpr size_t kNLits = sizeof(kLits) / sizeof(kLits[0]);

struct NoteRun {
  std::vector<std::string> dumps;
  std::map<std::string, uint64_t> seen;
  std::vector<std::string> first;
};
// mode 0 = always plain, 1 = always memo, 2 = flip every block
NoteRun RunNotes(int mode, uint32_t seed, uint64_t ops, double* ns_per_op = nullptr) {
  std::mt19937 rng(seed);
  Census c;
  NoteRun out;
  char buf[64];
  const double t0 = NowNs();
  for (const char* fresh : {"zero_first_a", "zero_first_b", "zero_first_c"}) {   // a name whose first count is 0: nothing is added, "first seen" is logged again at the next count
    c.Count(fresh, 0, mode != 0);
    c.Count(fresh, 0, mode != 0);
    c.Count(fresh, 2, mode != 0);
  }
  for (uint64_t i = 0; i < ops; ++i) {
    c.guard = i;
    const bool on = mode == 1 || (mode == 2 && ((i / 97) & 1));
    const uint32_t r = rng() % 100;
    uint64_t n = 1 + rng() % 3;
    if (rng() % 500 == 0) n = 0;   // a zero count: plain path adds nothing and logs "first seen" again while seen is 0
    if (r < 62) {
      c.Count(kLits[(rng() % 6)], n, on);   // the hot names (like viewport_z_inverted on every draw)
    } else if (r < 85) {
      c.Count(kLits[rng() % kNLits], n, on);
    } else if (r < 93) {   // a name built in a buffer that is reused with other texts (the same pointer, other characters)
      std::snprintf(buf, sizeof(buf), (rng() & 1) ? "blend_factor_unknown_%u" : "blend_op_unknown_%u", unsigned(rng() % 5));
      c.Count(buf, n, on);
    } else {   // a temporary std::string's c_str (the Note(name, n) overload)
      c.Count((std::string("prim_outside_census_") + std::to_string(rng() % 7)).c_str(), n, on);
    }
    if (i % 600 == 599) c.Clear(out.dumps);
  }
  if (ns_per_op) *ns_per_op = (NowNs() - t0) / double(ops);
  c.Clear(out.dumps);
  out.seen = c.seen;
  out.first = c.first_seen;
  return out;
}

void NoteArms() {
  const NoteRun a = RunNotes(0, 7, 300000), b = RunNotes(1, 7, 300000), f = RunNotes(2, 7, 300000);
  Check(a.dumps == b.dumps && a.seen == b.seen, "notes: census windows and seen set equal, switch off vs on", "(" + std::to_string(a.dumps.size()) + " windows)");
  Check(a.first == b.first, "notes: the 'first seen' lines equal, in order", "(" + std::to_string(a.first.size()) + " names)");
  Check(a.dumps == f.dumps && a.seen == f.seen && a.first == f.first, "notes: equal when the switch flips every block");
  // speed: only hot names, the shape of a draw (three notes), many ops
  double off = 0, on = 0;
  for (int k = 0; k < 3; ++k) {
    double x, y;
    (void)RunNotes(0, 11, 400000, &x);
    (void)RunNotes(1, 11, 400000, &y);
    off = k ? std::min(off, x) : x;
    on = k ? std::min(on, y) : y;
  }
  std::printf("INFO  notes: %.1f ns per note off, %.1f ns on (mixed stream, 62%% hot names; includes the stream's own rng and the buffer / temp-string notes, which cost the same on both)\n", off, on);
  // the hot path alone
  {
    Census c;
    for (int on_i = 0; on_i < 2; ++on_i) {
      const bool onb = on_i == 1;
      const double t0 = NowNs();
      for (uint64_t i = 0; i < 2000000; ++i) c.Count(kLits[i % 3], 1, onb);
      const double ns = (NowNs() - t0) / 2000000.0;
      std::printf("INFO  notes: hot literal only: %.1f ns per note %s\n", ns, onb ? "on" : "off");
      if (onb && !g_plant_run) Check(ns < 40.0, "notes: a hot literal costs under 40 ns on the memo path", std::to_string(ns) + " ns");
    }
  }
  if (!g_plant_run) Check(b.dumps.size() > 5, "notes: the arm covered several census windows");
  {   // F2: string-backed names (a buffer, a temporary's c_str: each a new pointer or new text) must not be slower with the memo on
    Census c;
    double ns[2] = {1e9, 1e9};
    for (int rep_i = 0; rep_i < 5; ++rep_i)
      for (int on_i = 0; on_i < 2; ++on_i) {
        const double t0 = NowNs();
        for (uint64_t i = 0; i < 300000; ++i) {
          const std::string tmp = "prim_outside_census_" + std::to_string(i % 11);
          c.Count(tmp.c_str(), 1, on_i == 1);
        }
        ns[on_i] = std::min(ns[on_i], (NowNs() - t0) / 300000.0);
      }
    std::printf("INFO  notes: string-backed names: %.1f ns off, %.1f ns on (includes building the string)\n", ns[0], ns[1]);
    if (!g_plant_run) Check(ns[1] <= ns[0] * 1.10, "notes: string-backed names are not slower with the memo on", std::to_string(ns[1]) + " vs " + std::to_string(ns[0]) + " ns");
  }
}

// ---- 2. shaders -----------------------------------------------------------------------------------------------------------------------------
struct Info {
  uint32_t id;
};
using Table = draw_memos::ShaderTable<Info>;
struct Guest {   // the guest's shader objects and microcode
  struct Shader {
    uint32_t object, addr;
    std::vector<uint8_t> code;
  };
  std::vector<Shader> shaders;
};
struct World {   // the reader's g_vs: key = (object, decl, variant bit, strides, hash of the microcode); infos are never moved or erased
  std::map<std::string, std::unique_ptr<Info>> map;
  uint32_t next = 1;
  uint64_t creates = 0;
  const Info* Resolve(const Table::Key& k, const uint8_t* src) {
    std::string key(reinterpret_cast<const char*>(&k.object), 4);
    key.append(reinterpret_cast<const char*>(&k.decl), 4);
    key.push_back(char(k.seven_c));
    key.append(reinterpret_cast<const char*>(k.strides), 24);
    const uint64_t h = XXH3_64bits(src, k.bytes);   // (what cap::Xxh3 is)
    key.append(reinterpret_cast<const char*>(&h), 8);
    auto it = map.find(key);
    if (it == map.end()) {
      it = map.emplace(key, std::make_unique<Info>(Info{next++})).first;
      ++creates;
    }
    return it->second.get();
  }
};
struct ShaderRun {
  std::vector<uint32_t> ids;
  uint64_t creates = 0, hits = 0, misses = 0;
};
ShaderRun RunShaders(int mode, uint32_t seed, uint32_t nshaders, uint64_t draws, bool mutate, double* ns_per_draw = nullptr) {
  std::mt19937 rng(seed);
  Guest g;
  uint32_t addr = 0x10000;
  for (uint32_t i = 0; i < nshaders; ++i) {
    Guest::Shader s;
    s.object = 0x40000000u + i * 0x180;
    s.addr = addr;
    s.code.resize(i % 11 == 0 ? 9000 : (i % 7 == 0 ? 6000 : 48 + (rng() % 90) * 8));   // a few past the table cap (8192), some big under it
    for (auto& b : s.code) b = uint8_t(rng());
    addr += uint32_t(s.code.size()) + 64;
    g.shaders.push_back(std::move(s));
  }
  World w;
  Table table;
  ShaderRun out;
  const double t0 = NowNs();
  for (uint64_t d = 0; d < draws; ++d) {
    if (mutate && d % 211 == 210) {   // a shader object made again at the same address: its tail, or its head, or its size, changes
      Guest::Shader& s = g.shaders[rng() % g.shaders.size()];
      const uint32_t what = rng() % 3;
      if (what == 0 && s.code.size() > 40) s.code[s.code.size() - 1 - rng() % 8] ^= uint8_t(1 + rng() % 255);
      else if (what == 1) s.code[rng() % 24] ^= uint8_t(1 + rng() % 255);
      else if (s.code.size() > 100) s.code.resize(s.code.size() - 8);
    }
    // zipf-ish: a few shaders carry most draws
    uint32_t pick = rng() % 100 < 70 ? rng() % std::min<uint32_t>(nshaders, 6) : rng() % nshaders;
    const Guest::Shader& s = g.shaders[pick];
    uint8_t strides[24] = {};
    const uint32_t decl = 0x1000 + (rng() % 30 == 0 ? rng() % 3 : 0);   // the same object with other declarations / strides
    const bool seven_c = rng() % 40 == 0;
    strides[0] = uint8_t(rng() % 30 == 0 ? 8 : 4);
    const Table::Key k = Table::MakeKey(s.object, decl, s.addr, uint32_t(s.code.size()), seven_c, strides);
    if (mode == 3) {   // the stream alone (the baseline the speed line subtracts)
      out.ids.push_back(k.decl + k.bytes);
      continue;
    }
    const bool on = mode == 1 || (mode == 2 && ((d / 131) & 1));
    const Info* info = nullptr;
    if (on) {
      info = table.Find(k, s.code.data());
      if (info) {
        ++out.hits;
      } else {
        ++out.misses;
      }
    }
    if (!info) {
      info = w.Resolve(k, s.code.data());
      if (on) table.Remember(k, s.code.data(), info);
    }
    out.ids.push_back(info->id);
  }
  if (ns_per_draw) *ns_per_draw = (NowNs() - t0) / double(draws);
  out.creates = w.creates;
  return out;
}

void ShaderArms() {
  const ShaderRun a = RunShaders(0, 3, 40, 60000, true), b = RunShaders(1, 3, 40, 60000, true), f = RunShaders(2, 3, 40, 60000, true);
  Check(a.ids == b.ids, "shaders: the info every draw gets equal, switch off vs on (microcode and sizes mutated in place)", "(" + std::to_string(a.ids.size()) + " draws, " + std::to_string(a.creates) + " infos made)");
  Check(a.ids == f.ids, "shaders: equal when the switch flips every block");
  Check(a.creates == b.creates && a.creates == f.creates, "shaders: the same number of infos made");
  const ShaderRun c = RunShaders(0, 5, 400, 60000, true), d = RunShaders(1, 5, 400, 60000, true);   // far more shaders than table slots: constant eviction
  Check(c.ids == d.ids, "shaders: equal with 400 shaders thrashing the table");
  if (!g_plant_run) {
    Check(b.hits > b.misses * 2, "shaders: the table takes most draws (steady shaders)", std::to_string(b.hits) + " taken, " + std::to_string(b.misses) + " declined");
    Check(d.hits > 0 && d.misses > 0, "shaders: the thrash arm both takes and declines");
  }
  {   // F1: one object drawn with 3 declarations in turn must hit above 90 % (2 ways per set: the index must spread the variants)
    std::vector<uint8_t> code(600, 0x5A);
    Table t;
    World w;
    uint64_t hit = 0, total = 0;
    uint8_t strides[24] = {4};
    for (uint32_t i = 0; i < 3000; ++i) {
      const Table::Key k = Table::MakeKey(0x40001000u, 0x1000 + (i % 3), 0x20000, 600, (i % 3) == 2, strides);
      const Info* info = t.Find(k, code.data());
      if (info) ++hit;
      else t.Remember(k, code.data(), w.Resolve(k, code.data()));
      ++total;
    }
    Check(hit * 10 > total * 9, "shaders: one object with 3 declarations in turn hits above 90 %", std::to_string(hit) + " of " + std::to_string(total));
  }
  // speed: 1,200 draws a frame over 40 shaders, nothing mutated, the microcode hash is xxh3 as in the reader
  double off = 1e9, on = 1e9, base = 1e9;
  for (int k = 0; k < 3; ++k) {
    double x, y, z;
    (void)RunShaders(0, 9, 40, 300000, false, &x);
    (void)RunShaders(1, 9, 40, 300000, false, &y);
    (void)RunShaders(3, 9, 40, 300000, false, &z);
    off = std::min(off, x);
    on = std::min(on, y);
    base = std::min(base, z);
  }
  std::printf("INFO  shaders: %.1f ns per shader lookup off (hash + map find), %.1f ns on (table), net of the stream's own %.1f ns\n", off - base, on - base, base);
}

// ---- 3. the pin set -------------------------------------------------------------------------------------------------------------------------
void SetArms() {
  std::mt19937_64 rng(99);
  uint64_t wrong = 0, probes = 0;
  for (uint32_t size : {0u, 1u, 2u, 7u, 300u, 5000u, 40000u}) {
    std::vector<uint64_t> v;
    for (uint32_t i = 0; i < size; ++i) v.push_back(rng());
    if (size >= 2) v[0] = 0;   // zero is a legal key
    if (size >= 7) v[3] = v[4];   // duplicates
    std::vector<uint64_t> sorted = v;
    std::sort(sorted.begin(), sorted.end());
    draw_memos::FlatSet64 set;
    set.Build(v);
    for (uint32_t p = 0; p < 40000; ++p) {
      uint64_t q = (p & 1) && size ? v[rng() % size] : rng();
      if (p % 977 == 0) q = 0;
      const bool want = std::binary_search(sorted.begin(), sorted.end(), q);
      ++probes;
      if (set.Contains(q) != want) ++wrong;
    }
    // every member
    for (uint64_t m : v) {
      ++probes;
      if (!set.Contains(m)) ++wrong;
    }
  }
  Check(wrong == 0, "pin hash: membership equals the binary search over sets of 0..40000 (zero, duplicates, members and strangers)", std::to_string(probes) + " probes, " + std::to_string(wrong) + " wrong");

  {   // F6: keys that differ only in their high bits (i << 44) must not pile into one probe run; an unbuilt set has no members and reads nothing
    std::vector<uint64_t> v;
    for (uint64_t i = 1; i <= 40000; ++i) v.push_back(i << 44);
    draw_memos::FlatSet64 hi;
    hi.Build(v);
    bool all = true;
    for (uint64_t m : v) all &= hi.Contains(m);
    Check(all && !hi.Contains((40001ull << 44)), "pin hash: keys differing only above bit 44 are all found");
    Check(hi.LongestRun() < 64, "pin hash: the longest probe run for i<<44 keys stays under 64", std::to_string(hi.LongestRun()));
    const draw_memos::FlatSet64 empty;
    Check(!empty.Contains(0) && !empty.Contains(12345), "pin hash: an unbuilt set contains nothing");
  }
  // the reader's query over a published set
  thin::PinPublisher pub;
  std::vector<uint64_t> keys, triples;
  struct Site {
    uint32_t pass, obj, part;
  };
  std::vector<Site> sites;
  for (uint32_t i = 0; i < 600; ++i) sites.push_back({0x82A00000u + (i % 9) * 0x40, 0x7000 + i * 3, i % 5});
  for (uint32_t i = 0; i < sites.size(); ++i) {
    if (i % 3 == 0) continue;   // a third of the sites are not pinned
    triples.push_back(thin::TripleHash(sites[i].pass, sites[i].obj, sites[i].part));
    for (uint32_t ord = 0; ord < 3; ++ord) keys.push_back(thin::KeyHash(sites[i].pass, sites[i].obj, sites[i].part, ord, 0x1234 + (i % 4)));
  }
  pub.Publish(keys, triples);
  const std::shared_ptr<const thin::PinSet> snap = pub.Acquire();
  Check(snap && snap->key_table.built() && snap->triple_table.built(), "pin hash: Publish builds both tables");
  auto run = [&](int mode, double* ns) {
    thin::FrameLite lite;
    std::vector<uint64_t> out;
    std::mt19937 r(5);
    const double t0 = NowNs();
    uint64_t draws = 0;
    for (uint64_t frame = 1; frame <= 200; ++frame) {
      const bool on = mode == 1 || (mode == 2 && ((frame / 7) & 1));
      draw_memos::g_pin_hash_on.store(on);
      lite.Begin(snap, frame);
      for (uint32_t d = 0; d < 1200; ++d) {
        const Site& s = sites[r() % sites.size()];
        const bool eligible = (r() % 11) != 0;
        const uint32_t program = 0x1234 + (r() % 5);
        if (mode == 3) {   // the stream alone (the baseline the speed line subtracts)
          out.push_back(program + eligible);
          ++draws;
          continue;
        }
        const bool lt = lite.Query(s.pass, s.obj, s.part, eligible, program);
        out.push_back(uint64_t(lt) | (uint64_t(lite.last_ordinal()) << 1) | (uint64_t(lite.lite_draws()) << 20));
        ++draws;
      }
    }
    draw_memos::g_pin_hash_on.store(false);
    if (ns) *ns = (NowNs() - t0) / double(draws);
    return out;
  };
  const auto a = run(0, nullptr), b = run(1, nullptr), f = run(2, nullptr);
  Check(a == b, "pin hash: the thin query's answers, ordinals and lite counts equal, switch off vs on", "(" + std::to_string(a.size()) + " draws)");
  Check(a == f, "pin hash: equal when the switch flips every few frames");
  Check(std::count_if(a.begin(), a.end(), [](uint64_t x) { return x & 1; }) > 1000, "pin hash: the stream really goes lite on pinned draws");
  double off = 1e9, on = 1e9, base = 1e9;
  for (int k = 0; k < 5; ++k) {
    double x, y, z;
    (void)run(0, &x);
    (void)run(1, &y);
    (void)run(3, &z);
    off = std::min(off, x);
    on = std::min(on, y);
    base = std::min(base, z);
  }
  std::printf("INFO  pin set: %.1f ns per draw off (binary search over %zu keys / %zu triples), %.1f ns on (hash table), net of the stream's own %.1f ns\n", off - base, snap->keys.size(), snap->triples.size(), on - base, base);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef T14_PLANTS
  if (argc >= 3 && std::strcmp(argv[1], "plant") == 0) {
    draw_memos::g_plant = std::atoi(argv[2]);
    g_plant_run = true;
  }
#else
  (void)argc;
  (void)argv;
#endif
  NoteArms();
  ShaderArms();
  SetArms();
  std::printf("%s: %d checks, %d failed\n", g_failed ? "FAILED" : "all checks passed", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
