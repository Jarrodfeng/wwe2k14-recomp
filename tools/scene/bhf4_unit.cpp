// bhf4_unit: offline proof of lane BHF4 (Session 103 overnight; docs\research\s103\BH4_integration.md, BH2_reuse.md). 0 game runs.
//  1. A record written before FXF (PostParamsRow 728 bytes) still reads: the post row arrives with fx[] zeroed, the model-runtime and input-stamp tables are kept.
//  2. A bad post row (unknown layout) drops ONLY post_params: model_runtime and input_stamps survive (counted in inputs_errors).
//  3. gsf::NoteStageOn forgets the previous arena's row on a stage change, and only then.
// Usage: bhf4_unit [--plant no_stage_reset|index_zero] [--expect-fail]  Exit 0 = proof holds (with --expect-fail: 0 when the plant is CAUGHT). Build: tools\scene\build_bhf4_unit.ps1
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/scene_live/gsf_latch.h"

using namespace rdr_scene;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  if (!ok) ++g_fail, std::printf("  FAIL %s\n", what);
}

static std::vector<uint8_t> Bhf4Record(uint32_t post_layout, bool shrink_post_to_v1) {
  SceneTables t;
  PostParamsRow p;
  p.layout = post_layout;
  p.frame = 77;
  p.s[3] = 0xABCD;
  for (auto& w : p.fx) w = 0xDEADBEEF;
  t.post_params.push_back(p);
  ModelRuntimeRow m;
  m.alpha = 0.5f;
  t.model_runtime.push_back(m);
  t.input_stamps.push_back(InputStampRow{});
  std::vector<uint8_t> b = SerializeRecord(t);
  if (shrink_post_to_v1) {   // an old writer: the post row is 728 bytes (the one row's first 728 bytes are the same bytes)
    SceneHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    for (uint32_t i = 0; i < h.section_count; ++i) {
      SectionEntry e;
      uint8_t* at = b.data() + sizeof(SceneHeader) + size_t(i) * sizeof(SectionEntry);
      std::memcpy(&e, at, sizeof e);
      if (e.id == kScPostParams) e.element_size = uint32_t(sizeof(PostParamsRow) - sizeof(PostParamsRow::fx)), std::memcpy(at, &e, sizeof e);
    }
  }
  return b;
}

int main(int argc, char** argv) {
  std::string plant;
  bool expect_fail = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) plant = argv[++i];
    else if (!std::strcmp(argv[i], "--expect-fail")) expect_fail = true;
  }
  std::string err;
  {   // 1: version 1 (728-byte) post row
    const auto b = Bhf4Record(kPostParamsLayout, true);
    SceneTables out;
    const bool ok = ParseRecord(b.data(), b.size(), out, err);
    Check(ok, "728-byte record parses");
    Check(out.post_params.size() == 1, "728-byte post row kept");
    if (out.post_params.size() == 1) {
      Check(out.post_params[0].frame == 77 && out.post_params[0].s[3] == 0xABCD, "728-byte row's words intact");
      bool z = true;
      for (auto w : out.post_params[0].fx) z = z && w == 0;
      Check(z, "728-byte row's fx[] zeroed");
    }
    Check(out.model_runtime.size() == 1 && out.input_stamps.size() == 1, "728-byte record keeps model_runtime and input_stamps");
    Check(out.inputs_errors == 0, "728-byte record counts no input error");
  }
  {   // 2: a bad post layout drops post_params only
    const auto b = Bhf4Record(99, false);
    SceneTables out;
    ParseRecord(b.data(), b.size(), out, err);
    Check(out.post_params.empty(), "bad post layout drops post_params");
    Check(out.model_runtime.size() == 1 && out.input_stamps.size() == 1, "bad post layout keeps model_runtime and input_stamps");
    Check(out.inputs_errors == 1, "bad post layout counted once");
  }
  {   // 3: the gGsf stage reset
    using namespace scene_inputs::gsf;
    State st;
    StageMark m;
    const uint32_t w[4] = {1, 2, 3, 4};
    st.arena_known = true, std::memcpy(st.arena, w, 16), std::memcpy(st.node, w, 16);
    Check(!NoteStageOn(st, m, false, 5), "unreadable stage changes nothing");
    Check(!NoteStageOn(st, m, true, 5) && st.arena_known, "first known stage only records it");
    Check(!NoteStageOn(st, m, true, 5) && st.arena_known, "same stage keeps the row");
    const bool changed = plant == "no_stage_reset" ? false : NoteStageOn(st, m, true, 9);
    Check(changed && !st.arena_known && st.arena[0] == 0 && st.node[3] == 0, "a stage change forgets the arena row");
    if (plant == "index_zero") st.index = 0;   // the first fix's sentinel: index 0 is a real moment tint (code 2)
    const uint32_t s208[4] = {0, 0, 0, 0};
    Check(Classify(st, true, s208).code == 0 && Classify(st, false, s208).code == 0, "Classify right after a stage change gives code 0 (no sepia, no tint)");
  }
  std::printf("bhf4_unit%s: %s\n", plant.empty() ? "" : (" (plant " + plant + ")").c_str(), g_fail ? "FAIL" : "PASS");
  const bool bad = g_fail != 0;
  return expect_fail ? (bad ? 0 : 1) : (bad ? 1 : 0);
}
