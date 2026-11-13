// ring_rows_test: the offline proof of lane 194's Ring D3 pieces R0-R2 (Session 101; docs\renderer\SKIP_GAME_WORK_MAP.md 16.4 / 16.7). 0 game runs, no SDK, no game memory.
//   ring_rows_test --sites tools\ring\ring_call_sites.tsv [--plant NAME] [--write-record PATH]
// It runs the SHIPPED code: the pure rules (src\scene_live\ring_rows_core.h), the model-entry tracker (ring_rows_hook.h) against a fake guest memory, the contributor's Emit (ring_rows_contrib.h) and
// the record file code (record_io: kScRingRows / kScRingNodes round trip, an older record, an unknown layout, a bad node range).
// THE ORACLE is the call-site table taken from the listings (tools\ring\ring_call_sites.tsv: class, G offset, call site, r5 meaning, calls a frame, sides x heights, node count and its path), never the stream's
// values: the fake guest is BUILT from that table (the model pointers in the object at [0x836F4C74], the node-count words, the rope buffers shared by a side's three heights and rewritten between heights),
// the game's frame is replayed in LIVE ORDER (a frame's calls, then the frame boundary's Emit with the present's guest frame; the consumer joins record R_f to stream frame f + 1 and keeps only the newest 8),
// and the expected rows are computed here from the values the "game" wrote at each call, by a formula of this file (not by arenaworld::RowsFromMatrix).
// Plants (each must make a NAMED check fail; exit code 1 when any check failed):
//   dedupe_model_r5       the arena's key (one entry per (model, r5)): ropes and instances collapse                            -> "hook.counts_per_group", "hook.per_height_rows"
//   late_copy             the rows read at the NEXT ring call, after the game rewrote the shared buffer                      -> "hook.per_height_rows"
//   endless_retry         an unreadable call kept and retried at every later call                                             -> "hook.unreadable_once"
//   dropped_frame_failure frames the sink skipped (calls of an older frame) counted as failures that latch                     -> "hook.old_calls_not_failures"
//   offset0               the +1 frame offset lost (the stamp is the stream's frame, not the hook's)                          -> "contrib.live_order_join"
//   late_record           the consumer sees record R_f only with stream frame f + 2 (a record one present late)                -> "contrib.live_order_join"
//   ordinal_by_model_r5   `call` counted per (model, r5) instead of per model: a different ordinal than the LightRef's         -> "contrib.cross_check"
//   alpha_zero            [model+252] not read                                                                                -> "hook.alpha"
//   no_forward            the node count read from [this+300] only (no forward through [this+220])                              -> "hook.per_height_rows", "hook.nothing_refused"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/ring_rows_contrib.h"

using namespace rdr_scene;
using namespace rdr_scene::live::ringrows;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;

static_assert(kScRingRows == 20 && kScRingNodes == 21 && sizeof(RingRows) == 40 && sizeof(RingNodeRows) == 48, "sections 20 / 21; the Python reader pins the same sizes");
static_assert(!kRingSkipBuilt, "lane 194 builds no skipping");

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// ---- a fake guest ----
struct Guest {
  std::map<uint32_t, uint32_t> words;   // 4-byte words (host values)
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  void PutFloat(uint32_t a, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    words[a] = b;
  }
  bool ReadMats(uint32_t a, uint32_t n, std::vector<Mat4>& out) const {
    out.resize(n);
    for (uint32_t i = 0; i < n * 16; ++i) {
      auto it = words.find(a + 4 * i);
      if (it == words.end()) return false;
      std::memcpy(&reinterpret_cast<float*>(out.data())[i], &it->second, 4);
    }
    return true;
  }
};

struct Site {
  std::string cls;
  uint32_t goff = 0;
  int calls = 0, sides = 0, heights = 0, nodes = 0;
  bool rope_buf = false, forward = false;
  uint32_t model = 0, sub = 0, own = 0;
};

static std::vector<Site> LoadSites(const std::string& path) {
  std::vector<Site> v;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#' || line.rfind("class\t", 0) == 0) continue;
    std::vector<std::string> c;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) c.push_back(cell);
    if (c.size() < 11) continue;
    Site s;
    s.cls = c[0];
    s.goff = uint32_t(std::stoul(c[1]));
    s.rope_buf = c[5] == "rope_buf";
    s.calls = std::stoi(c[6]), s.sides = std::stoi(c[7]), s.heights = std::stoi(c[8]), s.nodes = std::stoi(c[9]);
    s.forward = c[10] == "forward";
    v.push_back(s);
  }
  return v;
}

constexpr uint32_t kG = 0x40000000, kVt = 0x60000000, kRopeBuf = 0x90000000;
constexpr float kAlphaBase = 0.25f;

// The value the "game" writes into element e of node n for a call: exact in float (integers up to 2^20 times 0.5).
static float V(uint32_t frame, uint32_t site_ix, uint32_t call, uint32_t node, uint32_t e) {
  return float((frame % 1000u) * 1009u % 90000u + site_ix * 131u + call * 31u + node * 7u + e) * 0.5f;
}
// The rows the packer cuts from that 4x4: row k = (m[k], m[4+k], m[8+k], m[12+k]) [listing 0x82C0A038-0x82C0A090]: this file's own formula.
static void ExpectRows(uint32_t frame, uint32_t site_ix, uint32_t call, uint32_t node, float out[12]) {
  for (int k = 0; k < 3; ++k)
    for (int r = 0; r < 4; ++r) out[4 * k + r] = V(frame, site_ix, call, node, uint32_t(4 * r + k));
}

struct Expect {
  uint32_t frame, object, call, r5, n_nodes, flags;
  float alpha;
  std::vector<float> rows;   // n_nodes * 12
};

struct Run {
  Guest g;
  std::vector<Site> sites;
  std::vector<Expect> expect;     // every ring call the "game" made, in live order
  std::vector<LightRef> refs;     // LightRefs the oracle makes per frame from ITS count, delivered to the record
};

static uint32_t ModelOf(const Site& s) { return 0x50000000u + s.goff * 0x1000u; }

static void BuildGuest(Run& r, uint32_t g_base) {
  Guest& g = r.g;
  g.words[0x836F4C74] = g_base;
  g.words[kVt + 8] = kNodeCountFn;
  for (Site& s : r.sites) {
    s.model = ModelOf(s) + (g_base - kG) / 4;
    s.sub = s.model + 0x800;
    s.own = 0x80000000u + (s.model & 0x0FFFFFFFu) % 0x10000000u;
    g.words[g_base + s.goff] = s.model;
    g.words[s.model] = kVt;
    g.words[s.model + 200] = s.own;
    if (!s.forward) {
      g.words[s.model + 296] = 1, g.words[s.model + 300] = uint32_t(s.nodes);
    } else {
      g.words[s.model + 296] = 0, g.words[s.model + 220] = s.sub;
      g.words[s.sub] = kVt, g.words[s.sub + 296] = 1, g.words[s.sub + 300] = uint32_t(s.nodes);
    }
  }
}

static void WriteRows(Guest& g, uint32_t r5, uint32_t frame, uint32_t site_ix, uint32_t call, int nodes) {
  for (int n = 1; n <= nodes; ++n)
    for (uint32_t e = 0; e < 16; ++e) g.PutFloat(r5 + 64u * uint32_t(n - 1) + 4 * e, V(frame, site_ix, call, uint32_t(n), e));
}

// One frame of the game in live order. `hook(model, r5)` is the model-entry hook; `noise` models are non-ring wrestler/arena calls interleaved.
template <class Hook>
static void PlayFrame(Run& r, uint32_t frame, Hook&& hook, bool record_expect = true) {
  uint32_t nz = 0;
  const auto noise = [&](int n) {
    for (int i = 0; i < n; ++i, ++nz) {
      const uint32_t m = 0xAB000000u + nz * 0x40u;
      r.g.words[m] = kVt, r.g.words[m + 296] = 1, r.g.words[m + 300] = 5, r.g.words[m + 200] = 0xA0000000u + nz * 0x400u;
      hook(m, 0xA0000000u + nz * 0x400u);
    }
  };
  std::map<uint32_t, uint32_t> per_obj;
  const auto call = [&](size_t si, uint32_t r5, uint32_t call_in_site) {
    const Site& s = r.sites[si];
    WriteRows(r.g, r5, frame, uint32_t(si), call_in_site, s.nodes);
    const float alpha = kAlphaBase + float(frame % 7) * 0.125f + float(si) * 0.5f;
    r.g.PutFloat(s.model + kModelAlphaOffset, alpha);
    Expect e;
    e.frame = frame, e.object = s.model, e.call = per_obj[s.model]++, e.r5 = r5, e.n_nodes = uint32_t(s.nodes), e.flags = (s.forward ? kRrCountForwarded : 0) | (r5 != s.own ? kRrForeignR5 : 0);
    e.alpha = alpha;
    for (int n = 1; n <= s.nodes; ++n) {
      float rows[12];
      ExpectRows(frame, uint32_t(si), call_in_site, uint32_t(n), rows);
      e.rows.insert(e.rows.end(), rows, rows + 12);
    }
    if (record_expect) r.expect.push_back(e);
    LightRef lr;
    lr.object = s.model, lr.ordinal = e.call, lr.state = 0, lr.frame = frame;
    if (record_expect) r.refs.push_back(lr);
    hook(s.model, r5);
  };
  // the order the listings give: posts, turnbuckles, covers (sub_825CCFE0 / 250 / 668), ropes by side then height (sub_825CF2F8), skirts, the canvas
  const char* order[] = {"post_hi", "post_lo", "turnbuckle", "cover_hi", "cover_lo", "rope_hi", "rope_lo", "skirt", "canvas"};
  for (const char* cls : order) {
    for (size_t si = 0; si < r.sites.size(); ++si) {
      const Site& s = r.sites[si];
      if (s.cls != cls || s.calls == 0) continue;
      noise(3);
      if (s.rope_buf) {
        const int side0 = std::string(cls) == "rope_lo" ? 3 : 0;
        for (int side = 0; side < s.sides; ++side)
          for (int h = 0; h < s.heights; ++h) call(si, kRopeBuf + 0x2000u * uint32_t(side0 + side), uint32_t((side0 + side) * 3 + h));
      } else {
        for (int c = 0; c < s.calls; ++c) call(si, s.own, uint32_t(c));
      }
    }
  }
  noise(7);
}

static const char* GroupNameOf(const std::string& cls) {
  if (cls.rfind("post", 0) == 0) return "post";
  if (cls == "turnbuckle") return "turnbuckle";
  if (cls.rfind("cover", 0) == 0) return "cover";
  if (cls.rfind("rope", 0) == 0) return "rope";
  if (cls == "canvas") return "canvas";
  return "skirt";
}

int main(int argc, char** argv) {
  std::string plant, sites_path = "tools/ring/ring_call_sites.tsv", write_record;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--sites" && i + 1 < argc) sites_path = argv[++i];
    else if (a == "--write-record" && i + 1 < argc) write_record = argv[++i];
  }
  Plant tp = plant == "dedupe_model_r5" ? Plant::DedupeModelR5 : plant == "late_copy" ? Plant::LateCopy : plant == "endless_retry" ? Plant::EndlessRetry
             : plant == "dropped_frame_failure" ? Plant::CountFailureIsHole : plant == "offset0" ? Plant::Offset0 : plant == "ordinal_by_model_r5" ? Plant::OrdinalByModelR5
             : plant == "alpha_zero" ? Plant::AlphaZero : plant == "no_forward" ? Plant::NoForward : Plant::None;
  const std::vector<Site> sites = LoadSites(sites_path);
  if (sites.empty()) {
    std::printf("FAIL  no sites read from %s\n", sites_path.c_str());
    return 1;
  }

  // ---- the pure rules ----
  {
    bool ok = true;
    std::string why;
    for (const Site& s : sites) {
      bool found = false;
      for (uint32_t i = 0; i < kClassTableCount; ++i)
        if (kClassTable[i].offset == s.goff) {
          found = true;
          const std::string name = ClassName(kClassTable[i].cls);
          if (name != s.cls) ok = false, why += " offset " + std::to_string(s.goff) + " is '" + name + "' in the core, '" + s.cls + "' in the listings table;";
        }
      if (!found) ok = false, why += " offset " + std::to_string(s.goff) + " missing in the core;";
    }
    Check(ok && sites.size() == 12 + 0 && kClassTableCount == 13, "table.offsets_match_listings", ok ? "13 core entries (the proxy at +32 is not a drawn call site), every listed offset names the same class" : why);
    uint32_t d = 0;
    Check(RowsRegisterDontCare(2, 82) && !RowsRegisterDontCare(2, 81) && RowsRegisterDontCare(2, 138) && !RowsRegisterDontCare(2, 139) && !RowsRegisterDontCare(0, 100) && !RowsRegisterDontCare(21, 138) &&
              !RowsRegisterDontCare(20, 135) && RowsRegisterDontCare(20, 136) && !RowsRegisterDontCare(2, 76) && !RowsRegisterDontCare(2, 75),
          "pure.dont_care_registers", "c76 + 3 * npal .. c138 are don't care (npal 2: c82..c138); npal 0 = unknown = care; outside c76..c138 = care");
    uint32_t slot = 0, row = 0;
    Check(RowRegisterSlot(76, slot, row) && slot == 0 && row == 0 && RowRegisterSlot(138, slot, row) && slot == 20 && row == 2 && !RowRegisterSlot(139, slot, row) && !RowRegisterSlot(75, slot, row), "pure.row_register_slot", "c76 = slot 0 row 0, c138 = slot 20 row 2");
    (void)d;
  }

  // ---- the node-count rule (sub_82BD3AB8) ----
  {
    Guest g;
    g.words[kVt + 8] = kNodeCountFn;
    g.words[0x500] = kVt, g.words[0x500 + 296] = 7, g.words[0x500 + 300] = 21;                       // direct
    g.words[0x600] = kVt, g.words[0x600 + 296] = 0, g.words[0x600 + 220] = 0;                        // -1
    g.words[0x700] = kVt, g.words[0x700 + 296] = 0, g.words[0x700 + 220] = 0x720, g.words[0x720] = kVt, g.words[0x720 + 296] = 1, g.words[0x720 + 300] = 33;   // forward
    g.words[0x800] = 0x9000, g.words[0x9000 + 8] = 0x82000000, g.words[0x800 + 296] = 1, g.words[0x800 + 300] = 5;                                           // slot 2 is another function
    auto rw = [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); };
    int32_t n = 0;
    uint32_t fl = 0;
    bool direct = NodeCount(rw, 0x500, n, fl) && n == 21 && fl == 0;
    fl = 0;
    bool minus = NodeCount(rw, 0x600, n, fl) && n == -1;
    fl = 0;
    bool fwd = NodeCount(rw, 0x700, n, fl) && n == 33 && (fl & kRrCountForwarded);
    fl = 0;
    bool wrong_slot = !NodeCount(rw, 0x800, n, fl);
    fl = 0;
    bool unread = !NodeCount(rw, 0x1234, n, fl);
    Check(direct && minus && fwd && wrong_slot && unread, "hook.count_rules",
          "direct 21; -1 when [+296] and [+220] are 0; forwards through [+220] only when its slot 2 is the same function; another slot-2 function / unreadable = unknown");
  }

  // ---- the oracle run: the fake guest is built from the listings table, the frames are replayed in live order ----
  Run run;
  run.sites = sites;
  BuildGuest(run, kG);
  auto rw = [&](uint32_t a, uint32_t& v) { return run.g.ReadWord(a, v); };
  auto rm = [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return run.g.ReadMats(a, n, o); };
  RingRowsContributor C;
  C.tracker.plant = tp;
  const uint32_t kFrames = 60, kFirst = 5000;
  // the consumer: the newest 8 records, joined by (hook stamp, object, call); stream frame s = hook frame + 1 reads the record that closed hook frame s - 1 (the pairing of live.cpp / lane 152)
  std::vector<std::shared_ptr<SceneTables>> records;
  uint64_t join_ok = 0, join_missing = 0, join_wrong = 0, rows_wrong = 0, alpha_wrong = 0, flags_wrong = 0, calls_total = 0;
  bool groups_ok = true;
  std::string groups_why;
  const bool late_record = plant == "late_record";
  for (uint32_t f = kFirst; f < kFirst + kFrames; ++f) {
    const size_t before = run.expect.size();
    const size_t refs_before = run.refs.size();
    PlayFrame(run, f, [&](uint32_t model, uint32_t r5) { C.tracker.OnModelCall(model, r5, f, rw, rm); });
    if (f == kFirst + 20) {   // a late call of an older frame (another thread; the sink skipped that frame): counted as old, never a failure
      C.tracker.OnModelCall(run.sites[0].model, run.sites[0].own, f - 3, rw, rm);
      C.tracker.OnModelCall(run.sites[0].model, run.sites[0].own, f - 3, rw, rm);
    }
    // the frame boundary (live.cpp EndFrame): the present's guest frame is f + 1; the light-state contributor delivers its refs for the frames the hook closed
    FrameInfo fi;
    fi.guest_frame = f + 1;
    auto t = std::make_shared<SceneTables>();
    t->header.source = uint8_t(Source::Live);
    t->header.guest_frame = f + 1;
    Counters cn;
    // the LightRefs of frame f, made by the oracle from its OWN count (the rule: the index among all the object's model calls of the frame)
    for (size_t i = refs_before; i < run.refs.size(); ++i) t->light_refs.push_back(run.refs[i]);
    t->light_states.resize(1);
    C.Emit(fi, *t, cn);
    records.push_back(t);
    if (records.size() > 8) records.erase(records.begin());
    // stream frame f + 1 joins the record that closed hook frame f: late_record = the consumer only has the records of the earlier presents
    const size_t avail = late_record ? records.size() - 1 : records.size();
    std::map<std::string, uint32_t> gc;
    for (size_t k = before; k < run.expect.size(); ++k) {
      const Expect& e = run.expect[k];
      ++calls_total;
      const RingRows* hit = nullptr;
      const SceneTables* rec = nullptr;
      for (size_t ri = 0; ri < avail; ++ri)
        for (const RingRows& rr : records[ri]->ring_rows)
          if (rr.frame == e.frame && rr.object == e.object && rr.call == e.call) hit = &rr, rec = records[ri].get();
      if (!hit) { ++join_missing; continue; }
      ++join_ok;
      if (hit->r5 != e.r5 || hit->n_nodes != e.n_nodes) ++join_wrong;
      if ((hit->flags & (kRrForeignR5 | kRrCountForwarded)) != e.flags) ++flags_wrong;
      if (std::fabs(hit->alpha - e.alpha) > 1e-6f) ++alpha_wrong;
      bool same = hit->first_node + hit->n_nodes <= rec->ring_nodes.size();
      for (uint32_t n = 0; same && n < hit->n_nodes; ++n)
        for (int j = 0; j < 12; ++j)
          if (rec->ring_nodes[hit->first_node + n].rows[j] != e.rows[n * 12 + j]) { same = false; break; }
      if (!same) ++rows_wrong;
    }
    if (!late_record && !records.empty()) {
      const SceneTables& last = *records.back();
      for (const RingRows& rr : last.ring_rows)
        if (rr.frame == f) {
          uint8_t g = GroupOf(rr.klass);
          ++gc[GroupName(g)];
        }
      for (uint32_t g = 0; g < kRgOther; ++g)
        if (gc[GroupName(uint8_t(g))] != kExpectedCalls[g]) {
          groups_ok = false;
          if (groups_why.empty()) groups_why = std::string(GroupName(uint8_t(g))) + " " + std::to_string(gc[GroupName(uint8_t(g))]) + " calls in frame " + std::to_string(f) + ", want " + std::to_string(kExpectedCalls[g]);
        }
    }
  }
  // flush the last frame
  {
    C.tracker.OnModelCall(0xAB000000u, 0, kFirst + kFrames + 5, rw, rm);
  }
  const Stats st = C.tracker.GetStats();
  uint32_t want_calls_per_frame = 0;
  for (uint32_t g = 0; g < kRgOther; ++g) want_calls_per_frame += kExpectedCalls[g];
  {
    // the listing table's calls (sum of calls_per_frame over classes) equals the contract's per-group expectations
    std::map<std::string, int> sum;
    for (const Site& s : sites) sum[GroupNameOf(s.cls)] += s.calls;
    bool eq = true;
    for (uint32_t g = 0; g < kRgOther; ++g) eq = eq && sum[GroupName(uint8_t(g))] == int(kExpectedCalls[g]);
    Check(eq, "table.counts_match_contract", "post 4, turnbuckle 12, cover 12, rope 12, canvas 1, skirt 3 (16.1) from the listings table");
  }
  Check(groups_ok && st.ring_calls >= uint64_t(kFrames) * want_calls_per_frame, "hook.counts_per_group", groups_ok ? "every recorded frame holds exactly the listed calls per group (" + std::to_string(want_calls_per_frame) + " model calls a frame)" : groups_why);
  Check(join_missing == 0 && join_wrong == 0 && rows_wrong == 0 && flags_wrong == 0, "hook.per_height_rows",
        "calls " + std::to_string(calls_total) + ", missing " + std::to_string(join_missing) + ", wrong r5/count " + std::to_string(join_wrong) + ", rows wrong " + std::to_string(rows_wrong) + ", flags wrong " + std::to_string(flags_wrong) + " (the rope heights share one r5 and are rewritten between heights)");
  Check(alpha_wrong == 0 && join_ok > 0, "hook.alpha", "[model+252] copied as the entry's alpha, wrong " + std::to_string(alpha_wrong));
  Check(join_missing == 0 && join_ok == calls_total, "contrib.live_order_join", "every call joins by (hook stamp, object, call) in the record that closed its frame, kept 8 records: ok " + std::to_string(join_ok) + " missing " + std::to_string(join_missing));
  // the light-ref cross-check: the oracle's refs (made from ITS count of each object's calls) vs the entries' `call` (the contributor compared the two sets of ordinals per (frame, object) while emitting)
  {
    const RefCross rc = C.Cross();
    Check(rc.differ == 0 && rc.equal > 0 && rc.frames_joined >= kFrames - 2, "contrib.cross_check", "per (frame, object) the entries' calls == the LightRef ordinals: equal " + std::to_string(rc.equal) + ", differ " + std::to_string(rc.differ) + ", frames joined " + std::to_string(rc.frames_joined) + ", unjoined " + std::to_string(rc.unjoined_frames));
  }
  Check(st.unreadable == 0 && st.retries == 0, "hook.unreadable_once", "retries " + std::to_string(st.retries) + " unreadable " + std::to_string(st.unreadable));
  Check(st.old_calls >= 2 && st.latched == 0, "hook.old_calls_not_failures", "calls of an older frame: " + std::to_string(st.old_calls) + " counted, latched " + std::to_string(st.latched));
  Check(st.late == 0 && st.overflow == 0 && st.table_missing == 0 && st.alpha_unread == 0 && st.count_unknown == 0 && st.capped == 0, "hook.nothing_refused", "late/overflow/table_missing/alpha_unread/count_unknown/capped all 0");

  // ---- foreign r5 (the rope buffer), the forward path, the table re-read on a new G (L5) ----
  {
    uint64_t rope_foreign = 0, other_foreign = 0, cover_fwd = 0, other_fwd = 0;
    for (const auto& rec : records)
      for (const RingRows& r : rec->ring_rows) {
        const bool rope = r.klass == kRcRopeHi || r.klass == kRcRopeLo;
        if (r.flags & kRrForeignR5) (rope ? rope_foreign : other_foreign)++;
        if (r.flags & kRrCountForwarded) ((r.klass == kRcCoverHi || r.klass == kRcCoverLo) ? cover_fwd : other_fwd)++;
      }
    Check(rope_foreign > 0 && other_foreign == 0 && cover_fwd > 0 && other_fwd == 0, "hook.foreign_and_forward_flags", "ropes foreign " + std::to_string(rope_foreign) + ", covers forwarded " + std::to_string(cover_fwd) + ", others 0");
  }
  {
    // a new arena (L5): G changes, the models are new objects; the old model addresses must stop being ring objects and the new ones join
    Run r2;
    r2.sites = sites;
    BuildGuest(r2, kG + 0x1000);
    auto rw2 = [&](uint32_t a, uint32_t& v) { return r2.g.ReadWord(a, v); };
    auto rm2 = [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return r2.g.ReadMats(a, n, o); };
    RingRowsContributor C2;
    PlayFrame(r2, 100, [&](uint32_t m, uint32_t r5) { C2.tracker.OnModelCall(m, r5, 100, rw2, rm2); });
    const uint64_t a1 = C2.tracker.GetStats().ring_calls;
    const uint32_t old_model = run.sites[0].model;   // from the first G: not in the new table
    CallResult res = C2.tracker.OnModelCall(old_model, run.sites[0].own, 101, rw2, rm2);   // frame 101: the table is re-read, the old model is not a ring object
    Check(a1 == want_calls_per_frame && !res.ring, "table.membership_and_new_arena", "a new G gives " + std::to_string(a1) + " ring calls; a model of the old G is not a ring object");
  }

  // ---- unreadable rows, an unreadable count, the cap, overflow ----
  {
    Run r3;
    r3.sites = sites;
    BuildGuest(r3, kG);
    auto rw3 = [&](uint32_t a, uint32_t& v) { return r3.g.ReadWord(a, v); };
    auto rm3 = [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return r3.g.ReadMats(a, n, o); };
    RingRowsContributor C3;
    C3.tracker.plant = tp;
    const Site& s = r3.sites[2];   // turnbuckle model: no rows written at its palette
    CallResult a = C3.tracker.OnModelCall(s.model, s.own, 200, rw3, rm3);      // rows unreadable
    CallResult b = C3.tracker.OnModelCall(s.model, s.own, 200, rw3, rm3);
    r3.g.words[s.model + 300] = 500;                                           // count above the cap
    WriteRows(r3.g, s.own, 200, 2, 0, 96);
    CallResult c = C3.tracker.OnModelCall(s.model, s.own, 200, rw3, rm3);
    r3.g.words[s.model + 296] = 0, r3.g.words[s.model + 220] = 0;              // -1
    CallResult d = C3.tracker.OnModelCall(s.model, s.own, 200, rw3, rm3);
    // a later call in the same frame must not retry the unreadable ones
    WriteRows(r3.g, s.own, 200, 2, 0, 96);
    r3.g.words[s.model + 296] = 1, r3.g.words[s.model + 300] = 3;
    CallResult e = C3.tracker.OnModelCall(s.model, s.own, 200, rw3, rm3);
    const Stats s3 = C3.tracker.GetStats();
    Check(a.recorded && (a.flags & kRrUnreadable) && a.n_nodes == 0 && b.recorded && (b.flags & kRrUnreadable) && (c.flags & kRrCapped) && c.n_nodes == kRingMaxNodes && (d.flags & kRrCountUnknown) && d.n_nodes == 0 && e.n_nodes == 3 && s3.retries == 0 &&
              s3.unreadable == 2 && s3.capped == 1 && s3.count_unknown == 1,
          "hook.unreadable_once", std::string("unreadable ") + std::to_string(s3.unreadable) + " capped " + std::to_string(s3.capped) + " count_unknown " + std::to_string(s3.count_unknown) + " retries " + std::to_string(s3.retries));
    // overflow: more calls in a frame than the capacity: counted, never silently truncated
    RingRowsContributor C4;
    for (uint32_t i = 0; i < kMaxCalls + 20; ++i) C4.tracker.OnModelCall(s.model, s.own, 300, rw3, rm3);
    SceneTables t4;
    Counters cn4;
    FrameInfo fi4;
    fi4.guest_frame = 301;
    C4.Emit(fi4, t4, cn4);
    const Stats s4 = C4.tracker.GetStats();
    Check(t4.ring_rows.size() == kMaxCalls && s4.overflow == 20 && cn4.unknown >= 20, "hook.overflow_counted", "entries " + std::to_string(t4.ring_rows.size()) + ", overflow " + std::to_string(s4.overflow) + ", c.unknown " + std::to_string(cn4.unknown));
  }

  // ---- the record file code and the null-edit self-test ----
  {
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    t.header.guest_frame = kFirst + kFrames;
    for (const auto& rec : records) {
      const uint32_t base = uint32_t(t.ring_nodes.size());
      t.ring_nodes.insert(t.ring_nodes.end(), rec->ring_nodes.begin(), rec->ring_nodes.end());
      for (RingRows r : rec->ring_rows) r.first_node += base, t.ring_rows.push_back(r);
    }
    std::string err;
    std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables back;
    const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
    const bool same = ok && back.ring_rows.size() == t.ring_rows.size() && back.ring_nodes.size() == t.ring_nodes.size() &&
                      std::memcmp(back.ring_rows.data(), t.ring_rows.data(), t.ring_rows.size() * sizeof(RingRows)) == 0 &&
                      std::memcmp(back.ring_nodes.data(), t.ring_nodes.data(), t.ring_nodes.size() * sizeof(RingNodeRows)) == 0;
    Check(same && back.ring_errors == 0, "record.roundtrip", ok ? "kScRingRows / kScRingNodes round trip is byte-identical (" + std::to_string(t.ring_rows.size()) + " entries, " + std::to_string(t.ring_nodes.size()) + " pool rows)" : err);
    if (!write_record.empty()) {
      std::string werr;
      Check(WriteRecord(write_record, t, werr), "record.write_sample", werr);
    }
    SceneTables old;
    old.header.source = uint8_t(Source::Live);
    std::vector<uint8_t> ob = SerializeRecord(old);
    SceneTables oback;
    Check(ParseRecord(ob.data(), ob.size(), oback, err) && oback.ring_rows.empty() && oback.ring_errors == 0, "record.old_opens", "a record without the sections reads, with empty tables");
    SceneTables bad = t;
    bad.params.push_back(ParamValue{});
    bad.ring_rows[0].layout = 2;
    std::vector<uint8_t> bb = SerializeRecord(bad);
    SceneTables bback;
    err.clear();
    const bool parsed = ParseRecord(bb.data(), bb.size(), bback, err);
    Check(parsed && bback.ring_rows.empty() && bback.ring_nodes.empty() && bback.ring_errors == 1 && bback.params.size() == 1, "record.unknown_layout_dropped", parsed ? "the ring tables are dropped and counted; the params table is intact" : err);
    SceneTables oob = t;
    oob.header.source = uint8_t(Source::Live);
    oob.ring_rows[1].n_nodes = 60000;
    std::vector<uint8_t> ob2 = SerializeRecord(oob);
    SceneTables oback2;
    const bool p2 = ParseRecord(ob2.data(), ob2.size(), oback2, err);
    std::string verr;
    const bool v_bad = !rdr_scene::live::Validate(oob, verr);
    const std::string vmsg = verr;
    verr.clear();
    Check(p2 && oback2.ring_rows.empty() && oback2.ring_errors == 1 && v_bad && rdr_scene::live::Validate(t, verr), "record.node_range_checked", vmsg);
    // the null-edit self-test (LESSONS: an emulation needs one): the oracle replayed through the SAME pipeline with no edit equals the record (0 differences), and ONE flipped bit of ONE expected float is
    // detected (exactly one call differs): the comparison can see what it claims to compare.
    if (!records.empty() && !run.expect.empty()) {
      const SceneTables& rec = *records.back();
      uint64_t diffs0 = 0, diffs1 = 0, n_cmp = 0;
      for (int pass = 0; pass < 2; ++pass) {
        bool flipped = false;
        for (const Expect& e : run.expect) {
          if (e.frame != kFirst + kFrames - 1) continue;
          for (const RingRows& rr : rec.ring_rows) {
            if (rr.frame != e.frame || rr.object != e.object || rr.call != e.call) continue;
            std::vector<float> want = e.rows;
            if (pass == 1 && !flipped && !want.empty()) {
              uint32_t b;
              std::memcpy(&b, &want[5], 4);
              b ^= 1u;
              std::memcpy(&want[5], &b, 4);
              flipped = true;
            }
            bool same2 = true;
            for (uint32_t n = 0; n < rr.n_nodes; ++n)
              for (int j = 0; j < 12; ++j)
                if (rec.ring_nodes[rr.first_node + n].rows[j] != want[n * 12 + j]) same2 = false;
            ++n_cmp;
            if (!same2) (pass == 0 ? diffs0 : diffs1)++;
          }
        }
      }
      Check(diffs0 == 0 && diffs1 == 1 && n_cmp > 0, "selftest.null_edit", "unedited: " + std::to_string(diffs0) + " differences; one flipped bit: " + std::to_string(diffs1) + " of " + std::to_string(n_cmp / 2) + " calls");
    }
  }
  std::printf("ring_rows_test: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
