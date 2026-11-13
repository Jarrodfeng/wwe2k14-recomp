// shadow_casters_unit: the GPU-free unit test of the shipped ShadowCore (src\renderer_scene\shadow_casters_core.cpp; Session 101, lane 99, review F1). 0 game runs.
//   shadow_casters_unit --game DIR --capture DIR [--arena N] [--fault arena_stale]
// A ring-956 plane draw of a real frame is decided under arena A, then under arena B (a copy of A: the same meshes in other memory), then with no ring:
//   * the pairing and the built buffer of the (object, part) key are NOT reused across the swap: JoinArena names the arena of the frame, the old buffers are released, a new one is built;
//   * with no arena the draw is not replaced (not ready) and no pairing is left;
//   * the old arena can be freed while the core lives (the pairing held it: no dangling mesh pointer).
// --fault arena_stale plants the old bug (the ring state is kept across an arena change): the same checks must then FAIL (exit 1).
// Exit: 0 all hold, 1 a check failed, 2 cannot read the input, 3 bad usage.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "../../src/assets/ring_game_space.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/shadow_casters_core.h"

using namespace rdr_scene;
using rdr_adapter::KeepWhy;

namespace {
int g_fail = 0;
void Check(const char* name, bool ok, const char* fmt = "", ...) {
  char detail[500] = {};
  if (fmt && *fmt) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
  }
  std::printf("%s  %s%s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? "  (" : "", detail, detail[0] ? ")" : "");
  if (!ok) ++g_fail;
}
}  // namespace

int main(int argc, char** argv) {
  std::string game, capture, fault;
  int arena = 4;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--game") game = next();
    else if (a == "--capture") capture = next();
    else if (a == "--arena") arena = std::atoi(next());
    else if (a == "--fault") fault = next();
    else {
      std::fprintf(stderr, "shadow_casters_unit: unknown argument %s\n", a.c_str());
      return 3;
    }
  }
  if (game.empty() || capture.empty()) {
    std::fprintf(stderr, "usage: shadow_casters_unit --game DIR --capture DIR [--arena N] [--fault arena_stale]\n");
    return 3;
  }
  ShadowOptions opt;
  opt.verify_all = true;
  std::string err;
  if (!ShadowFaults::Parse(fault, opt.faults, err)) {
    std::fprintf(stderr, "shadow_casters_unit: %s\n", err.c_str());
    return 3;
  }
  rdr_adapter::CaptureReader cap;
  if (!cap.Open(capture)) {
    std::fprintf(stderr, "shadow_casters_unit: FAIL cannot open the capture at %s\n", capture.c_str());
    return 2;
  }
  if (!assets::ring_gs::Init(game)) {
    std::fprintf(stderr, "shadow_casters_unit: FAIL no game files at %s\n", game.c_str());
    return 2;
  }
  auto a_arena = std::make_shared<assets::ring_gs::RingArena>();
  if (!assets::ring_gs::Load(arena, *a_arena, err)) {
    std::fprintf(stderr, "shadow_casters_unit: FAIL ring of arena %d: %s\n", arena, err.c_str());
    return 2;
  }
  auto b_arena = std::make_shared<assets::ring_gs::RingArena>(*a_arena);   // the same meshes (the blobs are shared), another object in other memory
  std::shared_ptr<const assets::ring_gs::RingArena> current = a_arena;
  g_shadow_join_board = ShadowJoinBoard{};
  g_shadow_join_board.ring_arena = [&](const rdr_adapter::LoadedFrame&) { return current; };

  // a ring-956 plane draw of a real frame
  rdr_adapter::LoadedFrame frame;
  int draw_index = -1;
  for (uint32_t f = 150; f < cap.frame_count() && draw_index < 0; f += 25) {
    if (!cap.LoadFrame(f, frame)) continue;
    for (uint32_t d = 0; d < frame.draws.count && draw_index < 0; ++d) {
      const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
      if (!o || o->pass_site != rdr_adapter::kPlaneShadowSite || !o->object || frame.draws[d].layout >= frame.layouts.count) continue;
      const assets::ring_gs::RingMesh* m = a_arena->Find(956, int(o->part));
      if (m && m->vertices == frame.draws[d].vertex_count && frame.draws[d].index_bytes) draw_index = int(d);
    }
  }
  if (draw_index < 0) {
    std::fprintf(stderr, "shadow_casters_unit: FAIL no ring-956 plane draw in the capture\n");
    return 2;
  }
  const rdr_adapter::DrawOrigin& o = *frame.OriginOf(uint32_t(draw_index));
  const rdr_adapter::Draw& dr = frame.draws[uint32_t(draw_index)];
  rdr_adapter::ReplaceIn in;
  in.frame = &frame, in.draw = &dr, in.draw_index = uint32_t(draw_index), in.origin = &o;
  in.vertex_count = dr.vertex_count, in.index_count = dr.index_count;
  in.read_geometry = [&](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return cap.ReadGeometry(offset, bytes, out); };
  in.vertex_offset = dr.vertex_offset, in.index_offset = dr.index_offset;
  in.stream_index_32bit = dr.index_32bit;

  int released_hook = 0;
  ShadowCore core(opt);
  core.SetReleaseHook([&](ShadowPrepared&) { ++released_hook; });

  core.BeginFrame(frame);
  ShadowCore::Decision d1 = core.Decide(in);
  Check("arena_a.replaced", d1.ok && d1.klass == CasterClass::Ring, "ok %d class %s", d1.ok, CasterClassName(d1.klass));
  Check("arena_a.joined_to_a", core.JoinArena(o.object, o.part) == a_arena.get(), "the pairing names arena A");
  const uint64_t built_a = core.stats().prepared_built;

  current = b_arena;   // the ring hook changed its arena (another match, or the same one reloaded)
  core.BeginFrame(frame);
  ShadowCore::Decision d2 = core.Decide(in);
  Check("swap.replaced", d2.ok, "ok %d (the same draw under arena B)", d2.ok);
  Check("swap.join_not_reused", core.JoinArena(o.object, o.part) == b_arena.get(), "the pairing now names arena B, not A (A %p, B %p, joined %p)", (void*)a_arena.get(), (void*)b_arena.get(),
        (void*)core.JoinArena(o.object, o.part));
  Check("swap.buffer_not_reused", core.stats().prepared_built == built_a + 1 && core.stats().released >= 1 && released_hook >= 1,
        "a new buffer was built (%llu -> %llu) and the old one released (released %llu, hook %d)", (unsigned long long)built_a, (unsigned long long)core.stats().prepared_built,
        (unsigned long long)core.stats().released, released_hook);
  Check("swap.generation", core.ring_generation() >= 2 || opt.faults.arena_stale, "ring generation %u", core.ring_generation());

  a_arena.reset();   // the first arena's last owner but the core's old pairing: gone now (a dangling mesh pointer would be read below)
  core.BeginFrame(frame);
  ShadowCore::Decision d3 = core.Decide(in);
  Check("a_freed.still_ours", d3.ok, "ok %d with arena A released by its owner", d3.ok);

  current = nullptr;   // the ring hook has no arena (a load screen)
  core.BeginFrame(frame);
  ShadowCore::Decision d4 = core.Decide(in);
  Check("no_arena.not_replaced", !d4.ok && core.JoinArena(o.object, o.part) == nullptr, "ok %d, pairing %p", d4.ok, (void*)core.JoinArena(o.object, o.part));

  // lane 192: a part with SEVERAL index groups is drawn as one strip with a 0xFFFF between the groups (lane 179): the caster's index buffer must be that strip for every class; the wrestler class is
  // the one the plant flat_indices (the old plain sum) breaks. Synthetic part (groups 3 + 2 -> draw count 6); the real ch103 parts are multigroup_check's.
  {
    static const uint8_t g0[] = {0, 1, 0, 2, 0, 3}, g1[] = {0, 4, 0, 5};
    assets::jboy_parts::Part mp;
    mp.index_groups = 2, mp.index_count = 5, mp.groups = {{3, g0}, {2, g1}};
    const std::vector<uint16_t> want = {1, 2, 3, 0xFFFF, 4, 5};
    for (CasterClass c : {CasterClass::Wrestler, CasterClass::Ring, CasterClass::Proxy}) {
      const std::vector<uint16_t> got = ShadowCore::CasterIndices(mp, c, opt.faults);
      Check((std::string("multigroup.joined_") + CasterClassName(c)).c_str(), got == want, "%zu indices (the draw's count is %zu)", got.size(), want.size());
    }
    static const uint8_t s0[] = {0, 7, 0, 8, 0, 9};
    assets::jboy_parts::Part sp;
    sp.index_groups = 1, sp.index_count = 3, sp.groups = {{3, s0}};
    Check("multigroup.single_unchanged", ShadowCore::CasterIndices(sp, CasterClass::Wrestler, opt.faults) == std::vector<uint16_t>({7, 8, 9}), "a single-group part is its group");
  }

  std::printf("RESULT %s fault=%s failed=%d\n", g_fail ? "FAIL" : "OK", fault.empty() ? "none" : fault.c_str(), g_fail);
  return g_fail ? 1 : 0;
}
