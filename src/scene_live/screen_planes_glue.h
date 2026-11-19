// The stage screens' live pictures: the game plumbing (S103 lane M; docs\renderer\STAGE_LIVE_WRITERS.md section 2b). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Installs (1) the display-set flush function the sub_827FB540 hook calls (screen_planes_hook.h), (2) the ScreenSource provider of the stage-fx contributor, (3) the YuvScreenFeed over the store's
// PlaneSource as the render side's ScreenFeed. Read only: nothing is written to the guest, no register changes, no call is skipped, NO NEW HOOK (the flush hook already exists in d3d_textures.h).
// The flush is called inside the movie present (sub_827E0018) once the frame is copied into the display set, so the planes are final there. WHICH THREAD runs it is [U] (ADAPTER_CALLS.md 4.1 says resource calls may come from any game thread): the store is mutex-guarded and the render thread never reads guest memory.
#pragma once
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <cstring>
#include <mutex>

#include "../dev_build.h"     // IsDevBuild (S103 P4F: the DEV ONLY --wwe2k14_rdr_screen_bind is read by +dev builds only)
#include "../frame_stats.h"   // g_guest_frame
#include "../renderer_scene/rf_c.h"
#include "../renderer_scene/screen_feed_yuv.h"
#include "inputs_glue.h"   // ReadChecked (page-checked off Windows, SEH on Windows; review F1)
#include "screen_planes_contrib.h"
#include "screen_planes_hook.h"
#include "stage_fx_contrib.h"

REXCVAR_DEFINE_STRING(wwe2k14_rdr_screen_bind, "", "wwe2k14",
                      "S103 lane M (DEV ONLY, not a player setting): which titan movie channel (0-5) each stage screen key (800-809) shows and the picture size it expects, e.g. 804:0@320x320,805:2@256x128. Empty (the default) = the built-in "
                      "table (803/804/807 -> channel 0 @320x320, 805 -> channel 2 @256x128; the other keys unbound). 'none' = every screen unbound. Any other text REPLACES the built-in table; a malformed text changes nothing (the "
                      "built-in table stays). Read once at start; only used with --wwe2k14_rdr_scene (docs\\renderer\\STAGE_LIVE_WRITERS.md 2b)");

namespace wwe2k14::scene_screens_glue {

// Guest reads go through the page-checked, MMIO-refusing reader the stage-fx contributor uses (review F1): never a raw memcpy.
inline bool ReadBase(uint32_t guest, void* dst, size_t n) { return wwe2k14::scene_inputs_glue::ReadChecked(guest, dst, n); }

// Evidence logs for the key -> channel binding (docs\renderer\STAGE_LIVE_WRITERS.md 2b; tools\scene\screen_bind_join.py joins them). Both are diagnosis, capped, mode != 0 only.
// DISPLAY SETS: logged at the 1st, 3rd and 60th flush of each set (the 1st flush comes BEFORE the present sets rec+4 = 1, 0x827E00B4, so a channel cannot resolve then: run 11 printed "channels [none]" for
// that reason among others). One line: the set, the channels that resolve to it, the BINKFRAMEBUFFERS header, ALL the handle words of both buffers (set + 64 buf + 4 w, w = 0..7: four planes x pair),
// then `slots` = every controller slot (state words rec+0 / rec+4, movie object, that movie's display set), `tchan` = every titan channel (slot, the non-movie word) and `movie` = the 64 words of the
// movie object that owns this set (the memory movie's buffer pointer and size appear there; the stage-media owner object itself has no known address without a hook [U]).
inline std::string Hex32(uint32_t v) {
  char b[12];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}
inline void LogSet(uint32_t set, uint32_t nth, bool resized = false) {
  namespace scr = rdr_scene::live::screens;
  namespace sfx = rdr_scene::live::stagefx;
  std::string channels;
  for (uint32_t ch = 0; ch < scr::kChannels; ++ch)
    if (scr::PlaneStore::SetOfChannel(&ReadBase, ch, scr::Plants{}) == set) channels += (channels.empty() ? "" : ",") + std::to_string(ch);
  uint32_t h[6] = {};
  const bool ok = ReadBase(set + scr::kSetCount, h, sizeof h);
  for (auto& v : h) v = sfx::Swap32(v);
  std::string handles;
  for (uint32_t b = 0; b < 2; ++b)
    for (uint32_t w = 0; w < 8; ++w) {
      uint32_t v = 0;
      sfx::ReadWord(&ReadBase, set + 64 * b + 4 * w, v);
      handles += (handles.empty() ? "" : " ") + Hex32(v);
    }
  std::string slots, tchan, movie_words;
  uint32_t t = 0, ctrl = 0, own_movie = 0;
  if (sfx::ReadWord(&ReadBase, scr::kTitanGlobal, t) && t >= 0x10000 && sfx::ReadWord(&ReadBase, t + scr::kTitanController, ctrl) && ctrl >= 0x10000) {
    for (uint32_t s = 0; s < scr::kCtrlSlots; ++s) {
      const uint32_t rec = ctrl + scr::kCtrlRecords + scr::kCtrlStride * s;
      uint32_t st0 = 0, st4 = 0, mv = 0, ds = 0;
      sfx::ReadWord(&ReadBase, rec, st0), sfx::ReadWord(&ReadBase, rec + scr::kRecState, st4), sfx::ReadWord(&ReadBase, rec + scr::kRecMovie, mv);
      if (mv >= 0x10000) sfx::ReadWord(&ReadBase, mv + scr::kMovieDisplaySet, ds);
      if (ds == set) own_movie = mv;
      uint32_t sz[2] = {0, 0};   // the slot's display set size NOW (a pooled movie block is reopened with another movie: run 14 s1 kept the boot logo's address)
      if (ds >= 0x10000) ReadBase(ds + scr::kSetYW, sz, sizeof sz);
      slots += " s" + std::to_string(s) + "=" + std::to_string(st0) + "/" + std::to_string(st4) + ",mv" + Hex32(mv) + ",set" + Hex32(ds) + "," + std::to_string(sfx::Swap32(sz[0])) + "x" + std::to_string(sfx::Swap32(sz[1]));
    }
    for (uint32_t c = 0; c < scr::kChannels; ++c) {
      uint32_t sl = 0, nm = 0;
      sfx::ReadWord(&ReadBase, t + scr::kTitanStride * c + scr::kTitanSlotOfChannel, sl);
      sfx::ReadWord(&ReadBase, t + scr::kTitanStride * (sl < 8 ? sl : 0) + scr::kNonMovieFlag, nm);
      uint32_t r0 = 0, r44 = 0;   // the channel record's own words: [0] = requested, [+44] = 2 after sub_827FABA0 opened a memory movie (0x827FACB0) [C]: a real signal that the channel is OPEN, unlike the slot word (0 also for an unopened channel)
      sfx::ReadWord(&ReadBase, t + 28 + scr::kTitanStride * c, r0), sfx::ReadWord(&ReadBase, t + 28 + scr::kTitanStride * c + 44, r44);
      tchan += " c" + std::to_string(c) + "->s" + std::to_string(sl) + "(rec" + std::to_string(r0) + "/" + std::to_string(r44) + ")" + (nm ? "(nonmovie)" : "");
    }
  }
  if (own_movie >= 0x10000)
    for (uint32_t w = 0; w < 64; ++w) {
      uint32_t v = 0;
      sfx::ReadWord(&ReadBase, own_movie + 4 * w, v);
      movie_words += (w ? " " : "") + Hex32(v);
    }
  // sub_827F9D88 uses [T] + 540 / 544 (0x827F9E54-0x827F9E60) ONLY when the controller [T+16] is null or the plane index is above 3; a never-opened channel gets slot 0's planes, or (record not ready) the
  // controller's own pair at +16 / +20 (sub_827DFEF8). So `defpair` null does NOT mean a panel draws nothing [U]: both pairs are logged.
  uint32_t defpair[2] = {0, 0}, ctrlpair[2] = {0, 0};
  if (t >= 0x10000) sfx::ReadWord(&ReadBase, t + 540, defpair[0]), sfx::ReadWord(&ReadBase, t + 544, defpair[1]);
  if (ctrl >= 0x10000) sfx::ReadWord(&ReadBase, ctrl + 16, ctrlpair[0]), sfx::ReadWord(&ReadBase, ctrl + 20, ctrlpair[1]);
  tchan += " defpair " + Hex32(defpair[0]) + ":" + Hex32(defpair[1]) + " ctrlpair " + Hex32(ctrlpair[0]) + ":" + Hex32(ctrlpair[1]);
  REXLOG_INFO("[scene][screens] flush #{} of display set 0x{:08X} (guest frame {}): channels [{}] count {} Y {}x{} chroma {}x{} display_index {}{}{} handles [{}] slots [{}] tchan [{}] movie 0x{:08X} words [{}]", nth, set,
              wwe2k14::g_guest_frame.load(std::memory_order_relaxed), channels.empty() ? "none" : channels, h[0], h[1], h[2], h[3], h[4], h[5], ok ? "" : " (header unreadable)", resized ? " (header size differs from the last line)" : "", handles, slots, tchan, own_movie, movie_words);
}
// The slot / channel map, as text (no set words): the same reads as LogSet's `slots` and `tchan`, used to notice a CHANGE (a pooled movie block reopened with another movie, a channel opened).
inline std::string MapText() {
  namespace scr = rdr_scene::live::screens;
  namespace sfx = rdr_scene::live::stagefx;
  std::string out;
  uint32_t t = 0, ctrl = 0;
  if (!(sfx::ReadWord(&ReadBase, scr::kTitanGlobal, t) && t >= 0x10000 && sfx::ReadWord(&ReadBase, t + scr::kTitanController, ctrl) && ctrl >= 0x10000)) return out;
  for (uint32_t s = 0; s < scr::kCtrlSlots; ++s) {
    const uint32_t rec = ctrl + scr::kCtrlRecords + scr::kCtrlStride * s;
    uint32_t st0 = 0, mv = 0, ds = 0, sz[2] = {0, 0};
    sfx::ReadWord(&ReadBase, rec, st0), sfx::ReadWord(&ReadBase, rec + scr::kRecMovie, mv);
    if (mv >= 0x10000) sfx::ReadWord(&ReadBase, mv + scr::kMovieDisplaySet, ds);
    if (ds >= 0x10000) ReadBase(ds + scr::kSetYW, sz, sizeof sz);
    (void)st0;   // the playing state flips with every open / close and would burn the budget: the set and its size are what matter
    out += " s" + std::to_string(s) + ",set" + Hex32(ds) + "," + std::to_string(sfx::Swap32(sz[0])) + "x" + std::to_string(sfx::Swap32(sz[1]));
  }
  for (uint32_t c = 0; c < scr::kChannels; ++c) {
    uint32_t sl = 0, r44 = 0;
    sfx::ReadWord(&ReadBase, t + scr::kTitanStride * c + scr::kTitanSlotOfChannel, sl), sfx::ReadWord(&ReadBase, t + 28 + scr::kTitanStride * c + 44, r44);
    out += " c" + std::to_string(c) + "->s" + std::to_string(sl) + "/o" + std::to_string(r44);
  }
  return out;
}
inline void NoteFlush(uint32_t set) {
  namespace sfx = rdr_scene::live::stagefx;
  static std::mutex mu;   // BHF1 B4: the flush hook can run on more than one game thread; the statics (and last_map, a std::string) are guarded
  std::lock_guard<std::mutex> lk(mu);
  static uint32_t seen[24], count[24], lw[24], lh[24], nresize[24];
  static uint32_t n = 0, total = 0, map_lines = 0;
  static std::string last_map;
  uint32_t i = 0;
  while (i < n && seen[i] != set) ++i;
  if (i == n) {
    if (n >= 24) return;
    seen[n] = set, count[n] = 0, lw[i] = lh[i] = 0, nresize[i] = 0, ++n;
  }
  const uint32_t k = ++count[i];
  uint32_t sz[2] = {0, 0};
  ReadBase(set + 132, sz, sizeof sz);
  const uint32_t w = sfx::Swap32(sz[0]), h = sfx::Swap32(sz[1]);
  const bool resized = k > 1 && (w != lw[i] || h != lh[i]) && nresize[i] < 8;   // a pooled set block reopened with a movie of another size (at most 8 lines per set)
  lw[i] = w, lh[i] = h;
  if (k == 1 || k == 3 || k == 60 || resized) {
    if (resized) ++nresize[i];
    LogSet(set, k, resized);
  }
  if (++total % 30 == 0 && map_lines < 200) {   // every 30th flush: did the slot / channel map change?
    const std::string m = MapText();
    if (!m.empty() && m != last_map) {
      ++map_lines;
      last_map = m;
      REXLOG_INFO("[scene][screens] slot/channel map (guest frame {}):{}", wwe2k14::g_guest_frame.load(std::memory_order_relaxed), m);
    }
  }
}

// STAGE-SCREENS MODELS: the screens' pass site (sub_82C0CA08, site 0x82C0F4BC) draws part `part` of model object `model`. Run 11: the table at +260 (count +256) held count 1 / [0:0] at the FIRST draw of
// every model, so it is logged again when any table entry is first NON-ZERO and at draws 30, 120 and 480 of each model (a model is filled per frame just before its draw, or not at all [U]). Every line
// also carries the model object's 208 words (offsets 0..0x33C) and the 16 words behind the table pointer, so the join (screen_bind_join.py) matches ANY word of them against ANY handle word of the sets.
inline void OnPartDraw(uint8_t*, uint32_t model, uint32_t part) {
  namespace sfx = rdr_scene::live::stagefx;
  struct St { uint64_t key = 0; uint32_t draws = 0; bool nonzero_logged = false; };
  static std::mutex mu;   // BHF1 B4: two draw workers could both pass n >= 16 and write st[16]; the table and its bound are taken under one lock
  std::lock_guard<std::mutex> lk(mu);
  static St st[16];
  static uint32_t n = 0, lines = 0;
  const uint64_t key = (uint64_t(model) << 32) | part;
  St* s = nullptr;
  for (uint32_t i = 0; i < n; ++i)
    if (st[i].key == key) s = &st[i];
  if (!s) {
    if (n >= 16) return;
    s = &st[n++];
    s->key = key;
  }
  const uint32_t d = ++s->draws;
  if (lines >= 12) return;
  uint32_t count = 0, tbl = 0;
  sfx::ReadWord(&ReadBase, model + 256, count);
  sfx::ReadWord(&ReadBase, model + 260, tbl);
  std::string entries;
  bool nonzero = false;
  for (uint32_t i = 0; i < count && i < 32 && tbl; ++i) {
    uint32_t a = 0, b = 0;
    if (!sfx::ReadWord(&ReadBase, tbl + 8 * i, a) || !sfx::ReadWord(&ReadBase, tbl + 8 * i + 4, b)) break;
    nonzero = nonzero || a || b;
    entries += (i ? " " : "") + Hex32(a) + ":" + Hex32(b);
  }
  const bool first_nonzero = nonzero && !s->nonzero_logged;
  if (!(d == 1 || first_nonzero)) return;   // run 12 answered it (the table stays zero through 480 draws): the log is shrunk to the first draw and a first non-zero
  if (first_nonzero) s->nonzero_logged = true;
  ++lines;
  std::string words, behind;
  for (uint32_t w = 0; w < 16; ++w) {
    uint32_t v = 0;
    if (!sfx::ReadWord(&ReadBase, model + 4 * w, v)) break;
    words += (w ? " " : "") + Hex32(v);
  }
  for (uint32_t w = 0; w < 16 && tbl; ++w) {
    uint32_t v = 0;
    if (!sfx::ReadWord(&ReadBase, tbl + 4 * w, v)) break;
    behind += (w ? " " : "") + Hex32(v);
  }
  REXLOG_INFO("[scene][screens] stage-screens model 0x{:08X} part {} (guest frame {}, draw {}{}): table 0x{:08X} count {} entries [{}] behind [{}] words [{}]", model, part, wwe2k14::g_guest_frame.load(std::memory_order_relaxed), d,
              first_nonzero ? ", first non-zero" : "", tbl, count, entries, behind, words);
}
inline void OnFlush(uint8_t*, uint32_t set) {
  NoteFlush(set);
  rdr_scene::live::screens::Store().OnFlush(&ReadBase, set, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
}

inline rdr_scene::rf::c::YuvScreenFeed& Feed() {
  static rdr_scene::live::screens::StorePlaneSource src(&rdr_scene::live::screens::Store());
  static rdr_scene::rf::c::YuvScreenFeed feed(&src);
  return feed;
}

// Once, at start (wwe2k14_app.h OnPostSetup, right after scene_stagefx_glue::Register). Idempotent.
inline void Register() {
  namespace scr = rdr_scene::live::screens;
  static bool done = false;
  if (done) return;
  done = true;
  scr::g_now = +[]() -> uint64_t { return wwe2k14::g_guest_frame.load(std::memory_order_relaxed); };
  {
    const std::string spec = wwe2k14::IsDevBuild() ? std::string(REXCVAR_GET(wwe2k14_rdr_screen_bind)) : std::string();   // DEV ONLY: a release build keeps the built-in table
    std::string msg;
    const int n = scr::Store().binding.ApplySpec(spec, msg);   // the same function the unit tests (review B3)
    if (n < 0) REXLOG_WARN("[scene][screens] --wwe2k14_rdr_screen_bind '{}': {}", spec, msg);
    else REXLOG_INFO("[scene][screens] screen binding: {}{}", msg, spec.empty() ? "" : (" from --wwe2k14_rdr_screen_bind '" + spec + "'"));
  }
  scr::Store().stage_live = +[]() -> bool { return rdr_scene::live::stagefx::StageFx().StageLive(); };   // review C1
  rdr_scene::rf::c::SetScreenSizeFn(+[](uint32_t key, uint32_t& w, uint32_t& h) -> bool { return scr::Store().binding.ExpectedSize(key, w, h); });   // review B1
  rdr_scene::live::stagefx::StageFx().SetScreenGate(+[](uint32_t key) -> bool { return rdr_scene::live::arena::ScreenShown(uint16_t(key)); },
                                                    +[](uint32_t key) -> bool { return scr::Store().binding.ChannelOf(key) >= 0; },
                                                    +[]() -> int { return rdr_scene::live::arena::PublishedLookCode(); });   // review B2
  rdr_scene::live::stagefx::StageFx().SetScreenIdle(+[](rdr_scene::live::stagefx::ReadFn read) -> bool { return scr::Store().Idle(scr::Now()) && scr::Store().ChannelsQuiet(read); });   // S103 E7: no movie plays
  rdr_scene::live::stagefx::StageFx().SetScreens(&scr::ScreensFnImpl);
  rdr_scene::live::stagefx::StageFx().SetExtraCensus([](std::string& out) { scr::CensusLine(out, &Feed()); });
  rdr_scene::rf::c::SetScreenFeed(&Feed());
  wwe2k14::scene_screens::g_on_flush.store(&OnFlush, std::memory_order_relaxed);
  wwe2k14::scene_screens::g_on_part.store(&OnPartDraw, std::memory_order_relaxed);
}

}  // namespace wwe2k14::scene_screens_glue
