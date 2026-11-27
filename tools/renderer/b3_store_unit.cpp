// S105 B3 unit: the stream-free host's pipeline store wiring (0 game runs, no GPU, no SDK).
// entry: wwe2k14::scene_host::RegisterShaderCatalogFrom, wwe2k14::scene_host::store_progress::PrebuildAllowed, wwe2k14::scene_host::store_progress::OwnerPrecompileDone, rdr::pstore::WriteFileAtomicWhy
//   - catalog: a stream-free host registers every variant's spirv and its alpha twin AND every own shader's words (S110 RDRPREP) with the renderer, in ONE call, once the store is open; with the store off it registers nothing (as Binder did).
//   - gate: the store builds in slices in menus, never while a 3D scene is the frame's source; the precompile boot always may.
//   - precompile rule: done at frame >= 120 only when the store has nothing left (never "0 of 0" with records waiting, never before the store published).
//   - write failure: the log names the step and the OS error.
// Usage: b3_store_unit [plant N]   (plants: 1 catalog never registered, 2 precompile done at frame 120 with records waiting, 3 a build during a 3D scene, 4 a write failure with no reason)
// Exit 0 = every check passed, 1 = a check failed (its name is on the FAIL line).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../src/renderer/pipeline_store.h"
#include "../../src/renderer_host/pipeline_catalog.h"
#include "../../src/renderer_host/store_progress.h"

namespace sp = wwe2k14::scene_host::store_progress;
namespace sh = wwe2k14::scene_host;

static int g_fail = 0;
static void Check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) ++g_fail;
}

struct FakeRenderer {
  struct ShaderBlobDesc {
    const char* name;
    const uint32_t* words;
    size_t bytes;
  };
  bool active = true;
  std::vector<ShaderBlobDesc> got;
  int calls = 0;
  bool pipeline_store_active() const { return active; }
  void RegisterShaderCatalog(const ShaderBlobDesc* b, size_t n) { ++calls, got.assign(b, b + n); }
};

struct FakeVariant {   // rdr::fx::Variant's catalog fields (S111 TOOLLINK: no word address; the words come from FakeWords by row index, as rdr::fx::Words)
  const char* name;
  size_t bytes;
  size_t bytes_alpha;
};

struct FakeOwn {   // rdr::fx::OwnShader
  const char* name;
  const uint32_t* words;
  size_t bytes;
};

static const uint32_t kW1[4] = {1, 2, 3, 4}, kW2[4] = {5, 6, 7, 8}, kW3[4] = {9, 10, 11, 12}, kW4[4] = {13, 14, 15, 16}, kW5[4] = {17, 18, 19, 20};
static const FakeVariant kFake[] = {{"vs_a", 16, 0}, {"ps_a", 16, 16}, {"vs_b", 16, 0}};
static const uint32_t* const kFakeWords[][2] = {{kW1, nullptr}, {kW2, kW3}, {kW3, nullptr}};
static const uint32_t* FakeWords(size_t i, bool alpha) { return kFakeWords[i][alpha ? 1 : 0]; }
static const FakeOwn kFakeOwn[] = {{"rdr_own_vert", kW4, 16}, {"rdr_own_frag", kW5, 16}};

// What SceneHost::Start does after Renderer::Create (the wiring lint in b3_store_unit.py checks the real call is there). `own_range` false = the old variants-only walk (plant 6).
static size_t FakeStart(FakeRenderer& r, bool call_register, bool own_range = true) {
  if (!call_register) return 0;
  return sh::RegisterShaderCatalogFrom(r, std::begin(kFake), std::end(kFake), FakeWords, std::begin(kFakeOwn), own_range ? std::end(kFakeOwn) : std::begin(kFakeOwn));
}

int main(int argc, char** argv) {
  int plant = (argc >= 3 && std::strcmp(argv[1], "plant") == 0) ? std::atoi(argv[2]) : 0;

  {   // catalog
    FakeRenderer r;
    const size_t n = FakeStart(r, plant != 1, plant != 6);
    Check(n == 6 && r.calls == 1 && r.got.size() == 6, "catalog.registered_once_with_alpha_twins");
    Check(r.got.size() == 6 && r.got[2].words == kW3 && r.got[1].words == kW2, "catalog.blob_words_are_the_variants");
    Check(r.got.size() == 6 && r.got[4].words == kW4 && r.got[5].words == kW5, "catalog.own_blobs_in_the_same_call");
    FakeRenderer off;
    off.active = false;
    Check(FakeStart(off, true) == 0 && off.calls == 0, "catalog.store_off_registers_nothing");
  }

  {   // gate
    auto gate = [&](bool precompile, bool record, bool draw3d) { return plant == 3 ? true : sp::PrebuildAllowed(precompile, record, draw3d); };
    // (record, black_source): a record whose source is not Black (Draw3D, Hold3D, HoldF) shows a scene
    Check(!gate(false, true, false), "gate.no_slice_in_a_3d_scene");
    Check(gate(false, true, true), "gate.black_frame_with_a_record_may_slice");
    Check(gate(false, false, false), "gate.no_record_may_slice");
    Check(gate(true, true, false), "gate.precompile_boot_builds_in_any_scene");
  }

  {   // precompile rule
    auto rule = [&](uint64_t frame, bool expected, bool published, uint32_t left) {
      if (plant == 2) return frame >= 120;   // the old rule: done at frame 120, whatever the store holds
      return sp::OwnerPrecompileDone(frame, expected, published, left);
    };
    Check(!rule(100, true, true, 0), "rule.not_before_frame_120");
    Check(!rule(120, true, true, 17), "rule.not_done_with_records_waiting");
    Check(!rule(120, true, false, 0), "rule.not_done_before_the_store_published");
    Check(rule(120, true, true, 0), "rule.done_when_nothing_is_left");
    Check(rule(120, false, false, 0), "rule.no_store_done_at_120");
    Check(!rule(60, false, false, 0), "rule.no_store_not_before_120");
  }

  {   // flush before done (F1)
    sp::Progress p;
    p.store_expected.store(false);
    Check(sp::RequestFlushAndWait(p, 10) && p.flush_request.load() == 0, "flush.no_store_nothing_to_save");
    p.store_expected.store(true);
    int saved = 0;
    std::atomic<bool> stop{false};
    std::thread host([&] { while (!stop.load()) { if (plant != 5) sp::ServeFlush(p, [&] { ++saved; }); std::this_thread::sleep_for(std::chrono::milliseconds(2)); } });
    const bool acked = sp::RequestFlushAndWait(p, 1000);
    const int saved_at_ack = saved;   // the save has run by the time the caller may write "done"
    stop.store(true);
    host.join();
    Check(acked && saved_at_ack == 1, "flush.saved_before_the_ack");
  }

  {   // write failure
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "b3_store_unit";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::vector<uint8_t> bytes = {1, 2, 3};
    std::string why = "stale";
    Check(rdr::pstore::WriteFileAtomicWhy(dir / "ok.bin", bytes, why) && why.empty(), "write.success_has_no_reason");
    { std::ofstream f(dir / "afile"); f << "x"; }
    std::string bad_why;
    const bool wrote = rdr::pstore::WriteFileAtomicWhy(dir / "afile" / "pipelines.bin", bytes, bad_why);   // the "folder" is a file: the temp file cannot be opened
    if (plant == 4) bad_why.clear();
    Check(!wrote, "write.failure_returns_false");
    Check(bad_why.find("open the temp file") != std::string::npos && bad_why.find("errno") != std::string::npos, "write.failure_names_the_step_and_the_os_error");
    // the rename step: the target is a directory
    fs::create_directories(dir / "adir", ec);
    std::string ren_why;
    const bool r2 = rdr::pstore::WriteFileAtomicWhy(dir / "adir", bytes, ren_why);
    Check(!r2 && ren_why.find("rename the temp file") != std::string::npos, "write.rename_failure_names_the_step");
    Check(!rdr::pstore::WriteFileAtomic(dir / "afile" / "x.bin", bytes), "write.old_signature_still_false");
    fs::remove_all(dir, ec);
  }

  std::printf("%s\n", g_fail ? "b3_store_unit: FAILED" : "b3_store_unit: every check passed");
  return g_fail ? 1 : 0;
}
