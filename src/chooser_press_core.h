// wwe2k14 - the move chooser's own press rule, shared by the route wishes that press for a CPU seat (S110 FIRETABLE fix 2: one rule, two users:
// src\table_fire_core.h at the table-fire step sub_825FB230, src\omg_wish_core.h at the ring-break test sub_825FCEB8). Pure: no SDK, no game.
//
// The chooser sub_826056E0 sets its press word [ctx+48] before any step reads it (+A0..+EC): 1 from char+2176; cleared (+C4) when the
// character's act is in sub_82564518's set (53, 56, 58, 220, 234, 932, 1403, 7030, 15051, 23121) or sub_8256BD30 holds; and while the
// character has an 'FRA' follow-up (sub_825FCBF0, +CC) the press is the follow-up's own [fra+224] (+DC..+EC), never the pad's. A wish presses
// only where a press of the character's own would have counted there.
#pragma once

namespace wwe2k14::chooser_press {

inline bool PressCounts(bool act_clears, bool special_clears, bool has_follow_up) { return !act_clears && !special_clears && !has_follow_up; }

}  // namespace wwe2k14::chooser_press
