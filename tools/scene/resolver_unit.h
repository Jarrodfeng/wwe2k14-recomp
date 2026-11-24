// resolver_unit.h: the PictureResolver's lifecycle twins on a headless renderer (S103 PN4F; docs\renderer\PICTURES_NO_STREAM.md 10). Rig only. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// host_rig --mode resolver --scene DIR --capture DIR --rows-dir DIR --frames N --game DIR [--plant c1_old|d3_old|d4_permanent|t2f_old|t2f_forever]: the real resolver (worker thread, the player's file index) over the pictures of one capture
// frame (forged guest memory, host_rig_real.h), driven by hand, one "record" per step (Resolve of every row, then EndFrame):
//   rejoin keeps the picture (D3)  every key first answers as a GUEST copy and is drawn; the pacs are then added to the index and the source rejoins the keys to FILES: from the first step on, every row must still
//                                  draw (the guest image is the fallback while the worker loads the file), no step may drop a row, the file images must arrive, and the guest images must go after.
//   unused is not drawn (C1)       a file picture drawn in record 100, then a gap longer than kFileUnusedFrames, then drawn again in a record at 100 + gap: it is NOT released at that record's EndFrame (same image, no reload).
//   failed is retried (D4)         the first load of a picture fails (planted read error): the row is dropped, nothing is queued again before kFailedRetryFrames, and after it the picture is loaded and drawn.
//   first sight bridges (T2F)      a key that joins a FILE at its first sight draws the game's own mip 0 on the first record (rows dropped `picture` = 0), the file image replaces it, the bridge image goes.
// Twins (each makes the unit FAIL): t2f_old (no bridge), t2f_forever (bridge never dropped), c1_old (stamped with the previous record's frame), d3_old (no guest fallback), d4_permanent (never retried).
#pragma once
#include <chrono>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

#include "host_rig_real.h"
#include "../../src/renderer_scene/rows_quad2d.h"

namespace resolver_unit {

namespace ps = rdr_scene::live::picsrc;
using Rows = std::vector<rdr_scene::Quad2D>;

inline Rows TexRows(const rdr_scene::SceneTables& rec) {
  Rows rows;
  for (const rdr_scene::Quad2D& r : rec.quads2d) {
    if (r.kind == rdr_scene::kQkYuv || r.kind == rdr_scene::kQkYuvAlpha) continue;
    if ((r.have & rdr_scene::kQhPlanes) && r.tex_key[0]) rows.push_back(r);
  }
  return rows;
}

// One "record": every row resolved, then EndFrame(frame). Returns the rows that came back empty.
inline uint32_t Step(rdr_scene::PictureResolver& res, const Rows& rows, uint64_t frame) {
  uint32_t empty = 0;
  for (const rdr_scene::Quad2D& r : rows)
    if (res.Resolve(r).image == rdr::kNoImage) ++empty;
  res.EndFrame(frame);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return empty;
}

struct Case {
  const char* name;
  bool ok = true;
  std::string why;
  void Fail(const std::string& w) { if (ok) why = w; ok = false; }
};

struct Env {
  rdr::Renderer* renderer = nullptr;
  host_rig_2d::CapSource* cap = nullptr;
  const rdr_scene::SceneTables* rec = nullptr;
  std::string game, cache;
};

inline Rows FileRows(host_rig_real::Real& r, const Rows& all) {
  Rows out;
  for (const auto& row : all) {
    ps::Answer a;
    if (r.source.Lookup(row.tex_key[0], a) && a.kind == ps::Kind::File) out.push_back(row);
  }
  return out;
}

inline Case Rejoin(const Env& e) {
  Case c{"rejoin keeps the picture (D3)"};
  host_rig_real::Real r;
  if (!r.BuildIndex(e.game, e.cache, true)) return c.Fail("index"), c;
  rdr_scene::PictureResolver res(*e.renderer, r.source, r.index, e.game);
  const Rows all = TexRows(*e.rec);
  r.FeedPictures(*e.cap, *e.rec, 0);
  uint64_t frame = 1000;
  std::set<uint32_t> drawn;   // keys drawn as guest copies
  for (int i = 0; i < 60; ++i) {
    Step(res, all, frame++);
    drawn.clear();
    for (const auto& row : all)
      if (res.Resolve(row).image != rdr::kNoImage) drawn.insert(row.tex_key[0]);
    if (!drawn.empty() && res.Idle()) break;
  }
  r.AddStaged();
  r.FeedPictures(*e.cap, *e.rec, 0);   // the next first sight of each key: the source rejoins it to its file
  Rows moved;
  for (const auto& row : all) {
    ps::Answer a;
    if (drawn.count(row.tex_key[0]) && r.source.Lookup(row.tex_key[0], a) && a.kind == ps::Kind::File) moved.push_back(row);
  }
  std::printf("      rejoin: %zu rows, %zu keys drawn as guest copies, %zu rejoined to files\n", all.size(), drawn.size(), moved.size());
  if (moved.empty()) return c.Fail("no key was rejoined to a file (nothing to test)"), c;
  uint32_t empties = 0, quiet = 0;
  for (int i = 0; i < 4000 && quiet < 3; ++i) {
    const uint64_t pending0 = res.stats().pending;
    empties += Step(res, moved, frame++);
    quiet = (res.Idle() && res.stats().pending == pending0) ? quiet + 1 : 0;
  }
  const auto& s = res.stats();
  std::printf("      rejoin: empty rows %u, fallback rows %llu, file images %llu, guest released %llu, pending records %llu\n", empties, (unsigned long long)s.fallback_rows, (unsigned long long)s.uploads_file,
              (unsigned long long)s.released, (unsigned long long)s.pending_frames);
  if (empties) c.Fail(std::to_string(empties) + " rows dropped while the file loaded (the picture vanished)");
  if (!s.fallback_rows) c.Fail("no row was drawn from the guest fallback");
  if (!s.uploads_file) c.Fail("the file images never arrived");
  if (!s.released) c.Fail("the guest images were never released after the files arrived");
  return c;
}

// T2F: a key that joins a FILE at its first sight has no guest image; the pac is read on the worker (77-124 ms), so on the first record the row used to be dropped `picture`. With the bridge the game's own mip 0 (kept by the source) draws at once.
inline Case Bridge(const Env& e) {
  Case c{"first sight draws before its file loads (T2F bridge)"};
  host_rig_real::Real r;
  if (!r.BuildIndex(e.game, e.cache)) return c.Fail("index"), c;
  rdr_scene::PictureResolver res(*e.renderer, r.source, r.index, e.game);
  r.FeedPictures(*e.cap, *e.rec, 0);
  const Rows rows = FileRows(r, TexRows(*e.rec));
  if (rows.empty()) return c.Fail("no file row"), c;
  const Rows one{rows.front()};
  const uint32_t first = Step(res, one, 500);   // the very first record that names the key: the worker has not finished
  const uint64_t bridge = res.stats().bridge_rows, file0 = res.stats().uploads_file;
  uint64_t frame = 501;
  uint32_t later_empty = 0, quiet = 0;
  for (int i = 0; i < 4000 && quiet < 3; ++i) {   // the file arrives; the bridge image must go after it
    later_empty += Step(res, one, frame++);
    quiet = (res.Idle() && res.stats().uploads_file > file0 && res.stats().released) ? quiet + 1 : 0;
  }
  const auto& s = res.stats();
  std::printf("      bridge: first record dropped %u, bridge rows %llu, rows dropped while the file loaded %u, file images %llu, released %llu\n", first, (unsigned long long)bridge, later_empty,
              (unsigned long long)s.uploads_file, (unsigned long long)s.released);
  if (first) c.Fail("the row was dropped `picture` on its first record (the black gap)");
  if (!bridge) c.Fail("no row was drawn from the bridge image");
  if (later_empty) c.Fail("rows dropped while the file loaded");
  if (!s.uploads_file) c.Fail("the file image never arrived");
  if (!s.released) c.Fail("the bridge image was never dropped after the file arrived");
  return c;
}

inline Case Unused(const Env& e) {
  Case c{"a drawn picture is not released after a gap (C1)"};
  host_rig_real::Real r;
  if (!r.BuildIndex(e.game, e.cache)) return c.Fail("index"), c;
  rdr_scene::PictureResolver res(*e.renderer, r.source, r.index, e.game);
  r.FeedPictures(*e.cap, *e.rec, 0);
  const Rows rows = FileRows(r, TexRows(*e.rec));
  if (rows.empty()) return c.Fail("no file row"), c;
  const rdr_scene::Quad2D row = rows.front();
  uint32_t empty = 1;
  for (int i = 0; i < 4000 && !(res.stats().uploads_file && res.Idle()); ++i) Step(res, {row}, 100);   // T2F: the row draws from the bridge at once; wait for the FILE image (what C1 is about)
  empty = Step(res, {row}, 100);
  const rdr::ImageId img = res.Resolve(row).image;
  if (empty || img == rdr::kNoImage) return c.Fail("the picture never arrived"), c;
  res.EndFrame(100);
  const uint64_t uploads = res.stats().uploads_file, released = res.stats().released;
  const uint64_t gap = 100 + rdr_scene::PictureResolver::kFileUnusedFrames + 500;
  const uint32_t e2 = Step(res, {row}, gap);   // the record at the end of the gap draws the picture
  const rdr::ImageId again = res.Resolve(row).image;
  std::printf("      unused: image %u, after a %llu-frame gap: drawn %s, same image %s, released %llu -> %llu, uploads %llu -> %llu\n", unsigned(img), (unsigned long long)(gap - 100), e2 ? "NO" : "yes", again == img ? "yes" : "NO",
              (unsigned long long)released, (unsigned long long)res.stats().released, (unsigned long long)uploads, (unsigned long long)res.stats().uploads_file);
  if (e2 || again != img) c.Fail("the picture drawn in the record after the gap was released");
  if (res.stats().released != released) c.Fail("the resolver released an image at the record that drew it");
  if (res.stats().uploads_file != uploads) c.Fail("the picture was loaded again");
  return c;
}

inline Case Retried(const Env& e) {
  Case c{"a failed picture is retried (D4)"};
  host_rig_real::Real r;
  if (!r.BuildIndex(e.game, e.cache)) return c.Fail("index"), c;
  rdr_scene::UiFileFaults faults;
  faults.fail_loads = 1;
  rdr_scene::PictureResolver res(*e.renderer, r.source, r.index, e.game, faults);
  r.FeedPictures(*e.cap, *e.rec, 0);
  const Rows rows = FileRows(r, TexRows(*e.rec));
  if (rows.empty()) return c.Fail("no file row"), c;
  const Rows one{rows.front()};
  uint64_t frame = 200;
  for (int i = 0; i < 4000 && !(res.stats().load_failed && res.Idle()); ++i) Step(res, one, frame);
  if (!res.stats().load_failed) return c.Fail("the planted read error never reached the host"), c;
  const uint64_t loads0 = res.stats().loads;
  const uint32_t e_failed = Step(res, one, frame + 10);
  const bool queued_early = !res.Idle() || res.stats().loads != loads0;
  uint32_t empty = 1;
  const uint64_t later = frame + rdr_scene::PictureResolver::kFailedRetryFrames + 5;
  for (int i = 0; i < 4000 && !(res.stats().uploads_file && res.Idle()); ++i) empty = Step(res, one, later);
  // T2F: the failed picture's row keeps drawing from the bridge (the game's own bytes), so "dropped" is no longer the sign: the file image must still arrive after the retry window.
  std::printf("      retried: failed first read, row dropped %s, queued again early %s, after %llu frames: file images %llu (row %s), retries %llu\n", e_failed ? "yes" : "no (bridge)", queued_early ? "YES" : "no",
              (unsigned long long)rdr_scene::PictureResolver::kFailedRetryFrames, (unsigned long long)res.stats().uploads_file, empty ? "dropped" : "drawn", (unsigned long long)res.stats().failed_retries);
  if (queued_early) c.Fail("the failed picture was queued again before the retry window");
  if (!res.stats().uploads_file || !res.stats().failed_retries) c.Fail("the failed picture was never retried");
  return c;
}

inline int Run(const Env& e, const std::string& plant) {
  rdr_scene::g_resolver_plant = plant == "c1_old" ? 1 : plant == "d3_old" ? 2 : plant == "d4_permanent" ? 3 : plant == "t2f_old" ? 4 : plant == "t2f_forever" ? 5 : 0;
  int bad = 0;
  for (Case c : {Rejoin(e), Bridge(e), Unused(e), Retried(e)}) {
    std::printf("%s  %s%s%s\n", c.ok ? "PASS" : "FAIL", c.name, c.ok ? "" : ": ", c.why.c_str());
    bad += !c.ok;
  }
  std::printf("resolver unit%s: %s\n", plant.empty() ? "" : (" (plant " + plant + ")").c_str(), bad ? "FAIL" : "PASS");
  return bad ? 3 : 0;
}

}  // namespace resolver_unit
