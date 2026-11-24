// S108 RDS (docs\research\s108\RDS_plan.md, the class closure): what our side does when the game destroys a model. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// No SDK include: the hook (model_release_hook.h, REX_HOOK_RAW on sub_82BD3B40) calls this, and tools\scene\inputs_unit.cpp drives the SAME function (rds.release_forgets_address and its planted twin).
//
// Every container of ours that keeps a model's address for a later frame forgets it (the sweep in RDS_plan.md): the wrestlers' kept / pending / Known records (the runtime rows, the palette targets, the
// unlinked own-skeleton pieces that stayed targets through SubmitLatchConsumer), the learned light entries, and (RDS review) the inputs' copy of the last latch's targets that the OFF-latch fallback reads
// (InputsContributor::snap_targets_). Each container is behind its own mutex on every access (WrestlerContributor::mutex_, lightidx::Table::mu_, InputsContributor::snap_mu_) and this function holds one
// at a time, so the erase is safe from the game thread that destroys the model while our latch runs (no lock order to keep). The guest-model planner needs nothing: it reads only the model the game hands
// its hook in that call, and its layout pass reads the game's memory again on every call.
#pragma once
#include <cstdint>

#include "change_colour.h"
#include "inputs_contrib.h"
#include "light_index_core.h"
#include "prop_colour.h"
#include "wrestler_contrib.h"

namespace wwe2k14::model_release {
// kPlantKeepWrestlers / kPlantKeepLights / kPlantKeepSnap: the unit's twins (one erase left out: the address is still named after the release).
template <bool kPlantKeepWrestlers = false, bool kPlantKeepLights = false, bool kPlantKeepSnap = false>
inline void OnModelDestroyed(uint32_t model) {
  if (!model) return;
  if (!kPlantKeepWrestlers) rdr_scene::live::Wrestlers().ForgetModel(model);
  if (!kPlantKeepLights) rdr_scene::live::lightidx::The().Forget(model);
  if (!kPlantKeepSnap) rdr_scene::live::inputs::Inputs().ForgetModel(model);
  rdr_scene::live::propcolour::TheStore().ForgetModel(model);    // S108 THB: an entrance prop's colours (the editor's / the saved set's) go with its model
  rdr_scene::live::change_colour::Bodies().ForgetModel(model);   // S108 THR + RDS (merge): the Threads editor colours kept for this body go with it (values only; a new model at the same address starts clean)
}
}  // namespace wwe2k14::model_release
