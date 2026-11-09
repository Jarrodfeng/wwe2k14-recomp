#pragma once
// Session 98 (Jarrod: "could we have it export a zip that the phone launcher takes in? ... the easiest way for people to get it
// from their PC games to their phone game"): the tron previews (the PC launcher's Build previews / the phone's BUILD TRON PREVIEWS,
// src\tron_export.h: index.json, NNN.png, NNN.mp4 / NNN.alpha.mp4, evtmov\GROUP_Mnnn.png / .mp4) as ONE zip, made on the PC and
// taken in by the phone launcher. Shared by both launchers; std only (+ nlohmann/json, which both already carry).
//
// What goes in: only the export's own kinds of file (IsTronFile: index.json, *.png and *.mp4, at the top or in evtmov/), plus a
// small manifest, wwe2k14_tron_previews.json (format, source, counts, total bytes, created time, title id). Stored, not compressed
// (MP4 and PNG do not shrink), no ZIP64 (the whole set is ~0.8 GB; over 4 GB is refused).
// The previews are made from each player's own game files: the zip moves only between his own devices. It is never shipped or
// committed (CLAUDE.md rule 1).
//
// Import (the phone): the manifest is checked first, then every member's name (zip-slip: no absolute path, no "..", no folder but
// evtmov/, only the export's kinds of file; anything else is skipped and counted), sizes (per member and total caps) and free
// space; then everything is unpacked into <trons>.import, CRC-checked, and only then swapped in for <trons> (the old folder renamed
// to <trons>.old and removed after the swap): a failed or cut import leaves the old previews as they were.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets/char_mods.h"           // the zip reading helpers and raw inflate of the viewer's character import (reused)
#include "launcher_mobile/zip_writer.h"  // the phone launcher's stored zip writer (reused)

namespace tron_zip {

namespace fs = std::filesystem;

inline constexpr int kFormat = 1;
inline constexpr const char* kManifest = "wwe2k14_tron_previews.json";
inline constexpr const char* kKind = "wwe2k14_tron_previews";
inline constexpr const char* kTitleId = "545408B2";  // WWE 2K14 (Xbox 360), the only disc the project supports (CLAUDE.md)
inline constexpr const char* kDefaultName = "wwe2k14_tron_previews.zip";
inline constexpr uint64_t kMaxMember = 512ull << 20;    // one file (the biggest PC tron MP4 is ~10 MB)
inline constexpr uint64_t kMaxDeflated = 64ull << 20;   // one compressed member (inflated in memory: a re-zipped export)
inline constexpr uint64_t kMaxTotal = 0xFFFFFFFFull - (16ull << 20);  // no ZIP64: the zip must stay under 4 GB
inline constexpr int kMaxFiles = 20000;                 // the PC's set is ~1750 files
inline constexpr uint64_t kSpareBytes = 100ull << 20;   // free space kept after an import

struct Progress {
  std::atomic<uint64_t> done{0}, total{0};
  std::atomic<bool> cancel{false};
};

struct Counts {
  int files = 0, videos = 0, pictures = 0;
  uint64_t bytes = 0;
};

// "evtmov/MOV_M000.png" -> true. Names use '/' (as in a zip). One folder level at most, and that folder is evtmov; the file name is
// letters, digits, '_', '-', '.' (no leading dot); .png or .mp4, or index.json at the top.
inline bool IsTronFile(const std::string& rel) {
  if (rel.empty() || rel.size() > 128) return false;
  std::string dir, name = rel;
  if (const size_t s = rel.find('/'); s != std::string::npos) {
    dir = rel.substr(0, s), name = rel.substr(s + 1);
    if (dir != "evtmov" || name.find('/') != std::string::npos) return false;
  }
  if (name.empty() || name[0] == '.') return false;
  for (unsigned char c : name)
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  if (name == "index.json") return dir.empty();
  if (name.size() < 5) return false;
  std::string ext = name.substr(name.size() - 4);
  for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".png" || ext == ".mp4";
}
inline bool IsVideo(const std::string& rel) {
  return rel.size() > 4 && (rel.compare(rel.size() - 4, 4, ".mp4") == 0 || rel.compare(rel.size() - 4, 4, ".MP4") == 0);
}
// What the counts call a video: an MP4 that is not a stacked-alpha one (NNN.alpha.mp4: review of 9f55ec5a).
inline bool CountsAsVideo(const std::string& rel) {
  return IsVideo(rel) && !(rel.size() > 10 && rel.compare(rel.size() - 10, 10, ".alpha.mp4") == 0);
}

inline std::string U8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

inline std::string LocalTimeText() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[40];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %z", &tm);
  return buf;
}

// The files of a tron cache an export would take (relative names with '/', sorted: index.json first), and their counts.
inline std::vector<std::pair<std::string, uint64_t>> ListCache(const fs::path& cache, Counts& c) {
  std::vector<std::pair<std::string, uint64_t>> out;
  c = {};
  std::error_code ec;
  for (fs::recursive_directory_iterator it(cache, ec), end; !ec && it != end; it.increment(ec)) {
    if (it.depth() > 1) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    const std::string rel = U8(fs::relative(it->path(), cache, ec));
    if (ec || !IsTronFile(rel)) continue;
    const uint64_t sz = it->file_size(ec);
    if (ec) continue;
    out.push_back({rel, sz});
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    if ((a.first == "index.json") != (b.first == "index.json")) return a.first == "index.json";
    return a.first < b.first;
  });
  for (const auto& [rel, sz] : out) {
    ++c.files, c.bytes += sz;
    if (CountsAsVideo(rel)) ++c.videos;
    else if (!IsVideo(rel) && rel != "index.json") ++c.pictures;
  }
  return out;
}

// ------------------------------------------------------------------------------------------------------------------ export
// The cache -> one zip at `zip` (written as <zip>.part, renamed when complete). Returns "" or why not; `c` = what went in.
inline std::string Export(const fs::path& cache, const fs::path& zip, const std::string& source, const std::string& made_by,
                          Counts& c, Progress* prog = nullptr) {
  const auto files = ListCache(cache, c);
  if (c.pictures == 0) return "there are no tron previews to export yet (Build previews first)";
  if (files.empty() || files.front().first != "index.json")  // (the export run writes it last: without it the build did not finish)
    return "the previews are not finished (no index.json): Build previews again to finish them";
  if (c.bytes > kMaxTotal) return "the previews are bigger than 4 GB: too big for one zip";
  if (prog) prog->total = c.bytes, prog->done = 0;
  const nlohmann::json man = {{"kind", kKind},        {"format", kFormat},           {"source", source},
                              {"made_by", made_by},   {"created", LocalTimeText()},  {"title_id", kTitleId},
                              {"files", c.files},     {"videos", c.videos},          {"pictures", c.pictures},
                              {"total_bytes", c.bytes}};
  fs::path part = zip;
  part += ".part";
  std::error_code ec;
  std::string err;
  {
    mlauncher::zip::Writer w(part);
    if (!w.ok()) return "could not write " + U8(part);
    w.AddText(kManifest, man.dump(1) + "\n");  // first, so a reader finds it at once
    for (const auto& [rel, sz] : files) {
      if (prog && prog->cancel) {
        err = "cancelled";
        break;
      }
      if (!w.AddFile(rel, cache / fs::u8path(rel))) {
        err = "could not read or write " + rel + (w.ok() ? "" : " (is the disk full?)");
        break;
      }
      if (prog) prog->done += sz;
    }
    if (err.empty() && !w.Finish()) err = "could not finish the zip (is the disk full?)";
  }
  if (!err.empty()) return fs::remove(part, ec), err;
  fs::rename(part, zip, ec);  // (replaces an older zip of that name in one step)
  if (ec) return fs::remove(part, ec), "could not name the finished zip " + U8(zip);
  return "";
}

// ------------------------------------------------------------------------------------------------------------------ import
struct ImportResult {
  Counts counts;
  int skipped = 0;          // members that are not tron preview files (never written)
  std::string source;       // the manifest's "source" ("pc" / "phone")
  std::string created;      // the manifest's "created"
};

namespace detail {
using assets::char_mods::imp_detail::L16;
using assets::char_mods::imp_detail::L32;
using assets::char_mods::imp_detail::ReadAt;
using assets::char_mods::imp_detail::BadMemberPath;

struct Member {
  std::string name;
  uint16_t flags = 0, method = 0;
  uint32_t crc = 0, csize = 0, usize = 0, lho = 0;
};

// The central directory of the zip at `f` (size `sz`). "" or why not.
inline std::string ReadDirectory(std::ifstream& f, uint64_t sz, std::vector<Member>& out) {
  if (sz < 22) return "this is not a zip file";
  const size_t tail = size_t(std::min<uint64_t>(sz, 22 + 65535));
  std::vector<uint8_t> t(tail);
  if (!ReadAt(f, sz - tail, t.data(), tail)) return "the zip could not be read";
  size_t e = std::string::npos;
  for (size_t i = tail - 22 + 1; i-- > 0;)
    if (L32(&t[i]) == 0x06054b50u) {
      e = i;
      break;
    }
  if (e == std::string::npos) return "this is not a zip file (or it is incomplete: copy it again)";
  const uint16_t total = L16(&t[e + 10]);
  const uint32_t cd_size = L32(&t[e + 12]), cd_off = L32(&t[e + 16]);
  if (total == 0xFFFF || cd_off == 0xFFFFFFFFu) return "the zip is too big (ZIP64 is not read)";
  if (cd_size > (64u << 20) || uint64_t(cd_off) + cd_size > sz) return "the zip is damaged";
  std::vector<uint8_t> cd(cd_size);
  if (cd_size && !ReadAt(f, cd_off, cd.data(), cd_size)) return "the zip could not be read";
  size_t p = 0;
  for (uint16_t k = 0; k < total; ++k) {
    if (p + 46 > cd.size() || L32(&cd[p]) != 0x02014b50u) return "the zip is damaged";
    Member m;
    m.flags = L16(&cd[p + 8]), m.method = L16(&cd[p + 10]);
    m.crc = L32(&cd[p + 16]), m.csize = L32(&cd[p + 20]), m.usize = L32(&cd[p + 24]), m.lho = L32(&cd[p + 42]);
    const size_t nl = L16(&cd[p + 28]), xl = L16(&cd[p + 30]), cl = L16(&cd[p + 32]);
    if (p + 46 + nl + xl + cl > cd.size()) return "the zip is damaged";
    m.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nl);
    for (char& ch : m.name)
      if (ch == '\\') ch = '/';
    p += 46 + nl + xl + cl;
    out.push_back(std::move(m));
  }
  return "";
}

// One member's bytes into `to` (stored: copied in pieces; deflated: in memory, capped). CRC checked. "" or why not.
inline std::string Extract(std::ifstream& f, uint64_t sz, const Member& m, const fs::path& to, Progress* prog) {
  uint8_t lh[30];
  if (!ReadAt(f, m.lho, lh, 30) || L32(lh) != 0x04034b50u) return "the zip is damaged (" + m.name + ")";
  const uint64_t at = uint64_t(m.lho) + 30 + L16(lh + 26) + L16(lh + 28);
  if (at + m.csize > sz) return "the zip is damaged or incomplete (" + m.name + ")";
  std::ofstream o(to, std::ios::binary | std::ios::trunc);
  if (!o) return "could not write " + m.name;
  uint32_t crc = 0;
  if (m.method == 0) {
    std::vector<char> buf(1 << 20);
    uint64_t left = m.usize, off = at;
    f.clear();
    f.seekg(std::streamoff(off), std::ios::beg);
    while (left) {
      if (prog && prog->cancel) return "cancelled";
      const size_t n = size_t(std::min<uint64_t>(left, buf.size()));
      if (!f.read(buf.data(), std::streamsize(n))) return "the zip could not be read (" + m.name + ")";
      crc = mlauncher::zip::Crc32(crc, buf.data(), n);
      if (!o.write(buf.data(), std::streamsize(n))) return "could not write " + m.name + " (is the storage full?)";
      left -= n;
      if (prog) prog->done += n;
    }
  } else {
    std::vector<uint8_t> raw(m.csize), full;
    if (m.csize && !ReadAt(f, at, raw.data(), raw.size())) return "the zip could not be read (" + m.name + ")";
    if (!assets::char_mods::Inflate(raw.data(), raw.size(), full, size_t(m.usize) + 1) || full.size() != m.usize)
      return "the zip is damaged (" + m.name + ")";
    crc = mlauncher::zip::Crc32(0, full.data(), full.size());
    if (!o.write(reinterpret_cast<const char*>(full.data()), std::streamsize(full.size())))
      return "could not write " + m.name + " (is the storage full?)";
    if (prog) prog->done += m.usize;
  }
  o.close();
  if (!o) return "could not write " + m.name + " (is the storage full?)";
  if (crc != m.crc) return "the zip is damaged (" + m.name + " does not match its checksum: copy the zip again)";
  return "";
}
}  // namespace detail

// The import's folders next to `trons`: the new set while it unpacks, the old set during the swap. A complete new set holds kComplete
// (written after every file passed its CRC; IsTronFile never takes it, so it never travels in an export).
inline constexpr const char* kComplete = ".import_complete";
inline fs::path TmpDir(const fs::path& trons) { fs::path p = trons; return p += ".import"; }
inline fs::path OldDir(const fs::path& trons) { fs::path p = trons; return p += ".old"; }

// After a kill in the middle of an import's swap (review of 9f55ec5a): puts a set back in `trons` before anything is removed.
// trons missing: a complete new set is promoted, else the old set comes back. A complete new set next to a trons that was not yet
// moved aside is promoted too (its import had finished). Removes only what is no longer needed: an incomplete new set, and the old
// set once trons is in place. Called at the start of every import and at the phone launcher's start.
inline void Recover(const fs::path& trons) {
  std::error_code ec;
  const fs::path tmp = TmpDir(trons), old = OldDir(trons);
  const bool complete = fs::exists(tmp / kComplete, ec);
  if (complete) {
    bool room = !fs::exists(trons, ec);
    if (!room) {
      fs::remove_all(old, ec);  // (stale: trons is in place)
      ec.clear();
      fs::rename(trons, old, ec);
      room = !ec;
    }
    if (room) {
      ec.clear();
      fs::rename(tmp, trons, ec);
      if (!ec) fs::remove(trons / kComplete, ec);
    }
  }
  ec.clear();
  if (!fs::exists(trons, ec) && fs::exists(old, ec)) {
    ec.clear();
    fs::rename(old, trons, ec);
  }
  ec.clear();
  if (fs::exists(tmp, ec) && !fs::exists(tmp / kComplete, ec)) fs::remove_all(tmp, ec);
  ec.clear();
  if (fs::exists(trons, ec) && fs::exists(old, ec)) fs::remove_all(old, ec);
}

// The zip at `zip_u8` (a path; on the phone /proc/self/fd/N of the picked file) -> `trons` (replaced only when all of it unpacked).
// Returns "" or why not (the old previews are then untouched).
inline std::string Import(const std::string& zip_u8, const fs::path& trons, ImportResult& r, Progress* prog = nullptr) {
  using namespace detail;
  r = {};
  std::error_code ec;
  const fs::path src = fs::u8path(zip_u8);
  std::ifstream f(src, std::ios::binary);
  if (!f) return "the zip could not be opened";
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  if (end <= 0) return "the zip could not be read (pick it from the phone's own storage, not a cloud folder)";
  const uint64_t sz = uint64_t(end);
  std::vector<Member> members;
  if (std::string why = ReadDirectory(f, sz, members); !why.empty()) return why;
  // --- the manifest
  const Member* man = nullptr;
  for (const auto& m : members)
    if (m.name == kManifest) man = &m;
  if (!man) return "this zip is not a tron previews export (make it with EXPORT FOR PHONE in the PC launcher's Trons tab)";
  if (man->usize > (1u << 20) || man->csize > (1u << 20) || (man->method != 0 && man->method != 8) || (man->flags & 1))
    return "the zip's manifest is damaged";
  {
    uint8_t lh[30];
    if (!ReadAt(f, man->lho, lh, 30) || L32(lh) != 0x04034b50u) return "the zip's manifest is damaged";
    std::vector<uint8_t> raw(man->csize), full;
    if (!raw.empty() && !ReadAt(f, uint64_t(man->lho) + 30 + L16(lh + 26) + L16(lh + 28), raw.data(), raw.size()))
      return "the zip's manifest could not be read";
    if (man->method == 0) full = std::move(raw);
    else if (!assets::char_mods::Inflate(raw.data(), raw.size(), full, size_t(man->usize) + 1)) return "the zip's manifest is damaged";
    if (full.size() != man->usize) return "the zip's manifest is damaged";
    const std::string text(full.begin(), full.end());
    try {
      const auto j = nlohmann::json::parse(text);
      if (j.value("kind", "") != kKind) return "this zip is not a tron previews export";
      const int fmt = j.value("format", 0);
      if (fmt < 1) return "the zip's manifest is damaged";
      if (fmt > kFormat) return "this zip was made by a newer launcher: update this app first";
      if (j.contains("title_id") && j["title_id"].is_string() && j["title_id"].get<std::string>() != kTitleId)
        return "this zip was made from another game";
      r.source = j.value("source", "");
      r.created = j.value("created", "");
    } catch (const std::exception&) {
      return "the zip's manifest is damaged";
    }
  }
  // --- every member checked before anything is written
  std::vector<const Member*> take;
  uint64_t total = 0;
  bool index = false;
  for (const auto& m : members) {
    if (&m == man) continue;
    if (!m.name.empty() && m.name.back() == '/') continue;  // a folder entry
    if (BadMemberPath(m.name)) return "the zip holds an unsafe path (" + m.name + "): nothing was imported";
    if (!IsTronFile(m.name)) {
      ++r.skipped;
      continue;
    }
    if (m.flags & 1) return "the zip is password protected";
    if (m.method != 0 && m.method != 8) return "the zip uses a compression this app cannot read: make it again with EXPORT FOR PHONE";
    if (m.usize > kMaxMember || m.csize > kMaxMember || (m.method == 8 && (m.usize > kMaxDeflated || m.csize > kMaxDeflated)))
      return "a file in the zip is too big (" + m.name + ")";
    if (m.method == 0 && m.csize != m.usize) return "the zip is damaged (" + m.name + ")";
    if (int(take.size()) >= kMaxFiles) return "the zip holds too many files";
    total += m.usize;
    if (total > kMaxTotal) return "the zip unpacks to more than 4 GB";
    if (m.name == "index.json") index = true;
    take.push_back(&m);
    if (CountsAsVideo(m.name)) ++r.counts.videos;
    else if (!IsVideo(m.name) && m.name != "index.json") ++r.counts.pictures;
  }
  r.counts.files = int(take.size()), r.counts.bytes = total;
  if (r.counts.pictures == 0 || !index) return "the zip holds no tron previews";
  // --- room: the new set is unpacked next to the old one (swapped at the end)
  const fs::path tmp = TmpDir(trons), old = OldDir(trons);
  Recover(trons);  // (a killed import's sets first: never lose both)
  if (!fs::exists(trons, ec) && fs::exists(tmp / kComplete, ec))
    return "the previews of an earlier import could not be put in place: restart the app and try again";
  ec.clear();
  fs::remove_all(tmp, ec);  // (an incomplete one: left over from a cut import)
  ec.clear();
  if (fs::exists(trons, ec)) fs::remove_all(old, ec);
  ec.clear();
  fs::create_directories(tmp / "evtmov", ec);
  if (ec) return "could not make a folder in the app's storage";
  const auto space = fs::space(tmp, ec);
  if (!ec && space.available < total + kSpareBytes) {
    fs::remove_all(tmp, ec);
    return "not enough free storage: the previews need " + std::to_string((total + kSpareBytes) >> 20) + " MB, " +
           std::to_string(space.available >> 20) + " MB free";
  }
  if (prog) prog->total = total, prog->done = 0;
  for (const Member* m : take) {
    if (std::string why = Extract(f, sz, *m, tmp / fs::u8path(m->name), prog); !why.empty()) {
      fs::remove_all(tmp, ec);
      return why;
    }
  }
  f.close();
  // Every file is written and checked: the marker says this set may replace the old one (Recover promotes it after a kill).
  {
    std::ofstream mk(tmp / kComplete, std::ios::trunc);
    mk << "complete\n";
    mk.close();
    if (!mk) {
      fs::remove_all(tmp, ec);
      return "could not write into the app's storage (is it full?)";
    }
  }
  // --- the swap
  const bool had = fs::exists(trons, ec);
  ec.clear();
  if (had) {
    fs::rename(trons, old, ec);
    if (ec) {
      fs::remove_all(tmp, ec);  // (the old set is still in place)
      return "could not replace the old previews (are they in use? close the Arena Viewer and try again)";
    }
  }
  fs::rename(tmp, trons, ec);
  if (ec) {
    std::error_code e2;
    bool restored = !had;
    if (had) fs::rename(old, trons, e2), restored = !e2;  // the old ones back
    if (restored && had) fs::remove_all(tmp, e2);
    // (else both sets stay where they are: Recover puts one back at the next start or import)
    return "could not put the new previews in place";
  }
  fs::remove(trons / kComplete, ec);
  ec.clear();
  fs::remove_all(old, ec);
  return "";
}

}  // namespace tron_zip
