// wwe2k14 - the situation director's `fire SLOT` and `skip_replay` verbs (S110 FIRETABLE, docs/research/s110/FIRETABLE.md).
//
//   fire SLOT     arm the seat's wish: the next time the game's own table-fire step (sub_825FB230 in the move chooser) finds a standing table the
//                 seat may set on fire (its momentum meter full, a set-up table in reach and faced, rule byte G+56 == 1 or G+39 != 0), the step
//                 runs as if the seat had pressed, and the game queues its own Table_OnFire state. Nothing in the game is written here; the
//                 hook is src/table_fire.h, the store src/table_fire_core.h. One fire per call; the wish goes at the end of the match.
//   skip_replay   arm the skip: at the IN-MATCH instant replay's next playing frame (the 'IARP' controller sub_82608758, state 3) the game's
//                 own skip sub_826071F8 runs (the call a pad press makes). It needs the in-game sequence only (no match gate). FIX 2 (run
//                 318): the replay after a finish is not IARP's (the running-match flow clears IARP's request at the end, sub_827C2AB8 +40):
//                 a wish armed for it is reported NOT served at that replay's end (src\replay_fx_skip.h). Refused at once while the game's own
//                 replay lock [[0x8376AAF0]+0x129A8] is set (IARP never starts then, and its skip refuses).
//
// NOT a stand-alone header: situation_director_core.h includes it INSIDE namespace wwe2k14::situation, after SlotChar / Hex / Gate are declared
// (and src/table_fire_core.h at its top, outside the namespace), so it opens no namespace of its own. Pure: no game include.
#pragma once

template <class E>
std::string FireTable(E& e, int slot) {
  const std::string tag = "fire " + std::to_string(slot);
  uint32_t ch = 0;
  const std::string r = SlotChar(e, slot, ch);
  if (!r.empty()) {
    e.Log("director: " + tag + ": refused (" + r + ") clock " + e.Clock());
    return r;
  }
  const std::string a = ::wwe2k14::table_fire::SetFire(slot);
  e.Log("director: " + tag + ": " + a + " (char " + Hex(ch) + ") clock " + e.Clock());
  return "ok: " + tag + " (" + a + ")";
}

template <class E>
std::string SkipReplay(E& e) {
  const Gate g = e.GetGate();
  if (g.seq != kSeqInGame) {
    const std::string b = "busy: no match is running (the program is in sequence " + std::to_string(g.seq) + ", not " + std::to_string(kSeqInGame) + ")";
    e.Log("director: skip_replay: " + b + " clock " + e.Clock());
    return b;
  }
  const uint32_t p = e.U32(::wwe2k14::table_fire::kGameDataPtr);
  if (e.Ok(p) && ::wwe2k14::table_fire::ReplayLocked(e.U32(p + ::wwe2k14::table_fire::kReplayLockOff))) {
    const std::string r = "refused: the game's replay lock [P+0x129A8] is set: the in-match instant replay never starts and its skip "
                          "sub_826071F8 refuses (sub_826072B8 +28, sub_826071F8 +24); nothing armed";
    e.Log("director: skip_replay: " + r + " clock " + e.Clock());
    return r;
  }
  const std::string a = ::wwe2k14::table_fire::SetSkip();
  e.Log("director: skip_replay: " + a + " clock " + e.Clock());
  return "ok: skip_replay (" + a + ")";
}
