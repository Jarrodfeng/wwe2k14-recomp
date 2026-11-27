// floor_blood_test: the offline proof of the S111 BLOODMOD permanent blood mod (docs\research\s111\BLOODMOD.md, BLOODMOD2.md). 0 game runs, no SDK, no game memory: it runs the SHIPPED store
// (src\renderer_scene\floor_blood_keep.h floor_blood::Apply), the blood rows' identity (src\scene_live\blood_slot_id.h, the tracker's code, on a made-up pool) and the plan (weapon_plan.h Plan) on made-up records.
//   floor_blood_test [--plant drop_expired | match_rows | ignore_row_pool | replay_as_own]   each plant must make the run FAIL (exit 1):
//     drop_expired     a kept splat goes once the game's call for it is gone (the store following the game's timer)
//     match_rows       the BLOODMOD landing's sameness: every row float within 0.1 instead of the splat's identity
//     ignore_row_pool  a row recorded under the old pool count is kept into the new scene's store (BLOODMOD review finding 3)
//     replay_as_own    a record the replay track wrote is given its live particle's identity (BLOODMOD review finding 1)
//     flip_angle       (BLOODMOD3) a held splat's rows turned the other way round (the y turn's sign)
//     draw_held        (BLOODMOD3) the plan draws a held row (made from a live particle) as itself
//   keep.pass_through   a growing splat (alpha 1) and a weapon: the record's own view comes back, nothing is kept
//   keep.fade_kept      the first fading call (0 < alpha < 1) is kept at alpha 1 with its rows; the live fading call is not drawn twice
//   keep.rows_rebased   the record's other calls keep their own node rows behind the kept ones
//   keep.plan_items     the plan makes one blood item of the kept splat, its call at alpha 1
//   keep.no_duplicate   the same splat fading on (its height moved 0.03 by the floor word) is not kept twice
//   keep.after_expiry   the game's splat freed (no call): the kept one still draws                                             <- drop_expired
//   keep.order          the kept splats come first (stable pose slots), then the live growing splat, then the weapon
//   keep.no_cap         45 more splats that each fade and go: all 46 stay drawn, past the game's 30                            <- drop_expired
//   keep.new_scene      a new pool (the game's new scene) empties the store; so does another arena
//   keep.empty_record   a record with no calls at all still draws the kept splats
//   keep.long_pause     the same fading call 1300 guest frames later (the game's pause stops the pool's update, the frames go on) is still the same splat: not kept twice
//   keep.same_place     the slot's next splat landing on the first one's spot (rows within 0.1) is a new splat: both kept           <- match_rows
//   keep.old_pool_row   a fading row stamped with the old pool count, drawn after the game made the new pool: not kept           <- ignore_row_pool
//   keep.no_identity    a fading row with no identity (a played-back record) is drawn as the game drew it, never kept
//   id.one_life         one splat keeps one identity from its spawn (state 0) through its fade (state 3), stamped with the pool count
//   id.new_splat        the slot freed (state 4) and spawned again (state 0): a new identity
//   id.replayed_row     the replay word set, record 0 written from the recording (the freed splat, alpha 0.4): no identity, nothing kept <- replay_as_own
//   id.after_replay     the word cleared, the record still the played-back one (alpha bits differ from the particle's): no identity
//   id.next_splat_kept  the slot's new splat fades (its own record): kept as a second splat
//   id.new_count        the scene count moves (a match restart): the same particle gets a new identity under the new count
//   held.synth_rows     (BLOODMOD3) the rows made from another drawn splat equal the game's composition (a made-up inverse bind with a turn and an offset) <- flip_angle
//   held.kept           (BLOODMOD3) a held row is kept once at alpha 1 and drawn as the kept splat (no held bit); the same splat's own fading row later is not kept again
//   held.never_drawn    (BLOODMOD3) a held row the store cannot keep (an old pool count) is not a plan item                                               <- draw_held
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/floor_blood_keep.h"
#include "../../src/scene_live/blood_slot_id.h"
#include "../../src/scene_live/blood_held.h"

using namespace rdr_scene;
namespace W = rdr_scene::weapons;
namespace FB = rdr_scene::floor_blood;
namespace BI = rdr_scene::live::bloodid;
namespace BH = rdr_scene::live::bloodheld;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-20s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// A record's sections 60 / 61: calls and their node rows (12 floats a node).
struct Rec {
  std::vector<W::WeaponCall> calls;
  std::vector<float> nodes;
  W::View Get() const {
    W::View v;
    v.calls = calls.data(), v.n_calls = calls.size();
    v.nodes = nodes.data(), v.n_nodes = nodes.size() / 12;
    return v;
  }
  // One splat call as the tracker writes it with the mod on: particle slot k in slot_flags bits 0-7, identity `identity` in instance (weapon_rows.h BloodIdentity; 0 = none), texture tex, colour `colour`,
  // one node: the world row at (x, y, z), y angle a, scale sc.
  void Add(uint32_t frame, uint32_t identity, uint32_t k, float x, float y, float z, float a, float sc, float alpha, uint32_t tex, float colour) {
    W::WeaponCall c;
    c.object = 0x40001000, c.instance = identity, c.frame = frame, c.id = W::BloodIdOf(colour), c.slot_flags = W::PackBloodFlags(tex) | k, c.alpha = alpha;
    c.node_first = uint32_t(nodes.size() / 12), c.node_count = 1;
    const float cs = std::cos(a) * sc, sn = std::sin(a) * sc;
    const float r[12] = {cs, 0.f, sn, x, 0.f, 1.f, 0.f, y, -sn, 0.f, cs, z};
    nodes.insert(nodes.end(), r, r + 12);
    calls.push_back(c);
  }
  // A splat of colour 0.45 (one in its fade): serial `serial` in particle slot k, recorded under pool count `pools`.
  void Splat(uint32_t frame, uint32_t serial, uint32_t k, float x, float y, float z, float a, float sc, float alpha, uint32_t tex = 1, uint32_t pools = 1) {
    Add(frame, W::BloodIdentity(pools, serial), k, x, y, z, a, sc, alpha, tex, 0.45f);
  }
  // One plain weapon call (id, two nodes of made-up rows).
  void Weapon(uint32_t frame, int32_t id, float seed) {
    W::WeaponCall c;
    c.object = 0x40002000, c.frame = frame, c.id = id, c.slot_flags = W::PackSlotFlags(0, false, true), c.alpha = 1.f;
    c.node_first = uint32_t(nodes.size() / 12), c.node_count = 2;
    for (int i = 0; i < 24; ++i) nodes.push_back(seed + float(i));
    calls.push_back(c);
  }
};

static size_t CountBlood(const W::View& v) {
  size_t n = 0;
  for (size_t i = 0; i < v.n_calls; ++i) n += W::IsBloodCall(v.calls[i].slot_flags) ? 1u : 0u;
  return n;
}

// A made-up pool for blood_slot_id.h: Q's 30 particles (state, alpha) and records (alpha), the replay word.
struct Pool {
  static constexpr uint32_t kQ = 0x40100000;
  std::map<uint32_t, uint32_t> words;
  Pool() {
    words[W::kBloodPoolGlobal] = kQ, words[kQ + W::kBloodPoolOffReplay] = 0;
    for (uint32_t k = 0; k < W::kBloodRecCount; ++k) Set(k, W::kBloodStateFree, 0.f, 0.f);
  }
  static uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
  }
  static uint32_t Part(uint32_t k, uint32_t off) { return kQ + W::kBloodPartOff + W::kBloodPartStride * k + off; }
  static uint32_t RecAlpha(uint32_t k) { return kQ + W::kBloodRecOff + W::kBloodRecStride * k + W::kBloodRecOffAlpha; }
  void Set(uint32_t k, uint32_t state, float part_alpha, float rec_alpha) {
    words[Part(k, W::kBloodPartOffState)] = state, words[Part(k, W::kBloodPartOffAlpha)] = Bits(part_alpha), words[RecAlpha(k)] = Bits(rec_alpha);
  }
  bool Read(uint32_t a, uint32_t& v) const {
    const auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
};

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  static const char* const kPlants[] = {"drop_expired", "match_rows", "ignore_row_pool", "replay_as_own", "flip_angle", "draw_held"};
  bool known = plant.empty();
  for (const char* k : kPlants) known = known || plant == k;
  if (!known) {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }
  FB::Plants fp;
  fp.drop_expired = plant == "drop_expired", fp.match_rows = plant == "match_rows", fp.ignore_row_pool = plant == "ignore_row_pool";
  BI::Plants ip;
  ip.replay_as_own = plant == "replay_as_own";
  BH::Plants hp;
  hp.flip_angle = plant == "flip_angle";
  std::vector<W::WeaponCall> cb;
  std::vector<float> nb;

  {
    FB::Store s;
    Rec r;
    r.Splat(100, 1, 0, 10.f, -119.92f, 5.f, 0.3f, 0.8f, 1.f);
    r.Weapon(100, 3, 7.f);
    const W::View in = r.Get();
    const W::View out = FB::Apply(in, s, 1, 4, cb, nb, fp);
    Check(out.calls == in.calls && out.n_calls == 2 && s.calls.empty(), "keep.pass_through", "a growing splat and a weapon: the record's own view, nothing kept");
  }

  FB::Store s;   // one scene: pool count 1, arena 4
  {   // frame 200: splat A starts its fade
    Rec r;
    r.Weapon(200, 3, 7.f);
    r.Splat(200, 1, 0, 10.f, -119.92f, 5.f, 0.3f, 1.2f, 0.9967f);
    const W::View out = FB::Apply(r.Get(), s, 1, 4, cb, nb, fp);
    const bool two = out.n_calls == 2;
    const bool first_kept = two && W::IsBloodCall(out.calls[0].slot_flags) && out.calls[0].alpha == 1.f && out.calls[0].frame == 200 && out.calls[0].node_count == 1 &&
                            std::memcmp(out.nodes + size_t(out.calls[0].node_first) * 12, r.nodes.data() + 24, 12 * sizeof(float)) == 0;
    Check(first_kept && CountBlood(out) == 1 && s.calls.size() == 1, "keep.fade_kept", "the fading splat kept at alpha 1 with its rows, drawn once");
    const bool rebased = two && out.calls[1].id == 3 && out.calls[1].node_count == 2 && size_t(out.calls[1].node_first) + 2 <= out.n_nodes &&
                         std::memcmp(out.nodes + size_t(out.calls[1].node_first) * 12, r.nodes.data(), 24 * sizeof(float)) == 0;
    Check(rebased, "keep.rows_rebased", "the weapon's own rows behind the kept splat");
    W::Plants wp;
    W::Census wc;
    const std::vector<W::Item> items = W::Plan(out, wp, wc);
    size_t blood = 0;
    bool alpha1 = true;
    for (const W::Item& it : items)
      if (it.blood) ++blood, alpha1 = alpha1 && it.call < out.n_calls && out.calls[it.call].alpha == 1.f;
    Check(blood == 1 && alpha1, "keep.plan_items", "blood items " + std::to_string(blood));
  }
  {   // frame 201: the same splat fades on; the floor word moved its height by 0.03
    Rec r;
    r.Weapon(201, 3, 7.f);
    r.Splat(201, 1, 0, 10.f, -119.95f, 5.f, 0.3f, 1.2f, 0.6f);
    const W::View out = FB::Apply(r.Get(), s, 1, 4, cb, nb, fp);
    Check(s.calls.size() == 1 && CountBlood(out) == 1, "keep.no_duplicate", "kept " + std::to_string(s.calls.size()));
  }
  {   // frame 202: the game freed splat A (no call)
    Rec r;
    r.Weapon(202, 3, 7.f);
    const W::View out = FB::Apply(r.Get(), s, 1, 4, cb, nb, fp);
    Check(CountBlood(out) == 1, "keep.after_expiry", "blood calls drawn " + std::to_string(CountBlood(out)));
  }
  {   // frame 203: splat B grows (alpha 1, live); A's fading call is seen again (the twin keeps it anew from here)
    Rec r;
    r.Splat(203, 1, 0, 10.f, -119.92f, 5.f, 0.3f, 1.2f, 0.3f);
    r.Splat(203, 2, 1, -20.f, -119.92f, 8.f, 1.1f, 0.5f, 1.f);
    r.Weapon(203, 3, 9.f);
    const W::View out = FB::Apply(r.Get(), s, 1, 4, cb, nb, fp);
    const size_t kept = s.calls.size();
    bool ok = kept >= 1 && out.n_calls == kept + 2;
    for (size_t i = 0; ok && i < kept; ++i) ok = W::IsBloodCall(out.calls[i].slot_flags) && out.calls[i].alpha == 1.f && out.calls[i].frame == 203;
    ok = ok && W::IsBloodCall(out.calls[kept].slot_flags) && out.calls[kept].alpha == 1.f && out.calls[kept + 1].id == 3;
    Check(ok, "keep.order", "kept " + std::to_string(kept) + ", calls " + std::to_string(out.n_calls));
  }
  {   // frames 300..344: 45 more splats, each fading alone and then gone; frame 345: none
    for (uint32_t k = 0; k < 45; ++k) {
      Rec r;
      r.Weapon(300 + k, 3, 7.f);
      r.Splat(300 + k, 10 + k, k % 30, float(k) * 3.f - 60.f, -119.92f, float(k % 7) * 4.f, 0.1f * float(k), 0.9f, 0.95f, k % 3);
      FB::Apply(r.Get(), s, 1, 4, cb, nb, fp);
    }
    Rec r;
    r.Weapon(345, 3, 7.f);
    const W::View out = FB::Apply(r.Get(), s, 1, 4, cb, nb, fp);
    Check(CountBlood(out) == 46, "keep.no_cap", "blood calls drawn " + std::to_string(CountBlood(out)) + " (want 46)");
  }
  {   // a new pool (count 1 -> 2), then a splat kept under it, then another arena
    Rec r;
    r.Weapon(400, 3, 7.f);
    const W::View in = r.Get();
    const W::View out = FB::Apply(in, s, 2, 4, cb, nb, fp);
    const bool pool_clears = s.calls.empty() && CountBlood(out) == 0 && out.calls == in.calls;
    Rec r2;
    r2.Weapon(401, 3, 7.f);
    r2.Splat(401, 1, 0, 1.f, -119.92f, 1.f, 0.f, 1.f, 0.5f, 1, 2);
    FB::Apply(r2.Get(), s, 2, 4, cb, nb, fp);
    const bool refilled = s.calls.size() == 1;
    Rec r3;
    r3.Weapon(402, 3, 7.f);
    FB::Apply(r3.Get(), s, 2, 5, cb, nb, fp);
    Check(pool_clears && refilled && s.calls.empty(), "keep.new_scene", "a new pool and another arena both empty the store");
  }
  {   // a record with no calls at all (a frame with no weapon section) after a kept splat
    FB::Store e;
    Rec r;
    r.Splat(500, 1, 0, 4.f, -119.92f, 4.f, 0.f, 1.f, 0.5f, 1, 3);
    FB::Apply(r.Get(), e, 3, 4, cb, nb, fp);
    Rec none;
    const W::View out = FB::Apply(none.Get(), e, 3, 4, cb, nb, fp);
    Check(CountBlood(out) == 1 && out.n_calls == 1 && out.calls[0].alpha == 1.f, "keep.empty_record", "kept splats drawn in a record with no calls");
  }
  {   // BLOODMOD review: a splat mid-fade when the player pauses (the pool's update runs in pause-stopped task groups, docs\systems\task_runner.md), the same call again after the pause
    FB::Store g;
    Rec r;
    r.Splat(600, 1, 0, 7.f, -119.92f, 9.f, 0.4f, 1.f, 0.7f, 1, 5);
    FB::Apply(r.Get(), g, 5, 4, cb, nb, fp);
    Rec later;
    later.Splat(1900, 1, 0, 7.f, -119.92f, 9.f, 0.4f, 1.f, 0.7f, 1, 5);
    const W::View out = FB::Apply(later.Get(), g, 5, 4, cb, nb, fp);
    Check(g.calls.size() == 1 && CountBlood(out) == 1, "keep.long_pause", "kept " + std::to_string(g.calls.size()) + ", blood calls drawn " + std::to_string(CountBlood(out)) + " (want 1, 1)");
  }
  {   // BLOODMOD2: slot 2's splat fades and goes; the slot's next splat lands on the same spot (every row float within 0.1 of the first) and fades
    FB::Store a;
    Rec r1;
    r1.Splat(700, 1, 2, 5.f, -119.92f, 5.f, 0.2f, 1.f, 0.8f, 1, 7);
    FB::Apply(r1.Get(), a, 7, 4, cb, nb, fp);
    Rec r2;
    r2.Weapon(701, 3, 7.f);
    FB::Apply(r2.Get(), a, 7, 4, cb, nb, fp);
    Rec r3;
    r3.Splat(900, 2, 2, 5.03f, -119.92f, 5.02f, 0.25f, 1.f, 0.8f, 1, 7);
    const W::View out = FB::Apply(r3.Get(), a, 7, 4, cb, nb, fp);
    Check(a.calls.size() == 2 && CountBlood(out) == 2, "keep.same_place", "kept " + std::to_string(a.calls.size()) + ", blood calls drawn " + std::to_string(CountBlood(out)) + " (want 2, 2)");
  }
  {   // BLOODMOD2 (review finding 3): the old scene's last record drawn after the game made the new pool: its fading row carries pool 7, the drawer's count is 8
    FB::Store b;
    Rec r;
    r.Splat(1000, 3, 4, 2.f, -119.92f, 2.f, 0.f, 1.f, 0.6f, 1, 7);
    const W::View out = FB::Apply(r.Get(), b, 8, 4, cb, nb, fp);
    Check(b.calls.empty() && CountBlood(out) == 1 && out.calls[0].alpha == 0.6f, "keep.old_pool_row", "kept " + std::to_string(b.calls.size()) + " (want 0; the row drawn as the game drew it)");
  }
  {   // a fading row with no identity (the tracker gave it 0: a record the replay track wrote)
    FB::Store c0;
    Rec r;
    r.Add(1100, 0, 5, 3.f, -119.92f, 3.f, 0.f, 1.f, 0.4f, 1, 0.4f);
    const W::View out = FB::Apply(r.Get(), c0, 9, 4, cb, nb, fp);
    Check(c0.calls.empty() && c0.not_kept == 1 && CountBlood(out) == 1 && out.calls[0].alpha == 0.4f, "keep.no_identity", "kept " + std::to_string(c0.calls.size()) + ", not kept " + std::to_string(c0.not_kept));
  }
  {   // BLOODMOD2: the identity end to end: blood_slot_id.h (the tracker's code) on a made-up pool, the rows it stamps fed to the store
    Pool g;
    const auto rd = [&](uint32_t a, uint32_t& v) { return g.Read(a, v); };
    BI::Slots ids;
    FB::Store st;
    const uint32_t P = 11;
    uint32_t s1 = 0;
    bool same_life = true;
    for (uint32_t phase = 0; phase < 4; ++phase) {   // splat S1 in slot 0: spawned (state 0), grows (1), darkens (2), fades (3)
      const float al = phase < 3 ? 1.f : 0.9f;
      g.Set(0, phase, al, al);
      ids.Scan(rd, P);
      const uint32_t id = ids.IdOf(rd, 0, Pool::Bits(al), ids.ReplayOn(rd), ip);
      if (phase == 0) s1 = id;
      else same_life = same_life && id == s1;
    }
    Check(s1 != 0 && same_life && W::BloodIdentityPools(s1) == P, "id.one_life", "identity " + std::to_string(s1));
    {
      Rec r;
      r.Add(2000, s1, 0, 10.f, -119.92f, 5.f, 0.3f, 1.2f, 0.9f, 1, 0.45f);
      FB::Apply(r.Get(), st, P, 4, cb, nb, fp);
    }
    g.Set(0, W::kBloodStateFree, -0.001f, 0.f);   // S1 freed
    ids.Scan(rd, P);
    g.Set(0, 0, 1.f, 1.f);   // S2 spawned in slot 0
    ids.Scan(rd, P);
    g.Set(0, 1, 1.f, 1.f);
    ids.Scan(rd, P);
    const uint32_t s2 = ids.IdOf(rd, 0, Pool::Bits(1.f), ids.ReplayOn(rd), ip);
    Check(s2 != 0 && s2 != s1, "id.new_splat", "identities " + std::to_string(s1) + " -> " + std::to_string(s2));
    g.words[Pool::kQ + W::kBloodPoolOffReplay] = 1;   // the instant replay: record 0 holds the recorded S1, alpha and colour quantised to 6/15, its place a half float
    g.words[Pool::RecAlpha(0)] = Pool::Bits(0.4f);
    ids.Scan(rd, P);
    const uint32_t r_id = ids.IdOf(rd, 0, Pool::Bits(0.4f), ids.ReplayOn(rd), ip);
    {
      Rec r;
      r.Add(2100, r_id, 0, 10.0078f, -119.9375f, 5.f, 0.3f, 1.2f, 0.4f, 1, 0.4f);
      FB::Apply(r.Get(), st, P, 4, cb, nb, fp);
    }
    Check(r_id == 0 && st.calls.size() == 1, "id.replayed_row", "identity " + std::to_string(r_id) + ", kept " + std::to_string(st.calls.size()) + " (want 0, 1)");
    g.words[Pool::kQ + W::kBloodPoolOffReplay] = 0;   // slot 8 cleared the word; record 0 still holds the played-back S1 until the next copy
    ids.Scan(rd, P);
    const uint32_t a_id = ids.IdOf(rd, 0, Pool::Bits(0.4f), ids.ReplayOn(rd), ip);
    {
      Rec r;
      r.Add(2101, a_id, 0, 10.0078f, -119.9375f, 5.f, 0.3f, 1.2f, 0.4f, 1, 0.4f);
      FB::Apply(r.Get(), st, P, 4, cb, nb, fp);
    }
    Check(a_id == 0 && st.calls.size() == 1, "id.after_replay", "identity " + std::to_string(a_id) + ", kept " + std::to_string(st.calls.size()) + " (want 0, 1)");
    g.Set(0, 3, 0.95f, 0.95f);   // S2 fades, its own record again
    ids.Scan(rd, P);
    const uint32_t f_id = ids.IdOf(rd, 0, Pool::Bits(0.95f), ids.ReplayOn(rd), ip);
    {
      Rec r;
      r.Add(2200, f_id, 0, -4.f, -119.92f, 6.f, 1.f, 0.7f, 0.95f, 2, 0.45f);
      FB::Apply(r.Get(), st, P, 4, cb, nb, fp);
    }
    Check(f_id == s2 && st.calls.size() == 2, "id.next_splat_kept", "identity " + std::to_string(f_id) + ", kept " + std::to_string(st.calls.size()) + " (want s2, 2)");
    ids.Scan(rd, P + 1);   // RESTART MATCH YES moved the count; the pool is the same
    const uint32_t n_id = ids.IdOf(rd, 0, Pool::Bits(0.95f), ids.ReplayOn(rd), ip);
    Check(n_id != 0 && n_id != f_id && W::BloodIdentityPools(n_id) == P + 1, "id.new_count", "identity " + std::to_string(f_id) + " -> " + std::to_string(n_id));
  }
  {   // BLOODMOD3: the held rows' rule. The game's rows of a splat: inverse-bind B x diag(s, 1, s) x the y turn with the place as row 3 (row vectors, sub_826979F8), cut to rows by column (RowsFromMatrix).
    const auto mul = [](const float* a, const float* b, float* o) {
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
          float t = 0.f;
          for (int k = 0; k < 4; ++k) t += a[4 * i + k] * b[4 * k + j];
          o[4 * i + j] = t;
        }
    };
    const float cb = std::cos(0.3f), sb = std::sin(0.3f);
    const float B[16] = {1, 0, 0, 0, 0, cb, sb, 0, 0, -sb, cb, 0, 0.5f, -0.1f, 0.2f, 1};   // a made-up inverse bind (a turn about x and an offset): the rule must not need it
    const auto game_rows = [&](const BH::Fields& f, float* rows) {
      const float L[16] = {f.scale, 0, 0, 0, 0, 1, 0, 0, 0, 0, f.scale, 0, 0, 0, 0, 1};
      const float c = std::cos(f.angle), s = std::sin(f.angle);
      const float Wm[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, f.x, f.y, f.z, 1};   // sub_82C330D8 with the angles (0, a, 0): m[2] = -sin a, m[8] = sin a; sub_82C334A8 puts the place in row 3
      float BL[16], P[16];
      mul(B, L, BL), mul(BL, Wm, P);
      for (int k = 0; k < 3; ++k)
        for (int r = 0; r < 4; ++r) rows[4 * k + r] = P[4 * r + k];
    };
    BH::Fields a, t;
    a.x = 10.f, a.y = -119.92f, a.z = 5.f, a.scale = 0.4f, a.angle = 0.7f;
    t.x = -35.f, t.y = -119.95f, t.z = 61.f, t.scale = 1.3f, t.angle = 2.9f;
    float ra[12], rt[12], out[12];
    game_rows(a, ra), game_rows(t, rt);
    const bool made = BH::SynthRows(ra, a, t, out, hp);
    float err = 0.f;
    for (int i = 0; i < 12; ++i) err = std::max(err, std::fabs(out[i] - rt[i]));
    Check(made && err < 1e-3f, "held.synth_rows", "largest row difference " + std::to_string(err) + " (want < 0.001)");
  }
  {   // BLOODMOD3: a held row (made from a live particle the game faded undrawn) is kept once at alpha 1, drawn as the kept splat; the same splat's own fading row later is not kept again
    FB::Store h;
    Rec r;
    r.Splat(3000, 40, 3, 12.f, -119.92f, 7.f, 1.1f, 0.9f, 0.6f, 2, 21);
    r.calls.back().slot_flags |= W::kCallBloodHeld;
    r.Weapon(3000, 3, 7.f);
    const W::View out = FB::Apply(r.Get(), h, 21, 4, cb, nb, fp);
    W::Plants wp;
    wp.draw_held = plant == "draw_held";
    W::Census wc;
    const std::vector<W::Item> items = W::Plan(out, wp, wc);
    size_t blood = 0;
    bool alpha1 = true;
    for (const W::Item& it : items)
      if (it.blood) ++blood, alpha1 = alpha1 && it.call < out.n_calls && out.calls[it.call].alpha == 1.f && !W::IsBloodHeld(out.calls[it.call].slot_flags);
    Rec later;
    later.Splat(3001, 40, 3, 12.f, -119.95f, 7.f, 1.1f, 0.9f, 0.5f, 2, 21);
    const W::View out2 = FB::Apply(later.Get(), h, 21, 4, cb, nb, fp);
    Check(h.calls.size() == 1 && h.held_kept == 1 && blood == 1 && alpha1 && CountBlood(out2) == 1, "held.kept",
          "kept " + std::to_string(h.calls.size()) + ", from held rows " + std::to_string(h.held_kept) + ", blood items " + std::to_string(blood) + " (want 1, 1, 1)");
  }
  {   // BLOODMOD3: a held row the store cannot keep (stamped under the old pool count 7, the drawer's is 8) is never drawn
    FB::Store h;
    Rec r;
    r.Splat(3100, 41, 4, 2.f, -119.92f, 2.f, 0.f, 1.f, 0.6f, 1, 7);
    r.calls.back().slot_flags |= W::kCallBloodHeld;
    const W::View out = FB::Apply(r.Get(), h, 8, 4, cb, nb, fp);
    W::Plants wp;
    wp.draw_held = plant == "draw_held";
    W::Census wc;
    const std::vector<W::Item> items = W::Plan(out, wp, wc);
    size_t blood = 0;
    for (const W::Item& it : items) blood += it.blood ? 1u : 0u;
    Check(h.calls.empty() && h.held_unkept == 1 && blood == 0, "held.never_drawn", "kept " + std::to_string(h.calls.size()) + ", blood items " + std::to_string(blood) + " (want 0, 0)");
  }
  std::printf("%s\n", g_fail ? "floor_blood_test: FAILED" : "floor_blood_test: all checks passed");
  return g_fail ? 1 : 0;
}
