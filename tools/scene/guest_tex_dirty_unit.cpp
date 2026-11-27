// S111 DIRTYOWN: the dirty table (src\scene_live\guest_tex_dirty.h: an owner bit per reader, a table that grows instead of refusing) and the belt key store's answer to a refused watch
// (src\scene_live\belt_plate.h Store::ReadKey). Synthetic guest words and pixels only, 0 game runs (docs\research\s111\DIRTYOWN.md).
// entry: NoteFlush
//   guest_tex_dirty_unit                 exit 0 when every check passes
//   guest_tex_dirty_unit --plant NAME    NAME = owner_ignored | no_growth | refusal_silent | gen_kept | header_unseen: exit 1 with the plant's named check FAILing (tools\scene\guest_tex_dirty_check.py runs them all)
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" int stbi_zlib_decode_buffer(char*, int, const char*, int) { return -1; }   // pac.h's decoder hook (belt_plate.h -> jboy.h -> pac.h): this unit reads no pac, never called
#include "../../src/scene_live/belt_plate.h"

using namespace rdr_scene::live;
namespace gd = rdr_scene::live::guest_tex_dirty;

static int g_fail = 0;
static void Check(bool ok, const char* name, const char* what, const std::string& d = std::string()) {
  std::printf("%s  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name, what, d.empty() ? "" : "  : ", d.c_str());
  if (!ok) ++g_fail;
}

// The belt world of tools\scene\belt_plate_unit.cpp, cut down: one model whose key 1 is a 256 x 128 BC3 nameplate (handle 0x1002, wrapper 0x5200, header 0x5300).
struct World {
  std::map<uint32_t, uint32_t> w;
  std::map<uint32_t, std::vector<uint8_t>> px;
  bool Rw(uint32_t a, uint32_t& v) const {
    const auto it = w.find(a);
    if (it == w.end()) return false;
    return v = it->second, true;
  }
  bool Phys(uint32_t a, uint32_t n, uint8_t* d) const {
    a &= 0x1FFFFFFFu;
    for (const auto& [b, v] : px)
      if (a >= b && uint64_t(a) + n <= uint64_t(b) + v.size()) return std::memcpy(d, v.data() + (a - b), n), true;
    return false;
  }
};
constexpr uint32_t kReg = 0x2000, kEntries = 0x3000, kHeader = 0x5300, kBase = 0x18F00000u;
static std::vector<uint8_t> Source(uint8_t seed, size_t bytes = 64 * 32 * 16) {
  std::vector<uint8_t> v(bytes);
  for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(seed + 13 * i + (i >> 5));
  return v;
}
static std::vector<uint8_t> Stored(const std::vector<uint8_t>& src) {
  std::vector<uint8_t> m = src;
  for (size_t i = 0; i < m.size(); i += 16) guest_tex::SwapBlock(m.data() + i, 16, 1);
  return m;
}
struct Belt {
  World wd;
  std::vector<uint8_t> src;
  uint32_t model = 0x4000;
  explicit Belt(uint8_t seed) {
    wd.w[particles::kTextureHandleGlobal] = kReg, wd.w[kReg + 8] = kEntries, wd.w[kReg + 16] = kEntries + 8 * 0x10000u;
    const uint32_t id = 0x1002, wrapper = 0x5200, w = 256, h = 128;
    wd.w[kEntries + (id & 0xFFFF) * 8] = id, wd.w[kEntries + (id & 0xFFFF) * 8 + 4] = wrapper;
    wd.w[wrapper] = particles::kWrapperVtableA, wd.w[wrapper + particles::kWrapperKind] = 0, wd.w[wrapper + particles::kWrapperTexture] = kHeader;
    const uint32_t f[6] = {((w / 32u) << 22), kBase | (1u << 6) | 20u, (w - 1) | ((h - 1) << 13), 0, 0, 1u << 9};   // linear BC3, endian 1 (8-in-16), pitch = w / 32 units
    for (int i = 0; i < 6; ++i) wd.w[kHeader + particles::kHeaderFetch + 4 * i] = f[i];
    const uint32_t keys = model + 0x200;
    wd.w[model + beltplate::kModelKeyCount] = 2, wd.w[model + beltplate::kModelKeys] = keys;
    wd.w[keys] = 0, wd.w[keys + 4] = 0, wd.w[keys + 8] = id, wd.w[keys + 12] = 0;
    Paint(seed);
  }
  void Paint(uint8_t seed) { src = Source(seed), wd.px[kBase] = Stored(src); }   // the game's CPU rewrite in place (no flush: the caller decides)
  void Call(beltplate::Store& st) { st.OnEventCall(model, [&](uint32_t a, uint32_t& v) { return wd.Rw(a, v); }, [&](uint32_t a, uint32_t n, uint8_t* d) { return wd.Phys(a, n, d); }); }
  bool Shows(beltplate::Store& st) const {
    const auto s = st.Get(model);
    const beltplate::Plate* p = s ? s->Find(1) : nullptr;
    return p && p->blocks && *p->blocks == src;
  }
};

int main(int argc, char** argv) {
  std::string plant;
  if (argc > 2 && std::string(argv[1]) == "--plant") plant = argv[2];
  gd::g_dirty_plant = plant == "owner_ignored" ? 1 : plant == "no_growth" ? 2 : plant == "refusal_silent" ? 3 : 0;
  if (plant == "gen_kept") beltplate::g_plate_plant = 17;   // DIRTYOWN review: the key's stored generation is kept across a header change (the old rule)
  if (plant == "header_unseen") beltplate::g_plate_plant = 18;   // S111 KEYFRESH: the cached return is keyed on the handle alone (the old rule)
  if (!plant.empty() && gd::g_dirty_plant == 0 && beltplate::g_plate_plant == 0) return std::printf("unknown plant %s\n", plant.c_str()), 2;

  // 1. Two readers of one header: a flush is seen by both (nothing is consumed), one reader's free leaves the other's watch, a reader that never held it frees nothing, the last one frees the slot.
  {
    gd::Table t;
    const uint32_t H = 0x51000;
    const bool w1 = gd::Watch(H, gd::kOwnerBelt, t), w2 = gd::Watch(H, gd::kOwnerModelTex, t);
    const uint32_t g0 = gd::Gen(H, t);
    gd::NoteFlush(H, t);
    const uint32_t g1 = gd::Gen(H, t);
    Check(w1 && w2 && g0 != 0 && g1 != g0, "own.both_see", "two readers watch one header: a flush moves the one generation both compare their own stored copy with", std::to_string(g0) + " -> " + std::to_string(g1));
    gd::Unwatch(H, gd::kOwnerModelTex, t);
    gd::NoteFlush(H, t);
    const uint32_t g2 = gd::Gen(H, t);
    Check(g2 != 0 && g2 != g1 && gd::Owners(H, t) == gd::kOwnerBelt, "own.other_still_sees", "one reader frees its watch: the other reader still sees the next flush", "gen " + std::to_string(g2) + " owners " + std::to_string(gd::Owners(H, t)));
    gd::Unwatch(H, gd::kOwnerPaint, t);   // a reader that never watched H (a stale holder of a reused header address)
    Check(gd::Owners(H, t) == gd::kOwnerBelt && gd::Gen(H, t) == g2, "own.stale_unwatch", "a reader that does not hold the header frees nothing");
    gd::Unwatch(H, gd::kOwnerBelt, t);
    gd::NoteFlush(H, t);
    Check(gd::Gen(H, t) == 0 && gd::Owners(H, t) == 0, "own.last_frees", "the last reader's Unwatch frees the slot (a flush of it then records nothing)");
    Check(gd::Watch(H, gd::kOwnerPaint, t) && gd::Gen(H, t) != 0, "own.rewatch_nonzero", "a slot watched again starts at a non-zero generation (0 = unwatched never matches a stored read)");
  }
  // 2. A cluster: 40 headers on one home slot are all watched, each flush seen (the old table looked at 8 slots from the home and refused the 9th).
  {
    gd::Table t;
    std::vector<uint32_t> same;
    const uint32_t home = gd::Home(0x60000, gd::kFirstSlots);
    for (uint32_t a = 0x60000; same.size() < 40; a += 16)
      if (gd::Home(a, gd::kFirstSlots) == home) same.push_back(a);
    bool all = true, seen = true;
    for (uint32_t a : same) all = gd::Watch(a, gd::kOwnerModelTex, t) && all;
    for (uint32_t a : same) {
      const uint32_t g = gd::Gen(a, t);
      gd::NoteFlush(a, t);
      seen = seen && g != 0 && gd::Gen(a, t) != g;
    }
    Check(all && seen && t.watch_refused.load() == 0, "probe.cluster", "40 headers with one home slot are all watched and every flush is seen", "refused " + std::to_string(t.watch_refused.load()));
  }
  // 3. The readers' need: car_paint's list alone may hold 2048 entries (carpaint kMaxEntries), the model resolver trims at 256, the belt store holds 64 keys a model: 5000 headers are all watched
  // (the table grows a segment, never refuses) and every flush is seen.
  {
    gd::Table t;
    constexpr uint32_t kN = 5000;
    bool all = true, seen = true;
    for (uint32_t i = 0; i < kN; ++i) all = gd::Watch(0x100000 + 16 * i, gd::kOwnerPaint, t) && all;
    for (uint32_t i = 0; i < kN; ++i) {
      const uint32_t a = 0x100000 + 16 * i, g = gd::Gen(a, t);
      gd::NoteFlush(a, t);
      seen = seen && g != 0 && gd::Gen(a, t) != g;
    }
    Check(all && seen && t.watch_refused.load() == 0 && t.segments.load() >= 2, "grow.need", "5000 watched headers (over the first 1024-slot segment): all watched, every flush seen",
          "segments " + std::to_string(t.segments.load()) + " refused " + std::to_string(t.watch_refused.load()));
  }
  // 4. A table held to its first segment (max_slots) refuses past 3/4 full, and says so (watch_refused, Watch false).
  {
    gd::Table t;
    t.max_slots = gd::kFirstSlots;
    bool all = true;
    for (uint32_t i = 0; i < gd::kFirstSlots * 3 / 4; ++i) all = gd::Watch(0x200000 + 16 * i, gd::kOwnerPaint, t) && all;
    const bool extra = gd::Watch(0x7FFFF0, gd::kOwnerPaint, t);
    Check(all && !extra && t.watch_refused.load() == 1, "full.refused_counted", "a full table answers false and counts the refusal", "refused " + std::to_string(t.watch_refused.load()));
  }
  // 5. The belt store with a second reader on the same header (the editor: the belt key store and the guest-model resolver, or a stale holder of a reused address): the other reader's free leaves the
  // belt's watch, no spurious re-read, and the game's next rewrite + flush is read.
  {
    Belt b(10);
    beltplate::Store st;
    gd::Watch(kHeader, gd::kOwnerModelTex);   // the other reader watched the header first
    st.Want(b.model, 2), b.Call(st);
    const bool first = b.Shows(st);
    gd::Unwatch(kHeader, gd::kOwnerModelTex);
    b.Call(st);
    Check(first && st.Counts().reread == 0 && gd::Owners(kHeader) == gd::kOwnerBelt, "belt.other_unwatch_keeps", "the other reader's Unwatch leaves the belt's watch (no re-read without a flush)",
          "reread " + std::to_string(st.Counts().reread) + " owners " + std::to_string(gd::Owners(kHeader)));
    b.Paint(77);
    gd::NoteFlush(kHeader);
    b.Call(st);
    Check(b.Shows(st) && st.Counts().reread == 1, "belt.flush_seen", "...and the game's next rewrite and flush is read", "reread " + std::to_string(st.Counts().reread));
    st.Clear();
    Check(gd::Owners(kHeader) == 0, "belt.clear_frees", "the store's Clear frees its watch");
  }
  // 6. The belt store when the table refuses the watch: counted (unwatched), and the key is read again at every call, so a rewrite in place is still shown (no flush can be recorded for it).
  {
    gd::Table& t = gd::TheTable();
    t.max_slots = gd::kFirstSlots;
    std::vector<uint32_t> fill;
    for (uint32_t i = 0; t.first.live * 4u < t.first.size * 3u; ++i)
      if (gd::Watch(0x7000000 + 16 * i, gd::kOwnerModelTex)) fill.push_back(0x7000000 + 16 * i);
    Belt b(20);
    beltplate::Store st;
    st.Want(b.model, 2), b.Call(st);
    const bool first = b.Shows(st);
    b.Paint(99);
    b.Call(st);
    Check(first && b.Shows(st) && st.Counts().unwatched >= 1, "belt.refused_rereads", "a refused watch is counted and the key is re-read every call (a rewrite in place is shown)",
          "unwatched " + std::to_string(st.Counts().unwatched) + " shows " + (b.Shows(st) ? "new" : "old"));
    st.Clear();
    for (uint32_t a : fill) gd::Unwatch(a, gd::kOwnerModelTex);
    t.max_slots = gd::kMaxSlots;
  }
  // 7. DIRTYOWN review: the game rebinds the key to a new handle (wrapper 0x5210, header 0x5400) whose pixels cannot be read yet (Memory: the store keeps the old picture and retries). The generation
  // the key stored belongs to the OLD header; when the new header's generation happens to equal it (here: another reader watched it first and it was flushed up to that value) the old rule took the
  // cached return for good and the new picture was never read. The stored generation now goes with its header (0 on a change: a watched header's generation is never 0).
  {
    constexpr uint32_t kH2 = 0x5400, kBase2 = 0x19F00000u, kId2 = 0x1003, kWrapper2 = 0x5210;
    Belt b(40);
    beltplate::Store st;
    st.Want(b.model, 2), b.Call(st);
    const bool first = b.Shows(st);
    gd::Watch(kH2, gd::kOwnerModelTex);   // the other reader holds the new header already
    uint32_t g1 = gd::Gen(kHeader);
    for (int i = 0; i < 256 && gd::Gen(kH2) != g1; ++i) {
      if (gd::Gen(kH2) < g1) gd::NoteFlush(kH2);
      else gd::NoteFlush(kHeader), b.Call(st), g1 = gd::Gen(kHeader);   // (the belt re-reads the same pixels and stores the new generation)
    }
    const bool equal = gd::Gen(kH2) == g1 && g1 != 0;
    for (int i = 0; i < 6; ++i) b.wd.w[kH2 + particles::kHeaderFetch + 4 * i] = b.wd.w[kHeader + particles::kHeaderFetch + 4 * i];
    b.wd.w[kH2 + particles::kHeaderFetch + 4] = kBase2 | (1u << 6) | 20u;   // the new header's pixel page: not readable yet
    b.wd.w[kEntries + (kId2 & 0xFFFF) * 8] = kId2, b.wd.w[kEntries + (kId2 & 0xFFFF) * 8 + 4] = kWrapper2;
    b.wd.w[kWrapper2] = particles::kWrapperVtableA, b.wd.w[kWrapper2 + particles::kWrapperKind] = 0, b.wd.w[kWrapper2 + particles::kWrapperTexture] = kH2;
    b.wd.w[b.model + 0x200 + 8] = kId2;                                         // the game rebinds key 1 to the new handle
    b.Call(st);                                                                 // Memory: the old picture stands, retried
    const uint64_t unreadable = st.Counts().unreadable;
    b.src = Source(41), b.wd.px[kBase2] = Stored(b.src);                        // the new picture's pixels arrive (no flush in between)
    b.Call(st);
    Check(first && equal && unreadable == 1 && b.Shows(st), "belt.header_change_rereads", "a key rebound to a new header whose first read failed reads it at the next call, whatever the new header's generation",
          "gen equal " + std::to_string(equal) + " shows " + (b.Shows(st) ? "new" : "old"));
    st.Clear();
    gd::Unwatch(kH2, gd::kOwnerModelTex);
  }
  // 8. S111 KEYFRESH: the SAME handle with a new header behind its wrapper (the game's draw resolves handle -> wrapper -> header at every draw, sub_82C04DF0 0x82C05700): the new picture is read at the
  // next call although the old header never flushed, the old header's watch goes; then with the header and its generation unchanged nothing is read again.
  {
    constexpr uint32_t kH3 = 0x5500, kBase3 = 0x1AF00000u, kWrapper = 0x5200;
    Belt b(50);
    beltplate::Store st;
    st.Want(b.model, 2), b.Call(st);
    const bool first = b.Shows(st);
    for (int i = 0; i < 6; ++i) b.wd.w[kH3 + particles::kHeaderFetch + 4 * i] = b.wd.w[kHeader + particles::kHeaderFetch + 4 * i];
    b.wd.w[kH3 + particles::kHeaderFetch + 4] = kBase3 | (1u << 6) | 20u;   // the new header's own pixel page
    b.src = Source(51), b.wd.px[kBase3] = Stored(b.src);
    b.wd.w[kWrapper + particles::kWrapperTexture] = kH3;                    // handle 0x1002's wrapper now names the new header (no flush of the old one)
    b.Call(st);
    const bool shows_new = b.Shows(st);
    Check(first && shows_new && !(gd::Owners(kHeader) & gd::kOwnerBelt) && (gd::Owners(kH3) & gd::kOwnerBelt), "belt.same_handle_new_header",
          "the same handle, a new header behind its wrapper: the new picture is read at the next call, the watch moves to the new header", std::string("shows ") + (shows_new ? "new" : "old"));
    const beltplate::Counters c0 = st.Counts();
    b.Call(st);
    const beltplate::Counters c1 = st.Counts();
    Check(c1.read == c0.read && c1.reread == c0.reread && b.Shows(st), "belt.unchanged_no_reread", "the header and its generation unchanged: no read at the next call",
          "reread " + std::to_string(c0.reread) + " -> " + std::to_string(c1.reread));
    st.Clear();
  }
  std::printf("guest_tex_dirty_unit: %d failed%s%s\n", g_fail, plant.empty() ? "" : " (plant ", plant.empty() ? "" : (plant + ")").c_str());
  return g_fail ? 1 : 0;
}
