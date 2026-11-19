// Weapons and props: the per-call rows, the game plumbing (Session 103, lane PW-2). Included by fp_probe.h (the model-entry hook and the D3 glue's g_entry). Copyright (c) the wwe2k14-recomp authors.
// BSD-3-Clause. Wraps the pure tracker (weapon_rows_core.h) with the guest reads of wrestler_hook.h (ReadWord / ReadMatrices: bounds- and fault-checked, big-endian bytes). Read only: nothing is written
// to the guest, no register changes, no call is skipped. Registered with the scene records + light states (wwe2k14_app.h OnPostSetup); until then one relaxed load.
#pragma once
#include <rex/logging.h>

#include <atomic>

#include "draw_camera_core.h"  // S103 STG: kFlagA ... (static_assert below), ReadThreadSlot via caster_core.h
#include "weapon_rows_contrib.h"
#include "wrestler_hook.h"  // ReadWord / ReadMatrices

namespace wwe2k14::weaponrows {

static_assert(rdr_scene::live::weaponrows::kShadowFlagA == rdr_scene::live::drawcam::kFlagA && rdr_scene::live::weaponrows::kShadowModulePtr == rdr_scene::live::drawcam::kShadowModulePtr &&
                  rdr_scene::live::weaponrows::kShadowLightRecordB == rdr_scene::live::drawcam::kLightRecordB && rdr_scene::live::weaponrows::kShadowCamTable == scene_inputs::kCamTable,
              "S103 STG: the shadow-kind addresses of weapon_rows_core.h equal draw_camera_core.h's / inputs_core.h's");

// S103 STG (PMW.md): 0 colour / 1 props map / 2 map A or B for the calling thread (weapon_rows_core.h ShadowKindAt). Asked only when the frame is NOT OURS (under OURS the game never runs a shadow module render, flag A is never set: 0 with no read, so an OURS
// frame is byte-identical to before this lane). The thread's slot is read once per (thread, r13, frame), as drawcam does.
inline uint8_t ShadowKindHook(bool ours, uint32_t r13, uint64_t frame) {
  if (ours) return 0;
  struct Cache { uint32_t r13 = 0; uint64_t frame = ~0ull; uint32_t slot = 0; bool ok = false; };
  thread_local Cache c;
  auto rw = [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); };
  if (c.r13 != r13 || c.frame != frame) {
    uint32_t sl = 0;
    c.ok = rdr_scene::live::casters::ReadThreadSlot(rw, r13, sl), c.slot = sl, c.r13 = r13, c.frame = frame;
  }
  return c.ok ? rdr_scene::live::weaponrows::ShadowKindAt(rw, c.slot) : uint8_t(0);
}

// Called from the REX_HOOK_RAW on sub_82C04DF0 (draw_origin.h), FIRST, before its scene_skip::Ours(Model) test: r3 = the model object, r5 = the palette pointer the packer reads ([model+200], set by the callers). Whether the
// call is the weapon's own submit comes from the thread's slot-13 marker (weaponrows::t_slot13_model, set by the raw hook on sub_82C13C88). Returns true when the original must run (not OURS). Logs the first 40 weapon calls.
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
inline bool RawEntry(bool ours, uint32_t model, uint32_t palette_ptr, uint64_t frame, float f1 = 1.0f, uint32_t lr = 0, bool (*pass0)(void*) = nullptr, void* pass_ctx = nullptr, uint8_t shadow = 0) {
  namespace wr = rdr_scene::live::weaponrows;
  wr::CallResult r;
  const bool run = wr::RawHookEntry(
      wr::Rows().tracker, wr::WeaponRowsRegistered() && rdr_scene::live::Enabled(), ours, model, palette_ptr, frame, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); },
      [](uint32_t a, uint32_t n, std::vector<rdr_scene::Mat4>& out) { return wwe2k14::wrestlers::ReadMatrices(a, n, out); }, &r, f1, lr, pass0, pass_ctx, shadow);   // S103 CGF: f1 / lr (ctx.f1, ctx.lr at the hook) tag a stage structure call's family
  if (r.gm || r.blood) {   // S103 GBR: the first 40 set-object / splat rows (the step check reads them)
    static std::atomic<int> logged_gb{0};
    if (logged_gb.fetch_add(1, std::memory_order_relaxed) < 40)
      REXLOG_INFO("[wwe2k14][weapon_rows] {} piece: frame {}: object 0x{:08X} id {} k {} instance {} f1 {} r5 0x{:08X} nodes {}{}", r.gm ? "gm set" : "blood splat", frame, model, r.id, r.k, r.instance, f1, palette_ptr, r.n_nodes, r.recorded ? "" : " (not recorded)");
  } else if (r.stage) {
    static std::atomic<int> logged_stage{0};
    if (logged_stage.fetch_add(1, std::memory_order_relaxed) < 60)
      REXLOG_INFO("[wwe2k14][weapon_rows] stage piece: frame {}: object 0x{:08X} pach {} k {} instance {} own submit {} lr 0x{:08X} f1 {} r5 0x{:08X} nodes {}{}", frame, model, r.id, r.k, r.instance, wr::IsOwnSubmit(model), r.lr, r.f1, palette_ptr, r.n_nodes, r.recorded ? "" : " (not recorded)");
  } else if (r.weapon) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 40)
      REXLOG_INFO("[wwe2k14][weapon_rows] frame {}: object 0x{:08X} instance {} own submit {} r5 0x{:08X} nodes {} (counted {} n {} vtable_ok {} rigid {} data_leaf {} forward fn 0x{:08X} vtable 0x{:08X}){}{}{}", frame, model, r.instance, wr::IsOwnSubmit(model), palette_ptr, r.n_nodes, r.counted, r.n, r.vtable_ok, r.rigid, r.data_leaf, r.fwd_fn, r.fwd_vtable, r.recorded ? "" : " (not recorded)", ours ? " (OURS frame)" : "",
                   r.event ? " (EVENT PROP id " + std::to_string(r.id) + " k " + std::to_string(r.k) + " own_submit " + (r.own_submit ? "1" : "0") + ")" : std::string());
  }
  return run;
}

// S103 BLF (docs\research\s103\BELT.md Adversary 1 / 4): the event prop models, from two mid-asm hooks (config\event_props_hooks.toml; the functions are in fp_probe.h next to wwe2k14_model_entry).
// MADE: 0x8272E030 in CWeaponEventSD::MakeWeapon sub_8272D710, right after `bl 0x82bd4160` (r3 = the new CSkinModel or 0), up to 10 times per prop; r29 = k (the entry loop counter [r1+80]); the prop id is the u32 at [r1+2348]
// (the prologue's `stw r4,2348(r1)`; the only store to it). FREED: 0x8272D678 in the CWeapon's derived destructor sub_8272D670, r3 = the CWeapon, its models still alive (the base dtor deletes them later).
// Both only record into the tracker's table: nothing is written to the guest, no register changes, no call skipped. Until the weapon rows are registered they do nothing but one relaxed load.
inline void EventModelMade(uint32_t r1, uint32_t model, uint32_t k) {
  namespace wr = rdr_scene::live::weaponrows;
  if (model == 0 || !wr::WeaponRowsRegistered()) return;
  uint32_t prop_id = 0;
  if (!wwe2k14::wrestlers::ReadWord(r1 + 2348u, prop_id)) return;
  wr::Rows().tracker.EventMake(model, prop_id, k);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 120)   // the step check reads Cena's belt here: expect prop id 2004 / 3004 (the champion's title), not 2009 / 3009
    REXLOG_INFO("[wwe2k14][weapon_rows] event prop made: id {} k {} model 0x{:08X} (PACH: see the loader's line 'event prop id {} -> PACH')", prop_id, k, model, prop_id);
}
// S103 LDR (docs\research\s103\COV10_match_objects.md B1): the stage holder's request sub_82576768 (r3 = the 56-byte non-blocking reader just made, r4 = the guest path string: a static string at 0x8201D5E0..0x8201D61C or the
// caller's stack buffer "/STGL/%04d"). Entry hook (draw_origin.h): reads the string (<= 12 bytes, fault-checked) and latches the PACH of the ladder paths in the tracker. Read only; the call runs unchanged. Until the rows are registered: one relaxed load.
inline void StageRequestPath(uint32_t path_ptr) {
  namespace wr = rdr_scene::live::weaponrows;
  if (!wr::WeaponRowsRegistered()) return;
  char buf[13] = {};
  const bool ok = path_ptr >= 0x10000u && wwe2k14::wrestlers::ReadGuest(path_ptr, buf, sizeof buf);
  uint32_t n = 0;
  while (ok && n < sizeof buf && buf[n] != '\0') ++n;
  wr::Rows().tracker.NoteStagePath(buf, ok ? (n < sizeof buf ? n : uint32_t(sizeof buf)) : 0u);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 40)   // the step check reads the path and its PACH here: expect "/STGL/<title>" -> PACH 11 + row in a ladder match
    REXLOG_INFO("[wwe2k14][weapon_rows] stage request path '{}' -> PACH {}{}", ok ? buf : "<unreadable>", wr::Rows().tracker.stage_path_pach.load(std::memory_order_relaxed), ok ? "" : " (the string was not readable)");
}
inline void EventWeaponFreed(uint32_t weapon) {
  namespace wr = rdr_scene::live::weaponrows;
  if (weapon == 0 || !wr::WeaponRowsRegistered()) return;
  const uint32_t n = wr::Rows().tracker.EventFree(weapon, [](uint32_t a, uint32_t& v) { return wwe2k14::wrestlers::ReadWord(a, v); });
  if (n != 0) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 120) REXLOG_INFO("[wwe2k14][weapon_rows] event prop freed: weapon 0x{:08X}, {} model(s) dropped", weapon, n);
  }
}

}  // namespace wwe2k14::weaponrows
