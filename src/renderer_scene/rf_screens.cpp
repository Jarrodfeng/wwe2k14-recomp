// Lane C (Session 103): the stage screens (the tron, the wall, the minis: arena keys 800-809) from the live record. The screen meshes are the arena pac's; their picture is a
// live texture the record sizes (kFxScreen rows: slot, movie, serial, w x h) and a ScreenFeed fills (FrontEnd::UpdateTexture, exactly the texture's size, only when the serial
// is new). Live source of the pixels: the render thread's movie owner (src\renderer_scene\movie_ours.*, MOVIE_OURS.md; the game's decoder keeps decoding the planes, the Y / Cb / Cr
// -> RGB step is ours) [U: no live feed exists yet; offline a driver feeds a picture]. The big screen (slot 800) also shows the previous frame's scene in the match look
// (the front-end's own rule, SCENE_FRONTEND "live screen"): unchanged.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "../keep_stream.h"
#include <deque>
#include "rf_c.h"
#include "screen_idle_rows.h"
#include "stage_look.h"
#include "stage_show.h"   // S103 SCF

namespace rdr_scene::rf::c {

bool ApplyScreens(BuildCtx& ctx) {
  if (!(ctx.families & (1u << uint32_t(Family::Screens)))) return true;
  std::string why;
  bool miss = false;
  ArenaSet* as = EnsureArena(ctx, why, miss);   // (EnsureArena also applies the show bits as per-frame model masks: rf_c_core.cpp ApplyShowMasks)
  if (!as) {
    Report(ctx, Family::Screens, 0, 0, 1, why);
    return !miss;
  }
  // S103 SCF [C, docs\research\s103\SCM.md]: WHICH screens draw is the game's per-key show bit (stage_show.h), not a fixed set per look. The arena model holds both sets (stage_media::Cfg().all_keys): the match
  // set 800 801 805 806 808 (Part::Screens) and the entrance set 802 804 807 809 (Part::ScreensEntrance; its 805 is masked, 805 is drawn from Screens). Both are drawn every frame, each masked to the bits.
  const bool entrance = stage_look::Entrance(ctx.t);
  const StageShow show = EffectiveShow(ctx, *as);
  FamilyModel* const parts[2] = {&as->part[size_t(Part::Screens)], &as->part[size_t(Part::ScreensEntrance)]};
  if (parts[0]->model < 0 && parts[1]->model < 0) {
    Report(ctx, Family::Screens, 0, 0, 1, "arena " + std::to_string(as->arena) + " has no stage screens (keys 800-809)");
    return true;
  }
  uint32_t updated = 0, kept = 0, holes = 0, idle_black = 0, idle_grab = 0, objects = 0;   // S103 M (B2): holes = refusals only the render thread sees (a row with no texture, a size mismatch, a texture that never got a picture); an UNBOUND key has no row and is never a hole here (the decision input "screens" covers it)
  std::string reason;
  ScreenFeed* feed = GetScreenFeed();
  bool ordered = false, look_missing = false;
  std::vector<bool> pres[2];   // per model: the keys it holds a mesh for
  for (size_t pj = 0; pj < 2; ++pj) {
    pres[pj].assign(10, false);
    if (parts[pj]->model >= 0)
      for (int sl : parts[pj]->mesh_slot)
        if (sl >= 800 && sl <= 809) pres[pj][size_t(sl - 800)] = true;
  }
  const bool enabled = !WWE2K14_STREAM() && !PlantsRef().stale_movie_kept;   // S103 HN: OFF only (the stream build draws the walked frames' screens itself); Plants::stale_movie_kept = the old rule
  for (size_t pi = 0; pi < 2; ++pi) {
    FamilyModel& fm = *parts[pi];
    const bool entrance_part = pi == 1;
    FamilyModel& other = *parts[1 - pi];
    if (fm.model < 0) {
      // S103 M (N1): an arena with only ONE screen set (arena 29: 803 / 804 only) shows the other look's screens in the game; we draw none of them, so the frame stays the game's (a hole), never OPEN.
      if (entrance_part == entrance && other.model >= 0) look_missing = true;   // reported once, with the frame's one Screens report below
      continue;
    }
    const auto drawn_key = [&](int k) { return show.KeyShown(k) && !(k == 805 && entrance_part != entrance); };   // 805 is in both models: the one that matches the look draws it (one pool, one upload)
    const std::vector<bool>& present = pres[pi];
    bool any = false;
    for (int k = 800; k <= 809; ++k) any = any || (present[size_t(k - 800)] && drawn_key(k));
    if (any) {
      Object o;
      o.asset = uint32_t(fm.model);
      o.world = IdentityMat4();
      if (!ordered) LogOrder("screens"), ordered = true;   // S103 FOB
      ctx.frame.objects.push_back(o);
      ctx.frame.model_lights.push_back(LightsFor(ctx.t, fm, uint32_t(fm.model)));
      objects += fm.meshes;
    }
    // S103 UPL U2: the frame's screen pictures are collected and given to the front end as ONE batch (UpdateScreens): a picture shown on several screens is uploaded once and they share one image. The feed's pointer is
    // valid only until its NEXT Picture call (rf_c.h contract: YuvScreenFeed recycles its cache entries round-robin, a one-buffer feed overwrites), so each DISTINCT picture (by the feed's content id) is copied once into
    // a buffer this frame owns, and the batch points at the copies.
    struct Pending { FamilyModel::Screen* sc; uint32_t seq; ScreenPic pic; };
    std::vector<Pending> pend;
    std::deque<std::vector<uint8_t>> owned;
    struct Own { uint64_t id; uint32_t w, h; const uint8_t* ptr; };
    std::vector<Own> own;
    const auto flush = [&]() {
      if (pend.empty()) return;
      std::vector<ScreenPic> pics;
      pics.reserve(pend.size());
      for (const Pending& p : pend) pics.push_back(p.pic);
      std::unique_ptr<bool[]> ok(new bool[pics.size()]());
      ctx.models.front_end().UpdateScreens(fm.model, pics.data(), pics.size(), ok.get());
      for (size_t i = 0; i < pend.size(); ++i) {
        FamilyModel::Screen* sc = pend[i].sc;
        if (ok[i]) {
          sc->seq = pend[i].seq;
          sc->black = false;
          ++updated;
        } else {
          ++kept, ++holes;
          if (reason.empty()) reason = "screen " + std::to_string(sc->slot) + ": picture " + std::to_string(pend[i].pic.w) + "x" + std::to_string(pend[i].pic.h) + " does not fit its texture " + std::to_string(sc->w) + "x" + std::to_string(sc->h);
        }
      }
      pend.clear();
    };
    for (const StageFxRow& r : ctx.t.stage_fx) {
      if (r.kind != kFxScreen) continue;
      if (r.flags & kFxfScreenIdle) continue;   // S103 E7: an idle row (no movie): nothing to update; the screen binds the frame grab (idle mask below)
      if (r.slot < 800 || r.slot > 809) continue;
      if (!present[size_t(r.slot - 800)]) {   // not a mesh of this model: the other model's row; a key with a mesh in neither is one hole, counted once (by the first model)
        if (pi == 0 && !pres[1][size_t(r.slot - 800)] && show.KeyShown(int(r.slot))) { ++kept, ++holes; if (reason.empty()) reason = "screen " + std::to_string(r.slot) + " has no mesh in the arena's screen models (size " + std::to_string(r.w) + "x" + std::to_string(r.h) + ")"; }
        continue;
      }
      if (!drawn_key(int(r.slot))) continue;    // S103 SCF: a screen the game does not show (bit clear), or the other look's 805: no picture is uploaded for it
      if (r.w == 0 || r.h == 0 || r.w > 4096 || r.h > 4096) { ++kept; if (reason.empty()) reason = "screen " + std::to_string(r.slot) + ": picture size " + std::to_string(r.w) + "x" + std::to_string(r.h) + " refused (max 4096)"; continue; }
      FamilyModel::Screen* sc = nullptr;  // one live texture per slot, shared by its meshes: the slot's single entry
      for (FamilyModel::Screen& sx : fm.screens)
        if (sx.slot == int(r.slot)) sc = &sx;
      if (!sc) {
        ++kept, ++holes;
        if (reason.empty()) reason = "screen " + std::to_string(r.slot) + " has no live texture in the model (size " + std::to_string(r.w) + "x" + std::to_string(r.h) + ")";
        continue;
      }
      if (!g_plant_screen_reupload && SameSerialSkips(r.seq, sc->seq, sc->black)) continue;  // the same picture as last frame (S103 HN: unless the texture was blacked: a row back with the same serial must repaint it)
      if (r.w != sc->w || r.h != sc->h) {   // S103 M (review cost fix): the size is tested BEFORE the feed converts a picture it would then refuse
        ++kept, ++holes;
        if (reason.empty()) reason = "screen " + std::to_string(r.slot) + ": row " + std::to_string(r.w) + "x" + std::to_string(r.h) + " does not fit its texture " + std::to_string(sc->w) + "x" + std::to_string(sc->h);
        continue;
      }
      if (PlantsRef().never_update_screens) { ++kept; if (reason.empty()) reason = "screen updates off (planted)"; continue; }
      uint32_t w = 0, h = 0;
      const uint8_t* rgba = nullptr;
      uint64_t pid = 0;
      if (!feed || !feed->Picture(r.slot /* S103 C2: the screen key */, r.seq, w, h, rgba, pid) || !rgba) {
        ++kept;
        if (sc->seq == 0xFFFFFFFFu) ++holes;   // S103 M: a texture that never got its first picture
        if (reason.empty()) reason = feed ? "no new picture from the feed" : "no screen feed";
        continue;
      }
      if (w != sc->w || h != sc->h) {
        ++kept, ++holes;
        if (reason.empty()) reason = "screen " + std::to_string(r.slot) + ": picture " + std::to_string(w) + "x" + std::to_string(h) + " does not fit its texture " + std::to_string(sc->w) + "x" + std::to_string(sc->h);
        continue;
      }
      if (!pid) {   // a feed with no content id: the id is a hash of the bytes (top bit set: never a feed's id)
        pid = 1469598103934665603ull;
        for (size_t i = 0, n = size_t(w) * h * 4; i < n; ++i) pid = (pid ^ rgba[i]) * 1099511628211ull;
        pid |= 1ull << 63;
      }
      const uint8_t* mine = nullptr;
      for (const Own& ow : own)
        if (ow.id == pid && ow.w == w && ow.h == h) mine = ow.ptr;
      if (!mine) {   // the first screen of this picture in the batch: its bytes are copied before the next Picture call can recycle them
        owned.emplace_back(rgba, rgba + size_t(w) * h * 4);
        mine = owned.back().data();
        own.push_back(Own{pid, w, h, mine});
      }
      pend.push_back(Pending{sc, r.seq, ScreenPic{sc->texture, w, h, mine, pid}});
    }
    flush();
    // S103 SCF [C, SCM.md sub_8276AEF8 -> sub_8276A808]: a SHOWN screen with no movie draws the live frame grab, never black. A shown movie-key screen with no SIZED row this frame (no row at all, or an idle row) is marked in the
    // model's idle mask (FrontEnd::SetIdleScreens): the front end binds the one pre-post scene copy it already makes every frame (the 360's on-demand resolve is a dropped 360 habit: no extra copy) in place of its own picture. A spurious
    // one-frame hole in the rows must not blink the screen, so a screen that HAS a live texture goes idle only after kNoRowBlackAfter guest frames without a sized row (S103 HN); one with none (never sized) is idle at once.
    // The ENTRANCE look keeps the measured black (run 38 f5401, S103 SCN: the game's pixels there were (1,1,1); the mechanism is [U] in SCM.md): its idle screen is still painted black after those frames. OFF only.
    {
      static std::vector<uint8_t> black;
      uint16_t idle_mask = 0;
      for (int k = 800; k <= 809; ++k) {
        FamilyModel::Screen* sc = nullptr;
        for (FamilyModel::Screen& sx : fm.screens)
          if (sx.slot == k) sc = &sx;
        if (!drawn_key(k) || !present[size_t(k - 800)]) {
          if (sc) sc->no_row_gf = 0, sc->last_gf = 0;   // not shown: a later show starts the count again
          continue;
        }
        bool sized = false;
        for (const StageFxRow& r : ctx.t.stage_fx)
          if (r.kind == kFxScreen && int(r.slot) == k && !(r.flags & kFxfScreenIdle) && r.w && r.h) sized = true;
        const bool goes = sc ? NoRowGoesBlack(sized, ctx.t.header.guest_frame, sc->no_row_gf, sc->last_gf, enabled) : enabled;   // no live texture yet (a first movie's row is coming): idle = the frame grab, never the file's 1x1 black
        const IdleDraw how = IdleDrawFor(entrance, goes);
        if (how == IdleDraw::FrameGrab) {
          if (k != 800) idle_mask = uint16_t(idle_mask | 1u << (k - 800)), ++idle_grab;   // (800 is the front end's own camera feed: it always binds the grab in the match look)
          continue;
        }
        if (how != IdleDraw::Black || !sc || sc->black || !sc->w || !sc->h) continue;
        black.assign(size_t(sc->w) * sc->h * 4, 0);
        for (size_t q = 3; q < black.size(); q += 4) black[q] = 255;
        if (ctx.models.front_end().UpdateTexture(fm.model, sc->texture, sc->w, sc->h, black.data(), TexTag::Screen)) sc->black = true, sc->seq = 0xFFFFFFFFu, ++idle_black;   // (a batch of one: the black picture is the same bytes for every size-equal screen, so the share finds it)
      }
      ctx.models.front_end().SetIdleScreens(fm.model, entrance ? uint16_t(0) : idle_mask);
    }
  }
  Report(ctx, Family::Screens, objects, objects, kept, (reason.empty() ? std::to_string(updated) + " pictures updated" : reason) + (idle_black ? "; " + std::to_string(idle_black) + " to idle black (no row, entrance)" : "") + (idle_grab ? "; " + std::to_string(idle_grab) + " on the frame grab (no movie)" : ""));
  ctx.reports.back().holes += holes;
  if (look_missing) {   // S103 M (N1): an arena with only ONE screen set (arena 29: 803 / 804 only) shows the other look's screens in the game; we draw none of them, so the frame stays the game's (a hole), never OPEN
    ctx.reports.back().holes += 1;
    ctx.reports.back().why += "; this look's screen set is missing while the other look's exists (hole)";
  }
  return true;
}

}  // namespace rdr_scene::rf::c
