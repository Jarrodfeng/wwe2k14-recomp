// S111 CACTHUMB: R7, the Create a Title thumbnail (docs\research\s111\CACTHUMB.md). Pure: std + readback_ours.h's core (tools\readback\rbc_unit.cpp builds it).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C listings]: the thumbnail object (ctor sub_83343A88, kept at [0x839BFC30], a render node in category 50, update sub_83343C90) resolves the rect (0, 0, 256, 128) of the frame's target
// (state 3, sub_83343678, op 65) into its 256x128 8888 texture, then queues the render-thread callback sub_833438B0 (state 4, op 68; payload word 0 = the object). The callback locks the texture
// (lock return 0x8334398C), untiles it, encodes DXT5 256x128 into obj+168 behind the DDS header at obj+40, swaps the 16-bit words and sets obj+28 = 1; state 5 waits for obj+28. The save step
// sub_8333BF78 copies the 32896-byte DDS image into the title's record. The capture camera (sub_8333C438 mode 2) frames the belt into that top-left corner of the normal 3D frame.
// Ours (stream-free): no resolve runs, so the texture held stale memory (the garbled saved thumbnail). The callback is DEFERRED (title_thumb_off.h); at a present the picture (the scene's top-left
// 256/W x 128/H, box-scaled to 256x128) is handed over here and the callback is run; its lock writes it (ServeTitleShot). No picture = BLACK (never stale memory into the save).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// The core of readback_ours.h must come first (its game part includes THIS file; the unit includes it before this). No include of it here: a cycle under
// `#if !defined(WWE2K14_READBACK_CORE_ONLY)` never ends the cvar shard walk (tools\compile\cvar_shard.py reach).
#include "readback_tile.h"

namespace wwe2k14::title_thumb {

inline constexpr uint32_t kObjGlobal = 0x839BFC30u;   // the thumbnail object (sub_83334888 +48 writes it)
inline constexpr uint32_t kWaitState = 5;             // obj+16: state 5 waits for obj+28 (sub_83343C90, 0x83343CEC)
inline constexpr uint32_t kW = 256, kH = 128;         // the resolve rect (sub_83343678 +20 .. +30) = the texture (sub_83343B90 +98 .. +A0)

// The resolve rect (0, 0, 256, 128) of the game's game_w x game_h frame, in the pixels of our picture of that frame (pw x ph). Empty when a size is 0.
inline readback_ours::ThumbRect Crop(uint32_t pw, uint32_t ph, uint32_t game_w, uint32_t game_h) {
  readback_ours::ThumbRect r;
  if (!pw || !ph || !game_w || !game_h) return r;
  if (RBC_PLANT(14)) {   // (plant 14: the whole frame, not the resolve rect)
    r.r = int32_t(pw), r.b = int32_t(ph);
    return r;
  }
  r.r = int32_t(std::clamp<uint64_t>((uint64_t(kW) * pw + game_w / 2) / game_w, 1, pw));
  r.b = int32_t(std::clamp<uint64_t>((uint64_t(kH) * ph + game_h / 2) / game_h, 1, ph));
  return r;
}

// Our scene picture -> the 256x128 thumbnail (the host thread, once a capture). false = no usable picture.
inline bool MakePicture(const uint8_t* rgba, uint32_t pw, uint32_t ph, uint32_t game_w, uint32_t game_h, std::vector<uint8_t>& out) {
  out.clear();
  if (!rgba || !pw || !ph) return false;
  const readback_ours::ThumbRect r = Crop(pw, ph, game_w, game_h);
  if (r.r <= r.l || r.b <= r.t) return false;
  std::vector<uint8_t> crop;
  readback_ours::CropRgba(rgba, pw, r, crop);
  readback_tile::ScaleBox(crop.data(), uint32_t(r.r - r.l), uint32_t(r.b - r.t), kW, kH, out);
  return out.size() == size_t(kW) * kH * 4;
}

// The game's frame size: [0x8392DC3C] == 1 -> 1280x720, else 640x480 (the word R2 reads; sub_8333C438 picks the capture camera by it).
inline void GameSize(uint32_t hd_word, uint32_t& w, uint32_t& h) {
  w = hd_word == 1 ? 1280u : 640u;
  h = hd_word == 1 ? 720u : 480u;
}

// The callback's object may be run only while it still waits: still the thumbnail object, in state 5, obj+28 still 0.
template <class Load32>
inline bool Waiting(uint32_t obj, Load32&& ld) {
  if (RBC_PLANT(15)) return obj != 0;   // (plant 15: no guard: an object the editor moved on is run anyway)
  return obj != 0 && ld(kObjGlobal) == obj && ld(obj + 16) == kWaitState && ld(obj + 28) == 0;
}

// The picture handed to the callback's re-run (title_thumb_off.h sets it around __imp__sub_833438B0 on this thread; rgba null = black).
struct Delivered {
  bool active = false;
  const uint8_t* rgba = nullptr;   // kW x kH RGBA
};
inline thread_local Delivered t_delivered;

// R7's lock: the 256x128 guard, then the picture (exactly kW x kH) or BLACK. Counted served / zeros (+ unserved{none_why}) / unserved (refused: nothing written).
inline readback_tile::Why ServeTitleShot(int site, const uint32_t w[6], readback_ours::PhysFn phys, const readback_ours::Guard& g, const uint8_t* pic, readback_tile::Why none_why) {
  using readback_tile::Why;
  readback_tile::Plan p;
  Why why = readback_ours::Prepare(w, g, p);
  if (why != Why::Ok) {
    readback_ours::NoteUnserved(site, why);
    return why;
  }
  if (!pic) {
    if (RBC_PLANT(16)) {   // (plant 16: no picture leaves the memory as it was = garbage into the title's save)
      readback_ours::NoteUnserved(site, none_why);
      return none_why;
    }
    const Why wb = readback_ours::WriteBlack(p, phys, g);
    if (wb == Why::Ok) readback_ours::NoteZeros(site, none_why);
    else readback_ours::NoteUnserved(site, wb);
    return wb == Why::Ok ? none_why : wb;
  }
  why = readback_ours::Write(p, phys, g, pic, kW, kH);
  if (why == Why::Ok) readback_ours::NoteServed(site);
  else readback_ours::NoteUnserved(site, why);
  return why;
}

}  // namespace wwe2k14::title_thumb
