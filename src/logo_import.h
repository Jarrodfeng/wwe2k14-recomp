// wwe2k14 - logo importer v0 (Session 18 item 9; default off).
//
// --wwe2k14_logo_import=true   the open Create-a-Logo editor gets the picture mods\logos\import.png (or
//                              --wwe2k14_logo_import_file=PATH): scaled to the canvas (256x256), alpha kept. Applied
//                              at the editor's next image update after the importer is armed: when the setting turns
//                              on (e.g. `cvar wwe2k14_logo_import true` once the editor shows the slot) or when the
//                              file changes (a launcher button can write it). Arming it before the editor is open
//                              does nothing useful: opening a slot loads the slot's own pixels (run 1406). You can
//                              paint on the picture; SAVE in the editor should keep it as a real logo (the game's own
//                              save data; not checked yet). Backing out without saving leaves the slot as it was.
//
// How the editor shows a logo (Session 18, runs 1401-1409; tools\analysis\list_function.py): Create-a-Logo is a
// 256x256 paint canvas (the grid labels a logo "256x256"); no render-to-texture (the menus resolve only the final
// picture, run 1401). The editor object P keeps its image at P+96 (8-byte header, then w x h ARGB words; copies at
// P+0x40068 / P+0x80070, picked by the flags P+24 / P+28); when P+92 is set, sub_833A8730(P) copies it into the
// canvas O = [P+16] (current pixel buffer O + i * 0x40010 + 68, plus an 8-bit plane) and backs P+96 up to P+0x40068.
// sub_8341AF68(O) flips the canvas's two buffers (index O+0x80064); a render-command callback sub_8341B0F0(O, i)
// copies buffer i into the canvas texture (O+40, linear k_8_8_8_8 256x256) through its lock / unlock. Found with
// texture-load captures (run 1402) and write watches on the texture's 0xE0000000 mirror (run 1405) and on O+68 (run
// 1408). Writing the canvas buffers themselves (runs 1406 / 1407) is overwritten: they are refilled from P+96. The
// hook writes the picture into P's three images and sets P+92 (run 1409: the canvas and the preview show it).

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <wincodec.h>
#endif
#if defined(_WIN32)  // Session 63 (Android port): Windows libraries
#pragma comment(lib, "windowscodecs.lib")
#endif

#include "mods.h"

REXCVAR_DEFINE_BOOL(wwe2k14_logo_import, false, "wwe2k14",
                    "Logo importer v0 (Session 18): the open Create-a-Logo editor gets mods\\logos\\import.png (or "
                    "--wwe2k14_logo_import_file) when armed (setting on / file changed). See src/logo_import.h");
REXCVAR_DEFINE_STRING(wwe2k14_logo_import_file, "", "wwe2k14", "Logo importer: the PNG (default mods\\logos\\import.png)");

REX_EXTERN(__imp__sub_833A8730);

namespace wwe2k14::logo_import {

inline bool g_was_on = false;
inline std::filesystem::file_time_type g_applied_stamp{};

// The PNG as w x h straight-alpha BGRA (WIC), scaled with the Fant filter.
#if !defined(_WIN32)
// Session 63 (Android port): no WIC on the phone (the importer is off by default).
inline bool LoadPng(const std::filesystem::path&, uint32_t, uint32_t, std::vector<uint8_t>&, std::string& err) {
  err = "the logo importer reads PNGs with Windows Imaging Component (Windows only)";
  return false;
}
#else
inline bool LoadPng(const std::filesystem::path& file, uint32_t w, uint32_t h, std::vector<uint8_t>& bgra,
                    std::string& err) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* factory = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  IWICFormatConverter* conv = nullptr;
  bool ok = false;
  do {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
      err = "WIC unavailable";
      break;
    }
    if (FAILED(factory->CreateDecoderFromFilename(file.wstring().c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) {
      err = "can't read the image";
      break;
    }
    if (FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) {
      err = "can't convert to BGRA";
      break;
    }
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(conv, w, h, WICBitmapInterpolationModeFant))) {
      err = "can't scale";
      break;
    }
    bgra.resize(size_t(w) * h * 4);
    ok = SUCCEEDED(scaler->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data()));
    if (!ok) err = "CopyPixels failed";
  } while (false);
  if (scaler) scaler->Release();
  if (conv) conv->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (factory) factory->Release();
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}
#endif  // _WIN32

inline std::filesystem::path File() {
  const std::string f = REXCVAR_GET(wwe2k14_logo_import_file);
  if (!f.empty()) return std::filesystem::u8path(f);
  return mods::g_dir.empty() ? std::filesystem::path() : mods::g_dir / "logos" / "import.png";
}

// Before the editor's image -> canvas update (sub_833A8730, editor object `p`, canvas o = [p+16]): the picture into the
// editor's three images (p+96 / p+0x40068 / p+0x80070: 8-byte header, then w x h ARGB words), and the update flag
// p+92 set, so the game itself copies it into the canvas.
inline void BeforeEditorUpdate(uint8_t* base, uint32_t p) {
  const bool on = REXCVAR_GET(wwe2k14_logo_import);
  const bool armed_now = on && !g_was_on;  // the setting turned on
  g_was_on = on;
  if (!on || !p) return;
  const auto file = File();
  std::error_code ec;
  const auto stamp = file.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file, ec);
  if (!armed_now && (ec || stamp == g_applied_stamp)) return;  // nothing new
  auto ld = [&](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
    return _byteswap_ulong(v);
  };
  const uint32_t o = ld(p + 16);
  if (!o) return;
  const uint32_t w = ld(o + 32), h = ld(o + 36);
  if (w == 0 || h == 0 || w > 1024 || h > 1024 || uint64_t(w) * h * 4 > 0x40000) return;  // not the 256x256 canvas
  g_applied_stamp = stamp;
  std::vector<uint8_t> bgra;
  std::string err;
  if (file.empty() || !std::filesystem::is_regular_file(file, ec) || !LoadPng(file, w, h, bgra, err)) {
    REXLOG_WARN("[wwe2k14][logo_import] {}: {}", file.string(), err.empty() ? "not found" : err);
    return;
  }
  for (const uint32_t image : {96u, 0x40068u, 0x80070u}) {
    uint8_t* dst = rex::memory::GuestPtr(base, p + image + 8);
    for (size_t p = 0; p < size_t(w) * h; ++p) {
      const uint8_t* s = &bgra[p * 4];  // B G R A
      const uint32_t argb = (uint32_t(s[3]) << 24) | (uint32_t(s[2]) << 16) | (uint32_t(s[1]) << 8) | s[0];
      const uint32_t be = _byteswap_ulong(argb);
      std::memcpy(dst + p * 4, &be, 4);
    }
  }
  if (ld(p + 92) == 0) {  // the update flag: the game copies the image into the canvas
    const uint32_t one = _byteswap_ulong(1u);
    std::memcpy(rex::memory::GuestPtr(base, p + 92), &one, 4);
  }
  REXLOG_INFO("[wwe2k14][logo_import] {} -> the Create-a-Logo editor's image (editor 0x{:08X}, canvas 0x{:08X}, {}x{})",
              file.string(), p, o, w, h);
}

}  // namespace wwe2k14::logo_import

REX_HOOK_RAW(sub_833A8730) {  // the logo editor's image -> canvas update (see the header)
  wwe2k14::logo_import::BeforeEditorUpdate(base, ctx.r3.u32);
  __imp__sub_833A8730(ctx, base);
}
