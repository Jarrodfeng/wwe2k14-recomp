// S103 SVO: the guest-thread stamp of goto's stage push, offline (0 game runs). Built and run by tools\scene\svo_check.py.
//
// Production order driven (live_scene.cpp BuildFrame, called by BOTH frame-boundary paths): the director says "playing" (stage_view::SetPlaying, scene_director.h) -> the guest thread's frame boundary
// builds the record (live::EndFrame, or live::LatchFrame + EmitLatched as the game's swap thread does with the tail release on) -> the sink gets it. The stamp must be in the record the sink sees, taken at
// BUILD time: flipping the live flag after the record was built must not change it (the host reads the stamp, never the flag).
//   1. switch on, playing:      every record carries kSceneStagePlaying (EndFrame and LatchFrame + EmitLatched).
//   2. switch on, not playing:  none does; the bit is cleared again on the frame after a playing one (a reused pooled table never keeps it).
//   3. switch OFF, playing:     none does (the default: players never see it).
//   4. the flag flipped AFTER the build does not change a record already made.
//   5. a stamped record survives SerializeRecord / ParseRecord (a case zip / capture keeps the bit).
//   Planted twin (--plant nostamp): the switch never reaches the stamp (g_enabled stays false, as when the host start forgets to store it): the "is stamped" checks must FAIL.
// Usage: stage_stamp_unit [--plant nostamp]    Exit 0 = every check passed (the planted twin was caught), 1 = a failure.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/stage_view.h"
#include "../../src/renderer_scene/live_scene.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/scene_record.h"

namespace live = rdr_scene::live;
namespace sv = rdr_adapter::stage_view;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

struct Empty final : live::Contributor {
  const char* Name() const override { return "svo_empty"; }
  void Emit(const live::FrameInfo&, rdr_scene::SceneTables&, live::Counters&) override {}
  bool Latch(const live::FrameInfo&) override { return true; }   // lets LatchFrame defer the emit, as the real contributors do
};

struct Sink final : live::Sink {
  std::vector<uint32_t> flags;
  std::vector<uint8_t> last_bytes;
  void OnRecord(const live::FrameInfo&, const rdr_scene::SceneTables& t) override {
    flags.push_back(t.header.flags);
    last_bytes = rdr_scene::SerializeRecord(t);
  }
};

Empty g_c;
Sink g_sink;
uint64_t g_frame = 100;

// One frame as the game runs it: the director's flag, then the frame boundary on the guest thread. `via_latch` = the swap thread's two halves.
uint32_t Frame(bool via_latch) {
  const size_t before = g_sink.flags.size();
  ++g_frame;
  if (via_latch) {
    live::Latched l;
    live::LatchFrame(g_frame, live::kNoCaptureFrame, true, l);
    live::EmitLatched(l);
  } else {
    live::EndFrame(g_frame);
  }
  return g_sink.flags.size() == before + 1 ? g_sink.flags.back() : 0xFFFFFFFFu;
}
}  // namespace

int main(int argc, char** argv) {
  const bool plant_nostamp = argc > 2 && std::strcmp(argv[1], "--plant") == 0 && std::strcmp(argv[2], "nostamp") == 0;   // twin: the switch never reaches the stamp (the host start forgot g_enabled): the stamped-record checks must FAIL
  live::Register(&g_c);
  live::AddSink(&g_sink);
  const uint32_t bit = rdr_scene::kSceneStagePlaying;

  for (int via_latch = 0; via_latch < 2; ++via_latch) {
    const std::string path = via_latch ? "LatchFrame + EmitLatched" : "EndFrame";
    sv::g_host_enabled.store(!plant_nostamp);
    sv::SetPlaying(true);
    uint32_t f = Frame(via_latch != 0);
    Check(f != 0xFFFFFFFFu && (f & bit), (path + ": switch on, stage push playing: the record is stamped").c_str());
    f = Frame(via_latch != 0);
    Check(f != 0xFFFFFFFFu && (f & bit), (path + ": still playing: the next record is stamped too").c_str());

    sv::SetPlaying(false);
    f = Frame(via_latch != 0);
    Check(f != 0xFFFFFFFFu && !(f & bit), (path + ": the push finished: the stamp is gone from the next record (a reused table keeps nothing)").c_str());

    sv::g_host_enabled.store(false);
    sv::SetPlaying(true);
    f = Frame(via_latch != 0);
    Check(f != 0xFFFFFFFFu && !(f & bit), (path + ": switch OFF (the default), push playing: no stamp").c_str());
    sv::g_enabled = true;   // an ON-build test game (reader.h sets g_enabled from wwe2k14_stage_view): the host switch is off, so its records never carry the bit
    f = Frame(via_latch != 0);
    Check(f != 0xFFFFFFFFu && !(f & bit), (path + ": the stream build's g_enabled on, host switch off: no stamp").c_str());
    sv::g_enabled = false;

    // the flag flips AFTER the build: the record already in the sink keeps what it was made with
    sv::g_host_enabled.store(!plant_nostamp);
    sv::SetPlaying(true);
    f = Frame(via_latch != 0);
    const bool made_stamped = (f & bit) != 0;
    sv::SetPlaying(false);
    Check(made_stamped && (g_sink.flags.back() & bit), (path + ": the live flag cleared after the build does not change the made record").c_str());
    sv::SetPlaying(false);
    sv::g_host_enabled.store(false);
  }

  // the bit survives the file form (a captured / case-zip record replays the same)
  sv::g_host_enabled.store(!plant_nostamp);
  sv::SetPlaying(true);
  Frame(false);
  rdr_scene::SceneTables back;
  std::string err;
  const bool parsed = rdr_scene::ParseRecord(g_sink.last_bytes.data(), g_sink.last_bytes.size(), back, err);
  Check(parsed && (back.header.flags & bit), "a stamped record keeps the bit through SerializeRecord / ParseRecord");
  sv::SetPlaying(false);
  sv::g_host_enabled.store(false);

  std::printf("stage_stamp_unit: %s\n", g_fail ? "FAIL" : "all passed");
  return g_fail ? 1 : 0;
}
