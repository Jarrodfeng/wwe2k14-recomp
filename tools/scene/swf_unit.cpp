// Lane SWF (Session 103): unit proof of the liquid-map readers: the hook's tracker (src\scene_live\liquid_hook.h) over a FIXTURE guest memory (tools\scene\e5_fake_memory.h: no capture holds the CCLM component or its items),
// the record sections 51 / 52 / 53 (round trip, a bad layout drops the three, an older record has none) and BuildE2Input's use of them.
//   swf_unit                 every line PASS
//   swf_unit --plant NAME    exit 0 only when the check that sees the plant FAILS: last_call (LAST CALL), vtable (TEXTURE CLASS), stale_frame (FRAME STAMP), early_outs (EARLY OUTS);
//                            S108 BLD2: rebuild_skip (REBUILD: the old host, a rebuild row's rows never drawn), alt_left_set (EDGE: +200 not written back, the next row wipes the map), alt_carry (REBUILD LAST CALL);
//                            S108 BLD4: mirror_skip (MIRROR: an inverted box is empty);
//                            S108 BLD3: no_resend (REBUILD RESEND: the package rides only on its own frame's row);
//                            S108 BLD3 review: edge_ack (REBUILD ACK), no_prune (REBUILD PRUNE), loop1_one_call / regions_capped / pictures_capped (REBUILD PAGES)
// Marks: the memory is a FIXTURE; what is [C] is the field layout (SWB 1a / 1d: q asm sub_8279ACD8 / sub_82798748 / sub_82798888 / sub_82726170) and the texture chain (q vtable 0x8206ADB8 slot 2 = sub_82BD4288, guest_texture.h).
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/liquid_join.h"
#include "../../src/renderer_scene/liquid_plan.h"   // S108 BLD2
#include "../../src/renderer_scene/live_maps_e2.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_liquid.h"
#include "../../src/renderer_scene/rows_pictures.h"
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

  // ---- [REBUILD] S108 BLD2 (docs\research\s108\BLD_plan.md section 10): the game's rebuild (+200 set with +136 by sub_82727F30) read as sub_8279A328 draws it, planned in the game's order, written back ----
  // Fixture laid out as the game's: pool [map+84] = 4 items: blood sub-pool [0, 2) (+68 / +60), sweat sub-pool [2, 4) (+72 / +64); region lists [map+160] (blood, 2 records) / [map+164] (sweat, 1) with their entries
  // [map+76] / [map+80] (20 bytes: +8 texture, +12 dirty, +16 flag) and records [[list]+8] + 20j (+4..+16 = corners in map UV).
  {
    constexpr uint32_t kPool = 0x40070000u, kList0 = 0x40080000u, kList1 = 0x40080100u, kRecs0 = 0x40081000u, kRecs1 = 0x40081100u, kEnt0 = 0x40082000u, kEnt1 = 0x40082100u;
    const auto world = [&] {
      BuildWorld(true, false, false);
      e5::PutWord(kMap + 200, 1);
      e5::PutWord(kMap + 84, kPool), e5::PutWord(kMap + 60, 2), e5::PutWord(kMap + 64, 2), e5::PutWord(kMap + 68, 0), e5::PutWord(kMap + 72, 2);
      PutItem(kPool + 0 * 160, 0, 0, 0x80000000u | 0x40000000u | 0x10000000u | 0x08000000u, kWrap, 1, 0.10f, 0.10f, 0.20f, 0.20f);   // blood, finished by the rebuild (drawn by its REGION, not here)
      PutItem(kPool + 1 * 160, 0, 0, 0, kWrap, 1, 0.12f, 0.12f, 0.22f, 0.22f);                                                         // blood, never activated
      PutItem(kPool + 2 * 160, 0, 1, 0x80000000u | 0x40000000u | 0x10000000u | 0x08000000u, kWrap, 0, 0.30f, 0.30f, 0.40f, 0.40f);   // sweat, finished: loop 1's disc
      PutItem(kPool + 3 * 160, 0, 1, 0x08000000u, kWrap, 0, 0.32f, 0.32f, 0.42f, 0.42f);                                               // sweat, bit 27 alone: consumed, not drawn
      e5::PutWord(kMap + 160, kList0), e5::PutWord(kMap + 164, kList1), e5::PutWord(kMap + 76, kEnt0), e5::PutWord(kMap + 80, kEnt1);
      e5::PutWord(kList0 + 4, 2), e5::PutWord(kList0 + 8, kRecs0), e5::PutWord(kList1 + 4, 1), e5::PutWord(kList1 + 8, kRecs1);
      const float r0[4] = {0.25f, 0.50f, 0.75f, 1.0f}, r1[4] = {0.0f, 0.0f, 0.1f, 0.1f}, r2[4] = {0.5f, 0.0f, 0.6f, 0.25f};
      for (int q = 0; q < 4; ++q) e5::PutFloat(kRecs0 + 4 + 4 * q, r0[q]), e5::PutFloat(kRecs0 + 20 + 4 + 4 * q, r1[q]), e5::PutFloat(kRecs1 + 4 + 4 * q, r2[q]);
      for (uint32_t o = 0; o < 40; o += 4) e5::PutWord(kEnt0 + o, 0), e5::PutWord(kEnt1 + o, 0);
      e5::PutWord(kEnt0 + 0, 0), e5::PutWord(kEnt0 + 4, 2), e5::PutWord(kEnt0 + 8, kWrap), e5::PutWord(kEnt0 + 12, 1), e5::PutWord(kEnt0 + 16, 0);   // blood region 0: dirty, an item with bits 27 + 30
      e5::PutWord(kEnt0 + 20 + 8, kWrap), e5::PutWord(kEnt0 + 20 + 12, 0), e5::PutWord(kEnt0 + 20 + 16, 1);                                       // blood region 1: clean (not drawn)
      e5::PutWord(kEnt1 + 8, kWrap), e5::PutWord(kEnt1 + 12, 1), e5::PutWord(kEnt1 + 16, 1);                                                       // sweat region 0: dirty
    };
    const auto rd = [](uint32_t a, uint32_t& v) { return ReadWord(a, v); };
    const auto wr = [](uint32_t a, uint32_t v) { e5::PutWord(a, v); };
    const auto take = [](lq::Tracker& t, uint64_t next, lq::FrameBuffer& fb) {   // the newest closed frame with a row (CloseBefore leaves an empty frame open between two calls)
      t.CloseBefore(next);
      bool any = false;
      for (lq::FrameBuffer f; t.TakeNext(f);)
        if (f.maps.size() == 1) fb = std::move(f), any = true;
      return any;
    };
    const liquid_plan::Plant pp = plant == "rebuild_skip" ? liquid_plan::Plant::RebuildSkip : liquid_plan::Plant::None;

    RebuildAck().Reset();
    lq::Tracker tr;
    world();
    lq::AltConsume c;
    const uint32_t rec = tr.OnNode(kMap, 200, ReadWord, ReadBytes, ReadPhys, &c);
    lq::FrameBuffer fb;
    const bool got = take(tr, 201, fb);
    uint32_t live = 0, rebuild = 0, regions = 0;
    if (got)
      for (const LiquidItemRow& it : fb.maps[0].items) live += !(it.flags & (kLiRebuild | kLiRegion)), rebuild += (it.flags & kLiRebuild) != 0, regions += (it.flags & kLiRegion) != 0;
    Check(got && (rec & 4u) && (fb.maps[0].row.flags & kLmAlt) && (fb.maps[0].row.flags & kLmClear) && live == 2 && rebuild == 1 && regions == 2 && fb.maps[0].row.item_count == 5 &&
          (fb.maps[0].row.flags & kLmRebuild) && fb.maps[0].row.reserved[1] == 1, "REBUILD",
          "the rebuild row: alt + clear, 2 live items, 1 sweat disc (bits 27 + 30), 2 dirty regions (rec %u live %u rebuild %u regions %u)", rec, live, rebuild, regions);
    Check(c.map == kMap && c.items.size() == 2 && c.items[0] == kPool + 320 && c.items[1] == kPool + 480 && c.entries.size() == 2 && c.entries[0] == kEnt0 && c.entries[1] == kEnt1, "REBUILD",
          "the write-back names the two sweat items with bit 27 and the two dirty entries (items %zu entries %zu)", c.items.size(), c.entries.size());
    if (got) {
      const lq::MapEntry& m = fb.maps[0];
      const liquid_plan::Plan p = liquid_plan::Make(m.row, m.items, liquid_plan::RebuildIsNew(m.row, 0), pp);   // (the host: nothing applied yet)
      const bool order = p.alt && p.steps.size() == 3 && p.steps[0].kind == liquid_plan::Kind::Rebuild && p.steps[0].write_mask == liquid_plan::kMaskB && p.steps[1].kind == liquid_plan::Kind::Region && p.steps[1].pass == 0 &&
                         p.steps[1].write_mask == liquid_plan::kMaskR && p.steps[2].kind == liquid_plan::Kind::Region && p.steps[2].pass == 1 && p.steps[2].write_mask == liquid_plan::kMaskG;
      Check(order && p.live_on_alt == 2, "REBUILD", "planned in the game's order: sweat disc (mask B), blood region (mask R), sweat region (mask G); the live items are not drawn (%zu steps)", p.steps.size());
      if (order) {
        const LiquidItemRow& blood = m.items[p.steps[1].item];
        Check(std::fabs(blood.world_param[0] - 0.25f) < 1e-6f && std::fabs(blood.world_param[3] - 1.0f) < 1e-6f && blood.picture == kWrap && p.steps[1].src[1] == 1.f && p.steps[1].dst[0] == 1.f && p.steps[2].src[1] == 0.f &&
                  p.steps[2].dst[1] == 1.f && p.steps[2].dst[2] == 1.f, "REBUILD", "the blood region: its record's corners, the entry's texture, c203 (1, 1, 0, 0) (flag 0), c204 (1, 0, 0, 0); the sweat region c203 (1, 0, 0, 0) c204 (0, 1, 1, 0)");
        float sc[4];
        liquid_plan::RegionScreenScale(blood.world_param, sc);
        const float x1 = 1.f * sc[0] + sc[2], y1 = 1.f * sc[1] + sc[3], x0 = sc[2], y0 = sc[3];   // the unit quad's corners in clip space; the game's: p * (2, -2) + (-1, 1)
        Check(std::fabs(x0 - (0.25f * 2 - 1)) < 1e-6f && std::fabs(y0 - (1 - 0.5f * 2)) < 1e-6f && std::fabs(x1 - (0.75f * 2 - 1)) < 1e-6f && std::fabs(y1 - (1 - 1.0f * 2)) < 1e-6f, "REBUILD", "the region quad lands where the game's +240 quad does");
      }
    }
    // ---- [MIRROR] S108 BLD4: a region stored mirrored (x0 > x1) and a rebuild disc with a mirrored WorldParam are valid data: the plan keeps both steps, the bound is the ordered box, the quad keeps the mirror (synthetic numbers) ----
    {
      const liquid_plan::Plant mp = plant == "mirror_skip" ? liquid_plan::Plant::MirrorSkip : liquid_plan::Plant::None;
      LiquidMapRow mr{};
      mr.flags = kLmAlt | kLmRebuild;
      mr.reserved[1] = 1;
      std::vector<LiquidItemRow> mit(2);
      mit[0].flags = kLiRegion, mit[0].type = 0, mit[0].picture = 7;
      const float reg[4] = {0.8f, 0.25f, 0.75f, 0.5f};   // x1 < x0
      std::memcpy(mit[0].world_param, reg, 16);
      mit[1].flags = kLiRebuild, mit[1].type = 1, mit[1].picture = 7;
      const float disc[4] = {0.5f, 0.6f, 0.4f, 0.7f};    // x1 < x0
      std::memcpy(mit[1].world_param, disc, 16);
      const liquid_plan::Plan mp2 = liquid_plan::Make(mr, mit, true);
      float b0[4] = {}, b1[4] = {};
      if (mp2.steps.size() == 2) {
        liquid_plan::StepBox(mp2.steps[0], mit[mp2.steps[0].item], 1024, 512, b0, mp);   // (the plan's order: the disc first)
        liquid_plan::StepBox(mp2.steps[1], mit[mp2.steps[1].item], 1024, 512, b1, mp);
      }
      const bool two = mp2.steps.size() == 2 && mp2.steps[0].kind == liquid_plan::Kind::Rebuild && mp2.steps[1].kind == liquid_plan::Kind::Region;
      // (BLD4 review: the ordered box snapped outward to 32: disc x 409.6..512 -> 384..512, region x 768..819.2 -> 768..832, y 128..256 as is)
      Check(two && b0[2] > b0[0] && b0[3] > b0[1] && std::fabs(b0[0] - 384.f) < 1e-3f && std::fabs(b0[2] - 512.f) < 1e-3f &&
            b1[2] > b1[0] && b1[3] > b1[1] && std::fabs(b1[0] - 768.f) < 1e-3f && std::fabs(b1[2] - 832.f) < 1e-3f && std::fabs(b1[1] - 128.f) < 1e-3f && std::fabs(b1[3] - 256.f) < 1e-3f, "MIRROR",
            "a mirrored region and a mirrored rebuild disc: both steps planned, the bound is the ordered box (x %.1f..%.1f, %.1f..%.1f)", b1[0], b1[2], b0[0], b0[2]);
      // [MIP EDGE] (BLD4 review): the mips are rebuilt per draw over its bound at level k (live_map_sweat.cpp); a bound that is not a multiple of 2^k leaves the level-k texels whose footprint the draw only partly
      // touches stale (a rebuild has just cleared the map: they stay 0). The live items' rects are the game's 32-snapped ones; the rebuild's discs and regions must be too: aligned OUTWARD to 32, containing the box.
      const auto aligned = [](const float b[4], float x0, float y0, float x1, float y1) {
        for (int k = 0; k < 4; ++k)
          if (std::fmod(b[k], 32.f) != 0.f) return false;
        return b[0] <= x0 && b[1] <= y0 && b[2] >= x1 && b[3] >= y1;
      };
      Check(two && aligned(b0, 0.4f * 1024, 0.6f * 512, 0.5f * 1024, 0.7f * 512) && aligned(b1, 0.75f * 1024, 0.25f * 512, 0.8f * 1024, 0.5f * 512), "MIP EDGE",
            "a rebuild disc's and a region's bound are 32-aligned outward and hold the box (disc x %.1f..%.1f y %.1f..%.1f, region x %.1f..%.1f)", b0[0], b0[2], b0[1], b0[3], b1[0], b1[2]);
      float sc[4];
      liquid_plan::RegionScreenScale(reg, sc);
      LiquidItemRow live;
      const float lr[4] = {64.f, 0.f, 32.f, 32.f};
      std::memcpy(live.rect, lr, 16);
      Check(sc[0] < 0.f && sc[1] < 0.f && liquid_plan::Mirrored(reg) && liquid_plan::Mirrored(disc) && liquid_plan::Mirrored(live.rect), "MIRROR",
            "the quad scale keeps the mirror (x scale %.3f), Mirrored sees the stored corners; a live item's rect is kept as the game's (never reordered)", sc[0]);
      liquid_plan::Step is;
      is.kind = liquid_plan::Kind::Item;
      float bl[4];
      liquid_plan::StepBox(is, live, 1024, 512, bl, mp);
      Check(bl[0] == 64.f && bl[2] == 32.f, "MIRROR", "a live item's bound is its own rect (the held-back hole is the host's, counted)");
    }
    // the edge: the glue's write-back, then the next frame is a normal row and an item activated after the rebuild paints through the normal path
    if (plant != "alt_left_set") lq::ApplyAltConsume(c, rd, wr);
    e5::PutWord(kMap + 136, 0);   // (the glue consumes +136 the same way)
    uint32_t v200 = 9, s3 = 9, e0 = 9, s0 = 0;
    ReadWord(kMap + 200, v200), ReadWord(kPool + 480 + 60, s3), ReadWord(kEnt0 + 12, e0), ReadWord(kPool + 60, s0);
    Check(v200 == 0 && s3 == 0 && e0 == 0 && (s0 & 0x08000000u), "EDGE", "+200 = 0, the sweat item's bit 27 and the entry's +12 cleared; the blood item's bit 27 untouched (loop 1 walks the sweat sub-pool only) (+200 %u, item 0x%08X, entry %u)", v200, s3, e0);
    PutItem(kItemA, kItemB, 0, 0x80000000u, kWrap, 1, 0.50f, 0.50f, 0.55f, 0.55f);   // a fresh blood item (no bit 30), activated after the rebuild
    tr.OnNode(kMap, 202, ReadWord, ReadBytes, ReadPhys, &c);
    lq::FrameBuffer fb2;
    const bool got2 = take(tr, 203, fb2);
    const liquid_plan::Plan p2 = got2 ? liquid_plan::Make(fb2.maps[0].row, fb2.maps[0].items, liquid_plan::RebuildIsNew(fb2.maps[0].row, got ? fb.maps[0].row.reserved[1] : 0), pp) : liquid_plan::Plan{};   // (the host applied frame 200's package; frame 202 may re-send it: not new)
    Check(got2 && !(fb2.maps[0].row.flags & kLmAlt) && !(fb2.maps[0].row.flags & kLmClear) && p2.steps.size() == 1 && p2.steps[0].kind == liquid_plan::Kind::Item && p2.steps[0].pass == 1, "EDGE",
          "the next row is not a rebuild (no clear: the map is kept) and the new item paints through the normal paste (got %d flags 0x%X items %zu steps %zu)", int(got2), got2 ? fb2.maps[0].row.flags : 0u,
          got2 ? fb2.maps[0].items.size() : size_t(0), p2.steps.size());

    // a later call of the same frame (the first call's glue consumed +200) keeps the rebuild rows
    RebuildAck().Reset();
    lq::Tracker t3;
    if (plant == "alt_carry") t3.plant = lq::Plant::AltNotCarried;
    world();
    lq::AltConsume c3;
    t3.OnNode(kMap, 300, ReadWord, ReadBytes, ReadPhys, &c3);
    lq::ApplyAltConsume(c3, rd, wr);
    lq::AltConsume c4;
    const uint32_t rec4 = t3.OnNode(kMap, 300, ReadWord, ReadBytes, ReadPhys, &c4);
    lq::FrameBuffer fb3;
    const bool got3 = take(t3, 301, fb3);
    uint32_t regions3 = 0;
    if (got3)
      for (const LiquidItemRow& it : fb3.maps[0].items) regions3 += (it.flags & kLiRegion) != 0;
    Check(got3 && !(rec4 & 4u) && c4.Empty() && (fb3.maps[0].row.flags & kLmAlt) && regions3 == 2 && fb3.maps[0].row.item_count == fb3.maps[0].items.size(), "REBUILD LAST CALL",
          "the second call (no request left, nothing to consume) keeps the first call's rebuild rows (regions %u)", regions3);

    // [REBUILD RESEND] S108 BLD3 (run 18: the hook recorded 4 rebuild rows, the host applied none: the records of the scene change were never drawn). The package rides on the map's later rows until the host acknowledges it;
    // a host that never saw the ALT row applies it from a later row (clear, the package in the game's order, then that row's live items); once acknowledged it is not sent again.
    RebuildAck().Reset();
    lq::Tracker t4;
    if (plant == "no_resend") t4.plant = lq::Plant::NoResend;
    world();
    lq::AltConsume c5;
    t4.OnNode(kMap, 400, ReadWord, ReadBytes, ReadPhys, &c5);
    lq::ApplyAltConsume(c5, rd, wr);
    e5::PutWord(kMap + 136, 0);
    lq::FrameBuffer drop;
    take(t4, 401, drop);   // (frame 400's record: never drawn by the host)
    lq::AltConsume c6;
    t4.OnNode(kMap, 402, ReadWord, ReadBytes, ReadPhys, &c6);
    lq::FrameBuffer fb4;
    const bool got4 = take(t4, 403, fb4);
    uint32_t rb4 = 0, rg4 = 0;
    if (got4)
      for (const LiquidItemRow& it : fb4.maps[0].items) rb4 += (it.flags & kLiRebuild) != 0, rg4 += (it.flags & kLiRegion) != 0;
    const bool late = got4 && (fb4.maps[0].row.flags & kLmRebuild) && !(fb4.maps[0].row.flags & kLmAlt) && fb4.maps[0].row.reserved[1] == 1 && rb4 == 1 && rg4 == 2 && c6.Empty() &&
                      fb4.maps[0].row.item_count == fb4.maps[0].items.size();
    Check(late, "REBUILD RESEND", "the next frame's row re-sends the unacknowledged package (generation %u, discs %u, regions %u), with no write-back of its own", got4 ? fb4.maps[0].row.reserved[1] : 0u, rb4, rg4);
    if (got4) {
      const liquid_plan::Plan p4 = liquid_plan::Make(fb4.maps[0].row, fb4.maps[0].items, liquid_plan::RebuildIsNew(fb4.maps[0].row, 0));   // (a host that never saw frame 400)
      bool package_first = p4.rebuild && p4.steps.size() >= 3 && p4.steps[0].kind == liquid_plan::Kind::Rebuild && p4.steps[1].kind == liquid_plan::Kind::Region && p4.steps[2].kind == liquid_plan::Kind::Region;
      for (size_t i = 3; i < p4.steps.size(); ++i) package_first = package_first && p4.steps[i].kind == liquid_plan::Kind::Item;
      const liquid_plan::Plan p4b = liquid_plan::Make(fb4.maps[0].row, fb4.maps[0].items, liquid_plan::RebuildIsNew(fb4.maps[0].row, 1));   // (a host that applied generation 1 already)
      bool none_again = !p4b.rebuild;
      for (const liquid_plan::Step& st : p4b.steps) none_again = none_again && st.kind == liquid_plan::Kind::Item;
      Check(package_first && none_again, "REBUILD RESEND", "a host that missed the ALT row applies the package from the later row, package first, then its live items (%zu steps); a host that applied it draws only the live items", p4.steps.size());
    }
    RebuildAck().Ack(kMap, 1);
    t4.OnNode(kMap, 404, ReadWord, ReadBytes, ReadPhys, &c6);
    lq::FrameBuffer fb5;
    const bool got5 = take(t4, 405, fb5);
    const lq::Stats s4 = t4.GetStats();
    Check(got5 && !(fb5.maps[0].row.flags & kLmRebuild) && s4.resend_acked == 1 && (plant == "no_resend" || s4.resent == 1), "REBUILD RESEND",
          "acknowledged: the package is not sent again (resent %llu, acked %llu)", (unsigned long long)s4.resent, (unsigned long long)s4.resend_acked);

    // [REBUILD ACK] S108 BLD3 review: the host's own function (liquid_plan::AcceptRebuild, what live_map_sweat.cpp calls) acknowledges EVERY package row, so a lost acknowledgement (its slot reused) is repaired
    // by the next drawn row and the re-send stops; the twin (`edge_ack`, BLD3: acknowledged only when newly applied) re-sends until the expiry.
    {
      RebuildAck().Reset();
      lq::Tracker t5;
      world();
      lq::AltConsume a;
      t5.OnNode(kMap, 500, ReadWord, ReadBytes, ReadPhys, &a);
      lq::ApplyAltConsume(a, rd, wr);
      e5::PutWord(kMap + 136, 0);
      lq::FrameBuffer f0, f1, f2;
      const bool g0 = take(t5, 501, f0);
      t5.OnNode(kMap, 502, ReadWord, ReadBytes, ReadPhys, &a);   // (the hook runs ahead of the host: this row re-sends generation 1)
      const bool g1 = take(t5, 503, f1);
      uint32_t applied = 0;
      const bool fresh0 = g0 && liquid_plan::AcceptRebuild(f0.maps[0].row, applied, RebuildAck(), plant == "edge_ack");   // the host draws the ALT row: applied + acknowledged
      RebuildAck().Reset();   // the acknowledgement is lost before the hook read it (its slot reused by other maps)
      const bool fresh1 = g1 && liquid_plan::AcceptRebuild(f1.maps[0].row, applied, RebuildAck(), plant == "edge_ack");   // the host draws the re-sent row: not new (not applied twice)
      t5.OnNode(kMap, 504, ReadWord, ReadBytes, ReadPhys, &a);
      const bool g2 = take(t5, 505, f2);
      Check(fresh0 && g1 && (f1.maps[0].row.flags & kLmRebuild) && !fresh1 && applied == 1 && g2 && !(f2.maps[0].row.flags & kLmRebuild), "REBUILD ACK",
            "applied once (frame 500), the re-sent row (502) acknowledged again after the first acknowledgement was lost: frame 504 sends no package (flags 0x%X)", g2 ? f2.maps[0].row.flags : 0u);
    }

    // [REBUILD PRUNE] S108 BLD3 review: a package whose map is never called again (the game freed it) is dropped at the frame close past kResendFrames, not kept for the session; a map later made at the same
    // address carries the setup's clear (sub_82799A70 +136 = 1) and never receives an old package.
    {
      RebuildAck().Reset();
      lq::Tracker t6;
      if (plant == "no_prune") t6.plant = lq::Plant::NoPrune;
      world();
      lq::AltConsume a;
      t6.OnNode(kMap, 600, ReadWord, ReadBytes, ReadPhys, &a);
      lq::ApplyAltConsume(a, rd, wr);
      e5::PutWord(kMap + 136, 0);
      constexpr uint32_t kOther = 0x40011000u;   // another map, called every frame (the frames advance; kMap is never called again)
      for (uint32_t o = 0; o < 0x1000; o += 4)
        if (const auto it = e5::Mem().find(kMap + o); it != e5::Mem().end()) e5::PutWord(kOther + o, it->second);
      e5::PutWord(kOther + 200, 0), e5::PutWord(kOther + 136, 1);
      for (uint64_t f = 601; f <= 600 + lq::kResendFrames + 2; ++f) t6.OnNode(kOther, f, ReadWord, ReadBytes, ReadPhys);
      const lq::Stats s6 = t6.GetStats();
      Check(s6.resend_expired == 1, "REBUILD PRUNE", "the freed map's package is dropped at the frame close (expired %llu)", (unsigned long long)s6.resend_expired);
      lq::Tracker t7;   // the reuse: a package waits, then a new map is set up at the same address
      RebuildAck().Reset();
      world();
      t7.OnNode(kMap, 700, ReadWord, ReadBytes, ReadPhys, &a);
      lq::ApplyAltConsume(a, rd, wr);
      e5::PutWord(kMap + 136, 1), e5::PutWord(kMap + 92, 0);   // sub_82799A70: +136 = 1; no live item yet
      t7.OnNode(kMap, 701, ReadWord, ReadBytes, ReadPhys, &a);
      lq::FrameBuffer f7;
      const bool g7 = take(t7, 702, f7);
      Check(g7 && (f7.maps[0].row.flags & kLmClear) && !(f7.maps[0].row.flags & kLmRebuild) && t7.GetStats().resend_superseded == 1, "REBUILD PRUNE",
            "a new map at the same address: its setup clear drops the old package (flags 0x%X)", g7 ? f7.maps[0].row.flags : 0u);
    }
    RebuildAck().Reset();
  }

  // ---- [REBUILD PAGES] S108 BLD3 review: the game pages its rebuild (64 region draws, 63 sweat discs per call of sub_8279A328, +200 kept until the regions are done); the hook records the end state whole:
  // every dirty region, and 63 x (floor(R / 64) + 1) sweat discs; every picture they name travels with the frame. Fixture: 300 dirty regions (list 0: 200, list 1: 100), each its own texture wrapper; 70 sweat items
  // (bits 31 30 28 27, as sub_82727F30 leaves them). Twins: loop1_one_call (63 discs), regions_capped (256 regions), pictures_capped (256 pictures). Then 2 regions only: one call, 63 discs, the rest keep bit 27.
  {
    constexpr uint32_t kPool = 0x40100000u, kList0 = 0x40200000u, kList1 = 0x40200100u, kRecs = 0x40210000u, kEnts = 0x40220000u, kWraps = 0x40300000u;
    const auto world = [&](uint32_t r0, uint32_t r1, uint32_t sweat) {
      BuildWorld(true, false, false);
      e5::PutWord(kMap + 200, 1);
      e5::PutWord(kMap + 84, kPool), e5::PutWord(kMap + 60, 0), e5::PutWord(kMap + 68, 0), e5::PutWord(kMap + 64, sweat), e5::PutWord(kMap + 72, 0);
      for (uint32_t i = 0; i < sweat; ++i) PutItem(kPool + 160 * i, 0, 1, 0x80000000u | 0x40000000u | 0x10000000u | 0x08000000u, kWrap, 0, 0.01f * float(i % 50), 0.3f, 0.01f * float(i % 50) + 0.05f, 0.35f);
      const uint32_t n[2] = {r0, r1}, lists[2] = {kList0, kList1};
      for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t recs = kRecs + 0x8000u * k, ents = kEnts + 0x8000u * k;
        e5::PutWord(kMap + 160 + 4 * k, lists[k]), e5::PutWord(kMap + 76 + 4 * k, ents), e5::PutWord(lists[k] + 4, n[k]), e5::PutWord(lists[k] + 8, recs);
        for (uint32_t j = 0; j < n[k]; ++j) {
          const uint32_t wrap = kWraps + 0x100u * (k * 1024 + j);
          e5::PutWord(wrap, lq::kWrapperVtableA), e5::PutWord(wrap + 80, kHdr), e5::PutWord(wrap + 84, 0), e5::PutWord(wrap + 88, 0);
          const float c[4] = {0.002f * float(j), 0.1f * float(k), 0.002f * float(j) + 0.05f, 0.1f * float(k) + 0.05f};
          for (int q = 0; q < 4; ++q) e5::PutFloat(recs + 20 * j + 4 + 4 * q, c[q]);
          e5::PutWord(recs + 20 * j, 0);
          e5::PutWord(ents + 20 * j + 0, j), e5::PutWord(ents + 20 * j + 4, 1), e5::PutWord(ents + 20 * j + 8, wrap), e5::PutWord(ents + 20 * j + 12, 1), e5::PutWord(ents + 20 * j + 16, 0);
        }
      }
    };
    const auto count = [](const lq::FrameBuffer& fb, uint32_t& discs, uint32_t& regions) {
      discs = regions = 0;
      for (const LiquidItemRow& it : fb.maps[0].items) discs += (it.flags & kLiRebuild) != 0, regions += (it.flags & kLiRegion) != 0;
    };
    lq::Tracker tp;
    if (plant == "loop1_one_call") tp.plant = lq::Plant::Loop1OneCall;
    if (plant == "regions_capped") tp.plant = lq::Plant::RegionsCapped;
    if (plant == "pictures_capped") tp.plant = lq::Plant::PicturesCapped;
    world(200, 100, 70);
    lq::AltConsume c;
    tp.OnNode(kMap, 800, ReadWord, ReadBytes, ReadPhys, &c);
    tp.CloseBefore(801);
    lq::FrameBuffer fb;
    const bool got = tp.TakeNext(fb) && fb.maps.size() == 1;
    uint32_t discs = 0, regions = 0;
    if (got) count(fb, discs, regions);
    const std::vector<std::shared_ptr<const LiquidPictureRow>> pics = got ? fb.pictures : std::vector<std::shared_ptr<const LiquidPictureRow>>{};
    uint32_t named = 0;   // regions whose picture is in the frame's list
    if (got)
      for (const LiquidItemRow& it : fb.maps[0].items) {
        if (!(it.flags & kLiRegion)) continue;
        bool in = false;
        for (const auto& p : pics) in = in || p->key == it.picture;
        named += in ? 1u : 0u;
      }
    const lq::Stats sp = tp.GetStats();
    Check(got && regions == 300 && discs == 70 && c.entries.size() == 300 && c.items.size() == 70 && sp.alt_left == 0 && sp.alt_overflow == 0, "REBUILD PAGES",
          "300 dirty regions = 5 calls of the game: all 300 regions and all 70 sweat discs (reach 315) recorded and consumed (regions %u discs %u, left %llu)", regions, discs, (unsigned long long)sp.alt_left);
    Check(got && named == 300 && pics.size() == 301, "REBUILD PAGES", "every region's picture travels with the frame (%u of 300 named, %zu pictures)", named, pics.size());
    lq::Tracker tq;
    if (plant == "loop1_one_call") tq.plant = lq::Plant::Loop1OneCall;
    world(2, 0, 70);
    lq::AltConsume c2;
    tq.OnNode(kMap, 900, ReadWord, ReadBytes, ReadPhys, &c2);
    tq.CloseBefore(901);
    lq::FrameBuffer fb2;
    const bool got2 = tq.TakeNext(fb2) && fb2.maps.size() == 1;
    uint32_t discs2 = 0, regions2 = 0;
    if (got2) count(fb2, discs2, regions2);
    lq::ApplyAltConsume(c2, [](uint32_t a, uint32_t& v) { return ReadWord(a, v); }, [](uint32_t a, uint32_t v) { e5::PutWord(a, v); });
    uint32_t s62 = 0, s63 = 0, v200 = 9;
    ReadWord(kPool + 160 * 62 + 60, s62), ReadWord(kPool + 160 * 63 + 60, s63), ReadWord(kMap + 200, v200);
    Check(got2 && regions2 == 2 && discs2 == 63 && tq.GetStats().alt_left == 1 && !(s62 & 0x08000000u) && (s63 & 0x08000000u) && v200 == 0, "REBUILD PAGES",
          "2 regions = one call: 63 sweat discs, the 64th and later keep bit 27 as the game leaves them (discs %u, item 62 0x%08X, item 63 0x%08X)", discs2, s62, s63);
  }

  if (!plant.empty()) {
    const bool caught = g_fail > 0;
    std::printf("%s  plant %s: %s\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? g_tokens.c_str() : "NOT CAUGHT");
    return caught ? 0 : 1;
  }
  std::printf(g_fail ? "FAIL  %d check(s) failed %s\n" : "PASS  swf_unit: every check passed\n", g_fail, g_tokens.c_str());
  return g_fail ? 1 : 0;
}
