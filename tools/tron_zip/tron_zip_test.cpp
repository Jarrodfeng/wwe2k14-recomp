// Session 98: a PC-side check of src\tron_zip.h (the PC launcher's EXPORT FOR PHONE and the phone launcher's IMPORT TRON PREVIEWS).
//   tron_zip_test CACHE WORKDIR
// 1. exports CACHE (a tron cache: the launcher's %LOCALAPPDATA%\wwe2k14-launcher\trons, or a fake one) to WORKDIR\export.zip;
// 2. imports it into WORKDIR\files\trons twice (the second time over the first: the swap) and compares every file with CACHE;
// 3. refusals: a zip without the manifest, a zip-slip member, a member with a bad CRC, a newer format, another title id: each must
//    fail and leave the imported folder as it was; a zip with an unexpected member must import and skip it.
// Built by tools\tron_zip\build_test.ps1. Prints PASS / FAIL lines; exit code 0 = all passed.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "../../src/tron_zip.h"

namespace fs = std::filesystem;

static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}

static bool SameFile(const fs::path& a, const fs::path& b) {
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  if (!fa || !fb) return false;
  std::vector<char> ba(1 << 20), bb(1 << 20);
  for (;;) {
    fa.read(ba.data(), std::streamsize(ba.size()));
    fb.read(bb.data(), std::streamsize(bb.size()));
    if (fa.gcount() != fb.gcount()) return false;
    if (fa.gcount() == 0) return true;
    if (std::memcmp(ba.data(), bb.data(), size_t(fa.gcount())) != 0) return false;
  }
}

// A small zip from memory entries (name -> data); the manifest given as text ("" = none).
static void MakeZip(const fs::path& zip, const std::string& manifest, const std::vector<std::pair<std::string, std::string>>& entries) {
  mlauncher::zip::Writer w(zip);
  if (!manifest.empty()) w.AddText(tron_zip::kManifest, manifest);
  for (const auto& [n, d] : entries) w.AddText(n, d);
  w.Finish();
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--import") {  // tron_zip_test --import ZIP TRONS: the import alone (any zip)
    tron_zip::ImportResult r;
    const std::string err = tron_zip::Import(argv[2], fs::u8path(argv[3]), r);
    std::printf("%s: %d files, %d videos, %d pictures, skipped %d\n", err.empty() ? "imported" : err.c_str(), r.counts.files,
                r.counts.videos, r.counts.pictures, r.skipped);
    return err.empty() ? 0 : 1;
  }
  if (argc < 3) {
    std::printf("usage: tron_zip_test CACHE WORKDIR | tron_zip_test --import ZIP TRONS\n");
    return 2;
  }
  const fs::path cache = fs::u8path(argv[1]), work = fs::u8path(argv[2]);
  std::error_code ec;
  fs::create_directories(work, ec);
  const fs::path zip = work / "export.zip", trons = work / "files" / "trons";
  fs::remove_all(work / "files", ec);

  // 1. export
  tron_zip::Counts c;
  tron_zip::Progress prog;
  auto t0 = std::chrono::steady_clock::now();
  std::string err = tron_zip::Export(cache, zip, "pc", "tron_zip_test", c, &prog);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  Check(err.empty(), "export: " + (err.empty() ? std::string("ok") : err));
  std::printf("      export: %d files, %d videos, %d pictures, %.1f MB in %lld ms; zip %.1f MB; progress %llu / %llu\n", c.files,
              c.videos, c.pictures, c.bytes / 1048576.0, (long long)ms, fs::file_size(zip, ec) / 1048576.0,
              (unsigned long long)prog.done.load(), (unsigned long long)prog.total.load());
  Check(prog.done == prog.total && prog.total == c.bytes, "export progress reaches the total");
  Check(!fs::exists(fs::path(zip.string() + ".part"), ec), "export leaves no .part");
  if (!err.empty()) return 1;

  // 2. import (twice: the second replaces the first through the swap)
  for (int pass = 1; pass <= 2; ++pass) {
    tron_zip::ImportResult r;
    tron_zip::Progress ip;
    t0 = std::chrono::steady_clock::now();
    err = tron_zip::Import(tron_zip::U8(zip), trons, r, &ip);
    ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    Check(err.empty(), "import pass " + std::to_string(pass) + ": " + (err.empty() ? std::string("ok") : err));
    std::printf("      import: %d files, %d videos, %d pictures, %.1f MB in %lld ms; source %s, created %s, skipped %d\n",
                r.counts.files, r.counts.videos, r.counts.pictures, r.counts.bytes / 1048576.0, (long long)ms, r.source.c_str(),
                r.created.c_str(), r.skipped);
    Check(r.counts.files == c.files && r.counts.videos == c.videos && r.counts.pictures == c.pictures && r.counts.bytes == c.bytes,
          "import counts match the export");
    Check(ip.done == ip.total && ip.total == c.bytes, "import progress reaches the total");
    Check(!fs::exists(fs::path(trons.string() + ".import"), ec) && !fs::exists(fs::path(trons.string() + ".old"), ec),
          "no .import / .old folder left");
  }
  // every exported file is there, byte for byte, and nothing else
  tron_zip::Counts c2;
  const auto want = tron_zip::ListCache(cache, c);
  const auto got = tron_zip::ListCache(trons, c2);
  int diff = 0;
  for (const auto& [rel, sz] : want)
    if (!SameFile(cache / fs::u8path(rel), trons / fs::u8path(rel))) ++diff;
  size_t all = 0;
  for (fs::recursive_directory_iterator it(trons, ec), end; it != end; it.increment(ec))
    if (it->is_regular_file()) ++all;
  Check(diff == 0 && got.size() == want.size() && all == want.size(),
        "imported files identical to the cache (" + std::to_string(want.size()) + " files, " + std::to_string(diff) +
            " different, " + std::to_string(all) + " in the folder)");

  // 3. refusals (each must leave the imported folder as it was)
  const std::string good_man = std::string("{\"kind\":\"") + tron_zip::kKind + "\",\"format\":1,\"source\":\"pc\",\"title_id\":\"545408B2\"}";
  const std::vector<std::pair<std::string, std::string>> ok_files = {{"index.json", "{}"}, {"500.png", "png"}, {"500.mp4", "mp4"}};
  struct Bad {
    const char* what;
    std::string manifest;
    std::vector<std::pair<std::string, std::string>> files;
  };
  std::vector<Bad> bad = {
      {"no manifest", "", ok_files},
      {"zip-slip ../", good_man, {{"index.json", "{}"}, {"500.png", "png"}, {"../evil.png", "x"}}},
      {"zip-slip absolute", good_man, {{"index.json", "{}"}, {"500.png", "png"}, {"/evil.png", "x"}}},
      {"zip-slip drive", good_man, {{"index.json", "{}"}, {"500.png", "png"}, {"C:/evil.png", "x"}}},
      {"newer format", std::string("{\"kind\":\"") + tron_zip::kKind + "\",\"format\":99}", ok_files},
      {"another game", std::string("{\"kind\":\"") + tron_zip::kKind + "\",\"format\":1,\"title_id\":\"12345678\"}", ok_files},
      {"wrong kind", "{\"kind\":\"save\",\"format\":1}", ok_files},
      {"no pictures", good_man, {{"index.json", "{}"}, {"500.mp4", "mp4"}}},
      {"no index.json", good_man, {{"500.png", "png"}}},
  };
  const size_t before = all;
  for (const auto& b : bad) {
    const fs::path z = work / "bad.zip";
    MakeZip(z, b.manifest, b.files);
    tron_zip::ImportResult r;
    err = tron_zip::Import(tron_zip::U8(z), trons, r);
    size_t now = 0;
    for (fs::recursive_directory_iterator it(trons, ec), end; it != end; it.increment(ec))
      if (it->is_regular_file()) ++now;
    Check(!err.empty() && now == before && !fs::exists(work / "files" / "evil.png", ec) && !fs::exists(work / "evil.png", ec),
          std::string("refused: ") + b.what + " -> \"" + err + "\"");
  }
  {  // a damaged member: one byte of 500.png's data changed after the CRC was written
    const fs::path z = work / "bad.zip";
    MakeZip(z, good_man, {{"index.json", "{}"}, {"500.png", "PNGDATA_PNGDATA"}});
    std::string bytes;
    {
      std::ifstream in(z, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    const size_t at = bytes.find("PNGDATA_PNGDATA");
    bytes[at] = 'X';
    std::ofstream(z, std::ios::binary | std::ios::trunc).write(bytes.data(), std::streamsize(bytes.size()));
    tron_zip::ImportResult r;
    err = tron_zip::Import(tron_zip::U8(z), trons, r);
    Check(!err.empty() && fs::exists(trons / "index.json", ec) && tron_zip::ListCache(trons, c2).size() == want.size(),
          "refused: bad CRC -> \"" + err + "\" (old previews kept)");
  }
  {  // a truncated zip (half of the export)
    const fs::path z = work / "bad.zip";
    {
      std::ifstream in(zip, std::ios::binary);
      std::ofstream o(z, std::ios::binary | std::ios::trunc);
      std::vector<char> buf(1 << 20);
      uint64_t left = fs::file_size(zip, ec) / 2;
      while (left && in.read(buf.data(), std::streamsize(std::min<uint64_t>(left, buf.size())))) {
        o.write(buf.data(), in.gcount());
        left -= uint64_t(in.gcount());
      }
    }
    tron_zip::ImportResult r;
    err = tron_zip::Import(tron_zip::U8(z), trons, r);
    Check(!err.empty() && tron_zip::ListCache(trons, c2).size() == want.size(), "refused: truncated zip -> \"" + err + "\"");
    fs::remove(z, ec);
  }
  {  // an unexpected member is skipped, never written; the rest imports
    const fs::path z = work / "bad.zip", t2 = work / "files2" / "trons";
    fs::remove_all(work / "files2", ec);
    MakeZip(z, good_man, {{"index.json", "{}"}, {"500.png", "png"}, {"evtmov/MOV_M000.mp4", "mp4"}, {"run.sh", "x"},
                          {"other/500.png", "x"}, {"evtmov/.hidden.png", "x"}});
    tron_zip::ImportResult r;
    err = tron_zip::Import(tron_zip::U8(z), t2, r);
    Check(err.empty() && r.skipped == 3 && r.counts.files == 3 && fs::exists(t2 / "evtmov" / "MOV_M000.mp4", ec) &&
              !fs::exists(t2 / "run.sh", ec) && !fs::exists(t2 / "other", ec),
          "unexpected members skipped (" + std::to_string(r.skipped) + "): " + (err.empty() ? std::string("ok") : err));
    fs::remove_all(work / "files2", ec);
    fs::remove(z, ec);
  }
  {  // a kill mid-swap: Recover puts one set back (review of 9f55ec5a)
    const fs::path t = work / "rec" / "trons", tmp = tron_zip::TmpDir(t), old = tron_zip::OldDir(t);
    auto put = [](const fs::path& d, const char* tag, bool complete) {
      std::error_code e;
      fs::create_directories(d, e);
      std::ofstream(d / "index.json") << tag;
      if (complete) std::ofstream(d / tron_zip::kComplete) << "complete\n";
    };
    auto tag = [](const fs::path& d) {
      std::ifstream in(d / "index.json");
      return std::string(std::istreambuf_iterator<char>(in), {});
    };
    auto clean = [&] { fs::remove_all(work / "rec", ec); };
    clean(), put(old, "old", false), put(tmp, "new", false);  // killed while unpacking after a rename? (old aside, new incomplete)
    tron_zip::Recover(t);
    Check(tag(t) == "old" && !fs::exists(tmp, ec) && !fs::exists(old, ec), "recover: old set back, incomplete new set removed");
    clean(), put(old, "old", false), put(tmp, "new", true);  // killed between the two renames
    tron_zip::Recover(t);
    Check(tag(t) == "new" && !fs::exists(t / tron_zip::kComplete, ec) && !fs::exists(tmp, ec) && !fs::exists(old, ec),
          "recover: complete new set promoted, old removed");
    clean(), put(t, "old", false), put(tmp, "new", true);  // killed after the marker, before moving the old set aside
    tron_zip::Recover(t);
    Check(tag(t) == "new" && !fs::exists(tmp, ec) && !fs::exists(old, ec), "recover: complete new set beside trons promoted");
    clean(), put(t, "cur", false), put(tmp, "new", false);  // a plain cut import
    tron_zip::Recover(t);
    Check(tag(t) == "cur" && !fs::exists(tmp, ec), "recover: cut import removed, trons kept");
    clean();
  }
  {  // export refuses an unfinished cache (no index.json)
    const fs::path fc = work / "fakecache";
    fs::remove_all(fc, ec);
    fs::create_directories(fc, ec);
    std::ofstream(fc / "500.png") << "png";
    tron_zip::Counts fcc;
    err = tron_zip::Export(fc, work / "fake.zip", "pc", "t", fcc);
    Check(!err.empty() && !fs::exists(work / "fake.zip", ec), "export refuses without index.json -> \"" + err + "\"");
    fs::remove_all(fc, ec);
  }
  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}
