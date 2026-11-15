// S102 bundle 3 (DECISIONS 2026-11-15 08:1x, Jarrod): every renderer lever the Surface measured and KEPT is a player default, PC and phone.
// ONE LINE PER LEVER: add a line to WWE2K14_BUNDLE3_LEVERS when tools\surface\base_flags.txt gets the lever (a KEEP row); tools\bundle3\lint.py
// fails until the two agree (base_flags.txt minus the documented exclusions in that script = this list). A correctness fix that is not a
// measured speed lever goes in WWE2K14_BUNDLE3_FIXES (the lint then wants it in its reasons table). docs\renderer\BUNDLE3.md has the history
// and the per-platform audit.
// Applied in wwe2k14_app.h OnPreSetup, each only while the flag's source is still kDefault (the command line, a +dev APK's dev_flags.txt and the
// Surface bench's explicit flags all win), before anything reads them. Every entry is a PerFrame switch of renderer_adapter\rdr_switches.h
// (read through the table every frame) or a start-time read that happens after OnPreSetup (prelight_compute: the compute pipelines are made at
// renderer start). --wwe2k14_s102_bundle3=false gives v0.3.2's defaults.
#pragma once
#include <rex/cvar.h>

// X("cvar name", "value")
#define WWE2K14_BUNDLE3_LEVERS(X) \
  X("wwe2k14_rdr_lazy_banks", "true") \
  X("wwe2k14_rdr_card_cut", "true") \
  X("wwe2k14_rdr_fb_fast", "true") \
  X("wwe2k14_rdr_pack_once", "true") \
  X("wwe2k14_rdr_handoff_memory", "true") \
  X("wwe2k14_rdr_members_instanced", "true") \
  X("wwe2k14_rdr_lasting_draw", "true") \
  X("wwe2k14_rdr_low_no_fullsize_copies", "true") \
  X("wwe2k14_rdr_low_present_noclear", "true") \
  X("wwe2k14_rdr_low_shadow_small", "true") \
  X("wwe2k14_rdr_skip_dead_prelight", "true") \
  X("wwe2k14_rdr_alias_fullsize", "1") \
  X("wwe2k14_rdr_crowd_ids_once", "true") \
  X("wwe2k14_rdr_origin_memo", "true") \
  X("wwe2k14_rdr_bank_dirty", "true") \
  X("wwe2k14_rdr_note_memo", "true") \
  X("wwe2k14_rdr_shader_memo", "true") \
  X("wwe2k14_rdr_pin_hash", "true") \
  X("wwe2k14_rdr_lasting_nogroup", "true") \
  X("wwe2k14_rdr_skip_lean", "true") \
  X("wwe2k14_rdr_copy_direct", "true") \
  X("wwe2k14_rdr_origin_index", "true") \
  X("wwe2k14_rdr_scene_strings_once", "true") \
  X("wwe2k14_rdr_prelight_compute", "true") \
  X("wwe2k14_rdr_arena_fragments", "1") \
  X("wwe2k14_rdr_combine_lite", "1") \
  X("wwe2k14_rdr_upd_lean", "15") \
  X("wwe2k14_rdr_frame_facts", "true") \
  X("wwe2k14_rdr_frame_roundtrip", "2") \
  X("wwe2k14_rdr_swap_once", "7") \
  X("wwe2k14_rdr_crowd_fast", "true") \
  X("wwe2k14_nf_replay_loop", "true") \
  X("wwe2k14_nf_render_dispatch", "true") \
  X("wwe2k14_rdr_slot_content_skip", "true") \
  X("wwe2k14_rdr_slot_ways", "true") \
  X("wwe2k14_rdr_sampler_memo", "true") \
  X("wwe2k14_rdr_geo_hash_once", "true") \
  X("wwe2k14_rdr_bank_blocks", "true") \
  X("wwe2k14_rdr_frame_direct", "true") \
  X("wwe2k14_rdr_shadow_direct", "true") \
  X("wwe2k14_rdr_alias_small", "2") \
  X("wwe2k14_rdr_skip_blank_plane", "true") \
  X("wwe2k14_rdr_plan_memo", "7") \
  X("wwe2k14_rdr_render_lean", "7") \
  X("wwe2k14_rdr_stencil_dead", "true") \
  X("wwe2k14_rdr_member_cut", "true") \
  X("wwe2k14_rdr_record_skip", "7")

// Correctness fixes shipped with the bundle (not measured speed levers; BUNDLE3.md lists them apart).
#define WWE2K14_BUNDLE3_FIXES(X) \
  X("wwe2k14_rdr_run_end_keep", "true")

namespace wwe2k14::bundle3 {

// Sets every bundle-3 switch still at its default; returns how many were set (levers + fixes apart).
inline void Apply(int& levers, int& fixes) {
  levers = fixes = 0;
#define WWE2K14_B3_SET(name, value, counter)                                                         \
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {                              \
    rex::cvar::SetFlagByName(name, value);                                                           \
    ++counter;                                                                                       \
  }
#define WWE2K14_B3_LEVER(name, value) WWE2K14_B3_SET(name, value, levers)
#define WWE2K14_B3_FIX(name, value) WWE2K14_B3_SET(name, value, fixes)
  WWE2K14_BUNDLE3_LEVERS(WWE2K14_B3_LEVER)
  WWE2K14_BUNDLE3_FIXES(WWE2K14_B3_FIX)
#undef WWE2K14_B3_LEVER
#undef WWE2K14_B3_FIX
#undef WWE2K14_B3_SET
}

}  // namespace wwe2k14::bundle3
