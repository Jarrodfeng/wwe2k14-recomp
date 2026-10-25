// wwe2k14 - Session 61: the shader list. Before the SDK loads its shader storage (OnPreLaunchModule, right before
// ReXApp's InitializeShaderStorage), what the shipped list names and the player's storage lacks is added to it:
//   - shaders: rebuilt from the player's own game files (pac\_startup.pac, car\car.pac, edit\edit.pac, read through
//     GAME: so a disc image works too): a container's microcode found by its XXH3, or a patched vertex shader = an
//     original's microcode with the list's XOR masks on the changed dwords (the vertex declaration's fetch fields),
//     checked against the hash before it is written;
//   - pipeline descriptions (the emulated path's .xpso, the native renderer's .xnps): appended as they are.
// The SDK's own storage load then translates the shaders and makes the pipelines (stock ones at boot, native ones in
// the background from the first native frames: gpu_native_pipeline_prewarm_ms). Layout and list format:
// docs\systems\shaders.md. The list holds hashes, XOR masks and pipeline state only - no shader code.
//
// --wwe2k14_shader_list=PATH (default: shader_list.txt next to the exe; empty file name or missing file = off).
// Logged: "[wwe2k14][shaders] ...". Never touches a file whose header differs from the list's (another SDK version).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // GetModuleFileNameW

#include "game_files.h"
#include "native_bpe.h"
#include "xctd_file.h"

REXCVAR_DEFINE_STRING(wwe2k14_shader_list, "", "wwe2k14",
                      "Session 61: the shader list added to the shader storage before it loads (shaders rebuilt from "
                      "the player's own game files, pipeline descriptions); empty = shader_list.txt next to the exe, "
                      "'off' = none");

namespace wwe2k14::shader_list {

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

inline std::vector<uint8_t> Hex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  return out;
}

inline std::vector<uint8_t> ReadHost(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return f ? std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::vector<uint8_t>{};
}

// Every shader container's microcode in a buffer, by XXH3 (tools\shaders\shaderlib.py containers()).
inline void IndexContainers(const uint8_t* d, size_t n, std::unordered_map<uint64_t, std::vector<uint8_t>>& index) {
  for (size_t p = 0; p + 36 <= n; p += 4) {
    const uint32_t magic = Be32(d + p);
    if (magic != 0x102A1100u && magic != 0x102A1101u) continue;
    const uint32_t vsize = Be32(d + p + 4), psize = Be32(d + p + 8), desc = Be32(d + p + 24);
    if (desc < 28 || desc >= vsize || p + desc + 8 > n || p + uint64_t(vsize) + psize > n) continue;
    const uint32_t moff = Be32(d + p + desc), msize = Be32(d + p + desc + 4);
    if (!msize || (msize & 3) || uint64_t(moff) + msize > psize) continue;
    const uint8_t* code = d + p + vsize + moff;
    const uint64_t h = XXH3_64bits(code, msize);
    if (!index.count(h)) index.emplace(h, std::vector<uint8_t>(code, code + msize));
  }
}

// A pac's leaves the way tools\wm30\pac.py reads them: the XCTD layer, then the PACH archives (after EPAC's 16 KB
// header, on 2 KB sectors); BPE entries decoded; everything scanned for containers (the raw bytes too: plain leaves).
inline bool IndexPac(std::string_view guest_path, std::unordered_map<uint64_t, std::vector<uint8_t>>& index) {
  auto* entry = game_files::Resolve(guest_path);
  if (!entry || !entry->size()) return false;
  std::vector<uint8_t> file(entry->size());
  if (game_files::Read(guest_path, file) != file.size()) return false;
  std::vector<uint8_t> data;
  std::string error;
  if (!xctd::Unpack(file, data, error)) {
    REXLOG_WARN("[wwe2k14][shaders] {}: {}", guest_path, error);
    return false;
  }
  file.clear();
  IndexContainers(data.data(), data.size(), index);
  size_t pos = data.size() >= 4 && std::memcmp(data.data(), "EPAC", 4) == 0 ? 0x4000 : 0;
  while (pos + 8 <= data.size()) {
    if (std::memcmp(data.data() + pos, "PACH", 4) != 0) {
      pos += 0x800;
      continue;
    }
    const uint32_t count = Le32(data.data() + pos + 4);
    const size_t first = pos + 8 + size_t(12) * count;
    size_t end = first;
    if (first > data.size()) break;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* e = data.data() + pos + 8 + 12 * i;
      const size_t off = first + Le32(e + 4), size = Le32(e + 8);
      if (off + size > data.size()) break;
      end = std::max(end, off + size);
      if (size >= 16 && std::memcmp(data.data() + off, "BPE ", 4) == 0) {
        std::vector<uint8_t> out(native_bpe::ExpandedSize(data.data() + off));
        const native_bpe::Result r =
            native_bpe::Decode(data.data() + off, out.data(), uint32_t(out.size()), uint32_t(size - 16));
        if (!r.error) IndexContainers(out.data(), r.produced, index);
      }
    }
    pos = (end + 0x7FF) & ~size_t(0x7FF);
  }
  return true;
}

struct Stats {
  uint32_t shaders_listed = 0, shaders_had = 0, shaders_added = 0, shaders_missing = 0, shaders_bad = 0;
  uint32_t pipelines_added = 0, native_added = 0;
};

// A storage file of fixed-size records after a header: the record hashes in it (the first 8 bytes of each), or false
// when its header is not `header` (another SDK version: left alone). A missing file is created with the header.
inline bool OpenRecords(const std::filesystem::path& path, const std::vector<uint8_t>& header, size_t record_size,
                        std::unordered_set<uint64_t>& have) {
  std::vector<uint8_t> d = ReadHost(path);
  if (d.empty()) {
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    return bool(o);
  }
  if (d.size() < header.size() || std::memcmp(d.data(), header.data(), header.size()) != 0) return false;
  for (size_t p = header.size(); p + record_size <= d.size(); p += record_size) {
    uint64_t h;
    std::memcpy(&h, d.data() + p, 8);
    have.insert(h);
  }
  // A torn record at the end (a hard exit) would misalign what we append: cut it off first.
  const size_t whole = header.size() + (d.size() - header.size()) / record_size * record_size;
  if (whole != d.size()) std::filesystem::resize_file(path, whole);
  return true;
}

inline void Import(const std::filesystem::path& cache_root) {
  std::string setting = REXCVAR_GET(wwe2k14_shader_list);
  if (setting == "off" || cache_root.empty()) return;
  std::filesystem::path list_path;
  if (setting.empty()) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    list_path = std::filesystem::path(buf).parent_path() / "shader_list.txt";
  } else {
    list_path = setting;
  }
  std::ifstream lf(list_path);
  if (!lf) {
    if (!setting.empty()) REXLOG_WARN("[wwe2k14][shaders] no shader list at {}", list_path.string());
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  struct ShaderLine {
    uint64_t hash = 0, base = 0;
    bool pixel = false;
    std::vector<std::pair<uint32_t, uint32_t>> xors;
    bool exact = false;
  };
  std::vector<ShaderLine> shaders;
  std::vector<uint8_t> xsh_header;
  std::unordered_map<std::string, std::vector<uint8_t>> headers;  // "fsi" / "fbo" / "native-fsi" / "native-fbo"
  std::unordered_map<std::string, std::vector<std::vector<uint8_t>>> records;
  std::string line;
  while (std::getline(lf, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();  // (a checkout with CRLF line ends)
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> w;
    for (size_t p = 0; p < line.size();) {
      const size_t q = line.find(' ', p);
      w.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p));
      if (q == std::string::npos) break;
      p = q + 1;
    }
    try {
      if (w[0] == "S" && w.size() >= 5) {
        ShaderLine s;
        s.hash = std::stoull(w[1], nullptr, 16);
        s.pixel = w[2] == "PS";
        if (w[4] == "exact") {
          s.exact = true;
        } else if (w[4] == "patched" && w.size() >= 6) {
          s.base = std::stoull(w[5], nullptr, 16);
          for (size_t i = 6; i < w.size(); ++i) {
            const size_t c = w[i].find(':');
            s.xors.emplace_back(uint32_t(std::stoul(w[i].substr(0, c))), uint32_t(std::stoul(w[i].substr(c + 1), nullptr, 16)));
          }
        } else {
          continue;  // image / image-patched: the library's own shaders (met at the first frames anyway)
        }
        shaders.push_back(std::move(s));
      } else if (w[0] == "H" && w.size() == 3) {
        if (w[1] == "xsh") xsh_header = Hex(w[2]);
        else headers[w[1]] = Hex(w[2]);
      } else if (w[0] == "P" && w.size() == 3) {
        records[w[1]].push_back(Hex(w[2]));
      } else if (w[0] == "N" && w.size() == 3) {
        records["native-" + w[1]].push_back(Hex(w[2]));  // N fsi|fbo <record>: one file per render path
      }
    } catch (...) {
      REXLOG_WARN("[wwe2k14][shaders] {}: a line it cannot read: {}", list_path.string(), line.substr(0, 80));
    }
  }
  const std::filesystem::path dir = cache_root / "shaders" / "shareable";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  Stats st;
  st.shaders_listed = uint32_t(shaders.size());

  // Shaders: what the player's .xsh lacks, rebuilt from their files.
  const std::filesystem::path xsh_path = dir / "545408B2.xsh";
  std::vector<uint8_t> xsh = ReadHost(xsh_path);
  std::unordered_set<uint64_t> have;
  bool xsh_ok = xsh_header.size() == 8;
  if (xsh_ok && !xsh.empty() && (xsh.size() < 8 || std::memcmp(xsh.data(), xsh_header.data(), 8) != 0)) xsh_ok = false;
  size_t xsh_valid = 8;
  if (xsh_ok && !xsh.empty()) {
    for (size_t p = 8; p + 12 <= xsh.size();) {
      uint64_t h;
      std::memcpy(&h, xsh.data() + p, 8);
      const size_t n = 12 + size_t(Le32(xsh.data() + p + 8) & 0x7FFFFFFF) * 4;
      if (p + n > xsh.size()) break;
      have.insert(h);
      p += n;
      xsh_valid = p;
    }
  }
  std::vector<const ShaderLine*> wanted;
  for (const ShaderLine& s : shaders) {
    if (have.count(s.hash)) ++st.shaders_had;
    else wanted.push_back(&s);
  }
  if (xsh_ok && !wanted.empty()) {
    std::unordered_map<uint64_t, std::vector<uint8_t>> index;
    for (const char* pac : {"game:\\pac\\_startup.pac", "game:\\pac\\car\\car.pac", "game:\\pac\\edit\\edit.pac"}) {
      if (!IndexPac(pac, index)) REXLOG_WARN("[wwe2k14][shaders] {}: not read", pac);
    }
    std::vector<uint8_t> out;
    for (const ShaderLine* s : wanted) {
      auto it = index.find(s->exact ? s->hash : s->base);
      if (it == index.end()) {
        ++st.shaders_missing;
        continue;
      }
      std::vector<uint8_t> code = it->second;
      bool ok = true;
      for (const auto& [i, mask] : s->xors) {
        if (size_t(i) * 4 + 4 > code.size()) {
          ok = false;
          break;
        }
        const uint32_t v = Be32(code.data() + i * 4) ^ mask;
        code[i * 4] = uint8_t(v >> 24), code[i * 4 + 1] = uint8_t(v >> 16), code[i * 4 + 2] = uint8_t(v >> 8),
                  code[i * 4 + 3] = uint8_t(v);
      }
      if (!ok || XXH3_64bits(code.data(), code.size()) != s->hash) {
        ++st.shaders_bad;
        continue;
      }
      const uint32_t bits = uint32_t(code.size() / 4) | (s->pixel ? 0x80000000u : 0u);
      const size_t at = out.size();
      out.resize(at + 12 + code.size());
      std::memcpy(out.data() + at, &s->hash, 8);
      std::memcpy(out.data() + at + 8, &bits, 4);
      std::memcpy(out.data() + at + 12, code.data(), code.size());
      ++st.shaders_added;
    }
    if (!out.empty()) {
      if (xsh.empty()) {
        std::ofstream o(xsh_path, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(xsh_header.data()), 8);
      } else if (xsh_valid != xsh.size()) {
        std::filesystem::resize_file(xsh_path, xsh_valid);  // a torn record: cut before appending
      }
      std::ofstream o(xsh_path, std::ios::binary | std::ios::app);
      o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    }
  } else if (!xsh_ok) {
    REXLOG_WARN("[wwe2k14][shaders] the shader storage's header is not the list's: shaders not added");
  }

  // Pipeline descriptions. With the native renderer's truth frames (NATIVE, everyone's default) the emulated path makes
  // almost no pipelines (run 4100: none new in a whole match), while the SDK makes every stored one at boot before the
  // game starts: its records are left out there (they would only lengthen the first boot on a slow GPU).
  const bool native_truth =
      rex::cvar::GetFlagByName("gpu_native") == "2" && rex::cvar::GetFlagByName("gpu_native_truth") == "true";
  if (native_truth) {
    records.erase("fsi");
    records.erase("fbo");
  }
  const struct {
    const char* key;
    const char* file;
    size_t record;
  } kinds[] = {{"fsi", "545408B2.fsi.vk.xpso", 68}, {"fbo", "545408B2.fbo.vk.xpso", 68},
               {"native-fsi", "545408B2.native.fsi.vk.xnps", 72}, {"native-fbo", "545408B2.native.fbo.vk.xnps", 72}};
  for (const auto& k : kinds) {
    auto h = headers.find(k.key);
    auto r = records.find(k.key);
    if (h == headers.end() || r == records.end()) continue;
    std::unordered_set<uint64_t> have_records;
    const std::filesystem::path path = dir / k.file;
    if (!OpenRecords(path, h->second, k.record, have_records)) {
      REXLOG_WARN("[wwe2k14][shaders] {}: its header is not the list's (another version): not added", k.file);
      continue;
    }
    std::ofstream o(path, std::ios::binary | std::ios::app);
    for (const std::vector<uint8_t>& rec : r->second) {
      if (rec.size() != k.record) continue;
      uint64_t hh;
      std::memcpy(&hh, rec.data(), 8);
      if (!have_records.insert(hh).second) continue;
      o.write(reinterpret_cast<const char*>(rec.data()), std::streamsize(rec.size()));
      ++(std::strncmp(k.key, "native", 6) == 0 ? st.native_added : st.pipelines_added);
    }
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[wwe2k14][shaders] list {}: {} shaders ({} stored already, {} added from the game files, {} not found, "
              "{} failed the hash check), {} pipelines{} + {} native pipelines added ({:.0f} ms)",
              list_path.filename().string(), st.shaders_listed, st.shaders_had, st.shaders_added, st.shaders_missing,
              st.shaders_bad, st.pipelines_added, native_truth ? " (NATIVE: the emulated path's left out)" : "",
              st.native_added, ms);
}

}  // namespace wwe2k14::shader_list
