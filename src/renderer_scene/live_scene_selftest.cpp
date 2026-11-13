// live_scene_selftest.exe (Session 101, lane 26; tools\scene\live_writer_selftest.py drives it): the live scene writer with fake contributors and
// real sinks. No game, no GPU. Prints "OK name: detail" / "FAIL name: detail" lines, exit 0 only when every check passed.
//   live_scene_selftest --out DIR
// Writes under DIR: expected\frame_<guest>.rsc (what the contributors emitted, serialised by the test's own recording sink),
// capture\scene\frame_NNNNNN.rsc (the folder sink), cases\*.zip (the case ring), expected.txt (the zip's first guest frame, frame count).
//
// Checks: 1 off = free (10^6 EndFrame calls with no sink: time stated, nothing written); 2 two fake contributors (one with bones, one leaving
// fields unknown) x 300 frames into the folder sink + the case ring: 300 records read back byte-identical, census counts, the zip's scene part
// (python checks the bytes); 3 planted faults: a contributor that throws (std and non-std), one that appends a bone range outside the table,
// each refused loudly with the frame's record dropped and the game path unaffected; 4 a full queue drops and counts without blocking.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "../case/case_hook.h"
#include "../case/case_scene.h"
#include "../renderer_adapter/adapter_frame.h"
#include "live_scene.h"
#include "live_sinks.h"
#include "record_io.h"

using namespace rdr_scene;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

int g_fail = 0;
void Check(bool ok, const char* name, const std::string& detail) {
  std::printf("%s %s: %s\n", ok ? "OK" : "FAIL", name, detail.c_str());
  if (!ok) g_fail = 1;
}

std::mutex g_log_mu;
std::vector<std::string> g_log;
void LogLine(const char* line) {
  std::lock_guard<std::mutex> lock(g_log_mu);
  g_log.emplace_back(line);
  std::printf("LOG %s\n", line);
}
bool LogHas(const std::string& needle) {
  std::lock_guard<std::mutex> lock(g_log_mu);
  for (const std::string& l : g_log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

// Appends 3 skinned objects (4 bones each) and one asset per frame.
class FakeBones : public live::Contributor {
 public:
  const char* Name() const override { return "fake_bones"; }
  void Emit(const live::FrameInfo& f, SceneTables& t, live::Counters& c) override {
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = t.AddString("pac\\ch\\ch001.pac");
    a.entry = t.AddString("attire");
    const uint32_t asset = uint32_t(t.assets.size());
    t.assets.push_back(a);
    for (uint32_t k = 0; k < 3; ++k) {
      Object o;
      o.asset = asset;
      o.flags = kObjVisible | kObjSkinned;
      o.id = t.AddString("actor:slot:" + std::to_string(k));
      o.world = IdentityMat4();
      o.world.m[12] = float(f.guest_frame % 1000) + float(k);
      o.bones_first = uint32_t(t.bones.size());
      o.bones_count = 4;
      for (int b = 0; b < 4; ++b) {
        Mat4 m = IdentityMat4();
        m.m[13] = float(b) + float(f.guest_frame % 7);
        t.bones.push_back(m);
      }
      ParamValue p;
      p.name = t.AddString("g_SweatAmount");
      p.value[0] = float(k) * 0.25f;
      o.params_first = uint32_t(t.params.size());
      o.params_count = 1;
      t.params.push_back(p);
      t.objects.push_back(o);
      ++c.objects;
    }
  }
};

// Appends 2 objects that leave their mesh mask and light set unknown (the defaults 0xFFFFFFFF) and counts them as unknown.
class FakeUnknown : public live::Contributor {
 public:
  const char* Name() const override { return "fake_unknown"; }
  void Emit(const live::FrameInfo& f, SceneTables& t, live::Counters& c) override {
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    a.pac = t.AddString("pac\\bg\\bg01.pac");
    const uint32_t asset = uint32_t(t.assets.size());
    t.assets.push_back(a);
    for (uint32_t k = 0; k < 2; ++k) {
      Object o;
      o.asset = asset;
      o.id = t.AddString("crowd:block:3:member:" + std::to_string(k));
      o.world = IdentityMat4();
      o.world.m[12] = float(f.guest_frame % 13);
      t.objects.push_back(o);
      ++c.objects;
      c.unknown += 2;  // mesh mask, light set
      ++mesh_mask_, ++light_set_;
    }
  }
  // S101 lane 264: the breakdown of fields_unknown a real contributor appends (arena: look_unknown_slots ...; wrestlers: no_pac ...); reset per window.
  void Census(std::string& out) override {
    out += "[mesh_mask " + std::to_string(mesh_mask_) + ", light_set " + std::to_string(light_set_) + "]";
    mesh_mask_ = light_set_ = 0;
  }

 private:
  uint64_t mesh_mask_ = 0, light_set_ = 0;
};

// Planted fault: does nothing until a mode is set.
class Planted : public live::Contributor {
 public:
  enum Mode { None = 0, ThrowStd, ThrowOther, BadBones };
  std::atomic<int> mode{None};
  const char* Name() const override { return "planted"; }
  void Emit(const live::FrameInfo&, SceneTables& t, live::Counters& c) override {
    switch (mode.load()) {
      case ThrowStd: throw std::runtime_error("planted std exception");
      case ThrowOther: throw 42;
      case BadBones: {
        AssetRef a;
        a.pac = t.AddString("pac\\ch\\ch002.pac");
        t.assets.push_back(a);
        Object o;
        o.asset = uint32_t(t.assets.size() - 1);
        o.id = t.AddString("actor:slot:9");
        o.bones_first = 100;  // the palette holds far fewer
        o.bones_count = 50;
        t.objects.push_back(o);
        ++c.objects;
        break;
      }
      default: break;
    }
  }
};

// The test's own ground truth: serialises every record it is handed (the bytes the writer's sinks must reproduce).
class Recorder : public live::Sink {
 public:
  std::map<uint64_t, std::vector<uint8_t>> by_guest_frame;
  uint64_t calls = 0;
  void OnRecord(const live::FrameInfo& f, const SceneTables& t) override {
    ++calls;
    by_guest_frame[f.guest_frame] = SerializeRecord(t);
  }
};

bool ReadAll(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const std::streamoff n = f.tellg();
  out.resize(size_t(n));
  f.seekg(0);
  if (n) f.read(reinterpret_cast<char*>(out.data()), n);
  return bool(f);
}

}  // namespace

int main(int argc, char** argv) {
  fs::path out;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--out") == 0) out = argv[i + 1];
  if (out.empty()) {
    std::fprintf(stderr, "usage: live_scene_selftest --out DIR\n");
    return 2;
  }
  std::error_code ec;
  fs::remove_all(out, ec);
  fs::create_directories(out, ec);

  live::SetLog(&LogLine);
  static FakeBones bones;
  static FakeUnknown unknown;
  static Planted planted;
  live::Register(&bones);
  live::Register(&unknown);
  live::Register(&planted);

  // ---- 1. off = free ----
  {
    const int64_t n = 1000000;
    uint64_t sink = 0;
    const auto t0 = Clock::now();
    for (int64_t i = 0; i < n; ++i) {
      live::EndFrame(uint64_t(i));
      sink += live::Enabled() ? 1 : 0;
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / double(n);
    const live::Totals t = live::GetTotals();
    Check(ns < 100.0 && t.frames == 0 && t.records == 0 && sink == 0, "off_is_free",
          std::to_string(n) + " EndFrame+Enabled calls with no sink: " + std::to_string(ns) + " ns a call (limit 100), frames built " + std::to_string(t.frames) +
              ", records " + std::to_string(t.records));
    Check(!fs::exists(out / "capture"), "off_writes_nothing", "no capture folder was created");
  }

  // ---- 2. 300 frames into the folder sink + the case ring ----
  Recorder truth;
  live::FolderSink folder(512);
  {
    std::string err;
    Check(folder.Begin((out / "capture").string(), err), "folder_begin", err.empty() ? "scene folder created" : err);

    case_hook::Config cfg;
    cfg.enabled = true;
    cfg.window_s = 2.0;
    cfg.cap_mb = 512;
    cfg.dir = out / "cases";
    cfg.log = [](const std::string& line) { LogLine(line.c_str()); };
    case_hook::Configure(cfg);
    rdr_case::AttachSceneSink();
    live::AddSink(&truth);
    live::AddSink(&folder);
    Check(live::Enabled(), "enabled_with_sinks", "Enabled() is true with 3 sinks attached");

    const uint64_t first_guest = 1000;
    for (uint32_t i = 0; i < 300; ++i) {
      const uint64_t guest = first_guest + i;
      live::EndFrame(guest, i);  // the game's order: the record at the boundary, then the frame reaches the ring
      rdr_adapter::FrameFileHeader h;
      h.capture_frame = i;
      h.guest_frame = guest;
      case_hook::OnFrameAt(&h, sizeof(h), guest, double(i) / 60.0);
    }
    live::CensusLog(first_guest, first_guest + 300);
    live::RemoveSink(&folder);
    live::RemoveSink(&truth);
    folder.End();

    const live::Totals t = live::GetTotals();
    Check(t.records == 300 && t.refused == 0 && truth.calls == 300, "records_built", "records " + std::to_string(t.records) + ", refused " +
                                                                                       std::to_string(t.refused) + ", truth sink calls " +
                                                                                       std::to_string(truth.calls));
    Check(folder.written() == 300 && folder.dropped() == 0 && folder.failed() == 0, "folder_written",
          "written " + std::to_string(folder.written()) + ", dropped " + std::to_string(folder.dropped()) + ", failed " + std::to_string(folder.failed()));

    // each file: byte-identical to what was emitted, and ParseRecord -> Serialize gives the same bytes
    int identical = 0, reparse = 0, missing = 0;
    for (uint32_t i = 0; i < 300; ++i) {
      char name[48];
      std::snprintf(name, sizeof(name), "frame_%06u.rsc", i);
      std::vector<uint8_t> file;
      if (!ReadAll(out / "capture" / "scene" / name, file)) {
        ++missing;
        continue;
      }
      if (file == truth.by_guest_frame[first_guest + i]) ++identical;
      SceneTables parsed;
      std::string err2;
      if (ParseRecord(file.data(), file.size(), parsed, err2) && SerializeRecord(parsed) == file) ++reparse;
    }
    Check(identical == 300 && reparse == 300 && missing == 0, "folder_bytes_identical",
          std::to_string(identical) + " of 300 files byte-identical to the emitted record, " + std::to_string(reparse) + " re-serialise to themselves, " +
              std::to_string(missing) + " missing");
    // the expected bytes for the python side (the zip's scene part)
    fs::create_directories(out / "expected", ec);
    for (const auto& [g, b] : truth.by_guest_frame) {
      std::ofstream f(out / "expected" / ("frame_" + std::to_string(g) + ".rsc"), std::ios::binary);
      f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    }

    // census counts: 300 frames x 3 objects, 300 x 2 objects and 4 unknown fields
    Check(LogHas("fake_bones objects 900, fields_unknown 0") && LogHas("fake_unknown objects 600, fields_unknown 1200") && LogHas("records 300 of 300 frames") &&
              LogHas("folder accepted 300, written") && LogHas(", dropped 0, failed 0"),
          "census_counts", "the [scene][live] line carries objects 900 / 600, fields_unknown 0 / 1200, records 300 of 300, folder accepted 300 dropped 0");
    Check(LogHas("fake_unknown objects 600, fields_unknown 1200 [mesh_mask 600, light_set 600]") && !LogHas(", unknown "), "census_breakdown",
          "the line says fields_unknown (never the old 'unknown') and the contributor's own breakdown follows it, summing to the same 1200");
    Check(LogHas("[scene][live] frames 1000-1300:"), "census_line", "the line is the [scene][live] frames A-B form");

    // the case zip: the ring's window (2 s at 60 fps)
    case_hook::Context ctx;
    ctx.manifest.build.git = "selftest";
    ctx.manifest.build.sdk_pin = "none";
    ctx.manifest.build.kind = "dev";
    ctx.manifest.device.platform = "windows";
    ctx.manifest.device.gpu = "selftest (no GPU)";
    ctx.manifest.where.words = "live writer selftest";
    ctx.log_text = "selftest\n";
    ctx.states_text = "selftest\n";
    case_hook::Trigger(rdr_case::Trigger::Key, "live writer selftest", std::move(ctx));
    rdr_case::WaitIdle();
    const rdr_case::CaseResult r = case_hook::LastResult();
    const rdr_case::RingStats rs = case_hook::Stats();
    Check(r.ok && r.scene_records > 0 && r.scene_records == r.frames && r.scene_unmatched == 0, "zip_scene_part",
          std::string(r.ok ? "zip written" : "zip FAILED: " + r.error) + ": " + std::to_string(r.frames) + " frames, " + std::to_string(r.scene_records) +
              " scene records, unmatched " + std::to_string(r.scene_unmatched) + "; ring holds " + std::to_string(rs.scene_records) + " records, dropped " +
              std::to_string(rs.scene_dropped));
    if (r.ok) {
      std::ofstream f(out / "expected.txt");
      f << r.path.string() << "\n" << (300 - r.frames + first_guest) << "\n" << r.frames << "\n" << r.scene_records << "\n";
    }
    rdr_case::DetachSceneSink();
    case_hook::Shutdown();
  }

  // ---- 3. planted faults ----
  {
    Recorder rec;
    live::AddSink(&rec);
    const live::Totals before = live::GetTotals();
    planted.mode = Planted::ThrowStd;
    live::EndFrame(5000);
    planted.mode = Planted::ThrowOther;
    live::EndFrame(5001);
    planted.mode = Planted::BadBones;
    live::EndFrame(5002);
    planted.mode = Planted::None;
    live::EndFrame(5003);
    live::RemoveSink(&rec);
    const live::Totals after = live::GetTotals();
    Check(after.refused - before.refused == 3 && rec.by_guest_frame.size() == 1 && rec.by_guest_frame.count(5003), "faults_refused",
          "3 planted frames refused (throw, non-std throw, bone range outside the table), " + std::to_string(rec.by_guest_frame.size()) +
              " record reached the sink (the clean frame 5003); EndFrame returned normally each time");
    Check(LogHas("contributor \"planted\" threw") && LogHas("failed validation") && LogHas("bones 100+50 past"), "faults_loud",
          "the refusals were logged with the contributor / the check that failed");
    live::CensusLog(5000, 5004);
    Check(LogHas("REFUSED 2") && LogHas("refused 3"), "faults_counted", "the census line counts the refusals (contributor REFUSED 2, frames refused 3)");
  }

  // ---- 4. a full queue drops and counts ----
  {
    live::FolderSink small(8);
    std::string err;
    small.Begin((out / "full").string(), err);
    small.PauseWriterForTest(true);
    live::AddSink(&small);
    const auto t0 = Clock::now();
    for (uint32_t i = 0; i < 20; ++i) live::EndFrame(9000 + i, i);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    live::RemoveSink(&small);
    const uint64_t dropped = small.dropped();
    small.End();  // lets the writer drain what it holds
    Check(dropped == 12 && small.written() == 8 && ms < 1000.0, "full_queue_drops",
          "20 records into a queue of 8 with the writer held: dropped " + std::to_string(dropped) + " (12 expected), after release written " +
              std::to_string(small.written()) + " (8 expected); the 20 EndFrame calls took " + std::to_string(ms) + " ms (never blocked)");
    int files = 0;
    for (const auto& e : fs::directory_iterator(out / "full" / "scene", ec)) {
      (void)e;
      ++files;
    }
    Check(files == 8, "full_queue_files", std::to_string(files) + " files on disk (8 expected)");
  }

  // ---- 5. review fix 2: a record with no capture frame number is dropped and counted, never numbered by arrival ----
  {
    live::FolderSink cap(8);
    std::string err;
    cap.Begin((out / "unnumbered").string(), err);
    live::AddSink(&cap);
    live::EndFrame(9500);  // the capture stopped mid-frame: no frame number
    live::EndFrame(9501, 7);
    live::RemoveSink(&cap);
    cap.End();
    int files = 0;
    const bool seven = fs::exists(out / "unnumbered" / "scene" / "frame_000007.rsc"), zero = fs::exists(out / "unnumbered" / "scene" / "frame_000000.rsc");
    for (const auto& e : fs::directory_iterator(out / "unnumbered" / "scene", ec)) {
      (void)e;
      ++files;
    }
    Check(cap.dropped() == 1 && cap.written() == 1 && files == 1 && seven && !zero, "unnumbered_dropped",
          "a record without a capture frame number: dropped " + std::to_string(cap.dropped()) + ", written " + std::to_string(cap.written()) +
              "; frame_000007 present, no frame_000000 (it would have named a later real frame's file)");
  }

  // ---- 6. review fix 3: a failed write never touches the file under the real name ----
  {
    fs::create_directories(out / "atomic", ec);
    const std::string path = (out / "atomic" / "frame_000001.rsc").string();
    SceneTables good;
    good.header.guest_frame = 1;
    std::string err, err2;
    const bool first = WriteRecord(path, good, err);
    std::vector<uint8_t> before;
    ReadAll(path, before);
    fs::create_directories(path + ".tmp", ec);  // the temp name is a directory: the write cannot be made
    SceneTables other;
    other.header.guest_frame = 2;
    const bool second = WriteRecord(path, other, err2);
    std::vector<uint8_t> after;
    ReadAll(path, after);
    fs::remove_all(path + ".tmp", ec);
    Check(first && !second && !before.empty() && before == after && !err2.empty(), "write_is_atomic",
          "a write that cannot be made fails (" + err2 + ") and leaves the earlier record intact (" + std::to_string(after.size()) + " bytes)");
    SceneTables third;
    third.header.guest_frame = 3;
    const bool ok = WriteRecord(path, third, err);
    Check(ok && !fs::exists(path + ".tmp"), "write_replaces", "a good write replaces the record and leaves no .tmp");
  }

  // ---- 7. review fix 4: two records for one guest frame: the zip carries the newest (python checks the bytes) ----
  {
    case_hook::Config cfg;
    cfg.enabled = true;
    cfg.window_s = 2.0;
    cfg.cap_mb = 64;
    cfg.dir = out / "cases2";
    cfg.log = [](const std::string& line) { LogLine(line.c_str()); };
    case_hook::Configure(cfg);
    rdr_adapter::FrameFileHeader h;
    h.guest_frame = 7000;
    case_hook::OnScene(7000, 3, [] { return std::vector<uint8_t>{'A', 'A', 'A'}; });
    case_hook::OnScene(7000, 3, [] { return std::vector<uint8_t>{'B', 'B', 'B'}; });
    case_hook::OnFrameAt(&h, sizeof(h), 7000, 0.0);
    case_hook::Context ctx;
    ctx.manifest.build.git = "selftest";
    ctx.manifest.build.sdk_pin = "none";
    ctx.manifest.build.kind = "dev";
    ctx.manifest.device.platform = "windows";
    ctx.manifest.device.gpu = "selftest (no GPU)";
    ctx.manifest.where.words = "newest record wins";
    ctx.log_text = "selftest";
    ctx.states_text = "selftest";
    case_hook::Trigger(rdr_case::Trigger::Key, "newest wins", std::move(ctx));
    rdr_case::WaitIdle();
    const rdr_case::CaseResult r = case_hook::LastResult();
    Check(r.ok && r.scene_records == 1 && r.scene_unmatched == 1, "newest_record_wins_counted",
          "two records for guest frame 7000: " + std::to_string(r.scene_records) + " written, " + std::to_string(r.scene_unmatched) + " left out");
    if (r.ok) {
      std::ofstream f(out / "expected2.txt");
      f << r.path.string() << "\n";
    }
    case_hook::Shutdown();
  }

  std::printf("%s\n", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED");
  return g_fail;
}
