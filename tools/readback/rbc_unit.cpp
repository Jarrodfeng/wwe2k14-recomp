// S103 lane RBC: unit test of src\renderer_game\readback_tile.h (the guest writer) and readback_ours.h (the site table). No GPU, no game. Built and run by rbc_check.py:
//   rbc_unit                 exit 0 = every case behaved, 1 = a case failed
//   rbc_unit --plant N       (built with -DRBC_PLANTS) the planted twin N: the unit must exit 1 on every one (1 swizzle ignored = the wrong swap, 2 endian skipped, 3 tiled bit ignored, 4 nearest scale)
#define WWE2K14_READBACK_CORE_ONLY
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "renderer_game/readback_ours.h"
#include "renderer_game/thumb_scope.h"
#include "renderer_host/scene_rules.h"

namespace rt = wwe2k14::readback_tile;
namespace ro = wwe2k14::readback_ours;

static int g_bad = 0;
static void Expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_bad;
}

// The six fetch dwords a game texture header holds (host values). base_cpu: the CPU-view address of dword 1's base (the 0xA0000000 view: physical = low 29 bits).
static void Forge(uint32_t w[6], uint32_t width, uint32_t height, bool tiled, uint32_t endian, uint32_t swizzle, uint32_t format = 6, uint32_t base_phys = 0x00100000u) {
  const uint32_t pitch_div32 = (width + 31) / 32;
  w[0] = (tiled ? 0x80000000u : 0u) | (pitch_div32 << 22);
  w[1] = format | (endian << 6) | (0xA0000000u | base_phys);
  w[2] = (width - 1) | ((height - 1) << 13);
  w[3] = swizzle << 1;
  w[4] = 0;
  w[5] = 1u << 9;
}
constexpr uint32_t kSwzIdentity = 0u | (1u << 3) | (2u << 6) | (3u << 9);
constexpr uint32_t kSwzBgra = 2u | (1u << 3) | (0u << 6) | (3u << 9);   // the fetch X, Y, Z, W read memory bytes 2, 1, 0, 3

static std::vector<uint8_t> g_mem;
static uint8_t* Phys(uint32_t addr) { return addr == 0x00100000u ? g_mem.data() : nullptr; }
static std::vector<uint8_t> g_mem2;   // RBH: the second texture (R8's 128x128 at 0x00800000); g_mem is R9's
static uint8_t* Phys2(uint32_t addr) { return addr == 0x00100000u ? g_mem.data() : addr == 0x00800000u ? g_mem2.data() : nullptr; }

static std::vector<uint8_t> Picture(uint32_t w, uint32_t h) {
  std::vector<uint8_t> p(size_t(w) * h * 4);
  uint32_t s = 12345;
  for (auto& b : p) s = s * 1664525u + 1013904223u, b = uint8_t(s >> 24);
  return p;
}

// One round trip: write a known picture through the writer, read it back through the inverse, compare; also every texel's offset is distinct and inside the extent.
static void RoundTrip(uint32_t width, uint32_t height, bool tiled, uint32_t endian, uint32_t swizzle, const char* name) {
  uint32_t w[6];
  Forge(w, width, height, tiled, endian, swizzle);
  rt::Plan p;
  Expect(rt::MakePlan(w, p) == rt::Why::Ok && p.width == width && p.height == height && p.tiled == tiled, (std::string(name) + ": the fetch dwords make the plan").c_str());
  g_mem.assign(size_t(p.ExtentBytes()) + 64, 0xCD);
  const std::vector<uint8_t> pic = Picture(width, height);
  const rt::Why why = ro::WritePicture(w, &Phys, ro::Guard{}, pic.data(), width, height);
  Expect(why == rt::Why::Ok, (std::string(name) + ": written").c_str());
  std::vector<uint8_t> back;
  rt::ReadLevel0(p, g_mem.data(), back);
  Expect(back == pic, (std::string(name) + ": tile + untile gives the picture back").c_str());
  if (name[0] == '8') {   // the offset map is a bijection into the extent (once per case family)
    std::set<uint32_t> seen;
    bool inside = true;
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t o = rt::OffsetOf(p, x, y);
        seen.insert(o);
        if (uint64_t(o) + p.bpb > p.ExtentBytes()) inside = false;
      }
    Expect(seen.size() == size_t(width) * height && inside, (std::string(name) + ": every texel has its own offset, inside the extent").c_str());
  }
}

// The memory bytes of texel (0, 0) for a picture whose first texel is (R, G, B, A) = (1, 2, 3, 4): independent of the inverse.
static void FirstTexel(bool tiled, uint32_t endian, uint32_t swizzle, const uint8_t want[4], const char* name) {
  uint32_t w[6];
  Forge(w, 32, 32, tiled, endian, swizzle);
  g_mem.assign(32 * 32 * 4 + 64, 0);
  std::vector<uint8_t> pic(32 * 32 * 4, 9);
  pic[0] = 1, pic[1] = 2, pic[2] = 3, pic[3] = 4;
  Expect(ro::WritePicture(w, &Phys, ro::Guard{}, pic.data(), 32, 32) == rt::Why::Ok, (std::string(name) + ": written").c_str());
  Expect(std::memcmp(g_mem.data(), want, 4) == 0, name);
}

int main(int argc, char** argv) {
#ifdef RBC_PLANTS
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") rt::g_plant = std::atoi(argv[i + 1]), wwe2k14::scene_host::scene_rules::g_plant = rt::g_plant;
#else
  (void)argc, (void)argv;
#endif
  // -- the tiled writer: the two sizes of the sites (R1 128x128, R2 1280x720), the game's identity and BGRA swizzles, an endian swap
  RoundTrip(128, 128, true, 0, kSwzIdentity, "8888 128x128 tiled identity");
  RoundTrip(128, 128, true, 0, kSwzBgra, "8888 128x128 tiled BGRA");
  RoundTrip(1280, 720, true, 0, kSwzBgra, "8888 1280x720 tiled BGRA");
  RoundTrip(1280, 720, true, 2, kSwzIdentity, "8888 1280x720 tiled identity, 32-bit endian swap");
  RoundTrip(128, 128, false, 0, kSwzBgra, "linear 128x128 BGRA");
  // -- bytes in memory, asserted independently of the inverse (this is what catches a wrong swap / endian / tiling)
  const uint8_t ident[4] = {1, 2, 3, 4}, bgra[4] = {3, 2, 1, 4}, endian2[4] = {4, 3, 2, 1};
  FirstTexel(false, 0, kSwzIdentity, ident, "texel 0 stored R G B A under the identity fetch");
  FirstTexel(false, 0, kSwzBgra, bgra, "texel 0 stored B G R A under the BGRA fetch (the swap comes from the swizzle)");
  FirstTexel(false, 2, kSwzIdentity, endian2, "texel 0 byte-swapped under endian 2");
  {   // tiled offsets known by hand: (1, 0) is the next 4 bytes, (0, 1) is 16 bytes on (the Xenos micro tile)
    rt::Plan p;
    uint32_t w[6];
    Forge(w, 32, 32, true, 0, kSwzIdentity);
    rt::MakePlan(w, p);
    Expect(rt::OffsetOf(p, 0, 0) == 0 && rt::OffsetOf(p, 1, 0) == 4 && rt::OffsetOf(p, 0, 1) == 16, "tiled offsets: (0,0) 0, (1,0) 4, (0,1) 16");
    uint32_t wl[6];
    Forge(wl, 32, 32, false, 0, kSwzIdentity);
    rt::Plan q;
    rt::MakePlan(wl, q);
    Expect(rt::OffsetOf(q, 1, 0) == 4 && rt::OffsetOf(q, 0, 1) == 128, "linear offsets: (1,0) 4, (0,1) one pitch (128 bytes)");
  }
  // -- refusals are named, nothing is written wrong
  {
    uint32_t w[6];
    Forge(w, 128, 128, true, 0, kSwzIdentity, 18);
    rt::Plan p;
    Expect(rt::MakePlan(w, p) == rt::Why::Format, "a DXT1 destination is refused by name (format)");
    Forge(w, 128, 128, true, 0, kSwzIdentity);
    w[5] = 2u << 9;
    Expect(rt::MakePlan(w, p) == rt::Why::Not2D, "a non-2D destination is refused");
    Forge(w, 128, 128, true, 0, kSwzIdentity);
    Expect(ro::WritePicture(w, &Phys, ro::Guard{}, Picture(128, 128).data(), 128, 128, 1) == rt::Why::Level, "level 1 is refused");
  }
  // -- the presented picture is scaled to the texture's own size (R1: any frame size -> 128x128); a constant picture stays constant, a 2x2 mean is the mean
  {
    std::vector<uint8_t> flat(1280 * 720 * 4, 77), out;
    rt::ScaleBox(flat.data(), 1280, 720, 128, 128, out);
    bool same = out.size() == 128 * 128 * 4;
    for (uint8_t b : out) same = same && b == 77;
    Expect(same, "box scale of a constant picture is that constant");
    const uint8_t src[2 * 2 * 4] = {0, 0, 0, 255, 100, 100, 100, 255, 100, 100, 100, 255, 200, 200, 200, 255};
    rt::ScaleBox(src, 2, 2, 1, 1, out);
    Expect(out.size() == 4 && out[0] == 100 && out[3] == 255, "box scale 2x2 -> 1x1 is the mean (100)");
    uint32_t w[6];
    Forge(w, 128, 128, true, 0, kSwzBgra);
    rt::Plan p;
    rt::MakePlan(w, p);
    g_mem.assign(size_t(p.ExtentBytes()) + 64, 0);
    std::vector<uint8_t> colour(1280 * 720 * 4);
    for (size_t i = 0; i < colour.size(); i += 4) colour[i] = 200, colour[i + 1] = 100, colour[i + 2] = 50, colour[i + 3] = 255;
    Expect(ro::WritePicture(w, &Phys, ro::Guard{}, colour.data(), 1280, 720) == rt::Why::Ok, "a 1280x720 picture is written into the 128x128 texture");
    std::vector<uint8_t> back;
    rt::ReadLevel0(p, g_mem.data(), back);
    bool all = back.size() == 128 * 128 * 4;
    for (size_t i = 0; all && i < back.size(); i += 4) all = back[i] == 200 && back[i + 1] == 100 && back[i + 2] == 50 && back[i + 3] == 255;
    Expect(all, "every texel of the scaled thumbnail is the picture's colour (R 200 G 100 B 50)");
  }
  // -- the site table
  {
    ro::ResetCountersForTest();
    ro::Action a;
    a = ro::Classify(0x8274FADC);
    Expect(a == ro::Action::Serve, "R1 lock return 0x8274FADC is served");
    a = ro::Classify(0x822A635C);
    Expect(a == ro::Action::Serve, "R2 lock return 0x822A635C is served");
    Expect(ro::UnservedTotal() == 0, "served sites do not count as unserved");
    uint64_t n = 0;
    a = ro::Classify(0x8341B75C, &n);
    Expect(a == ro::Action::AtUntile && n == 1 && ro::UnservedTotal() == 0 && ro::g_counters.at_untile.load() == 1, "R3 (the CAW portrait lane's) is served at the untile: counted apart (at_untile), not unserved");
    for (uint32_t lr : {0x83327B20u, 0x83327B78u, 0x83327B90u, 0x8334398Cu}) Expect(ro::Classify(lr) == ro::Action::Unserved, "R5 / R7 are known unserved sites");
    Expect(ro::UnservedTotal() == 4, "four unserved locks counted (R5 x3, R7)");
    Expect(ro::Classify(0x8228F3FC) == ro::Action::Serve && ro::Classify(0x82295A2C) == ro::Action::Serve && ro::UnservedTotal() == 4, "R9 (0x8228F3FC) and R8 (0x82295A2C) are served sites now (RBH): Serve, not unserved");
    Expect(ro::SiteIndex(0x8228F3FC) >= 0 && ro::kSites[ro::SiteIndex(0x8228F3FC)].kind == ro::Kind::ReelPicture && ro::kSites[ro::SiteIndex(0x82295A2C)].kind == ro::Kind::ReelThumb, "R9 is the reel picture, R8 the reel thumbnail");
    Expect(ro::Classify(0x83418024) == ro::Action::Composed && ro::Classify(0x833A1BB0) == ro::Action::Composed, "R4 / R6 are composed on the host: nothing written, not unserved");
    Expect(ro::UnservedTotal() == 4, "composed sites are not unserved");
    n = 0;
    a = ro::Classify(0x12345678, &n);
    Expect(a == ro::Action::Other && n == 1, "an unknown lr is `other`, first sight (an upload / init lock)");
    a = ro::Classify(0x12345678, &n);
    Expect(n == 2 && ro::UnservedTotal() == 4 && ro::g_counters.lock_other.load() == 2, "an unknown lr is lock_other (twice here) and stays OUT of UnservedTotal (still 4)");
    const std::string line = ro::CensusLine();
    Expect(line.find("lock_other 2") != std::string::npos && line.find("12345678=2") != std::string::npos && line.find("readback_unserved{8341B75C") == std::string::npos && line.find("at_untile 1") != std::string::npos && line.find("readback_unserved{83327B20 R5a}=1") != std::string::npos, "the census line names every unserved lr, shows R3 as at_untile, and the busiest lock_other one");
    std::set<uint32_t> lrs;
    for (const auto& s : ro::kSites) lrs.insert(s.lr);
    Expect(lrs.size() == ro::kSiteCount, "the table has no duplicate lr");
    Expect(ro::SiteIndex(0x8274FADC) == 0 && ro::kSites[0].kind == ro::Kind::Thumb && ro::kSites[1].kind == ro::Kind::ArenaShot, "R1 and R2 are the first two rows");
  }
  // -- the site guard (item 5): the site's own size, packed mips, committed / writable pages; a refused write touches nothing
  {
    ro::ResetCountersForTest();
    uint32_t w[6];
    Forge(w, 1280, 720, true, 0, kSwzBgra);
    rt::Plan p;
    rt::MakePlan(w, p);
    g_mem.assign(size_t(p.ExtentBytes()) + 64, 0xCD);
    const std::vector<uint8_t> untouched = g_mem;
    ro::Guard g720;
    g720.want_w = 1280, g720.want_h = 720;
    ro::Guard g480;
    g480.want_w = 640, g480.want_h = 480;
    std::vector<uint8_t> pic(1280 * 720 * 4, 50);
    Expect(ro::WritePicture(w, &Phys, g480, pic.data(), 1280, 720) == rt::Why::SizeMismatch && g_mem == untouched, "R2 guard: a 1280x720 texture where [0x8392DC3C] says 640x480 is refused, memory untouched");
    Expect(ro::WritePicture(w, &Phys, g720, pic.data(), 1280, 720) == rt::Why::Ok && g_mem != untouched, "R2 guard: 1280x720 where 1280x720 is expected is written");
    uint32_t wp[6];
    Forge(wp, 16, 16, true, 0, kSwzBgra);
    wp[5] |= (1u << 11);
    rt::Plan pp;
    rt::MakePlan(wp, pp);
    g_mem.assign(size_t(pp.ExtentBytes()) + 64, 0xCD);
    const std::vector<uint8_t> untouched2 = g_mem;
    Expect(ro::WritePicture(wp, &Phys, ro::Guard{}, pic.data(), 1280, 720) == rt::Why::Packed && g_mem == untouched2, "a 16x16 texture with the packed-mips bit is refused (level 0 sits in the packed tail), memory untouched");
    Forge(w, 1280, 720, true, 0, kSwzBgra);
    g_mem.assign(size_t(p.ExtentBytes()) + 64, 0xCD);
    ro::Guard gw = g720;
    gw.writable = [](uint32_t, uint64_t) { return false; };
    Expect(ro::WritePicture(w, &Phys, gw, pic.data(), 1280, 720) == rt::Why::NotWritable && g_mem == untouched, "pages that are not committed / writable are refused, memory untouched");
    gw.writable = [](uint32_t phys, uint64_t bytes) { return phys == 0x00100000u && bytes >= 1280ull * 720 * 4; };
    Expect(ro::WritePicture(w, &Phys, gw, pic.data(), 1280, 720) == rt::Why::Ok, "the writable check is asked for the whole extent (>= the texture's bytes)");
  }
  // -- F6: the physical heap's own table, per page, every page of the range must be read-write (a read-only or missing page anywhere in it refuses the write)
  {
    const uint32_t base = 0x00100000u;
    const auto only_rw = [&](uint32_t bad_page) { return [bad_page](uint32_t a) { return (a >> 12) != bad_page; }; };
    Expect(ro::PhysWritable(base, 0x4000, only_rw(0xFFFFF)), "PhysWritable: four read-write pages pass");
    Expect(!ro::PhysWritable(base, 0x4000, only_rw((base >> 12) + 3)), "PhysWritable: a read-only page in the LAST page of the range refuses it");
    Expect(!ro::PhysWritable(base + 0xFF0, 0x20, only_rw((base >> 12) + 1)), "PhysWritable: a range that straddles into a bad page refuses it");
    Expect(ro::PhysWritable(base + 0xFF0, 0x10, only_rw((base >> 12) + 1)), "PhysWritable: a range that ends before the bad page passes");
    Expect(!ro::PhysWritable(0x1FFFF000u, 0x2000, only_rw(0xFFFFF)), "PhysWritable: a range that leaves the 512 MB physical heap refuses");
  }
  // -- F1 / F2: the scene read-back rules of the host (src\renderer_host\scene_rules.h)
  {
    namespace sr = wwe2k14::scene_host::scene_rules;
    Expect(sr::Fresh(true, true, true, 100, 100) && sr::Fresh(true, true, true, 102, 100), "F1: a scene drawn this frame, or 2 frames ago, is fresh");
    Expect(!sr::Fresh(true, true, true, 103, 100), "F1: a scene drawn 3 frames ago is STALE (a menu / a replay the game draws itself after a match): no scene");
    Expect(!sr::Fresh(true, true, true, 99, 100), "F1: a scene 'from the future' is not fresh either");
    Expect(!sr::Fresh(false, true, true, 100, 100) && !sr::Fresh(true, false, true, 100, 100) && !sr::Fresh(true, true, false, 100, 100), "F1: out not held / no complete 3D picture / no image: no scene");
    Expect(sr::Decide(false, false) == sr::Step::NoScene && sr::Decide(false, true) == sr::Step::Queue, "F1: a stale `out` answers NoScene, a fresh one is queued");
    Expect(sr::Decide(true, true) == sr::Step::Wait && sr::Decide(true, false) == sr::Step::Wait, "F2: while a screenshot copy is in flight the scene callers WAIT (the shot has priority), fresh or not");
    int r = 0;
    Expect(sr::Retry(r) && sr::Retry(r) && sr::Retry(r) && !sr::Retry(r), "F2: a refused scene copy is retried 3 frames, then the callers are answered");
    Expect(sr::ShotRefusalIsWait(true, true) && !sr::ShotRefusalIsWait(false, true) && !sr::ShotRefusalIsWait(true, false), "F2: a screenshot refused while a scene copy holds the key WAITS; any other refusal is a real one");
  }
  // -- ServeSite (items 2 / 3): a site with no 3D scene writes NOTHING and is counted unserved; a scene is written and counted served
  {
    ro::ResetCountersForTest();
    uint32_t w[6];
    Forge(w, 1280, 720, true, 0, kSwzBgra);
    rt::Plan p;
    rt::MakePlan(w, p);
    g_mem.assign(size_t(p.ExtentBytes()) + 64, 0xCD);
    const std::vector<uint8_t> untouched = g_mem;
    ro::Guard g;
    g.want_w = 1280, g.want_h = 720;
    const rt::Why none = ro::ServeSite(1, w, &Phys, g, [](ro::ScenePic&) { return false; });
    Expect(none == rt::Why::NoScene && g_mem == untouched, "R2 with no 3D scene (a menu): nothing is written, the memory is untouched");
    Expect(ro::g_counters.unserved[1].load() == 1 && ro::g_counters.served[1].load() == 0 && ro::g_counters.why[1][uint32_t(rt::Why::NoScene)].load() == 1, "...and counted readback_unserved{R2} (no 3D scene)");
    std::vector<uint8_t> scene(1280 * 720 * 4, 120);
    bool asked = false;
    const rt::Why ok = ro::ServeSite(1, w, &Phys, g, [&](ro::ScenePic& pic) { asked = true, pic.rgba = scene.data(), pic.w = 1280, pic.h = 720; return true; });
    Expect(ok == rt::Why::Ok && asked && g_mem != untouched && ro::g_counters.served[1].load() == 1, "R2 with a scene: written, counted served");
    uint32_t wbad[6];
    Forge(wbad, 640, 480, true, 0, kSwzBgra);
    asked = false;
    const rt::Why bad = ro::ServeSite(1, wbad, &Phys, g, [&](ro::ScenePic&) { asked = true; return true; });
    Expect(bad == rt::Why::SizeMismatch && !asked, "a refused texture never asks for the picture (no capture is paid for a write that cannot happen)");
  }
  // -- RBH: Create a Highlight reel. The production order: R9's lock (Classify, then the worker's ServeReelPictureCore: drop the old reel, guard, scene copy, write, keep the picture), later R8's lock (Classify, then the render
  //    thread's ServeThumb: crop the kept picture by the game's rect, box-scale, write; never a capture). Textures: R9 = the screen size (1280x720) at 0x00100000, R8 = 128x128 at 0x00800000, both 8888 tiled BGRA.
  {
    constexpr uint32_t kR9 = 0x8228F3FCu, kR8 = 0x82295A2Cu;
    const int i9 = ro::SiteIndex(kR9), i8 = ro::SiteIndex(kR8);
    struct P { static uint8_t* F(uint32_t a) { return Phys2(a); } };
    uint32_t w9[6], w8[6];
    Forge(w9, 1280, 720, true, 0, kSwzBgra, 6, 0x00100000u);
    Forge(w8, 128, 128, true, 0, kSwzBgra, 6, 0x00800000u);
    rt::Plan p9, p8;
    rt::MakePlan(w9, p9);
    rt::MakePlan(w8, p8);
    ro::Guard g9, g8;   // g9 wants no size: R9's picture is the texture's own (F2)
    g8.want_w = 128, g8.want_h = 128;
    const int32_t def[4] = {-1, -1, -1, -1};   // stands for the words at 0x836F2BE0 .. EC
    // a scene: red on the left 160 columns, green in the middle 960, blue on the right 160 (R, G, B, A)
    std::vector<uint8_t> scene(1280 * 720 * 4, 255);
    for (uint32_t y = 0; y < 720; ++y)
      for (uint32_t x = 0; x < 1280; ++x) {
        uint8_t* t = &scene[(size_t(y) * 1280 + x) * 4];
        t[0] = x < 160 ? 255 : 0, t[1] = (x >= 160 && x < 1120) ? 255 : 0, t[2] = x >= 1120 ? 255 : 0, t[3] = 255;
      }
    const auto fresh = [&]() {
      ro::ResetCountersForTest();
      ro::ReelDrop();
      g_mem.assign(size_t(p9.ExtentBytes()) + 64, 0xCD);
      g_mem2.assign(size_t(p8.ExtentBytes()) * 4 + 64, 0xCD);   // (room for the wrong-size twin: a 256x128 texture written by a skipped guard must not run off the buffer)
    };
    const auto have_scene = [&](std::vector<uint8_t>& out) { return out = scene, std::make_pair(1280u, 720u); };
    const auto no_scene = [&](std::vector<uint8_t>&) { return std::make_pair(0u, 0u); };
    const auto thumb_px = [&](std::vector<uint8_t>& back) { rt::ReadLevel0(p8, g_mem2.data(), back); };
    const auto all_px = [&](const std::vector<uint8_t>& px, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
      for (size_t k = 0; k + 3 < px.size(); k += 4)
        if (px[k] != r || px[k + 1] != g || px[k + 2] != b || px[k + 3] != a) return false;
      return px.size() == 128 * 128 * 4;
    };
    // every texel of [x0, x1) x [y0, y1) of a 128x128 picture is (r, g, b, 255)
    const auto region = [&](const std::vector<uint8_t>& px, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t r, uint8_t g, uint8_t b) {
      if (px.size() != 128 * 128 * 4) return false;
      for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x) {
          const uint8_t* t = &px[(size_t(y) * 128 + x) * 4];
          if (t[0] != r || t[1] != g || t[2] != b || t[3] != 255) return false;
        }
      return true;
    };
    std::vector<uint8_t> back;

    // (1) the full flow: R9 serves the scene, R8 crops it. Default rect + [0x8392DC3C] != 0 = the centre 4:3 (the game's own arithmetic: x = 159 for 1280x720).
    fresh();
    Expect(ro::Classify(kR9) == ro::Action::Serve, "RBH: the R9 lock is classified Serve");
    rt::Why why = ro::ServeReelPictureCore(i9, w9, &P::F, g9, have_scene);
    rt::ReadLevel0(p9, g_mem.data(), back);
    Expect(why == rt::Why::Ok && back == scene && ro::g_counters.served[i9].load() == 1, "RBH R9: the scene is written into the 1280x720 texture (read back = the scene), counted served");
    ro::ThumbRect tr;
    std::vector<uint8_t> kept0;
    uint32_t kw0 = 0, kh0 = 0;
    Expect(ro::ThumbCrop(def, def, true, 1280, 720, tr) == rt::Why::Ok && tr.l == 159 && tr.t == 0 && tr.r == 1121 && tr.b == 720, "RBH R8: the default rect with [0x8392DC3C] set is the centre 4:3 of 1280x720: x = trunc((1280 - 720 * 4/3) / 2) = 159 (the game's single-precision arithmetic), (159, 0, 1121, 720)");
    Expect(ro::ThumbCrop(def, def, false, 1280, 720, tr) == rt::Why::Ok && tr.l == 0 && tr.r == 1280 && tr.b == 720, "RBH R8: the default rect without [0x8392DC3C] is the whole picture");
    Expect(ro::Classify(kR8) == ro::Action::Serve, "RBH: the R8 lock is classified Serve");
    why = ro::ServeThumb(i8, w8, &P::F, g8, def, def, true, 1280, 720);
    thumb_px(back);
    // F1: the game LETTERBOXES: a black 128x128 target, the default crop drawn 128x96 at (0, 16) (sub_82295B50 0x82295CC4..CD8)
    Expect(why == rt::Why::Ok && ro::g_counters.served[i8].load() == 1 && region(back, 8, 24, 120, 104, 0, 255, 0), "RBH R8: R9's picture is cropped to the centre 4:3 (all green inside: the side bars are cut), box-scaled and written, counted served");
    Expect(region(back, 0, 0, 128, 16, 0, 0, 0) && region(back, 0, 112, 128, 128, 0, 0, 0), "RBH R8 F1: rows 0-15 and 112-127 are BLACK (the default crop is letterboxed 128x96 at (0, 16), not stretched over 128x128)");
    Expect(ro::ReelCopy(kept0, kw0, kh0) == false, "RBH R8: the reel's picture is dropped after R8 served it (the next reel never crops it)");
    // a rect the game set (not the default): (0, 0, 160, 720) = the red bar, (1120, 0, 1280, 720) = the blue one; drawn 90x128 at (19, 0)
    const int32_t red[4] = {0, 0, 160, 720}, blue[4] = {1120, 0, 1280, 720};
    ro::ReelSet(scene.data(), 1280, 720);
    why = ro::ServeThumb(i8, w8, &P::F, g8, red, def, true, 1280, 720);
    thumb_px(back);
    Expect(why == rt::Why::Ok && region(back, 19, 0, 109, 128, 255, 0, 0), "RBH R8: a rect the game set is applied (left bar = red, the default's 4:3 crop is NOT used), drawn 90x128 at (19, 0)");
    Expect(region(back, 0, 0, 19, 128, 0, 0, 0) && region(back, 109, 0, 128, 128, 0, 0, 0), "RBH R8 F1: a custom rect is letterboxed: columns 0-18 and 109-127 are BLACK");
    ro::ReelSet(scene.data(), 1280, 720);
    why = ro::ServeThumb(i8, w8, &P::F, g8, blue, def, true, 1280, 720);
    thumb_px(back);
    Expect(why == rt::Why::Ok && region(back, 19, 0, 109, 128, 0, 0, 255), "RBH R8: the rect's left / right are the corners (x, x + w): the right bar = blue");
    // (2) R8 with no R9 picture: black, never garbage, never a wait
    fresh();
    why = ro::ServeThumb(i8, w8, &P::F, g8, def, def, true, 1280, 720);
    thumb_px(back);
    Expect(why == rt::Why::NoPicture && all_px(back, 0, 0, 0, 255) && ro::g_counters.zeros[i8].load() == 1 && ro::g_counters.served[i8].load() == 0 && ro::g_counters.unserved[i8].load() == 1,
           "RBH R8: with no R9 picture the texture gets BLACK (not the 0xCD garbage), counted zeros (+ unserved)");
    Expect(ro::g_r8_waits.load() == 0, "RBH R8 NEVER waits for a scene on the render thread (no capture path in ServeThumb; the wait counter stays 0)");
    // (3) R8 refusals that write black: another source size, an unusable rect
    fresh();
    ro::ReelSet(scene.data(), 1280, 720);
    why = ro::ServeThumb(i8, w8, &P::F, g8, def, def, true, 512, 256);
    thumb_px(back);
    Expect(why == rt::Why::SizeMismatch && all_px(back, 0, 0, 0, 255), "RBH R8: a source texture of another size than R9's picture ([obj+44] x [obj+48] = 512 x 256) writes black, counted zeros");
    const int32_t empty[4] = {500, 0, 400, 720};
    why = ro::ServeThumb(i8, w8, &P::F, g8, empty, def, true, 1280, 720);
    Expect(why == rt::Why::Rect && ro::g_counters.zeros[i8].load() == 2, "RBH R8: an empty rect (right < left) writes black, counted zeros");
    // (4) the guard: a texture that is not 128x128 8888 is never touched
    fresh();
    ro::ReelSet(scene.data(), 1280, 720);
    uint32_t wdxt[6], wbig[6];
    Forge(wdxt, 128, 128, true, 0, kSwzBgra, 18, 0x00800000u);
    Forge(wbig, 256, 128, true, 0, kSwzBgra, 6, 0x00800000u);
    const std::vector<uint8_t> before2 = g_mem2;
    why = ro::ServeThumb(i8, wdxt, &P::F, g8, def, def, true, 1280, 720);
    Expect(why == rt::Why::Format && g_mem2 == before2 && ro::g_counters.zeros[i8].load() == 0 && ro::g_counters.unserved[i8].load() == 1, "RBH R8: a DXT1 texture is refused by the format guard, memory untouched, counted refused (not zeros)");
    why = ro::ServeThumb(i8, wbig, &P::F, g8, def, def, true, 1280, 720);
    Expect(why == rt::Why::SizeMismatch && g_mem2 == before2, "RBH R8: a 256x128 texture is refused by the size guard, memory untouched");
    // (5) R9: no scene = BLACK written (the picture goes into the save), the old reel's picture is dropped; the guard refuses a wrong texture without asking for a scene
    fresh();
    ro::ReelSet(scene.data(), 1280, 720);   // an earlier reel's picture
    why = ro::ServeReelPictureCore(i9, w9, &P::F, g9, no_scene);
    rt::ReadLevel0(p9, g_mem.data(), back);
    bool black = back.size() == size_t(1280) * 720 * 4;
    for (size_t k = 0; black && k + 3 < back.size(); k += 4) black = back[k] == 0 && back[k + 1] == 0 && back[k + 2] == 0 && back[k + 3] == 255;
    std::vector<uint8_t> kept;
    uint32_t kw = 0, kh = 0;
    Expect(why == rt::Why::NoScene && black && ro::g_counters.zeros[i9].load() == 1 && ro::g_counters.served[i9].load() == 0, "RBH R9: with no 3D scene the texture gets BLACK (never the 0xCD garbage), counted zeros");
    Expect(!ro::ReelCopy(kept, kw, kh), "RBH R9: the previous reel's kept picture is dropped at the lock (R8 never crops an earlier reel)");
    fresh();
    bool asked = false;
    uint32_t wbad[6], wdxt9[6];
    Forge(wbad, 640, 480, true, 0, kSwzBgra, 6, 0x00100000u);
    Forge(wdxt9, 1280, 720, true, 0, kSwzBgra, 18, 0x00100000u);
    const std::vector<uint8_t> before9 = g_mem;
    // F2: the picture's size is the texture's own (never the live screen size): a 640x480 texture is served, the 1280x720 scene scaled to it, and that 640x480 picture is kept
    why = ro::ServeReelPictureCore(i9, wbad, &P::F, g9, [&](std::vector<uint8_t>& o) { asked = true; return have_scene(o); });
    Expect(why == rt::Why::Ok && asked && g_mem != before9 && ro::ReelCopy(kept, kw, kh) && kw == 640 && kh == 480, "RBH R9 F2: a 640x480 texture is served at ITS OWN size (the 1280x720 scene is scaled to it); the kept picture is 640x480");
    ro::ReelDrop();
    asked = false;
    g_mem = before9;
    why = ro::ServeReelPictureCore(i9, wdxt9, &P::F, g9, [&](std::vector<uint8_t>& o) { asked = true; return have_scene(o); });
    Expect(why == rt::Why::Format && !asked && g_mem == before9, "RBH R9: a DXT1 texture is refused by the format guard (8888 only): no scene asked for, memory untouched");
    uint32_t wpk[6];   // a packed-mip texture (16x16): level 0 sits in the packed tail, refused
    Forge(wpk, 16, 16, true, 0, kSwzBgra, 6, 0x00100000u);
    wpk[5] |= (1u << 11);
    why = ro::ServeReelPictureCore(i9, wpk, &P::F, g9, [&](std::vector<uint8_t>& o) { asked = true; return have_scene(o); });
    Expect(why == rt::Why::Packed && !asked && g_mem == before9, "RBH R9: a packed-mip texture is refused (nothing written, no scene asked for)");
    // (6) R9 with a scene of another size: written at the texture's size, and R8 crops THAT (rect pixels are the texture's)
    fresh();
    std::vector<uint8_t> big(1920 * 1080 * 4, 255);
    for (uint32_t y = 0; y < 1080; ++y)
      for (uint32_t x = 0; x < 1920; ++x) big[(size_t(y) * 1920 + x) * 4] = x < 960 ? 200 : 20;
    why = ro::ServeReelPictureCore(i9, w9, &P::F, g9, [&](std::vector<uint8_t>& o) { return o = big, std::make_pair(1920u, 1080u); });
    Expect(why == rt::Why::Ok && ro::ReelCopy(kept, kw, kh) && kw == 1280 && kh == 720, "RBH R9: a 1920x1080 scene is box-scaled to the texture's 1280x720 and that is the picture kept for R8");
    // (7) the census names the reel sites
    const std::string line = ro::CensusLine();
    Expect(line.find("reel: R8 served 0 zeros 0 refused 0 R9 served 1 zeros 0 refused 0") != std::string::npos, "RBH: the census line carries `reel: R8 served .. zeros .. refused .. R9 ...`");
  }
  // -- the thumbnail scope (P2D): the frame restore is counted inside it, the scope nests and closes
  {
    namespace ts = wwe2k14::thumb_scope;
    Expect(!ts::Active() && ts::kRestoreSite == 0x82BC32B8u, "no thumbnail scope outside the render method; the restore site is 0x82BC32B8");
    {
      ts::Scope a(true);
      ts::Scope off(false);
      Expect(ts::Active(), "the scope is open inside the thumbnail render");
      Expect(ts::NoteRestore(ts::kRestoreSite) == 1 && ts::NoteRestore(ts::kRestoreSite) == 2 && ts::g_first_site.load() == ts::kRestoreSite, "habit_thumb_restore counts, the first site is named");
    }
    Expect(!ts::Active() && ts::g_scopes.load() == 1, "the scope closes with the render (one scope counted; a non-thumbnail object opens none)");
  }
  std::printf(g_bad ? "rbc_unit: FAIL (%d)\n" : "rbc_unit: PASS\n", g_bad);
  return g_bad ? 1 : 0;
}
