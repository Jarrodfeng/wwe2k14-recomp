// wwe2k14 - one-shot dump of the loaded guest image (Session 8, task 5: code index strings + RTTI).
//
// --wwe2k14_dump_image=<path> writes the guest image range [image_base, image_base + image_size)
// (0x82000000 + REX_IMAGE_SIZE, from PPCImageConfig) to <path>, byte for byte as guest memory holds it
// (big-endian), once, from OnPreLaunchModule: the XEX is loaded and decompressed, guest code has not run.
// tools\q\build_index.py reads it for string references and MSVC RTTI. Default empty = off (nothing runs).
//
// The file is game-derived: write it under out\harness\ (gitignored), e.g. out\harness\index\image.bin.
// Pages the host cannot read (none expected: the startup scan reads the whole range) are written as zeros
// and counted in the log line.

#pragma once

#include <rex/cvar.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

REXCVAR_DEFINE_STRING(wwe2k14_dump_image, "", "wwe2k14",
                      "Write the loaded guest image (0x82000000 + image size) to this file once at launch (empty = off)");

namespace wwe2k14 {

inline void DumpGuestImage(rex::Runtime* runtime, const rex::PPCImageInfo& image, const std::string& path) {
  if (path.empty()) return;
  const uint32_t base = uint32_t(image.image_base);
  const uint32_t size = uint32_t(image.image_size);
  const uint8_t* host = runtime->memory()->TranslateVirtual<const uint8_t*>(base);

  std::vector<uint8_t> buf(size, 0);
  size_t unreadable = 0;
  constexpr uint32_t kPage = 0x1000;
  for (uint32_t off = 0; off < size; off += kPage) {
    const uint32_t n = (size - off) < kPage ? (size - off) : kPage;
    bool readable = true;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(host + off, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
      readable = false;
    }
#endif
    if (readable) {
      std::memcpy(buf.data() + off, host + off, n);
    } else {
      ++unreadable;
    }
  }

  std::error_code ec;
  const std::filesystem::path out(path);
  if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
  FILE* f = nullptr;
#ifdef _WIN32
  if (_wfopen_s(&f, out.wstring().c_str(), L"wb") != 0) f = nullptr;
#else
  f = std::fopen(out.string().c_str(), "wb");
#endif
  if (!f) {
    REXLOG_WARN("[wwe2k14][image_dump] cannot open {} for writing", path);
    return;
  }
  const size_t written = std::fwrite(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  REXLOG_INFO("[wwe2k14][image_dump] wrote {} of {} bytes (guest 0x{:08X}-0x{:08X}, {} unreadable pages zeroed) to {}",
              written, buf.size(), base, base + size, unreadable, path);
}

}  // namespace wwe2k14
