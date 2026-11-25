// S110 FIRETABLE: host unit of src/table_fire_core.h (the fire wish's one pressed call of sub_825FB230 and the replay skip's one call of
// sub_826071F8). Built and run by tools/bus/table_fire_selftest.py (clang++ -std=c++17 -I src). Prints one line per failed check and a summary.
#include <cstdio>
#include <map>
#include <string>

#include "table_fire_core.h"

using namespace wwe2k14::table_fire;

static int g_fail = 0, g_n = 0;
static void Check(bool ok, const char* name, const char* what) {
  ++g_n;
  if (!ok) {
    ++g_fail;
    std::printf("FAIL %s: %s\n", name, what);
  }
}

struct Mem {
  std::map<uint32_t, uint32_t> w;
  uint32_t Rd(uint32_t a) const {
    auto it = w.find(a);
    return it == w.end() ? 0 : it->second;
  }
};

int main() {
  // ---- the numbers (each from a listing: FIRETABLE.md 1 / 3) ----
  Check(kCtxPress == 48 && kLinkOff == 1340 && kSeatOff == 1220 && kNoLink == 0xFFFFFFFFu, "numbers.fire", "ctx+48, char+1340, char+1220, -1");
  Check(kCtrlState == 48 && kPlaying == 3 && kSkipFade == 4 && kSeats == 9, "numbers.skip", "ctrl+48, states 3 / 4, 9 seats");

  // ---- Queued: -1 before, a table after ----
  Check(Queued(kNoLink, 5) && Queued(kNoLink, 0), "queued.yes", "-1 -> a table index (0 is a table too)");
  Check(!Queued(kNoLink, kNoLink), "queued.no_link", "-1 -> -1 is no fire");
  Check(!Queued(3, 5) && !Queued(3, 3), "queued.already_linked", "a character linked before the call did not light this one");

  // ---- the store ----
  Clear();
  Check(FireState(0) == -1 && g_armed.load() == 0 && !Armed(0), "store.empty", "no wish after Clear");
  Check(SetFire(9) == "no such seat" && SetFire(-1) == "no such seat" && g_armed.load() == 0, "store.bad_seat", "seat 9 / -1 refused");
  SetFire(0);
  Check(Armed(0) && !Armed(1) && FireState(0) == 0 && FireState(1) == -1 && g_armed.load() == 1, "store.armed", "seat 0 armed only");
  SetFire(3);
  Check(g_armed.load() == 2 && Armed(3), "store.two", "two seats armed");
  OnQueued(0);
  Check(FireState(0) == 1 && !Armed(0) && g_armed.load() == 1, "store.queued", "a queued fire spends the seat's wish (state 1, not armed)");
  OnQueued(9);
  Check(g_armed.load() == 1, "store.queued_bad_seat", "OnQueued(9) changes nothing");
  Check(FireState(9) == -1 && !Armed(9) && !Armed(0xFFFFFFFFu), "store.read_bad_seat", "seat 9 reads -1");
  Clear();
  Check(FireState(0) == -1 && FireState(3) == -1 && g_armed.load() == 0 && !g_skip.load(), "store.clear", "Clear drops every wish");

  // ---- PressedStep: the press is 1 during the game's call and the game's own value after it ----
  const uint32_t chooser = 0x70000000u, ch = 0x40000000u;
  {
    Mem m;
    m.w[chooser + 48] = 0;
    m.w[ch + 1340] = kNoLink;
    uint32_t seen = 0xDEAD;
    const bool q = PressedStep([&](uint32_t a) { return m.Rd(a); }, [&](uint32_t a, uint32_t v) { m.w[a] = v; },
                               [&] { seen = m.Rd(chooser + 48); if (seen != 0) m.w[ch + 1340] = 7; }, chooser, ch);
    Check(seen == 1, "press.set", "the game's step sees the press 1");
    Check(q, "press.queued", "the step that linked the table reports a fire");
    Check(m.Rd(chooser + 48) == 0, "press.restored", "the chooser's own press 0 is back after the call");
  }
  {
    Mem m;
    m.w[chooser + 48] = 1;  // the game itself pressed: its value stays 1
    m.w[ch + 1340] = kNoLink;
    const bool q = PressedStep([&](uint32_t a) { return m.Rd(a); }, [&](uint32_t a, uint32_t v) { m.w[a] = v; }, [&] {}, chooser, ch);
    Check(!q && m.Rd(chooser + 48) == 1, "press.no_table", "no table: no fire, the game's press 1 back");
  }
  {
    Mem m;
    m.w[chooser + 48] = 0;
    m.w[ch + 1340] = 2;  // linked already (a lean, a fire): the game's gate refuses; nothing new
    const bool q = PressedStep([&](uint32_t a) { return m.Rd(a); }, [&](uint32_t a, uint32_t v) { m.w[a] = v; }, [&] {}, chooser, ch);
    Check(!q && m.Rd(chooser + 48) == 0, "press.linked_before", "a character linked before is no fire");
  }

  // ---- the chooser's own press rule (S110 FIRETABLE review; sub_826056E0 +A0..+EC): the wish presses only where a press of the character's own counts ----
  Check(PressCounts(false, false, false), "press_rule.free", "no act-set act, no sub_8256BD30, no FRA follow-up: a press counts");
  Check(!PressCounts(true, false, false), "press_rule.act_set", "an act in sub_82564518's set clears the press (+C4)");
  Check(!PressCounts(false, true, false), "press_rule.special", "sub_8256BD30 clears the press (+C4)");
  Check(!PressCounts(false, false, true), "press_rule.follow_up", "with an FRA follow-up the press is the follow-up's own [fra+224] (+DC..+EC)");

  // ---- the skip ----
  Clear();
  const uint32_t ctrl = 0x50000000u;
  {
    Mem m;
    m.w[ctrl + 48] = 3;
    int calls = 0;
    Check(!SkipOnce([&](uint32_t a) { return m.Rd(a); }, [&] { ++calls; m.w[ctrl + 48] = 4; }, ctrl) && calls == 0, "skip.unarmed", "no wish: no call");
    Check(!SkipDue(3), "skip.due_unarmed", "SkipDue false with no wish");
    SetSkip();
    Check(SkipDue(3) && !SkipDue(1) && !SkipDue(2) && !SkipDue(4) && !SkipDue(0), "skip.due", "only in the playing state 3");
    m.w[ctrl + 48] = 1;  // clip end (the handler moved on): no skip
    Check(!SkipOnce([&](uint32_t a) { return m.Rd(a); }, [&] { ++calls; }, ctrl) && calls == 0 && g_skip.load(), "skip.not_playing", "state 1: no call, still armed");
    m.w[ctrl + 48] = 3;
    Check(!SkipOnce([&](uint32_t a) { return m.Rd(a); }, [&] { ++calls; }, ctrl) && calls == 1 && g_skip.load(), "skip.refused",
          "the game's skip refused (state stays 3): still armed for the next frame");
    Check(SkipOnce([&](uint32_t a) { return m.Rd(a); }, [&] { ++calls; m.w[ctrl + 48] = 4; }, ctrl) && calls == 2 && !g_skip.load(), "skip.done",
          "state 3 -> 4: the skip went through, the wish is spent");
    m.w[ctrl + 48] = 3;
    Check(!SkipOnce([&](uint32_t a) { return m.Rd(a); }, [&] { ++calls; }, ctrl) && calls == 2, "skip.once", "a spent wish skips nothing more");
  }
  SetSkip();
  Check(!TakeSkip(3) && g_skip.load() && TakeSkip(4) && !g_skip.load(), "skip.take", "TakeSkip only on state 4");
  SetSkip();
  Clear();
  Check(!g_skip.load() && !g_touched.load(), "skip.clear", "Clear drops the skip");

  // ---- fix 2 (run 318): an armed skip that a replay's end finds still armed was never served (that replay was not the IARP one) ----
  Clear();
  Check(!ExpireSkip(), "expire.unarmed", "no wish: a replay end expires nothing");
  SetSkip();
  Check(ExpireSkip() && !g_skip.load(), "expire.armed", "an armed wish at a replay's end: reported once and dropped (it cannot skip a later replay)");
  Check(!ExpireSkip(), "expire.once", "reported once");
  SetSkip();
  Check(TakeSkip(4) && !ExpireSkip(), "expire.after_served", "a served skip (state 4) leaves nothing for the end to report");
  Check(ReplayLocked(1) && ReplayLocked(0x100) && !ReplayLocked(0), "replay_locked",
        "the game's word [P+0x129A8] != 0: IARP never starts (sub_826072B8 +28) and its skip refuses (sub_826071F8 +24)");
  Check(kGameDataPtr == 0x8376AAF0u && kReplayLockOff == 0x129A8u, "numbers.lock", "P = [0x8376AAF0], the word at P+0x129A8 (sub_827D73C8)");
  Clear();

  // ---- the host state names ----
  Check(StateSeat("table_fire_0") == 0 && StateSeat("table_fire_8") == 8, "state.names", "table_fire_0 .. table_fire_8");
  Check(StateSeat("table_fire_9") == -1 && StateSeat("table_fire_") == -1 && StateSeat("table_fire_01") == -1 && StateSeat("finisher_fit_0") == -1,
        "state.not_names", "seat 9, no digit, two digits, another prefix");

  std::printf("table_fire unit: %d checks, %d failed\n", g_n, g_fail);
  return g_fail == 0 ? 0 : 1;
}
