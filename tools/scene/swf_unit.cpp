// Lane SWF (Session 103): unit proof of the liquid-map readers: the hook's tracker (src\scene_live\liquid_hook.h) over a FIXTURE guest memory (tools\scene\e5_fake_memory.h: no capture holds the CCLM component or its items),
// the record sections 51 / 52 / 53 (round trip, a bad layout drops the three, an older record has none) and BuildE2Input's use of them.
//   swf_unit                 every line PASS
//   swf_unit --plant NAME    exit 0 only when the check that sees the plant FAILS: last_call (LAST CALL), vtable (TEXTURE CLASS), stale_frame (FRAME STAMP), early_outs (EARLY OUTS)
// Marks: the memory is a FIXTURE; what is [C] is the field layout (SWB 1a / 1d: q asm sub_8279ACD8 / sub_82798748 / sub_82798888 / sub_82726170) and the texture chain (q vtable 0x8206ADB8 slot 2 = sub_82BD4288, guest_texture.h).
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/liquid_join.h"
#include "../../src/renderer_scene/live_maps_e2.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/liquid_hook.h"
#include "e5_fake_memory.h"

using namespace rdr_scene;
namespace lq = rdr_scene::live::liquid;

static int g_fail = 0;
static std::string g_tokens;
static void Check(bool ok, const char* token, const char* fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", ok ? "" : token, ok ? "" : ": ", b);
  if (!ok) ++g_fail, g_tokens += std::string("[") + token + "]";
}

static std::vector<uint8_t> g_pix;   // the fixture's pixel memory (a physical address space starting at kPixBase)
static constexpr uint32_t kPixBase = 0x00100000u;
static bool ReadWord(uint32_t a, uint32_t& v) {
  const auto it = e5::Mem().find(a);
  if (it == e5::Mem().end()) return false;
  v = it->second;
  return true;
}
static bool ReadBytes(uint32_t a, void* d, size_t n) { return e5::ReadGuest(a, d, n); }
static bool ReadPhys(uint32_t a, uint32_t n, uint8_t* d) {
  if (a < kPixBase || size_t(a - kPixBase) + n > g_pix.size()) return false;
  std::memcpy(d, g_pix.data() + (a - kPixBase), n);
  return true;
}

static constexpr uint32_t kMap = 0x40010000u, kChar = 0x40020000u, kSkel = 0x40030000u, kItemA = 0x40040000u, kItemB = 0x400400A0u, kWrap = 0x40050000u, kHdr = 0x40060000u;

// One item of the 160-byte pool: next +8, type +20, state +60, texture +64, LocalArea +80, WorldParam +112, rect +128, pass +144.
static void PutItem(uint32_t at, uint32_t next, uint32_t type, uint32_t state, uint32_t tex, uint32_t pass, float wx0, float wy0, float wx1, float wy1) {
  for (uint32_t o = 0; o < 160; o += 4) e5::PutWord(at + o, 0);
  e5::PutWord(at + 8, next), e5::PutWord(at + 20, type), e5::PutWord(at + 60, state), e5::PutWord(at + 64, tex), e5::PutWord(at + 144, pass);
  const float local[4] = {wx0 - 0.01f, wy0 - 0.01f, wx1 + 0.01f, wy1 + 0.01f};
  const float world[4] = {wx0, wy0, wx1, wy1};
  const float rect[4] = {std::floor(wx0 * 1024.f / 32.f) * 32.f, std::floor(wy0 * 512.f / 32.f) * 32.f, std::ceil(wx1 * 1024.f / 32.f) * 32.f, std::ceil(wy1 * 512.f / 32.f) * 32.f};
  for (int k = 0; k < 4; ++k) e5::PutFloat(at + 80 + 4 * k, local[k]), e5::PutFloat(at + 112 + 4 * k, world[k]), e5::PutFloat(at + 128 + 4 * k, rect[k]);
}

static void BuildWorld(bool clear, bool dim, bool bad_vtable) {
  e5::Mem().clear();
  g_pix.assign(0x4000, 0);   // an untiled 64x64 BC1: 16 x 16 blocks of 8 bytes, pitch 32 blocks (row stride 256 bytes)
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x)
      for (uint32_t b = 0; b < 8; ++b) g_pix[size_t(y) * 256 + x * 8 + b] = uint8_t(y * 16 + x + b);
  // the map
  e5::PutWord(kMap + 2572, 1), e5::PutWord(kMap + 88, 2), e5::PutWord(kMap + 92, kItemA), e5::PutWord(kMap + 136, clear ? 1 : 0), e5::PutWord(kMap + 200, 0);
  e5::PutWord(kMap + 124, 1024), e5::PutWord(kMap + 128, 512), e5::PutWord(kMap + 132, 3), e5::PutWord(kMap + 48, kChar);
  for (uint32_t i = 0; i < 6; ++i) e5::PutWord(kMap + 152 + 4 * i, 0x1000u + i);   // the resources: all six words non-zero
  e5::PutWord(kChar + 48, kSkel), e5::PutWord(kChar + 5072, 0), e5::PutWord(kChar + 5076, dim ? 1 : 0);
  // the texture: wrapper (vtable A) -> +80 header -> six fetch dwords at +28: untiled 2D BC1 64x64, pitch 64 texels, endian 0
  e5::PutWord(kWrap, bad_vtable ? 0x82000000u : lq::kWrapperVtableA), e5::PutWord(kWrap + 80, kHdr);
  e5::PutWord(kHdr + 28, 2u << 22);                                  // dword 0: not tiled (bit 31 clear), pitch / 32 = 2 in bits 22-30, type 0
  e5::PutWord(kHdr + 32, kPixBase | 18u);                            // dword 1: format 18 (BC1), endian 0, base address
  e5::PutWord(kHdr + 36, 63u | (63u << 13));                         // dword 2: width - 1, height - 1
  e5::PutWord(kHdr + 40, 0), e5::PutWord(kHdr + 44, 0);
  e5::PutWord(kHdr + 48, 1u << 9);                                   // dword 5: dimension 1 = 2D
  PutItem(kItemA, kItemB, 1, 0x80000000u, kWrap, 0, 0.30f, 0.40f, 0.38f, 0.52f);          // a tear (sweat), alive
  PutItem(kItemB, 0, 0, 0x80000000u | 0x40000000u, kWrap, 0, 0.60f, 0.60f, 0.70f, 0.70f);   // a blood decal, paused + not drawn (bit 30)
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  const auto reader = [](lq::Tracker& t, uint32_t map, uint64_t frame) { return t.OnNode(map, frame, ReadWord, ReadBytes, ReadPhys); };

  // ---- [DECODE] the map, its character, its items, the source texture ----
  {
    lq::Tracker tr;
    if (plant == "vtable") tr.plant = lq::Plant::NoVtableCheck;
    if (plant == "stale_frame") tr.plant = lq::Plant::StaleFrame;
    BuildWorld(false, true, false);
    const bool rec = reader(tr, kMap, 77);
    tr.CloseBefore(78);
    lq::FrameBuffer fb;
    const bool got = tr.TakeNext(fb);
    Check(rec && got && fb.maps.size() == 1, "DECODE", "one map row recorded and taken (rec %d got %d maps %zu)", int(rec), int(got), got ? fb.maps.size() : size_t(0));
    if (got && fb.maps.size() == 1) {
      const LiquidMapRow& r = fb.maps[0].row;
      Check(r.map == kMap && r.object == kSkel && r.width == 1024 && r.height == 512 && r.levels == 3, "DECODE", "map 0x%08X object 0x%08X %ux%u x%u", r.map, r.object, r.width, r.height, r.levels);
      Check((r.flags & kLmEnabled) && (r.flags & kLmResources) && (r.flags & kLmDim) && !(r.flags & kLmClear), "DECODE", "flags 0x%X: enabled, resources, dim (char+5076), no clear", r.flags);
      Check(fb.maps[0].items.size() == 2, "DECODE", "two items in list order (%zu)", fb.maps[0].items.size());
      if (fb.maps[0].items.size() == 2) {
        const LiquidItemRow& a = fb.maps[0].items[0];
        const LiquidItemRow& b = fb.maps[0].items[1];
        Check(a.type == 1 && a.pass == 0 && std::fabs(a.world_param[0] - 0.30f) < 1e-6f && std::fabs(a.rect[2] - 416.f) < 1e-3f && std::fabs(a.local_area[0] - 0.29f) < 1e-6f, "DECODE", "item 0: type %u pass %u world %.2f rect x1 %.0f", a.type, a.pass, a.world_param[0], a.rect[2]);
        Check(b.type == 0 && (b.state & kLiStateNotDrawn) == 0x40000000u, "DECODE", "item 1: blood, bit 30 set (not drawn) state 0x%08X", b.state);
        Check(a.picture == kWrap && (a.flags & kLiHasSource), "TEXTURE CLASS", "item 0's picture key = the wrapper 0x%08X (vtable checked)", a.picture);
      }
      Check(fb.pictures.size() == 1 && LiquidPictureValid(*fb.pictures[0]) && fb.pictures[0]->width == 64 && fb.pictures[0]->nbytes == 2048, "DECODE", "one BC1 64x64 picture of 2048 bytes (%zu rows)", fb.pictures.size());
      if (!fb.pictures.empty()) Check(fb.pictures[0]->bytes[0] == 0 && fb.pictures[0]->bytes[8] == 1 && fb.pictures[0]->bytes[128] == 16, "DECODE", "the blocks come out row-major from the untiled memory (blocks 0, 1 and the second row's first)");
      Check(r.frame == 77, "FRAME STAMP", "the row carries the hook frame 77 (got %u)", r.frame);
    }
    // a texture whose object is not the wrapper class: no picture, counted, never a guess
    lq::Tracker t2;
    if (plant == "vtable") t2.plant = lq::Plant::NoVtableCheck;
    BuildWorld(false, false, true);
    reader(t2, kMap, 5);
    t2.CloseBefore(6);
    lq::FrameBuffer f2;
    if (t2.TakeNext(f2) && !f2.maps.empty() && !f2.maps[0].items.empty()) {
      const bool none = f2.maps[0].items[0].picture == 0 && f2.pictures.empty() && t2.GetStats().tex_class >= 1;
      Check(none, "TEXTURE CLASS", "a source object of another class records no picture and is counted (tex_class %llu)", (unsigned long long)t2.GetStats().tex_class);
    } else {
      Check(false, "TEXTURE CLASS", "the bad-class map was not recorded");
    }
  }

  // ---- [EARLY OUTS] the node's own: +2572 == 0; no live item and no clear request ----
  {
    lq::Tracker tr;
    if (plant == "early_outs") tr.plant = lq::Plant::NoEnableCheck;
    BuildWorld(false, false, false);
    e5::PutWord(kMap + 2572, 0);
    const bool r1 = reader(tr, kMap, 10);
    BuildWorld(false, false, false);
    e5::PutWord(kMap + 92, 0);
    const bool r2 = reader(tr, kMap, 11);
    BuildWorld(true, false, false);
    e5::PutWord(kMap + 92, 0);
    e5::PutWord(kMap + 88, 0);
    const bool r3 = reader(tr, kMap, 12);   // a clear request alone IS recorded: the map must be cleared
    tr.CloseBefore(13);
    lq::FrameBuffer fb;
    bool any = false;
    int clears = 0, items = 0;
    while (tr.TakeNext(fb)) {
      any = true;
      for (const lq::MapEntry& m : fb.maps) clears += (m.row.flags & kLmClear) != 0, items += int(m.items.size());
    }
    Check(!r1 && !r2 && r3 && any && clears == 1 && items == 0, "EARLY OUTS", "disabled and empty maps record nothing; a clear request alone records a row with no item (%d %d %d, clears %d, items %d)", int(r1), int(r2), int(r3), clears, items);
  }

  // ---- [LAST CALL] a map called twice in a frame: the later wins, a clear seen by the earlier call is kept ----
  {
    lq::Tracker tr;
    if (plant == "last_call") tr.plant = lq::Plant::FirstCall;
    BuildWorld(true, false, false);
    reader(tr, kMap, 20);
    BuildWorld(false, false, false);
    e5::PutWord(kMap + 88, 1), e5::PutFloat(kItemA + 112, 0.11f);   // the second call sees the same item moved
    reader(tr, kMap, 20);
    tr.CloseBefore(21);
    lq::FrameBuffer fb;
    const bool got = tr.TakeNext(fb);
    const bool ok = got && fb.maps.size() == 1 && fb.maps[0].items.size() == 2 && std::fabs(fb.maps[0].items[0].world_param[0] - 0.11f) < 1e-6f && (fb.maps[0].row.flags & kLmClear);
    Check(ok, "LAST CALL", "the later call's items win (world x %.2f) and the earlier call's clear request survives", got && !fb.maps.empty() && !fb.maps[0].items.empty() ? fb.maps[0].items[0].world_param[0] : -1.f);
  }

  // ---- [RECORD] sections 51 / 52 / 53 round trip; a bad section drops the three; an older record has none ----
  {
    lq::Tracker tr;
    BuildWorld(true, true, false);
    reader(tr, kMap, 30);
    tr.CloseBefore(31);
    lq::FrameBuffer fb;
    SceneTables t;
    if (tr.TakeNext(fb))
      for (lq::MapEntry& m : fb.maps) {
        m.row.item_first = uint32_t(t.liquid_items.size());
        t.liquid_items.insert(t.liquid_items.end(), m.items.begin(), m.items.end());
        t.liquid_maps.push_back(m.row);
      }
    for (const auto& pp : fb.pictures) t.liquid_pictures.push_back(*pp);
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables back;
    std::string err;
    const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
    Check(ok && back.liquid_maps.size() == 1 && back.liquid_items.size() == 2 && back.liquid_pictures.size() == 1 && back.liquid_errors == 0 && back.liquid_maps[0].object == kSkel && back.liquid_items[0].picture == kWrap &&
              std::memcmp(back.liquid_pictures[0].bytes, t.liquid_pictures[0].bytes, 2048) == 0,
          "RECORD", "sections 51 / 52 / 53 round-trip (maps %zu items %zu pictures %zu errors %u: %s)", back.liquid_maps.size(), back.liquid_items.size(), back.liquid_pictures.size(), back.liquid_errors, err.c_str());
    SceneTables dry;
    const std::vector<uint8_t> dbytes = SerializeRecord(dry);
    SceneTables dback;
    const bool dok = ParseRecord(dbytes.data(), dbytes.size(), dback, err);
    Check(dok && dback.liquid_maps.empty() && dback.liquid_errors == 0, "RECORD", "a dry record writes no liquid section and reads back with none");
    SceneTables bad = t;
    bad.liquid_maps[0].item_count = 9;   // the item range runs past the table
    const std::vector<uint8_t> bbytes = SerializeRecord(bad);
    SceneTables bback;
    const bool bok = ParseRecord(bbytes.data(), bbytes.size(), bback, err);
    Check(bok && bback.liquid_maps.empty() && bback.liquid_items.empty() && bback.liquid_errors == 1, "RECORD", "an item range outside the table drops the three sections and counts one error; the record still reads (errors %u)", bback.liquid_errors);
    // ---- [E2 INPUT] BuildE2Input carries the rows, their items and only the pictures they name ----
    E2Input in;
    BuildE2Input(t, E2Faults(), in);
    Check(in.liquid.size() == 1 && in.liquid[0].items.size() == 2 && in.liquid_pictures.size() == 1 && in.liquid_pictures[0]->key == kWrap && in.sweat_missing.n == 0, "E2 INPUT", "BuildE2Input: %zu map, %zu items, %zu picture, nothing missing", in.liquid.size(), in.liquid.empty() ? size_t(0) : in.liquid[0].items.size(), in.liquid_pictures.size());
    E2Input dry_in;
    BuildE2Input(dry, E2Faults(), dry_in);
    Check(dry_in.liquid.empty() && dry_in.sweat_missing.n == 0, "E2 INPUT", "a dry record is not a missing input");
  }

  // ---- [JOIN] one invisible object ahead of two bodies: the frame index (1, 2) finds the engine object, the dense index of the front-end's objs list (0, 1) does not ----
  {
    struct OE {   /* the rows of SceneFrame::object_engine (front_end.h) */ uint32_t object, engine; };
    const std::vector<OE> object_engine = {{1, 0xA0A0u}, {2, 0xB0B0u}};   // frame.objects: [0] invisible, [1] body A, [2] body B
    const uint32_t dense_a = 0, dense_b = 1;                 // their positions in objs (the invisible one is skipped)
    const bool right = EngineObjectOf(object_engine, 1) == 0xA0A0u && EngineObjectOf(object_engine, 2) == 0xB0B0u && EngineObjectOf(object_engine, 0) == 0;
    const bool dense_wrong = EngineObjectOf(object_engine, dense_a) != 0xA0A0u && EngineObjectOf(object_engine, dense_b) != 0xB0B0u;
    Check(right && dense_wrong && (plant != "join_dense"), "JOIN", "the engine object is found by the frame index (right %d), the dense index would join the wrong body or none (%d)", int(right), int(dense_wrong));
  }

  // ---- [BLOOD SIZE] S105 A12: the blood decals' real sizes (chCommon.pac PACH 0 entry 10: 64x128 = 4 KB .. 256x256 = 32 KB, DXT1) are read, kept, and round-trip through section 53 ----
  {
    lq::Tracker tr;
    if (plant == "old_cap") tr.plant = lq::Plant::OldPictureCap;
    BuildWorld(false, false, false);
    g_pix.assign(0x20000, 0);
    constexpr uint32_t kWrapB = 0x40051000u, kHdrB = 0x40061000u, kWrapC = 0x40052000u, kHdrC = 0x40062000u;
    constexpr uint32_t kOffB = 0x1000u, kOffC = 0x8000u;   // untiled DXT1: B 64x128 (16 x 32 blocks, pitch 64 texels), C 256x256 (64 x 64 blocks, pitch 256 texels); bases are 4 KB aligned
    // rows are padded to 32 blocks (360 habit H-CS2): B's 16 blocks sit in a 256-byte stride, C's 64 blocks fill a 512-byte stride; the expected payload is the rows without padding
    std::vector<uint8_t> want_b, want_c;
    for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 128; ++x) g_pix[kOffB + y * 256 + x] = uint8_t(y * 31 + x * 7 + 1), want_b.push_back(g_pix[kOffB + y * 256 + x]);
    for (uint32_t y = 0; y < 64; ++y)
      for (uint32_t x = 0; x < 512; ++x) g_pix[kOffC + y * 512 + x] = uint8_t(y * 13 + x * 5 + 3), want_c.push_back(g_pix[kOffC + y * 512 + x]);
    const auto put_tex = [](uint32_t wrap, uint32_t hdr, uint32_t off, uint32_t w, uint32_t h) {
      e5::PutWord(wrap, lq::kWrapperVtableA), e5::PutWord(wrap + 80, hdr);
      e5::PutWord(hdr + 28, (w / 32u) << 22), e5::PutWord(hdr + 32, (kPixBase + off) | 18u), e5::PutWord(hdr + 36, (w - 1) | ((h - 1) << 13));
      e5::PutWord(hdr + 40, 0), e5::PutWord(hdr + 44, 0), e5::PutWord(hdr + 48, 1u << 9);
    };
    put_tex(kWrapB, kHdrB, kOffB, 64, 128);
    put_tex(kWrapC, kHdrC, kOffC, 256, 256);
    PutItem(kItemA, kItemB, 0, 0x80000000u, kWrapB, 0, 0.30f, 0.40f, 0.38f, 0.52f);   // two live blood items (type 0)
    PutItem(kItemB, 0, 0, 0x80000000u, kWrapC, 0, 0.60f, 0.60f, 0.70f, 0.70f);
    reader(tr, kMap, 90);
    tr.CloseBefore(91);
    lq::FrameBuffer fb;
    const bool got = tr.TakeNext(fb);
    const lq::Stats st = tr.GetStats();
    const bool two = got && fb.pictures.size() == 2 && fb.maps.size() == 1 && fb.maps[0].items.size() == 2 && fb.maps[0].items[0].picture == kWrapB && fb.maps[0].items[1].picture == kWrapC;
    Check(two && st.tex_size == 0 && st.tex_fail == 0, "BLOOD SIZE", "a 64x128 and a 256x256 DXT1 blood item both get a picture (pictures %zu, tex_size %llu tex_fail %llu)", got ? fb.pictures.size() : size_t(0), (unsigned long long)st.tex_size, (unsigned long long)st.tex_fail);
    if (two) {
      const LiquidPictureRow& pb = *fb.pictures[0];
      const LiquidPictureRow& pc = *fb.pictures[1];
      Check(pb.width == 64 && pb.height == 128 && pb.nbytes == 4096 && LiquidPictureValid(pb) && std::memcmp(pb.bytes, want_b.data(), 4096) == 0 &&
                pc.width == 256 && pc.height == 256 && pc.nbytes == 32768 && LiquidPictureValid(pc) && std::memcmp(pc.bytes, want_c.data(), 32768) == 0,
            "BLOOD SIZE", "the blocks come out whole: %ux%u %u bytes and %ux%u %u bytes", pb.width, pb.height, pb.nbytes, pc.width, pc.height, pc.nbytes);
      SceneTables t;
      for (lq::MapEntry& m : fb.maps) {
        m.row.item_first = uint32_t(t.liquid_items.size());
        t.liquid_items.insert(t.liquid_items.end(), m.items.begin(), m.items.end());
        t.liquid_maps.push_back(m.row);
      }
      for (const auto& pp : fb.pictures) t.liquid_pictures.push_back(*pp);
      const std::vector<uint8_t> bytes = SerializeRecord(t);
      SceneTables back;
      std::string err;
      const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
      Check(ok && back.liquid_pictures.size() == 2 && back.liquid_pictures[1].hash[0] == pc.hash[0] && back.liquid_pictures[1].hash[1] == pc.hash[1] && (pc.hash[0] | pc.hash[1]) != 0, "BLOOD SIZE", "the picture's hash (made once at the read) survives the record");
    Check(ok && back.liquid_errors == 0 && back.liquid_pictures.size() == 2 && back.liquid_pictures[1].nbytes == 32768 && std::memcmp(back.liquid_pictures[1].bytes, pc.bytes, 32768) == 0, "BLOOD SIZE",
            "section 53 carries the 32 KB picture through a record round trip (errors %u: %s)", back.liquid_errors, err.c_str());
    } else {
      Check(false, "BLOOD SIZE", "the blood pictures were refused (tex_size %llu)", (unsigned long long)st.tex_size);
    }
  }

  // ---- [BLOOD OVERSIZE / TILED] S105 A12 review F4: a 512x256 DXT1 item (64 KB) is refused for its size alone (tex_size 1, tex_fail 0); a TILED 256x256 blood texture reads whole ----
  {
    lq::Tracker tr;
    if (plant == "old_cap") tr.plant = lq::Plant::OldPictureCap;
    BuildWorld(false, false, false);
    g_pix.assign(0x40000, 0);
    constexpr uint32_t kWrapB = 0x40051000u, kHdrB = 0x40061000u, kWrapC = 0x40052000u, kHdrC = 0x40062000u;
    constexpr uint32_t kOffB = 0x1000u, kOffC = 0x20000u;
    const auto put_tex = [](uint32_t wrap, uint32_t hdr, uint32_t off, uint32_t w, uint32_t h, bool tiled) {
      e5::PutWord(wrap, lq::kWrapperVtableA), e5::PutWord(wrap + 80, hdr);
      e5::PutWord(hdr + 28, (tiled ? 0x80000000u : 0u) | ((w / 32u) << 22)), e5::PutWord(hdr + 32, (kPixBase + off) | 18u), e5::PutWord(hdr + 36, (w - 1) | ((h - 1) << 13));
      e5::PutWord(hdr + 40, 0), e5::PutWord(hdr + 44, 0), e5::PutWord(hdr + 48, 1u << 9);
    };
    put_tex(kWrapB, kHdrB, kOffB, 512, 256, false);   // 128 x 64 blocks x 8 = 65536 B: over the 32 KB row
    put_tex(kWrapC, kHdrC, kOffC, 256, 256, true);
    std::vector<uint8_t> want;
    for (uint32_t y = 0; y < 64; ++y)
      for (uint32_t x = 0; x < 64; ++x)
        for (uint32_t b = 0; b < 8; ++b) {
          const uint8_t v = uint8_t(y * 5 + x * 3 + b + 1);
          g_pix[kOffC + live::guest_tex::TiledOffset2D(int32_t(x), int32_t(y), 64, 3) + b] = v;
          want.push_back(v);
        }
    PutItem(kItemA, kItemB, 0, 0x80000000u, kWrapB, 0, 0.30f, 0.40f, 0.38f, 0.52f);
    PutItem(kItemB, 0, 0, 0x80000000u, kWrapC, 0, 0.60f, 0.60f, 0.70f, 0.70f);
    reader(tr, kMap, 95);
    tr.CloseBefore(96);
    lq::FrameBuffer fb;
    const bool got = tr.TakeNext(fb);
    const lq::Stats st = tr.GetStats();
    Check(got && st.tex_size == 1 && st.tex_fail == 0, "BLOOD OVERSIZE", "a 512x256 DXT1 item is counted tex_size once, never tex_fail (tex_size %llu tex_fail %llu)", (unsigned long long)st.tex_size, (unsigned long long)st.tex_fail);
    Check(got && fb.pictures.size() == 1 && fb.pictures[0]->width == 256 && fb.pictures[0]->nbytes == 32768 && std::memcmp(fb.pictures[0]->bytes, want.data(), 32768) == 0, "BLOOD TILED",
          "a tiled 256x256 DXT1 blood texture reads whole (pictures %zu)", got ? fb.pictures.size() : size_t(0));
  }

  if (!plant.empty()) {
    const bool caught = g_fail > 0;
    std::printf("%s  plant %s: %s\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? g_tokens.c_str() : "NOT CAUGHT");
    return caught ? 0 : 1;
  }
  std::printf(g_fail ? "FAIL  %d check(s) failed %s\n" : "PASS  swf_unit: every check passed\n", g_fail, g_tokens.c_str());
  return g_fail ? 1 : 0;
}
