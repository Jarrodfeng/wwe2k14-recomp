// S111 OLDCHAIN (docs\research\s111\OLDCHAIN.md): the effect technique tables from the player's own files, built once per process and published
// (read_rule.h EffectTableNow / LogoEffectTableNow / StsEffectTableNow, sampler_table.h). Moved here from live_assets.cpp (S103 D1) so the game
// (GameFilesSource::SetRoot) and the Arena Viewer's rdr backend (renderer_rdr.cpp, from its own --game files) publish the SAME tables: a wrestler
// (yCh_*) material is drawn only with its table row (char_effects.cpp ChooseCharEffectTable); there is no other pick.
// A failed build (no pac\_startup.pac, or no table parsed) says ONE loud line through `say` (NoTableLine) and publishes nothing; every wrestler draw
// is then a counted hole (front_end.cpp: char_hole_no_table per bind, char_hole_draws per draw, both on the census line).
// Header only. The includer has stb_image.h's declarations included first (pac.h's zlib); xxHash is the SDK's third-party copy or any -I that holds it.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#if __has_include("../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h")
#include "../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h"
#elif __has_include(<xxHash/xxhash.h>)
#include <xxHash/xxhash.h>
#else
#include <xxhash.h>
#endif

#include "../assets/pac.h"
#include "read_rule.h"
#include "sampler_table.h"

namespace rdr_scene {
namespace read_rule {

using Say = std::function<void(const std::string&)>;

// TEST ONLY planted twins (tools\scene\oldchain_unit.cpp --plant; never set by the game or the viewer).
inline std::atomic<bool> g_plant_silent_no_table{false};    // a failed build says nothing (the old silent fallback for the whole session)
inline std::atomic<bool> g_plant_viewer_no_publish{false};  // the viewer publishes no table (the old rdr backend: every wrestler took the old pick)

// The one line a failed startup-table build says.
inline std::string NoTableLine(const std::string& why) {
  return "[read_rule] NO EFFECT TABLE (" + why + "): every wrestler draw is a counted HOLE (census: char_hole_no_table per bind, char_hole_draws per draw); "
         "the game itself cannot draw without pac\\_startup.pac";
}

inline bool SayNoTable(const Say& say, const std::string& why) {
  if (!g_plant_silent_no_table.load(std::memory_order_relaxed)) say(NoTableLine(why));
  return false;
}

// The startup table (the effect-table leaf: tools\shaders\effect_table.py load_all's rule, every top-level entry holding a shader container whose head
// parses as the table; the first effect of a name wins), its sampler blocks (S103 B1), then the editors' tables of pac\edit\edit.pac (S105 A5c, S108 THR2).
// Built once per process. true = a startup table is published (now or by an earlier call).
inline bool BuildEffectTable(launcher::GameFiles& files, const Say& say) {
  if (EffectTableNow()) return true;
  static std::mutex build_mu;
  std::lock_guard<std::mutex> l(build_mu);
  if (EffectTableNow()) return true;
  char line[256];
  assets::pac::Pac p;
  std::string err;
  if (!assets::pac::Open(files, "pac\\_startup.pac", p, err)) return SayNoTable(say, "no pac\\_startup.pac: " + err);
  auto table = std::make_shared<EffectTable>();
  auto samplers = std::make_shared<sampler_table::Table>();   // S103 B1: the effect passes' sampler state, from the same leaf
  uint32_t sampler_conflicts = 0;
  const auto hash = [](const uint8_t* d, size_t n) { return uint64_t(XXH3_64bits(d, n)); };
  for (const assets::pac::Pach& pach : p.pachs)
    for (const assets::pac::Ent& e : pach.ents) {
      const assets::Bytes dec = assets::pac::Decode(p.bytes.data() + e.off, e.size);
      static const uint8_t kMagic[3] = {0x10, 0x2A, 0x11};
      if (dec.size() < 16 || std::search(dec.begin(), dec.end(), kMagic, kMagic + 3) == dec.end()) continue;
      EffectTable part;
      if (!ParseEffectLeaf(dec.data(), dec.size(), hash, part)) continue;
      for (auto& kv : part) table->emplace(kv.first, std::move(kv.second));
      sampler_table::Table sp;
      if (sampler_table::ParseLeaf(dec.data(), dec.size(), sp, &sampler_conflicts))
        for (auto& kv : sp) samplers->emplace(kv.first, std::move(kv.second));
    }
  if (table->empty()) return SayNoTable(say, "pac\\_startup.pac: no effect table parsed");
  std::snprintf(line, sizeof line, "[read_rule] effect technique table: %zu effects from pac\\_startup.pac", table->size());
  say(line);
  size_t with_samplers = 0;
  for (const auto& kv : *samplers) with_samplers += kv.second.any;
  std::snprintf(line, sizeof line, "[read_rule] sampler table: %zu effects with a sampler block, %u pass disagreements", with_samplers, unsigned(sampler_conflicts));
  say(line);
  sampler_table::Publish(std::move(samplers));
  PublishEffectTable(std::move(table));
  // S105 A5c fix F1: the editor's table, pac\edit\edit.pac leaf p7/0 (tools\shaders\effect_table.py names it "p7/0": top-level PACH 7, entry 0). Its yCh rows are the logo variants (and drop bss / sweat); a CAW material
  // with a logo takes its row from here. A failure leaves the startup row in force (no logo drawn).
  assets::pac::Pac ep;
  std::string eerr;
  if (!assets::pac::Open(files, "pac\\edit\\edit.pac", ep, eerr)) {
    say("[read_rule] no editor effect table: " + eerr);
    return true;
  }
  // S108 THR2: the editors' tables share one reader: top-level PACH `pach`, entry 0 (p7/0 = /EDIT/CSS, the CAW logo rows; p9/0 = /EDIT/STS, the Threads change-colour rows).
  const auto editor_leaf = [&](size_t pach, const char* what) -> std::shared_ptr<EffectTable> {
    if (ep.pachs.size() <= pach || ep.pachs[pach].ents.empty()) {
      std::snprintf(line, sizeof line, "[read_rule] edit.pac: no top-level entry p%zu/0 (%s)", pach, what);
      say(line);
      return nullptr;
    }
    const assets::pac::Ent& le = ep.pachs[pach].ents[0];
    const assets::Bytes ldec = assets::pac::Decode(ep.bytes.data() + le.off, le.size);
    auto ltable = std::make_shared<EffectTable>();
    if (!ParseEffectLeaf(ldec.data(), ldec.size(), hash, *ltable)) {
      std::snprintf(line, sizeof line, "[read_rule] edit.pac p%zu/0: no effect table parsed (%s)", pach, what);
      say(line);
      return nullptr;
    }
    std::snprintf(line, sizeof line, "[read_rule] editor (%s) effect table: %zu effects from pac\\edit\\edit.pac p%zu/0", what, ltable->size(), pach);
    say(line);
    return ltable;
  };
  if (auto lt = editor_leaf(7, "logo")) PublishLogoEffectTable(std::move(lt));
  if (auto st = editor_leaf(9, "threads change colour")) PublishStsEffectTable(std::move(st));   // a failure: no body is drawn recoloured (the startup rows)
  return true;
}

// The Arena Viewer's rdr backend (renderer_rdr.cpp): the same tables from the viewer's own game files (loader_seam.h ViewerGameFiles; null = none:
// the loud line, nothing published). true = a startup table is published.
inline bool PublishFromViewerFiles(launcher::GameFiles* files, const Say& say) {
  if (!files) return SayNoTable(say, "the viewer has no game files (--game)");
  if (g_plant_viewer_no_publish.load(std::memory_order_relaxed)) return false;
  return BuildEffectTable(*files, say);
}

}  // namespace read_rule
}  // namespace rdr_scene
