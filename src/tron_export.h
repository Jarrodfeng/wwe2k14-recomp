// wwe2k14 - tron preview export (Session 17 launcher 2.0, default off).
//
// --wwe2k14_tron_export=DIR   once the game has booted far enough to play movies (guest frame
//                             --wwe2k14_tron_export_frame, default 900; the title logo is a Bink movie at ~450), decode
//                             every titantron (GAME:\movies\titantron\*.bik2) with the game's OWN Bink library and write
//                             DIR\NNN.png (the first frame that is not black: many trons fade in) and DIR\NNN.mp4 (a faithful copy:
//                             every frame at the file's own rate, the whole video, the original size; H.264 through Windows Media
//                             Foundation) plus DIR\index.json, then quit. The launcher's "Build previews" runs this and
//                             shows the results (thumbnails, hover playback). Nothing here touches the game's files; the
//                             outputs are made from the user's own disc, so they belong in the user's own folders.
// --wwe2k14_tron_export_only=343,324   only these trons (testing).
// --wwe2k14_tron_export_media_at=2,5.5  also write frames at these times (seconds) as NNN_tNN.png / <GROUP>_<Mnnn>_tNN.png (tron
//                             sheets and stage movies), so an A/B against a game picture can take the frame of its moment.
// A plain export (no _only filters) writes every tron NNN.png/.mp4 and every pacevtevtmov*.pac movie (MOV, MOVT, MVSW, MVSB,
// MVAB, MOVN, MOVL: all wrestlers) to DIRevtmovGROUP_Mnnn.png, the layout the asset viewer (srcssetsstage_media.h) reads.
//
// The Bink calls are the ones the movie module uses (src\tron_shim.h header): BinkSetIOSize / BinkSetIO (the game's IO
// provider 0x83598F78) + BinkOpen with BINKIOSIZE | BINKIOPROCESSOR | BINKNOFRAMEBUFFERS (0x03000400, as the movie
// module's file opens, without the sound-track flag), BinkGetFrameBuffersInfo, our own plane buffers in guest memory,
// BinkRegisterFrameBuffers, then BinkDoFrame / BinkNextFrame per frame and BinkClose. They run on the guest thread in
// VdSwap (the game is paused meanwhile; the window shows the last frame). Planes: Y full size, cR / cB half size,
// 8 bits; converted with BT.601 limited range for the PNG, passed as NV12 to the encoder.

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <codecapi.h>          // CODECAPI_AVEncMPVGOPSize (Session 98: short GOPs so the viewer's seeks stay short)
#elif defined(__ANDROID__)
#include <fcntl.h>             // Session 98 (phone tron video): the MP4s through the NDK's encoder + muxer (libmediandk)
#include <unistd.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <thread>
#endif

#include "android_lifecycle.h"  // Session 98: WriteExitRequest (the export's end, straight from the guest thread)
#include "diag_jpeg.h"        // Session 98 (phone): the PNGs off Windows (stb_image_write)
#include "input_script.h"     // SavePngWic
#include "kernel_overrides.h"  // g_vdswap_hook
#include "tron_shim.h"         // Ld32 / St32, the Bink function symbols, MF startup

REXCVAR_DEFINE_STRING(wwe2k14_tron_export, "", "wwe2k14",
                      "Tron previews (Session 17): decode every titantron with the game's Bink into DIR (NNN.png first "
                      "frame + NNN.mp4 preview + index.json), then quit. See src/tron_export.h");
REXCVAR_DEFINE_STRING(wwe2k14_tron_export_only, "", "wwe2k14", "Tron previews: only these tron numbers (a,b,c)");
REXCVAR_DEFINE_BOOL(wwe2k14_tron_export_debug, false, "wwe2k14",
                    "Tron previews: log the first frames' buffer bookkeeping (index before / after, mean Y of each buffer)");
REXCVAR_DEFINE_INT32(wwe2k14_tron_export_frame, 900, "wwe2k14", "Tron previews: start at this guest frame");
REXCVAR_DEFINE_INT32(wwe2k14_tron_export_seconds, 0, "wwe2k14",
                     "Tron previews: length in seconds (0 = the whole video, the default since Jarrod's 'faithful')");
REXCVAR_DEFINE_INT32(wwe2k14_tron_export_fps, 0, "wwe2k14",
                     "Tron previews: frame rate (0 = the file's own, every frame: the default)");
REXCVAR_DEFINE_INT32(wwe2k14_tron_export_max_width, 0, "wwe2k14",
                     "Tron previews: halve the picture until it is at most this wide (0 = the original size: default)");

REXCVAR_DEFINE_BOOL(wwe2k14_tron_export_trons, true, "wwe2k14",
                    "Tron previews: export the loose titantrons (false = only the stage movies below)");
REXCVAR_DEFINE_BOOL(wwe2k14_tron_export_media, true, "wwe2k14",
                    "Tron previews (Session 97): also decode the stage movies in pac/evt/evtmov*.pac (tron screens, "
                    "arena header / name plate art) into DIR/evtmov/GROUP_Mnnn.png. Skipped when "
                    "--wwe2k14_tron_export_only names trons and no media_only list is given");
REXCVAR_DEFINE_BOOL(wwe2k14_tron_export_media_mp4, false, "wwe2k14",
                    "Tron previews (Session 98): also write every multi-frame stage movie as DIR/evtmov/GROUP_Mnnn.mp4 (the whole "
                    "movie; alpha ones flattened over black), which the asset viewer plays on the walls / pillars / mini-tron. "
                    "Default false (harness / census / reference runs only want the stills); the launcher's Build previews passes true");
REXCVAR_DEFINE_STRING(wwe2k14_tron_export_media_only, "", "wwe2k14",
                      "Tron previews: only these stage movies (GROUP_Mnnn,GROUP_Mnnn e.g. MOVL_M005,MOVN_M005)");

REXCVAR_DEFINE_STRING(wwe2k14_tron_export_media_at, "", "wwe2k14",
                      "Tron previews (Session 97): extra frames at these times in seconds (a,b,c e.g. 2,5.5), written as "
                      "NNN_tNN.png / GROUP_Mnnn_tNN.png (NN = whole seconds, 02; a fraction adds p and the tenths: 02p5), so an "
                      "A/B can pick the frame of the game's moment. Applies to the trons and to every stage movie");

REX_EXTERN(sub_83599C58);  // BinkSetIOSize
REX_EXTERN(sub_83599C68);  // BinkSetIO
REX_EXTERN(sub_83599390);  // BinkGetError

namespace wwe2k14::tron_export {

// Session 98 (first phone test: "the tron loading doesn't work", the Arena Viewer's VIDEOS / stage walls had no pictures on the phone):
// the export runs on the phone too, PNGs only - the phone launcher's BUILD TRON PREVIEWS starts the game with
// --wwe2k14_tron_export=<files>/trons, like the PC launcher's Build previews. Off Windows there is no Media Foundation: first no MP4s (the
// phone viewer showed the stills; below: the NDK encoder now), each tron decoded only up to its thumbnail (and any _media_at frames), and the exit goes through
// g_request_quit, which on Android brings the launcher back (wwe2k14_app.h OnClosing).
// Session 98 (phone tron video, Jarrod: "Titantrons and other videos don't seem to reliably play when entrance mode is selected, often
// stuck on the first frame"): on Android the MP4s are written too, through the NDK's H.264 encoder (AMediaCodec) and MP4 muxer
// (AMediaMuxer), same names / layout as the PC, so the phone viewer plays them (view\video_player_android.cpp). The stills stay the
// result there (kStillsAreResult): a phone whose encoder refuses a size still gets its pictures, and decoding then stops after the
// thumbnail as before.
#if defined(_WIN32)
inline constexpr bool kHaveMp4 = true;
inline constexpr bool kStillsAreResult = false;
#elif defined(__ANDROID__)
inline constexpr bool kHaveMp4 = true;
inline constexpr bool kStillsAreResult = true;
#else
inline constexpr bool kHaveMp4 = false;
inline constexpr bool kStillsAreResult = true;
#endif
using tron_shim::GP;
using tron_shim::Ld32;
using tron_shim::St32;

inline void (*g_prev_hook)(uint64_t) = nullptr;
inline bool g_done = false;
inline uint64_t g_media_mp4_bytes = 0, g_media_mp4_count = 0;  // Session 98: the stage-movie MP4s written this run

// ---- Session 98 (Jarrod: "building tron previews definitely needs a progress bar ... a notification that lists the progress";
// "very poor user experience right now"): where each item's time goes, and how far the run is.
//
// Timings: each item's wall time split by phase (steady_clock around the calls, nanoseconds, the current item in g_ph):
//   open    BinkOpen + the frame-buffer setup (the trons' file is read by Bink's IO inside decode, not here)
//   read    stage movies only: the movie read out of its evtmov pac + the word swap into guest memory
//   decode  BinkDoFrame + BinkNextFrame (the game's recompiled Bink; for the trons this includes Bink's own file reads)
//   convert the planes copied out of guest memory, Shrink, the stacked-alpha / over-black frames, the thumbnail / coverage sums
//   encode  Mp4Writer::Write minus the muxer's share (the phone: AMediaCodec queue / dequeue; the PC: the whole sink writer)
//   mux     the phone only: AMediaMuxer_addTrack / start / writeSampleData (the file writes of the MP4)
//   png     WritePng (RGB conversion + the PNG encoder + the file write)
//   close   Mp4Writer::Close minus the muxer's share (end of stream, muxer stop) + the rename
// written as one log line per item and <DIR>/timings.json (one row per item + totals; rewritten every 10 items and at the end).
// Progress: <DIR>/progress.json (renamed into place) at every item and about once a second inside one, for the game's screen
// (GameActivity), the phone's notification (TronExportService) and the launcher. Nothing here changes how the export runs.
using SteadyClock = std::chrono::steady_clock;
inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count());
}
struct Phases {
  uint64_t open = 0, read = 0, decode = 0, convert = 0, encode = 0, mux = 0, png = 0, close = 0;
};
inline Phases g_ph;  // the item being exported
struct Timed {       // adds the scope's time to `acc`
  uint64_t& acc;
  uint64_t t0;
  explicit Timed(uint64_t& a) : acc(a), t0(NowNs()) {}
  ~Timed() { acc += NowNs() - t0; }
};
// Runs f, adds its time minus the muxer's share (counted in g_ph.mux meanwhile) to `acc`.
template <class F>
inline auto TimedLessMux(uint64_t& acc, F&& f) {
  const uint64_t m0 = g_ph.mux, t0 = NowNs();
  auto r = f();
  const uint64_t dt = NowNs() - t0, dm = g_ph.mux - m0;
  acc += dt > dm ? dt - dm : 0;
  return r;
}

struct Progress {
  std::filesystem::path dir;
  int total = 0, done = 0, ok = 0, trons = 0, media = 0;
  std::string phase, item;          // "titantrons" / "stage movies"; "tron 343" / "MOVL_M005"
  uint32_t item_frame = 0, item_frames = 0;
  std::filesystem::path part;       // the MP4 being written (its size counts toward mb)
  uint64_t bytes = 0;               // the finished items' files
  uint64_t t0 = 0, last_write = 0;  // NowNs
  std::string state = "running", message;
  Phases sum;                       // all finished items
};
inline Progress g_prog;

inline std::string JsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}
inline double Ms(uint64_t ns) { return double(ns) / 1e6; }

inline void WriteProgress() {
  Progress& p = g_prog;
  if (p.dir.empty()) return;
  const uint64_t now = NowNs();
  p.last_write = now;
  std::error_code ec;
  uint64_t bytes = p.bytes;
  if (!p.part.empty()) {
    const auto s = std::filesystem::file_size(p.part, ec);
    if (!ec) bytes += s;
  }
  const double elapsed = double(now - p.t0) / 1e9;
  // ETA: the time per finished item so far times the items left (items differ a lot in length: rough until a few are done)
  const double eta = p.done >= 3 && p.total > p.done ? elapsed / p.done * (p.total - p.done) : -1;
  const auto wall = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::ostringstream o;
  o << "{\"state\": \"" << p.state << "\", \"phase\": \"" << JsonEsc(p.phase) << "\", \"done\": " << p.done << ", \"total\": " << p.total
    << ", \"ok\": " << p.ok << ", \"trons\": " << p.trons << ", \"media\": " << p.media << ", \"item\": \"" << JsonEsc(p.item)
    << "\", \"item_frame\": " << p.item_frame << ", \"item_frames\": " << p.item_frames << ", \"mb\": " << double(bytes) / 1048576.0
    << ", \"elapsed_s\": " << elapsed << ", \"eta_s\": " << eta << ", \"updated\": " << wall << ", \"message\": \"" << JsonEsc(p.message)
    << "\"}\n";
  const std::filesystem::path tmp = p.dir / "progress.json.tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    f << o.str();
  }
  std::filesystem::rename(tmp, p.dir / "progress.json", ec);
}
// Inside an item, once per decoded frame: rewrites progress.json about once a second.
inline void Tick(uint32_t frame, uint32_t frames) {
  g_prog.item_frame = frame, g_prog.item_frames = frames;
  if (NowNs() - g_prog.last_write >= 1000000000ull) WriteProgress();
}
inline void StartItem(const std::string& phase, const std::string& item, const std::filesystem::path& part) {
  g_ph = Phases{};
  g_prog.phase = phase, g_prog.item = item, g_prog.part = part, g_prog.item_frame = 0, g_prog.item_frames = 0;
  WriteProgress();
}

struct TimingRow {
  std::string kind, name;
  bool ok = false;
  uint32_t w = 0, h = 0, frames = 0, decoded = 0, encoded = 0;
  uint64_t bytes = 0, total_ns = 0;
  Phases ph;
};
inline std::vector<TimingRow> g_rows;

inline void WriteTimings(bool final_write) {
  if (g_prog.dir.empty()) return;
  auto phases = [](std::ostringstream& o, const Phases& p, uint64_t total) {
    const uint64_t known = p.open + p.read + p.decode + p.convert + p.encode + p.mux + p.png + p.close;
    o << "\"open_ms\": " << Ms(p.open) << ", \"read_ms\": " << Ms(p.read) << ", \"decode_ms\": " << Ms(p.decode)
      << ", \"convert_ms\": " << Ms(p.convert) << ", \"encode_ms\": " << Ms(p.encode) << ", \"mux_ms\": " << Ms(p.mux)
      << ", \"png_ms\": " << Ms(p.png) << ", \"close_ms\": " << Ms(p.close) << ", \"other_ms\": " << Ms(total > known ? total - known : 0)
      << ", \"total_ms\": " << Ms(total);
  };
  std::ostringstream o;
  o << "{\n  \"made_by\": \"wwe2k14 --wwe2k14_tron_export timings (src/tron_export.h)\",\n";
#if defined(__ANDROID__)
  o << "  \"platform\": \"android\",\n";
#elif defined(_WIN32)
  o << "  \"platform\": \"windows\",\n";
#else
  o << "  \"platform\": \"other\",\n";
#endif
  o << "  \"complete\": " << (final_write ? "true" : "false") << ",\n  \"items\": [";
  uint64_t items_ns = 0, frames = 0, decoded = 0, encoded = 0, bytes = 0;
  for (size_t i = 0; i < g_rows.size(); ++i) {
    const TimingRow& r = g_rows[i];
    items_ns += r.total_ns, frames += r.frames, decoded += r.decoded, encoded += r.encoded, bytes += r.bytes;
    o << (i ? "," : "") << "\n    {\"kind\": \"" << r.kind << "\", \"name\": \"" << JsonEsc(r.name) << "\", \"ok\": " << (r.ok ? "true" : "false")
      << ", \"width\": " << r.w << ", \"height\": " << r.h << ", \"frames\": " << r.frames << ", \"decoded\": " << r.decoded
      << ", \"encoded\": " << r.encoded << ", \"bytes\": " << r.bytes << ", ";
    phases(o, r.ph, r.total_ns);
    o << "}";
  }
  const uint64_t wall = NowNs() - g_prog.t0;
  o << "\n  ],\n  \"totals\": {\"items\": " << g_rows.size() << ", \"ok\": " << g_prog.ok << ", \"frames\": " << frames
    << ", \"decoded\": " << decoded << ", \"encoded\": " << encoded << ", \"bytes\": " << bytes << ", \"wall_ms\": " << Ms(wall) << ", ";
  phases(o, g_prog.sum, items_ns);
  o << "}\n}\n";
  std::error_code ec;
  const std::filesystem::path tmp = g_prog.dir / "timings.json.tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    f << o.str();
  }
  std::filesystem::rename(tmp, g_prog.dir / "timings.json", ec);
}

// Ends the item started by StartItem: its row, the log line, the running sums, the bytes it wrote (`outputs`).
inline void EndItem(const std::string& kind, const std::string& name, bool ok, uint32_t w, uint32_t h, uint32_t frames,
                    uint32_t decoded, uint32_t encoded, uint64_t t0, const std::vector<std::filesystem::path>& outputs) {
  TimingRow r;
  r.kind = kind, r.name = name, r.ok = ok, r.w = w, r.h = h, r.frames = frames, r.decoded = decoded, r.encoded = encoded;
  r.total_ns = NowNs() - t0, r.ph = g_ph;
  std::error_code ec;
  for (const auto& p : outputs) {
    const auto s = std::filesystem::file_size(p, ec);
    if (!ec) r.bytes += s;
  }
  Phases& s = g_prog.sum;
  s.open += r.ph.open, s.read += r.ph.read, s.decode += r.ph.decode, s.convert += r.ph.convert, s.encode += r.ph.encode;
  s.mux += r.ph.mux, s.png += r.ph.png, s.close += r.ph.close;
  REXLOG_INFO("[wwe2k14][tron_export] timing {} {}: {}x{}, {} frames ({} decoded, {} encoded), {} bytes; {:.0f} ms = open {:.0f}, read {:.0f}, "
              "decode {:.0f}, convert {:.0f}, encode {:.0f}, mux {:.0f}, png {:.0f}, close {:.0f}",
              kind, name, w, h, frames, decoded, encoded, r.bytes, Ms(r.total_ns), Ms(r.ph.open), Ms(r.ph.read), Ms(r.ph.decode),
              Ms(r.ph.convert), Ms(r.ph.encode), Ms(r.ph.mux), Ms(r.ph.png), Ms(r.ph.close));
  g_prog.bytes += r.bytes, g_prog.part.clear(), ++g_prog.done, g_prog.ok += ok;
  g_rows.push_back(std::move(r));
  WriteProgress();
  if (g_rows.size() % 10 == 0) WriteTimings(false);
}

struct Planes {  // one decoded frame on the host
  uint32_t w = 0, h = 0;          // visible size
  std::vector<uint8_t> y, u, v;   // y: w*h; u (cB) / v (cR): (w/2)*(h/2), w and h even
  std::vector<uint8_t> a;         // w*h opacity (0 = clear) when the movie has alpha (the title reveals), else empty
};

// Session 18: a movie with alpha is previewed as a "stacked alpha" frame (colour on top, the alpha as grey below,
// limited range) so any MP4 player shows it and the launcher can rebuild the transparency.
inline Planes Stack(const Planes& f) {
  Planes g;
  g.w = f.w, g.h = f.h * 2;
  g.y = f.y, g.u = f.u, g.v = f.v;
  g.y.resize(size_t(g.w) * g.h);
  for (size_t i = 0; i < f.a.size(); ++i) g.y[f.y.size() + i] = uint8_t(16 + (f.a[i] * 219 + 127) / 255);
  g.u.resize(size_t(g.w / 2) * (g.h / 2), 128), g.v.resize(g.u.size(), 128);
  return g;
}

// Session 98: an alpha frame composited over black (limited-range YUV: black = Y 16, U / V 128), the alpha dropped. The chroma takes the
// alpha of the top-left pixel of each 2x2 block.
inline Planes FlattenOverBlack(const Planes& f) {
  Planes g = f;
  g.a.clear();
  for (size_t i = 0; i < f.y.size(); ++i) g.y[i] = uint8_t(16 + (int(f.y[i]) - 16 > 0 ? (int(f.y[i]) - 16) * f.a[i] / 255 : 0));
  const uint32_t cw = f.w / 2, ch = f.h / 2;
  for (uint32_t y = 0; y < ch; ++y)
    for (uint32_t x = 0; x < cw; ++x) {
      const int a = f.a[size_t(2 * y) * f.w + 2 * x];
      const size_t i = size_t(y) * cw + x;
      g.u[i] = uint8_t(128 + (int(f.u[i]) - 128) * a / 255);
      g.v[i] = uint8_t(128 + (int(f.v[i]) - 128) * a / 255);
    }
  return g;
}

// Calls a guest function with up to 4 word arguments; returns r3.
inline uint32_t Call(void (*fn)(PPCContext&, uint8_t*), uint8_t* base, uint32_t a0 = 0, uint32_t a1 = 0,
                     uint32_t a2 = 0, uint32_t a3 = 0) {
  PPCContext& ctx = *rex::runtime::ThreadState::Get()->context();
  const PPCContext saved = ctx;
  ctx.r3.u64 = a0, ctx.r4.u64 = a1, ctx.r5.u64 = a2, ctx.r6.u64 = a3;
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

inline bool WritePng(const std::filesystem::path& path, const Planes& f) {
  Timed timed(g_ph.png);
  rex::ui::RawImage img;
  img.width = f.w, img.height = f.h, img.stride = f.w * 4;
  img.data.resize(size_t(img.stride) * f.h);
  for (uint32_t yy = 0; yy < f.h; ++yy) {
    for (uint32_t xx = 0; xx < f.w; ++xx) {
      const float Y = 1.164f * (float(f.y[size_t(yy) * f.w + xx]) - 16.0f);
      const size_t ci = size_t(yy / 2) * (f.w / 2) + xx / 2;
      const float U = float(f.u[ci]) - 128.0f, V = float(f.v[ci]) - 128.0f;
      uint8_t* d = img.data.data() + size_t(yy) * img.stride + xx * 4;
      d[0] = uint8_t(std::clamp(Y + 1.596f * V, 0.0f, 255.0f));
      d[1] = uint8_t(std::clamp(Y - 0.392f * U - 0.813f * V, 0.0f, 255.0f));
      d[2] = uint8_t(std::clamp(Y + 2.017f * U, 0.0f, 255.0f));
      d[3] = f.a.empty() ? 255 : f.a[size_t(yy) * f.w + xx];
    }
  }
#if defined(_WIN32)
  return SavePngWic(path, img, !f.a.empty());
#else
  // (SavePngWic is the diagnostic run's JPEG path off Windows and writes nothing outside it: stb_image_write, alpha kept)
  return diag::WritePngRgba(path, img.data.data(), img.width, img.height, img.stride, !f.a.empty());
#endif
}

#if defined(_WIN32)
// H.264 MP4 through Media Foundation's sink writer, NV12 frames in.
class Mp4Writer {
 public:
  // fps_num / fps_den: the exact rate (30000 / 1001 for the trons); sample times are computed from it, not rounded.
  bool Open(const std::filesystem::path& path, uint32_t w, uint32_t h, uint32_t fps_num, uint32_t fps_den) {
    w_ = w, h_ = h, num_ = fps_num, den_ = fps_den;
    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 2);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);  // the name ends in .part
    HRESULT hr = MFCreateSinkWriterFromURL(path.wstring().c_str(), nullptr, attrs, &writer_);
    attrs->Release();
    if (FAILED(hr)) return false;
    IMFMediaType* out = nullptr;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    // ~0.3 bits per pixel per frame, at least 1 Mbps: 320x320 at 29.97 -> 1 Mbps, 880x720 -> 5.7 Mbps.
    out->SetUINT32(MF_MT_AVG_BITRATE,
                   std::max<uint32_t>(1000000, uint32_t(double(w) * h * fps_num / fps_den * 0.3)));
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out, MF_MT_FRAME_RATE, fps_num, fps_den);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer_->AddStream(out, &stream_);
    out->Release();
    if (FAILED(hr)) return false;
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, fps_num, fps_den);
    MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // Session 98: a key frame about every second (GOP = the frame rate), so a seek in the viewer decodes at most ~1 s forward.
    IMFAttributes* enc = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&enc, 1)) && enc)
      enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, std::max<uint32_t>(1, (fps_num + fps_den / 2) / std::max<uint32_t>(1, fps_den)));
    hr = writer_->SetInputMediaType(stream_, in, enc);
    if (enc) enc->Release();
    in->Release();
    if (FAILED(hr)) return false;
    return SUCCEEDED(writer_->BeginWriting());
  }
  bool Write(const Planes& f) {
    const DWORD size = w_ * h_ * 3 / 2;
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateMemoryBuffer(size, &buf))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    std::memcpy(p, f.y.data(), size_t(w_) * h_);
    uint8_t* uv = p + size_t(w_) * h_;
    for (size_t i = 0; i < size_t(w_ / 2) * (h_ / 2); ++i) uv[2 * i] = f.u[i], uv[2 * i + 1] = f.v[i];
    buf->Unlock();
    buf->SetCurrentLength(size);
    IMFSample* s = nullptr;
    MFCreateSample(&s);
    s->AddBuffer(buf);
    const LONGLONG t0 = n_ * 10000000LL * den_ / num_, t1 = (n_ + 1) * 10000000LL * den_ / num_;
    s->SetSampleTime(t0);
    s->SetSampleDuration(t1 - t0);
    const HRESULT hr = writer_->WriteSample(stream_, s);
    s->Release();
    buf->Release();
    ++n_;
    return SUCCEEDED(hr);
  }
  bool Close() {
    if (!writer_) return false;
    const HRESULT hr = writer_->Finalize();
    writer_->Release();
    writer_ = nullptr;
    return SUCCEEDED(hr);
  }
  ~Mp4Writer() {
    if (writer_) writer_->Release();
  }

 private:
  IMFSinkWriter* writer_ = nullptr;
  DWORD stream_ = 0;
  uint32_t w_ = 0, h_ = 0;
  LONGLONG num_ = 30000, den_ = 1001, n_ = 0;
};
#elif defined(__ANDROID__)
// Session 98 (phone tron video): H.264 MP4 through the NDK. AMediaCodec "video/avc" encoder fed NV12 (COLOR_FormatYUV420SemiPlanar = 21)
// in byte buffers laid out by the encoder's own input format ("stride" / "slice-height", API 28), AMediaMuxer MPEG-4 out. A key frame
// every second (as the PC's GOP). Rate: 0.2 bits per pixel per frame, at least 0.6 Mbps (the PC: 0.3 / 1 Mbps) - the phone's storage:
// ~0.8 GB for every tron and stage movie instead of ~1.2 GB (estimate from the PC's index sizes, not measured on a phone).
class Mp4Writer {
 public:
  bool Open(const std::filesystem::path& path, uint32_t w, uint32_t h, uint32_t fps_num, uint32_t fps_den) {
    Reset();
    w_ = w, h_ = h, num_ = std::max<uint32_t>(1, fps_num), den_ = std::max<uint32_t>(1, fps_den);
    if (w < 16 || h < 16 || (w & 1) || (h & 1)) return Fail("bad size");
    fd_ = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd_ < 0) return Fail("could not create the file");
    muxer_ = AMediaMuxer_new(fd_, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!muxer_) return Fail("AMediaMuxer_new failed");
    codec_ = AMediaCodec_createEncoderByType("video/avc");
    if (!codec_) return Fail("no H.264 encoder");
    AMediaFormat* f = AMediaFormat_new();
    const double fps = double(num_) / den_;
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, int32_t(w));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, int32_t(h));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);  // COLOR_FormatYUV420SemiPlanar (NV12)
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, int32_t(std::max(600000.0, double(w) * h * fps * 0.2)));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, std::max(1, int(fps + 0.5)));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    media_status_t st = AMediaCodec_configure(codec_, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) return Fail("the encoder refused the format");
    if (AMediaCodec_start(codec_) != AMEDIA_OK) return Fail("the encoder did not start");
    started_ = true;
    // The input layout: the encoder's own stride / slice-height. Review: no guessing a tight packing when they are missing - the
    // slice height is then padded to 16 rows (the macroblock height; 1280x280 -> 288), and a buffer it does not fit fails the movie.
    stride_ = int32_t(w), slice_ = int32_t((h + 15) & ~15u);
    if (AMediaFormat* in = AMediaCodec_getInputFormat(codec_)) {
      int32_t v = 0;
      if (AMediaFormat_getInt32(in, "stride", &v) && v >= int32_t(w)) stride_ = v;
      if (AMediaFormat_getInt32(in, "slice-height", &v) && v >= int32_t(h)) slice_ = v;
      AMediaFormat_delete(in);
    }
    return true;
  }
  bool Write(const Planes& f) {
    if (!started_ || failed_ || f.w != w_ || f.h != h_) return false;
    ssize_t idx = -1;
    const auto t0 = std::chrono::steady_clock::now();
    while ((idx = AMediaCodec_dequeueInputBuffer(codec_, 2000)) < 0) {  // no free input: take the encoder's output meanwhile
      if (!Drain(false)) return Fail("muxer write failed");
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3)) return Fail("the encoder stopped taking frames");
    }
    size_t cap = 0;
    uint8_t* p = AMediaCodec_getInputBuffer(codec_, size_t(idx), &cap);
    const size_t stride = size_t(stride_), slice = size_t(slice_);
    const size_t need = stride * slice + stride * (slice / 2);  // Y plane, then the interleaved UV plane
    if (!p || cap < need) return Fail("the encoder's input buffer does not fit its layout");  // (the movie keeps its still)
    if (slice > h_) std::memset(p + stride * h_, 0, stride * (slice - h_));  // (padding rows: defined bytes)
    for (uint32_t y = 0; y < h_; ++y) std::memcpy(p + y * stride, &f.y[size_t(y) * w_], w_);
    uint8_t* uv = p + stride * slice;
    const uint32_t cw = w_ / 2;
    for (uint32_t y = 0; y < h_ / 2; ++y) {
      uint8_t* row = uv + y * stride;
      const uint8_t* u = &f.u[size_t(y) * cw];
      const uint8_t* v = &f.v[size_t(y) * cw];
      for (uint32_t x = 0; x < cw; ++x) row[2 * x] = u[x], row[2 * x + 1] = v[x];
    }
    const uint64_t pts = uint64_t(n_) * 1000000ull * den_ / num_;
    if (AMediaCodec_queueInputBuffer(codec_, size_t(idx), 0, need, pts, 0) != AMEDIA_OK) return Fail("queueInputBuffer failed");
    ++n_;
    return Drain(false) || Fail("muxer write failed");
  }
  bool Close() {
    bool ok = false;
    if (started_ && !failed_) {
      ssize_t idx = -1;
      const auto t0 = std::chrono::steady_clock::now();
      while ((idx = AMediaCodec_dequeueInputBuffer(codec_, 2000)) < 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3))
        Drain(false);
      if (idx >= 0) {
        const uint64_t pts = uint64_t(n_) * 1000000ull * den_ / num_;
        AMediaCodec_queueInputBuffer(codec_, size_t(idx), 0, 0, pts, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        ok = Drain(true) && eos_ && written_ > 0;
      }
    }
    if (muxer_ && muxing_) {
      Timed timed(g_ph.mux);
      ok = AMediaMuxer_stop(muxer_) == AMEDIA_OK && ok;
    }
    Reset();
    return ok;
  }
  ~Mp4Writer() { Reset(); }
  const std::string& Error() const { return err_; }

 private:
  // Takes what the encoder has finished into the muxer. `eos`: wait for the end of the stream (up to 5 s).
  bool Drain(bool eos) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      AMediaCodecBufferInfo info{};
      const ssize_t o = AMediaCodec_dequeueOutputBuffer(codec_, &info, eos ? 10000 : 0);
      if (o >= 0) {
        size_t cap = 0;
        uint8_t* data = AMediaCodec_getOutputBuffer(codec_, size_t(o), &cap);
        bool ok = true;
        if (data && info.size > 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {  // (the SPS / PPS travel in the format)
          if (!muxing_) ok = false;
          else {
            Timed timed(g_ph.mux);  // (Session 98 timings: the MP4's file writes)
            ok = AMediaMuxer_writeSampleData(muxer_, size_t(track_), data, &info) == AMEDIA_OK, written_ += ok;
          }
        }
        AMediaCodec_releaseOutputBuffer(codec_, size_t(o), false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return eos_ = true, ok;
        if (!ok) return false;
      } else if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        if (!muxing_) {
          Timed timed(g_ph.mux);
          AMediaFormat* of = AMediaCodec_getOutputFormat(codec_);
          track_ = of ? AMediaMuxer_addTrack(muxer_, of) : -1;
          if (of) AMediaFormat_delete(of);
          if (track_ < 0 || AMediaMuxer_start(muxer_) != AMEDIA_OK) return false;
          muxing_ = true;
        }
      } else if (o == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        if (!eos) return true;  // (nothing more finished now)
      } else if (o != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {  // (BUFFERS_CHANGED: nothing to do with getOutputBuffer)
        return Fail("the encoder reported an error");  // any other negative value: a codec error, never spin on it
      }
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) return Fail("the encoder stalled");  // (every branch)
    }
  }
  bool Fail(const char* why) {
    if (err_.empty()) err_ = why;
    failed_ = true;
    return false;
  }
  void Reset() {
    if (codec_) {
      if (started_) AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
    }
    if (muxer_) AMediaMuxer_delete(muxer_);
    if (fd_ >= 0) ::close(fd_);
    codec_ = nullptr, muxer_ = nullptr, fd_ = -1, started_ = muxing_ = eos_ = failed_ = false, track_ = -1, n_ = 0, written_ = 0;
  }
  AMediaCodec* codec_ = nullptr;
  AMediaMuxer* muxer_ = nullptr;
  int fd_ = -1;
  bool started_ = false, muxing_ = false, eos_ = false, failed_ = false;
  ssize_t track_ = -1;
  uint32_t w_ = 0, h_ = 0, num_ = 30000, den_ = 1001;
  int32_t stride_ = 0, slice_ = 0;
  uint64_t n_ = 0, written_ = 0;
  std::string err_;
};
#else
// Off Windows: no encoder (kHaveMp4 false); Open fails, so no MP4 is ever started.
class Mp4Writer {
 public:
  bool Open(const std::filesystem::path&, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
  bool Write(const Planes&) { return false; }
  bool Close() { return false; }
};
#endif

// Box-halves a frame until it is at most `max_w` wide (880x720 title videos -> 440x360); keeps sizes even.
inline Planes Shrink(Planes f, uint32_t max_w) {
  while (f.w > max_w && f.w >= 4 && f.h >= 4) {
    Planes g;
    g.w = (f.w / 2) & ~1u, g.h = (f.h / 2) & ~1u;
    g.y.resize(size_t(g.w) * g.h), g.u.resize(size_t(g.w / 2) * (g.h / 2)), g.v.resize(g.u.size());
    for (uint32_t y = 0; y < g.h; ++y)
      for (uint32_t x = 0; x < g.w; ++x) {
        const uint8_t* s = &f.y[size_t(2 * y) * f.w + 2 * x];
        g.y[size_t(y) * g.w + x] = uint8_t((s[0] + s[1] + s[f.w] + s[f.w + 1] + 2) / 4);
      }
    if (!f.a.empty()) {
      g.a.resize(g.y.size());
      for (uint32_t y = 0; y < g.h; ++y)
        for (uint32_t x = 0; x < g.w; ++x) {
          const uint8_t* s = &f.a[size_t(2 * y) * f.w + 2 * x];
          g.a[size_t(y) * g.w + x] = uint8_t((s[0] + s[1] + s[f.w] + s[f.w + 1] + 2) / 4);
        }
    }
    const uint32_t cw = f.w / 2;
    for (uint32_t y = 0; y < g.h / 2; ++y)
      for (uint32_t x = 0; x < g.w / 2; ++x) {
        const size_t i = size_t(2 * y) * cw + 2 * x;
        g.u[size_t(y) * (g.w / 2) + x] = uint8_t((f.u[i] + f.u[i + 1] + f.u[i + cw] + f.u[i + cw + 1] + 2) / 4);
        g.v[size_t(y) * (g.w / 2) + x] = uint8_t((f.v[i] + f.v[i + 1] + f.v[i + cw] + f.v[i + cw + 1] + 2) / 4);
      }
    f = std::move(g);
  }
  return f;
}

// --wwe2k14_tron_export_media_at: the frames (number, file suffix "_t02" / "_t02p5") of a movie at `fps` that are asked for.
inline std::vector<std::pair<uint32_t, std::string>> AtFrames(double fps, uint32_t frames) {
  std::vector<std::pair<uint32_t, std::string>> out;
  std::stringstream ss(REXCVAR_GET(wwe2k14_tron_export_media_at));
  for (std::string t; std::getline(ss, t, ',');) {
    if (t.empty()) continue;
    const double sec = std::atof(t.c_str());
    if (!(sec >= 0) || fps <= 0) continue;
    const uint32_t fr = std::min<uint32_t>(uint32_t(sec * fps + 0.5), frames ? frames - 1 : 0);
    const int tenths = int(sec * 10 + 0.5);
    char suf[32];
    if (tenths % 10 == 0) std::snprintf(suf, sizeof suf, "_t%02d", tenths / 10);
    else std::snprintf(suf, sizeof suf, "_t%02dp%d", tenths / 10, tenths % 10);
    out.push_back({fr, suf});
  }
  return out;
}

struct Result {
  int tron = 0;
  bool ok = false;
  uint32_t w = 0, h = 0, frames = 0, fps_num = 0, fps_den = 0, written = 0, thumb_frame = 0;
  uint32_t decoded = 0;  // Session 98 timings: BinkDoFrame calls
  bool alpha = false;  // the movie has an alpha plane: the MP4 is stacked alpha (twice as tall), the PNG has alpha
  double thumb_mean = 0;
  std::string error;
};

inline Result ExportOne(uint8_t* base, int tron, const std::filesystem::path& dir) {
  Result r;
  r.tron = tron;
  auto* mem = REX_KERNEL_MEMORY();
  uint64_t open_t0 = NowNs();  // (timings: open = BinkOpen .. BinkRegisterFrameBuffers)
  const std::string path = "GAME:\\movies\\titantron\\" + std::to_string(tron) + ".bik2";
  const uint32_t name = mem->SystemHeapAlloc(uint32_t(path.size() + 1));
  std::memcpy(GP(base, name), path.c_str(), path.size() + 1);
  Call(sub_83599C58, base, 0x400000);  // BinkSetIOSize (the movie slot's default)
  Call(sub_83599C68, base, 0x83598F78);  // BinkSetIO (the game's IO provider)
  // Session 18: files with alpha (header flag 0x100000: the title reveals) are opened with BINKALPHA (the same bit as
  // an open flag), else Bink decodes no alpha plane.
  const tron_shim::BinkHeader hd = tron_shim::ReadGameBinkHeader(tron);  // Session 26: through GAME: (ISO too)
  const uint32_t h = Call(sub_8359E140, base, name, 0x03000400 | (hd.flags & 0x00100000u));
  mem->SystemHeapFree(name);
  if (!h) {
    const uint32_t e = Call(sub_83599390, base);
    r.error = e ? std::string(reinterpret_cast<const char*>(GP(base, e))) : "BinkOpen failed";
    g_ph.open += NowNs() - open_t0;
    return r;
  }
  r.w = Ld32(base, h + 0), r.h = Ld32(base, h + 4), r.frames = Ld32(base, h + 8);
  r.fps_num = hd.ok ? hd.fps_num : 30000, r.fps_den = hd.ok ? hd.fps_den : 1001;  // the header (30000/1001 if none)
  // Frame buffers: the info block, then our own planes in guest memory (pitch rounded up to 128).
  const uint32_t fb = mem->SystemHeapAlloc(128);
  std::memset(GP(base, fb), 0, 128);
  Call(sub_8359AB88, base, h, fb);
  const uint32_t total = std::max(1u, std::min(2u, Ld32(base, fb + 0)));
  const uint32_t yw = Ld32(base, fb + 4), yh = Ld32(base, fb + 8), cw = Ld32(base, fb + 12), ch = Ld32(base, fb + 16);
  std::vector<uint32_t> allocs;
  for (uint32_t f = 0; f < total; ++f) {
    const uint32_t rec = fb + 24 + f * 48;
    for (uint32_t pl = 0; pl < 4; ++pl) {
      if (!Ld32(base, rec + pl * 12)) continue;  // not allocated (e.g. no alpha)
      const uint32_t pw = pl == 0 || pl == 3 ? yw : cw, ph = pl == 0 || pl == 3 ? yh : ch;
      const uint32_t pitch = (pw + 127) & ~127u;
      const uint32_t a = mem->SystemHeapAlloc(pitch * ph, 4096);
      allocs.push_back(a);
      St32(base, rec + pl * 12 + 4, a);
      St32(base, rec + pl * 12 + 8, pitch);
    }
  }
  Call(sub_8359AC90, base, h, fb);
  g_ph.open += NowNs() - open_t0;
  const double src_fps = double(r.fps_num) / r.fps_den;
  // Output rate: the file's own (every frame) unless --wwe2k14_tron_export_fps asks for fewer.
  const int fps_flag = REXCVAR_GET(wwe2k14_tron_export_fps);
  const uint32_t out_num = fps_flag > 0 ? uint32_t(fps_flag) : r.fps_num, out_den = fps_flag > 0 ? 1u : r.fps_den;
  const double out_fps = double(out_num) / out_den;
  const int secs = REXCVAR_GET(wwe2k14_tron_export_seconds);
  const uint32_t want = secs > 0 ? std::min<uint32_t>(r.frames, uint32_t(src_fps * secs)) : r.frames;
  const int max_w = REXCVAR_GET(wwe2k14_tron_export_max_width);
  Mp4Writer mp4;
  bool mp4_ok = false;
  double next_out = 0;  // the next preview frame's time in source frames
  const auto at_frames = AtFrames(src_fps, r.frames);  // --wwe2k14_tron_export_media_at
  bool thumb_done = false, thumb_written = false;
  uint32_t last_at = 0;  // (no MP4: decoding stops after the thumbnail and the last asked-for frame)
  for (const auto& a : at_frames) last_at = std::max(last_at, a.first);
  const uint32_t vw = r.w & ~1u, vh = r.h & ~1u;
  for (uint32_t i = 0; i < std::max(1u, want); ++i) {
    if (kStillsAreResult && !mp4_ok && thumb_done && i > last_at) break;  // (no MP4 being written: the stills are done)
    // Bink owns the buffer index (block +20): each BinkDoFrame decodes into the OTHER buffer, using the current one as
    // its reference picture, then points the index at the buffer it wrote (run 1356 log). The game only reads the
    // index (texset_prepare_next_buffer sub_827FBA90 prepares FrameNum + 1's texture). Setting it ourselves broke it:
    // i % total lost frame 0 (all zeros: green) and saved every later frame one decode late; forcing "next" made
    // Bink decode on a blank reference. So: never write it; read which buffer was filled.
    const uint32_t index_before = Ld32(base, fb + 20);
    {
      Timed timed(g_ph.decode);
      Call(sub_8359E6E8, base, h);  // BinkDoFrame
    }
    ++r.decoded;
    Tick(i, want);
    const uint32_t slot = Ld32(base, fb + 20) % total;
    if (REXCVAR_GET(wwe2k14_tron_export_debug) && i < 8) {
      auto meanY = [&](uint32_t k) {  // sparse mean of buffer k's Y plane
        const uint32_t rec = fb + 24 + k * 48, yb = Ld32(base, rec + 4), yp = Ld32(base, rec + 8);
        uint64_t sum = 0, n = 0;
        for (uint32_t y = 0; y < r.h; y += 8)
          for (uint32_t x = 0; x < r.w; x += 8) sum += GP(base, yb)[size_t(y) * yp + x], ++n;
        return n ? double(sum) / n : 0.0;
      };
      REXLOG_INFO("[wwe2k14][tron_export] {} frame {}: index {} -> {}, handle frame {}, mean Y buf0 {:.1f} buf1 {:.1f}",
                  tron, i, index_before, Ld32(base, fb + 20), Ld32(base, h + 12), meanY(0),
                  total > 1 ? meanY(1) : -1.0);
    }
    bool at_hit = false;
    for (const auto& a : at_frames) at_hit = at_hit || a.first == i;
    const bool need = i == 0 || double(i) + 1e-6 >= next_out || !thumb_done || at_hit;
    if (need) {
      uint64_t conv_t0 = NowNs();  // (timings: convert = the copy out of guest memory + Shrink)
      Planes f;
      f.w = vw, f.h = vh;
      f.y.resize(size_t(vw) * vh), f.u.resize(size_t(vw / 2) * (vh / 2)), f.v.resize(f.u.size());
      const uint32_t rec = fb + 24 + slot * 48;
      const uint32_t yb = Ld32(base, rec + 4), yp = Ld32(base, rec + 8);
      const uint32_t rb = Ld32(base, rec + 16), rp = Ld32(base, rec + 20);
      const uint32_t bb = Ld32(base, rec + 28), bp = Ld32(base, rec + 32);
      const uint32_t ab = Ld32(base, rec + 36) ? Ld32(base, rec + 40) : 0, ap = Ld32(base, rec + 44);
      for (uint32_t y = 0; y < vh; ++y) std::memcpy(&f.y[size_t(y) * vw], GP(base, yb) + size_t(y) * yp, vw);
      if (ab) {
        f.a.resize(f.y.size());
        for (uint32_t y = 0; y < vh; ++y) std::memcpy(&f.a[size_t(y) * vw], GP(base, ab) + size_t(y) * ap, vw);
        r.alpha = true;
      }
      for (uint32_t y = 0; y < vh / 2; ++y) {
        std::memcpy(&f.v[size_t(y) * (vw / 2)], GP(base, rb) + size_t(y) * rp, vw / 2);  // cR = V
        std::memcpy(&f.u[size_t(y) * (vw / 2)], GP(base, bb) + size_t(y) * bp, vw / 2);  // cB = U
      }
      if (max_w > 0) f = Shrink(std::move(f), uint32_t(max_w));
      g_ph.convert += NowNs() - conv_t0;
      for (const auto& a : at_frames)
        if (a.first == i) WritePng(dir / (std::to_string(tron) + a.second + ".png"), f);
      if (i == 0)
        mp4_ok = TimedLessMux(g_ph.open, [&] {
          return mp4.Open(dir / (std::to_string(tron) + ".mp4.part"), f.w, f.a.empty() ? f.h : f.h * 2, out_num, out_den);
        });
      if (!thumb_done) {
        conv_t0 = NowNs();
        uint64_t sum = 0;
        for (uint8_t v : f.y) sum += v;
        const double mean = f.y.empty() ? 0 : double(sum) / f.y.size();
        g_ph.convert += NowNs() - conv_t0;
        if (mean > 24.0 || double(i) >= src_fps * 3) {  // 16 = black in limited range; give up after 3 s
          thumb_written = WritePng(dir / (std::to_string(tron) + ".png"), f);
          thumb_done = true, r.thumb_frame = i, r.thumb_mean = mean;
        }
      }
      if (double(i) + 1e-6 >= next_out) {
        const Planes* enc = &f;
        Planes stacked;
        if (mp4_ok && !f.a.empty()) {
          Timed timed(g_ph.convert);
          stacked = Stack(f), enc = &stacked;
        }
        if (mp4_ok && TimedLessMux(g_ph.encode, [&] { return mp4.Write(*enc); })) ++r.written;
#if defined(__ANDROID__)
        else if (mp4_ok) mp4_ok = false;  // (the phone's encoder gave up: stop feeding it; the .part is removed below, the stills stay)
#endif
        next_out += src_fps / out_fps;
      }
    }
    {
      Timed timed(g_ph.decode);
      Call(sub_8359E6F0, base, h);  // BinkNextFrame
    }
  }
  {
    Timed timed(g_ph.close);
    Call(sub_8359EBC8, base, h);  // BinkClose
  }
  for (uint32_t a : allocs) mem->SystemHeapFree(a);
  mem->SystemHeapFree(fb);
  std::error_code ec;
  bool mp4_done = false;
  if (mp4_ok && TimedLessMux(g_ph.close, [&] { return mp4.Close(); })) {
    Timed timed(g_ph.close);
    std::filesystem::rename(dir / (std::to_string(tron) + ".mp4.part"), dir / (std::to_string(tron) + (r.alpha ? ".alpha.mp4" : ".mp4")), ec);
    std::error_code ec2;  // a reveal's older, colour-only preview (before Session 18)
    if (r.alpha) std::filesystem::remove(dir / (std::to_string(tron) + ".mp4"), ec2);
    mp4_done = !ec;
  } else {
    std::filesystem::remove(dir / (std::to_string(tron) + ".mp4.part"), ec);
  }
  r.ok = !kStillsAreResult ? r.written > 0 && !ec : thumb_written;  // (off Windows the PNG is the whole result)
  if (!r.ok && r.error.empty()) r.error = !kHaveMp4 || kStillsAreResult ? "the PNG could not be written" : mp4_ok ? "no frames written" : "MP4 writer failed";
#if defined(__ANDROID__)
  if (!mp4_done) {  // the still is the result; say why this tron has no video (an older MP4 of it would play stale: remove it)
    r.error += std::string(r.error.empty() ? "" : "; ") + "no MP4: " + (mp4.Error().empty() ? "not written" : mp4.Error()) + " (still only)";
    std::error_code ec3;
    std::filesystem::remove(dir / (std::to_string(tron) + ".mp4"), ec3);
    std::filesystem::remove(dir / (std::to_string(tron) + ".alpha.mp4"), ec3);
  }
#else
  (void)mp4_done;
#endif
  return r;
}

// ---- Session 97: the memory movies (stage art). pac\evt\evtmov*.pac hold Bink files the game opens from a buffer
// (sub_82806020: flags 0x07000400, the whole buffer byte-swapped per 32-bit word first, then BinkOpen). Same Bink calls
// as ExportOne, no MP4. Output: DIR\evtmov\<GROUP>_<Mnnn>.png (the first frame with picture in it) and, for movies with
// alpha, <GROUP>_<Mnnn>_end.png (the last frame: the settled header art). Group "MOV " is written MOV.
struct MediaEntry {
  std::string pac, group, key;  // pac: guest path; group: MOV / MOVT / MVSW / ...; key: M005
  uint64_t offset = 0;
};
struct MediaResult {
  std::string group, key;
  bool ok = false, alpha = false;
  uint32_t w = 0, h = 0, frames = 0, first_frame = 0;
  uint32_t decoded = 0, encoded = 0;  // Session 98 timings
  std::string error;
};

// The directory of one evtmov pac (16 KB header): at 0x800 group records {tag4, words, pad4} then words/3 records
// {name4, offset in 2 KB sectors from 0x4000, size/256} (S97 reader, stage_video.md section 1).
inline void ListMedia(const std::string& pac, std::vector<MediaEntry>& out) {
  std::vector<uint8_t> hd(0x4000);
  if (game_files::Read(pac, {hd.data(), hd.size()}) != hd.size() || std::memcmp(hd.data(), "EPAC", 4) != 0) return;
  auto le = [&](size_t o) { uint32_t v; std::memcpy(&v, &hd[o], 4); return v; };
  size_t p = 0x800;
  while (p + 12 <= hd.size() && hd[p] == 'M') {
    std::string group(reinterpret_cast<const char*>(&hd[p]), 4);
    while (!group.empty() && (group.back() == ' ' || group.back() == '\0')) group.pop_back();
    const uint32_t words = le(p + 4);
    p += 12;
    for (uint32_t r = 0; r < words / 3 && p + 12 <= hd.size(); ++r, p += 12) {
      MediaEntry e;
      e.pac = pac, e.group = group, e.key.assign(reinterpret_cast<const char*>(&hd[p]), 4);
      e.offset = 0x4000 + uint64_t(le(p + 4)) * 2048;
      out.push_back(e);
    }
  }
}

// Fraction (0..1) of pixels that are visible (alpha > 32) and not dark (Y > 40); for opaque movies the mean luma.
inline double Coverage(const Planes& f) {
  if (f.y.empty()) return 0;
  if (f.a.empty()) {
    uint64_t s = 0;
    for (uint8_t v : f.y) s += v;
    return double(s) / f.y.size();
  }
  size_t n = 0;
  for (size_t i = 0; i < f.y.size(); ++i) n += f.a[i] > 32 && f.y[i] > 40;
  return double(n) / f.y.size();
}

inline MediaResult ExportMedia(uint8_t* base, const MediaEntry& e, const std::filesystem::path& dir) {
  MediaResult r;
  r.group = e.group, r.key = e.key;
  auto* mem = REX_KERNEL_MEMORY();
  uint64_t read_t0 = NowNs();  // (timings: read = the pac reads + the word swap)
  uint32_t head[11] = {};
  if (game_files::Read(e.pac, {reinterpret_cast<uint8_t*>(head), sizeof(head)}, e.offset) != sizeof(head) ||
      (std::memcmp(head, "BIK", 3) != 0 && std::memcmp(head, "KB2", 3) != 0)) {
    r.error = "no Bink header";
    g_ph.read += NowNs() - read_t0;
    return r;
  }
  const tron_shim::BinkHeader hd = tron_shim::ParseBinkHeader(head, true);
  const size_t size = size_t(head[1]) + 8, padded = (size + 3) & ~size_t(3);
  std::vector<uint8_t> file(padded + 4, 0);
  if (game_files::Read(e.pac, {file.data(), size}, e.offset) != size) {
    r.error = "short read";
    g_ph.read += NowNs() - read_t0;
    return r;
  }
  // The guest buffer: every 32-bit word byte-swapped, as the loop in sub_82806020 does (bytes 0<->3, 1<->2).
  const uint32_t buf = mem->SystemHeapAlloc(uint32_t(padded + 16), 4096);
  uint8_t* dst = GP(base, buf);
  for (size_t i = 0; i < padded; i += 4)
    dst[i] = file[i + 3], dst[i + 1] = file[i + 2], dst[i + 2] = file[i + 1], dst[i + 3] = file[i];
  g_ph.read += NowNs() - read_t0;
  const uint64_t open_t0 = NowNs();
  Call(sub_83599C58, base, 0x400000);
  Call(sub_83599C68, base, 0x83598F78);
  const uint32_t h = Call(sub_8359E140, base, buf, 0x07000400 | (hd.flags & 0x00100000u));
  if (!h) {
    const uint32_t er = Call(sub_83599390, base);
    r.error = er ? std::string(reinterpret_cast<const char*>(GP(base, er))) : "BinkOpen failed";
    mem->SystemHeapFree(buf);
    g_ph.open += NowNs() - open_t0;
    return r;
  }
  r.w = Ld32(base, h + 0), r.h = Ld32(base, h + 4), r.frames = Ld32(base, h + 8);
  const uint32_t fb = mem->SystemHeapAlloc(128);
  std::memset(GP(base, fb), 0, 128);
  Call(sub_8359AB88, base, h, fb);
  const uint32_t total = std::max(1u, std::min(2u, Ld32(base, fb + 0)));
  const uint32_t yw = Ld32(base, fb + 4), yh = Ld32(base, fb + 8), cw = Ld32(base, fb + 12), ch = Ld32(base, fb + 16);
  std::vector<uint32_t> allocs;
  for (uint32_t f = 0; f < total; ++f) {
    const uint32_t rec = fb + 24 + f * 48;
    for (uint32_t pl = 0; pl < 4; ++pl) {
      if (!Ld32(base, rec + pl * 12)) continue;
      const uint32_t pw = pl == 0 || pl == 3 ? yw : cw, ph = pl == 0 || pl == 3 ? yh : ch;
      const uint32_t pitch = (pw + 127) & ~127u;
      const uint32_t a = mem->SystemHeapAlloc(pitch * ph, 4096);
      allocs.push_back(a);
      St32(base, rec + pl * 12 + 4, a);
      St32(base, rec + pl * 12 + 8, pitch);
    }
  }
  Call(sub_8359AC90, base, h, fb);
  g_ph.open += NowNs() - open_t0;
  const double src_fps = hd.fps_den ? double(hd.fps_num) / hd.fps_den : 30.0;
  const uint32_t vw = r.w & ~1u, vh = r.h & ~1u;
  const std::string stem = e.group + "_" + e.key;
  const auto at_frames = AtFrames(src_fps, r.frames);  // --wwe2k14_tron_export_media_at
  uint32_t last_at = 0;
  for (const auto& a : at_frames) last_at = std::max(last_at, a.first);
  Planes first, last, best;
  bool have_first = false;
  double best_cov = -1;
  bool alpha_movie = false;
  // Session 98 (tron video): the whole movie as DIR\evtmov\<GROUP>_<Mnnn>.mp4 too (the asset viewer plays it on the walls / pillars /
  // mini-tron). Alpha movies are flattened over black (the stage draws them over black: stage_media.h OverBlack). One-picture movies
  // get no MP4.
  Mp4Writer mp4;
  bool mp4_ok = false;
  const bool want_mp4 = REXCVAR_GET(wwe2k14_tron_export_media_mp4) && r.frames > 1;
  const std::filesystem::path mp4_part = dir / (stem + ".mp4.part");
  g_prog.part = want_mp4 ? mp4_part : std::filesystem::path();
  for (uint32_t i = 0; i < std::max(1u, r.frames); ++i) {
    {
      Timed timed(g_ph.decode);
      Call(sub_8359E6E8, base, h);  // BinkDoFrame
    }
    ++r.decoded;
    Tick(i, r.frames);
    uint64_t conv_t0 = NowNs();  // (timings: convert = the copy out of guest memory)
    const uint32_t slot = Ld32(base, fb + 20) % total;
    Planes f;
    f.w = vw, f.h = vh;
    f.y.resize(size_t(vw) * vh), f.u.resize(size_t(vw / 2) * (vh / 2)), f.v.resize(f.u.size());
    const uint32_t rec = fb + 24 + slot * 48;
    const uint32_t yb = Ld32(base, rec + 4), yp = Ld32(base, rec + 8);
    const uint32_t rb = Ld32(base, rec + 16), rp = Ld32(base, rec + 20);
    const uint32_t bb = Ld32(base, rec + 28), bp = Ld32(base, rec + 32);
    const uint32_t ab = Ld32(base, rec + 36) ? Ld32(base, rec + 40) : 0, ap = Ld32(base, rec + 44);
    for (uint32_t y = 0; y < vh; ++y) std::memcpy(&f.y[size_t(y) * vw], GP(base, yb) + size_t(y) * yp, vw);
    if (ab) {
      f.a.resize(f.y.size());
      for (uint32_t y = 0; y < vh; ++y) std::memcpy(&f.a[size_t(y) * vw], GP(base, ab) + size_t(y) * ap, vw);
      alpha_movie = true;
    }
    for (uint32_t y = 0; y < vh / 2; ++y) {
      std::memcpy(&f.v[size_t(y) * (vw / 2)], GP(base, rb) + size_t(y) * rp, vw / 2);  // cR = V
      std::memcpy(&f.u[size_t(y) * (vw / 2)], GP(base, bb) + size_t(y) * bp, vw / 2);  // cB = U
    }
    g_ph.convert += NowNs() - conv_t0;
    for (const auto& a : at_frames)
      if (a.first == i) WritePng(dir / (stem + a.second + ".png"), f);
    if (want_mp4) {
      if (i == 0) mp4_ok = TimedLessMux(g_ph.open, [&] { return mp4.Open(mp4_part, vw, vh, hd.fps_num, hd.fps_den); });
      if (mp4_ok) {
        const Planes* enc = &f;
        Planes flat;
        if (!f.a.empty()) {
          Timed timed(g_ph.convert);
          flat = FlattenOverBlack(f), enc = &flat;
        }
        mp4_ok = TimedLessMux(g_ph.encode, [&] { return mp4.Write(*enc); });
        r.encoded += mp4_ok;
      }
    }
    conv_t0 = NowNs();  // (timings: convert also = the coverage sums and the kept frames' copies)
    const double cov = Coverage(f);
    const double threshold = f.a.empty() ? 24.0 : 0.01;
    const bool last_frame = i + 1 >= std::max(1u, r.frames);
    if (!have_first && (cov > threshold || double(i) >= src_fps * 3)) {
      first = f, have_first = true, r.first_frame = i;
    }
    if (cov > best_cov) best_cov = cov, best = f;
    if (last_frame) last = std::move(f);
    g_ph.convert += NowNs() - conv_t0;
    {
      Timed timed(g_ph.decode);
      Call(sub_8359E6F0, base, h);  // BinkNextFrame
    }
    if (have_first && !alpha_movie && !mp4_ok && i >= last_at) break;  // opaque: the first picture (and the asked-for times) are enough (no MP4 wanted)
  }
  {
    Timed timed(g_ph.close);
    Call(sub_8359EBC8, base, h);  // BinkClose
  }
  if (mp4_ok && TimedLessMux(g_ph.close, [&] { return mp4.Close(); })) {
    Timed timed(g_ph.close);
    std::error_code mec;
    std::filesystem::rename(mp4_part, dir / (stem + ".mp4"), mec);
    if (mec) {  // e.g. an older .mp4 that is still open elsewhere: say so and leave no .part behind
      REXLOG_WARN("[wwe2k14][tron_export] media {}: could not replace {}.mp4: {}", stem, stem, mec.message());
      std::error_code rec;
      std::filesystem::remove(mp4_part, rec);
    } else {
      std::error_code sec;
      const auto sz = std::filesystem::file_size(dir / (stem + ".mp4"), sec);
      if (!sec) g_media_mp4_bytes += sz, ++g_media_mp4_count;
    }
  } else {
    std::error_code mec;
    if (want_mp4) TimedLessMux(g_ph.close, [&] { return mp4.Close(); });
    std::filesystem::remove(mp4_part, mec);
#if defined(__ANDROID__)
    if (want_mp4) {  // the phone's encoder refused this movie: the still stays; an older MP4 of it would play stale
      REXLOG_WARN("[wwe2k14][tron_export] media {}: no MP4: {} (still only)", stem, mp4.Error().empty() ? "not written" : mp4.Error());
      std::filesystem::remove(dir / (stem + ".mp4"), mec);
    }
#endif
  }
  for (uint32_t a : allocs) mem->SystemHeapFree(a);
  mem->SystemHeapFree(fb);
  mem->SystemHeapFree(buf);
  r.alpha = alpha_movie;
  if (!have_first && !best.y.empty()) first = best, have_first = true;
  if (have_first) r.ok = WritePng(dir / (stem + ".png"), first);
  if (alpha_movie && !last.y.empty()) WritePng(dir / (stem + "_end.png"), last);
  if (!r.ok && r.error.empty()) r.error = "no frame written";
  return r;
}

inline void Run(uint64_t frame) {
  uint8_t* base = REX_KERNEL_MEMORY()->virtual_membase();
  const std::filesystem::path dir = REXCVAR_GET(wwe2k14_tron_export);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  // Which trons: every NNN.bik2 in the game's titantron folder (through GAME:, Session 26), or the given list.
  std::set<int> trons;
  std::stringstream only(REXCVAR_GET(wwe2k14_tron_export_only));
  for (std::string t; std::getline(only, t, ',');)
    if (!t.empty()) trons.insert(std::atoi(t.c_str()));
  if (!REXCVAR_GET(wwe2k14_tron_export_trons)) {
    trons.clear();
  } else if (trons.empty()) {
    for (const std::string& name : game_files::ListFiles("game:\\movies\\titantron")) {
      const std::filesystem::path p(name);
      const std::string stem = p.stem().string();
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (ext == ".bik2" && !stem.empty() && std::all_of(stem.begin(), stem.end(), ::isdigit))
        trons.insert(std::atoi(stem.c_str()));
    }
  }
  // Session 97: the stage movies (evtmov pacs), through the same Bink with the buffer opened from memory. (Session 98: listed before
  // the trons start, so the progress knows the whole count.)
  const std::string media_only = REXCVAR_GET(wwe2k14_tron_export_media_only);
  const bool want_media = REXCVAR_GET(wwe2k14_tron_export_media) &&
                          (media_only.size() || REXCVAR_GET(wwe2k14_tron_export_only).empty());
  std::vector<MediaEntry> media;
  size_t media_listed = 0;
  if (want_media) {
    std::vector<MediaEntry> list;
    for (const char* f : {"evtmov", "evtmov2", "evtmov2_2", "evtmov3"})
      ListMedia(std::string("game:\\pac\\evt\\") + f + ".pac", list);
    media_listed = list.size();
    std::set<std::string> only_media;
    std::stringstream os(media_only);
    for (std::string t; std::getline(os, t, ',');)
      if (!t.empty()) only_media.insert(t);
    for (const MediaEntry& e : list)
      if (only_media.empty() || only_media.count(e.group + "_" + e.key)) media.push_back(e);
  }
  REXLOG_INFO("[wwe2k14][tron_export] frame {}: exporting {} tron(s) and {} stage movie(s) to {}", frame, trons.size(), media.size(),
              dir.string());
#if defined(_WIN32)
  static std::once_flag mf;
  std::call_once(mf, [] { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); });
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
  const auto t0 = std::chrono::steady_clock::now();
  g_prog = Progress{};
  g_rows.clear();
  g_prog.dir = dir, g_prog.t0 = NowNs();
  g_prog.trons = int(trons.size()), g_prog.media = int(media.size()), g_prog.total = g_prog.trons + g_prog.media;
  g_prog.phase = "titantrons";
  WriteProgress();
  std::ostringstream idx;
  idx << "{\n  \"made_by\": \"wwe2k14 --wwe2k14_tron_export (the game's own Bink decoder)\",\n  \"trons\": [";
  int ok = 0, n = 0;
  for (int tron : trons) {
    const std::string name = std::to_string(tron);
    StartItem("titantrons", "tron " + name, dir / (name + ".mp4.part"));
    const uint64_t item_t0 = NowNs();
    const auto r = ExportOne(base, tron, dir);
    EndItem("tron", name, r.ok, r.w, r.h, r.frames, r.decoded, r.written, item_t0,
            {dir / (name + ".png"), dir / (name + (r.alpha ? ".alpha.mp4" : ".mp4"))});
    ok += r.ok;
    REXLOG_INFO("[wwe2k14][tron_export] {} {}: {}x{}, {} frames at {}/{}; thumbnail frame {} (mean luma {:.0f}); "
                "preview {} frame(s){}{}{}", tron, r.ok ? "ok" : "FAILED", r.w, r.h, r.frames, r.fps_num, r.fps_den,
                r.thumb_frame, r.thumb_mean, r.written, r.alpha ? " (stacked alpha)" : "", r.error.empty() ? "" : ": ",
                r.error);
    idx << (n++ ? "," : "") << "\n    {\"tron\": " << tron << ", \"ok\": " << (r.ok ? "true" : "false")
        << ", \"width\": " << r.w << ", \"height\": " << r.h << ", \"frames\": " << r.frames
        << ", \"fps\": " << (r.fps_den ? double(r.fps_num) / r.fps_den : 0)
        << ", \"stacked_alpha\": " << (r.alpha ? "true" : "false") << "}";
  }
  idx << "\n  ]";
  if (want_media) {
    const std::filesystem::path mdir = dir / "evtmov";
    std::filesystem::create_directories(mdir, ec);
    REXLOG_INFO("[wwe2k14][tron_export] {} stage movie(s) listed; exporting to {}", media_listed, mdir.string());
    idx << ",\n  \"media\": [";
    int mok = 0, mn = 0, mtried = 0;
    for (const MediaEntry& e : media) {
      ++mtried;
      const std::string stem = e.group + "_" + e.key;
      StartItem("stage movies", stem, {});  // (ExportMedia names its .part itself when it writes one)
      const uint64_t item_t0 = NowNs();
      const MediaResult r = ExportMedia(base, e, mdir);
      EndItem("media", stem, r.ok, r.w, r.h, r.frames, r.decoded, r.encoded, item_t0,
              {mdir / (stem + ".png"), mdir / (stem + ".mp4"), mdir / (stem + "_end.png")});
      mok += r.ok;
      REXLOG_INFO("[wwe2k14][tron_export] media {}_{} {}: {}x{}, {} frames, picture from frame {}{}{}{}", r.group, r.key,
                  r.ok ? "ok" : "FAILED", r.w, r.h, r.frames, r.first_frame, r.alpha ? " (alpha)" : "",
                  r.error.empty() ? "" : ": ", r.error);
      idx << (mn++ ? "," : "") << "\n    {\"group\": \"" << r.group << "\", \"key\": \"" << r.key
          << "\", \"ok\": " << (r.ok ? "true" : "false") << ", \"width\": " << r.w << ", \"height\": " << r.h
          << ", \"frames\": " << r.frames << ", \"alpha\": " << (r.alpha ? "true" : "false") << "}";
    }
    idx << "\n  ]";
    REXLOG_INFO("[wwe2k14][tron_export] stage movies: {} of {} ok; {} MP4(s), {} bytes ({:.1f} MB) in total", mok, mtried, g_media_mp4_count,
                g_media_mp4_bytes, double(g_media_mp4_bytes) / 1048576.0);
  }
  idx << "\n}\n";
  std::ofstream(dir / "index.json") << idx.str();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[wwe2k14][tron_export] done: {} of {} ok in {:.1f} s; quitting", ok, trons.size(), secs);
  // Session 98: the totals by phase (timings.json has every item).
  {
    const Phases& s = g_prog.sum;
    REXLOG_INFO("[wwe2k14][tron_export] timing totals: {} item(s) in {:.1f} s: open {:.1f} s, read {:.1f} s, decode {:.1f} s, convert {:.1f} s, "
                "encode {:.1f} s, mux {:.1f} s, png {:.1f} s, close {:.1f} s; {:.1f} MB written",
                g_rows.size(), secs, Ms(s.open) / 1e3, Ms(s.read) / 1e3, Ms(s.decode) / 1e3, Ms(s.convert) / 1e3, Ms(s.encode) / 1e3,
                Ms(s.mux) / 1e3, Ms(s.png) / 1e3, Ms(s.close) / 1e3, double(g_prog.bytes) / 1048576.0);
  }
  WriteTimings(true);
  g_prog.item.clear(), g_prog.part.clear();
  g_prog.state = g_prog.ok > 0 || g_prog.total == 0 ? "done" : "failed";
  g_prog.message = std::to_string(g_prog.ok) + " of " + std::to_string(g_prog.total) + " made";
  WriteProgress();
#if defined(__ANDROID__)
  // Session 98: the exit request straight from here. g_request_quit goes through the UI thread, which SDL holds still while the app
  // is in the background - an export that ends there would wait for the player to come back. GameActivity's poll ends the process.
  rex::FlushLogging();
  android_lifecycle::WriteExitRequest("the tron export finished");
#endif
  if (g_request_quit) g_request_quit();
}

inline void OnSwap(uint64_t frame) {
  if (g_prev_hook) g_prev_hook(frame);
  if (g_done || int64_t(frame) < REXCVAR_GET(wwe2k14_tron_export_frame)) return;
  g_done = true;
  Run(frame);
}

inline void Install() {
  if (REXCVAR_GET(wwe2k14_tron_export).empty()) return;
  g_prev_hook = wwe2k14::g_vdswap_hook;
  wwe2k14::g_vdswap_hook = &OnSwap;
  REXLOG_INFO("[wwe2k14][tron_export] armed: export to {} at frame {}", REXCVAR_GET(wwe2k14_tron_export),
              REXCVAR_GET(wwe2k14_tron_export_frame));
}

}  // namespace wwe2k14::tron_export
