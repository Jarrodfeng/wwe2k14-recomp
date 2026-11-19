// screen_planes_unit: the offline proof of S103 lane M, the stage screens' live PlaneSource (docs\renderer\STAGE_LIVE_WRITERS.md section 2b). 0 game runs, no SDK, no game memory.
//   screen_planes_unit [--plant NAME]
// It runs the SHIPPED code: the flush copy, the channel -> set chain and the store (src\scene_live\screen_planes_core.h), the render-side PlaneSource and the latch-side rows
// (screen_planes_contrib.h), the flush entry's mode gate (screen_planes_hook.h), the stage-fx contributor's screen rows, and the YuvScreenFeed's shared conversion (screen_feed_yuv.h) against a FAKE GUEST
// built from the offsets the listings give (screen_planes_core.h header: the titan manager T -> controller -> slot record -> movie -> display set with the BINKFRAMEBUFFERS block).
// THE ORACLE is a pattern per (movie, buffer, frame): Y(x, y) = (3x + 5y + 40 movie + 17 buffer + 7 frame) & 255, the chroma and alpha planes analogous, so a wrong movie, a wrong half of the
// double buffer, a frame too old, a swapped chroma plane or a wrong pitch each shows as a different byte at a named place.
// A plant is a FAULT in the code under test (screen_planes_core.h Plants) or in the input; each must make a NAMED check fail (exit 1):
//   swap_channels      channels 1 and 2 are resolved to each other's movie (a screen bound to the WRONG MOVIE SLOT: the "reversed / wrong-surface" class)   -> "bind.key_to_movie"
//   wrong_buffer       the other half of the display double buffer is copied                                                                              -> "copy.display_buffer"
//   ignore_pitch       rows are read as if pitch == width                                                                                                  -> "copy.pitch"
//   swap_chroma        data planes 1 and 2 in the old wrong order                                                                                          -> "copy.chroma_order"
//   lazy_copy          the "copy" keeps a view of the guest bytes (tears when the decoder rewrites the source)                                              -> "copy.stable"
//   unbound_falls_back a key with no binding shows the first bound key's movie                                                                            -> "bind.unbound_none"
//   ignore_serial      the row's serial is ignored (always the newest frame)                                                                              -> "serial.exact"
//   ignore_state       a channel whose record is not ready (state != 1) is treated as ready                                                                -> "bind.not_ready_not_copied"
//   banner_to_ch5      the default table binds the banner band 807 to channel 5 (the UI nameplate movie)                                                  -> "bind.default_table"
//   panels_ch2         the default table binds the panels 801 802 806 808 809 to channel 2 (the mini-tron movie) instead of channel 1                       -> "bind.panels_channel1"
//   size_ignored       the expected-size check is skipped (an alias frame of another size is copied and given a row)                                         -> "bind.expected_size"
//   stamp_always       the decision input "screens" is stamped though a shown screen has no row                                                            -> "screens.complete_gate"
//   gate_800           the gate waits for screen 800 too (the front end owns it)                                                                          -> "screens.complete_gate"
//   stage_ignored      the flush copies with no stage object live                                                                                          -> "copy.needs_stage"
//   mode_ignored       the flush entry runs with --wwe2k14_rdr_scene = 0                                                                                  -> "mode.off_no_work"
//   show_unread        S103 SCF: the stage's show bits cannot be read (the fixed sets by look: the gate waits for screens the game switched off)                -> "show.read_bits" + "show.gate_follows_bits"
//   art_erased         S103 SCF: the live loader erases the art slots 301 / 302 / 304 (the pre-SCF rule: black quads over the art the game shows)               -> "show.loader_keeps_art"
//   idle_black         S103 SCF: a shown screen with no movie is painted black in the match look (the pre-SCF rule)                                           -> "show.idle_draw"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/load_plants.h"   // S103 SCF
#include "../../src/renderer_scene/screen_feed_yuv.h"
#include "../../src/renderer_scene/screen_idle_rows.h"
#include "../../src/scene_live/screen_planes_contrib.h"
#include "../../src/scene_live/screen_planes_hook.h"
#include "../../src/scene_live/stage_fx_contrib.h"

using namespace rdr_scene;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;
namespace scr = rdr_scene::live::screens;
namespace sfx = rdr_scene::live::stagefx;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// ---- the fake guest: header words (host order, read back big-endian) + byte regions for the planes
struct Region {
  uint32_t base = 0;
  std::vector<uint8_t> bytes;
};
struct Guest {
  std::map<uint32_t, uint32_t> words;
  std::vector<Region> regions;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  bool Read(uint32_t a, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    for (const Region& r : regions)
      if (a >= r.base && uint64_t(a) + n <= uint64_t(r.base) + r.bytes.size()) {
        std::memcpy(p, r.bytes.data() + (a - r.base), n);
        return true;
      }
    if ((a & 3) || (n & 3)) return false;
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t be = sfx::Swap32(it->second);
      std::memcpy(p + i, &be, 4);
    }
    return true;
  }
  Region& Alloc(uint32_t base, size_t n) {
    regions.push_back(Region{base, std::vector<uint8_t>(n, 0)});
    return regions.back();
  }
  Region* Find(uint32_t base) {
    for (Region& r : regions)
      if (r.base == base) return &r;
    return nullptr;
  }
};
static Guest g_guest;
static bool ReadThunk(uint32_t a, void* dst, size_t n) { return g_guest.Read(a, dst, n); }

// ---- the layout: T (titan manager) -> controller -> slot record -> movie -> display set
static constexpr uint32_t kT = 0x40100000, kCtrl = 0x40200000;
static constexpr uint32_t kW = 64, kH = 40, kCW = 32, kCH = 20, kPitchY = 96, kPitchC = 64, kBufs = 2;
static uint32_t MovieOfSlot(uint32_t slot) { return 0x40300000 + slot * 0x1000; }
static uint32_t SetOfSlot(uint32_t slot) { return 0x40400000 + slot * 0x1000; }
static uint32_t PlanePtr(uint32_t slot, uint32_t buf, uint32_t k) { return 0x50000000 + slot * 0x100000 + buf * 0x40000 + k * 0x10000; }
static uint32_t PitchOf(uint32_t k) { return (k == 0 || k == 3) ? kPitchY : kPitchC; }

// pattern oracle
static uint8_t Pat(uint32_t k, uint32_t x, uint32_t y, uint32_t slot, uint32_t buf, uint32_t frame) { return uint8_t(3 * x + 5 * y + 40 * slot + 17 * buf + 7 * frame + 11 * k); }

static void BuildSet(uint32_t slot, bool alpha) {
  const uint32_t S = SetOfSlot(slot);
  g_guest.Put(S + 128, kBufs);
  g_guest.Put(S + 132, kW), g_guest.Put(S + 136, kH), g_guest.Put(S + 140, kCW), g_guest.Put(S + 144, kCH);
  g_guest.Put(S + 148, 0);
  for (uint32_t b = 0; b < kBufs; ++b)
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t base = S + 152 + 12 * k + 48 * b;
      const bool present = k < 3 || alpha;
      g_guest.Put(base, present ? 1 : 0);
      g_guest.Put(base + 4, present ? PlanePtr(slot, b, k) : 0);
      g_guest.Put(base + 8, present ? PitchOf(k) : 0);
      if (present) g_guest.Alloc(PlanePtr(slot, b, k), size_t(PitchOf(k)) * kH);
    }
}
// The decoder's present: toggle, write the new frame into the display buffer, then the game's flush is called.
static void Decode(uint32_t slot, uint32_t buf, uint32_t frame, bool alpha) {
  g_guest.Put(SetOfSlot(slot) + 148, buf);
  g_guest.Put(MovieOfSlot(slot) + 192, buf);   // the toggle the present flipped (sub_827E0018 0x827E0098): equal to the set index at every real flush
  for (uint32_t k = 0; k < (alpha ? 4u : 3u); ++k) {
    Region* r = g_guest.Find(PlanePtr(slot, buf, k));
    const uint32_t w = (k == 0 || k == 3) ? kW : kCW, h = (k == 0 || k == 3) ? kH : kCH;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) r->bytes[size_t(y) * PitchOf(k) + x] = Pat(k, x, y, slot, buf, frame);
    for (uint32_t y = 0; y < h; ++y)   // the pitch padding holds a marker that must never be copied
      for (uint32_t x = w; x < PitchOf(k); ++x) r->bytes[size_t(y) * PitchOf(k) + x] = 0xEE;
  }
}
static void Scribble(uint32_t slot) {   // the decoder rewriting the source after our copy
  for (uint32_t b = 0; b < kBufs; ++b)
    for (uint32_t k = 0; k < 4; ++k)
      if (Region* r = g_guest.Find(PlanePtr(slot, b, k))) std::fill(r->bytes.begin(), r->bytes.end(), uint8_t(0x5A));
}
static void OpenChannel(uint32_t ch, uint32_t slot, uint32_t state = 1) {
  g_guest.Put(kT + 84 * ch + 32, slot);
  const uint32_t rec = kCtrl + 2144 + 32 * slot;
  g_guest.Put(rec + 4, state);
  g_guest.Put(rec + 28, MovieOfSlot(slot));
  g_guest.Put(MovieOfSlot(slot) + 36, SetOfSlot(slot));
}

static bool MatchesFrame(const scr::Frame& f, uint32_t slot, uint32_t buf, uint32_t frame, bool alpha, std::string* why = nullptr) {
  auto bad = [&](const char* what, uint32_t x, uint32_t y) {
    if (why) *why = std::string(what) + " at " + std::to_string(x) + "," + std::to_string(y);
    return false;
  };
  if (f.w != kW || f.h != kH || f.cw != kCW || f.ch != kCH || f.y.size() != size_t(kW) * kH || f.cb.size() != size_t(kCW) * kCH) return bad("size", f.w, f.h);
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) {
      if (f.y[size_t(y) * kW + x] != Pat(0, x, y, slot, buf, frame)) return bad("y", x, y);
      if (alpha && f.a[size_t(y) * kW + x] != Pat(3, x, y, slot, buf, frame)) return bad("a", x, y);
    }
  for (uint32_t y = 0; y < kCH; ++y)
    for (uint32_t x = 0; x < kCW; ++x) {
      if (f.cb[size_t(y) * kCW + x] != Pat(2, x, y, slot, buf, frame)) return bad("cb", x, y);
      if (f.cr[size_t(y) * kCW + x] != Pat(1, x, y, slot, buf, frame)) return bad("cr", x, y);
    }
  return true;
}

static int g_mode = 1;
static uint64_t g_now = 1000;
static int g_hook_calls = 0;

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
  scene_frame::g_mode_cvar.store(&g_mode);
  scr::g_now = +[]() -> uint64_t { return g_now; };

  // The world: channel 1 -> controller slot 2 (movie A, no alpha), channel 2 -> slot 4 (movie B, with alpha), channel 3 -> slot 5 opened but NOT ready (state 0).
  g_guest.Put(sfx::kStageModels, 0);   // (unused word so the map is never empty)
  g_guest.Put(scr::kTitanGlobal, kT);
  g_guest.Put(kT + 16, kCtrl);
  BuildSet(2, false);
  BuildSet(4, true);
  BuildSet(5, false);
  BuildSet(3, false);   // channel 0: the streamed titantron (320x320 in the game; the unit keeps the small test size)
  BuildSet(1, false);   // channel 5: the entrance UI movie
  OpenChannel(1, 2);
  OpenChannel(2, 4);
  OpenChannel(3, 5, 0);
  OpenChannel(0, 3);
  OpenChannel(5, 1);

  scr::PlaneStore& st = scr::Store();
  st.plants.wrong_buffer = Plant("wrong_buffer");
  st.plants.swap_channels = Plant("swap_channels");
  st.plants.lazy_copy = Plant("lazy_copy");
  st.plants.swap_chroma = Plant("swap_chroma");
  st.plants.unbound_falls_back = Plant("unbound_falls_back");
  st.plants.ignore_pitch = Plant("ignore_pitch");
  st.plants.ignore_serial = Plant("ignore_serial");
  st.plants.ignore_state = Plant("ignore_state");
  st.plants.ignore_expected_size = Plant("size_ignored");

  // ---------------------------------------------------------------- 0. the binding text
  {
    scr::ScreenBinding b;
    const int ok = b.Parse("801:1,802:1, 803:2");
    Check(ok == 3 && b.ChannelOf(801) == 1 && b.ChannelOf(802) == 1 && b.ChannelOf(803) == 2 && b.ChannelOf(800) == -1, "binding.parse", "n " + std::to_string(ok));
    const int bad1 = b.Parse("801:9"), bad2 = b.Parse("700:1"), bad3 = b.Parse("801");
    Check(bad1 < 0 && bad2 < 0 && bad3 < 0 && b.ChannelOf(801) == 1, "binding.refuses_malformed", "a malformed text leaves the old table");
    b.Parse("");
    Check(!b.AnyBound(), "binding.empty_is_unbound");
  }

  // ---------------------------------------------------------------- 1. nothing bound: the flush copies nothing, the rows are empty, the feed gives no picture
  {
    st.binding.Clear();   // (the shipped default table is tested in section 15)
    Decode(2, 0, 1, false);
    const bool stored = st.OnFlush(&ReadThunk, SetOfSlot(2), g_now);
    uint16_t keys[10];
    uint32_t se[10], ws[10], hs[10];
    const size_t n = st.Rows(nullptr, g_now, keys, se, ws, hs, 10);
    scr::StorePlaneSource ps(&st);
    rf::c::YuvScreenFeed feed(&ps);
    uint32_t w = 0, h = 0;
    const uint8_t* px = nullptr;
    Check(!stored && n == 0 && !feed.Picture(801, 1, w, h, px) && st.Census().copied == 0 && st.Census().unbound_skipped == 1, "bind.unbound_none_at_start", "copied " + std::to_string(st.Census().copied));
  }

  // ---------------------------------------------------------------- 2. bind: panels 801 and 802 show channel 1, 803 shows channel 2; 805 / 806 are unbound
  st.binding.Parse("801:1,802:1,803:2,804:3");
  scr::StorePlaneSource ps(&st);
  rf::c::YuvScreenFeed feed(&ps);
  Decode(2, 0, 10, false);
  Decode(4, 1, 20, true);   // movie B's display buffer is 1 this frame
  Decode(5, 0, 30, false);
  const bool s2 = st.OnFlush(&ReadThunk, SetOfSlot(2), g_now);
  const bool s4 = st.OnFlush(&ReadThunk, SetOfSlot(4), g_now);
  const bool s5 = st.OnFlush(&ReadThunk, SetOfSlot(5), g_now);
  Check(s2 && s4, "copy.flushes_stored", "stored " + std::to_string(int(s2)) + std::to_string(int(s4)));
  Check(!s5 && scr::PlaneStore::SetOfChannel(ReadThunk, 3, scr::Plants{}) == 0 && scr::PlaneStore::SetOfChannel(ReadThunk, 1, scr::Plants{}) == SetOfSlot(2), "bind.not_ready_not_copied", "channel 3 (bound to 804) has record state 0: no set, no copy");

  rf::c::YuvPlanes p801;
  uint64_t id801 = 0;
  std::shared_ptr<const scr::Frame> first801 = st.Find(nullptr, 801, 0, g_now);   // (serial 0 names no frame: the newest)
  const uint32_t serial_a = first801 ? uint32_t(first801->id) : 0;
  const bool g801 = ps.Planes(801, serial_a, p801, id801);
  std::string why801;
  std::shared_ptr<const scr::Frame> f801 = st.Find(nullptr, 801, serial_a, g_now);
  std::shared_ptr<const scr::Frame> f803 = st.Find(nullptr, 803, 0, g_now);
  Check(g801 && f801 && MatchesFrame(*f801, 2, 0, 10, false, &why801), "bind.key_to_movie", "801 -> channel 1 -> movie slot 2: " + (f801 ? why801 : std::string("none")));
  std::string why803;
  Check(f803 && MatchesFrame(*f803, 4, 1, 20, true, &why803), "bind.key_to_movie_b", "803 -> channel 2 -> movie slot 4 (alpha): " + (f803 ? why803 : std::string("none")));
  // 'the right buffer' = the toggle the game published (buffer 1 for movie B): a copy of the OTHER half fails here (the pattern carries the buffer number)
  Check(f803 && MatchesFrame(*f803, 4, 1, 20, true), "copy.display_buffer", "movie B toggled to buffer 1");
  Check(f801 && f801->y.size() == size_t(kW) * kH && f801->y[kW * 3 + kW - 1] == Pat(0, kW - 1, 3, 2, 0, 10) && f801->y[kW * 3] == Pat(0, 0, 3, 2, 0, 10), "copy.pitch", "row 3 ends at the picture's edge, not in the padding marker");
  Check(f801 && f801->cb[5] == Pat(2, 5, 0, 2, 0, 10) && f801->cr[5] == Pat(1, 5, 0, 2, 0, 10) && f801->cb[5] != f801->cr[5], "copy.chroma_order", "data plane 1 (cR) -> Cr, data plane 2 (cB) -> Cb (the game's crossed dispatch, sub_827F9D88)");

  // ---------------------------------------------------------------- 3. no tearing: the decoder rewrites the source after the flush; the copy is untouched
  Scribble(2);
  Scribble(4);
  std::shared_ptr<const scr::Frame> after = st.Find(nullptr, 801, serial_a, g_now);
  std::string why_after;
  Check(after && MatchesFrame(*after, 2, 0, 10, false, &why_after), "copy.stable", why_after);

  // ---------------------------------------------------------------- 4. one conversion per decoded frame, shared by the two screens that show it
  {
    uint32_t w = 0, h = 0;
    const uint8_t *r801 = nullptr, *r802 = nullptr, *r803 = nullptr;
    const uint32_t sA = uint32_t(st.Find(nullptr, 801, 0, g_now)->id), sB = uint32_t(st.Find(nullptr, 803, 0, g_now)->id);
    const bool a = feed.Picture(801, sA, w, h, r801), b = feed.Picture(802, sA, w, h, r802), c = feed.Picture(803, sB, w, h, r803);
    Check(a && b && c && w == kW && h == kH, "feed.pictures", "a " + std::to_string(int(a)) + std::to_string(int(b)) + std::to_string(int(c)));
    Check(feed.converted() == 2 && feed.shared() == 1 && r801 == r802 && r801 != r803, "feed.shared_once", "converted " + std::to_string((unsigned long long)feed.converted()) + " shared " + std::to_string((unsigned long long)feed.shared()));
    // a NEW decoded frame of movie A: one more conversion for the pair, not two
    Decode(2, 1, 11, false);
    st.OnFlush(&ReadThunk, SetOfSlot(2), g_now);
    const uint32_t sA2 = uint32_t(st.Find(nullptr, 801, 0, g_now)->id);
    const bool a2 = feed.Picture(801, sA2, w, h, r801), b2 = feed.Picture(802, sA2, w, h, r802);
    Check(a2 && b2 && sA2 != sA && feed.converted() == 3 && feed.shared() == 2, "feed.one_per_frame", "converted " + std::to_string((unsigned long long)feed.converted()) + " shared " + std::to_string((unsigned long long)feed.shared()));
  }

  // ---------------------------------------------------------------- 4b. S103 UPL (review): the content id names the decoded frame, and a cache hit's pointer is valid only until the NEXT call (the cache recycles round-robin)
  {
    struct Fake : rf::c::PlaneSource {
      std::vector<uint8_t> yp, cbp, crp;
      bool Planes(uint32_t, uint32_t serial, rf::c::YuvPlanes& out, uint64_t& id) override {
        yp.assign(16 * 16, uint8_t(serial * 7)), cbp.assign(8 * 8, 128), crp.assign(8 * 8, 128);
        out = rf::c::YuvPlanes();
        out.w = out.h = 16, out.y = yp.data(), out.y_pitch = 16, out.cb = cbp.data(), out.cb_pitch = 8, out.cr = crp.data(), out.cr_pitch = 8;
        id = 1000 + serial;
        return true;
      }
    } fake;
    rf::c::YuvScreenFeed f4(&fake);
    uint32_t w = 0, h = 0;
    const uint8_t* ptr[6] = {};
    uint64_t ids[6] = {};
    bool all = true;
    for (uint32_t i = 1; i <= 4; ++i) all = f4.Picture(800 + i, i, w, h, ptr[i], ids[i]) && all;
    const size_t n = size_t(w) * h * 4;
    const std::vector<uint8_t> taken(ptr[1], ptr[1] + n);   // what a caller that copies at the time has
    const bool hit = f4.Picture(801, 1, w, h, ptr[0], ids[0]);
    Check(all && hit && ptr[0] == ptr[1] && ids[0] == 1001 && ids[1] == 1001 && ids[2] == 1002 && ids[3] != ids[4], "feed.content_id", "ids " + std::to_string((unsigned long long)ids[1]) + " " + std::to_string((unsigned long long)ids[2]) + " hit " + std::to_string((unsigned long long)ids[0]));
    const bool fifth = f4.Picture(805, 5, w, h, ptr[5], ids[5]);   // a fifth distinct frame recycles the oldest entry: the one the hit pointed at
    const std::vector<uint8_t> now(ptr[1], ptr[1] + n);
    Check(fifth && ptr[5] == ptr[1] && ids[5] == 1005 && now != taken, "feed.recycled_under_a_kept_pointer", "a pointer kept past the next call reads another picture (the contract: valid until the next Picture call; rf_screens copies per content id)");
  }

  // ---------------------------------------------------------------- 5. the row's serial picks its frame (the record lags the game by a frame): the earlier frame is still found
  {
    const std::shared_ptr<const scr::Frame> cur = st.Find(nullptr, 801, 0, g_now);
    std::string w1;
    const std::shared_ptr<const scr::Frame> old = st.Find(nullptr, 801, serial_a, g_now);   // movie A's FIRST frame, asked for by its serial after a newer one arrived
    Check(old && cur && uint32_t(old->id) == serial_a && cur->id != old->id && MatchesFrame(*old, 2, 0, 10, false, &w1), "serial.exact", old ? ("got id " + std::to_string((unsigned long long)old->id) + " " + w1) : std::string("none"));
    Check(st.Census().serial_exact >= 1, "serial.counted");
  }

  // ---------------------------------------------------------------- 6. unbound keys: no picture, no row
  {
    rf::c::YuvPlanes q;
    uint64_t id = 0;
    uint32_t w = 0, h = 0;
    const uint8_t* px = nullptr;
    const bool p805 = ps.Planes(805, 0, q, id), p800 = ps.Planes(800, 0, q, id), pf = feed.Picture(806, 1, w, h, px);
    Check(!p805 && !p800 && !pf, "bind.unbound_none", "805 " + std::to_string(int(p805)) + " 800 " + std::to_string(int(p800)) + " 806 " + std::to_string(int(pf)));
    uint16_t keys[10];
    uint32_t se[10], ws[10], hs[10];
    const size_t n = st.Rows(nullptr, g_now, keys, se, ws, hs, 10);
    const bool only = n == 3 && keys[0] == 801 && keys[1] == 802 && keys[2] == 803;
    Check(only && ws[0] == kW && hs[0] == kH && se[0] == se[1] && se[2] != se[0], "rows.bound_keys_only", "n " + std::to_string(n));
  }

  // ---------------------------------------------------------------- 7. stale: a movie that stopped flushing shows nothing
  {
    const bool fresh = st.Find(nullptr, 801, 0, g_now + scr::kStaleFrames) != nullptr;
    const bool stale = st.Find(nullptr, 801, 0, g_now + scr::kStaleFrames + 1) == nullptr;
    uint16_t keys[10];
    uint32_t se[10], ws[10], hs[10];
    const size_t n = st.Rows(nullptr, g_now + scr::kStaleFrames + 1, keys, se, ws, hs, 10);
    Check(fresh && stale && n == 0, "stale.none", "fresh " + std::to_string(int(fresh)) + " stale " + std::to_string(int(stale)) + " rows " + std::to_string(n));
  }

  // ---------------------------------------------------------------- 8. a layout we do not trust is refused, counted, never copied
  {
    const uint64_t before = st.Census().bad_layout;
    g_guest.Put(SetOfSlot(2) + 148, 7);   // display index beyond the buffer count
    g_guest.Put(MovieOfSlot(2) + 192, 7);   // (the toggle agrees, so the layout check is what refuses)
    const bool r1 = st.OnFlush(&ReadThunk, SetOfSlot(2), g_now);
    g_guest.Put(SetOfSlot(2) + 148, 0);
    g_guest.Put(MovieOfSlot(2) + 192, 0);
    g_guest.Put(SetOfSlot(2) + 132, 0);   // zero width
    const bool r2 = st.OnFlush(&ReadThunk, SetOfSlot(2), g_now);
    g_guest.Put(SetOfSlot(2) + 132, kW);
    g_guest.Put(SetOfSlot(2) + 140, 3);   // chroma narrower than half the luma
    const bool r3 = st.OnFlush(&ReadThunk, SetOfSlot(2), g_now);
    g_guest.Put(SetOfSlot(2) + 140, kCW);
    Check(!r1 && !r2 && !r3 && st.Census().bad_layout >= before + 3, "layout.refused", "bad_layout " + std::to_string((unsigned long long)(st.Census().bad_layout - before)));
  }

  // ---------------------------------------------------------------- 9. the mode gate: --wwe2k14_rdr_scene = 0 does no work at the flush
  {
    wwe2k14::scene_screens::g_on_flush.store(+[](uint8_t*, uint32_t) { ++g_hook_calls; }, std::memory_order_relaxed);
    g_mode = Plant("mode_ignored") ? 2 : 0;
    g_hook_calls = 0;
    wwe2k14::scene_screens::OnDisplayFlush(nullptr, SetOfSlot(2));
    const int at_off = g_hook_calls;
    g_mode = 1;
    wwe2k14::scene_screens::OnDisplayFlush(nullptr, SetOfSlot(2));
    wwe2k14::scene_screens::g_on_flush.store(nullptr, std::memory_order_relaxed);
    Check(at_off == 0 && g_hook_calls == 1, "mode.off_no_work", "calls at mode 0: " + std::to_string(at_off) + ", at mode 1: " + std::to_string(g_hook_calls - at_off));
  }

  // ---------------------------------------------------------------- 10. the stage-fx contributor turns the bound keys into kFxScreen rows
  {
    sfx::StageFxContributor con;
    con.SetReader(ReadThunk);
    con.SetScreens(&scr::ScreensFnImpl);
    con.SetExtraCensus([](std::string& o) { scr::CensusLine(o, nullptr); });
    FrameInfo fi;
    fi.guest_frame = 700;
    SceneTables t;
    for (uint8_t k = 0; k < 10; ++k) {   // the arena contributor's part: a TronPicture Target (index = key - 800) + an OnTarget Movie per screen
      Target tg;
      tg.role = rdr_adapter::SurfaceRole::TronPicture;
      tg.index = k;
      t.targets.push_back(tg);
      Movie m;
      std::memset(static_cast<void*>(&m), 0, sizeof m);
      m.show = uint8_t(MovieShow::OnTarget);
      m.target = uint32_t(t.targets.size() - 1);
      t.movies.push_back(m);
    }
    con.Latch(fi);
    Counters c;
    con.Emit(fi, t, c);
    size_t rows = 0, r801 = 0, r803 = 0, r805 = 0;
    uint32_t seq801 = 0;
    for (const StageFxRow& r : t.stage_fx) {
      if (r.kind != kFxScreen) continue;
      ++rows;
      if (r.slot == 801) ++r801, seq801 = r.seq;
      if (r.slot == 803) ++r803;
      if (r.slot == 805) ++r805;
    }
    Check(rows == 3 && r801 == 1 && r803 == 1 && r805 == 0 && seq801 == uint32_t(st.Find(nullptr, 801, 0, g_now)->id), "rows.contributor", "rows " + std::to_string(rows) + " seq801 " + std::to_string(seq801));
    std::string census;
    con.Census(census);
    const size_t at = census.find("[screens_live");
    Check(at != std::string::npos && census.find("copied", at) != std::string::npos, "census.line", at == std::string::npos ? std::string("missing") : census.substr(at, 110));
  }

  // ---------------------------------------------------------------- 11. review F2: header values that cannot be a real set are refused
  {
    const uint32_t S2 = SetOfSlot(2);
    const uint64_t before = st.Census().bad_layout;
    g_guest.Put(S2 + 128, 3);   // three buffers: a set holds two
    const bool a = st.OnFlush(&ReadThunk, S2, g_now);
    g_guest.Put(S2 + 128, kBufs);
    g_guest.Put(S2 + 160, 9000);   // plane 0 buffer 0 pitch beyond 8192
    const bool b = st.OnFlush(&ReadThunk, S2, g_now);
    g_guest.Put(S2 + 160, kPitchY);
    g_guest.Put(S2 + 156, 0xFFFFFF00u);   // rows that would run past 2^32
    const bool c = st.OnFlush(&ReadThunk, S2, g_now);
    g_guest.Put(S2 + 156, PlanePtr(2, 0, 0));
    g_guest.Put(S2 + 164, 0);   // plane 1 not allocated
    const bool d = st.OnFlush(&ReadThunk, S2, g_now);
    g_guest.Put(S2 + 164, 1);
    Check(!a && !b && !c && !d && st.Census().bad_layout >= before + 4, "layout.refused_f2", "bad_layout +" + std::to_string((unsigned long long)(st.Census().bad_layout - before)));
  }
  // ---------------------------------------------------------------- 12. review F7: a flush with no copy behind it (toggle != set index) stores nothing
  {
    Decode(2, 0, 12, false);
    g_guest.Put(MovieOfSlot(2) + 192, 1);   // the toggle flipped, the copy has not happened
    const uint64_t before = st.Census().toggle_mismatch, copied = st.Census().copied;
    const bool r = st.OnFlush(&ReadThunk, SetOfSlot(2), g_now);
    g_guest.Put(MovieOfSlot(2) + 192, 0);
    Check(!r && st.Census().toggle_mismatch == before + 1 && st.Census().copied == copied, "copy.toggle_mismatch", "no frame stored");
  }
  // ---------------------------------------------------------------- 13. review F5: a slot whose [T + 84 slot + 60] is set is not a movie (the planes come from another object)
  {
    g_guest.Put(kT + 84 * 4 + 60, 0xA0000000u);
    bool nm = false;
    const uint32_t r = scr::PlaneStore::SetOfChannel(ReadThunk, 2, scr::Plants{}, nullptr, &nm);
    g_guest.Put(kT + 84 * 4 + 60, 0);
    Check(r == 0 && nm && scr::PlaneStore::SetOfChannel(ReadThunk, 2, scr::Plants{}) == SetOfSlot(4), "bind.non_movie_refused", "channel 2 -> slot 4 with the non-movie word set");
  }
  // ---------------------------------------------------------------- 14. review F3: a set nobody shows any more is dropped
  {
    scr::PlaneStore s2;
    s2.binding.Parse("801:1");
    Decode(2, 0, 20, false);
    s2.OnFlush(&ReadThunk, SetOfSlot(2), 100);
    OpenChannel(1, 5);   // channel 1 now plays another movie (slot 5)
    Decode(5, 0, 21, false);
    s2.OnFlush(&ReadThunk, SetOfSlot(5), 300);
    OpenChannel(1, 2);
    const scr::StoreCensus c = s2.Census();
    Check(c.pruned == 1 && c.sets_seen == 1, "store.prunes_dead_sets", "pruned " + std::to_string((unsigned long long)c.pruned) + " sets " + std::to_string((unsigned long long)c.sets_seen));
  }

  // ---------------------------------------------------------------- 15. the shipped DEFAULT table: the tron surfaces take channel 0's whole plane, the mini-tron channel 2, nothing else is bound
  {
    scr::ScreenBinding b;
    b.LoadDefaults(Plant("banner_to_ch5"), Plant("panels_ch2"));   // the plant binds the banner band 807 to channel 5 (the entrance UI's movie)
    uint32_t ew = 0, eh = 0;
    const bool sizes = b.ExpectedSize(804, ew, eh) && ew == 320 && eh == 320 && b.ExpectedSize(805, ew, eh) && ew == 256 && eh == 128;
    const bool table = sizes && b.ChannelOf(800) == -1 && b.ChannelOf(803) == -1 && b.ChannelOf(804) == 0 && b.ChannelOf(807) == 0 && b.ChannelOf(805) == 2 &&
                       b.ChannelOf(801) == 1 && b.ChannelOf(802) == 1 && b.ChannelOf(806) == 1 && b.ChannelOf(808) == 1 && b.ChannelOf(809) == 1 &&
                       b.ExpectedSize(802, ew, eh) && ew == 256 && eh == 256;   // S103 M2: the panels = channel 1 @ 256x256 (run 18)
    scr::PlaneStore sd;
    sd.binding.LoadDefaults(Plant("banner_to_ch5"), Plant("panels_ch2"));
    for (uint32_t k : {804u, 807u, 805u, 801u, 802u, 806u, 808u, 809u}) sd.binding.Set(k, sd.binding.ChannelOf(k), kW, kH);   // (the unit's movies are 64x40; the shipped sizes were checked above)
    Decode(3, 0, 40, false);   // channel 0: the titantron
    Decode(4, 1, 41, true);    // channel 2: the mini-tron movie
    Decode(1, 0, 42, false);   // channel 5: the UI movie
    Decode(2, 0, 43, false);   // channel 1: the panel sheet
    sd.OnFlush(&ReadThunk, SetOfSlot(2), g_now);
    sd.OnFlush(&ReadThunk, SetOfSlot(3), g_now);
    sd.OnFlush(&ReadThunk, SetOfSlot(4), g_now);
    sd.OnFlush(&ReadThunk, SetOfSlot(1), g_now);
    auto f804 = sd.Find(nullptr, 804, 0, g_now), f807 = sd.Find(nullptr, 807, 0, g_now), f803 = sd.Find(nullptr, 803, 0, g_now), f805 = sd.Find(nullptr, 805, 0, g_now);
    std::string w;
    const bool tron = f804 && f807 && !f803 && f804 == f807 && MatchesFrame(*f804, 3, 0, 40, false, &w);   // ONE frame shared by the centre, the banner band and the sheet quad
    Check(table && tron && f805 && MatchesFrame(*f805, 4, 1, 41, true), "bind.default_table",
          std::string("table ") + (table ? "ok" : "WRONG") + " tron " + (tron ? "ok" : ("WRONG " + w)));
    // S103 M2: the panels (all five keys) show channel 1's frame: ONE frame shared (the same pooled movie), not the tron's, not the mini-tron's
    std::string wp;
    bool panels = true;
    std::shared_ptr<const scr::Frame> first_panel;
    for (uint32_t k : {801u, 802u, 806u, 808u, 809u}) {
      auto fp = sd.Find(nullptr, k, 0, g_now);
      if (!fp) { panels = false; wp = "no frame for " + std::to_string(k); break; }
      if (!first_panel) first_panel = fp;
      if (fp != first_panel || !MatchesFrame(*fp, 2, 0, 43, false, &wp)) { panels = false; break; }
    }
    Check(panels && table, "bind.panels_channel1", panels ? "801 802 806 808 809 -> channel 1 -> one shared 256x256 sheet" : "WRONG " + wp);
  }

  // ---------------------------------------------------------------- 16. review B1: an unopened channel ALIASES slot 0's movie (another size): never copied, never a row, the expected size stays
  {
    scr::PlaneStore sb;
    sb.plants.ignore_expected_size = Plant("size_ignored");
    sb.binding.Clear();
    sb.binding.Set(804, 0, kW, kH);
    const uint32_t S3 = SetOfSlot(3);
    g_guest.Put(S3 + 132, 48), g_guest.Put(S3 + 136, 32), g_guest.Put(S3 + 140, 24), g_guest.Put(S3 + 144, 16);   // channel 0 -> slot 3, which for now holds a smaller movie (the title logo's role)
    Decode(3, 0, 50, false);
    const bool alias_copied = sb.OnFlush(&ReadThunk, S3, g_now);
    uint16_t keys[10];
    uint32_t se[10], ws[10], hs[10];
    const size_t n_alias = sb.Rows(nullptr, g_now, keys, se, ws, hs, 10);
    const uint64_t skipped = sb.Census().wrong_size_skipped;
    uint32_t ew = 0, eh = 0;
    const bool size_kept = sb.binding.ExpectedSize(804, ew, eh) && ew == kW && eh == kH;
    g_guest.Put(S3 + 132, kW), g_guest.Put(S3 + 136, kH), g_guest.Put(S3 + 140, kCW), g_guest.Put(S3 + 144, kCH);   // the tron opens: the real size
    Decode(3, 0, 51, false);
    const bool real_copied = sb.OnFlush(&ReadThunk, S3, g_now);
    const size_t n_real = sb.Rows(nullptr, g_now, keys, se, ws, hs, 10);
    Check(!alias_copied && n_alias == 0 && skipped == 1 && size_kept && real_copied && n_real == 1 && keys[0] == 804 && ws[0] == kW && hs[0] == kH, "bind.expected_size",
          "alias copied " + std::to_string(int(alias_copied)) + " rows " + std::to_string(n_alias) + " skipped " + std::to_string((unsigned long long)skipped) + "; real rows " + std::to_string(n_real));
  }
  // ---------------------------------------------------------------- 17. review B2: the decision input "screens" is stamped only when every screen the LOOK shows (that has a Movie) has a row
  {
    scr::PlaneStore& g = scr::Store();
    g.binding.Clear();
    g.binding.Set(805, 2, kW, kH);   // only the mini-tron is bound (no titantron playing)
    Decode(4, 1, 61, true);
    g.OnFlush(&ReadThunk, SetOfSlot(4), g_now);
    static bool s_show801 = true;
    static int s_look = 1;   // the published arena look: 0 unknown, 1 match, 2 entrance
    s_show801 = true, s_look = 1;
    sfx::StageFxContributor con;
    con.SetReader(ReadThunk);
    con.SetScreens(&scr::ScreensFnImpl);
    con.SetScreenGate(+[](uint32_t key) -> bool { return key == 800 || key == 805 || (key == 801 && s_show801); }, +[](uint32_t key) -> bool { return scr::Store().binding.ChannelOf(key) >= 0; },
                      +[]() -> int { return s_look; });
    con.PlantStampAlways(Plant("stamp_always"));
    con.PlantRequire800(Plant("gate_800"));   // the front end owns 800 in the match look: the gate must not wait for it
    SceneTables t;
    for (uint8_t k = 0; k < 10; ++k) {
      Target tg;
      tg.role = rdr_adapter::SurfaceRole::TronPicture;
      tg.index = k;
      t.targets.push_back(tg);
      Movie m;
      std::memset(static_cast<void*>(&m), 0, sizeof m);
      m.show = uint8_t(MovieShow::OnTarget);
      m.target = uint32_t(t.targets.size() - 1);
      t.movies.push_back(m);
    }
    const int bit = scene_inputs::kInStageScreens;
    auto run = [&](uint64_t frame) {
      rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].store(0);
      FrameInfo f;
      f.guest_frame = frame;
      con.Latch(f);
      Counters c;
      SceneTables tt = t;
      con.Emit(f, tt, c);
      return rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].load();
    };
    const uint64_t s1 = run(800);                    // match look, 801 shown with a Movie and no row (unbound): NOT complete
    s_show801 = false;
    const uint64_t s2 = run(801);                    // 801 gone: the match set is 800 (the front end's, no tron playing: skipped) and 805 (row): complete
    s_look = 0;
    const uint64_t s3 = run(802);                    // look unknown: fails closed, 800 is required again
    s_look = 1;
    g.binding.Set(804, 0, kW, kH);                   // a titantron plays (a row for 804): the main rectangle carries it, so 800 is required in the match look
    Decode(3, 0, 62, false);
    g.OnFlush(&ReadThunk, SetOfSlot(3), g_now);
    const uint64_t s4 = run(803);
    std::string census;
    con.Census(census);
    Check(s1 == 0 && s2 == 800 && s3 == 0 && s4 == 0 && census.find("of_which_unbound") != std::string::npos, "screens.complete_gate",
          "stamps: match+unbound 801 " + std::to_string((unsigned long long)s1) + ", complete " + std::to_string((unsigned long long)s2) + ", unknown look " + std::to_string((unsigned long long)s3) + ", tron playing " + std::to_string((unsigned long long)s4));
    sfx::StageFxContributor nogate;
    nogate.SetReader(ReadThunk);
    nogate.SetScreens(&scr::ScreensFnImpl);
    rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].store(0);
    FrameInfo f3;
    f3.guest_frame = 810;
    nogate.Latch(f3);
    SceneTables t3 = t;
    Counters c3;
    nogate.Emit(f3, t3, c3);
    Check(rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].load() == 0, "screens.no_provider_no_stamp", "without a `shown` provider the input is never stamped (fails closed)");
    // S103 E7: the game plays NO movie (run 26: a match with the entrances skipped, 0 flushes in 3949 match frames): a shown screen without a row is then complete without one, in the KNOWN MATCH look only.
    // Only 805 is bound (channel 2 -> controller slot 4), so a flush of slot 4 is the only flush and the 60-frame window is what separates the stamps (review B1: +30, +60, +61); an OPEN channel or a
    // non-movie flag keeps the rule off (review B4); the idle rows tell the applier to drop a live texture a movie left behind (review B3).
    {
      scr::PlaneStore& gs = scr::Store();
      gs.binding.Clear();
      gs.binding.Set(805, 2, kW, kH);
      gs.SetIdleWindow(Plant("idle_window0") ? 0 : scr::kIdleFlushFrames);
      gs.plants.ignore_channels = Plant("idle_ignores_channels");
      const uint64_t keep_now = g_now;
      Decode(4, 1, 63, true);
      gs.OnFlush(&ReadThunk, SetOfSlot(4), keep_now);   // the last flush of any set, at keep_now
      sfx::StageFxContributor ic;
      ic.SetReader(ReadThunk);
      ic.SetScreens(&scr::ScreensFnImpl);
      ic.SetScreenGate(+[](uint32_t key) -> bool { return key == 800 || key == 805 || key == 801; }, +[](uint32_t key) -> bool { return scr::Store().binding.ChannelOf(key) >= 0; }, +[]() -> int { return s_look; });
      ic.SetScreenIdle(+[](sfx::ReadFn read) -> bool { return scr::Store().Idle(scr::Now()) && scr::Store().ChannelsQuiet(read); });
      ic.PlantNoIdle(Plant("idle_off"));
      std::vector<StageFxRow> last_rows;
      size_t idle_rows = 0;
      auto runi = [&](uint64_t frame, uint64_t at) {
        g_now = keep_now + at;
        rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].store(0);
        FrameInfo fx;
        fx.guest_frame = frame;
        ic.Latch(fx);
        Counters cx;
        SceneTables tx = t;
        ic.Emit(fx, tx, cx);
        last_rows = tx.stage_fx;
        idle_rows = 0;
        for (const StageFxRow& r : tx.stage_fx) idle_rows += (r.kind == kFxScreen && (r.flags & kFxfScreenIdle)) ? 1 : 0;
        return rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].load();
      };
      const uint32_t rec4 = kCtrl + 2144 + 32 * 4 + 4;
      s_look = 1;
      const uint64_t o200 = runi(900, 200);        // the channel is still OPEN (state 1): a movie may be about to flush: not idle
      g_guest.Put(rec4, 0);                        // the movie closed
      const uint64_t w30 = runi(901, 30), w60 = runi(902, 60);   // inside the window
      const uint64_t w61 = runi(903, 61);          // 61 frames after the last flush: idle (805 still has its fresh row; 801 gets an idle row)
      const size_t idle_at_61 = idle_rows;
      const uint64_t w200 = runi(904, 200);        // 805's row is stale too: both get idle rows
      const std::vector<StageFxRow> rows200 = last_rows;
      g_guest.Put(kT + 84 * 4 + 60, 1);            // a picture that does not come through the flush (the titan's non-movie flag)
      const uint64_t nm200 = runi(905, 200);
      g_guest.Put(kT + 84 * 4 + 60, 0);
      s_look = 2;
      const uint64_t ent = runi(906, 200);         // the entrance look: a movie may be about to start, the frame stays the game's
      s_look = 0;
      const uint64_t unk = runi(907, 200);         // unknown look: fails closed
      s_look = 1;
      OpenChannel(2, 4);
      g_now = keep_now;
      gs.SetIdleWindow(scr::kIdleFlushFrames);
      gs.plants.ignore_channels = false;
      std::string ics;
      ic.Census(ics);
      Check(o200 == 0 && w30 == 0 && w60 == 0 && w61 == 902 && idle_at_61 == 1 && w200 == 903 && nm200 == 0 && ent == 0 && unk == 0 && ics.find("idle_no_movie") != std::string::npos, "screens.idle_gate",
            "stamps: channel open " + std::to_string((unsigned long long)o200) + ", +30 " + std::to_string((unsigned long long)w30) + ", +60 " + std::to_string((unsigned long long)w60) + ", +61 " + std::to_string((unsigned long long)w61) + " (idle rows " +
                std::to_string((unsigned long long)idle_at_61) + "), +200 " + std::to_string((unsigned long long)w200) + ", non-movie flag " + std::to_string((unsigned long long)nm200) + ", entrance look " + std::to_string((unsigned long long)ent) +
                ", unknown look " + std::to_string((unsigned long long)unk));
      // review B3: a movie ran (805 had a live texture) and stopped; 91+ frames on, the idle rows take the texture out so the file's own picture is drawn
      std::map<int, std::pair<uint32_t, uint32_t>> live_tex;
      live_tex[805] = {kW, kH};
      const size_t dropped = rdr_scene::rf::c::DropIdleScreens(live_tex, rows200, !Plant("idle_keeps_texture"));
      Check(dropped == 1 && live_tex.count(805) == 0, "screens.idle_drops_texture", "idle rows at +200: dropped " + std::to_string((unsigned long long)dropped) + ", live texture of 805 " + (live_tex.count(805) ? "kept" : "gone"));
      // S103 HN (ONF review M1): a shown screen with no sized row keeps the previous entrant's picture; it goes to the idle black after kNoRowBlackAfter frames without one, and a row coming back ends it
      {
        const bool on = !Plant("stale_movie_kept");
        uint64_t nr = 0, lg = 0;
        const bool f1 = rdr_scene::rf::c::NoRowGoesBlack(false, 1000, nr, lg, on);                 // the first frame with no row: still the last picture (a one-frame hole must not blink)
        const bool f2 = rdr_scene::rf::c::NoRowGoesBlack(false, 1001, nr, lg, on);
        const bool f3 = rdr_scene::rf::c::NoRowGoesBlack(false, 1000 + rdr_scene::rf::c::kNoRowBlackAfter, nr, lg, on);   // two frames on: black
        const bool back = rdr_scene::rf::c::NoRowGoesBlack(true, 1010, nr, lg, on);                // a sized row: a picture, the count restarts
        const bool again = rdr_scene::rf::c::NoRowGoesBlack(false, 1011, nr, lg, on);
        // Opus review: the look-switch return and a backwards frame jump start the count again (a stale no_row_gf must not black a screen at once)
        uint64_t nr2 = 0, lg2 = 0;
        rdr_scene::rf::c::NoRowGoesBlack(false, 2000, nr2, lg2, on);                                // look A: no row since 2000
        const bool ret = rdr_scene::rf::c::NoRowGoesBlack(false, 2500, nr2, lg2, on);              // back at the look 500 frames later: the old count is stale, so NOT black yet
        const bool back_jump = rdr_scene::rf::c::NoRowGoesBlack(false, 100, nr2, lg2, on);         // the counter went backwards: counted again from here
        // a row coming back with the SAME serial after the black must repaint the texture
        const bool skip_plain = rdr_scene::rf::c::SameSerialSkips(7, 7, false), skip_black = rdr_scene::rf::c::SameSerialSkips(7, 7, true);
        Check(!f1 && !f2 && f3 && !back && !again && nr == 1011 && !ret && !back_jump && nr2 == 100 && skip_plain && !skip_black, "screens.no_row_black", std::string("frames 0/1/2 without a row: ") + (f1 ? "B" : "-") + (f2 ? "B" : "-") + (f3 ? "B" : "-") + ", row back " + (back ? "B" : "-") + ", gap restarts " + (again ? "B" : "-"));
      }
    }
  }
  // ---------------------------------------------------------------- 18. review B3: the cvar text goes through ApplySpec (the glue's call); a malformed text keeps the DEFAULT table
  {
    scr::ScreenBinding b;
    std::string m;
    const int a = b.ApplySpec("", m);
    const bool def = a == 0 && b.ChannelOf(804) == 0 && b.ChannelOf(805) == 2;
    const int bad = b.ApplySpec("804:9", m);
    const bool kept = bad < 0 && m.find("DEFAULT") != std::string::npos && b.ChannelOf(804) == 0 && b.ChannelOf(805) == 2;
    const int bad2 = b.ApplySpec("804:0@320", m);
    b.ApplySpec("none", m);
    const bool none = !b.AnyBound();
    const int n = b.ApplySpec("804:0@320x240,805:2", m);
    uint32_t w = 0, h = 0;
    const bool replaced = n == 2 && b.ExpectedSize(804, w, h) && w == 320 && h == 240 && !b.ExpectedSize(805, w, h) && b.ChannelOf(805) == 2 && b.ChannelOf(803) == -1;
    Check(def && kept && bad2 < 0 && none && replaced, "binding.apply_spec", std::string("default ") + (def ? "ok" : "no") + " malformed-keeps " + (kept ? "ok" : "no") + " none " + (none ? "ok" : "no") + " replace " + (replaced ? "ok" : "no"));
  }
  // ---------------------------------------------------------------- 19. review C1: no stage object, no resolution and no copy
  {
    scr::PlaneStore sc;
    sc.binding.Clear();
    sc.binding.Set(804, 0, kW, kH);
    if (!Plant("stage_ignored")) sc.stage_live = +[]() -> bool { return false; };
    Decode(3, 0, 70, false);
    const bool off = sc.OnFlush(&ReadThunk, SetOfSlot(3), g_now);
    const uint64_t skipped = sc.Census().no_stage_skipped;
    sc.stage_live = +[]() -> bool { return true; };
    const bool on = sc.OnFlush(&ReadThunk, SetOfSlot(3), g_now);
    Check(!off && skipped == 1 && on, "copy.needs_stage", "stage not live: skipped " + std::to_string((unsigned long long)skipped) + "; live: copied");
  }

  // ---------------------------------------------------------------- 20. S103 SCN (A): the mini-tron 805 (channel 2, 256x128) shows a MOVING picture: a synthetic equaliser at serial 1, a changed one at serial 2.
  // The live texture's bytes (the shipped store -> StorePlaneSource -> YuvScreenFeed -> the applier's "new serial" rule) must equal an INDEPENDENT convert (integer BT.601, written here, +-2) and CHANGE within
  // one flush. Twin stale_screen_pic (the store keeps only the first copy of a set) must fail "screens.pic_refresh" with "static picture" (run 38: 805 identical in frames 300 apart).
  {
    const Guest saved = g_guest;
    g_guest = Guest();
    constexpr uint32_t EW = 256, EH = 128, ECW = 128, ECH = 64, EPY = 256, EPC = 160;   // chroma pitch wider than the width (a padded row): the pitch must be honoured
    const uint32_t set = 0x41000000, movie = 0x41100000, ctrl = 0x41200000, tt = 0x41300000;
    g_guest.Put(scr::kTitanGlobal, tt);
    g_guest.Put(tt + 16, ctrl);
    g_guest.Put(tt + 84 * 2 + 32, 3);                          // channel 2 -> controller slot 3
    g_guest.Put(ctrl + 2144 + 32 * 3 + 4, 1);                  // record state 1 = open
    g_guest.Put(ctrl + 2144 + 32 * 3 + 28, movie);
    g_guest.Put(movie + 36, set);
    g_guest.Put(set + 128, 2), g_guest.Put(set + 132, EW), g_guest.Put(set + 136, EH), g_guest.Put(set + 140, ECW), g_guest.Put(set + 144, ECH), g_guest.Put(set + 148, 0);
    const uint32_t pitch[4] = {EPY, EPC, EPC, EPY}, pw[4] = {EW, ECW, ECW, EW}, ph[4] = {EH, ECH, ECH, EH};
    auto ptr = [](uint32_t b, uint32_t k) { return 0x60000000u + b * 0x40000 + k * 0x10000; };
    for (uint32_t b = 0; b < 2; ++b)
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t base = set + 152 + 12 * k + 48 * b;
        const bool present = k < 3;
        g_guest.Put(base, present ? 1 : 0), g_guest.Put(base + 4, present ? ptr(b, k) : 0), g_guest.Put(base + 8, present ? pitch[k] : 0);
        if (present) g_guest.Alloc(ptr(b, k), size_t(pitch[k]) * ph[k]);
      }
    // an equaliser: 16 bars of 16 px, bar i is lit up to a height that depends on (i, frame); lit = bright (Y 200, red chroma), dark = Y 20 (blue chroma); the padding marker must never show
    std::vector<uint8_t> want[2];   // the oracle's RGBA per frame
    auto decode = [&](uint32_t buf, uint32_t frame, std::vector<uint8_t>& rgba) {
      g_guest.Put(set + 148, buf), g_guest.Put(movie + 192, buf);
      uint8_t* pl[3];
      for (uint32_t k = 0; k < 3; ++k) {
        pl[k] = g_guest.Find(ptr(buf, k))->bytes.data();
        std::fill(g_guest.Find(ptr(buf, k))->bytes.begin(), g_guest.Find(ptr(buf, k))->bytes.end(), uint8_t(0xEE));
      }
      std::vector<uint8_t> Y(EW * EH), cr(ECW * ECH), cb(ECW * ECH);
      for (uint32_t y = 0; y < EH; ++y)
        for (uint32_t x = 0; x < EW; ++x) {
          const uint32_t bar = x / 16, top = 8 + ((bar * 37 + frame * 53) % 100);   // lit from row `top` down
          Y[y * EW + x] = y >= top ? 200 : 20;
        }
      for (uint32_t y = 0; y < ECH; ++y)
        for (uint32_t x = 0; x < ECW; ++x) {
          const bool lit = Y[(y * 2) * EW + x * 2] == 200;
          cr[y * ECW + x] = lit ? 200 : 110, cb[y * ECW + x] = lit ? 100 : 190;
        }
      for (uint32_t y = 0; y < EH; ++y) std::memcpy(pl[0] + size_t(y) * EPY, &Y[y * EW], EW);
      for (uint32_t y = 0; y < ECH; ++y) std::memcpy(pl[1] + size_t(y) * EPC, &cr[y * ECW], ECW), std::memcpy(pl[2] + size_t(y) * EPC, &cb[y * ECW], ECW);   // plane 1 = cR, plane 2 = cB
      rgba.assign(size_t(EW) * EH * 4, 0);   // the independent convert: integer BT.601 limited range
      auto cl = [](int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
      for (uint32_t y = 0; y < EH; ++y)
        for (uint32_t x = 0; x < EW; ++x) {
          const int c = int(Y[y * EW + x]) - 16, d = int(cb[(y / 2) * ECW + x / 2]) - 128, e = int(cr[(y / 2) * ECW + x / 2]) - 128;
          uint8_t* o = &rgba[(size_t(y) * EW + x) * 4];
          o[0] = cl((298 * c + 409 * e + 128) >> 8), o[1] = cl((298 * c - 100 * d - 208 * e + 128) >> 8), o[2] = cl((298 * c + 516 * d + 128) >> 8), o[3] = 255;
        }
    };
    scr::PlaneStore eq;
    eq.plants.stale_screen_pic = Plant("stale_screen_pic");
    eq.binding.LoadDefaults(false);   // the SHIPPED table: 805 -> channel 2 @ 256x128
    scr::StorePlaneSource eps(&eq);
    rf::c::YuvScreenFeed efeed(&eps);
    uint32_t texw = 0, texh = 0;
    std::vector<uint8_t> tex;   // the live texture of 805 as the applier keeps it (it uploads only when the row's serial is new)
    uint32_t tex_seq = 0xFFFFFFFFu;
    auto flush_and_apply = [&](uint32_t buf, uint32_t frame, std::vector<uint8_t>& oracle, uint64_t gf) {
      decode(buf, frame, oracle);
      eq.OnFlush(&ReadThunk, set, gf);
      uint16_t keys[10];
      uint32_t se[10], ws[10], hs[10];
      const size_t n = eq.Rows(nullptr, gf, keys, se, ws, hs, 10);
      for (size_t i = 0; i < n; ++i) {
        if (keys[i] != 805 || se[i] == tex_seq) continue;   // rf_screens.cpp: `if (r.seq == sc->seq) continue`
        uint32_t w = 0, h = 0;
        const uint8_t* px = nullptr;
        if (efeed.Picture(805, se[i], w, h, px) && w == EW && h == EH) tex.assign(px, px + size_t(w) * h * 4), tex_seq = se[i], texw = w, texh = h;
      }
    };
    flush_and_apply(0, 1, want[0], 2000);
    const std::vector<uint8_t> tex1 = tex;
    flush_and_apply(1, 2, want[1], 2002);
    const std::vector<uint8_t> tex2 = tex;
    auto maxdiff = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
      if (a.size() != b.size() || a.empty()) return 999;
      int m = 0;
      for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
      return m;
    };
    const int d1 = maxdiff(tex1, want[0]), d2 = maxdiff(tex2, want[1]);
    const int moved = maxdiff(tex1, tex2) > 40 && want[0] != want[1];
    std::string why = "texture " + std::to_string(texw) + "x" + std::to_string(texh) + ": serial 1 vs the independent convert max diff " + std::to_string(d1) + ", serial 2 vs it " + std::to_string(d2) + (moved ? ", changed within one flush" : ", static picture (serial 2 equals serial 1)");
    Check(d1 <= 2 && d2 <= 2 && moved, "screens.pic_refresh", why);
    g_guest = saved;
  }

  // ---------------------------------------------------------------- 22. S103 SCF: the game's show bits (docs\research\s103\SCM.md): a screen / art slot is drawn when its stage record has bit 0x80 of byte +9; never black
  {
    constexpr uint32_t kSt = 0x41000000;   // a fake stage object: key records at +6008 + 12 * (key - 800), slot records at +1220 + 12 * slot
    const auto put_rec = [&](uint32_t rec, bool on) {
      g_guest.Put(rec, 0), g_guest.Put(rec + 4, 0), g_guest.Put(rec + 8, on ? 0x00800000u : 0u);   // byte +9 = bit 0x80 of the word at +8, big-endian (b8 b9 b10 b11)
    };
    const auto set_bits = [&](uint16_t keys, uint8_t art) {
      for (int k = 800; k <= 809; ++k)
        if (!(Plant("show_unread") && k == 805)) put_rec(kSt + 6008 + 12 * uint32_t(k - 800), (keys >> (k - 800)) & 1);   // plant: one key record unreadable = the whole read fails = the fixed sets by look
      for (int a = 301; a <= 304; ++a) put_rec(kSt + 1220 + 12 * uint32_t(a), (art >> (a - 301)) & 1);
    };
    const auto rd = [](uint32_t a, void* d, size_t n) { return g_guest.Read(a, d, n); };
    const uint16_t kOff = 1u << 0;                                              // the screens OFF branch: only 800 (the art slots stand where the screens would be)
    const uint16_t kOn = 1u << 0 | 1u << 1 | 1u << 5 | 1u << 6 | 1u << 8;      // the screens ON branch: 800 801 805 806 808 (art slots off)
    StageShow so, sn;
    set_bits(kOff, 0xF);
    const bool r_off = ReadStageShow(rd, kSt, so);
    set_bits(kOn, 0x0);
    const bool r_on = ReadStageShow(rd, kSt, sn);
    Check(Plant("show_unread") ? false : (r_off && r_on && so.known && so.keys == kOff && so.art == 0xF && sn.keys == kOn && sn.art == 0 && so.ArtShown(303) && !sn.ArtShown(303) && sn.KeyShown(805) && !so.KeyShown(805)),
          "show.read_bits", "off branch keys " + std::to_string(so.keys) + " art " + std::to_string(so.art) + "; on branch keys " + std::to_string(sn.keys) + " art " + std::to_string(sn.art));
    // the per-frame masks: the OFF branch hides every screen but 800 and shows the art; the ON branch hides the art and shows the screens; 805 is drawn from the Screens model only
    const auto ca = ArtCull(so), cn = ArtCull(sn), cs = ScreenCull(sn, false, false), ce = ScreenCull(sn, true, false), cso = ScreenCull(so, false, false), cse = ScreenCull(sn, true, true), css = ScreenCull(sn, false, true);
    Check(ca[301] == 0 && ca[302] == 0 && ca[303] == 0 && ca[304] == 0 && cn[301] == 3 && cn[302] == 3 && cn[303] == 3 && cn[304] == 3 && cso[801] == 3 && cso[800] == 0 && cs[801] == 0 && cs[802] == 3 && cs[805] == 0 && ce[805] == 3 && cse[805] == 0 && css[805] == 3 && cs[803] == 3,
          "show.masks", "off: art " + std::to_string(ca[301]) + std::to_string(ca[302]) + std::to_string(ca[303]) + std::to_string(ca[304]) + ", on: art " + std::to_string(cn[301]) + std::to_string(cn[304]) + ", 805 (screens/entrance) " + std::to_string(cs[805]) + std::to_string(ce[805]));
    // the rows and the fail-closed rule: 14 rows round-trip; a frame with none keeps the LAST bits (never all-on, never all-off)
    std::vector<StageFxRow> srows;
    ShowToRows(sn, srows);
    StageShow back = so;   // the applier's last bits (the OFF branch); the rows below replace them
    const bool got = ShowFromRows(srows, back);
    std::vector<StageFxRow> none;
    StageShow keep = sn;
    const bool got_none = ShowFromRows(none, keep);
    StageShow unread;
    std::vector<StageFxRow> unread_rows;
    ShowToRows(unread, unread_rows);   // known = false writes nothing
    Check(srows.size() == 14 && got && back == sn && !got_none && keep == sn && unread_rows.empty(), "show.rows_fail_closed", "rows " + std::to_string(srows.size()) + ", round trip " + std::to_string(int(back == sn)) + ", no rows keeps the last " + std::to_string(int(!got_none && keep == sn)));
    // the writer's gate follows the bits: every screen has a Movie and none has a picture row; the match look, no titantron. OFF branch: only 800 is shown (the front end's own) = complete. ON branch: 801 is shown with no row = NOT complete.
    {
      static int s_lk = 1;
      s_lk = 1;
      sfx::StageFxContributor con;
      con.SetReader(ReadThunk);
      con.SetStage(kSt);
      con.SetScreenGate(+[](uint32_t) -> bool { return true; }, +[](uint32_t) -> bool { return true; }, +[]() -> int { return s_lk; });
      SceneTables t;
      for (uint8_t k = 0; k < 10; ++k) {
        Target tg;
        tg.role = rdr_adapter::SurfaceRole::TronPicture;
        tg.index = k;
        t.targets.push_back(tg);
        Movie m;
        std::memset(static_cast<void*>(&m), 0, sizeof m);
        m.show = uint8_t(MovieShow::OnTarget);
        m.target = uint32_t(t.targets.size() - 1);
        t.movies.push_back(m);
      }
      const int bit = scene_inputs::kInStageScreens;
      size_t show_rows = 0;
      const auto run = [&](uint64_t frame) {
        rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].store(0);
        FrameInfo f;
        f.guest_frame = frame;
        con.Latch(f);
        Counters c;
        SceneTables tt = t;
        con.Emit(f, tt, c);
        show_rows = 0;
        for (const StageFxRow& r : tt.stage_fx) show_rows += r.kind == kFxShow;
        return rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].load();
      };
      set_bits(kOff, 0xF);
      const uint64_t g_off = run(2200);
      const size_t rows_off = show_rows;
      set_bits(kOn, 0x0);
      const uint64_t g_on = run(2201);
      set_bits(kOff, 0xF);
      run(2202);                                                    // read: the last bits are now the OFF branch
      g_guest.words.erase(kSt + 6008 + 8);                         // the key block stops reading
      const uint64_t g_keep = run(2203);                           // fail closed: the LAST bits (800 only) still decide = complete; the fixed match set would wait for 801
      const size_t rows_keep = show_rows;
      Check(g_keep != 0 && rows_keep == 14, "show.writer_keeps_last", "unreadable bits: stamp " + std::to_string((unsigned long long)g_keep) + " (want set: the last bits kept), rows " + std::to_string(rows_keep));
      set_bits(kOn, 0x0);
      run(2204);
      std::string cen;
      con.Census(cen);
      const bool census_ok = cen.find("[stage_show art 301:0 302:0 303:0 304:0, keys 800:1 801:1 802:0 803:0 804:0 805:1 806:1 807:0 808:1 809:0,") != std::string::npos;
      Check(g_off != 0 && g_on == 0 && (Plant("show_unread") || (rows_off == 14 && census_ok)), "show.gate_follows_bits",
            "screens-off branch stamp " + std::to_string((unsigned long long)g_off) + " (want set), screens-on branch with no 801 row " + std::to_string((unsigned long long)g_on) + " (want 0), show rows " + std::to_string(rows_off) + ", census " + (census_ok ? "ok" : "MISSING: " + cen.substr(cen.find("[stage_show") == std::string::npos ? 0 : cen.find("[stage_show"))));
    }
    Check(LegacyShow(false, false).art == 0xF && LegacyShow(false, true).art == 4 && !LegacyShow(false, true).known && LegacyShow(true, true).KeyShown(804) && !LegacyShow(true, true).KeyShown(800), "show.legacy_art",
          "an arena with no 801 / 802 draws every art slot; with them only 303 (the old loader's rule)");
    // the loader keeps the art slots in the LIVE model (all_keys): the game draws them whenever the match turns the screens off; plant: the old rule (erased)
    const bool live_erases = assets::load_detail::ErasesArtSlots(true, Plant("art_erased") ? false : true);
    const bool viewer_erases = assets::load_detail::ErasesArtSlots(true, false);
    Check(!live_erases && viewer_erases, "show.loader_keeps_art", std::string("live model erases 301 / 302 / 304: ") + (live_erases ? "yes (the pre-SCF rule: the art the game shows is gone)" : "no") + ", viewer: " + (viewer_erases ? "yes" : "no"));
    // a shown screen with no movie draws the live frame grab in the match, never black (the game's sub_8276A808); the entrance keeps the measured black; a screen with a movie draws its own
    const bool pb = Plant("idle_black");
    Check(rf::c::IdleDrawFor(false, true, pb) == rf::c::IdleDraw::FrameGrab && rf::c::IdleDrawFor(true, true, pb) == rf::c::IdleDraw::Black && rf::c::IdleDrawFor(false, false, pb) == rf::c::IdleDraw::Own,
          "show.idle_draw", std::string("match idle -> ") + (rf::c::IdleDrawFor(false, true, pb) == rf::c::IdleDraw::FrameGrab ? "frame grab" : "BLACK"));
  }

  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
