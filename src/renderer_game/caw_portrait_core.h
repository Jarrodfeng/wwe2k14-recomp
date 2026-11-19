// S103 CPB (docs\research\s103\CPR.md section 6): the Create a Superstar roster portrait in the stream-free (OFF) build. Std-only core: the decisions and the byte
// writer, no SDK, no guest memory, so tools\renderer\caw_portrait_unit.cpp builds it alone. The hooks that use it are src\renderer_game\caw_portrait.h.
//
// The game renders the portrait (736x1280, A8R8G8B8, scene object [singleton+60]) and then a worker job (sub_8341B6A8) locks the picture and calls
// xg_untile_to_buffer sub_82CFA318 at 0x8341B77C (return 0x8341B780) to copy it into a CPU buffer; the game crops / resamples / DXT5-encodes that buffer into the
// roster pictures and (CPR 6, [L]) saves them into the player's .cas. In the stream-free build nothing ever wrote the lock's memory, so the buffer was garbage.
// G3 (this file's kUntileLr site) writes OUR picture there, or zeros (a transparent portrait) when there is none: garbage never reaches the save.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace wwe2k14::caw_portrait {

inline constexpr uint32_t kUntileLr = 0x8341B780;   // return address of `bl 0x82cfa318` at 0x8341B77C in sub_8341B6A8 [C asm, q asm sub_8341B6A8]
inline constexpr uint32_t kWidth = 736, kHeight = 1280, kTexel = 4;
inline constexpr uint32_t kMinPitch = kWidth * kTexel;   // 2944: the destination row pitch (r4) is at least one row

// XGUntileSurface's arguments as the call site loads them (asm 0x8341B75C-0x8341B778 [C]):
//   r3 = destination CPU buffer, r4 = destination row pitch ([sp+180]), r5 = destination point (sp+80: x, y words), r6 = the locked bits,
//   r7 = Width, r8 = Height, r9 = source rect (sp+96: left, top, right, bottom words), r10 = texel pitch.
struct UntileCall {
  uint32_t lr = 0, dest = 0, pitch = 0, point = 0, width = 0, height = 0, rect = 0, texel = 0;
};

// The register half of the filter: only the portrait's own call passes. Every other caller of sub_82CFA318 runs the original unchanged.
inline bool RegsMatch(const UntileCall& c) {
#if defined(WWE2K14_CAW_PLANT) && WWE2K14_CAW_PLANT == 2   // planted twin 2: the filter ignores the return address (every caller of sub_82CFA318 would be answered)
  return c.width == kWidth && c.height == kHeight && c.texel == kTexel && c.pitch >= kMinPitch && c.dest != 0 && c.point != 0 && c.rect != 0;
#else
  return c.lr == kUntileLr && c.width == kWidth && c.height == kHeight && c.texel == kTexel && c.pitch >= kMinPitch && c.dest != 0 && c.point != 0 &&
         c.rect != 0;
#endif
}
// The memory half: the destination point is (0, 0) and the source rect is (0, 0, 736, 1280). `rd32` reads one big-endian guest word (nullable-safe: returns false when unreadable).
template <class Rd32>
inline bool PointRectMatch(const UntileCall& c, Rd32&& rd32) {
  uint32_t px = 1, py = 1, rl = 1, rt = 1, rr = 0, rb = 0;
  if (!rd32(c.point, px) || !rd32(c.point + 4, py) || !rd32(c.rect, rl) || !rd32(c.rect + 4, rt) || !rd32(c.rect + 8, rr) || !rd32(c.rect + 12, rb)) return false;
  return px == 0 && py == 0 && rl == 0 && rt == 0 && rr == kWidth && rb == kHeight;
}
// The whole filter (registers first: the memory reads happen only for the portrait's own call).
template <class Rd32>
inline bool IsPortraitUntile(const UntileCall& c, Rd32&& rd32) {
  return RegsMatch(c) && PointRectMatch(c, rd32);
}

// Bytes the destination buffer must hold: pitch * height (the last row needs only width * texel, but the game allocates whole rows).
inline uint64_t DestBytes(uint32_t pitch) { return uint64_t(pitch) * kHeight; }

// No picture: transparent black over the whole destination (every pitch byte, padding included): the roster shows no portrait, and what is saved is clean.
inline void WriteZero(uint8_t* dst, uint32_t pitch) { std::memset(dst, 0, size_t(DestBytes(pitch))); }

// Our picture, host RGBA8 (R, G, B, A in memory order, rw x rh), into the guest buffer: rows at `pitch`, bytes A, R, G, B per pixel (D3DFMT_A8R8G8B8 as the big-endian
// dword 0xAARRGGBB; WriteCopyBack's swap = true branch for a BGRA-swizzled 8888 texture, reader.h WriteCopyBack, then its endian 2 swap [L: not run on the game here]).
// rw x rh other than 736 x 1280 is nearest-scaled; the padding of a row is zeroed.
inline void WritePicture(uint8_t* dst, uint32_t pitch, const uint8_t* rgba, uint32_t rw, uint32_t rh) {
  if (!rgba || !rw || !rh) {
    WriteZero(dst, pitch);
    return;
  }
  for (uint32_t y = 0; y < kHeight; ++y) {
    uint8_t* row = dst + size_t(y) * pitch;
    const uint32_t sy = rh == kHeight ? y : uint32_t(uint64_t(y) * rh / kHeight);
    const uint8_t* src = rgba + size_t(sy) * rw * 4;
    for (uint32_t x = 0; x < kWidth; ++x) {
      const uint32_t sx = rw == kWidth ? x : uint32_t(uint64_t(x) * rw / kWidth);
      const uint8_t* p = src + size_t(sx) * 4;
      uint8_t* o = row + size_t(x) * 4;
#if defined(WWE2K14_CAW_PLANT) && WWE2K14_CAW_PLANT == 1   // planted twin 1: the wrong byte order (R, G, B, A)
      o[0] = p[0];
      o[1] = p[1];
      o[2] = p[2];
      o[3] = p[3];
#else
      o[0] = p[3];   // A
      o[1] = p[0];   // R
      o[2] = p[1];   // G
      o[3] = p[2];   // B
#endif
    }
    if (pitch > kMinPitch) std::memset(row + kMinPitch, 0, pitch - kMinPitch);
  }
}

// CPB2: the game's sub_8341BF58 r31 ([this+40], "the editor's model object") is the CHARACTER object, not the record's engine object. Step-check run 021 [C log]: request model 2863738400 = 0xAAB12E20, and the
// liquid line of the same run reads "character 0xAAB12E20 skeleton 0xAAB142F0" (the CAW census, the guest_model rows and the record's skel:AAB142F0 object all use 0xAAB142F0). The skeleton is [char+48]
// (liquid_hook.h kCharSkeleton, light_latch.h kCharSkeletonOff: sub_8259FAE8 0x8259FCE4 `lwz r3,48(r30)`). The key the record and the host match on is therefore [char+48].
inline constexpr uint32_t kCharSkeletonOff = 48;
template <class Rd32>
inline uint32_t ModelKey(uint32_t character, Rd32&& rd32) {
  uint32_t skel = 0;
  if (character < 0x10000 || !rd32(character + kCharSkeletonOff, skel) || skel < 0x10000) return 0;
  return skel;
}
// On a miss, the engine objects a record holds (first `max`), so the next run says why.
inline std::string CandidatesText(const uint32_t* objs, size_t n, size_t max = 12) {
  std::string s = std::to_string(n) + " engine object(s):";
  size_t shown = 0;
  for (size_t i = 0; i < n && shown < max; ++i) {
    if (!objs[i]) continue;
    char b[16];
    std::snprintf(b, sizeof b, " %08X", objs[i]);
    s += b, ++shown;
  }
  return s;
}

// D1: the portrait draws ONLY the edited wrestler. An object of the record's copy stays visible to the Wrestlers family when its engine object is the model; the draw must then have drawn exactly one object.
inline bool KeepsObject(uint32_t engine_object, uint32_t model) {
#if defined(WWE2K14_CAW_PLANT) && WWE2K14_CAW_PLANT == 4   // planted twin 4: every object is kept (the other wrestlers of the record are drawn into the portrait)
  (void)engine_object, (void)model;
  return true;
#else
  return model != 0 && engine_object == model;
#endif
}
inline bool DrawnCountOk(uint32_t drawn) { return drawn == 1; }

// H1's retry window: a request whose assets are still loading is asked again each host frame. Never longer than G2's wait (kWaitTicks game ticks; the host handles about one present per tick, so the same number of
// frames is the same ~2 s): when the game gave up and ran the job, a host still drawing is wasted work, and G3 cancels the request anyway. The count restarts for each new request id.
struct RetryGate {
  uint64_t id = 0;
  uint32_t tries = 0;
  bool Allow(uint64_t request_id, uint32_t max_tries) {   // true = ask again next frame
#if !(defined(WWE2K14_CAW_PLANT) && WWE2K14_CAW_PLANT == 5)   // planted twin 5: the count carries over from the previous request
    if (request_id != id) id = request_id, tries = 0;
#endif
    return ++tries < max_tries;
  }
  void Done() { tries = 0; }
};

// Counters of the census line (the game's tick thread and the job's worker write them, one at a time: the job only runs after the tick released it).
struct Census {
  uint32_t served = 0, unserved = 0, late = 0, refused = 0, requests = 0, failed = 0;
  uint64_t landed_frame = 0;   // the guest frame the host picture landed on (0 = none yet)
};

// G2: the portrait renderer's tick (sub_8341C600, vtable slot 1, state word [this+48]). State 4 queues the worker job that locks and untiles the picture. While OUR picture for this render has not landed the tick
// returns without running (state stays 4, the job is not queued); after kWaitTicks held ticks the original runs anyway and the portrait is counted late (G3 then writes zeros). No request pending = the original runs at once.
inline constexpr uint32_t kStateQueueJob = 4, kWaitTicks = 120;   // 120 ticks = ~2 s at the game's 60 Hz (the host's retry window, RetryGate, is no longer than this)
enum class TickAct : uint8_t { Run, Hold };
struct TickGate {
  uint32_t waited = 0;
  TickAct Step(uint32_t state, bool request_pending, uint32_t max_wait, Census& c) {
    if (state != kStateQueueJob || !request_pending) {
      waited = 0;
      return TickAct::Run;
    }
    if (waited < max_wait) {
      ++waited;
      return TickAct::Hold;
    }
#if !(defined(WWE2K14_CAW_PLANT) && WWE2K14_CAW_PLANT == 3)   // planted twin 3: a timed-out wait is not counted late
    ++c.late;
#endif
    waited = 0;
    return TickAct::Run;
  }
};

}  // namespace wwe2k14::caw_portrait
