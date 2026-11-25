// The crowd class of the record -> frame table (Session 103, lane D2; docs\renderer\LIVE_CROWD.md 6): lane D's LiveCrowd, one per FrontEnd, reached through the class table.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include "front_end.h"
#include "live_crowd.h"

namespace rdr_scene::rf::crowd {

// Made by the owner of the FrontEnd (LiveSceneDraw::EnsureFrontEnd) so the crowd's bodies load on a worker thread (the live frame never loads). A FrontEnd with no Attach gets a
// crowd on the applier's first use (offline tools: frame_rig, rsc_draw), its bodies loaded on the worker too when the cache does not load in line. Render thread only.
void Attach(FrontEnd& fe, bool async_bodies);
// S103 SG2: the live drawer sets true (a walked frame's sign rows, flagged kCpfInPlace, are drawn by the stream in place: skipped and counted), the mirror drawer false. Off by default (fview, the rigs). Render thread only.
void SetSkipInPlace(FrontEnd& fe, bool on);
// MUST be called before the FrontEnd is destroyed (the crowd holds the front-end's models and images; it waits for the GPU and releases them). No-op when none.
void Detach(FrontEnd& fe);
// The counts of the last frame the crowd recorded for this front-end (null when it has none): the rig prints them.
const live::LiveCrowdCounts* LastCounts(const FrontEnd& fe);
// S110 ENTRBODIES: every crowd class of `arena` asked now (LiveCrowd::PrefetchArena), for a scene start held behind the loading card. true = none still loading. Render thread only.
bool PrefetchArena(FrontEnd& fe, int arena);

// Test only (tools\scene frame_rig --plant-crowd): a planted fault of LiveCrowdOptions by name (palettes | cards_black | hidden | cards_back | inst_first); "" = none. Set before the crowd is made.
void SetFaultForTest(const char* name);

// S103 B4F: the arena's stage light-map page (UpdatePages' key and pixels) for the crowd of this front-end: every resident crowd model (bodies, card quad, props) gets it, a model made later takes it. With no crowd yet the
// page is kept and given to the crowd when it is made. PageStale = the crowd does not hold `key`.
bool PageStale(const FrontEnd& fe, uint64_t key);
void SetStagePage(FrontEnd& fe, uint64_t key, uint32_t w, uint32_t h, const uint8_t* rgba);

}  // namespace rdr_scene::rf::crowd
