// wwe2k14 - run-ins on demand (Session 16, item 9; EXPERIMENT, default off). What is known (PLAYBOOK Session 16 item 9):
//   - The game has a run-in ("interference") manager, pointer at 0x83763B58, created at match load by the scene setup
//     sub_825C0EC8 (ctor sub_82599B78) only when sub_825999F8() returns 1: the "playable interference" flag
//     (sub_82B5AA90), or match settings G+32 (0x8376AB14) set with a simple match (G flags +4/+6/+9/+20/+41/+59/+60
//     clear, <= 4 wrestlers, G+18576 in {0, 3, 7}, no character id 2100..2152). Exhibitions leave G+32 at 0.
//   - Its current entry is at manager +4080 (+72 = the interferer's character slot 0..8 or -1, +76 / +88 / +92 flags);
//     the Lua natives IsPlayableInterFerence / IsPlayableInterFerenceEndPossible read them.
//   - A per-frame controller (class with vtable 0x82029418, update sub_82723BD8 -> sub_827237D8) picks a wrestler in the
//     ring at random, stores a reason (2 / 3 / 4) at its +68 and calls sub_8259B1E8(wrestler, 0), which re-checks
//     sub_825999F8 and calls the trigger sub_8259A5A0(manager, wrestler).
// Settings:
//   --wwe2k14_runin=true        sub_825999F8 returns 1 (logged once): the manager is created in any match and triggers
//                               are allowed. UNSAFE outside simple 1v1s (the game's own condition excludes Rumbles etc.).
//   F9 / cvar wwe2k14_runin_now 1  (hotkey / script) at the next frame, call the game's trigger sub_8259B1E8(wrestler, 0)
//                               for the wrestler in slot --wwe2k14_runin_target (default -1 = a wrestler the trigger's
//                               team check accepts: engaged with an opponent of another team, else one not on team 0);
//                               logs the manager's entry before / after. Resets to 0.
//   --wwe2k14_runin_char=ID     who runs in: for the moment of the trigger, the target's run-in list (5 character ids,
//                               halfwords at record + 70; record = [0x8376F3C0] + 204 * index, index = halfword at
//                               0x835DDF90 + 2 * id: sub_822C0A78 / sub_827DA788 / sub_822C0BF8) is set to {512 x 4,
//                               ID} and put back right after. The trigger walks the list from the end (run 1298: Bryan's
//                               list 371, 368, 370, 367, 369 -> Seth Rollins 369 came out). -1 = the game's own list.
// Session 17 additions:
//   --wwe2k14_runin_force=true  our triggers skip the chance roll (the word at 0x83763A0C, 50 in exhibition, read by
//                               both rolls in sub_8259A5A0 as `lwz r31,604(r21)`) by setting it to 100 for the call.
//   --wwe2k14_runin_second=N    the second character: the trigger builds the run-in entry (sub_8259CA88, called from
//                               sub_8259A5A0 at 0x8259B180) from TWO records: A = the interferer (from the target's
//                               list), B = a character from the INTERFERER's own list, walked from the end, skipping
//                               ones already in the match (run 1324: Bryan -> Rollins 369, Rollins' list [107, 139,
//                               353, 103, 324] -> B = Undertaker 103, flag 1: the "Undertaker mystery" of Session 16).
//                               -1 = the game's choice, 0 = none, ID = that character.
//   --wwe2k14_runin_probe=true  log every call of the run-in functions (diagnosis).
//   Replay: each trigger is written to a recording as "E F runin target=.. char=.. second=.. force=.." and replayed on
//   frame F (src/replay.h events); while a recording plays, triggers from F9 / the cvar are ignored.
// What the trigger needs (sub_8259A5A0, non-Universe path): the manager not busy (+4084 == 0), the target's current
// opponent (+2672) not the referee and on another team (none counts as team 0: so slot 0 alone never works), a
// rand() % 100 roll under the chance at 0x83763A0C (50 in exhibition), and a list entry that passes its checks.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/ui/keybinds.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "frame_stats.h"
#include "kernel_overrides.h"  // g_vdswap_hook
#include "replay.h"            // run-in events in recordings (Session 17)

REXCVAR_DEFINE_BOOL(wwe2k14_runin, false, "wwe2k14",
                    "Run-ins (Session 16 experiment): force the game's run-in enable check (sub_825999F8) to pass, so "
                    "its interference manager exists in any match. Unsafe outside simple 1v1s. See src/runin.h");
REXCVAR_DEFINE_INT32(wwe2k14_runin_now, 0, "wwe2k14",
                     "Run-ins: set to 1 (cvar in a script) to call the game's trigger once at the next frame");
REXCVAR_DEFINE_INT32(wwe2k14_runin_char, -1, "wwe2k14",
                     "Run-ins: character id that runs in (replaces the target's run-in list for the trigger); -1 = the "
                     "game's own choice");
REXCVAR_DEFINE_INT32(wwe2k14_runin_target, -1, "wwe2k14",
                     "Run-ins: character slot (0..5) the run-in targets; -1 = a wrestler the game's team check accepts");

REXCVAR_DEFINE_BOOL(wwe2k14_runin_force, false, "wwe2k14",
                    "Run-ins (Session 17): skip the game's chance roll (50% in exhibition) for our triggers, so a "
                    "trigger always starts a run-in when the game would allow one at all");
REXCVAR_DEFINE_INT32(wwe2k14_runin_second, -1, "wwe2k14",
                     "Run-ins (Session 17): the second character who comes out after the interferer (the game takes "
                     "one from the interferer's own rivals list): -1 = the game's choice, 0 = none, ID = this one");
REXCVAR_DEFINE_BOOL(wwe2k14_runin_probe, false, "wwe2k14",
                    "Run-ins (Session 17 diagnosis): log every call into the game's run-in functions (trigger "
                    "sub_8259B1E8 / sub_8259A5A0, controller path sub_8259A268, entry setup sub_8259CA88) with the "
                    "guest caller, the wrestler's character id and the two 48-byte records the entry is built from");

REX_EXTERN(__imp__sub_825999F8);
REX_EXTERN(__imp__sub_8259B1E8);
REX_EXTERN(__imp__sub_8259A5A0);
REX_EXTERN(__imp__sub_8259A268);
REX_EXTERN(__imp__sub_8259CA88);
REX_EXTERN(sub_8259B1E8);

namespace wwe2k14::runin {

constexpr uint32_t kManagerPtr = 0x83763B58;
constexpr uint32_t kSlotTable = 0x836F38D0;
inline std::atomic<bool> g_forced_logged{false};
inline void (*g_prev_hook)(uint64_t) = nullptr;

inline uint32_t U32(const uint8_t* base, uint32_t a) {
  return (uint32_t(base[a]) << 24) | (uint32_t(base[a + 1]) << 16) | (uint32_t(base[a + 2]) << 8) | base[a + 3];
}

inline void LogEntry(const uint8_t* base, const char* when, uint64_t frame) {
  const uint32_t mgr = U32(base, kManagerPtr);
  const uint32_t ent = mgr ? U32(base, mgr + 4080) : 0;
  if (!ent) {
    REXLOG_INFO("[wwe2k14][runin] {} (frame {}): manager {:08X}, no entry", when, frame, mgr);
    return;
  }
  REXLOG_INFO("[wwe2k14][runin] {} (frame {}): manager {:08X} entry {:08X}: slot {} +76 {} +88 {} +92 {}", when, frame,
              mgr, ent, int32_t(U32(base, ent + 72)), int32_t(U32(base, ent + 76)), int32_t(U32(base, ent + 88)),
              int32_t(U32(base, ent + 92)));
}

// A character's run-in relations: 5 halfwords at record + 70 (record = [0x8376F3C0] + 204 * index, index = halfword at
// 0x835DDF90 + 2 * id; sub_822C0A78 / sub_827DA788 / sub_822C0BF8). 0 = no record.
inline uint32_t RelationList(const uint8_t* base, int32_t id) {
  if (id < 0 || id >= 500) return 0;
  const uint32_t index = (uint32_t(base[0x835DDF90 + 2 * id]) << 8) | base[0x835DDF91 + 2 * id];
  return index == 512 ? 0 : U32(base, 0x8376F3C0) + 204 * index + 70;
}
inline int32_t ListEntry(const uint8_t* base, uint32_t list, int i) {
  return int16_t((uint16_t(base[list + 2 * i]) << 8) | base[list + 2 * i + 1]);
}

// Temporary edits to game data for one trigger call, undone in reverse order.
struct Patches {
  struct Saved {
    uint32_t addr;
    std::vector<uint8_t> bytes;
  };
  std::vector<Saved> saved;
  uint8_t* base;
  explicit Patches(uint8_t* b) : base(b) {}
  void Write(uint32_t addr, const uint8_t* data, size_t n) {
    saved.push_back({addr, std::vector<uint8_t>(base + addr, base + addr + n)});
    std::memcpy(base + addr, data, n);
  }
  void SetList(uint32_t list, int32_t last) {  // {512 x 4, last} (last = 512: an empty list)
    uint8_t v[10];
    for (int i = 0; i < 4; ++i) v[2 * i] = 0x02, v[2 * i + 1] = 0x00;
    v[8] = uint8_t(uint32_t(last) >> 8), v[9] = uint8_t(last);
    Write(list, v, 10);
  }
  ~Patches() {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) std::memcpy(base + it->addr, it->bytes.data(), it->bytes.size());
  }
};

struct TriggerArgs {
  int target = -1;    // slot, -1 = auto
  int who = -1;       // interferer id, -1 = the game's choice
  int second = -1;    // second character (the interferer's rival): -1 = the game's choice, 0 = none, else the id
  bool force = false; // skip the game's chance roll
  std::string Text() const {
    return fmt::format("runin target={} char={} second={} force={}", target, who, second, force ? 1 : 0);
  }
  static TriggerArgs Parse(const std::string& text) {
    TriggerArgs a;
    std::istringstream in(text);
    std::string kv;
    while (in >> kv) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = kv.substr(0, eq);
      const int v = std::atoi(kv.c_str() + eq + 1);
      if (k == "target") a.target = v;
      else if (k == "char") a.who = v;
      else if (k == "second") a.second = v;
      else if (k == "force") a.force = v != 0;
    }
    return a;
  }
};

constexpr uint32_t kChanceAddr = 0x83763A0C;  // the run-in chance (50 in exhibition), read as a word by both rolls

// Calls the game's trigger sub_8259B1E8(wrestler, 0) now (guest thread, VdSwap). Returns whether a run-in started.
inline bool Trigger(uint64_t frame, const TriggerArgs& a) {
  uint8_t* base = REX_KERNEL_MEMORY()->virtual_membase();
  const uint32_t mgr = U32(base, kManagerPtr);
  if (!mgr) {
    REXLOG_WARN("[wwe2k14][runin] trigger at frame {}: no run-in manager in this match (start the match with "
                "--wwe2k14_runin=true)", frame);
    return false;
  }
  // The target: a wrestler (role +3720 == 0), as the game's controller picks one.
  int slot = a.target;
  uint32_t ch = 0;
  if (slot >= 0 && slot < 6) {
    ch = U32(base, kSlotTable + 4 * slot);
  } else {
    // A wrestler the trigger can work on (sub_8259A5A0's team check): engaged with an opponent (+2672) of another
    // team that is not the referee, else one whose own team (+1480) is not 0 ("no opponent" counts as team 0).
    for (int i = 0; i < 6 && !ch; ++i) {
      const uint32_t c = U32(base, kSlotTable + 4 * i);
      if (!c || U32(base, c + 3720) != 0) continue;
      const uint32_t opp = U32(base, c + 2672);
      if (opp && U32(base, opp + 3720) != 1 && U32(base, opp + 1480) != U32(base, c + 1480)) ch = c, slot = i;
    }
    for (int i = 0; i < 6 && !ch; ++i) {
      const uint32_t c = U32(base, kSlotTable + 4 * i);
      if (c && U32(base, c + 3720) == 0 && !U32(base, c + 2672) && U32(base, c + 1480) != 0) ch = c, slot = i;
    }
    for (int i = 0; i < 6 && !ch; ++i) {
      const uint32_t c = U32(base, kSlotTable + 4 * i);
      if (c && U32(base, c + 3720) == 0) ch = c, slot = i;
    }
  }
  if (!ch) {
    REXLOG_WARN("[wwe2k14][runin] trigger at frame {}: no wrestler in slot {}", frame, slot);
    return false;
  }
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts || !ts->context()) return false;
  LogEntry(base, "before the trigger", frame);
  bool started = false;
  {
    Patches patch(base);
    // Who runs in: the target's list -> {512 x 4, ID} for the trigger (restored below).
    const int32_t target_id = int32_t(U32(base, ch + 776) / 100);  // character + costume (e.g. 32402) -> id
    const uint32_t target_list = RelationList(base, target_id);
    if (a.who >= 0 && a.who < 500) {
      if (target_list) {
        patch.SetList(target_list, a.who);
        REXLOG_INFO("[wwe2k14][runin] target id {}: run-in list set to {{512 x4, {}}} for the trigger", target_id, a.who);
      } else {
        REXLOG_WARN("[wwe2k14][runin] target id {} has no character record: char={} ignored", target_id, a.who);
      }
    }
    // The second character: the trigger takes it from the INTERFERER's own list, walked from the end, skipping
    // characters already in the match (Session 17, run 1324: Bryan -> Rollins 369, then Rollins' list [107, 139, 353,
    // 103, 324] -> Undertaker 103). second=0: every candidate interferer's list emptied; second=ID: set to {512 x 4, ID}.
    if (a.second >= 0 && target_list) {
      for (int i = 0; i < 5; ++i) {
        const int32_t cand = ListEntry(base, target_list, i);  // (already the patched list when char= is given)
        if (cand == 512 || cand < 0) continue;
        const uint32_t l = RelationList(base, cand);
        if (l) patch.SetList(l, a.second == 0 ? 512 : a.second);
      }
      REXLOG_INFO("[wwe2k14][runin] second character: {} for the trigger", a.second == 0 ? std::string("none")
                                                                                         : std::to_string(a.second));
    }
    if (a.force) {
      const uint32_t chance = U32(base, kChanceAddr);
      const uint8_t hundred[4] = {0, 0, 0, 100};
      patch.Write(kChanceAddr, hundred, 4);
      REXLOG_INFO("[wwe2k14][runin] force: chance {} -> 100 for the trigger", chance);
    }
    PPCContext& ctx = *ts->context();
    const PPCContext saved = ctx;
    ctx.r3.u64 = ch;
    ctx.r4.u64 = 0;
    sub_8259B1E8(ctx, base);
    ctx = saved;
    started = U32(base, mgr + 4084) != 0;
  }
  REXLOG_INFO("[wwe2k14][runin] trigger: sub_8259B1E8(slot {} character {:08X}, 0) called at frame {} ({})", slot, ch,
              frame, a.Text());
  LogEntry(base, "after the trigger", frame);
  REXLOG_INFO("[wwe2k14][runin] manager busy word +4084 = {:08X} ({})", U32(base, mgr + 4084),
              started ? "a run-in is on" : "no run-in");
  return started;
}

// Replay playback: "runin ..." events from the recording run the trigger on their frame.
inline void OnReplayEvent(uint64_t frame, const std::string& text) {
  if (text.rfind("runin", 0) != 0) return;
  Trigger(frame, TriggerArgs::Parse(text));
}

inline void OnSwap(uint64_t frame) {
  if (g_prev_hook) g_prev_hook(frame);
  if (REXCVAR_GET(wwe2k14_runin_now) == 0) return;
  rex::cvar::SetFlagByName("wwe2k14_runin_now", "0");
  if (replay::g_replay.Playing()) {
    REXLOG_INFO("[wwe2k14][runin] trigger at frame {} ignored: a recording is playing (its own run-ins replay on "
                "their frames)", frame);
    return;
  }
  TriggerArgs a;
  a.target = REXCVAR_GET(wwe2k14_runin_target);
  a.who = REXCVAR_GET(wwe2k14_runin_char);
  a.second = REXCVAR_GET(wwe2k14_runin_second);
  a.force = REXCVAR_GET(wwe2k14_runin_force);
  // Recorded before the call, so the line carries the frame it ran on whatever the trigger does.
  replay::g_replay.RecordEvent(frame, a.Text());
  Trigger(frame, a);
}

// The hotkey (UI side, from the app's OnCreateDialogs like CineCam's F8): F9 = trigger a run-in at the next frame.
inline void InstallBind() {
  if (!REXCVAR_GET(wwe2k14_runin)) return;
  rex::ui::RegisterBind("bind_wwe2k14_runin", "F9", "Trigger a run-in (wwe2k14 experiment)",
                        [] { rex::cvar::SetFlagByName("wwe2k14_runin_now", "1"); });
  REXLOG_INFO("[wwe2k14][runin] F9 (bind_wwe2k14_runin) triggers a run-in");
}

inline void Install() {
  if (!REXCVAR_GET(wwe2k14_runin)) return;
  g_prev_hook = wwe2k14::g_vdswap_hook;
  wwe2k14::g_vdswap_hook = &OnSwap;
  replay::g_event_handlers.push_back(&OnReplayEvent);  // "runin ..." lines of a recording being played
  REXLOG_INFO("[wwe2k14][runin] run-in experiment armed: enable check forced, trigger with `cvar wwe2k14_runin_now 1`");
}

}  // namespace wwe2k14::runin

// The game's run-in enable check (see the header): forced to 1 with --wwe2k14_runin.
REX_HOOK_RAW(sub_825999F8) {
  __imp__sub_825999F8(ctx, base);
  if (REXCVAR_GET(wwe2k14_runin) && ctx.r3.u32 == 0) {
    ctx.r3.u64 = 1;
    if (!wwe2k14::runin::g_forced_logged.exchange(true))
      REXLOG_INFO("[wwe2k14][runin] enable check sub_825999F8 forced to 1 (frame {})", wwe2k14::g_guest_frame.load());
  }
}

// --wwe2k14_runin_probe (Session 17, Track 2 diagnosis): who calls the run-in functions, with what.
namespace wwe2k14::runin {
inline int32_t CharId(const uint8_t* base, uint32_t ch) {
  if (!ch || ch < 0x40000000u) return -1;
  return int32_t(U32(base, ch + 776) / 100);
}
inline std::string Words(const uint8_t* base, uint32_t a, int n) {
  std::string out;
  if (!a || a < 0x40000000u) return "-";
  for (int i = 0; i < n; ++i) out += fmt::format("{}{:08X}", i ? " " : "", U32(base, a + 4 * i));
  return out;
}
}  // namespace wwe2k14::runin

REX_HOOK_RAW(sub_8259B1E8) {  // trigger wrapper (wrestler, 0)
  if (!REXCVAR_GET(wwe2k14_runin_probe)) return __imp__sub_8259B1E8(ctx, base);
  const uint32_t lr = uint32_t(ctx.lr), ch = ctx.r3.u32, a1 = ctx.r4.u32;
  REXLOG_INFO("[wwe2k14][runin][probe] sub_8259B1E8(character {:08X} id {}, {}) from lr {:08X} frame {}", ch,
              wwe2k14::runin::CharId(base, ch), a1, lr, wwe2k14::g_guest_frame.load());
  __imp__sub_8259B1E8(ctx, base);
}
REX_HOOK_RAW(sub_8259A5A0) {  // trigger (manager, wrestler)
  if (!REXCVAR_GET(wwe2k14_runin_probe)) return __imp__sub_8259A5A0(ctx, base);
  const uint32_t lr = uint32_t(ctx.lr), mgr = ctx.r3.u32, ch = ctx.r4.u32;
  __imp__sub_8259A5A0(ctx, base);
  REXLOG_INFO("[wwe2k14][runin][probe] sub_8259A5A0(manager {:08X}, character {:08X} id {}) -> {:08X}, busy {:08X}, "
              "from lr {:08X} frame {}", mgr, ch, wwe2k14::runin::CharId(base, ch), ctx.r3.u32,
              mgr ? wwe2k14::runin::U32(base, mgr + 4084) : 0, lr, wwe2k14::g_guest_frame.load());
}
REX_HOOK_RAW(sub_8259A268) {  // the controller's other path (manager, wrestler, 101)
  if (!REXCVAR_GET(wwe2k14_runin_probe)) return __imp__sub_8259A268(ctx, base);
  const uint32_t lr = uint32_t(ctx.lr), mgr = ctx.r3.u32, ch = ctx.r4.u32, a2 = ctx.r5.u32;
  __imp__sub_8259A268(ctx, base);
  REXLOG_INFO("[wwe2k14][runin][probe] sub_8259A268(manager {:08X}, character {:08X} id {}, {}) -> {:08X} from lr "
              "{:08X} frame {}", mgr, ch, wwe2k14::runin::CharId(base, ch), a2, ctx.r3.u32, lr,
              wwe2k14::g_guest_frame.load());
}
REX_HOOK_RAW(sub_8259CA88) {  // entry setup (entry, record A (48 bytes), record B (48 bytes))
  if (REXCVAR_GET(wwe2k14_runin_probe)) {
    REXLOG_INFO("[wwe2k14][runin][probe] sub_8259CA88(entry {:08X}) from lr {:08X} frame {}: A [{}] B [{}]",
                ctx.r3.u32, uint32_t(ctx.lr), wwe2k14::g_guest_frame.load(),
                wwe2k14::runin::Words(base, ctx.r4.u32, 12), wwe2k14::runin::Words(base, ctx.r5.u32, 12));
  }
  __imp__sub_8259CA88(ctx, base);
}
