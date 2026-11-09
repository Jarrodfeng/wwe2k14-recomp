// Session 98 (tron video, Jarrod: "Orton's entrance doesn't have his video Trons playing"): the stage's tron slots play VIDEO.
// The launcher's tron export (src\tron_export.h) leaves, on the player's own machine, per titantron NNN.mp4 (the whole 320x320 sheet:
// big screen = rows 0-223, four 24-row banner strips below) and, with the evtmov export, evtmov\GROUP_Mnnn.mp4 for the stage movies.
// Three layers, one clock (video seconds):
//   main  -> textures "main&banner" (the whole sheet) and "titantron_movie" (rows 0-223): NNN.mp4, or the player's mod video while
//            the mods toggle is on (same rule as mods_lookup: the video on rows 0-223, the banner strips from ModSheet)
//   wall  -> "ppv_header&wall" / "ss_header&wall": evtmov\MVSW_Mnnn.mp4 (wrestler) else MOV_Mnnn.mp4 (arena)
//   mini  -> "mini_tron": evtmov\MVSB_Mnnn.mp4 else MVAB_Mnnn.mp4
// A layer with no MP4 keeps the still the stage was loaded with. The clock: the entrance clock (ent.t / 60) in an entrance, else the
// preview clock (real time, looping; `tron_time` / `tron_play` set it). Videos loop when the clock passes their end.
// Live view: a worker decodes toward the newest request, late pictures are dropped. Shots / renders: FrameAt, blocking and
// deterministic (the picture whose time stamp is the last one <= t).
#pragma once
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../mods_lookup.h"
#include "../stage_media.h"  // (Cfg().default_dir: the platform's tron cache)
#include "renderer.h"
#include "video_player.h"

namespace view {

class TronVideo {
 public:
  bool on = true;       // `look tron_video`
  bool play = true;     // plain preview: the clock runs in real time (tron_play)
  double clock = 0;     // plain preview clock, seconds
  // `tron_offset`: seconds of the entrance clock (ent.t / 60, intro_hold included) at which the video is at 0. Default 0 = the start of cue 0
  // (the video wait). [U] where the game really starts the tron: a milestone run can set it from the game.
  // Session 98 (integration): the picture the game SHOWS, not the file open. Run 6816's big screen, correlated against every frame of
  // the tron (out\s98\integrate\notes.md): bookmark 8944 (entrance clock 1479 frames) shows video 24.42 s (peak 0.86, one frame wide)
  // -> the video is at 0 at 14 frames (0.233 s) after the start of cue 0 (was -0.3 = the file open, 18 frames before). Bookmark 10144
  // (clock 2679) shows 17.88 s: the second pass, so each loop holds ~0.31 s (+-0.15: a flat peak) before it starts again (loop_gap;
  // entrance clock only) [C for Orton's tron, U for other videos].
  double offset = 0.233;
  double loop_gap = 0.31;
  bool entrance_clock = false;  // set by the caller: t is the entrance clock (loops with loop_gap) rather than the preview clock
  double EntranceVideoTime(double ent_t_frames) const { return std::max(0.0, ent_t_frames / 60.0 - offset); }
  bool Applied() const { return applied_; }
  void Forget() { applied_ = false; }

  // Puts the picture for video time `t` on the stage's tron slots. `sync` = block and decode exactly t (shots, renders).
  void Update(Renderer& r, int wrestler, int arena, const std::string& media_dir, double t, bool sync) {
    if (!on || arena < 0) return;  // (wrestler < 0 = the match look: the arena's own MOV / MVAB videos)
    const std::string key = std::to_string(wrestler) + "|" + std::to_string(arena) + "|" + media_dir + "|" + ModKey(wrestler);
    if (key != key_) {
      key_ = key;
      Resolve(wrestler, arena, media_dir);
    }
    const uint32_t gen = r.ModelsGen();
    for (Layer& L : layers_) {
      if (L.path.empty() || L.failed) continue;
      if (!L.vp.IsOpen() || L.vp.Path() != L.path) {
        std::string e;
        if (!L.vp.Open(L.path, e)) { L.failed = true, L.err = e; continue; }
        L.serial = 0, L.have = false;
      }
      VideoFrame f;
      bool fresh = false;
      double tl = t;
      if (const double d = L.vp.Duration(); entrance_clock && loop_gap > 0 && d > 0) {  // the game's loop: the last picture held loop_gap s
        tl = std::fmod(std::max(0.0, t), d + loop_gap);
        if (tl > d - 1e-3) tl = d - 1e-3;
      }
      if (sync) {
        std::string e;
        if (L.vp.FrameAt(tl, f, e)) fresh = true;
        else { L.err = e; continue; }
      } else {
        L.vp.Request(tl);
        if (L.vp.TakeLatest(f, L.serial)) fresh = true;
      }
      if (fresh) L.last = std::move(f), L.have = true;
      else if (!(L.have && L.gen != gen)) continue;  // nothing new, and the stage was not re-loaded
      if (L.have) Apply(r, L, wrestler), L.gen = gen;
    }
  }
  // One line per layer for the command reply.
  std::string Info() const {
    std::string s = "[";
    for (size_t i = 0; i < layers_.size(); i++) {
      const Layer& L = layers_[i];
      std::string p = L.path;
      for (char& c : p) if (c == '\\') c = '/';
      s += std::string(i ? "," : "") + "{\"layer\":\"" + L.name + "\",\"file\":\"" + p + "\",\"open\":" + (L.vp.IsOpen() ? "true" : "false") +
           ",\"duration\":" + std::to_string(L.vp.Duration()) + (L.err.empty() ? "" : ",\"error\":\"" + L.err + "\"") + "}";
    }
    return s + "]";
  }

 private:
  struct Layer {
    const char* name = "";
    std::string path;
    bool failed = false, have = false;
    std::string err;
    VideoPlayer vp;
    uint64_t serial = 0;
    uint32_t gen = ~0u;
    VideoFrame last;
    bool mod = false;
    assets::mods_lookup::Frame base;  // mod: the sheet with the banner strips (rows 224+)
    std::vector<std::string> tex;
  };
  std::vector<Layer> layers_ = std::vector<Layer>(3);
  std::string key_;
  bool applied_ = false;

  static std::string ModKey(int w) {
    const std::filesystem::path f = assets::mods_lookup::TronFor(w);
    return f.empty() ? std::string() : assets::mods_lookup::U8(f);
  }
  static std::filesystem::path CacheDir(const std::string& media_dir) {
    namespace fs = std::filesystem;
    if (!media_dir.empty()) return fs::u8path(media_dir);
    if (const char* la = std::getenv("LOCALAPPDATA")) return fs::path(la) / "wwe2k14-launcher" / "trons";
    // Session 98 (phone tron video): no %LOCALAPPDATA% on the phone: the platform's tron cache (viewer_main.cpp sets it: <files>/trons)
    if (!assets::stage_media::Cfg().default_dir.empty()) return fs::u8path(assets::stage_media::Cfg().default_dir);
    return {};
  }
  static bool Exists(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
  }
  void Resolve(int wrestler, int arena, const std::string& media_dir) {
    namespace fs = std::filesystem;
    namespace ml = assets::mods_lookup;
    const fs::path dir = CacheDir(media_dir);
    for (Layer& L : layers_) {
      L.vp.Close();
      L.path.clear(), L.failed = false, L.have = false, L.err.clear(), L.mod = false, L.base = ml::Frame(), L.serial = 0, L.gen = ~0u;
    }
    char nm[48];
    // main: the mod video (toggle on, not Bink) else the cache's NNN.mp4
    Layer& m = layers_[0];
    m.name = "main", m.tex = {"main&banner", "titantron_movie"};
    const fs::path mf = ml::TronFor(wrestler);
    if (!mf.empty() && !ml::IsBink(mf)) {
      m.path = ml::U8(mf), m.mod = true;
      ml::Frame orig;
      int w = 0, h = 0, n = 0;
      if (unsigned char* d = stbi_load(ml::U8(dir / (std::to_string(wrestler) + ".png")).c_str(), &w, &h, &n, 4)) {
        orig.w = w, orig.h = h, orig.rgba.assign(d, d + size_t(w) * h * 4);
        stbi_image_free(d);
      }
      m.base = ml::ModSheet(wrestler, &orig);  // (may be empty while the still decodes: black strips then)
    } else if (!mf.empty()) {
      // a Bink (.bik2) mod: the stage keeps the mod's black still (the viewer has no Bink decoder); never the original video
    } else if (wrestler >= 0 && !dir.empty()) {
      for (const char* ext : {".mp4", ".alpha.mp4"})  // (an alpha tron is stacked: colour on top, alpha below)
        if (m.path.empty() && Exists(dir / (std::to_string(wrestler) + ext))) m.path = ml::U8(dir / (std::to_string(wrestler) + ext));
    }
    auto pick = [&](Layer& L, const char* nmL, std::vector<std::string> tex, const char* g1, int k1, const char* g2, int k2) {
      L.name = nmL, L.tex = std::move(tex);
      if (dir.empty()) return;
      auto stem = [&](const char* g, int k, const char* ext) {
        std::snprintf(nm, sizeof nm, "%s_M%03d%s", g, k, ext);
        return dir / "evtmov" / nm;
      };
      if (wrestler >= 0 && k1 >= 0) {
        if (Exists(stem(g1, k1, ".mp4"))) return void(L.path = ml::U8(stem(g1, k1, ".mp4")));
        // the wrestler's own movie is a single picture (PNG only, no MP4): keep that still, do not play the arena's video over it
        if (Exists(stem(g1, k1, ".png")) || Exists(stem(g1, k1, "_end.png"))) return;
      }
      if (Exists(stem(g2, k2, ".mp4"))) L.path = ml::U8(stem(g2, k2, ".mp4"));
    };
    pick(layers_[1], "wall", {"ppv_header&wall", "ss_header&wall"}, "MVSW", wrestler, "MOV", arena);
    pick(layers_[2], "mini", {"mini_tron"}, "MVSB", wrestler, "MVAB", arena);
  }
  // `src` fitted to w x h: the same size passes through, a taller same-width picture is cropped to its top rows (the sheet's big
  // screen), anything else is area-resampled.
  static assets::mods_lookup::Frame Fit(const VideoFrame& src, int w, int h) {
    assets::mods_lookup::Frame o;
    o.w = w, o.h = h;
    if (src.w == w && src.h >= h) {
      o.rgba.assign(src.rgba.begin(), src.rgba.begin() + std::ptrdiff_t(size_t(w) * h * 4));
      return o;
    }
    o.rgba.assign(size_t(w) * h * 4, 0);
    assets::mods_lookup::Frame s;
    s.w = src.w, s.h = src.h, s.rgba = src.rgba;
    assets::mods_lookup::Scale(s, 0, 0, s.w, s.h, o, 0, 0, w, h);
    return o;
  }
  void Apply(Renderer& r, Layer& L, int wrestler) {
    namespace ml = assets::mods_lookup;
    (void)wrestler;
    for (const std::string& name : L.tex) {
      int tw = 0, th = 0;
      if (!r.LiveTextureSize(name, tw, th)) continue;
      ml::Frame out;
      if (L.mod) {  // the mod's video on the big-screen rows, the banner strips from the mod sheet
        VideoFrame src = L.last;
        const bool stacked = L.path.find(".alpha.") != std::string::npos;
        if (stacked && src.h >= 4) src.h /= 2, src.rgba.resize(size_t(src.w) * src.h * 4);
        constexpr int S = 320, kBig = 224;
        if (tw == S && th >= kBig) {
          out.w = tw, out.h = th;
          out.rgba.assign(size_t(tw) * th * 4, 0);
          for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
          if (L.base.ok() && L.base.w == S && L.base.h == S && th == S) out.rgba = L.base.rgba;
          ml::Frame s;
          s.w = src.w, s.h = src.h, s.rgba = std::move(src.rgba);
          ml::Scale(s, 0, 0, s.w, s.h, out, 0, 0, S, kBig);
        } else {
          out = Fit(src, tw, th);
        }
      } else {
        VideoFrame src = L.last;
        if (L.path.find(".alpha.") != std::string::npos && src.h >= 4) src.h /= 2, src.rgba.resize(size_t(src.w) * src.h * 4);  // stacked alpha: the colour half
        out = Fit(src, tw, th);
      }
      const int n = r.UpdateLiveTexture(name, tw, th, out.rgba.data());
      if (std::getenv("WWE_TRON_LOG")) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < out.rgba.size(); i += 17) h = (h ^ out.rgba[i]) * 16777619u;
        std::fprintf(stderr, "tron: %s %s %dx%d src %dx%d updated %d hash %08x\n", L.name, name.c_str(), tw, th, L.last.w, L.last.h, n, h);
      }
      if (n > 0) applied_ = true;
    }
  }
};

}  // namespace view
