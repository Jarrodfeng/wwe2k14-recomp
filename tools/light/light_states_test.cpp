// light_states_test: the offline proof of lanes LS0-LS3 (Session 101, lane 109; docs\renderer\PER_DRAW_LIGHTS.md). 0 game runs, no SDK, no game memory.
//   light_states_test [--plant NAME] [--banks FILE]
// It runs the SHIPPED code: record_io (round trip of kScLightStates / kScLightRefs), the hook's tracker (src\scene_live\light_state_hook.h) against a fake
// guest memory, the contributor's Emit, and the rows builder (src\renderer_scene\light_state_rows.h). --banks FILE = the binary written by
// tools\light\light_states_selftest.py from a capture's wrestler draws (the stream's own banks); without it the bank checks are skipped and say so.
// Plants (each must make a NAMED check fail; the exit code is 1 when any check failed, so the selftest expects exit 1 and the check's name in the output):
//   slot0_always      the hook reads thread slot 0 whatever the caller            -> "hook.slot1_state"
//   no_compare        every call appends a new state                            -> "hook.dedupe_count"
//   stale_state       a slot never notices a change after its first state       -> "hook.install_between"
//   state_prev_object each ref points at the previous call's state              -> "hook.refs_exact"
//   w_dropped         colour w zeroed (today's record shape)                    -> "rows.c197_c199"
//   negate_dirs       directions negated                                        -> "rows.dif_lgh_vec"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/light_state_rows.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/light_state_contrib.h"

using namespace rdr_scene;
using namespace rdr_scene::live::lights;

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const char* why = "") {
  ++g_checks;
  std::printf("%s  %-26s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) ++g_fail;
}

// ---- a fake guest ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kG = 0x40000000, kArr = 0x41000000;
struct Guest {
  std::map<uint32_t, uint32_t> words;
  std::vector<uint8_t> recs = std::vector<uint8_t>(kMaxSlot * kRecordSize, 0);  // big-endian bytes, like the game
  void SetFloat(uint32_t slot, uint32_t off, float f) {
    uint32_t w;
    std::memcpy(&w, &f, 4);
    w = __builtin_bswap32(w);
    std::memcpy(&recs[slot * kRecordSize + off], &w, 4);
  }
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  bool ReadBytes(uint32_t a, void* dst, size_t n) const {
    if (a < kArr || uint64_t(a) + n > uint64_t(kArr) + recs.size()) return false;
    std::memcpy(dst, &recs[a - kArr], n);
    return true;
  }
  // a thread: r13 -> tls -> block -> slot index
  void Thread(uint32_t r13, uint32_t slot) {
    words[r13] = r13 + 0x1000;
    words[r13 + 0x1000 + 1048] = r13 + 0x2000;
    words[r13 + 0x2000 + 32] = slot;
  }
};

static void FillSlot(Guest& g, uint32_t slot, float seed) {
  for (int k = 0; k < 5; ++k)
    for (int c = 0; c < 4; ++c) {
      g.SetFloat(slot, 16 + 16 * k + 4 * c, seed + 0.1f * k + 0.01f * c);        // dirs
      g.SetFloat(slot, 96 + 16 * k + 4 * c, 2.f * seed + 0.2f * k + 0.02f * c);  // colours
    }
  for (int i = 0; i < 16; ++i) { g.SetFloat(slot, 912 + 4 * i, seed + i); g.SetFloat(slot, 976 + 4 * i, seed - i); g.SetFloat(slot, 1040 + 4 * i, seed * i); }
  for (int i = 0; i < 15; ++i) g.SetFloat(slot, 1104 + 4 * i, seed + 0.5f * i);
  for (int i = 0; i < 16; ++i) g.SetFloat(slot, 2144 + 4 * i, seed + 3 * i);
}

static void HookTests(Plant plant) {
  Guest g;
  g.words[kGlobalWord] = kG;
  g.words[kG + 4] = kArr;
  const float W = 1024.f, H = 512.f;
  uint32_t wb, hb;
  std::memcpy(&wb, &W, 4), std::memcpy(&hb, &H, 4);
  g.words[kG + 208] = wb, g.words[kG + 212] = hb;
  g.Thread(0x1000, 0);  // thread A runs slot 0
  g.Thread(0x5000, 1);  // thread B runs slot 1
  g.words[0x8000] = 0x9000, g.words[0x9000 + 1048] = 0;  // thread C: no recording-list block: the game uses slot 0 (review F1)
  FillSlot(g, 0, 1.f);
  FillSlot(g, 1, 7.f);
  const std::vector<uint8_t> r0_original(g.recs.begin(), g.recs.begin() + kRecordSize);

  LightStateContributor c;
  c.tracker.SetPlant(plant);
  c.tracker.SetLogBudget(3);
  std::vector<std::string> log;
  auto rw = [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); };
  auto rb = [&](uint32_t a, void* d, size_t n) { return g.ReadBytes(a, d, n); };
  auto lg = [&](const char* l) { log.push_back(l); };
  auto call = [&](uint32_t obj, uint32_t r13, uint64_t frame) { c.tracker.OnModelCall(obj, r13, frame, rw, rb, lg); };
  constexpr uint64_t F = 10;
  const uint32_t A = 0xA0, B = 0xB0, C = 0xC0, D = 0xD0, E = 0xE0;
  call(A, 0x1000, F);  // 0: slot 0, state R0
  call(A, 0x1000, F);  // 1: same object again (ordinal 1), same state
  call(B, 0x5000, F);  // 2: slot 1 (a different state)
  FillSlot(g, 0, 2.f);          // an install (sub_82BCC018) between two calls of slot 0
  call(C, 0x1000, F);  // 3: the new state
  std::memcpy(&g.recs[0], r0_original.data(), kRecordSize);  // the crowd's save / restore (sub_827FD698): R0 comes back
  call(D, 0x1000, F);  // 4: back to state 0's content: deduped to the first state
  g.SetFloat(0, 1104 + 8, 123.5f);  // a shadow-field change without an install
  call(E, 0x1000, F);  // 5: a fourth state
  call(E, 0x1000, F);  // 6
  call(0x96, 0x8000, F);   // 7: a null block = slot 0 (shadow-changed R0 now)
  call(0x99, 0x7777, F);   // an unreadable chain: no ref
  g.Thread(0x6000, 99);
  call(0x98, 0x6000, F);   // a slot index past kMaxSlot: refused
  call(0x97, 0x1000, F + 1);  // the next frame: closes frame F
  FrameBuffer fb;
  const bool took = c.tracker.TakeNext(fb);
  Check(took && fb.frame == F, "hook.ready_frame", "the closed frame is taken once");
  Check(!c.tracker.TakeNext(fb), "hook.take_once", "a second take gives nothing");
  const Stats st = c.tracker.GetStats();
  Check(st.unreadable == 1 && st.bad_slot == 1, "hook.guards", "an unreadable chain and a bad slot are counted, not recorded");
  Check(fb.states.size() == 4, "hook.dedupe_count", ("states in the frame: " + std::to_string(fb.states.size()) + " (want 4)").c_str());
  // refs: object, ordinal, expected state content
  struct Want { uint32_t obj, ord; uint32_t slot; int content; };  // content: 0 = R0 original, 1 = B's slot 1, 2 = installed, 3 = shadow changed
  const Want want[] = {{A, 0, 0, 0}, {A, 1, 0, 0}, {B, 0, 1, 1}, {C, 0, 0, 2}, {D, 0, 0, 0}, {E, 0, 0, 3}, {E, 1, 0, 3}};
  bool refs_ok = fb.refs.size() == 8;
  for (size_t i = 0; refs_ok && i < 7; ++i) {
    const LightRef& r = fb.refs[i];
    refs_ok = r.object == want[i].obj && r.ordinal == want[i].ord && r.state < fb.states.size();
  }
  Check(refs_ok, "hook.refs_shape", "8 refs: object and ordinal as called");
  Check(refs_ok && fb.refs[7].object == 0x96 && fb.refs[7].state == fb.refs[6].state && st.null_block == 1, "hook.null_block_slot0", "a null [[r13]+1048] reads slot 0, like sub_82BCBFD8");
  // exact content of each ref's state
  Guest g0 = g;  // expected contents rebuilt from a clean fake
  Guest ref;
  ref.recs = std::vector<uint8_t>(kMaxSlot * kRecordSize, 0);
  FillSlot(ref, 0, 1.f);
  FillSlot(ref, 1, 7.f);
  LightState content[4];
  DecodeRecord(&ref.recs[0], content[0]);
  DecodeRecord(&ref.recs[kRecordSize], content[1]);
  { Guest t; t.recs = ref.recs; FillSlot(t, 0, 2.f); DecodeRecord(&t.recs[0], content[2]); }
  { Guest t; t.recs = ref.recs; t.SetFloat(0, 1104 + 8, 123.5f); DecodeRecord(&t.recs[0], content[3]); }
  bool exact = refs_ok;
  std::string why;
  for (size_t i = 0; exact && i < 7; ++i) {
    const LightState& s = fb.states[fb.refs[i].state];
    if (std::memcmp(s.light, content[want[i].content].light, sizeof s.light) || std::memcmp(s.shadow_f, content[want[i].content].shadow_f, sizeof s.shadow_f) ||
        std::memcmp(s.shadow_m, content[want[i].content].shadow_m, sizeof s.shadow_m)) {
      exact = false;
      why = "ref " + std::to_string(i) + " reads the wrong state";
    }
  }
  Check(exact, "hook.refs_exact", why.c_str());
  Check(refs_ok && fb.states[fb.refs[2].state].slot == 1 && std::memcmp(fb.states[fb.refs[2].state].light, content[1].light, sizeof content[1].light) == 0,
        "hook.slot1_state", "a slot 1 caller gets slot 1's state");
  Check(refs_ok && fb.refs[3].state != fb.refs[0].state, "hook.install_between", "an install between two calls of a slot gives a new state");
  Check(refs_ok && fb.refs[4].state == fb.refs[0].state, "hook.restore_dedupes", "the restored light block is the first state again");
  Check(refs_ok && fb.refs[5].state != fb.refs[4].state, "hook.shadow_change", "a shadow-field change without an install is a new state");
  Check(fb.states.size() > 0 && (fb.states[0].flags & kLsMapKnown) && fb.states[0].map_wh[0] == W && fb.states[0].map_wh[1] == H, "hook.map_size", "G+208/+212 read");
  Check(log.size() == 3 && log[0].find("object 000000A0 ordinal 0 state 0") != std::string::npos, "hook.log_budget", "the first 3 calls log (object, ordinal, state)");
  (void)g0;

  // The contributor: the next record carries the tables; a record with them round-trips; the stale frame says so.
  call(0x97, 0x1000, F + 2);  // closes frame F+1 (one ref)
  SceneTables t;
  live::Counters cn;
  c.Emit(live::FrameInfo{}, t, cn);
  Check(t.light_states.size() == 1 && t.light_refs.size() == 1 && t.light_refs[0].object == 0x97 && t.light_refs[0].frame == F + 1, "contrib.emit", "Emit appends the last closed frame");
  SceneTables t2;
  live::Counters cn2;
  c.Emit(live::FrameInfo{}, t2, cn2);
  Check(t2.light_refs.empty() && cn2.stale == 1, "contrib.once", "no new frame: nothing appended, counted stale");
  std::string err;
  const bool valid = live::Validate(t, err);
  Check(valid, "contrib.validate", err.c_str());
  // The ring (review F3): six frames close before Emit runs. The four newest survive, oldest first, with rebased state indices; two are counted late.
  for (uint64_t fr = F + 3; fr <= F + 8; ++fr) call(0x40 + uint32_t(fr), 0x1000, fr);  // each call closes the previous frame (one ref each)
  SceneTables t3;
  live::Counters cn3;
  const uint64_t late_before = c.tracker.GetStats().late;
  c.Emit(live::FrameInfo{}, t3, cn3);
  bool ordered = t3.light_refs.size() == 4;
  for (size_t i = 1; ordered && i < t3.light_refs.size(); ++i) ordered = t3.light_refs[i].frame == t3.light_refs[i - 1].frame + 1;
  std::string verr;
  Check(ordered && live::Validate(t3, verr) && t3.light_states.size() == 4 && t3.light_refs[3].state == 3, "contrib.ring_order", ("4 frames emitted oldest first: " + verr).c_str());
  Check(late_before == 2 || c.tracker.GetStats().late >= 2, "contrib.late_counted", "closed frames overwritten before Emit are counted");
  std::string census;
  c.Census(census);
  Check(census.find("late ") != std::string::npos && census.find("unreadable 1") != std::string::npos, "contrib.census", census.c_str());
}

static LightState MakeState(float seed) {
  LightState s;
  std::memset(&s, 0, sizeof s);
  for (uint32_t i = 0; i < kLsLightFloats; ++i) s.light[i] = seed + float(i);
  for (int m = 0; m < 3; ++m)
    for (int i = 0; i < 16; ++i) s.shadow_m[m][i] = seed * (m + 1) + float(i);
  for (int i = 0; i < 16; ++i) s.shadow_f[i] = -seed + float(i);
  for (int b = 0; b < 2; ++b)
    for (int i = 0; i < 8; ++i) s.shadow_x[b][i] = 0.25f * seed + float(b * 8 + i);
  s.map_wh[0] = 1024.f, s.map_wh[1] = 512.f, s.slot = 1, s.flags = kLsMapKnown;
  return s;
}

static void RecordTests() {
  SceneTables t;
  t.header.guest_frame = 77;
  t.light_states = {MakeState(1.f), MakeState(2.f), MakeState(3.f)};
  t.light_refs = {{0xA0, 0, 0, 10}, {0xA0, 1, 0, 10}, {0xB0, 0, 2, 10}, {0xC0, 0, 1, 10}};
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
  Check(ok && back.light_states.size() == 3 && back.light_refs.size() == 4 &&
            std::memcmp(back.light_states.data(), t.light_states.data(), 3 * sizeof(LightState)) == 0 &&
            std::memcmp(back.light_refs.data(), t.light_refs.data(), 4 * sizeof(LightRef)) == 0,
        "record.round_trip", err.c_str());
  Check(ok && SerializeRecord(back) == bytes, "record.byte_identical", "write -> read -> write gives the same bytes");
  // an older record (no light sections) reads with empty tables
  SceneTables old;
  old.header.guest_frame = 5;
  SceneTables oldback;
  const std::vector<uint8_t> ob = SerializeRecord(old);
  Check(ParseRecord(ob.data(), ob.size(), oldback, err) && oldback.light_states.empty() && oldback.light_refs.empty(), "record.older_reads", "sections absent = no states");
  // an older READER skips the new sections: simulate by an unknown id (the reader's default branch) = the same path; here: a states-only record reads
  SceneTables only;
  only.light_states = {MakeState(4.f)};
  const std::vector<uint8_t> sb = SerializeRecord(only);
  SceneTables sback;
  Check(ParseRecord(sb.data(), sb.size(), sback, err) && sback.light_states.size() == 1 && sback.light_refs.empty(), "record.states_only", "refs are optional");
  // loud failures
  SceneTables bad = t;
  bad.light_refs.push_back({0xD0, 0, 9, 10});
  const std::vector<uint8_t> bb = SerializeRecord(bad);
  SceneTables bback;
  const bool rej = !ParseRecord(bb.data(), bb.size(), bback, err) && err.find("LightRef") != std::string::npos;
  Check(rej, "record.bad_ref_refused", err.c_str());
  err.clear();
  std::vector<uint8_t> tr(bytes.begin(), bytes.end() - 200);
  const bool trunc_rej = !ParseRecord(tr.data(), tr.size(), bback, err);
  Check(trunc_rej, "record.truncated_refused", err.c_str());
  std::vector<uint8_t> es = bytes;
  {  // element_size of the first light section changed
    SceneHeader h;
    std::memcpy(&h, es.data(), sizeof h);
    for (uint32_t i = 0; i < h.section_count; ++i) {
      SectionEntry e;
      std::memcpy(&e, es.data() + sizeof h + i * sizeof e, sizeof e);
      if (e.id == kScLightStates) { e.element_size += 4; std::memcpy(es.data() + sizeof h + i * sizeof e, &e, sizeof e); }
    }
  }
  const bool es_rej = !ParseRecord(es.data(), es.size(), bback, err) && err.find("element_size") != std::string::npos;
  Check(es_rej, "record.element_size_refused", err.c_str());
  Check(sizeof(LightState) == 1232 && sizeof(LightRef) == 16 && kScLightStates == 13 && kScLightRefs == 14, "record.ids_pinned", "section ids 13 / 14, element sizes 1232 / 16");
}

// ---- the rows builder ------------------------------------------------------------------------------------------------------------------
static bool Bits(const float* a, const float* b, int n) { return std::memcmp(a, b, size_t(n) * 4) == 0; }

static void HandRowsTests() {
  LightState s;
  std::memset(&s, 0, sizeof s);
  for (int m = 0; m < 3; ++m)
    for (int i = 0; i < 4; ++i) s.shadow_m[m][i * 5] = 1.f;  // identity
  s.map_wh[0] = s.map_wh[1] = 2.f, s.flags = kLsMapKnown;
  LightRows r = BuildLightRows(s);
  // bias(2, 2) = rows (0.5,0,0,0) (0,-0.5,0,0) (0,0,1,0) (0.75,0.25,0,1); identity matrices: P = bias; TexProjLight = its columns.
  const float c68[4] = {0.5f, 0, 0, 0.75f}, c69[4] = {0, -0.5f, 0, 0.25f}, c70[4] = {0, 0, 1.f, 0}, c71[4] = {0, 0, 0, 1.f};
  Check(r.tex_proj_valid && Bits(r.tex_proj_light[0], c68, 4) && Bits(r.tex_proj_light[1], c69, 4) && Bits(r.tex_proj_light[2], c70, 4) && Bits(r.tex_proj_light[3], c71, 4),
        "rows.tex_proj_hand", "identity matrices: the columns of bias(2, 2) (hand-checked; exact proof waits on a capture with tables)");
  for (int i = 0; i < 16; ++i) s.shadow_m[2][i] = (i % 5 == 0) ? (i == 15 ? 1.f : 2.f) : 0.f;  // diag(2, 2, 2, 1)
  r = BuildLightRows(s);
  const float d68[4] = {1.f, 0, 0, 1.5f}, d69[4] = {0, -1.f, 0, 0.5f}, d70[4] = {0, 0, 2.f, 0};
  Check(Bits(r.tex_proj_light[0], d68, 4) && Bits(r.tex_proj_light[1], d69, 4) && Bits(r.tex_proj_light[2], d70, 4), "rows.tex_proj_scaled", "diag(2,2,2,1) scales columns 0-2 (bias x S)");
  LightState nomap = s;
  nomap.flags = 0;
  Check(!BuildLightRows(nomap).tex_proj_valid, "rows.tex_proj_unknown_size", "no shadow-map size: not valid, never a guess");
  s.shadow_f[(1108 - 1104) / 4] = 11.f, s.shadow_f[0] = 12.f, s.shadow_f[(1112 - 1104) / 4] = 13.f, s.shadow_f[(1120 - 1104) / 4] = 14.f;
  r = BuildLightRows(s);
  const float prm0[4] = {11.f, 13.f, 12.f, 14.f};
  Check(Bits(r.shadow_prm[0], prm0, 4), "rows.shadow_prm_order", "row 0 = (+1108, +1112, +1104, +1120)");
}

struct BankRec { uint32_t col[5][4], dir[5][4], c197[4], c199[4], vs_c4[4], vs_c12[4]; uint32_t has; };  // has: 1 c197, 2 c199, 4 vs key
static std::vector<BankRec> LoadBanks(const char* path) {
  std::vector<BankRec> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) { std::printf("FAIL  banks.open               cannot open %s\n", path); ++g_fail; return v; }
  uint32_t n = 0;
  if (std::fread(&n, 4, 1, f) != 1) n = 0;
  v.resize(n);
  if (n && std::fread(v.data(), sizeof(BankRec), n, f) != n) v.clear();
  std::fclose(f);
  return v;
}
static bool BitsU(const float* a, const uint32_t* b, int n) { return std::memcmp(a, b, size_t(n) * 4) == 0; }

static void BankTests(const char* path, Plant plant, const std::string& rplant) {
  const std::vector<BankRec> v = LoadBanks(path);
  Check(v.size() > 100, "banks.count", (std::to_string(v.size()) + " wrestler draws (want > 100)").c_str());
  size_t n197 = 0, n199 = 0, nkey = 0, bad_vec = 0, bad_col = 0, bad_max = 0, bad_maxa = 0, bad_key = 0;
  for (const BankRec& b : v) {
    LightState s;
    std::memset(&s, 0, sizeof s);
    for (int k = 0; k < 5; ++k) {
      std::memcpy(&s.light[kLsDir + 4 * k], b.dir[k], 16);
      std::memcpy(&s.light[kLsCol + 4 * k], b.col[k], 16);
      if (rplant == "w_dropped") s.light[kLsCol + 4 * k + 3] = 0.f;           // planted: colour w lost (the record's shape today)
      if (rplant == "negate_dirs") for (int c = 0; c < 3; ++c) s.light[kLsDir + 4 * k + c] *= -1.f;
    }
    const LightRows r = BuildLightRows(s);
    for (int k = 0; k < 5; ++k) {
      bad_vec += !BitsU(r.dif_lgh_vec[k], b.dir[k], 4);
      bad_col += !BitsU(r.dif_lgh_col[k], b.col[k], 4);
    }
    if (b.has & 1) { ++n197; bad_max += !BitsU(r.dif_lgh_max, b.c197, 4); }
    if (b.has & 2) { ++n199; bad_maxa += !BitsU(r.dif_lgh_max_a, b.c199, 4); }
    if (b.has & 4) { ++nkey; bad_key += !(BitsU(r.key_light_vec, b.vs_c12, 4) && BitsU(r.key_light_color, b.vs_c4, 4)); }
  }
  char m[200];
  std::snprintf(m, sizeof m, "c197 %zu/%zu c199 %zu/%zu bit-equal", n197 - bad_max, n197, n199 - bad_maxa, n199);
  Check(n197 > 50 && n199 > 10 && bad_max == 0 && bad_maxa == 0, "rows.c197_c199", m);
  std::snprintf(m, sizeof m, "%zu/%zu key-light draws bit-equal to VS c12 / c4", nkey - bad_key, nkey);
  Check(nkey > 50 && bad_key == 0, "rows.key_light", m);
  std::snprintf(m, sizeof m, "%zu of %zu direction rows differ", bad_vec, v.size() * 5);
  Check(bad_vec == 0, "rows.dif_lgh_vec", m);
  std::snprintf(m, sizeof m, "%zu of %zu colour rows differ", bad_col, v.size() * 5);
  Check(bad_col == 0, "rows.dif_lgh_col", m);
  (void)plant;
}

// --write-rsc PATH: a synthetic record (2 states, 3 refs) at PATH and the rows the C++ builder makes of each state at PATH.rows (floats), for the Python port's
// bit-for-bit comparison (tools\light\light_states_selftest.py).
static void WriteRscAndRows(const std::string& path) {
  SceneTables t;
  t.header.guest_frame = 5;
  LightState a = MakeState(0.37f), b = MakeState(1.9f);
  for (int i = 0; i < 16; ++i) { a.shadow_m[0][i] = 0.1f * float(i % 7) - 0.3f; a.shadow_m[2][i] = 0.07f * float(i % 5) + 0.01f; }
  b.flags = 0;  // the second state has no shadow-map size
  t.light_states = {a, b};
  t.light_refs = {{0x100, 0, 0, 5}, {0x100, 1, 1, 5}, {0x200, 0, 1, 5}};
  std::string err;
  if (!WriteRecord(path, t, err)) { std::printf("FAIL  write_rsc                 %s\n", err.c_str()); ++g_fail; return; }
  FILE* f = std::fopen((path + ".rows").c_str(), "wb");
  if (!f) { std::printf("FAIL  write_rows               cannot open\n"); ++g_fail; return; }
  for (const LightState& s : t.light_states) {
    const LightRows r = BuildLightRows(s);
    const float valid = r.tex_proj_valid ? 1.f : 0.f;
    std::fwrite(r.dif_lgh_vec, 4, 20, f), std::fwrite(r.dif_lgh_col, 4, 20, f), std::fwrite(r.key_light_vec, 4, 4, f), std::fwrite(r.key_light_color, 4, 4, f);
    std::fwrite(r.dif_lgh_max, 4, 4, f), std::fwrite(r.dif_lgh_max_a, 4, 4, f), std::fwrite(r.block_a, 4, 4, f), std::fwrite(r.block_b, 4, 4, f);
    std::fwrite(&valid, 4, 1, f), std::fwrite(r.tex_proj_light, 4, 16, f), std::fwrite(r.shadow_prm, 4, 12, f);
    std::fwrite(r.shadow_view_t, 4, 16, f), std::fwrite(r.light_view_t, 4, 16, f), std::fwrite(r.shadow_x, 4, 16, f);
  }
  std::fclose(f);
}

int main(int argc, char** argv) {
  std::string plant, banks, write_rsc;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) plant = argv[++i];
    else if (!std::strcmp(argv[i], "--banks") && i + 1 < argc) banks = argv[++i];
    else if (!std::strcmp(argv[i], "--write-rsc") && i + 1 < argc) write_rsc = argv[++i];
    else { std::printf("FAIL  usage                    light_states_test [--plant NAME] [--banks FILE]\n"); return 2; }
  }
  Plant hp = Plant::None;
  if (plant == "slot0_always") hp = Plant::Slot0Always;
  else if (plant == "no_compare") hp = Plant::NoCompare;
  else if (plant == "stale_state") hp = Plant::StaleState;
  else if (plant == "state_prev_object") hp = Plant::StatePrevObject;
  else if (!plant.empty() && plant != "w_dropped" && plant != "negate_dirs") { std::printf("FAIL  plant                    unknown plant %s\n", plant.c_str()); return 2; }
  RecordTests();
  HookTests(hp);
  HandRowsTests();
  if (!banks.empty()) BankTests(banks.c_str(), hp, plant);
  else std::printf("SKIP  rows.bank_proof           no --banks FILE (the stream's banks): the derived-rule proof did not run\n");
  if (!write_rsc.empty()) WriteRscAndRows(write_rsc);
  std::printf("light_states_test: %s (%d checks)\n", g_fail ? "FAIL" : "PASS", g_checks);
  return g_fail ? 1 : 0;
}
