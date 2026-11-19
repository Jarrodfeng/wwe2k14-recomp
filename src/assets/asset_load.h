// Session 96 (asset runtime loader): wrestlers and arenas from the player's own game files -> assets::Model.
// Wrestler: pac\ch\chNNN.pac = EPAC -> one PACH per attire (p0, p1, ...). In a PACH: entry 0 = the body JBOY, further JBOY
// pieces (26, 27, 10000.., 20002, 21000..), texture directories 10 / 12 / 13-16. Arena: pac\bg\bgNN.pac = one PACH
// (p0): keys 0-399 stage JBOY models, 400 / 401 / 402 texture directories.
// Not thread-safe: the last opened pac is cached so that loading every attire of a wrestler unpacks the file once.
#pragma once

#include <algorithm>
#include "ring_low_keys.h"   // S103 RLB2: kRingLowKeys
#include "load_plants.h"   // S103 RL: load_detail::PropsSet100Plant (the rig's planted twin)
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "arena_lm.h"   // S103 B3: the arena's LightMapParam (c184) from the game's own table
#include "asset_model.h"
#include "car_arena.h"   // S103 CAR: Create-an-Arena venues (ids 200-249): the merged part pac
#include "crowd_motion.h"
#include "dds.h"
#include "entrance_light.h"
#include "jboy.h"
#include "piece_keys.h"   // S103 W2B / ENT
#include "pac.h"
#include "stage_key.h"
#include "stage_media.h"
#include "stage_pools.h"
#include "stage_tree.h"

namespace assets {

namespace load_detail {

// S101 lane 40 (Opus review A1): the loader's pac cache (TheCache), stage_pools' cache and CrowdPose() are process-wide statics and the loader is not thread-safe, while the crowd's
// and the arena's workers both load. Every public entry that reaches them (crowd_body.cpp, arena_game_space.cpp) holds this. Recursive: LoadBodyLodGameSpace calls LoadBody.
inline std::recursive_mutex& LoadMutex() {
  static std::recursive_mutex m;
  return m;
}

struct Cache {
  std::string rel;
  pac::Pac pac;
};
inline Cache& TheCache() {
  static Cache c;
  return c;
}

// Opens (or re-uses) the pac; `cached` says whether the unpack was skipped.
inline const pac::Pac* OpenCached(launcher::GameFiles& files, const std::string& rel, std::string& err) {
  Cache& c = TheCache();
  // S103 CAR: a created arena's name (pac\bg\bg200-249.pac) is the merged part pac of the published venue (car_arena.h), keyed by its part indices; with no venue published it stays the (missing) bg name
  int car_id = 0;
  car::Sel car_sel;
  const bool car_custom = car::CustomIdOfRel(rel, car_id) && (car_sel = car::Current()).valid && car_sel.arena == car_id;   // S103 CAR fix 4: the selection must be this id's
  const std::string want = car_custom ? rel + "#" + car_sel.Key() : rel;
  if (c.rel == want && !c.pac.pachs.empty()) return &c.pac;
  c.rel.clear();
  if (car_custom) {
    car::MergeInfo mi;
    if (!car::BuildFromFiles(files, car_sel, c.pac, mi, err)) return car::Stats().build_failed.fetch_add(1, std::memory_order_relaxed), nullptr;
    car::NoteBuilt(mi);
    car::PartMap() = mi.part_of;   // S103 CAR fix 2: which part each entry came from (LoadArena's per-part texture sets); written under the loader mutex
  } else if (!pac::Open(files, rel, c.pac, err)) {
    return nullptr;
  }
  c.rel = want;
  return &c.pac;
}

inline std::string WrestlerRel(int id) {
  char b[64];
  std::snprintf(b, sizeof b, "pac\\ch\\ch%03d.pac", id);
  return b;
}
inline std::string ArenaRel(int id) {
  char b[64];
  std::snprintf(b, sizeof b, "pac\\bg\\bg%02d.pac", id);
  return b;
}

// Session 98 (step 16): "pac\\bg\\bg05.pac" -> "bg05.pac"
inline std::string BaseName(const std::string& rel) {
  const size_t at = rel.find_last_of("\\/");
  return at == std::string::npos ? rel : rel.substr(at + 1);
}

inline double NowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

inline bool IsJboy(const Bytes& b) { return b.size() >= 8 && std::memcmp(b.data(), "JBOY", 4) == 0; }

// Digits-only id from "<prefix>NNN.pac" (exactly `digits` digits), else -1.
inline int ParseId(const std::string& name, const char* prefix, size_t digits) {
  const std::string pre(prefix);
  if (name.size() != pre.size() + digits + 4) return -1;
  if (Lower(name.substr(0, pre.size())) != Lower(pre) || Lower(name.substr(name.size() - 4)) != ".pac") return -1;
  int v = 0;
  for (size_t i = 0; i < digits; ++i) {
    const char c = name[pre.size() + i];
    if (c < '0' || c > '9') return -1;
    v = v * 10 + (c - '0');
  }
  return v;
}

}  // namespace load_detail

inline std::vector<int> ListWrestlerIds(launcher::GameFiles& files) {
  std::vector<int> ids;
  for (const std::string& n : files.List("pac\\ch")) {
    const int id = load_detail::ParseId(n, "ch", 3);
    if (id >= 0) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

inline std::vector<int> ListArenaIds(launcher::GameFiles& files) {
  std::vector<int> ids;
  for (const std::string& n : files.List("pac\\bg")) {
    const int id = load_detail::ParseId(n, "bg", 2);
    if (id >= 0) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

namespace load_detail {
inline int CountAttires(const pac::Pac& p) {
  int n = 0;
  for (const pac::Pach& h : p.pachs)
    if (pac::Find(h, 0)) ++n;
  return n;
}
}  // namespace load_detail

namespace load_detail {
// Session 98 (generalisation, probe run 6819): the model a wrestler wears in his ENTRANCE, from the pac's own directory. The EPAC header
// holds at 0x800 "EMD " and 16-byte entries from 0x80C (8-char name, LE32 start in 2 KB sectors from 0x4000, LE32 size in 256-byte
// units: the same directory as the motion EPK8s). A ch pac names each model PACH "000NNNAK": NNN the character, A the game's attire,
// K the kind: 2 = the ring model, 4 = the entrance model [C: ch161 "1610 2 / 1610 4 / 1611 2" -> Orton's entrance = PACH 1 (run 6816);
// ch324 "3240 2 / 3240 4" -> Bryan's entrance = PACH 1 (run 6819, 15 / 15 vertex counts); ch100 "1000 2 / 1001 2 / 1002 2" (no
// entrance model) -> The Rock enters in PACH 0, his ring model (run 6819, 27 / 30; PACH 1 = his second attire, 23 / 30)].
// -> the viewer's attire index (the n-th PACH holding a model) of attire `game_attire`'s entrance model, else its ring model; -1 when
// the directory is missing or does not name it (a community pac: the caller keeps its old rule).
inline int EntranceModelIndex(const pac::Pac& p, int game_attire) {
  const Bytes& b = p.bytes;
  if (b.size() < 0x4000 || std::memcmp(b.data() + 0x800, "EMD ", 4) != 0) return -1;
  int best = -1, best_kind = 0;
  for (size_t o = 0x80C; o + 16 <= 0x4000; o += 16) {
    char nm[9] = {0};
    std::memcpy(nm, b.data() + o, 8);
    bool digits = true;
    for (int i = 0; i < 8; ++i) digits = digits && nm[i] >= '0' && nm[i] <= '9';
    if (!digits) break;
    const int attire = nm[6] - '0', kind = nm[7] - '0';
    if (attire != game_attire || (kind != 2 && kind != 4) || kind <= best_kind) continue;
    const size_t base = 0x4000 + size_t(Le32(b, o + 8)) * 0x800;
    int idx = 0;
    for (const pac::Pach& h : p.pachs) {
      if (h.base == base) {
        if (pac::Find(h, 0)) best = idx, best_kind = kind;
        break;
      }
      if (pac::Find(h, 0)) ++idx;
    }
  }
  return best;
}
// Session 98 (attire props): the directory name of the model PACH that is the viewer's attire index `viewer_attire` (the n-th PACH
// holding a model), as a number: "00013904" -> 13904 = character * 100 + game attire * 10 + kind (2 ring / 4 entrance) = the key of
// the game's prop table 101 (evt_props.h). -1 when the directory does not name it.
inline int ModelKeyIn(const pac::Pac& p, int viewer_attire) {
  const Bytes& b = p.bytes;
  if (viewer_attire < 0 || b.size() < 0x4000 || std::memcmp(b.data() + 0x800, "EMD ", 4) != 0) return -1;
  for (size_t o = 0x80C; o + 16 <= 0x4000; o += 16) {
    char nm[9] = {0};
    std::memcpy(nm, b.data() + o, 8);
    bool digits = true;
    for (int i = 0; i < 8; ++i) digits = digits && nm[i] >= '0' && nm[i] <= '9';
    if (!digits) break;
    const size_t base = 0x4000 + size_t(Le32(b, o + 8)) * 0x800;
    int idx = 0;
    for (const pac::Pach& h : p.pachs) {
      if (h.base == base) {
        if (pac::Find(h, 0) && idx == viewer_attire) return std::atoi(nm);
        break;
      }
      if (pac::Find(h, 0)) ++idx;
    }
  }
  return -1;
}
}  // namespace load_detail

inline int ModelKey(launcher::GameFiles& files, int id, int viewer_attire) {
  std::string err;
  const pac::Pac* p = load_detail::OpenCached(files, load_detail::WrestlerRel(id), err);
  return p ? load_detail::ModelKeyIn(*p, viewer_attire) : -1;
}

inline int EntranceAttire(launcher::GameFiles& files, int id, int game_attire) {
  std::string err;
  const pac::Pac* p = load_detail::OpenCached(files, load_detail::WrestlerRel(id), err);
  return p ? load_detail::EntranceModelIndex(*p, game_attire) : -1;
}
inline int EntranceAttireFile(const std::string& path_u8, int game_attire);

// Attires of a wrestler = the PACH archives in his pac that hold a model (entry 0). 0 when the pac cannot be read.
inline int AttireCount(launcher::GameFiles& files, int id) {
  std::string err;
  const pac::Pac* p = load_detail::OpenCached(files, load_detail::WrestlerRel(id), err);
  return p ? load_detail::CountAttires(*p) : 0;
}

// S103 B: the viewer attire ORDINAL (the n-th model PACH, what LoadWrestler takes) of the PACH whose EMD name is `key` (character * 100 + game attire * 10 + kind: the
// inverse of ModelKey). -1 when the pac holds no PACH of that name. A record that names a model by the game's key resolves it here, never by "variant = the attire digit".
inline int OrdinalOfModelKey(launcher::GameFiles& files, int id, int key) {
  const int n = AttireCount(files, id);
  for (int i = 0; i < n; ++i)
    if (ModelKey(files, id, i) == key) return i;
  return -1;
}

inline bool LoadWrestlerPac(const pac::Pac* p, const std::string& label, const std::string& pacname, int attire, Model& m, std::string& err,
                            bool prop_slots = false);  // (Session 99: prop_slots = also the PACH's model slots 1..9, LoadProp)

namespace load_detail {
// Session 98 (wrestler fix 3): the texture directories every wrestler shares (pac\ch\chCommon.pac: `noise2`, the gear shader's tiled
// detail texture g_mNoiseTilePrm, lives there and in no ch pac [C: texlist of ch161 / chCommon]). Read once, decoded; a wrestler's
// own directories come first, these are the fallback. Empty until LoadWrestler has run once with the game files (a custom pac loaded
// first gets them from the next load on).
inline std::vector<Bytes>& CommonTexDirs() {
  static std::vector<Bytes> d;
  return d;
}
inline void LoadCommonTexDirs(launcher::GameFiles& files) {
  static bool tried = false;
  if (tried) return;
  tried = true;
  pac::Pac p;
  std::string e;
  if (!pac::Open(files, "pac\\ch\\chCommon.pac", p, e)) return;
  for (const pac::Pach& h : p.pachs)
    for (const pac::Ent& en : h.ents)
      if (en.id >= 10 && en.id <= 29 && en.size >= 0x30) CommonTexDirs().push_back(pac::Decode(p.bytes.data() + en.off, en.size));
}
}  // namespace load_detail

inline bool LoadWrestler(launcher::GameFiles& files, int id, int attire, Model& m, std::string& err) {
  using namespace load_detail;
  m = {};
  LoadCommonTexDirs(files);
  char lab[64];
  std::snprintf(lab, sizeof lab, "wrestler %d attire %d", id, attire);
  const pac::Pac* p = OpenCached(files, WrestlerRel(id), err);
  if (!p) return false;
  return LoadWrestlerPac(p, lab, BaseName(WrestlerRel(id)), attire, m, err);
}

// Session 98 (gen check 4): an event PROP of pac\evt\evtobj.pac: PACH `pach` (entry 0 = the JBOY, 10 = its textures). The title belts
// are PACHs 349-366+ (yProp_BumpMap, a shared 782-vertex strap; 36 nodes 'root', 'bone01'..): probe run 6820 drew Cena's WWE
// Championship as exactly PACH 352's three meshes (650 / 592 / 782 vertices at stride 44, VS 1063A665 / 0D7075D5).
namespace load_detail {
// Session 98 (DLC): a DLC pack's evtobj_dlcPP.pac (small: 37 KB - 2.7 MB), kept apart from OpenCached's one slot so a DLC prop never
// evicts the disc's 160 MB evtobj.pac. Read once per file; nullptr when the file is missing or unreadable.
struct DlcPropState {
  std::map<std::string, pac::Pac> cache;
  std::map<std::string, std::string> failed;
  bool releasable = false;   // S103 BLF: set by the live game's loader only (live_assets.cpp): the viewer never sets it, so ReleaseEvtobj never frees under the viewer
  int releases = 0;
};
inline DlcPropState& DlcProps() {
  static DlcPropState s;
  return s;
}
inline const pac::Pac* OpenDlcProps(launcher::GameFiles& files, const std::string& rel, std::string& err) {
  DlcPropState& st = DlcProps();
  if (auto it = st.cache.find(rel); it != st.cache.end()) return &it->second;
  if (auto it = st.failed.find(rel); it != st.failed.end()) return err = it->second, nullptr;
  pac::Pac p;
  if (!pac::Open(files, rel, p, err)) return st.failed[rel] = err, nullptr;
  return &(st.cache[rel] = std::move(p));
}
// S103 BLF (decision 2026-11-19): the live game frees the disc evtobj.pac's unpack (~160 MB) when its last event prop model is retired (the entrance is over); the next entrance prop reopens it. Holds the loader lock, so it never
// frees under a load in flight; does nothing unless the live loader marked the slot releasable (the viewer shares OpenPropFile and keeps its pac). Returns true when a pac was closed.
// S103 GBR (BH4 item 4): called on the render thread; the loader's lock can be held for seconds by a worker: try_lock, and `busy` = true when it could not be taken (the caller retries next frame). Never blocks.
inline bool ReleaseEvtobj(bool* busy = nullptr) {
  std::unique_lock<std::recursive_mutex> lk(LoadMutex(), std::try_to_lock);
  if (busy) *busy = !lk.owns_lock();
  if (!lk.owns_lock()) return false;
  DlcPropState& st = DlcProps();
  if (!st.releasable) return false;
  const bool had = st.cache.erase("pac\\evt\\evtobj.pac") != 0;
  if (had) ++st.releases;
  return had;
}
inline const pac::Pac* OpenPropFile(launcher::GameFiles& files, int pach, std::string& err) {
  const std::string rel = dlc_files::PropFile(pach);
  return OpenDlcProps(files, rel, err);   // S103 BLF review 3: the disc evtobj.pac (160 MB) has its OWN persistent slot too: the one OpenCached slot thrashed against the wrestler ch pacs (every miss re-unpacked 160 MB under the loader lock)
}
}  // namespace load_detail

// Session 98 (DLC): `pach` >= dlc_files::kPropPackStride names PACH (pach % stride) of pack (pach / stride)'s evtobj_dlcPP.pac.
inline bool LoadProp(launcher::GameFiles& files, int pach, Model& m, std::string& err) {
  using namespace load_detail;
  m = {};
  const std::string name = BaseName(dlc_files::PropFile(pach));
  const pac::Pac* p = pach < 0 ? nullptr : OpenPropFile(files, pach, err);
  if (!p) return pach < 0 ? (err = "prop PACH must be >= 0", false) : false;
  const int at = dlc_files::PropIndex(pach);
  if (size_t(at) >= p->pachs.size() || !pac::Find(p->pachs[size_t(at)], 0))
    return err = name + " PACH " + std::to_string(at) + " holds no model", false;
  int index = 0;  // LoadWrestlerPac's "attire" = the ordinal among the PACHs that hold an entry 0
  for (int i = 0; i < at; ++i)
    if (pac::Find(p->pachs[size_t(i)], 0)) ++index;
  char lab[64];
  std::snprintf(lab, sizeof lab, "prop %d", pach);
  // (Session 99 review) the title belts (the pac directory's "BOBJ" section: 154 of evtobj.pac's PACHs, 349-366, 451-460 ..) keep slot 0
  // only: their entry 4 is one 20-vertex piece bound to `root` alone (a plate at the bind pose) that would not follow the posed strap;
  // the game's draw of Cena's 352 (no entry 4) is the only belt reference [U whether the game draws a belt's entry 4].
  bool belt = false;
  if (p->bytes.size() >= 0x4000 && std::memcmp(p->bytes.data(), "EPAC", 4) == 0) {
    std::string sec(reinterpret_cast<const char*>(p->bytes.data() + 0x800), 4);
    for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
      const uint8_t* rr = p->bytes.data() + o;
      const uint32_t st = Le32(p->bytes, o + 4);
      if (!(std::isdigit(rr[0]) && std::isdigit(rr[1]) && std::isdigit(rr[2]) && std::isdigit(rr[3]))) {
        if (rr[0] == 0 && st == 0) break;
        if (std::memcmp(rr, "LIST", 4) != 0) sec.assign(reinterpret_cast<const char*>(rr), 4);
        continue;
      }
      if (0x4000 + size_t(st) * 0x800 == p->pachs[size_t(at)].base) { belt = sec == "BOBJ"; break; }
    }
  }
  return LoadWrestlerPac(p, lab, name, index, m, err, !belt);  // (Session 99: + model slots 1..9, not for belts)
}

// S103 BLF (docs\research\s103\BELT.md Adversary 1; docs\formats\evtobj_pac.md): ONE model of an entrance's event prop, for the POSED draw: entry `k` (0..9) of evtobj.pac PACH `pach` (DLC numbering as LoadProp). MakeWeapon
// sub_8272D710 builds one CSkinModel per present entry id k of the prop's PACH (loop 0x8272DEAC..0x8272EA58), each posed by its own palette, so the live drawer loads each entry as its own model (LoadProp merges slots 1..9 into
// one, which only a single palette could pose). Read like LoadM2Weapon's: arena stream layout, each mesh keeps its palette -> node map (Mesh::palette_nodes / rigid_node / src_skin_*) and the vertices stay in the viewer's
// space (FinishModel), NO placement: the call's node rows pose it (weapon_draw_core.h). Textures: entries 10..19 of the same PACH (the game's key 10 [C: LoadProp's read]).
inline bool LoadPropEntry(launcher::GameFiles& files, int pach, uint32_t k, Model& m, std::string& err) {
  using namespace load_detail;
  m = {};
  if (pach < 0) return err = "prop PACH must be >= 0", false;
  if (k > 9) return err = "prop entry above 9", false;
  const pac::Pac* p = OpenPropFile(files, pach, err);
  if (!p) return false;
  const int at = dlc_files::PropIndex(pach);
  const std::string name = BaseName(dlc_files::PropFile(pach));
  if (size_t(at) >= p->pachs.size()) return err = name + " has no PACH " + std::to_string(at), false;
  const pac::Pach& h = p->pachs[size_t(at)];
  const pac::Ent* e = pac::Find(h, k);
  if (!e) return err = name + " PACH " + std::to_string(at) + " has no entry " + std::to_string(k), false;
  const Bytes blob = pac::Decode(p->bytes.data() + e->off, e->size);
  if (!IsJboy(blob)) return err = name + " PACH " + std::to_string(at) + " entry " + std::to_string(k) + " is not a JBOY", false;
  TexSet tex;
  for (const pac::Ent& t : h.ents)
    if (((t.id >= 10 && t.id <= 19) || t.id == 20) && t.size >= 0x30) tex.AddDir(pac::Decode(p->bytes.data() + t.off, t.size));   // S103 RFL: + entry 20, the PACH's second texture directory (the reflection cubes: cube_ref / cube_01 ...; census: every named cube of a prop is found there; the m2 weapons read 10 + 20 the same way, LoadM2Weapon)
  JboyOptions o;
  o.arena = true;
  o.prop_slot = k >= 1;   // (the slots' cut-out materials are alpha-blended: LoadWrestlerPac's rule for prop slots)
  o.part = "prop " + std::to_string(pach) + "/" + std::to_string(k);
  o.kind = 4, o.key = pach * 16 + int(k), o.pac = name;
  o.tex = &tex;
  m.label = "prop " + std::to_string(pach) + " entry " + std::to_string(k);
  if (!LoadJboy(blob, o, m, err)) return false;
  FinishModel(m);
  if (m.meshes.empty()) return err = m.label + ": no meshes loaded", false;
  return true;
}

// S103 PW-3 (docs\research\s103\PW_adversary.md 4a, 4b): a WEAPON / prop of the match's weapon manager, from the player's own pac\m2.pac: top-level PACH 0, nested key 1000, nested key `id` = the weapon id
// (the game's own path: sub_82582D08 finds key 1000, sub_8258FF90 finds key = id [C]); entry 0 = the JBOY model, textures = entries 10..19 of that leaf (the game's key 10; the same read as
// prop_game_space.cpp AddSource). Read like the arena loader's props (AddProp): arena stream layout, each mesh keeps its palette -> node map (Mesh::palette_nodes / rigid_node), vertices in the viewer's space (FinishModel),
// NO placement: the call's node rows pose it (src\renderer_scene\weapon_draw_core.h). Id 14 (the belts) is nested one level deeper under a sub-key the game takes from [0x836F2D88] [L]: refused here, counted by the drawer.
// `model_entry` = which leaf entry is the model: 0 = the weapon's main model, 1 / 2 = its SECONDARY models k = 0 / 1 (the game's keys 1 / 2 of the weapon's PACH, sub_8258FF90 loop k < 2 [C]; the k -> key k+1 order [L]; m2 p0 has them for id 89 only);
// a secondary shares the leaf's textures (key 10 [L]). Textures: key 10 and key 20 (a second texture directory in ids 29-37, read with key 10 by MakeSkinModel sub_8258E270 [C]; same directory layout as key 10, 12464 bytes [C census]).
// `tex_first` = the first texture entry read (10 = the game's); the offline unit plants 11 to prove the key matters. Opens m2.pac (31 MB) itself, per call: the callers cache the model by id, so each id is read once.
inline bool LoadM2Weapon(launcher::GameFiles& files, int id, Model& m, std::string& err, uint32_t tex_first = 10, uint32_t model_entry = 0, int belt_sub = -1) {   // S103 LDR: belt_sub = id 14's sub-key (the title's belt; the game falls back to 0 for a key the pac lacks)
  using namespace load_detail;
  m = {};
  if (model_entry > 2) return err = "m2 weapon: model entry above 2", false;
  if (id < 0) return err = "m2 weapon: negative id", false;
  if (id == 14 && belt_sub < 0) return err = "m2 weapon 14 (the belts) is nested one level deeper under the title's sub-key: no sub-key given, not loaded", false;
  pac::Pac p;
  if (!pac::Open(files, "pac\\m2.pac", p, err)) return false;
  Bytes b1, nb;
  pac::Pach p1, np;
  // S103 LDR (COV10 C2; docs\formats\props_coverage.md section 2): the game picks the weapon set by the match's rule type (sub_8258CBB0): the normal set (m2 p0 = bgEtc /STG/WPON) or the backstage-brawl set (m2 p1 = /STG/WPBS). Ids 17, 51-58, 61,
  // 62, 64, 68, 69 (box, cart, laptop, TV, cases, bottle) exist ONLY in p1; every id in both sets has equal leaf sizes [C census COV3 row 5]. So the lookup is p0, then p1: an id the player's match does not ask for is never looked up.
  if (p.pachs.empty()) return err = "m2.pac has no PACH", false;
  bool found = false;
  for (size_t pi = 0; pi < p.pachs.size() && pi < 2 && !found; ++pi)
    found = pac::GetNested(p.bytes, p.pachs[pi], 1000u, b1, p1) && pac::GetNested(b1, p1, uint32_t(id), nb, np);
  if (!found) return err = "m2.pac holds no p0/1000/" + std::to_string(id) + " (nor p1: the brawl set)", false;
  if (id == 14) {   // S103 LDR (COV10 B2): p0/1000/14/<sub-key>/{0 model, 10 textures} [C pac.py]; a key the pac lacks = sub-key 0 (SetWeaponData 0x82590134)
    Bytes nb2;
    pac::Pach np2;
    if (!pac::GetNested(nb, np, uint32_t(belt_sub), nb2, np2) && !pac::GetNested(nb, np, 0u, nb2, np2)) return err = "m2.pac p0/1000/14 holds no sub-key " + std::to_string(belt_sub) + " (nor 0)", false;
    nb = std::move(nb2), np = std::move(np2);
  }
  const Bytes blob = pac::Get(nb, np, model_entry);
  if (!IsJboy(blob)) return err = "m2.pac p0/1000/" + std::to_string(id) + ": entry " + std::to_string(model_entry) + " is not a JBOY", false;
  TexSet tex;
  for (const pac::Ent& e : np.ents)
    if (((e.id >= tex_first && e.id <= 19) || (e.id == 20 && tex_first <= 10)) && e.size >= 0x30) tex.AddDir(pac::Decode(nb.data() + e.off, e.size));
  JboyOptions o;
  o.arena = true;
  o.part = "weapon " + std::to_string(id) + (model_entry ? "/" + std::to_string(model_entry) : std::string());
  o.kind = 4, o.key = id == 14 ? 14000 + belt_sub : id, o.pac = "m2.pac";
  o.tex = &tex;
  m.label = "m2 weapon " + std::to_string(id) + (model_entry ? "/" + std::to_string(model_entry) : std::string());
  if (!LoadJboy(blob, o, m, err)) return false;
  FinishModel(m);
  if (m.meshes.empty()) return err = m.label + ": no meshes loaded", false;
  return true;
}

// S103 CGF (docs\research\s103\CAGE.md section 1 / 3, docs\formats\bgetc_stg_structures.md): ONE model of a STAGE STRUCTURE (steel cage CAGE / CABU / CABA, Hell in a Cell, Elimination Chamber, Inferno) from the player's own
// pac\bg\bgEtc.pac: top-level PACH `pach` (the game's path row: 0 CAGE, 1 CABU, 2 CABA, 3 CELL, 6 CHAI, 7 INFE), nested key 1 = the models, entry `k` = the model the holder's slot [H+76+4k] was made from (JBOY); textures = the PACH's
// top-level entries 0 and 10..19 (the game's [H] texture set, sub_82577300). Read like LoadPropEntry's: arena stream layout, each mesh keeps its palette -> node map, vertices in the viewer's space (FinishModel), NO placement (the
// call's node rows pose it) and NOT the prop-slot alpha rule (k is a structure part here, not a cut-out slot). bgEtc is opened through OpenProps (the arena loader's static copy: already open in a match, never a second 100+ MB unpack).
namespace load_detail {
inline const pac::Pac* OpenProps(launcher::GameFiles& files);   // defined below (the arena loader's static bgEtc copy)
}
inline bool LoadStageEntry(launcher::GameFiles& files, int pach, uint32_t k, Model& m, std::string& err) {
  using namespace load_detail;
  m = {};
  if (pach < 0 || pach > 61) return err = "stage structure PACH must be 0..61", false;   // S103 LDR: 0..7 the cage / cell / chamber / Inferno, 4 / 5 / 11..61 the ladder rigs (LADM / LADW / the per-title /STGL rows)
  if (k > 31) return err = "stage structure entry above 31", false;
  const pac::Pac* p = OpenProps(files);
  if (!p || size_t(pach) >= p->pachs.size()) return err = "pac\\bg\\bgEtc.pac is not open or has no PACH " + std::to_string(pach), false;
  const pac::Pach& h = p->pachs[size_t(pach)];
  Bytes nb;
  pac::Pach np;
  if (!pac::GetNested(p->bytes, h, 1, nb, np)) return err = "bgEtc PACH " + std::to_string(pach) + " holds no nested key 1", false;
  const Bytes blob = pac::Get(nb, np, k);
  if (!IsJboy(blob)) return err = "bgEtc PACH " + std::to_string(pach) + " key 1 entry " + std::to_string(k) + " is not a JBOY", false;
  TexSet tex;
  for (const pac::Ent& t : h.ents)
    if ((t.id == 0 || (t.id >= 10 && t.id <= 19)) && t.size >= 0x30) tex.AddDir(pac::Decode(p->bytes.data() + t.off, t.size));
  JboyOptions o;
  o.arena = true;
  o.prop_slot = false;
  o.part = "stage " + std::to_string(pach) + "/" + std::to_string(k);
  o.kind = 4, o.key = 4000 + pach * 32 + int(k), o.pac = "bgEtc.pac";
  o.tex = &tex;
  m.label = "stage structure " + std::to_string(pach) + " entry " + std::to_string(k);
  if (!LoadJboy(blob, o, m, err)) return false;
  FinishModel(m);
  if (m.meshes.empty()) return err = m.label + ": no meshes loaded", false;
  return true;
}

// S103 GBR (docs\research\s103\GMS.md (A); docs\formats\gm_gmev.md "GMGA / GMGB model banks"): the GMSK set objects' models, from the player's own pac\gm.pac. The bank is the arena's (the game's path rule sub_827C7038: type 2 + arena
// id -> /GMGA/%04d, arena 78 / 80 -> /GMGB/%02dFR). The EPAC directory (header 0x800: records of {name[4], start (0x800 blocks from 0x4000), size}) names the banks: GMGB's are named "78FR" / "80FR", which the digit-only reader of
// tools\assets\prtc.py takes for section names, so the directory is read here by name. A bank is a PACH whose entry 4 is a nested PACH of JBOY models (keys 1000.. / 1500..); a GMGB (FR) bank wraps it: entry 0 = the bank, entry 1 =
// the texture set. Textures: FR = the bank's entry-4 key 0 (a texture directory) + every directory of the FR bank's entry 1 (each entry a PACH {PRTC, directory}); GMGA 0021 (the Elimination Chamber's pieces) = the chamber's own
// bgEtc PACH 6 set, the one its structure uses (entries 0 and 10..19; [L]: the game hands the holder's group [[0x837631C4]] when its settings byte +41 is set).
namespace load_detail {
inline const pac::Pac* OpenGm(launcher::GameFiles& files) {   // gm.pac is 18 MB unpacked: opened once per program on the first set object asked for
  static pac::Pac gm;
  static int state = 0;  // 0 = not tried, 1 = open, 2 = failed
  if (state == 0) {
    std::string e;
    state = pac::Open(files, "pac\\gm.pac", gm, e) ? 1 : 2;
  }
  return state == 1 ? &gm : nullptr;
}
// The byte offset of arena `arena`'s bank in the unpacked gm.pac (0 = none) and whether it is an FR (GMGB) bank. Pure: the unit runs it on a made-up directory.
inline size_t GmBankOffset(const Bytes& b, int arena, bool& fr) {
  fr = false;
  if (b.size() < 0x4000 || arena < 0 || arena > 999) return 0;
  char want_fr[8];
  std::snprintf(want_fr, sizeof want_fr, "%02dFR", arena);
  std::string sec = std::string(reinterpret_cast<const char*>(b.data() + 0x800), 4);
  for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
    const std::string nm(reinterpret_cast<const char*>(b.data() + o), 4);
    const uint32_t st = Le32(b, o + 4);
    if (nm == std::string(4, '\0') && st == 0) break;
    const bool digits = std::all_of(nm.begin(), nm.end(), [](char c) { return c >= '0' && c <= '9'; });
    const bool fr_name = nm.size() == 4 && nm[2] == 'F' && nm[3] == 'R' && nm[0] >= '0' && nm[0] <= '9' && nm[1] >= '0' && nm[1] <= '9';
    if (!digits && !fr_name) { sec = nm; continue; }   // a section header
    if (sec == "GMGB" && fr_name && nm == want_fr) return fr = true, size_t(0x4000) + size_t(st) * 0x800;
    if (sec == "GMGA" && digits && std::atoi(nm.c_str()) == arena && !(arena == 78 || arena == 80)) return size_t(0x4000) + size_t(st) * 0x800;
  }
  return 0;
}
}  // namespace load_detail
inline bool LoadGmEntry(launcher::GameFiles& files, int arena, uint32_t key, Model& m, std::string& err) {
  using namespace load_detail;
  m = {};
  if (key < 1000 || key > 1755) return err = "gm set object key outside 1000..1755", false;
  const pac::Pac* p = OpenGm(files);
  if (!p) return err = "pac\\gm.pac is not open", false;
  bool fr = false;
  const size_t at = GmBankOffset(p->bytes, arena, fr);
  if (at == 0) return err = "gm.pac has no GMGA / GMGB bank for arena " + std::to_string(arena), false;
  pac::Pach top;
  if (!pac::ParsePach(p->bytes, at, p->bytes.size(), top)) return err = "gm.pac bank of arena " + std::to_string(arena) + " is not a PACH", false;
  Bytes bank_buf;
  pac::Pach bank;
  const Bytes* bbuf = &p->bytes;
  const pac::Pach* bp = &top;
  if (fr) {   // FR: entry 0 = the bank itself
    if (!pac::GetNested(p->bytes, top, 0, bank_buf, bank)) return err = "gm.pac FR bank entry 0 is not a PACH", false;
    bbuf = &bank_buf, bp = &bank;
  }
  Bytes mb;
  pac::Pach mp;
  if (!pac::GetNested(*bbuf, *bp, 4, mb, mp)) return err = "gm.pac bank holds no models (entry 4)", false;
  const Bytes blob = pac::Get(mb, mp, key);
  if (!IsJboy(blob)) return err = "gm.pac bank of arena " + std::to_string(arena) + " key " + std::to_string(key) + " is not a JBOY", false;
  TexSet tex;
  if (fr) {
    tex.AddDir(pac::Get(mb, mp, 0));   // the name table is a texture directory
    Bytes tb;
    pac::Pach tp;
    if (pac::GetNested(p->bytes, top, 1, tb, tp))
      for (const pac::Ent& e : tp.ents) {
        Bytes sb;
        pac::Pach sp;
        if (!pac::GetNested(tb, tp, e.id, sb, sp)) continue;
        if (const pac::Ent* d = pac::Find(sp, 1)) tex.AddDir(pac::Decode(sb.data() + d->off, d->size));
      }
  } else if (const pac::Pac* ep = OpenProps(files); ep && ep->pachs.size() > 6) {   // the chamber's own bgEtc PACH 6 textures (LoadStageEntry's set)
    for (const pac::Ent& t : ep->pachs[6].ents)
      if ((t.id == 0 || (t.id >= 10 && t.id <= 19)) && t.size >= 0x30) tex.AddDir(pac::Decode(ep->bytes.data() + t.off, t.size));
  }
  JboyOptions o;
  o.arena = true;
  o.prop_slot = false;
  o.part = "gm " + std::to_string(arena) + "/" + std::to_string(key);
  o.kind = 4, o.key = int(key), o.pac = "gm.pac";
  o.tex = &tex;
  m.label = "gm set object " + std::to_string(arena) + "/" + std::to_string(key);
  if (!LoadJboy(blob, o, m, err)) return false;
  FinishModel(m);
  if (m.meshes.empty()) return err = m.label + ": no meshes loaded", false;
  return true;
}

// S103 GBR (GMS.md (B)): the blood splat model of arena `arena`: its own bgNN.pac, top-level PACH 0 key 32000 (one JBOY, material `blood`) with the texture directory key 32010 (blood01 / 02 / 03, in the directory's order = the
// game's surface list [P+4] [L: the list is built from the directory in order]). The game binds texture `tex` per splat (model slot 15); here each texture index is its own model: every material's diffuse is that directory
// entry (the file's own texture name is not in the directory). The colour scalar is applied by the drawer (diffuse_color).
inline bool LoadBloodEntry(launcher::GameFiles& files, int arena, uint32_t tex_index, Model& m, std::string& err) {
  using namespace load_detail;
  m = {};
  std::string rel = ArenaRel(arena);
  const pac::Pac* p = OpenCached(files, rel, err);
  if (!p) return false;
  const pac::Pach& h = p->pachs[0];
  const Bytes blob = pac::Get(p->bytes, h, 32000);
  if (!IsJboy(blob)) return err = rel + " has no blood model (key 32000)", false;
  const Bytes dir = pac::Get(p->bytes, h, 32010);
  TexSet tex;
  Bytes dcopy = dir;
  if (!tex.AddDir(std::move(dcopy))) return err = rel + " key 32010 is not a texture directory", false;
  const uint32_t count = dir.size() >= 4 ? Le32(dir, 0) : 0;
  if (tex_index >= count) return err = rel + " blood texture index " + std::to_string(tex_index) + " is past the directory's " + std::to_string(count), false;
  char nm[17] = {};
  std::memcpy(nm, dir.data() + 0x10 + size_t(tex_index) * 32, 16);
  JboyOptions o;
  o.arena = true;
  o.prop_slot = false;
  o.part = "blood " + std::to_string(arena);
  o.kind = 4, o.key = 32000, o.pac = BaseName(rel);
  o.tex = &tex;
  m.label = "blood splat " + std::to_string(arena) + "/" + std::to_string(tex_index);
  if (!LoadJboy(blob, o, m, err)) return false;
  Texture t;
  std::string why;
  if (!tex.Peek(nm, t, why)) return err = m.label + ": texture " + nm + ": " + why, false;
  m.textures.clear();
  m.textures.push_back(std::move(t));
  for (Material& mat : m.materials) mat.diffuse = 0;
  FinishModel(m);
  if (m.meshes.empty()) return err = m.label + ": no meshes loaded", false;
  return true;
}

// Session 98 (gen check 6): the evtobj.pac PACH of entrance prop id `prop_id` (entrance.h PropAttach): the first PACH whose model (entry 0)
// has a node named "<id>" or "<id>_..." (2774 -> 231, 3038 -> 301 "3038_wwe_mic_02"). -1: none. The answer is cached per id.
// Session 98 (DLC): then each DLC pack's evtobj_dlcPP.pac in the game's pack order (dlc_files.h): Fandango's 2591 / 2592 live in
// evtobj_dlc04.pac PACHs 0 / 1 (nodes "2591_0", "2592"), numbered 400000 / 400001. The cache key includes the packs in use.
inline int PropPachIn(const pac::Pac* p, uint32_t prop_id);
inline int PropPach(launcher::GameFiles& files, uint32_t prop_id) {
  using namespace load_detail;
  static std::map<std::pair<std::vector<int>, uint32_t>, int> cache;
  const std::vector<int> packs = dlc_files::PackIds(files);
  if (auto it = cache.find({packs, prop_id}); it != cache.end()) return it->second;
  std::string err;
  int found = -1;
  if (const pac::Pac* p = OpenDlcProps(files, "pac\\evt\\evtobj.pac", err)) found = PropPachIn(p, prop_id);   // (S103 BLF review 3: the evtobj slot)
  for (size_t k = 0; found < 0 && k < packs.size(); ++k) {
    if (packs[k] <= 0 || packs[k] >= 21000) continue;  // (the PACH number must fit an int)
    const int at = PropPachIn(OpenDlcProps(files, dlc_files::PropFile(packs[k] * dlc_files::kPropPackStride), err), prop_id);
    if (at >= 0) found = packs[k] * dlc_files::kPropPackStride + at;
  }
  cache[{packs, prop_id}] = found;
  return found;
}
// Session 98 (cap / particles): the game finds an entrance prop by NAME in the pac's own directory, not by a model node: the 16 KB EPAC
// header of evtobj.pac lists 12-byte records from +0x80C (char[4] name, LE32 start in 2 KB sectors from +0x4000, LE32 size in 256-byte
// units); a record whose name is not 4 digits opens a section: "SOBJ" (named at +0x800; its first record is "LIST", the 64-byte
// id + name table), "BOBJ" (the title belts 2009 / 3009 / 5009 / ..), "SPMT" (a second, small entry per cap / hat id: 2718-2721, 2799,
// 2800, 2414, 2647, 2762 ..: [U] physics or spring data, not a model). The game's paths are "/SOBJ/%04d" then "/BOBJ/%04d" (strings at
// 0x8201EAA0 / 0x825AE780; sub_825AE5D8 resolves an act's id against SOBJ, sub_825AE720 against BOBJ) [C: the strings and callers].
// The directory names every no-model prop of the props-general census: 2718 -> PACH 179 / 2719 -> 180 / 2720 -> 181 / 2721 -> 182 (Cena's
// caps, node "hat"), 2799 / 2800 -> 256 / 257 (caps, "35604cap"), 2081 / 3081 -> 22 / 304 (caps), 2253 -> 52 (druid), 2647 -> 125 (scarf),
// 2653 -> 129, 2074 -> 20, 2414 -> 70, 2762 -> 220, 2717 -> 178 (its model is entry 5: no entry 0, still skipped) [C: the directory;
// run 6824's s0_ring capture draws PACH 181's mesh (first vertex position byte-identical, 755 vertices, 1985 strip indices) on Cena's
// head]. Returns the PACH index in `p`, -1 when the pac has no such directory record (DLC pacs without a header: the node rule below).
// (attire props, review) `bobj_only`: only the "BOBJ" section (sub_8230B4C0's precheck sub_82310000 -> sub_825AE680 "/BOBJ/%04d").
inline int PropDirIn(const pac::Pac* p, uint32_t prop_id, bool bobj_only = false, bool require_entry0 = true, bool sobj_only = false) {
  if (!p || prop_id > 9999 || p->bytes.size() < 0x4000) return -1;
  const Bytes& b = p->bytes;
  if (std::memcmp(b.data(), "EPAC", 4) != 0 && std::memcmp(b.data(), "EPK8", 4) != 0) return -1;
  char want[8];
  std::snprintf(want, sizeof want, "%04u", unsigned(prop_id));
  auto is_id = [](const uint8_t* s) { return std::isdigit(s[0]) && std::isdigit(s[1]) && std::isdigit(s[2]) && std::isdigit(s[3]); };
  // (the first section's name sits at +0x800; "LIST" is its first record, not a section)
  for (const char* section : {"SOBJ", "BOBJ"}) {
    if (bobj_only && std::strcmp(section, "BOBJ") != 0) continue;
    if (sobj_only && std::strcmp(section, "SOBJ") != 0) continue;
    std::string cur(reinterpret_cast<const char*>(b.data() + 0x800), 4);
    for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
      const uint8_t* r = b.data() + o;
      const uint32_t st = Le32(b, o + 4);
      if (!is_id(r)) {
        if (r[0] == 0 && st == 0) break;  // the end of the directory
        if (std::memcmp(r, "LIST", 4) != 0) cur.assign(reinterpret_cast<const char*>(r), 4);
        continue;
      }
      if (cur != section || std::memcmp(r, want, 4) != 0) continue;
      const size_t at = 0x4000 + size_t(st) * 0x800;
      for (size_t i = 0; i < p->pachs.size(); ++i)
        if (p->pachs[i].base == at) return (!require_entry0 || pac::Find(p->pachs[i], 0)) ? int(i) : -1;  // (review: 2717's model is entry 5 only: skipped)
      return -1;
    }
  }
  return -1;
}
// The PACH of `p` named `prop_id` in its directory (PropDirIn), else the one whose model has a node "<id>" / "<id>_..", -1 none (p may be
// null).
inline int PropPachIn(const pac::Pac* p, uint32_t prop_id) {
  if (!p) return -1;
  if (const int d = PropDirIn(p, prop_id); d >= 0) return d;
  const std::string id = std::to_string(prop_id);
  int found = -1;
  for (size_t i = 0; i < p->pachs.size() && found < 0; ++i) {
    const pac::Ent* en = pac::Find(p->pachs[i], 0);
    if (!en || en->off + en->size > p->bytes.size()) continue;
    const Bytes blob = pac::Decode(p->bytes.data() + en->off, en->size);
    if (blob.size() < 0x48 || std::memcmp(blob.data(), "JBOY", 4) != 0) continue;
    const uint8_t* b = blob.data() + 8;
    const size_t n = blob.size() - 8;
    if (n < 0x28) continue;
    const uint32_t nnode = Be32(b + 0x18), tab = Be32(b + 0x20);
    if (nnode > 4096 || tab > n || uint64_t(tab) + uint64_t(nnode) * 80 > n) continue;
    for (uint32_t k = 0; k < nnode; ++k) {
      const char* s = reinterpret_cast<const char*>(b + tab + 80ull * k);
      const std::string nm(s, strnlen(s, 16));
      if (nm.compare(0, id.size(), id) == 0 && (nm.size() == id.size() || nm[id.size()] == '_')) { found = int(i); break; }
    }
  }
  return found;
}

// The PACH of event prop `prop_id` for the posed draw (S103 BLF review 2 / 6): the game's own directory order (SOBJ then BOBJ: sub_825AE5D8) WITHOUT the entry-0 test of PropDirIn's default (2717's model is entry 5 only: PACH 178 must
// draw; LoadPropEntry refuses a missing entry k itself), then PropPach's node rule / DLC packs. `*other` = the BOBJ PACH when the id is in BOTH sections with different PACHs (SOBJ wins, as in the game; the caller logs it once), else -1.
inline int PropPachEvt(launcher::GameFiles& files, uint32_t prop_id, int* other = nullptr) {
  using namespace load_detail;
  if (other) *other = -1;
  std::string err;
  if (const pac::Pac* p = OpenDlcProps(files, "pac\\evt\\evtobj.pac", err)) {
    const int s = PropDirIn(p, prop_id, false, false, true), b = PropDirIn(p, prop_id, true, false);
    if (s >= 0 && b >= 0 && s != b && other) *other = b;
    if (s >= 0) return s;
    if (b >= 0) return b;
  }
  return PropPach(files, prop_id);
}

// Session 98 (cap / particles): true when prop PACH `pach` is a WORN item: its model (entry 0) has a node named "hat" or ending in "hat"
// (evtobj 22, 118, 179-182, 244-257, 304 "hat" = ids 2081, 2620, 2718-2721, 2787-2800, 3081; 130 "2658_Jmhat"; LIST names "Cena's Cap",
// "Cena's Hat", "Cena's Cap(Green)" / "(Thuganomics)" - every model with such a node, and only these). An act whose
// cue motion is still at zero (entrance.h kPropZero) plays such a prop on the wearer's head: run 6824 shows Cena's cap on his head in
// cues 5, 10, 15 and 20 of bank 602 while act 2718's base is zero in all four [C: shots 6824_sw_s0_stage / ramp / ringin / ring and the
// s0_ring capture's PACH 181 draw]. [U] the game's own switch (the node name is our rule) and the throw (no non-zero 2718 key in 602).
inline bool PropWorn(launcher::GameFiles& files, int pach) {
  using namespace load_detail;
  static std::map<int, bool> cache;
  if (auto it = cache.find(pach); it != cache.end()) return it->second;
  std::string err;
  bool worn = false;
  const pac::Pac* p = pach < 0 ? nullptr : OpenPropFile(files, pach, err);
  const int at = dlc_files::PropIndex(pach);
  const pac::Ent* en = p && size_t(at) < p->pachs.size() ? pac::Find(p->pachs[size_t(at)], 0) : nullptr;
  if (en && en->off + en->size <= p->bytes.size()) {
    const Bytes blob = pac::Decode(p->bytes.data() + en->off, en->size);
    if (blob.size() >= 0x48 && std::memcmp(blob.data(), "JBOY", 4) == 0) {
      const uint8_t* b = blob.data() + 8;
      const size_t n = blob.size() - 8;
      const uint32_t nnode = Be32(b + 0x18), tab = Be32(b + 0x20);
      if (nnode <= 4096 && tab <= n && uint64_t(tab) + uint64_t(nnode) * 80 <= n)
        for (uint32_t k = 0; k < nnode && !worn; ++k) {
          std::string nm(reinterpret_cast<const char*>(b + tab + 80ull * k), strnlen(reinterpret_cast<const char*>(b + tab + 80ull * k), 16));
          for (char& ch : nm) ch = char(std::tolower(static_cast<unsigned char>(ch)));
          worn = nm.size() >= 3 && nm.compare(nm.size() - 3, 3, "hat") == 0;
        }
    }
  }
  cache[pach] = worn;
  return worn;
}

// Session 98 (props general): the title belt models come in three runs of evtobj.pac PACHs: LEFT hand 349-398 (nodes 2xxx), RIGHT hand
// 399-450 (3xxx), WAIST 451-502 (5xxx); one title = one PACH in each run with the SAME texture entry (entry 10): 352 / 402 / 454 the WWE
// Championship (New_WWE nodes), 357 / 407 / 459 the generic 2009 / 3009 / 5009 [C: out\s98\census\entrances_06_08_belts.csv, every
// entry-10 twin set]. BeltTwin(pach, side) = that title's PACH in the run of `side` (2 left, 3 right, 5 waist): `pach` itself when it is
// in that run, else the run's PACH whose entry 10 is byte-identical; several (391-395 share one atlas) = the one of the same rank. -1:
// none (e.g. 364, 379, 388, 389, 397 have no twin in some run).
inline int BeltTwin(launcher::GameFiles& files, int pach, int side) {
  using namespace load_detail;
  const int lo = side == 2 ? 349 : side == 3 ? 399 : side == 5 ? 451 : -1, hi = side == 2 ? 398 : side == 3 ? 450 : 502;
  if (lo < 0) return -1;
  if (pach >= lo && pach <= hi) return pach;
  static std::map<std::pair<int, int>, int> cache;
  if (auto it = cache.find({pach, side}); it != cache.end()) return it->second;
  std::string err;
  const pac::Pac* p = OpenDlcProps(files, "pac\\evt\\evtobj.pac", err);
  if (!p || pach < 349 || pach > 502 || size_t(502) >= p->pachs.size()) return -1;
  auto tex = [&](int i) -> Bytes {
    const pac::Ent* en = pac::Find(p->pachs[size_t(i)], 10);
    if (!en || en->off + en->size > p->bytes.size()) return {};
    return Bytes(p->bytes.begin() + std::ptrdiff_t(en->off), p->bytes.begin() + std::ptrdiff_t(en->off + en->size));
  };
  const Bytes mine = tex(pach);
  int found = -1;
  if (!mine.empty()) {
    const int src_lo = pach <= 398 ? 349 : pach <= 450 ? 399 : 451;
    int rank = 0;  // how many earlier PACHs of the source run share the texture
    for (int i = src_lo; i < pach; ++i) if (tex(i) == mine) ++rank;
    std::vector<int> cand;
    for (int i = lo; i <= hi; ++i) if (tex(i) == mine) cand.push_back(i);
    if (!cand.empty()) found = cand[size_t(std::min<int>(rank, int(cand.size()) - 1))];
  }
  cache[{pach, side}] = found;
  return found;
}

// Session 98 (custom characters): a community character pac from an arbitrary file (uncompressed EPAC / EPK8, same PACH layout as a
// game ch pac). The file is untrusted: all reads are bounds-checked; a failure is an error string, never a crash. The unpacked pac of the
// last file is kept (file size + time are the key), so loading every attire reads it once.
namespace load_detail {
struct FileCache {
  std::string key;
  pac::Pac pac;
};
inline FileCache& TheFileCache() {
  static FileCache c;
  return c;
}
inline const pac::Pac* OpenFileCached(const std::string& path_u8, std::string& err) {
  FileCache& c = TheFileCache();
  std::error_code ec;
  const std::filesystem::path fp = std::filesystem::u8path(path_u8);
  const auto sz = std::filesystem::file_size(fp, ec);
  if (ec) return err = "cannot read " + path_u8, nullptr;
  if (sz < 0x4000 || sz > (400ull << 20)) return err = "not a character pac (size " + std::to_string((unsigned long long)sz) + ")", nullptr;
  const std::string key = path_u8 + "|" + std::to_string((unsigned long long)sz) + "|" +
                          std::to_string((long long)std::filesystem::last_write_time(fp, ec).time_since_epoch().count());
  if (c.key == key && !c.pac.pachs.empty()) return &c.pac;
  c.key.clear();
  c.pac = {};
  Bytes raw;
  raw.resize(size_t(sz));
  {
    std::ifstream f(fp, std::ios::binary);
    if (!f || !f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(sz))) return err = "read failed " + path_u8, nullptr;
  }
  if (std::memcmp(raw.data(), "EPAC", 4) != 0 && std::memcmp(raw.data(), "EPK8", 4) != 0 && !wwe2k14::xctd::IsXctd(raw))
    return err = "not a character pac (no EPAC / EPK8 tag)", nullptr;
  if (!pac::OpenBytes(std::move(raw), BaseName(path_u8), c.pac, err)) return nullptr;
  c.key = key;
  return &c.pac;
}
}  // namespace load_detail

inline int AttireCountFile(const std::string& path_u8) {
  std::string err;
  const pac::Pac* p = load_detail::OpenFileCached(path_u8, err);
  return p ? load_detail::CountAttires(*p) : 0;
}

inline int EntranceAttireFile(const std::string& path_u8, int game_attire) {
  std::string err;
  const pac::Pac* p = load_detail::OpenFileCached(path_u8, err);
  return p ? load_detail::EntranceModelIndex(*p, game_attire) : -1;
}

inline bool LoadWrestlerFile(const std::string& path_u8, int label_id, int attire, Model& m, std::string& err) {
  m = {};
  const pac::Pac* p = load_detail::OpenFileCached(path_u8, err);
  if (!p) return false;
  char lab[64];
  std::snprintf(lab, sizeof lab, "wrestler %d attire %d", label_id, attire);
  try {
    return LoadWrestlerPac(p, lab, load_detail::BaseName(path_u8), attire, m, err);
  } catch (const std::exception& e) {
    return err = std::string("character pac is damaged: ") + e.what(), false;
  }
}

inline bool LoadWrestlerPac(const pac::Pac* p, const std::string& label, const std::string& pacname, int attire, Model& m, std::string& err,
                            bool prop_slots) {
  using namespace load_detail;
  const double t0 = NowMs();
  m = {};
  const std::string lab = label;
  m.label = lab;
  // The attire-th PACH that holds a model.
  const pac::Pach* h = nullptr;
  int seen = 0;
  for (const pac::Pach& c : p->pachs)
    if (pac::Find(c, 0) && seen++ == attire) {
      h = &c;
      break;
    }
  if (!h) return err = lab + ": no such attire", false;
  const Bytes& buf = p->bytes;

  // Texture directories: this attire's first, then attire 0's as a fallback (unconfirmed that attires share textures).
  TexSet tex;
  auto add_dirs = [&](const pac::Pach& hh) {
    for (const pac::Ent& e : hh.ents)
      if ((e.id >= 10 && e.id <= 19) && e.size >= 0x30) tex.AddDir(pac::Decode(buf.data() + e.off, e.size));
  };
  add_dirs(*h);
  if (h != &p->pachs[0]) {
    for (const pac::Pach& c : p->pachs)
      if (pac::Find(c, 0)) {
        if (&c != h) add_dirs(c);
        break;
      }
  }
  for (const Bytes& b : CommonTexDirs()) tex.AddDir(Bytes(b));  // (wrestler fix 3: chCommon's shared textures, last)

  // JBOY pieces: 0 (body), 26, 27, and 10000..39999 (hair, hats, extras). Others are not models.
  // Session 99 (scene objects): an event PROP (prop_slots) also has its model slots 1..9: the game's prop maker CWeaponEventSD::MakeWeapon
  // (sub_8272D710, called by the evtobj prop task sub_83391E00) loops entry ids 0..9 of the prop's PACH (0x8272DEAC..0x8272EA58: find
  // the id, skip a missing one, build a model piece) [C]. Fandango's DLC stage set 2591 keeps its moving rig there: entry 0 = the frame +
  // 10 lamp heads (4 meshes, 21 nodes), entry 4 = 59 meshes skinned over 638 of its 659 nodes (yProp_Reflect), entry 5 = 3 meshes on the
  // lamp heads. 96 of evtobj.pac's 529 PACHs have an entry 4, 5, 6 or 9 (60 of them title belts, whose entry 4 is one 20-vertex piece;
  // others 2064_1, 2473_1 / 2473_9, 2042_02).
  // [U] that the game draws every slot at once (slot use after the load is not traced).
  std::vector<uint32_t> ids;
  for (const pac::Ent& e : h->ents)
    if ((!load_detail::SpringNodes() || load_detail::OldEntryFilterPlant()) ? WrestlerEntryIsModelOld(e.id, prop_slots) : WrestlerEntryIsModel(e.id, prop_slots)) ids.push_back(e.id);   // S103 W2B / ENT: the game's own key list (piece_keys.h), not 0 / 26 / 27 / 10000-39999; S103 XLF: with the spring-hair switch OFF the old list too (28 / 29 add nodes the old by-name merge cannot take)
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());  // (duplicate PACH ids: load a piece once)
  int pieces = 0;
  for (uint32_t pid : ids) {
    const pac::Ent* e = pac::Find(*h, pid);
    // Cheap pre-check on a raw entry that is not wrapped: skip non-models without decoding them.
    Bytes blob = pac::Decode(buf.data() + e->off, e->size);
    if (!IsJboy(blob)) {
      if (pid == 0) return err = lab + ": entry 0 is not a JBOY", false;
      continue;
    }
    JboyOptions o;
    o.part = pid == 0 ? "body" : "piece " + std::to_string(pid);
    o.kind = 2, o.key = int(pid), o.pac = pacname;  // Session 98 (step 16)
    o.prop_slot = prop_slots && pid >= 1 && pid <= 9;  // Session 99 (scene objects): JboyOptions::prop_slot
    o.tex = &tex;
    std::string e2;
    if (!LoadJboy(blob, o, m, e2)) {
      if (pid == 0) return err = e2, false;
      m.warnings.push_back("piece " + std::to_string(pid) + " failed: " + e2);
      continue;
    }
    ++pieces;
  }
  FinishModel(m);
  if (m.meshes.empty()) return err = lab + ": no meshes loaded", false;
  m.load_ms = NowMs() - t0;
  return true;
}

namespace load_detail {

// Row-vector helpers in FILE space (-Y up); MirrorY turns a result into viewer space.
inline Matrix RotYFile(float a) {  // column form [[c,0,s],[0,1,0],[-s,0,c]] (Session 97 skeleton reader), stored transposed
  const float c = std::cos(a), s = std::sin(a);
  return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}
inline Matrix TransFile(float x, float y, float z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }

// Session 97: the ring set's pieces sit at one corner / side in the file and the game draws copies (skeleton.md s.5).
// [C] posts: RotY(i * pi/2), i = 0..3 (sub_825CCFE0; the angle step is the float at 0x82000ADC = pi/2), canvas as stored.
// [U] ropes: RotY(s * pi/2) * T(0, y_j, -320) (shape and side distance from the data; the heights are written at run time
// by the rope simulation, not in the pac: ours below are a guess judged against the game census shots), corner covers
// and turnbuckles the same way per corner, skirts RotY(s * pi/2) * T(0, 0, -320) alternating 974 / 973.
// Session 101 lane 275 [C, ring-rows records of the RL1 capture, arena 4, node 2 of each instance = the rest pose; docs\renderer\FILE_CENSUS.md "Lane 275"]: the TURNBUCKLE (952) is NOT the
// post's RotY copy. Its file mesh sits beside the post at (-324..-283, y, -322..-318); the game's world for corner 0 is RotY(-45 deg) with the translation (-310.015, y, 142.534), so the pad
// swings onto the corner diagonal and its strap reaches the post (before this, T(0, y, 0) * RotY(corner) left it floating beside the post). Heights y = -154 / -196 / -238 (node 2),
// the other three corners = RotY(i * pi/2) after it (checked against all 12 instances). The cover (964) world is a pure shift (-2.931, y, -2.928) then RotY(i * pi/2), y = -153.844 / -195.879 / -237.844.
inline void PlaceRing(const std::map<uint32_t, std::pair<size_t, size_t>>& ring, Model& m) {
  const float kPi = 3.14159265f;
  const float canvas_y = -111.f;                       // file space: the canvas corner bones (C)
  // [C, live run 068 2026-11-08] the rope simulation's bone positions (ring context + 104 + 12n and + 5000 + 768 s, read
  // in a match): y = -154.5 / -195.5 / -237.5 in file space = 43.5 / 84.5 / 126.5 above the canvas, at z = -288 (bones
  // every 24 units from x -288 to 288).
  const float rope_above[3] = {43.5f, 84.5f, 126.5f};
  const float rope_z = -288.f;
  auto place = [&](uint32_t key, const std::vector<Matrix>& file_mats, const char* label) {
    auto it = ring.find(key);
    if (it == ring.end() || it->second.first == it->second.second) return;
    Placement pl;
    pl.label = label;
    for (const Matrix& f : file_mats) pl.transforms.push_back(MirrorY(f));
    m.placements.push_back(std::move(pl));
    for (size_t i = it->second.first; i < it->second.second; ++i) m.meshes[i].placement = int(m.placements.size()) - 1;
  };
  const float buckle_above[3] = {43.f, 85.f, 127.f};                  // lane 275 [C]: turnbuckle node-2 rest heights -154 / -196 / -238
  const float cover_y[3] = {-153.844f, -195.879f, -237.844f};          // lane 275 [C]: cover node-2 heights (calls 0-2 of the capture)
  std::vector<Matrix> corners, ropes, per_corner_rope, buckles, covers, skirt_a, skirt_b;
  for (int i = 0; i < 4; ++i) {
    const Matrix r = RotYFile(float(i) * kPi * 0.5f);
    corners.push_back(r);
    for (float h : rope_above) {
      ropes.push_back(Mul(TransFile(0, canvas_y - h, rope_z), r));
      per_corner_rope.push_back(Mul(TransFile(0, canvas_y - h, 0), r));
    }
    for (float h : buckle_above) buckles.push_back(Mul(Mul(RotYFile(-kPi * 0.25f), TransFile(-310.015f, canvas_y - h, 142.534f)), r));
    for (float y : cover_y) covers.push_back(Mul(TransFile(-2.931f, y, -2.928f), r));
    (i % 2 == 0 ? skirt_b : skirt_a).push_back(Mul(TransFile(0, 0, -320.f), r));
  }
  if (ring.count(1050) || ring.count(1017)) {   // S103 CRR: a created arena's ALT ring [C, CRK section 2: sub_825CCFE0 / sub_825CE668 / sub_825CF2F8 in 200..249]
    // posts: loop 1 = ar_post01 (1050) at corners 1, 2; loop 2 = ar_post02 (1051) + the logo 02 (1081) at corners 0, 3 and the logo 01 (1080) at corners 1, 2 (angle index r21 * (r10 + i): 1, 2 for h 1; 0, -1 for h 0 / 2)
    const auto rot = [&](int idx) { return RotYFile(float(idx) * kPi * 0.5f); };
    const std::vector<Matrix> c12 = {rot(1), rot(2)}, c03 = {rot(0), rot(-1)};
    place(1050, c12, "ring post");
    place(1051, c03, "ring post");
    place(1080, c12, "ring post logo");
    place(1081, c03, "ring post logo");
    place(1021, ropes, "ring rope");   // (the 960 rule)
    place(952, buckles, "ring turnbuckle");
    // covers: the corner at angle p * pi/2 holds 1070 + p (CRR fix [C]: the alt branch of sub_825CE668 0x825CE848-0x825CE89C poses call corner r25 at angle {pi/2, pi, -pi/2, 0}, so r25 0..3 = 1071, 1072, 1073, 1070 = the corners p = 1, 2, 3, 0).
    // [L] the shift T(-2.931, y, -2.928) and the three heights are the NORMAL cover 964's node-2 bind (lane 275); 1070-1073 have the same vertex counts and positions among themselves (crr_unit), their own bind vs 964 is not read.
    static const int kCoverKey[4] = {1070, 1071, 1072, 1073};
    for (int i = 0; i < 4; ++i) {
      std::vector<Matrix> cc;
      for (float y : cover_y) cc.push_back(Mul(TransFile(-2.931f, y, -2.928f), RotYFile(float(i) * kPi * 0.5f)));
      place(uint32_t(kCoverKey[i]), cc, "ring corner cover");
    }
    place(1064, skirt_a, "ring skirt");   // G[332..344] = 1065, 1064, 1065, 1064 = 974, 973, 974, 973's models
    place(1065, skirt_b, "ring skirt");
    return;
  }
  place(951, corners, "ring post");
  place(960, ropes, "ring rope");
  place(952, buckles, "ring turnbuckle");
  place(964, covers, "ring corner cover");
  place(973, skirt_a, "ring skirt");
  place(974, skirt_b, "ring skirt");
}

// Session 97 (props builder): the ring steps. The game draws one small concrete step block (3 meshes: 181 / 32 / 20 vertices,
// textures `ar_stair*`) at two diagonal ring corners, turned 45 degrees: world = [[.7071 0 -.7071 328] [0 1 0 0] [.7071 0 .7071 -328]]
// and [[-.7071 0 -.7071 328] [0 1 0 0] [.7071 0 -.7071 328]] [C: swaps 6728 / 20911 / 22453, draws 1204-1209 / 1564-1569 /
// 1140-1145, the world matrix rows c76..c78]. The model is a leaf of `pac\bg\bgEtc.pac` (a shared prop library, not the
// arena's pac): PACH 9, nested entry 29 / 32 / 35 (three texture variants of the same 181 / 32 / 20 vertex meshes; 30 / 33 / 36 and
// 31 / 34 / 37 are larger sets of the same family) [C counts and bounds]. Which variant an arena uses is not found (the probe has
// no textures): [U] we take entry 29 for every arena. Opens bgEtc once per program (its own cache, so the arena pac stays valid).
inline const pac::Pac* OpenProps(launcher::GameFiles& files) {
  static pac::Pac props;
  static int state = 0;  // 0 = not tried, 1 = open, 2 = failed
  if (state == 0) {
    std::string e;
    state = pac::Open(files, "pac\\bg\\bgEtc.pac", props, e) && props.pachs.size() > 9 ? 1 : 2;
  }
  return state == 1 ? &props : nullptr;
}

// Row-vector matrix (v * M) from the game's column form rows [r0 r1 r2] with translation in the 4th column.
inline Matrix FromGameRows(const float (&r)[12]) {
  return {r[0], r[4], r[8], 0, r[1], r[5], r[9], 0, r[2], r[6], r[10], 0, r[3], r[7], r[11], 1};
}

// Adds a bgEtc prop (PACH 9, entry `key`) at `mats` (file space); returns the number of meshes added.
// (Session 97 props2: swaps red and blue in the colour blocks of a BC3 texture; BC3 colour blocks ignore the endpoint order.)
inline void SwapRedBlueBC3(Texture& t) {
  if (t.format != TexFormat::kBC3) return;
  for (size_t o = 0; o + 16 <= t.data.size(); o += 16)
    for (int e = 0; e < 2; ++e) {
      uint8_t* q = &t.data[o + 8 + 2 * size_t(e)];
      const uint16_t c = uint16_t(q[0] | (q[1] << 8)), r = (c >> 11) & 31, b = c & 31;
      const uint16_t n = uint16_t((b << 11) | (c & 0x7E0) | r);
      q[0] = uint8_t(n & 255), q[1] = uint8_t(n >> 8);
    }
}

inline size_t AddProp(launcher::GameFiles& files, uint32_t key, const std::vector<Matrix>& mats, const char* label, Model& m,
                      bool swap_rb = false, const std::vector<std::vector<Matrix>>* per_mesh = nullptr) {
  const pac::Pac* p = OpenProps(files);
  if (!p) return 0;
  const Bytes& buf = p->bytes;
  Bytes nb;
  pac::Pach np;
  if (!pac::GetNested(buf, p->pachs[9], key, nb, np)) return 0;
  const Bytes blob = pac::Get(nb, np, 0);
  if (!IsJboy(blob)) return 0;
  TexSet tex;
  for (const pac::Ent& e : np.ents)
    if (((e.id >= 10 && e.id <= 19) || e.id == 20) && e.size >= 0x30) tex.AddDir(pac::Decode(nb.data() + e.off, e.size));   // S103 RFL: + key 20 (the second texture directory: the reflection cubes)
  JboyOptions o;
  o.arena = true;
  o.part = std::string(label) + " " + std::to_string(key);
  o.kind = 4, o.key = int(key), o.pac = "bgEtc.pac";  // Session 98 (step 16)
  o.tex = &tex;
  const size_t first = m.meshes.size();
  std::string e2;
  if (!LoadJboy(blob, o, m, e2)) {
    m.warnings.push_back(std::string(label) + " " + std::to_string(key) + " failed: " + e2);
    return 0;
  }
  if (swap_rb) {
    std::vector<int> done;
    for (size_t k = first; k < m.meshes.size(); ++k) {
      const int mi = m.meshes[k].material;
      const int d = mi >= 0 ? m.materials[size_t(mi)].diffuse : -1;
      if (d < 0 || std::find(done.begin(), done.end(), d) != done.end()) continue;
      done.push_back(d);
      SwapRedBlueBC3(m.textures[size_t(d)]);
    }
  }
  if (per_mesh && !per_mesh->empty()) {  // one placement per mesh (the game draws a prop's parts with their own matrices)
    for (size_t k = first; k < m.meshes.size(); ++k) {
      Placement pl;
      pl.label = label;
      for (const Matrix& f : (*per_mesh)[std::min(k - first, per_mesh->size() - 1)]) pl.transforms.push_back(MirrorY(f));
      m.placements.push_back(std::move(pl));
      m.meshes[k].placement = int(m.placements.size()) - 1;
    }
    return m.meshes.size() - first;
  }
  Placement pl;
  pl.label = label;
  for (const Matrix& f : mats) pl.transforms.push_back(MirrorY(f));
  m.placements.push_back(std::move(pl));
  for (size_t k = first; k < m.meshes.size(); ++k) m.meshes[k].placement = int(m.placements.size()) - 1;
  return m.meshes.size() - first;
}

// Session 97: the crowd's pose. The game CPU-skins every crowd body each frame, one pose per person type, from a
// 17-node skeleton (entry 20200; sub_8250BAF0 / sub_82509CB8, out\runtime\s97\crowd_pose.md). The motions are in the
// player's own pac\m2.pac (src\assets\crowd_motion.h, out\runtime\s97\m2_motion.md): per person type one key set for each
// crowd act 14001-14011. CrowdPose(): game = pose each type with act CrowdAct() at CrowdFrame() (-1 = frame (7 * type) mod
// the motion's length, so the types differ); stored = leave the bodies in their stored T-pose (also the fallback when
// m2.pac or a type's entry is missing).
enum class CrowdPoseMode { kGame, kStored };
inline CrowdPoseMode& CrowdPose() {
  static CrowdPoseMode mode = CrowdPoseMode::kGame;
  return mode;
}
inline int& CrowdAct() {
  static int act = 14005;  // the act seen live (out\runtime\s97\m2_motion.md s.4)
  return act;
}
inline int& CrowdFrame() {
  static int frame = -1;
  return frame;
}
// Bakes the pose into the body's vertices: skin = inverse(bind_world) * pose_world per node, weights blended. `local` =
// the pose's local matrices (one per node, row vectors, the body's units).
inline void PoseCrowdBody(const std::vector<Bone>& nodes, const std::vector<Matrix>& local, Model& m, size_t first_mesh) {
  const size_t n = nodes.size();
  if (n == 0 || n > 256 || local.size() != n) return;
  std::vector<Matrix> bind(n), pose(n), skin(n);
  for (size_t i = 0; i < n; ++i) {
    const Bone& b = nodes[i];
    const Matrix lb = FromQuatT(b.local_rotation, b.local_translation);
    const Matrix& lp = local[i];
    const bool hp = b.parent >= 0 && size_t(b.parent) < i;
    bind[i] = hp ? Mul(lb, bind[size_t(b.parent)]) : lb;
    pose[i] = hp ? Mul(lp, pose[size_t(b.parent)]) : lp;
    skin[i] = Mul(InverseRigid(bind[i]), pose[i]);
  }
  for (size_t k = first_mesh; k < m.meshes.size(); ++k)
    for (Vertex& v : m.meshes[k].vertices) {
      float p[3] = {0, 0, 0}, nn[3] = {0, 0, 0};
      float wsum = 0;
      for (int j = 0; j < 4; ++j) {
        const float w = v.weights[j];
        if (!(w > 0.f) || v.bones[j] >= n) continue;
        const Matrix& t = skin[v.bones[j]];
        for (int c = 0; c < 3; ++c) {
          p[c] += w * (v.pos[0] * t[c] + v.pos[1] * t[4 + c] + v.pos[2] * t[8 + c] + t[12 + c]);
          nn[c] += w * (v.normal[0] * t[c] + v.normal[1] * t[4 + c] + v.normal[2] * t[8 + c]);
        }
        wsum += w;
      }
      if (wsum > 0.f)
        for (int c = 0; c < 3; ++c) v.pos[c] = p[c] / wsum, v.normal[c] = nn[c] / wsum;
    }
}

// Session 97 (crowd reader, out\runtime\s97\crowd.md; parser sub_8250ECC0): the audience set's seat text (21100 inside
// entry 20000): rows "L(x0,z0,dx,dz)" (letters A-Z, a-z), then "[N] n" per person type and lines "Lkk[(bNN)]": seat k of
// row L at (x0 + dx k, z0 + dz k), facing the origin, yaw quantised to 128 steps. Body = entry 20000 + N (near detail),
// texture aud_NNa. Height: the game stores y = 0 (these are the floor seats; the stands are the separate block crowd);
// we use the nearest chair's lowest point, which is the floor there.
inline void AddCrowd(const pac::Pach& h, const Bytes& buf, TexSet& tex, const std::vector<std::array<size_t, 3>>& chairs,
                     Model& m, launcher::GameFiles* files = nullptr) {
  Bytes sb;
  pac::Pach sp;
  if (!pac::GetNested(buf, h, 20000, sb, sp)) return;
  if (const pac::Ent* e = pac::Find(sp, 21000)) tex.AddDir(pac::Decode(sb.data() + e->off, e->size));
  const Bytes tb = pac::Get(sb, sp, 21100);
  if (tb.empty()) return;
  // The game's tokeniser: spaces / tabs dropped, ';' ends a line's content, CR / LF split lines.
  std::vector<std::string> ls;
  std::string cur;
  bool cut = false;
  for (uint8_t c : tb) {
    if (c == '\r' || c == '\n') {
      if (!cur.empty()) ls.push_back(cur);
      cur.clear(), cut = false;
    } else if (c == ';') {
      cut = true;
    } else if (!cut && c != ' ' && c != '\t') {
      cur.push_back(char(c));
    }
  }
  if (!cur.empty()) ls.push_back(cur);
  struct Row { float x0, z0, dx, dz; bool ok = false; };
  Row rows[52];
  auto row_index = [](char c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? 26 + c - 'a' : -1; };
  size_t i = 0;
  while (i < ls.size() && ls[i][0] != '{') ++i;
  for (++i; i < ls.size() && ls[i][0] != '}'; ++i) {
    const int r = row_index(ls[i][0]);
    float v[4];
    if (r >= 0 && std::sscanf(ls[i].c_str() + 1, "(%f,%f,%f,%f)", &v[0], &v[1], &v[2], &v[3]) == 4)
      rows[r] = {v[0], v[1], v[2], v[3], true};
  }
  // Chair points (file space) for the height.
  std::vector<std::array<float, 3>> chair_pts;
  for (const auto& c : chairs)  // {first mesh, end, r / y bits}: mirrored / turned chair blocks count too (S97 review)
    for (size_t k = c[0]; k < c[1]; ++k)
      for (const Vertex& v : m.meshes[k].vertices) {
        chair_pts.push_back({v.pos[0], v.pos[1], v.pos[2]});
        if (c[2] & 1) chair_pts.push_back({-v.pos[0], v.pos[1], v.pos[2]});
        if (c[2] & 2) chair_pts.push_back({-v.pos[0], v.pos[1], -v.pos[2]});
      }
  std::map<int, std::vector<Matrix>> by_type;
  int people = 0, no_chair = 0;
  for (++i; i < ls.size(); ++i) {
    int type = 0, n = 0;
    if (std::sscanf(ls[i].c_str(), "[%d]%d", &type, &n) != 2 || type < 0 || type > 19) continue;  // (20 near-detail bodies, 20000-20019)
    while (i < ls.size() && ls[i][0] != '{') ++i;
    for (++i; i < ls.size() && ls[i][0] != '}' && people < 4000; ++i) {
      const std::string& s = ls[i];
      const int r = s.size() >= 3 ? row_index(s[0]) : -1;
      if (r < 0 || !rows[r].ok || !std::isdigit((unsigned char)s[1]) || !std::isdigit((unsigned char)s[2])) continue;
      if (s.size() > 4 && s[3] == '(' && (s[4] == 'c' || s[4] == 'w')) continue;  // child objects: not drawn by us [U]
      const int k = (s[1] - '0') * 10 + (s[2] - '0');
      const float x = rows[r].x0 + rows[r].dx * float(k), z = rows[r].z0 + rows[r].dz * float(k);
      const float kPi = 3.14159265f;
      float ang = std::atan2(z, -x) - kPi / 2;
      while (ang > kPi) ang -= 2 * kPi;
      while (ang < -kPi) ang += 2 * kPi;
      const int q = (int((ang + 2 * kPi) * 651.8986206f) >> 5) & 0x7F;
      // Height: the lowest point (largest file y: -Y up) of the chair nearest in x/z (within 80 units; points up to 40
      // farther than the nearest count as the same chair).
      float dmin = 1e30f;
      for (const auto& p : chair_pts) dmin = std::min(dmin, std::hypot(p[0] - x, p[2] - z));
      float y = 0;
      if (dmin <= 80.f) {
        y = -1e30f;
        for (const auto& p : chair_pts)
          if (std::hypot(p[0] - x, p[2] - z) <= dmin + 40.f) y = std::max(y, p[1]);
      } else {
        ++no_chair;
      }
      // Scale: the crowd rig is a 1/10 rig (its shin 4.348 vs a wrestler's calf 43.48: crowd.md s.6, [U] that the game
      // multiplies by 10: no constant found) times the game's per-person lerp(0.8421, 1.0105, rand) (sub_8250E470); our
      // "random" is a fixed hash of the seat so shots repeat.
      const float rnd = float((uint32_t(r) * 2654435761u ^ uint32_t(k) * 40503u) % 1000u) / 999.f;
      const float sc = 10.f * (0.8421f + (1.0105f - 0.8421f) * rnd);
      const Matrix scale = {sc, 0, 0, 0, 0, sc, 0, 0, 0, 0, sc, 0, 0, 0, 0, 1};
      by_type[type].push_back(Mul(Mul(scale, RotYFile(float(q) * 2 * kPi / 128)), TransFile(x, y, z)));
      if (++people >= 4000) break;  // (S97 review: hostile text must not hang the chair search; bg05 has 379)
    }
  }
  // The stands (block crowd, sub_82513D78; crowd.md s.7): zone quads 21201 ("NR") and 21202 ("R"), one per line, 12
  // numbers = 4 corners x (x, z, h) in decimetres -> file (x*10, -h*10, z*10). Rows along edge B (corner 0 -> 2, step
  // 105), members along edge A (0 -> 1, step 92), the row length blending from edge A's count to edge C's (2 -> 3), odd
  // rows shifted half a step; one yaw per quad. The list is then mirrored (x and yaw negated). [U / ours]: quads that
  // straddle x = 0 are not mirrored (they would double up); the game's small jitter is left out; bodies are dealt to the
  // 20 types round-robin with our own fixed shuffle.
  int stands = 0;
  {
    const float kPi = 3.14159265f;
    uint32_t deal = 0;
    for (uint32_t key : {21201u, 21202u}) {
      const Bytes qb = pac::Get(sb, sp, key);
      std::string line;
      for (size_t c = 0; c <= qb.size(); ++c) {
        const char ch = c < qb.size() ? char(qb[c]) : '\n';
        if (ch != '\n' && ch != '\r') {
          line.push_back(ch);
          continue;
        }
        float q[12];
        const bool ok = !line.empty() && line[0] != ';' &&
                        std::sscanf(line.c_str(), "%f %f %f %f %f %f %f %f %f %f %f %f", &q[0], &q[1], &q[2], &q[3], &q[4],
                                    &q[5], &q[6], &q[7], &q[8], &q[9], &q[10], &q[11]) == 12;
        line.clear();
        if (!ok) continue;
        float P[4][3];
        for (int k = 0; k < 4; ++k) P[k][0] = q[3 * k] * 10, P[k][1] = -q[3 * k + 2] * 10, P[k][2] = q[3 * k + 1] * 10;
        auto len = [](const float* a, const float* b) {
          return std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
        };
        const float la = len(P[0], P[1]), lb = len(P[0], P[2]), lc = len(P[2], P[3]);
        // (S97 review: NaN / huge edges must not explode the count; edge C counts too)
        if (!(la >= 1e-3f && lb >= 1e-3f && lc >= 0.f && la < 400 * 92.f && lb < 400 * 105.f && lc < 400 * 92.f)) continue;
        const int na = int(la / 92.f + 1), nb = int(lb / 105.f + 1), nc = int(lc / 92.f + 1);
        if (na > 400 || nb > 400) continue;  // (a corrupt line must not explode the count)
        float sa[3], sbv[3];
        for (int k = 0; k < 3; ++k) sa[k] = (P[1][k] - P[0][k]) / la * 92.f, sbv[k] = (P[2][k] - P[0][k]) / lb * 105.f;
        const float yaw = std::atan2(-(P[1][2] - P[0][2]), P[1][0] - P[0][0]);
        const int qyaw = (int((yaw + 2 * kPi) * 651.8986206f) >> 5) & 0x7F;
        const float minx = std::min(std::min(P[0][0], P[1][0]), std::min(P[2][0], P[3][0]));
        const float maxx = std::max(std::max(P[0][0], P[1][0]), std::max(P[2][0], P[3][0]));
        const bool mirror = !(minx < 0 && maxx > 0);
        for (int r = 0; r < nb; ++r) {
          const int cnt = int(float(nb - r) * na / nb + float(r) * nc / nb + 0.5f);
          for (int k = 0; k < cnt && stands < 30000; ++k) {
            float p[3];
            for (int a = 0; a < 3; ++a) p[a] = P[0][a] + r * sbv[a] + (r & 1 ? 0.5f * sa[a] : 0.f) + k * sa[a];
            for (int side = 0; side < (mirror ? 2 : 1); ++side) {
              const uint32_t hsh = (deal++ * 2654435761u) ^ 0x9E3779B9u;
              const int type = int((hsh >> 7) % 20u);
              const float rnd = float((hsh >> 3) % 1000u) / 999.f;
              const float sc = 10.f * (0.8421f + (1.0105f - 0.8421f) * rnd);
              const Matrix scale = {sc, 0, 0, 0, 0, sc, 0, 0, 0, 0, sc, 0, 0, 0, 0, 1};
              const float ang = float(side ? (128 - qyaw) & 0x7F : qyaw) * 2 * kPi / 128;
              Matrix mm = Mul(Mul(scale, RotYFile(ang)), TransFile(side ? -p[0] : p[0], p[1], p[2]));
              mm[15] = 2.f;  // Session 98 (gen check 3): a STANDS member (block crowd): the card LOD rule applies (renderer.cpp vs; xyz unaffected)
              by_type[type].push_back(mm);
              if (++stands >= 30000) break;  // (the game: 768 per type x 20 = 15360 per half)
            }
          }
        }
      }
    }
  }
  int posed = 0, fallback = 0;
  std::string pose_err;
  const bool use_game = CrowdPose() == CrowdPoseMode::kGame && files && crowdmot::TheM2().Load(*files, pose_err);
  for (auto& [type, mats] : by_type) {
    const Bytes blob = pac::Get(sb, sp, 20000u + uint32_t(type));
    if (!IsJboy(blob)) continue;
    JboyOptions o;
    o.arena = true;
    o.part = "crowd " + std::to_string(type);
    o.kind = 3, o.key = 20000 + type;  // Session 98 (step 16): the pac name is filled in by LoadArena
    o.tex = &tex;
    std::vector<Bone> nodes;
    o.own_nodes = &nodes;
    const size_t before = m.meshes.size();
    std::string e2;
    if (!LoadJboy(blob, o, m, e2)) {
      m.warnings.push_back("crowd body " + std::to_string(type) + " failed: " + e2);
      continue;
    }
    if (use_game) {
      const crowdmot::Keys& ks = crowdmot::TheM2().KeysFor(CrowdAct(), type);
      std::vector<Matrix> local;
      const int len = crowdmot::Length(ks);
      const double frame = len > 0 ? (CrowdFrame() >= 0 ? CrowdFrame() : 7 * type) % len : 0;
      if (crowdmot::Pose(ks, frame, nodes, local)) {
        PoseCrowdBody(nodes, local, m, before);
        ++posed;
      } else {
        ++fallback;  // no entry for this type: the stored pose
      }
    }
    char tn[16];
    std::snprintf(tn, sizeof tn, "aud_%02da", type);
    const int ti = tex.Get(m, tn);
    Placement pl;
    pl.label = std::string("crowd ") + tn;
    for (const Matrix& f : mats) {
      Matrix t = MirrorY(f);
      t[15] = f[15];  // (the stands tag survives the mirror: MirrorY leaves [15] as is, kept explicit)
      pl.transforms.push_back(t);
    }
    m.placements.push_back(std::move(pl));
    for (size_t k = before; k < m.meshes.size(); ++k) {
      m.meshes[k].placement = int(m.placements.size()) - 1;
      if (ti >= 0 && m.meshes[k].material >= 0) m.materials[size_t(m.meshes[k].material)].diffuse = ti;
      // (S97 probe: the game lights the crowd with the stage lightmap too, gain c184.z; stage_crowd_light.md)
      if (m.meshes[k].material >= 0) m.materials[size_t(m.meshes[k].material)].has_lm_slot = true;
      if (m.meshes[k].material >= 0) m.materials[size_t(m.meshes[k].material)].crowd = true;
      // Session 98 (arena fix): the crowd draws' c189 = c190 = (1, 1, 1, 1) [C: probe run 6816, every crowd member draw (VS C1028A64 /
      // CB5B16DA) of frames 8941 / 10741 / 11341; stage_crowd_light.md for run 069], whatever the body's own material says.
      if (m.meshes[k].material >= 0)
        for (int c = 0; c < 4; ++c) m.materials[size_t(m.meshes[k].material)].ambient[c] = m.materials[size_t(m.meshes[k].material)].diffuse_color[c] = 1.f;
    }
  }
  if (CrowdPose() == CrowdPoseMode::kGame && !use_game && !by_type.empty())
    m.warnings.push_back("crowd: pose from m2.pac unavailable (" + pose_err + "), stored pose (info)");
  else if (fallback)
    m.warnings.push_back("crowd: " + std::to_string(fallback) + " person types without a motion for act " + std::to_string(CrowdAct()) +
                         " in m2.pac, stored pose (info)");
  if (no_chair) m.warnings.push_back("crowd: " + std::to_string(no_chair) + " people with no chair within 80 units (height 0)");
  if (people || stands)
    m.warnings.push_back("crowd: " + std::to_string(people) + " people seated, " + std::to_string(stands) +
                         " in the stands (info)");
}

}  // namespace load_detail

// S103 D1: one entry-50001 token into the game's slot record (tools\harvest\choice_predict.py rule_slot_record, the same order of tests):
//   [R7 byte +10 bits] sub_824E8628 0x824E8C88-0x824E8DF4: x -> +10 |= 0xC0, xg -> 0x80, xe -> 0x40
//   [R10 class3] parser case 'p' 0x824E8C40-0x824E8C70: pg -> +9 |= 0x04, pe -> 0x02, p -> 0x06
//   [R8 slot record] case 0x824E8B00: t<N> -> +7 = N (a byte); bare w -> +6 |= 0x02; else the jump table 0x824E8948 by the first letter
inline void SlotRecordToken(const std::string& t, Model::SlotRecord& r) {
  if (t == "x") { r.b10 |= 0xC0; return; }
  if (t == "xg") { r.b10 |= 0x80; return; }
  if (t == "xe") { r.b10 |= 0x40; return; }
  if (t == "pg") { r.b9 |= 0x04; return; }
  if (t == "pe") { r.b9 |= 0x02; return; }
  if (t == "p") { r.b9 |= 0x06; return; }
  if (t[0] == 't') {
    size_t i = 1;
    while (i < t.size() && (t[i] < '0' || t[i] > '9')) ++i;
    uint32_t n = 0;
    bool any = false;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') n = n * 10 + uint32_t(t[i++] - '0'), any = true;
    r.b7 = any ? uint8_t(n & 0xFF) : 0;
    return;
  }
  if (t == "w") { r.b6 |= 0x02; return; }
  switch (t[0]) {   // letter -> (byte, bit) [R8 slot record]
    case 'a': r.b8 |= 0x40; break;
    case 's': r.b8 |= 0x20; break;
    case 'k': r.b8 |= 0x10; break;
    case 'z': r.b8 |= 0x08; break;
    case 'n': r.b8 |= 0x04; break;
    case 'r': r.b8 |= 0x02; break;
    case 'y': r.b8 |= 0x01; break;
    case 'e': r.b6 |= 0x01; break;
    case 'h': r.b6 |= 0x04; break;
    case 'g': r.b6 |= 0x08; break;
    case 'c': r.b9 |= 0x40; break;
    case 'i': r.b10 |= 0x10; break;
    case 'j': r.b10 |= 0x08; break;
    case 'o': r.b10 |= 0x04; break;
    case 'u': r.b10 |= 0x01; break;
    default: break;
  }
}

// S103 RL: the light set the arena's own props (the ring steps, the announce desk and its top: bgEtc entries 32 / 6 / 7) are drawn with = key set 50 when the arena
// has it, else 100 (the old choice). [C run29 f500 (arena 4, match): every game draw of the steps / desk (yProp_BumpMap / yProp_Default, 4 + 2 + 1 + 1 + 1 + 1 + 3 +
// 13 + 2 + 1 draws) carries DifLghCol 0.384 / 0.230 / 0.230 / 0.184, sky 0.784, ground 0.392 = the record's evaluated set 50 to 3 digits; set 100 (ours before)
// has no lights (Q4 claims_Q34.jsonl "ring steps"); docs\formats\light_set.md: 50 is one of the ring + props sets]. Where the game picks 50 for the props is not
// read [U]; the choice is [L fitted]: scored 4 of 4 arenas (4 on its frame, 8 / 9 / 16 at frame 100 of a0x_match, col0 / col1 / col3 / sky / ground to 0.0000, tools\scene\rl_check.py), arenas
// without a set 50 fall back to 100. A read-rule lead, not confirmed: the class constructor sub_825E0F60 resolves id 100 when sub_825DDC70(kind) (kinds 51-59 / 61 / 62 / 64 / 68 / 69) else 50
// (docs\formats\light_set.md). Planted twin: load_plants.h.
inline int PropsLightSet(const std::map<int, LightSet>& all_sets) {
  return !load_detail::PropsSet100Plant() && all_sets.count(50) ? 50 : 100;
}

// S103 B3: the arena post table (misc02_ingame.pac p0/102/5), parsed once per process from the player's own file (null = unreadable: arena_lm::Compute counts it).
inline const arena_lm::Table* ArenaLmTable(launcher::GameFiles& files) {
  static std::mutex mu;
  static int state = 0;   // 0 = not tried, 1 = ready, 2 = failed
  static arena_lm::Table table;
  std::lock_guard<std::mutex> lk(mu);
  if (state == 0) {
    state = 2;
    pac::Pac p;
    std::string e;
    if (pac::Open(files, "pac\\misc02_ingame.pac", p, e) && !p.pachs.empty()) {
      Bytes nb;
      pac::Pach np;
      if (pac::GetNested(p.bytes, p.pachs[0], 102, nb, np)) {
        const Bytes leaf = pac::Get(nb, np, 5);
        if (arena_lm::ParseLeaf(leaf.data(), leaf.size(), table)) state = 1;
      }
    }
  }
  return state == 1 ? &table : nullptr;
}

// S103 WMB (docs\research\s103\WMB.md, Jarrod 2026-11-19 07:4x "grab from other arenas"): texture names an arena's own pac lacks, where the game binds nothing and draws a stale
// slot (a 360 engine artefact, 360_HABITS), are borrowed from the donor arena's directory 400. Admit a name ONLY when its bytes are identical in every bg pac that ships it:
// beam_moya01 = DXT5 256x256, one SHA-1 over the DDS bytes in all 44 pacs (bg00-09, 11-14, 16, 17, 21, 22, 29-35, 37-55; hashed 2026-11-19, tools: scratch wmb_hash.py).
// Never per-arena data (fade_lightmap00 has 9 variants) and never a name that differs between pacs (stand_ground0, truss_01).
struct BorrowName {
  const char* name;
  int donor_arena;
};
// More names, each one SHA-1 over the DDS bytes in EVERY bg pac that ships it (hashed 2026-11-19, dir 400): ji_chair 48 pacs, lightmap00 45, msg_score1 11, slope_panel 3 (donor = a pac holding that hash).
// Not admitted: stand_ground0 / truss_01 (3 variants each), fade_lightmap00 (9, per-arena data), specular / sv_obj02 / sv_obj05 / wm27_titan1_ref (in no bg pac).
inline constexpr BorrowName kBorrowTextures[] = {{"beam_moya01", 0}, {"ji_chair", 0}, {"lightmap00", 0}, {"msg_score1", 4}, {"slope_panel", 43}};

inline bool LoadArena(launcher::GameFiles& files, int id, Model& m, std::string& err) {
  using namespace load_detail;
  const double t0 = NowMs();
  m = {};
  char lab[64];
  std::snprintf(lab, sizeof lab, "arena %d", id);
  m.label = lab;
  const pac::Pac* p = OpenCached(files, ArenaRel(id), err);
  if (!p) return false;
  const pac::Pach& h = p->pachs[0];
  const Bytes& buf = p->bytes;
  TexSet tex;
  const bool car_id = car::IsCustomId(id);
  TexSet tex_part[3];   // S103 CAR fix 2: a created arena's texture names collide across its parts (truss_00, ar_shdw, beam_glow01 differ per part): one set per part, a mesh takes its own part's
  if (car_id) {
    // CASD {400}, CAEN {403 = its 400, 401}, CARG {404 = its 400, 402} (car_arena.h); `tex` holds all of them in that order for the lookups that are not per mesh (the stage light map)
    for (const std::pair<uint32_t, int> kp : {std::pair<uint32_t, int>{400u, 0}, {403u, 1}, {401u, 1}, {404u, 2}, {402u, 2}})
      if (const pac::Ent* e = pac::Find(h, kp.first)) {
        const Bytes dir = pac::Decode(buf.data() + e->off, e->size);
        tex.AddDir(Bytes(dir)), tex_part[kp.second].AddDir(Bytes(dir));
      }
  } else {
    for (uint32_t k : {400u, 401u, 402u})
      if (const pac::Ent* e = pac::Find(h, k)) tex.AddDir(pac::Decode(buf.data() + e->off, e->size));
    // S103 WMB: a shared name this arena lacks comes from the donor arena's directory 400, holes only (AddDir keeps the first directory's texture)
    if (BorrowTextures() && !BorrowEmptyTablePlant())
      for (const BorrowName& bn : kBorrowTextures) {
        if (tex.Has(bn.name) || bn.donor_arena == id) continue;
        // The donor is opened into a LOCAL pac: OpenCached holds ONE pac, and opening bg00 through it would free the arena's own pac (`p`, `h`, `buf` above) under us.
        std::string derr;
        pac::Pac dpac;
        const pac::Ent* de = pac::Open(files, ArenaRel(bn.donor_arena), dpac, derr) && !dpac.pachs.empty() ? pac::Find(dpac.pachs[0], 400) : nullptr;
        if (de) tex.AddDir(pac::Decode(dpac.bytes.data() + de->off, de->size), std::vector<std::string>{bn.name});   // the log line comes from TexSet::Get when a material uses it
      }
  }
  // Session 97: the light sets (nested PACH 30002) and the per-slot `l(N)` choice (text entry 50001).
  std::map<int, LightSet> all_sets;
  {
    Bytes nb;
    pac::Pach np;
    if (pac::GetNested(buf, h, 30002, nb, np))
      for (const pac::Ent& e : np.ents) {
        const Bytes d = pac::Decode(nb.data() + e.off, e.size);
        if (d.size() != 784 && d.size() != 896) continue;
        const bool big = d.size() == 896;
        LightSet ls;
        ls.id = int(e.id);
        for (int i = 0; i < 4; ++i)
          for (int k = 0; k < 3; ++k) {
            ls.dir[i][k] = BeF(d.data() + 0x10 + 16 * i + 4 * k);
            ls.col[i][k] = BeF(d.data() + (big ? 0x60 : 0x50) + 16 * i + 4 * k);
          }
        for (int i = 0; i < 4; ++i) {  // Session 98 (skin fix): the colour's w = the light's entrance specular-lobe weight
          const float w = BeF(d.data() + (big ? 0x60 : 0x50) + 16 * i + 12);
          ls.wt[i] = std::isfinite(w) ? w : 0.f;
        }
        if (!big)
          for (int k = 0; k < 3; ++k) ls.sky[k] = BeF(d.data() + 0x100 + 4 * k), ls.ground[k] = BeF(d.data() + 0x110 + 4 * k);
        if (!big)  // S101 lane33: the per-light camera-attach flags (asset_model.h LightSet::follow)
          for (int i = 0; i < 4; ++i) {
            const uint8_t* q = d.data() + 0x120 + 4 * i;
            ls.follow[i] = (uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | uint32_t(q[3])) == 1u;
          }
        if (big)  // Session 98 (match oracle): the cube falloff per light (asset_model.h LightSet::fall)
          for (int i = 0; i < 4; ++i) {
            const float f = BeF(d.data() + 0x140 + 4 * i);
            ls.fall[i] = std::isfinite(f) ? std::clamp(f, 0.f, 1.f) : 0.f;
          }
        for (int i = 0; i < 4; ++i)
          for (int k = 0; k < 3; ++k)
            if (!std::isfinite(ls.dir[i][k]) || !std::isfinite(ls.col[i][k])) ls.col[i][0] = ls.col[i][1] = ls.col[i][2] = 0;
        for (int k = 0; k < 3; ++k)
          if (!std::isfinite(ls.sky[k]) || !std::isfinite(ls.ground[k])) ls.sky[k] = ls.ground[k] = 0;
        all_sets[ls.id] = ls;
      }
  }
  // Session 97 (stage key, out\runtime\s97\stage_key.md): entry 30000 holds the light sets as the game evaluates them
  // (reproduces the live draw probe: colours to 0.4 %, directions exactly in most draws, and gives the sky / ground of the
  // 896-byte kind), entry 30001 the slot motions m(N). Both are read from the player's own pac; without them (or with
  // stage_key::Cfg().enabled false) the 30002 records are used as before.
  Bytes key_l_buf, key_m_buf;
  stage_key::Index key_l, key_m;
  bool have_kl = false, have_km = false;
  if (stage_key::Cfg().enabled) {
    key_l_buf = pac::Get(buf, h, 30000);
    key_m_buf = pac::Get(buf, h, 30001);
    have_kl = !key_l_buf.empty() && key_l.Parse(key_l_buf);
    have_km = !key_m_buf.empty() && key_m.Parse(key_m_buf);
  }
  if (have_kl)
    for (auto& kv : all_sets) {
      LightSet k;
      if (!stage_key::EvalLightSet(key_l, kv.first, stage_key::Cfg().time, k)) continue;
      std::memcpy(kv.second.dir, k.dir, sizeof k.dir);
      std::memcpy(kv.second.col, k.col, sizeof k.col);
      std::memcpy(kv.second.sky, k.sky, sizeof k.sky);
      std::memcpy(kv.second.ground, k.ground, sizeof k.ground);
    }
  auto black = [have_kl](const LightSet& s) {
    // Session 98 (arena fix): with the key, set 502 is drawn as it is (no lights, sky = ground = the key's 0.615) by the game: probe run
    // 6816, every l(502) truss-lamp / glow-sheet draw (slots 219-221, 240-252 ...) in the entrance frames 8341-12241 and the match frame
    // 13741 has c0-c3 = 0 and c181 = c182 = 0.615 [C]. (The ring pieces' l(502) are set 31 / 32 below: their own evidence.)
    if (have_kl && s.id == 502) return false;
    // S101 lane33: set 100 (the default `l(100)` of a slot with no choice) with the key is drawn as the key gives it, like 502: lights 0, sky /
    // ground as keyed (arena 29: sky 0.784, ground 0.588; 36 + 29 draws of the wm30 and the Triple H / Brock matches had exactly those
    // c0-c3 = 0 / c181 / c182, not the stand-in's lights) [C: docs\renderer\LIGHT_MATCH.md]. A set 100 whose key has no ambient still falls back.
    if (have_kl && s.id == 100 && (s.sky[0] + s.sky[1] + s.sky[2] + s.ground[0] + s.ground[1] + s.ground[2]) > 1e-4f) return false;
    float t = 0;
    for (int i = 0; i < 4; ++i)
      for (int k = 0; k < 3; ++k) t += s.col[i][k];
    if (!have_kl)  // (the key gives every set an ambient; the sets 100 / 502 are lights-off placeholders that the game blends to the cue)
      for (int k = 0; k < 3; ++k) t += s.sky[k] + s.ground[k];
    return !(t > 1e-4f);
  };
  std::map<int, int> slot_set;  // stage slot -> l(N)
  std::map<int, int> slot_mirror;  // stage slot -> 1 `r`, 2 `y` (bits)
  std::map<int, int> slot_blend;   // stage slot -> 1 `a` additive, 2 `s` translucent (bits)
  std::map<int, int> slot_when;    // stage slot -> 1 `e` events only, 2 `g` game (match) only (bits)
  std::map<int, int> slot_motion;  // stage slot -> m(N): the slot is moved by the stage key's motion N (entry 30001)
  std::map<int, Model::SlotRecord> slot_rec;   // S103 D1: stage slot -> the game's slot record bytes +6..+10 (SlotRecordToken)
  struct SlotRecPublish {   // S103 D1: published into the model on every return path below
    Model& mm; std::map<int, Model::SlotRecord>& rec;
    int id;
    ~SlotRecPublish() { mm.slot_records.assign(rec.begin(), rec.end()), mm.arena = id; }
  } slot_rec_publish{m, slot_rec, id};
  if (const pac::Ent* e = pac::Find(h, 50001)) {
    const Bytes txt = pac::Decode(buf.data() + e->off, e->size);
    size_t pos = 0;
    while (pos < txt.size()) {
      size_t eol = pos;
      while (eol < txt.size() && txt[eol] != '\n') ++eol;
      const std::string line(reinterpret_cast<const char*>(txt.data()) + pos, eol - pos);
      pos = eol + 1;
      size_t i = 0;
      while (i < line.size() && line[i] == ' ') ++i;
      if (i >= line.size() || line[i] < '0' || line[i] > '9') continue;
      int slot = 0;
      while (i < line.size() && line[i] >= '0' && line[i] <= '9') slot = std::min(slot * 10 + (line[i++] - '0'), 1000000);
      {  // Session 97: whole-word flags `r` (also drawn mirrored in the YZ plane) and `y` (also drawn turned 180 about Y)
        std::string tok;
        for (size_t c = i; c <= line.size(); ++c) {
          const char ch = c < line.size() ? line[c] : ' ';
          if (ch == ' ' || ch == '\t' || ch == '\r' || ch == ';') {
            if (tok == "r") slot_mirror[slot] |= 1;
            if (tok == "y") slot_mirror[slot] |= 2;
            if (tok == "a") slot_blend[slot] |= 1;  // additive (the file's own comment: translucent `s` / additive `a`)
            if (tok == "s") slot_blend[slot] |= 2;
            // Session 98 (stage fix): the file's own legend (its `//` header, Shift-JIS): `e` = shown only in events (entrances),
            // `g` = shown only in the game (the match). Arena 5: slots 223 / 226 (the LED-scroll twisted pillars, `g`) sit inside the
            // entrance's movie pillars (802); the game draws none of them in the entrance (RenderDoc rp_ramp, run 6818: pixel history).
            if (tok == "e") slot_when[slot] |= 1;
            if (tok == "g") slot_when[slot] |= 2;
            if (!tok.empty()) SlotRecordToken(tok, slot_rec[slot]);   // S103 D1: the game's slot record bytes (the read rule)
            tok.clear();
            if (ch == ';') break;
          } else {
            tok.push_back(ch);
          }
        }
      }
      {  // Session 97: `m(N)` = the slot follows stage-key motion N
        const size_t mp = line.find(" m(", i);
        if (mp != std::string::npos) {
          int mn = 0;
          size_t j = mp + 3;
          while (j < line.size() && line[j] >= '0' && line[j] <= '9') mn = std::min(mn * 10 + (line[j++] - '0'), 1000000);
          if (j < line.size() && line[j] == ')') slot_motion[slot] = mn;
        }
      }
      const size_t lp = line.find("l(", i);
      if (lp == std::string::npos) continue;
      int ln = 0;
      size_t j = lp + 2;
      while (j < line.size() && line[j] >= '0' && line[j] <= '9') ln = std::min(ln * 10 + (line[j++] - '0'), 1000000);
      if (j < line.size() && line[j] == ')') slot_set[slot] = ln;
    }
  }
  // OUR CHOICE (not the game's): a mesh with no `l(N)` gets set 100. In the file it is all zero; with the stage key (S101 lane33) it has the
  // key's sky / ground and is drawn as such like the game (black() above; it now also shows in the lighting editor's list). Other black sets
  // (and a set 100 whose key has no ambient) still take the stand-in. Those meshes use the arena's commonest usable set (121 in bg05).
  int fallback_id = -1;
  {
    std::map<int, int> use;
    for (auto& kv : slot_set)
      if (kv.second != 502 && kv.second != 100 && all_sets.count(kv.second) && !black(all_sets[kv.second])) ++use[kv.second];  // (502 / S101 lane33: 100: never the stand-in)
    int best = 0;
    for (auto& kv : use)
      if (kv.second > best) best = kv.second, fallback_id = kv.first;
    if (fallback_id < 0 && all_sets.count(121)) fallback_id = 121;
  }
  std::map<int, int> set_index;  // set id -> index in m.light_sets
  auto resolve_set = [&](int want) -> int {
    auto it = all_sets.find(want);
    bool fb = false;
    if (it == all_sets.end() || black(it->second)) it = all_sets.find(fallback_id), fb = true;
    if (it == all_sets.end()) return -1;  // no usable set at all: the viewer's default rig
    const int key = fb ? -1 - it->first : it->first;  // Session 98: a fallback use is its own entry (the lighting editor skips it: edit_id -1)
    auto f = set_index.find(key);
    if (f != set_index.end()) return f->second;
    m.light_sets.push_back(it->second);
    if (fb) m.light_sets.back().edit_id = -1;
    return set_index[key] = int(m.light_sets.size()) - 1;
  };
  int models = 0;
  // Stage slots 0-399, then the ring set (Session 97, out\runtime\s97\skeleton.md): its pieces are stored in bind space
  // at ONE corner / side and drawn several times (placements below). Low-detail copies 980 (rope), 981 (post), 982
  // (cover) are loaded only by the live arena load (load_detail::RingLowKeys(), S103 RLB2: the posed ring draws the game's own LOD choice); 956 (a 705-unit bar through the origin at floor height) is left out: its role is unknown.
  std::vector<uint32_t> keys;
  for (uint32_t key = 0; key < 400; ++key) keys.push_back(key);
  for (uint32_t key : {951u, 952u, 953u, 960u, 964u, 973u, 974u}) keys.push_back(key);
  if (car_id)   // S103 CRR: a created arena's ring is the ALT set (CRK section 1): the game loads it by the arena id alone; 952 above is shared, 956 / 980 are in CARG too
    for (int ak : kAltRingHighKeys) keys.push_back(uint32_t(ak));
  if (load_detail::RingLowKeys())
    for (const RingLowKey& lk : kRingLowKeys)
      if (car_id || !IsAltRingKey(lk.low)) keys.push_back(uint32_t(lk.low));
  // Session 97 (out\runtime\s97\stage_crowd_light.md, matched to the live probe of run 069 by vertex / index counts): the
  // entrance stage and video wall are keys 800-809, drawn by code (no 50001 row): pairs of a PPV and an SS (Smackdown)
  // variant. We draw the PPV set 801 / 803 / 805 / 806 / 808 [C for arena 5; U that the pairing holds in every arena],
  // and then leave out slots 301 / 302 / 304 (`titan_OFF`, the stage with the screen off, not drawn by the game then).
  bool stage_set = false;
  {
    const pac::Pach& hh = p->pachs[0];
    for (uint32_t key : stage_media::StageKeys())
      if (pac::Find(hh, key)) keys.push_back(key), stage_set = stage_set || key == 801u || key == 802u;
  }
  // S103 SCF [C, docs\research\s103\SCM.md]: the LIVE model (all_keys) keeps 301-304: the game draws them whenever the match turns the screens off (the per-slot show bit, bit 0x80 of byte +9 of the stage record); a per-frame
  // mask applies the bit (rf_c_core.cpp ApplyShowMasks). Only the viewer / offline loads (no all_keys) keep the old fixed rule.
  if (load_detail::ErasesArtSlots(stage_set, stage_media::Cfg().all_keys)) keys.erase(std::remove_if(keys.begin(), keys.end(), [](uint32_t k) { return k == 301 || k == 302 || k == 304; }), keys.end());
  {  // Session 98 (stage fix): `e` / `g` slots (slot_when): the entrance look draws no `g` slot, the match look no `e` slot
    const int hide = stage_media::Cfg().wrestler >= 0 ? 2 : 1;
    if (!load_detail::KeepLookSlots())   // S103 WSL: the live load keeps both looks' slots; the applier masks them per frame
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&](uint32_t k) {
                 auto it = slot_when.find(int(k));
                 return k < 800 && it != slot_when.end() && (it->second & hide) && !(it->second & (3 - hide));
               }), keys.end());
  }
  std::map<uint32_t, std::pair<size_t, size_t>> ring_meshes;  // key -> [first, end) mesh index
  std::vector<std::array<size_t, 3>> chair_meshes;
  for (uint32_t key : keys) {
    const pac::Ent* e = pac::Find(h, key);
    if (!e) continue;
    Bytes blob = pac::Decode(buf.data() + e->off, e->size);
    if (!IsJboy(blob)) continue;
    const size_t first_mesh = m.meshes.size();
    JboyOptions o;
    o.arena = true;
    o.part = "slot " + std::to_string(key);
    o.kind = 1, o.key = int(key), o.pac = BaseName(ArenaRel(id));  // Session 98 (step 16)
    o.tex = car_id ? &tex_part[car::PartOfKey(key)] : &tex;   // S103 CAR fix 2
    o.uv_in_s2 = key >= 800 && key <= 809;  // the movie leaves keep their UV in S2 (S1 is a constant)
    std::string e2;
    const size_t before = m.meshes.size();
    if (!LoadJboy(blob, o, m, e2)) {
      m.warnings.push_back("slot " + std::to_string(key) + " failed: " + e2);
      continue;
    }
    if (key >= 951) ring_meshes[key] = {before, m.meshes.size()};
    {  // chairs / seats (the crowd's height): by the slot's first node name
      const uint8_t* body = blob.data() + 8;
      const uint32_t nt = Be32(body + 0x20);
      if (blob.size() >= 8 + size_t(nt) + 16) {
        std::string nm(reinterpret_cast<const char*>(body + nt), strnlen(reinterpret_cast<const char*>(body + nt), 16));
        for (char& c : nm) c = char(std::tolower((unsigned char)c));
        if (nm.find("chair") != std::string::npos || nm.find("seat") != std::string::npos)
          chair_meshes.push_back({before, m.meshes.size(), size_t(slot_mirror.count(int(key)) ? slot_mirror[int(key)] : 0)});
      }
    }
    ++models;
    // (Session 97) light set of the meshes this slot added
    auto sit = slot_set.find(int(key));
    const int ls = resolve_set(sit == slot_set.end() ? 100 : sit->second);
    for (size_t k = first_mesh; k < m.meshes.size(); ++k) m.meshes[k].light_set = ls;
    if (key >= 800 && key <= 809) {
      // Session 97 (probe run 069, draws 1107-1111): the game draws the stage / screens with no lights, sky = ground = 1
      // and materials 1: the movie art at full brightness, unlit, no stage lightmap.
      int unlit = -1;
      for (size_t s = 0; s < m.light_sets.size(); ++s)
        if (m.light_sets[s].id == -800) unlit = int(s);
      if (unlit < 0) {
        LightSet u;
        u.id = -800;  // ours: not a file set
        for (int c = 0; c < 3; ++c) u.sky[c] = u.ground[c] = 1.f;
        m.light_sets.push_back(u);
        unlit = int(m.light_sets.size()) - 1;
      }
      for (size_t k = first_mesh; k < m.meshes.size(); ++k) {
        m.meshes[k].light_set = unlit;
        if (m.meshes[k].material >= 0) {
          Material& mt = m.materials[size_t(m.meshes[k].material)];
          mt.has_lm_slot = false;
          for (int c = 0; c < 4; ++c) mt.ambient[c] = mt.diffuse_color[c] = 1.f;
        }
      }
    }
    if (auto bit = slot_blend.find(int(key)); bit != slot_blend.end())  // Session 97: `a` / `s` slots blend
      for (size_t k = first_mesh; k < m.meshes.size(); ++k)
        if (m.meshes[k].material >= 0) {
          Material& mt = m.materials[size_t(m.meshes[k].material)];
          mt.alpha_blend = true, mt.cutout_blend = 2;  // S103 F
          mt.additive = mt.additive || (bit->second & 1);
          mt.alpha_test = false;
        }
    // Session 97: `r` / `y` slots are drawn once as stored and once mirrored (x -> -x) / turned 180 degrees about Y.
    // Before this, half of every symmetric arena (stands, walls) was missing.
    if (auto mit = slot_mirror.find(int(key)); mit != slot_mirror.end() && first_mesh < m.meshes.size()) {
      Placement pl;
      pl.label = "slot " + std::to_string(key) + ((mit->second & 1) ? " mirrored" : " turned");
      pl.transforms.push_back(Identity());
      if (mit->second & 1) pl.transforms.push_back({-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
      if (mit->second & 2) pl.transforms.push_back({-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1});
      m.placements.push_back(std::move(pl));
      for (size_t k = first_mesh; k < m.meshes.size(); ++k) m.meshes[k].placement = int(m.placements.size()) - 1;
    }
    // Session 97 (props builder): the fence corner pieces 390-397 (`ar_fence` corner blocks and the `ar_fen_wwe` signs on them) are
    // stored with their corner at the origin (bounds within +-200) and drawn translated to the four corners of the barricade square.
    // [C for arena 5, swap 6728 draws 849-852, 868-870, 880-882, 1078-1081 matched by vertex / index counts; world matrices are pure
    // translations]: 390 and 394 (561, 0, -560); 391 and 395 (-561, 0, -560); 392 and 396 (532.9, 0, 560); 393 and 397
    // (-532.9, 0, 560). [U] that other arenas use the same offsets (the pieces are the same in bg00-04, 06-09; bg02 differs).
    if (key >= 390 && key <= 397 && first_mesh < m.meshes.size() && m.meshes[first_mesh].placement < 0) {
      float lim = 0;
      for (size_t k = first_mesh; k < m.meshes.size(); ++k)
        for (const Vertex& vx : m.meshes[k].vertices) lim = std::max(lim, std::max(std::fabs(vx.pos[0]), std::fabs(vx.pos[2])));
      if (lim <= 200.f) {
        static const float kx[4] = {561.f, -561.f, 532.9f, -532.9f}, kz[4] = {-560.f, -560.f, 560.f, 560.f};
        const int c = int((key - 390) & 3);
        Placement pl;
        pl.label = "slot " + std::to_string(key) + " corner";
        pl.transforms.push_back(MirrorY(TransFile(kx[c], 0, kz[c])));
        m.placements.push_back(std::move(pl));
        for (size_t k = first_mesh; k < m.meshes.size(); ++k) m.meshes[k].placement = int(m.placements.size()) - 1;
      }
    }
    // Session 97 (stage key): a slot with `m(N)` is stored near its own origin and placed by the key's motion N (entry
    // 30001): world = Rz Ry Rx * p + key position / 100 [C: the live draw probe's world matrices c76..c78 of the ring
    // truss pieces equal this for the position exactly and the rotation to 0.001-0.009]. Slots that also carry r / y keep
    // the mirror placement only (none do in the arenas checked).
    if (have_km && first_mesh < m.meshes.size() && m.meshes[first_mesh].placement < 0)
      if (auto mo = slot_motion.find(int(key)); mo != slot_motion.end()) {
        Matrix fm;
        if (stage_key::EvalMotion(key_m, mo->second, stage_key::Cfg().time, fm)) {
          Placement pl;
          pl.label = "slot " + std::to_string(key) + " m(" + std::to_string(mo->second) + ")";
          pl.transforms.push_back(MirrorY(fm));
          m.placements.push_back(std::move(pl));
          for (size_t k = first_mesh; k < m.meshes.size(); ++k) m.meshes[k].placement = int(m.placements.size()) - 1;
        } else {
          m.warnings.push_back("slot " + std::to_string(key) + ": no stage-key motion m(" + std::to_string(mo->second) + ")");
        }
      }
  }
  if (tex.Has("fade_lightmap00")) m.stage_lightmap = tex.Get(m, "fade_lightmap00");
  // c184 of the stage (light A/B, Session 97): measured live on arena 5 (draw probe of its entrance): (1/5600, 1/5750, 2.5, 1). The files
  // hold no matching float (scan of every top-level entry); the CTAB default span is 3500 (no arena measured uses it, see below).
  // Session 98 (generalisation 2): the same span in arena 1 (RAW, a raised stage): c184 = (1/5600, 1/5750, 2.5 | crowd 2.125) in every
  // light-mapped draw of probe run 6819 (frames 6881 / 10747) [C] -> one engine-wide span for every arena (two arenas measured, none
  // differs). Arena 1 used 1/3500 before: the stage light map (and the entrance pools drawn on it) landed 1.6x too far out.
  // S103 B3 (T3b_lightmap_writer.md): c184 is the game's own per-arena value, NOT a constant: sub_827F22C8 computes (1 / X, 1 / Z, G * 0.01, 1) from the arena's record in the player's
  // `pac\misc02_ingame.pac` leaf p0/102/5 (arena_lm.h). Read here at arena load, so the game, the viewer and the rig agree. No table (a missing file) = arena 4's record, counted.
  {
    const arena_lm::Param lp = arena_lm::Compute(ArenaLmTable(files), id, car::Current().bg);   // S103 CAR: a created arena borrows the real bg id the game keeps at [stage + 21192]
    m.lm_scale[0] = lp.v[0], m.lm_scale[1] = lp.v[1], m.lm_gain = lp.v[2];
    if (lp.clamped || lp.no_table) m.warnings.push_back("arena " + std::to_string(id) + ": LightMapParam " + (lp.no_table ? "table unreadable (misc02_ingame.pac p0/102/5)" : "id out of range") + ": arena 4's record");
    if (lp.zero) m.warnings.push_back("arena " + std::to_string(id) + ": LightMapParam record unused (0, 0, 0): gain 0, as the game");
  }
  {  // Session 98 (arena fix 2, stage_pools.h): the stage light map as a live RGBA8 page (the entrance adds the rig's light pools to it
     // every frame); the pool sprite = the arena's `lightmap00`. Without either the stored map is used as before.
    stage_pools::Cache& pc = stage_pools::TheCache();
    pc = stage_pools::Cache();
    std::vector<float> basef, spr;
    if (m.stage_lightmap >= 0 && stage_pools::Decode(m.textures[size_t(m.stage_lightmap)], basef)) {
      const Texture& bt = m.textures[size_t(m.stage_lightmap)];
      pc.w = int(bt.width), pc.h = int(bt.height);
      pc.base.resize(basef.size());
      for (size_t i = 0; i < basef.size(); ++i) pc.base[i] = uint8_t(std::clamp(basef[i], 0.f, 1.f) * 255.f + 0.5f);
      pc.lm_xy[0] = m.lm_scale[0], pc.lm_xy[1] = m.lm_scale[1];
      // Session 98 (match look, stage_pools.h): the match page = `fade_lightmap01` (same size), the settled end of the match's fade
      // (decoded on the CPU only: TexSet::Peek, no unused GPU copy; the base's size first: `bt` points into m.textures)
      const uint32_t bw = bt.width, bh = bt.height;
      if (tex.Has("fade_lightmap01")) {
        Texture mt;
        std::string why;
        std::vector<float> matchf;
        if (!tex.Peek("fade_lightmap01", mt, why)) {
          m.warnings.push_back("stage pools: `fade_lightmap01` " + why + " (the match keeps the entrance page)");
        } else if (mt.width != bw || mt.height != bh) {
          m.warnings.push_back("stage pools: `fade_lightmap01` is " + std::to_string(mt.width) + "x" + std::to_string(mt.height) + ", not the page's " +
                               std::to_string(bw) + "x" + std::to_string(bh) + " (the match keeps the entrance page)");
        } else if (!stage_pools::Decode(mt, matchf)) {
          m.warnings.push_back("stage pools: `fade_lightmap01` format " + std::to_string(int(mt.format)) + " not decoded (the match keeps the entrance page)");
        } else {
          pc.match.resize(matchf.size());
          for (size_t i = 0; i < matchf.size(); ++i) pc.match[i] = uint8_t(std::clamp(matchf[i], 0.f, 1.f) * 255.f + 0.5f);
        }
      }
      Texture live;
      live.name = stage_pools::LiveName(), live.format = TexFormat::kR8G8B8A8, live.width = bw, live.height = bh, live.mips = 1;
      live.data = pc.match.empty() ? pc.base : pc.match, live.live = true;  // (the default view is the match look; an entrance recomposes)
      m.textures.push_back(std::move(live));
      m.stage_lightmap = int(m.textures.size()) - 1;
      const int si = tex.HasOwn("lightmap00") ? tex.Get(m, "lightmap00") : -1;
      if (si >= 0 && stage_pools::Decode(m.textures[size_t(si)], spr)) {
        pc.sw = int(m.textures[size_t(si)].width), pc.sh = int(m.textures[size_t(si)].height);
        for (size_t i = 0; i < spr.size(); i += 4)
          for (int k = 0; k < 3; ++k) spr[i + size_t(k)] *= spr[i + 3];  // (rgb x alpha: the sprite's light)
        pc.sprite = std::move(spr);
      } else {
        m.warnings.push_back(si < 0 ? "stage pools: no `lightmap00` sprite (entrance pools not drawn)"
                                    : "stage pools: `lightmap00` format " + std::to_string(int(m.textures[size_t(si)].format)) + " not decoded (entrance pools not drawn)");
      }
      pc.ok = true;
    } else if (m.stage_lightmap >= 0) {
      m.warnings.push_back("stage pools: `fade_lightmap00` format " + std::to_string(int(m.textures[size_t(m.stage_lightmap)].format)) +
                           " not decoded (the stored map is used; entrance pools not drawn)");
    }
  }
  {  // Session 97 (polish; the matched-camera probe of the celebration, swap 22453 of run 069): the ring pieces carry `l(502)`, the
     // black placeholder the game blends to the current cue. In that frame every post / turnbuckle / cover / skirt draw (vertex
     // counts 40 / 119, 117 / 183, 95 / 91 / 139, 28-42) has exactly the colours of key set 31 and the three rope draws (1063A665,
     // 3170 and 11847 vertices) exactly those of set 32 (cols 0.800 0.800 0.692 / 0.800 / 0.800 / 1.774; sky 0.843 0.824 0.765,
     // ground 0.615): [C for arena 5 match mode; U that other arenas / cues pick the same two]. The entrance frame (swap 6728)
     // pushes its own tuple on the ring (an entrance-script override, not in the files): left as it was.
    const int ring_set = all_sets.count(31) ? resolve_set(31) : -1, rope_set = all_sets.count(32) ? resolve_set(32) : -1;
    for (auto& kv : ring_meshes) {
      // Session 97 (props builder): the canvas (key 953, VS 34E8768C in swap 22453 draw 748 / FE6B770B in swap 6728 draw 702) is lit by
      // key set 30, not the arena fallback: its constants c0..c3 = (0.5998 0.5998 0.4921) / 0.7998 / 0.7998 / (0.9998 0.8921 0.9998),
      // dirs (0.012 0.707 0.707) / (0.695 0.719 0) / (-0.719 0.695 0) / (-0.013 0.719 0.695), sky (1.133 1.133 1.176), ground 0.138
      // equal `stagekey.py --arena 5 --set 30` to 4 digits [C]; MatAmb (0.65 0.63 0.58) / MatDif 0.3 are the file's own (c189 / c190).
      // The swap 6728 entrance pushes its own tuple on it (entrance-script override), left to the entrance look.
      if (kv.first == 953 || kv.first == 1017) {   // (S103 CRR: 1017 = the alt canvas, the same 20-node model)
        const int canvas_set = all_sets.count(30) ? resolve_set(30) : -1;
        if (canvas_set >= 0)
          for (size_t k = kv.second.first; k < kv.second.second; ++k) m.meshes[k].light_set = canvas_set;
        continue;
      }
      const int want = (kv.first == 960 || RingHighTwin(int(kv.first)) == 960 || kv.first == 1021 || RingHighTwin(int(kv.first)) == 1021) ? rope_set : ring_set;  // S103 RLB2: the low rope 980 takes the rope set too
      if (want < 0) continue;
      for (size_t k = kv.second.first; k < kv.second.second; ++k) m.meshes[k].light_set = want;
    }
  }
  PlaceRing(ring_meshes, m);
  // S103 CEF: the steps / desk come from bgEtc.pac, which the game opens only on the match path, and only ring arenas (ring_meshes) are match
  // arenas; the CAW editor (BG66, no ring set) must not get them (the strip at the bottom that the 360 does not draw) [C: docs/research/s103/CEB.md].
  const bool add_props = !ring_meshes.empty() || load_detail::AlwaysPropsPlant();
  if (add_props) {  // Session 97 (props builder): the ring steps at two diagonal corners (see AddProp)
    static const float kA[12] = {0.70710678f, 0, -0.70710678f, 328.f, 0, 1, 0, 0, 0.70710678f, 0, 0.70710678f, -328.f};
    static const float kB[12] = {-0.70710678f, 0, -0.70710678f, 328.f, 0, 1, 0, 0, 0.70710678f, 0, -0.70710678f, 328.f};
    const size_t first = m.meshes.size();
    if (AddProp(files, std::getenv("WWE_STEPS") ? uint32_t(std::atoi(std::getenv("WWE_STEPS"))) : 32u /* S97: entry 32 = the pale grey steps of the game frame (29 blue, 35 dark); [U] which variant each arena uses; WWE_STEPS overrides */, {FromGameRows(kA), FromGameRows(kB)}, "ring steps", m, false)) {
      // (lighting: the probe's step draws use the yProp vertex shader like the ring posts (VS 962408F8), not the stage-lightmap
      // family: no lightmap). S103 RL: the props' light set is key set 50 (PropsLightSet below), not 100.
      const int ls = resolve_set(PropsLightSet(all_sets));
      for (size_t k = first; k < m.meshes.size(); ++k) {
        m.meshes[k].light_set = ls;
        if (m.meshes[k].material >= 0) m.materials[size_t(m.meshes[k].material)].has_lm_slot = false;
      }
    }
  }
  if (add_props) {  // Session 97 (props2): the announce desk: bgEtc PACH 9 entry 6 (ji_bl 274 / ji_meta 405 / ji_desk 37 vertices) and entry 7 (ji_mon 98 /
    // ji_top 10 / ji_bl 50), drawn by swap 20911 draws 1558-1563 with pure translations: entry 6 meshes 1-2 at (0, 0, -775.4), mesh 3 at
    // (0, 0, -760), entry 7 at (0, -48.25, -760) [C: world matrix rows c76..c78, vertex counts = vfetch bytes / 32 and index counts
    // matched]. [U] the same place in every arena.
    const size_t first = m.meshes.size();
    const std::vector<std::vector<Matrix>> desk = {{TransFile(0, 0, -775.4f)}, {TransFile(0, 0, -775.4f)}, {TransFile(0, 0, -760.f)}};
    const std::vector<std::vector<Matrix>> top = {{TransFile(0, -48.25f, -760.f)}};
    AddProp(files, 6, {}, "announce desk", m, false, &desk);
    AddProp(files, 7, {}, "announce desk top", m, false, &top);
    const int ls = resolve_set(PropsLightSet(all_sets));   // S103 RL: key set 50, as the steps
    for (size_t k = first; k < m.meshes.size(); ++k) {
      m.meshes[k].light_set = ls;
      if (m.meshes[k].material >= 0) m.materials[size_t(m.meshes[k].material)].has_lm_slot = false;
    }
  }
  const size_t crowd_first = m.meshes.size();
  AddCrowd(h, buf, car_id ? tex_part[0] : tex, chair_meshes, m, &files);   // (S103 CAR fix 2: the crowd is CASD's)
  for (size_t k = crowd_first; k < m.meshes.size(); ++k) m.meshes[k].src_pac = BaseName(ArenaRel(id));  // Session 98 (step 16)
  {  // Session 97: the crowd is lit by light set 20 (its decoded key values equal the crowd draws' live constants in
     // the match frame of run 069: three 0.396 lights, sky 0.278, ground 0.121) [C for arena 5].
    const int crowd_set = resolve_set(20);
    for (size_t k = crowd_first; k < m.meshes.size(); ++k) m.meshes[k].light_set = crowd_set;
  }
  // Session 98 (replay S3/S4): set 10 is the ENTRANT's set (viewer_main ApplyEntrance copies its lobe weights, colour w = c0..c3.w), but
  // no stage mesh uses it, so it never reached m.light_sets and the entrant got weights 0 (no specular lobes). Kept (unused by any mesh)
  // when the arena has it [C: arena 5 record 30002/10 colour w = (0.6275, 0.3529, 1.451, 0.2745) = the eye / skin / gear draws' c0..c3.w in
  // probe runs 6816 / 6818 and capture rp_ringin, every frame].
  if (auto s10 = all_sets.find(10); s10 != all_sets.end() && !set_index.count(10)) {
    m.light_sets.push_back(s10->second);
    set_index[10] = int(m.light_sets.size()) - 1;
  }
  {  // Session 98 (flash fix, crowd_flash.h): the crowd's camera-flash sprite = the arena pac's entry 31000 [C: the vertex bytes of the
     // game's flash draws (run 6818, VS 1D3A5C66) are this JBOY's, in every arena pac]. Not drawn in the scene: the renderer's flash pass
     // places it after the post chain (part name "crowd flash").
    if (const pac::Ent* e = pac::Find(h, 31000u)) {
      Bytes blob = pac::Decode(buf.data() + e->off, e->size);
      if (IsJboy(blob)) {
        JboyOptions o;
        o.arena = true;
        o.part = "crowd flash 31000";
        o.kind = 1, o.key = 31000, o.pac = BaseName(ArenaRel(id));
        o.tex = car_id ? &tex_part[2] : &tex;   // (CARG's flash)
        std::string e2;
        if (!LoadJboy(blob, o, m, e2)) m.warnings.push_back("crowd flash 31000 failed: " + e2);
      }
    }
  }
  FinishModel(m);
  if (models == 0 || m.meshes.empty()) return err = std::string(lab) + ": no stage models loaded", false;
  for (uint32_t gk = 60010; gk <= 60025; ++gk) {  // S103 V1: slot-cycle groups (docs\formats\stage_visibility.md section 3)
    const pac::Ent* ge = pac::Find(h, gk);
    if (!ge) continue;
    m.cycle_groups = true;
    // S103 UVF: the cycled slots = the even-position integers of the group's "slot ticks" text (the same parse as tools\harvest\choice_predict.py
    // cycled_slots(): slot, ticks, slot, ticks ...). The game's builder sub_824E82A0 (0x824E8554-0x824E8578) gives each the no-pass bit and the cycle task
    // sub_83420EA0 draws it without setting the selector word: read_rule::Selector returns the outer selector for them.
    const Bytes txt = pac::Decode(buf.data() + ge->off, ge->size);
    int pos = 0;
    std::vector<int> grp;   // S103 CAR: this group's slots, in text order
    for (size_t i = 0; i < txt.size();) {
      const bool neg = txt[i] == '-' && i + 1 < txt.size() && txt[i + 1] >= '0' && txt[i + 1] <= '9';
      if (!neg && !(txt[i] >= '0' && txt[i] <= '9')) { ++i; continue; }
      long v = 0;
      for (i += neg ? 1 : 0; i < txt.size() && txt[i] >= '0' && txt[i] <= '9'; ++i) v = v * 10 + (txt[i] - '0');
      if ((pos++ & 1) == 0) slot_rec[neg ? int(-v) : int(v)].cycled = true, grp.push_back(neg ? int(-v) : int(v));
    }
    if (!grp.empty()) m.cycle_slot_groups.push_back(std::move(grp));
  }
  if (const pac::Ent* e = pac::Find(h, 50020)) {  // S103 V1: the visibility tree -> a box per stage slot (stage_tree.h); a slot the stage key moves (m(N)) is never boxed
    std::set<int> moving, mirrored;
    for (const auto& kv : slot_motion) moving.insert(kv.first);
    for (const auto& kv : slot_mirror)
      if (kv.second & 1) mirrored.insert(kv.first);
    const Bytes tree_txt = pac::Decode(buf.data() + e->off, e->size);
    stage_tree::Fill(m, std::string_view(reinterpret_cast<const char*>(tree_txt.data()), tree_txt.size()), moving, mirrored);
  }
  {  // Session 97: the stage movies (keys 800-809 draw video frames; stage_media.h)
    const std::string note = stage_media::Apply(m, id);
    if (!note.empty() && !stage_media::Cfg().dir.empty()) m.warnings.push_back(note);  // (only for an explicit folder: a default cache must not flag the census)
  }
  if (stage_media::Cfg().wrestler >= 0 && stage_media::Cfg().ent_light) {
    // Session 97 (entrance_light.h): the entrance look takes the wrestler's own entrance key for each arena set's rig slot:
    // colours / sky / ground replaced, the arena set's DIRECTIONS kept (the key's directions are unproven, [U]).
    std::string le;
    const entrance_light::Rig rig = entrance_light::Load(files, stage_media::Cfg().wrestler, le);
    if (!rig.ok) {
      m.warnings.push_back("entrance light: " + le + " (arena lighting kept)");
    } else {
      for (LightSet& ls : m.light_sets) {
        LightSet k;
        if (!entrance_light::EvalForSet(rig, ls.id, int(stage_media::Cfg().ent_cue), stage_media::Cfg().ent_t, k)) continue;
        std::memcpy(ls.col, k.col, sizeof ls.col);
        std::memcpy(ls.sky, k.sky, sizeof ls.sky);
        std::memcpy(ls.ground, k.ground, sizeof ls.ground);
      }
    }
  }
  m.load_ms = NowMs() - t0;
  return true;
}

// S103 CAR (BCA G1 piece 1): a Create-an-Arena venue = CASD n + CAEN n + CARG 0 + CARL n of pac\car\ merged (car_arena.h) and read by LoadArena exactly like a bg pac. `sel` = the three part indices the game
// built the stage from (stage_fx_contrib.h reads them at the latch) + the real bg id it borrows. The merge is made by OpenCached under this thread's selection; the Model carries its key (custom_key) so the applier can
// tell a model of another selection. NOT done (BCA [U]): the player's paint (TextureSetup_CAR, /CART/TEXP, /CARM/TSnn): the parts draw with their default textures.
inline bool LoadCustomArena(launcher::GameFiles& files, int id, const car::Sel& sel, Model& m, std::string& err) {
  if (!car::IsCustomId(id)) return err = "arena " + std::to_string(id) + " is not a created arena (200-249)", false;
  if (!sel.valid) return err = "created arena " + std::to_string(id) + ": its part indices were not read", false;
  if (sel.arena != id) return err = "created arena " + std::to_string(id) + ": the selection is for arena " + std::to_string(sel.arena), false;
  const car::ScopedThreadSel scoped(sel);
  if (!LoadArena(files, id, m, err)) return false;
  m.custom_key = sel.Key();
  m.label = "created arena " + std::to_string(id) + " (CAEN " + std::to_string(sel.caen) + ", CASD " + std::to_string(sel.casd) + ", CARL " + std::to_string(sel.carl) + ")";
  return true;
}

}  // namespace assets
