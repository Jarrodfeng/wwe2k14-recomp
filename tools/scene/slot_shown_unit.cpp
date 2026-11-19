// slot_shown_unit: the offline proof of S103 lane V1 (R17 [C]): the game hides a stage slot when its record byte +9 bit 0x80 is clear (sub_824E7190 0x824E71C8-0x824E71F0), so it gets no light install and no
// draw; our arena contributor must not list it as visible nor ask bit 4 for its light. 0 game runs, no SDK, no game memory.
//   slot_shown_unit [--plant NAME]
// It runs the SHIPPED code: arena_contrib.cpp (Latch, Emit, EmittedSlots, the slot-record reader hook) against a FAKE GUEST of 399 x 12-byte slot records written by hand (byte +8 / +9 as the listing
// tests them), and wrestler_contrib.h's EmittedSkeletons against the real WrestlerContributor.
// A plant is a FAULT in the thing under test; each must make its NAMED check fail (exit 1 when any check failed):
//   ignore_bit      the contributor reads the bits but ignores them (the pre-V1 twin)       -> "arena.hidden_not_emitted"
//   no_reader       no reader is wired                                                        -> "arena.hidden_not_emitted"
//   all_skeletons   EmittedSkeletons-like: the check asks for every kept skeleton (pre-V1)   -> "wrestler.non_slot_not_asked"
//   stale_latch     the second latch is not re-read (a bit flip mid-run is not picked up)    -> "arena.flip_picked_up"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/arena_contrib.h"
#include "../../src/scene_live/wrestler_contrib.h"

using namespace rdr_scene;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;
namespace ar = rdr_scene::live::arena;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// The fake guest: 399 records of 12 bytes. A shown slot has byte 9 = 0x80 (the bit the listing tests); byte 8 = 0.
static uint8_t g_rec[ar::kStageSlotRecords * ar::kStageSlotRecordSize];
static int g_reads = 0;
static void SetRec(int slot, uint8_t b8, uint8_t b9) {
  g_rec[slot * 12 + 8] = b8;
  g_rec[slot * 12 + 9] = b9;
}
static bool ReadRecords(uint8_t* out, size_t n) {
  ++g_reads;
  if (Plant("stale_latch") && g_reads > 1) return false;   // the later latch reads nothing: the first answer must then be gone (old behaviour = nothing hidden) -> flip not picked up
  std::memcpy(out, g_rec, n);
  return true;
}

static ar::SlotSnap Slot(uint16_t s) {
  ar::SlotSnap x;
  x.slot = s;
  x.light_set = 100;
  return x;
}

// One latched frame of the arena contributor: Latch (reads the records), Emit. Returns the visible flag of each slot's base object by slot.
static std::vector<bool> Frame(uint64_t f, std::vector<uint16_t>& emitted, size_t& objects) {
  FrameInfo fi;
  fi.guest_frame = f;
  auto* c = ar::Instance();
  c->Latch(fi);
  SceneTables t;
  Counters cn;
  c->Emit(fi, t, cn);
  objects = t.objects.size();
  ar::EmittedSlots(emitted);
  std::vector<bool> vis;
  for (const Object& o : t.objects) vis.push_back((o.flags & kObjVisible) != 0);
  return vis;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];

  // ---------------------------------------------------------------- the arena
  for (int s = 0; s < int(ar::kStageSlotRecords); ++s) SetRec(s, 0, 0x80);   // every slot shown
  SetRec(301, 0, 0x00);          // the bit clear: the game hides it (a 'screen off' slot lit by 801/802)
  SetRec(302, 0x80, 0x80);       // byte +8 bit 0x80 set: hidden
  SetRec(304, 0, 0x88);          // byte +9 bit 0x08 set: hidden
  SetRec(300, 0, 0x80);          // shown (the control)
  ar::ArenaSnapshot snap;
  snap.arena_id = 4;
  snap.look = ar::Look::Entrance;
  for (uint16_t s : {300, 301, 302, 304}) snap.slots.push_back(Slot(s));
  ar::PublishAll(snap);
  if (!Plant("no_reader")) ar::SetSlotRecordReader(&ReadRecords);
  ar::PlantIgnoreShown(Plant("ignore_bit"));

  // the pure predicate, from the listing's three tests
  {
    uint8_t r[12] = {};
    bool ok = !ar::SlotShown(r);                       // 0x80 clear
    r[9] = 0x80;
    ok = ok && ar::SlotShown(r);
    r[8] = 0x80;
    ok = ok && !ar::SlotShown(r);                      // +8 bit 0x80
    r[8] = 0;
    r[9] = 0x88;
    ok = ok && !ar::SlotShown(r);                      // +9 bit 0x08
    r[9] = 0xF7;
    ok = ok && ar::SlotShown(r);                       // every other bit set: shown
    Check(ok, "arena.predicate", "sub_824E7190 0x824E71C8-0x824E71F0");
  }

  std::vector<uint16_t> emitted;
  size_t n = 0;
  std::vector<bool> vis = Frame(100, emitted, n);
  {
    bool hidden_ok = n == 4 && vis[0] && !vis[1] && !vis[2] && !vis[3];
    bool asked_ok = emitted.size() == 1 && emitted[0] == 300;
    Check(hidden_ok && asked_ok, "arena.hidden_not_emitted", "visible " + std::to_string(int(vis.size() > 1 ? vis[1] : 0)) + " asked " + std::to_string(emitted.size()));
  }
  // a bit flip mid-run: slot 301's bit comes back (the screen goes off) and slot 300 loses it: picked up on the next latch
  SetRec(301, 0, 0x80);
  SetRec(300, 0, 0x00);
  vis = Frame(101, emitted, n);
  Check(vis.size() == 4 && !vis[0] && vis[1] && !vis[2] && !vis[3] && emitted.size() == 1 && emitted[0] == 301, "arena.flip_picked_up",
        "emitted " + std::to_string(emitted.size()) + (emitted.empty() ? "" : " first " + std::to_string(emitted[0])));
  // an unreadable stage (no stage object): nothing is hidden (the old behaviour), never a guess
  ar::SetSlotRecordReader([](uint8_t*, size_t) { return false; });
  vis = Frame(102, emitted, n);
  Check(vis.size() == 4 && vis[0] && vis[1] && vis[2] && vis[3] && emitted.size() == 4, "arena.unreadable_hides_nothing");
  ar::SetSlotRecordReader(nullptr);
  ar::PlantIgnoreShown(false);

  // ---------------------------------------------------------------- the wrestlers
  {
    live::WrestlerContributor w;
    constexpr uint32_t kA = 0x40001000, kB = 0x40002000, kC = 0x40003000, kD = 0x40004000;
    std::vector<Mat4> skin(3);
    for (auto& m : skin) m = IdentityMat4();
    w.SubmitSkin(kA, 0, 3, skin.data(), 99);    // slot 0
    w.SubmitSkin(kB, -1, 3, skin.data(), 99);   // no slot (a manager / waiting entrance model): kept, emitted invisible
    w.SubmitSkin(kC, 1, 3, skin.data(), 99);    // slot 1
    w.SubmitSkin(kD, 1, 3, skin.data(), 99);    // slot 1 again: the second claim is emitted invisible too
    FrameInfo fi;
    fi.guest_frame = 100;
    w.Latch(fi);
    SceneTables t;
    Counters cn;
    w.Emit(fi, t, cn);
    size_t visible = 0;
    for (const Object& o : t.objects) visible += (o.flags & kObjVisible) ? 1 : 0;
    std::vector<uint32_t> asked;
    w.EmittedSkeletons(asked);
    if (Plant("all_skeletons")) w.KeptSkeletons(asked);   // the pre-V1 answer: every kept key (the shipped KeptSkeletons, not a hand-written list)
    bool has_b = false, has_d = false, has_a = false, has_c = false;
    for (uint32_t k : asked) has_a |= k == kA, has_b |= k == kB, has_c |= k == kC, has_d |= k == kD;
    Check(t.objects.size() == 4 && visible == 2 && has_a && has_c && !has_b && !has_d && asked.size() == 2, "wrestler.non_slot_not_asked",
          "objects " + std::to_string(t.objects.size()) + " visible " + std::to_string(visible) + " asked " + std::to_string(asked.size()));
  }

  // S103 V1F F1: two bodies share slot 1, the lower address (kE) is quiet-unseen (the update hook last saw it at frame 10, the game's draw never): Collect writes it slot -1 (invisible), so the SECOND claimant
  // (kC) is the one drawn as actor:slot:1 and the one to ask for light. The old rebuild from last_/known_ asked kE (hidden) and not kC.
  {
    live::WrestlerContributor w;
    constexpr uint32_t kE = 0x40000800, kC = 0x40003000;
    std::vector<Mat4> skin(3);
    for (auto& m : skin) m = IdentityMat4();
    w.SubmitSkin(kE, 1, 3, skin.data(), 10);
    w.SubmitSkin(kC, 1, 3, skin.data(), 99);
    FrameInfo fi;
    fi.guest_frame = 100;
    w.Latch(fi);
    SceneTables t;
    Counters cn;
    w.Emit(fi, t, cn);
    size_t visible = 0;
    for (const Object& o : t.objects) visible += (o.flags & kObjVisible) ? 1 : 0;
    std::vector<uint32_t> asked;
    w.EmittedSkeletons(asked);
    if (Plant("all_skeletons")) w.KeptSkeletons(asked);
    Check(t.objects.size() == 2 && visible == 1 && asked.size() == 1 && asked[0] == kC, "wrestler.quiet_unseen_claim",
          "objects " + std::to_string(t.objects.size()) + " visible " + std::to_string(visible) + " asked " + std::to_string(asked.size()) + (asked.empty() ? "" : " first " + std::to_string(asked[0])));
  }

  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
