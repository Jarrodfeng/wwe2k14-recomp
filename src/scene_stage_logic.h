// wwe2k14 - the stage push, pure part (Session 101 lane 38: "play any scene from the main menu").
//
// No game or SDK includes, so tools/goto/scene_stage_test.cpp compiles this exact file and the offline gate (tools/goto/stage_selftest.py) runs the
// shipped code: the "is the stage ready" test and the bytes of the one-record fake story the push hands to the game's own sub_82A30F08.
// src/scene_director.h does the guest reads / calls around it. Design and marks: docs/research/s101/GOANY_scene_from_menu.md
// ([C] = read in a listing by the design reader or by lane 38, [L] = a lead, [U] = unknown). Every address and offset below is from that page
// or from a listing marked here; none is guessed.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace wwe2k14::scene_stage {

// ---- guest addresses (globals) ----
constexpr uint32_t kManagerPtr = 0x836F2E58;     // R = scene manager (sub_8230F390 loads it, listing 0x8230F390) [C]
constexpr uint32_t kG = 0x8376AAF4;              // G = sub_827D7578() [C: cutscene_motion.md]
constexpr uint32_t kStoryRootPtr = 0x839295CC;   // M = the Create a Story root [C: design Q1 table, 0x82A30F18]
constexpr uint32_t kParticipantsPtr = 0x836F2D98;  // participant array [C: design 0x82A30F3C]
constexpr uint32_t kCameraTablePtr = 0x836F3AA8;   // the handler stores 1 at [this]+8 before the call [C: design, 0x82A41290-0x82A412C4]
constexpr uint32_t kUiManagerPtr = 0x836F2E54;     // [this]+156 is the story command's first gate [C: sub_8232D810 0x8232D820-0x8232D83C; meaning U]

// ---- offsets from G ----
constexpr uint32_t kGameMode = 18576;     // "story mode" when == 4 [C: 0x8232D86C, 0x8231A5F0]
constexpr uint32_t kGameModeStory = 4;
// ---- offsets from R ----
constexpr uint32_t kRingRead = 16 + 4;    // ring+4 = READ index [C: cutscene_motion.md]
constexpr uint32_t kRingWrite = 16 + 8;   // ring+8 = WRITE index [C]
constexpr uint32_t kCtxBase = 16 + 12;    // ctx(i) = ring + 12 + 332*i [C]
constexpr uint32_t kCtxSize = 332;
constexpr uint32_t kRingSlots = 10;
constexpr uint32_t kCtxSkip = 4;          // +4 skip flag [C]
constexpr uint32_t kCtxType = 8;          // +8 scene type [C]
constexpr uint32_t kCtxArena = 16;        // +16 arena [C]
constexpr uint32_t kCtxSlot0 = 140;       // +140 slot 0, first word = bank id [C]
constexpr uint32_t kStoryClassObj = 9632; // R+9632 holds the command object pointer (stw r3,9632(r30), listing 0x8231C5C0) [C]
constexpr uint32_t kStoryListener = 5432 + 20;  // the story program registers here [C: design step 3]
constexpr uint32_t kBodyObj = 5456;       // the story body object is EMBEDDED at R+5456 (sub_8231C2C0 = addi r3,r3,5456; ctor sub_8231A918 stores its vtable at +0) [C]
constexpr uint32_t kPending = 5456 + 8;   // "[R+5456]+8" in the design = the word at +8 of the object at R+5456 (sub_8232E350: lwz 8(r3) 0x8232E368) [C]
constexpr uint32_t kBodyValid = 5456 + 4116;    // "body valid" word, written by sub_8231A5D0 [C]
constexpr uint32_t kDone = 9640;          // R+9640 = list done [C]
constexpr uint32_t kState = 9648;         // R+9648 = state function pointer [C]
constexpr uint32_t kStoryClassVtable = 0x8200A410;  // sub_8232D1A0 class [C: design step 5]
constexpr uint32_t kStageBank = 4025;     // the default scene (sub_82A19A18) [C]
constexpr uint32_t kCamFlag = 8;          // [0x836F3AA8] + 8 = 1 [C: design]
constexpr uint32_t kUiGate = 156;         // [0x836F2E54] + 156 [C read; U meaning]

// ---- the one-record fake story F (25,360 bytes = S+21248+4112, design "Push") ----
constexpr uint32_t kStoryOffsetInRoot = 40;   // M+40 = S = the story [C: sub_82A266C0]
constexpr uint32_t kFakeStorySize = 25360;
constexpr uint32_t kStorySelection = 88;      // u16 S+88 != 0xFFFF = i [C]
constexpr uint32_t kStoryVariant = 1100;      // S+1100 != 0 picks a variant; keep 0 [C]
constexpr uint32_t kStoryIndexTable = 1248;   // u16 at S+1248+8i = k [C]
constexpr uint32_t kRecordOffset = 21248;     // record = S+21248+4112k [C]
constexpr uint32_t kRecordSize = 4112;
constexpr uint32_t kBodyOffset = kRecordOffset + 8;  // body = record+8 [C]
constexpr uint32_t kBodyBank = 0;             // body+0 u16 scene bank id [C]
constexpr uint32_t kBodyArena = 2;            // body+2 u16 arena [C]
constexpr uint32_t kBodyCount = 5;            // body+5 u8 performer count [C]
constexpr uint32_t kBodyPerformers = 16;      // body+16+12j [C]
constexpr uint32_t kPerformerSize = 12;
constexpr uint32_t kPerformerNone = 512;      // linked created character: 512 = none [C]
constexpr uint32_t kMaxPerformers = 6;        // a scene context holds 6 performers (ctx+44.., 16 bytes each) [C: cutscene_motion.md]
constexpr uint32_t kCallerObjectSize = 64;    // the argument object O: only +32 is written by the callee [C: design Q1, 0x82A31424]

// The words the ready test reads (the director fills it from the guest on the owner update).
struct StageWords {
  uint32_t seq = 0;          // SequenceId()
  uint32_t r = 0;            // [0x836F2E58]
  uint32_t g_mode = 0;       // G+18576
  uint32_t cmd_obj = 0;      // R+9632
  uint32_t cmd_vtable = 0;   // [R+9632]
  uint32_t ring_read = 0;    // R+20
  uint32_t ring_write = 0;   // R+24
  uint32_t slot0 = 0;        // ctx(read) +140 (0 when the read index is out of range)
  uint32_t listener = 0;     // R+5452
  uint32_t pending = 0;      // R+5464
  uint32_t story_root = 0;   // M
  uint32_t participants = 0; // [0x836F2D98]
  uint32_t ui_manager = 0;   // [0x836F2E54]
  uint32_t ui_gate = 0;      // [ui_manager]+156 (0 when there is no manager)
};

// "" = the stage is ready, else the first reason it is not. held_bank 0: the stage as built (ctx(read) slot 0 must be 4025: the "stage: ready" line).
// held_bank != 0: also accept the bank a previous push left holding (WSD / Universe / entrance banks 1-699 use the story class, which holds at its
// end until the next push: design "After the scene"), so the next scene can be pushed.
inline std::string NotReady(const StageWords& w, uint32_t held_bank) {
  if (w.seq != 9) return "the in-game sequence is not up (sequence " + std::to_string(w.seq) + ", want 9)";
  if (w.r == 0) return "the scene manager R is null";
  if (w.g_mode != kGameModeStory) return "G+18576 is " + std::to_string(w.g_mode) + ", not story mode (4)";
  if (w.cmd_obj == 0) return "R+9632 (the command object) is null";
  if (w.cmd_vtable != kStoryClassVtable) return "the command object is not the story class (vtable word differs from 0x8200A410)";
  if (w.ring_read >= kRingSlots || w.ring_write >= kRingSlots) return "the ring indexes are out of range";
  if (w.slot0 != kStageBank && !(held_bank != 0 && w.slot0 == held_bank)) {
    return "ctx(read) slot 0 is " + std::to_string(w.slot0) + ", not the stage bank 4025" + (held_bank ? " or the held bank " + std::to_string(held_bank) : std::string());
  }
  if (w.listener == 0) return "the story listener (R+5432+20) is not registered";
  if (w.pending != 0) return "a push is already pending (R+5456+8 != 0)";
  if (w.ring_write != (w.ring_read + 1) % kRingSlots) return "ring write is not read+1 (a context is queued)";
  if (w.story_root == 0) return "the story root M is null";
  if (w.participants == 0) return "the participant array is null";
  if (w.ui_manager != 0 && w.ui_gate == 0) {
    return "the UI manager [0x836F2E54] exists and its +156 is 0: the story command then uses the base end rule, so the stage does not hold and a push would be skipped";
  }
  return "";
}

// S101 lane 59 (docs/research/s101/STAGE_CRASH.md Q3, run 7066): does a pushed type-4 context of this bank get the story command class, the only one that HOLDS in the
// Create a Story stage? The game's own rule, sub_8231C538 (0x8231C560-0x8231C5C0): 4000-5999 (sub_82313F30), or 1-699 with context type 4 (sub_823116C8; ours are type 4).
// Any other bank (Season 1000-3999, 700+ entrances, Winning 6000+) gets another class, ends at its own end and ends the stage WITHOUT the orderly stop: [0x839294B8] /
// [0x839294B4] stay stale and the main menu then reads them (the run 7066 crash). So `!scene play` refuses such a bank. [C: the rule; the crash chain]
inline bool HoldsInStage(uint32_t bank) { return (bank >= 1 && bank <= 699) || (bank >= 4000 && bank <= 5999); }

// ---- S101 lane 72 (docs/research/s101/GOANY_stage_set_and_leave.md): the arena, and the way out ----

// Guest addresses / offsets the arena log and the leave verb read. Each from the design page (listing marks there); none guessed.
constexpr uint32_t kStageLoadMgrPtr = 0x83924738;  // SL = the stage / arena load manager (made by sub_827E12C8; vtable 0x82033B30) [C: design]
constexpr uint32_t kSLArena = 36;                  // SL+36 = the arena last requested (-1 from the main menu) [C: design 0x827E4090]
constexpr uint32_t kLoadedStagePtr = 0x83763544;   // CS = the loaded stage object (stored by sub_825E6078 at 0x825E6178) [C: design]
constexpr uint32_t kGArena = 64;                   // G+64 = the arena the scene machine wants (sub_8232BD60 writes ctx(read)+16 into it) [C: design]
constexpr uint32_t kStoryProgramVtable = 0x82057BD0;  // P (the listener at R+5432+20; ctor sub_82A19C20, 0x82A19C38-0x82A19C4C) [C: design]
constexpr uint32_t kStoryObjectVtable = 0x820585B0;   // Q = P+12 (ctor sub_82A3EAF8, 0x82A3EB1C-0x82A3EB34) [C: design]
constexpr uint32_t kQSlot7Word = kStoryObjectVtable + 28;  // the vtable word of slot 7: a data word pointing at sub_82A3E130 [C: q.py fn 0x82A3E130 "data words pointing at it: 0x820585CC"]
constexpr uint32_t kQCancelFunction = 0x82A3E130;     // Q slot 7 = the editor's cancel finish (sub_823BE7D0: Q+16 = 4, Q+20 = 0) [C: design]
constexpr uint32_t kProgState = 4;     // P+4: 0 wait for M, 1 Q running, 2 finish (M shutdown), 3 wait M gone / delete [C: design sub_82A19D58]
constexpr uint32_t kProgOwnsRoot = 8;  // P+8: 0 = P built M (the shutdown runs on any finish), 1 = M pre-dated P [C: design 0x82A19C68-0x82A19C84]
constexpr uint32_t kProgQ = 12;        // P+12 = Q [C: design]
constexpr uint32_t kQLife = 16;        // Q+16: lifecycle (2 running, 3 / 4 finishing, 5 finished) [C: design sub_823BE6D8 family]
constexpr uint32_t kQMode = 28;        // Q+28 == 2: mode 2, no calendar UI [C: design]
constexpr uint32_t kQExit = 40;        // Q+40: the exit code the program reads (3 = cancel) [C: design 0x82A3ECAC-0x82A3ECC4]
constexpr uint32_t kQExitCancel = 3;
constexpr uint32_t kRootState = 20;    // M+20: 2 up, 3 shutting down, 4 gone [C: design]
constexpr uint32_t kRootStep = 24;     // M+24 [C: design]
constexpr uint32_t kLoaderH = 0x839294B8;  // H: read by the watcher, never written by us [C: STAGE_CRASH.md]
constexpr uint32_t kLoaderL = 0x839294B4;  // L: read by the watcher, never written by us [C: STAGE_CRASH.md]
constexpr uint32_t kIngameProgramPtr = 0x8376AAF0;  // PG = sub_827D7348(); PG+88 = the next action after a story [C: design]
constexpr uint32_t kPGNext = 88;
constexpr uint32_t kLeaveTimeoutFrames = 1800;
constexpr uint32_t kEmptyStageArena = 76;

// The arenas that have a pac\bg\bgNN.pac in the player's files [C: the 57 files in game_data\extracted\PAC\BG, listed by lane 71 and again by lane 72]:
// 0-9, 11-17, 21-23, 28-56, 66-68, 76-80. 76 = the editor's empty stage ("blank" 76 / 77 / 79: sub_824E6FE8). Created arenas (200+) are refused: they need the
// 0x9904C-byte copy the editor's preview handler makes. An id with no file would queue a load of a missing pac [U what the loader does].
inline bool ArenaLoadable(uint32_t n) {
  return n <= 9 || (n >= 11 && n <= 17) || (n >= 21 && n <= 23) || (n >= 28 && n <= 56) || (n >= 66 && n <= 68) || (n >= 76 && n <= 80);
}

// "" = loadable, else the refusal text (no guest write has happened yet when FirePlay returns it).
inline std::string ArenaRefusal(uint32_t n) {
  if (ArenaLoadable(n)) return "";
  if (n >= 200) return "refused: arena " + std::to_string(n) + " is a created arena (200+); it needs the editor's own copy of the arena data, which the push does not make";
  return "refused: arena " + std::to_string(n) + " has no BGnn.PAC in the game files (loadable: 0-9, 11-17, 21-23, 28-56, 66-68, 76-80); the load would ask for a missing pac";
}

// The sentence the play answer ends with, so nobody mistakes the black default for a set.
inline std::string ArenaNote(uint32_t arena, bool given) {
  if (arena == kEmptyStageArena) {
    return given ? "arena 76 = the editor's empty stage (a black stage, no set)" : "arena 76 = the editor's empty stage (a black stage, no set): pass arena=N for a set";
  }
  return "arena " + std::to_string(arena) + (given ? " (loaded through the game's own stage check)" : " (the stage's own, unchanged)");
}

// The words the leave guard reads (the director fills it from the guest on the owner update).
struct LeaveWords {
  uint32_t seq = 0;          // SequenceId()
  uint32_t r = 0;            // [0x836F2E58]
  uint32_t g_mode = 0;       // G+18576
  uint32_t p = 0;            // R+5452 (the listener = the story program P)
  uint32_t p_vtable = 0;     // [P]
  uint32_t p_state = 0;      // P+4
  uint32_t p_owns = 0;       // P+8
  uint32_t q = 0;            // P+12
  uint32_t q_vtable = 0;     // [Q]
  uint32_t q_life = 0;       // Q+16
  uint32_t q_mode = 0;       // Q+28
  uint32_t q_child = 0;      // Q+36 (null in mode 2; slot 7 deletes it when set) [S101 lane 72 review]
  uint32_t q_slot7 = 0;      // the vtable word at 0x820585B0+28
  uint32_t m = 0;            // [0x839295CC]
  uint32_t m_state = 0;      // M+20
  uint32_t pending = 0;      // R+5464
  bool play_pending_ours = false;  // the director's own push has not been committed yet
};

// "" = `!scene leave` may do the editor's cancel, else the first reason it may not. No guest write has happened when this answers.
inline std::string LeaveRefusal(const LeaveWords& w) {
  if (w.seq != 9) return "the in-game sequence is not up (sequence " + std::to_string(w.seq) + ", want 9)";
  if (w.r == 0) return "the scene manager R is null";
  if (w.g_mode != kGameModeStory) return "G+18576 is " + std::to_string(w.g_mode) + ", not story mode (4)";
  if (w.p == 0) return "the story program (R+5432+20) is not registered";
  if (w.p_vtable != kStoryProgramVtable) return "the listener is not the story program (vtable word differs from 0x82057BD0)";
  if (w.p_state == 0) return "busy: the story program is still waiting for its root (P+4 is 0)";
  if (w.p_state == 2 || w.p_state == 3) return "already leaving (P+4 is " + std::to_string(w.p_state) + ")";
  if (w.p_state != 1) return "the story program is in an unknown state (P+4 is " + std::to_string(w.p_state) + ")";
  if (w.q == 0) return "the story object (P+12) is null";
  if (w.q_vtable != kStoryObjectVtable) return "P+12 is not the story object (vtable word differs from 0x820585B0)";
  if (w.q_life != 2) return "the story object is not running (Q+16 is " + std::to_string(w.q_life) + ", want 2)";
  if (w.q_mode != 2) return "the story object is not in mode 2 (Q+28 is " + std::to_string(w.q_mode) + ")";
  if (w.q_child != 0) return "the story object has a child (Q+36 is non-zero; the design has it null in mode 2, slot 7 would delete it)";
  if (w.m == 0) return "the story root M is null";
  if (w.m_state != 2) return "the story root is not up (M+20 is " + std::to_string(w.m_state) + ", want 2)";
  if (w.p_owns != 0) {
    return "the story root pre-dates this stage (P+8 is " + std::to_string(w.p_owns) + "): a cancel would leave the root and its loader objects alive (the run 7066 crash class)";
  }
  if (w.pending != 0 || w.play_pending_ours) return "a push is still pending (R+5464 != 0)";
  if (w.q_slot7 != kQCancelFunction) return "the story object's slot 7 is not the editor's cancel (vtable word differs from 0x82A3E130)";
  return "";
}

// At the main menu after a leave: "" = the listener and the story's loader objects are all gone (the orderly end), else what was left behind. "Sequence 75 with the
// listener registered" is the run 7066 crash precondition (H / L stale), checked on the log words.
inline std::string BackAtMenuProblem(uint32_t listener, uint32_t h, uint32_t l, uint32_t m) {
  std::string s;
  if (listener) s += " listener=" + std::to_string(listener);
  if (h) s += " H=" + std::to_string(h);
  if (l) s += " L=" + std::to_string(l);
  if (m) s += " M=" + std::to_string(m);
  return s.empty() ? "" : "left behind at the main menu:" + s;
}

inline void PutU16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }

// Fills the fake story F (kFakeStorySize bytes, zeroed, then the game's body defaults sub_82A22580(F+kBodyOffset) already applied by the caller)
// with the fields sub_82A30F08 reads: no selection variant, one record at index 0, the bank, the arena, the performers as the "clean path" entry
// {character u16, attire u8, override s8 = -1, u8 0, custom music u8 0, linked created character u16 = 512}. Big-endian, as the guest reads it.
// Returns false (and writes nothing) for arguments the director refuses anyway.
inline bool FillStory(uint8_t* f, uint32_t bank, uint32_t arena, int n, const int32_t* chars, const int32_t* attires) {
  if (!f || bank == 0 || bank > 65535 || arena > 65535 || n < 0 || n > int(kMaxPerformers)) return false;
  PutU16(f + kStorySelection, 0);                // i = 0 (!= 0xFFFF)
  PutU16(f + kStoryIndexTable, 0);               // k = 0
  std::memset(f + kStoryVariant, 0, 4);          // no variant
  f[kRecordOffset] = 0;                          // record+0: no custom-music record
  uint8_t* body = f + kBodyOffset;
  PutU16(body + kBodyBank, bank);
  PutU16(body + kBodyArena, arena);
  body[kBodyCount] = uint8_t(n);
  for (int j = 0; j < n; ++j) {
    uint8_t* e = body + kBodyPerformers + kPerformerSize * j;
    PutU16(e + 0, uint32_t(chars[j]));
    e[2] = uint8_t(attires[j]);
    e[3] = 0xFF;                                 // attire override -1 = none
    // e[4], e[5] stay as the game's body defaults (sub_82A22580) left them: 255 / 255 = none; 255 is outside 200..209, so the custom-music path stays off
    PutU16(e + 6, kPerformerNone);
  }
  return true;
}

}  // namespace wwe2k14::scene_stage
