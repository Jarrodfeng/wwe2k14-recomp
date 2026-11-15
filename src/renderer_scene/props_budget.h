// S102 lane T17: the prop meshes' load taken off the frame that first uses it (docs\systems\props_budget.md; R14 section 5, cause 4: `[rdr][props] 223 prop meshes ... 240 file textures` was
// followed by three render-thread frames of 14.9 / 17.3 / 22.4 ms against a median 12.5). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// No game or SDK includes, no Vulkan: plain data and atomics, shared by props_ours.cpp (the hook), props_ours_core.cpp (the file hash table) and the readers of the switch (live.cpp, fview).
//
// --wwe2k14_rdr_props_budget (PerFrame, default OFF), + _bytes (default 2 MB) and _us (default 1000 us): what the props hook MAKES (GPU buffers and images, with their uploads) in one frame is held to
// the budget. A draw whose buffers / images would pass it keeps the STREAM's draw for that frame (KeepWhy::NotReady; our geometry is byte-equal to it, props_ours_check.py bar 1) and is made on a
// later frame; the first creation of every frame is always allowed (progress). Off = the hook as it was.
// Worker side (the part that needs no device): the file textures' prefix hashes (the content check of every slot hashed the file's mips ON THE RENDER THREAD at first use) and the 4:4:4:4 textures'
// widened texels, built once on the `ours_props` worker when the switch is on at the load.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "../assets/prop_game_space.h"
#include "ring_hash.h"

namespace rdr_scene::props_budget {

inline std::atomic<bool> g_on{false};
inline std::atomic<int64_t> g_bytes{2 * 1024 * 1024}, g_us{1000};
// Tests only (fview --props-budget-plant N): 1 = no_budget (every slice made in the first frame), 2 = hole (a deferred draw is answered with an empty draw instead of the stream's), 3 = image_twice (an
// image is made again on every request). 0 = off.
inline std::atomic<int> g_plant{0};   // 4 ignore_us, 5 no_reset, 6 need_ignores_slot, 7 need_zero_texture, 8 stale_pre, 9 deferred_as_not_ready (also: see the list above)
inline std::atomic<int> g_force_noimage{-1};   // fview --props-budget-noimage-slot N: the binder's slot N reads as kNoImage to the hook (a role the draw does not bind)
// Tests only (fview --props-budget-noprogress): the "first creation of a frame is always allowed" rule is off, so with a tiny budget NOTHING is ever made and every draw keeps the stream's for the whole run.
inline std::atomic<bool> g_no_progress{false};
inline std::atomic<bool> g_record{false};   // fview: keep the bytes made per frame (g_frame_index is the replay's frame number, set by fview before each frame)
inline std::atomic<uint32_t> g_frame_index{0};
inline std::map<uint32_t, uint64_t>& FrameBytes() { static std::map<uint32_t, uint64_t> m; return m; }
inline std::map<uint32_t, uint64_t>& FrameMade() { static std::map<uint32_t, uint64_t> m; return m; }       // fview: bytes the hook actually made (buffers + uploaded texels) per frame; FrameBytes = bytes it NEEDED
inline std::map<uint32_t, uint32_t>& FrameDeferred() { static std::map<uint32_t, uint32_t> m; return m; }   // fview: draws deferred per frame
inline void NoteMade(uint64_t n) { if (g_record.load(std::memory_order_relaxed)) FrameMade()[g_frame_index.load(std::memory_order_relaxed)] += n; }
inline std::map<uint32_t, double>& FrameReplaceMs() { static std::map<uint32_t, double> m; return m; }   // fview: milliseconds inside the hook's Replace per frame (Decide, the file hashes, the buffers / images made)

inline void Set(bool on, int64_t bytes, int64_t us) {
  g_on.store(on, std::memory_order_relaxed);
  g_bytes.store(std::max<int64_t>(1, bytes), std::memory_order_relaxed);
  g_us.store(std::max<int64_t>(1, us), std::memory_order_relaxed);
}

inline size_t MipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

// The mips of the image made for file texture `t` when at most `max_mips` are wanted (0 = the texture cannot be made). The one rule MakeTexture and the budget's size estimate share.
inline uint32_t FitMips(const assets::Texture& t, uint32_t max_mips) {
  if (t.width == 0 || t.height == 0 || t.format == assets::TexFormat::kUnknown) return 0;
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if ((bc && ((t.width % 4) || (t.height % 4))) || t.width > 16384 || t.height > 16384) return 0;
  uint32_t mips = std::min(std::max(1u, t.mips), std::max(1u, max_mips)), mx = std::max(t.width, t.height), maxmips = 1;
  while (mx > 1) mx >>= 1, ++maxmips;
  mips = std::min(mips, maxmips);
  size_t total = 0;
  uint32_t fit = 0;
  for (uint32_t i = 0; i < mips; ++i) {
    const size_t sz = MipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (total + sz > t.data.size()) break;
    total += sz, ++fit;
  }
  return fit;
}

// What the worker prepares from the loaded set (no device).
struct Pre {
  std::vector<std::vector<uint64_t>> hash;                    // [texture][mips - 1]: RingXxh3 of the file's first `mips` mips (exactly PropsCore::FileHash's formula)
  std::vector<std::vector<std::vector<uint8_t>>> wide;        // [texture][mip]: RGBA8 texels of a B4G4R4A4 texture's mips (empty for the other formats)
  size_t bytes = 0;                                           // what the tables hold
  const void* set = nullptr;                                  // the PropSet these tables were made from (asserted in PropsCore::SetPre and when the widened texels are used)
};

inline uint64_t FilePrefixHash(const assets::Texture& t, uint32_t mips) {
  size_t bytes = 0;
  for (uint32_t m = 0; m < mips; ++m) bytes += MipBytes(t.format, std::max(1u, t.width >> m), std::max(1u, t.height >> m));
  return RingXxh3(t.data.data(), std::min(bytes, t.data.size()));
}

// The bytes UpdateImage is given for a texture's first `fit` mips (a 4:4:4:4 texture is widened to 8:8:8:8: twice the file's bytes). One rule for the budget's need and for what is made.
inline size_t UploadBytes(const assets::Texture& t, uint32_t fit) {
  size_t n = 0;
  for (uint32_t i = 0; i < fit; ++i) {
    const uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
    n += t.format == assets::TexFormat::kB4G4R4A4 ? size_t(mw) * mh * 4 : MipBytes(t.format, mw, mh);
  }
  return n;
}

inline void WidenMip(const assets::Texture& t, size_t src, uint32_t mw, uint32_t mh, std::vector<uint8_t>& wide) {
  wide.resize(size_t(mw) * mh * 4);
  for (size_t k = 0; k < size_t(mw) * mh; ++k) {
    const uint16_t px = uint16_t(t.data[src + 2 * k] | (t.data[src + 2 * k + 1] << 8));
    wide[4 * k] = uint8_t(((px >> 8) & 15) * 17), wide[4 * k + 1] = uint8_t(((px >> 4) & 15) * 17), wide[4 * k + 2] = uint8_t((px & 15) * 17), wide[4 * k + 3] = uint8_t(((px >> 12) & 15) * 17);
  }
}

inline std::shared_ptr<const Pre> Prepare(const assets::prop_gs::PropSet& set) {
  auto pre = std::make_shared<Pre>();
  pre->set = &set;
  pre->hash.resize(set.textures.size());
  pre->wide.resize(set.textures.size());
  for (size_t i = 0; i < set.textures.size(); ++i) {
    const assets::Texture& t = set.textures[i];
    if (t.data.empty() || t.width == 0 || t.height == 0) continue;
    const uint32_t n = std::min(std::max(1u, t.mips), 16u);
    pre->hash[i].resize(n);
    for (uint32_t m = 1; m <= n; ++m) pre->hash[i][m - 1] = FilePrefixHash(t, m);
    pre->bytes += size_t(n) * 8;
    const uint32_t fit = t.format == assets::TexFormat::kB4G4R4A4 ? FitMips(t, 255) : 0;
    size_t src = 0;
    pre->wide[i].resize(fit);
    for (uint32_t m = 0; m < fit; ++m) {
      const uint32_t mw = std::max(1u, t.width >> m), mh = std::max(1u, t.height >> m);
      WidenMip(t, src, mw, mh, pre->wide[i][m]);
      pre->bytes += pre->wide[i][m].size();
      src += MipBytes(t.format, mw, mh);
    }
  }
  if (g_plant.load(std::memory_order_relaxed) == 8 && pre->hash.size() > 1) std::rotate(pre->hash.begin(), pre->hash.begin() + 1, pre->hash.end());   // plant 8 stale_pre: the table of another texture order (the identity check passes: the content checks must fail)
  return pre;
}

}  // namespace rdr_scene::props_budget
