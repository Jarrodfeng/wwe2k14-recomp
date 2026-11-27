// car_paint_unit: the offline proof of S108 lane CAP (docs\research\s108\CAP_plan.md): a created arena's textures are the GAME's painted pictures (src\scene_live\car_paint.h), not the part files'.
//   car_paint_unit [--game DIR] [--plant NAME]
// The guest memory is forged by THIS file from the source format (never by the reader under test): the texture registry at 0x8392DD9C ({id, wrapper} entries at [mgr+8, mgr+16)), 96-byte wrappers
// (vtable 0x8206ADB8, kind 0 at +52, the header at +80, the 16-byte name at +16), headers with the six fetch dwords at +28 (tiled, pitch, game format 18 / 20, endian 2, base in the 0xE view, the mip
// page with the packed tail, the level count), the blocks tiled + endian-swapped the 360 way (mip 0 at the base, the levels at the mip page), and the stage's list [stage + 14452] (+4 entries, +8 count,
// 8 bytes each: {handle, cached wrapper}). The production order: carpaint::SetPhys (the glue) -> carpaint::OnLatch (the latch's one call) -> guest_tex_dirty::NoteFlush (what the unlock-flush hook
// calls when the game repaints) -> carpaint::Apply (the applier's bind, rf_c_core.cpp) on a model holding the part files' pictures.
// Part A (no game files): synthetic pictures. Part B (--game DIR, the player's own pac\car files read in place): the real created arena CAEN 0 / CASD 0 / CARL 0 + CARG 0 through LoadCustomArena,
// every one of its BC textures forged into the guest: the takes the read needs (the bound), every level read back equal to the file (the mip reader on real files), and the CPU cost of a rebuild.
// A plant is a FAULT in the thing under test; each must make a NAMED check fail (exit 1):
//   file_kept       Apply leaves the part file's picture (the old rule)           -> "bind.guest_pictures"
//   no_reread       a flush of a painted texture is not read again                -> "flush.reread"
//   silent_fail     a texture that cannot be read is not reported                 -> "report.unreadable_named"
//   budget_ignored  the whole list is read in one take past the budget           -> "budget.first_take_bounded"
//   last_dup_wins   the last of two same-named entries is taken (the game: first) -> "bind.dup_first_wins"
//   gen_ignored     a new stage object at a reused address keeps the old one's state -> "stage.same_address_new_stage"   (S108 CAP review)
//   not2d_reported  a cube / volume entry of the list is reported as not taken     -> "report.not2d_quiet"             (S108 CAP review)
//   never_ready_unread    a list never read is never ready (the old rule)          -> "list.null_ready_file_pictures"   (S108 CAP3b)
//   late_paint_ignored    once the set is built, a flush is no longer read again   -> "built.rebuilt_with_painted"      (S108 CAP3)
//   tail_corner     mip 0 of a packed-tail texture read from the tail's corner      -> "tail.mip0_read"                  (S108 CAP2; the shared reader's g_tex_plant 4)
//   header_unseen   a read entry whose handle now names another header is not read again -> "key.same_handle_new_header" (S111 KEYFRESH)
// S108 CAP2: part D (the manager's words decide), part T (tail textures laid out by the SDK's own functions, tools\scene\sdk_tex_layout.cpp), part C (--game: CASD 3's tail textures).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/scene_live/car_paint.h"
#include "sdk_tex_layout.h"   // S108 CAP2: the SDK's own layout (the independent side of the tail-read proof)

using namespace rdr_scene::live;
namespace gt = rdr_scene::live::guest_tex;

static int g_fail = 0, g_checks = 0;
static std::string g_plant;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// ---- the fake guest -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Guest {
  std::unordered_map<uint32_t, uint8_t> v;   // virtual memory (big-endian words as the 360 stores them)
  uint32_t px_base = 0x10000000u;
  std::vector<uint8_t> px;                   // physical pixel memory from px_base
  uint32_t hole_lo = 0, hole_hi = 0;         // an unreadable physical range (a texture whose pages are not there)
  uint64_t phys_calls = 0, phys_over = 0;
  void W32(uint32_t a, uint32_t x) { for (int i = 0; i < 4; ++i) v[a + i] = uint8_t(x >> (24 - 8 * i)); }
  void Bytes(uint32_t a, const void* s, size_t n) { for (size_t i = 0; i < n; ++i) v[a + uint32_t(i)] = static_cast<const uint8_t*>(s)[i]; }
  uint8_t* At(uint32_t a, uint32_t n) {
    if (uint64_t(a - px_base) + n > px.size()) px.resize(size_t(a - px_base) + n, 0);
    return px.data() + (a - px_base);
  }
};
static Guest g;
static bool Rd(uint32_t a, void* d, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const auto it = g.v.find(a + uint32_t(i));
    if (it == g.v.end()) return false;
    static_cast<uint8_t*>(d)[i] = it->second;
  }
  return true;
}
static bool Phys(uint32_t a, uint32_t n, uint8_t* d) {   // the shared reader's contract: <= 1 MB a call, a range check
  ++g.phys_calls;
  if (!n || n > (1u << 20)) return ++g.phys_over, false;
  if (a < g.px_base || uint64_t(a - g.px_base) + n > g.px.size()) return false;
  if (g.hole_hi > g.hole_lo && a < g.hole_hi && a + n > g.hole_lo) return false;
  std::memcpy(d, g.px.data() + (a - g.px_base), n);
  return true;
}

// ---- the forge: this file's own copy of the 360 layout rules (as prt_unit.cpp) ---------------------------------------------------------------------------------------------------------------------
static uint32_t L2c(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) ++r; return r; }
static uint32_t Al(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
static void PutBlock(uint32_t region, uint32_t off, const uint8_t* blk, uint32_t bpb) {
  uint8_t b[16];
  std::memcpy(b, blk, bpb);
  for (uint32_t i = 0; i + 4 <= bpb; i += 4) std::swap(b[i], b[i + 3]), std::swap(b[i + 1], b[i + 2]);   // endian 2 (8-in-32)
  std::memcpy(g.At(region + off, bpb), b, bpb);
}
static uint32_t g_px_next = 0x10000000u, g_v_next = 0x40000000u;
constexpr uint32_t kReg = 0x30000000u, kEntries = 0x30001000u, kStage = 0x31000000u, kList = 0x31100000u, kListData = 0x31101000u;
struct Forged { uint32_t id = 0, wrapper = 0, header = 0, base = 0, mip_base = 0; };
// The registry entry, the 96-byte wrapper (vtable, kind 0, header, name) and the header's six fetch dwords of one texture object.
static void WriteObject(Forged& f, uint32_t index, const std::string& name, const uint32_t fd[6]) {
  f.id = 0x00050000u | index;   // a generation in the high half, the index in the low (the registry's rule: stored id == handle)
  f.wrapper = g_v_next, f.header = g_v_next + 0x100;
  g_v_next += 0x200;
  g.W32(kEntries + index * 8, f.id), g.W32(kEntries + index * 8 + 4, f.wrapper);
  g.W32(f.wrapper, particles::kWrapperVtableA), g.W32(f.wrapper + particles::kWrapperKind, 0), g.W32(f.wrapper + particles::kWrapperTexture, f.header);
  char nm[16] = {};
  std::memcpy(nm, name.data(), std::min<size_t>(16, name.size()));
  g.Bytes(f.wrapper + 16, nm, 16);
  for (int i = 0; i < 6; ++i) g.W32(f.header + particles::kHeaderFetch + 4 * i, fd[i]);
}
// S108 CAP2: the CAA manager [0x836F2C04] and the words the latch reads (docs\research\s108\CAP2_plan.md): +40 / +41 bytes (the branch's sub-state, the category), +48 (the loop runs),
// +72 (the load machine), +76 (the edit machine), +156 byte (categories copied). mgr = 0: no manager.
static void Mgr(uint32_t mgr, uint32_t s72, uint32_t w48, uint32_t c41, uint32_t n156 = 0, uint32_t e76 = 8) {
  g.W32(carpaint::kPaintMgrGlobal, mgr);
  if (!mgr) return;
  g.W32(mgr + 40, (4u << 24) | ((c41 & 255u) << 16));
  g.W32(mgr + 48, w48), g.W32(mgr + 72, s72), g.W32(mgr + 76, e76), g.W32(mgr + 156, (n156 & 255u) << 24);
}
// One texture with its chain: mip 0 tiled at a new base, levels 1.. at a new mip page (non-base pitch from the next power of two, 32-block rows, 4 KB slices), levels >= the packed level in one tail.
// `mips_named` = the level count the header names (fewer than the data's = the game binds fewer levels). Rewriting at the same addresses = `again`.
static bool Forge(const assets::Texture& t, uint32_t index, const std::string& name, Forged& f, bool again = false, uint32_t mips_named = 0) {
  if (t.format != assets::TexFormat::kBC1 && t.format != assets::TexFormat::kBC3 && t.format != assets::TexFormat::kBC2) return false;
  const uint32_t bpb = t.format == assets::TexFormat::kBC1 ? 8 : 16, l2 = bpb == 8 ? 3u : 4u;
  const uint32_t fmt = t.format == assets::TexFormat::kBC1 ? 18 : t.format == assets::TexFormat::kBC2 ? 19 : 20;
  const uint32_t log2min = L2c(std::min(t.width, t.height));
  if (log2min <= 4 || t.faces != 1 || (t.width & 3) || (t.height & 3)) return false;   // mip 0 in the tail / a cube: not this reader's case
  const uint32_t packed_level = log2min - 4, pitch_div32 = (t.width + 31) / 32, pitch0 = Al(Al(pitch_div32 << 5, 4) / 4, 32);
  if (!again) {
    f.base = g_px_next;
    g_px_next += Al(Al((t.width + 3) / 4, 32) * Al((t.height + 3) / 4, 32) * bpb, 4096) + 4096;
    f.mip_base = g_px_next;
  }
  uint32_t level_off[16] = {}, acc = 0;
  for (uint32_t lv = 1; lv < t.mips && lv <= packed_level; ++lv) {
    level_off[lv] = acc;
    const uint32_t rw = std::max((1u << L2c(t.width)) >> lv, 1u), rh = std::max((1u << L2c(t.height)) >> lv, 1u);
    acc += Al(Al(Al(rw, 4) / 4, 32) * bpb * Al(Al(rh, 4) / 4, 32), 4096);
  }
  if (!again) g_px_next += acc + 8192;
  size_t at = 0;
  for (uint32_t lv = 0; lv < t.mips; ++lv) {
    const uint32_t lw = std::max(t.width >> lv, 1u), lh = std::max(t.height >> lv, 1u), bx = (lw + 3) / 4, by = (lh + 3) / 4;
    uint32_t region = f.base, pitch = pitch0, ox = 0, oy = 0;
    if (lv) {
      const uint32_t stored = std::min(lv, packed_level);
      region = f.mip_base + level_off[stored];
      pitch = Al(Al(std::max((1u << L2c(t.width)) >> stored, 1u), 4) / 4, 32);
      if (lv >= packed_level) {
        const uint32_t lw2 = L2c(t.width), lh2 = L2c(t.height), pk = lv - packed_level;
        if (pk < 3) (lw2 > lh2 ? oy : ox) = 16u >> pk;
        else if (lw2 > lh2) ox = (1u << (lw2 - packed_level)) >> (pk - 2);
        else oy = (1u << (lh2 - packed_level)) >> (pk - 2);
        ox /= 4, oy /= 4;
      }
    }
    for (uint32_t y = 0; y < by; ++y)
      for (uint32_t x = 0; x < bx; ++x) PutBlock(region, gt::TiledOffset2D(int32_t(ox + x), int32_t(oy + y), pitch, l2), t.data.data() + at + (size_t(y) * bx + x) * bpb, bpb);
    at += size_t(bx) * by * bpb;
  }
  if (again) return true;
  const uint32_t named = mips_named ? mips_named : t.mips;
  const uint32_t fd[6] = {0x80000000u | (pitch_div32 << 22), fmt | (2u << 6) | (wwe2k14::guest_view::ToEViewCpu(f.base) & 0xFFFFF000u), (t.width - 1) | ((t.height - 1) << 13), 0,
                          (named - 1) << 6, named > 1 ? ((wwe2k14::guest_view::ToEViewCpu(f.mip_base) & 0xFFFFF000u) | (1u << 9) | (1u << 11)) : (1u << 9)};
  WriteObject(f, index, name, fd);
  return true;
}
// S108 CAP2: a texture laid out by the SDK's OWN functions (tools\scene\sdk_tex_layout.cpp: GetGuestTextureLayout / GetPackedMipOffset / GetTiledOffset2D), packed-mips bit set, so a texture
// whose smaller side is <= 16 texels has its mip 0 inside the base's packed tail. Nothing here uses the reader's arithmetic; the bytes go in as the SDK placed them.
static bool ForgeSdk(const assets::Texture& t, uint32_t index, const std::string& name, bool tiled, Forged& f, sdk_layout::Out* info = nullptr) {
  if (t.faces != 1 || (t.format != assets::TexFormat::kBC1 && t.format != assets::TexFormat::kBC2 && t.format != assets::TexFormat::kBC3)) return false;
  sdk_layout::In in;
  in.width = t.width, in.height = t.height, in.mips = t.mips, in.tiled = tiled, in.packed = true, in.data = t.data.data(), in.size = t.data.size();
  in.gpu_format = t.format == assets::TexFormat::kBC1 ? 18 : t.format == assets::TexFormat::kBC2 ? 19 : 20;
  sdk_layout::Out o;
  if (!sdk_layout::Lay(in, o)) return false;
  f.base = g_px_next;
  g_px_next += Al(uint32_t(o.base.size()), 4096) + 4096;
  std::memcpy(g.At(f.base, uint32_t(o.base.size())), o.base.data(), o.base.size());
  f.mip_base = 0;
  if (!o.mips.empty()) {
    f.mip_base = g_px_next;
    g_px_next += Al(uint32_t(o.mips.size()), 4096) + 4096;
    std::memcpy(g.At(f.mip_base, uint32_t(o.mips.size())), o.mips.data(), o.mips.size());
  }
  const uint32_t fd[6] = {(tiled ? 0x80000000u : 0u) | (o.pitch_div32 << 22), in.gpu_format | (2u << 6) | (wwe2k14::guest_view::ToEViewCpu(f.base) & 0xFFFFF000u),
                          (t.width - 1) | ((t.height - 1) << 13), 0, (t.mips - 1) << 6,
                          (f.mip_base ? (wwe2k14::guest_view::ToEViewCpu(f.mip_base) & 0xFFFFF000u) : 0u) | (1u << 9) | (1u << 11)};
  WriteObject(f, index, name, fd);
  if (info) *info = std::move(o);
  return true;
}
// The registry and the stage's list over `ids` (in list order).
static void WriteList(const std::vector<uint32_t>& ids, uint32_t stage = kStage, uint32_t list = kList, uint32_t data = kListData) {
  g.W32(particles::kTextureHandleGlobal, kReg);
  g.W32(kReg + 8, kEntries), g.W32(kReg + 16, kEntries + 8 * 4096);
  g.W32(stage + carpaint::kStageTexList, list);
  g.W32(list + carpaint::kListData, data), g.W32(list + carpaint::kListCount, uint32_t(ids.size()));
  for (size_t i = 0; i < ids.size(); ++i) g.W32(data + uint32_t(i) * 8, ids[i]), g.W32(data + uint32_t(i) * 8 + 4, 0);
}

static assets::Texture Tex(const std::string& name, assets::TexFormat f, uint32_t w, uint32_t h, uint32_t mips, uint32_t seed) {
  assets::Texture t;
  t.name = name, t.format = f, t.width = w, t.height = h, t.mips = mips;
  t.data.resize(carpaint::ChainBytes(w, h, mips, f == assets::TexFormat::kBC1 ? 8 : 16));
  uint32_t x = seed * 2654435761u + 1;
  for (uint8_t& b : t.data) x = x * 1664525u + 1013904223u, b = uint8_t(x >> 24);
  return t;
}
static bool SameTex(const assets::Texture& a, const assets::Texture& b) { return a.format == b.format && a.width == b.width && a.height == b.height && a.mips == b.mips && a.data == b.data; }
static const assets::Texture* Named(const assets::Model& m, const std::string& n) {
  for (const assets::Texture& t : m.textures) if (t.name == n) return &t;
  return nullptr;
}
static std::string Fmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

int main(int argc, char** argv) {
  std::string game;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--plant" && i + 1 < argc) g_plant = argv[++i];
  }
  const std::map<std::string, int> plants = {{"file_kept", 1},      {"no_reread", 2},   {"silent_fail", 3},    {"budget_ignored", 4},       {"last_dup_wins", 5},
                                             {"gen_ignored", 6},    {"not2d_reported", 7}, {"late_paint_ignored", 8}, {"never_ready_unread", 9}, {"header_unseen", 10}, {"tail_corner", 100}};
  if (!g_plant.empty()) {
    if (!plants.count(g_plant)) return std::printf("unknown plant %s\n", g_plant.c_str()), 2;
    if (g_plant == "tail_corner") gt::g_tex_plant = 4;   // the shared reader's plant: mip 0 of a tail texture read from the tail's corner (its block offset ignored)
    else carpaint::g_paint_plant = plants.at(g_plant);
  }
  carpaint::SetPhys(&Phys);   // the glue's one line (stage_fx_glue.h)

  // ---------------------------------------------------------------- A. synthetic: the painted panels, an unpainted texture, a duplicate, an unreadable one, two big ones
  {
    // the GAME's pictures (what it painted) and the FILE's (the placeholders)
    const assets::Texture g_toronB = Tex("caa_m_toronB", assets::TexFormat::kBC3, 512, 256, 10, 1), g_toronA = Tex("caa_m_toronA", assets::TexFormat::kBC1, 512, 256, 10, 2);
    const assets::Texture g_truss = Tex("truss_00", assets::TexFormat::kBC3, 256, 256, 9, 3);   // unpainted: the game holds the file's bytes
    const assets::Texture g_dup2 = Tex("caa_m_toronB", assets::TexFormat::kBC3, 512, 256, 10, 9);   // a second entry of the same name later in the list
    const assets::Texture g_light = Tex("light_00", assets::TexFormat::kBC1, 256, 256, 9, 4);    // its pages are not readable
    const assets::Texture g_short = Tex("slo_window", assets::TexFormat::kBC1, 512, 256, 10, 5); // the game's header names ONE level
    const assets::Texture g_big0 = Tex("big_0", assets::TexFormat::kBC3, 2048, 1024, 12, 6), g_big1 = Tex("big_1", assets::TexFormat::kBC3, 2048, 1024, 12, 7);   // 2 MB mip 0: two takes, sliced reads
    std::vector<Forged> f(8);
    bool fo = Forge(g_toronB, 1, "caa_m_toronB", f[0]) && Forge(g_toronA, 2, "caa_m_toronA", f[1]) && Forge(g_truss, 3, "truss_00", f[2]) && Forge(g_light, 4, "light_00", f[3]) &&
              Forge(g_short, 5, "slo_window", f[4], false, 1) && Forge(g_big0, 6, "big_0", f[5]) && Forge(g_big1, 7, "big_1", f[6]) && Forge(g_dup2, 8, "caa_m_toronB", f[7]);
    Check(fo, "forge.all");
    g.hole_lo = f[3].base, g.hole_hi = f[3].base + 4096;   // light_00's mip 0 page is unreadable
    WriteList({f[0].id, f[1].id, f[2].id, f[3].id, f[4].id, f[5].id, f[6].id, f[7].id});
    // the FILE model: placeholders for the painted names, the same bytes for the unpainted one, one name the list does not hold
    assets::Model file;
    file.textures.push_back(Tex("caa_m_toronB", assets::TexFormat::kBC3, 512, 256, 10, 100));
    file.textures.push_back(Tex("caa_m_toronA", assets::TexFormat::kBC1, 512, 256, 10, 101));
    file.textures.push_back(g_truss);
    file.textures.push_back(Tex("light_00", assets::TexFormat::kBC1, 256, 256, 9, 102));
    file.textures.push_back(g_short);
    file.textures.push_back(Tex("big_0", assets::TexFormat::kBC3, 2048, 1024, 12, 103));
    file.textures.push_back(Tex("big_1", assets::TexFormat::kBC3, 2048, 1024, 12, 104));
    file.textures.push_back(Tex("ar_curtain", assets::TexFormat::kBC1, 256, 256, 9, 105));
    file.textures[0].name = "CAA_M_TORONB";   // the game compares lowercased
    const assets::Model file_kept_copy = file;

    Mgr(0, 0, 0, 0);   // no paint manager: nothing paints, ready as soon as every entry was tried (part D tests the manager's words)
    carpaint::OnLatch(kStage, &Rd);   // take 1
    auto s1 = carpaint::TheStore().For(kStage);
    Check(s1 && s1->list_read && !s1->complete && !s1->ready, "budget.first_take_bounded",
          s1 ? Fmt("list_read %d complete %d ready %d (the 4 MB budget stops before the second 2.7 MB picture)", int(s1->list_read), int(s1->complete), int(s1->ready)) : "no snapshot");
    carpaint::OnLatch(kStage, &Rd);   // take 2
    auto s2 = carpaint::TheStore().For(kStage);
    Check(s2 && s2->complete && s2->ready, "budget.completes", s2 ? Fmt("takes %u", s2->takes) : "no snapshot");
    Check(g.phys_over == 0 && g.phys_calls > 0, "phys.sliced_1mb", Fmt("calls %llu over-1MB %llu", (unsigned long long)g.phys_calls, (unsigned long long)g.phys_over));
    if (!s2) return std::printf("RESULT FAIL\n"), 1;

    assets::Model m = file;
    carpaint::ApplyResult ar;
    carpaint::Apply(m, *s2, ar);
    const assets::Texture* tb = Named(m, "CAA_M_TORONB");
    const assets::Texture* ta = Named(m, "caa_m_toronA");
    Check(tb && SameTex(*tb, [&] { assets::Texture t = g_toronB; t.name = tb->name; return t; }()) && ta && ta->data == g_toronA.data && Named(m, "big_1")->data == g_big1.data,
          "bind.guest_pictures", Fmt("bound %u", ar.bound));
    Check(tb && tb->data != g_dup2.data && ar.dup == 1, "bind.dup_first_wins", Fmt("dup %u", ar.dup));
    Check(Named(m, "ar_curtain")->data == file.textures[7].data && ar.file_only == 1, "bind.file_only_kept", Fmt("file_only %u", ar.file_only));
    std::string names;
    for (const auto& ms : ar.missing) names += ms.name + " (" + ms.why + ") ";
    Check(ar.missing.size() == 1 && ar.missing[0].name == "light_00", "report.unreadable_named", names.empty() ? "nothing reported" : names);
    Check(Named(m, "light_00")->data == file.textures[3].data, "report.file_picture_for_missing");
    Check(ar.same == 1 && ar.shorter == 1 && ar.differs == 4, "check.equal_short_differ", Fmt("equal %u short %u differ %u", ar.same, ar.shorter, ar.differs));
    Check(ar.differ_mip0 == 4 && ar.differ_levels == 0 && ar.differ_shape == 0 && ar.short_guest_min == 1 && ar.short_guest_max == 1 && ar.short_file_min == 10 && ar.short_file_max == 10,
          "check.split_short_levels", Fmt("differ mip0 %u levels %u shape %u; short game %u..%u file %u..%u", ar.differ_mip0, ar.differ_levels, ar.differ_shape, ar.short_guest_min, ar.short_guest_max,
                                          ar.short_file_min, ar.short_file_max));
    const std::string census = carpaint::CensusLine(*s2, ar, carpaint::TheStore().Counts());
    std::printf("info: %s\n", census.c_str());
    Check(census.find("e.g. slo_window 1/10 fetch4 00000000 fetch5 00000200") != std::string::npos, "census.short_example", "the first short texture with its level-count fetch dwords");
    const assets::Texture* sw = Named(m, "slo_window");
    Check(sw && sw->mips == 1 && sw->data.size() == carpaint::ChainBytes(512, 256, 1, 8), "bind.levels_as_the_game_names", sw ? Fmt("mips %u", sw->mips) : "");

    // the game repaints caa_m_toronB in place and flushes it (sub_822A1DD8 -> sub_82CA6BE0 -> the hook's NoteFlush)
    const uint64_t h_before = s2->hash;
    carpaint::OnLatch(kStage, &Rd);   // no flush: nothing is read again
    const carpaint::Counters c0 = carpaint::TheStore().Counts();
    Check(carpaint::TheStore().For(kStage)->hash == h_before && c0.reread == 0, "flush.none_no_reread", Fmt("reread %llu", (unsigned long long)c0.reread));
    const assets::Texture repaint = Tex("caa_m_toronB", assets::TexFormat::kBC3, 512, 256, 10, 50);
    Forge(repaint, 1, "caa_m_toronB", f[0], true);
    guest_tex_dirty::NoteFlush(f[0].header);
    carpaint::OnLatch(kStage, &Rd);
    auto s3 = carpaint::TheStore().For(kStage);
    assets::Model m3 = file;
    carpaint::ApplyResult ar3;
    carpaint::Apply(m3, *s3, ar3);
    Check(s3->hash != h_before && Named(m3, "CAA_M_TORONB")->data == repaint.data, "flush.reread", Fmt("reread %llu", (unsigned long long)carpaint::TheStore().Counts().reread));
    Check(file.textures[0].data == file_kept_copy.textures[0].data, "bind.file_model_untouched");

    // S108 CAP3b, in production order. (a) the stage's list pointer is null for some takes (the game has not built the list yet): the set is READY with no pictures (the applier draws the part files'
    // pictures and reports once), then the list appears: the normal rule takes over (every entry tried -> the painted set, the identity moves). (b) a list that is never readable: ready, the file's
    // pictures, the reason named, and the rule never changes.
    const uint32_t stage2 = 0x32000000u, list2 = 0x32100000u, data2 = 0x32101000u;
    g.W32(stage2 + carpaint::kStageTexList, 0);
    for (uint32_t i = 0; i < 3; ++i) carpaint::OnLatch(stage2, &Rd);
    const auto sn = carpaint::TheStore().For(stage2);
    assets::Model mn = file;
    carpaint::ApplyResult arn;
    if (sn) carpaint::Apply(mn, *sn, arn);
    Check(sn && sn->ready && !sn->list_read && sn->list_why == 1 && sn->pics.empty() && arn.bound == 0 && mn.textures[0].data == file.textures[0].data, "list.null_ready_file_pictures",
          sn ? Fmt("ready %d list_read %d why %u bound %u", int(sn->ready), int(sn->list_read), sn->list_why, arn.bound) : "no snapshot");
    const uint64_t h_null = sn ? sn->hash : 0;
    WriteList({f[0].id}, stage2, list2, data2);   // the game builds the list: the painted entry
    carpaint::OnLatch(stage2, &Rd);
    const auto sl = carpaint::TheStore().For(stage2);
    assets::Model ml = file;
    carpaint::ApplyResult arl;
    if (sl) carpaint::Apply(ml, *sl, arl);
    Check(sl && sl->ready && sl->list_read && sl->list_why == 0 && sl->complete && sl->hash != h_null && arl.bound == 1 && ml.textures[0].data == repaint.data, "list.appears_painted_set_follows",
          sl ? Fmt("ready %d complete %d identity moved %d bound %u", int(sl->ready), int(sl->complete), int(sl->hash != h_null), arl.bound) : "no snapshot");
    Check(carpaint::TheStore().For(kStage) != nullptr, "stage.previous_kept", "the record lags the latch: the old stage's snapshot stays for it");
    carpaint::TheStore().Clear();
    const uint32_t stage4 = 0x34000000u;
    g.W32(stage4 + carpaint::kStageTexList, 0x3F000000u);   // a list pointer into unreadable memory
    for (uint32_t i = 0; i < 40; ++i) carpaint::OnLatch(stage4, &Rd);
    const auto su = carpaint::TheStore().For(stage4);
    uint64_t hu = su ? su->hash : 0;
    carpaint::OnLatch(stage4, &Rd);
    Check(su && su->ready && !su->list_read && su->list_why == 2 && su->list == 0x3F000000u && su->pics.empty() && carpaint::TheStore().For(stage4)->hash == hu, "list.unreadable_ready_file_pictures",
          su ? Fmt("ready %d why %u list %08X; the set never changes", int(su->ready), su->list_why, su->list) : "no snapshot");
    carpaint::TheStore().Clear();
    // a READ list needs every entry tried: the budget take of this part stays not ready
    Check(s1 && s1->list_read && !s1->ready, "ready.needs_every_entry_tried", "a list that was read is ready only when every entry was tried once");
    carpaint::TheStore().Clear();
  }

  // ---------------------------------------------------------------- A2 (S108 CAP review). The game frees a created arena's stage and builds the next one at the SAME address (the contributor's stage
  // generation moves: OnStageFreed and SetStage each bump it). The new stage's list also holds a cube entry (CAEN 401 cube_00: sub_82BA9D68 r6 = 1 -> wrapper kind 1), which is not a 2D picture.
  {
    using TF = assets::TexFormat;
    const assets::Texture old_pic = Tex("caa_headderB", TF::kBC3, 512, 256, 10, 60);
    const assets::Texture n_a = Tex("rin_mat00", TF::kBC3, 2048, 1024, 12, 61), n_b = Tex("ar_dm", TF::kBC3, 2048, 1024, 12, 62), n_c = Tex("caa_floor", TF::kBC1, 512, 512, 10, 63);
    const assets::Texture cube = Tex("cube_00", TF::kBC1, 128, 128, 1, 64);
    const assets::Texture step = Tex("slo_step", TF::kBC1, 128, 128, 8, 65);   // unpainted: the file differs from the guest only in its last level (what a mip-reader fault looks like)
    Forged f1, fa, fb, fc, fq, fs;
    const bool fo = Forge(old_pic, 20, "caa_headderB", f1) && Forge(n_a, 21, "rin_mat00", fa) && Forge(n_b, 22, "ar_dm", fb) && Forge(n_c, 23, "caa_floor", fc) && Forge(cube, 24, "cube_00", fq) &&
                    Forge(step, 25, "slo_step", fs);
    g.W32(fq.wrapper + particles::kWrapperKind, 1);   // the cube's wrapper: kind 1 (header at +84 in the game; WalkTextureHandle refuses it before any header read)
    WriteList({f1.id});
    carpaint::OnLatch(kStage, &Rd, 1);   // the first stage at kStage (generation 1)
    const auto a = carpaint::TheStore().For(kStage);
    WriteList({fa.id, fb.id, fq.id, fc.id, fs.id});   // freed; the next stage at the same address, its own list (generation 3)
    carpaint::OnLatch(kStage, &Rd, 3);       // take 1 of the new stage: the 4 MB budget stops after the two 2.7 MB pictures
    const auto b = carpaint::TheStore().For(kStage);
    bool stale = false;
    if (b)
      for (const carpaint::Pic& p : b->pics) stale = stale || p.handle == f1.id;
    Check(fo && a && a->ready && b && !b->ready && !stale, "stage.same_address_new_stage",
          b ? Fmt("old ready %d; new: ready %d complete %d entries %zu old picture kept %d", int(a && a->ready), int(b->ready), int(b->complete), b->pics.size(), int(stale)) : "no snapshot");
    for (int i = 0; i < 4; ++i) carpaint::OnLatch(kStage, &Rd, 3);
    const auto c = carpaint::TheStore().For(kStage);
    assets::Model m;
    m.textures.push_back(Tex("rin_mat00", TF::kBC3, 2048, 1024, 12, 161));
    m.textures.push_back(Tex("ar_dm", TF::kBC3, 2048, 1024, 12, 162));
    m.textures.push_back(Tex("caa_floor", TF::kBC1, 512, 512, 10, 163));
    m.textures.push_back(step);
    m.textures.back().data.back() ^= 0x5A;   // the file's last level differs, mip 0 equal
    carpaint::ApplyResult ar;
    if (c) carpaint::Apply(m, *c, ar);
    std::string names;
    for (const auto& ms : ar.missing) names += ms.name + " (" + ms.why + ") ";
    const carpaint::Counters cn = carpaint::TheStore().Counts();
    Check(c && c->complete && ar.missing.empty() && ar.bound == 4 && cn.not2d == 1, "report.not2d_quiet",
          Fmt("complete %d bound %u not2d %llu missing: %s", int(c && c->complete), ar.bound, (unsigned long long)cn.not2d, names.empty() ? "none" : names.c_str()));
    Check(ar.differ_levels == 1 && ar.differ_mip0 == 3 && ar.differ_shape == 0, "check.differ_levels_only",
          Fmt("differ mip0 %u levels_only %u shape %u", ar.differ_mip0, ar.differ_levels, ar.differ_shape));
    // CAP2 review (run 17: levels_only 3, shape 4, unnamed): the census names each levels_only texture with its first differing level (the mip-reader question needs the names)
    const std::string census_l = c ? carpaint::CensusLine(*c, ar, cn) : std::string();
    std::printf("info: %s\n", census_l.c_str());
    Check(census_l.find("levels_only: slo_step@7") != std::string::npos, "census.levels_names", "the levels_only texture named with its first differing level");
    carpaint::TheStore().Clear();
  }

  // ---------------------------------------------------------------- D (S108 CAP2). The GAME's paint manager decides when the set is built: its words at 0x836F2C04, read fresh at every take.
  {
    using TF = assets::TexFormat;
    const auto For = [](uint32_t st) { return carpaint::TheStore().For(st); };
    const assets::Texture pa = Tex("caa_m_toronB", TF::kBC3, 256, 128, 9, 70), pb = Tex("caa_headderA", TF::kBC1, 256, 128, 9, 71);
    Forged fa, fb;
    const bool fo = Forge(pa, 30, "caa_m_toronB", fa) && Forge(pb, 31, "caa_headderA", fb);
    const uint32_t stage3 = 0x33000000u, list3 = 0x33100000u, data3 = 0x33101000u, mgr = 0x35000000u;
    WriteList({fa.id, fb.id}, stage3, list3, data3);
    // The case the run hit (CAP3), in production order: the stage is seen while the manager is NULL (the game has not created it yet); the set is built at the first complete take (ready = every entry
    // tried, nothing else); then the manager appears and paints (entries change in place, the unlock flush marks them): the set is REBUILT with the painted pictures (its identity moves: the
    // applier's debounced rebuild keys on it), and the census words show the build came before the paint.
    Mgr(0, 0, 0, 0);
    carpaint::OnLatch(stage3, &Rd, 7);
    const auto sb = For(stage3);
    const uint64_t h_built = sb ? sb->hash : 0;
    Check(fo && sb && sb->complete && sb->ready && sb->built_take == 1 && !sb->built_game.mgr && !sb->seen_busy, "built.first_complete_take_manager_null",
          sb ? Fmt("ready %d at take %u, manager %08X", int(sb->ready), sb->built_take, sb->built_game.mgr) : "no snapshot");
    assets::Model m0;
    m0.textures.push_back(Tex("caa_m_toronB", TF::kBC3, 256, 128, 9, 170));
    m0.textures.push_back(Tex("caa_headderA", TF::kBC1, 256, 128, 9, 171));
    carpaint::ApplyResult ar0;
    if (sb) carpaint::Apply(m0, *sb, ar0);
    Check(sb && std::string(carpaint::GameText(*sb)).find("built_take 1 built_game none game: mgr 00000000") != std::string::npos, "census.built_before_the_game", "built_take 1, built_game none");
    Mgr(mgr, 3, 1, 5, 5);   // the game creates and starts its manager: the load machine at 3, the category loop running
    carpaint::OnLatch(stage3, &Rd, 7);
    Check(For(stage3)->ready && For(stage3)->seen_busy && For(stage3)->hash == h_built && For(stage3)->built_take == 1, "built.manager_appears_keeps_set",
          "the manager's words gate nothing: the built set stays, built_take stays 1, seen_busy is evidence");
    const assets::Texture paint_a = Tex("caa_m_toronB", TF::kBC3, 256, 128, 9, 72), paint_b = Tex("caa_headderA", TF::kBC1, 256, 128, 9, 73);   // the game paints (colour, pattern, logo) in place
    Forge(paint_a, 30, "caa_m_toronB", fa, true), Forge(paint_b, 31, "caa_headderA", fb, true);
    guest_tex_dirty::NoteFlush(fa.header), guest_tex_dirty::NoteFlush(fb.header);   // sub_82CA6BE0, the unlock flush
    Mgr(mgr, 4, 0, 255, 28);   // the load's end
    carpaint::OnLatch(stage3, &Rd, 7);
    const auto sd = For(stage3);
    assets::Model m;
    m.textures.push_back(Tex("caa_m_toronB", TF::kBC3, 256, 128, 9, 170));
    m.textures.push_back(Tex("caa_headderA", TF::kBC1, 256, 128, 9, 171));
    carpaint::ApplyResult ar;
    if (sd) carpaint::Apply(m, *sd, ar);
    Check(sd && sd->hash != h_built && ar.bound == 2 && m.textures[0].data == paint_a.data && m.textures[1].data == paint_b.data && m.textures[0].data != m0.textures[0].data, "built.rebuilt_with_painted",
          sd ? Fmt("the set identity moved %d, painted pictures bound %u (reread %llu)", int(sd->hash != h_built), ar.bound, (unsigned long long)carpaint::TheStore().Counts().reread) : "no snapshot");
    const std::string census = sd ? carpaint::CensusLine(*sd, ar, carpaint::TheStore().Counts()) : std::string();
    std::printf("info: %s\n", census.c_str());
    Check(census.find("ready 1 built_take 1 built_game none game: mgr 35000000 readable 1 seen_busy 1 load72 4 loop48 0 cat41 255 copied156 28 edit76 8") != std::string::npos, "census.game_words",
          "the census names when the set was built (take, the manager at that take) and the manager's words now");
    carpaint::TheStore().Clear();

    // the manager exists at the first complete take, mid paint: built there too (it gates nothing), built_game carries the words
    Mgr(mgr, 3, 1, 5, 5);
    carpaint::OnLatch(stage3, &Rd, 8);
    const auto sm = For(stage3);
    Check(sm && sm->ready && sm->built_take == 1 && sm->built_game.mgr == mgr && sm->built_game.load == 3 && sm->built_game.loop == 1 && sm->built_game.category == 5 &&
              carpaint::GameText(*sm).find("built_game load72 3 loop48 1 cat41 5") != std::string::npos,
          "built.during_paint_named", "the set is built mid paint, the census says so");
    carpaint::TheStore().Clear();

    g.W32(carpaint::kPaintMgrGlobal, 0x3F000000u);   // a manager pointer whose words do not read: nothing gates, the words are reported unreadable
    carpaint::OnLatch(stage3, &Rd, 10);
    Check(For(stage3) && For(stage3)->ready && !For(stage3)->game.readable, "built.unreadable_manager_still_ready");
    carpaint::TheStore().Clear();
    Mgr(0, 0, 0, 0);
  }

  // ---------------------------------------------------------------- S111 KEYFRESH: the same handle, a new header behind its wrapper (the game resolves handle -> wrapper -> header at every draw,
  // sub_82C04DF0 0x82C05700): the next take reads the new header's picture although the old header never flushed; then, nothing changed, nothing is read again.
  {
    using TF = assets::TexFormat;
    const assets::Texture ka = Tex("caa_m_keyA", TF::kBC1, 256, 128, 9, 90), kb = Tex("caa_m_keyB", TF::kBC1, 256, 128, 9, 91);
    Forged fa, fb;
    const bool fo = Forge(ka, 90, "caa_m_keyA", fa) && Forge(kb, 91, "caa_m_keyB", fb);
    const uint32_t stage6 = 0x37000000u;
    WriteList({fa.id, fb.id}, stage6, 0x37100000u, 0x37101000u);
    carpaint::OnLatch(stage6, &Rd, 11);
    const auto k0 = carpaint::TheStore().For(stage6);
    const bool two = k0 && k0->pics.size() == 2 && k0->pics[0].ok && k0->pics[1].ok && k0->pics[0].hash != k0->pics[1].hash;
    g.W32(fa.wrapper + particles::kWrapperTexture, fb.header);   // the game puts entry B's header behind entry A's wrapper (no flush)
    carpaint::OnLatch(stage6, &Rd, 11);
    const auto k1 = carpaint::TheStore().For(stage6);
    const bool moved = k1 && k1->pics.size() == 2 && k1->pics[0].header == fb.header && k1->pics[0].hash == k1->pics[1].hash;
    Check(fo && two && moved, "key.same_handle_new_header", Fmt("two %d moved %d (reread %llu)", int(two), int(moved), (unsigned long long)carpaint::TheStore().Counts().reread));
    const uint64_t rr = carpaint::TheStore().Counts().reread;
    carpaint::OnLatch(stage6, &Rd, 11);
    Check(carpaint::TheStore().Counts().reread == rr, "key.unchanged_no_reread", Fmt("reread %llu -> %llu", (unsigned long long)rr, (unsigned long long)carpaint::TheStore().Counts().reread));
    carpaint::TheStore().Clear();
  }

  // ---------------------------------------------------------------- T (S108 CAP2). Mip 0 inside the packed mip tail (smaller side <= 16 texels), laid out by the SDK's OWN functions
  // (tools\scene\sdk_tex_layout.cpp), read back through the production path (OnLatch -> Apply): mip 0 and every level must equal the source.
  {
    using TF = assets::TexFormat;
    struct TT { const char* name; TF f; uint32_t w, h, mips; bool tiled; };
    const TT tt[] = {{"tail_16", TF::kBC1, 16, 16, 5, true},     {"tail_32x16", TF::kBC1, 32, 16, 6, true}, {"tail_16x64", TF::kBC3, 16, 64, 7, true},
                     {"tail_8", TF::kBC3, 8, 8, 4, true},        {"tail_lin16", TF::kBC1, 16, 16, 5, false}, {"tail_lin64x16", TF::kBC3, 64, 16, 7, false},
                     {"tail_one", TF::kBC1, 16, 16, 1, true}};
    std::vector<assets::Texture> src;
    std::vector<uint32_t> ids;
    bool fo = true;
    std::string offs;
    for (size_t i = 0; i < std::size(tt); ++i) {
      src.push_back(Tex(tt[i].name, tt[i].f, tt[i].w, tt[i].h, tt[i].mips, uint32_t(80 + i)));
      Forged ft;
      sdk_layout::Out info;
      fo = ForgeSdk(src.back(), uint32_t(40 + i), tt[i].name, tt[i].tiled, ft, &info) && fo;
      ids.push_back(ft.id);
      offs += Fmt("%s mip0 at block (%u,%u) packed level %u; ", tt[i].name, info.mip0_x, info.mip0_y, info.packed_level);
    }
    const uint32_t stage5 = 0x36000000u;
    WriteList(ids, stage5, 0x36100000u, 0x36101000u);
    carpaint::OnLatch(stage5, &Rd, 12);
    auto st = carpaint::TheStore().For(stage5);
    assets::Model m;
    for (size_t i = 0; i < src.size(); ++i) m.textures.push_back(Tex(tt[i].name, tt[i].f, tt[i].w, tt[i].h, tt[i].mips, uint32_t(180 + i)));   // the file's placeholders
    carpaint::ApplyResult ar;
    if (st) carpaint::Apply(m, *st, ar);
    bool mip0 = st && st->ready && ar.bound == src.size(), levels = mip0;
    std::string bad;
    for (size_t i = 0; i < src.size(); ++i) {
      const assets::Texture& b = m.textures[i];
      const size_t m0 = carpaint::ChainBytes(src[i].width, src[i].height, 1, src[i].format == TF::kBC1 ? 8 : 16);
      const bool ok0 = b.data.size() >= m0 && std::memcmp(b.data.data(), src[i].data.data(), m0) == 0;
      const bool okl = ok0 && b.mips == src[i].mips && b.data == src[i].data;
      mip0 = mip0 && ok0, levels = levels && okl;
      if (!okl) bad += Fmt("%s (mip0 %s, mips %u/%u) ", tt[i].name, ok0 ? "equal" : "DIFFERS", b.mips, src[i].mips);
    }
    for (const auto& ms : ar.missing) bad += ms.name + " not taken (" + ms.why + ") ";
    Check(fo && mip0, "tail.mip0_read", Fmt("bound %u of %zu; %s%s", ar.bound, src.size(), offs.c_str(), bad.c_str()));
    Check(levels, "tail.levels_read", bad.empty() ? "every level of every tail texture equals the source" : bad);
    carpaint::TheStore().Clear();
  }

  // ---------------------------------------------------------------- B. the player's own files: the real created arena, every texture forged, read back, timed
  if (!game.empty()) {
    launcher::GameFiles files;
    files.SetRoot(game);
    assets::car::Sel sel;
    sel.valid = true, sel.arena = 200, sel.caen = 0, sel.casd = 0, sel.carl = 0, sel.bg = 5;
    assets::Model real;
    std::string err;
    assets::car::Clear();
    const bool ok = assets::LoadCustomArena(files, 200, sel, real, err);
    Check(ok, "real.loads_created_arena", ok ? real.label : err);
    if (ok) {
      g = Guest();
      g_px_next = 0x10000000u, g_v_next = 0x40000000u;
      Mgr(0, 0, 0, 0);
      std::vector<uint32_t> ids;
      size_t bytes = 0, skipped = 0;
      for (size_t i = 0; i < real.textures.size(); ++i) {
        const assets::Texture& t = real.textures[i];
        Forged fr;
        if (t.live || !Forge(t, uint32_t(ids.size() + 1), t.name, fr)) { ++skipped; continue; }
        ids.push_back(fr.id), bytes += t.data.size();
      }
      WriteList(ids);
      const auto t0 = std::chrono::steady_clock::now();
      uint32_t takes = 0;
      std::shared_ptr<const carpaint::Snapshot> s;
      while (takes < 10) {
        carpaint::OnLatch(kStage, &Rd), ++takes;
        s = carpaint::TheStore().For(kStage);
        if (s && s->complete) break;
      }
      const double read_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      Check(s && s->complete && takes <= 3, "real.read_bound",
            Fmt("%zu textures (%zu not 2D BC / tail-only, kept from the file), %.2f MB, complete in %u takes, %.1f ms in all", ids.size(), skipped, bytes / 1048576.0, takes, read_ms));
      if (s) {
        const auto t1 = std::chrono::steady_clock::now();
        assets::Model copy = real;   // the rebuild's copy of the kept pristine (rf_c_core.cpp)
        const auto t2 = std::chrono::steady_clock::now();
        carpaint::ApplyResult ar;
        carpaint::Apply(copy, *s, ar);
        const auto t3 = std::chrono::steady_clock::now();
        size_t tex_bytes = 0;
        for (const assets::Texture& t : copy.textures) tex_bytes += t.data.size();
        Check(ar.bound == ids.size() && ar.missing.empty(), "real.all_bound", Fmt("bound %u missing %zu file_only %u dup %u", ar.bound, ar.missing.size(), ar.file_only, ar.dup));
        Check(ar.same == ar.bound && ar.differs == 0 && ar.shorter == 0, "real.every_level_equal", Fmt("equal %u differ %u short %u (the guest chain read back against the file, every level)", ar.same, ar.differs, ar.shorter));
        std::printf("TIMING rebuild CPU part: copy of the pristine %.1f ms, Apply %.1f ms; textures in the model %.2f MB (each family model that uses a texture uploads its own copy: front_end AddModel has no sharing)\n",
                    std::chrono::duration<double, std::milli>(t2 - t1).count(), std::chrono::duration<double, std::milli>(t3 - t2).count(), tex_bytes / 1048576.0);
      }
    }
  }
  // ---------------------------------------------------------------- C (S108 CAP2). The player's CASD 3: every BC texture whose smaller side is <= 16 texels (banner01 16x16, paint category 15;
  // mb_jeep1 32x16 among them), decoded by the FILE loader, laid out by the SDK's functions, read back through OnLatch -> Apply: every level equal to the file.
  if (!game.empty()) {
    carpaint::TheStore().Clear();
    launcher::GameFiles files;
    files.SetRoot(game);
    assets::car::Sel sel;
    sel.valid = true, sel.arena = 200, sel.caen = 0, sel.casd = 3, sel.carl = 0, sel.bg = 5;
    assets::Model real;
    std::string err;
    assets::car::Clear();
    const bool ok = assets::LoadCustomArena(files, 200, sel, real, err);
    Check(ok, "real.loads_casd3", ok ? real.label : err);
    if (ok) {
      g = Guest();
      g_px_next = 0x10000000u, g_v_next = 0x40000000u;
      Mgr(0, 0, 0, 0);
      std::vector<uint32_t> ids;
      std::vector<size_t> which;
      std::string names;
      bool banner = false, jeep = false;
      // The stage's list holds every texture of CASD 3's directory 400 (sub_82764200), drawn or not; the merged model holds only what a material names. banner01 (paint category 15) is in the
      // directory but no material of this selection names it: it is decoded from the directory itself by the file loader (TexSet), as the model's textures are.
      {
        assets::pac::Pac stud;
        std::string perr;
        uint32_t used = 0;
        bool fell = false;
        const assets::pac::Pach* ph = assets::pac::Open(files, assets::car::kPacStudium, stud, perr) ? assets::car::PartPach(stud, 3, used, fell) : nullptr;
        const assets::pac::Ent* e = ph && !fell ? assets::pac::Find(*ph, 400) : nullptr;
        assets::TexSet dir;
        if (e && e->off <= stud.bytes.size() && e->size <= stud.bytes.size() - e->off && dir.AddDir(assets::pac::Decode(stud.bytes.data() + e->off, e->size)))
          for (const char* want : {"banner01", "mb_jeep1"}) {
            bool have = false;
            for (const assets::Texture& t : real.textures) have = have || carpaint::Lower(t.name) == want;
            if (!have) dir.Get(real, want);   // appended to the model as the loader does (the file's decode)
          }
        else
          std::printf("info: CASD 3 directory 400 not read (%s)\n", perr.c_str());
      }
      for (size_t i = 0; i < real.textures.size(); ++i) {
        const assets::Texture& t = real.textures[i];
        if (t.live || t.faces != 1 || std::min(t.width, t.height) > 16) continue;
        Forged fr;
        if (!ForgeSdk(t, uint32_t(ids.size() + 1), t.name, true, fr)) continue;
        ids.push_back(fr.id), which.push_back(i);
        const std::string ln = carpaint::Lower(t.name);
        banner = banner || ln == "banner01", jeep = jeep || ln == "mb_jeep1";
        if (ids.size() <= 12) names += Fmt("%s %ux%u fmt %d mips %u; ", t.name.c_str(), t.width, t.height, int(t.format), t.mips);
      }
      WriteList(ids);
      std::shared_ptr<const carpaint::Snapshot> s;
      for (int i = 0; i < 4 && !(s && s->ready); ++i) carpaint::OnLatch(kStage, &Rd, 20), s = carpaint::TheStore().For(kStage);
      assets::Model copy = real;
      carpaint::ApplyResult ar;
      if (s) carpaint::Apply(copy, *s, ar);
      bool equal = s && s->ready && ar.bound == ids.size() && ar.missing.empty() && !ids.empty();
      std::string bad;
      for (size_t i : which)
        if (copy.textures[i].data != real.textures[i].data || copy.textures[i].mips != real.textures[i].mips) equal = false, bad += real.textures[i].name + " ";
      for (const auto& ms : ar.missing) bad += ms.name + " not taken (" + ms.why + ") ";
      Check(equal && banner && jeep, "real.tail_textures_equal",
            Fmt("%zu tail textures (banner01 %d, mb_jeep1 %d), bound %u, equal %u: %s%s", ids.size(), int(banner), int(jeep), ar.bound, ar.same, names.c_str(), bad.c_str()));
    }
  }
  std::printf("RESULT %s (%d checks, %d failed)\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
