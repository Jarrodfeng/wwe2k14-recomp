// The stage screens' live picture source, the pure rules (S103 lane M; docs\renderer\STAGE_LIVE_WRITERS.md section 2b). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only; NO game, SDK or renderer-adapter includes: the game glue (screen_planes_glue.h) and the offline unit (tools\scene\screen_planes_unit.cpp) run THESE functions.
// Marks: [C] read in a listing (cited), [L] a reading, [U] unknown (names the work).
//
// WHAT IT DOES. The movie present (sub_827E0018) copies each new decoded frame into the movie's DISPLAY texture set and flushes it (sub_827FB540, r3 = the set) [C listing 0x827E0098-0x827E00B0:
// `xori 1` the toggle at movie+192, `bl sub_82805960` the copy, `lwz r3,36(r31); bl sub_827FB540`]. A flush therefore means "these planes hold a complete new frame". At that flush (inside the present; which thread is [U])
// OnFlush COPIES the frame's planes into a store (a snapshot the render thread can hold for as long as it likes); nothing here is ever read from guest memory by the render thread.
//
// THE SET LAYOUT (a Bink BINKFRAMEBUFFERS block inside the 248-byte texture set) [C sub_827FBBF0 0x827FBBFC-0x827FBD64 and sub_827FB540's loop, video_tron.md]:
//   +128 count of frame buffers (the creation loop's bound; the flush walks it)       +132 / +136  Y (and A) plane width / height  (sub_827FBBF0: r3 = [r29+132], r4 = [r29+136] for planes 0 and 3)
//   +140 / +144  chroma plane width / height (planes 1 and 2: r3 = [r29+140], r4 = [r29+144])
//   +148 the DISPLAY buffer index = the toggle (sub_82805960 0x828059A0 `stw r30,148(r10)` with r30 = movie+192; sub_827DFEB8 reads the same toggle from movie+192)
//   buffer i, plane k (0 Y, 1 cR, 2 cB, 3 A): allocate flag +152 + 12k + 48i, data pointer +156 + 12k + 48i, pitch +160 + 12k + 48i (sub_827FBBF0 passes r7 = r30 (pointer slot) and r8 = r30 + 4 (pitch slot);
//   sub_827FBB10 stores the lock's pitch there). Plane k of the draw = the sprite's sampler slot k (sub_82BC2B50 / 2290 / 2298 / 22B0 take planes 0..3), slot 1 = Cb and slot 2 = Cr in the game's own
//   shader (yuv_rgba.h). The draw's plane argument is NOT the data plane: sub_827F9D88 0x827F9E08-0x827F9E4C hands draw plane 1 (Cb) DATA plane 2 and draw plane 2 (Cr) DATA plane 1
//   (Bink order Y, cR, cB, A), so DATA plane 1 (cR) is stored in Frame::cr and DATA plane 2 (cB) in Frame::cb [C B1 claims, S103]. (Before: the other way, trons showed blue where the game is red.)
// THE CHANNEL -> SET CHAIN (what sub_827F9D88 + sub_827DFEB8 do) [C listings]: T = [0x8392477C] (the titan manager); channel ch (0..5): slot = [T + 84 ch + 32]; controller = [T + 16];
//   the slot's record is controller + 2144 + 32 slot (state at +4 == 1 means ready, sub_827DFEB8 0x827DFEC4); the movie = [record + 28]; the display set = [movie + 36].
// WHICH CHANNEL A STAGE SCREEN SHOWS: the key -> channel table is ScreenBinding::LoadDefaults below (803 / 804 / 807 -> channel 0, 805 -> channel 2 and the panels 801 / 802 / 806 / 808 / 809 -> channel 1, each with the picture size it expects; 800 / 803 unbound and counted).
// An unopened channel ALIASES controller slot 0 (slot word 0), so a frame is used only when its size is one a bound key expects.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "stage_fx_core.h"   // ReadFn, ReadWord, Swap32
#include "../tron_sheet_core.h"   // S110 TRONSHEET: kSheetSize (std only)

namespace rdr_scene::live::screens {

using stagefx::ReadFn;
// S108 RDS phase 2: a plane's rows in one call (`rows` x `width` bytes, `pitch` apart, packed into dst); the game's is wrestler_hook.h ReadGuestRows (one page check of the rows' hull: safe_guest_pages.h
// RowsHullOk / ReadRows). Null = one `read` a row (the units, and the old path).
using RowsFn = bool (*)(uint32_t guest, uint32_t pitch, uint32_t rows, uint32_t width, uint8_t* dst);

inline constexpr uint32_t kTitanGlobal = 0x8392477C;   // [C q addr] the titan manager pointer
inline constexpr uint32_t kTitanController = 16;       // T + 16 [C sub_827F9D88 0x827F9D9C `lwz r4,16(r4)`]
inline constexpr uint32_t kTitanSlotOfChannel = 32;    // T + 84 ch + 32 [C 0x827F9DBC-0x827F9DC4]
inline constexpr uint32_t kTitanStride = 84;
inline constexpr uint32_t kChannels = 6;               // sub_827F9D88 accepts 0 <= ch < 6 (0x827F9DB0-0x827F9DB8)
inline constexpr uint32_t kCtrlSlots = 6;              // sub_827E0018 walks 6 slot records (r27 = 6)
inline constexpr uint32_t kCtrlRecords = 2144, kCtrlStride = 32, kRecState = 4, kRecMovie = 28;   // [C sub_827DFEB8 0x827DFEB8-0x827DFED0, sub_827E0018 0x827E004C]
inline constexpr uint32_t kMovieDisplaySet = 36;       // movie + 36 [C sub_827E0018 0x827E00AC]
inline constexpr uint32_t kSetCount = 128, kSetYW = 132, kSetYH = 136, kSetCW = 140, kSetCH = 144, kSetIndex = 148, kSetPlanes = 152, kPlaneStride = 12, kBufferStride = 48;
inline constexpr uint32_t kMaxDim = 4096;              // the screens' own limit (rf_c.h)
inline constexpr uint32_t kMaxBuffers = 2;             // a set holds 2 frame buffers: 152 + 2 * 48 = 248 = the set's size (sub_827FBBF0's 248-byte set; review F2)
inline constexpr uint32_t kMaxPitch = 8192;            // a plane row is at most 8192 bytes (largest movie 1280 wide; refuses a garbled header)
inline constexpr uint32_t kNonMovieFlag = 60;          // T + 84 slot + 60: non-zero = sub_827F9D88 takes the plane from [T+24]'s vtable +56, not from the movie [C 0x827F9DD0-0x827F9E04]
inline constexpr uint32_t kRing = 4;                   // frames kept per set: the record lags the game by a frame or two, so a row's serial is found, not "the newest"
inline constexpr size_t kMaxSets = 12;                 // display sets kept (review F3)
// S103 E7: no display-set flush of ANY set for this many guest frames = the game plays no movie (a decoded frame flushes every video frame): see PlaneStore::Idle.
inline constexpr uint64_t kIdleFlushFrames = 60;
inline constexpr uint64_t kStaleFrames = 90;           // a set with no flush for this many guest frames emits no row and answers no picture, so the screen's texture FREEZES on its last picture (nothing is uploaded; the movie ended / was closed)

// Planted faults (offline unit only; the live path never sets them). Each makes one thing wrong and must fail the check that names it.
struct Plants {
  bool ignore_channels = false; // S103 E7 twin: ChannelsQuiet always true
  bool wrong_buffer = false;    // the other half of the display double buffer (the frame BEFORE this one)
  bool swap_channels = false;   // channels 1 and 2 are resolved to each other's movie (a screen bound to the wrong movie slot)
  bool lazy_copy = false;       // the "copy" keeps a view of the guest bytes and reads them at Planes() time (tears when the decoder rewrites)
  bool swap_chroma = false;     // data planes 1 and 2 handed over the old, wrong way round (cR into Cb)
  bool unbound_falls_back = false; // a key with no binding shows the first bound key's movie
  bool ignore_serial = false;   // the row's serial is ignored: always the newest frame
  bool ignore_state = false;    // a channel whose record is not ready (state != 1) is treated as ready
  bool ignore_expected_size = false; // the expected-size check is skipped (a 512x256 alias frame is copied and shown for a 320x320 key)
  bool ignore_pitch = false;    // rows read as if pitch == width
  bool stale_screen_pic = false; // S103 SCN twin: every flush after a set's FIRST copy is dropped (the picture never refreshes: run 38's static 805)
  bool header_chroma = false;   // S103 P4: copy the chroma planes at the set header's own size (+140 / +144: a 1280x280 movie has 640x144 chroma textures, the decoder pads to 16 rows) instead of (w+1)/2 x (h+1)/2; the movie draw's uv spans the WHOLE plane texture, so a 140-row copy would rescale the chroma. Default false: the stage screens' conversion expects (w+1)/2 x (h+1)/2
};

// One decoded frame, copied (packed rows). cb / cr are (w+1)/2 x (h+1)/2.
struct Frame {
  uint32_t set = 0, w = 0, h = 0, cw = 0, ch = 0;
  uint64_t id = 0;                // global, strictly increasing over accepted flushes: the content id; its low 32 bits are the row serial
  uint64_t guest_frame = 0;       // the flush's guest frame (staleness)
  bool has_alpha = false;
  std::vector<uint8_t> y, cb, cr, a;
  // Plant lazy_copy only: where the bytes really are (read at use)
  uint32_t lazy_ptr[4] = {};
  uint32_t lazy_pitch[4] = {};
};

struct StoreCensus {
  uint64_t flushes = 0, copied = 0, bytes = 0, sets_seen = 0, bad_layout = 0, unreadable = 0, unbound_skipped = 0, resolved_none = 0;
  uint64_t asked = 0, answered = 0, serial_exact = 0, serial_newest = 0, stale = 0, no_frame = 0, non_movie = 0, toggle_mismatch = 0, pruned = 0, wrong_size_skipped = 0, no_stage_skipped = 0, rows_wrong_size = 0;
};

// The mapping screen key (800-809) -> titan channel (0..5) with the picture size the key's texture expects. Text form "803:0@320x320,805:2@256x128" (the size is optional: no size = any).
// Unopened channels ALIAS controller slot 0 (their slot word is 0: run 12 tchan c0..c5 -> s0 until the tron opened, when slot 0 held the 512x256 title logo): a bound key therefore carries an EXPECTED SIZE, and a
// frame of another size is never copied, never given a row and never sizes the screen's texture (review B1).
class ScreenBinding {
 public:
  ScreenBinding() { LoadDefaults(false); }
  // THE DEFAULT TABLE (S103 lane M, run 12 + the S97 mapping). Evidence per key:
  //  803 / 804 / 807 = sampler `main&banner` = the WHOLE 320x320 titantron sheet by the arena file's UVs [C by UV arithmetic, src\assets\stage_media.h, S97 run 6812]: 804 is the big centre screen (v 0-0.699 = rows
  //    0-223 of the sheet), 807 the banner band all round the arena (the strips of rows 224-319), 803 the same quad over the whole sheet; the sheet is the streamed titantron = titan channel 0 (run 12: once
  //    343.bik2 opened, channel 0 -> display set 320x320) [C log]; Jarrod: the centre screen shows the titantron, the banners are the strips at the bottom of the same file [Jarrod]. Expected size 320x320.
  //  805 = sampler `mini_tron` = the wall movie MVSB / MVAB (sub_8232B608 `which` 1 -> the entrance object's second memory movie = channel 2, sub_82344420) [C listings]; run 12: channel 2 -> display set 256x128
  //    = the size stage_media.h gives for it [L]. Expected size 256x128.
  //  800 (titantron_movie) is left UNBOUND: stage_media.h:11 and :271-274 give it rows 0-223 only (marked unknown), front_end.cpp overrides it in the match look; binding the whole sheet is not backed [U].
  //  801 / 802 / 806 / 808 / 809 = sampler `ppv_header&wall` / `ss_header&wall` = the panel movie MVSW else MOV (sub_8232B608 `which` 0 = the entrance object's FIRST memory movie = channel 1 [C listings]).
  //    BOUND to channel 1 @ 256x256 (S103 M2, mode-1 run 18, wwe2k14_7271.log, `screen_bind_join.py`): channel 1 opens at guest frame 4217 (c1->s1, rec+44 = 5 = open) on display set 0xAF3A5F10 (movie 0xAF3A5D00,
  //    the pooled block that held the 512x256 boot logo), whose size changes ONCE, 512x256 (frame 518) -> 256x256 (frame 4203), and stays 256x256 to the end of the log (frame 5721) [C log]; stage_media.h
  //    gives the panel sheet as the 256x256 `ppv_header&wall` texture used by UV as stored (S97 run 6812's real frames) [C by size]. Two independent sources agree on the size; that the channel-1 movie IS the
  //    panels' MVSW / MOV rests on the `which` chain + that size match, not on a table-handle join (the screens' model table stays zero) [L]. The expected size keeps the boot-logo alias (512x256) out.
  //    `wrong_panels` is the planted fault: the panels bound to channel 2 (the 256x128 mini-tron movie).
  //  The 1280x280 set on channel 5 is the entrance's 2D remnant movie (most likely the location banner), not a stage screen.
  // `wrong` is the planted fault (the unit): the banner band 807 bound to channel 5 (a 1280x280 movie against the 320x320 the key expects) instead of the titantron.
  void LoadDefaults(bool wrong, bool wrong_panels = false) {
    Clear();
    // (803 is the same quad as 804 over the whole sheet; the game draws it in NEITHER look and the live model has no family for it, so it is not bound: a row for it would be a render-side hole)
    constexpr uint32_t kSheet = wwe2k14::tron_sheet::kSheetSize;   // S110 TRONSHEET: the sheet size is ONE constant; the tron shim hands the game a texture of exactly it
    Set(804, 0, kSheet, kSheet), Set(807, wrong ? 5 : 0, kSheet, kSheet), Set(805, 2, 256, 128);
    for (uint32_t k : {801u, 802u, 806u, 808u, 809u}) Set(k, wrong_panels ? 2 : 1, 256, 256);   // S103 M2: the panel sheet (channel 1, 256x256)
  }
  void Clear() {
    for (auto& c : ch_) c.store(-1, std::memory_order_relaxed);
    for (auto& s : size_) s.store(0, std::memory_order_relaxed);
  }
  void Set(uint32_t key, int channel, uint32_t w = 0, uint32_t h = 0) {
    if (key < 800 || key > 809 || channel < -1 || channel >= int(kChannels) || w > 65535 || h > 65535) return;
    ch_[key - 800].store(int8_t(channel), std::memory_order_relaxed);
    size_[key - 800].store((w << 16) | h, std::memory_order_relaxed);
  }
  int ChannelOf(uint32_t key) const { return key >= 800 && key <= 809 ? ch_[key - 800].load(std::memory_order_relaxed) : -1; }
  // The picture size key `key` expects; false = the key has no expectation (unbound, or bound without a size).
  bool ExpectedSize(uint32_t key, uint32_t& w, uint32_t& h) const {
    if (key < 800 || key > 809 || ch_[key - 800].load(std::memory_order_relaxed) < 0) return false;
    const uint32_t v = size_[key - 800].load(std::memory_order_relaxed);
    if (!v) return false;
    w = v >> 16, h = v & 0xFFFF;
    return true;
  }
  // true when some key bound to `channel` accepts a w x h picture (a key with no expected size accepts any).
  bool ChannelAccepts(uint32_t channel, uint32_t w, uint32_t h) const {
    for (uint32_t k = 0; k < 10; ++k) {
      if (ch_[k].load(std::memory_order_relaxed) != int(channel)) continue;
      const uint32_t v = size_[k].load(std::memory_order_relaxed);
      if (!v || ((v >> 16) == w && (v & 0xFFFF) == h)) return true;
    }
    return false;
  }
  bool AnyBound() const {
    for (const auto& c : ch_)
      if (c.load(std::memory_order_relaxed) >= 0) return true;
    return false;
  }
  bool ChannelWanted(uint32_t channel) const {
    for (const auto& c : ch_)
      if (c.load(std::memory_order_relaxed) == int(channel)) return true;
    return false;
  }
  // "803:0@320x320,805:2" -> bindings; returns the number parsed, -1 on a malformed entry (nothing changed then).
  int Parse(const std::string& text) {
    std::array<int8_t, 10> next;
    std::array<uint32_t, 10> nsize;
    next.fill(-1), nsize.fill(0);
    int n = 0;
    size_t i = 0;
    while (i < text.size()) {
      size_t e = text.find(',', i);
      if (e == std::string::npos) e = text.size();
      std::string item = text.substr(i, e - i);
      i = e + 1;
      while (!item.empty() && item.front() == ' ') item.erase(item.begin());
      if (item.empty()) continue;
      const size_t c = item.find(':');
      if (c == std::string::npos) return -1;
      char* end = nullptr;
      const long key = std::strtol(item.c_str(), &end, 10);
      if (end != item.c_str() + c) return -1;
      const long ch = std::strtol(item.c_str() + c + 1, &end, 10);
      uint32_t sz = 0;
      if (*end == '@') {
        const long w = std::strtol(end + 1, &end, 10);
        if (*end != 'x') return -1;
        const long h = std::strtol(end + 1, &end, 10);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
        sz = (uint32_t(w) << 16) | uint32_t(h);
      }
      if (*end != 0 || key < 800 || key > 809 || ch < 0 || ch >= long(kChannels)) return -1;
      next[size_t(key - 800)] = int8_t(ch);
      nsize[size_t(key - 800)] = sz;
      ++n;
    }
    for (size_t k = 0; k < 10; ++k) ch_[k].store(next[k], std::memory_order_relaxed), size_[k].store(nsize[k], std::memory_order_relaxed);
    return n;
  }
  // The one place the dev cvar text is applied (the glue and the unit call THIS): "" = the default table stays; "none" = every screen unbound; else the table is REPLACED by the text; a malformed text
  // changes nothing (the DEFAULT table stays). `msg` says what happened.
  int ApplySpec(const std::string& spec, std::string& msg) {
    if (spec.empty()) return msg = "default table", 0;
    if (spec == "none") return Clear(), msg = "every screen unbound (none)", 0;
    const int n = Parse(spec);
    if (n < 0) return msg = "malformed (key:channel[@WxH],... with key 800-809, channel 0-5, or 'none'): the DEFAULT table stays", -1;
    msg = std::to_string(n) + " binding(s), replacing the default table";
    return n;
  }
 private:
  std::array<std::atomic<int8_t>, 10> ch_;
  std::array<std::atomic<uint32_t>, 10> size_;   // (w << 16) | h, 0 = any
};

// The decoded frames, by display set. OnFlush runs inside the movie present (thread [U]); Latest / Find / Census on others: one mutex over the pointers (the copy itself is outside it).
class PlaneStore {
 public:
  Plants plants;
  ScreenBinding binding;
  bool (*stage_live)() = nullptr;   // review C1: null = always (the unit); the glue sets StageFx().Stage() != 0

  // Channel -> display set, by the chain above. 0 = unresolved (no manager, channel unopened, not ready).
  static uint32_t SetOfChannel(ReadFn read, uint32_t channel, const Plants& pl, uint32_t* movie_out = nullptr, bool* non_movie = nullptr) {
    if (channel >= kChannels) return 0;
    if (pl.swap_channels) channel = channel == 1 ? 2 : channel == 2 ? 1 : channel;   // planted: channels 1 and 2 trade movies
    uint32_t t = 0, ctrl = 0, slot = 0, state = 0, movie = 0, set = 0;
    if (!stagefx::ReadWord(read, kTitanGlobal, t) || t < 0x10000) return 0;
    if (!stagefx::ReadWord(read, t + kTitanController, ctrl) || ctrl < 0x10000) return 0;
    if (!stagefx::ReadWord(read, t + kTitanStride * channel + kTitanSlotOfChannel, slot) || slot >= kCtrlSlots) return 0;
    uint32_t nm = 0;   // review F5: sub_827F9D88 tests [T + 84 slot + 60] first; non-zero = the planes come from another object, not a movie display set
    if (stagefx::ReadWord(read, t + kTitanStride * slot + kNonMovieFlag, nm) && nm != 0) {
      if (non_movie) *non_movie = true;
      return 0;
    }
    const uint32_t rec = ctrl + kCtrlRecords + kCtrlStride * slot;
    if (!stagefx::ReadWord(read, rec + kRecState, state) || (state != 1 && !pl.ignore_state)) return 0;
    if (!stagefx::ReadWord(read, rec + kRecMovie, movie) || movie < 0x10000) return 0;
    if (!stagefx::ReadWord(read, movie + kMovieDisplaySet, set) || set < 0x10000) return 0;
    if (movie_out) *movie_out = movie;
    return set;
  }

  // The flush of display set `set` at guest frame `gf`: copy its display buffer if some bound channel resolves to it. Returns true when a frame was stored.
  bool OnFlush(ReadFn read, uint32_t set, uint64_t gf, RowsFn rows = nullptr) {
    if (plants.lazy_copy) lazy_read_ = read;   // (the lazy_copy plant only)
    {
      std::lock_guard<std::mutex> lk(m_);
      ++c_.flushes;
      if (gf > last_flush_gf_) last_flush_gf_ = gf;   // S103 E7: any set, before every refusal below (a movie that plays flushes, whether or not we copy it)
    }
    if (!binding.AnyBound()) {
      std::lock_guard<std::mutex> lk(m_);
      ++c_.unbound_skipped;
      return false;
    }
    if (stage_live && !stage_live()) {   // review C1: no stage object = no stage screen to feed: no resolution, no copy
      Bump(&StoreCensus::no_stage_skipped);
      return false;
    }
    bool wanted = false;
    uint32_t wanted_movie = 0;
    for (uint32_t ch = 0; ch < kChannels; ++ch) {   // refresh the channel -> set cache for every bound channel (the latch and the render thread read the cache, never the guest)
      if (!binding.ChannelWanted(ch)) continue;
      uint32_t mv = 0;
      bool nm = false;
      const uint32_t cs = SetOfChannel(read, ch, plants, &mv, &nm);
      if (nm) Bump(&StoreCensus::non_movie);
      NoteChannelSet(ch, cs);
      if (cs == set) wanted = true, wanted_movie = mv;
    }
    if (!wanted) {
      std::lock_guard<std::mutex> lk(m_);
      ++c_.resolved_none;
      return false;
    }
    {   // review B1: an unopened channel aliases controller slot 0 (a movie of another size, e.g. the 512x256 title logo): only a frame of a size some bound key expects is copied
      uint32_t hw[2] = {0, 0};
      if (read && read(set + kSetYW, hw, sizeof hw)) {
        const uint32_t w = stagefx::Swap32(hw[0]), h = stagefx::Swap32(hw[1]);
        bool accepts = plants.ignore_expected_size;
        for (uint32_t ch = 0; ch < kChannels && !accepts; ++ch)
          if (ChannelSetIs(ch, set) && binding.ChannelAccepts(ch, w, h)) accepts = true;
        if (!accepts) {
          Bump(&StoreCensus::wrong_size_skipped);
          return false;
        }
      }
    }
    {   // the present flips the toggle (movie+192) and then copies into the set (set+148 = the toggle) before it flushes: a flush where they differ had no copy behind it (review F7)
      uint32_t tog = 0, idx = 0;
      if (stagefx::ReadWord(read, wanted_movie + 192, tog) && stagefx::ReadWord(read, set + kSetIndex, idx) && tog != idx) {
        Bump(&StoreCensus::toggle_mismatch);
        return false;
      }
    }
    if (plants.stale_screen_pic) {   // planted fault: the first copy of a set is kept, every later one dropped
      std::lock_guard<std::mutex> lk(m_);
      if (Slot* old = FindSlot(set); old && old->latest) return false;
    }
    std::shared_ptr<Frame> f = TakeSpare(set);
    if (!CopyDisplay(read, set, *f, rows)) return false;
    f->guest_frame = gf;
    std::lock_guard<std::mutex> lk(m_);
    f->id = ++next_id_;
    Prune(gf);
    Slot* s = SlotOf(set);
    s->ring[s->next++ % kRing] = f;
    s->latest = f;
    ++c_.copied;
    c_.bytes += f->y.size() + f->cb.size() + f->cr.size() + f->a.size();
    return true;
  }

  // The frame screen `key` shows for row serial `serial` (low 32 bits of a frame id): the exact one if still held, else the newest. null = unbound / no frame / stale.
  std::shared_ptr<const Frame> Find(ReadFn read, uint32_t key, uint32_t serial, uint64_t gf_now) {
    int ch = binding.ChannelOf(key);
    if (ch < 0 && plants.unbound_falls_back)
      for (uint32_t k = 800; k <= 809 && ch < 0; ++k) ch = binding.ChannelOf(k);
    std::lock_guard<std::mutex> lk(m_);
    ++c_.asked;
    if (ch < 0) return nullptr;
    const uint32_t set = SetOfChannelCached(read, uint32_t(ch));
    Slot* s = set ? FindSlot(set) : nullptr;
    if (!s || !s->latest) {
      ++c_.no_frame;
      return nullptr;
    }
    // render-clock: lookup - [U] screen stale by guest-frame gap; the game's movie ended state is not looked up yet
    if (gf_now > s->latest->guest_frame + kStaleFrames) {
      ++c_.stale;
      return nullptr;
    }
    uint32_t ew = 0, eh = 0;
    const bool sized = !plants.ignore_expected_size && binding.ExpectedSize(key, ew, eh);
    for (const auto& f : s->ring)
      if (!plants.ignore_serial && f && uint32_t(f->id) == serial) {
        if (sized && (f->w != ew || f->h != eh)) return ++c_.rows_wrong_size, nullptr;
        ++c_.answered, ++c_.serial_exact;
        return Materialise(read, f);
      }
    if (sized && (s->latest->w != ew || s->latest->h != eh)) return ++c_.rows_wrong_size, nullptr;
    ++c_.answered, ++c_.serial_newest;
    return Materialise(read, s->latest);
  }

  // The rows the latch emits: every bound key whose set holds a fresh frame, its newest serial and size. Never reads guest memory itself when the cache holds the channel's set.
  size_t Rows(ReadFn read, uint64_t gf_now, uint16_t* keys, uint32_t* serials, uint32_t* ws, uint32_t* hs, size_t cap) {
    size_t n = 0;
    for (uint32_t key = 800; key <= 809 && n < cap; ++key) {
      const int ch = binding.ChannelOf(key);
      if (ch < 0) continue;
      std::lock_guard<std::mutex> lk(m_);
      const uint32_t set = SetOfChannelCached(read, uint32_t(ch));
      Slot* s = set ? FindSlot(set) : nullptr;
      // render-clock: lookup - [U] screen stale by guest-frame gap; the game's movie ended state is not looked up yet
      if (!s || !s->latest || gf_now > s->latest->guest_frame + kStaleFrames) {
        ++rows_no_frame_;
        ++key_no_frame_[key - 800];
        continue;
      }
      uint32_t ew = 0, eh = 0;   // review B1: a key that expects a size gets a row only for a frame of that size (counted rows_wrong_size)
      if (!plants.ignore_expected_size && binding.ExpectedSize(key, ew, eh) && (s->latest->w != ew || s->latest->h != eh)) {
        ++c_.rows_wrong_size;
        continue;
      }
      keys[n] = uint16_t(key), serials[n] = uint32_t(s->latest->id), ws[n] = s->latest->w, hs[n] = s->latest->h;
      ++n;
      ++rows_bound_;
      ++key_bound_[key - 800];
    }
    if (!binding.AnyBound()) ++rows_unbound_calls_;
    return n;
  }

  StoreCensus Census() const {
    std::lock_guard<std::mutex> lk(m_);
    StoreCensus c = c_;
    c.sets_seen = slots_.size();
    return c;
  }
  // S103 E7: the game plays NO movie right now: no display set has flushed for kIdleFlushFrames guest frames (the decoder's present flushes a set per decoded video frame; run 26, a match with the
  // entrances skipped: 2857 flushes, all before the stage, none in 3950 match frames, and the 666 captured match frames hold 0 movie-class draws). Then no screen has a picture to wait for.
  // `gf_now` is the guest frame now (Now()); a store that never saw a flush is idle once gf_now > the gap (nothing has ever played).
  bool Idle(uint64_t gf_now) const {
    std::lock_guard<std::mutex> lk(m_);
    return gf_now > last_flush_gf_ + idle_window_;
  }
  void SetIdleWindow(uint64_t frames) { std::lock_guard<std::mutex> lk(m_); idle_window_ = frames; }   // (unit twin: 0)
  // S103 E7 (review B4): pictures that do not come through the flush, and a movie that is open but has not flushed yet. At the latch, for each BOUND channel: the titan's non-movie flag (the planes come from
  // another object) or the controller record's state == 1 (open) means something is, or is about to be, shown: not quiet. An unreadable chain is not quiet either (nothing proves there is no movie).
  // Unopened channels alias controller slot 0 (see ScreenBinding): a stale open slot 0 keeps this false. `plants.ignore_channels` (unit twin) says quiet always.
  bool ChannelsQuiet(ReadFn read) const {
    if (plants.ignore_channels) return true;
    uint32_t t = 0, ctrl = 0;
    if (!stagefx::ReadWord(read, kTitanGlobal, t) || t < 0x10000) return false;
    if (!stagefx::ReadWord(read, t + kTitanController, ctrl) || ctrl < 0x10000) return false;
    for (uint32_t ch = 0; ch < kChannels; ++ch) {
      if (!binding.ChannelWanted(ch)) continue;
      uint32_t slot = 0, nm = 0, state = 0;
      if (!stagefx::ReadWord(read, t + kTitanStride * ch + kTitanSlotOfChannel, slot) || slot >= kCtrlSlots) return false;
      if (!stagefx::ReadWord(read, t + kTitanStride * slot + kNonMovieFlag, nm) || nm != 0) return false;
      if (!stagefx::ReadWord(read, ctrl + kCtrlRecords + kCtrlStride * slot + kRecState, state) || state == 1) return false;
    }
    return true;
  }
  uint64_t rows_bound() const { std::lock_guard<std::mutex> lk(m_); return rows_bound_; }
  uint64_t rows_no_frame() const { std::lock_guard<std::mutex> lk(m_); return rows_no_frame_; }
  // S103 SCN: the same two counts per screen key 800-809 (a key whose rows_no_frame outgrows its rows_bound is the one that never gets a picture)
  uint64_t key_rows_bound(uint32_t key) const { std::lock_guard<std::mutex> lk(m_); return key >= 800 && key <= 809 ? key_bound_[key - 800] : 0; }
  uint64_t key_rows_no_frame(uint32_t key) const { std::lock_guard<std::mutex> lk(m_); return key >= 800 && key <= 809 ? key_no_frame_[key - 800] : 0; }
  uint64_t rows_unbound_calls() const { std::lock_guard<std::mutex> lk(m_); return rows_unbound_calls_; }
  // channel -> set cache, refreshed by every flush of a wanted set (the flush runs on the thread that may read guest memory; the latch and the render thread use the cache only).
  void NoteChannelSet(uint32_t channel, uint32_t set) {
    std::lock_guard<std::mutex> lk(m_);
    if (channel < kChannels) ch_set_[channel] = set;
  }

 private:
  uint64_t idle_window_ = kIdleFlushFrames;
  uint64_t last_flush_gf_ = 0;   // S103 E7: the newest guest frame any flush ran at (under m_)
  struct Slot {
    uint32_t set = 0;
    std::shared_ptr<const Frame> ring[kRing], latest;
    uint32_t next = 0;
  };
  // (the cache is filled by OnFlushResolve below; Find / Rows read it under the lock.)
  uint32_t SetOfChannelCached(ReadFn, uint32_t channel) const { return channel < kChannels ? ch_set_[channel] : 0; }
  bool ChannelSetIs(uint32_t channel, uint32_t set) const {   // (the cache OnFlush just refreshed; takes the lock)
    std::lock_guard<std::mutex> lk(m_);
    return channel < kChannels && ch_set_[channel] == set;
  }
  Slot* FindSlot(uint32_t set) {
    for (auto& s : slots_)
      if (s.set == set) return &s;
    return nullptr;
  }
  Slot* SlotOf(uint32_t set) {
    if (Slot* s = FindSlot(set)) return s;
    slots_.push_back(Slot{});
    slots_.back().set = set;
    return &slots_.back();
  }
  // A frame to copy into: the oldest ring entry of this set when nobody else holds it (no allocation, no zero-fill), else a new one (review F7).
  std::shared_ptr<Frame> TakeSpare(uint32_t set) {
    std::lock_guard<std::mutex> lk(m_);
    if (Slot* s = FindSlot(set)) {
      std::shared_ptr<const Frame>& old = s->ring[s->next % kRing];
      if (old && old.use_count() == 1) {
        std::shared_ptr<Frame> f = std::const_pointer_cast<Frame>(old);
        old.reset();
        return f;
      }
    }
    return std::make_shared<Frame>();
  }
  // Drop sets nobody can show any more: no flush for kStaleFrames and not a bound channel's set; at most kMaxSets kept (the oldest go first) (review F3). Caller holds m_.
  void Prune(uint64_t gf) {
    auto bound = [&](uint32_t set) {
      for (uint32_t c = 0; c < kChannels; ++c)
        if (ch_set_[c] == set) return true;
      return false;
    };
    for (size_t i = 0; i < slots_.size();) {
      // render-clock: lookup - [U] screen stale by guest-frame gap; the game's movie ended state is not looked up yet
      const bool dead = !slots_[i].latest || (slots_[i].latest->guest_frame + kStaleFrames < gf && !bound(slots_[i].set));
      if (dead) slots_.erase(slots_.begin() + long(i)), ++c_.pruned;
      else ++i;
    }
    while (slots_.size() >= kMaxSets) {
      size_t oldest = 0;
      for (size_t i = 1; i < slots_.size(); ++i)
        if (slots_[i].latest->guest_frame < slots_[oldest].latest->guest_frame) oldest = i;
      slots_.erase(slots_.begin() + long(oldest)), ++c_.pruned;
    }
  }
  // lazy_copy plant: the frame's bytes are read now, from the guest (a "copy" that is a view).
  std::shared_ptr<const Frame> Materialise(ReadFn, const std::shared_ptr<const Frame>& f) {
    if (!plants.lazy_copy || !lazy_read_) return f;
    const ReadFn read = lazy_read_;
    auto g = std::make_shared<Frame>(*f);
    std::vector<uint8_t>* dst[4] = {&g->y, &g->cr, &g->cb, &g->a};
    const uint32_t wd[4] = {g->w, g->cw, g->cw, g->w}, ht[4] = {g->h, g->ch, g->ch, g->h};
    for (int k = 0; k < 4; ++k) {
      if (!f->lazy_ptr[k] || (k == 3 && !f->has_alpha)) continue;
      for (uint32_t r = 0; r < ht[k]; ++r) read(f->lazy_ptr[k] + r * f->lazy_pitch[k], dst[k]->data() + size_t(r) * wd[k], wd[k]);
    }
    return g;
  }
 public:
  // Copy the display buffer of `set` into `f` (packed). false = the layout is not one we trust (counted bad_layout) or unreadable.
  bool CopyDisplay(ReadFn read, uint32_t set, Frame& f, RowsFn rows = nullptr) {
    uint32_t hdr[6];
    if (!read || !read(set + kSetCount, hdr, sizeof hdr)) { Bump(&StoreCensus::unreadable); return false; }
    for (auto& v : hdr) v = stagefx::Swap32(v);
    const uint32_t count = hdr[0], yw = hdr[1], yh = hdr[2], cw = hdr[3], chh = hdr[4];
    uint32_t idx = hdr[5];
    if (plants.wrong_buffer && count > 1) idx = (idx + 1) % count;
    const uint32_t w = yw, h = yh, need_cw = (w + 1) / 2, need_ch0 = (h + 1) / 2, need_ch = plants.header_chroma ? chh : need_ch0;   // (P4: the header's chroma height when header_chroma)
    if (!count || count > kMaxBuffers || idx >= count || !w || !h || w > kMaxDim || h > kMaxDim || cw < need_cw || chh < need_ch0 || cw > kMaxDim || chh > kMaxDim) {
      Bump(&StoreCensus::bad_layout);
      return false;
    }
    uint32_t ptr[4], pitch[4], alloc[4];
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t base = set + kSetPlanes + kPlaneStride * k + kBufferStride * idx;
      uint32_t pw[3];   // S108 RDS: the plane's three words as ONE 12-byte read (its pages are the three word reads' pages: safe_guest_pages.h HullOk); was three checks
      if (!read(base, pw, sizeof pw)) { Bump(&StoreCensus::unreadable); return false; }
      alloc[k] = stagefx::Swap32(pw[0]), ptr[k] = stagefx::Swap32(pw[1]), pitch[k] = stagefx::Swap32(pw[2]);
      if (k < 3 && !alloc[k]) { Bump(&StoreCensus::bad_layout); return false; }   // planes 0-2 must be allocated (review F2)
    }
    f.set = set, f.w = w, f.h = h, f.cw = plants.header_chroma ? cw : need_cw, f.ch = need_ch;
    f.has_alpha = ptr[3] != 0 && alloc[3] != 0;
    const uint32_t cw_use = plants.header_chroma ? cw : need_cw;
    const uint32_t wdt[4] = {w, cw_use, cw_use, w}, hgt[4] = {h, need_ch, need_ch, h};
    std::vector<uint8_t>* dst[4] = {&f.y, &f.cr, &f.cb, &f.a};
    for (uint32_t k = 0; k < 4; ++k) {
      if (k == 3 && !f.has_alpha) continue;
      if (!ptr[k] || pitch[k] < wdt[k] || pitch[k] > kMaxPitch || uint64_t(ptr[k]) + uint64_t(hgt[k] - 1) * pitch[k] + wdt[k] > 0x100000000ull) { Bump(&StoreCensus::bad_layout); return false; }   // 64-bit row addresses (review F2)
      dst[k]->resize(size_t(wdt[k]) * hgt[k]);   // every byte is overwritten below: no zero-fill
      f.lazy_ptr[k] = ptr[k], f.lazy_pitch[k] = plants.ignore_pitch ? wdt[k] : pitch[k];
      if (plants.lazy_copy) continue;   // the planted "copy" is a view: bytes are read at use
      const uint32_t pk = plants.ignore_pitch ? wdt[k] : pitch[k];
      if (rows) {   // S108 RDS: the plane in one call (one page check of the rows' hull when RowsHullOk; else a read a row inside it): was one check a row, ~1,400 a movie flush
        if (!rows(ptr[k], pk, hgt[k], wdt[k], dst[k]->data())) { Bump(&StoreCensus::unreadable); return false; }
        continue;
      }
      for (uint32_t r = 0; r < hgt[k]; ++r)
        if (!read(ptr[k] + r * pk, dst[k]->data() + size_t(r) * wdt[k], wdt[k])) { Bump(&StoreCensus::unreadable); return false; }
    }
    if (plants.swap_chroma) std::swap(f.cb, f.cr);
    return true;
  }
  void Bump(uint64_t StoreCensus::*field) {
    std::lock_guard<std::mutex> lk(m_);
    ++(c_.*field);
  }

 private:
  mutable std::mutex m_;
  std::vector<Slot> slots_;
  uint32_t ch_set_[kChannels] = {};
  ReadFn lazy_read_ = nullptr;
  uint64_t key_bound_[10] = {}, key_no_frame_[10] = {};
  uint64_t next_id_ = 0, rows_bound_ = 0, rows_no_frame_ = 0, rows_unbound_calls_ = 0;
  StoreCensus c_;
};

}  // namespace rdr_scene::live::screens
