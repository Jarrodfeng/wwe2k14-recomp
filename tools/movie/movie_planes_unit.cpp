// movie_planes_unit: the offline proof of S103 lane P4, the movie planes taken from GUEST MEMORY at the game's plane flush (src\scene_live\movie_planes.h; docs\renderer\MOVIE_PLANES_GUEST.md). 0 game runs, no SDK, no game memory.
//   movie_planes_unit [--plant NAME] [--capture DIR]...
// It runs the SHIPPED code: the flush copy (movie_planes.h MovieStore::OnFlush over screen_planes_core.h CopyDisplay), the draw-order choice (Find, U3's census), the upload (PlaneImages) against a FAKE GUEST
// built from the set layout the listings give (screen_planes_core.h header). Part 1 is the synthetic fixture (a pattern oracle: bytes that are ours, not the game's). Part 2 (`--capture DIR`, repeatable) is the RIG:
// for every movie draw of a capture (call site 0x82BC7E50) the four plane images the STREAM bound are put into a fake guest as that frame's display buffer (pitch padded, the game's crossed data-plane order),
// the flush is called, the draw asks the store, and the planes the store serves AND the bytes PlaneImages hands the sink are compared with the stream images: max diff must be 0.
// [U] A capture holds no raw guest bytes (the stream's plane images are what the reader decoded OUT of guest memory), so this is a round trip of the copy / crossing / pitch / order / upload path on the
// capture's real plane bytes; it does not compare the live reader with the live flush copy (a live check: MOVIE_PLANES_GUEST.md section 6).
// A plant is a FAULT in the code under test (movie_planes.h Plants) or in the input; each must make a NAMED check fail (exit 1):
//   stale_plane       the previous flush of the set is served                                    -> "planes.fresh"
//   future_frame      a frame flushed AFTER the draw is served (U3 ignored)                      -> "order.flush_before_draw"
//   any_size          the draw's size is ignored (another movie's frame is served)               -> "select.size"
//   no_set_hint       the set hint is ignored (the newest flush of any set)                      -> "select.set"
//   swap_chroma       data planes 1 and 2 handed over the old way round                          -> "planes.chroma_order"
//   wrong_buffer      the other half of the display double buffer                                -> "planes.display_buffer"
//   upload_always     the upload re-sends a frame the images already hold (a shared frame is not shared) -> "upload.shared"
//   switch_on         g_guest_planes defaults to true                                            -> "switch.default_off"
//   rig_stale         (rig) each draw is given the previous draw's frame                         -> "rig.planes_equal"
//   no_ambiguity_guard  no set hint, two sets flushed together: the newest is served             -> "select.ambiguous_refused"
//   no_age_limit      a frame older than the window is served                                     -> "order.too_old"
//   leak_stream       the stream's (poison) image ids are handed out                              -> "guest.no_stream_image"
//   alpha_required    PlaneImages refuses a frame with no alpha plane (the S103 SVY bug: the season video drew black) -> "upload.no_alpha_served"
//   kind_guard_off    the served set's alpha-ness is not checked against the draw kind -> "upload.kind_guard"
//   unbounded         PlaneImages keeps every set forever                                         -> "upload.bounded"
//   key_ignored       (N2) the row's plane key is ignored: every row carries the last flush's set -> "select.key"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/scene_live/movie_planes.h"

namespace mp = rdr_scene::live::movieplanes;
namespace sfx = rdr_scene::live::stagefx;
namespace scr = rdr_scene::live::screens;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}
static std::string N(uint64_t v) { return std::to_string(v); }

// ---- a fake guest -----------------------------------------------------------------------------------------------------------------
struct Region {
  uint32_t base;
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
    for (Region& r : regions)
      if (r.base == base) {
        r.bytes.assign(n, 0);
        return r;
      }
    regions.push_back(Region{base, std::vector<uint8_t>(n, 0)});
    return regions.back();
  }
  Region* Find(uint32_t base) {
    for (Region& r : regions)
      if (r.base == base) return &r;
    return nullptr;
  }
  void Clear() { words.clear(), regions.clear(); }
};
static Guest g_guest;
static bool ReadThunk(uint32_t a, void* dst, size_t n) { return g_guest.Read(a, dst, n); }

// The display set layout (screen_planes_core.h): +128 count, +132/+136 Y size, +140/+144 chroma size, +148 display index, buffer b plane k at +152 + 12 k + 48 b (alloc, ptr, pitch).
struct SetSpec {
  uint32_t set, w, h, bufs, pitch_pad;
  bool alpha;
  uint32_t cw_h = 0, ch_h = 0;   // the header's chroma size when it is not (w+1)/2 x (h+1)/2 (the decoder pads rows)
};
static uint32_t Cw(const SetSpec& s) { return s.cw_h ? s.cw_h : (s.w + 1) / 2; }
static uint32_t Ch(const SetSpec& s) { return s.ch_h ? s.ch_h : (s.h + 1) / 2; }
static uint32_t PlanePtr(const SetSpec& s, uint32_t buf, uint32_t k) { return 0x50000000u + ((s.set >> 12) & 0x7) * 0x4000000u + buf * 0x1000000u + k * 0x200000u; }   // distinct ranges per set / buffer / plane
static uint32_t DW(const SetSpec& s, uint32_t k) { return (k == 0 || k == 3) ? s.w : Cw(s); }
static uint32_t DH(const SetSpec& s, uint32_t k) { return (k == 0 || k == 3) ? s.h : Ch(s); }
static uint32_t Pitch(const SetSpec& s, uint32_t k) { return DW(s, k) + s.pitch_pad; }
static void BuildSet(const SetSpec& s) {
  g_guest.Put(s.set + 128, s.bufs);
  g_guest.Put(s.set + 132, s.w), g_guest.Put(s.set + 136, s.h), g_guest.Put(s.set + 140, Cw(s)), g_guest.Put(s.set + 144, Ch(s));
  g_guest.Put(s.set + 148, 0);
  for (uint32_t b = 0; b < s.bufs; ++b)
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t base = s.set + 152 + 12 * k + 48 * b;
      const bool present = k < 3 || s.alpha;
      g_guest.Put(base, present ? 1 : 0), g_guest.Put(base + 4, present ? PlanePtr(s, b, k) : 0), g_guest.Put(base + 8, present ? Pitch(s, k) : 0);
      if (present) g_guest.Alloc(PlanePtr(s, b, k), size_t(Pitch(s, k)) * DH(s, k));
    }
}
// N2: the 2 x 4 plane handle keys of a set (word 0 of each pair at set + 64 b + 8 k).
static void PutKeys(const SetSpec& s, uint32_t k0) {
  for (uint32_t b = 0; b < 2; ++b)
    for (uint32_t k = 0; k < 4; ++k) g_guest.Put(s.set + 64 * b + 8 * k, k0 + 0x10 * b + k), g_guest.Put(s.set + 64 * b + 8 * k + 4, 0x70000000u + k0);
}
// What the decoder writes: data plane k (0 Y, 1 cR, 2 cB, 3 A) rows from `src[k]` (packed), the pitch padding a marker that must never be copied; then the display index.
static void Decode(const SetSpec& s, uint32_t buf, const std::vector<uint8_t> src[4]) {
  g_guest.Put(s.set + 148, buf);
  for (uint32_t k = 0; k < (s.alpha ? 4u : 3u); ++k) {
    Region* r = g_guest.Find(PlanePtr(s, buf, k));
    for (uint32_t y = 0; y < DH(s, k); ++y) {
      std::memcpy(r->bytes.data() + size_t(y) * Pitch(s, k), src[k].data() + size_t(y) * DW(s, k), DW(s, k));
      std::memset(r->bytes.data() + size_t(y) * Pitch(s, k) + DW(s, k), 0xEE, Pitch(s, k) - DW(s, k));
    }
  }
}
// The pattern oracle (bytes that are ours): value of (data plane k, x, y, frame tag).
static uint8_t Pat(uint32_t k, uint32_t x, uint32_t y, uint32_t tag) { return uint8_t(3 * x + 5 * y + 29 * k + 61 * tag + 7); }
static void Pattern(const SetSpec& s, uint32_t tag, std::vector<uint8_t> out[4]) {
  for (uint32_t k = 0; k < 4; ++k) {
    out[k].resize(size_t(DW(s, k)) * DH(s, k));
    for (uint32_t y = 0; y < DH(s, k); ++y)
      for (uint32_t x = 0; x < DW(s, k); ++x) out[k][size_t(y) * DW(s, k) + x] = Pat(k, x, y, tag);
  }
}
static bool Same(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0; }

// A fake image sink: records every upload.
struct FakeSink {
  struct Img {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bytes;
    bool live = false;
  };
  std::vector<Img>* imgs;
  uint32_t* updates;
  uint32_t Create(uint32_t w, uint32_t h) const {
    imgs->push_back(Img{w, h, {}, true});
    return uint32_t(imgs->size());
  }
  void Update(uint32_t id, const uint8_t* d, size_t n) const {
    (*imgs)[id - 1].bytes.assign(d, d + n);
    ++*updates;
  }
  void Destroy(uint32_t id) const { (*imgs)[id - 1].live = false; }
};

static void ApplyPlants(mp::MovieStore& st) {
  st.plants.stale_plane = Plant("stale_plane");
  st.plants.future_frame = Plant("future_frame");
  st.plants.any_size = Plant("any_size");
  st.plants.no_set_hint = Plant("no_set_hint");
  st.plants.swap_chroma = Plant("swap_chroma");
  st.plants.wrong_buffer = Plant("wrong_buffer");
  st.plants.half_chroma = Plant("half_chroma");
  st.plants.no_ambiguity_guard = Plant("no_ambiguity_guard");
  st.plants.no_age_limit = Plant("no_age_limit");
  st.plants.leak_stream = Plant("leak_stream");
  st.plants.key_ignored = Plant("key_ignored");
}

// ---- part 1: the synthetic fixture ------------------------------------------------------------------------------------------------
static void Synthetic() {
  g_guest.Clear();
  const SetSpec A{0x40400000, 64, 40, 2, 32, true}, B{0x40401000, 32, 16, 2, 16, true};   // two movies of different sizes
  BuildSet(A), BuildSet(B);
  mp::MovieStore st;
  ApplyPlants(st);
  if (Plant("switch_on")) mp::g_guest_planes.store(true);   // the plant: the shipped default is true
  Check(!mp::g_guest_planes.load(), "switch.default_off", "the guest-planes switch defaults to false (the player path is unchanged)");
  mp::g_guest_planes.store(false);

  std::vector<uint8_t> f1[4], f2[4], f3[4], fb[4];
  Pattern(A, 1, f1), Pattern(A, 2, f2), Pattern(A, 3, f3), Pattern(B, 9, fb);

  // 1. copy: Y, Cb (draw slot 1) = DATA plane 2, Cr (slot 2) = DATA plane 1; the buffer is the display index; the pitch padding is never copied; the copy is stable.
  Decode(A, 1, f1);
  const bool ok1 = st.OnFlush(&ReadThunk, A.set, 10);
  mp::Query q = mp::MakeQuery(mp::DrawHint{A.set, 0, 0, 0}, 10);
  std::shared_ptr<const scr::Frame> got = st.Find(q);
  Check(ok1 && got != nullptr, "planes.copied", "a flush of the display set stores a frame");
  if (got) {
    Check(Same(got->y, f1[0]) && Same(got->a, f1[3]), "planes.display_buffer", "Y and A are the display buffer's bytes, pitch padding dropped");
    Check(Same(got->cb, f1[2]) && Same(got->cr, f1[1]), "planes.chroma_order", "slot 1 (Cb) = data plane 2, slot 2 (Cr) = data plane 1 (the game's crossed dispatch)");
    std::vector<uint8_t> scribble(f2[0].size(), 0x5A);   // the decoder rewrites the buffer after the flush
    for (uint32_t b = 0; b < 2; ++b)
      for (uint32_t k = 0; k < 4; ++k)
        if (Region* r = g_guest.Find(PlanePtr(A, b, k))) std::fill(r->bytes.begin(), r->bytes.end(), uint8_t(0x5A));
    Check(Same(got->y, f1[0]) && Same(got->cb, f1[2]), "planes.stable", "the copy does not change when the decoder rewrites the guest planes");
  }

  {   // a movie whose chroma textures are taller than (h+1)/2 (1280x280 -> 640x144 chroma): the copy has the header's size, so the draw's uv (the whole texture) is not rescaled
    const SetSpec E{0x40403000, 64, 36, 2, 16, true, 32, 24};
    BuildSet(E);
    std::vector<uint8_t> fe[4];
    Pattern(E, 6, fe);
    Decode(E, 0, fe);
    st.OnFlush(&ReadThunk, E.set, 5);
    const mp::Query qe = mp::MakeQuery(mp::DrawHint{E.set, 0, 0, 0}, 5);
    auto ge = st.Find(qe);
    Check(ge && ge->cw == 32 && ge->ch == 24 && Same(ge->cb, fe[2]) && Same(ge->cr, fe[1]), "planes.chroma_size", "chroma planes are the header's 32x24, not (w+1)/2 x (h+1)/2 = 32x18");
  }
  // 2. fresh + order (U3). Flush 2 at gf 12 (buffer 0), flush 3 at gf 14 (buffer 1).
  Decode(A, 0, f2);
  st.OnFlush(&ReadThunk, A.set, 12);
  Decode(A, 1, f3);
  st.OnFlush(&ReadThunk, A.set, 14);
  q.guest_frame = 12;
  auto at12 = st.Find(q);
  Check(at12 && Same(at12->y, f2[0]) && Same(at12->cb, f2[2]), "planes.fresh", "a draw at guest frame 12 gets the frame flushed at 12 (not the one before)");
  q.guest_frame = 13;
  auto at13 = st.Find(q);
  Check(at13 && Same(at13->y, f2[0]), "order.flush_before_draw", "a draw at 13 gets the flush at 12, never the one at 14 that came after it");
  q.guest_frame = 14;
  auto at14 = st.Find(q);
  Check(at14 && Same(at14->y, f3[0]), "order.newest_at_its_frame", "a draw at 14 gets the flush at 14");
  q.guest_frame = 9;   // before every flush of the set
  auto at9 = st.Find(q);
  Check(!at9, "order.no_flush_yet", "a draw before the set's first flush gets nothing (the stream's draw is kept)");
  // exact order by serial: the draw was stamped when 2 flushes had happened (the flush at 12 was the 2nd): the flush at 14 (3rd) is after it.
  mp::Query qs = mp::MakeQuery(mp::DrawHint{A.set, 0, 0, 3}, 20);   // flushes so far: A@10 (1), E@5 (2), A@12 (3), A@14 (4)
  auto bys = st.Find(qs);
  Check(bys && Same(bys->y, f2[0]), "order.by_serial", "a draw stamped after the 3rd flush gets the flush at 12's frame although it is on a later guest frame");
  const mp::Census c = st.census();
  Check(c.flushes == 4 && c.copied == 4, "census.flushes", "flushes " + N(c.flushes) + ", copied " + N(c.copied));
  Check(c.no_flush == 1 && c.flush_after >= 1 && c.flush_before >= 2 && c.flush_same == 0 + c.flush_same, "census.u3", "no_flush " + N(c.no_flush) + ", flush_after " + N(c.flush_after) + ", flush_before " + N(c.flush_before) + ", flush_same " + N(c.flush_same));

  // 3. selection among two sets.
  Decode(B, 0, fb);
  st.OnFlush(&ReadThunk, B.set, 15);
  const mp::Query by_size = mp::MakeQuery(mp::DrawHint{0, 64, 40, 0}, 16);
  auto a_by_size = st.Find(by_size);
  Check(a_by_size && a_by_size->w == 64, "select.size", "the size picks the 64x40 movie though the 32x16 one flushed later");
  const mp::Query by_set = mp::MakeQuery(mp::DrawHint{A.set, 0, 0, 0}, 16);
  auto a_by_set = st.Find(by_set);
  Check(a_by_set && a_by_set->set == A.set && a_by_set->w == 64, "select.set", "the set hint picks its own set although another set flushed later");

  {   // N2: the 30 Years menu: three movies flush every frame in slot order, so every row is stamped with the LAST flush's set. A row picks its set by its own plane key.
    mp::MovieStore s3;
    ApplyPlants(s3);
    g_guest.Clear();
    const SetSpec S0{0x40420000, 32, 16, 2, 0, true}, S1{0x40421000, 64, 16, 2, 0, false}, S2{0x40422000, 64, 16, 2, 0, false};   // s0 = 0502 (alpha), s1 / s2 = 0500 / 0501 (no alpha)
    BuildSet(S0), BuildSet(S1), BuildSet(S2);
    PutKeys(S0, 0x1000), PutKeys(S1, 0x2000), PutKeys(S2, 0x3000);
    std::vector<uint8_t> g0[4], g1[4], g2[4];
    Pattern(S0, 11, g0), Pattern(S1, 12, g1), Pattern(S2, 13, g2);
    Decode(S0, 0, g0), Decode(S1, 0, g1), Decode(S2, 0, g2);
    s3.OnFlush(&ReadThunk, S0.set, 200), s3.OnFlush(&ReadThunk, S1.set, 200), s3.OnFlush(&ReadThunk, S2.set, 200);
    mp::DrawHint h0{S2.set, 0, 0, 0, 0x1000}, h1{S2.set, 0, 0, 0, 0x2000}, h2{S2.set, 0, 0, 0, 0x3000};   // every row's hint = the last flush's set
    auto r0 = s3.Find(mp::MakeQuery(h0, 200)), r1 = s3.Find(mp::MakeQuery(h1, 200)), r2 = s3.Find(mp::MakeQuery(h2, 200));
    mp::DrawHint hm{S2.set, 0, 0, 0, 0x9999};   // a key no set holds: today's rule (the hint), counted
    auto rm = s3.Find(mp::MakeQuery(hm, 200));
    const bool alpha_ok = r0 && r0->set == S0.set && r0->has_alpha;   // the alpha row is served the alpha set: SetMatchesKind would pass it
    Check(alpha_ok && r1 && r1->set == S1.set && r2 && r2->set == S2.set && Same(r1->y, g1[0]) && Same(r2->y, g2[0]), "select.key", "three rows, one hint (the last flush's set): each served its own set by its plane key; the alpha row from the alpha set");
    Check(rm && rm->set == S2.set && s3.census().key_miss == 1 && s3.census().key_hit == 3 && s3.census().key_hint_differs == 2, "select.key_fallback", "a key no set holds falls back to the hint and is counted (key_hit " + N(s3.census().key_hit) + ", key_miss " + N(s3.census().key_miss) + ", differs " + N(s3.census().key_hint_differs) + ")");
    const bool refused = r0 && !mp::SetMatchesKind(true, !r0->has_alpha);
    Check(!refused, "select.key_alpha_row_not_refused", "the alpha row over its own set passes the kind guard (the old hint refused it)");
    {   // N2 F6: a row with no hint that finds its set by key is not "the hint was wrong"
      const uint64_t d0 = s3.census().key_hint_differs;
      auto nh = s3.Find(mp::MakeQuery(mp::DrawHint{0, 0, 0, 0, 0x2000}, 200));
      Check(nh && nh->set == S1.set && s3.census().key_hint_differs == d0, "select.key_no_hint_not_differs", "no hint + key: served by key, key_hint_differs unchanged");
      Check(s3.SetOfKey(0x2010) == S1.set && s3.SetOfKey(0x1003) == S0.set && s3.SetOfKey(0x9999) == 0 && s3.SetOfKey(0) == 0, "select.set_of_key", "either buffer's key names its set (SetOfKey), an unknown key and 0 name none");
    }
    {   // N2 F5: only the buffers the set has (count at +128): a one-buffer set's second key words are stale memory
      const SetSpec S3{0x40423000, 32, 16, 1, 0, false};
      BuildSet(S3);
      PutKeys(S3, 0x4000);
      std::vector<uint8_t> g3[4];
      Pattern(S3, 14, g3);
      Decode(S3, 0, g3);
      s3.OnFlush(&ReadThunk, S3.set, 200);
      const uint64_t m0 = s3.census().key_miss;
      auto stale = s3.Find(mp::MakeQuery(mp::DrawHint{S2.set, 0, 0, 0, 0x4010}, 200));   // buffer 1's key of a 1-buffer set
      Check(s3.census().key_miss == m0 + 1 && s3.SetOfKey(0x4000) == S3.set && s3.SetOfKey(0x4010) == 0, "select.key_buffers_by_count", "a one-buffer set holds only buffer 0's keys (the stale words are not read)");
      (void)stale;
    }
  }
  // 4. the upload.
  std::vector<FakeSink::Img> imgs;
  uint32_t updates = 0;
  mp::PlaneImages<FakeSink> images(FakeSink{&imgs, &updates});
  images.plant_always_upload = Plant("upload_always");
  images.plant_alpha_required = Plant("alpha_required");
  mp::PlaneImages<FakeSink>::Ids ids;
  const bool up1 = a_by_set && images.Get(a_by_set->set, *a_by_set, ids);
  Check(up1 && imgs.size() == 4 && updates == 4, "upload.created", "four images, four uploads");
  if (up1) {
    Check(Same(imgs[ids.id[0] - 1].bytes, a_by_set->y) && Same(imgs[ids.id[1] - 1].bytes, a_by_set->cb) && Same(imgs[ids.id[2] - 1].bytes, a_by_set->cr) && Same(imgs[ids.id[3] - 1].bytes, a_by_set->a),
          "upload.bytes", "the slots' images hold Y, Cb, Cr, A");
    mp::PlaneImages<FakeSink>::Ids again;
    images.Get(a_by_set->set, *a_by_set, again);
    Check(updates == 4 && again.id[0] == ids.id[0], "upload.shared", "the same frame again: no second upload, the same images");
    const mp::Query q12 = mp::MakeQuery(mp::DrawHint{A.set, 0, 0, 0}, 12);
    auto o = st.Find(q12);
    if (o) images.Get(o->set, *o, again);
    Check(o && updates == 8 && Same(imgs[ids.id[0] - 1].bytes, o->y) && images.census().reuploads == 1, "upload.older_frame", "a draw that needs an OLDER frame re-uploads it (counted)");
    {   // review F6: the guest path hands out OUR images only; the stream's image ids (poison here) are a bind requirement, never an output
      const uint32_t poison[4] = {9001, 9002, 9003, 9004};
      mp::GuestSlot gs[4];
      const mp::Serve r = mp::ServeGuest(st, images, by_set, poison, false, gs);
      bool leaked = false, ours = true;
      for (int s = 0; s < 4; ++s) {
        for (uint32_t p : poison) leaked = leaked || gs[s].image == p;
        ours = ours && gs[s].image != 0 && gs[s].image <= imgs.size();
      }
      Check(r == mp::Serve::Ok && !leaked && ours, "guest.no_stream_image", "four slots hold our images, none of the stream's poison ids 9001..9004");
      const uint32_t missing[4] = {9001, 0, 9003, 9004};
      Check(mp::ServeGuest(st, images, by_set, missing, false, gs) == mp::Serve::NoStreamSlot, "guest.stream_slot_required", "a slot with no stream image is refused (the stream's draw is kept)");
    }
  }
  {   // review F1: an entrance flushes several movies together; with no set hint the draw is REFUSED, a size or a hint picks one
    mp::MovieStore s2;
    ApplyPlants(s2);
    g_guest.Clear();
    const SetSpec X{0x40410000, 32, 16, 2, 0, true}, Y{0x40411000, 16, 16, 2, 0, true};
    BuildSet(X), BuildSet(Y);
    std::vector<uint8_t> fx[4], fy[4];
    Pattern(X, 1, fx), Pattern(Y, 2, fy);
    Decode(X, 0, fx), Decode(Y, 0, fy);
    s2.OnFlush(&ReadThunk, X.set, 100), s2.OnFlush(&ReadThunk, Y.set, 101);
    const auto none = s2.Find(mp::MakeQuery(mp::DrawHint{}, 101));
    Check(!none && s2.census().ambiguous == 1, "select.ambiguous_refused", "no hint, two sets flushed within 2 frames: no frame served, counted (ambiguous " + N(s2.census().ambiguous) + ")");
    const auto by_sz = s2.Find(mp::MakeQuery(mp::DrawHint{0, 32, 16, 0}, 101));
    Check(by_sz && by_sz->set == X.set, "select.size_unique", "the Y size alone picks the one candidate");
    // review F4: the served frame may not be older than the window (no hint) / kIdleFrames (hint). X flushes at 100 and 110; a draw at 108 would be served the frame of 100.
    Decode(X, 1, fx);
    s2.OnFlush(&ReadThunk, X.set, 110);
    const auto old_nohint = s2.Find(mp::MakeQuery(mp::DrawHint{0, 32, 16, 0}, 108));
    Check(!old_nohint && s2.census().too_old == 1, "order.too_old", "no hint: a frame 8 guest frames old is not served (too_old " + N(s2.census().too_old) + ")");
    s2.OnFlush(&ReadThunk, X.set, 300);
    const auto old_hint = s2.Find(mp::MakeQuery(mp::DrawHint{X.set, 0, 0, 0}, 299));
    Check(!old_hint, "order.too_old_hinted", "with a hint: a frame 189 guest frames old is not served either");
  }
  {   // review F7: the image sets are bounded like the store's (kMaxSets)
    std::vector<FakeSink::Img> bi;
    uint32_t bu = 0;
    mp::PlaneImages<FakeSink> bounded(FakeSink{&bi, &bu});
    bounded.plant_unbounded = Plant("unbounded");
    for (uint32_t k = 0; k < mp::kMaxSets + 3; ++k) {
      scr::Frame fr;
      fr.set = 0x40500000u + k * 0x1000, fr.w = 2, fr.h = 2, fr.cw = 1, fr.ch = 1, fr.id = k + 1, fr.has_alpha = true;
      fr.y.assign(4, 1), fr.cb.assign(1, 2), fr.cr.assign(1, 3), fr.a.assign(4, 4);
      mp::PlaneImages<FakeSink>::Ids bid;
      bounded.Get(fr.set, fr, bid);
    }
    size_t live = 0;
    for (const auto& im : bi) live += im.live ? 1 : 0;
    Check(bounded.sets() <= mp::kMaxSets && live <= mp::kMaxSets * 4, "upload.bounded", "sets held " + N(bounded.sets()) + " (max " + N(mp::kMaxSets) + "), live images " + N(live));
  }
  {   // a frame with no alpha plane cannot be shown; a pooled set reopened with a movie of another size recreates the images
    const SetSpec C{0x40402000, 16, 8, 2, 0, false};
    BuildSet(C);
    std::vector<uint8_t> fc[4];
    Pattern(C, 4, fc);
    Decode(C, 0, fc);
    st.OnFlush(&ReadThunk, C.set, 30);
    mp::Query qc;
    qc.set = C.set, qc.guest_frame = 30;
    auto fr = st.Find(qc);
    mp::PlaneImages<FakeSink>::Ids idc;
    const size_t before_c = imgs.size();
    const uint32_t updates_c = updates;
    const bool got_c = fr && !fr->has_alpha && images.Get(C.set, *fr, idc);
    Check(got_c && !idc.alpha && idc.id[3] == idc.id[0] && imgs.size() == before_c + 3 && updates == updates_c + 3 && Same(imgs[idc.id[0] - 1].bytes, fr->y) && Same(imgs[idc.id[2] - 1].bytes, fr->cr),
          "upload.no_alpha_served", "S103 SVY: a frame with no alpha plane (the season video, T_SpriteYUV) is served with three images and no alpha upload; slot 3 is the Y image again (the stand-in), Ids::alpha false");
    if (got_c) {
      const uint32_t ids_c[4] = {idc.id[0], idc.id[1], idc.id[2], idc.id[3]};
      Check(images.Holds(C.set, ids_c), "upload.no_alpha_holds", "Holds accepts the stand-in as the no-alpha set's 4th slot");
      mp::GuestSlot gs_c[4];
      const uint32_t ph_c[4] = {9001, 9002, 9003, 9004};
      const mp::Serve rs = mp::ServeGuest(st, images, qc, ph_c, false, gs_c);
      Check(mp::SetMatchesKind(true, true) == false && mp::SetMatchesKind(false, true) && mp::SetMatchesKind(true, false) && !mp::SetMatchesKind(false, false) && mp::SetMatchesKind(false, false, true) == true, "upload.kind_guard", "an Alpha row over a no-alpha set and a no-alpha row over an alpha set are mismatches; the matching pairs pass");
      Check(mp::SetMatchesKind(true, gs_c[3].stand_in, Plant("kind_guard_off")) == false, "upload.kind_guard", "the host/movie_ours guard refuses the Alpha draw served the no-alpha set (twin kind_guard_off: no guard)");
      Check(rs == mp::Serve::Ok && gs_c[3].stand_in && gs_c[0].stand_in && gs_c[3].image == gs_c[0].image, "upload.no_alpha_standin", "ServeGuest marks slot 3 of a no-alpha set as the stand-in (an Alpha row over it is refused by the host)");
    }
    const SetSpec A2{A.set, 32, 16, 2, 8, true};   // the pooled set block reopened as a 32x16 movie
    BuildSet(A2);
    std::vector<uint8_t> fz[4];
    Pattern(A2, 5, fz);
    Decode(A2, 0, fz);
    st.OnFlush(&ReadThunk, A2.set, 31);
    mp::Query qa;
    qa.set = A2.set, qa.guest_frame = 31;
    auto fr2 = st.Find(qa);
    const size_t before = imgs.size();
    mp::PlaneImages<FakeSink>::Ids id2;
    const bool ok2 = fr2 && up1 && images.Get(A2.set, *fr2, id2);
    Check(ok2 && fr2->w == 32 && imgs.size() == before + 4 && !imgs[ids.id[0] - 1].live && imgs[id2.id[0] - 1].w == 32, "upload.resized", "a pooled set reopened with another size: old images destroyed, four new ones");
  }
}

// ---- part 2: the rig on a capture ---------------------------------------------------------------------------------------------------
struct RigResult {
  uint64_t draws = 0, flushes = 0, compared = 0, skipped = 0, bytes = 0, maxdiff_store = 0, maxdiff_upload = 0, mismatch_frames = 0;
};
static RigResult Rig(const std::string& dir) {
  RigResult r;
  rdr_adapter::CaptureReader cap;
  if (!cap.Open(dir)) {
    Check(false, "rig.open", dir + ": cannot open");
    return r;
  }
  std::unordered_map<uint32_t, rdr_adapter::ContentRef> content;   // image -> its newest content as of the current frame
  mp::MovieStore st;
  ApplyPlants(st);
  std::vector<FakeSink::Img> imgs;
  uint32_t updates = 0;
  mp::PlaneImages<FakeSink> images(FakeSink{&imgs, &updates});
  const uint32_t kSet = 0x41000000;
  uint64_t last_key[4] = {};   // the offset of each plane's content at the last flush: a flush only when the decoder wrote a new frame
  bool built = false;
  uint32_t cur_w = 0, cur_h = 0, buf = 0;
  std::shared_ptr<const scr::Frame> prev_served;   // (rig_stale)
  rdr_adapter::LoadedFrame f;
  for (uint32_t fi = 0; cap.LoadFrame(fi, f); ++fi) {
    for (uint32_t i = 0; i < f.contents.count; ++i) content[f.contents[i].image] = f.contents[i];
    for (uint32_t d = 0; d < f.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = f.OriginOf(d);
      if (!o || o->klass != rdr_adapter::DrawClass::Movie || o->call_site != 0x82BC7E50u) continue;
      ++r.draws;
      const rdr_adapter::Draw& dr = f.draws[d];
      uint32_t img[4] = {};
      for (uint32_t t = 0; t < dr.texture_count; ++t) {
        const rdr_adapter::TextureSlot& ts = f.texture_slots[dr.texture_first + t];
        if (ts.slot < 4) img[ts.slot] = ts.image;
      }
      rdr_adapter::LoadedTexture tex[4];
      std::vector<uint8_t> plane[4];
      uint32_t pw[4] = {}, ph[4] = {};
      bool ok = true;
      for (int s = 0; s < 4 && ok; ++s) {
        auto it = content.find(img[s]);
        if (!img[s] || it == content.end() || !cap.ReadTexture(it->second, tex[s]) || tex[s].header.subresource_count < 1) { ok = false; break; }
        const rdr_adapter::Subresource& sr = tex[s].subresources[0];
        if (sr.row_bytes != sr.width || sr.rows != sr.height) { ok = false; break; }
        pw[s] = sr.width, ph[s] = sr.height;
        plane[s].assign(tex[s].data + sr.offset, tex[s].data + sr.offset + size_t(sr.width) * sr.height);
      }
      if (!ok || pw[1] != pw[2] || ph[1] != ph[2] || pw[0] != pw[3] || ph[0] != ph[3] || pw[1] < (pw[0] + 1) / 2 || ph[1] < (ph[0] + 1) / 2) {
        if (++r.skipped <= 2) std::printf("rig skip (draw %u frame %u): ok %d sizes Y %ux%u Cb %ux%u Cr %ux%u A %ux%u images %u %u %u %u\n", d, fi, int(ok), pw[0], ph[0], pw[1], ph[1], pw[2], ph[2], pw[3], ph[3], img[0], img[1], img[2], img[3]);
        continue;
      }
      uint64_t key[4];
      for (int s = 0; s < 4; ++s) key[s] = content[img[s]].offset;
      const bool new_frame = !built || pw[0] != cur_w || ph[0] != cur_h || std::memcmp(key, last_key, sizeof key) != 0;
      if (new_frame) {   // the decoder wrote a new picture into the display buffer: the game's crossed order (data plane 1 = Cr = draw slot 2, data plane 2 = Cb = draw slot 1), then the flush
        SetSpec s{kSet, pw[0], ph[0], 2, 64, true, pw[1], ph[1]};
        if (!built || pw[0] != cur_w || ph[0] != cur_h) BuildSet(s), buf = 0;
        buf ^= 1;
        std::vector<uint8_t> data[4] = {plane[0], plane[2], plane[1], plane[3]};
        Decode(s, buf, data);
        st.OnFlush(&ReadThunk, kSet, f.header.guest_frame);
        ++r.flushes;
        built = true, cur_w = pw[0], cur_h = ph[0];
        std::memcpy(last_key, key, sizeof key);
      }
      const mp::Query q = mp::MakeQuery(mp::DrawHint{kSet, pw[0], ph[0], st.seq()}, f.header.guest_frame);   // a recorder's stamp: the serial when the draw was recorded
      std::shared_ptr<const scr::Frame> got = st.Find(q);
      if (Plant("rig_stale") && prev_served) {
        std::shared_ptr<const scr::Frame> keep = got;
        got = prev_served;
        prev_served = keep;
      } else {
        prev_served = got;
      }
      if (!got) { ++r.mismatch_frames; continue; }
      mp::PlaneImages<FakeSink>::Ids ids;
      const bool up = images.Get(kSet, *got, ids);
      const std::vector<uint8_t>* served[4] = {&got->y, &got->cb, &got->cr, &got->a};
      ++r.compared;
      bool bad = !up;
      for (int s = 0; s < 4 && up; ++s) {
        r.bytes += plane[s].size();
        if (served[s]->size() != plane[s].size() || imgs[ids.id[s] - 1].bytes.size() != plane[s].size()) { bad = true; continue; }
        for (size_t b = 0; b < plane[s].size(); ++b) {
          const uint64_t d1 = uint64_t(std::abs(int((*served[s])[b]) - int(plane[s][b]))), d2 = uint64_t(std::abs(int(imgs[ids.id[s] - 1].bytes[b]) - int(plane[s][b])));
          r.maxdiff_store = std::max(r.maxdiff_store, d1), r.maxdiff_upload = std::max(r.maxdiff_upload, d2);
        }
      }
      if (bad) ++r.mismatch_frames;
    }
  }
  const mp::Census c = st.census();
  std::printf("rig %s: movie draws %llu, flushes %llu, compared %llu, skipped %llu, bytes %llu; store max diff %llu, upload max diff %llu; U3 before %llu same %llu after %llu no_flush %llu\n", dir.c_str(),
              (unsigned long long)r.draws, (unsigned long long)r.flushes, (unsigned long long)r.compared, (unsigned long long)r.skipped, (unsigned long long)r.bytes, (unsigned long long)r.maxdiff_store,
              (unsigned long long)r.maxdiff_upload, (unsigned long long)c.flush_before, (unsigned long long)c.flush_same, (unsigned long long)c.flush_after, (unsigned long long)c.no_flush);
  return r;
}

int main(int argc, char** argv) {
  std::vector<std::string> captures;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--plant") == 0 && i + 1 < argc) g_plant = argv[++i];
    else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) captures.push_back(argv[++i]);
  }
  Synthetic();
  for (const std::string& dir : captures) {
    const RigResult r = Rig(dir);
    Check(r.compared > 0, "rig.has_movie", dir + ": " + N(r.compared) + " movie draws compared of " + N(r.draws) + " (skipped " + N(r.skipped) + ")");
    Check(r.mismatch_frames == 0 && r.maxdiff_store == 0 && r.maxdiff_upload == 0, "rig.planes_equal", "planes from the (fake) guest bytes at the flush vs the stream images: max diff " + N(std::max(r.maxdiff_store, r.maxdiff_upload)) + ", unmatched draws " + N(r.mismatch_frames));
  }
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
