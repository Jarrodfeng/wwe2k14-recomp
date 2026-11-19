// A scene record -> a frame the front-end draws (Session 103; interface by lane S, built by lane R; docs\renderer\FRAME_CONTRACT.md 7).
// The same code draws a captured record offline (tools\scene\frame_rig, rsc_draw) and the live record on our render thread: one path.
// Each class (arena, ring, props, wrestlers, crowd, screens, ...) turns its part of the record into SceneFrame objects and stage hooks with
// an applier in its OWN file; this file only orders them. Includes the front-end and the record only: no game, no SDK, no viewer.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../assets/asset_model.h"
#include "../renderer/problem.h"
#include "../assets/attire_build.h"   // S103 ARM2: ClearFailed also clears the build-step latch
#include "front_end.h"
#include "named_load_fail.h"   // S103 DLW
#include "record_io.h"

namespace rdr_scene::rf {

// The scene families a record's objects fall into (by object id prefix and class; SCENE_RECORD.md "Stable ids"). Bit masks select families
// for per-class pictures (the rig's "per class" panels) and for coverage counts.
enum class Family : uint8_t { Arena = 0, Ring = 1, Props = 2, Wrestlers = 3, Crowd = 4, Screens = 5, Flashes = 6, Particles = 7, Other = 8, Count };
inline constexpr uint32_t kAllFamilies = (1u << uint32_t(Family::Count)) - 1;
const char* FamilyName(Family f);
Family FamilyOf(const SceneTables& t, const Object& o);

// Loads an asset by name (pac + entry + variant) from the player's own files, in our host formats. Offline: the viewer's loader seam
// (loader_seam.h, as rsc_draw does) or GameFilesSource. Live: lane S's loader thread. Never game memory.
class AssetSource {
 public:
  virtual ~AssetSource() = default;
  virtual bool Load(const std::string& pac, const std::string& entry, uint32_t variant, assets::Model& out, std::string& error) = 0;
};

// What the record's assets are, parsed ONCE per asset index per record (BuildFrame fills it; appliers read it, never re-parse a pac / entry string per object).
struct AssetInfo {
  enum Kind : uint8_t { Other = 0, ArenaSlot = 1, Crowd = 2 };
  Kind kind = Other;
  int arena = -1;  // ArenaSlot / Crowd: the arena id (pac\bg\bgNN.pac)
  int slot = -1;   // ArenaSlot: the slot K
};

// Front-end models by asset key, kept across frames. Offline: Get() loads on a miss. Live (lane S): Get() never loads on the render thread;
// a miss is queued for the loader and answers -1 with why = "not resident" (the frame is then GameWalks: FRAME_CONTRACT 3 item 2).
// A load that FAILED is remembered (failed_): Get / Stored answer -1 with why "load failed: <error>", never count it as a miss and never queue it again until
// ClearFailed() (call it when the game folder changes).
class ModelCache {
 public:
  ModelCache(FrontEnd& fe, AssetSource& source, bool load_on_miss);
  // The front-end model of record asset `asset_index`, or -1 (why says why: unknown asset, not resident, load failed: reason).
  int Get(const SceneTables& t, uint32_t asset_index, std::string* why = nullptr);
  // S103 R: the asset as the loader gave it (not added to the front-end): what an applier re-poses or rebuilds (the crowd's stored-pose bodies, the arena to be
  // made to show the record). Kept until RetireOther. nullptr + why when it cannot be had. StoredNamed: by name, independent of the record (the arena is asked
  // by pac, not by slot, so its 20+ slot assets are one load).
  const assets::Model* Stored(const SceneTables& t, uint32_t asset_index, std::string* why = nullptr);
  // `count_miss` false (S103 PW review fix 2): a not-resident answer is still queued for the loader but does not count in misses_this_frame(), so a weapon's first sight never makes the frame a LOADING miss (the rest is drawn).
  const assets::Model* StoredNamed(const std::string& pac, const std::string& entry, uint32_t variant, std::string* why = nullptr, bool count_miss = true);
  // A model the applier built itself: kept under `key`.
  static constexpr uint8_t kGroupFromLabel = 0xFF;   // S103 GSTP: the model's GPU group is not named by its owner: the label rule (model_group.h) decides
  int Put(const std::string& key, const assets::Model& m, uint8_t group = kGroupFromLabel);   // S103 GSTP: `group` = the owner's rdr::GroupId (FrontEnd::SetModelGroup)
    // S103 CTX2: Put with the pictures shared by content across builds (the guest model's 4 + 2 + 0.5 MB pictures are uploaded once, not once per slider-drag build). The images belong to this cache's `guest_images_`;
  // TrimGuestImages(keep) releases (deferred by the renderer) every shared image `keep`'s textures do not use: the caller runs it when the older models were retired.
  int PutGuest(const std::string& key, const assets::Model& m, uint8_t group = kGroupFromLabel);   // (merge GSTP + CTX2: the owner names the group here too)
  void TrimGuestImages(const assets::Model& keep);
  void DropStored(const std::string& pac, const std::string& entry, uint32_t variant);  // forget the CPU copy (a failure memo stays)
  int Find(const std::string& key) const;
  // S103 R: posed models, bounded. Pool `pool` holds one front-end model per simultaneously posed palette of one body; slot `index` is rewritten IN PLACE (vertex
  // buffer only: FrontEnd::UpdateModelVertices) every frame; slots after the first share the first's images (FrontEnd::AddModelSharing): no per-pose texture copy.
  // S103 C5: `textureless` = `posed` was copied without its textures (the pool already holds an image owner): a model that would have to be (re)built without an owner is NOT built (returns -1,
  // nothing half-made); the caller restores the textures and asks again.
  int PosedModel(const std::string& pool, size_t index, const assets::Model& posed, bool textureless = false, uint8_t group = kGroupFromLabel);   // S103 GSTP: the owner names the group (the posed ring: ring, a weapon: weapon)
  // S103 C5: the live slot's id without touching the GPU (-1 = none): lets a caller skip an unchanged pose.
  int PosedFind(const std::string& pool, size_t index) const;
  // S103 R: deferred release. RetireOther drops every model / pool / stored copy / memo whose key starts with `prefix` and does not contain `keep` (an arena
  // change); the front-end models go on retired_ and are released by ReleaseRetired(), which the caller runs when the GPU is idle (FrontEnd::ReleaseModel's rule).
  // Until it does, the models stay and are counted (retired_pending()).
  void RetireOther(const std::string& prefix, const std::string& keep);
  void ReleaseRetired();
  size_t retired_pending() const { return retired_.size(); }
  uint64_t retired_total() const { return retired_total_; }
  // S103 R2: keep the CPU copy of every WRESTLER model (pac\ch\) that Get() / PutLoaded() makes (a few MB each; no other family): the live maps hook builds its caster geometry
  // from it (live_maps.h ModelProvider). Set before the first Get. CpuModel(fe id) = the copy, nullptr when none (not a wrestler, not kept, retired).
  void KeepWrestlerCpu(bool on) { keep_cpu_ = on; }
  const assets::Model* CpuModel(int fe_model) const;
  bool IsGuestCaster(int fe_model) const { return guest_cpu_.count(fe_model) != 0; }   // S103 CS1B: the CPU copy belongs to a PutGuest model (census `guest casters`)
  size_t cpu_count() const { return cpu_.size(); }   // S103 CS1B: tests
  // S103 MBC: the CPU copy of a POSED PROP (weapon / event prop / structure piece; ApplyWeaponRows, tagged kObjCastsMapB) for map B's caster pass: a copy of the posed vertices, `serial` moves each time they change. Kept only while
  // KeepWrestlerCpu is on (the live maps hook exists). Never returned by CpuModel (the other maps' casters would take a prop with a character material for a wrestler). Goes with the model (RetireOther / pool retire).
  bool KeepsCpu() const { return keep_cpu_; }
  void SetPropCpu(int fe_model, const assets::Model& posed);
  const assets::Model* PropCpuModel(int fe_model, uint32_t& serial) const;
  void DropPropCpu(int fe_model);   // a frame that will not record map B props: the copy is dropped (absent = re-copied when wanted, never stale)
  uint64_t uploads() const { return uploads_; }  // front-end models made (Put + new pool slots): the "no per-frame growth" count
  // Live: models that finished loading are added to the front-end here (render thread, between frames). (Offline: nothing loads in the background.)
  void AdoptLoaded();
  // S103 S (live, load_on_miss = false): the assets Get() / Stored() could not answer since the last call (each once), for the caller's loader thread,
  // and the way the loaded model comes back: under the SAME key Get() looks up, so the next Get() finds it. `stored` = it was asked for by Stored(): PutLoaded
  // keeps the CPU copy only and uploads nothing.
  struct Missed { std::string pac, entry; uint32_t variant = 0; std::string key; bool stored = false; };
  std::vector<Missed> TakeMisses();
  // The loader gave up on it: remembered as failed (never queued again until ClearFailed).
  void MissFailed(const Missed& m, const std::string& error = std::string());
  void ClearFailed() { failed_.clear(); rdr_scene::namedfail::Clear(); attire_build::Latch::Instance().Clear(); }   // S103 ARM2 F2: + the build-step colours captured under the old files   // S103 DLW: a new game folder also forgets which named wrestlers failed
  // Returns the front-end model, or -1 for a Stored() asset (kept CPU-side only).
  int PutLoaded(const Missed& m, const assets::Model& model);
  uint32_t misses_this_frame() const { return misses_; }
  void NoteMiss() { ++misses_; }                       // S103 D2: a class applier's own asset is not resident (the crowd's bodies): this frame is a miss like a Get() miss
  bool load_on_miss() const { return load_on_miss_; }  // S103 D2: offline caches load in line; the live cache never loads on the render thread
  void NewFrame() { misses_ = 0; }
  // BuildFrame's per-record scratch: the asset infos and keys of the record being built (reused across records).
  std::vector<AssetInfo>& InfoScratch() { return info_; }
  void BeginRecord(const SceneTables& t);
  void EndRecord();
  // stderr, once per key (the per-frame path never prints twice).
  // S103: inline so tools that link rf_*.cpp without record_frame.cpp (c_driver, s103_B / C / C3) still link (LVF moved it to rdr::Log)
  void LogOnce(const std::string& key, const std::string& message) {
    if (logged_.insert(key).second) rdr::Log("[record_frame] %s", message.c_str());
  }
  // Applier memos kept across frames: the hash of the state last applied to a model, and a model-shaped scratch (the arena's light / placement skeleton).
  std::unordered_map<std::string, uint64_t>& StateHashes() { return state_; }
  std::unordered_map<std::string, assets::Model>& Aux() { return aux_; }
  // S103 L32F: the posed ring's per-cache slot state (rf_ring.cpp owns its type) and the ring set key it was built for: a new cache starts empty, a set-key change retires this cache's old pools
  std::shared_ptr<void>& RingStateSlot() { return ring_state_; }
  // S103 CW-2b (CW2_review bug 7): the guest-model actor path's per-cache state (guestmodel::CacheState: its byte store, the built-model memo): the live and the mirror cache each keep their own.
  std::shared_ptr<void>& GuestStateSlot() { return guest_state_; }
  std::string& RingSetKey() { return ring_set_key_; }
  // S103 PW review 9: the weapon drawer's per-cache state (rf_props.cpp owns its type): the slot id set it was built for (retire on a change), its census line printed last. A new cache starts empty.
  std::shared_ptr<void>& WeaponStateSlot() { return weapon_state_; }
  FrontEnd& front_end() { return fe_; }
  AssetSource& source() { return src_; }
 private:
  int GetNamed(const std::string& key, std::string_view pac, std::string_view entry, uint32_t variant, std::string* why);
  const assets::Model* StoredKeyed(const std::string& key, std::string_view pac, std::string_view entry, uint32_t variant, std::string* why, bool count_miss = true);
  FrontEnd& fe_;
  AssetSource& src_;
  bool load_on_miss_;
  bool keep_cpu_ = false;                            // S103 R2: wrestler models keep their CPU copy (the maps' casters read it)
  struct PropCpu { assets::Model model; uint32_t serial = 0; };   // S103 MBC
  std::unordered_map<int, PropCpu> prop_cpu_;
  std::unordered_set<int> guest_cpu_;                // S103 CS1B: the ids in cpu_ that PutGuest put there
  std::unordered_map<int, assets::Model> cpu_;       // front-end model id -> CPU copy (wrestlers only, while keep_cpu_)
  static bool IsWrestlerPac(std::string_view pac);
  std::unordered_map<std::string, int> models_;
  static constexpr size_t kMaxTaggedPerKey = 3;                          // S103 CMB fix 5: composite attire models kept per loaded key (colour sets)
  std::unordered_map<std::string, std::vector<std::string>> tagged_;     // "pac|model_key:K" -> its tagged model keys, oldest first
  void NoteTagged(const std::string& key);
  std::unordered_map<std::string, assets::Model> stored_;
  std::unordered_map<std::string, std::string> failed_;    // key -> error: a failed load is not retried
  std::unordered_map<std::string, std::vector<int>> pools_;
  std::unordered_map<std::string, uint64_t> state_;
  std::unordered_map<std::string, assets::Model> aux_;
  std::shared_ptr<void> ring_state_, weapon_state_;
  std::shared_ptr<void> guest_state_;   // S103 CW-2b
  std::string ring_set_key_;
  std::unordered_set<std::string> logged_;
  std::unordered_map<std::string, rdr::ImageId> guest_images_;   // S103 CTX2: FrontEnd::ImageKey -> the shared image (PutGuest)
  std::vector<int> retired_;
  uint64_t retired_total_ = 0;
  uint64_t uploads_ = 0;
  uint32_t misses_ = 0;
  std::vector<Missed> missed_;                       // queued, not yet taken
  std::unordered_set<std::string> pending_;          // keys queued or being loaded (deduplicated until PutLoaded / MissFailed)
  std::vector<AssetInfo> info_;
  std::vector<std::string> keys_;                    // per asset index of the current record (BeginRecord)
  std::vector<int> resolved_;                        // per asset index: the front-end model once found (-1 = not yet)
  const SceneTables* record_ = nullptr;              // the record BeginRecord was called with (keys_ / resolved_ belong to it)
};

// What one class did with a frame (the rig prints these; the live census sums them).
struct ClassReport {
  Family family = Family::Other;
  uint32_t objects = 0;   // record objects of this family
  uint32_t drawn = 0;     // turned into front-end objects / stage-hook draws
  uint32_t skipped = 0;   // left out, with the first reason below
  uint32_t holes = 0;     // S103 R: skips that leave a HOLE in the picture (asset unknown, load failed): the live caller treats a non-zero total as a miss.
                          // "not resident" is not one (the cache counts it as a miss)
  uint32_t absent_pieces = 0;   // S103 W2B (review I): spring-hair pieces the game draws and the record could not supply (their own meshes are skipped, the body draws): NOT holes, but a rig that judges "the picture is the game's" must read this
  std::string why;        // the first SKIP reason (a skip need not be a hole)
  std::string hole_why;   // S103 NRBF (review F5): the first reason that made a HOLE (empty = the family did not name one: the census falls back to `why`)
};

enum BuildMode : uint32_t {
  kBuildDraw = 0,
  kBuildWarm = 1,  // only ASK for the assets (queue misses; a loading cache loads): no posing, no Put, no objects. For the walked frames of mode 2
};

struct BuildCtx {
  const SceneTables& t;
  ModelCache& models;
  SceneFrame& frame;
  uint32_t families = kAllFamilies;  // which families to build (the rest are left out: per-class pictures)
  std::vector<ClassReport>& reports;
  // S103 R (additive):
  uint32_t mode = kBuildDraw;
  bool convert_live = false;                 // the record is in game space: Emit() mirrors the world (the one space rule); bones() is already converted
  const std::vector<Mat4>* bones_ptr = nullptr;
  const std::vector<AssetInfo>* info_ptr = nullptr;
  const std::vector<Mat4>& bones() const { return *bones_ptr; }
  const AssetInfo& info(uint32_t asset) const { static const AssetInfo none; return info_ptr && asset < info_ptr->size() ? (*info_ptr)[asset] : none; }
  const std::vector<uint8_t>* fam_ptr = nullptr;  // per record object: its Family, computed once per record
  Family family(size_t object_index) const { return Family((*fam_ptr)[object_index]); }
  bool warm() const { return mode == kBuildWarm; }
  void Emit(const Object& o);                // appends to frame.objects (space rule applied)
};

// A class applier (one per class, in the class's own file): appends its objects / stage hooks to ctx.frame and its report. Returns false only
// on a fault that makes the frame undrawable (the live caller then counts a miss). A missing asset is NOT a fault: skipped + why.
using ClassApplier = bool (*)(BuildCtx& ctx);
struct ClassEntry {
  const char* name;
  Family family;
  ClassApplier apply;
};
// The fixed table, in draw-relevant order (record_frame_classes.cpp; each class lane adds its row there, nothing else).
const std::vector<ClassEntry>& Classes();

// Builds the frame from a record: the camera, time, space rule, then every class's applier for the families asked for. width / height = the
// target's size. A Live record is converted to the space our loaders build in WITHOUT copying the tables (camera, objects and bones are converted as they go into
// `out`; `out`'s vectors keep their capacity across calls). `out` is fully reset (post, maps, hooks, params ...).
bool BuildFrame(const SceneTables& t, ModelCache& models, uint32_t families, uint32_t width, uint32_t height, SceneFrame& out,
                std::vector<ClassReport>& reports, std::string& error, uint32_t mode = kBuildDraw);
// The sum of ClassReport::holes: non-zero = a frame with a hole (the live caller counts a miss).
uint32_t TotalHoles(const std::vector<ClassReport>& reports);

}  // namespace rdr_scene::rf
