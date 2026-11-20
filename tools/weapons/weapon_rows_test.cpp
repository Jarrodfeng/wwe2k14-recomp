// entry: RawHookEntry
// weapon_rows_test: the offline proof of lane PW-2 (Session 103; contract src\renderer_scene\weapon_rows.h, docs\research\s103\PW_adversary.md). 0 game runs, no SDK, no game memory.
//   weapon_rows_test [--plant NAME] [--capture DIR] [--object 0xAA141900]
// It runs the SHIPPED code: the tracker (src\scene_live\weapon_rows_core.h) against a fake guest memory laid out through the contract's offsets (manager singleton, slot table, main /
// secondary models, node-count words of the skinned model), the contributor's Emit (weapon_rows_contrib.h) and the record file code (record_io: kScWeaponCalls / kScWeaponNodes).
// THE ORACLE is this file's own formula for the rows (row k = (m[k], m[4+k], m[8+k], m[12+k]) of the matrix the "game" wrote at the call), never arenaworld::RowsFromMatrix.
// The headline case is TWO CHAIRS: one model object, one r5 pointer, the game rewrites the buffer between the two submits (PW_adversary.md 0.2): both rows are kept, instance 0 and 1, each with the
// buffer contents of ITS call.
// --capture DIR (a kept capture folder with scene\*.rsc): the existing records carry one LightRef per model call (object, ordinal); the calls of --object (default 0xAA141900, the ring steps: 2
// instances, RING_PROMOTION_MAP.md 97) are replayed through the tracker frame by frame, the object declared a weapon: instances must equal the LightRef ordinals. The records carry no r5
// buffer bytes, so the node rows of that arm come from a synthetic per-call buffer; what it proves from the captures is the CALL STRUCTURE (how many calls, in which order), not pixels.
// Plants (each must make a NAMED check fail; exit code 1 when any check failed):
//   dedupe_object_r5   the arena's key (one row per (object, r5)): the second chair is dropped                -> "hook.two_chairs", "capture.instances"
//   late_read          the rows read at the NEXT call, not at the call ([model+200] at emit)                  -> "hook.two_chairs"
//   node_count_plus1   the node count off by one                                                              -> "hook.two_chairs"
//   cached_members     the membership kept across frames                                                      -> "hook.freed_slot_not_weapon", "hook.one_read_per_frame"
//   count_every_submit   every call of a weapon's model is recorded, not only the slot-13 submit (review fix 4)  -> "submit.extra_passes"
//   rows_after_ours_check  the rows are taken after the raw hook's Ours test (review fix 1: the OFF build)        -> "hook.ours_mode_records"
//   shadow_as_colour   S103 STG: the flag A tag is ignored (a props-map / map B submit is taken as a colour call)      -> "shadow.tagged_not_colour"
//   no_frame / no_id_at_call  the row's frame stamp / id left empty                                            -> "row.frame" / "row.id_at_call"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/weapon_plan.h"   // S103 STG: Plan, ShadowCasterRows
#include "../../src/scene_live/weapon_rows_contrib.h"

using namespace rdr_scene;
using namespace rdr_scene::live::weaponrows;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;
namespace W = rdr_scene::weapons;

static_assert(W::kScWeaponCalls == 60 && W::kScWeaponNodes == 61 && sizeof(W::WeaponCall) == 32 && sizeof(RingNodeRows) == 48, "sections 60 / 61; the contract's sizes (v2: WeaponCall 32 bytes)");

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-32s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// ---- a fake guest ----
struct Guest {
  std::map<uint32_t, uint32_t> words;
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
  // the node matrices the "game" writes into a palette buffer: float j of node i = seed * 1000 + i * 16 + j
  void WriteNodes(uint32_t buf, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t j = 0; j < 16; ++j) PutFloat(buf + 64 * i + 4 * j, float(seed * 1000 + i * 16 + j));
  }
  void MakeModel(uint32_t obj, uint32_t nodes, float alpha) {   // a skinned model whose slot 2 is sub_82BD3AB8 and whose count is in [this+300]
    words[obj] = 0x8206AF90, words[0x8206AF90 + 8] = 0x82BD3AB8;   // the listing's addresses, spelled out (not the shipped constants)
    words[obj + 296] = 1, words[obj + 300] = nodes;
    PutFloat(obj + 252, alpha);
  }
};

// the oracle: the 12 floats of node i of the buffer written with `seed` (independent of RowsFromMatrix: spelled out)
static void ExpectRows(uint32_t node, uint32_t seed, float out[12]) {
  float m[16];
  for (uint32_t j = 0; j < 16; ++j) m[j] = float(seed * 1000 + node * 16 + j);
  for (int k = 0; k < 3; ++k) out[4 * k + 0] = m[k], out[4 * k + 1] = m[4 + k], out[4 * k + 2] = m[8 + k], out[4 * k + 3] = m[12 + k];
}
static bool RowsAre(const FrameBuffer& fb, const W::WeaponCall& c, uint32_t nodes, uint32_t seed) {
  if (c.node_count != nodes || uint64_t(c.node_first) + nodes > fb.nodes.size()) return false;
  for (uint32_t i = 0; i < nodes; ++i) {
    float e[12];
    ExpectRows(i, seed, e);
    if (std::memcmp(fb.nodes[c.node_first + i].rows, e, sizeof e) != 0) return false;
  }
  return true;
}

constexpr uint32_t kMgr = 0x70000000;
// a manager with `n` slots: ids[i] (-1 = empty), mains[i], secondaries (slot i's first secondary) optional
static void MakeManager(Guest& g, uint32_t n, const std::vector<int32_t>& ids, const std::vector<uint32_t>& mains, const std::vector<uint32_t>& sec0 = {}) {
  g.words[W::kMgrGlobal] = kMgr;
  g.words[kMgr + W::kOffSlotCount] = n;
  for (uint32_t i = 0; i < n; ++i) {
    g.words[kMgr + W::kOffSlotId + 4 * i] = uint32_t(ids[i]);
    g.words[kMgr + W::kOffSlotMain + 4 * i] = mains[i];
    for (uint32_t k = 0; k < W::kMaxSecondary; ++k) g.words[kMgr + W::kOffSlotSecondary + 16 * i + 4 * k] = (k == 0 && i < sec0.size()) ? sec0[i] : 0;
  }
}

struct Rig {
  Guest g;
  WeaponRowsContributor C;
  Plant plant = Plant::None;
  CallResult Call(uint32_t model, uint32_t r5, uint64_t frame, bool own_submit = true, uint8_t shadow = 0) {
    return C.tracker.OnModelCall(model, r5, frame, own_submit, [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return g.ReadMats(a, n, o); }, 1.0f, 0, 0, nullptr, nullptr, shadow);
  }
};

static Plant PlantFromName(const std::string& s) {
  if (s == "dedupe_object_r5") return Plant::DedupeObjectR5;
  if (s == "late_read") return Plant::LateRead;
  if (s == "node_count_plus1") return Plant::NodeCountPlus1;
  if (s == "cached_members") return Plant::CachedMembers;
  if (s == "count_every_submit") return Plant::CountEverySubmit;
  if (s == "rows_after_ours_check") return Plant::RowsAfterOursCheck;
  if (s == "no_frame") return Plant::NoFrame;
  if (s == "no_id_at_call") return Plant::NoIdAtCall;
  if (s == "rigid_as_unknown") return Plant::RigidAsUnknown;
  if (s == "strict_forward") return Plant::StrictForward;
  if (s == "shadow_as_colour") return Plant::ShadowAsColour;
  return Plant::None;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash under a planted twin must not lose the lines before it
  std::string plant_name, capture, object_s = "0xAA141900";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant_name = argv[++i];
    else if (a == "--capture" && i + 1 < argc) capture = argv[++i];
    else if (a == "--object" && i + 1 < argc) object_s = argv[++i];
  }
  const Plant plant = PlantFromName(plant_name);
  if (!plant_name.empty() && plant == Plant::None) {
    std::printf("unknown plant %s\n", plant_name.c_str());
    return 2;
  }

  const uint32_t kChair = 0xA0001000, kChairBuf = 0xB0001000, kSteps = 0xA0002000, kStepsBuf = 0xB0002000, kSecondary = 0xA0003000, kSecBuf = 0xB0003000, kIdle = 0xA0004000, kWrestler = 0xA0005000,
                 kWrBuf = 0xB0005000;
  // ---- the two chairs, a wrestler, a secondary, a named-never-drawn object; frame after frame ----
  {
    Rig r;
    r.C.tracker.plant = plant;
    r.g.MakeModel(kChair, 3, 1.0f), r.g.MakeModel(kSteps, 2, 0.5f), r.g.MakeModel(kSecondary, 1, 1.0f), r.g.MakeModel(kIdle, 1, 1.0f), r.g.MakeModel(kWrestler, 5, 1.0f);
    // slots: 0 = chair (id 7), 1 = steps (id 32), 2 = id 89 with a secondary model, 3 = an empty slot (id -1) whose main is stale, 4 = never drawn
    MakeManager(r.g, 5, {7, 32, 89, -1, 55}, {kChair, kSteps, 0xA0009000, kWrestler, kIdle}, {0, 0, kSecondary});
    SceneTables dummy;
    const uint64_t F = 100;
    // frame 100: wrestler, chair (buffer A), the game rewrites the buffer, chair again (buffer B, SAME r5), steps twice, the secondary, the wrestler again
    r.g.WriteNodes(kWrBuf, 5, 9);
    r.Call(kWrestler, kWrBuf, F);
    r.g.WriteNodes(kChairBuf, 3, 11);
    const CallResult c0 = r.Call(kChair, kChairBuf, F);
    r.g.WriteNodes(kChairBuf, 3, 12);
    const CallResult c1 = r.Call(kChair, kChairBuf, F);
    r.g.WriteNodes(kStepsBuf, 2, 21);
    r.Call(kSteps, kStepsBuf, F);
    r.g.WriteNodes(kStepsBuf, 2, 22);
    r.Call(kSteps, kStepsBuf, F);
    r.g.WriteNodes(kSecBuf, 1, 31);
    r.Call(kSecondary, kSecBuf, F);
    r.Call(kWrestler, kWrBuf, F);
    // frame 101 closes it; the slot table changes: the chair's slot is freed and its address comes back for another id's slot (a reload), the steps stay
    r.g.words[kMgr + W::kOffSlotId + 0] = uint32_t(-1);
    r.g.words[kMgr + W::kOffSlotMain + 4 * 3] = kChair, r.g.words[kMgr + W::kOffSlotId + 4 * 3] = 12;   // slot 3 is now live, main = the chair's old address (another id, the same object address)
    r.g.WriteNodes(kChairBuf, 3, 41);
    const CallResult d0 = r.Call(kChair, kChairBuf, F + 1);   // still a member (slot 3): recorded, instance 0 again (a new frame)
    r.g.words[kMgr + W::kOffSlotId + 4 * 3] = uint32_t(-1);
    r.Call(kSteps, kStepsBuf, F + 2);   // frame 102: slot 3 is gone again: a chair call in this frame is not a weapon
    const CallResult gone = r.Call(kChair, kChairBuf, F + 2);

    FrameBuffer fb;
    bool got = r.C.tracker.TakeNext(fb);
    Check(got && fb.frame == F && fb.calls.size() == 5, "hook.rows_per_call", "weapon calls recorded in frame 100: " + std::to_string(fb.calls.size()) + " (2 chairs, 2 steps, 1 secondary; the wrestler's 2 calls are no weapons)");
    int chairs = 0;
    bool chair_inst_ok = false, chair_rows_ok = false, chair_alpha_ok = false;
    std::vector<const W::WeaponCall*> cc;
    for (const W::WeaponCall& c : fb.calls)
      if (c.object == kChair) cc.push_back(&c);
    chairs = int(cc.size());
    if (chairs == 2) {
      chair_inst_ok = cc[0]->instance == 0 && cc[1]->instance == 1;
      chair_rows_ok = RowsAre(fb, *cc[0], 3, 11) && RowsAre(fb, *cc[1], 3, 12);   // each row = the buffer AT ITS CALL, not the last contents
      chair_alpha_ok = cc[0]->alpha == 1.0f;
    }
    Check(chairs == 2 && chair_inst_ok && chair_rows_ok && chair_alpha_ok && c0.weapon && c1.weapon && c0.instance == 0 && c1.instance == 1, "hook.two_chairs",
          "chairs kept " + std::to_string(chairs) + ", instances 0 / 1 " + (chair_inst_ok ? "ok" : "BAD") + ", node rows = the buffer at each call " + (chair_rows_ok ? "ok" : "BAD"));
    int steps = 0;
    bool steps_ok = true;
    uint32_t k = 0;
    for (const W::WeaponCall& c : fb.calls)
      if (c.object == kSteps) steps_ok = steps_ok && c.instance == k && RowsAre(fb, c, 2, 21 + k) && c.alpha == 0.5f, ++k, ++steps;
    {   // review fixes 2 / 5: every row says its frame, its id (from the table of THAT frame) and its slot (bit 8 = secondary)
      bool frame_ok = true, ids_ok = true, flags_ok = true;
      for (const W::WeaponCall& c : fb.calls) {
        frame_ok = frame_ok && c.frame == uint32_t(F);
        const int32_t want = c.object == kChair ? 7 : c.object == kSteps ? 32 : 89;
        ids_ok = ids_ok && c.id == want;
        const uint32_t want_slot = c.object == kChair ? 0 : c.object == kSteps ? 1 : 2;
        flags_ok = flags_ok && (c.slot_flags & 0xFF) == want_slot && ((c.slot_flags & W::kCallSecondary) != 0) == (c.object == kSecondary) && W::CallSecondaryIndex(c.slot_flags) == 0 && (c.slot_flags >> 16) == 0;
      }
      Check(frame_ok, "row.frame", "every WeaponCall carries the frame of its call");
      Check(ids_ok, "row.id_at_call", "chair id 7, steps id 32, the secondary model id 89 (the slot's id)");
      Check(flags_ok, "row.slot_flags", "slot index in bits 0-7, the secondary bit only on the secondary model (k 0), bits 16-31 reserved 0");
    }
    Check(steps == 2 && steps_ok, "hook.two_steps", "the ring-steps shape (one object, 2 instances): " + std::to_string(steps) + " rows");
    int sec = 0, wr = 0;
    for (const W::WeaponCall& c : fb.calls) sec += c.object == kSecondary, wr += c.object == kWrestler;
    Check(sec == 1 && wr == 0 && fb.calls.size() == 5, "hook.members_only", "the secondary model is a weapon (" + std::to_string(sec) + "), the wrestler is not (" + std::to_string(wr) + "), rows " + std::to_string(fb.calls.size()));
    Check(fb.named == 5 && fb.never_drawn == 2 && fb.objects == 3 + 0 && true, "census.named_never_drawn",
          "named " + std::to_string(fb.named) + " (chair, steps, secondary, 0xA0009000 main of slot 2, idle) never drawn " + std::to_string(fb.never_drawn) + " objects " + std::to_string(fb.objects));
    // frame 101: the freed slot's address is a member again through slot 3: recorded as instance 0; frame 102: not a member
    FrameBuffer fb2, fb3;
    r.C.tracker.TakeNext(fb2);
    Check(d0.weapon && d0.instance == 0 && fb2.frame == F + 1 && fb2.calls.size() == 1 && RowsAre(fb2, fb2.calls[0], 3, 41) && fb2.calls[0].id == 12 && fb2.calls[0].frame == uint32_t(F + 1), "hook.members_per_frame",
          "frame 101: the reused address is named through its new slot, and its row says id 12 (frame 100's row of the same address says id 7)");
    Check(!gone.weapon, "hook.freed_slot_not_weapon", "frame 102: slot 3 freed: a call of the address is no weapon (the table is re-read every frame)");
    // the secondary (k = 0) object of slot 2 is named even though its main is not drawn; one membership read per frame
    const Stats s = r.C.tracker.GetStats();
    Check(s.member_reads == 3 && s.frames == 2, "hook.one_read_per_frame", "member reads " + std::to_string(s.member_reads) + " for 3 frames");
    (void)fb3;
  }

  // ---- no manager / a bad count / unreadable rows ----
  {
    Rig r;
    r.C.tracker.plant = plant;
    r.g.MakeModel(kChair, 3, 1.0f);
    const CallResult a = r.Call(kChair, kChairBuf, 200);   // no manager at all
    MakeManager(r.g, 2, {7, 8}, {kChair, kSteps});
    r.g.words[kMgr + W::kOffSlotCount] = 49;   // above the game's own cap: not a table of this manager
    const CallResult b = r.Call(kChair, kChairBuf, 201);
    r.g.words[kMgr + W::kOffSlotCount] = 2;
    const CallResult c = r.Call(kChair, kChairBuf, 202);   // the palette buffer is unreadable (nothing written there)
    r.g.words.erase(kChairBuf);
    const CallResult d = r.Call(kChair, kChairBuf, 203);
    const Stats s = r.C.tracker.GetStats();
    r.g.words[W::kMgrGlobal] = 0x8000;   // below 0x10000: not a manager pointer
    const CallResult b2 = r.Call(kChair, kChairBuf, 204);
    r.g.words[W::kMgrGlobal] = kMgr;
    const Stats s2 = r.C.tracker.GetStats();
    Check(!a.weapon && !b.weapon && !b2.weapon && c.weapon && c.recorded && c.n_nodes == 0 && d.recorded && d.n_nodes == 0 && s2.slots_unreadable >= 1 && s2.mgr_missing >= 1 && s2.slots_refused == 1 && s2.unreadable >= 1, "hook.refusals_counted",
          "no manager (unreadable global) / count 49 (refused, the one rule of both readers) / a pointer below 0x10000 are no weapons; an unreadable buffer keeps the row with 0 nodes (counted)");
    // overflow: more calls than the capacity: counted, never a silent truncation
    r.g.WriteNodes(kChairBuf, 3, 1);
    for (uint32_t i = 0; i < kMaxCalls + 10; ++i) r.Call(kChair, kChairBuf, 300);
    r.C.tracker.CloseBefore(400);
    FrameBuffer fb;
    uint32_t big = 0;
    while (r.C.tracker.TakeNext(fb))
      if (fb.frame == 300) big = uint32_t(fb.calls.size());
    Check(big == kMaxCalls && r.C.tracker.GetStats().overflow == 10, "hook.overflow_counted", "rows " + std::to_string(big) + ", overflow " + std::to_string(r.C.tracker.GetStats().overflow));
  }

  // ---- the contributor and the record file code ----
  {
    Rig r;
    r.C.tracker.plant = plant;
    r.g.MakeModel(kChair, 3, 1.0f);
    MakeManager(r.g, 1, {7}, {kChair});
    for (uint64_t f = 500; f < 502; ++f) {
      r.g.WriteNodes(kChairBuf, 3, uint32_t(f));
      r.Call(kChair, kChairBuf, f);
      r.g.WriteNodes(kChairBuf, 3, uint32_t(f + 50));
      r.Call(kChair, kChairBuf, f);
    }
    SceneTables t;
    t.header.source = uint8_t(Source::Live);
    t.header.guest_frame = 502;
    Counters cn;
    FrameInfo fi;
    fi.guest_frame = 502;
    r.C.Emit(fi, t, cn);   // closes frame 501 and emits 500 and 501
    // PW review fix 7: only the NEWEST closed frame's calls are written (frame 500's are dropped and counted): 2 rows, 6 pool rows, all of frame 501, the instance restarts at 0 in that frame
    bool rebased = t.weapon_calls.size() == 2 && t.weapon_nodes.size() == 6;
    for (size_t i = 0; rebased && i < t.weapon_calls.size(); ++i) rebased = t.weapon_calls[i].node_first == 3 * i && t.weapon_calls[i].node_count == 3 && t.weapon_calls[i].frame == 501u;
    Check(rebased && t.weapon_calls[0].instance == 0 && t.weapon_calls[1].instance == 1, "contrib.newest_frame_only", "2 rows, 6 pool rows, all frame 501; frame 500's two calls dropped (counted); the instance restarts each frame");
    {   // a newest closed frame with NO weapon call must not leave an older frame's weapons in the record
      Rig q;
      q.C.tracker.plant = plant;
      q.g.MakeModel(kChair, 3, 1.0f), q.g.MakeModel(kWrestler, 5, 1.0f);
      MakeManager(q.g, 1, {7}, {kChair});
      q.g.WriteNodes(kChairBuf, 3, 1), q.g.WriteNodes(kWrBuf, 5, 2);
      q.Call(kChair, kChairBuf, 600);
      q.Call(kWrestler, kWrBuf, 601);   // frame 601 has model calls and no weapon call
      SceneTables t2;
      t2.header.source = uint8_t(Source::Live);
      Counters c2;
      FrameInfo f2;
      f2.guest_frame = 602;
      q.C.Emit(f2, t2, c2);   // closes 600 and 601 (frame 602 has no call yet: CloseBefore through the latch)
      Check(t2.weapon_calls.empty(), "contrib.empty_newest_frame", "frame 601 had no weapon call: the record carries none (frame 600's chair is not left on screen)");
    }
    std::string cen;
    r.C.Census(cen);
    Check(cen.find("[scene][weapon_rows] calls 4 objects 2, named but never drawn 0") == 0 && cen.find("other passes 0") != std::string::npos && cen.find("older frames dropped 1 (2 calls)") != std::string::npos, "census.line", cen.substr(0, 100));
    std::string err;
    std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables back;
    const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
    const bool same = ok && back.weapon_calls.size() == 2 && back.weapon_nodes.size() == 6 && std::memcmp(back.weapon_calls.data(), t.weapon_calls.data(), 2 * sizeof(W::WeaponCall)) == 0 &&
                      std::memcmp(back.weapon_nodes.data(), t.weapon_nodes.data(), 6 * sizeof(RingNodeRows)) == 0 && back.weapon_call_errors == 0;
    Check(same, "record.roundtrip", ok ? "sections 60 / 61 round trip byte-identical" : err);
    SceneTables old;
    old.header.source = uint8_t(Source::Live);
    std::vector<uint8_t> ob = SerializeRecord(old);
    SceneTables oback;
    Check(ParseRecord(ob.data(), ob.size(), oback, err) && oback.weapon_calls.empty() && oback.weapon_nodes.empty() && oback.weapon_call_errors == 0, "record.old_opens", "a record without the sections reads, with empty tables");
    // the sections are ids 60 / 61 in the file (the contract), and an unknown reader would skip them: the directory says so
    bool ids = false;
    {
      SceneHeader h;
      std::memcpy(&h, bytes.data(), sizeof h);
      for (uint32_t i = 0; i < h.section_count; ++i) {
        SectionEntry e;
        std::memcpy(&e, bytes.data() + sizeof h + i * sizeof e, sizeof e);
        if (e.id == 60 && e.element_size == 32 && e.count == 2) ids = true;
      }
    }
    Check(ids, "record.section_ids", "section 60 holds 2 x 32-byte rows");
    SceneTables bad = t;
    bad.params.push_back(ParamValue{});
    if (!bad.weapon_calls.empty()) bad.weapon_calls[0].node_count = 60000;   // a node range outside the pool
    std::vector<uint8_t> bb = SerializeRecord(bad);
    SceneTables bback;
    err.clear();
    const bool parsed = ParseRecord(bb.data(), bb.size(), bback, err);
    Check(parsed && bback.weapon_calls.empty() && bback.weapon_nodes.empty() && bback.weapon_call_errors == 1 && bback.params.size() == 1, "record.node_range_checked", parsed ? "the weapon tables are dropped and counted; the params table is intact" : err);
  }

  // ---- review fix 4: only the slot-13 submit is a weapon call; r4 = 0 is NOT the mark (slot 19 / slot 32 pass 0 too) ----
  {
    Rig r;
    r.C.tracker.plant = plant;
    r.g.MakeModel(kChair, 3, 1.0f);
    MakeManager(r.g, 1, {7}, {kChair});
    r.g.WriteNodes(kChairBuf, 3, 1);
    const CallResult a0 = r.Call(kChair, kChairBuf, 700, true);    // the weapon's own submit (called from inside sub_82C13C88)
    const CallResult a1 = r.Call(kChair, kChairBuf, 700, false);   // another pass of the SAME chair (slot 19 / 32: r4 can be 0 here too)
    const CallResult a2 = r.Call(kChair, kChairBuf, 700, false);
    const CallResult b0 = r.Call(kChair, kChairBuf, 700, true);    // the second chair
    r.C.tracker.CloseBefore(701);
    FrameBuffer fb;
    bool got = false;
    while (r.C.tracker.TakeNext(fb))
      if (fb.frame == 700) { got = true; break; }
    const Stats s = r.C.tracker.GetStats();
    Check(got && a0.instance == 0 && b0.instance == 1 && fb.calls.size() == 2 && a1.weapon && a2.weapon && !a1.recorded && !a2.recorded && s.other_pass == 2 && s.weapon_calls == 2, "submit.extra_passes",
          "two own submits = two chairs (instances 0 / 1); the two other passes are counted (other_pass " + std::to_string(s.other_pass) + ") and never recorded: rows " + std::to_string(fb.calls.size()));
    // the thread-local mark itself: Slot13Scope sets / restores it, nested scopes restore the outer one
    bool scope_ok = !IsOwnSubmit(kChair);
    {
      Slot13Scope outer(kChair);
      scope_ok = scope_ok && IsOwnSubmit(kChair) && !IsOwnSubmit(kSteps);
      { Slot13Scope inner(kSteps); scope_ok = scope_ok && IsOwnSubmit(kSteps) && !IsOwnSubmit(kChair); }
      scope_ok = scope_ok && IsOwnSubmit(kChair);
    }
    scope_ok = scope_ok && !IsOwnSubmit(kChair) && !IsOwnSubmit(0);
    Check(scope_ok, "submit.slot13_scope", "Slot13Scope marks the model of the slot-13 submit in flight, restores the outer one on exit; 0 is never an own submit");
  }

  // ---- review fix 1: the raw hook's own order, in OURS mode (every frame of the stream-free build) ----
  // RawHookEntry is the function the REX_HOOK_RAW on sub_82C04DF0 runs (RawEntry in weapon_rows_glue.h wraps it with the guest readers): the rows are taken BEFORE the Ours test. This drives it with Ours = true,
  // the slot-13 mark set the way the hook on sub_82C13C88 sets it, and asserts the rows are taken and the original is NOT asked to run.
  {
    Rig r;
    r.C.tracker.plant = plant;
    r.g.MakeModel(kChair, 3, 1.0f), r.g.MakeModel(kWrestler, 5, 1.0f);
    MakeManager(r.g, 1, {7}, {kChair});
    const auto rd = [&](uint32_t a, uint32_t& v) { return r.g.ReadWord(a, v); };
    const auto rm = [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return r.g.ReadMats(a, n, o); };
    r.g.WriteNodes(kChairBuf, 3, 61);
    CallResult c0, c1, cw;
    bool run0 = true, run1 = true, runw = true, run_not_ours = false;
    {
      Slot13Scope s13(kChair);
      run0 = RawHookEntry(r.C.tracker, true, /*ours=*/true, kChair, kChairBuf, 800, rd, rm, &c0, 1.0f, 0, nullptr, nullptr, 0, nullptr);
      r.g.WriteNodes(kChairBuf, 3, 62);
      run1 = RawHookEntry(r.C.tracker, true, true, kChair, kChairBuf, 800, rd, rm, &c1, 1.0f, 0, nullptr, nullptr, 0, nullptr);   // the second chair, the same shared buffer rewritten
    }
    r.g.WriteNodes(kWrBuf, 5, 63);
    { Slot13Scope s13(kWrestler); runw = RawHookEntry(r.C.tracker, true, true, kWrestler, kWrBuf, 800, rd, rm, &cw, 1.0f, 0, nullptr, nullptr, 0, nullptr); }   // a wrestler's own submit: not a weapon
    {
      Slot13Scope s13(kChair);
      r.g.WriteNodes(kChairBuf, 3, 64);
      CallResult c2;
      run_not_ours = RawHookEntry(r.C.tracker, true, /*ours=*/false, kChair, kChairBuf, 801, rd, rm, &c2, 1.0f, 0, nullptr, nullptr, 0, nullptr);   // a walked (not OURS) frame: the original runs, the rows are taken the same
      (void)c2;
    }
    r.C.tracker.CloseBefore(900);
    FrameBuffer fb, fb2;
    r.C.tracker.TakeNext(fb);
    r.C.tracker.TakeNext(fb2);
    const bool rows_ok = fb.frame == 800 && fb.calls.size() == 2 && RowsAre(fb, fb.calls[0], 3, 61) && RowsAre(fb, fb.calls[1], 3, 62) && fb.calls[0].instance == 0 && fb.calls[1].instance == 1;
    Check(!run0 && !run1 && !runw && run_not_ours && c0.recorded && c1.recorded && !cw.weapon && rows_ok && fb2.frame == 801 && fb2.calls.size() == 1 && RowsAre(fb2, fb2.calls[0], 3, 64), "hook.ours_mode_records",
          "OURS frame: the original is skipped (returns false) AND both chairs' rows are taken (instances 0 / 1, the buffer of each call); a walked frame runs the original and takes the same rows; a wrestler is no weapon");
    // not registered: nothing recorded, the Ours answer is still the same
    Rig q;
    q.g.MakeModel(kChair, 3, 1.0f);
    MakeManager(q.g, 1, {7}, {kChair});
    q.g.WriteNodes(kChairBuf, 3, 1);
    CallResult cu;
    bool run_u = true;
    { Slot13Scope s13(kChair); run_u = RawHookEntry(q.C.tracker, false, true, kChair, kChairBuf, 810, [&](uint32_t a, uint32_t& v) { return q.g.ReadWord(a, v); }, [&](uint32_t a, uint32_t n, std::vector<Mat4>& o) { return q.g.ReadMats(a, n, o); }, &cu, 1.0f, 0, nullptr, nullptr, 0, nullptr); }
    Check(!run_u && !cu.weapon && q.C.tracker.GetStats().calls == 0, "hook.ours_mode_unregistered", "not registered: no tracker work at all, the original is still skipped under OURS");
  }

  // ---- S103 STG (PMW.md): calls made under the game's shadow flag A are tagged, never colour rows ----
  // A walked (non-OURS) frame submits every caster twice under flag A (props map = 1, map B = 2) before the colour call; each is a slot-13 own submit. The tag keeps the colour row at instance 0 (a weapon is not drawn 3x), the colour drawer's plan
  // drops the tagged rows, the test oracle ShadowCasterRows lists them, and an OURS frame (the hook passes shadow 0: it never reads flag A) is unchanged.
  {
    Rig r;
    r.C.tracker.plant = plant;
    r.g.MakeModel(kChair, 3, 1.0f);
    MakeManager(r.g, 1, {7}, {kChair});
    r.g.WriteNodes(kChairBuf, 3, 81);
    const CallResult p = r.Call(kChair, kChairBuf, 900, true, 1);   // the props pass
    r.g.WriteNodes(kChairBuf, 3, 82);
    const CallResult m = r.Call(kChair, kChairBuf, 900, true, 2);   // map B
    r.g.WriteNodes(kChairBuf, 3, 83);
    const CallResult c = r.Call(kChair, kChairBuf, 900, true, 0);   // the colour pass
    r.C.tracker.CloseBefore(901);
    FrameBuffer fb;
    r.C.tracker.TakeNext(fb);
    size_t n_colour = 0, n_props = 0, n_b = 0;
    bool colour_ok = false, shadow_flags_ok = true;
    for (const W::WeaponCall& w : fb.calls) {
      if (!W::IsShadowCall(w.slot_flags)) { ++n_colour, colour_ok = w.instance == 0 && RowsAre(fb, w, 3, 83) && w.id == 7; }
      else if (W::IsShadowPropsCall(w.slot_flags)) ++n_props;
      else ++n_b;
      shadow_flags_ok = shadow_flags_ok && w.id == 7 && (w.slot_flags & 0xFFu) == 0;   // the slot bits are the same as a colour row's
    }
    W::View v;
    v.calls = fb.calls.data(), v.n_calls = fb.calls.size(), v.nodes = reinterpret_cast<const float*>(fb.nodes.data()), v.n_nodes = fb.nodes.size();
    W::Census cen;
    const std::vector<W::Item> items = W::Plan(v, W::Plants(), cen);
    W::Coverage cov;
    W::CountRecorded(v, cov);
    const std::vector<size_t> props_rows = W::ShadowCasterRows(v, true), any_rows = W::ShadowCasterRows(v, false);
    const Stats s = r.C.tracker.GetStats();
    Check(p.weapon && m.weapon && c.weapon && n_colour == 1 && colour_ok && n_props == 1 && n_b == 1 && shadow_flags_ok && s.shadow_calls == 2 && s.shadow_props_calls == 1 && s.weapon_calls == 1 && items.size() == 1 && cen.shadow_rows == 2 &&
              props_rows.size() == 1 && any_rows.size() == 2 && cov.rec[W::CovClassOfCall(fb.calls[0])] == 1,
          "shadow.tagged_not_colour",
          "a walked frame: props-map + map B + colour calls of one chair = 1 colour row (instance 0, the colour buffer), 1 props row, 1 map-B row (both tagged); the plan draws " + std::to_string(items.size()) + " item (not 3), shadow_rows " +
              std::to_string(cen.shadow_rows) + ", oracle rows " + std::to_string(props_rows.size()) + "/" + std::to_string(any_rows.size()));
    // ShadowKindAt: flag A (u32 [0x83929A48 + 4 * slot]) 0 = colour; set + camera = module + 1312 = props map (1); set + any other camera = 2; an unreadable flag = 0
    Guest kg;
    const uint32_t kMod = 0x91000000, kSlot = 1;
    kg.words[0x83924748] = kMod;
    const auto rd = [&](uint32_t a, uint32_t& v2) { return kg.ReadWord(a, v2); };
    kg.words[0x83929A48 + 4 * kSlot] = 0;
    const uint8_t k0 = ShadowKindAt(rd, kSlot);
    kg.words[0x83929A48 + 4 * kSlot] = 1, kg.words[0x83929A40 + 4 * kSlot] = kMod + 1312;
    const uint8_t k1 = ShadowKindAt(rd, kSlot);
    kg.words[0x83929A40 + 4 * kSlot] = kMod + 304;
    const uint8_t k2 = ShadowKindAt(rd, kSlot);
    const uint8_t k3 = ShadowKindAt(rd, 3);   // slot 3: nothing readable
    Check(k0 == 0 && k1 == 1 && k2 == 2 && k3 == 0, "shadow.kind_at", "flag A 0 -> " + std::to_string(k0) + ", props light -> " + std::to_string(k1) + ", map A / B light -> " + std::to_string(k2) + ", unreadable -> " + std::to_string(k3));
    // the OURS frame: the hook passes shadow 0 (no read) and the row is as before this lane
    Rig o;
    o.C.tracker.plant = plant;
    o.g.MakeModel(kChair, 3, 1.0f);
    MakeManager(o.g, 1, {7}, {kChair});
    o.g.WriteNodes(kChairBuf, 3, 91);
    const auto ord = [&](uint32_t a, uint32_t& v2) { return o.g.ReadWord(a, v2); };
    const auto orm = [&](uint32_t a, uint32_t n, std::vector<Mat4>& out) { return o.g.ReadMats(a, n, out); };
    CallResult oc;
    { Slot13Scope s13(kChair); RawHookEntry(o.C.tracker, true, /*ours=*/true, kChair, kChairBuf, 910, ord, orm, &oc, 1.0f, 0, nullptr, nullptr, /*shadow=*/0, /*plate_phys=*/nullptr); }
    o.C.tracker.CloseBefore(911);
    FrameBuffer ofb;
    o.C.tracker.TakeNext(ofb);
    Check(oc.recorded && ofb.calls.size() == 1 && !W::IsShadowCall(ofb.calls[0].slot_flags) && ofb.calls[0].instance == 0 && RowsAre(ofb, ofb.calls[0], 3, 91) && o.C.tracker.GetStats().shadow_calls == 0, "shadow.ours_unchanged",
          "an OURS frame (shadow 0): one untagged colour row, instance 0, no shadow census");
  }

  // ---- review fix 3: the secondary's index k travels in the row (bits 10-11); main = 0 ----
  {
    Rig r;
    r.C.tracker.plant = plant;
    const uint32_t kSec1 = 0xA0006000, kSec1Buf = 0xB0006000;
    r.g.MakeModel(kSecondary, 1, 1.0f), r.g.MakeModel(kSec1, 1, 1.0f), r.g.MakeModel(kChair, 3, 1.0f);
    MakeManager(r.g, 1, {89}, {kChair}, {kSecondary});
    r.g.words[kMgr + W::kOffSlotSecondary + 4] = kSec1;   // slot 0's second secondary model (k = 1)
    r.g.WriteNodes(kSecBuf, 1, 71), r.g.WriteNodes(kSec1Buf, 1, 72), r.g.WriteNodes(kChairBuf, 3, 73);
    r.Call(kChair, kChairBuf, 850), r.Call(kSecondary, kSecBuf, 850), r.Call(kSec1, kSec1Buf, 850);
    r.C.tracker.CloseBefore(851);
    FrameBuffer fb;
    r.C.tracker.TakeNext(fb);
    bool ok = fb.calls.size() == 3;
    for (const W::WeaponCall& c : fb.calls) {
      const uint32_t want_k = c.object == kSec1 ? 1u : 0u;
      const bool sec = c.object != kChair;
      ok = ok && c.id == 89 && ((c.slot_flags & W::kCallSecondary) != 0) == sec && W::CallSecondaryIndex(c.slot_flags) == want_k && W::CallSlot(c.slot_flags) == 0;
    }
    Check(ok, "row.secondary_index", "main = no secondary bit; the secondary models carry k = 0 and k = 1 in slot_flags bits 10-11 (each its own id-89 row)");
  }


  // ---- S103 LVF: a RIGID prop (chair, ladder, table: no skeleton, [+296] == 0 and [+220] == 0, NodeCount answers -1) gets ONE row, not none ----
  {
    Rig r;
    r.C.tracker.plant = plant;
    const uint32_t kRigid = 0xA0006000, kRigidBuf = 0xB0006000, kRigid2 = 0xA0007000, kRigid2Buf = 0xB0007000, kDesc = 0xC0006000;
    r.g.words[kRigid] = 0x8206AF90, r.g.words[0x8206AF90 + 8] = 0x82BD3AB8;   // the listing's addresses, spelled out
    r.g.words[kRigid + 296] = 0, r.g.words[kRigid + 220] = 0;
    r.g.PutFloat(kRigid + 252, 1.0f);
    r.g.words[kRigid + 824] = kDesc, r.g.words[kDesc + 8] = 1, r.g.words[kDesc + 12] = 3;   // part 0's bone list: one bone, node 3 -> palette row 2
    r.g.words[kRigid2] = 0x8206AF90, r.g.words[kRigid2 + 296] = 0, r.g.words[kRigid2 + 220] = 0;   // no part descriptor at all: node 1 assumed
    r.g.PutFloat(kRigid2 + 252, 1.0f);
    MakeManager(r.g, 2, {7, 8}, {kRigid, kRigid2});
    r.g.WriteNodes(kRigidBuf, 4, 51);
    r.g.WriteNodes(kRigid2Buf, 4, 52);
    const CallResult a = r.Call(kRigid, kRigidBuf, 500);
    const CallResult b = r.Call(kRigid2, kRigid2Buf, 500);
    FrameBuffer fb;
    r.C.tracker.CloseBefore(501);
    const bool got = r.C.tracker.TakeNext(fb);
    bool rows_ok = got && fb.calls.size() == 2;
    if (rows_ok) {
      for (const W::WeaponCall& c : fb.calls) {
        float e[12];
        ExpectRows(c.object == kRigid ? 2u : 0u, c.object == kRigid ? 51u : 52u, e);   // the node part 0 names (3 -> row 2); node 1 when unreadable
        rows_ok = rows_ok && c.node_count == 1 && uint64_t(c.node_first) + 1 <= fb.nodes.size() && std::memcmp(fb.nodes[c.node_first].rows, e, sizeof e) == 0;
      }
    }
    const Stats s = r.C.tracker.GetStats();
    Check(a.recorded && b.recorded && a.n_nodes == 1 && b.n_nodes == 1 && a.counted && a.n == -1 && a.rigid && rows_ok && s.rigid == 2 && s.rigid_node_assumed == 1 && s.count_unknown == 0, "rigid.one_row",
          "a rigid weapon (+296 == 0, +220 == 0): 1 row (node " + std::string(rows_ok ? "ok" : "BAD") + "), rigid " + std::to_string(s.rigid) + ", node assumed " + std::to_string(s.rigid_node_assumed) + ", count_unknown " + std::to_string(s.count_unknown));
    // an UNREADABLE count (a bad vtable) stays count_unknown, never a guessed row
    r.g.words[0x8206AF90 + 8] = 0x82000000;
    const CallResult c = r.Call(kRigid, kRigidBuf, 501);
    Check(c.recorded && c.n_nodes == 0 && !c.counted && r.C.tracker.GetStats().count_unknown == 1, "rigid.unreadable_count_stays_unknown", "a model whose slot 2 is not the count function: 0 rows, count_unknown 1");
  }

  // ---- S103 WC R1 / R2 (WNCC_adversary.md): a weapon whose [+296] == 0 and [+220] holds the DATA object (class 0x8206C7A0, slot 2 = sub_82C27620): the game's count is [[y+12]+24] and the draw copies that many rows ----
  {
    Rig r;
    r.C.tracker.plant = plant;
    const uint32_t kW = 0xA0008000, kWBuf = 0xB0008000, kY = 0xC0008000, kH = 0xC0009000, kW0 = 0xA0008800, kW0Buf = 0xB0008800, kY0 = 0xC0008800, kWF = 0xA0008C00, kWFBuf = 0xB0008C00, kYF = 0xC0008C00, kFvt = 0xC0008E00;
    r.g.words[0x8206C7A0 + 8] = 0x82C27620;   // the data class's slot 2, spelled out
    r.g.words[kW] = 0x8206AF90, r.g.words[0x8206AF90 + 8] = 0x82BD3AB8, r.g.words[kW + 296] = 0, r.g.words[kW + 220] = kY;
    r.g.words[kY] = 0x8206C7A0, r.g.words[kY + 12] = kH, r.g.words[kH + 24] = 3, r.g.words[kH + 32] = 0;
    r.g.PutFloat(kW + 252, 1.0f);
    r.g.words[kW0] = 0x8206AF90, r.g.words[kW0 + 296] = 0, r.g.words[kW0 + 220] = kY0, r.g.words[kY0] = 0x8206C7A0, r.g.words[kY0 + 12] = 0;   // H = 0: the game copies nothing
    r.g.PutFloat(kW0 + 252, 1.0f);
    r.g.words[kWF] = 0x8206AF90, r.g.words[kWF + 296] = 0, r.g.words[kWF + 220] = kYF, r.g.words[kYF] = kFvt, r.g.words[kFvt + 8] = 0x82001234;   // a foreign class: count unknown, its slot 2 is named
    r.g.PutFloat(kWF + 252, 1.0f);
    MakeManager(r.g, 3, {7, 8, 9}, {kW, kW0, kWF});
    r.g.WriteNodes(kWBuf, 4, 61), r.g.WriteNodes(kW0Buf, 4, 62), r.g.WriteNodes(kWFBuf, 4, 63);
    const CallResult a = r.Call(kW, kWBuf, 520);
    const CallResult z = r.Call(kW0, kW0Buf, 520);
    const CallResult f = r.Call(kWF, kWFBuf, 520);
    FrameBuffer fb;
    r.C.tracker.CloseBefore(521);
    const bool got = r.C.tracker.TakeNext(fb);
    bool a_rows = false;
    for (const W::WeaponCall& c : fb.calls)
      if (c.object == kW) a_rows = RowsAre(fb, c, 3, 61);
    const Stats s = r.C.tracker.GetStats();
    Check(got && a.recorded && a.counted && a.data_leaf && a.n == 3 && a.n_nodes == 3 && !a.rigid && a_rows && s.data_leaf == 2, "wn.data_leaf_rows",
          "a weapon forwarding into the data object: n = [[y+12]+24] = 3 rows from its palette (the draw's memcpy of n << 6 bytes), data_leaf " + std::to_string(s.data_leaf));
    Check(z.recorded && z.counted && z.n == 0 && z.n_nodes == 0 && !z.rigid && s.zero_nodes == 1, "wn.zero_count_no_rows", "[y+12] == 0 -> n = 0: a counted call with 0 rows, not a guessed rigid row");
    Check(f.recorded && !f.counted && f.n_nodes == 0 && f.fwd_vtable == kFvt && f.fwd_fn == 0x82001234u && s.foreign_fwd == 1 && s.count_unknown == 1, "wn.foreign_class_named", "a forward into another class: uncounted, its vtable and slot 2 reported");
  }

  // ---- the existing captures: the call structure of the ring steps ----
  if (!capture.empty()) {
    const uint32_t object = uint32_t(std::strtoul(object_s.c_str(), nullptr, 0));
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(capture) / "scene", ec))
      if (e.path().extension() == ".rsc") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    uint64_t frames_with = 0, bad_frames = 0, max_calls = 0, other_calls = 0, ordinal_bad = 0, rows_bad = 0, records = 0;
    uint64_t hook_frame = 1000;
    Rig r;
    r.C.tracker.plant = plant;
    r.g.MakeModel(object, 1, 1.0f);
    MakeManager(r.g, 1, {32}, {object});
    const uint32_t buf = 0xB0100000;
    std::map<uint32_t, uint32_t> seeds;   // call index -> seed, per frame
    for (const auto& p : files) {
      SceneTables t;
      std::string err;
      if (!ReadRecord(p.string(), t, err)) continue;
      ++records;
      // the refs of this record, by hook frame (a record can carry more than one frame): the calls of `object` in ordinal order, the other objects' calls interleaved first
      std::map<uint32_t, std::vector<LightRef>> by_frame;
      for (const LightRef& lr : t.light_refs) by_frame[lr.frame].push_back(lr);
      for (auto& kv : by_frame) {
        std::vector<LightRef>& refs = kv.second;
        std::sort(refs.begin(), refs.end(), [](const LightRef& a, const LightRef& b) { return a.object != b.object ? a.object < b.object : a.ordinal < b.ordinal; });
        const uint64_t f = ++hook_frame;
        uint32_t expect_calls = 0;
        for (const LightRef& lr : refs) {
          if (lr.object == object) {
            r.g.WriteNodes(buf, 1, uint32_t(f * 8 + lr.ordinal));   // the SAME buffer for every instance, rewritten between the calls
            r.Call(object, buf, f);
            ++expect_calls;
          } else {
            r.g.MakeModel(lr.object, 1, 1.0f);   // a non-weapon model: never recorded
            r.Call(lr.object, buf, f);
            ++other_calls;
          }
        }
        if (expect_calls == 0) continue;
        r.C.tracker.CloseBefore(f + 1);
        FrameBuffer fb;
        bool found = false;
        while (r.C.tracker.TakeNext(fb)) {
          if (fb.frame != f) continue;
          found = true;
          uint32_t seen = 0;
          for (const W::WeaponCall& c : fb.calls) {
            if (c.object != object) continue;
            if (c.instance != seen) ++ordinal_bad;
            if (!RowsAre(fb, c, 1, uint32_t(f * 8 + seen))) ++rows_bad;
            ++seen;
          }
          if (seen != expect_calls) ++bad_frames;
          max_calls = std::max<uint64_t>(max_calls, seen);
          ++frames_with;
        }
        if (!found) ++bad_frames;
      }
    }
    Check(records > 0 && frames_with > 0, "capture.read", "records " + std::to_string(records) + ", frames with calls of " + object_s + " " + std::to_string(frames_with) + ", max calls a frame " + std::to_string(max_calls));
    Check(frames_with > 0 && bad_frames == 0 && max_calls >= 2, "capture.instances", "every frame keeps one row per LightRef call (the steps are 2 instances of one object), frames lost " + std::to_string(bad_frames));
    Check(ordinal_bad == 0 && rows_bad == 0, "capture.rows_at_call", "instance = the LightRef ordinal in every frame; each row = the buffer of its own call (bad " + std::to_string(ordinal_bad) + " / " + std::to_string(rows_bad) + "); other models' calls ignored: " + std::to_string(other_calls));
  }

  std::printf("%d checks, %d failed%s\n", g_checks, g_fail, plant_name.empty() ? "" : (" (plant " + plant_name + ")").c_str());
  return g_fail ? 1 : 0;
}
