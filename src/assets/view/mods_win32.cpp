// Session 98 (plan step 14): the Windows side of the mods panel (ui_mods.h RegisterModsPlatform): one video frame through Media
// Foundation (like src\launcher\launcher_core.h MfInfo / src\tron_export.h), the file picker, the Recycle Bin, Explorer.
// Windows only; ui_mods.cpp and mods_lookup.h stay portable.
#include <windows.h>
#include <commdlg.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../mods_lookup.h"
#include "ui_mods.h"

namespace {

namespace ml = assets::mods_lookup;

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

template <class T>
struct Com {
  T* p = nullptr;
  ~Com() {
    if (p) p->Release();
  }
  T** put() { return &p; }
  T* operator->() const { return p; }
};

// One frame at about `at` seconds (clamped to the first half of the clip) as RGBA.
bool DecodeFrame(const std::filesystem::path& file, double at, ml::Frame& out, std::string& err) {
  static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
  if (!started) return err = "Media Foundation is not available", false;
  // The decode runs on a worker thread: its own COM apartment (the main thread's is STA from RegisterModsPlatform;
  // RPC_E_CHANGED_MODE just means the thread already has one).
  struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
      if (SUCCEEDED(hr)) CoUninitialize();
    }
  } com_scope;
  Com<IMFAttributes> attrs;
  if (FAILED(MFCreateAttributes(attrs.put(), 1)) || !attrs.p) return err = "MFCreateAttributes failed", false;
  attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  Com<IMFSourceReader> r;
  HRESULT hr = MFCreateSourceReaderFromURL(file.wstring().c_str(), attrs.p, r.put());
  if (FAILED(hr)) {
    char b[96];
    std::snprintf(b, sizeof b, "Windows can't open the video (0x%08lX)", static_cast<unsigned long>(hr));
    return err = b, false;
  }
  r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  r->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
  Com<IMFMediaType> t;
  MFCreateMediaType(t.put());
  t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.p);
  if (FAILED(hr)) return err = "the video can't be converted to RGB", false;
  Com<IMFMediaType> cur;
  if (FAILED(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, cur.put()))) return err = "no video stream", false;
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(cur.p, MF_MT_FRAME_SIZE, &w, &h);
  if (!w || !h || w > 8192 || h > 8192) return err = "bad video size", false;
  INT32 stride = INT32(w) * 4;
  UINT32 sv = 0;
  if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &sv))) stride = INT32(sv);
  double seconds = 0;
  PROPVARIANT var;
  PropVariantInit(&var);
  if (SUCCEEDED(r->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) seconds = double(var.uhVal.QuadPart) * 1e-7;
  PropVariantClear(&var);
  const LONGLONG target = LONGLONG(std::min(at, seconds * 0.5) * 1e7);
  if (target > 0) {
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = target;
    r->SetCurrentPosition(GUID_NULL, pos);
  }
  for (int tries = 0; tries < 160; ++tries) {
    DWORD flags = 0;
    LONGLONG ts = 0;
    Com<IMFSample> s;
    if (FAILED(r->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, s.put())) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
    if (!s.p) continue;
    if (ts + 400000 < target && tries < 150) continue;  // the seek lands on a key frame before the target
    Com<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(b.put()))) continue;
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(b->Lock(&data, nullptr, &len))) continue;
    const size_t astride = size_t(stride < 0 ? -stride : stride);
    if (len < astride * h) {
      b->Unlock();
      continue;
    }
    out.w = int(w), out.h = int(h);
    out.rgba.resize(size_t(w) * h * 4);
    for (UINT32 y = 0; y < h; ++y) {
      const BYTE* src = data + size_t(stride < 0 ? h - 1 - y : y) * astride;  // negative stride: the first row in memory is the bottom one
      uint8_t* dst = &out.rgba[size_t(y) * w * 4];
      for (UINT32 x = 0; x < w; ++x) dst[4 * x] = src[4 * x + 2], dst[4 * x + 1] = src[4 * x + 1], dst[4 * x + 2] = src[4 * x], dst[4 * x + 3] = 255;
    }
    b->Unlock();
    return true;
  }
  return err = "no frame could be read from the video", false;
}

std::string PickVideo() {
  wchar_t buf[MAX_PATH * 4] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFilter = L"Video files (*.mp4;*.mov;*.mkv)\0*.mp4;*.mov;*.mkv\0All files\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = DWORD(sizeof buf / sizeof buf[0]);
  ofn.lpstrTitle = L"Choose a video for this tron";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) return {};
  return Utf8(buf);
}

std::string PickCharacter() {
  wchar_t buf[MAX_PATH * 4] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFilter = L"Character files (*.pac;*.zip)\0*.pac;*.zip\0All files\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = DWORD(sizeof buf / sizeof buf[0]);
  ofn.lpstrTitle = L"Choose a custom character (.pac or .zip)";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) return {};
  return Utf8(buf);
}

// Session 98 (Superstar Importer, bulk import): several .pac / .zip at once (the dialog's multi-select: folder, then the names).
std::vector<std::string> PickCharacters() {
  std::vector<wchar_t> buf(64 * 1024, L'\0');
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFilter = L"Character files (*.pac;*.zip)\0*.pac;*.zip\0All files\0*.*\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = DWORD(buf.size());
  ofn.lpstrTitle = L"Choose characters: a zip of pac files, or several .pac / .zip files";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
  std::vector<std::string> out;
  if (!GetOpenFileNameW(&ofn)) return out;
  const std::wstring first(buf.data());
  const wchar_t* p = buf.data() + first.size() + 1;
  if (!*p) return {Utf8(first)};  // one file: the full path
  for (; *p; p += wcslen(p) + 1) out.push_back(Utf8((std::filesystem::path(first) / p).wstring()));
  return out;
}

std::string PickImage() {
  wchar_t buf[MAX_PATH * 4] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFilter = L"Pictures (*.png;*.jpg;*.jpeg)\0*.png;*.jpg;*.jpeg\0All files\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = DWORD(sizeof buf / sizeof buf[0]);
  ofn.lpstrTitle = L"Choose a roster picture (PNG with a transparent background works best)";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) return {};
  return Utf8(buf);
}

bool Recycle(const std::filesystem::path& file, std::string& err) {
  std::wstring from = file.wstring();
  from.push_back(L'\0');  // SHFileOperation wants a double-null list
  SHFILEOPSTRUCTW op = {};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  const int rc = SHFileOperationW(&op);
  if (rc != 0 || op.fAnyOperationsAborted) {
    err = "Recycle Bin error " + std::to_string(rc);
    return false;
  }
  return true;
}

void OpenFolder(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  ShellExecuteW(nullptr, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

namespace view {
namespace ui {

void RegisterModsPlatform() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // main thread: the file picker, SHFileOperation and Media Foundation need COM (once; never uninitialised)
  ml::Platform& p = ml::Plat();
  p.decode = DecodeFrame;
  p.pick_video = PickVideo;
  p.pick_character = PickCharacter;
  p.pick_characters = PickCharacters;  // Session 98 (Superstar Importer)
  p.pick_image = PickImage;
  p.recycle = Recycle;
  p.open_folder = OpenFolder;
  if (const char* la = std::getenv("LOCALAPPDATA")) p.cache_dir = std::filesystem::path(la) / "wwe2k14-asset-viewer" / "mod_frames";
}

}  // namespace ui
}  // namespace view
