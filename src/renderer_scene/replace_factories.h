// S101 lane 41: how a class plugs its in-place DrawReplaceHook into the two places that run the binder with a scene record (the live render thread, live.cpp,
// and fview). Header only: no new source file in any build list. The arena (lane 40) and the wrestlers (lane 45) each add ONE marked line to
// InstallReplaceFactories() below, plus their include; the switches, the record keeping and the census are already wired (binder.h, live.cpp, fview).
//
//   // S101 lane 40: SetArenaFactory([](const ReplaceFactoryArgs& a) { auto h = std::make_unique<ArenaOurs>(*a.renderer, ...); ... return ReplaceHookHandle{...}; });
//
// The factory builds the hook and says how the record reaches it. The record kept for a frame follows the crowd's pairing (stream frame j with record
// j-1, CROWD_PROMOTION.md); a class whose pairing differs measures it and keeps its own offset inside the hook. No game or SDK includes.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../renderer_adapter/image_content_table.h"  // S101 UI contract
#include "record_io.h"

namespace rdr_scene {

struct ReplaceFactoryArgs {
  rdr::Renderer* renderer = nullptr;
  std::string game_root;   // the player's game files (empty until a live record names one)
  std::string fault;       // fview --replace-fault NAME: the class's own planted-fault name ("" in the live path)
  // S101 lane 40 (review A5): a capture image record (the id a frame's texture_slots carry) -> its width / height; null when the owner cannot tell (live: the binder does not expose its
  // image table yet; the class then counts the checks it could not make).
  std::function<bool(uint32_t record_id, uint32_t& w, uint32_t& h)> image_info;
  bool low_ram = false;    // lane 126: --wwe2k14_low_ram is in force (the UI skips bgEtc)
  bool live = false;       // true: the live render thread (verify cheaply); false: fview (verify every frame)
  // S101 UI contract: a stream image record id -> its size / format / mips / content hash (image_content_table.h); empty when the owner has no feed (the UI then
  // joins by declared key only, and counts the content joins it could not make).
  std::function<bool(uint32_t record_id, rdr_adapter::ImageContent& out)> image_content;
};

struct ReplaceHookHandle {
  std::unique_ptr<rdr_adapter::DrawReplaceHook> hook;
  // The record for the frame about to be bound (null: none, the claimed draws stay the stream's). May be empty for a hook that needs none.
  std::function<void(std::shared_ptr<const SceneTables>)> set_record;
  std::function<void(int)> announce_arena;                 // S101 lane 271: the match card named this arena (live: before any record does); may be empty
  std::function<void(const std::string&)> set_game_root;   // live: the game folder may only be known later; may be empty
  std::function<std::string()> report;                     // fview prints it after the census line; may be empty
  bool wants_content = false;                              // S101 lane 66: the hook reads args.image_content: the owner feeds the ImageContentTable (and hashes) only when some hook asks
  std::string error;                                       // non-empty: the factory refused (the switch stays off; the message is logged / printed)
};

using ReplaceFactory = ReplaceHookHandle (*)(const ReplaceFactoryArgs&);
inline ReplaceFactory g_arena_factory = nullptr;
inline ReplaceFactory g_wrestlers_factory = nullptr;
inline void SetArenaFactory(ReplaceFactory f) { g_arena_factory = f; }
inline void SetWrestlersFactory(ReplaceFactory f) { g_wrestlers_factory = f; }
inline ReplaceFactory g_caw_factory = nullptr;   // lane 153 (N3)
inline void SetCawFactory(ReplaceFactory f) { g_caw_factory = f; }
inline ReplaceFactory g_ring_factory = nullptr;                                 // S101 ring contract (RING_PROMOTION_MAP.md 4)
inline void SetRingFactory(ReplaceFactory f) { g_ring_factory = f; }
inline ReplaceFactory g_ui_factory = nullptr;                                   // S101 UI contract (UI_PROMOTION_MAP.md 4)
inline void SetUiFactory(ReplaceFactory f) { g_ui_factory = f; }
inline ReplaceFactory g_ui_text_factory = nullptr;                              // S101 lane 89 (UT): the game's text drawn by us
inline void SetUiTextFactory(ReplaceFactory f) { g_ui_text_factory = f; }
inline ReplaceFactory g_flash_factory = nullptr;                                // S101 flash contract
inline void SetFlashFactory(ReplaceFactory f) { g_flash_factory = f; }
inline ReplaceFactory g_props_factory = nullptr;                                // S101 lane 84
inline void SetPropsFactory(ReplaceFactory f) { g_props_factory = f; }
inline ReplaceFactory g_screens_factory = nullptr;                              // S101 lane 115
inline void SetScreensFactory(ReplaceFactory f) { g_screens_factory = f; }
inline ReplaceFactory g_shadow_casters_factory = nullptr;                       // S101 lane 99
inline void SetShadowCastersFactory(ReplaceFactory f) { g_shadow_casters_factory = f; }
inline ReplaceFactory g_movie_factory = nullptr;                                // S101 lane 131: the movie planes
inline void SetMovieFactory(ReplaceFactory f) { g_movie_factory = f; }
inline ReplaceFactory g_crowd_props_factory = nullptr;                                // S101 lane 133
inline void SetCrowdPropsFactory(ReplaceFactory f) { g_crowd_props_factory = f; }
inline ReplaceFactory g_rim_factory = nullptr;                                  // lane 124 (rim_map, fate 15)
inline void SetRimFactory(ReplaceFactory f) { g_rim_factory = f; }
inline ReplaceFactory g_circles_factory = nullptr;                              // lane 136 (SH3 slice 1)
inline void SetCirclesFactory(ReplaceFactory f) { g_circles_factory = f; }
inline ReplaceFactory g_plane_shadow_factory = nullptr;                         // lane 139 (SH3 slice 2)
inline void SetPlaneShadowFactory(ReplaceFactory f) { g_plane_shadow_factory = f; }
inline ReplaceFactory g_small_passes_factory = nullptr;                         // lane 135 (small_passes, fate 18)
inline void SetSmallPassesFactory(ReplaceFactory f) { g_small_passes_factory = f; }
inline ReplaceFactory g_rim_chain_factory = nullptr;                         // lane 151 (rim_chain, fate 21)
inline void SetRimChainFactory(ReplaceFactory f) { g_rim_chain_factory = f; }

// S101 ring contract: the classes by name, in the order their hooks are added to a binder (a later class sees a draw only after every earlier one passed it on as
// NotOurs). One table so fview never grows another ternary per class.
struct ReplaceClass {
  const char* name;
  ReplaceFactory* factory;
  bool* enabled;
};
inline const ReplaceClass kReplaceClasses[] = {
    {"arena", &g_arena_factory, &rdr_adapter::g_arena_ours},
    {"wrestlers", &g_wrestlers_factory, &rdr_adapter::g_wrestlers_ours},
    {"ring", &g_ring_factory, &rdr_adapter::g_ring_ours},
    {"flash", &g_flash_factory, &rdr_adapter::g_flash_ours},
    {"screens", &g_screens_factory, &rdr_adapter::g_screens_ours},   // S101 lane 115: another pass site (0x82C0F4BC), no overlap with the model-part classes
    {"props", &g_props_factory, &rdr_adapter::g_props_ours},   // S101 lane 84 (after the ring: the ring passes these objects on as NotOurs)
    {"caw", &g_caw_factory, &rdr_adapter::g_caw_ours},   // lane 153 (N3): AFTER the wrestlers, the ring and the props (review B1): everything that is theirs is taken first; caw answers NotOurs for any part it never byte-confirmed
    {"crowd_props", &g_crowd_props_factory, &rdr_adapter::g_crowd_props_ours},   // S101 lane 133: the crowd's sign / foam-hand draws (call sites of the crowd loops: no overlap with any model-part class; the order is free. live.cpp adds it before the props, this table lists it after: both are disjoint sites)
    {"shadow_casters", &g_shadow_casters_factory, &rdr_adapter::g_shadow_casters_ours},   // S101 lane 99 (after the props: it reads the wrestlers' and the ring's joins; its sites are the shadow passes, no colour hook claims them)
    {"circles", &g_circles_factory, &rdr_adapter::g_circles_ours},   // lane 136 (its site is the circle call; no colour hook claims it)
    {"movie", &g_movie_factory, &rdr_adapter::g_movie_ours},   // S101 lane 131: its own call site (0x82BC7E50, the movie quad); before "ui", which would count the quad as a movie and keep it
    {"plane_shadow", &g_plane_shadow_factory, &rdr_adapter::g_plane_shadow_ours},   // lane 139 (after the shadow casters and the circles: it takes the plane draws they pass on)
    {"rim_map", &g_rim_factory, &rdr_adapter::g_rim_ours},   // lane 124 (after the shadow casters: both read the wrestlers' joins; its site is the rim light map's, no colour hook claims it)
    {"small_passes", &g_small_passes_factory, &rdr_adapter::g_small_passes_ours},   // lane 135: call sites of its own (no model-part pass site), no overlap with the other classes
    {"rim_chain", &g_rim_chain_factory, &rdr_adapter::g_rim_chain_ours},   // lane 151: call sites of its own (the rim light map's reductions and blur), no overlap with the other classes
    {"ui_text", &g_ui_text_factory, &rdr_adapter::g_ui_text_ours},   // S101 lane 89: before "ui": a glyph draw ours lays out is not offered to the texture hook
    {"ui", &g_ui_factory, &rdr_adapter::g_ui_ours},
};
inline const ReplaceClass* FindReplaceClass(const std::string& name) {
  for (const ReplaceClass& c : kReplaceClasses)
    if (name == c.name) return &c;
  return nullptr;
}

ReplaceHookHandle MakeCawHook(const ReplaceFactoryArgs& args);   // lane 153: defined in caw_ours.cpp
ReplaceHookHandle MakeWrestlersHook(const ReplaceFactoryArgs& args);   // S101 lane 45: defined in wrestler_ours.cpp (a declaration, not an include: that header includes this one)
}  // namespace rdr_scene
#include "arena_ours_factory.h"  // S101 lane 40: MakeArenaOurs (needs the types above)
#include "ui_ours_factory.h"  // S101 lane 66: MakeUiOurs
#include "ui_text_ours_factory.h"  // S101 lane 89: MakeUiTextOurs
#include "flash_ours_factory.h"  // S101 lane 68: MakeFlashOurs
#include "ring_ours_factory.h"   // S101 lane 70: MakeRingOurs
#include "props_ours_factory.h"  // S101 lane 84: MakePropsOurs
#include "screens_ours_factory.h"  // S101 lane 115: MakeScreensOurs
#include "shadow_casters_factory.h"  // S101 lane 99: MakeShadowCastersOurs
#include "movie_ours_factory.h"  // S101 lane 131: MakeMovieOurs
#include "crowd_props_factory.h"  // S101 lane 133: MakeCrowdPropsOurs
#include "rim_map_factory.h"  // lane 124: MakeRimMapOurs
#include "circle_shadows_factory.h"  // lane 136: MakeCirclesOurs
#include "plane_shadow_factory.h"  // lane 139: MakePlaneShadowOurs
#include "small_passes_factory.h"  // lane 135: MakeSmallPassesOurs
#include "rim_chain_factory.h"  // lane 151: MakeRimChainOurs
namespace rdr_scene {

// Called once at startup by the live sink and by fview, before a hook is made.
inline void InstallReplaceFactories() {
  SetArenaFactory(&MakeArenaOurs);  // S101 lane 40: the arena's DrawReplaceHook (arena_ours_factory.h)
  // (each class edits only its own marked line; this line stays so the two edits never touch neighbouring lines)
  SetCawFactory(&MakeCawHook);  // lane 153 (N3): the editor wrestler's hook (src/renderer_scene/caw_ours.h)
  SetWrestlersFactory(&MakeWrestlersHook);  // S101 lane 45: the wrestlers' DrawReplaceHook (src\renderer_scene\wrestler_ours.h)
  SetRingFactory(&MakeRingOurs);  // S101 lane 70: the ring's DrawReplaceHook (ring_ours_factory.h)
  // (this line stays: it keeps the ring's and the UI's edits off neighbouring lines)
  SetUiTextFactory(&MakeUiTextOurs);  // S101 lane 89 (UT): the text hook (ui_text_ours_factory.h)
  SetUiFactory(&MakeUiOurs);  // S101 lane 66: the UI textures' DrawReplaceHook (ui_ours_factory.h)
  // (this line stays: it keeps the UI's and the flashes' edits off neighbouring lines)
  SetFlashFactory(&MakeFlashOurs);  // S101 lane 68: the crowd flashes' DrawReplaceHook (flash_ours_factory.h)
  SetScreensFactory(&MakeScreensOurs);  // S101 lane 115: the stage screens' DrawReplaceHook (screens_ours_factory.h)
  SetPropsFactory(&MakePropsOurs);  // S101 lane 84: the props of other pacs' DrawReplaceHook (props_ours_factory.h)
  SetMovieFactory(&MakeMovieOurs);  // S101 lane 131: the movie planes' DrawReplaceHook (movie_ours_factory.h)
  SetShadowCastersFactory(&MakeShadowCastersOurs);  // S101 lane 99: the shadow passes' casters (shadow_casters_factory.h)
  SetCrowdPropsFactory(&MakeCrowdPropsOurs);  // S101 lane 133: the crowd props' DrawReplaceHook (crowd_props_factory.h)
  SetRimFactory(&MakeRimMapOurs);  // lane 124: the rim light map's casters (rim_map_factory.h)
  SetCirclesFactory(&MakeCirclesOurs);  // lane 136: the circle blob shadows (circle_shadows_factory.h)
  SetPlaneShadowFactory(&MakePlaneShadowOurs);  // lane 139: the plane shadow chain (plane_shadow_factory.h)
  SetSmallPassesFactory(&MakeSmallPassesOurs);  // lane 135: the sweat map and skin pre-light passes (small_passes_factory.h)
  SetRimChainFactory(&MakeRimChainOurs);  // lane 151: the rim light map's reductions and blur (rim_chain_factory.h)
}

}  // namespace rdr_scene
