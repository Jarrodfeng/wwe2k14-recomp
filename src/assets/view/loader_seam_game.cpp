// Asset viewer: the real seam - the loaders (src\assets\asset_load.h) over the player's own game files (--game: a
// folder or a disc image, src\launcher\disc_image.h). Built by tools\assets\build_viewer.ps1 -Seam this file.
// Session 96. Nothing from the game is kept: everything is read at run time and lives in memory only.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG  // (only its zlib decoder is used; PNG keeps the parser small)
#define STBI_ONLY_JPEG  // Session 98 (Superstar Importer): UPLOAD IMAGE takes PNG and JPG
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "../asset_load.h"
#include "../char_mods.h"
#include "../entrance.h"
#include "../evt_props.h"
#include "../particles_load.h"
#include "loader_seam.h"

namespace {
std::string mods_lookup_u8(const std::filesystem::path& p) { return assets::mods_lookup::U8(p); }
launcher::GameFiles g_files;
bool g_ready = false;
// Session 98 (attire props): evtdata.pac's tables (+ the DLC packs' rows), cleared whenever the game folder or the DLC set changes
std::map<int, assets::evt_props::Table> g_evt_tables;
std::string g_evt_err;
bool g_evt_loaded = false;
void ForgetEvtTables() { g_evt_tables.clear(), g_evt_err.clear(), g_evt_loaded = false; }
}  // namespace

void ViewerInit(const std::string& game_path) {
  ForgetEvtTables();
  if (game_path.empty()) return;
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
}

std::string ViewerSetDlc(bool on, const std::string& dir, const std::string& off) {
  ForgetEvtTables();
  if (!g_ready) on = false;
  return assets::dlc_files::Configure(g_files, g_files.root(), on, dir.empty() ? std::filesystem::path() : std::filesystem::u8path(dir), off);
}

std::string ViewerDlcJson() {
  auto esc = [](const std::string& s) {
    std::string o;
    for (char c : s) {
      if (c == '"' || c == '\\') o += '\\', o += c;
      else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
      else o += c;
    }
    return o;
  };
  std::string s = "[";
  for (const auto& p : assets::dlc_files::St().packs)
    s += (s.size() > 1 ? "," : "") + std::string("{\"pack\":") + std::to_string(p.pack_id) + ",\"folder\":\"" + esc(p.folder) +
         "\",\"name\":\"" + esc(p.name) + "\",\"state\":\"" + esc(p.state) + "\"}";
  return s + "]";
}

std::vector<std::string> ViewerDlcMotionFiles() { return g_ready ? assets::dlc_files::MotionFiles(g_files) : std::vector<std::string>(); }

int ViewerCustomAttires(int id) {
  assets::char_mods::Entry ce;
  return assets::char_mods::Find(id, ce) ? assets::AttireCountFile(mods_lookup_u8(ce.file)) : 0;
}

std::string ViewerCheckCharacterFile(const std::string& path_u8, std::string& warning) {
  warning.clear();
  try {
    if (assets::AttireCountFile(path_u8) <= 0) return "not a character pac (no attire with a body model)";
    assets::Model m;
    std::string err;
    if (!assets::LoadWrestlerFile(path_u8, 0, 0, m, err)) return "not a usable character pac: " + err;
    for (const assets::Bone& b : m.bones)
      if (b.name.compare(0, 3, "Bip") == 0) return "";
    // (review: an unusual skeleton may still be a real mod - kept, with a warning; the game's motions drive Bip bones)
    warning = "no Bip bones in its skeleton: motions and entrances may not move it";
    return "";
  } catch (...) {
    return "not a usable character pac (damaged)";
  }
}

int ViewerEntranceAttire(int id, int game_attire) {
  if (assets::char_mods::IsCustom(id)) {
    assets::char_mods::Entry ce;
    return assets::char_mods::Find(id, ce) ? assets::EntranceAttireFile(mods_lookup_u8(ce.file), game_attire) : -1;
  }
  return g_ready ? assets::EntranceAttire(g_files, id, game_attire) : -1;
}

bool ViewerLoad(const std::string& kind, int id, int attire, assets::Model& out, std::string& err) {
  if (!g_ready) {
    err = "no game files (start with --game PATH: the folder with default.xex, or a disc image)";
    return false;
  }
  out = assets::Model();
  const auto t0 = std::chrono::steady_clock::now();
  bool ok = false;
  if (kind == "wrestler" && assets::char_mods::IsCustom(id)) {  // Session 98: an imported community character (virtual id)
    assets::char_mods::Entry ce;
    if (!assets::char_mods::Find(id, ce)) {
      err = "custom character not found (mods toggle off, or the file was removed)";
      return false;
    }
    const int n = assets::AttireCountFile(mods_lookup_u8(ce.file));
    if (n == 0) {
      err = "'" + ce.name + "' is not a usable character pac (damaged, truncated or not a wrestler)";
      return false;
    }
    if (attire < 0 || attire >= n) {
      err = "attire " + std::to_string(attire) + " does not exist (wrestler " + std::to_string(id) + " has " + std::to_string(n) + ")";
      return false;
    }
    ok = assets::LoadWrestlerFile(mods_lookup_u8(ce.file), id, attire, out, err);
    if (ok) out.label += " [custom " + ce.name + "]";
  } else if (kind == "wrestler") {
    const int n = assets::AttireCount(g_files, id);
    if (attire < 0 || attire >= n) {
      err = "attire " + std::to_string(attire) + " does not exist (wrestler " + std::to_string(id) + " has " +
            std::to_string(n) + ")";
      return false;
    }
    ok = assets::LoadWrestler(g_files, id, attire, out, err);
  } else if (kind == "arena") {
    ok = assets::LoadArena(g_files, id, out, err);
  } else if (kind == "prop") {  // Session 98 (gen check 4): an evtobj.pac PACH (title belts 349-366+)
    ok = assets::LoadProp(g_files, id, out, err);
  } else {
    err = "kind must be wrestler or arena";
    return false;
  }
  if (ok && out.load_ms == 0)
    out.load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return ok;
}

std::vector<int> ViewerList(const std::string& kind) {
  if (!g_ready) return {};
  if (kind == "wrestler") return assets::ListWrestlerIds(g_files);
  if (kind == "arena") return assets::ListArenaIds(g_files);
  return {};
}

bool ViewerReadFile(const std::string& rel, std::vector<uint8_t>& out, std::string& err) {
  if (!g_ready) return err = "no game files (start with --game PATH)", false;
  if (!g_files.Exists(rel)) return err = "missing " + rel, false;
  out.clear();
  if (!g_files.Stream(rel, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); })) return err = "read failed " + rel, false;
  return true;
}

bool ViewerEntrance(int wrestler, assets::entrance::Show& out, std::string& err) {
  if (!g_ready) return err = "no game files (start with --game PATH)", false;
  out = assets::entrance::Load(g_files, wrestler, err);
  return out.ok;
}

int ViewerChampBank(int wrestler) { return g_ready ? assets::entrance::ChampBank(g_files, wrestler) : -1; }

int ViewerPropPach(unsigned prop_id) { return g_ready ? assets::PropPach(g_files, prop_id) : -1; }
int ViewerPropPachDir(unsigned prop_id) {
  if (!g_ready) return -1;
  std::string err;
  return assets::PropDirIn(assets::load_detail::OpenCached(g_files, "pac\\evt\\evtobj.pac", err), prop_id);
}
bool ViewerPropWorn(int pach) { return g_ready && assets::PropWorn(g_files, pach); }
static assets::particles::Pacs& ParticlePacs() {
  static assets::particles::Pacs pacs;  // (review) prtcls.pac / gm.pac kept beside g_files: reopened when the game folder changes
  return pacs;
}
bool ViewerParticles(int bank, assets::particles::Bank& out, std::string& err) {
  if (!g_ready) return err = "no game files (start with --game PATH)", false;
  out = assets::particles::LoadBank(g_files, bank, err, ParticlePacs());
  return out.ok;
}
bool ViewerSceneLights(int bank, assets::particles::SceneLights& out) {
  out = assets::particles::SceneLights();
  if (!g_ready) return false;
  out = assets::particles::LoadSceneLights(g_files, bank, ParticlePacs().gm);
  return !out.Empty();
}
int ViewerBeltTwin(int pach, int side) { return g_ready ? assets::BeltTwin(g_files, pach, side) : -1; }
int ViewerModelKey(int id, int viewer_attire) {
  return g_ready && !assets::char_mods::IsCustom(id) ? assets::ModelKey(g_files, id, viewer_attire) : -1;
}
bool ViewerEvtTable(int key, std::vector<std::vector<int>>& rows, std::string& err) {
  if (!g_ready) return err = "no game files (start with --game PATH)", false;
  if (!g_evt_loaded) {  // (read once per game folder / DLC set: evtdata.pac is 0.8 MB, the tables a few KB; a failure retries next time)
    g_evt_tables.clear(), g_evt_err.clear();
    assets::pac::Pac p;
    if (assets::pac::Open(g_files, "pac\\evt\\evtdata.pac", p, g_evt_err)) g_evt_tables = assets::evt_props::Decode(p, g_evt_err);
    if (!g_evt_tables.empty()) {
      g_evt_loaded = true;
      for (int id : assets::dlc_files::PackIds(g_files)) {  // the packs' evtdata_dlcPP.pac, in pack order (evt_props.h Merge)
        char rel[64];
        std::snprintf(rel, sizeof rel, "pac\\evt\\evtdata_dlc%02d.pac", id);
        std::string e2;
        assets::pac::Pac dp;
        if (!g_files.Exists(rel) || !assets::pac::Open(g_files, rel, dp, e2)) continue;
        assets::evt_props::Merge(g_evt_tables, assets::evt_props::Decode(dp, e2));
      }
    }
  }
  const auto it = g_evt_tables.find(key);
  if (it == g_evt_tables.end()) return err = g_evt_err.empty() ? "evtdata.pac has no table " + std::to_string(key) : g_evt_err, false;
  rows.assign(it->second.begin(), it->second.end());
  return true;
}
bool ViewerPropBobj(unsigned prop_id) {
  if (!g_ready) return false;
  std::string err;
  return assets::PropDirIn(assets::load_detail::OpenCached(g_files, "pac\\evt\\evtobj.pac", err), prop_id, true) >= 0;
}

bool ViewerEntranceRig(int bank,assets::entrance::Show& io, std::string& err) {
  if (!g_ready) return err = "no game files (start with --game PATH)", false;
  assets::entrance_light::Rig rig = assets::entrance::LoadRig(g_files, bank, err);
  if (!rig.ok) return false;
  io.rig = std::move(rig);
  return true;
}

bool ViewerStageKey(bool enabled, double time, std::string& err) {
  if (!(time >= 0 && time < 1e7)) return err = "time must be a game frame number >= 0", false;
  assets::stage_key::Cfg().enabled = enabled;
  assets::stage_key::Cfg().time = time;
  return true;
}

bool ViewerStageMedia(const std::string& dir, int tron, bool off, const std::vector<std::pair<std::string, std::string>>& map, std::string& err) {
  if (tron < 0 || tron > 99999) return err = "tron must be a titantron number", false;
  std::error_code ec;
  if (!dir.empty() && !std::filesystem::is_directory(std::filesystem::u8path(dir), ec)) return err = "no such folder " + dir, false;
  assets::stage_media::Cfg().dir = dir;
  assets::stage_media::Cfg().tron = tron;
  assets::stage_media::Cfg().off = off;
  assets::stage_media::Cfg().map.clear();
  assets::stage_media::Cfg().wrestler = -1;
  assets::stage_media::Cfg().keys.clear();
  assets::stage_media::Cfg().ent_cue = 0, assets::stage_media::Cfg().ent_t = 68, assets::stage_media::Cfg().ent_light = true;
  for (const auto& kv : map) {
    if (kv.first == "@wrestler") assets::stage_media::Cfg().wrestler = std::atoi(kv.second.c_str());
    else if (kv.first == "@cue") assets::stage_media::Cfg().ent_cue = std::atof(kv.second.c_str());
    else if (kv.first == "@t") assets::stage_media::Cfg().ent_t = std::atof(kv.second.c_str());
    else if (kv.first == "@nolight") assets::stage_media::Cfg().ent_light = false;
    else if (kv.first == "@keys") assets::stage_media::Cfg().keys = kv.second;
    else assets::stage_media::Cfg().map[kv.first] = kv.second;
  }
  return true;
}

bool ViewerCrowd(bool stored, int act, int frame, std::string& err) {
  if (act < 0 || act > 65535) return err = "act must be 0..65535", false;
  assets::load_detail::CrowdPose() = stored ? assets::load_detail::CrowdPoseMode::kStored : assets::load_detail::CrowdPoseMode::kGame;
  assets::load_detail::CrowdAct() = act;
  assets::load_detail::CrowdFrame() = frame;
  return true;
}

void ViewerStageWrestlers(const std::string& dir, std::vector<int>& ids, std::string& used_dir) {
  namespace fs = std::filesystem;
  ids.clear();
  const std::string saved = assets::stage_media::Cfg().dir;
  assets::stage_media::Cfg().dir = dir;
  const fs::path d = assets::stage_media::DefaultDir();  // an explicit folder, else the launcher's tron cache
  assets::stage_media::Cfg().dir = saved;
  used_dir = d.empty() ? std::string() : d.string();
  std::error_code ec;
  if (d.empty() || !fs::is_directory(d, ec)) return;
  std::vector<int> v;
  auto digits = [](const std::string& s) { return !s.empty() && s.size() < 6 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
  for (const auto& e : fs::directory_iterator(d, ec))
    if (e.path().extension() == ".png" && digits(e.path().stem().string())) v.push_back(std::atoi(e.path().stem().string().c_str()));
  for (const auto& e : fs::directory_iterator(d / "evtmov", ec)) {
    const std::string st = e.path().stem().string();  // MVSW_M161 / MVSB_M161 (their _end copies too)
    if (e.path().extension() == ".png" && (st.rfind("MVSW_M", 0) == 0 || st.rfind("MVSB_M", 0) == 0) && digits(st.substr(6, 3)))
      v.push_back(std::atoi(st.substr(6, 3).c_str()));
  }
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  ids = v;
}
