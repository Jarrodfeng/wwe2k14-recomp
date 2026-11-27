// S111 DYNSTATE unit: draw state set per draw instead of baked into the pipeline (src\renderer\dyn_state.h), and what it does to the stored pipeline list
// and the shipped seed (src\renderer\pipeline_store.h LimitsWord / ParseList, src\renderer\pipeline_seed.h MergeSeed). 0 game runs, no GPU, no Vulkan, no SDK.
// entry: main
// What it holds:
//   key      two DrawStates that differ only in blend, or only in depth write, are ONE pipeline key where the device sets that group per draw, and TWO where
//            it does not; what is not dynamic (program, layout, topology, stencil, variant) always stays in the key
//   record   after every draw the state at the command buffer is that draw's own, for every byte the key no longer holds (also between two draws on ONE
//            pipeline); a value already set is not set again; a group not in use issues no command
//   caps     a blend into an attachment whose format cannot blend is switched off (counted), as the baked path does; a plain write is left alone
//   device   a group needs its extension AND its feature bits (all three for blend) AND the caller's leave
//   list     a list written under a smaller dynamic set (an old list of full states on a device that now sets state per draw) is read as this device's keys,
//            never as keys of another shape; one written under a set holding a group this device bakes is refused whole; a device with no dynamic state keeps
//            its old list as it is
//   seed     seed records are full states: where state is dynamic, records that differ only in it become one pending pipeline
// Usage: dyn_state_unit [plant N]   (each plant is the UNSAFE direction of one verdict; the check named in tools\renderer\dyn_state_unit.py must fail)
// Build: clang++ -std=c++20 -Wall -Wextra -o out\tools\dyn_state_unit.exe tools\renderer\dyn_state_unit.cpp
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/renderer/dyn_state.h"
#include "../../src/renderer/pipeline_seed.h"

namespace ps = rdr::pstore;
namespace dy = rdr::dyn;
using rdr::BlendState;
using rdr::DrawState;

static int g_fail = 0;
static int g_plant = 0;
static void Check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) ++g_fail;
}

static bool Same(const DrawState& a, const DrawState& b) { return std::memcmp(&a, &b, sizeof(DrawState)) == 0; }

// The key as GetPipeline makes it. Plant 1: the key ignores the device (never shrinks). Plant 4: the key shrinks on a device that sets nothing per draw.
static DrawState Key(const DrawState& s, uint32_t mask) {
  if (g_plant == 1) return dy::KeyState(s, 0);
  if (g_plant == 4) return dy::KeyState(s, dy::kAll);
  return dy::KeyState(s, mask);
}

// What the command buffer holds: every command is recorded here.
struct Sink {
  int calls = 0;
  uint8_t cull = 0xEE, front = 0xEE, dt = 0xEE, dw = 0xEE, dc = 0xEE;
  uint8_t en[rdr::kMaxColorTargets] = {0xEE, 0xEE, 0xEE, 0xEE};
  uint8_t wm[rdr::kMaxColorTargets] = {0xEE, 0xEE, 0xEE, 0xEE};
  BlendState eq[rdr::kMaxColorTargets] = {};
  bool eq_set[rdr::kMaxColorTargets] = {};
  std::string log;
  void CullMode(uint8_t v) { cull = v, ++calls, log += 'C'; }
  void FrontFace(uint8_t v) { front = v, ++calls, log += 'F'; }
  void DepthTest(bool on) { dt = on, ++calls, log += 'T'; }
  void DepthWrite(bool on) { dw = on, ++calls, log += 'W'; }
  void DepthCompare(uint8_t v) { dc = v, ++calls, log += 'O'; }
  void BlendEnable(uint32_t c, bool on) { en[c] = on, ++calls, log += 'E'; }
  void BlendEquation(uint32_t c, const BlendState& b) { eq[c] = b, eq_set[c] = true, ++calls, log += 'Q'; }
  void WriteMask(uint32_t c, uint8_t m) { wm[c] = m, ++calls, log += 'M'; }
};

// One render pass being recorded: the bound pipeline (by key), what was set, the sink.
struct Pass {
  Sink sink;
  dy::Applied last;
  dy::Counts n;
  DrawState bound{};
  bool have = false;
  int binds = 0;
};

static void Draw(Pass& p, const DrawState& s, uint32_t mask, uint32_t colors, uint32_t blend_ok = 0xF, bool fallback = true) {
  const DrawState k = dy::KeyState(s, mask);
  const bool same_pipeline = p.have && Same(k, p.bound);
  if (!same_pipeline) ++p.binds;
  p.bound = k, p.have = true;
  if (g_plant == 2 && same_pipeline) return;   // plant 2: state is set only when the pipeline changes (the old bind rule applied to state)
  dy::Apply(p.sink, p.last, s, mask, colors, blend_ok, fallback, p.n);
}

// Does the command buffer hold this draw's own value for every byte the key no longer holds?
static bool Holds(const Sink& k, const DrawState& s, uint32_t mask, uint32_t colors) {
  if (mask & dy::kRaster)
    if (k.cull != s.cull || k.front != s.front_face || k.dt != (s.depth_test ? 1 : 0) || k.dw != (s.depth_write ? 1 : 0) || k.dc != s.depth_compare) return false;
  if (mask & dy::kBlend)
    for (uint32_t c = 0; c < colors; ++c) {
      const BlendState& b = s.blend[c];
      if (k.en[c] != (b.enable ? 1 : 0) || k.wm[c] != b.write_mask || !k.eq_set[c]) return false;
      if (k.eq[c].src_color != b.src_color || k.eq[c].dst_color != b.dst_color || k.eq[c].color_op != b.color_op || k.eq[c].src_alpha != b.src_alpha || k.eq[c].dst_alpha != b.dst_alpha || k.eq[c].alpha_op != b.alpha_op) return false;
    }
  return true;
}

static DrawState Base() {
  DrawState s{};
  s.program = 7, s.vertex_layout = 3, s.topology = 3, s.cull = 2, s.front_face = 1;
  s.depth_test = 1, s.depth_write = 1, s.depth_compare = 6, s.stencil_test = 0;
  s.blend[0] = {0, 1, 0, 0, 1, 0, 0, 0xF};   // opaque: a plain write
  return s;
}
static DrawState Blended(DrawState s) {
  s.blend[0] = {1, 6, 7, 0, 1, 7, 0, 0xF};   // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
  return s;
}

static ps::Record Rec(const DrawState& state, uint64_t vh, const char* name, uint16_t depth) {
  ps::Record r;
  r.state = state;
  r.state.program = 0, r.state.vertex_layout = 0;
  r.vhash = vh, r.fhash = vh + 1;
  r.pass.color_count = 1, r.pass.has_depth = 1, r.pass.samples = 1;
  r.pass.formats[0] = 37;
  r.pass.formats[rdr::kMaxColorTargets] = depth;
  std::strncpy(r.name, name, ps::kNameBytes - 1);
  return r;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "plant" && i + 1 < argc) g_plant = std::atoi(argv[++i]);

  const DrawState opaque = Base();
  const DrawState blended = Blended(opaque);
  DrawState no_zwrite = opaque;
  no_zwrite.depth_write = 0;
  DrawState other_program = opaque;
  other_program.program = 8;
  DrawState prepass = opaque;
  prepass.variant = rdr::kVariantDepthOnly;
  DrawState stencilled = opaque;
  stencilled.stencil_test = 1, stencilled.front.pass = 2;

  // ---- key ----
  Check(Same(Key(opaque, dy::kAll), Key(blended, dy::kAll)), "key.blend_twins_are_one_pipeline_where_supported");
  Check(Same(Key(opaque, dy::kAll), Key(no_zwrite, dy::kAll)), "key.depth_write_twins_are_one_pipeline_where_supported");
  Check(!Same(Key(opaque, 0), Key(blended, 0)) && !Same(Key(opaque, 0), Key(no_zwrite, 0)), "key.twins_stay_two_pipelines_where_not_supported");
  Check(Same(Key(opaque, 0), opaque), "key.a_device_with_no_dynamic_state_keys_by_the_whole_state_as_before");
  Check(Same(Key(opaque, dy::kRaster), Key(no_zwrite, dy::kRaster)) && !Same(Key(opaque, dy::kRaster), Key(blended, dy::kRaster)), "key.raster_only_device_collapses_depth_write_and_keeps_blend_in_the_key");
  Check(Same(Key(opaque, dy::kBlend), Key(blended, dy::kBlend)) && !Same(Key(opaque, dy::kBlend), Key(no_zwrite, dy::kBlend)), "key.blend_only_device_collapses_blend_and_keeps_depth_write_in_the_key");
  Check(!Same(Key(opaque, dy::kAll), Key(other_program, dy::kAll)) && !Same(Key(opaque, dy::kAll), Key(prepass, dy::kAll)) && !Same(Key(opaque, dy::kAll), Key(stencilled, dy::kAll)),
        "key.program_variant_and_stencil_stay_in_the_key");
  {
    const DrawState k = dy::KeyState(blended, dy::kAll);
    Check(k.program == 7 && k.vertex_layout == 3 && k.topology == 3 && k.cull == 0 && k.depth_write == 0 && k.blend[0].enable == 0 && k.blend[0].write_mask == 0, "key.dynamic_bytes_are_zero_the_rest_kept");
    Check(Same(dy::KeyState(k, dy::kAll), k), "key.making_the_key_twice_changes_nothing");
  }
  {  // the renderer's own twins: the blend-off twin is the same pipeline where blend is dynamic; the depth-only prepass twin keeps its variant
    DrawState blend_off = blended;
    for (auto& b : blend_off.blend) b.enable = 0;
    DrawState twin = blended;
    twin.variant = rdr::kVariantDepthOnly;
    for (auto& b : twin.blend) b.write_mask = 0;
    Check(Same(Key(blended, dy::kAll), Key(blend_off, dy::kAll)) && !Same(Key(blended, 0), Key(blend_off, 0)), "key.the_blend_off_twin_is_the_draws_own_pipeline_where_blend_is_dynamic");
    Check(!Same(Key(blended, dy::kAll), Key(twin, dy::kAll)) && Same(Key(twin, dy::kAll), Key(prepass, dy::kAll)), "key.one_prepass_twin_for_every_blend_of_a_program");
  }

  // ---- record ----
  {
    Pass p;
    Draw(p, opaque, dy::kAll, 1);
    Check(p.sink.calls == 8 && Holds(p.sink, opaque, dy::kAll, 1), "record.the_first_draw_of_a_pass_sets_everything_in_use");
    const int after_first = p.sink.calls;
    Draw(p, opaque, dy::kAll, 1);
    Check(p.sink.calls == after_first, "record.a_value_already_set_is_not_set_again");
    p.sink.log.clear();
    Draw(p, no_zwrite, dy::kAll, 1);
    Check(p.binds == 1 && p.sink.log == "W" && Holds(p.sink, no_zwrite, dy::kAll, 1), "record.sets_what_the_pipeline_no_longer_bakes");
    p.sink.log.clear();
    Draw(p, blended, dy::kAll, 1);
    Check(p.binds == 1 && p.sink.log == "WEQ" && Holds(p.sink, blended, dy::kAll, 1), "record.a_blend_change_on_one_pipeline_sets_enable_and_equation_only");
    DrawState blend_off = blended;
    blend_off.blend[0].enable = 0;
    p.sink.log.clear();
    Draw(p, blend_off, dy::kAll, 1);
    Check(p.sink.log == "E" && p.sink.en[0] == 0, "record.the_blend_off_twin_is_one_command");
  }
  {  // a sequence of states on few pipelines: after every draw the buffer holds that draw's state
    const DrawState seq[] = {opaque, blended, no_zwrite, other_program, Blended(other_program), opaque, Blended(no_zwrite), prepass, opaque};
    const uint32_t masks[] = {dy::kAll, dy::kRaster, dy::kBlend};
    bool ok = true;
    for (uint32_t mask : masks) {
      Pass p;
      for (const DrawState& s : seq) {
        Draw(p, s, mask, 1);
        ok = ok && Holds(p.sink, s, mask, 1);
      }
    }
    Check(ok, "record.after_every_draw_the_buffer_holds_that_draws_state");
  }
  {
    Pass p;
    Draw(p, blended, 0, 1);
    Draw(p, no_zwrite, 0, 1);
    Check(p.sink.calls == 0 && p.binds == 2, "record.a_device_with_no_dynamic_state_issues_no_command_and_binds_two_pipelines");
    Pass r;
    Draw(r, blended, dy::kRaster, 1);
    Check(r.sink.log == "CFTWO", "record.a_raster_only_device_never_sets_blend");
    Pass m;
    DrawState two = blended;
    two.blend[1] = {0, 1, 0, 0, 1, 0, 0, 0x3};
    Draw(m, two, dy::kBlend, 2);
    Check(m.sink.calls == 6 && m.sink.wm[1] == 0x3 && m.sink.wm[2] == 0xEE, "record.every_colour_attachment_of_the_pass_is_set_and_no_other");
  }
  // ---- caps ----
  {
    Pass p;
    Draw(p, blended, dy::kAll, 1, 0x0, true);
    Check(p.sink.en[0] == 0 && p.n.blend_masked == 1 && p.sink.eq[0].src_color == 6, "caps.a_blend_into_a_format_that_cannot_blend_is_switched_off");
    Pass q;
    Draw(q, blended, dy::kAll, 1, 0x0, false);
    Check(q.sink.en[0] == 1 && q.n.blend_illegal == 1 && q.n.blend_masked == 0, "caps.fallbacks_off_asks_the_driver_anyway_and_counts_it");
    Pass w;
    DrawState plain = opaque;
    plain.blend[0].enable = 1;   // enabled, but ONE / ZERO / ADD: a plain write
    Draw(w, plain, dy::kAll, 1, 0x0, true);
    Check(w.sink.en[0] == 1 && w.n.blend_masked == 0 && dy::PlainWrite(plain.blend[0]) && !dy::PlainWrite(blended.blend[0]), "caps.a_plain_write_needs_no_blend_feature");
  }
  // ---- device ----
  {
    dy::Report r;
    Check(dy::MaskOf(r, ~0u) == 0, "device.nothing_listed_is_nothing_in_use");
    r.ext1 = true;
    Check(dy::MaskOf(r, ~0u) == 0, "device.an_extension_without_its_feature_bit_is_not_used");
    r.feature1 = true;
    Check(dy::MaskOf(r, ~0u) == dy::kRaster, "device.raster_group_needs_extension_and_feature");
    r.ext3 = true, r.blend_enable = true, r.blend_equation = true;
    Check(dy::MaskOf(r, ~0u) == dy::kRaster, "device.blend_group_needs_all_three_feature_bits");
    r.write_mask = true;
    Check(dy::MaskOf(r, ~0u) == dy::kAll && dy::MaskOf(r, 0) == 0 && dy::MaskOf(r, dy::kRaster) == dy::kRaster, "device.the_callers_leave_is_applied");
  }
  // ---- list ----
  {
    const ps::DeviceId id{0x10DE, 0x2684, 1, {}};
    std::vector<ps::Record> old_list = {Rec(opaque, 100, "a", 130), Rec(blended, 100, "a", 130)};
    const std::vector<uint8_t> file = ps::SerialiseList(old_list, 5, id, ps::LimitsWord(0, 0));   // what a build before this one wrote: full states, no dynamic bits
    std::vector<ps::Record> out;
    uint32_t session = 0, dropped = 0;
    const uint32_t reader = g_plant == 3 ? ps::LimitsWord(0, 0) : ps::LimitsWord(0, dy::kAll);   // plant 3: the reader does not say what it sets per draw
    bool reshaped = false;
    const std::string why = ps::ParseList(file, out, session, id, reader, dropped, false, &reshaped);
    Check(why.empty() && out.size() == 2 && reshaped && Same(out[0].state, dy::KeyState(old_list[0].state, dy::kAll)) && Same(out[1].state, dy::KeyState(old_list[1].state, dy::kAll)) &&
          ps::RecordKey(out[0]) == ps::RecordKey(out[1]),
          "list.an_old_full_state_list_becomes_this_devices_keys");
    const std::string same = ps::ParseList(file, out, session, id, ps::LimitsWord(0, 0), dropped, false, &reshaped);
    Check(same.empty() && out.size() == 2 && !reshaped && Same(out[0].state, old_list[0].state) && Same(out[1].state, old_list[1].state), "list.a_device_with_no_dynamic_state_keeps_its_old_list");
    const ps::Record raster_rec = Rec(dy::KeyState(blended, dy::kRaster), 100, "a", 130);   // a kRaster device's key: blend still in it
    const std::vector<uint8_t> raster = ps::SerialiseList({raster_rec}, 6, id, ps::LimitsWord(0, dy::kRaster));
    Check(ps::ParseList(raster, out, session, id, ps::LimitsWord(0, dy::kAll), dropped, false, &reshaped).empty() && out.size() == 1 && reshaped && raster_rec.state.blend[0].enable == 1 &&
              Same(out[0].state, dy::KeyState(raster_rec.state, dy::kAll)),
          "list.a_list_of_a_smaller_dynamic_set_becomes_this_devices_keys");
    const std::vector<uint8_t> now = ps::SerialiseList({Rec(dy::KeyState(opaque, dy::kAll), 100, "a", 130)}, 6, id, ps::LimitsWord(0, dy::kAll));
    Check(ps::ParseList(now, out, session, id, ps::LimitsWord(0, dy::kAll), dropped, false, &reshaped).empty() && out.size() == 1 && !reshaped, "list.a_list_of_this_dynamic_set_loads");
    Check(!ps::ParseList(now, out, session, id, ps::LimitsWord(0, dy::kRaster), dropped, false).empty() && !ps::ParseList(now, out, session, id, ps::LimitsWord(0, 0), dropped, false).empty() &&
              out.empty() && !ps::ParseList(raster, out, session, id, ps::LimitsWord(0, dy::kBlend), dropped, false).empty(),
          "list.a_list_missing_a_state_this_device_bakes_is_refused");
    const std::string caps = ps::ParseList(now, out, session, id, ps::LimitsWord(0x2, dy::kAll), dropped, false);
    Check(!caps.empty() && caps.find("format-caps") != std::string::npos, "list.the_format_caps_refusal_keeps_its_own_reason");
    Check(ps::LimitsWord(0x80000042u, dy::kAll) == 0x83000042u && ps::LimitsWord(0x42, 0) == 0x42, "list.the_word_keeps_the_format_caps_bits");
  }
  // ---- seed ----
  {
    const uint32_t masks[2] = {dy::kAll, 0};
    uint32_t added[2] = {0, 0}, known[2] = {0, 0};
    for (int m = 0; m < 2; ++m) {
      std::vector<ps::Record> seed = {Rec(opaque, 200, "s", 130), Rec(blended, 200, "s", 130), Rec(no_zwrite, 200, "s", 130), Rec(opaque, 300, "t", 130)};
      for (ps::Record& r : seed) r.seed = true;
      if (g_plant != 5)   // plant 5: the seed's records are merged as they are (full states on a device that keys without them)
        for (ps::Record& r : seed) r.state = dy::KeyState(r.state, masks[m]);
      std::vector<ps::Record> records;
      std::unordered_map<uint64_t, uint32_t> index;
      std::vector<uint32_t> pending;
      const ps::SeedMerge sm = ps::MergeSeed(records, index, pending, seed);
      added[m] = sm.added, known[m] = sm.known;
    }
    Check(added[0] == 2 && known[0] == 2, "seed.records_differing_only_in_dynamic_state_merge_to_one");
    Check(added[1] == 4 && known[1] == 0, "seed.a_device_with_no_dynamic_state_keeps_every_seed_record");
  }
  std::printf("dyn_state_unit: %s (%d failed)\n", g_fail ? "FAILED" : "every check passed", g_fail);
  return g_fail ? 1 : 0;
}
