// S105 lane PLATE: the championship belt's nameplate is the game's own registered texture (docs\research\s105\PLATE.md). Synthetic wrapper / registry / pixels only (no game files). Pure compile, 0 game runs.
// entry: RawHookEntry
// (enters through weaponrows::RawHookEntry, the pure body of the raw hook weapon_rows_glue.h RawEntry; RawEntry itself needs the game build, so the unit cannot name it as `entry:`)
//   belt_plate_unit                       exit 0 when every check passes
//   belt_plate_unit --plant NAME          NAME = name_dropped | wrong_t | silent_white | shared_pool | make_no_drop | free_no_drop: exit 0 only when the checks SEE the plant (same convention as texreg_unit)
// Production order driven: RawHookEntry (the weapon rows' raw hook on an event prop, OURS mode, which calls OnEventCall) -> Get / PoolSuffix (the drawer's pool key) -> BindPlate (the drawer's bind), plus a flush (guest_tex_dirty::NoteFlush) and a Drop.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" int stbi_zlib_decode_buffer(char*, int, const char*, int) { return -1; }   // pac.h's decoder hook (belt_plate.h -> jboy.h -> pac.h): this unit reads no pac, never called
#include "../../src/scene_live/belt_plate.h"
#include "../../src/scene_live/weapon_rows_core.h"   // RawHookEntry: the production entry (the weapon rows' raw hook runs it first, before the OURS test)

using namespace rdr_scene::live;

static int g_fail = 0;
static void Check(bool ok, const char* what, const std::string& d = std::string()) {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, d.empty() ? "" : "  : ", d.c_str());
  if (!ok) ++g_fail;
}

struct World {
  std::map<uint32_t, uint32_t> w;           // guest words (host values of the big-endian words)
  std::map<uint32_t, std::vector<uint8_t>> px;   // physical memory by base
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

constexpr uint32_t kReg = 0x2000, kEntries = 0x3000;
static void Registry(World& wd) { wd.w[particles::kTextureHandleGlobal] = kReg, wd.w[kReg + 8] = kEntries, wd.w[kReg + 16] = kEntries + 8 * 0x10000u; }

// A registered 2D texture: the registry entry {id, wrapper}, the wrapper (vtable A, kind 0, name at +16, header at +80), the header's six fetch dwords.
static void PutTexture(World& wd, uint32_t id, uint32_t wrapper, const char* name, uint32_t header, uint32_t base, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stored_id = 0) {
  wd.w[kEntries + (id & 0xFFFF) * 8] = stored_id ? stored_id : id, wd.w[kEntries + (id & 0xFFFF) * 8 + 4] = wrapper;
  wd.w[wrapper] = particles::kWrapperVtableA, wd.w[wrapper + particles::kWrapperKind] = 0, wd.w[wrapper + particles::kWrapperTexture] = header;
  char n[16] = {};
  for (int i = 0; i < 15 && name[i]; ++i) n[i] = name[i];
  for (uint32_t i = 0; i < 4; ++i) wd.w[wrapper + beltplate::kWrapperName + 4 * i] = uint32_t(uint8_t(n[4 * i])) << 24 | uint32_t(uint8_t(n[4 * i + 1])) << 16 | uint32_t(uint8_t(n[4 * i + 2])) << 8 | uint8_t(n[4 * i + 3]);
  const uint32_t f[6] = {((w / 32u) << 22), base | (1u << 6) | fmt, (w - 1) | ((h - 1) << 13), 0, 0, 1u << 9};   // linear (no tile bit), endian 1 (8-in-16), pitch = w / 32 units
  for (int i = 0; i < 6; ++i) wd.w[header + particles::kHeaderFetch + 4 * i] = f[i];
}
// The plate's bytes as the game keeps them: 64 x 32 BC3 blocks (16 B), each block 8-in-16 swapped, row pitch 1024 B.
static std::vector<uint8_t> Source(uint8_t seed) {
  std::vector<uint8_t> v(64 * 32 * 16);
  for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(seed + 13 * i + (i >> 5));
  return v;
}
static std::vector<uint8_t> Stored(const std::vector<uint8_t>& src) {
  std::vector<uint8_t> m = src;
  for (size_t i = 0; i < m.size(); i += 16) guest_tex::SwapBlock(m.data() + i, 16, 1);
  return m;
}
static void PutModel(World& wd, uint32_t model, const std::vector<uint32_t>& handles) {
  const uint32_t keys = model + 0x200;
  wd.w[model + beltplate::kModelKeyCount] = uint32_t(handles.size()), wd.w[model + beltplate::kModelKeys] = keys;
  for (size_t i = 0; i < handles.size(); ++i) wd.w[keys + 8 * uint32_t(i)] = handles[i], wd.w[keys + 8 * uint32_t(i) + 4] = 0;
}

// One belt world: keys [color t0, nameplate t1, color2 t2, stale t3].
struct Belt {
  World wd;
  std::vector<uint8_t> src;
  uint32_t model = 0x4000;
  explicit Belt(uint8_t seed, const char* plate_name = "nameplate", uint32_t base = 0x18F00000u) {
    Registry(wd);
    src = Source(seed);
    wd.px[base] = Stored(src);
    wd.px[0x18E00000u] = std::vector<uint8_t>(32 * 32 / 2, 0x5A);
    PutTexture(wd, 0x1001, 0x5000, "color", 0x5100, 0x18E00000u, 18, 32, 32);
    PutTexture(wd, 0x1002, 0x5200, plate_name, 0x5300, base, 20, 256, 128);
    PutTexture(wd, 0x1003, 0x5400, "color2", 0x5500, 0x18E00000u, 18, 32, 32);
    PutTexture(wd, 0x1004, 0x5600, "stale", 0x5700, 0x18E00000u, 18, 32, 32, /*stored_id*/ 0x2004);   // the registry entry's stored id differs: a stale handle
    PutModel(wd, model, {0x1001, 0x1002, 0x1003, 0x1004});
  }
  void Call(beltplate::Store& st) { st.OnEventCall(model, [&](uint32_t a, uint32_t& v) { return wd.Rw(a, v); }, [&](uint32_t a, uint32_t n, uint8_t* d) { return wd.Phys(a, n, d); }); }
};

static const World* g_world = nullptr;   // the hook's pixel reader is a plain function pointer
static bool WorldPhys(uint32_t a, uint32_t n, uint8_t* d) { return g_world && g_world->Phys(a, n, d); }

// The production entry on a belt model: the model's words as the game's CSkinModel, then one raw-hook call at `frame` (OURS = the stream-free build).
static void ModelWords(World& wd, uint32_t model) {
  const float one = 1.0f;
  wd.w[model] = 0x8206AF90, wd.w[0x8206AF90 + 8] = 0x82BD3AB8, wd.w[model + 296] = 1, wd.w[model + 300] = 1;
  std::memcpy(&wd.w[model + 252], &one, 4);
  for (uint32_t i = 0; i < 16; ++i) std::memcpy(&wd.w[0x7000 + 4 * i], &one, 4);
}
static weaponrows::CallResult Hook(World& wd, weaponrows::Tracker& tr, uint32_t model, uint64_t frame) {
  g_world = &wd;
  weaponrows::Slot13Scope own(model);
  weaponrows::CallResult out;
  auto rw = [&](uint32_t a, uint32_t& v) { return wd.Rw(a, v); };
  auto rm = [&](uint32_t a, uint32_t n, std::vector<rdr_scene::Mat4>& o) {
    o.resize(n);
    for (uint32_t i = 0; i < n * 16; ++i) {
      uint32_t v = 0;
      if (!wd.Rw(a + 4 * i, v)) return false;
      std::memcpy(&reinterpret_cast<float*>(o.data())[i], &v, 4);
    }
    return true;
  };
  weaponrows::RawHookEntry(tr, true, /*ours*/ true, model, 0x7000, frame, rw, rm, &out, 1.0f, 0, nullptr, nullptr, 0, &WorldPhys);
  g_world = nullptr;
  return out;
}

int main(int argc, char** argv) {
  std::string plant;
  if (argc > 2 && std::string(argv[1]) == "--plant") plant = argv[2];
  beltplate::g_plate_plant = plant == "name_dropped" ? 1 : plant == "wrong_t" ? 2 : plant == "silent_white" ? 3 : plant == "shared_pool" ? 4 : plant == "make_no_drop" ? 5 : plant == "free_no_drop" ? 6 : plant == "share_unset" ? 7 : 0;

  // 1. the plate resolves: its key is found by the wrapper's name, t is its index, shape and bytes are the game's.
  {
    Belt b(10);
    beltplate::Store st;
    b.Call(st);
    const auto p = st.Get(b.model);
    Check(p && p->blocks && p->t == 1 && p->width == 256 && p->height == 128, "a fixture wrapper named `nameplate` resolves: t = its key index (1), BC3 256 x 128", p ? "t " + std::to_string(p->t) + " " + std::to_string(p->width) + "x" + std::to_string(p->height) : "none");
    Check(p && p->blocks && *p->blocks == b.src, "its 32 KB are the game's bytes (linear, 8-in-16 swapped back)", p && p->blocks ? std::to_string(p->blocks->size()) + " bytes" : "none");
    guest_tex::Resolved r;
    uint32_t t = 9;
    auto rw = [&](uint32_t a, uint32_t& v) { return b.wd.Rw(a, v); };
    const bool found = beltplate::FindPlate(rw, b.model, r, t) == beltplate::Find::Found;   // (called first: the detail string below reads r after it)
    Check(found && r.wrapper == 0x5200, "FindPlate names the plate's wrapper", "wrapper " + std::to_string(r.wrapper));
  }
  // 2. the name test is case-insensitive (the game lowercases both sides) and exact.
  {
    Belt up(10, "NamePlate");
    beltplate::Store st;
    up.Call(st);
    Check(st.Get(up.model) != nullptr, "`NamePlate` (upper case in the registry) still matches");
    Belt other(10, "nameplate2");
    beltplate::Store st2;
    other.Call(st2);
    Check(st2.Get(other.model) == nullptr, "`nameplate2` is not the plate (the name is exact)");
  }
  // 3. a model with no `nameplate` key has no plate (never the first key's picture) and the search is decided once.
  {
    Belt b(10);
    PutModel(b.wd, b.model, {0x1001, 0x1003});
    beltplate::Store st;
    b.Call(st);
    b.Call(st);
    Check(st.Get(b.model) == nullptr && st.Counts().no_plate == 1, "a model without a `nameplate` key has no plate, counted once", "no_plate " + std::to_string(st.Counts().no_plate));
  }
  // 4. two holders' belts never share a pool: the plate hash is in the pool key.
  {
    Belt a(10);
    const std::vector<uint8_t> src2 = Source(77);
    // second holder: its own handle / wrapper / header so both models resolve in one world
    World wd = a.wd;
    wd.px[0x18F40000u] = Stored(src2);
    PutTexture(wd, 0x1012, 0x6200, "nameplate", 0x6300, 0x18F40000u, 20, 256, 128);
    PutModel(wd, 0x4800, {0x1001, 0x1012});
    beltplate::Store st;
    auto rw = [&](uint32_t x, uint32_t& v) { return wd.Rw(x, v); };
    auto ph = [&](uint32_t x, uint32_t n, uint8_t* d) { return wd.Phys(x, n, d); };
    st.OnEventCall(0x4000, rw, ph), st.OnEventCall(0x4800, rw, ph);
    const std::string sa = beltplate::PoolSuffix(st.Get(0x4000)), sb = beltplate::PoolSuffix(st.Get(0x4800));
    Check(!sa.empty() && !sb.empty() && sa != sb, "two holders (two plates) give two pool keys", sa + " / " + sb);
    Check(st.Get(0x4800) && st.Get(0x4800)->t == 1 && *st.Get(0x4800)->blocks == src2, "the second holder's plate is its own bytes");
  }
  // 5. the bind: the material whose diffuse sampler is `nameplate` gets the plate as a BC3 texture; others are untouched; a missing plate is counted.
  {
    Belt b(10);
    beltplate::Store st;
    b.Call(st);
    assets::Model m;
    assets::Material plate_mat, other;
    plate_mat.diffuse_name = "nameplate";
    other.diffuse_name = "color", other.diffuse = 0;
    m.materials = {plate_mat, other};
    assets::Texture existing;
    existing.name = "color";
    m.textures.push_back(existing);
    const uint32_t n = beltplate::BindPlate(m, st.Get(b.model), st);
    const bool bound = n == 1 && m.materials[0].diffuse == 1 && m.textures.size() == 2 && m.textures[1].format == assets::TexFormat::kBC3 && m.textures[1].width == 256 && m.textures[1].height == 128 && m.textures[1].data == b.src && m.materials[1].diffuse == 0;
    Check(bound, "the `nameplate` material binds the plate (BC3 256 x 128, the game's bytes); the other material is untouched", "bound " + std::to_string(n));
    assets::Model m2;
    m2.materials = {plate_mat};
    beltplate::Store st2;
    const uint32_t n2 = beltplate::BindPlate(m2, nullptr, st2);
    Check(n2 == 0 && m2.materials[0].diffuse < 0 && st2.Counts().missing == 1, "a belt with no plate stays unbound AND is counted (never silent white)", "missing " + std::to_string(st2.Counts().missing));
    // S110 BELTPLATE: the bound material carries the loader's alpha fact for the late texture: an all-zero BC3 plate (the 5009 canvas) = 1.0, a fully opaque plate = 0.
    {
      Belt z(10);
      z.src.assign(z.src.size(), 0);
      z.wd.px[0x18F00000u] = Stored(z.src);
      beltplate::Store sz;
      z.Call(sz);
      assets::Model mz;
      mz.materials = {plate_mat};
      float shz = -1.f;
      beltplate::BindPlate(mz, sz.Get(z.model), sz, &shz);
      Check(mz.materials[0].diffuse >= 0 && mz.materials[0].diffuse_alpha_clear == 1.0f && shz == 1.0f, "an all-zero BC3 plate (the 5009 canvas) gives the bound material diffuse_alpha_clear 1.0", "share " + std::to_string(mz.materials[0].diffuse_alpha_clear));
      // S110 PLATEINK: share 1.0 does not mean empty. Alpha 0 / 108 only (the 5009 name: black ink at alpha <= 108): share 1.0 AND ink > 0, max 108. An all-zero plate: share 1.0, ink 0.
      {
        Belt k(10);
        for (size_t i = 0; i < k.src.size(); i += 16) std::memset(&k.src[i], 0, 16), k.src[i] = 108, k.src[i + 1] = 0, k.src[i + 2] = (i % 32 == 0) ? 0x09 : 0;   // a0 108, a1 0; indices 0 (=108) or, in every other block, texels 0 and 1 = a1 (0)
        k.wd.px[0x18F00000u] = Stored(k.src);
        beltplate::Store sk;
        k.Call(sk);
        assets::Model mk;
        mk.materials = {plate_mat};
        float shk = -1.f;
        beltplate::Ink ik, iz;
        beltplate::BindPlate(mk, sk.Get(k.model), sk, &shk, &ik);
        Check(shk == 1.0f && ik.texels > 0 && ik.max_alpha == 108, "a plate with alpha 0 / 108 only: below-255 share 1.0 AND ink > 0, max alpha 108", "share " + std::to_string(shk) + " ink " + std::to_string(ik.texels) + " max " + std::to_string(ik.max_alpha));
        float shz2 = -1.f;
        beltplate::Store sz2;
        z.Call(sz2);
        assets::Model mz2;
        mz2.materials = {plate_mat};
        beltplate::BindPlate(mz2, sz2.Get(z.model), sz2, &shz2, &iz);
        Check(shz2 == 1.0f && iz.texels == 0 && iz.max_alpha == 0, "an all-zero plate: share 1.0, ink 0", "share " + std::to_string(shz2) + " ink " + std::to_string(iz.texels));
      }
      Belt o(10);
      for (size_t i = 0; i < o.src.size(); i += 16) std::memset(&o.src[i], 0, 16), o.src[i] = o.src[i + 1] = 255;   // alpha0 = alpha1 = 255, indices 0: every texel alpha 255
      o.wd.px[0x18F00000u] = Stored(o.src);
      beltplate::Store so;
      o.Call(so);
      assets::Model mo;
      mo.materials = {plate_mat};
      beltplate::BindPlate(mo, so.Get(o.model), so);
      Check(mo.materials[0].diffuse >= 0 && mo.materials[0].diffuse_alpha_clear == 0.0f, "a fully opaque plate gives diffuse_alpha_clear 0", "share " + std::to_string(mo.materials[0].diffuse_alpha_clear));
    }
  }
  // 6. a rewrite in place (the unlock flush) is re-read; a freed model address is dropped.
  {
    Belt b(10);
    beltplate::Store st;
    b.Call(st);
    const uint64_t h0 = st.Get(b.model) ? st.Get(b.model)->hash : 0;
    b.src = Source(99);
    b.wd.px[0x18F00000u] = Stored(b.src);
    b.Call(st);   // no flush yet: the cached picture stands (one table probe)
    const bool stale = st.Get(b.model) && st.Get(b.model)->hash == h0;
    guest_tex_dirty::NoteFlush(0x5300);
    b.Call(st);
    const auto p = st.Get(b.model);
    Check(stale && p && p->hash != h0 && p->blocks && *p->blocks == b.src, "an unlock flush of the plate's header re-reads the new picture", p ? std::to_string(p->hash) : "none");
    st.Drop(b.model);
    Check(st.Get(b.model) == nullptr, "a dropped model keeps no plate (a freed address reused by another belt)");
  }
  // 7. the production entry: the raw hook's first call (RawHookEntry, OURS = the stream-free build) on an event prop model reads the plate; a non-event model reads none.
  {
    Belt b(10);
    g_world = &b.wd;
    beltplate::TheStore().Clear();
    const uint32_t buf = 0x7000;
    b.wd.w[b.model] = 0x8206AF90, b.wd.w[0x8206AF90 + 8] = 0x82BD3AB8, b.wd.w[b.model + 296] = 1, b.wd.w[b.model + 300] = 1;
    const float one = 1.0f;
    std::memcpy(&b.wd.w[b.model + 252], &one, 4);
    for (uint32_t i = 0; i < 16; ++i) std::memcpy(&b.wd.w[buf + 4 * i], &one, 4);
    weaponrows::Tracker tr;
    tr.EventMake(b.model, 2004, 0);
    weaponrows::Slot13Scope own(b.model);
    weaponrows::CallResult out;
    auto rw = [&](uint32_t a, uint32_t& v) { return b.wd.Rw(a, v); };
    auto rm = [&](uint32_t a, uint32_t n, std::vector<rdr_scene::Mat4>& o) {
      o.resize(n);
      for (uint32_t i = 0; i < n * 16; ++i) {
        uint32_t v = 0;
        if (!b.wd.Rw(a + 4 * i, v)) return false;
        std::memcpy(&reinterpret_cast<float*>(o.data())[i], &v, 4);
      }
      return true;
    };
    const bool run = weaponrows::RawHookEntry(tr, true, /*ours*/ true, b.model, buf, 200, rw, rm, &out, 1.0f, 0, nullptr, nullptr, 0, &WorldPhys);
    const auto p = beltplate::TheStore().Get(b.model);
    Check(!run && out.event && p && p->blocks && *p->blocks == b.src, "the raw hook's first call on an event prop (OURS) reads the plate and does not run the original", p ? "t " + std::to_string(p->t) : "none");
    weaponrows::CallResult other;
    const bool run2 = weaponrows::RawHookEntry(tr, true, true, 0x4C00, buf, 200, rw, rm, &other, 1.0f, 0, nullptr, nullptr, 0, &WorldPhys);
    Check(!run2 && !other.event && !beltplate::TheStore().Get(0x4C00), "a model the event table does not know reads no plate");
    beltplate::TheStore().Clear();
    g_world = nullptr;
  }
  // 8. S105 PLATE review 1: a model address reused by a NEW belt (the free the destructor hook missed) must not keep the old plate: EventMake drops the store's entry.
  {
    Belt b(10);
    beltplate::TheStore().Clear();
    ModelWords(b.wd, b.model);
    weaponrows::Tracker tr;
    tr.EventMake(b.model, 2004, 0);
    Hook(b.wd, tr, b.model, 300);
    const auto p1 = beltplate::TheStore().Get(b.model);
    const std::vector<uint8_t> src2 = Source(77);
    b.wd.px[0x18F40000u] = Stored(src2);
    PutTexture(b.wd, 0x1002, 0x5200, "nameplate", 0x5800, 0x18F40000u, 20, 256, 128);   // the new holder's plate: a new header (no flush ever names the old one)
    tr.EventMake(b.model, 2004, 0);   // the same address made again
    Hook(b.wd, tr, b.model, 301);
    const auto p2 = beltplate::TheStore().Get(b.model);
    Check(p1 && p1->blocks && *p1->blocks == b.src && p2 && p2->blocks && *p2->blocks == src2, "a new belt made at a reused model address reads ITS plate, not the old holder's", p2 ? std::to_string(p2->hash) : "none");
    // the other way: a mic left `not a belt` at the address
    beltplate::TheStore().Clear();
    Belt m(10);
    PutModel(m.wd, m.model, {0x1001, 0x1003});
    ModelWords(m.wd, m.model);
    weaponrows::Tracker tm;
    tm.EventMake(m.model, 1, 0);
    Hook(m.wd, tm, m.model, 302);
    PutModel(m.wd, m.model, {0x1001, 0x1002, 0x1003});
    tm.EventMake(m.model, 2004, 0);
    Hook(m.wd, tm, m.model, 303);
    Check(beltplate::TheStore().Get(m.model) != nullptr, "a belt made where a mic stood is not left as `not a belt` (white forever)");
    beltplate::TheStore().Clear();
  }
  // 9. S105 PLATE review 5: the three Drop wirings (EventFree, Prune, NoteScene's Clear) each empty the store of the freed belt.
  {
    const auto setup = [&](Belt& b, weaponrows::Tracker& tr) {
      beltplate::TheStore().Clear();
      ModelWords(b.wd, b.model);
      tr.EventMake(b.model, 2004, 0);
      Hook(b.wd, tr, b.model, 400);
    };
    {   // EventFree: the CWeapon's destructor hook
      Belt b(10);
      weaponrows::Tracker tr;
      setup(b, tr);
      const bool had = beltplate::TheStore().Get(b.model) != nullptr;
      b.wd.w[0x9000] = rdr_scene::weapons::kEventWeaponVtable, b.wd.w[0x9000 + rdr_scene::weapons::kOffEventModels] = b.model;
      auto rw = [&b](uint32_t a, uint32_t& v) { return b.wd.Rw(a, v); };
      tr.EventFree(0x9000, rw);
      Check(had && beltplate::TheStore().Get(b.model) == nullptr, "EventFree (the CWeapon's destructor) drops the freed belt's plate");
    }
    {   // Prune: the model's vtable word is no longer a CSkinModel's
      Belt b(10);
      weaponrows::Tracker tr;
      setup(b, tr);
      const bool had = beltplate::TheStore().Get(b.model) != nullptr;
      b.wd.w[b.model] = 0;
      Hook(b.wd, tr, b.model, 401);   // the next frame's first model call runs NoteScene (-> Prune)
      Check(had && beltplate::TheStore().Get(b.model) == nullptr, "Prune (a model that is no longer a CSkinModel) drops its plate");
    }
    {   // NoteScene: the stage holder changes (a new arena / match): the list and the store clear
      Belt b(10);
      weaponrows::Tracker tr;
      setup(b, tr);
      const bool had = beltplate::TheStore().Get(b.model) != nullptr;
      b.wd.w[rdr_scene::weapons::kHolderGlobal] = 0x1111;
      Hook(b.wd, tr, b.model, 401);   // (0 -> X is a first write: no clear)
      b.wd.w[rdr_scene::weapons::kHolderGlobal] = 0x2222;
      Hook(b.wd, tr, b.model, 402);   // a change from a non-zero holder: the scene changed
      Check(had && beltplate::TheStore().Get(b.model) == nullptr, "NoteScene's holder change (Clear) empties the plate store");
    }
    beltplate::TheStore().Clear();
  }
  // 10. S105 PLATE review 3 / 4: a refused size is decided once (never retried), an unreadable read is retried and counted once; the plate header is unwatched on drop.
  {
    Belt b(10);
    beltplate::Store st;
    b.wd.px.clear();   // the pixels are gone: ReadMip0 answers Memory
    b.Call(st), b.Call(st), b.Call(st);
    Check(st.Counts().unreadable == 1 && !st.Get(b.model) && st.LastRefusal() == "plate pixels unreadable", "a Memory refusal is retried but counted once", "unreadable " + std::to_string(st.Counts().unreadable));
    b.wd.px[0x18F00000u] = Stored(b.src);
    b.Call(st);
    Check(st.Get(b.model) != nullptr, "...and the plate is read once the pixels are there");
    st.Drop(b.model);
    // a dropped plate's header is unwatched: a flush of it no longer moves any generation
    guest_tex_dirty::NoteFlush(0x5300);
    Check(guest_tex_dirty::Gen(0x5300) == 0, "Drop unwatches the plate's header (a dirty-table slot is not held for the rest of the run)");
    Belt big(10);
    PutTexture(big.wd, 0x1002, 0x5200, "nameplate", 0x5300, 0x18F00000u, 20, 1024, 1024);   // 1 MB of BC3: over the 64 KB cap = Size
    beltplate::Store s2;
    big.Call(s2), big.Call(s2);
    Check(s2.Counts().bad_shape == 1 && s2.Counts().unreadable == 0 && s2.Counts().no_plate == 0, "an over-size plate is decided (bad_shape), not retried every call", "bad_shape " + std::to_string(s2.Counts().bad_shape));
  }
  std::printf("belt_plate_unit: %d failed%s%s\n", g_fail, plant.empty() ? "" : " (plant ", plant.empty() ? "" : (plant + ")").c_str());
  if (!plant.empty()) return g_fail ? 0 : 1;   // a planted twin: exit 0 only when the checks SEE the plant
  return g_fail ? 1 : 0;
}
