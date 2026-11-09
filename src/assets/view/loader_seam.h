// Asset viewer: the seam between the viewer (D3D11, cameras, pipe) and the loaders (src\assets\asset_load.h).
// The test build links loader_seam_test.cpp (a built-in test model); the integration links a real seam that calls
// LoadWrestler / LoadArena / ListWrestlerIds / ListArenaIds / AttireCount.
#pragma once
#include <string>
#include <vector>

#include "../asset_model.h"

// Session 98 (custom characters): the attires of an imported character (virtual id, char_mods.h); 0 when unreadable / unknown.
int ViewerCustomAttires(int id);
// Session 98 (phone import): "" when the file is a usable character pac (attire 0 loads), else why not. `warning` is set when its
// skeleton lacks the Bip bones the game's motions drive (kept anyway). The test seam accepts everything.
std::string ViewerCheckCharacterFile(const std::string& path_u8, std::string& warning);
// Session 98 (generalisation): the viewer attire index of game attire `game_attire`'s ENTRANCE model (the pac directory's "..A4"
// model, else its ring model "..A2"); -1 when the pac names none (the caller keeps its fallback). asset_load.h EntranceModelIndex.
int ViewerEntranceAttire(int id, int game_attire);
// Called once at start with --game PATH ("" when not given).
void ViewerInit(const std::string& game_path);
// Session 98 (DLC, src\assets\dlc_files.h): which DLC packs every later read also searches, in the game's order. `on` false = none;
// `dir` "" = the usual folder (`dlc` beside the game data); `off` = pack ids switched off ("3,5"). Returns a one-line summary.
std::string ViewerSetDlc(bool on, const std::string& dir, const std::string& off);
// The packs found by the last ViewerSetDlc as a JSON array [{"pack":4,"folder":..,"name":..,"state":"used"|why not}, ..].
std::string ViewerDlcJson();
// The motion pacs the DLC packs add ("dlc_mot04", ..: pac\dlc_motPP.pac), in pack order.
std::vector<std::string> ViewerDlcMotionFiles();
// kind is "wrestler" or "arena". On failure returns false and fills err.
bool ViewerLoad(const std::string& kind, int id, int attire, assets::Model& out, std::string& err);
// ids available for kind ("wrestler" / "arena"); empty when unknown.
std::vector<int> ViewerList(const std::string& kind);
// Session 97 (motions): the raw bytes of a file under the game folder / disc image, e.g. "pac\mot00.pac" (the motion pacs
// are plain EPK8, not XCTD). False with err when there are no game files or the file is missing.
bool ViewerReadFile(const std::string& rel, std::vector<uint8_t>& out, std::string& err);
// Session 97 (crowd motion): how the next arena loads pose the crowd. `stored` = the bodies' stored T-pose; otherwise the
// motion of `act` (crowd acts 14001-14011) at game frame `frame` (-1 = a different frame per person type). Returns false
// (and err) when the seam has no crowd (the test seam).
bool ViewerCrowd(bool stored, int act, int frame, std::string& err);
// Session 97 (stage key): whether the next arena loads use the stage motion key (pac entries 30000 light sets, 30001 slot motions m(N))
// and at which game frame it is evaluated. False (and err) when the seam has no stage key (the test seam).
bool ViewerStageKey(bool enabled, double time, std::string& err);
// Session 97 (stage movies, srcssets\stage_media.h): the folder of PNGs the game exported (--wwe2k14_tron_export) that feeds the
// entrance stage / video wall of arenas loaded from now on ("" = the launcher's tron cache, if any; off = untextured as before), and
// the titantron number that feeds the big screen and banner ring.
// `map` = A/B overrides (sampler name -> spec, see Settings::map in stage_media.h); empty keeps the built-in mapping.
bool ViewerStageMedia(const std::string& dir, int tron, bool off, const std::vector<std::pair<std::string, std::string>>& map, std::string& err);
// Tron preview: the wrestler numbers that have stage media (a tron NNN.png, or evtmov\MVSW_Mnnn.png / MVSB_Mnnn.png) in `dir`
// ("" = the launcher's tron cache), sorted; `used_dir` = the folder looked at ("" when there is none). Never fails: none = empty.
void ViewerStageWrestlers(const std::string& dir, std::vector<int>& ids, std::string& used_dir);
// Session 98 (entrance): a wrestler's whole entrance (src\assets\entrance.h) read from the game files. False with err on failure.
namespace assets { namespace entrance { struct Show; } }
bool ViewerEntrance(int wrestler, assets::entrance::Show& out, std::string& err);
// Session 98 (gen check 5): the Entrance06 champion bank named after the wrestler's entrance (entrance.h ChampBank), -1 none.
int ViewerChampBank(int wrestler);
// Session 98 (gen check 6): replaces io.rig with entrance bank `bank`'s light rig (entrance.h LoadRig: an entrance played from another
// bank is lit by the wrestler's own rig). False (io unchanged) when that bank has no rig.
bool ViewerEntranceRig(int bank, assets::entrance::Show& io, std::string& err);
// Session 98 (gen check 6): the evtobj.pac PACH of an entrance prop id (asset_load.h PropPach), -1 none.
int ViewerPropPach(unsigned prop_id);
// Session 98 (attire props): the same, from evtobj.pac's own directory only (asset_load.h PropDirIn: no model scan), -1 none.
int ViewerPropPachDir(unsigned prop_id);
// Session 98 (cap / particles): true when that prop's model is worn on the head (asset_load.h PropWorn: a node named "..hat").
bool ViewerPropWorn(int pach);
// Session 98 (cap / particles): the PCE6 particle effects of entrance bank `bank` (assets\particles.h). False with err when none.
namespace assets { namespace particles { struct Bank; struct SceneLights; } }
bool ViewerParticles(int bank, assets::particles::Bank& out, std::string& err);
// Session 99 (scene objects): the bank's GMEV screen flashes (type 6) and soft-particle switches (type 11). False when none.
bool ViewerSceneLights(int bank, assets::particles::SceneLights& out);
// Session 98 (props general): the title belt PACH of the same title in the left (2) / right (3) / waist (5) run (asset_load.h BeltTwin).
int ViewerBeltTwin(int pach, int side);
// Session 98 (attire props): the model key (asset_load.h ModelKey: char * 100 + game attire * 10 + 2 / 4) of wrestler `id`'s viewer
// attire index, -1 when the pac's directory does not name it (a custom character, no game files).
int ViewerModelKey(int id, int viewer_attire);
// Session 98 (attire props): text table `key` (101, 103, ..) of evtdata.pac /CSDT/DATA (evt_props.h), read once; false with err when
// the file or the table is missing. Rows of signed integers.
bool ViewerEvtTable(int key, std::vector<std::vector<int>>& rows, std::string& err);
// Session 98 (attire props, review): true when evtobj.pac's directory has a "/BOBJ/%04d" model for the id (table 103's precheck).
bool ViewerPropBobj(unsigned prop_id);
