// See prop_game_space.h. S101 lane 84.
// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like ring_game_space.cpp.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <cmath>
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "prop_game_space.h"

#include <filesystem>
#include <map>

#include "asset_load.h"

namespace assets::prop_gs {
namespace {
launcher::GameFiles g_files;
bool g_ready = false;

// The model fingerprints (vertex counts of the mesh records in order) of the evtobj.pac objects, measured on the whole pac (S101 lane 79 / lane 84: 17 table PACHs, 3 microphones).
const std::vector<uint32_t> kTableCounts = {284, 92, 652, 38, 385, 99, 29};
const std::vector<uint32_t> kMicCounts = {10, 24, 171, 272};

// A leaf's texture directories (entries 10..19 of its PACH) and its model (entry 0), decoded, added to `out` as source `src`.
bool AddSource(PropSet& out, const Bytes& buf, const pac::Pach& h, const Source& src, std::shared_ptr<Bytes> blob, const Bytes* parent_dir = nullptr) {   // lane 137: parent_dir = a texture directory that sits beside the model PACH (the cage: entry 0 of top-level PACH 1 / 2)
  TexSet tex;
  if (parent_dir && parent_dir->size() >= 0x30) tex.AddDir(Bytes(*parent_dir));   // lane 137
  for (const pac::Ent& e : h.ents)
    if (e.id >= 10 && e.id <= 19 && e.size >= 0x30) tex.AddDir(pac::Decode(buf.data() + e.off, e.size));
  jboy_parts::Parts parts;
  std::string perr;
  if (!jboy_parts::Parse(*blob, parts, perr)) return false;
  Model model;
  JboyOptions o;
  o.arena = true;
  o.part = src.label;
  o.kind = 1, o.key = src.id, o.pac = "props";
  o.tex = &tex;
  std::string e2;
  {   // review F1: the loader's process-wide mutex only around the asset_load.h call (TexSet / LoadJboy keep statics); the pac open / decode / scan below take none (pac.h, xctd_file.h, jboy_parts are pure)
    std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());
    if (!LoadJboy(*blob, o, model, e2)) return false;
  }
  std::map<int, int> remap;
  auto map_tex = [&](int idx) -> int {
    if (idx < 0 || size_t(idx) >= model.textures.size()) return -1;
    auto it = remap.find(idx);
    if (it == remap.end()) {
      out.textures.push_back(model.textures[size_t(idx)]);
      it = remap.emplace(idx, int(out.textures.size()) - 1).first;
    }
    return it->second;
  };
  bool any = false;
  for (const Mesh& me : model.meshes) {
    if (me.src_record < 0 || size_t(me.src_record) >= parts.parts.size()) continue;
    const jboy_parts::Part& pt = parts.parts[size_t(me.src_record)];
    if (!pt.ok || !pt.tiles) continue;
    ring_gs::RingMesh r;
    r.entry = src.id, r.part = me.src_record, r.effect = pt.effect, r.blob = blob, r.data = pt, r.vertices = pt.vertices, r.pairs = pt.stride[1] / 8;
    if (me.material >= 0 && size_t(me.material) < model.materials.size()) {
      const Material& mt = model.materials[size_t(me.material)];
      r.diffuse = map_tex(mt.diffuse), r.normal_map = map_tex(mt.normal), r.specular = map_tex(mt.specular);
    }
    for (uint32_t v = 0; v < pt.vertices && (r.first_neg_u < 0 || r.first_strict_neg_u < 0); ++v) {   // S101 lane 220: the SIGN bit (a file u of -0.0 is stored signed in the stream, patch 109), and separately the first STRICTLY negative one
      const float fu = BeF(pt.s[2] + 8ull * v);
      if (r.first_neg_u < 0 && std::signbit(fu)) r.first_neg_u = int(v);
      if (r.first_strict_neg_u < 0 && fu < 0.0f) r.first_strict_neg_u = int(v);
    }
    out.meshes.push_back(std::move(r));
    any = true;
  }
  return any;
}

bool Counts(const jboy_parts::Parts& p, const std::vector<uint32_t>& want) {
  if (p.parts.size() != want.size()) return false;
  for (size_t i = 0; i < want.size(); ++i)
    if (p.parts[i].vertices != want[i]) return false;
  return true;
}

int NewSource(PropSet& out, Group g, const std::string& label, bool family32 = false) {
  Source s;
  s.id = int(out.sources.size()), s.group = g, s.label = label, s.family32 = family32;
  out.sources.push_back(s);
  return s.id;
}

// A nested leaf: top-level PACH `pach` of `p`, keys `k1` (and `k2` when >= 0): the PACH whose entry 0 is the model.
bool Nested(const pac::Pac& p, size_t pach, uint32_t k1, int k2, Bytes& nb, pac::Pach& np) {
  if (pach >= p.pachs.size()) return false;
  Bytes b1;
  pac::Pach p1;
  if (!pac::GetNested(p.bytes, p.pachs[pach], k1, b1, p1)) return false;
  if (k2 < 0) return nb = std::move(b1), np = p1, true;
  return pac::GetNested(b1, p1, uint32_t(k2), nb, np);
}
}  // namespace

bool Init(const std::string& game_path) {
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
  return g_ready;
}

bool Load(PropSet& out, std::string& err) {
  if (!g_ready) return err = "prop_game_space: no game files (Init first)", false;
  out = PropSet{};
  // ---- the steps: bgEtc.pac and m2.pac (the 10 leaves) ----
  struct Step {
    const char* rel;
    const char* tag;
    size_t pach;
    int outer;   // -1: the key is directly in the PACH; else the nested PACH key (m2: 1000)
    uint32_t key;
  };
  const Step steps[] = {{"pac\\bg\\bgEtc.pac", "bgEtc", 9, -1, 29}, {"pac\\bg\\bgEtc.pac", "bgEtc", 9, -1, 32}, {"pac\\bg\\bgEtc.pac", "bgEtc", 9, -1, 35}, {"pac\\bg\\bgEtc.pac", "bgEtc", 62, -1, 32},
                        {"pac\\m2.pac", "m2", 0, 1000, 29},       {"pac\\m2.pac", "m2", 0, 1000, 32},       {"pac\\m2.pac", "m2", 0, 1000, 35},
                        {"pac\\m2.pac", "m2", 2, 1000, 29},       {"pac\\m2.pac", "m2", 2, 1000, 32},       {"pac\\m2.pac", "m2", 2, 1000, 35}};
  std::string last_rel;
  pac::Pac pc;
  bool bgetc_family = false;   // review F1: m2.pac (31 MB) holds the same leaves; it is only read when bgEtc.pac gave no "32" family leaf
  for (const Step& st : steps) {
    if (st.outer >= 0 && bgetc_family) continue;
    if (last_rel != st.rel) {
      pc = pac::Pac{};
      last_rel = st.rel;
      std::string e;
      if (!pac::Open(g_files, st.rel, pc, e)) {
        err = e;
        last_rel.clear();
        continue;
      }
    }
    Bytes nb;
    pac::Pach np;
    bool ok;
    if (st.outer < 0) {
      ok = pac::GetNested(pc.bytes, pc.pachs.size() > st.pach ? pc.pachs[st.pach] : pac::Pach{}, st.key, nb, np);
    } else {
      Bytes b1;
      pac::Pach p1;
      ok = st.pach < pc.pachs.size() && pac::GetNested(pc.bytes, pc.pachs[st.pach], uint32_t(st.outer), b1, p1) && pac::GetNested(b1, p1, st.key, nb, np);
    }
    if (!ok) continue;
    auto blob = std::make_shared<Bytes>(pac::Get(nb, np, 0));
    if (blob->size() < 0x48) continue;
    char lab[64];
    if (st.outer < 0) std::snprintf(lab, sizeof lab, "%s p%zu/%u", st.tag, st.pach, st.key);
    else std::snprintf(lab, sizeof lab, "%s p%zu/%d/%u", st.tag, st.pach, st.outer, st.key);
    const bool fam = st.key == 32 && !(st.pach == 62);
    const int id = NewSource(out, Group::Steps, lab, fam);
    if (!AddSource(out, nb, np, out.sources[size_t(id)], blob)) out.sources.pop_back();
    else if (fam && st.outer < 0) bgetc_family = true;
  }
  {   // ---- lane 137 (WHATS_LEFT N9): the scene props of bgEtc.pac that WM30 draws at the colour site (found by bytes: tools\assets\find_bytes.py + the capture's first positions) ----
    // PACH path in the pac's own naming (tools\wm30\pac.py): "p1/1/3" = top-level PACH 1, nested key 1, entry 3 = the JBOY model; "p9/14/14/0" = PACH 9, nested keys 14 then 14, entry 0.
    // cage_03 steel cage: the wall panel (1088 v / 3316 i, drawn 3x by quarter turns), the panel with the door (1200 / 3587) and the door (240 / 546); PACHs 1 and 2 are twins: vertex + index bytes identical, but the model
    // blobs differ in one material float4 (the stream's materials are used, not the file's) and their cage_03 pictures differ (review 3), so the twin is chosen by the picture content. The cage_03 picture sits in entry 0 of the
    // TOP-LEVEL PACH 1 / 2 (a texture directory beside nested key 1), not in the nested PACH: it is fed in as the parent directory (review 4). cage_wak_1 (a second name in that directory) is not used here [U].
    // p9 key 14 / k: 91 siblings (k 0-45, 86-90, ~18 KB each, one prop with a different mesh per k; the WM30 object is k = 14), loaded whole (review 5): only the counts + positions + bytes pick one.
    // ji_* the broadcast (commentary) desk set: desk + chairs (148 / 168 / 46 / 244 v), keys 105 / 106, and its monitors (98 / 10 / 50 / 26 v), keys 7 / 107 and PACH 62 key 7; k_color: one prop (384 v).
    struct Scn {
      size_t pach;
      uint32_t k1;
      int k2;
      uint32_t entry;
    };
    const Scn scn[] = {{1, 1, -1, 0}, {1, 1, -1, 3}, {1, 1, -1, 6}, {2, 1, -1, 0}, {2, 1, -1, 3}, {2, 1, -1, 6}, {9, 105, -1, 0}, {9, 106, -1, 0},
                       {9, 7, -1, 0}, {9, 107, -1, 0}, {62, 7, -1, 0}};
    pac::Pac bg_local;
    const pac::Pac* bgp = &pc;
    if (last_rel != "pac\\bg\\bgEtc.pac") {   // the steps loop ended on m2.pac (bgEtc gave no 32-family leaf): open bgEtc for this block
      std::string e;
      bgp = pac::Open(g_files, "pac\\bg\\bgEtc.pac", bg_local, e) ? &bg_local : nullptr;
      if (!bgp) err = e;
    }
    std::vector<Scn> all(std::begin(scn), std::end(scn));
    for (int k2 = 0; k2 <= 90; ++k2) all.push_back({9, 14, k2, 0});   // review 5: the whole p9/14/* family (absent keys are skipped by Nested)
    for (const Scn& s : all) {
      if (!bgp) break;
      Bytes nb;
      pac::Pach np;
      if (!Nested(*bgp, s.pach, s.k1, s.k2, nb, np)) continue;
      auto blob = std::make_shared<Bytes>(pac::Get(nb, np, s.entry));
      if (blob->size() < 0x48 || std::memcmp(blob->data(), "JBOY", 4) != 0) continue;
      char lab[64];
      if (s.k2 < 0) std::snprintf(lab, sizeof lab, "bgEtc p%zu/%u/%u", s.pach, s.k1, s.entry);
      else std::snprintf(lab, sizeof lab, "bgEtc p%zu/%u/%d/%u", s.pach, s.k1, s.k2, s.entry);
      Bytes parent_dir;   // review 4: the cage's picture directory is entry 0 of the top-level PACH
      if (s.pach <= 2 && s.pach < bgp->pachs.size()) parent_dir = pac::Get(bgp->bytes, bgp->pachs[s.pach], 0);
      const int id = NewSource(out, Group::Scene, lab);
      if (!AddSource(out, nb, np, out.sources[size_t(id)], blob, parent_dir.empty() ? nullptr : &parent_dir)) out.sources.pop_back();
    }
  }
  pc = pac::Pac{};   // free bgEtc / m2 before the 162 MB evtobj is opened
  // ---- the commentary table and the microphones: evtobj.pac, found by the model fingerprint ----
  {
    pac::Pac ev;
    std::string e;
    if (!pac::Open(g_files, "pac\\evt\\evtobj.pac", ev, e)) {
      err = e;
    } else {
      for (size_t i = 0; i < ev.pachs.size(); ++i) {
        const pac::Ent* en = pac::Find(ev.pachs[i], 0);
        if (!en || en->off + en->size > ev.bytes.size()) continue;
        auto blob = std::make_shared<Bytes>(pac::Decode(ev.bytes.data() + en->off, en->size));
        if (blob->size() < 0x48 || std::memcmp(blob->data(), "JBOY", 4) != 0) continue;
        jboy_parts::Parts parts;
        std::string pe;
        if (!jboy_parts::Parse(*blob, parts, pe)) continue;
        const bool table = Counts(parts, kTableCounts), mic = !table && Counts(parts, kMicCounts);
        if (!table && !mic) continue;
        const int id = NewSource(out, table ? Group::Table : Group::Mic, "evtobj PACH " + std::to_string(i));
        if (!AddSource(out, ev.bytes, ev.pachs[i], out.sources[size_t(id)], blob)) out.sources.pop_back();
      }
    }
  }
  if (out.meshes.empty()) return err = "prop_game_space: no prop source found" + (err.empty() ? std::string() : " (" + err + ")"), false;
  err.clear();
  return true;
}

}  // namespace assets::prop_gs
