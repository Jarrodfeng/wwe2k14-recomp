// a6_unit: the offline proof of S103 lane A6 (bits 4 and 7 read from memory at the latch; docs\renderer\INPUTS_A6.md). 0 game runs, no SDK, no game memory.
//   a6_unit [--plant NAME]
// It runs the SHIPPED code: src\scene_live\light_latch.h (the predictors), light_latch_board.h (the two compares and the board the lights contributor's Emit reads) and Table::Find / OnLatchEntry of
// light_index_core.h, against a fake big-endian guest built by hand from the offsets the listings give (INPUTS_A6.md section 2), never from the reader's own tables.
// A plant is a FAULT in the thing under test (the reference or the fake guest); each must make its NAMED check fail (exit code 1 when any check failed):
//   a6_embedded_2736  the fake guest keeps the wrestler's embedded object at char + 2736 (the reader: + 3936)    -> "a6.wrestler_embedded"
//   a6_skeleton_52    the reader takes the model from [char + 52]                                                -> "a6.wrestler_embedded"
//   a6_gate_ignored   the gate rule is not applied (a gated wrestler is resolved through the override word)        -> "a6.wrestler_gated_embedded"
//   a6_ring_swapped   the ropes and the fixtures read each other's index word                                    -> "a6.ring_indices"
//   a6_stale_counted  an entry the walk did not confirm this frame is compared as if it had been                  -> "a6.compare_stale_not_counted"
//   a6_state_prefix   only the first 100 words of the block are compared                                        -> "a6.state_difference_named"
//   a6_no_feed        the latch entries are not fed to the table                                                 -> "a6.latch_entry_covers"
//   a6_unchanged_counted the state compare also counts a prediction whose block did not change                       -> "a6.state_unchanged_not_counted"
//   a6_flag_ignored   the manager row's kLoBlockRead flag is not required (a failed word read takes the embedded path) -> "a6.wrestler_manager_unread"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/scene_live/light_latch.h"

using namespace rdr_scene;
namespace ll = rdr_scene::live::lightlatch;
namespace li = rdr_scene::live::lightidx;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-36s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;   // guest word address -> value (host order)
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t be = scene_inputs::Swap32(it->second);   // the guest is big-endian
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
};
static Guest g;
static bool ReadThunk(uint32_t a, void* d, size_t n) { return g.Read(a, d, n); }

static uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
// a block of 224 words whose content names the object (so two objects never compare equal by accident)
static void Fill(uint32_t* w, uint32_t seed) {
  for (uint32_t i = 0; i < kLsLightFloats; ++i) w[i] = Bits(float(seed) + 0.25f * float(i));
}
static void PutBlock(uint32_t addr, uint32_t seed) {
  uint32_t w[kLsLightFloats];
  Fill(w, seed);
  for (uint32_t i = 0; i < kLsLightFloats; ++i) g.Put(addr + 4 * i, w[i]);
}

// the manager row + one block row per set: set i's object lives at 0x64000000 + 0x1000 * i, its block (object + 48) is pattern 1000 + i
constexpr uint32_t kSetObj0 = 0x64000000, kSetStride = 0x1000, kSets = 8;
static std::vector<LightObjectRow> BuildRows(uint32_t ov0, uint32_t ov1, uint32_t gate, uint32_t gate_flag) {
  std::vector<LightObjectRow> rows(1 + kSets);
  LightObjectRow& m = rows[0];
  m.kind = kLoManager, m.flags = kLoBlockRead, m.aux0 = kSets, m.aux1 = 0, m.remap = -1;
  m.words[kLmOverrideMode0] = ov0, m.words[kLmOverrideMode1] = ov1, m.words[kLmGateObject] = gate, m.words[kLmGateFlag] = gate_flag;
  for (uint32_t k = 0; k < kLmRefCount; ++k) m.words[kLmRefBase + k] = kLightNoRow;
  for (uint32_t i = 0; i < kSets; ++i) {
    LightObjectRow& r = rows[1 + i];
    r.kind = kLoSet, r.index = i, r.object = kSetObj0 + kSetStride * i, r.flags = kLoBlockRead;
    Fill(r.words, 1000 + i);
    m.words[kLmRefBase + i] = 1 + i;
    m.words[kLmSetInfoBase + i] = 100 + i;   // id; no pool remap
  }
  return rows;
}

constexpr uint32_t kCh0 = 0x60001000, kCh1 = 0x60010000, kSkel0 = 0x61000000, kSkel1 = 0x61001000;
static void BuildGuest() {
  g = Guest{};
  g.Put(ll::kCharSlotTable + 0, kCh0);
  g.Put(ll::kCharSlotTable + 4, 0);          // an empty slot
  g.Put(ll::kCharSlotTable + 8, kCh1);
  for (uint32_t s = 3; s < ll::kCharSlots; ++s) g.Put(ll::kCharSlotTable + 4 * s, 0);
  g.Put(kCh0 + 48, kSkel0);
  g.Put(kCh1 + 48, kSkel1);
  const uint32_t emb = Plant("a6_embedded_2736") ? 2736 : 3936;   // the node's embedded object; its block is object + 48
  PutBlock(kCh0 + emb + 48, 5000);
  PutBlock(kCh1 + emb + 48, 6000);
  // the ring: G = word 0x836F4C74, the models at G + off, the index words at X + 9584 / 9588 and Y(=X) + 1232 / 1236
  constexpr uint32_t kG = 0x62000000, kX = 0x63000000;
  g.Put(ll::kRingGlobal, kG);
  const uint32_t offs[] = {20, 24, 36, 40, 44, 48, 52, 56, 332, 336, 340, 344};
  for (uint32_t i = 0; i < 12; ++i) g.Put(kG + offs[i], 0x65000000 + 0x100 * i);
  g.Put(kX + 9584, 3);    // ropes
  g.Put(kX + 9588, 5);    // posts / turnbuckles / covers
  g.Put(kX + 1232, 1);    // canvas
  g.Put(kX + 1236, 2);    // skirts
  for (uint32_t i = 0; i < kSets; ++i) PutBlock(kSetObj0 + kSetStride * i + 48, 1000 + i);   // the sets' blocks in guest memory (the table's object addresses point here)
}
constexpr uint32_t kX = 0x63000000;

static const ll::Pred* FindPred(const std::vector<ll::Pred>& v, uint32_t model) {
  for (const ll::Pred& p : v)
    if (p.model == model) return &p;
  return nullptr;
}
static bool BlockIs(const ll::Pred& p, uint32_t seed) {
  uint32_t w[kLsLightFloats];
  Fill(w, seed);
  return std::memcmp(w, p.block, sizeof w) == 0;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) g_plant = argv[++i];
  BuildGuest();
  ll::LatchFaults ft;
  ft.skeleton_52 = Plant("a6_skeleton_52");
  ft.ignore_gate = Plant("a6_gate_ignored");
  ft.swap_ring_words = Plant("a6_ring_swapped");
  ft.count_stale = Plant("a6_stale_counted");
  ft.ignore_block_flag = Plant("a6_flag_ignored");
  const ll::LatchFaults* f = &ft;

  // ---- wrestlers: the node's EMBEDDED object (ungated, no override word): the model is the skeleton [char + 48], the source is char + 3936 + 48, the install carries no index
  {
    const auto rows = BuildRows(0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0);
    std::vector<ll::Pred> out;
    ll::Stats st;
    ll::PredictWrestlers(ReadThunk, rows, out, st, f);
    const ll::Pred* a = FindPred(out, kSkel0);
    const ll::Pred* b = FindPred(out, kSkel1);
    const bool ok = out.size() == 2 && a && b && a->cls == ll::kWrestler && a->source == kCh0 + 3936 + 48 && a->source_exact && a->index == li::kNoIndex && a->mode == li::kNoIndex && BlockIs(*a, 5000) && BlockIs(*b, 6000) &&
                    b->source == kCh1 + 3984;
    Check(ok, "a6.wrestler_embedded", "preds " + std::to_string(out.size()) + " unreadable " + std::to_string(st.unreadable[ll::kWrestler]));
  }
  // ---- a manager row whose override / gate words were not all read (no kLoBlockRead): w0 = 0xFFFFFFFF would take the embedded path silently; no prediction, counted unresolved
  {
    auto rows = BuildRows(2, 0xFFFFFFFFu, 0, 0);
    rows[0].flags = 0;
    std::vector<ll::Pred> out;
    ll::Stats st;
    ll::PredictWrestlers(ReadThunk, rows, out, st, f);
    Check(out.empty() && st.unresolved[ll::kWrestler] == 1, "a6.wrestler_manager_unread", "preds " + std::to_string(out.size()));
  }
  // ---- wrestlers through the override word: ungated, mode-0 word = 2 -> sub_826203E8(-1, 0) resolves set 2 (index raw 0xFFFFFFFF, mode 0)
  {
    const auto rows = BuildRows(2, 0xFFFFFFFFu, 0, 0);
    std::vector<ll::Pred> out;
    ll::Stats st;
    ll::PredictWrestlers(ReadThunk, rows, out, st);
    const ll::Pred* a = FindPred(out, kSkel0);
    Check(a && a->index == 0xFFFFFFFFu && a->mode == 0 && !a->source_exact && a->source == kSetObj0 + 2 * kSetStride + 48 && BlockIs(*a, 1002), "a6.wrestler_override_set", "preds " + std::to_string(out.size()));
  }
  // ---- gated (the gate object non-null with [+152] != 0): the override word is skipped and the embedded object is installed, even with a valid override word
  {
    const auto rows = BuildRows(2, 0xFFFFFFFFu, 0x66000000, 1);
    std::vector<ll::Pred> out;
    ll::Stats st;
    ll::PredictWrestlers(ReadThunk, rows, out, st, f);
    const ll::Pred* a = FindPred(out, kSkel0);
    Check(a && a->source_exact && a->index == li::kNoIndex && BlockIs(*a, 5000), "a6.wrestler_gated_embedded");
  }
  // ---- the ring: ropes [X+9584], fixtures [X+9588], canvas [Y+1232], skirts [Y+1236], mode 1; the index resolves through the manager (set i = index i)
  std::vector<ll::Pred> ring;
  {
    const auto rows = BuildRows(0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0);
    ll::Stats st;
    ll::PredictRing(ReadThunk, rows, kX, ring, st, f);
    const ll::Pred* rope = FindPred(ring, 0x65000000);
    const ll::Pred* fix = FindPred(ring, 0x65000200);       // G + 44 (the turnbuckle)
    const ll::Pred* canvas = FindPred(ring, 0x65000700);    // G + 56
    const ll::Pred* skirt = FindPred(ring, 0x65000B00);     // G + 344
    const bool ok = ring.size() == 12 && rope && fix && canvas && skirt && rope->index == 3 && rope->mode == 1 && BlockIs(*rope, 1003) && fix->index == 5 && BlockIs(*fix, 1005) && canvas->index == 1 && BlockIs(*canvas, 1001) &&
                    skirt->index == 2 && BlockIs(*skirt, 1002);
    Check(ok, "a6.ring_indices", "preds " + std::to_string(ring.size()));
    // a ring that was not noted (the task is not running) predicts nothing and says so
    std::vector<ll::Pred> none;
    ll::Stats s2;
    ll::PredictRing(ReadThunk, rows, 0, none, s2);
    Check(none.empty() && s2.ring_unnoted == 1, "a6.ring_unnoted_counted");
    // an index < 0 installs nothing (sub_826203E8 0x82620474): no prediction, counted
    g.Put(kX + 9588, 0xFFFFFFFFu);
    std::vector<ll::Pred> neg;
    ll::Stats s3;
    ll::PredictRing(ReadThunk, rows, kX, neg, s3);
    Check(neg.size() == 7 && s3.no_install[ll::kFixture] == 5, "a6.ring_negative_installs_nothing", "preds " + std::to_string(neg.size()));
    g.Put(kX + 9588, 5);
  }

  // ---- the index compare: only entries the walk confirmed THIS frame are compared; stale ones are counted apart, never as equal
  {
    li::Table t;
    const uint32_t hook = 1000;
    // the fixture model G+44 under set 5 (correct); the rope G+20 under set 4 (wrong); the canvas G+56 confirmed 500 frames ago; the first skirt has no entry
    auto install = [&](uint32_t model, uint32_t set, uint32_t frame) {
      t.OnInstall(0, set, 1);
      t.OnInstallObject(0, kSetObj0 + kSetStride * set + 48);
      t.OnModel(model, 0, frame);
    };
    install(0x65000200, 5, hook);
    install(0x65000000, 4, hook);
    install(0x65000700, 1, hook - 500);
    for (ll::Pred& q : ring) q.changed = true;   // the block compare counts changed predictions only
    ll::Stats cmp;
    ll::CompareIndex(
        ring, hook, [&](uint32_t m, li::NodeLight& e) { return t.Find(m, e); }, [&](uint32_t a, uint32_t* w) { return scene_inputs::ReadLightBlockAt(ReadThunk, a, w); }, cmp, f);
    Check(cmp.idx_eq[ll::kFixture] == 1 && cmp.idx_diff[ll::kRope] == 1, "a6.compare_equal_and_differ", "fixture eq " + std::to_string(cmp.idx_eq[ll::kFixture]) + " rope diff " + std::to_string(cmp.idx_diff[ll::kRope]));
    Check(cmp.idx_not_walked[ll::kCanvas] == 1 && cmp.idx_eq[ll::kCanvas] == 0 && cmp.idx_diff[ll::kCanvas] == 0, "a6.compare_stale_not_counted");
    Check(cmp.idx_no_entry[ll::kSkirt] == 4 && cmp.first_idx[0] != 0, "a6.compare_no_entry_and_first", std::string(cmp.first_idx));
    // the block at the table's object address equals the predicted block for the correct entry, differs for the wrong one
    Check(cmp.blk_eq[ll::kFixture] == 1 && cmp.blk_diff[ll::kRope] == 1, "a6.compare_block_at_object");
  }

  // ---- the state compare: the walk's LightState against the block read at the latch, bit for bit, per delta; a difference names its first word
  {
    ll::Pred p;
    p.cls = ll::kWrestler, p.model = kSkel0, p.have_block = true, p.changed = true;
    Fill(p.block, 5000);
    std::vector<LightState> states(2);
    std::memcpy(states[0].light, p.block, sizeof p.block);
    std::memcpy(states[1].light, p.block, sizeof p.block);
    uint32_t tmp = 0;
    std::memcpy(&tmp, &p.block[200], 4);
    tmp ^= 1u;   // one bit of word 200, beyond a 100-word prefix
    std::memcpy(&states[1].light[200], &tmp, 4);
    std::vector<LightRef> refs = {{kSkel0, 0, 0, 100}, {kSkel0, 1, 1, 101}};   // frame 100: delta 0, the same words; frame 101: delta +1, word 200 differs
    ll::Stats st;
    ll::LatchFaults fs;
    fs.compare_words = Plant("a6_state_prefix") ? 100 : kLsLightFloats;
    fs.count_unchanged = Plant("a6_unchanged_counted");
    std::vector<ll::Pred> one = {p};
    ll::CompareStates(one, 100, states.data(), 2, refs.data(), 2, st, &fs);
    Check(st.st_eq[ll::kWrestler][1] == 1, "a6.state_equal_delta0");
    const bool named = st.st_diff[ll::kWrestler][2] == 1 && std::string(st.first_state).find("first word 200") != std::string::npos;
    Check(named, "a6.state_difference_named", st.first_state);
    // a prediction whose block did not change since the previous latch proves nothing about staleness: not counted
    ll::Stats su;
    std::vector<ll::Pred> still = one;
    still[0].changed = false;
    ll::CompareStates(still, 100, states.data(), 2, refs.data(), 2, su, &fs);
    Check(su.st_eq[ll::kWrestler][1] + su.st_diff[ll::kWrestler][2] == 0, "a6.state_unchanged_not_counted");
    // Board::Mark: no baseline = not changed; the same block again = not changed; one float moved = changed; a dup hook is seen and counted
    ll::Board mb;
    std::vector<ll::Pred> m1 = one;
    mb.Mark(m1);
    Check(!m1[0].changed, "a6.mark_first_not_changed");
    mb.Put(100, std::move(m1), ll::Stats{});
    std::vector<ll::Pred> m2 = one;
    mb.Mark(m2);
    const bool same_again = !m2[0].changed;
    m2[0].block[7] ^= 1u;
    mb.Mark(m2);
    Check(same_again && m2[0].changed && mb.HasHook(100) && !mb.HasHook(101) && mb.Get().dup_hooks == 1, "a6.mark_changed_and_dup_hook");
    std::vector<ll::Pred> m3 = one;
    mb.NoteNoPred(m3, std::vector<uint32_t>{kSkel0, 0x61999999u});
    Check(mb.Get().emitted_no_pred == 1 && std::string(mb.Get().first_no_pred) == "61999999", "a6.no_prediction_counted", mb.Get().first_no_pred);
    // another model's ref, or a frame two away, is never paired
    std::vector<LightRef> far = {{kSkel1, 0, 0, 100}, {kSkel0, 0, 0, 103}};
    ll::Stats s2;
    ll::CompareStates(one, 100, states.data(), 2, far.data(), 2, s2);
    uint64_t any = 0;
    for (int d = 0; d < 3; ++d) any += s2.st_eq[ll::kWrestler][d] + s2.st_diff[ll::kWrestler][d];
    Check(any == 0, "a6.state_unpaired_ignored");
    // the board pairs a closed walk frame with the latch of the hook frame it carries (delta 0 and +1), and counts nothing for a latch more than one frame away
    ll::Board board;
    std::vector<ll::Pred> keep = one;
    board.Put(100, std::move(keep), ll::Stats{});
    board.OnWalkFrame(100, states.data(), 2u, refs.data(), 1u);
    board.OnWalkFrame(500, states.data(), 2u, refs.data(), 2u);
    const ll::Stats bs = board.Get();
    Check(bs.st_eq[ll::kWrestler][1] == 1 && bs.st_diff[ll::kWrestler][1] + bs.st_diff[ll::kWrestler][0] + bs.st_diff[ll::kWrestler][2] == 0, "a6.board_pairs_by_hook_frame", board.Census().substr(0, 140));
  }

  // ---- feeding the predictions to the table (kLatchEntries, off in the shipped header): an OURS frame then has an entry for every asked model, and the entry never ages out while it is re-asserted
  {
    li::Table t;
    const uint32_t hook = 5000;
    if (!Plant("a6_no_feed"))
      for (const ll::Pred& p : ring) t.OnLatchEntry(p.model, p.source, p.index, p.mode, hook);
    std::vector<li::NodeLight> snap;
    t.Snapshot(hook, snap);
    std::vector<uint32_t> models;
    for (const ll::Pred& p : ring) models.push_back(p.model);
    std::sort(models.begin(), models.end());
    const li::Cover c = li::Coverage(snap, models, hook);
    Check(c.ok() && c.drawn == 12 && t.LatchEntries() == 12, "a6.latch_entry_covers", "missing " + std::to_string(c.missing) + " fed " + std::to_string((unsigned long long)t.LatchEntries()));
    t.OnLatchEntry(ring[0].model, ring[0].source, ring[0].index, ring[0].mode, hook + 1);   // re-asserted next latch
    li::NodeLight e;
    Check(t.Find(ring[0].model, e) && e.frame == hook + 1, "a6.latch_entry_reasserted");
  }
  Check(!ll::kLatchEntries, "a6.feed_switch_off_by_default");

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
