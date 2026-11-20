// frame_rig: OURS for the offline rig (Session 103, lane R; docs\agents\s103\briefs\R.md, round 2 R2): one scene record drawn from the record ALONE through the shared record -> frame
// library (src\renderer_scene\record_frame.h) and FrontEnd::RenderToPixels, whole and once per family. The game's picture, the pairing and the sheet are frame_rig.py.
//   frame_rig --record F.rsc --game DIR --size WxH --prefix OUTPREFIX [--camera 16 floats] [--eye x,y,z] [--families MASK] [--per-family]
//             [--link ID=CHAR:ATTIRE ...] [--see-order game|distance] [--no-hooks] [--synth-camera] [--lists N [--lists-plant | --lists-plant-specular]]
// Writes OUTPREFIX_whole.png (or _mask<MASK>.png when --families), OUTPREFIX_<family>.png per family with objects (--per-family), OUTPREFIX_map_shadow.png / _map_circles.png
// when the maps hook made them, and OUTPREFIX_report.txt.
// A live record carries no camera yet (FRAME_CONTRACT: lane A / S): --camera is the game-space row-vector world -> clip matrix the caller read from the stream's own frame
// (OFFLINE ONLY: live, the camera must come from a reader at its source). Exit 0 ok, 2 bad record / no camera, 3 failure, 4 a kept-lists draw differed from the plain draw.
//
// R2 (round 2): the whole panel (and a --families panel) is made the way the live frame is: SeeOrder::Game (the game's see-through order), lane E's maps hook (LiveMapsHook at
// Stage::Maps, its inputs from BuildMapsInput on the record) and lane F's particle hook (LiveParticles at Stage::AfterBlended, the record's particle tables), exactly as live_scene_draw
// would attach them; --no-hooks leaves them off. --link names the model of an object whose record carries none (a record taken before lane B's writer): TOLD, labelled in the report,
// never from the record. --lists is lane G's lists check (the kept draw lists draw the same picture as no lists; ported from rsc_draw, which lost it when it moved onto the library).
#include "../../src/renderer_scene/body_scale.h"   // S103 W2
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_STATIC   // S103 PS1: the metrics read their reference picture (a PNG) here; static, no stbi_* symbol leaves this file (live_assets.cpp carries its own)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "offline_assets.h"
#include "rig_metrics.h"   // S103 PS1
#include "rig_metrics_gpu.h"   // S103 PS1: the same measurement as one compute dispatch
#include "png_out.h"
#include "particle_probe.h"  // S103 F
#include "particle_fixture.h"  // S103 F5
#include "../../src/renderer/renderer.h"
#include "../../src/renderer/outside.h"   // S103 UPL: rdr::g_upload_batch
#include "../../src/assets/load_plants.h"           // S103 RL: load_detail::PropsSet100Plant
#include "../../src/assets/crowd_body.h"           // S103 D4: Palettes (the synthetic single people)
#include "../../src/renderer_scene/crowd_prop_rows.h"   // S103 D4: the writer API the rig feeds its synthetic rows through
#include "../../src/renderer_scene/liquid_switch.h"   // S103 SWF: --liquid-plant
#include "../../src/renderer_scene/props_map_switch.h"   // S103 PMS: WWE_RDR_PROPS_MAP
#include "../../src/renderer_scene/weapon_plan.h"   // S103 MBC: CastsMapB (the --prop-fixture goes through the same tag filter)
#include "../../src/renderer_scene/body_scale.h"   // S103 L9: --body-scale-one
#include "../../src/renderer_scene/plane_blank_rule.h"   // S103 SLV: g_skip_blank_plane
#include "../../src/renderer_scene/plane_game_gate.h"   // S103 RML: g_plane_game_no_module / _suppressed (the rig sets them from the capture: the game built no plane module there)
#include "../../src/renderer_scene/sampler_table.h"   // S103 B1: the rig-only old-rules plant
#include <map>
#include "../../src/renderer_scene/draw_lists.h"
#include "../../src/renderer_scene/frame_hooks.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/live_assets.h"
#include "../../src/renderer_scene/live_maps.h"
#include "../../src/renderer_scene/live_particles.h"
#include "../../src/renderer_scene/post_from_record.h"   // S103 P2: the rig draws the post the live mirror draws
#include "../../src/renderer_scene/record_frame.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rf_c.h"       // S103 D2: SetOfflineLoad
#include "../../src/renderer_scene/rf_crowd.h"   // S103 D2
#include "../../src/renderer_scene/rf_wrestlers.h"   // S103 NRBF: WrestlerStatsText
#include "../../src/renderer_scene/rf_c.h"
#include "../../src/renderer_scene/see_through_order.h"
#include "../../src/renderer_scene/stage_look.h"  // S103 L

using namespace rdr_scene;

// S103 UPL: the rig's screen feed (the capture has no movie planes; the live feed is the game's decoder). WWE_RDR_RIG_SCREEN_FEED=same: every screen shows the same picture of a serial (one movie on all of them, what the
// live YuvScreenFeed hands back for screens of one decoded frame); =distinct: a picture per screen key (nothing to share). The pictures change with the row's serial, so a screen's upload cadence is the record's.
class RigScreenFeed : public rf::c::ScreenFeed {
 public:
  bool distinct = false;
  uint32_t rows = 0, sized = 0;   // the last record's kFxScreen rows / the ones with a size (the up_screens line)
  void SizesFrom(const SceneTables& t) {
    rows = sized = 0;
    for (const StageFxRow& r : t.stage_fx)
      if (r.kind == kFxScreen) {
        ++rows;
        if (!(r.flags & kFxfScreenIdle) && r.w && r.h && r.w <= 4096 && r.h <= 4096) size_[r.slot] = {r.w, r.h}, ++sized;
      }
  }
  // The feed shapes of the live path (rf_c.h contract: the pointer is valid until the NEXT call). cache_n == 0: ONE buffer overwritten by every call (c_driver's TestFeed shape); cache_n > 0: a round-robin cache of
  // that many converted pictures looked up by content id, a hit hands back the cached buffer (YuvScreenFeed's shape; with more distinct pictures than entries a later conversion recycles an earlier hit).
  // `distinct` = the id (and the pixels) include the screen key; else every screen shows the same picture of a serial.
  size_t cache_n = 0;
  // WGS (docs\agents\arena_sheet.md): WWE_RDR_RIG_SCREEN_FEED=pattern: a TEST PATTERN per key (one saturated colour per key, a grid in a darker shade of it, the key number drawn in a still darker
  // shade): every pixel has the key's hue, so tools\scene\arena_sheet.py can find each screen in the drawn frame by colour. The colours are PatternColour's (arena_sheet.py PATTERN keeps the same table).
  bool pattern = false;
  // WGS DIM1 (docs\research\harness\DIM1.md): WWE_RDR_RIG_SCREEN_FEED=flat, or the per-job flag --flat-feed: one SOLID colour per key (no grid, no digits): arena_sheet.py judges a screen's
  // brightness on this picture against the key colour x the interpolated vertex colour (the pattern's 0.35 digits and 0.6 grid made a piece that shows mostly digits read dim)
  bool flat = false;
  static void PatternColour(uint32_t key, uint8_t rgb[3]) {
    static const uint8_t kCol[10][3] = {{255, 0, 0}, {0, 255, 0}, {0, 64, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}, {255, 128, 0}, {128, 0, 255}, {0, 255, 128}, {255, 0, 128}};
    const uint8_t* c = kCol[(key >= 800 && key <= 809) ? key - 800 : 0];
    rgb[0] = c[0], rgb[1] = c[1], rgb[2] = c[2];
  }
  // WGS6 round 2: the FLAT feed's colour per key (arena_sheet.py FLAT keeps the same table; the unit checks they are equal). Pastels with ALL THREE channels at 100 or more, distinct per key, every largest channel 255, and
  // |R - B| >= 70 on every key: the flat render is where the vertex colour's per-channel tint is read, and a key colour with no blue (800, 801, 803, 806: the pattern's) cannot show a blue-channel tint
  // and a red / blue swap changes nothing where R = B = 0. The pattern feed keeps its saturated colours (the picture Jarrod reads).
  static void FlatColour(uint32_t key, uint8_t rgb[3]) {
    static const uint8_t kFlat[10][3] = {{255, 170, 150}, {200, 255, 130}, {150, 185, 255}, {255, 235, 140}, {255, 140, 170}, {140, 255, 235}, {255, 205, 120}, {170, 150, 255}, {130, 255, 200}, {255, 105, 175}};
    const uint8_t* c = kFlat[(key >= 800 && key <= 809) ? key - 800 : 0];
    rgb[0] = c[0], rgb[1] = c[1], rgb[2] = c[2];
  }
  static void DrawPattern(uint32_t key, uint32_t w, uint32_t h, uint8_t* buf, bool flat_only = false) {
    // a 3x5 digit font, one row per digit, 3 bits per row (bit 2 = left)
    static const uint8_t kFont[10][5] = {{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};
    uint8_t c[3];
    if (flat_only) FlatColour(key, c);   // WGS6 round 2: the flat feed has its own pastel colours (FlatColour)
    else PatternColour(key, c);
    const uint32_t cell = std::max<uint32_t>(1, std::min(w / 16, h / 7));   // three digits of 3 cells + 2 gaps fill about 11 of 16 columns
    const uint32_t tw = cell * 11, th = cell * 5, x0 = (w > tw ? (w - tw) / 2 : 0), y0 = (h > th ? (h - th) / 2 : 0);
    const uint32_t gstep = std::max<uint32_t>(8, std::min(w, h) / 4), gw = std::max<uint32_t>(1, gstep / 16);
    char digits[4];
    std::snprintf(digits, sizeof digits, "%03u", key % 1000);
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        float s = 1.f;
        if (!flat_only && (x % gstep < gw || y % gstep < gw)) s = 0.6f;   // the grid
        if (!flat_only && x >= x0 && y >= y0 && x < x0 + tw && y < y0 + th) {
          const uint32_t cx = (x - x0) / cell, cy = (y - y0) / cell, d = cx / 4, col = cx % 4;
          if (d < 3 && col < 3 && (kFont[digits[d] - '0'][cy] >> (2 - col) & 1u)) s = 0.35f;   // the key number
        }
        uint8_t* p = &buf[(size_t(y) * w + x) * 4];
        p[0] = uint8_t(c[0] * s), p[1] = uint8_t(c[1] * s), p[2] = uint8_t(c[2] * s), p[3] = 255;
      }
  }
  bool Picture(uint32_t key, uint32_t seq, uint32_t& w, uint32_t& h, const uint8_t*& rgba, uint64_t& content_id) override {
    const auto it = size_.find(key);
    if (it == size_.end()) return false;
    w = it->second.first, h = it->second.second;
    const uint32_t k = (distinct || pattern || flat) ? key : 0;
    const uint64_t id = (uint64_t(k) << 32) | seq | (flat ? (1ull << 63) : 0ull);   // the flat picture is other pixels than the pattern: its own content id
    content_id = id;
    Entry* e = nullptr;
    if (cache_n) {
      if (cache_.size() != cache_n) cache_.assign(cache_n, Entry());
      for (Entry& c : cache_)
        if (c.valid && c.id == id && c.w == w && c.h == h) { rgba = c.buf.data(); return true; }
      e = &cache_[next_++ % cache_n];
    } else {
      e = &one_;
    }
    e->valid = true, e->id = id, e->w = w, e->h = h;
    e->buf.resize(size_t(w) * h * 4);
    if (pattern || flat) {
      DrawPattern(key, w, h, e->buf.data(), flat);
      rgba = e->buf.data();
      return true;
    }
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        uint8_t* p = &e->buf[(size_t(y) * w + x) * 4];
        p[0] = uint8_t((x * 3 + seq * 11 + k * 29) & 255), p[1] = uint8_t((y * 5 + seq * 7 + k * 13) & 255), p[2] = uint8_t(((x ^ y) + seq * 3) & 255), p[3] = 255;
      }
    rgba = e->buf.data();
    return true;
  }
 private:
  struct Entry { bool valid = false; uint64_t id = 0; uint32_t w = 0, h = 0; std::vector<uint8_t> buf; };
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> size_;
  std::vector<Entry> cache_;
  Entry one_;
  size_t next_ = 0;
};
static RigScreenFeed* rig_feed_ptr = nullptr;
// S103 UPL: WWE_RDR_RIG_SCREEN_ROWS=1 makes the record carry live screen rows (the captures have none: their screens show the arena file's picture). One kFxScreen row per stage screen key of both looks, 256x128, a
// serial that moves with every build of the job (a first build + the --repeat builds: the steady state of a playing movie). Rows from an earlier build are replaced (flags bit 0x4000 marks ours).
static uint32_t rig_screen_serial = 0;
static void RigInjectScreenRows(SceneTables& t) {
  if (!std::getenv("WWE_RDR_RIG_SCREEN_ROWS")) return;
  t.stage_fx.erase(std::remove_if(t.stage_fx.begin(), t.stage_fx.end(), [](const StageFxRow& r) { return r.kind == kFxScreen && (r.flags & 0x4000u); }), t.stage_fx.end());
  ++rig_screen_serial;
  const bool stagger = std::getenv("WWE_RDR_RIG_SCREEN_STAGGER") != nullptr, drop = std::getenv("WWE_RDR_RIG_SCREEN_DROP") != nullptr;
  uint32_t idx = 0;
  const char* feed = std::getenv("WWE_RDR_RIG_SCREEN_FEED");
  if (feed && std::string(feed) == "pattern") {   // WGS round 2: the test pattern feeds 803 too (the screen check judges every key with a mesh; without a row a shown 803 is the idle black)
    StageFxRow r;
    r.kind = kFxScreen, r.slot = 803, r.movie = 0, r.seq = rig_screen_serial, r.w = 256, r.h = 128, r.flags = 0x4000u;
    t.stage_fx.push_back(r);
  }
  for (uint16_t key : {800, 801, 802, 804, 805, 806, 807, 808, 809}) {
    const uint32_t i = idx++;
    uint32_t seq = rig_screen_serial;
    if (stagger && (i & 1)) seq = (rig_screen_serial & 1) ? rig_screen_serial : rig_screen_serial - 1;   // odd screens advance only on odd builds: on an even build they lag one serial
    if (drop && i >= 2 && i <= 4) {   // three screens' rows (802, 804, 805: shown in the entrance look) drop for the build with serial 4 and come back with the serial they had at 3
      if (rig_screen_serial == 4) continue;
      if (rig_screen_serial >= 5 && rig_screen_serial < 8) seq = 3;
    }
    StageFxRow r;
    r.kind = kFxScreen, r.slot = key, r.movie = 0, r.seq = seq, r.w = 256, r.h = 128, r.flags = 0x4000u;
    t.stage_fx.push_back(r);
  }
}
// S103 SWF: --liquid-told FILE (or WWE_RDR_LIQUID_TOLD): the liquid-map rows a capture's record cannot carry (the captures are older than the writer, src\scene_live\liquid_*.h): ONE map, its items, ONE source texture. A labelled
// FIXTURE of the rig, never a live path; every number comes from the file (the check makes it from the capture's own sweat draws' VS constants c205 / c206 and PS c203 / c204: the item the game drew). The texture is ours: a BC1
// ring (white around 0.6 of the radius, black elsewhere: R = G = B) made here, key 1, as the record's kScLiquidPictures row would carry it.
//   object IDX | actor            the map's body: IDX = an index into the record's objects; `actor` = the first object whose id starts with "actor:slot:" and that has an engine object
//   map W H LEVELS FLAGS          LiquidMapRow (flags: 2 = clear first, 8 = dim)
//   item TYPE PASS lx0 ly0 lx1 ly1 wx0 wy0 wx1 wy1     one item: LocalArea (c205), WorldParam (c206); the dirty rect is WorldParam x (W, H) snapped OUT to 32 as the game's is
// Rows are repeated into every frame the rig draws (the hook's per-hook-frame check applies each once).
static void RigBlobBc1(LiquidPictureRow& row) {
  row = LiquidPictureRow{};
  row.key = 1, row.width = 64, row.height = 64, row.format = kCpfBc1, row.nbytes = 16 * 16 * 8;
  const auto level = [](int x, int y) {   // 0..3 over the 4x4 block grid: white centre, black rim, a soft falloff
    const float dx = (float(x) + 0.5f) / 64.f - 0.5f, dy = (float(y) + 0.5f) / 64.f - 0.5f;
    const float d = std::sqrt(dx * dx + dy * dy) * 2.f;
    return std::min(1.f, std::max(0.f, (1.f - d) * 4.f));   // white over most of the texture, a soft edge: the item maps only a small central part of it (LocalArea is far larger than WorldParam), and its disc strength term is 0 at the centre / 1 at the rim
  };
  for (int by = 0; by < 16; ++by)
    for (int bx = 0; bx < 16; ++bx) {
      uint8_t* p = row.bytes + (by * 16 + bx) * 8;
      const uint16_t c0 = 0xFFFF, c1 = 0x0000;   // white, black: four gray levels 1, 2/3, 1/3, 0
      std::memcpy(p, &c0, 2), std::memcpy(p + 2, &c1, 2);
      uint32_t idx = 0;
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const float v = level(bx * 4 + x, by * 4 + y);
          const int code = v > 0.83f ? 0 : v > 0.5f ? 2 : v > 0.17f ? 3 : 1;   // c0 = 1, c2 = 2/3, c3 = 1/3, c1 = 0
          idx |= uint32_t(code) << (2 * (y * 4 + x));
        }
      std::memcpy(p + 4, &idx, 4);
    }
}
static bool RigInjectLiquid(SceneTables& t, const std::string& path, std::string& note) {
  std::ifstream f(path);
  if (!f) { note = "cannot read " + path; return false; }
  LiquidMapRow row;
  row.frame = 1, row.flags = kLmEnabled | kLmResources, row.width = 1024, row.height = 512, row.levels = 3;
  std::vector<LiquidItemRow> items;
  int object_index = -2;
  std::string ln;
  while (std::getline(f, ln)) {
    std::istringstream ss(ln);
    std::string key;
    ss >> key;
    if (key == "object") {
      std::string v;
      ss >> v;
      object_index = v == "actor" ? -1 : std::atoi(v.c_str());
    } else if (key == "map") {
      uint32_t w = 1024, h = 512, lv = 3, fl = 0;
      ss >> w >> h >> lv >> fl;
      row.width = uint16_t(w), row.height = uint16_t(h), row.levels = lv;
      row.flags = kLmEnabled | kLmResources | ((fl & 2u) ? kLmClear : 0u) | ((fl & 8u) ? kLmDim : 0u);
    } else if (key == "item") {
      LiquidItemRow it;
      ss >> it.type >> it.pass;
      for (int k = 0; k < 4; ++k) ss >> it.local_area[k];
      for (int k = 0; k < 4; ++k) ss >> it.world_param[k];
      const float x0 = std::min(it.world_param[0], it.world_param[2]), x1 = std::max(it.world_param[0], it.world_param[2]), y0 = std::min(it.world_param[1], it.world_param[3]), y1 = std::max(it.world_param[1], it.world_param[3]);
      it.rect[0] = std::floor(x0 * float(row.width) / 32.f) * 32.f, it.rect[2] = std::ceil(x1 * float(row.width) / 32.f) * 32.f;
      it.rect[1] = std::floor(y0 * float(row.height) / 32.f) * 32.f, it.rect[3] = std::ceil(y1 * float(row.height) / 32.f) * 32.f;
      it.state = 0x80000000u, it.picture = 1, it.flags = kLiHasSource;
      if (ss) items.push_back(it);
    }
  }
  if (items.empty()) { note = "no item in " + path; return false; }
  if (t.engine_objects.size() != t.objects.size()) { note = "the record has no engine objects (the map cannot be joined to a body)"; return false; }
  uint32_t object = 0;
  if (object_index >= 0) {
    if (size_t(object_index) < t.engine_objects.size()) object = t.engine_objects[size_t(object_index)];
  } else {
    for (size_t i = 0; i < t.objects.size() && !object; ++i)
      if (t.engine_objects[i] && std::string(t.Str(t.objects[i].id)).rfind("actor:slot:", 0) == 0) object = t.engine_objects[i];
  }
  if (!object) { note = "no body object found for the map"; return false; }
  row.object = object, row.map = 0xC0DE0001u, row.item_first = uint32_t(t.liquid_items.size()), row.item_count = uint32_t(items.size());
  t.liquid_items.insert(t.liquid_items.end(), items.begin(), items.end());
  t.liquid_maps.push_back(row);
  LiquidPictureRow pic;
  RigBlobBc1(pic);
  t.liquid_pictures.clear();
  t.liquid_pictures.push_back(pic);
  note = "liquid fixture: " + std::to_string(items.size()) + " item(s) on object 0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%08X", object); return std::string(b); }();
  return true;
}
static uint64_t RigHash(const std::vector<uint8_t>& v) {   // FNV-1a of a picture (the last repeat's: the arms must draw the same pixels)
  uint64_t h = 1469598103934665603ull;
  for (uint8_t b : v) h = (h ^ b) * 1099511628211ull;
  return h;
}

// S103 MBC (RMP.md 2.3 site 7): no offline record carries weapon rows (sections 59-61 are written by the live hooks only), so the props in map B are proved on a FIXTURE: a box posed as a prop,
// given to the front end like ApplyWeaponRows gives a weapon (ModelCache::PosedModel + SetPropCpu) and tagged by the SAME filter (weapons::CastsMapB on a synthetic item). `c` = x,y,z,half in the GAME's space
// (y down); `gm` = the item is a gm set object (the filter must refuse it; the plant mapb_casts_gm tags it). Labelled in the report; never a game record.
static void InjectMapBFixture(rdr_scene::rf::ModelCache& cache, SceneFrame& f, const std::vector<float>& c, bool gm, int slot, std::string& report) {
  const float mirror = f.viewer_space ? -1.f : 1.f;
  assets::Model m;
  m.label = "mapb fixture";
  assets::Material mat;
  mat.shader = "yProp_Default";
  m.materials.push_back(mat);
  assets::Mesh me;
  me.material = 0;
  me.part = "mapb fixture";
  const float h = c[3];
  const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (int fc = 0; fc < 6; ++fc) {
    const float* n = N[fc];
    const int a = (fc / 2 + 1) % 3, b = (fc / 2 + 2) % 3;   // the two other axes
    const uint32_t base = uint32_t(me.vertices.size());
    for (int q = 0; q < 4; ++q) {
      const float sa = (q & 1) ? 1.f : -1.f, sb = (q & 2) ? 1.f : -1.f;
      float p[3] = {c[0], c[1], c[2]};
      p[fc / 2] += n[fc / 2] * h;
      p[a] += sa * h, p[b] += sb * h;
      assets::Vertex v{};
      v.pos[0] = p[0], v.pos[1] = p[1] * mirror, v.pos[2] = p[2];
      v.normal[0] = n[0], v.normal[1] = n[1] * mirror, v.normal[2] = n[2];
      v.color = 0xFFFFFFFFu;
      v.weights[0] = 1.f;
      me.vertices.push_back(v);
    }
    const uint32_t tri[6] = {0, 1, 2, 2, 1, 3};
    for (uint32_t t : tri) me.indices.push_back(base + t);   // both windings are drawn (the caster pass uses cull none)
    for (uint32_t t : tri) me.indices.push_back(base + (t == 1 ? 2 : t == 2 ? 1 : t));
  }
  m.meshes.push_back(me);
  const int id = cache.PosedModel("weapon:mapbfx" + std::to_string(slot), 0, m, false, rdr::kGrpWeapon);
  if (id < 0) { report += "mapb_fixture slot " + std::to_string(slot) + ": the front end refused the model\n"; return; }
  Object o;
  o.asset = uint32_t(id);
  o.world = IdentityMat4();
  weapons::Item it;
  it.id = gm ? 5 : 1;
  it.gm = gm;
  weapons::WeaponCall w{};
  const bool tagged = rdr_scene::mapb_props::On() && weapons::CastsMapB(it, w, rdr_scene::mapb_props::g_plant_cast_gm.load(std::memory_order_relaxed));
  const bool props_tagged = rdr_scene::props_casters::On() && weapons::CastsPropsMap(it, w, rdr_scene::props_casters::g_plant_cast_gm.load(std::memory_order_relaxed));   // S103 PM2: the props map's own filter
  if (tagged) o.flags |= kObjCastsMapB;
  if (props_tagged) o.flags |= kObjCastsProps;
  if (weapons::PropCpuWanted(tagged, props_tagged, rdr_scene::props_map::On(), true, f.phase_word, f.participants)) cache.SetPropCpu(id, m);   // S103 PM2 review 1: the production predicate (the rig's SHADOWS setting is on)
  f.objects.push_back(o);
  char line[256];
  std::snprintf(line, sizeof line, "mapb_fixture slot %d %s centre %.2f %.2f %.2f half %.2f tagged %d\n", slot, gm ? "gm-set item" : "weapon item", c[0], c[1], c[2], c[3], int(tagged));
  report += line;
  std::snprintf(line, sizeof line, "pm2_fixture slot %d %s props_tagged %d\n", slot, gm ? "gm-set item" : "weapon item", int(props_tagged));   // S103 PM2 (pm2_check.py reads it; a separate line: the MBC parser's `tagged (\d)` must not see it)
  report += line;
}

static std::vector<float> ParseFloats(const char* s) {
  std::vector<float> v;
  for (const char* p = s; *p;) {
    char* e = nullptr;
    v.push_back(std::strtof(p, &e));
    if (e == p) break;
    p = (*e == ',') ? e + 1 : e;
  }
  return v;
}

// --synth-camera (lane G's, moved from rsc_draw): a capture's record carries no camera yet, so a plain look-at at the objects' centroid exercises the draw path. Made in VIEWER space like the
// old tool, then handed back in the record's own space (a Live record's camera is the game's: BuildFrame converts it, so the inverse of that conversion is applied here).
static void SynthCamera(SceneTables& t) {
  const bool live = t.header.source == uint8_t(Source::Live);
  double cx = 0, cy = 0, cz = 0;
  size_t n = 0;
  // objects at the origin (the arena slots, the wrestlers before their pose) carry no position: the centroid and the spread come from the placed ones; the view distance scales with the
  // spread (the game's units: a crowd sits thousands of units out)
  const auto placed = [](const Object& o) { return o.world.m[12] != 0 || o.world.m[13] != 0 || o.world.m[14] != 0; };
  for (const Object& o : t.objects)
    if (placed(o)) cx += o.world.m[12], cy += (live ? -1.0 : 1.0) * o.world.m[13], cz += o.world.m[14], ++n;
  if (n) cx /= double(n), cy /= double(n), cz /= double(n);
  double spread = 0;
  for (const Object& o : t.objects)
    if (placed(o)) {
      const double dx = o.world.m[12] - cx, dy = (live ? -1.0 : 1.0) * o.world.m[13] - cy, dz = o.world.m[14] - cz;
      spread += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
  spread = n ? spread / double(n) : 25.0;
  if (spread < 25.0) spread = 25.0;
  const double eye[3] = {cx, cy + 0.35 * spread, cz - 1.6 * spread}, up[3] = {0, 1, 0};
  double z[3] = {cx - eye[0], cy - eye[1], cz - eye[2]};
  const auto norm = [](double* v) { const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); for (int i = 0; i < 3; ++i) v[i] /= l; };
  const auto cross = [](const double* a, const double* b, double* o) { o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0]; };
  double x[3], y[3];
  norm(z), cross(up, z, x), norm(x), cross(z, x, y);
  const double V[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
                        -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]), -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1};
  const double ys = 1.0 / std::tan(0.5 * 40.0 * 3.14159265358979 / 180.0), xs = ys * 9.0 / 16.0, zn = 0.02 * spread, zf = 8.0 * spread;
  const double P[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += V[i * 4 + k] * P[k * 4 + j];
      t.camera.view.m[i * 4 + j] = float(s);
      t.camera.proj.m[i * 4 + j] = i == j ? 1.f : 0.f;
    }
  for (int i = 0; i < 3; ++i) t.camera.eye[i] = float(eye[i]);
  if (live) {  // BuildFrame converts a Live record's camera (view row 1 and the eye's y negate): hand it the unconverted one
    for (int k = 4; k < 8; ++k) t.camera.view.m[k] = -t.camera.view.m[k];
    t.camera.eye[1] = -t.camera.eye[1];
  }
  t.camera.flags = kCamViewIsViewProj;
  t.has_camera = true;
}

// --link ID=CHAR:ATTIRE: the model of the object ID, TOLD (the record names none). The asset is the one lane B's writer would write: pac chNNN, entry model_key:(ch * 100 + attire * 10 + 2).
// Fills ONLY an object whose asset the record leaves out (index out of range, or a placeholder with no pac): an object that names its model keeps it (the record wins).
// filled = objects given the model, kept = objects with that id that already named one.
static int ApplyLink(SceneTables& t, const std::string& link, int& filled, int& kept) {
  filled = kept = 0;
  const size_t eq = link.find('=');
  int ch = 0, at = 0;
  if (eq == std::string::npos || std::sscanf(link.c_str() + eq + 1, "%d:%d", &ch, &at) != 2) return -1;
  AssetRef a;
  a.kind = uint8_t(AssetKind::Model);
  char pac[32];
  std::snprintf(pac, sizeof pac, "pac\\ch\\ch%03d.pac", ch);
  a.pac = t.AddString(pac);
  a.entry = t.AddString("model_key:" + std::to_string(ch * 100 + at * 10 + 2));
  a.variant = uint32_t(at);
  const uint32_t ai = uint32_t(t.assets.size());
  t.assets.push_back(a);
  const std::string id = link.substr(0, eq);
  for (Object& o : t.objects) {
    if (std::string(t.Str(o.id)) != id) continue;
    const bool named = o.asset < ai && t.assets[o.asset].pac != 0;
    if (named) ++kept;
    else o.asset = ai, ++filled;
  }
  return filled;
}

// S103 NRBF: --piece-link SKELID=SLOT:PIECE: the own-skeleton piece SKELID (an invisible skel: object) is told to be a SUBM piece of the owner actor:slot:SLOT that the owner's body model draws, exactly as
// the live contributor writes it after its proof (wrestler_contrib.cpp EmitItems): the object gets kObjDrawnByOwner and stays invisible with no model of its own. PIECE is informational (the id).
// -1 = bad spec, 0 = the object or its owner (a visible actor:slot:SLOT) is not in the record. plant_hide (--piece-link-plant-hide) = the game did not draw the piece this frame: its light refs are removed
// (rf_wrestlers' hole rule, review F5: linked + owner drawn + no light ref = a hole).
static int ApplyPieceLink(SceneTables& t, const std::string& spec, bool plant_hide) {
  const size_t eq = spec.find('=');
  int slot = -1;
  unsigned piece = 0;
  if (eq == std::string::npos || std::sscanf(spec.c_str() + eq + 1, "%d:%u", &slot, &piece) != 2 || slot < 0 || piece == 0) return -1;
  const std::string id = spec.substr(0, eq), owner = "actor:slot:" + std::to_string(slot);
  int pi = -1, oi = -1;
  for (size_t k = 0; k < t.objects.size(); ++k) {
    const std::string s(t.Str(t.objects[k].id));
    if (s == id) pi = int(k);
    if (s == owner) oi = int(k);
  }
  if (pi < 0 || oi < 0 || !(t.objects[size_t(oi)].flags & kObjVisible)) return 0;
  t.objects[size_t(pi)].flags |= kObjDrawnByOwner;
  t.objects[size_t(pi)].pad = uint16_t(piece), t.objects[size_t(pi)].layer = uint8_t(slot + 1);   // S103 PZF2: the id and the owner slot the draw-time mesh check reads (as the contributor writes them)
  if (plant_hide && size_t(pi) < t.engine_objects.size()) {
    const uint32_t addr = t.engine_objects[size_t(pi)];
    for (size_t k = t.light_refs.size(); k-- > 0;)
      if (t.light_refs[k].object == addr) t.light_refs.erase(t.light_refs.begin() + long(k));
  }
  return 1;
}

static void Mul4(const float* a, const float* b, float* o) {  // row-major, row-vector convention: o = a * b
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
      o[i * 4 + j] = s;
    }
}

static bool WriteImagePng(rdr::Renderer& renderer, rdr::ImageId id, const std::string& path, uint64_t& changed_px, std::string& note) {
  if (id == rdr::kNoImage) { note = "no map"; return false; }
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  rdr::ImageRef ref;
  ref.image = id;
  if (!renderer.ReadImage(ref, rgba, w, h)) { note = "read back failed"; return false; }
  changed_px = 0;
  for (size_t k = 0; k + 3 < rgba.size(); k += 4) changed_px += (rgba[k] != 255 || rgba[k + 1] != 255 || rgba[k + 2] != 255);
  if (!scene_png::WritePngRgb(path, rgba, w, h)) { note = "cannot write"; return false; }
  note = std::to_string(w) + "x" + std::to_string(h);
  return true;
}

// ---- S103 PS1: LOAD ONCE, DRAW MANY. `frame_rig --serve` (jobs on stdin, one argument line each) and `frame_rig --batch FILE` (the same lines from a file) run many jobs in ONE process: the renderer, the
// game-file source, the front-end and the model cache (the arena, the wrestlers, the crowd bodies, every texture) are made by the first job and kept for the next ones. A job is exactly a one-shot command
// line (same flags, same picture); what a job switches (the stage-look rows, --plant-look, --no-hooks, --look-*, the body scales, the L2 page dump, the metrics) is per job, never a process env var.
// What is process-level and so fixed by the first job: the game root, --assets, --no-textures, WWE_RDR_PLANT_ONE_LAYER (the front-end options), and every env plant the library reads once. A job that would
// need other front-end options, or a fresh process (--async-crowd, --plant-crowd, --skip-in-place, --particles-probe, --lists, --also-record, --also-rearena), is REFUSED (exit 5) and the caller runs it alone.
// Output: each job's text is framed "@@JOB_BEGIN n" ... "@@JOB_END n rc". The job's pictures and report files are written as a one-shot run writes them.
namespace {
struct Shared {
  bool serve = false;
  bool built = false;
  float built_scale = 1.f;   // S105 A1 review F8: the render scale the renderer of this process was made with (a serve process keeps it: a job with another --scale is refused)
  std::string game;
  bool use_offline = false, fo_textures = true, fo_one_layer = false;
  GameFilesSource source;
  std::unique_ptr<rdr::Renderer> renderer;
  std::unique_ptr<offline::Assets> offline_source;
  std::unique_ptr<FrontEnd> fe;
  std::unique_ptr<rf::ModelCache> cache;
  std::map<std::string, std::map<std::string, std::vector<uint8_t>>> cov_store;   // metrics: coverage masks kept by key (the `before` arm's) for the other arms of the same frame
  rig_metrics_gpu::Gpu gpu;   // S103 PS1: the compare pass (made on the first metrics job; destroyed before the renderer)
  size_t gpu_px = 0;
  bool gpu_tried = false;
};

void ParseBodyScales(const std::string& v) {
  size_t at = 0;
  while (at < v.size()) {
    size_t end = v.find(',', at);
    if (end == std::string::npos) end = v.size();
    const std::string item = v.substr(at, end - at);
    const size_t eq = item.find('=');
    if (eq != std::string::npos) {
      const std::string key = item.substr(0, eq);
      const float sv = std::strtof(item.c_str() + eq + 1, nullptr);
      if (key.rfind("0x", 0) == 0) rdr_scene::BodyScaleTable::Get().Set(uint32_t(std::strtoul(key.c_str(), nullptr, 0)), sv);   // an engine object address
      else rdr_scene::BodyScaleTable::Get().SetByName(key, sv);                                                                  // a record object id (a record with no engine addresses)
    }
    at = end + 1;
  }
}

std::vector<uint8_t> LoadRefRgb(const std::string& path, uint32_t& w, uint32_t& h) {
  int x = 0, y = 0, n = 0;
  unsigned char* d = stbi_load(path.c_str(), &x, &y, &n, 3);
  std::vector<uint8_t> v;
  if (!d) return v;
  w = uint32_t(x), h = uint32_t(y);
  v.assign(d, d + size_t(x) * size_t(y) * 3);
  stbi_image_free(d);
  return v;
}
}  // namespace

static int RunJob(int argc, char** argv, Shared& sh) {
  std::string rec, game, prefix, write_record;
  std::string body_scales_arg;   // S103 PS1: --body-scales "0xOBJECT=s,id=s" (per job; the one-shot reads WWE_RIG_BODY_SCALES)
  bool no_family_png = false;   // S103 PS1: --no-family-png: the per-family panels are not written as PNG files (the whole picture still is)
  std::vector<std::pair<std::string, std::string>> metric_refs;   // S103 PS1: --metrics-ref LABEL=PNG (repeatable)
  std::string metrics_out, cov_store_key, cov_from_key;           // --metrics-out FILE.json, --metrics-cov-store KEY, --metrics-cov-from KEY
  std::string metrics_device = std::getenv("WWE_RIG_METRICS") ? std::getenv("WWE_RIG_METRICS") : "gpu";   // S103 PS1: --metrics-device gpu | cpu | both (both: measure twice, FAIL when the GPU and the CPU disagree)
  std::string l2_dump_arg;       // S103 PS1: --l2-dump FILE: the composed page (WWE_RDR_L2_DUMP) for THIS job
  uint32_t w = 1280, h = 720, mask = rf::kAllFamilies;
  bool probe = false, probe_fault = false;  // S103 F: --particles-probe [--particles-probe-fault no_mirror]
  std::vector<std::string> also_rec, also_cam, also_eye;   // S103 D2: further records drawn in the SAME process after the first (cross-frame state of the hooks)
  std::string plant_crowd;
  bool async_crowd = false;
  int rearena = -1;   // S103 D2
  bool maps_fit_force = false;   // S103 E3
  bool body_scale_one = false;   // S103 L9: --body-scale-one: a rig-only fixture (a case from a live run has no body-scale table): every engine object is told scale 1, [U]
  std::vector<float> wrk_told;   // S103 WRK: --wrk-told
  std::string liquid_told_path;  // S103 SWF: --liquid-told FILE (see RigInjectLiquid)
  std::string plane_told_path;   // S103 L9: --plane-told FILE: the plane inputs the record does not carry (plane_eye / plane_pos / plane_fade / plane_fit / plane_camera: the game's own draw values), TOLD to the rig
  std::vector<float> told_key;
  std::string maps_fault_list;
  std::vector<float> prop_fixture, gm_fixture;   // S103 MBC: --prop-fixture x,y,z,half / --gm-fixture (game space)
  int scene_word = -1, participants = -1;        // S103 MBC: --scene-word N / --participants N: the phase row the offline records lack (the mode gate reads them)
  bool have_mask = false, per_family = false, plant_leak = false, no_hooks = false, synth_camera = false, lists_plant = false, lists_plant_spec = false, use_offline = false;
  int repeat = 1;
  long lists_n = 0;
  SeeOrder see_order = SeeOrder::Game;
  std::vector<float> cam, eye, cam_view, stream_vp;
  float cam_fov = 0.f, cam_near = 0.f, cam_far = 0.f;
  std::vector<std::string> links, piece_links;
  bool piece_plant_hide = false;   // S103 NRB
  std::vector<StageLookRow> look_rows;  // S103 L
  bool flat_feed = false;               // WGS DIM1: --flat-feed
  uint32_t prime_grab_key = 0;          // WGS PAR1: --prime-grab KEY
  std::string depth_out;                // SCU: --depth-out FILE (the whole / mask panel's scene depth, raw)
  StageShow show_told;                  // WGS: --show KEYS (screen keys 800-809 and art slots 301-304 shown; TOLD, written as the kFxShow rows a live writer would write)
  bool props_in_place = false, skip_in_place = false;   // S103 SG2: --props-in-place flags every --props row kCpfInPlace; --skip-in-place is the live drawer's rule (rf::crowd::SetSkipInPlace)
  uint32_t prop_picture_key = 0;        // S103 FHB: --prop-picture KEY: every --props row names this picture key (a guest handle)
  std::string picture_row_spec;         // S103 FHB: --picture-row KEY,W,H,FMT,FILE: a record CrowdPictureRow whose blocks are FILE's bytes
  std::string props_file;               // S103 D4: --props FILE ("P kind key variant c0..c11" a line): rows appended through crowd_rows::AppendProp
  bool move_source = false;
  std::vector<float> single_like;   // S103 D4: --single-like dx,dy,dz: a clone of the first posed stands person of the record (its body, palette and world), moved by (dx,dy,dz), as a SINGLE row
  float a1_scale = 1.f;   // S105 A1
  std::vector<std::string> single_specs;   // S103 D4: --single ARENA,TYPE,c0..c11: a single person with the type's palette (act 0, frame 0)
  if (sh.serve) stage_look::PlantsRef() = stage_look::Plants();   // S103 PS1: every switch a job can set starts from its default (the one-shot process starts that way)
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--record" && i + 1 < argc) rec = argv[++i];
    else if (a == "--scale" && i + 1 < argc) a1_scale = std::strtof(argv[++i], nullptr);   // S105 A1: the renderer's render scale; the scene targets are drawn at S x the logical size
    else if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
    else if (a == "--size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2) { std::fprintf(stderr, "bad --size\n"); return 1; } }
    else if (a == "--camera" && i + 1 < argc) cam = ParseFloats(argv[++i]);
    else if (a == "--eye" && i + 1 < argc) eye = ParseFloats(argv[++i]);
    else if (a == "--families" && i + 1 < argc) { mask = uint32_t(std::strtoul(argv[++i], nullptr, 0)); have_mask = true; }
    else if (a == "--per-family") per_family = true;
    else if (a == "--write-record" && i + 1 < argc) write_record = argv[++i];
    else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
    else if (a == "--plant-leak") plant_leak = true;
    // S103 L (STAGE_LIGHTING.md): the stage look rows a live writer does not write offline (the entrance flag; a rig slot's 30 floats: 12 dir, 12 col, 3 sky, 3 ground), injected from the game's own draw constants
    else if (a == "--props" && i + 1 < argc) props_file = argv[++i];
    else if (a == "--prop-picture" && i + 1 < argc) prop_picture_key = uint32_t(std::strtoul(argv[++i], nullptr, 0));   // S103 FHB
    else if (a == "--picture-row" && i + 1 < argc) picture_row_spec = argv[++i];   // S103 FHB
    else if (a == "--props-in-place") props_in_place = true;   // S103 SG2
    else if (a == "--skip-in-place") skip_in_place = true;     // S103 SG2
    else if (a == "--single" && i + 1 < argc) single_specs.push_back(argv[++i]);
    else if (a == "--single-like" && i + 1 < argc) single_like = ParseFloats(argv[++i]);
    else if (a == "--move-source") move_source = true;   // S103 D4: with --single-like: move the source person itself instead of adding a single row (the check compares the two masks)
    else if (a == "--look-entrance") { StageLookRow r; r.kind = kSlEntrance, r.flags = kSlfEntrance | kSlfEntrancePage; look_rows.push_back(r); }
    else if (a == "--flat-feed") flat_feed = true;
    else if (a == "--depth-out" && i + 1 < argc) depth_out = argv[++i];   // SCU (TEST ONLY): write the panel's scene depth (FrontEnd::ReadDepthForTest)
    else if (a == "--prime-grab" && i + 1 < argc) prime_grab_key = uint32_t(std::strtoul(argv[++i], nullptr, 0));   // WGS PAR1 (TEST ONLY): the frame grab = this key's test picture   // WGS DIM1: this job's screens show the flat feed (one solid colour per key), in a WWE_RDR_RIG_SCREEN_FEED=pattern process
    else if (a == "--show" && i + 1 < argc) {   // WGS: --show 800,801,805,303 (keys 800-809, art slots 301-304; "none" = nothing shown): the show bits TOLD, as kFxShow rows (ShowToRows)
      show_told.known = true;
      const std::string v = argv[++i];
      if (v != "none")
        for (float f : ParseFloats(v.c_str())) {
          const int k = int(f);
          if (k >= 800 && k <= 809) show_told.keys = uint16_t(show_told.keys | 1u << (k - 800));
          else if (k >= int(kFirstArtSlot) && k < int(kFirstArtSlot + kArtSlots)) show_told.art = uint8_t(show_told.art | 1u << (k - int(kFirstArtSlot)));
          else { std::fprintf(stderr, "bad --show %s: keys 800-809, art slots 301-304\n", v.c_str()); return 1; }
        }
    }
    else if (a == "--look-rig" && i + 2 < argc) {
      StageLookRow r;
      r.kind = kSlRigSlot, r.rig_slot = uint16_t(std::atoi(argv[++i]));
      const std::vector<float> v = ParseFloats(argv[++i]);
      if (v.size() != 30) { std::fprintf(stderr, "bad --look-rig: 30 floats\n"); return 1; }
      std::memcpy(r.dir, v.data(), 12 * 4), std::memcpy(r.col, v.data() + 12, 12 * 4), std::memcpy(r.sky, v.data() + 24, 12), std::memcpy(r.ground, v.data() + 27, 12);
      look_rows.push_back(r);
    }
    else if (a == "--look-pool" && i + 1 < argc) {  // S103 L2: x,z,half,r,g,b
      const std::vector<float> v = ParseFloats(argv[++i]);
      if (v.size() != 6) { std::fprintf(stderr, "bad --look-pool: x,z,half,r,g,b\n"); return 1; }
      StageLookRow r;
      r.kind = kSlPool, r.dir[0] = v[0], r.dir[1] = v[1], r.dir[2] = v[2], r.col[0] = v[3], r.col[1] = v[4], r.col[2] = v[5];
      look_rows.push_back(r);
    }
    else if (a == "--look-spot" && i + 1 < argc) {  // S103 L2: x,z,r,g,b: the follow spot at an absolute place (what the live row will carry once the rig's source is read)
      const std::vector<float> v = ParseFloats(argv[++i]);
      if (v.size() != 5) { std::fprintf(stderr, "bad --look-spot: x,z,r,g,b\n"); return 1; }
      StageLookRow r;
      r.kind = kSlSpot, r.flags = 0, r.dir[0] = v[0], r.dir[1] = v[1], r.col[0] = v[2], r.col[1] = v[3], r.col[2] = v[4];
      look_rows.push_back(r);
    }
    else if (a == "--look-spot-entrant" && i + 1 < argc) {  // S103 L2: r,g,b: the follow spot at the entrant's root (actor:slot:0 of the record)
      const std::vector<float> v = ParseFloats(argv[++i]);
      if (v.size() != 3) { std::fprintf(stderr, "bad --look-spot-entrant: r,g,b\n"); return 1; }
      StageLookRow r;
      r.kind = kSlSpot, r.flags = kSlfAtEntrant, r.col[0] = v[0], r.col[1] = v[1], r.col[2] = v[2];
      look_rows.push_back(r);
    }
    else if (a == "--plant-look" && i + 1 < argc) {
      const std::string p = argv[++i];
      if (p == "entrance") stage_look::PlantsRef().ignore_entrance = true;
      else if (p == "rig") stage_look::PlantsRef().ignore_rig = true;
      else if (p == "wrong_slot") stage_look::PlantsRef().rig_wrong_slot = true;
      else if (p == "pools") stage_look::PlantsRef().ignore_pools = true;
      else if (p == "spot") stage_look::PlantsRef().spot_wrong = true;
      else if (p == "spot150") stage_look::PlantsRef().spot_near = true;
      else if (p == "pool_swap") stage_look::PlantsRef().pool_swap = true;
      else { std::fprintf(stderr, "bad --plant-look\n"); return 1; }
    }
    else if (a == "--particles-probe") probe = true;  // S103 F
    else if (a == "--particles-probe-fault" && i + 1 < argc) probe = true, probe_fault = std::string(argv[++i]) == "no_mirror";
    else if (a == "--also-record" && i + 1 < argc) also_rec.push_back(argv[++i]);   // S103 D2 (with --also-camera / --also-eye, same order)
    else if (a == "--also-camera" && i + 1 < argc) also_cam.push_back(argv[++i]);
    else if (a == "--also-eye" && i + 1 < argc) also_eye.push_back(argv[++i]);
    else if (a == "--also-rearena" && i + 1 < argc) rearena = std::atoi(argv[++i]);   // S103 D2
    else if (a == "--async-crowd") async_crowd = true;   // S103 D2: the crowd's bodies load on the worker (the live path): builds answer "miss" until resident
    else if (a == "--plant-crowd" && i + 1 < argc) plant_crowd = argv[++i];        // S103 D2: palettes | cards_black | hidden | cards_back | inst_first
    else if (a == "--link" && i + 1 < argc) links.push_back(argv[++i]);
    else if (a == "--piece-link" && i + 1 < argc) piece_links.push_back(argv[++i]);   // S103 NRB
    else if (a == "--piece-link-plant-hide") piece_plant_hide = true;                  // S103 NRB plant
    else if (a == "--no-hooks") no_hooks = true;
    else if (a == "--body-scale-one") body_scale_one = true;   // S103 L9
    else if (a == "--plane-told" && i + 1 < argc) plane_told_path = argv[++i];   // S103 L9
    else if (a == "--liquid-told" && i + 1 < argc) liquid_told_path = argv[++i];   // S103 SWF
    else if (a == "--wrk-told" && i + 1 < argc) wrk_told = ParseFloats(argv[++i]);   // S103 WRK: the six g_fWklLv0..5 (+ eight g_fIdMapLv0..7) every wrestler body is told (a capture's record has no kScParamOverrides rows)
    else if (a == "--wrk-off") assets::g_wrk_live.store(false);   // S103 WRK: the switch OFF (the picture before the lane)
    else if (a == "--liquid-plant" && i + 1 < argc) {   // S103 SWF: off | unbound | no_prev | mask_white (comma list): the planted twins (liquid_switch.h Plants)
      std::string v = argv[++i];
      auto& pl = rdr_scene::liquid_map::Plants();
      pl.off = v.find("off") != std::string::npos, pl.unbound = v.find("unbound") != std::string::npos, pl.no_prev = v.find("no_prev") != std::string::npos, pl.mask_white = v.find("mask_white") != std::string::npos;
    }
    else if (a == "--maps-fit") maps_fit_force = true;   // S103 E3: the wrestlers' shadow camera is FITTED (shadow_fit.h) even when the record has a caster state
    else if (a == "--key-light" && i + 1 < argc) { const std::vector<float> v = ParseFloats(argv[++i]); if (v.size() != 3) { std::fprintf(stderr, "bad --key-light (x,y,z)\n"); return 1; } told_key.assign(v.begin(), v.end()); }   // S103 E3: TOLD (the record carries no light rows)
    else if (a == "--prop-fixture" && i + 1 < argc) { prop_fixture = ParseFloats(argv[++i]); if (prop_fixture.size() != 4) { std::fprintf(stderr, "bad --prop-fixture (x,y,z,half)\n"); return 1; } }
    else if (a == "--gm-fixture" && i + 1 < argc) { gm_fixture = ParseFloats(argv[++i]); if (gm_fixture.size() != 4) { std::fprintf(stderr, "bad --gm-fixture (x,y,z,half)\n"); return 1; } }
    else if (a == "--scene-word" && i + 1 < argc) scene_word = std::atoi(argv[++i]);
    else if (a == "--participants" && i + 1 < argc) participants = std::atoi(argv[++i]);
    else if (a == "--maps-fault" && i + 1 < argc) maps_fault_list = argv[++i];   // S103 E3: the maps' planted faults (live_maps.h MapsFaults)
    else if (a == "--see-order" && i + 1 < argc) {
      const std::string v = argv[++i];
      if (v == "game") see_order = SeeOrder::Game;
      else if (v == "distance") see_order = SeeOrder::Distance;
      else { std::fprintf(stderr, "bad --see-order (game | distance)\n"); return 1; }
    }
    else if (a == "--assets" && i + 1 < argc) { const std::string v = argv[++i]; if (v == "offline") use_offline = true; else if (v != "files") { std::fprintf(stderr, "bad --assets (files | offline)\n"); return 1; } }
    else if (a == "--camera-view" && i + 1 < argc) cam_view = ParseFloats(argv[++i]);   // a record-style camera: 16 floats (game-space row-vector view) + --fov / --near / --far
    else if (a == "--fov" && i + 1 < argc) cam_fov = std::strtof(argv[++i], nullptr);
    else if (a == "--near" && i + 1 < argc) cam_near = std::strtof(argv[++i], nullptr);
    else if (a == "--far" && i + 1 < argc) cam_far = std::strtof(argv[++i], nullptr);
    else if (a == "--stream-viewproj" && i + 1 < argc) stream_vp = ParseFloats(argv[++i]);   // the stream's row-vector ViewProj: compared with view x BuildFrame's projection
    else if (a == "--synth-camera") synth_camera = true;
    else if (a == "--lists" && i + 1 < argc) lists_n = std::atol(argv[++i]);
    else if (a == "--lists-plant") lists_plant = true;                 // the planted fault: the lists ignore a moved world
    else if (a == "--lists-plant-specular") lists_plant_spec = true;   // review B1's fault: SpecularCol's mask without the light
    else if (a == "--body-scales" && i + 1 < argc) body_scales_arg = argv[++i];   // S103 PS1
    else if (a == "--no-family-png") no_family_png = true;                         // S103 PS1
    else if (a == "--metrics-ref" && i + 1 < argc) { const std::string v = argv[++i]; const size_t eq = v.find('='); metric_refs.push_back({v.substr(0, eq), eq == std::string::npos ? "" : v.substr(eq + 1)}); }   // S103 PS1
    else if (a == "--metrics-out" && i + 1 < argc) metrics_out = argv[++i];
    else if (a == "--metrics-device" && i + 1 < argc) metrics_device = argv[++i];
    else if (a == "--metrics-cov-store" && i + 1 < argc) cov_store_key = argv[++i];
    else if (a == "--metrics-cov-from" && i + 1 < argc) cov_from_key = argv[++i];
    else if (a == "--l2-dump" && i + 1 < argc) l2_dump_arg = argv[++i];
  }
  {  // S103 W2: the offline rig has no game stream, so the body scales the live WrestlerCore would measure come in by env (frame_rig.py runs w2_skin_path_check --scales on the capture's own stream draws):
     // "0xOBJECT=s,0xOBJECT=s". A Live record's wrestler with no scale stays a hole (the real rule: rf_wrestlers.cpp). S103 PS1: per job by --body-scales (a serve process starts every job from an empty table).
    if (sh.serve) BodyScaleTable::Get().Clear();
    if (!body_scales_arg.empty()) ParseBodyScales(body_scales_arg);
    else if (const char* e = std::getenv("WWE_RIG_BODY_SCALES")) ParseBodyScales(e);
  }
  if (sh.serve) {
    if (!l2_dump_arg.empty()) _putenv_s("WWE_RDR_L2_DUMP", l2_dump_arg.c_str());
    else _putenv_s("WWE_RDR_L2_DUMP", "");
  } else if (!l2_dump_arg.empty()) _putenv_s("WWE_RDR_L2_DUMP", l2_dump_arg.c_str());
  if (rec.empty() || game.empty() || prefix.empty()) { std::fprintf(stderr, "usage: frame_rig --record F.rsc --game DIR --prefix P [--size WxH] [--camera 16f] [--eye 3f] [--families MASK] [--per-family] [--link ID=CH:AT] [--see-order game|distance] [--no-hooks] [--synth-camera] [--lists N]\n"); return 1; }
  if (metrics_device != "gpu" && metrics_device != "cpu" && metrics_device != "both") { std::fprintf(stderr, "frame_rig: FAIL unknown --metrics-device '%s' (gpu | cpu | both)\n", metrics_device.c_str()); return 1; }   // S103 PS1 F3
  SceneTables t;
  std::string err;
  if (!ReadRecord(rec, t, err)) { std::fprintf(stderr, "frame_rig: FAIL bad record: %s\n", err.c_str()); return 2; }
  if (body_scale_one)   // S103 L9
    for (uint32_t addr : t.engine_objects) BodyScaleTable::Get().Set(addr, 1.0f);
  t.stage_look.insert(t.stage_look.end(), look_rows.begin(), look_rows.end());  // S103 L
  if (show_told.known) {   // WGS: the told show bits replace any the record carries (two sets of rows would OR together)
    t.stage_fx.erase(std::remove_if(t.stage_fx.begin(), t.stage_fx.end(), [](const StageFxRow& r) { return r.kind == kFxShow; }), t.stage_fx.end());
    ShowToRows(show_told, t.stage_fx);
  }
  if (!wrk_told.empty()) {   // S103 WRK: a FIXTURE of the rig (never a live path): the level rows the latch would have read, as model-wide rows of every body's engine object
    static const char* const kNames[14] = {"g_fWklLv0", "g_fWklLv1", "g_fWklLv2", "g_fWklLv3", "g_fWklLv4", "g_fWklLv5", "g_fIdMapLv0", "g_fIdMapLv1", "g_fIdMapLv2", "g_fIdMapLv3", "g_fIdMapLv4", "g_fIdMapLv5", "g_fIdMapLv6", "g_fIdMapLv7"};
    for (uint32_t addr : t.engine_objects) {
      if (!addr) continue;
      for (size_t k = 0; k < wrk_told.size() && k < 14; ++k) {
        ParamOverrideRow r;
        r.frame = 1, r.model = addr, r.part = kParamModelWide, r.index = uint32_t(k), r.flags = kPoNameRead | kPoValueRead;
        std::strncpy(r.name, kNames[k], 15);
        std::memcpy(&r.value[0], &wrk_told[k], 4);
        t.param_overrides.push_back(r);
      }
    }
  }
  std::string liquid_note;
  if (!liquid_told_path.empty()) {
    if (!RigInjectLiquid(t, liquid_told_path, liquid_note)) { std::fprintf(stderr, "frame_rig: FAIL liquid fixture: %s\n", liquid_note.c_str()); return 2; }
    std::fprintf(stderr, "frame_rig: %s\n", liquid_note.c_str());
  }
  std::string rows_report;   // S103 D4: what the writer API took
  if (!props_file.empty() || !single_specs.empty() || single_like.size() == 3) {
    crowd_rows::Counts rc_;
    if (single_like.size() == 3) {
      const Object* src = nullptr;
      double best_dist = 1e30;
      for (const Object& o : t.objects) {
        if (o.klass != rdr_adapter::DrawClass::Crowd || !(o.flags & kObjVisible) || !(o.flags & kObjSkinned) || !o.bones_count || size_t(o.bones_first) + o.bones_count > t.bones.size() || o.asset >= t.assets.size()) continue;
        int entry = 0;
        if (std::sscanf(std::string(t.Str(t.assets[o.asset].entry)).c_str(), "%d", &entry) != 1 || entry < 20000 || entry >= 20120) continue;   // a body of any LOD (this capture holds LOD 1 / 2 people near the camera; the asset carries its LOD)
        double dist = 0;   // the person nearest the screen centre (so the clone is on screen): the row-vector view-projection of the --camera, translation of the world
        if (cam.size() == 16) {
          double c4[4];
          for (int j = 0; j < 4; ++j) c4[j] = double(o.world.m[12]) * cam[size_t(j)] + double(o.world.m[13]) * cam[size_t(4 + j)] + double(o.world.m[14]) * cam[size_t(8 + j)] + cam[size_t(12 + j)];
          if (!(c4[3] > 1.0)) continue;
          dist = std::hypot(c4[0] / c4[3], c4[1] / c4[3]);
          if (dist > 1.1 || std::fabs(c4[0] / c4[3]) > 0.8 || std::fabs(c4[1] / c4[3]) > 0.8) continue;
        }
        if (!src || dist < best_dist) src = &o, best_dist = dist;
      }
      if (!src) {
        unsigned crowd = 0, vis = 0, skinned = 0, with_bones = 0;
        std::string first_entries;
        for (const Object& o : t.objects) {
          if (o.klass != rdr_adapter::DrawClass::Crowd) continue;
          ++crowd, vis += (o.flags & kObjVisible) ? 1u : 0u, skinned += (o.flags & kObjSkinned) ? 1u : 0u, with_bones += o.bones_count ? 1u : 0u;
          if (first_entries.size() < 80 && o.asset < t.assets.size()) first_entries += std::string(t.Str(t.assets[o.asset].entry)) + " ";
        }
        std::fprintf(stderr, "frame_rig: FAIL --single-like: the record holds no posed stands person to clone (crowd objects %u, visible %u, skinned %u, with a palette %u, entries: %s; runs %zu)\n", crowd, vis, skinned, with_bones, first_entries.c_str(), t.crowd_runs.size());
        return 2;
      }
      crowd_rows::SingleIn s;
      s.asset = src->asset, s.type = uint8_t((std::atoi(std::string(t.Str(t.assets[src->asset].entry)).c_str()) - 20000) % 40);
      Mat4 w = src->world;
      w.m[12] += single_like[0], w.m[13] += single_like[1], w.m[14] += single_like[2];
      crowd_rows::ConstantsFromWorld(w, s.c);
      const std::vector<Mat4> pal(t.bones.begin() + src->bones_first, t.bones.begin() + src->bones_first + src->bones_count);   // (a copy: AppendSingle grows t.bones)
      s.palette = pal.data(), s.nodes = uint32_t(pal.size());
      if (move_source) { Object* mo = const_cast<Object*>(src); mo->world = w; }   // the control: the SOURCE person itself moves (stands path), no row
      else crowd_rows::AppendSingle(t, s, &rc_);
      char sl[200];
      std::snprintf(sl, sizeof sl, "single-like: the person %s (type %d) moved by (%g, %g, %g): world translation now (%g, %g, %g)\n", std::string(t.Str(src->id)).c_str(), int(s.type), single_like[0], single_like[1], single_like[2], w.m[12], w.m[13], w.m[14]);
      rows_report += sl;
    }
    if (!props_file.empty()) {
      std::ifstream in(props_file);
      if (!in) { std::fprintf(stderr, "frame_rig: FAIL cannot read --props %s\n", props_file.c_str()); return 2; }
      std::string ln;
      while (std::getline(in, ln)) {
        if (ln.empty() || ln[0] != 'P') continue;
        std::istringstream is(ln.substr(1));
        crowd_rows::PropIn p;
        unsigned kind = 0, key = 0, variant = 0;
        if (!(is >> kind >> key >> variant)) continue;
        p.kind = uint8_t(kind), p.model_key = key, p.variant = uint16_t(variant);
        if (props_in_place) p.flags |= kCpfInPlace;   // S103 SG2: the rows a walked frame's record carries
        for (float& f : p.c) is >> f;
        p.picture = prop_picture_key;   // S103 FHB
        crowd_rows::AppendProp(t, p, &rc_);
      }
    }
    if (!picture_row_spec.empty()) {   // S103 FHB: KEY,W,H,FMT,FILE
      unsigned key = 0, w = 0, h = 0, fmt = 0;
      char fname[512] = {};
      if (std::sscanf(picture_row_spec.c_str(), "%i,%u,%u,%u,%511[^\n]", &key, &w, &h, &fmt, fname) != 5) { std::fprintf(stderr, "bad --picture-row KEY,W,H,FMT,FILE\n"); return 1; }
      std::ifstream pf(fname, std::ios::binary);
      std::vector<char> blob((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
      CrowdPictureRow pr;
      pr.key = key, pr.width = uint16_t(w), pr.height = uint16_t(h), pr.format = fmt, pr.nbytes = uint32_t(blob.size());
      if (blob.size() > sizeof pr.bytes) { std::fprintf(stderr, "--picture-row: %zu bytes is over the row", blob.size()); return 1; }
      std::memcpy(pr.bytes, blob.data(), blob.size());
      if (!CrowdPictureValid(pr)) { std::fprintf(stderr, "--picture-row: not a valid picture row\n"); return 1; }
      t.crowd_pictures.push_back(pr);
    }
    for (const std::string& spec : single_specs) {
      const std::vector<float> v = ParseFloats(spec.c_str());
      if (v.size() != 14) { std::fprintf(stderr, "bad --single %s (ARENA,TYPE,c0..c11)\n", spec.c_str()); return 1; }
      const int arena = int(v[0]), type = int(v[1]);
      assets::crowd_body::Init(game);
      std::vector<std::vector<assets::Matrix>> pal;
      std::string perr;
      const bool have_pal = assets::crowd_body::Palettes(arena, 0, 0, pal, perr) && type >= 0 && size_t(type) < pal.size() && !pal[size_t(type)].empty();
      crowd_rows::SingleIn s;
      s.asset = crowd_rows::BodyAsset(t, arena, type), s.type = uint8_t(type);
      std::memcpy(s.c, v.data() + 2, sizeof s.c);
      std::vector<Mat4> mats;
      if (have_pal) {
        mats.resize(pal[size_t(type)].size());
        for (size_t k = 0; k < mats.size(); ++k) std::memcpy(mats[k].m, pal[size_t(type)][k].data(), sizeof mats[k].m);
        s.palette = mats.data(), s.nodes = uint32_t(mats.size());
      }
      crowd_rows::AppendSingle(t, s, &rc_);
    }
    char rl[300];
    std::snprintf(rl, sizeof rl, "crowd rows (writer API): %s; record now holds %zu props, %zu singles\n", rc_.Text().c_str(), t.crowd_props.size(), t.crowd_singles.size());
    rows_report += rl;
  }
  if (cam.size() == 16) {
    std::memcpy(t.camera.view.m, cam.data(), sizeof(float) * 16);
    t.camera.proj = IdentityMat4();
    t.camera.flags = kCamViewIsViewProj;
    if (eye.size() == 3) std::memcpy(t.camera.eye, eye.data(), sizeof(float) * 3);
    t.has_camera = true;
  }
  if (cam_view.size() == 16) {   // the camera as a record's own would be: a rigid game-space view + FOV (kCamProjFromFov): BuildFrame builds the projection for this target
    std::memcpy(t.camera.view.m, cam_view.data(), sizeof(float) * 16);
    t.camera.proj = IdentityMat4();
    t.camera.flags = kCamProjFromFov;
    t.camera.fov_y_deg = cam_fov, t.camera.near_z = cam_near, t.camera.far_z = cam_far;
    if (eye.size() == 3) std::memcpy(t.camera.eye, eye.data(), sizeof(float) * 3);
    t.has_camera = true;
  }
  if (!t.has_camera && synth_camera) SynthCamera(t);
  if (!t.has_camera) { std::fprintf(stderr, "frame_rig: FAIL the record has no camera and none was given (--camera)\n"); return 2; }
  if (!write_record.empty()) {  // the camera test: the same record with its camera written in the file (record_io.h "CAMERA LAYOUT"), drawn again without --camera
    if (!WriteRecord(write_record, t, err)) { std::fprintf(stderr, "frame_rig: FAIL cannot write the record: %s\n", err.c_str()); return 3; }
    return 0;
  }
  std::string report = rows_report;
  char line[512];
  if (show_told.known) {   // WGS
    std::snprintf(line, sizeof line, "show TOLD keys 0x%03x art 0x%x (kFxShow rows from --show, not from the record)\n", unsigned(show_told.keys), unsigned(show_told.art));
    report += line;
  }
  int links_filled = 0, links_kept = 0;
  for (const std::string& l : links) {
    int filled = 0, kept = 0;
    if (ApplyLink(t, l, filled, kept) < 0) { std::fprintf(stderr, "bad --link %s (ID=CHAR:ATTIRE)\n", l.c_str()); return 1; }
    links_filled += filled, links_kept += kept;
    std::snprintf(line, sizeof line, "LINK (told, not in the record): %s -> filled %d object(s), kept %d that name their own model\n", l.c_str(), filled, kept);
    report += line;
  }
  if (!links.empty()) { std::snprintf(line, sizeof line, "links_total filled %d kept %d\n", links_filled, links_kept); report += line; }
  {   // S103 NRB
    int told = 0;
    for (const std::string& l : piece_links) {
      const int r = ApplyPieceLink(t, l, piece_plant_hide);
      if (r < 0) { std::fprintf(stderr, "bad --piece-link %s (SKELID=SLOT:PIECE)\n", l.c_str()); return 1; }
      told += r;
      std::snprintf(line, sizeof line, "PIECE LINK (told, not in the record): %s -> %s\n", l.c_str(), r ? "drawn by its owner" : "NOT FOUND (object or visible owner missing)");
      report += line;
    }
    if (!piece_links.empty()) { std::snprintf(line, sizeof line, "piece_links_total %d of %zu\n", told, piece_links.size()); report += line; }
  }

  FrontEndOptions fo;
  if (const char* v = std::getenv("WWE_RDR_NO_TEXTURES")) fo.textures = std::atoi(v) == 0;
  if (const char* sb = std::getenv("WWE_RDR_SKIP_BLANK_PLANE")) rdr_scene::g_skip_blank_plane.store(std::strcmp(sb, "0") != 0);   /* S103 SLV2: "0" turns the rule OFF, anything else on */   // RIG-ONLY: S103 SLV (slv_check.py): the blank plane rule on (the game start stores it from wwe2k14_rdr_skip_blank_plane, start_settings.cpp)
  if (const char* pco = std::getenv("WWE_RDR_PROPS_CASTERS")) rdr_scene::props_casters::g_on.store(std::strcmp(pco, "0") != 0);   // RIG-ONLY: S103 PM2: "0" = the props map without its prop casters (the switch's off arm)
  if (const char* gco = std::getenv("WWE_RDR_GAME_CLEAR")) rdr_scene::game_clear::g_on.store(std::strcmp(gco, "0") != 0);   // RIG-ONLY: S103 CLRB: "1" = a live record clears to the game's colour (black without a row); the header default is OFF (the slate), the game turns it on at start (frame_rig.py --game-clear)
  if (const char* pmo = std::getenv("WWE_RDR_PROPS_MAP")) rdr_scene::props_map::g_on.store(std::strcmp(pmo, "0") != 0);   // RIG-ONLY: S103 PMS: "0" = the old binding (the floor reads map B); the game start stores the cvar wwe2k14_rdr_props_map (start_settings.cpp)
  fo.plant_one_layer = std::getenv("WWE_RDR_PLANT_ONE_LAYER") != nullptr;   // S103 LAY planted twin (lay_check.py --plant)
  fo.plant_old_alpha_ref = std::getenv("WWE_RDR_PLANT_OLD_ALPHA_REF") != nullptr;   // S103 B2 planted twin (b2_check.py --plant old_ref)
  fo.plant_cutout_noblend = std::getenv("WWE_RDR_PLANT_CUTOUT_NOBLEND") != nullptr;   // S103 B2 planted twin (b2_check.py --plant cutout_noblend)
  fo.plant_untested_alpha = std::getenv("WWE_RDR_PLANT_UNTESTED_ALPHA") != nullptr;   // S103 B2 planted twin (b2_check.py --plant untested_alpha)
  fo.plant_mat_plain_when_no_module = std::getenv("WWE_RDR_PLANT_MAT_PLAIN") != nullptr;   // S103 RML planted twin (rml_check.py --faults): today's binding, no contact shadow on the mat
  fo.plant_vs_prm_ps_rows = std::getenv("WWE_RDR_PLANT_VS_PRM_PS") != nullptr;             // S103 RML planted twin (pms_check.py --rig): the vertex ShadowPrm block gets the pixel rows
  fo.plant_light_stamp_no_mat = std::getenv("WWE_RDR_PLANT_LIGHT_STAMP") != nullptr;      // S103 RML planted twin (rml_check.py --lists): the kept lists' light stamp leaves out the mat
  fo.plant_mat_reads_mapb = std::getenv("WWE_RDR_PLANT_MAT_MAPB") != nullptr;             // S103 RML planted twin: the mat's slot 12 is the wrestlers' map B
  const bool lists_hooks = std::getenv("WWE_RDR_RML_LISTS_HOOKS") != nullptr;   // S103 RML (RIG-ONLY): --lists keeps the maps hook on (the props map exists, so the ring mat's light pass is drawn through the kept lists)
  const bool plane_off = std::getenv("WWE_RDR_PLANE_GAME_OFF") != nullptr;                 // S103 RML (RIG-ONLY): the game built no plane module in this capture (wm30: 3 wrestlers; the live path writes the flag from the latch, inputs_contrib.h)
  rdr_scene::g_plane_game_no_module.store(plane_off, std::memory_order_relaxed);
  const bool b2_plants = fo.plant_old_alpha_ref || fo.plant_cutout_noblend || fo.plant_untested_alpha || fo.plant_mat_plain_when_no_module || fo.plant_mat_reads_mapb || fo.plant_vs_prm_ps_rows || fo.plant_light_stamp_no_mat || plane_off;   // S103: B2 / RML plants change the front end: never served
  const bool reusable = !b2_plants && !std::getenv("WWE_RDR_STAGE_CULL_FLIP") && !plant_leak && !async_crowd && plant_crowd.empty() && !skip_in_place && !probe && lists_n <= 0 && also_rec.empty() && rearena < 0;
  if (sh.serve && !reusable) { std::fprintf(stderr, "frame_rig: SERVE REFUSED: this job needs a fresh process (async crowd / crowd plant / skip-in-place / particle probe / lists / also-record / plant-leak): run it alone\n"); return 5; }
  if (sh.serve && sh.built && (sh.game != game || sh.use_offline != use_offline || sh.fo_textures != fo.textures || sh.fo_one_layer != fo.plant_one_layer)) {
    std::fprintf(stderr, "frame_rig: SERVE REFUSED: the front-end options of this job (game root / --assets / textures / one-layer plant) differ from the ones this process was made with: run it in another process\n");
    return 5;
  }
  if (sh.built && a1_scale != sh.built_scale) { std::fprintf(stderr, "frame_rig: FAIL --scale %g: this process' renderer was made with --scale %g (the scale is fixed when the renderer is made: start another process)\n", double(a1_scale), double(sh.built_scale)); return 1; }
  if (!sh.built) {
    sh.game = game, sh.use_offline = use_offline, sh.fo_textures = fo.textures, sh.fo_one_layer = fo.plant_one_layer;
    if (!sh.source.SetRoot(game)) { std::fprintf(stderr, "frame_rig: FAIL no game files under %s\n", game.c_str()); return 3; }
    if (use_offline) { ViewerInit(game); assets::crowd_body::Init(game); }
    rdr::RendererOptions ro;
    if (const char* v = std::getenv("WWE_RDR_VALIDATION")) ro.validation = std::atoi(v);
    ro.render_scale = a1_scale;   // S105 A1
    sh.renderer = rdr::Renderer::Create(ro, err);
    if (!sh.renderer) { std::fprintf(stderr, "frame_rig: FAIL renderer: %s\n", err.c_str()); return 3; }
    sh.built = true, sh.built_scale = a1_scale;
  }
  auto& renderer = sh.renderer;
  rf::crowd::SetFaultForTest(plant_crowd.c_str());   // S103 D2 (before the crowd is made)
  {   // SCU (tools\scene\stage_cull_proof.py): RIG-ONLY stand-ins of the switch wwe2k14_rdr_stage_cull (=0: the stage meshes two-sided, the "off" arm) and its planted twin (the `r` copy without the winding swap). Set per job (a served job resets them)
    const char* sc = std::getenv("WWE_RDR_STAGE_CULL");
    rdr_scene::g_stage_cull.store(!(sc && std::string(sc) == "0"), std::memory_order_relaxed);
    rdr_scene::g_plant_stage_cull_no_flip.store(std::getenv("WWE_RDR_PLANT_STAGE_CULL_NO_FLIP") != nullptr, std::memory_order_relaxed);
  }
  {   // S105 C3 (tools\scene\family_cull_proof.py): RIG-ONLY stand-ins of the switch wwe2k14_rdr_family_cull (=0: the non-stage families two-sided, the "off" arm) and its planted twin (the non-stage families with a swapped front face)
    const char* fc = std::getenv("WWE_RDR_FAMILY_CULL");
    rdr_scene::g_family_cull.store(!(fc && std::string(fc) == "0"), std::memory_order_relaxed);
    rdr_scene::g_plant_family_cull_flip.store(std::getenv("WWE_RDR_PLANT_FAMILY_CULL_FLIP") != nullptr, std::memory_order_relaxed);
  }
  {   // S105 SZW (tools\scene\stage_zwrite_proof.py): RIG-ONLY stand-ins of the switch wwe2k14_rdr_stage_zwrite (=0: no depth write for `s` / `a`, no forced test, far to near: the "off" arm = today's draw) and its planted twin (the depth write with NO forced alpha test)
    const char* sz = std::getenv("WWE_RDR_STAGE_ZWRITE");
    rdr_scene::g_stage_zwrite.store(!(sz && std::string(sz) == "0"), std::memory_order_relaxed);
    rdr_scene::g_plant_szw_no_test.store(std::getenv("WWE_RDR_PLANT_SZW_NO_TEST") != nullptr, std::memory_order_relaxed);
  }
  const bool stage_cull_flip = std::getenv("WWE_RDR_STAGE_CULL_FLIP") != nullptr;   // SCU (RIG-ONLY): every --repeat frame flips the switch's stand-in (with the kept draw lists on)
  if (std::getenv("WWE_RDR_PLANT_NO_SCREEN_RULE")) rf::c::PlantsRef().no_screen_rule = true;   // S103 SCR planted twin: the loader applies no movie-surface rule (scr_check.py --plant)
  if (std::getenv("WWE_RDR_PLANT_SCREEN_HIGHLIGHT")) rf::c::PlantsRef().screen_highlight_kept = true;   // WGS planted twin of TRN4: the movie keys keep the file's highlight (arena_sheet.py --plant highlight)
  if (const char* tp = std::getenv("WWE_RDR_TABLE_PICK_ALL"); tp && std::string(tp) == "0") rdr_scene::g_table_pick_all.store(false);   // RIG-ONLY (WGS for lane WGD): =0 = the TQ2 switch off (front_end.h g_table_pick_all; the "before" arm); anything else keeps the default. The game start stores it from the cvar wwe2k14_rdr_table_pick_all (start_settings.cpp)
  if (std::getenv("WWE_RDR_PLANT_STALE_MOVIE_KEPT")) rf::c::PlantsRef().stale_movie_kept = true;   // S103 HN planted twin: a shown screen with no row keeps its last picture
  if (std::getenv("WWE_RDR_PLANT_IDLE_SCREEN_WHITE")) rf::c::PlantsRef().idle_screen_white = true;   // S103 SCN planted twin: an idle movie-key screen binds white_ (scn_check.py --plant)
  if (const char* e = std::getenv("WWE_RDR_HIDE_STAGE_SLOT")) g_hide_stage_slot = *e ? std::atoi(e) : -1;   // S103 ENV: CAWB dev plant, now rdr_scene::g_hide_stage_slot (front_end.h) (was a getenv in front_end.cpp)
  if (const char* e = std::getenv("WWE_RDR_GUEST_FLAT")) rf::Plants().guest_flat = *e && *e != '0';   // S103 ENV: CAWD dev plant, now rf::WrestlerPlants::guest_flat (was a getenv in rf_wrestlers.cpp)
  if (std::getenv("WWE_RDR_NO_ARENA_FOLLOW")) rf::c::PlantsRef().no_arena_follow = true;  // S103 RL planted twin: the camera-attached arena lights stay unturned (rl_check.py --plant follow)
  if (std::getenv("WWE_RDR_PROPS_SET100")) assets::load_detail::PropsSet100Plant() = true;   // S103 RL planted twin: the steps / desk on the old set 100 (before the arena is loaded)
  int rc = 0;
  uint64_t lists_bad = 0;
  {
    if (!sh.fe) {   // the front-end and the model cache: made by the first job (the one-shot's only job), kept by a serve process
      sh.fe = std::make_unique<FrontEnd>(*renderer, fo);
      sh.fe->SetScaledTargets(a1_scale != 1.f);   // S105 A1: the stream-free host does the same from render_scale
      rf::c::SetOfflineLoad(true);  // lane C's arena seam: an offline tool waits for the arena and loads it on this thread (live_scene_draw's mirror path does the same)
      sh.offline_source = std::make_unique<offline::Assets>();  // --assets offline: tools\scene\offline_assets.h (the viewer's loader seam: the pictures must equal GameFilesSource's)
      rf::AssetSource& chosen = use_offline ? static_cast<rf::AssetSource&>(*sh.offline_source) : static_cast<rf::AssetSource&>(sh.source);
      sh.cache = std::make_unique<rf::ModelCache>(*sh.fe, chosen, true);
      if (sh.serve && !std::getenv("WWE_RIG_PLANT_NO_KEEP_CPU")) sh.cache->KeepWrestlerCpu(true);   // S103 PS1 F2: a --no-hooks job may come first; a later hooks job needs the CPU models of the wrestlers it finds cached (frame_hooks.cpp turns it on only when ITS job has maps)
    }
    rdr_scene::g_rig_page_per_caller = std::getenv("WWE_RDR_PAGE_PER_CALLER") != nullptr;   // S103 UP: the rig's stand-in for the +dev switch wwe2k14_rdr_page_per_caller (every caller sends into the shared image)
    rdr_scene::g_plant_page_no_rejoin = std::getenv("WWE_RDR_PLANT_PAGE_NO_REJOIN") != nullptr;   // S103 UP twin: a model stays in the page group it joined first
    {   // S103 UPL: U2 / U3 arms of up_check.py. The game reads cvars; the rig reads env.
      const char* sh = std::getenv("WWE_RDR_SCREEN_SHARE");
      rdr_scene::g_screen_share.store(!(sh && std::string(sh) == "0"), std::memory_order_relaxed);   // "before" arm = 0  // RIG-ONLY: the rig reads env; the stream-free game start stores it from the cvar wwe2k14_rdr_screen_share (start_settings.cpp)
      rdr_scene::g_plant_screen_reupload = std::getenv("WWE_RDR_PLANT_SCREEN_REUPLOAD") != nullptr;   // planted twin: every screen re-sent every frame
      {   // S103 PVR: the posed-vertex ring (wwe2k14_rdr_posed_ring): WWE_RDR_POSED_RING=0 is the "before" arm (staged copy per mesh); WWE_RDR_PLANT_POSED_RING = 1 (offset one vertex off) / 2 (stale ring offset)
        const char* pr = std::getenv("WWE_RDR_POSED_RING");   // RIG-ONLY: the rig reads env; the stream-free game start stores it from the cvar wwe2k14_rdr_posed_ring (start_settings.cpp)
        rdr_scene::g_posed_ring.store(!(pr && std::string(pr) == "0"), std::memory_order_relaxed);
        const char* pp = std::getenv("WWE_RDR_PLANT_POSED_RING");
        rdr_scene::g_plant_posed_ring.store(pp ? std::atoi(pp) : 0, std::memory_order_relaxed);
        const char* rg = std::getenv("WWE_RDR_RING_GPU_SKIN");   // RIG-ONLY: S103 RGP (the rig reads env; the stream-free game start stores it from the cvar wwe2k14_rdr_ring_gpu_skin): 0 = the CPU-posed ring (the "before" arm)
        rdr_scene::g_ring_gpu_skin.store(!(rg && std::string(rg) == "0"), std::memory_order_relaxed);
        const char* pd = std::getenv("WWE_RDR_POSED_DIRECT");   // S103 PF2 (RIG-ONLY: the rig reads env; the stream-free game start stores it from the cvar wwe2k14_rdr_posed_direct): 0 = the ring path on a unified device
        rdr_scene::g_posed_direct.store(!(pd && std::string(pd) == "0"), std::memory_order_relaxed);  // RIG-ONLY: the rig reads env
        const char* pf = std::getenv("WWE_RDR_POSED_DIRECT_FORCE");   // RIG-ONLY: the direct path on this (discrete) card too
        rdr_scene::g_posed_direct_force.store(pf && std::string(pf) == "1", std::memory_order_relaxed);
        const char* ppd = std::getenv("WWE_RDR_PLANT_POSED_DIRECT");   // RIG-ONLY: the PF2 planted twins 5..7 (front_end.h)
        rdr_scene::g_plant_posed_direct.store(ppd ? std::atoi(ppd) : 0, std::memory_order_relaxed);
      }
      const char* ub = std::getenv("WWE_RDR_UPLOAD_BATCH");
      rdr::g_upload_batch.store(!(ub && std::string(ub) == "0"), std::memory_order_relaxed);   // the shipped stream-free default is ON; "before" arm = 0  // RIG-ONLY: the rig reads env; the stream-free game start stores it from the cvar wwe2k14_rdr_upload_batch (start_settings.cpp)
      static RigScreenFeed rig_feed;
      if (const char* sf = std::getenv("WWE_RDR_RIG_SCREEN_FEED")) {   // "same" = every screen shows the same picture of a serial (one movie on all of them); "distinct" = a picture per screen key
        rig_feed.distinct = std::string(sf) == "distinct";
        rig_feed.pattern = std::string(sf) == "pattern";   // WGS: the test pattern per key (arena_sheet.py)
        rig_feed.flat = std::string(sf) == "flat" || flat_feed;   // WGS DIM1: one solid colour per key (env, or this job's --flat-feed over a pattern process: a serve process keeps one env)
        if (const char* cn = std::getenv("WWE_RDR_RIG_FEED_CACHE")) rig_feed.cache_n = size_t(std::atoi(cn));   // 0 = one buffer for every call
        rf::c::SetScreenFeed(&rig_feed);
      }
      rig_feed_ptr = &rig_feed;
    }
    rdr_scene::g_plant_page_per_model = std::getenv("WWE_RDR_PLANT_PAGE_PER_MODEL") != nullptr;   // S103 UP twin: the old per-model page copies (up_check.py; the game reads no env). Process-level: a served process keeps the arena it built
    rdr_scene::live::g_plant_lights_key_old = std::getenv("WWE_RDR_PLANT_LIGHTS_KEY_OLD") != nullptr;   // S103: the Q5 twin, read per job by the rig (the game reads no env)
    rdr_scene::sampler_table::g_plant_old_samplers = std::getenv("WWE_RDR_PLANT_OLD_SAMPLERS") != nullptr;   // S103 B1: the old sampler rules + fixed 8x (the rig sets it, the game reads no env)
    if (sh.serve && !std::getenv("WWE_RIG_PLANT_NO_RESET")) rf::c::ResetForJob();   // S103 PS1 F1: the arena's unions / debounce are per job (a fresh process starts empty); the built set stays when the job's signature matches
    FrontEnd& fe = *sh.fe;
    // WGS PAR1: a served job equals a one-shot: the frame grab (key 800's picture) never carries over from an earlier job (a kept grab nested the earlier jobs' frames in the quad).
    // --prime-grab KEY (TEST ONLY): the grab is KEY's pattern (or flat, with --flat-feed) picture, sampled through the grab's own binding, no seed frame
    fe.DropGrabForTest();
    if (prime_grab_key) {
      std::vector<uint8_t> pic(size_t(256) * 128 * 4);
      RigScreenFeed::DrawPattern(prime_grab_key, 256, 128, pic.data(), flat_feed);
      fe.PrimeGrabForTest(pic.data(), 256, 128);
    } else {
      fe.PrimeGrabForTest(nullptr, 0, 0);
    }
    fe.SetMatProbe(true);   // S103 RML (rig only): the ring mat's resolved constant blocks, printed below (rml_check.py)
    rf::ModelCache& cache = *sh.cache;
    const uint64_t uploads_base = cache.uploads();   // S103 PS1: "models made" is counted per job (a serve process keeps the cache between jobs)
    if (skip_in_place) rf::crowd::SetSkipInPlace(fe, true);   // S103 SG2: the live drawer's rule
    if (async_crowd) {   // S103 D2: the live path's crowd (bodies on a worker): count the builds that were a MISS (the live caller keeps the game's frame), then it must become whole
      rf::crowd::Attach(fe, true);
      uint32_t build_n = 0, miss_builds = 0, first_misses = 0;
      for (; build_n < 3000; ++build_n) {
        SceneFrame fa;
        std::vector<rf::ClassReport> ra;
        if (!rf::BuildFrame(t, cache, have_mask ? mask : rf::kAllFamilies, w, h, fa, ra, err)) { std::fprintf(stderr, "frame_rig: FAIL async build: %s\n", err.c_str()); rc = 3; break; }
        if (cache.misses_this_frame() == 0 && rf::TotalHoles(ra) == 0) break;
        if (!miss_builds++) first_misses = cache.misses_this_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      std::snprintf(line, sizeof line, "async crowd: first build missed %u, %u builds were a miss before the frame became whole (build %u)\n", first_misses, miss_builds, build_n);
      report += line;
    }
    std::map<std::string, std::vector<uint8_t>> panel_store;   // S103 PS1: this job's panels (RGBA8, w * h) for the metrics
    struct Run { std::string name; uint32_t mask; };
    std::vector<Run> runs;
    if (have_mask) { char n[32]; std::snprintf(n, sizeof n, "mask%u", mask); runs.push_back({n, mask}); }
    else runs.push_back({"whole", rf::kAllFamilies});
    if (per_family && !have_mask && lists_n <= 0) for (uint32_t f = 0; f < uint32_t(rf::Family::Count); ++f) runs.push_back({rf::FamilyName(rf::Family(f)), 1u << f});
    FrameHooksOptions ho;
    ho.maps = ho.particles = !no_hooks && (lists_n <= 0 || lists_hooks);
    ho.order = see_order;
    ho.particles_skip_unnamed = false;   // CAWH: the rig's fixture flare is a white stand-in by design
    ho.maps_fit.force_fit = maps_fit_force;   // S103 E3
    if (plane_told_path.empty()) if (const char* e = std::getenv("WWE_RDR_PLANE_TOLD")) plane_told_path = e;   // S103 L9: the same fixture by environment (feature_diff.py --env)
    if (!plane_told_path.empty()) {   // S103 L9: a fixture (labelled in the report), the same keys as maps_draw --told
      ho.maps_input_patch = [plane_told_path](MapsInput& in) {
        std::ifstream f(plane_told_path);
        std::string ln;
        PlaneFrame pf;
        bool any = false;
        E2Input& e2 = in.e2;
        while (std::getline(f, ln)) {
          std::istringstream ss(ln);
          std::string key;
          ss >> key;
          std::vector<float> v;
          float x;
          while (ss >> x) v.push_back(x);
          if (key == "plane_eye" && v.size() >= 3) { e2.plane_eye_told = true; for (size_t k = 0; k < 4; ++k) e2.plane_eye[k] = k < v.size() ? v[k] : 1.f; }
          else if (key == "plane_pos" && v.size() >= 3) { if (!e2.plane_pos_from_record) { for (size_t k = 0; k < 4 && k < v.size(); ++k) e2.plane_pos[k] = v[k]; e2.have_plane_pos = true; } any = true; }
          else if (key == "plane_fade" && v.size() >= 10) { std::memcpy(pf.fade, v.data(), sizeof pf.fade); pf.have |= kPlaneHaveFade; any = true; }
          else if (key == "plane_fit" && v.size() >= 16) { std::memcpy(pf.fit_matrix, v.data(), sizeof pf.fit_matrix); pf.have |= kPlaneHaveFit; }
          else if (key == "plane_camera" && v.size() >= 16) { std::memcpy(pf.camera, v.data(), sizeof pf.camera); pf.have |= kPlaneHaveCamera; }
          else if (key == "plane_normal" && v.size() >= 4) { std::memcpy(pf.plane_normal, v.data(), sizeof pf.plane_normal); pf.have |= kPlaneHaveNormal; }
        }
        if (any && pf.have == kPlaneHaveFade + kPlaneHaveFit + kPlaneHaveCamera) pf.have |= kPlaneHaveNormal;
        if (any) e2.plane_frame = pf, e2.have_plane = pf.have == kPlaneHaveAll, e2.plane_told = true;
      };
    }
    ho.maps_input_repeatable = true;           // S103 E3: RenderToPixels may Record twice per attach (the seed frame)
    if (told_key.size() == 3) ho.maps_fit.have_told_key = true, std::memcpy(ho.maps_fit.told_key, told_key.data(), sizeof ho.maps_fit.told_key);
    { std::string ferr; if (!MapsFaults::Parse(maps_fault_list, ho.maps_faults, ferr)) { std::fprintf(stderr, "frame_rig: %s\n", ferr.c_str()); return 1; } }
    if (std::getenv("WWE_RDR_PARTICLE_FIXTURE") || !t.particles.empty()) { rdr_scene::live::particles::NameIndex sprite_idx; std::string se; if (!BuildParticleSprites(game, sprite_idx, se)) report += "particle sprites: " + se + "\n"; }   // S103 F5: the decoded sprites, built once here (live: on the namer worker)
    FrameHooks hooks(*renderer, cache, ho);   // (made before the first Get: it turns the wrestlers' CPU copies on)
    if (hooks.maps()) hooks.maps()->SetBlockingLoads(true), hooks.maps()->SetGameRoot(game);   // S103 B2F: the rope proxy's arena pac
    SeeOrder used = see_order;
    SeeOrderFromEnv(used);                    // the front-end reads WWE_RDR_ORDER over the frame's own choice: say what it will ACTUALLY use
    std::snprintf(line, sizeof line, "see_order requested %s used %s hooks %s\n", see_order == SeeOrder::Game ? "game" : "distance", used == SeeOrder::Game ? "game" : "distance", ho.maps ? "maps+particles" : "off");
    report += line;
    for (const Run& r : runs) {
      SceneFrame f;
      std::vector<rf::ClassReport> reports;
      const uint64_t up_pc0 = fe.texture_update_count(rdr_scene::TexTag::Page), up_pb0 = fe.texture_update_bytes(rdr_scene::TexTag::Page), up_sc0 = fe.texture_update_count(rdr_scene::TexTag::Screen),
                     up_sb0 = fe.texture_update_bytes(rdr_scene::TexTag::Screen), up_oc0 = fe.texture_update_count(rdr_scene::TexTag::Other), up_ob0 = fe.texture_update_bytes(rdr_scene::TexTag::Other),
                     up_sk0 = fe.page_skips_key(), up_sh0 = fe.page_skips_hash();   // S103 UP
      RigInjectScreenRows(t);
      if (rig_feed_ptr) rig_feed_ptr->SizesFrom(t);   // S103 UPL: the rig feed's picture sizes are the record's rows' (the texture's own size)
      const rdr::Renderer::UploadStats us0 = renderer->LastUploadStats();
      const uint64_t up_rf0 = us0.total_frames, up_ri0 = us0.total_images, up_rb0 = us0.total_buffers, up_rr0 = us0.total_barriers, up_ry0 = us0.total_bytes,
                     up_sp0 = fe.screen_pics(), up_ss0 = fe.screen_shared(), up_sm0 = fe.screen_moved();   // S103 UPL
      if (!rf::BuildFrame(t, cache, r.mask, w, h, f, reports, err)) { std::fprintf(stderr, "frame_rig: FAIL %s: %s\n", r.name.c_str(), err.c_str()); rc = 3; break; }
      if (scene_word >= 0) f.phase_word = scene_word;   // S103 MBC: the phase row the offline record lacks
      if (participants >= 0) f.participants = participants;
      if (plane_off) f.plane_game_off = 1;   // S103 RML (RIG-ONLY): the offline record has no module word; the check tells the rig the game built no plane module
      if (!no_hooks && (lists_n <= 0 || lists_hooks) && (r.name == "whole" || have_mask)) {
        if (prop_fixture.size() == 4) InjectMapBFixture(cache, f, prop_fixture, false, 0, report);
        if (gm_fixture.size() == 4) InjectMapBFixture(cache, f, gm_fixture, true, 1, report);
      }
      if (r.name == "whole" || have_mask) {   // S103 UP: what this frame's build uploaded, by caller (up_check.py reads this line)
        uint32_t pcar = 0, pon = 0, prr = 0;
        rf::c::PageCensus(fe, pcar, pon, prr);
        std::snprintf(line, sizeof line, "up_counter %s page_copies %llu page_bytes %llu screen_copies %llu screen_bytes %llu other_copies %llu other_bytes %llu page_skip_key %llu page_skip_hash %llu page_models %u page_groups %u page_carriers %u page_on_group %u ring_rest_page %u\n", r.name.c_str(),
                      (unsigned long long)(fe.texture_update_count(rdr_scene::TexTag::Page) - up_pc0), (unsigned long long)(fe.texture_update_bytes(rdr_scene::TexTag::Page) - up_pb0),
                      (unsigned long long)(fe.texture_update_count(rdr_scene::TexTag::Screen) - up_sc0), (unsigned long long)(fe.texture_update_bytes(rdr_scene::TexTag::Screen) - up_sb0),
                      (unsigned long long)(fe.texture_update_count(rdr_scene::TexTag::Other) - up_oc0), (unsigned long long)(fe.texture_update_bytes(rdr_scene::TexTag::Other) - up_ob0),
                      (unsigned long long)(fe.page_skips_key() - up_sk0), (unsigned long long)(fe.page_skips_hash() - up_sh0), fe.page_share_models(), fe.page_groups(), pcar, pon, prr);
        report += line;
      }
      // The frame's see order and stage hooks, attached by the SAME function the live drawer calls (frame_hooks.h): whole / mask panels only; a per-family panel has no hooks.
      const bool hooks_on = !no_hooks && (lists_n <= 0 || lists_hooks) && (r.name == "whole" || have_mask);
      PostParams post_store;   // S103 P2: the frame's post from the record's post row, as LiveSceneDraw::BuildPost does (whole / mask panels; WWE_RDR_NO_SCENE_POST=1 off;
      post_from_record::Info post_info;   // WWE_RDR_PLANT_POST=dof_off|bloom_off|glare_off|all_off plants a twin that lacks that pass)
      if ((r.name == "whole" || have_mask) && !std::getenv("WWE_RDR_NO_SCENE_POST")) {
        if (post_from_record::Build(t, f.camera, w, h, post_store, post_info)) {
          if (const char* pl = std::getenv("WWE_RDR_PLANT_POST")) {
            const std::string ps = pl;
            if (ps == "dof_off" || ps == "all_off") post_store.dof = false;
            if (ps == "bloom_off" || ps == "all_off") post_store.bloom = false, post_store.glare = false, post_store.dead_bloom = true, post_store.dead_glare = true;
            if (ps == "fog_off" || ps == "all_off") post_store.fog = false;
            if (ps == "glare_off")post_store.glare = false, post_store.dead_glare = true;
          }
          f.post = &post_store;
        }
        if (f.post) { char pb[400]; std::snprintf(pb, sizeof pb, "post_rig_values RGB %.4f %.4f %.4f Alpha %.4f ZAdj %.4f DOFParam %.4f %.4f %.4f %.4f DOFParam2 %.4f %.4f q %.7f nq %.5f depth_from_viewer %d\n", post_store.RGB[0], post_store.RGB[1], post_store.RGB[2], post_store.Alpha[0], post_store.ZAdj[0], post_store.DOFParam[0], post_store.DOFParam[1], post_store.DOFParam[2], post_store.DOFParam[3], post_store.DOFParam2[0], post_store.DOFParam2[1], post_info.q, post_info.nq, int(post_store.depth_from_viewer)); report += pb; }
        report += "post_rig " + post_from_record::Describe(post_info) + (f.post ? "" : std::string(" (no post: ") + post_info.why + ")") + "\n";
      }
      if (hooks_on) InstallParticleFixture(t, f, report);   // S103 F5: a labelled fixture flare (WWE_RDR_PARTICLE_FIXTURE), none by default
      if (hooks_on) AttachFrameHooks(f, t, hooks);
      else f.see_order = see_order;
      if (r.name == "whole" || have_mask) {  // the camera the record gave vs the stream's (row-vector view x the projection BuildFrame built): both signs and the planes in one number
        if ((t.camera.flags & kCamProjFromFov) && stream_vp.size() == 16) {
          float view[16], m[16], maxd = 0, maxv = 0;
          std::memcpy(view, f.camera.view.m, sizeof view);
          for (int k = 4; k < 8; ++k) view[k] = -view[k];   // BuildFrame mirrored a Live record's view into viewer space: back to the game's
          Mul4(view, f.camera.proj.m, m);
          for (int k = 0; k < 16; ++k) maxd = std::max(maxd, std::fabs(m[k] - stream_vp[k])), maxv = std::max(maxv, std::fabs(stream_vp[k]));
          std::snprintf(line, sizeof line, "viewproj_compare record camera (fov %.3f near %.3f far %.1f) x BuildFrame projection vs the stream's: max |diff| %.4g, relative %.3e\n", cam_fov, cam_near, cam_far, maxd, maxd / std::max(1e-9f, maxv));
          report += line;
        } else if (stream_vp.size() == 16) report += "viewproj_compare: not run (the camera is the borrowed ViewProj, not a record camera)\n";
      }
      fe.ResetCoverage();
      if (!depth_out.empty()) fe.KeepDepthForTest();   // SCU: the scene depth stored, read back below (TEST ONLY)
      static DrawLists flip_lists;   // SCU --flip-check: the kept draw lists on, as the live path has them, so a list kept from the frame before the flip is exercised
      if (stage_cull_flip && lists_n <= 0) fe.SetDrawLists(&flip_lists);
      std::vector<uint8_t> rgba;
      uint32_t rw_ = w, rh_ = h;   // S105 A1: the read-back's real size (the logical size x the render scale)
      if (lists_n > 0) {
        // lane G's lists check (SCENE_PACING.md 4): the frame drawn N times through the kept draw lists, each repeat with the time, the camera, a light colour and one object's alpha moved
        // a little, and each time once more WITHOUT the lists: the two pictures must be byte equal. The picture written is the last lists draw.
        DrawLists lists_obj;
        uint64_t rml_mat_compared = 0, rml_mat_stale = 0;   // S103 RML (F1)
        if (lists_plant) lists_obj.PlantIgnore(kStWorld | kStPalette | kStTime);
        if (lists_plant_spec) PlantFieldMask("SpecularCol", kStMaterial);
        fe.SetDrawLists(&lists_obj);
        const SceneFrame base = f;
        for (long i = 0; i < lists_n && rc == 0; ++i) {
          f = base;
          f.time_s += 0.0167 * double(i);
          if (i % 2 == 1) for (Object& o : f.objects) o.world.m[12] += 0.02f * float(i);  // every other repeat the objects move: only their world fields may be rewritten
          fe.SetPropsNudge(0.01f * float(i));   // S103 RML (F1): the props map's matrix moves every repeat; the mat's probe of the lists draw must equal the no-lists draw's
          if (i > 0) {
            f.camera.view.m[12] += 0.01f * float(i), f.camera.eye[0] += 0.05f * float(i);
            assets::LightSet rig = assets::DefaultRig();
            for (int l = 0; l < 4; ++l) rig.col[l][0] += 0.03f * float(i), rig.col[l][1] += 0.015f * float(i);
            f.light_sets.assign(1, rig);
            f.model_lights.clear();
            std::set<uint32_t> seen_assets;
            for (Object& o : f.objects) {
              o.light_set = 0;
              if (seen_assets.insert(o.asset).second) f.model_lights.push_back({o.asset, {rig}, {}});
            }
            if (!f.objects.empty()) f.objects[size_t(i * 7) % f.objects.size()].tint[3] = 1.f - 0.07f * float(i);
          }
          std::vector<uint8_t> plain;
          fe.SetDrawLists(&lists_obj);
          fe.ClearMatProbes();
          if (!fe.RenderToPixels(f, rgba, err)) { std::fprintf(stderr, "frame_rig: FAIL draw: %s\n", err.c_str()); rc = 3; break; }
          const std::vector<FrontEnd::MatProbe> probes_lists = fe.mat_probes();
          fe.SetDrawLists(nullptr);
          fe.ClearMatProbes();
          if (!fe.RenderToPixels(f, plain, err)) { std::fprintf(stderr, "frame_rig: FAIL draw: %s\n", err.c_str()); rc = 3; break; }
          {   // S103 RML (F1): the ring mat's resolved light-pass constants, kept lists vs plain, every repeat
            const std::vector<FrontEnd::MatProbe>& probes_plain = fe.mat_probes();
            for (size_t pi2 = 0; pi2 < probes_lists.size() && pi2 < probes_plain.size(); ++pi2) {
              if (!probes_lists[pi2].props_reader) continue;
              ++rml_mat_compared;
              for (size_t k = 0; k < probes_lists[pi2].vs_f.size() && k < probes_plain[pi2].vs_f.size(); ++k)
                if (probes_lists[pi2].vs_f[k].v != probes_plain[pi2].vs_f[k].v) { ++rml_mat_stale; std::fprintf(stderr, "[frame_rig] RML repeat %ld: the mat's %s of the lists draw is stale\n", i, probes_lists[pi2].vs_f[k].name.c_str()); break; }
            }
          }
          if (plain != rgba) { ++lists_bad; std::fprintf(stderr, "[frame_rig] LISTS repeat %ld: the picture differs from the no-lists draw\n", i); }
        }
        fe.SetDrawLists(nullptr);
        fe.SetPropsNudge(0.f);
        std::snprintf(line, sizeof line, "  rml_lists mat_compared %llu mat_stale %llu\n", (unsigned long long)rml_mat_compared, (unsigned long long)rml_mat_stale);
        report += line;
        std::snprintf(line, sizeof line, "%s\n[frame_rig] lists: %ld repeats, %llu differ from the no-lists draw\n", lists_obj.Census().c_str(), lists_n, (unsigned long long)lists_bad);
        std::fputs(line, stderr);
        report += line;
        if (rc != 0) break;
      } else if (!fe.RenderToPixels(f, rgba, err, &rw_, &rh_)) { std::fprintf(stderr, "frame_rig: FAIL draw %s: %s\n", r.name.c_str(), err.c_str()); rc = 3; break; }
      if (a1_scale != 1.f && (r.name == "whole" || have_mask)) {   // S105 A1: the sizes of the scene targets and of every live image with its kUsageScaled flag (a1_check.py reads these lines)
        uint32_t cw = 0, ch = 0, dw = 0, dh = 0;
        bool cs = false, ds = false;
        fe.TargetExtent(fe.scene_colour_image(), cw, ch, cs), fe.TargetExtent(fe.scene_depth_image(), dw, dh, ds);
        std::snprintf(line, sizeof line, "a1-sizes: scale %.4f logical %ux%u picture %ux%u colour %ux%u scaled %d depth %ux%u scaled %d\n", a1_scale, w, h, rw_, rh_, cw, ch, int(cs), dw, dh, int(ds));
        report += line;
        for (const rdr::Renderer::LiveImageInfo& li : renderer->LiveImageInfos()) {
          std::snprintf(line, sizeof line, "a1-image: id %u %ux%u format %u scaled %d\n", li.id, li.width, li.height, li.format, int(li.scaled));
          report += line;
        }
      }
      if (!depth_out.empty() && (r.name == "whole" || have_mask)) {   // SCU (stage_cull_proof.py): the panel's scene depth, raw 4 bytes a texel, rows top to bottom as the picture
        std::vector<uint8_t> raw;
        uint32_t dw = 0, dh = 0;
        if (!fe.ReadDepthForTest(raw, dw, dh) || dw != w || dh != h) { std::fprintf(stderr, "frame_rig: FAIL --depth-out: the scene depth could not be read back\n"); rc = 3; break; }
        FILE* df = std::fopen(depth_out.c_str(), "wb");
        if (!df || std::fwrite(raw.data(), 1, raw.size(), df) != raw.size()) { if (df) std::fclose(df); std::fprintf(stderr, "frame_rig: FAIL cannot write %s\n", depth_out.c_str()); rc = 3; break; }
        std::fclose(df);
        std::snprintf(line, sizeof line, "depth_out %s %ux%u bytes %zu\n", depth_out.c_str(), dw, dh, raw.size());
        report += line;
      }
      if (r.name == "whole" || have_mask) {   // S103 UPL: what the renders of this panel recorded in ApplyUploads (calls, barriers, bytes), and the screen counters (up_check.py reads both lines)
        const rdr::Renderer::UploadStats us = renderer->LastUploadStats();
        std::snprintf(line, sizeof line, "up_render %s frames %llu images %llu buffers %llu barriers %llu bytes %llu batched %d\n", r.name.c_str(), (unsigned long long)(us.total_frames - up_rf0), (unsigned long long)(us.total_images - up_ri0),
                      (unsigned long long)(us.total_buffers - up_rb0), (unsigned long long)(us.total_barriers - up_rr0), (unsigned long long)(us.total_bytes - up_ry0), us.batched ? 1 : 0);
        report += line;
        std::snprintf(line, sizeof line, "pvr_render %s buffers %llu by_producer posed %llu cloth %llu other %llu ring_pushes %llu ring_bytes %llu ring_overflow %llu\n", r.name.c_str(), (unsigned long long)(us.total_buffers - up_rb0),
                      (unsigned long long)us.total_by_producer[1], (unsigned long long)us.total_by_producer[2], (unsigned long long)us.total_by_producer[0], (unsigned long long)rdr_scene::g_posed_ring_pushes.load(), (unsigned long long)rdr_scene::g_posed_ring_bytes.load(),
                      (unsigned long long)rdr_scene::g_posed_ring_overflow.load());   // S103 PVR: cumulative since the renderer started (pvr_check.py differences them); `buffers` = this panel's
        report += line;
        std::snprintf(line, sizeof line, "up_screens %s pics %llu shared %llu moved %llu images %llu rows %u sized %u\n", r.name.c_str(), (unsigned long long)(fe.screen_pics() - up_sp0), (unsigned long long)(fe.screen_shared() - up_ss0),
                      (unsigned long long)(fe.screen_moved() - up_sm0), (unsigned long long)fe.screen_images(), rig_feed_ptr ? rig_feed_ptr->rows : 0u, rig_feed_ptr ? rig_feed_ptr->sized : 0u);
        report += line;
      }
      const bool family_panel = per_family && !have_mask && r.name != "whole";
      if (!(no_family_png && family_panel) && !scene_png::WritePngRgb(prefix + "_" + r.name + ".png", rgba, rw_, rh_)) { std::fprintf(stderr, "frame_rig: FAIL cannot write\n"); rc = 3; break; }
      if (!metric_refs.empty() || !cov_store_key.empty()) panel_store[r.name] = rgba;   // S103 PS1: kept for the metrics below (the repeat builds overwrite rgba)
      // covered pixels: not the clear colour (an empty panel says so)
      const uint8_t cr = uint8_t(std::lround(f.clear[0] * 255)), cg = uint8_t(std::lround(f.clear[1] * 255)), cb = uint8_t(std::lround(f.clear[2] * 255));
      uint64_t covered = 0;
      for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        if (std::abs(int(rgba[i]) - cr) > 2 || std::abs(int(rgba[i + 1]) - cg) > 2 || std::abs(int(rgba[i + 2]) - cb) > 2) ++covered;
      // distinct 15-bit colours: a textured picture has thousands, a flat-grey one (textures off) a few hundred (the rig's texture check)
      std::vector<uint8_t> seen(32768, 0);
      uint32_t colours = 0;
      for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const uint32_t k = (uint32_t(rgba[i] >> 3) << 10) | (uint32_t(rgba[i + 1] >> 3) << 5) | uint32_t(rgba[i + 2] >> 3);
        if (!seen[k]) seen[k] = 1, ++colours;
      }
      uint32_t drawn = 0;
      for (const rf::ClassReport& c : reports) drawn += c.drawn;
      std::snprintf(line, sizeof line, "panel %s families 0x%x objects_drawn %u front_end_objects %zu covered_pixels %llu of %llu colours %u validation_errors %llu\n", r.name.c_str(), r.mask, drawn,
                    f.objects.size(), (unsigned long long)covered, (unsigned long long)w * h, colours, (unsigned long long)fe.validation_errors());
      report += line;
      if (covered == 0) report += "panel " + r.name + " EMPTY: nothing was drawn (every pixel is the clear colour)\n";
      for (const rf::ClassReport& c : reports) {
        std::snprintf(line, sizeof line, "  family %s objects %u drawn %u skipped %u holes %u why %s\n", rf::FamilyName(c.family), c.objects, c.drawn, c.skipped, c.holes, c.why.empty() ? "-" : c.why.c_str());
        report += line;
        if (c.family == rf::Family::Wrestlers) report += "  " + rf::WrestlerStatsText() + "\n";   // S103 NRBF: the pieces' census ("pieces drawn by their owner's body N") for nrb_check
      }
      if (hooks_on) {
        // what the hooks did (counters only), and the maps as pictures
        report += "  hook " + hooks.maps()->CensusLine() + "\n";
        report += "  hook " + hooks.particles()->CensusLine() + "\n";
        {   // S103 PMS: the floor bound to the props map in a real front end (pms_check.py --rig reads this line)
          const MapImage* pm = hooks.maps()->maps().Find(MapRole::PropsMap);
          std::snprintf(line, sizeof line, "  pms_floor props_map %d props_bound %llu d1_shadow_nomap %llu sites %llu %llu %llu %llu %llu %llu slot12_is_props_image %d slot12_is_shadow_image %d props_holes %s\n", int(pm != nullptr), (unsigned long long)fe.props_bound(),
                        (unsigned long long)fe.d1_shadow_nomap(), (unsigned long long)fe.pms_site(0), (unsigned long long)fe.pms_site(1), (unsigned long long)fe.pms_site(2), (unsigned long long)fe.pms_site(3), (unsigned long long)fe.pms_site(4),
                        (unsigned long long)fe.pms_site(5), int(pm && fe.pms_slot12() == pm->image), int(fe.pms_slot12() != rdr::kNoImage && fe.pms_slot12() == hooks.maps()->shadow_image()), hooks.maps()->counts().props_skip[0] ? hooks.maps()->counts().props_skip : "none");
          report += line;
        }
        {   // S103 RML (rml_check.py): the ring mat's pass + its RESOLVED constants (every draw bound to the props / plane pass; a field = name, vec4 count, floats), the census counters, the planted twins' state
          std::snprintf(line, sizeof line, "  rml_census props_mat_bound %llu plane_bound %llu plane_wanted_plain %llu plane_game_off %d probes %zu viewer_space %d\n", (unsigned long long)fe.props_mat_bound(), (unsigned long long)fe.plane_bound(), (unsigned long long)fe.plane_wanted_plain(), int(rdr_scene::PlaneGameOff()), fe.mat_probes().size(), int(f.viewer_space));
          report += line;
          size_t pi = 0;
          for (const FrontEnd::MatProbe& mp : fe.mat_probes()) {
            {
              const MapImage* ppm = hooks.maps()->maps().Find(MapRole::PropsMap);
              report += std::string("  rml_draw ") + std::to_string(pi) + " kind " + (mp.props_reader ? "props" : mp.plane ? "plane" : "floor") + " vs " + mp.vs + " ps " + mp.ps + " slot12_is_props_image " + std::to_string(int(mp.props_reader && ppm && mp.slot12 == ppm->image)) +
                        " slot12_is_shadow_image " + std::to_string(int(mp.props_reader && mp.slot12 != rdr::kNoImage && mp.slot12 == hooks.maps()->shadow_image())) + "\n";
            }
            for (int side = 0; side < 2; ++side)
              for (const FrontEnd::MatProbeField& f : side == 0 ? mp.vs_f : mp.ps_f) {
                std::string l = "  rml_field " + std::to_string(pi) + (side == 0 ? " vs " : " ps ") + f.name + " " + std::to_string(f.vec4s);
                for (float x : f.v) { char b[40]; std::snprintf(b, sizeof b, " %.9g", x); l += b; }
                report += l + "\n";
              }
            ++pi;
          }
        }
        {   // S103 PM2 (pm2_check.py): the props map's floor matrix + ShadowPrm as the floor reads them, and where the first wrestler stands (circle entry 0's body points, the game's space: y is DOWN, the floor is the largest y), so the check can place a fixture prop
          const MapImage* pm = hooks.maps()->maps().Find(MapRole::PropsMap);
          const MapsInput& mi = hooks.input();
          if (pm && !mi.circles.empty()) {
            std::string ms = "  pm2_props_matrix";
            for (int k = 0; k < 16; ++k) { char b[40]; std::snprintf(b, sizeof b, " %.9g", pm->matrix[k]); ms += b; }
            std::snprintf(line, sizeof line, "\n  pm2_props_prm %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", pm->prm[0][0], pm->prm[0][1], pm->prm[0][2], pm->prm[0][3], pm->prm[1][0], pm->prm[1][1], pm->prm[1][2], pm->prm[1][3], pm->prm[2][0], pm->prm[2][1], pm->prm[2][2], pm->prm[2][3]);
            report += ms + line;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t k = 0; k < kCirclePoints; ++k)
              for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], mi.circles[0].points[k][a]), hi[a] = std::max(hi[a], mi.circles[0].points[k][a]);
            std::snprintf(line, sizeof line, "  pm2_anchor lo %.4f %.4f %.4f hi %.4f %.4f %.4f viewer_space %d\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], int(f.viewer_space));
            report += line;
          } else {
            report += "  pm2_props_none: no props map / circle entry this frame\n";
          }
        }
        const MapsCounts& mc = hooks.maps()->counts();
        std::snprintf(line, sizeof line, "  maps_made %u shadow_skip \"%s\" circles_skip \"%s\" casters_objects %u circles_drawn %u; light: %s; record has caster calls %d circle rows %d; particle batches %zu\n", mc.maps_made,
                      mc.shadow_skip, mc.circles_skip, mc.casters_objects, mc.circles_drawn, hooks.input().have_light ? "found" : hooks.input().light_why, int(hooks.input().has_caster_records), int(hooks.input().has_circle_records),
                      t.particles.batches.size());
        report += line;
        if (const MapImage* sm = hooks.maps()->maps().Find(MapRole::ShadowMap)) {   // S103 E3: the matrix + params the readers get (the check compares maps through them)
          std::string ms = "  map_shadow_matrix";
          for (int k = 0; k < 16; ++k) { char b[40]; std::snprintf(b, sizeof b, " %.9g", sm->matrix[k]); ms += b; }
          std::snprintf(line, sizeof line, "\n  map_shadow_params %.9g %.9g %.9g %.9g\n", sm->params[0], sm->params[1], sm->params[2], sm->params[3]);
          report += ms + line;
          const MapsCounts& fc = hooks.maps()->counts();
          std::snprintf(line, sizeof line, "  map_fit fitted %d key_told %d center %.4f %.4f %.4f lo %.4f %.4f %.4f hi %.4f %.4f %.4f samples %u holes %u\n", int(fc.shadow_fitted), int(hooks.input().fit_key_told), fc.fit_center[0], fc.fit_center[1],
                        fc.fit_center[2], fc.fit_lo[0], fc.fit_lo[1], fc.fit_lo[2], fc.fit_hi[0], fc.fit_hi[1], fc.fit_hi[2], fc.fit_samples, fc.holes);
          report += line;
        } else {
          std::snprintf(line, sizeof line, "  map_shadow_hole \"%s\" holes %u\n", hooks.maps()->counts().shadow_skip, hooks.maps()->counts().holes);
          report += line;
        }
        if (const MapImage* rm = hooks.maps()->maps().Find(MapRole::RimMap)) {   // S103 E3: the rim map's depth range (compared with the stream's RimLightDepth)
          std::snprintf(line, sizeof line, "  map_rim_params %.9g %.9g %.9g %.9g\n", rm->params[0], rm->params[1], rm->params[2], rm->params[3]);
          report += line;
        }
        const struct { const char* name; rdr::ImageId id; } outs[] = {{"map_shadow", hooks.maps()->shadow_image()}, {"map_circles", hooks.maps()->circles_image()}, {"map_plane", hooks.maps()->plane_map_image()}, {"map_plane_mask", hooks.maps()->plane_mask_image()}, {"map_liquid", hooks.maps()->e2().sweat_color}};   // S103 L9
        for (const auto& o : outs) {
          uint64_t changed = 0;
          std::string note;
          const bool ok = WriteImagePng(*renderer, o.id, prefix + "_" + o.name + ".png", changed, note);
          std::snprintf(line, sizeof line, "  %s: %s%s (%llu pixels differ from the clear)\n", o.name, ok ? "" : "NOT MADE: ", note.c_str(), (unsigned long long)changed);
          report += line;
        }
      }
      if ((r.name == "whole" || have_mask) && repeat > 1 && lists_n <= 0) {
        // the live path builds the same record every frame: no frame may add a model (VRAM) once the first has been built
        const uint64_t first = cache.uploads() - uploads_base;
        for (int k = 1; k < repeat; ++k) {
          if (stage_cull_flip) rdr_scene::g_stage_cull.store(!rdr_scene::g_stage_cull.load(std::memory_order_relaxed), std::memory_order_relaxed);   // SCU: the switch flipped between two frames of one process (stage_cull_proof.py --flip-check)
          if (plant_leak) { assets::Model tiny; tiny.label = "leak"; cache.Put("leak#" + std::to_string(k), tiny); }  // the planted fault: a model per frame
          SceneFrame f2;
          std::vector<rf::ClassReport> r2;
          RigInjectScreenRows(t);
          if (rig_feed_ptr) rig_feed_ptr->SizesFrom(t);
          const rdr::Renderer::UploadStats rs0 = renderer->LastUploadStats();
          const uint64_t rp0 = fe.screen_pics(), rsh0 = fe.screen_shared(), rmv0 = fe.screen_moved(), rsc0 = fe.texture_update_count(rdr_scene::TexTag::Screen), rsb0 = fe.texture_update_bytes(rdr_scene::TexTag::Screen);
          if (!rf::BuildFrame(t, cache, r.mask, w, h, f2, r2, err) || !fe.RenderToPixels(f2, rgba, err)) { std::fprintf(stderr, "frame_rig: FAIL repeat %d: %s\n", k, err.c_str()); rc = 3; break; }
          const rdr::Renderer::UploadStats rs1 = renderer->LastUploadStats();
          std::snprintf(line, sizeof line, "up_rep %d images %llu buffers %llu barriers %llu bytes %llu screen_copies %llu screen_bytes %llu pics %llu shared %llu moved %llu hash %016llx\n", k, (unsigned long long)(rs1.total_images - rs0.total_images),
                        (unsigned long long)(rs1.total_buffers - rs0.total_buffers), (unsigned long long)(rs1.total_barriers - rs0.total_barriers), (unsigned long long)(rs1.total_bytes - rs0.total_bytes),
                        (unsigned long long)(fe.texture_update_count(rdr_scene::TexTag::Screen) - rsc0), (unsigned long long)(fe.texture_update_bytes(rdr_scene::TexTag::Screen) - rsb0), (unsigned long long)(fe.screen_pics() - rp0),
                        (unsigned long long)(fe.screen_shared() - rsh0), (unsigned long long)(fe.screen_moved() - rmv0), (unsigned long long)RigHash(rgba));
          report += line;
          std::snprintf(line, sizeof line, "pvr_rep %d buffers %llu by_producer posed %llu cloth %llu other %llu ring_pushes %llu ring_bytes %llu ring_overflow %llu hash %016llx\n", k, (unsigned long long)(rs1.total_buffers - rs0.total_buffers),
                        (unsigned long long)rs1.total_by_producer[1], (unsigned long long)rs1.total_by_producer[2], (unsigned long long)rs1.total_by_producer[0], (unsigned long long)rdr_scene::g_posed_ring_pushes.load(), (unsigned long long)rdr_scene::g_posed_ring_bytes.load(),
                        (unsigned long long)rdr_scene::g_posed_ring_overflow.load(), (unsigned long long)RigHash(rgba));   // S103 PVR
          report += line;
        }
        std::snprintf(line, sizeof line, "repeat %d: models made after the first build %llu, after the last %llu\n", repeat, (unsigned long long)first, (unsigned long long)(cache.uploads() - uploads_base));
        report += line;
      }
      if (const live::LiveCrowdCounts* cc = rf::crowd::LastCounts(fe); cc && (r.mask & (1u << uint32_t(rf::Family::Crowd))))   // S103 D2: what the crowd hook drew
        report += "  " + cc->Text() + "\n";
      if (r.name == "whole" || have_mask) report += "  coverage:\n" + fe.CoverageText();
      if (lists_n > 0) break;
    }
    if (rc == 0 && (!metric_refs.empty() || !cov_store_key.empty())) {   // S103 PS1: the proof metrics from the pictures just drawn (tools\scene\rig_metrics.h; l_metrics.py's numbers)
      const size_t px = size_t(w) * size_t(h);
      const auto ring_it = panel_store.find("ring");
      std::map<std::string, std::vector<uint8_t>> cov_own;
      for (const auto& kv : panel_store)
        if (kv.first != "whole" && kv.second.size() == px * 4) cov_own[kv.first] = rig_metrics::CoverageOf(kv.second, ring_it != panel_store.end() ? &ring_it->second : nullptr, px);
      if (!cov_store_key.empty()) sh.cov_store[cov_store_key] = cov_own;
      const std::map<std::string, std::vector<uint8_t>>* cov_ref = nullptr;
      if (!cov_from_key.empty()) {
        const auto it = sh.cov_store.find(cov_from_key);
        if (it == sh.cov_store.end()) { std::fprintf(stderr, "frame_rig: FAIL --metrics-cov-from %s: no job stored that coverage (a job with --metrics-cov-store %s runs first)\n", cov_from_key.c_str(), cov_from_key.c_str()); rc = 3; }
        else cov_ref = &it->second;
      }
      // the device: gpu (default) = rig_metrics.comp on the renderer's device; cpu = MeasureCpu; both = each, and a FAIL when they disagree. A GPU that cannot be made falls back to the CPU (said once).
      bool use_gpu = metrics_device != "cpu", both = metrics_device == "both";
      if (use_gpu && (!sh.gpu.ready() || sh.gpu_px != px)) {
        if (!sh.gpu_tried || sh.gpu_px != px) { sh.gpu_tried = true; sh.gpu_px = px; sh.gpu.Init(*renderer, px); if (!sh.gpu.ready()) std::fprintf(stderr, "[metrics] the GPU compare is not available (%s): measuring on the CPU\n", sh.gpu.why().c_str()); }
        use_gpu = sh.gpu.ready();
      }
      double gpu_ms = 0, cpu_ms = 0;
      unsigned n_meas = 0, n_bad = 0;
      const auto now = [] { return std::chrono::steady_clock::now(); };
      const auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      const auto measure = [&](const std::vector<uint8_t>& ours, const std::vector<uint8_t>& ref_rgb, const uint8_t* mask, const char* what) {
        rig_metrics::Row row;
        ++n_meas;
        if (use_gpu) {
          const auto t0 = now();
          if (!sh.gpu.Measure(ours.data(), mask, row)) { std::fprintf(stderr, "[metrics] GPU measurement failed (%s): CPU\n", sh.gpu.why().c_str()); use_gpu = false; }
          gpu_ms += ms(t0, now());
        }
        if (!use_gpu || both) {
          const auto t0 = now();
          const rig_metrics::Row c = rig_metrics::MeasureCpu(ours.data(), ref_rgb.data(), px, mask);
          cpu_ms += ms(t0, now());
          if (both && use_gpu) {   // integers must agree; the sums to 1e-9 of their size; the washed count (float on the GPU) to 2 pixels or 2e-5 of the pixels
            const auto close = [](double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); };
            const uint64_t dw = row.washed > c.washed ? row.washed - c.washed : c.washed - row.washed;
            if (row.n != c.n || !close(row.diff_sum, c.diff_sum) || !close(row.lum_ours, c.lum_ours) || !close(row.lum_ref, c.lum_ref) || dw > std::max<uint64_t>(2, uint64_t(2e-5 * double(c.n)))) {
              ++n_bad;
              std::fprintf(stderr, "frame_rig: FAIL metrics gpu vs cpu (%s): n %llu/%llu diff_sum %.9g/%.9g lum_ours %.9g/%.9g lum_ref %.9g/%.9g washed %llu/%llu\n", what, (unsigned long long)row.n, (unsigned long long)c.n, row.diff_sum, c.diff_sum,
                           row.lum_ours, c.lum_ours, row.lum_ref, c.lum_ref, (unsigned long long)row.washed, (unsigned long long)c.washed);
            }
          } else row = c;
        }
        return row;
      };
      std::string js = "{";
      for (size_t ri = 0; ri < metric_refs.size() && rc == 0; ++ri) {
        uint32_t rw = 0, rh = 0;
        const std::vector<uint8_t> ref = LoadRefRgb(metric_refs[ri].second, rw, rh);
        if (ref.empty() || rw != w || rh != h) { std::fprintf(stderr, "frame_rig: FAIL metrics reference %s: unreadable or not %ux%u (it is %ux%u)\n", metric_refs[ri].second.c_str(), w, h, rw, rh); rc = 3; break; }
        if (use_gpu) sh.gpu.SetRef(ref.data());
        js += std::string(ri ? ", " : "") + "\"" + metric_refs[ri].first + "\": {";
        bool first_key = true;
        const auto wh = panel_store.find("whole");
        if (wh != panel_store.end() && wh->second.size() == px * 4) {
          js += "\"whole\": " + rig_metrics::RowJson(measure(wh->second, ref, nullptr, "whole"), true);
          first_key = false;
        }
        for (const auto& kv : panel_store) {
          if (kv.first == "whole" || kv.second.size() != px * 4) continue;
          const auto own = cov_own.find(kv.first);
          const std::vector<uint8_t>* mask = (own != cov_own.end()) ? &own->second : nullptr;
          if (cov_ref) { const auto rm = cov_ref->find(kv.first); if (rm != cov_ref->end() && rm->second.size() == px) mask = &rm->second; }
          if (!mask) continue;
          js += std::string(first_key ? "" : ", ") + "\"" + kv.first + "\": " + rig_metrics::RowJson(measure(kv.second, ref, mask->data(), kv.first.c_str()), false);
          first_key = false;
        }
        js += "}";
      }
      if (n_bad) rc = 3;
      { char ml[200]; std::snprintf(ml, sizeof ml, "metrics: %u measurements on %s, %.2f ms gpu, %.2f ms cpu%s\n", n_meas, use_gpu ? (both ? "gpu + cpu" : "gpu") : "cpu", gpu_ms, cpu_ms, n_bad ? " (GPU and CPU DISAGREE)" : ""); report += ml; }
      js += "}\n";
      if (rc == 0 && !metrics_out.empty()) {
        FILE* mf = std::fopen(metrics_out.c_str(), "wb");
        if (mf) { std::fwrite(js.data(), 1, js.size(), mf); std::fclose(mf); }
        else { std::fprintf(stderr, "frame_rig: FAIL cannot write %s\n", metrics_out.c_str()); rc = 3; }
      }
    }
    if (const char* sig_slot = std::getenv("WWE_RDR_SIG_CHANGE_SLOT"); sig_slot && *sig_slot && rc == 0) {   // S103 UP twin of review F1 / F2 (up_check.py): a mid-run SIGNATURE change, as an entrance's moving slot makes it
      // Consecutive guest frames of the record's tables; from frame 2 on a synthetic kFxSlotWorld row names a slot the arena does not have: the set's signature changes, EnsureArena rebuilds the set after the debounce
      // (8 frames) into a NEW page group and retires the old one (released at once here: the GPU is idle in the rig). Each frame prints what it uploaded and how many page groups / models are alive.
      const uint16_t slot = uint16_t(std::atoi(sig_slot));
      SceneTables ts = t;
      const uint64_t base = t.header.guest_frame;
      for (int i = 0; i < 14 && rc == 0; ++i) {
        ts.header.guest_frame = base + 1 + uint64_t(i);
        if (i == 2) { StageFxRow row; row.kind = rdr_scene::kFxSlotWorld, row.slot = slot; ts.stage_fx.push_back(row); }
        const uint64_t pc0 = fe.texture_update_count(rdr_scene::TexTag::Page), pb0 = fe.texture_update_bytes(rdr_scene::TexTag::Page), sk0 = fe.page_skips_key(), sh0 = fe.page_skips_hash();
        SceneFrame fs;
        std::vector<rf::ClassReport> rs;
        if (!rf::BuildFrame(ts, cache, rf::kAllFamilies, w, h, fs, rs, err)) { std::fprintf(stderr, "frame_rig: FAIL sig-change frame %d: %s\n", i, err.c_str()); rc = 3; break; }
        cache.ReleaseRetired();
        for (int m : rf::c::TakeRetired()) fe.ReleaseModel(m);
        uint32_t qcar = 0, qon = 0, qrr = 0;
        rf::c::PageCensus(fe, qcar, qon, qrr);
        std::snprintf(line, sizeof line, "up_seq %d page_copies %llu page_bytes %llu page_skip_key %llu page_skip_hash %llu page_models %u page_groups %u page_carriers %u page_on_group %u ring_rest_page %u\n", i, (unsigned long long)(fe.texture_update_count(rdr_scene::TexTag::Page) - pc0),
                      (unsigned long long)(fe.texture_update_bytes(rdr_scene::TexTag::Page) - pb0), (unsigned long long)(fe.page_skips_key() - sk0), (unsigned long long)(fe.page_skips_hash() - sh0), fe.page_share_models(), fe.page_groups(), qcar, qon, qrr);
        report += line;
      }
    }
    if (probe && rc == 0 && !ParticleProbe(*renderer, fe, cache, t, mask, w, h, probe_fault, report)) rc = 1;  // S103 F
    for (size_t k = 0; k < also_rec.size() && rc == 0; ++k) {   // S103 D2: more records through the same front-end, cache and crowd (no process restart)
      SceneTables t2;
      if (!ReadRecord(also_rec[k], t2, err) || k >= also_cam.size() || k >= also_eye.size()) { std::fprintf(stderr, "frame_rig: FAIL also record %zu: %s\n", k, err.c_str()); rc = 2; break; }
      const std::vector<float> c2 = ParseFloats(also_cam[k].c_str()), e2 = ParseFloats(also_eye[k].c_str());
      if (c2.size() != 16 || e2.size() != 3) { std::fprintf(stderr, "frame_rig: FAIL also camera %zu\n", k); rc = 2; break; }
      std::memcpy(t2.camera.view.m, c2.data(), sizeof(float) * 16);
      t2.camera.proj = IdentityMat4(), t2.camera.flags = kCamViewIsViewProj;
      std::memcpy(t2.camera.eye, e2.data(), sizeof(float) * 3);
      t2.has_camera = true;
      SceneFrame f3;
      std::vector<rf::ClassReport> r3;
      std::vector<uint8_t> rgba3;
      const uint32_t m3 = have_mask ? mask : rf::kAllFamilies;
      if (rearena >= 0)   // S103 D2: the record names ANOTHER arena's pac (an arena change: the old arena's crowd is released, the new one loaded)
        for (AssetRef& as : t2.assets) {
          const std::string pac(t2.Str(as.pac));
          if (pac.rfind("pac\\bg\\bg", 0) != 0) continue;
          char nb[48];
          std::snprintf(nb, sizeof nb, "pac\\bg\\bg%02d.pac", rearena);
          as.pac = t2.AddString(nb);
        }
      uint32_t miss_builds = 0;
      bool ok3 = false;
      for (int tries = 0; tries < 3000 && !ok3; ++tries) {   // (async bodies: the frame is a miss until they are resident)
        if (!rf::BuildFrame(t2, cache, m3, w, h, f3, r3, err)) break;
        if (cache.misses_this_frame() == 0) { ok3 = true; break; }
        ++miss_builds;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!ok3 || !fe.RenderToPixels(f3, rgba3, err)) { std::fprintf(stderr, "frame_rig: FAIL also draw %zu: %s\n", k, err.c_str()); rc = 3; break; }
      if (miss_builds) { std::snprintf(line, sizeof line, "also %zu: %u builds were a miss before the frame was whole\n", k, miss_builds); report += line; }
      if (!scene_png::WritePngRgb(prefix + "_also" + std::to_string(k) + ".png", rgba3, w, h)) { rc = 3; break; }
      std::snprintf(line, sizeof line, "also %zu: %s validation_errors %llu\n", k, also_rec[k].c_str(), (unsigned long long)fe.validation_errors());
      report += line;
      if (const live::LiveCrowdCounts* cc = rf::crowd::LastCounts(fe); cc) report += "  also " + std::to_string(k) + " " + cc->Text() + "\n";
    }
    renderer->WaitIdle();
    // S103 D2: the crowd's models and images go before the front-end. A serve job ends the same way: the crowd of the NEXT job is built again (its bodies come from the game-file source's caches). Measured
    // (PS1, brock_losswin_rt frame 100, `after` then `before` on one process): a crowd kept between jobs draws a different `before` picture (38 081 crowd pixels in the top right, max 44 levels) than a fresh process
    // does: the crowd carries the previous job's look state. WWE_RIG_SERVE_KEEP_CROWD=1 keeps it anyway (faster; NOT byte-equal to one-shot runs).
    if (!sh.serve || std::getenv("WWE_RIG_SERVE_DROP_CROWD")) rf::crowd::Detach(fe);   // S103 PS1 Q5: the crowd is kept between serve jobs (LiveCrowd's per-frame light cache is keyed on the record's light + stage-look rows now); DROP_CROWD=1 = the old per-job drop
    cache.ReleaseRetired();                                  // (GPU idle) the models an arena / roster change retired
    for (int m : rf::c::TakeRetired()) fe.ReleaseModel(m);   // ... and lane C's arena sets
  }
  FILE* fp = std::fopen((prefix + "_report.txt").c_str(), "wb");
  if (fp) { std::fwrite(report.data(), 1, report.size(), fp); std::fclose(fp); }
  std::fputs(report.c_str(), stdout);
  if (rc == 0 && lists_bad) rc = 4;
  return rc;
}

// A serve process at its end: the same order a one-shot job ends in (crowd, retired models), then the Shared destructor drops the cache, front-end, renderer.
static void ServeShutdown(Shared& sh) {
  if (!sh.fe) return;
  sh.renderer->WaitIdle();
  rf::crowd::Detach(*sh.fe);
  sh.cache->ReleaseRetired();
  for (int m : rf::c::TakeRetired()) sh.fe->ReleaseModel(m);
}

// One argument line -> argv (double quotes group; no escapes: the paths and floats the rig is given hold none).
static std::vector<std::string> SplitLine(const std::string& line) {
  std::vector<std::string> v;
  std::string cur;
  bool in_q = false, have = false;
  for (char c : line) {
    if (c == '"') { in_q = !in_q, have = true; continue; }
    if (!in_q && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) { if (have) v.push_back(cur), cur.clear(), have = false; continue; }
    cur += c, have = true;
  }
  if (have) v.push_back(cur);
  return v;
}

static int ServeLoop(std::istream& in, Shared& sh) {
  std::string line;
  int job = 0, worst = 0;
  while (std::getline(in, line)) {
    std::vector<std::string> toks = SplitLine(line);
    if (toks.empty() || toks[0][0] == '#') continue;
    if (toks[0] == "quit") break;
    toks.insert(toks.begin(), "frame_rig");
    std::vector<char*> av;
    for (std::string& s : toks) av.push_back(s.data());
    std::printf("@@JOB_BEGIN %d\n", job);
    std::fflush(stdout);
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = RunJob(int(av.size()), av.data(), sh);
    std::fflush(stderr);
    std::printf("@@JOB_END %d %d %.3f\n", job, rc, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::fflush(stdout);
    worst = std::max(worst, rc);
    ++job;
  }
  ServeShutdown(sh);
  return worst;
}

int main(int argc, char** argv) {
  Shared sh;
  if (argc >= 2 && std::string(argv[1]) == "--serve") { sh.serve = true; return ServeLoop(std::cin, sh); }
  if (argc >= 3 && std::string(argv[1]) == "--batch") {
    std::ifstream f(argv[2]);
    if (!f) { std::fprintf(stderr, "frame_rig: cannot read the job file %s\n", argv[2]); return 1; }
    sh.serve = true;
    return ServeLoop(f, sh);
  }
  return RunJob(argc, argv, sh);
}
