// Session 103, landing 2, piece PN2 (lane PN23): the unit proof of PictureSource (src\scene_live\picture_source.h), 0 game runs.
//
//   picture_source_unit.exe --game DIR --cache DIR --captures DIR [--only c2d_menu,c2d_play,...] [--plant NAME]
//
// For every textured sprite / glyph row of the c2d_* captures (the 2D corpus: menu, play, options, select, title, load ...) the unit builds FORGED GUEST MEMORY from the capture's own texture blob of the row's
// image (the handle registry at 0x8392DD9C, the wrapper, the library header, the six fetch dwords, the pixel bytes tiled and endian-swapped with the reader's own TiledOffset2D / SwapBlock: the I2 unit
// proved those against a written-out layout), runs the game's events in order (OnBorn at the first sight of a key, Seen on every row) and then asks PictureSource for the answer. The reference is the
// STREAM path: UiCore::Decide over the same rows (the join the stream-built UiOurs made). Per capture: joined / guest-copied / refused, and mismatches (bar 0):
//   a row UiCore joined must be answered File with the SAME index entry (or an entry holding the same bytes, counted `equiv`);
//   a row UiCore left unjoined must be answered Guest with blocks byte-equal to the capture image's mip 0 (or refused by NAME: the 1 MB read cap, guest_tex::Why);
//   a hit never reads or hashes again (reads == first sights).
// Synthetic checks (no capture): death, reused slot (the birth counter), render-target creator, movie creator, the refill guard (the S100 stale roster render: new bytes under a live key).
// Review A1-A8 (lane PN23F) synthetic checks: RACE (a read in flight when the key dies stores a dead answer, trimmed), SWZ (a 4-byte picture is read only with the BGRA identity fetch swizzle, joins only a BGRA8
// file), UNCLASS (an Unknown creator's 4-byte picture is refused), BUDGET / RELEASE (resident cap, blocks freed after the upload), PARITY (the stream hash comparison), PHYS (the physical range bound).
// Planted twins (--plant NAME; each must fail its NAMED check): death_ignored [DEATH], counter_ignored [COUNTER], rehash_every_hit [REHASH], rt_as_file [RT], wrong_mip [JOIN], no_swap [JOIN], tiled_ignored [JOIN],
// race_ignored [RACE], swizzle_ignored [SWZ], unknown_8888 [UNCLASS], file_any_order [SWZ], no_budget [BUDGET], keep_blocks [RELEASE].
// Lines: PASS|FAIL [TAG] detail. Exit 0 ok, 1 a check failed, 2 an input is missing.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include "../../src/renderer_scene/ui_ours_core.h"
#include "../../src/scene_live/picture_source.h"
#include "picture_forge.h"

using namespace rdr_scene;
namespace ps = rdr_scene::live::picsrc;
namespace gt = rdr_scene::live::guest_tex;
namespace particles = rdr_scene::live::particles;
using rdr_adapter::CaptureReader;
using rdr_adapter::ContentRef;
using rdr_adapter::LoadedFrame;
using rdr_adapter::LoadedTexture;

namespace {

std::string g_failed;
int g_checks = 0, g_failures = 0;
void Check(bool ok, const char* tag, const std::string& detail) {
  ++g_checks;
  std::printf("%s  [%s] %s\n", ok ? "PASS" : "FAIL", tag, detail.c_str());
  if (!ok) {
    ++g_failures;
    if (g_failed.find(tag) == std::string::npos) g_failed += std::string(g_failed.empty() ? "" : ", ") + "[" + tag + "]";
  }
}
std::string N(unsigned long long v) { return std::to_string(v); }

// ---- the forged guest memory ---------------------------------------------------------------------------------------------------------------------------
using picforge::Forge;   // S103 PN4: the forged guest memory lives in picture_forge.h (the host rig uses it too)

// The stream path's reference: a provider that hands UiCore the index entry's own shape (no GPU, no loader: the content is the index's).
class RefProvider : public UiTextureProvider {
 public:
  explicit RefProvider(const UiFileIndex& index) : index_(index) {}
  const UiTexture* Texture(uint32_t entry_id, const UiFileEntry& e, uint32_t mips, bool& pending) override {
    pending = false;
    const uint64_t k = uint64_t(entry_id) | uint64_t(mips) << 32;
    UiTexture& t = tex_[k];
    t.handle = entry_id + 1, t.width = e.width, t.height = e.height, t.mips = mips, t.format = e.format, t.prefix = e.prefix;
    return &t;
  }

 private:
  const UiFileIndex& index_;
  std::map<uint64_t, UiTexture> tex_;
};

struct Args {
  std::string game, cache, captures, only, plant;
};

bool IndexTier1(const Args& a, UiFileIndex& index) {
  UiIndexOptions o;
  o.game_root = a.game, o.cache_dir = a.cache;
  for (uint8_t pac = 0; pac <= uint8_t(kUiBgEtc); ++pac) {   // tier 1 + bgEtc (the second tier the entrance / match pictures need: UiOurs starts it on demand)
    std::vector<UiFileEntry> entries;
    std::string err;
    if (!BuildPacIndex(o, pac, entries, err)) {
      std::printf("      %s: %s\n", UiPacPath(pac).c_str(), err.c_str());
      return false;
    }
    index.AddPac(pac, std::move(entries));
  }
  std::printf("      index: %s\n", index.Summary().c_str());
  return index.texture_count() > 0;
}

// ---- one capture ----------------------------------------------------------------------------------------------------------------------------------------
struct CapResult {
  uint64_t rows = 0, keys = 0, joined_same = 0, joined_equiv = 0, guest_ok = 0, guest_bad = 0, mismatch = 0, file_only = 0, skipped = 0, size_risk = 0;
  std::map<std::string, uint64_t> refused;
  ps::Counters c;
};

bool RunCapture(const Args& a, const std::string& name, const UiFileIndex& index, CapResult& r) {
  CaptureReader capture;
  if (!capture.Open(a.captures + "\\" + name)) return std::fprintf(stderr, "cannot open the capture %s\n", name.c_str()), false;
  rdr_adapter::ImageContentTable table;
  RefProvider provider(index);
  UiFaults faults;
  UiCore core(faults, table.Supplier(), &index);
  ps::PictureSource src(&index);
  Forge forge;
  std::map<uint32_t, ContentRef> latest;
  std::map<uint32_t, uint32_t> key_image;   // key -> the capture image its forged texture holds
  std::map<uint32_t, uint64_t> key_gen;
  LoadedFrame frame;
  LoadedTexture scratch, blob;
  const auto phys = [&](uint32_t addr, uint32_t n, uint8_t* dst) { return forge.phys(addr, n, dst); };
  const uint32_t step = std::max(1u, capture.frame_count() / 3);   // the census used frames n/4, n/2, 3n/4: three frames of the capture
  std::set<uint32_t> frames_wanted = {capture.frame_count() / 4, capture.frame_count() / 2, capture.frame_count() * 3 / 4};
  (void)step;
  uint64_t first_sights_expected = 0;
  int shown = 0;
  for (uint32_t f = 0; f < capture.frame_count(); ++f) {
    if (!capture.LoadFrame(f, frame)) return std::fprintf(stderr, "cannot read frame %u of %s\n", f, name.c_str()), false;
    rdr_adapter::FeedImageContents(frame, capture, table, scratch, true);
    for (const ContentRef& ref : frame.contents) latest[ref.image] = ref;
    if (!frames_wanted.count(f)) continue;
    core.BeginFrame();
    src.TrimDrawn(f);
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const rdr_adapter::DrawOrigin* origin = frame.OriginOf(i);
      if (!origin || !core.Claims(*origin)) continue;
      const rdr_adapter::Draw& d = frame.draws[i];
      uint32_t image = 0, view = 0, minl = 0, maxl = 15;
      const uint8_t* swz = nullptr;
      for (uint32_t t = 0; t < d.texture_count && d.texture_first + t < frame.texture_slots.count; ++t) {
        const auto& ts = frame.texture_slots[d.texture_first + t];
        if (ts.slot == 0) image = ts.image, swz = ts.swizzle, view = ts.view_format, minl = ts.min_level, maxl = ts.max_level;
      }
      const UiCore::Verdict v = core.Decide(origin->part, image, swz, view, minl, maxl, provider);
      rdr_adapter::ImageContent c;
      const uint32_t key = origin->part;
      if (!image || !key || !table.Lookup(image, c) || c.hash == 0) {   // untextured / no key / a movie plane (P4's) / never told: not a row of the picture source
        ++r.skipped;
        continue;
      }
      const auto lr = latest.find(image);
      if (lr == latest.end() || !capture.ReadTexture(lr->second, blob)) return std::fprintf(stderr, "no content for image %u\n", image), false;
      const rdr::Format fmt = rdr::Format(uint8_t(c.format));
      if (!(fmt == rdr::Format::BC1 || fmt == rdr::Format::BC2 || fmt == rdr::Format::BC3 || fmt == rdr::Format::RGBA8 || fmt == rdr::Format::BGRA8)) {
        ++r.skipped;
        continue;
      }
      const size_t mip0 = UiMipBytes(fmt, c.width, c.height);
      if (c.mips > 1 && std::min(c.width, c.height) <= 16) ++r.size_risk;   // [L] a real small texture with the packed-mips bit is refused `size` by guest_tex (review 3); the forge does not set the bit
      ++r.rows;
      // The game's birth (the first sight of this key) and the forged texture.
      auto kit = key_image.find(key);
      if (kit == key_image.end() || key_gen[key] != c.generation) {
        const uint32_t held = forge.KeyAt(key);
        if (held && held != key) src.OnDied(held, f);   // the slot's old life ends
        uint32_t fswz = 0;   // the game's fetch selectors of the row's slot: slot swizzle component 3 R / 4 G / 5 B / 6 A -> memory byte 0..3, 1 zero -> 4, 2 one -> 5, 0 identity
        for (int i = 0; i < 4; ++i) {
          const uint32_t s = swz ? swz[i] : 0, sel = s == 0 ? uint32_t(i) : (s >= 3 && s <= 6) ? s - 3 : s == 1 ? 4u : 5u;
          fswz |= sel << (3 * i);
        }
        forge.Place(key, fmt, c.width, c.height, blob.data, (key & 1) == 0, 1 + (key >> 4) % 3, fswz);   // tiled on even keys, endian 1 / 2 / 3 by key
        if (kit == key_image.end()) src.OnBorn(key, 0x82BAA908u), ++r.keys, ++first_sights_expected;
        key_image[key] = image, key_gen[key] = c.generation;
      }
      src.Seen(key, f, forge, phys);
      ps::Answer ans;
      const bool have = src.Lookup(key, ans);
      if (!have) {
        ++r.mismatch;
        if (shown++ < 6) std::printf("      row key %08X image %u: no answer\n", key, image);
        continue;
      }
      if (v.ours) {
        UiFileEntry mine, ref;
        if (ans.kind == ps::Kind::Refused) ++r.refused[std::string("joined:") + ps::WhyName(ans.why)];   // A: the stream joined it, the source refuses by NAME (bounded by kAllowedRefusals, below)
        else if (ans.kind == ps::Kind::File && ans.entry == v.entry) ++r.joined_same;
        else if (ans.kind == ps::Kind::File && index.Get(ans.entry, mine) && index.Get(v.entry, ref) && mine.prefix[0] == ref.prefix[0] && UiByteClass(mine.format) == UiByteClass(ref.format)) ++r.joined_equiv;
        else {
          ++r.mismatch;
          if (shown++ < 6) std::printf("      row key %08X image %u (%ux%u fmt %u mips %u): stream joined entry %u, source answered kind %d why %s entry %u\n", key, image, c.width, c.height, c.format, c.mips, v.entry, int(ans.kind), ps::WhyName(ans.why), ans.entry);
        }
      } else if (v.why == rdr_adapter::KeepWhy::Unjoined) {
        if (ans.kind == ps::Kind::Guest && ans.blocks && ans.blocks->size() == mip0 && std::memcmp(ans.blocks->data(), blob.data, mip0) == 0) ++r.guest_ok;
        else if (ans.kind == ps::Kind::Refused) ++r.refused[std::string("unjoined:") + ps::WhyName(ans.why)];   // named and counted, bounded by kAllowedRefusals (a 1280x720 8888 picture is over the read cap)
        else {
          ++r.guest_bad;
          if (shown++ < 6) std::printf("      row key %08X image %u: unjoined on the stream path, source answered kind %d (%s)\n", key, image, int(ans.kind), ps::WhyName(ans.why));
        }
      } else {
        // The stream path kept the draw for a stream-only reason (mips / view / not ready): the source's answer is File or Guest by the bytes; reported, never a mismatch.
        if (ans.kind == ps::Kind::File) ++r.file_only;
        else if (ans.kind == ps::Kind::Refused) ++r.refused[std::string("kept:") + ps::WhyName(ans.why)];
      }
    }
  }
  r.c = src.counters();
  r.mismatch += r.guest_bad;
  (void)first_sights_expected;
  return true;
}

// ---- synthetic checks ----------------------------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> Pattern(size_t n, uint8_t salt) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i * 7 + salt);
  return v;
}

void Synthetic() {
  constexpr uint32_t kLoader = 0x82BAA908u, kRt = 0x827E8D64u, kMovie = 0x827FBB84u;
  const auto phys_of = [](Forge& f) { return [&f](uint32_t a, uint32_t n, uint8_t* d) { return f.phys(a, n, d); }; };
  // DEATH + COUNTER: slot 5 holds key A (counter 1); A dies; the slot is reused by key B (counter 2) with other bytes.
  {
    Forge f;
    ps::PictureSource src(nullptr);
    const auto phys = phys_of(f);
    const uint32_t A = 0x00010005u, B = 0x00020005u;
    const auto pa = Pattern(16 * 16 / 16 * 8, 3), pb = Pattern(16 * 16 / 16 * 8, 99);   // 16 x 16 BC1: 16 blocks of 8 bytes
    f.Place(A, rdr::Format::BC1, 16, 16, pa.data(), true, 2);
    src.OnBorn(A, kLoader);
    src.Seen(A, 10, f, phys);
    ps::Answer a;
    const bool ok_a = src.Lookup(A, a) && a.kind == ps::Kind::Guest && a.blocks && *a.blocks == pa;
    src.OnDied(A, 11);
    f.Unplace(A);
    f.Place(B, rdr::Format::BC1, 16, 16, pb.data(), true, 2);
    src.OnBorn(B, kLoader);
    src.Seen(B, 12, f, phys);
    ps::Answer b;
    const bool ok_b = src.Lookup(B, b) && b.kind == ps::Kind::Guest && b.blocks && *b.blocks == pb;
    Check(ok_a && ok_b, "COUNTER", "a reused registry slot (same low 16 bits, new birth counter) answers the NEW texture's bytes, never the old key's");
    ps::Answer dead;
    const bool served_in_grace = src.Lookup(A, dead) || true;   // (within the grace the dead key may still answer: the host draws record j late)
    (void)served_in_grace;
    src.TrimDrawn(13);   // the host has drawn record 13: A (died in frame 11) is no longer named by any record to come
    ps::Answer gone;
    Check(!src.Lookup(A, gone) && src.resident() == 1 && src.counters().trimmed >= 1, "DEATH", "a key the game released is dropped after its grace: not answered, not resident (resident " + N(src.resident()) + ", bar 1)");
  }
  // SEQUENCE (PN3's events, as the hooks hand them over: the call site is lr - 4 of the creator's `bl`): born -> seen -> died -> trimmed, and the creator class by site.
  {
    Forge f;
    ps::PictureSource src(nullptr);
    const auto phys = phys_of(f);
    const uint32_t K = 0x00070011u;
    const auto p = Pattern(16 * 8, 41);
    f.Place(K, rdr::Format::BC1, 16, 16, p.data(), true, 2);
    src.OnBorn(K, 0x82BAA908u);
    src.Seen(K, 5, f, phys);
    src.OnDied(K, 6);
    src.Seen(K, 7, f, phys);   // a row after the death (the game's own late draw): served from the answer, never read again
    src.TrimDrawn(7);
    const ps::Counters c = src.counters();
    const bool classes = ps::CreatorOfSite(0x82BAA908u) == ps::Creator::Loader && ps::CreatorOfSite(0x827E8D64u) == ps::Creator::Gpu && ps::CreatorOfSite(0x827FBB84u) == ps::Creator::Movie && ps::CreatorOfSite(0x12345678u) == ps::Creator::Unknown;
    Check(c.born == 1 && c.seen == 1 && c.died == 1 && c.trimmed == 1 && c.reads == 1 && src.resident() == 0 && classes, "SEQUENCE",
          "born 1, seen 1, died 1, trimmed 1, one read, nothing resident; the creator class follows the call site (born " + N(c.born) + ", seen " + N(c.seen) + ", died " + N(c.died) + ", trimmed " + N(c.trimmed) + ", reads " + N(c.reads) + ")");
  }
  // REHASH: a hit never reads or hashes again.
  {
    Forge f;
    ps::PictureSource src(nullptr);
    const auto phys = phys_of(f);
    const uint32_t K = 0x00030007u;
    const auto p = Pattern(16 * 8, 5);
    f.Place(K, rdr::Format::BC1, 16, 16, p.data(), true, 1);
    src.OnBorn(K, kLoader);
    for (uint64_t fr = 0; fr < 60; ++fr) src.Seen(K, fr, f, phys);
    const ps::Counters c = src.counters();
    Check(c.seen == 1 && c.reads == 1 && c.hashes == 1 && c.hits == 59, "REHASH", "60 sights of one key: " + N(c.reads) + " guest read, " + N(c.hashes) + " hash, " + N(c.hits) + " hits (bar 1 / 1 / 59: no per-hit re-read)");
  }
  // RT + movie: a key born by a render-target creator is refused by name and never read; a movie plane is P4's.
  {
    Forge f;
    ps::PictureSource src(nullptr);
    const auto phys = phys_of(f);
    const uint32_t R = 0x00040009u, M = 0x0005000Bu;
    const auto p = Pattern(16 * 8, 17);
    f.Place(R, rdr::Format::BC1, 16, 16, p.data(), true, 2);
    f.Place(M, rdr::Format::BC1, 16, 16, p.data(), true, 2);
    src.OnBorn(R, kRt), src.OnBorn(M, kMovie);
    src.Seen(R, 1, f, phys), src.Seen(M, 1, f, phys);
    ps::Answer a, m;
    const bool ra = src.Lookup(R, a) && a.kind == ps::Kind::Refused && a.why == ps::Why::Gpu;
    const bool rm = src.Lookup(M, m) && m.kind == ps::Kind::Refused && m.why == ps::Why::Movie;
    Check(ra && rm && src.counters().reads == 0, "RT", "a render-target creator's key is refused gpu_picture and a movie plane refused movie, with no guest read (reads " + N(src.counters().reads) + ", bar 0)");
  }
  // GUARD (past-bug re-find, S100's stale roster render): new bytes written under a LIVE key with no birth are caught by the sample guard, counted refill_unannounced, and answered fresh.
  {
    Forge f;
    ps::PictureSource src(nullptr);
    const auto phys = phys_of(f);
    const uint32_t K = 0x00060003u;
    const auto p1 = Pattern(64 * 8, 11), p2 = Pattern(64 * 8, 222);   // 32 x 8 BC1 = 8 x 2 blocks... use 32 x 32: 64 blocks
    f.Place(K, rdr::Format::BC1, 32, 32, p1.data(), true, 2);
    src.OnBorn(K, kLoader);
    src.Seen(K, 1, f, phys);
    ps::Answer a;
    src.Lookup(K, a);
    const uint64_t s1 = a.serial;
    // the game refills the same texture in place (same slot, same fetch dwords): rewrite the pixel region only
    const uint32_t wrapper = f.words[Forge::kEntries + 8 * (K & 0xFFFF) + 4], header = f.words[wrapper + particles::kWrapperTexture];
    const uint32_t base = wwe2k14::guest_view::ToPhysical(f.words[header + particles::kHeaderFetch + 4]) & 0xFFFFF000u;   // (S103 TX: the header word is the CPU E-view address)
    Forge g;   // a scratch forge to get the new tiled bytes
    g.Place(K, rdr::Format::BC1, 32, 32, p2.data(), true, 2);
    f.regions[base] = g.regions.begin()->second;
    src.Seen(K, 2, f, phys);
    ps::Answer b;
    src.Lookup(K, b);
    const ps::Counters c = src.counters();
    Check(c.refill_unannounced == 1 && b.serial != s1 && b.blocks && *b.blocks == p2, "GUARD", "S100's stale roster render (new bytes under a live key, no birth): refill_unannounced " + N(c.refill_unannounced) + " (bar 1), the answer is the new bytes");
  }
}

// ---- review A1-A8 -------------------------------------------------------------------------------------------------------------------------------------------
void SyntheticReview() {
  constexpr uint32_t kLoader = 0x82BAA908u, kUnknown = 0x82BB07C4u;
  const auto phys_of = [](Forge& f) { return [&f](uint32_t a, uint32_t n, uint8_t* d) { return f.phys(a, n, d); }; };
  // RACE (A2): the key dies while its first read is in flight (the guest read is outside the lock): the answer is stored DEAD and the host's trim drops it.
  {
    Forge f;
    ps::PictureSource src(nullptr);
    const uint32_t K = 0x00090013u;
    const auto p = Pattern(16 * 8, 61);
    f.Place(K, rdr::Format::BC1, 16, 16, p.data(), true, 2);
    src.OnBorn(K, kLoader);
    bool fired = false;
    const auto racing = [&](uint32_t a, uint32_t n, uint8_t* d) {
      if (!fired) fired = true, src.OnDied(K, 20);   // (the game's slot free on another thread, mid-read)
      return f.phys(a, n, d);
    };
    src.Seen(K, 20, f, racing);
    src.TrimDrawn(21);
    ps::Answer a;
    Check(fired && !src.Lookup(K, a) && src.resident() == 0, "RACE", "a key that died while its read was in flight ends dead and is trimmed by the host (resident " + N(src.resident()) + ", bar 0)");
    // a death of a key never seen leaves a tombstone: it is never read afterwards, and it is trimmed too
    ps::PictureSource s2(nullptr);
    Forge g;
    const uint32_t K2 = 0x000A0015u;
    g.Place(K2, rdr::Format::BC1, 16, 16, p.data(), true, 2);
    s2.OnBorn(K2, kLoader);
    s2.OnDied(K2, 30);
    s2.Seen(K2, 30, g, phys_of(g));
    Check(s2.counters().reads == 0 && !s2.Lookup(K2, a) && s2.resident() == 1, "RACE", "a death before the first sight leaves a tombstone: no read, no answer (reads " + N(s2.counters().reads) + ")");
    s2.TrimDrawn(31);
    Check(s2.resident() == 0, "RACE", "the tombstone is trimmed once the host has drawn a newer record (resident " + N(s2.resident()) + ")");
  }
  // SWZ (A3) + UNCLASS (A4): 4-byte pictures.
  {
    const auto px = Pattern(8 * 8 * 4, 77);   // 8 x 8 texels, 4 bytes each (memory order after the swap: B, G, R, A)
    const uint32_t kBgra = gt::kBgraIdentitySwizzle, kAbgr = 0u | (1u << 3) | (2u << 6) | (3u << 9), kXrgb = 2u | (1u << 3) | (0u << 6) | (5u << 9);   // W forced ONE
    struct Case { const char* name; uint32_t swz; uint32_t site; bool answered; ps::Why why; };
    const Case cases[] = {{"BGRA identity, a loader", kBgra, kLoader, true, ps::Why::Ok}, {"A8B8G8R8 (0,1,2,3)", kAbgr, kLoader, false, ps::Why::Format}, {"X8R8G8B8 (alpha forced one)", kXrgb, kLoader, false, ps::Why::Format},
                          {"BGRA identity, an Unknown creator", kBgra, kUnknown, false, ps::Why::Unclassified}};
    bool swz_ok = true, uncl_ok = true;
    std::string seen;
    uint32_t k = 0x000B0001u;
    for (const Case& c : cases) {
      Forge f;
      ps::PictureSource src(nullptr);
      f.Place(k, rdr::Format::BGRA8, 8, 8, px.data(), false, 2, c.swz);
      src.OnBorn(k, c.site);
      src.Seen(k, 1, f, phys_of(f));
      ps::Answer a;
      const bool have = src.Lookup(k, a);
      bool ok;
      if (c.answered) ok = have && a.kind == ps::Kind::Guest && a.format == rdr::Format::BGRA8 && a.blocks && *a.blocks == px;
      else ok = have && a.kind == ps::Kind::Refused && a.why == c.why;
      (c.why == ps::Why::Unclassified ? uncl_ok : swz_ok) &= ok;
      seen += std::string(" [") + c.name + " -> " + (have ? ps::WhyName(a.why) : "none") + (have && a.kind == ps::Kind::Guest ? " guest" : "") + "]";
      ++k;
    }
    Check(swz_ok, "SWZ", "a 4-byte picture is read only with the BGRA identity fetch swizzle; A8B8G8R8 and X8R8G8B8 are refused `format`:" + seen);
    Check(uncl_ok, "UNCLASS", "an Unknown creator's 4-byte picture is refused `unclassified_creator` (a loader's is read); an Unknown creator's BC picture is still read");
    {
      Forge f;
      ps::PictureSource src(nullptr);
      const auto p = Pattern(16 * 8, 5);
      f.Place(0x000C0003u, rdr::Format::BC1, 16, 16, p.data(), true, 2);
      src.OnBorn(0x000C0003u, kUnknown);
      src.Seen(0x000C0003u, 1, f, phys_of(f));
      ps::Answer a;
      Check(src.Lookup(0x000C0003u, a) && a.kind == ps::Kind::Guest, "UNCLASS", "an Unknown creator's BC1 picture is answered (BC is never a render target)");
    }
    // The file join: only a BGRA8 file shows the same colours as the BGRA identity picture (an RGBA8 file holding the same bytes would be R / B swapped).
    {
      const uint64_t hash = rdr_adapter::ImageContentHash(px.data(), px.size());
      const auto entry_of = [&](rdr::Format fmt) {
        UiFileEntry e;
        e.pac = 0, e.path = "p0/0", e.width = 8, e.height = 8, e.format = fmt, e.mips = 1, e.bytes = uint32_t(px.size()), e.prefix = {hash};
        return e;
      };
      UiFileIndex only_rgba, with_bgra;
      only_rgba.AddPac(0, std::vector<UiFileEntry>{entry_of(rdr::Format::RGBA8)});
      with_bgra.AddPac(0, std::vector<UiFileEntry>{entry_of(rdr::Format::RGBA8), [&] { UiFileEntry e = entry_of(rdr::Format::BGRA8); e.path = "p0/1"; e.prefix = {hash}; return e; }()});
      const uint32_t K = 0x000D0005u;
      Forge f;
      f.Place(K, rdr::Format::BGRA8, 8, 8, px.data(), false, 2, kBgra);
      ps::PictureSource s1(&only_rgba), s2(&with_bgra);
      s1.OnBorn(K, kLoader), s2.OnBorn(K, kLoader);
      s1.Seen(K, 1, f, phys_of(f)), s2.Seen(K, 1, f, phys_of(f));
      ps::Answer a1, a2;
      UiFileEntry fe;
      const bool ok1 = s1.Lookup(K, a1) && a1.kind == ps::Kind::Guest;
      const bool ok2 = s2.Lookup(K, a2) && a2.kind == ps::Kind::File && with_bgra.Get(a2.entry, fe) && fe.format == rdr::Format::BGRA8;
      Check(ok1 && ok2, "SWZ", "the BGRA picture stays a Guest copy when only an RGBA8 file holds its bytes, and joins the BGRA8 file when there is one (kinds " + N(int(a1.kind)) + " / " + N(int(a2.kind)) + ")");
      // BRIDGE (T2F): a File answer keeps its mip 0 only for a consumer (the resolver sets g_pic_keep_bridge) and only within half the resident cap, so Guest copies keep headroom; the file path serves it either way.
      {
        const uint64_t saved_cap = ps::g_pic_resident_cap;
        const bool saved_keep = ps::g_pic_keep_bridge;
        const auto join_with = [&](bool keep, uint64_t cap, uint32_t key, ps::Answer& out, uint64_t& resident) {
          ps::g_pic_keep_bridge = keep, ps::g_pic_resident_cap = cap;
          Forge fb;
          fb.Place(key, rdr::Format::BGRA8, 8, 8, px.data(), false, 2, kBgra);
          ps::PictureSource sb(&with_bgra);
          sb.OnBorn(key, kLoader);
          sb.Seen(key, 1, fb, phys_of(fb));
          sb.Lookup(key, out);
          resident = sb.counters().resident_bytes;
        };
        ps::Answer b1, b2, b3;
        uint64_t r1 = 0, r2 = 0, r3 = 0;
        join_with(true, 1u << 20, 0x000D0007u, b1, r1);
        join_with(false, 1u << 20, 0x000D0009u, b2, r2);
        join_with(true, px.size() + 10, 0x000D000Bu, b3, r3);   // half the cap is below the picture
        ps::g_pic_keep_bridge = saved_keep, ps::g_pic_resident_cap = saved_cap;
        Check(b1.kind == ps::Kind::File && b1.blocks && r1 == px.size(), "BRIDGE", "a File join keeps its mip 0 for the resolver's bridge (blocks " + std::string(b1.blocks ? "kept" : "dropped") + ", resident " + N(r1) + ")");
        Check(b2.kind == ps::Kind::File && !b2.blocks && r2 == 0, "BRIDGE", "no consumer (no resolver): a File join keeps no blocks, the dev tap does not pin every mip 0");
        Check(b3.kind == ps::Kind::File && !b3.blocks && r3 == 0, "BRIDGE", "a File join over half the cap loses only its bridge bytes: still a File answer, Guest keeps headroom");
      }
    }
  }
  // BUDGET + RELEASE (A6): the resident cap refuses by name; ReleaseBlocks frees the CPU copy once uploaded.
  {
    const uint64_t saved = ps::g_pic_resident_cap;
    ps::g_pic_resident_cap = 300;   // three 16 x 16 BC1 pictures are 3 x 128 bytes
    Forge f;
    ps::PictureSource src(nullptr);
    const auto p = Pattern(16 * 8, 9);
    uint32_t ks[4];
    for (int i = 0; i < 4; ++i) {
      ks[i] = 0x000E0001u + 2u * i;
      f.Place(ks[i], rdr::Format::BC1, 16, 16, p.data(), true, 2);
      src.OnBorn(ks[i], kLoader);
    }
    for (int i = 0; i < 3; ++i) src.Seen(ks[i], 1, f, phys_of(f));
    ps::Answer a0, a2;
    src.Lookup(ks[0], a0), src.Lookup(ks[2], a2);
    const bool budget = a0.kind == ps::Kind::Guest && a2.kind == ps::Kind::Refused && a2.why == ps::Why::Budget && src.counters().resident_bytes == 256;
    Check(budget, "BUDGET", "past the resident cap a first sight is refused memory_budget (resident " + N(src.counters().resident_bytes) + " of cap 300, third picture " + ps::WhyName(a2.why) + ")");
    src.ReleaseBlocks(ks[0], a0.serial);
    const ps::Counters c = src.counters();
    src.Seen(ks[3], 2, f, phys_of(f));
    ps::Answer a3;
    src.Lookup(ks[3], a3);
    Check(c.resident_bytes == 128 && c.blocks_released == 1 && a3.kind == ps::Kind::Guest, "RELEASE", "after the host uploaded a picture its blocks are freed (resident " + N(c.resident_bytes) + ", bar 128) and the room is reused");
    // the answer still answers (File / Guest identity) without its blocks
    ps::Answer again;
    Check(src.Lookup(ks[0], again) && again.kind == ps::Kind::Guest && !again.blocks && again.serial == a0.serial, "RELEASE", "a released Guest answer keeps its serial (the host's image stays valid)");
    ps::g_pic_resident_cap = saved;
  }
  // PARITY (dev): the stream's hash of the same key against ours.
  {
    Forge f;
    ps::PictureSource src(nullptr);
    const auto p = Pattern(16 * 8, 13);
    const uint32_t K = 0x000F0003u;
    f.Place(K, rdr::Format::BC1, 16, 16, p.data(), true, 2);
    src.OnBorn(K, kLoader);
    src.Seen(K, 1, f, phys_of(f));
    ps::Answer a;
    src.Lookup(K, a);
    src.NoteStreamHash(K, a.hash, 1), src.NoteStreamHash(K, a.hash ^ 1, 1), src.NoteStreamHash(K, a.hash, 3), src.NoteStreamHash(0x00100001u, 5, 1);
    const ps::Counters c = src.counters();
    Check(c.guest_parity_ok == 1 && c.guest_parity_bad == 1 && c.parity_skipped == 1 && c.parity_no_answer == 1, "PARITY", "the stream-hash comparison counts ok / bad / skipped (mips) / no answer: " + N(c.guest_parity_ok) + " / " + N(c.guest_parity_bad) + " / " + N(c.parity_skipped) + " / " + N(c.parity_no_answer));
  }
  // PHYS (A8): the physical range bound.
  Check(gt::PhysRangeOk(0x1FFFFFE0u, 32) && !gt::PhysRangeOk(0x1FFFFFF0u, 32) && !gt::PhysRangeOk(0x1FFFFFFFu, 2) && !gt::PhysRangeOk(0x100u, 0) && gt::PhysRangeOk(0xA0000000u, 16), "PHYS", "PhysRangeOk: the read must end inside the 512 MB of physical memory (the mask wraps the address like the game's mapping)");
}

}  // namespace

// The refusals a capture may show (named, bounded: a NEW refusal or a growth fails [REFUSED]). Measured on the first run of this lane; "key:count" per capture, empty = none allowed.
const std::map<std::string, std::map<std::string, uint64_t>>& AllowedRefusals() {
  static const std::map<std::string, std::map<std::string, uint64_t>> k = {
      // (measured, this lane: every c2d capture shows 0 refusals, so no entry is allowed; a refusal appearing later must be named here with its count, and that is a reviewed change)
  };
  return k;
}

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (s == "--game") a.game = next();
    else if (s == "--cache") a.cache = next();
    else if (s == "--captures") a.captures = next();
    else if (s == "--only") a.only = next();
    else if (s == "--plant") a.plant = next();
    else return std::fprintf(stderr, "unknown argument %s\n", s.c_str()), 2;
  }
  struct Plant { const char* name; const char* tag; int pic, tex; };
  static const Plant kPlants[] = {{"death_ignored", "DEATH", 1, 0}, {"counter_ignored", "COUNTER", 2, 0}, {"rehash_every_hit", "REHASH", 3, 0}, {"rt_as_file", "RT", 4, 0},
                                  {"wrong_mip", "JOIN", 5, 0}, {"no_swap", "JOIN", 0, 3}, {"tiled_ignored", "JOIN", 0, 2},
                                  {"race_ignored", "RACE", 6, 0}, {"swizzle_ignored", "SWZ", 0, 6}, {"unknown_8888", "UNCLASS", 7, 0}, {"file_any_order", "SWZ", 8, 0}, {"no_budget", "BUDGET", 9, 0}, {"keep_blocks", "RELEASE", 10, 0}, {"no_bridge", "BRIDGE", 11, 0}, {"bridge_cap", "BRIDGE", 12, 0}};
  const Plant* plant = nullptr;
  for (const Plant& p : kPlants)
    if (a.plant == p.name) plant = &p;
  if (!a.plant.empty() && !plant) return std::fprintf(stderr, "unknown plant %s\n", a.plant.c_str()), 2;
  if (a.game.empty() || a.captures.empty()) return std::fprintf(stderr, "usage: picture_source_unit --game DIR --cache DIR --captures DIR [--only a,b] [--plant NAME]\n"), 2;
  if (plant) ps::g_pic_plant = plant->pic, gt::g_tex_plant = plant->tex;

  Synthetic();
  SyntheticReview();

  UiFileIndex index;
  if (!IndexTier1(a, index)) return std::fprintf(stderr, "cannot build the pac index\n"), 2;
  std::vector<std::string> names = {"c2d_menu", "c2d_play", "c2d_options", "c2d_select", "c2d_title", "c2d_load", "c2d_oneonone", "c2d_pause", "c2d_match_hud", "c2d_entrance_tron"};
  if (!a.only.empty()) {
    names.clear();
    for (size_t at = 0; at <= a.only.size();) {
      const size_t e = a.only.find(',', at);
      names.push_back(a.only.substr(at, e == std::string::npos ? std::string::npos : e - at));
      if (e == std::string::npos) break;
      at = e + 1;
    }
  }
  uint64_t tot_mismatch = 0, tot_rows = 0;
  bool reads_ok = true, refused_ok = true;
  for (const std::string& n : names) {
    CapResult r;
    if (!RunCapture(a, n, index, r)) return 2;
    uint64_t refused_total = 0;
    std::string refused_txt;
    for (const auto& [k, v] : r.refused) refused_total += v, refused_txt += " " + k + "=" + N(v);
    std::printf("      %-20s rows %llu, keys %llu: joined %llu (+%llu equiv), guest-copied %llu, file-only (stream kept it) %llu, refused %llu%s, skipped (no picture / movie) %llu, mismatch %llu; reads %llu hashes %llu hits %llu guard %llu trips %llu; small-mip risk %llu\n",
                n.c_str(), (unsigned long long)r.rows, (unsigned long long)r.keys, (unsigned long long)r.joined_same, (unsigned long long)r.joined_equiv, (unsigned long long)r.guest_ok, (unsigned long long)r.file_only,
                (unsigned long long)refused_total, refused_txt.c_str(), (unsigned long long)r.skipped, (unsigned long long)r.mismatch, (unsigned long long)r.c.reads, (unsigned long long)r.c.hashes, (unsigned long long)r.c.hits,
                (unsigned long long)r.c.guard_checks, (unsigned long long)r.c.refill_unannounced, (unsigned long long)r.size_risk);
    tot_mismatch += r.mismatch, tot_rows += r.rows;
    {   // [REFUSED]: every refusal is named and bounded by the capture's allowance (a growth, or a reason not listed, fails)
      const auto al = AllowedRefusals().find(n);
      std::string over;
      for (const auto& [why, cnt] : r.refused) {
        uint64_t allow = 0;
        if (al != AllowedRefusals().end())
          if (const auto w = al->second.find(why); w != al->second.end()) allow = w->second;
        if (cnt > allow) over += " " + why + "=" + N(cnt) + " (allowed " + N(allow) + ")";
      }
      refused_ok = refused_ok && over.empty();
      if (!over.empty()) std::printf("      %s: refusals over the allowance:%s\n", n.c_str(), over.c_str());
    }
    reads_ok = reads_ok && r.c.reads == r.c.seen && r.c.hashes <= r.c.seen && r.c.refill_unannounced == 0;
  }
  Check(tot_rows > 0 && tot_mismatch == 0, "JOIN", N(tot_rows) + " textured rows over " + N(names.size()) + " captures: every row the stream path joined is answered by the SAME index entry, every unjoined one by its own bytes; mismatch " + N(tot_mismatch) + " (bar 0)");
  Check(refused_ok, "REFUSED", "every refusal the capture rows show is named and within the capture's allowance (AllowedRefusals)");
  Check(reads_ok, "ONCE", "per capture: one guest read and one hash per first sight, none on a hit, the guard never trips on a still picture");
  if (plant) {
    const bool caught = g_failed.find(std::string("[") + plant->tag + "]") != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant->name, caught ? "caught by its named check" : "its named check did NOT fail", g_failed.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s: picture_source_unit: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
