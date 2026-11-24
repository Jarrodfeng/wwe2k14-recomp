// S108 lane THR: census of the Superstar Threads editor's effect table (pac\edit\edit.pac PACH 9 = /EDIT/STS, the pack the editor's constructor sub_8339E020 requests into the second effect
// registry [0x83929A94], which the model draw uses for model+392 while model+604 is set). Reads the PLAYER's file at run time with the production parsers
// (assets::pac::OpenBytes / Decode, read_rule::ParseEffectLeaf, the shader registry's hash table); prints, per yCh effect, every technique / pass pixel shader
// as our registry names it, and the verdict: does each yCh effect's table carry change-colour fragments? Nothing from the file is stored or committed.
//   thr_census.exe <path to edit.pac> [pach index, default 9]
// Build: pwsh -NoProfile -File tools\scene\build_thr_census.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/pac.h"
#include "../../src/renderer_scene/read_rule.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"

namespace rr = rdr_scene::read_rule;

static std::vector<std::string> NamesOf(uint64_t h) {
  std::vector<std::string> out;
  const auto* first = std::begin(rdr::fx::kHashes);
  const auto* last = std::end(rdr::fx::kHashes);
  const auto* it = std::lower_bound(first, last, h, [](const rdr::fx::HashEntry& e, uint64_t v) { return e.hash < v; });
  for (; it != last && it->hash == h; ++it)
    if (rdr::fx::kVariants[it->variant].name) out.push_back(rdr::fx::kVariants[it->variant].name);
  return out;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: thr_census.exe <edit.pac> [pach]\n"); return 2; }
  const int pach = argc > 2 ? std::atoi(argv[2]) : 9;
  std::ifstream f(argv[1], std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
  assets::Bytes raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  assets::pac::Pac p;
  std::string err;
  if (!assets::pac::OpenBytes(std::move(raw), "edit.pac", p, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  if (size_t(pach) >= p.pachs.size() || p.pachs[size_t(pach)].ents.empty()) { std::fprintf(stderr, "no PACH %d\n", pach); return 2; }
  const assets::pac::Ent& e = p.pachs[size_t(pach)].ents[0];
  const assets::Bytes dec = assets::pac::Decode(p.bytes.data() + e.off, e.size);
  rr::EffectTable t;
  const auto hash = [](const uint8_t* d, size_t n) { return uint64_t(XXH3_64bits(d, n)); };
  if (!rr::ParseEffectLeaf(dec.data(), dec.size(), hash, t)) { std::fprintf(stderr, "PACH %d: no effect table\n", pach); return 2; }
  std::printf("edit.pac PACH %d: %zu effects\n", pach, t.size());
  std::map<std::string, const rr::EffectTech*> sorted;
  for (const auto& kv : t) sorted[kv.first] = &kv.second;
  int ych = 0, ych_cc = 0, ych_rows = 0, ych_rows_cc = 0;
  for (const auto& kv : sorted) {
    const rr::EffectTech& et = *kv.second;
    const bool is_ych = kv.first.rfind("ych_", 0) == 0;
    int rows = 0, rows_cc = 0, unknown = 0;
    std::set<std::string> kinds;
    for (size_t ti = 0; ti < et.techs.size(); ++ti)
      for (size_t pi = 0; pi < et.techs[ti].size(); ++pi) {
        const rr::PassShaders& s = et.techs[ti][pi];
        if (!s.ps_ok) continue;
        ++rows;
        const std::vector<std::string> n = NamesOf(s.ps);
        if (n.empty()) { ++unknown; continue; }
        bool cc = false;
        for (const std::string& x : n) cc = cc || x.find("change_color") != std::string::npos || x.find("_camo") != std::string::npos;
        rows_cc += cc;
        if (ti == 0 && pi == 0) kinds.insert(n[0]);
      }
    std::printf("  %-20s techniques %zu, pixel rows %d, change-colour rows %d, not in our registry %d; T0 p0 = %s\n", et.name.c_str(), et.techs.size(), rows, rows_cc, unknown,
                kinds.empty() ? "-" : kinds.begin()->c_str());
    if (is_ych) ++ych, ych_rows += rows, ych_rows_cc += rows_cc, ych_cc += rows_cc > 0;
  }
  std::printf("VERDICT: yCh effects %d, with change-colour rows %d; yCh pixel rows %d, change-colour %d\n", ych, ych_cc, ych_rows, ych_rows_cc);
  return 0;
}
