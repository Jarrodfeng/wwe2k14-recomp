// See live_assets.h. Session 103, lane S.
#include "live_assets.h"
#include "load_order.h"   // S105 SKB fix F3: kEvtobjPrewarmEntry

// stb_image compiled INTO this translation unit, static (crowd_body.cpp's rule: no stbi_* symbol leaves it; the game exe and the phone
// library must not export stbi_*). asset_load.h's decoders use it.
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
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>

#include "../assets/asset_load.h"
#include "../assets/attire_compose.h"     // S103 CMB: the composite attire's host compose
#include "../assets/attire_remap.h"       // S103 ARM: the game's attire remap (pac\misc01_Start.pac p0/600)
#include "../assets/attire_build.h"       // S103 ARM2: the colours the game painted with (build-step latch); the census counts
#include "named_load_fail.h"              // S103 ARM: MarkHole (a failed composite / remapped attire stays a hole)
#include "../assets/crowd_body.h"
#include "../assets/particle_sprites.h"   // S103 F5
#include "../renderer/problem.h"          // S103 BLF: rdr::Log (the run log)
#include "weapon_draw_core.h"             // S103 BLF: g_evtobj_release
#include "read_rule.h"                    // S103 D1
#include "../renderer_adapter/rdr_switches.h"   // S103 WMB: wwe2k14_rdr_borrow_textures (no game or SDK includes)
#include "wrestler_src.h"                 // S103 DLW: the game's DLC packs for the live loader + the census word
#include "sampler_table.h"                // S103 B1

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#if __has_include("../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h")
#include "../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h"
#else
#include <xxHash/xxhash.h>
#endif

namespace rdr_scene {

namespace read_rule {
// S103 D1: the effect technique table from the player's pac\_startup.pac (the effect-table leaf: tools\shaders\effect_table.py load_all's rule, every top-level entry
// holding a shader container whose head parses as the table; the first effect of a name wins). Built once per process; a failure leaves the old heuristics in force.
static void BuildEffectTable(launcher::GameFiles& files) {
  if (EffectTableNow()) return;
  static std::mutex build_mu;
  std::lock_guard<std::mutex> l(build_mu);
  if (EffectTableNow()) return;
  assets::pac::Pac p;
  std::string err;
  if (!assets::pac::Open(files, "pac\\_startup.pac", p, err)) { std::fprintf(stderr, "[read_rule] no effect table: %s\n", err.c_str()); return; }
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
  if (table->empty()) { std::fprintf(stderr, "[read_rule] pac\\_startup.pac: no effect table parsed\n"); return; }
  std::fprintf(stderr, "[read_rule] effect technique table: %zu effects from pac\\_startup.pac\n", table->size());
  size_t with_samplers = 0;
  for (const auto& kv : *samplers) with_samplers += kv.second.any;
  std::fprintf(stderr, "[read_rule] sampler table: %zu effects with a sampler block, %u pass disagreements\n", with_samplers, sampler_conflicts);
  sampler_table::Publish(std::move(samplers));
  PublishEffectTable(std::move(table));
  // S105 A5c fix F1: the editor's table, pac\edit\edit.pac leaf p7/0 (tools\shaders\effect_table.py names it "p7/0": top-level PACH 7, entry 0). Its yCh rows are the logo variants (and drop bss / sweat); a CAW material
  // with a logo takes its row from here. A failure leaves the startup row in force (no logo drawn, as before).
  assets::pac::Pac ep;
  std::string eerr;
  if (!assets::pac::Open(files, "pac\\edit\\edit.pac", ep, eerr)) { rdr::Log("[read_rule] no editor effect table: %s", eerr.c_str()); return; }
  if (ep.pachs.size() <= 7 || ep.pachs[7].ents.empty()) { rdr::Log("[read_rule] edit.pac: no top-level entry p7/0"); return; }
  const assets::pac::Ent& le = ep.pachs[7].ents[0];
  const assets::Bytes ldec = assets::pac::Decode(ep.bytes.data() + le.off, le.size);
  auto ltable = std::make_shared<EffectTable>();
  if (!ParseEffectLeaf(ldec.data(), ldec.size(), hash, *ltable)) { rdr::Log("[read_rule] edit.pac p7/0: no effect table parsed"); return; }
  rdr::Log("[read_rule] editor (logo) effect table: %zu effects from pac\\edit\\edit.pac p7/0", ltable->size());
  PublishLogoEffectTable(std::move(ltable));
}
}  // namespace read_rule

struct GameFilesSource::Impl {
  launcher::GameFiles files;
  uint64_t overlay_gen = ~0ull;   // S103 DLW: the dlc_overlays generation this reader applied
};

GameFilesSource::GameFilesSource() : impl_(new Impl) {}
GameFilesSource::~GameFilesSource() { delete impl_; }

bool GameFilesSource::SetRoot(const std::string& game_root) {
  ready_ = false;
  if (game_root.empty()) return false;
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());   // S103 S: the shared pac cache (see Load)
  assets::attire_remap::Reset();   // S103 ARM: a new game folder re-reads the colour table
  assets::load_detail::EvtLogHook() = [](const std::string& line) { rdr::Log("[wwe2k14][weapon_rows] %s", line.c_str()); };   // S105 SKB3 fix L1 / L5: the evtobj.pac fallbacks reach the run log
  impl_->files.SetRoot(std::filesystem::u8path(game_root));
  rdr_scene::wrestler_src::ApplyOverlays(impl_->files, &impl_->overlay_gen);   // S103 DLW: the packs the game was offered (src\dlc.h), so a DLC wrestler's pac\ch\ch385.pac opens
  ready_ = impl_->files.Exists("pac\\ch") || impl_->files.Exists("default.xex");
  if (ready_) assets::crowd_body::Init(game_root);
  if (ready_) read_rule::BuildEffectTable(impl_->files);   // S103 D1: the read rule's technique table (once per process)
  return ready_;
}

// S103 CMB: a composite attire (the game builds it on the GPU at the load; the stream-free build draws nothing of that): the entry carries the tag the model link read
// (attire_compose.h: "/c:<96 hex>", stored inside the body's link). Compose the PACH's base (entry 12) + masks 13..16 with those 48 colours on the host, once per
// distinct entry, here on the loader thread (never the render thread), as BC1 with mips, and replace the model's "color" texture in place (the Image is moved into it:
// nothing else is kept). Upload format is the renderer's BC1 path (UNORM, never SRGB).
// Stream-free rule (decided S103): a refusal is a HOLE, never the plain base: a refusal tag ("/r": the hook could not read the colours) or a compose that fails returns false with
// the reason, the load fails, ModelCache notes it (named_load_fail.h) and the record stops naming the model (the game's own geometry draws it, counted). A plain entry returns true untouched.
static bool ApplyCompositeAttire(launcher::GameFiles& files, int id, int attire, const std::string& entry, int key, assets::Model& m, std::string& error) {
  attire_compose::Input in;
  if (attire_compose::IsRefusalTag(entry)) {
    error = "composite attire refused at the link (its colours could not be read)";
    rdr::Log("[attire_compose] key %d: refused (%s): a hole, the plain base is not drawn", key, error.c_str());
    return false;
  }
  if (!attire_compose::ParseEntryTag(entry, in.colours)) return true;
  std::string why;
  attire_compose::Image img;
  std::vector<assets::Bytes> keep;   // the decoded entries (the Input points into them)
  do {
    std::string err;
    const assets::pac::Pac* p = assets::load_detail::OpenCached(files, assets::load_detail::WrestlerRel(id), err);
    if (!p) { why = "pac: " + err; break; }
    const assets::pac::Pach* h = nullptr;
    int seen = 0;
    for (const assets::pac::Pach& c : p->pachs)
      if (assets::pac::Find(c, 0) && seen++ == attire) { h = &c; break; }
    if (!h) { why = "no such attire PACH"; break; }
    keep.reserve(5);
    auto entry_bytes = [&](uint32_t e) -> const assets::Bytes* {
      const assets::pac::Ent* en = assets::pac::Find(*h, e);
      if (!en) return nullptr;
      keep.push_back(assets::pac::Decode(p->bytes.data() + en->off, en->size));
      return &keep.back();
    };
    const assets::Bytes* b = entry_bytes(attire_compose::kBaseEntry);
    if (!b) { why = "no base (entry 12)"; break; }
    in.base = b->data(), in.base_n = b->size();
    for (int k = 0; k < attire_compose::kMasks; ++k)
      if (const assets::Bytes* mk = entry_bytes(attire_compose::kFirstMaskEntry + uint32_t(k))) in.mask[k] = mk->data(), in.mask_n[k] = mk->size();
    attire_compose::Options opt;   // bc1 on, white paints
    if (!attire_compose::Compose(in, opt, img, why)) break;
    assets::Texture* tex = nullptr;
    for (assets::Texture& t : m.textures)
      if (assets::Lower(t.name) == "color" && t.width == img.w && t.height == img.h) { tex = &t; break; }
    if (!tex) { why = "the model has no \"color\" texture of " + std::to_string(img.w) + "x" + std::to_string(img.h); break; }
    tex->format = assets::TexFormat::kBC1, tex->mips = img.mips;
    const uint32_t alpha0 = img.alpha0;
    const size_t bytes = img.data.size();
    tex->data = std::move(img.data);
    rdr::Log("[attire_compose] key %d: composed (BC1, %u mips, %zu bytes, base alpha-0 texels %u)", key, tex->mips, bytes, alpha0);
    return true;
  } while (false);
  error = "composite attire refused: " + why;
  rdr::Log("[attire_compose] key %d: refused (%s): a hole, the plain base is not drawn", key, why.c_str());
  return false;
}

// S103 DLW census (a step check reads it): one line per distinct named wrestler ref, where its pac came from. A failed load is said by ModelCache (named_load_fail.h: "load failed ... guest rows draw it").
// S105 A7: the named loader's "texture 'X' not found" warnings (dds.h TexSet::Get; the part then draws white) reached no game log: one line per name per pac.
static void WrestlerTexWarnings(const std::string& pac, const assets::Model& m) {
  static std::mutex mu;
  static std::set<std::string> told;
  for (const std::string& w : m.warnings) {
    if (w.compare(0, 9, "texture '") != 0 || w.find("not found") == std::string::npos) continue;
    { std::lock_guard<std::mutex> l(mu); if (!told.insert(pac + "|" + w).second) continue; }
    rdr::Log("[wwe2k14][wrestler_src] %s (%s): %s: the sampler stays unbound (a diffuse then draws white; a normal or wrinkle map just stays off)", m.label.c_str(), pac.c_str(), w.c_str());
  }
}

static void WrestlerSrcCensus(const launcher::GameFiles& files, const std::string& pac, const std::string& entry, uint32_t variant, bool ok) {
  if (!ok) return;
  static std::mutex mu;
  static std::set<std::string> told;
  const std::string key = pac + "|" + entry + "|" + std::to_string(variant);
  { std::lock_guard<std::mutex> l(mu); if (!told.insert(key).second) return; }
  rdr::Log("[wwe2k14][wrestler_src] key %s variant %u (%s): loaded from %s", entry.c_str(), variant, pac.c_str(), rdr_scene::wrestler_src::Where(files, pac));
}

namespace {
// S103 GBR (BH4 4): the release hook returns false when the loader's lock was busy: the drawer retries next frame. Shared by the prewarm and the first event prop load.
void MarkEvtobjReleasable() {
  assets::load_detail::DlcProps().releasable = true;
  rdr_scene::weapons::g_evtobj_release = [] { bool busy = false; assets::load_detail::ReleaseEvtobj(&busy); return !busy; };
}
}  // namespace

bool GameFilesSource::Load(const std::string& pac, const std::string& entry, uint32_t variant, assets::Model& out, std::string& error) {
  if (!ready_) { error = "no game files (SetRoot first)"; return false; }
  // S103 S (run 7260 crash: pac::Decode inside LoadArena on rdr_render, 0xC0000005): asset_load.h is not thread-safe (its pac cache is shared); every
  // loader in the game takes this one lock (arena / crowd / ring / props / screens / flash *_game_space, crowd_body). One loader at a time.
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());
  rdr_scene::wrestler_src::ApplyOverlays(impl_->files, &impl_->overlay_gen);   // S103 DLW (the game publishes the packs after this reader may have been made: re-applied when they change)
  std::string norm = pac;
  std::replace(norm.begin(), norm.end(), '/', '\\');
  out = assets::Model();
  int id = 0, num = 0, type = 0;
  // A crowd person (S101 lane 27, rsc_draw's rule): arena pac + entry 20000 + 40 * LOD + type, or "card:TT" (the type's near body stands in
  // for the 360's run-time impostor). LOD 1 / 2 are drawn as the near body (the loader has no other; the record's palette poses it).
  if (std::sscanf(norm.c_str(), "pac\\bg\\bg%d.pac", &id) == 1 &&
      (std::sscanf(entry.c_str(), "card:%d", &type) == 1 || (std::sscanf(entry.c_str(), "%d", &num) == 1 && num >= 20000 && num < 20200))) {
    if (entry.rfind("card:", 0) != 0) {
      const int lod = (num - 20000) / 40;
      type = (num - 20000) % 40;
      if (lod > 2 || type > 19) { error = "crowd body " + entry + ": not a person body"; return false; }
    }
    if (type < 0 || type > 19) { error = "crowd body type out of range: " + entry; return false; }
    return assets::crowd_body::LoadBody(id, type, out, error);
  }
  {   // S103 GBR: a blood splat texture model of the arena's own bg pac (weapon_draw_core.h BloodEntryOf): "blood:<texture index>" = key 32000 with the n-th entry of the key-32010 directory (the arena branch below ignores the entry)
    int bt = 0;
    if (std::sscanf(norm.c_str(), "pac\\bg\\bg%d.pac", &id) == 1 && std::sscanf(entry.c_str(), "blood:%d", &bt) == 1) return assets::LoadBloodEntry(impl_->files, id, uint32_t(bt < 0 ? 0 : bt), out, error);
  }
  {   // S103 GBR: a GMSK set object (weapon_draw_core.h GmEntryOf): pac\gm.pac, entry "gm:<arena>/<key>" = the arena's bank (GMGB 78 / 80, else GMGA), the model of that key
    int ga = 0, gk = 0;
    if (norm == "pac\\gm.pac" && std::sscanf(entry.c_str(), "gm:%d/%d", &ga, &gk) == 2) return assets::LoadGmEntry(impl_->files, ga, uint32_t(gk), out, error);
  }
  if (std::sscanf(norm.c_str(), "pac\\bg\\bg%d.pac", &id) == 1) {
    // S103 M (N2): all_keys is a sticky global tested before @wrestler: set it for this load only and restore it, as arena_game_space.cpp does for CrowdPose
    const bool saved_all_keys = assets::stage_media::Cfg().all_keys;
    const bool saved_low = assets::load_detail::RingLowKeys();
    const bool saved_keep = assets::load_detail::KeepLookSlots();
    const bool saved_borrow = assets::load_detail::BorrowTextures();
    assets::load_detail::BorrowTextures() = rdr_sw::BoolOr(rdr_sw::kBorrowTextures, true);   // S103 WMB: the shared texture names an arena's pac lacks (wwe2k14_rdr_borrow_textures)
    assets::load_detail::KeepLookSlots() = true;   // S103 WSL: e and g slots both stay in the live model (the applier masks them by the game's look gate)
    assets::load_detail::RingLowKeys() = true;   // S103 RLB2: the game's low ring pieces 980 / 981 / 982 (the posed ring draws the LOD the rows ask for)
    assets::stage_media::Cfg().all_keys = true;   // S103 M: both looks' screens in the live model (we hold the load lock; the game exe has no viewer setting these)
    // S103 CAR: a created arena (200-249) = the merged part pac of the venue the applier published (the game's CASD / CAEN / CARG / CARL, car_arena.h); every other id is its bg pac
    const bool arena_ok = assets::car::IsCustomId(id) ? assets::LoadCustomArena(impl_->files, id, assets::car::Current(), out, error) : assets::LoadArena(impl_->files, id, out, error);
    assets::stage_media::Cfg().all_keys = saved_all_keys;
    assets::load_detail::RingLowKeys() = saved_low;
    assets::load_detail::KeepLookSlots() = saved_keep;
    assets::load_detail::BorrowTextures() = saved_borrow;
    return arena_ok;
  }
  if (std::sscanf(norm.c_str(), "pac\\ch\\ch%d.pac", &id) == 1) {
    // S103 B: entry "model_key:K" (what the wrestler contributor writes): the model PACH whose EMD name is K (character * 100 + attire * 10 + kind), else the same attire's
    // kind 2 (ring) model. `variant` is NOT an attire digit for such a record (the loader reads it as the n-th model PACH).
    int key = 0;
    if (std::sscanf(entry.c_str(), "model_key:%d", &key) == 1) {
      // S103 ARM (HNN fix 1): a composite attire has no model of its own in the pac (Heenan 24912): the game remaps it to another attire's model (p0/600, attire_remap.h). Only when the pac lacks the key.
      static const assets::attire_remap::Table kNoTable;
      const bool remap_on = rdr_sw::BoolOr(rdr_sw::kAttireRemap, true);
      namedfail::HoleRule().store(remap_on, std::memory_order_relaxed);
      std::string table_err;
      const assets::attire_remap::Table& table = remap_on ? assets::attire_remap::TheTable(impl_->files, &table_err) : kNoTable;
      if (!table_err.empty()) rdr::Log("[attire_remap] no colour table (%s): no composite attire is remapped", table_err.c_str());
      const assets::attire_remap::Pick pick = assets::attire_remap::PickModel(impl_->files, table, id, key);
      uint8_t tag_colours[attire_compose::kColourBytes];
      const bool tagged = attire_compose::IsRefusalTag(entry) || attire_compose::ParseEntryTag(entry, tag_colours);
      // S103 ARM (fix 3, the stream-free rule as `/r`): a composite or remapped attire body whose named load fails stays a counted HOLE, never the guest rows (they bind atlases this build never fills).
      auto fail = [&](const std::string& why) {
        error = why;
        if (namedfail::HoleOnFailure(remap_on && pick.hole_class, remap_on && tagged, pac, entry, variant)) {   // review F1: the switch off is the old load (guest rows)
          if (remap_on && tagged && !pick.remapped && !pick.hole_class) attire_build::Count().identity_hole.fetch_add(1, std::memory_order_relaxed);   // S105 A5d: an identity tagged load that failed: a hole where the plain base drew before
          rdr::Log("[attire_remap] attire_load_hole %llu (key %d): %s: a hole, the guest rows do not draw it", static_cast<unsigned long long>(namedfail::Holes()), key, why.c_str());
        }
        return false;
      };
      if (pick.ord < 0) return fail("no model PACH named " + std::to_string(key) + " (or its kind 2) in " + pac);
      const int ord = pick.ord;
      if (pick.remapped) rdr::Log("[attire_remap] key %d -> %d (colour slot %d): the game's remap (p0/600), model of attire PACH %d%s", key, pick.loaded, pick.slot, ord, tagged ? "" : "; no colour tag in the record: the plain base");
      if (pick.remapped && !tagged) {   // S103 ARM2: no build-step capture reached the record (the game did not build it: mode 0, e.g. Hogan's kind-4 keys; or the build comes later and the tagged entry loads then)
        attire_build::Count().missing.fetch_add(1, std::memory_order_relaxed);
        rdr::Log("%s", attire_build::CensusLine().c_str());
      }
      const bool ok = assets::LoadWrestler(impl_->files, id, ord, out, error);
      if (!ok) return fail(error);
      if (!ApplyCompositeAttire(impl_->files, id, ord, entry, key, out, error)) return fail(error);   // S103 CMB: a refusal is a failed load (a hole); a plain entry has no tag: nothing happens
      WrestlerSrcCensus(impl_->files, norm, entry, variant, ok);
      WrestlerTexWarnings(norm, out);   // S105 A7
      return ok;
    }
    const int n = assets::AttireCount(impl_->files, id);
    if (int(variant) >= n) { error = "attire " + std::to_string(variant) + " does not exist (wrestler " + std::to_string(id) + " has " + std::to_string(n) + ")"; return false; }
    const bool ok = assets::LoadWrestler(impl_->files, id, int(variant), out, error);
    WrestlerSrcCensus(impl_->files, norm, entry, variant, ok);
    if (ok) WrestlerTexWarnings(norm, out);   // S105 A7
    return ok;
  }
  // S103 BLF: an entrance's event prop (weapon_draw_core.h EvtEntryOf): "evtprop:<prop id>/<k>" = entry k of the PACH the game's own directory gives the prop id (assets::PropPach), the model MakeWeapon built for entry k.
  {
    int pid = 0, k = 0;
    if (norm == "pac\\evt\\evtobj.pac" && entry == kEvtobjPrewarmEntry) {   // S105 SKB fix F3: the 160 MB unpack, started when the match loads (LiveSceneDraw::QueueMisses), not at the first entrance prop's ask; a cache hit when already open
      MarkEvtobjReleasable();
      // S105 SKB3: the disc pac is read BY BLOCKS now (assets::load_detail::OpenEvtRanged: the block index by ranged reads + the directory, ~25 ms, never the 160 MB unpack); the whole-file open stays the fallback when a ranged read fails
      std::string rerr;
      if (assets::load_detail::EvtRanged* rg = assets::load_detail::OpenEvtRanged(impl_->files, rerr)) {
        static bool told = false;
        if (!told) { told = true; rdr::Log("[wwe2k14][weapon_rows] evtobj.pac ranged: %u blocks indexed by %zu reads in %.1f ms, directory %zu PACHs", rg->ix.count, rg->ix.reads, rg->index_ms, rg->rows.size()); }
        return true;
      }
      rdr::Log("[wwe2k14][weapon_rows] evtobj.pac ranged unavailable (%s): whole-file open", rerr.c_str());
      return assets::load_detail::OpenPropFile(impl_->files, 0, error) != nullptr;   // pach 0 = the disc evtobj.pac slot (dlc_files.h PropFile); `out` stays empty (the caller never stores it)
    }
    if (norm == "pac\\evt\\evtobj.pac" && std::sscanf(entry.c_str(), "evtprop:%d/%d", &pid, &k) == 2) {
      if (pid < 0 || k < 0 || k > 9) { error = "event prop " + entry + ": id or entry out of range"; return false; }
      MarkEvtobjReleasable();   // the live game: the drawer may close the evtobj.pac slot when the entrance's last prop model retires (weapon_draw_core.h ReleaseEvtobjSlot)
      int other = -1;
      const int pach = assets::PropPachEvt(impl_->files, uint32_t(pid), &other);
      rdr::Log("[wwe2k14][weapon_rows] event prop id %d -> PACH %d (entry %d)", pid, pach, k);
      if (other >= 0) {   // review 6: once per id
        static std::set<int> told;
        if (told.insert(pid).second) rdr::Log("[wwe2k14][weapon_rows] event prop id %d is in BOTH SOBJ (PACH %d, used: the game looks SOBJ first) and BOBJ (PACH %d)", pid, pach, other);
      }   // the step check reads Cena's belt here (2004 / 3004, not 2009 / 3009)
      if (pach < 0) { error = "event prop " + std::to_string(pid) + ": no PACH in evtobj.pac's directory"; return false; }
      return assets::LoadPropEntry(impl_->files, pach, uint32_t(k), out, error);
    }
  }
  // S103 CGF: a stage structure piece (weapon_draw_core.h StgEntryOf): pac\bg\bgEtc.pac, entry "stg:<PACH>/<k>" = nested key 1 entry k of top-level PACH (steel cage 0 / 1 / 2, Hell in a Cell 3, Elimination Chamber 6, Inferno 7).
  // (The arena / crowd rules above need a number in "bg<N>.pac": bgEtc has none, so they never take this.)
  {
    int pach = 0, k = 0;
    if (norm == "pac\\bg\\bgEtc.pac" && std::sscanf(entry.c_str(), "stg:%d/%d", &pach, &k) == 2) return assets::LoadStageEntry(impl_->files, pach, uint32_t(k), out, error);
  }
  if (norm == "pac\\evt\\evtobj.pac" && std::sscanf(entry.c_str(), "%d", &id) == 1) return assets::LoadProp(impl_->files, id, out, error);
  // S103 PW-3: a weapon / prop of the match's weapon manager: pac\m2.pac, entry "p0/1000/<id>" (src\renderer_scene\weapon_draw_core.h EntryOf). Id 14 (belts) is refused by the loader (sub-key [L]).
  if (norm == "pac\\m2.pac") {
    int bsub = 0;   // S103 LDR: "belt:<sub-key>" = the weapon manager's id 14 (the belt) under the title's sub-key (weapon_draw_core.h EntryOf)
    if (std::sscanf(entry.c_str(), "belt:%d", &bsub) == 1) return assets::LoadM2Weapon(impl_->files, 14, out, error, 10, 0u, bsub);
    int e = 0;   // "p0/1000/<id>" = the main model; "p0/1000/<id>/<1|2>" = its secondary model k = 0 / 1 (weapon_draw_core.h EntryOf)
    const int n = std::sscanf(entry.c_str(), "p0/1000/%d/%d", &id, &e);
    if (n >= 1) return assets::LoadM2Weapon(impl_->files, id, out, error, 10, n == 2 && e >= 1 && e <= 2 ? uint32_t(e) : 0u);
  }
  error = "no loader for pac \"" + pac + "\" entry \"" + entry + "\"";
  return false;
}

namespace {
std::mutex g_sprites_mu;
std::shared_ptr<const ParticleSpriteMap> g_sprites;
}  // namespace

std::shared_ptr<const ParticleSpriteMap> ParticleSpritesNow() {
  std::lock_guard<std::mutex> l(g_sprites_mu);
  return g_sprites;
}

bool BuildParticleSprites(const std::string& game_root, rdr_scene::live::particles::NameIndex& index, std::string& error) {
  if (game_root.empty()) return error = "no game root", false;
  launcher::GameFiles files;   // our own reader
  files.SetRoot(std::filesystem::u8path(game_root));
  auto map = std::make_shared<ParticleSpriteMap>();
  {
    assets::pac::Pac p;   // the decoded pac lives only inside this scope
    if (!assets::pac::Open(files, "pac\\prtcls.pac", p, error)) return false;
    assets::particle_sprites::ForEachDirectory(p, [&](const std::string& leaf, const uint8_t* dir, size_t n) { index.AddSpriteDirectory(dir, n, "pac\\prtcls.pac", leaf); });
    index.ForEach([&](uint64_t, const rdr_scene::live::particles::NameIndex::Name& nm) {
      assets::Texture t;
      std::string why;
      if (assets::particle_sprites::LoadSprite(p, nm.entry, t, why)) (*map)[nm.entry] = std::move(t);
    });
  }
  std::lock_guard<std::mutex> l(g_sprites_mu);
  g_sprites = std::move(map);
  return true;
}

}  // namespace rdr_scene
