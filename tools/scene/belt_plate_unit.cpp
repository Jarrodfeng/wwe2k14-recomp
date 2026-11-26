// S105 lane PLATE -> S111 CACMATCHTEX: an event prop's run-time textures are the live model's texture KEYS, read by name-table index (docs\research\s111\CACMATCHTEX.md; PLATE.md). Synthetic wrapper /
// registry / pixels only (no game files). Pure compile, 0 game runs.
// entry: RawHookEntry
// (enters through weaponrows::RawHookEntry, the pure body of the raw hook weapon_rows_glue.h RawEntry; RawEntry itself needs the game build, so the unit cannot name it as `entry:`)
//   belt_plate_unit                       exit 0 when every check passes
//   belt_plate_unit --plant NAME          NAME = index_ignored | wrong_t | silent_white | shared_pool | make_no_drop | free_no_drop | share_unset | diffuse_only | no_pending | pend_any | no_retire |
//                                         none_is_miss: exit 0 only when the checks SEE
//                                         the plant (same convention as texreg_unit)
// Production order driven: the loader's sampler step (matsamp::ApplySamplerFacts: the key indices) -> Want (the drawer's ask) -> RawHookEntry (the weapon rows' raw hook on an event prop, OURS mode, which
// calls OnEventCall) -> Want again / PoolSuffix (the drawer's pool key) -> BindKeys (the drawer's bind), plus a flush (guest_tex_dirty::NoteFlush), a key rebind and a Drop.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
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

constexpr uint32_t kReg = 0x2000, kEntries = 0x3000, kWrapperName = 16;
static void Registry(World& wd) { wd.w[particles::kTextureHandleGlobal] = kReg, wd.w[kReg + 8] = kEntries, wd.w[kReg + 16] = kEntries + 8 * 0x10000u; }

// A registered 2D texture: the registry entry {id, wrapper}, the wrapper (vtable A, kind 0, name at +16, header at +80), the header's six fetch dwords. The name is never read by the store (no name rule).
static void PutTexture(World& wd, uint32_t id, uint32_t wrapper, const char* name, uint32_t header, uint32_t base, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stored_id = 0) {
  wd.w[kEntries + (id & 0xFFFF) * 8] = stored_id ? stored_id : id, wd.w[kEntries + (id & 0xFFFF) * 8 + 4] = wrapper;
  wd.w[wrapper] = particles::kWrapperVtableA, wd.w[wrapper + particles::kWrapperKind] = 0, wd.w[wrapper + particles::kWrapperTexture] = header;
  char n[16] = {};
  for (int i = 0; i < 15 && name[i]; ++i) n[i] = name[i];
  for (uint32_t i = 0; i < 4; ++i) wd.w[wrapper + kWrapperName + 4 * i] = uint32_t(uint8_t(n[4 * i])) << 24 | uint32_t(uint8_t(n[4 * i + 1])) << 16 | uint32_t(uint8_t(n[4 * i + 2])) << 8 | uint8_t(n[4 * i + 3]);
  const uint32_t f[6] = {((w / 32u) << 22), base | (1u << 6) | fmt, (w - 1) | ((h - 1) << 13), 0, 0, 1u << 9};   // linear (no tile bit), endian 1 (8-in-16), pitch = w / 32 units
  for (int i = 0; i < 6; ++i) wd.w[header + particles::kHeaderFetch + 4 * i] = f[i];
}
// A picture's bytes as the reader returns them: `bytes` of blocks; Stored = as the game keeps them (each block 8-in-16 swapped). Every fixture's row is a multiple of 32 blocks (no pitch padding).
static std::vector<uint8_t> Source(uint8_t seed, size_t bytes = 64 * 32 * 16) {
  std::vector<uint8_t> v(bytes);
  for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(seed + 13 * i + (i >> 5));
  return v;
}
static std::vector<uint8_t> Stored(const std::vector<uint8_t>& src, uint32_t bpb = 16) {
  std::vector<uint8_t> m = src;
  for (size_t i = 0; i < m.size(); i += bpb) guest_tex::SwapBlock(m.data() + i, bpb, 1);
  return m;
}
static void PutModel(World& wd, uint32_t model, const std::vector<uint32_t>& handles) {
  const uint32_t keys = model + 0x200;
  wd.w[model + beltplate::kModelKeyCount] = uint32_t(handles.size()), wd.w[model + beltplate::kModelKeys] = keys;
  for (size_t i = 0; i < handles.size(); ++i) wd.w[keys + 8 * uint32_t(i)] = handles[i], wd.w[keys + 8 * uint32_t(i) + 4] = 0;
}
static assets::Material Mat(int kd, int kn = -1, int ks = -1, int diffuse = -1, const char* dname = "") {
  assets::Material m;
  m.key_diffuse = kd, m.key_normal = kn, m.key_specular = ks, m.diffuse = diffuse, m.diffuse_name = dname;
  return m;
}

// One nameplate world (a stock belt's entry-4 model): keys [color t0, nameplate t1, color2 t2, stale t3]; its material samples t1 as the diffuse.
struct Belt {
  World wd;
  std::vector<uint8_t> src;
  uint32_t model = 0x4000;
  explicit Belt(uint8_t seed, uint32_t base = 0x18F00000u) {
    Registry(wd);
    src = Source(seed);
    wd.px[base] = Stored(src);
    wd.px[0x18E00000u] = std::vector<uint8_t>(32 * 8 * 8, 0x5A);
    PutTexture(wd, 0x1001, 0x5000, "color", 0x5100, 0x18E00000u, 18, 32, 32);
    PutTexture(wd, 0x1002, 0x5200, "nameplate", 0x5300, base, 20, 256, 128);
    PutTexture(wd, 0x1003, 0x5400, "color2", 0x5500, 0x18E00000u, 18, 32, 32);
    PutTexture(wd, 0x1004, 0x5600, "stale", 0x5700, 0x18E00000u, 18, 32, 32, /*stored_id*/ 0x2004);   // the registry entry's stored id differs: a stale handle
    PutModel(wd, model, {0x1001, 0x1002, 0x1003, 0x1004});
  }
  void Call(beltplate::Store& st) { st.OnEventCall(model, [&](uint32_t a, uint32_t& v) { return wd.Rw(a, v); }, [&](uint32_t a, uint32_t n, uint8_t* d) { return wd.Phys(a, n, d); }); }
  void Read(beltplate::Store& st, uint64_t mask = 1u << 1) { st.Want(model, mask), Call(st); }
};
static const assets::Material kPlateMat = Mat(1, -1, -1, -1, "nameplate");

// The Create a Championship belt (BOBJ 2926 entry 0): name table plate specular plate_n belt belt_n nameplate; keys as sub_833250A8 / sub_83335270 leave them: 0, 3, 5 = T0 (the composited plate,
// BC3), 2, 4 = T1 (the normal map, BC3), 1 = a specular picture (BC1 128 x 128) or none. Three meshes: (plate, plate_n, specular), twice (belt, belt_n, specular).
struct Cab {
  World wd;
  std::vector<uint8_t> t0, t1, sp;
  uint32_t model = 0x4400;
  explicit Cab(bool with_spec = true) {
    Registry(wd);
    t0 = Source(30), t1 = Source(60), sp = Source(90, 32 * 32 * 8);
    wd.px[0x18A00000u] = Stored(t0), wd.px[0x18A40000u] = Stored(t1), wd.px[0x18A80000u] = Stored(sp, 8);
    PutTexture(wd, 0x1101, 0x5800, "", 0x5900, 0x18A00000u, 20, 256, 128);
    PutTexture(wd, 0x1102, 0x5A00, "", 0x5B00, 0x18A40000u, 20, 256, 128);
    PutTexture(wd, 0x1103, 0x5C00, "specular", 0x5D00, 0x18A80000u, 18, 128, 128);
    PutModel(wd, model, {0x1101, with_spec ? 0x1103u : 0u, 0x1102, 0x1101, 0x1102, 0x1101});
  }
  static assets::Model File() {
    assets::Model m;
    m.materials = {Mat(0, 2, 1, -1, "plate"), Mat(3, 4, 1, -1, "belt"), Mat(3, 4, 1, -1, "belt")};
    return m;
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
static weaponrows::CallResult Hook(World& wd, weaponrows::Tracker& tr, uint32_t model, uint64_t frame, bool* run = nullptr) {
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
  const bool r = weaponrows::RawHookEntry(tr, true, /*ours*/ true, model, 0x7000, frame, rw, rm, &out, 1.0f, 0, nullptr, nullptr, 0, &WorldPhys);
  if (run) *run = r;
  g_world = nullptr;
  return out;
}

int main(int argc, char** argv) {
  std::string plant;
  if (argc > 2 && std::string(argv[1]) == "--plant") plant = argv[2];
  beltplate::g_plate_plant = plant == "index_ignored" ? 1 : plant == "wrong_t" ? 2 : plant == "silent_white" ? 3 : plant == "shared_pool" ? 4 : plant == "make_no_drop" ? 5 : plant == "free_no_drop" ? 6
                           : plant == "share_unset" ? 7 : plant == "diffuse_only" ? 8 : plant == "no_pending" ? 9 : plant == "pend_any" ? 10 : plant == "no_retire" ? 11 : plant == "none_is_miss" ? 12 : 0;
  if (!plant.empty() && beltplate::g_plate_plant == 0) return std::printf("unknown plant %s\n", plant.c_str()), 2;

  // 0. the loader keeps each sampler's name-table index (the key index), whether or not a texture directory holds the name: BOBJ 2926 entry 0 mesh 0 (yProp_BumpMap).
  {
    assets::Material m;
    m.shader = "yProp_BumpMap";
    const assets::matsamp::Ctx c{true, false};
    assets::matsamp::ApplySamplerFacts(m, "sampDiffusePrm", 0, true, "plate", c);
    assets::matsamp::ApplySamplerFacts(m, "sampNormalPrm", 2, true, "plate_n", c);
    assets::matsamp::ApplySamplerFacts(m, "sampSpecularPrm", 1, true, "specular", c);
    Check(m.key_diffuse == 0 && m.key_normal == 2 && m.key_specular == 1 && m.diffuse_name == "plate" && m.diffuse < 0, "the sampler step keeps the diffuse / normal / specular key indices (0, 2, 1) and the diffuse name",
          std::to_string(m.key_diffuse) + " " + std::to_string(m.key_normal) + " " + std::to_string(m.key_specular));
    assets::Model fm;
    fm.materials = {m};
    Check(beltplate::WantMask(fm) == 0x7u, "the drawer asks for exactly those keys (bits 0, 1, 2)", std::to_string(beltplate::WantMask(fm)));
    fm.materials[0].diffuse = fm.materials[0].normal = fm.materials[0].specular = 0;
    Check(beltplate::WantMask(fm) == 0, "a model the file fills asks for nothing (a stock belt reads no key)");
  }
  // 1. the nameplate is key t of its own name-table index: asked, pending until the hook's next call, then read (shape and bytes the game's).
  {
    Belt b(10);
    beltplate::Store st;
    assets::Model fm;
    fm.materials = {kPlateMat};
    const uint64_t mask = beltplate::WantMask(fm);
    const beltplate::Ask a0 = st.Want(b.model, mask);
    Check(mask == 2u && a0.pending && !a0.set, "the first ask is pending (the key was never tried): the drawer waits one frame", a0.pending ? "pending" : "not pending");
    b.Call(st);
    const beltplate::Ask a1 = st.Want(b.model, mask);
    const beltplate::Plate* p = a1.set ? a1.set->Find(1) : nullptr;
    Check(!a1.pending && p && p->blocks && p->width == 256 && p->height == 128 && p->format == assets::TexFormat::kBC3, "after the hook's call key 1 is read: BC3 256 x 128",
          p ? std::to_string(p->width) + "x" + std::to_string(p->height) : "none");
    Check(p && p->blocks && *p->blocks == b.src, "its 32 KB are the game's bytes (linear, 8-in-16 swapped back)", p && p->blocks ? std::to_string(p->blocks->size()) + " bytes" : "none");
  }
  // 2. the bind: the diffuse that wanted key 1 gets it as a texture; a material the file fills is untouched; a diffuse with nothing is counted (never silent white); the alpha facts are set.
  {
    Belt b(10);
    beltplate::Store st;
    b.Read(st);
    assets::Model m;
    m.materials = {kPlateMat, Mat(-1, -1, -1, 0, "color"), Mat(1, -1, -1, 0, "color")};   // (S111 CACMATCHTEX review: [2] names key 1 too, but its file fills the slot: the file's texture stands)
    assets::Texture existing;
    existing.name = "color";
    m.textures.push_back(existing);
    const uint32_t n = beltplate::BindKeys(m, st.Get(b.model), st);
    const bool bound = n == 1 && m.materials[0].diffuse == 1 && m.textures.size() == 2 && m.textures[1].name == "nameplate" && m.textures[1].format == assets::TexFormat::kBC3 && m.textures[1].width == 256 &&
                       m.textures[1].height == 128 && m.textures[1].data == b.src && m.materials[1].diffuse == 0 && m.materials[2].diffuse == 0;
    Check(bound, "the material whose diffuse wanted key 1 binds the plate (BC3 256 x 128, the game's bytes); the other materials are untouched (one names key 1 but its file fills the slot)", "bound " + std::to_string(n));
    assets::Model m2;
    m2.materials = {kPlateMat};
    beltplate::Store st2;
    const uint32_t n2 = beltplate::BindKeys(m2, nullptr, st2);
    Check(n2 == 0 && m2.materials[0].diffuse < 0 && st2.Counts().missing == 1, "a diffuse with no key read stays unbound AND is counted (never silent white)", "missing " + std::to_string(st2.Counts().missing));
    {   // S110 BELTPLATE: an all-zero BC3 plate (the 5009 canvas) gives diffuse_alpha_clear 1.0; a fully opaque one 0
      Belt z(10);
      z.src.assign(z.src.size(), 0);
      z.wd.px[0x18F00000u] = Stored(z.src);
      beltplate::Store sz;
      z.Read(sz);
      assets::Model mz;
      mz.materials = {kPlateMat};
      float shz = -1.f;
      beltplate::Ink iz;
      beltplate::BindKeys(mz, sz.Get(z.model), sz, &shz, &iz);
      Check(mz.materials[0].diffuse >= 0 && mz.materials[0].diffuse_alpha_clear == 1.0f && shz == 1.0f && iz.texels == 0, "an all-zero BC3 plate: diffuse_alpha_clear 1.0, ink 0", "share " + std::to_string(mz.materials[0].diffuse_alpha_clear));
      Belt k(10);   // S110 PLATEINK: alpha 0 / 108 only (the 5009 name: black ink at alpha <= 108): share 1.0 AND ink > 0, max 108
      for (size_t i = 0; i < k.src.size(); i += 16) std::memset(&k.src[i], 0, 16), k.src[i] = 108, k.src[i + 1] = 0, k.src[i + 2] = (i % 32 == 0) ? 0x09 : 0;
      k.wd.px[0x18F00000u] = Stored(k.src);
      beltplate::Store sk;
      k.Read(sk);
      assets::Model mk;
      mk.materials = {kPlateMat};
      float shk = -1.f;
      beltplate::Ink ik;
      beltplate::BindKeys(mk, sk.Get(k.model), sk, &shk, &ik);
      Check(shk == 1.0f && ik.texels > 0 && ik.max_alpha == 108, "a plate with alpha 0 / 108 only: below-255 share 1.0 AND ink > 0, max alpha 108", "share " + std::to_string(shk) + " ink " + std::to_string(ik.texels));
      Belt o(10);
      for (size_t i = 0; i < o.src.size(); i += 16) std::memset(&o.src[i], 0, 16), o.src[i] = o.src[i + 1] = 255;
      o.wd.px[0x18F00000u] = Stored(o.src);
      beltplate::Store so;
      o.Read(so);
      assets::Model mo;
      mo.materials = {kPlateMat};
      beltplate::BindKeys(mo, so.Get(o.model), so);
      Check(mo.materials[0].diffuse >= 0 && mo.materials[0].diffuse_alpha_clear == 0.0f, "a fully opaque plate gives diffuse_alpha_clear 0", "share " + std::to_string(mo.materials[0].diffuse_alpha_clear));
    }
  }
  // 3. S111 CACMATCHTEX: the created belt (no texture in its file) draws the composited plate at the diffuse, its normal map at the normal and the key-1 picture at the specular, one texture per key handle.
  {
    Cab c;
    beltplate::Store st;
    assets::Model bound = Cab::File();
    const uint64_t mask = beltplate::WantMask(bound);
    Check(mask == 0x1Fu, "the created belt asks for keys 0..4 (plate, specular, plate_n, belt, belt_n)", std::to_string(mask));
    st.Want(c.model, mask), c.Call(st);
    const beltplate::Ask a = st.Want(c.model, mask);
    const uint32_t n = beltplate::BindKeys(bound, a.set, st);
    const auto data = [&](int i) { return i >= 0 && size_t(i) < bound.textures.size() ? bound.textures[size_t(i)].data : std::vector<uint8_t>(); };
    const auto& m0 = bound.materials[0];
    const auto& m1 = bound.materials[1];
    Check(!a.pending && data(m0.diffuse) == c.t0 && data(m1.diffuse) == c.t0 && bound.materials[2].diffuse == m1.diffuse, "the plate AND the strap draw T0 (keys 0 and 3) at the diffuse",
          "diffuse " + std::to_string(m0.diffuse) + " / " + std::to_string(m1.diffuse));
    Check(data(m0.normal) == c.t1 && data(m1.normal) == c.t1 && bound.textures[size_t(std::max(m0.normal, 0))].format == assets::TexFormat::kBC3, "plate_n and belt_n (keys 2, 4) draw T1 at the normal",
          "normal " + std::to_string(m0.normal));
    Check(data(m0.specular) == c.sp && bound.textures[size_t(std::max(m0.specular, 0))].format == assets::TexFormat::kBC1, "key 1 draws at the specular (BC1, the key's own format)", "specular " + std::to_string(m0.specular));
    Check(n == 9 && bound.textures.size() == 3 && m0.diffuse == m1.diffuse, "nine slots, three textures: T0 is uploaded once for keys 0 and 3 (one handle)", "bound " + std::to_string(n) + " textures " + std::to_string(bound.textures.size()));
    Check(st.Counts().missing == 0 && st.Counts().other_unbound == 0, "nothing counted missing");
    // the editor composites again into NEW textures and rebinds keys 0 / 3 / 5 (sub_83335270): the next call reads the new plate
    const std::vector<uint8_t> t0b = Source(120);
    c.wd.px[0x18AC0000u] = Stored(t0b);
    PutTexture(c.wd, 0x1104, 0x5E00, "", 0x5F00, 0x18AC0000u, 20, 256, 128);
    PutModel(c.wd, c.model, {0x1104, 0x1103, 0x1102, 0x1104, 0x1102, 0x1104});
    const uint64_t h0 = a.set ? a.set->hash : 0;
    c.Call(st);
    const auto s2 = st.Get(c.model);
    const beltplate::Plate* p0 = s2 ? s2->Find(0) : nullptr;
    Check(p0 && p0->blocks && *p0->blocks == t0b && s2->hash != h0, "a key the game rebinds to a new texture is read again (a new pool key)", p0 ? std::to_string(p0->handle) : "none");
    // no specular key (handle 0): the specular stays the file's (none) and is counted, the diffuse / normal still bind
    Cab ns(false);
    beltplate::Store s3;
    assets::Model b3 = Cab::File();
    s3.Want(ns.model, mask), ns.Call(s3);
    beltplate::BindKeys(b3, s3.Get(ns.model), s3);
    Check(b3.materials[0].specular < 0 && b3.materials[0].diffuse >= 0 && b3.materials[0].normal >= 0 && s3.Counts().game_none == 3 && s3.Counts().other_unbound == 0 && s3.Counts().no_plate == 1 &&
              s3.Counts().missing == 0,
          "a key-1 handle of 0 (sub_82294F20 resolves it to nothing: the game binds no specular either): specular unbound, counted game_none 3 (not other_unbound), diffuse / normal bound",
          "game_none " + std::to_string(s3.Counts().game_none) + " other " + std::to_string(s3.Counts().other_unbound));
  }
  // 4. two holders' belts never share a pool: the set's hash is in the pool key.
  {
    Belt a(10);
    World wd = a.wd;
    const std::vector<uint8_t> src2 = Source(77);
    wd.px[0x18F40000u] = Stored(src2);
    PutTexture(wd, 0x1012, 0x6200, "nameplate", 0x6300, 0x18F40000u, 20, 256, 128);
    PutModel(wd, 0x4800, {0x1001, 0x1012});
    beltplate::Store st;
    auto rw = [&](uint32_t x, uint32_t& v) { return wd.Rw(x, v); };
    auto ph = [&](uint32_t x, uint32_t n, uint8_t* d) { return wd.Phys(x, n, d); };
    st.Want(0x4000, 2), st.Want(0x4800, 2);
    st.OnEventCall(0x4000, rw, ph), st.OnEventCall(0x4800, rw, ph);
    const std::string sa = beltplate::PoolSuffix(st.Get(0x4000)), sb = beltplate::PoolSuffix(st.Get(0x4800));
    Check(!sa.empty() && !sb.empty() && sa != sb, "two holders (two plates) give two pool keys", sa + " / " + sb);
    const auto s48 = st.Get(0x4800);
    Check(s48 && s48->Find(1) && *s48->Find(1)->blocks == src2, "the second holder's plate is its own bytes");
  }
  // 5. a rewrite in place (the unlock flush) is re-read; a freed model address is dropped.
  {
    Belt b(10);
    beltplate::Store st;
    b.Read(st);
    const uint64_t h0 = st.Get(b.model) ? st.Get(b.model)->hash : 0;
    b.src = Source(99);
    b.wd.px[0x18F00000u] = Stored(b.src);
    b.Call(st);   // no flush yet: the cached picture stands (one key word, one generation compare)
    const bool stale = st.Get(b.model) && st.Get(b.model)->hash == h0;
    guest_tex_dirty::NoteFlush(0x5300);
    b.Call(st);
    const auto s = st.Get(b.model);
    Check(stale && s && s->hash != h0 && s->Find(1) && *s->Find(1)->blocks == b.src && st.Counts().reread == 1, "an unlock flush of the key's header re-reads the new picture", s ? std::to_string(s->hash) : "none");
    st.Drop(b.model);
    Check(st.Get(b.model) == nullptr, "a dropped model keeps no set (a freed address reused by another belt)");
  }
  // 6. the production entry: an asked event prop model is read at the raw hook's call (RawHookEntry, OURS = the stream-free build); a non-event model reads none.
  {
    Belt b(10);
    beltplate::TheStore().Clear();
    ModelWords(b.wd, b.model);
    weaponrows::Tracker tr;
    tr.EventMake(b.model, 2004, 0);
    beltplate::TheStore().Want(b.model, 2);
    bool run = true;
    const weaponrows::CallResult out = Hook(b.wd, tr, b.model, 200, &run);
    const auto s = beltplate::TheStore().Get(b.model);
    Check(!run && out.event && s && s->Find(1) && *s->Find(1)->blocks == b.src, "the raw hook's call on an asked event prop (OURS) reads its key and does not run the original");
    beltplate::TheStore().Want(0x4C00, 2);
    bool run2 = true;
    const weaponrows::CallResult other = Hook(b.wd, tr, 0x4C00, 200, &run2);
    Check(!run2 && !other.event && !beltplate::TheStore().Get(0x4C00), "a model the event table does not know reads nothing");
    beltplate::TheStore().Clear();
  }
  // 7. S105 PLATE review 1: a model address reused by a NEW belt (the free the destructor hook missed) must not keep the old set: EventMake drops the store's entry.
  {
    Belt b(10);
    beltplate::TheStore().Clear();
    ModelWords(b.wd, b.model);
    weaponrows::Tracker tr;
    tr.EventMake(b.model, 2004, 0);
    beltplate::TheStore().Want(b.model, 2);
    Hook(b.wd, tr, b.model, 300);
    const auto s1 = beltplate::TheStore().Get(b.model);
    const std::vector<uint8_t> src2 = Source(77);
    b.wd.px[0x18F40000u] = Stored(src2);
    PutTexture(b.wd, 0x1002, 0x5200, "nameplate", 0x5800, 0x18F40000u, 20, 256, 128);   // the new holder's plate: the same handle, a new header (no flush ever names the old one)
    tr.EventMake(b.model, 2004, 0);   // the same address made again
    beltplate::TheStore().Want(b.model, 2);
    Hook(b.wd, tr, b.model, 301);
    const auto s2 = beltplate::TheStore().Get(b.model);
    Check(s1 && s1->Find(1) && *s1->Find(1)->blocks == b.src && s2 && s2->Find(1) && *s2->Find(1)->blocks == src2, "a new belt made at a reused model address reads ITS plate, not the old holder's",
          s2 ? std::to_string(s2->hash) : "none");
    beltplate::TheStore().Clear();
  }
  // 8. S105 PLATE review 5: the three Drop wirings (EventFree, Prune, NoteScene's Clear) each empty the store of the freed belt.
  {
    const auto setup = [&](Belt& b, weaponrows::Tracker& tr) {
      beltplate::TheStore().Clear();
      ModelWords(b.wd, b.model);
      tr.EventMake(b.model, 2004, 0);
      beltplate::TheStore().Want(b.model, 2);
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
      Check(had && beltplate::TheStore().Get(b.model) == nullptr, "EventFree (the CWeapon's destructor) drops the freed belt's set");
    }
    {   // Prune: the model's vtable word is no longer a CSkinModel's
      Belt b(10);
      weaponrows::Tracker tr;
      setup(b, tr);
      const bool had = beltplate::TheStore().Get(b.model) != nullptr;
      b.wd.w[b.model] = 0;
      Hook(b.wd, tr, b.model, 401);   // the next frame's first model call runs NoteScene (-> Prune)
      Check(had && beltplate::TheStore().Get(b.model) == nullptr, "Prune (a model that is no longer a CSkinModel) drops its set");
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
      Check(had && beltplate::TheStore().Get(b.model) == nullptr, "NoteScene's holder change (Clear) empties the store");
    }
    beltplate::TheStore().Clear();
  }
  // 9. S105 PLATE review 3 / 4: a refused size is decided once (never retried), an unreadable read is retried and counted once; the key's header is unwatched on drop; a pending ask is bounded.
  {
    Belt b(10);
    beltplate::Store st;
    b.wd.px.clear();   // the pixels are gone: ReadMip0 answers Memory
    st.Want(b.model, 2), b.Call(st), b.Call(st), b.Call(st);
    Check(st.Counts().unreadable == 1 && (!st.Get(b.model) || st.Get(b.model)->keys.empty()) && st.LastRefusal() == "key pixels unreadable", "a Memory refusal is retried but counted once", "unreadable " + std::to_string(st.Counts().unreadable));
    b.wd.px[0x18F00000u] = Stored(b.src);
    b.Call(st);
    Check(st.Get(b.model) && st.Get(b.model)->Find(1), "...and the key is read once the pixels are there");
    st.Drop(b.model);
    guest_tex_dirty::NoteFlush(0x5300);
    Check(guest_tex_dirty::Gen(0x5300) == 0, "Drop unwatches the key's header (a dirty-table slot is not held for the rest of the run)");
    Belt big(10);
    PutTexture(big.wd, 0x1002, 0x5200, "nameplate", 0x5300, 0x18F00000u, 20, 2048, 1024);   // 2 MB of BC3: over the 1 MB cap = Size
    beltplate::Store s2;
    s2.Want(big.model, 2), big.Call(s2), big.Call(s2);
    Check(s2.Counts().bad_shape == 1 && s2.Counts().unreadable == 0 && s2.Counts().no_plate == 0, "an over-size key texture is decided (bad_shape), not retried every call", "bad_shape " + std::to_string(s2.Counts().bad_shape));
    beltplate::Store s3;
    bool last = true;
    for (uint32_t i = 0; i <= beltplate::kMaxPendingAsks; ++i) last = s3.Want(0x4000, 2).pending;
    Check(!last && s3.Counts().pending == beltplate::kMaxPendingAsks, "a model the hook never calls is pending at most kMaxPendingAsks asks (then drawn: never hidden for good)", "pending " + std::to_string(s3.Counts().pending));
  }
  // 10. S111 CACMATCHTEX review: only an unread DIFFUSE key holds a prop back. A prop whose file lacks only its specular is drawn on its first frame (and in an offline rig, which never feeds the store, on
  // every ask); its specular is not counted while unread and binds once the hook has read it.
  {
    assets::Model fm;
    fm.materials = {Mat(0, 1, 2, 0)};   // diffuse key 0 and normal key 1 filled by the file; the specular (key 2) is not
    fm.materials[0].normal = 0;
    const uint64_t mask = beltplate::WantMask(fm), hold = beltplate::HoldMask(fm);
    beltplate::Store st;
    const beltplate::Ask a0 = st.Want(0x4000, mask, hold);
    bool rig = !a0.pending;
    for (int i = 0; i < 12; ++i) rig = rig && !st.Want(0x4000, mask, hold).pending;
    assets::Model b0 = fm;
    beltplate::BindKeys(b0, a0.set, st);
    Check(mask == 4u && hold == 0 && !a0.pending && rig && b0.materials[0].specular < 0 && st.Counts().other_unbound == 0 && st.Counts().missing == 0 && st.Counts().pending == 0,
          "a prop whose file lacks only its specular draws on its first frame and on every ask of a rig that never feeds the store; the unread specular is not counted",
          std::string(a0.pending ? "pending" : "drawn") + " mask " + std::to_string(mask) + " hold " + std::to_string(hold));
    assets::Model pm;
    pm.materials = {kPlateMat};
    beltplate::Store s2;
    Check(s2.Want(0x4000, beltplate::WantMask(pm), beltplate::HoldMask(pm)).pending, "an unread DIFFUSE key still holds the prop until the hook's call (the nameplate)");
    Belt bb(10);   // key 2 = color2 (BC1 32 x 32)
    beltplate::Store s3;
    s3.Want(bb.model, mask, hold), bb.Call(s3);
    assets::Model b3 = fm;
    beltplate::BindKeys(b3, s3.Want(bb.model, mask, hold).set, s3);
    Check(b3.materials[0].specular >= 0 && b3.textures.size() == 1 && b3.textures[0].format == assets::TexFormat::kBC1, "...and its specular binds once the hook has read it (a new set: a new pool)");
  }
  // 11. S111 CACMATCHTEX review: one live pool per model. The drawer's pool names (PoolSuffix) through PoolTrack, retired at the next frame's start as rf_props.cpp does (RetireOther(old) drops every name
  // it prefixes): N editor recomposites of the created belt leave ONE pool, not N.
  {
    Cab c;
    beltplate::Store st;
    const assets::Model fm = Cab::File();
    const uint64_t mask = beltplate::WantMask(fm), hold = beltplate::HoldMask(fm);
    beltplate::PoolTrack track;
    std::set<std::string> live;   // the pools the model cache holds
    int made = 0;
    const auto frame = [&]() {
      for (const std::string& old : track.TakeRetire())
        for (auto it = live.begin(); it != live.end();) it = it->rfind(old, 0) == 0 ? live.erase(it) : std::next(it);
      const beltplate::Ask a = st.Want(c.model, mask, hold);
      if (a.pending) return;
      const std::string pool = "weapon:ev2926/0" + beltplate::PoolSuffix(a.set, mask != 0);
      if (live.insert(pool).second) ++made;
      track.Note(c.model, pool);
    };
    frame(), c.Call(st), frame();
    const int kN = 5;
    for (int i = 0; i < kN; ++i) {   // the editor composites into a NEW texture and rebinds keys 0 / 3 / 5 (sub_83335270)
      const uint32_t base = 0x18B00000u + 0x40000u * uint32_t(i), id = 0x1120u + uint32_t(i);
      c.wd.px[base] = Stored(Source(uint8_t(140 + i)));
      PutTexture(c.wd, id, 0x6400 + 0x200 * uint32_t(i), "", 0x6500 + 0x200 * uint32_t(i), base, 20, 256, 128);
      PutModel(c.wd, c.model, {id, 0x1103, 0x1102, id, 0x1102, id});
      c.Call(st), frame();
    }
    frame();
    Check(made == kN + 1 && live.size() == 1, "N recomposites of one model leave ONE live pool (each new set retires the model's previous pool)",
          "made " + std::to_string(made) + " live " + std::to_string(live.size()));
    beltplate::PoolTrack t2;
    t2.Note(1, "weapon:ev1/0#pa"), t2.Note(2, "weapon:ev1/0#pa"), t2.Note(1, "weapon:ev1/0#pb");
    Check(t2.TakeRetire().empty(), "a pool another model still draws is not retired (two holders of one set)");
  }
  // 12. S111 CACMATCHTEX review: `missing` is a key the game's draw binds and ours did not. A key sub_82294F20 resolves to nothing (a stale handle) or an index past [model+256] is the game's own empty
  // slot (sub_82C04DF0 binds nothing there): game_none, never a miss. A key the game binds and ours refuses (an uncompressed picture) IS a miss.
  {
    Belt b(10);   // key 3 = "stale" (the registry's stored id differs); the table has 4 keys
    beltplate::Store st;
    assets::Model m;
    m.materials = {Mat(3, -1, -1, -1, "stale"), Mat(9, -1, -1, -1, "past")};
    st.Want(b.model, beltplate::WantMask(m), beltplate::HoldMask(m)), b.Call(st);
    beltplate::BindKeys(m, st.Want(b.model, beltplate::WantMask(m), beltplate::HoldMask(m)).set, st);
    Check(st.Counts().missing == 0 && st.Counts().game_none == 2 && m.materials[0].diffuse < 0 && m.materials[1].diffuse < 0,
          "a diffuse whose key the game resolves to nothing (a stale handle; an index past the table) counts game_none, not missing", "missing " + std::to_string(st.Counts().missing) + " game_none " + std::to_string(st.Counts().game_none));
    Belt u(10);
    PutTexture(u.wd, 0x1002, 0x5200, "nameplate", 0x5300, 0x18F00000u, 6, 64, 64);   // game format 6 (8888): ShapeOfFetch refuses it here
    beltplate::Store s2;
    assets::Model m2;
    m2.materials = {kPlateMat};
    s2.Want(u.model, 2, 2), u.Call(s2);
    beltplate::BindKeys(m2, s2.Want(u.model, 2, 2).set, s2);
    Check(s2.Counts().missing == 1 && s2.Counts().game_none == 0, "a diffuse whose key the game binds and ours refuses (8888) is a miss", "missing " + std::to_string(s2.Counts().missing));
  }
  std::printf("belt_plate_unit: %d failed%s%s\n", g_fail, plant.empty() ? "" : " (plant ", plant.empty() ? "" : (plant + ")").c_str());
  if (!plant.empty()) return g_fail ? 0 : 1;   // a planted twin: exit 0 only when the checks SEE the plant
  return g_fail ? 1 : 0;
}
