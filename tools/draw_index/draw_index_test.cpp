// S102 lane T14b: the offline proof of wwe2k14_rdr_origin_index and wwe2k14_rdr_shader_serial (0 game runs).
// Built by tools\draw_index\draw_index_check.py against the SHIPPED src\renderer_adapter\origin_intern.h and draw_memos.h (-DRDR_ORIGIN_INDEX_PLANTS -DRDR_ORIGIN_MEMO_PLANTS -DT14B_PLANTS).
//   test.exe            every arm; exit 0 only when no FAIL line was printed
//   test.exe plant N    one planted fault switched on (1-3 origin index, 4-5 shader epoch rule); exit 1 with a FAIL line when it is caught
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../../src/renderer_adapter/draw_memos.h"
#include "../../src/renderer_adapter/origin_intern.h"

namespace og = rdr_adapter::origin;
namespace dm = rdr_adapter::draw_memos;

static int g_fails = 0, g_passes = 0;
static void Pass(const char* name, const std::string& extra = "") { ++g_passes; std::printf("PASS %s%s%s\n", name, extra.empty() ? "" : ": ", extra.c_str()); }
static void Fail(const char* name, const std::string& why) { ++g_fails; std::printf("FAIL %s: %s\n", name, why.c_str()); }

static og::Key MakeKey(uint32_t i) {
  og::Key k;
  k.call_site = 0x82000000u + (i % 977) * 4, k.pass_site = i % 13, k.object = 0x40000000u + i, k.part = i % 5, k.op = 15 + 4 * (i % 3), k.widget = i % 7, k.text = i & 1, k.run = i % 11;
  return k;
}

static double NowNs() { return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

// ---- origin: the index answers what the memo / map path answers ---------------------------------------------------------------------------
static void OriginEqual() {
  og::Table t;
  std::mt19937 rng(14);
  std::vector<uint32_t> tags;
  const uint32_t kN = 20000;   // crosses four chunk boundaries (4096 keys a chunk)
  for (uint32_t i = 0; i < kN; ++i) tags.push_back(t.Intern(MakeKey(i)));
  uint64_t checked = 0;
  std::string why;
  for (int arm = 0; arm < 2 && why.empty(); ++arm) {
    for (uint32_t step = 0; step < 200000 && why.empty(); ++step) {
      uint32_t tag = tags[rng() % tags.size()];
      const uint32_t r = rng() % 16;
      if (r == 0) tag = rng();                                        // garbage
      else if (r == 1) tag ^= 0x01000000u;                            // a valid id, wrong check bits
      else if (r == 2) tag = og::MakeTag(kN + 1 + rng() % 50);        // an id not interned yet, valid check bits
      else if (r == 3) tag = 0;
      else if (r == 4) tag = og::MakeTag(og::kMaxIds);                // the bound
      else if (r == 5) tag = og::MakeTag(4096 + (rng() % 3) - 1);     // chunk boundary
      og::Key ka, kb;
      og::g_index_on.store(false);
      og::g_memo_on.store(arm == 1);
      const bool a = t.Resolve(tag, ka);
      og::g_index_on.store(true);
      const bool b = t.Resolve(tag, kb);
      ++checked;
      if (a != b || (a && !(ka == kb))) why = "arm " + std::to_string(arm) + " step " + std::to_string(step) + " tag " + std::to_string(tag) + " plain " + std::to_string(a) + " index " + std::to_string(b);
    }
  }
  og::g_index_on.store(false);
  og::g_memo_on.store(false);
  if (why.empty()) Pass("origin: the index answers what the map answers (every id, garbage, wrong check bits, unknown ids, the bound, chunk edges)", std::to_string(checked) + " lookups x2 arms");
  else Fail("origin: the index answers what the map answers", why);
}

// A second table, a table destroyed and re-made at the same address: every answer is that table's own (T11 review F4's class: a memo outliving its table).
static void OriginTables() {
  std::string why;
  og::g_index_on.store(true);
  {
    og::Table a, b;
    std::vector<std::pair<uint32_t, og::Key>> ka, kb;
    for (int i = 0; i < 6000; ++i) {
      og::Key k = MakeKey(uint32_t(i));
      ka.push_back({a.Intern(k), k});
      k.object += 777777;   // different keys, the same ids
      kb.push_back({b.Intern(k), k});
    }
    for (int rep = 0; rep < 60000 && why.empty(); ++rep) {
      const size_t i = size_t(rep * 7919) % ka.size();
      for (int which = 0; which < 2 && why.empty(); ++which) {
        const og::Table& t = which ? b : a;
        const auto& e = (which ? kb : ka)[i];
        og::Key got;
        if (!t.Resolve(e.first, got) || !(got == e.second)) why = "rep " + std::to_string(rep) + " table " + (which ? "B" : "A");
      }
    }
  }
  for (int round = 0; round < 3 && why.empty(); ++round) {   // a table made again where the last one stood (same address likely), different contents
    auto* t = new og::Table;
    const uint32_t tag = t->Intern(MakeKey(uint32_t(round) * 1000 + 5));
    og::Key got;
    if (!t->Resolve(tag, got) || !(got == MakeKey(uint32_t(round) * 1000 + 5))) why = "re-made table round " + std::to_string(round);
    og::Key none;
    if (why.empty() && t->Resolve(og::MakeTag(2), none)) why = "a fresh table answered an id it does not hold";
    delete t;
  }
  og::g_index_on.store(false);
  if (why.empty()) Pass("origin: two tables and a table made again answer each their own keys (the F4 class)");
  else Fail("origin: two tables and a table made again", why);
}

// The same class on the OLD path: the T11 memo plant 2 (the memo blind to which table it came from) is wrong there and right on the index (past-bug re-find).
static void OriginPastBug() {
  og::Table a, b;
  og::Key ka = MakeKey(1), kb = MakeKey(2);
  const uint32_t ta = a.Intern(ka), tb = b.Intern(kb);   // the same tag (id 1), different keys
  og::Key got;
  og::g_index_on.store(false);
  og::g_memo_on.store(true);
  og::g_memo_plant = 2;
  a.Resolve(ta, got);
  const bool old_wrong = !b.Resolve(tb, got) || !(got == kb);   // the plain path with the T11 plant: a's key answers for b
  og::g_memo_plant = 0;
  og::g_memo_on.store(false);
  og::g_index_on.store(true);
  const bool idx_right = a.Resolve(ta, got) && got == ka && b.Resolve(tb, got) && got == kb;
  og::g_index_on.store(false);
  if (old_wrong && idx_right) Pass("origin past-bug re-find (T11 review F4: a memo outliving its table): the old path with the plant answers the other table's key, the index does not need a memo");
  else Fail("origin past-bug re-find", std::string("old path wrong ") + (old_wrong ? "yes" : "NO (plant did not reproduce the bug)") + ", index right " + (idx_right ? "yes" : "NO"));
}

// A writer interns while readers resolve every tag it published: no torn key, no false refusal of a published id (the release / acquire pair).
static void OriginThreads() {
  og::Table t;
  og::g_index_on.store(true);
  const uint32_t kN = 60000;
  std::vector<std::atomic<uint32_t>> published(kN);
  for (auto& p : published) p.store(0);
  std::atomic<uint32_t> upto{0};
  std::atomic<int> bad{0};
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (uint32_t i = 0; i < kN; ++i) {
      published[i].store(t.Intern(MakeKey(i)), std::memory_order_release);
      upto.store(i + 1, std::memory_order_release);
    }
    done.store(true);
  });
  auto reader = [&](uint32_t seed) {
    std::mt19937 rng(seed);
    while (!done.load() || upto.load() < kN) {
      const uint32_t n = upto.load(std::memory_order_acquire);
      if (!n) continue;
      for (int s = 0; s < 400; ++s) {
        const uint32_t i = rng() % n;
        og::Key got;
        const uint32_t tag = published[i].load(std::memory_order_acquire);
        if (!t.Resolve(tag, got) || !(got == MakeKey(i))) bad.fetch_add(1);
      }
      if (done.load()) break;
    }
  };
  std::thread r1(reader, 1), r2(reader, 2);
  writer.join(); r1.join(); r2.join();
  og::g_index_on.store(false);
  if (bad.load() == 0) Pass("origin: a writer interning while two readers resolve (no torn key, no refusal of a published id)", std::to_string(kN) + " keys");
  else Fail("origin: concurrent writer / readers", std::to_string(bad.load()) + " bad answers");
}

static void OriginCensusAndBench() {
  og::Table t;
  std::vector<uint32_t> tags;
  for (uint32_t i = 0; i < 3000; ++i) tags.push_back(t.Intern(MakeKey(i)));
  og::t_index_stats = {};
  og::g_index_on.store(true);
  og::Key k;
  for (uint32_t tag : tags) t.Resolve(tag, k);
  t.Resolve(0x01000005u, k);               // wrong check bits
  t.Resolve(og::MakeTag(9999999), k);      // past the count
  const og::IndexStats s = og::t_index_stats;
  og::g_index_on.store(false);
  if (s.taken == tags.size() && s.bad_tag == 1 && s.past_count == 1) Pass("origin census: taken / declined counted by reason", "taken " + std::to_string(s.taken) + ", bad tag " + std::to_string(s.bad_tag) + ", past count " + std::to_string(s.past_count));
  else Fail("origin census", "taken " + std::to_string(s.taken) + " bad " + std::to_string(s.bad_tag) + " past " + std::to_string(s.past_count));
  // ns per lookup the way the reader makes them: each replay record has its own tag, so a one-entry memo misses (R20 section 3)
  std::vector<uint32_t> draws;
  std::mt19937 rng(3);
  for (int i = 0; i < 1240; ++i) draws.push_back(tags[rng() % 400]);   // a frame's records: a few hundred distinct origins, ~1240 draws
  auto run = [&](bool memo, bool index) {
    og::g_memo_on.store(memo);
    og::g_index_on.store(index);
    og::Key kk;
    uint64_t sink = 0;
    for (int w = 0; w < 50; ++w) for (uint32_t tag : draws) { t.Resolve(tag, kk); sink += kk.object; }   // warm
    const double t0 = NowNs();
    const int reps = 2000;
    for (int r = 0; r < reps; ++r) for (uint32_t tag : draws) { t.Resolve(tag, kk); sink += kk.object; }
    const double t1 = NowNs();
    og::g_memo_on.store(false);
    og::g_index_on.store(false);
    if (sink == 1) std::printf(" ");
    return (t1 - t0) / (double(reps) * double(draws.size()));
  };
  const double plain = run(false, false), memo = run(true, false), idx = run(false, true);
  std::printf("BENCH origin Resolve, ns per lookup over a frame's tags (per-record tags, the memo misses): map path %.1f, with the T11 memo %.1f, index %.1f (x%.1f faster than the map path)\n", plain, memo, idx, plain / idx);
}

// ---- shaders: the epoch rule answers what the whole compare answers --------------------------------------------------------------------------
struct Info { int id; };
using Table = dm::ShaderTable<Info>;

struct World {
  // 300 guest shader objects: address, size, microcode bytes (the "guest memory"); every id has its Info.
  struct Obj { uint32_t object, decl, code_addr, bytes; bool seven_c; uint8_t strides[24]; std::vector<uint8_t> code; };
  std::vector<Obj> objs;
  std::vector<Info> infos;     // the reader's map values: stable addresses
  std::map<std::string, int> by_content;   // the plain path's identity: (fields + all the bytes) -> info id
  std::mt19937 rng{99};
  explicit World(int n) {
    infos.reserve(100000);
    for (int i = 0; i < n; ++i) {
      Obj o{};
      o.object = 0x40000000u + uint32_t(i) * 0x400u, o.decl = 0x50000000u + uint32_t(i % 17) * 0x40u, o.code_addr = 0x60000000u + uint32_t(i) * 0x2000u;
      o.bytes = 64 + 4 * (rng() % 120), o.seven_c = (i % 5) == 0;
      for (int s = 0; s < 24; ++s) o.strides[s] = uint8_t(s * (i % 3));
      o.code.resize(o.bytes);
      for (auto& b : o.code) b = uint8_t(rng());
      objs.push_back(o);
    }
  }
  std::string Ident(const Obj& o) const {
    std::string s((const char*)&o.object, 4);
    s.append((const char*)&o.decl, 4).append((const char*)&o.code_addr, 4).append((const char*)&o.bytes, 4).push_back(char(o.seven_c));
    s.append((const char*)o.strides, 24).append((const char*)o.code.data(), o.code.size());
    return s;
  }
  // the reader's plain path: identity -> Info (created on first sight)
  const Info* Plain(const Obj& o) {
    const std::string id = Ident(o);
    auto it = by_content.find(id);
    if (it == by_content.end()) { infos.push_back({int(infos.size())}); it = by_content.emplace(id, infos.back().id).first; }
    return &infos[size_t(it->second)];
  }
  Table::Key KeyOf(const Obj& o) const { return Table::MakeKey(o.object, o.decl, o.code_addr, o.bytes, o.seven_c, o.strides); }
};

// A stream of draws; epochs bump every `epoch_len` draws; at each epoch boundary some microcode is rewritten in place (head, tail, size) and some objects are
// recreated at reused addresses. `mid_head`: also rewrite a HEAD inside an epoch (the one-entry memo's own rule must catch it).
static bool ShaderStream(bool use_epoch, int draws, bool mid_head, bool mid_tail, uint64_t* trusted_out, uint64_t* verified_out, uint64_t* wrong_tail, std::string& why) {
  World w(300);
  Table table;
  std::mt19937 rng(7);
  uint64_t epoch = 1;
  std::vector<int> seen_ids;
  *wrong_tail = 0;
  for (int d = 0; d < draws; ++d) {
    if (d % 700 == 0) {   // the frame part ends: epoch bumps, some shaders change under their address
      ++epoch;
      for (int m = 0; m < 12; ++m) {
        World::Obj& o = w.objs[rng() % w.objs.size()];
        const uint32_t kind = rng() % 4;
        if (kind == 0) o.code[rng() % std::min<size_t>(32, o.code.size())] ^= 0x5A;                       // head
        else if (kind == 1) o.code[o.code.size() - 1 - rng() % 16] ^= 0xA5;                              // tail
        else if (kind == 2) { o.bytes += 4; o.code.push_back(uint8_t(rng())); }                          // size
        else { for (auto& b : o.code) b = uint8_t(rng()); }                                              // recreated at the same address
      }
    }
    World::Obj& o = w.objs[(rng() % 6 == 0) ? rng() % w.objs.size() : (d / 3) % 40];   // a working set with a tail
    if (mid_head && d % 700 == 350 && d > 700) o.code[rng() % std::min<size_t>(32, o.code.size())] ^= 0x33;    // a head rewritten inside the epoch
    if (mid_tail && d % 700 == 351 && d > 700) { o.code[o.code.size() - 1] ^= 0x77; }                    // the accepted case: a tail rewritten inside the epoch
    const Info* want = w.Plain(o);
    const Table::Key key = w.KeyOf(o);
    const Info* got = table.Find(key, o.code.data(), use_epoch ? epoch : 0);
    if (got != want) {
      if (got && mid_tail && use_epoch && d % 700 >= 351) { ++*wrong_tail; got = nullptr; }   // counted, not a failure: the documented rule
      else if (got) { why = "draw " + std::to_string(d) + ": the table answered info " + std::to_string(got->id) + ", the plain path " + std::to_string(want->id); return false; }
    }
    if (!got || got != want) table.Remember(key, o.code.data(), want, use_epoch ? epoch : 0);
  }
  *trusted_out = table.stats.trusted;
  *verified_out = table.stats.verified;
  return true;
}

static void ShaderEqual() {
  std::string why;
  uint64_t tr_off = 0, ve_off = 0, tr_on = 0, ve_on = 0, wt = 0;
  bool a = ShaderStream(false, 120000, false, false, &tr_off, &ve_off, &wt, why);
  bool b = a && ShaderStream(true, 120000, false, false, &tr_on, &ve_on, &wt, why);
  if (a && b && tr_off == 0 && tr_on > 0) Pass("shaders: the info every draw gets equals the plain path's (microcode rewritten at epoch boundaries: head, tail, size, recreated)",
                                              "off: verified " + std::to_string(ve_off) + ", trusted " + std::to_string(tr_off) + "; on: verified " + std::to_string(ve_on) + ", trusted " + std::to_string(tr_on) + " (whole compares " + std::to_string(100.0 * double(ve_on) / double(ve_on + tr_on)).substr(0, 4) + " % of hits)");
  else Fail("shaders: equal at epoch boundaries", why.empty() ? "no trusted hits (the rule never took) or trusted hits with the switch off" : why);
  why.clear();
  const bool c = ShaderStream(true, 120000, true, false, &tr_on, &ve_on, &wt, why);
  if (c) Pass("shaders: a head rewritten INSIDE an epoch is seen (the head compare, the one-entry memo's own rule)");
  else Fail("shaders: a head rewritten inside an epoch", why);
  why.clear();
  const bool d = ShaderStream(true, 120000, false, true, &tr_on, &ve_on, &wt, why);
  if (d && wt > 0) std::printf("NOTE accepted rule: a tail rewritten inside one frame part (same address, size, head) is not seen until the epoch turns: %llu of the stale answers in the test stream, all after the rewrite and within the same epoch (the plain path would hash it). See docs\\renderer\\DRAW_INDEX.md\n", (unsigned long long)wt);
  else if (!d) Fail("shaders: the stream with an in-epoch tail rewrite", why);
  else Fail("shaders: the stream with an in-epoch tail rewrite", "the accepted case never occurred (the test does not exercise the rule)");
}

static void ShaderBench() {
  World w(300);
  Table table;
  std::vector<World::Obj*> picks;
  std::mt19937 rng(5);
  for (int i = 0; i < 1244; ++i) picks.push_back(&w.objs[rng() % 120]);
  std::vector<Table::Key> keys;
  for (auto* o : picks) keys.push_back(w.KeyOf(*o));
  for (auto* o : picks) table.Remember(w.KeyOf(*o), o->code.data(), w.Plain(o[0]), 1);
  auto run = [&](uint64_t epoch) {
    uint64_t sink = 0;
    for (int wm = 0; wm < 20; ++wm) for (size_t i = 0; i < picks.size(); ++i) sink += (uintptr_t)table.Find(keys[i], picks[i]->code.data(), epoch);
    const double t0 = NowNs();
    const int reps = 1500;
    for (int r = 0; r < reps; ++r) for (size_t i = 0; i < picks.size(); ++i) sink += (uintptr_t)table.Find(keys[i], picks[i]->code.data(), epoch);
    const double t1 = NowNs();
    if (sink == 1) std::printf(" ");
    return (t1 - t0) / (double(reps) * double(picks.size()));
  };
  const double whole = run(0);   // epoch 0: the whole compare at every hit
  const double rule = run(1);    // verified in epoch 1 by Remember: the head compare
  std::printf("BENCH shader table Find, ns per hit (microcode %d-%d bytes): whole compare %.1f, epoch rule %.1f (x%.1f faster)\n", 64, 64 + 4 * 119, whole, rule, whole / rule);
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "plant") {
    const int n = std::atoi(argv[2]);
    if (n >= 1 && n <= 3) og::g_index_plant = n;
    else if (n == 4) dm::g_plant_b = 1;
    else if (n == 5) dm::g_plant_b = 2;
    else { std::printf("FAIL unknown plant %d\n", n); return 1; }
  }
  OriginEqual();
  OriginTables();
  OriginPastBug();
  OriginThreads();
  OriginCensusAndBench();
  ShaderEqual();
  ShaderBench();
  std::printf("%s: %d passed, %d failed\n", g_fails ? "FAILED" : "all checks passed", g_passes, g_fails);
  return g_fails ? 1 : 0;
}
