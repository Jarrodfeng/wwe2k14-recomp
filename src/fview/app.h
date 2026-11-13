// Session 101 lane 2 (fview): the pieces main.cpp and the window share.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "describe.h"
#include "effects.h"
#include "session.h"

namespace fview {

struct Options {
  std::string capture;       // a capture folder
  std::string case_zip;      // --case: a case zip (docs\renderer\CASE_ZIP.md: a capture folder at capture\)
  int frame = -1;            // -1: the capture's last frame
  int through_op = -1;       // --through-op N: the pixel history as of op N
  int op = -1;               // --op N: the op the picture / case is taken after (-1: the pixel's top "wrote" draw, else the last op)
  bool have_pixel = false;
  int px = 0, py = 0;
  uint32_t surface = 0;      // 0: the frame's presented surface
  std::string json_out;      // --json OUT
  bool list_ops = false;     // --ops
  bool save_case = false;    // --save-case
  std::string reload_test;   // --reload-test EFFECT
  std::string surface_hash;   // --surface-hash B[,A]: render through op A first (if given), then through op B, and print a hash of the surface (the selftest compares the same op reached two ways)
  bool legacy_prefix = false; // --legacy-prefix: TEST ONLY fault injector: every render re-renders ops 0..N over the previous result (the pre-fix behaviour), so the selftest can see its drift check fail
  std::string apply_reload;  // --apply-reload EFFECT: rebuild the effect from --fx-dir and bind with it BEFORE the --pixel / --ops / --save-case work (the planted-fault test)
  std::string fx_dir;        // --fx-dir DIR
  std::string judge_out;     // S101 lane 20: --judge-out DIR: write picture.png + pixels.json there (tools\judge\judge.py)
  std::string judge_pixels;  // S101 lane 20: --judge-pixels FILE: lines 'x y'
  std::string const_census;  // S101 lane 164: --const-census OUT.csv
  std::string shader_dir;    // S101 lane24: --shader_dir DIR: every <variant>.spv / .alpha.spv in DIR replaces the compiled SPIR-V (src\renderer_scene\shader_reload.h), layouts checked
  bool reload_continue = false;  // S101 lane24: --reload-continue: a failed --apply-reload is said and the run goes on with the old programs (the broken-edit test)
  std::string root;          // --root DIR: where out\ lives (default: three folders above the exe)
  std::string python = "py";
  std::string screenshot;    // --screenshot PNG: window mode draws a few frames, writes the window and exits (for the doc and the hand-back)
  int validation = -1;       // --validation N (also: headless replay like rplay)
  int frames = 3;            // --frames N for the replay
  bool window = false;
  bool replay = false;       // headless replay like rplay
  int gpu = -1;
  uint32_t format_caps_mask = 0;  // lane 200: --format-caps-mask HEX (renderer.h)
  float scale = 1.0f;
  // S101 lane 35 (the crowd's first promotion): --crowd-ours draws the crowd from the capture's scene records (needs scene\), --crowd-fault NAME plants a fault
  // (bind_pose, identity_vp, no_cards, offset), --game DIR is the game files the crowd's bodies are read from (default: <root>\game_data\extracted).
  std::string crowd_census;  // S101 lane 35: --crowd-census FILE (with --replay --frames N --crowd-ours): frame,members drawn by us per frame
  std::string image_png;  // S101 lane 35: --image-png ID,OUT.png: a capture image (a texture or surface) after the frame's last op, as PNG
  std::string sweep;         // S101 lane 99: --sweep OUT.csv [--sweep-images 214,217,221]: all frames in order, a hash of the picture and of those images per frame
  std::string sweep_images;
  int sweep_slot = -1;       // --sweep-slot N: also hash every distinct image bound at fetch slot N (12 = the shadow readers' maps)
  bool crowd_ours = false;
  std::string dump_frames, dump_dir;  // lane 155: --dump-frames 189,199 --dump-dir DIR (with --replay): the presented surface of those frames as PNG
  std::string hash_frames;           // S101 lane CD: --hash-frames FILE (with --replay): one line "frame,hash" per replayed frame (FNV-1a of the presented surface), for the byte-equal proofs
  bool crowd_cards = false;          // S101 lane CD: --crowd-cards (with --crowd-ours): the far cards ours (one instanced draw per person type)
  std::string crowd_members_thin_counts;   // lane 272: --crowd-members-thin-counts FILE (the cut tool's sidecar: hook_frame,site,seen,dropped,world per line)
  bool crowd_members_thin = false;   // lane 272: --crowd-members-thin: a frame whose stream holds only the first member of each mesh run is drawn from the record
  bool crowd_members_probe = false;  // lane 272: --crowd-members-probe: the census (class-uniformity verify + stamp probe + gate results) on the capture as it is, nothing thin
  std::string crowd_cards_thin_counts;   // lane 175: --crowd-cards-thin-counts FILE (the cut tool's sidecar: hook_frame,seen,dropped)
  bool crowd_cards_thin = false;     // lane 175: --crowd-cards-thin: a frame whose stream holds only the first card of each type's loop is drawn from the record
  bool crowd_cards_unchecked = false;  // S101 lane CD (test only): --crowd-cards-unchecked: a bad frame is drawn anyway (the planted faults show in the picture)
  bool crowd_floor = false;          // S101 lane 82: --crowd-floor (with --crowd-ours): the seated floor people (site 0x8276ED60) ours too
  std::string crowd_floor_synth;     // S101 lane 82: --crowd-floor-synth FILE (test only): floor members added to a copy of each record from FILE (tools/crowd/floor_join.py --emit)
  bool stage_view = false;  // S101 lane 59: --stage-view: as if a `!scene play` were running (the scene's declared target is presented; stage_view.h)
  bool stage_playing = false;  // S101 lane 59: --stage-playing: the director says "playing" but no frame is stamped
  std::string stage_pick_fault;  // S101 lane 59: --stage-pick-fault last_by_id (planted twin)
  // S101 lane 41 (the generic in-place replace seam): --replace-test (the test hook), --arena-ours, --wrestlers-ours, --replace-fault LIST (the hook's planted faults).
  bool post_ours = false;       // S101 lane PO: --post-ours [--post-fault LIST] [--post-limits no_r32f_linear] (the post chain recorded by our own pass; no scene record, no --game)
  std::string post_fault, post_limits;
  bool particles_ours = false;  // S101 lane 67: --particles-ours [--particles-fault NAME] (particle runs as one draw each)
  std::string particles_fault;
  bool caw_ours = false;   // lane 153
  bool replace_test = false, arena_ours = false, wrestlers_ours = false, ring_ours = false, ui_ours = false, flash_ours = false, props_ours = false, shadow_casters_ours = false, screens_ours = false, crowd_props = false, movie_ours = false;  // S101 lane 133: --crowd-props  // S101 lane 115: screens;  // S101 lane 99: shadow casters; ring / ui / flash: S101 contracts; props: S101 lane 84
  bool rim_ours = false;  // lane 124: --rim-ours (the rim light map's casters); faults: --replace-fault rim.NAME
  bool plane_shadow_ours = false;   // lane 139: --plane-shadow-ours (the plane shadow chain's constants from the scene record); faults: --replace-fault plane.NAME (inv_identity, camera_stale, eye_shift, no_verify)
  std::string plane_oracle, plane_check;   // lane 139: --plane-oracle FILE.plane (tools\shadow\plane_oracle.py dump: the writer's record played from the capture's own constants); --plane-check OUT.json
  bool circles_ours = false;   // lane 136: --circles-ours (the circle blob shadows from the scene record); faults: --replace-fault circles.NAME (bind_pose, join_by_index, no_fade, no_verify)
  std::string circles_oracle, circles_check;   // lane 136: --circles-oracle FILE.circ (tools\shadow\circles_oracle.py dump: the writer's record played from the capture's own constants); --circles-check OUT.json (lockstep stream vs circles, picture compared)
  bool small_passes_ours = false;  // lane 135: --small-passes-ours (the sweat map and skin pre-light passes); faults: --replace-fault small.NAME
  bool rim_chain_ours = false;  // lane 151: --rim-chain-ours (the rim light map's reductions and blur by our programs); faults: --replace-fault rimchain.NAME
  std::string rim_chain_check, rim_chain_images;   // lane 151: --rim-chain-check OUT.json (two sessions in lockstep: the stream vs the rim chain by our programs); --rim-chain-images 222,223,224,225
  std::string rim_chain_override;   // lane 151: --rim-chain-override FROM:TO (capture image ids): in BOTH sessions the rim reductions' input FROM is read as TO (a structured image: tests the reduction's logic on dark and lit texels)
  uint32_t rim_chain_fs_capacity = 0xFFFFFFFFu;   // lane 151: --rim-chain-fs-capacity N (TEST): the pixel block buffer of arm B holds N bytes
  uint32_t rim_chain_skip = 0;   // lane 151: --rim-chain-skip N: the first N frames are played but not compared
  bool ui_text_ours = false;  // S101 lane 89 (UT): the game's text laid out by us (--ui-text-ours)
  std::string replace_fault;
  bool shadows_ours = false;          // S101 lane 123: --shadows-ours [--shadow-map-size N] [--shadows-ours-fault NAME]
  uint32_t shadow_map_size = 512, shadow_plane_percent = 100;  // --shadow-plane-scale P (percent; 0 = follow --scale)
  std::string shadows_ours_fault, shadows_check;  // --shadows-check OUTFILE: lockstep A (stream) / B (ours) compare, writes a json report
  std::string shadow_keying;
  uint32_t check_every = 1;  // --check-every K: --shadows-check compares every K-th frame (all frames are played)
  bool d3_no_proof = false;
  bool d3_hook_from_record = false;   // lane 258: --d3-hook-from-record: the ring chain's hook tally comes from the capture's ring-rows entries (the real hook's calls), not from the emulation (needs --d3 ring --d3-mode record)
  bool d3_verify = false;      // lane 152 (D3-F): --d3-verify: markers are written, nothing is dropped, the stream's draws are compared with the marker's sequence
  int d3_record_offset = -1;   // lane 152: --d3-record-offset N: the hook-to-stream frame offset of the record arm (default +1, measured on J1)
  std::string d3, d3_mode, d3_fault, d3_ledger_in, d3_ledger_out;   // lane 138: --d3 arena [--d3-mode stream|sourced] [--d3-fault NAME] (needs --thin arena)
  std::string thin, thin_fault, thin_run;  // lane 122: --thin CLASS [--thin-fault NAME] ; --thin-run DIR (bind+render frames 0..--frames-1 once each, write DIR\frames.csv)
  int thin_lag = 2, thin_k = -1, thin_churn = 0, thin_swap = -1, thin_epoch_at = -1, thin_isolate_from = 0, thin_vswap = -1, thin_release_part = -1;   // lane 129: --thin-vertex-swap FRAME (F4), --thin-release-part FRAME (F2), both TEST ONLY
  std::string thin_isolate;  // --thin-epoch-at FRAME, --thin-isolate NAME [--thin-isolate-from FRAME] (test only)  // --thin-lag N (the reader sees the pins N binds late), --thin-k K (proof rule; 0 = never), --thin-churn N (test), --thin-swap-slot FRAME (test)
  std::string image_info = "capture";  // S101 lane 47: --image-info capture|live|live-fault|none
  std::string crowd_fault;
  std::string game;
};

// The folder holding out\ and src\: --root, else the exe's grandparent's parent (out\renderer\dev\fview.exe -> the repo), else the current folder.
std::filesystem::path RepoRoot(const Options& options, const char* argv0);

struct ReloadVariant {
  std::string name;
  uint64_t before = 0, after = 0, before_alpha = 0, after_alpha = 0;  // SPIR-V hashes: before = what the binder had, after = the rebuilt
  bool changed = false;
};
struct ReloadResult {
  bool ok = false;
  double seconds = 0;
  std::vector<ReloadVariant> variants;
  std::vector<std::string> failed;  // the compiler's text for a shader that did not build; a layout the compiled table does not have; a shader the driver refused
  std::vector<std::string> skipped; // rebuilt variants the compiled registry does not have (not drawable until it is rebuilt)
  std::string log;
};
// Rebuilds one effect from `fx_dir` (tools\fview\reload_effect.py), loads the SPIR-V into the session's binder and binds the selected frame again.
bool ReloadEffect(Session& session, const std::string& effect, const std::filesystem::path& fx_dir, const std::filesystem::path& root, const std::string& python, ReloadResult& result);
void WriteReloadJson(JsonWriter& w, const ReloadResult& result);

// "Save case": out\cases\fview_<capture>_<frame>_<op>_<time>\picture.png + case.json. Returns the folder ("" on failure, error says why).
std::string SaveCase(Session& session, const EffectIndex* effects, const std::filesystem::path& root, uint32_t surface, int op, bool have_pixel, int px, int py, std::string& error);

// The pixel history as JSON (the headless --pixel output and case.json's history).
void WriteHistoryJson(JsonWriter& w, Session& session, const EffectIndex* effects, uint32_t surface, int px, int py, int through_op = -1);

// lane 139: --plane-check (plane_check.cpp).
int RunPlaneCheck(const Options& options, const std::filesystem::path& root);
// lane 136: --circles-check (circles_check.cpp).
int RunCirclesCheck(const Options& options, const std::filesystem::path& root);
// S101 lane 123: --shadows-check (shadows_check.cpp).
int RunShadowsCheck(const Options& options, const std::filesystem::path& root);
// lane 151: --rim-chain-check (rim_chain_check.cpp).
int RunRimChainCheck(const Options& options, const std::filesystem::path& root);

// Unpacks a case zip into out\cases\fview_unpack_<time>\ and returns its capture folder ("" on failure).
std::string UnpackCase(const std::string& zip, const std::filesystem::path& root, std::string& error);

// S101 lane 20: the judge's dump (judge.cpp).
int JudgeDump(Session& session, const EffectIndex* effects, const std::string& out_dir, const std::string& pixels_file, std::string& error);

int RunWindow(Session& session, EffectIndex* effects, const Options& options, const std::filesystem::path& root);

}  // namespace fview
