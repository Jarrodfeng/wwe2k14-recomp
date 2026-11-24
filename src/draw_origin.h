// wwe2k14 - Session 101 (renderer arc A1a, docs\renderer\NAMES.md "Draw origin"): every draw record the game writes carries the name of
// the code that submitted it, across the engine's record / replay boundary, so the renderer never has to guess what a draw is.
//
// Recorders (each writes a 32-byte record header and copies it into the thread's command list; none writes the word at +12, and the
// replay handlers read only +16..+28: listings of 0x82BB10C0, 0x82C1B350, 0x82BCBDD8, 0x82BB41E8 read S101; docs\research\s101\A0-1.md):
//   op 15 sub_82BB10C0 indexed draw   (hooked in scene_crowd.h; that hook opens a RecordTag first)
//   op 19 sub_82C1B350 inline vertices (hooked here)
//   op 68 sub_82BCBDD8 render-thread callback: post effects, edit-mode composites (hooked here)
// For the copy only, +12 of the caller's record holds a tag (origin_intern.h): the caller's own word is put back right after, so the only
// memory that changes is the command list's copy, which nothing else reads.
// The model-part draw sub_82BDED40 (r3 model object, r4 part index; db model_part_draw) runs the op-15 recorder inside it: its entry hook
// keeps {object, part, the pass that called it} for the recorder hook on the same thread.
// Replay: hooks on the three record handlers (op 15 sub_82BB41E8, op 19 sub_82C1B2F8, op 68 sub_82BB4540; r3 = the record) set
// t_replay_tag for the handler's run; the reader's draw hooks run inside it on the same thread (S101 review: confirmed).
// Tags are written and read only while the reader records or draws live (origin_intern.h g_on), so players with the old renderer pay
// nothing. --wwe2k14_draw_origin (default true): false = no tags at all (every draw unnamed).
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <cstdint>

#include "cost_counters.h"  // S101 lane 83: the passive cost counters (--wwe2k14_cost_counters)
#include "renderer_scene/rows_caster.h"   // S106 G4-2
#include "renderer_scene/rows_quad2d.h"
#include "scene_live/span_timer.h"  // lane 122: records per span
#include <cstring>
#include "guest_read.h"  // S101 lane18: GuestWordReadableFast (ReadUnitsBe)
#include "feed_seen.h"   // S110 FEEDREC
#include "renderer_adapter/origin_intern.h"
#include "renderer_adapter/post_origin.h"  // S101 lane PN: ApplyPostSite (the post effect that queued an op-68 quad)
#include "scene_live/plane_glue.h"  // lane 139: the plane shadow chain's snapshot at the mask part draw (read only)
#include "scene_live/screen_planes_hook.h"  // S103 M: the screens' pass site logs its model's handle table once (diagnosis for the key -> channel binding)
#include "post_origin_hook.h"  // S101 lane PN: hooks on the post queue functions 82C1F420 / 4C0 / 610
#include "scene_live/caster_glue.h"  // lane 202: the caster passes' per-entry record (Caster D3 E0-E2, read only, default off); hooks below
#include "scene_live/sphere_glue.h"  // lane 242: the wrestlers' sphere occluders (H3, read only, rides the caster record's switches); the hook on sub_826DF8E0 below
#include "keep_stream.h"  // S103 P9: WWE2K14_KEEP_STREAM
#include "scene_live/draw_camera_glue.h"   // S103 WC R5: the draw thread camera
#include "scene_live/weapon_rows_glue.h"  // S103 PW-2 (review fix 1): RawEntry, the weapons' per-call rows taken in the raw hook below, BEFORE its Ours test (both builds)
#if WWE2K14_STREAM()  // S103 P9 (B3): the D3 skip hooks write markers for the stream's binder
#include "scene_live/d3_arena_hook.h"  // lane 138: D3 skip the object (CallScope in the sub_82C04DF0 hook below, NotePart in the sub_82BDED40 hook)
#include "scene_live/d3_caster_hook.h"  // lane 228: D3 skip the pass (CasterScope in the three caster pass hooks below, NoteCasterPart in the sub_82BDED40 hook)
#include "scene_live/d3_ring_hook.h"  // lane 246: D3 skip the call for the RING class (RingScope in the sub_82C04DF0 hook below, before the arena's CallScope)
#include "scene_live/d3_wrestler_hook.h"  // lane 245: D3 skip the wrestler's colour call (WrestlerScope in the sub_82C04DF0 hook below, NoteWrestlerPart in the sub_82BDED40 hook)
#endif
#include "scene_live/guest_model_contrib.h"  // S103 CW-2c: OnModelEntry, called from the raw hook on sub_82C04DF0 below
#include "scene_live/scene_skip_nodes.h"  // S103 H: the scene skips (--wwe2k14_rdr_scene=2): the cvar, the frame clock and the hooks the scene tasks did not have
#include "scene_live/quads2d_tap.h"  // S103 I1: the 2D layer's record (--wwe2k14_rdr_2d_record, default off; read only): the scope on the T_Rect / movie leaves, the vertices at the op-19 recorder below
#include "scene_live/own_node_hook.h"  // S102 P4P5: the node switch on the shadow module's (sub_827E9740) and the rim map's (sub_827EA7B8) draw nodes; compiled and reviewed, not run
#include "scene_live/walk_single.h"  // S110 WALKSINGLE: the game's single-worker walk (the setter sub_82BB4128 stores a null range table for the manager) + the caption detectors

REXCVAR_DEFINE_BOOL(wwe2k14_draw_origin, true, "wwe2k14",
                    "S101: tag every draw record with the code that submitted it (renderer names, docs\\renderer\\NAMES.md); false = all draws unnamed");

namespace wwe2k14::draw_origin {

inline uint32_t Ld32(const uint8_t* base, uint32_t a WWE_FEED_LOC_PARAM) {
  WWE_FEED_NOTE(a, 4);
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

struct PartScope {
  uint32_t object = 0, part = 0, pass_site = 0;
  uint32_t widget = 0;  // S101 lane18: Widgets() id of the UI element + screen (2D scopes only)
  bool ui = false;      // S101 lane18: a 2D draw scope (a nested 2D scope keeps the outer one)
  uint32_t text = 0, run = 0;  // S101 U1: inside a text draw (a glyph quad) / the Runs() id of its whole string
};
inline thread_local PartScope t_part;

// Opened at the top of a recorder hook: tags the record for the copy, restores the caller's word when the hook returns.
class RecordTag {
 public:
  RecordTag(PPCContext& ctx, uint8_t* base, uint32_t op) : base_(base) {
    if (!rdr_adapter::origin::g_on.load(std::memory_order_relaxed) || !REXCVAR_GET(wwe2k14_draw_origin)) return;
    const uint32_t rec = ctx.r3.u32;
    if (rec < 0x10000u || rec >= 0xC0000000u) return;  // never a physical view: the scratch records live on stacks / heaps
    rdr_adapter::origin::Key k;
    k.op = op;
    k.call_site = uint32_t(ctx.lr) - 4;
    if (t_part.pass_site) k.pass_site = t_part.pass_site, k.object = t_part.object, k.part = t_part.part, k.widget = t_part.widget, k.text = t_part.text, k.run = t_part.run;  // S101 lane18: widget; U1: glyph + run
    rdr_adapter::origin::ApplyPostSite(k, op, uint32_t(ctx.lr) - 4);  // S101 lane PN: AFTER the t_part copy (it must not overwrite part): op 68 inside a post scope -> call_site = the effect, part = the generic queue site
    const uint32_t tag = rdr_adapter::origin::Global().Intern(k);
    at_ = rec + 12;
    saved_ = Ld32(base, at_);
    St32(base, at_, tag);
  }
  ~RecordTag() {
    if (at_) St32(base_, at_, saved_);
  }
  RecordTag(const RecordTag&) = delete;
  RecordTag& operator=(const RecordTag&) = delete;

 private:
  uint8_t* base_ = nullptr;
  uint32_t at_ = 0, saved_ = 0;
};

// Replay: the three record handlers each take r3 = the record (listings, S101 review). The tag is set for the handler's run and cleared
// after, so a draw made outside a tagged record's handler is never given another record's name, and an op-68 callback's several draws all
// share its tag. (Not the dispatcher sub_82BB53D0: ops 81 / 82 switch lists in place and dispatch the next record inline, review 2.)
class ReplayTag {
 public:
  ReplayTag(const PPCContext& ctx, const uint8_t* base, uint32_t op) {
    if (!rdr_adapter::origin::g_on.load(std::memory_order_relaxed) || !REXCVAR_GET(wwe2k14_draw_origin)) return;
    const uint32_t rec = ctx.r3.u32;
    if (rec < 0x10000u) return;
    rdr_adapter::origin::t_replay_tag = Ld32(base, rec + 12);
    rdr_adapter::origin::t_replay_op = op;
  }
  ~ReplayTag() {
    rdr_adapter::origin::t_replay_tag = 0;
    rdr_adapter::origin::t_replay_op = 0;
  }
  ReplayTag(const ReplayTag&) = delete;
  ReplayTag& operator=(const ReplayTag&) = delete;
};

// ---- S101 lane18: 2D draws (docs\renderer\NAMES.md "2D draws") -----------------------------------------------------------------------
// A UI text element's draw method (vtable 0x82065FF0 slot 15, sub_82B553F8: r3 = the text element; listing S101 lane18) opens
// t_element for its run: every 2D draw recorded inside it (on this thread) belongs to that element. [U: that the glyph draws are recorded
// inside this call, not queued - the live proof is the [rdr][names] "2D draws, with widget" line.]
struct ElementScope {
  uint32_t element = 0, node = 0, text_id = 0;
};
inline thread_local ElementScope t_element;

inline bool GuestPtr(uint32_t a) { return a >= 0x10000u && a < 0xC0000000u; }

// The screen: sequence id [[0x839400F8]+0]+48 (menu_ui.md, game_atlas.md "Layers"); inside sequence 75 also the SmartMenu's menu id
// [[0x8392945C]+364]. Same chains event_bus.h and guest_peek.h read.
inline void ReadScreen(const uint8_t* base, uint32_t& seq, uint32_t& menu) {
  seq = menu = 0xFFFFFFFFu;
  const uint32_t a = Ld32(base, 0x839400F8u);
  if (!GuestPtr(a)) return;
  const uint32_t b = Ld32(base, a);
  if (!GuestPtr(b)) return;
  seq = Ld32(base, b + 48);
  if (seq == 75) {
    const uint32_t sm = Ld32(base, 0x8392945Cu);
    if (GuestPtr(sm)) menu = Ld32(base, sm + 364);
  }
}

// The widget id for a 2D draw: the element being drawn (t_element, opened by ElementDraw), else the screen alone. (Review 2: no lookup of the
// scope object in the set-text registry - NAMES.md marks "a sprite is the text element itself" [U] - and Intern answers repeats from a
// thread-local cache with no lock.) 0 only if the table cannot hold even the screen entry.
// The label: the UTF-16BE string the text object's draw (sub_82BD06B8, r4) is drawing, held in t_label for that call only (see the
// LabelScope / FilterLabel comment in origin_intern.h; the old +524 read was removed after the review of a83e727d).

inline uint32_t WidgetFor(const uint8_t* base, uint32_t object, bool text_object) {
  uint32_t seq, menu;
  ReadScreen(base, seq, menu);
  const rdr_adapter::origin::OpenElement open{t_element.element, t_element.node, t_element.text_id};
  return rdr_adapter::origin::WidgetIdFor(open, seq, menu,
                                          rdr_adapter::origin::LabelFor(rdr_adapter::origin::g_labels.load(std::memory_order_relaxed), text_object));
}

// The set-text hook's note (mouse_menu.h calls this after the game's set-text ran): r3 element, r4 text id (listing of sub_82B53068 S101
// lane18: r4 goes straight to GetString, the result is stored at element+1136). The parent node word +16 is the one mouse_menu.h reads. The
// string word and node are kept so a later lookup can tell a recycled address (review 3).
inline void NoteText(const uint8_t* base, uint32_t element, uint32_t text_id) {
  if (!rdr_adapter::origin::g_on.load(std::memory_order_relaxed) || !GuestPtr(element)) return;
  rdr_adapter::origin::Widgets().NoteText(element, Ld32(base, element), Ld32(base, element + 16), Ld32(base, element + 1136), text_id);
}

class ElementDraw {
 public:
  ElementDraw(const PPCContext& ctx, const uint8_t* base) : saved_(t_element) {
    if (!rdr_adapter::origin::g_on.load(std::memory_order_relaxed) || !REXCVAR_GET(wwe2k14_draw_origin)) return;
    const uint32_t e = ctx.r3.u32;
    if (!GuestPtr(e)) return;
    rdr_adapter::origin::WidgetTable::Element known;
    const uint32_t node = Ld32(base, e + 16);
    const bool found = rdr_adapter::origin::Widgets().FindElement(e, Ld32(base, e), node, Ld32(base, e + 1136), known);
    rdr_adapter::origin::g_slot15_hits.fetch_add(1, std::memory_order_relaxed);
    if (found) rdr_adapter::origin::g_slot15_registered.fetch_add(1, std::memory_order_relaxed);
    t_element = {e, node, found ? known.text_id : 0};  // text 0 = set before recording began, or the address was recycled
  }
  ~ElementDraw() { t_element = saved_; }
  ElementDraw(const ElementDraw&) = delete;
  ElementDraw& operator=(const ElementDraw&) = delete;

 private:
  ElementScope saved_;
};

// A sprite-family draw function (r3 = the sprite object, the texture handle's key word at r3 + tex_off, each offset read from the function's
// own listing): the op-19 draws it records carry {object = the sprite, part = the handle key, pass_site = its caller} and the widget.
// A 2D scope inside another keeps the outer one (the gradient draw runs inside the variants draw).
class UiScope {
 public:
  UiScope(const PPCContext& ctx, const uint8_t* base, uint32_t tex_off, bool text_object = false) : saved_(t_part) {
    if (!rdr_adapter::origin::g_on.load(std::memory_order_relaxed) || t_part.ui) return;
    const uint32_t obj = ctx.r3.u32;
    if (!GuestPtr(obj)) return;
    const rdr_adapter::origin::RunTake rt = rdr_adapter::origin::RunFor(text_object);  // S101 U1: a glyph quad inside a text draw is class text
    t_part = {obj, tex_off ? Ld32(base, obj + tex_off) : 0u, uint32_t(ctx.lr) - 4, REXCVAR_GET(wwe2k14_draw_origin) ? WidgetFor(base, obj, text_object) : 0u, true, rt.text, rt.run};
  }
  ~UiScope() { t_part = saved_; }
  UiScope(const UiScope&) = delete;
  UiScope& operator=(const UiScope&) = delete;

 private:
  PartScope saved_;
};

}  // namespace wwe2k14::draw_origin

// S101 lane18: one hook per 2D draw function. tex_off 0 = the function draws no texture (a filled rectangle).
#define S101_UI_SCOPE(fn, tex_off)                              \
  REX_HOOK_RAW(fn) {                                            \
    wwe2k14::draw_origin::UiScope scope(ctx, base, tex_off);    \
    __imp__##fn(ctx, base);                                     \
  }
// The same for the functions a TEXT OBJECT's draw chain calls with r3 = the text object (sub_82BD0380 -> sub_82BC3518 / sub_82BC8180):
// the widget also carries the text the object shows.
#define S101_UI_TEXT_SCOPE(fn, tex_off)                               \
  REX_HOOK_RAW(fn) {                                                  \
    wwe2k14::draw_origin::UiScope scope(ctx, base, tex_off, true);    \
    __imp__##fn(ctx, base);                                           \
  }

// S103 I1 (REMNANT_MAP.md 5.1 T3): the two 2D leaves the first piece of landing 2 records as rows, through the SAME hook as above plus the 2D record's scope (default off: one relaxed load).
#define S103_I1_UI_SCOPE(fn, tex_off, kind)                      \
  REX_HOOK_RAW(fn) {                                            \
    wwe2k14::draw_origin::UiScope scope(ctx, base, tex_off);    \
    wwe2k14::q2d_tap::LeafScope rows(ctx, kind);                \
    __imp__##fn(ctx, base);                                     \
  }

// S103 I3 (REMNANT_MAP.md section 12): the two glyph leaves, through the SAME hook as S101_UI_TEXT_SCOPE plus the 2D record's glyph scope (default off: one relaxed load). One hook per function: these two were already hooked.
#define S103_I3_UI_TEXT_SCOPE(fn, tex_off)                              \
  REX_HOOK_RAW(fn) {                                                  \
    wwe2k14::draw_origin::UiScope scope(ctx, base, tex_off, true);    \
    wwe2k14::q2d_tap::LeafScope rows(ctx, rdr_scene::kQkGlyph);       \
    __imp__##fn(ctx, base);                                           \
  }

// The replay handlers: op 15 indexed draw (calls DrawIndexed 0x82CB19E0), op 19 inline vertices, op 68 render-thread callback.
REX_HOOK_RAW(sub_82BB41E8) {
  wwe2k14::draw_origin::ReplayTag tag(ctx, base, 15);
  __imp__sub_82BB41E8(ctx, base);
}
REX_HOOK_RAW(sub_82C1B2F8) {
  wwe2k14::draw_origin::ReplayTag tag(ctx, base, 19);
  __imp__sub_82C1B2F8(ctx, base);
}
REX_HOOK_RAW(sub_82BB4540) {
  wwe2k14::draw_origin::ReplayTag tag(ctx, base, 68);
  __imp__sub_82BB4540(ctx, base);
}

// The model-part draw: who (object, part) and which pass, for the op-15 recorder it calls.
REX_HOOK_RAW(sub_82BDED40) {
  namespace dor = wwe2k14::draw_origin;
  if (wwe2k14::plane::Active()) wwe2k14::plane::OnPartDraw(ctx);  // lane 139: the plane mask's part draw (LR 0x82BF4D3C); read only
  wwe2k14::scene_screens::OnPartDraw(base, ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr) - 4);  // S103 M: one compare, then (screens site, mode != 0) a once-per-part log
  const dor::PartScope saved = dor::t_part;
  dor::t_part = {ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr) - 4};
  {
#if WWE2K14_STREAM()  // S103 P9 (B3)
    wwe2k14::d3hook::NotePart(uint32_t(ctx.lr) - 4);  // lane 138 (D3): the open sub_82C04DF0 call counts this part (keyed or not); a thread_local flag test when no call is open
    wwe2k14::d3hook::NoteCasterPart(uint32_t(ctx.lr) - 4, ctx.r4.u32);  // lane 228 (D3 casters): the open caster pass call counts this part (its own pass's site: keyed); a thread_local flag test when none is open
    wwe2k14::d3hook::NoteWrestlerPart(uint32_t(ctx.lr) - 4);  // lane 245 (D3 wrestlers): the open wrestler colour call counts this part (colour / mirror site: keyed); a thread_local flag test when none is open
#endif
    wwe2k14::cost::PartSpan cost_span(ctx.r3.u32, uint32_t(ctx.lr) - 4);  // S101 lane 83: one relaxed load when --wwe2k14_cost_counters is off
    __imp__sub_82BDED40(ctx, base);
  }
  dor::t_part = saved;
}

// lane 202 (Caster D3 phase 1, SKIP_GAME_WORK_MAP.md 17.3 E1 / 17.7): the three caster pass entries. r3 = the model, r4 = the pass key, r5 = the palette pointer or 0, r13 = the guest thread.
// The tracker is read only (caster_hook.h records R, the key, the palette and the collapse flag at the entry); the skip is lane 228's CasterScope below (default off). Off = one relaxed load each.
// REX_HOOK_RAW on a function needs no codegen (no config entry): the hooks on sub_82C04DF0 / sub_82BDED40 above work the same way.
// lane 228 (Caster D3 live seam, SKIP_GAME_WORK_MAP.md 17.12): after the tracker's record, CasterScope decides (off = one relaxed load): in the verify arm it writes the marker and the original still runs; otherwise a proven pass
// on cache path 1 is replaced by the marker and this call returns 1 here.
// lane 299 (the plane skip arm, SKIP_GAME_WORK_MAP.md 17.15): a skipped plane call never reaches the part-draw hook above, so CasterScope replays its part draws to the plane tracker (the PlaneDraw entries plane_shadow_ours joins
// the mask draws by). The callbacks are always installed (static init); the plane skip asks g_plane_active before every skip: the tracker is registered (--wwe2k14_scene_plane or --wwe2k14_rdr_plane_shadow_ours) and the scene
// records are on, else the call is recorded in full (counted, first one logged). The replay returns whether the entry was made.
#if WWE2K14_STREAM()  // S103 P9 (B3): installs into the D3 skip seam (d3_*_hook.h), the stream's
inline const bool g_d3_plane_replay_installed = (wwe2k14::d3hook::g_plane_active = []() { return wwe2k14::plane::Active(); },
                                                 wwe2k14::d3hook::g_plane_replay = [](uint32_t model, uint32_t part) {
                                                   if (!wwe2k14::plane::Active()) return false;
                                                   rdr_scene::live::plane::The().tracker.OnPartDraw(model, part, wwe2k14::plane::Frame());
                                                   return true;
                                                 }, true);
#endif
#if WWE2K14_STREAM()  // S103 P9 (B5): the D3 caster skip is the stream's
#define S101_CASTER_ENTRY(fn, pass)                                                                                                                                 \
  REX_HOOK_RAW(fn) {                                                                                                                                                 \
    const uint64_t entry_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);   /* N1: read once, used by the tracker and the decision */   \
    rdr_scene::live::casters::CallResult entry_result{};                                                                                                             \
    if (rdr_scene::live::casters::CastersRegistered())                                                                                                               \
      entry_result = wwe2k14::casters::PassEntryHook(pass, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r13.u32, entry_frame);  \
    wwe2k14::d3hook::CasterScope caster_scope(ctx, base, wwe2k14::draw_origin::t_part, pass, entry_result.recorded, entry_result.ordinal, entry_frame);                           \
    if (caster_scope.skipped()) return;                                                                                                                              \
    __imp__##fn(ctx, base);                                                                                                                                          \
  }
#else
#define S101_CASTER_ENTRY(fn, pass)                                                                                                                                 \
  REX_HOOK_RAW(fn) {                                                                                                                                                 \
    const uint64_t entry_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);   /* N1: read once, used by the tracker and the decision */   \
    rdr_scene::live::casters::CallResult entry_result{}; (void)entry_result;                                                                                                             \
    if (rdr_scene::live::casters::CastersRegistered())                                                                                                               \
      entry_result = wwe2k14::casters::PassEntryHook(pass, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r13.u32, entry_frame);  \
                           \
                                                                                                                              \
    __imp__##fn(ctx, base);                                                                                                                                          \
  }
#endif
S101_CASTER_ENTRY(sub_82C01FE0, rdr_scene::kCpMap)    // T_ShadowMap: the props map, wrestler map A, map B
S101_CASTER_ENTRY(sub_82BF22C8, rdr_scene::kCpPlane)  // T_PlaneShadowMap
S101_CASTER_ENTRY(sub_82BF9878, rdr_scene::kCpRim)    // T_RimLightMap

// lane 242 (H3, SKIP_GAME_WORK_MAP.md 18.8): sub_826DF8E0 = the 'CCSO' sphere occluders' per-frame update (this = r3, vtable slot 0x821EF7D0). Post-hook: the original, then a read of the component's
// output arrays (sphere_hook.h). Read only; one relaxed load when the sphere contributor is not registered (it rides the caster record's switches, never on Android).
REX_HOOK_RAW(sub_826DF8E0) {
  const uint32_t comp = ctx.r3.u32;   // saved before the call (r3 is clobbered afterwards)
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  __imp__sub_826DF8E0(ctx, base);
  wwe2k14::spheres::UpdateHook(comp, frame);
}

// S101 lane 83: the model draw (r3 = the model object): the game thread's whole recording span for a wrestler / arena / ring object. Counters only;
// nothing else. (The mid-asm hook wwe2k14_model_entry sits inside this function at 0x82C04E1C and is unchanged.)
// lane 245 (SKIP_GAME_WORK_MAP.md 18.9, 18.7 precondition 7): the wrestler scope asks the caster tracker whether this hook frame already holds the light camera (a map entry with a palette) and the rim depth.
#if WWE2K14_STREAM()  // S103 P9 (B3): installs into the D3 skip seam (d3_*_hook.h), the stream's
inline const bool g_d3_wcams_installed = (wwe2k14::d3hook::g_wcams = [](uint64_t frame, bool& have_camera, bool& have_rim) {
  if (rdr_scene::live::casters::CastersRegistered()) rdr_scene::live::casters::Casters().tracker.FrameHasWrestlerRecords(frame, have_camera, have_rim);
}, true);
#endif
// lane 245 (reviewer SF2): the sphere tracker's answer for an object the wrestler scope would skip (0 no sphere skeleton, 1 row this frame, 2 known but no row yet).
#if WWE2K14_STREAM()  // S103 P9 (B3): installs into the D3 skip seam (d3_*_hook.h), the stream's
inline const bool g_d3_wspheres_installed = (wwe2k14::d3hook::g_wspheres = [](uint64_t frame, uint32_t object) -> int {
  return rdr_scene::live::spheres::SpheresRegistered() ? rdr_scene::live::spheres::Spheres().tracker.RowState(frame, object) : 0;
}, true);
#endif

// S103 PW (review fix 4): the weapon's OWN submit is the call made from inside the slot-13 dispatcher sub_82C13C88 (vtable slot 13 of the skinned model): this hook marks the model for the nested sub_82C04DF0 call below
// (slot 19 sub_82C13A80 and slot 32 sub_82C13EC8 also pass r4 = 0, so r4 cannot tell them apart). Two thread-local stores; no skip, no change to the call.
REX_EXTERN(__imp__sub_82C13C88);
REX_HOOK_RAW(sub_82C13C88) {
  rdr_scene::live::weaponrows::Slot13Scope slot13(ctx.r3.u32, uint32_t(ctx.lr));   // S103 CGF review 3: + the caller of the slot-13 dispatch (the stage rows' own-family tag)
  __imp__sub_82C13C88(ctx, base);
}

// S103 LDR (COV10 B1): the stage holder's request sub_82576768 (sole caller sub_82576DD8, 11 sites; r4 = the guest path string). A ladder holder has five paths (LADU / LADC / LADM / LADW / the title-keyed /STGL/%04d): the PACH of the
// path it asked for is the only thing that names its models. Entry hook, read only, no skip, no register change; the latch is the tracker's (weapon_rows_core.h NoteStagePath).
REX_EXTERN(__imp__sub_82576768);
REX_HOOK_RAW(sub_82576768) {
  wwe2k14::weaponrows::StageRequestPath(ctx.r4.u32);
  __imp__sub_82576768(ctx, base);
}

// S103 GBR: "the thread's shadow flag A (sub_82BA77A8 returns u32 [0x83929A48 + 4 * thread slot], r3 is ignored: PMW.md) is 0" (the colour pass), handed to the weapon rows' tracker as a callback and asked ONLY for a gm set object / a blood splat call; ctx is restored.
// (A named function, not a lambda in the hook: ours_feed_lint reads any `return` before the RawEntry feed as an early exit.)
struct GbrPassCtx { PPCContext* ctx; uint8_t* base; };
inline bool GbrPass0(void* p) {
  GbrPassCtx* pc = static_cast<GbrPassCtx*>(p);
  const PPCContext saved = *pc->ctx;
  pc->ctx->r3.u64 = wwe2k14::scene_skip_nodes::Rd(pc->base, wwe2k14::scene_skip_nodes::kCameraTablePtr);
  sub_82BA77A8(*pc->ctx, pc->base);
  const bool zero = pc->ctx->r3.u32 == 0;
  *pc->ctx = saved;
  return zero;
}

REX_HOOK_RAW(sub_82C04DF0) {
  rdr_scene::live::guestmodel::OnModelEntry(ctx.r3.u32, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));   // S103 CW-2c: the guest model reader, BEFORE the Ours return (the mid-asm wwe2k14_model_entry below __imp__ never runs when the frame is OURS)
  wwe2k14::drawcam::NoteDraw(ctx.r13.u32, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));   // S103 WC R5 (draw_camera_glue.h)
  // S103 PW (review fix 1): the weapons' rows are taken FIRST, whatever the frame's mode. Under OURS the Ours test below returns before the original, so the mid-asm hook inside it (0x82C04E1C) never runs: the stream-free build
  // (every frame OURS) recorded no weapon call at all. RawEntry is weapon_rows_core.h RawHookEntry (the unit runs the same order with Ours = true). r5 here is [model+200], set by the callers.
  if (rdr_scene::live::WrestlersRegistered() && rdr_scene::live::Enabled()) rdr_scene::live::Wrestlers().NoteDrawAsked(ctx.r3.u32, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), wwe2k14::wrestlers::ReadWordNoNote);   // S103 DRL: the game asked to draw this model (liveness of a body nothing updates: the post-match freeze frame)
  const bool ours_model = wwe2k14::scene_skip::Ours(::scene_skip::Hook::Model);
  // S103 GBR: "the thread's shadow flag A (sub_82BA77A8: u32 [0x83929A48 + 4 * slot]) is 0" (the colour pass), asked by the tracker ONLY for a gm set object / a blood splat call (GMS.md (A) / (B)); ctx is restored
  GbrPassCtx pass_ctx{&ctx, base};
  if (!wwe2k14::weaponrows::RawEntry(ours_model, ctx.r3.u32, ctx.r5.u32, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), float(ctx.f1.f64), uint32_t(ctx.lr), &GbrPass0, &pass_ctx, wwe2k14::weaponrows::ShadowKindHook(ours_model, ctx.r13.u32, wwe2k14::g_guest_frame.load(std::memory_order_relaxed)))) {   // S103 STG: flag A at the call (0 colour / 1 props map / 2 map A-B), read only when the frame is not OURS   // S103 CGF: f1 = MdlAlpha, lr = the guest caller (the cage's two draw nodes tell apart by it)   // S103 H: the frame is OURS (scene_frame.h): no recorded draw for this model; the D3 scopes below are bypassed (one switch, no mix). SCENE_SKIPS.md row 1
    ctx.r3.u64 = 1;   // what the function returns when it drew (the D3 skip returns the same)
    return;
  }
#if WWE2K14_STREAM()  // S103 P9 (B3): the four D3 skips (ring, wrestler colour call, object) are the stream's
  wwe2k14::d3hook::RingScope ring_scope(ctx, base, wwe2k14::draw_origin::t_part);   // lane 246 (Ring D3 live seam, 16.9): a call whose model is one of the ring's 13 pointers is the RING chain's (one call, one chain); off = one relaxed load. It leaves the open-call slot set, so the arena's scope below does not decide the same call
  if (ring_scope.skipped()) return;
  wwe2k14::d3hook::WrestlerScope d3w_scope(ctx, base, wwe2k14::draw_origin::t_part);   // lane 245 (D3 skip the wrestler's colour call, the wrestler chain's own state): off = one acquire load; the arena's scope below is untouched
  if (d3w_scope.skipped()) return;
  wwe2k14::d3hook::CallScope d3_scope(ctx, base, wwe2k14::draw_origin::t_part);   // lane 138 (D3 skip the object): off = one relaxed load; on, a proven object is replaced by a marker and this call returns here
  if (d3_scope.skipped()) return;
#endif
  wwe2k14::cost::RecordSpan cost_span(ctx.r3.u32);
  // lane 122 (span_timer.h): the records this span appended to the thread's command list, by tag (only with the cost counters on, only the outermost span of a thread)
  struct GuestListMem {
    const uint8_t* base;
    uint32_t Ld32(uint32_t a) const {
      uint32_t v;
      std::memcpy(&v, base + a, 4);
      return __builtin_bswap32(v);
    }
  } list_mem{base};
  wwe2k14::spans::CursorSpan<GuestListMem> span(list_mem, ctx.r13.u32, wwe2k14::spans::kDescriptorStride, wwe2k14::cost::On() && wwe2k14::cost::t_span_depth == 1);
  const uint32_t object = ctx.r3.u32;
  __imp__sub_82C04DF0(ctx, base);
  if (span.on()) span.Finish(int(wwe2k14::cost::ClassOf(object, 0)));  // (review 7: ClassOf only when the span is being counted)
}

// The UI sprite / text draw (docs\research\s101\A0-4b.md; listing read S101: r3 = the sprite object, kept in r31; its texture handle's
// key word at +400): the op-19 draws it records carry {object = the sprite, part = the handle key, pass_site = its caller}, so a 2D draw
// names its widget object and its texture. (pass_site is not a scene pass here: the class comes from the submitter row, sprite.)
S103_I1_UI_SCOPE(sub_82BC4890, 400, rdr_scene::kQkSprite)  // (S101 lane18: through the shared UiScope, which adds the widget; S103 I2: also the 2D record's sprite scope)
// S101 lane18: the other op-19 2D draws. Each listing was read: r3 = the object (kept in a callee-saved register and used for the +172..177
// flag bytes / +256 / +260 / +268 like sub_82BC4890), the texture handle resolved by sub_82294F20 at r3 + tex_off (word 0 = key, word 1 =
// resolved); docs\renderer\NAMES.md "2D draws" has the table. A rectangle (T_Rect*) has no texture: tex_off 0.
S101_UI_SCOPE(sub_82BC59D0, 464)  // coloured sprite variants (T_SpriteC*): handle at +464
S101_UI_SCOPE(sub_82BC6A18, 416)  // gradient variants (T_SpriteG*): its draw tests the word at +420 (handle +416, 0x82BC6C30); +400 is an embedded object and +464 a second handle passed to sub_82BB1418 (review, unconfirmed); runs inside sub_82BC8180 (the outer scope is kept)
S103_I3_UI_TEXT_SCOPE(sub_82BC8180, 416)  // sprite variants (T_Sprite*M / C / R): handle at +416 (S103 I3: also the 2D record's glyph scope; S103 P3b: a draw from this leaf's site 0x82BC8BEC is a sprite row when it is the plain T_Sprite (technique 0x8206AA0C) (vertex format 5, stride 20, no c4 / c2 / c3 upload in the scope), else a counted hole)
S101_UI_SCOPE(sub_82BC3098, 400)  // textured rectangle / line; no direct callers (vtable only; ran in in_ring coverage only)
S103_I3_UI_TEXT_SCOPE(sub_82BC3518, 416)  // textured rectangle variant = the glyph quad of a text run (S103 I3: also the 2D record's glyph scope)
S103_I1_UI_SCOPE(sub_82BC3C18, 0, rdr_scene::kQkRect)    // T_Rect (S103 I1: also the 2D record's scope)
S103_I1_UI_SCOPE(sub_82BC3F30, 0, rdr_scene::kQkRectColor)    // T_RectColor (S105 A8: also the 2D record's scope: its single draw at 0x82BC4404 is a vertex-colour rect row, the 30 Years narration caption's dark box; docs/research/s105/A8.md)
S103_I1_UI_SCOPE(sub_82BC4450, 0, rdr_scene::kQkRect)    // T_Rect / T_RectColor / T_RectRGradation (S103 P3b: also the 2D record's rect scope: only its single draw at 0x82BC484C with vertex format 1 (stride 12) is a row; the colour / gradation formats have another stride, the band loop's site 0x82BC47C8 is a hole)
S103_I1_UI_SCOPE(sub_82BC75B0, 400, rdr_scene::kQkYuv)  // T_SpriteYUV: plane handles at +400 / +408 / +416 (+424 is its technique); S103 SVY: also the 2D record's scope (the no-alpha movie sprite of the season videos)
S103_I1_UI_SCOPE(sub_82BC7A10, 400, rdr_scene::kQkYuvAlpha)  // T_SpriteYUVAlpha: first plane handle at +400 (Y), then +408 / +416 / +424 (S103 I1: also the 2D record's scope)
// The text object's draw (sub_82BD06B8: r3 = the object, r4 = the string to draw, UTF-16 big-endian: its listing calls wcslen sub_82C7C5C8 on r4
// at 0x82BD06F4 [C]). Reached from the vtable slots 49 / 51 tail-calls (sub_82BD0BD8 / 0BE8) and the narrow path after its conversion. The
// code units are read here once, ONLY while labels are on (a capture is active), and held in t_label for this call's duration.
inline void ReadUnitsBe(const uint8_t* base, uint32_t p, uint16_t* units, size_t cap, size_t& n) {
  n = 0;
  for (; n < cap; ++n) {
    const uint32_t a = p + uint32_t(2 * n);
    // A page boundary: check the page of BOTH bytes before reading (an odd start puts a unit across two pages; Opus re-review D1).
    if ((n == 0 || (a & 0xFFFu) < 2 || ((a + 1) & 0xFFFu) == 0) &&
        !(wwe2k14::GuestWordReadableFast(a) && wwe2k14::GuestWordReadableFast(a + 1))) break;
    units[n] = uint16_t((uint16_t(base[a]) << 8) | base[a + 1]);
    if (!units[n]) { ++n; break; }
  }
}

// The text element's DRAW method (vtable 0x82065FF0 slot 15, sub_82B553F8; data word 0x8206602C; listing: r3 = the element, kept in r31, its
// string word +1136 and size / scale floats read from it, then sub_82B546E0(element) which gets the text object, sets its string and runs its
// draw chain -> sub_82BD0380 -> sub_82BC3518 / sub_82BC8180). Slots 30 / 31 (sub_82B554E0 / sub_82B555E8) only MEASURE the text (Opus review of
// 3ddc317f); they are not hooked. ElementDraw reads ctx.r3 before the original runs.
REX_HOOK_RAW(sub_82BD06B8) {
  namespace og = rdr_adapter::origin;
  const uint32_t str = ctx.r4.u32;  // read before the original runs
  if (!og::g_on.load(std::memory_order_relaxed) || !REXCVAR_GET(wwe2k14_draw_origin)) {
    __imp__sub_82BD06B8(ctx, base);
    return;
  }
  // S101 U1: every glyph quad recorded inside this call is class text (the scope below, taken by sub_82BC3518 / sub_82BC8180); with labels on
  // (capture / case ring) the whole string is also read (up to 512 units), the label being its first 40.
  if (!og::g_labels.load(std::memory_order_relaxed) || !wwe2k14::draw_origin::GuestPtr(str)) {
    og::TextRunScope run(false, nullptr, 0, false);
    __imp__sub_82BD06B8(ctx, base);
    return;
  }
  uint16_t units[og::kLabelUnitsRun];
  size_t n;
  ReadUnitsBe(base, str, units, og::kLabelUnitsRun, n);
  const bool truncated = n && units[n - 1] != 0;  // no NUL read: cut at the cap or at the end of the readable guest page (review D1)
  og::LabelScope label(units, n < og::kLabelUnits ? n : og::kLabelUnits);
  og::TextRunScope run(true, units, n, truncated);
  __imp__sub_82BD06B8(ctx, base);
}
REX_HOOK_RAW(sub_82B553F8) {
  wwe2k14::draw_origin::ElementDraw element(ctx, base);
  __imp__sub_82B553F8(ctx, base);
}

REX_HOOK_RAW(sub_82C1B350) {
  wwe2k14::q2d_tap::OnOp19(ctx, base);   // S103 I1: read only, before the recorder clobbers r4-r7 (a thread_local flag test when no 2D scope is open)
  wwe2k14::draw_origin::RecordTag tag(ctx, base, 19);
  __imp__sub_82C1B350(ctx, base);
}

// S103 I2 (REMNANT_MAP.md 5.1 T2b): the op-28 recorder (db append_tag_28_record_with_r6_16_byte_units_to_per_thread_buffer; r4 = first pixel-constant register, r5 = payload, r6 = count; no other hook on it): read only, before the recorder
// clobbers r4-r6. Inside an open 2D sprite scope it keeps the hue-shift triple and the blur / gradation marker; a thread_local flag test otherwise (479 call sites game-wide).
REX_HOOK_RAW(sub_82BB1CF8) {
  wwe2k14::q2d_tap::OnOp28(ctx, base);   // S103 I2
  __imp__sub_82BB1CF8(ctx, base);
}

// S103 SC (landing 2: our 2D must clip like the game's): the op-79 recorder (SetScissorRect: db append_32byte_record_op79_copy_4_words_to_slot_buffer; r4 = the four ints left / top / right / bottom; its only caller is sub_82C22DD0 <- sub_82BB7D48 <- the
// game's clip routine sub_823C8B30). Read only, before the recorder: the tap keeps this thread's last rect; the scissor ENABLE is read from the state object at each op-19 (quads2d_core.h FillScissor).
REX_HOOK_RAW(sub_82C2C4B8) {
  wwe2k14::q2d_tap::OnOp79(ctx, base);
  __imp__sub_82C2C4B8(ctx, base);
}

// S103 SC (scope add): the colour-target recorders. op 45 SetRenderTarget (db init_record_45_and_append_to_indexed_buffer: r4 = slot, r5 = the surface) and op 42 SetViewport (sub_82BCBB68 copies the 6 words at r4 into the record). Read only.
REX_HOOK_RAW(sub_82BB3978) {
  wwe2k14::q2d_tap::OnSetRenderTarget(ctx, base);
  __imp__sub_82BB3978(ctx, base);
}
REX_HOOK_RAW(sub_82BB3FC0) {   // op 47 reset_rt_ds_default (executor sub_82BB3910 -> SetRT(0, engine+232), SetDS): the default target again, the scissor rewritten (review F2)
  wwe2k14::q2d_tap::OnResetRtDs(ctx, base);
  __imp__sub_82BB3FC0(ctx, base);
}
REX_HOOK_RAW(sub_82BB3B08) {   // op 48 SetDepthStencil (executor sub_82BB43C8 -> sub_82CAC088 -> sub_82CABC68): the scissor rewritten (review F2)
  wwe2k14::q2d_tap::OnSetDepthStencil(ctx, base);
  __imp__sub_82BB3B08(ctx, base);
}
// S103 PFR (docs\research\s103\PFR.md): op 46 GetRenderTarget (r4 = slot, r5 = holder), op 73 (the scene colour target) and op 74 (resolve + the default target again). Read only, before the recorder.
REX_HOOK_RAW(sub_82BB3A40) {
  wwe2k14::q2d_tap::OnGetRenderTarget(ctx, base);
  __imp__sub_82BB3A40(ctx, base);
}
REX_HOOK_RAW(sub_82BB3C98) {   // op 58 (release of *r4): the save under that holder is dead (PFR review 5)
  wwe2k14::q2d_tap::OnReleaseHolder(ctx, base);
  __imp__sub_82BB3C98(ctx, base);
}
REX_HOOK_RAW(sub_82BB6FC8) {
  wwe2k14::q2d_tap::OnSceneBegin(ctx, base);
  __imp__sub_82BB6FC8(ctx, base);
}
REX_HOOK_RAW(sub_82BB7078) {
  wwe2k14::q2d_tap::OnSceneEnd(ctx, base);
  __imp__sub_82BB7078(ctx, base);
}
// S105 A4B (docs\research\s105\A4B_ADV.md item 5): the MAIN frame walk sub_82C2FA10 calls sub_82C3B200 (r3 = [0x8394011C], r4 = the category, r5 = the walk key) before each category and sub_82C3B218 after it
// (reached on every path, one call site each). The tap marks scene-colour rows recorded below the first wrestler category (they draw before the 3D). The begin's return (r3) is left alone.
// NOT sub_82C2F960 (the sub-walker: its categories are sub-walk ids). Runs in the stream-free build: the walk is the game's own.
REX_HOOK_RAW(sub_82C3B200) {
  wwe2k14::q2d_tap::OnWalkBegin(ctx.r4.u32);
  __imp__sub_82C3B200(ctx, base);
}
REX_HOOK_RAW(sub_82C3B218) {
  __imp__sub_82C3B218(ctx, base);
  wwe2k14::q2d_tap::OnWalkEnd();
#if !WWE2K14_STREAM()
  wwe2k14::walk1::Census(base);   // S110 WALKSINGLE: the [walk1] census line every 600 guest frames (scene_live/walk_single.h): this hook runs every frame, captions or not
#endif
}
REX_HOOK_RAW(sub_82BCBB68) {
  wwe2k14::q2d_tap::OnSetViewport(ctx, base);
  __imp__sub_82BCBB68(ctx, base);
}

REX_HOOK_RAW(sub_82BCBDD8) {
  wwe2k14::draw_origin::RecordTag tag(ctx, base, 68);
  __imp__sub_82BCBDD8(ctx, base);
}
