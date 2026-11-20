// S105 E: the sequence-id -> name table, moved out of session_log.h (verbatim) so the perf report can name a load by its sequence without including the session log.
#pragma once
#include <cstdint>

namespace wwe2k14::session_log {

// Session 61: every id with the name the game registers it under (the second argument of the sequence base
// constructor sub_82C3A298 at each of its 27 call sites; docs\systems\menu_ui.md), our own description first where
// we have one.
inline const char* SequenceName(uint32_t seq) {
  switch (seq) {
    case 3: return "startup / title: CStartupCtrlSequence";
    case 4: return "OverlaySequence";
    case 5: return "CInGameStoreSequence";
    case 7: return "CExhibitionEndSequence";
    case 9: return "in-game: CGameMainSequence";
    case 10: return "CCutScenePreviewSequence";
    case 11: return "story / cutscenes: CCutSceneStartupSequence";
    case 14: return "SequenceSelectSequence";
    case 20: return "MovesMenuSequence";
    case 21: return "COnlineMenuSequence";
    case 22: return "COMS_ResultMenuSequence";
    case 23: return "CUCC_ResultMenuSequence";
    case 25: return "Create an Entrance: CreateEntranceSequence";
    case 26: return "Create a Finisher: CCafMenuSequence";
    case 27: return "CCahSequenceBase";
    case 28: return "Create an Arena: Create a Ring Menu Sequence";
    case 29: return "Create a Title: CAB MENU";
    case 30: return "CTitleMatchResultSequence / CKotRResultSequence";
    case 31: return "30 Years of WrestleMania: CRTWBeginSequence";
    case 35: return "CWM30MeunSequence";
    case 36: return "CStreakMeunSequence";
    case 75: return "main menu / match setup: CSmartMenuSequence";
    case 76: return "Create a Superstar: Create a Superstar Ex Menu Sequence";
    case 77: return "SuperstarThreads Menu Sequence";
    case 78: return "Create a Logo: PaintTool Sequence";
    case 82: return "CWWEUniverseNextMatchSequence";
    default: return "?";
  }
}

}  // namespace wwe2k14::session_log
