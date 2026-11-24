// The weapon slots: the game-side half (Session 103, lane PW-1). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Gives the pure contributor (weapon_contrib.h) the fault-checked guest word reader (wrestler_hook.h ReadWord) and registers it with the scene writer. Read only: no hook, nothing written to the guest.
#pragma once
#include "weapon_contrib.h"
#include "wrestler_hook.h"   // ReadWord

namespace wwe2k14::scene_weapons_glue {

// Once, at start (wwe2k14_app.h OnPostSetup, with the other class contributors). Idempotent.
inline void Register() { rdr_scene::live::weapons::RegisterWeapons(&wwe2k14::wrestlers::ReadWordNoNote); }   // S110 FEEDREC: a ReadWordFn is a plain pointer (no location rides it)

}  // namespace wwe2k14::scene_weapons_glue
