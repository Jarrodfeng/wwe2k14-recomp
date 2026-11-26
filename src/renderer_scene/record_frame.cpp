// A scene record -> a frame the front-end draws (Session 103, lane R). See record_frame.h. The same code draws a captured record offline
// (tools\scene\frame_rig, rsc_draw) and, with lane S's loader, the live record on the render thread (every frame: nothing here may cost a per-frame
// allocation storm, a stderr line, a GPU upload of something already uploaded, or a deep copy of the record).
#include "record_frame.h"
#include "rows_part_clock.h"   // S106 G4-2
#include "rows_stage_look.h"
#include "../renderer_host/wide_frame.h"   // S105 A2: TallScale (pure)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string_view>

#include "../renderer/problem.h"   // S103 LVF: rdr::Log
#include "guest_model_draw.h"  // S103 CW-2b: AbsorbRecord at the top of BuildFrame
#include "rf_appliers.h"
#include "rf_wrestlers.h"   // S110 ENTRBLACK round 6: Plants().warm_stops_at_miss (the warm-build twin)
#include "named_load_fail.h"   // S103 DLW
#include "stage_look.h"  // S103 L
#include "card_gain.h"   // S103 B3c
#include "clear_pick.h"  // S103 CLRB
#include "props_map_switch.h"   // S103 CLRB: game_clear::On (S111 PIECELINK: the CS1B guest_casters switch is gone)

namespace rdr_scene::rf {

// S103 CS1B review F1: the CPU copy a caster pass keeps holds only what LiveMapsHook::CasterOf reads (label, meshes, materials), never the decoded pictures (~6.5 MB a wrestler, up to 16 alive).
static assets::Model CasterCopy(const assets::Model& m) {
  assets::Model c;
  c.label = m.label;
  c.meshes = m.meshes;
  c.materials = m.materials;
  return c;
}

const char* FamilyName(Family f) {
  static const char* const k[] = {"arena", "ring", "props", "wrestlers", "crowd", "screens", "flashes", "particles", "other"};
  return uint8_t(f) < uint8_t(Family::Count) ? k[uint8_t(f)] : "?";
}

static bool StartsWith(std::string_view s, const char* p) { return s.rfind(p, 0) == 0; }

// By the object's stable id (SCENE_RECORD.md "Stable ids": "arena:slot:12", "actor:slot:0", "skel:AAF67D80", "crowd:card:301", "crowd:block:..."; the ring, props,
// screens and effects are named by the lanes that write them: the prefixes below are the ones the contributors use and lane C / F will use, unmatched ones are Other).
Family FamilyOf(const SceneTables& t, const Object& o) {
  const std::string_view id = t.Str(o.id);
  if (StartsWith(id, "arena:")) return Family::Arena;
  if (StartsWith(id, "ring")) return Family::Ring;
  if (StartsWith(id, "prop")) return Family::Props;
  if (StartsWith(id, "actor:") || StartsWith(id, "skel:")) return Family::Wrestlers;
  if (StartsWith(id, "crowd:") || o.klass == rdr_adapter::DrawClass::Crowd) return Family::Crowd;
  if (StartsWith(id, "screen") || StartsWith(id, "tron") || StartsWith(id, "movie")) return Family::Screens;
  if (StartsWith(id, "flash") || StartsWith(id, "beam")) return Family::Flashes;
  if (StartsWith(id, "part") || o.klass == rdr_adapter::DrawClass::Particle) return Family::Particles;
  return Family::Other;
}

ClassReport& ReportFor(BuildCtx& ctx, Family f) {
  for (ClassReport& r : ctx.reports)
    if (r.family == f) return r;
  ctx.reports.push_back(ClassReport());
  ctx.reports.back().family = f;
  return ctx.reports.back();
}

bool IsHole(const std::string& why) { return why.rfind("not resident", 0) != 0; }

uint32_t TotalHoles(const std::vector<ClassReport>& reports) {
  uint32_t n = 0;
  for (const ClassReport& r : reports) n += r.holes;
  return n;
}

// ---- ModelCache ----
ModelCache::ModelCache(FrontEnd& fe, AssetSource& source, bool load_on_miss) : fe_(fe), src_(source), load_on_miss_(load_on_miss) {}

static std::string AssetKey(const SceneTables& t, const AssetRef& a) {
  std::string k(t.Str(a.pac));
  k += '|';
  k += t.Str(a.entry);
  k += '|';
  k += std::to_string(a.variant);
  return k;
}

void ModelCache::BeginRecord(const SceneTables& t) {
  record_ = &t;
  keys_.resize(t.assets.size());
  for (size_t i = 0; i < t.assets.size(); ++i) keys_[i] = AssetKey(t, t.assets[i]);
  resolved_.assign(t.assets.size(), -1);
}

void ModelCache::EndRecord() { record_ = nullptr; }


// S103 DLW: a named WRESTLER model that failed to load: the record stops naming it (the wrestler contributor reads namedfail) and the GuestModel rows draw the body (COV2 GAP 2 part 3).
static void NoteNamedFail(std::string_view pac, std::string_view entry, uint32_t variant, const std::string& err) {
  if (rdr_scene::namedfail::Note(pac, entry, variant)) {
    if (rdr_scene::namedfail::IsHole(pac, entry, variant)) {   // S103 ARM (review F7): a composite / remapped attire is a hole, the guest rows do not draw it
      rdr::Log("[wwe2k14][wrestler_src] key %.*s variant %u: load failed (%s): a hole (composite attire), the guest rows do not draw it", int(entry.size()), entry.data(), variant, err.c_str());
      return;
    }
    rdr::Log("[wwe2k14][wrestler_src] key %.*s variant %u: load failed (%s): the record stops naming it, the game's own geometry (guest rows) draws it", int(entry.size()), entry.data(), variant, err.c_str());
  }
}

int ModelCache::GetNamed(const std::string& key, std::string_view pac, std::string_view entry, uint32_t variant, std::string* why) {
  auto say = [&](const std::string& s) { if (why) *why = s; };
  auto it = models_.find(key);
  if (it != models_.end()) return it->second;
  auto fl = failed_.find(key);
  if (fl != failed_.end()) { say("load failed: " + fl->second); return -1; }
  if (!load_on_miss_) {
    ++misses_;
    if (pending_.insert(key).second) missed_.push_back({.pac = std::string(pac), .entry = std::string(entry), .variant = variant, .key = key, .stored = false, .gates = true});  // S105 SKB3: designated (the positional init put count_miss into bulk)
    say("not resident");
    return -1;
  }
  assets::Model m;
  std::string err;
  if (!src_.Load(std::string(pac), std::string(entry), variant, m, err)) {
    failed_[key] = err;
    NoteNamedFail(pac, entry, variant, err);
    say("load failed: " + err);
    return -1;
  }
  const int id = Put(key, m);
  if (keep_cpu_ && IsWrestlerPac(pac)) cpu_[id] = CasterCopy(m);   // S103 R2 (CS1B F1: only what CasterOf reads)
  return id;
}

bool ModelCache::IsWrestlerPac(std::string_view pac) { return pac.find("\\ch\\") != std::string_view::npos || pac.find("/ch/") != std::string_view::npos; }

const assets::Model* ModelCache::CpuModel(int fe_model) const {
  const auto it = cpu_.find(fe_model);
  return it == cpu_.end() ? nullptr : &it->second;
}

void ModelCache::SetPropCpu(int fe_model, const assets::Model& posed) {   // S103 MBC
  if (!keep_cpu_ || fe_model < 0) return;
  PropCpu& p = prop_cpu_[fe_model];
  bool same = p.model.meshes.size() == posed.meshes.size();   // a re-pose keeps the shape: only the vertices are assigned (the vectors keep their capacity)
  for (size_t i = 0; same && i < posed.meshes.size(); ++i) same = p.model.meshes[i].vertices.size() == posed.meshes[i].vertices.size() && p.model.meshes[i].indices.size() == posed.meshes[i].indices.size();
  if (same) {
    for (size_t i = 0; i < posed.meshes.size(); ++i) p.model.meshes[i].vertices = posed.meshes[i].vertices;
  } else {
    p.model.meshes = posed.meshes;   // the geometry the caster pass reads (materials are read for alpha_blend)
    p.model.materials = posed.materials;
  }
  ++p.serial;
}

void ModelCache::DropPropCpu(int fe_model) { prop_cpu_.erase(fe_model); }   // S103 MBC review 5

const assets::Model* ModelCache::PropCpuModel(int fe_model, uint32_t& serial) const {
  const auto it = prop_cpu_.find(fe_model);
  if (it == prop_cpu_.end()) return nullptr;
  serial = it->second.serial;
  return &it->second.model;
}

int ModelCache::Get(const SceneTables& t, uint32_t asset_index, std::string* why) {
  auto say = [&](const std::string& s) { if (why) *why = s; };
  if (asset_index >= t.assets.size()) { say("unknown asset index " + std::to_string(asset_index)); return -1; }
  const AssetRef& a = t.assets[asset_index];
  if (a.kind != uint8_t(AssetKind::Model)) { say("not a model asset"); return -1; }
  if (a.pac == 0) { say("asset unknown (no pac: a contributor placeholder)"); return -1; }
  const bool cached = record_ == &t && keys_.size() == t.assets.size();
  if (cached && resolved_[asset_index] >= 0) return resolved_[asset_index];
  const std::string local = cached ? std::string() : AssetKey(t, a);
  const int id = GetNamed(cached ? keys_[asset_index] : local, t.Str(a.pac), t.Str(a.entry), a.variant, why);
  if (cached && id >= 0) resolved_[asset_index] = id;
  return id;
}

const assets::Model* ModelCache::StoredKeyed(const std::string& key, std::string_view pac, std::string_view entry, uint32_t variant, std::string* why, bool count_miss) {
  auto say = [&](const std::string& s) { if (why) *why = s; };
  auto it = stored_.find(key);
  if (it != stored_.end()) return &it->second;
  auto fl = failed_.find(key);
  if (fl != failed_.end()) { say("load failed: " + fl->second); return nullptr; }
  if (!load_on_miss_) {
    if (count_miss) ++misses_;
    if (pending_.insert(key).second) missed_.push_back({.pac = std::string(pac), .entry = std::string(entry), .variant = variant, .key = key, .stored = true, .gates = count_miss});  // S105 SKB3
    say("not resident");
    return nullptr;
  }
  assets::Model m;
  std::string err;
  if (!src_.Load(std::string(pac), std::string(entry), variant, m, err)) {
    failed_[key] = err;
    say("load failed: " + err);
    return nullptr;
  }
  return &stored_.emplace(key, std::move(m)).first->second;
}

const assets::Model* ModelCache::Stored(const SceneTables& t, uint32_t asset_index, std::string* why) {
  auto say = [&](const std::string& s) { if (why) *why = s; };
  if (asset_index >= t.assets.size()) { say("unknown asset index"); return nullptr; }
  const AssetRef& a = t.assets[asset_index];
  if (a.pac == 0) { say("asset unknown (no pac)"); return nullptr; }
  const bool cached = record_ == &t && keys_.size() == t.assets.size();
  const std::string local = cached ? std::string() : AssetKey(t, a);
  return StoredKeyed(cached ? keys_[asset_index] : local, t.Str(a.pac), t.Str(a.entry), a.variant, why);
}

const assets::Model* ModelCache::StoredNamed(const std::string& pac, const std::string& entry, uint32_t variant, std::string* why, bool count_miss) {
  const std::string key = pac + "|" + entry + "|" + std::to_string(variant);
  return StoredKeyed(key, pac, entry, variant, why, count_miss);
}

void ModelCache::DropStored(const std::string& pac, const std::string& entry, uint32_t variant) { stored_.erase(pac + "|" + entry + "|" + std::to_string(variant)); }

int ModelCache::Put(const std::string& key, const assets::Model& m, uint8_t group) {
  std::vector<std::string> warnings;
  const int id = fe_.AddModel(m, warnings);
  if (group != kGroupFromLabel) fe_.SetModelGroup(id, group);   // S103 GSTP
  ++uploads_;
  for (const std::string& w : warnings) LogOnce("warn#" + w, w);
  models_[key] = id;
  NoteTagged(key);
  return id;
}

// S103 CMB fix 5: a composite attire's models ("pac|model_key:K/c:<hex>|variant", one per distinct colour set: the colours alone decide the picture) are kept as a small LRU per loaded key
// (kMaxTaggedPerKey): a body that gets new colours for the same key retires the oldest instead of keeping every colour set a session ever saw (each is ~1.4 MB of BC1 + the mesh).
void ModelCache::NoteTagged(const std::string& key) {
  const size_t t = key.find("/c:");
  if (t == std::string::npos || key.find("|model_key:") == std::string::npos) return;
  auto& lru = tagged_[key.substr(0, t)];
  lru.erase(std::remove(lru.begin(), lru.end(), key), lru.end());
  lru.push_back(key);
  while (lru.size() > kMaxTaggedPerKey) {
    const std::string old = lru.front();
    lru.erase(lru.begin());
    RetireOther(old, "\x01");   // (the key itself is the prefix: nothing else starts with a 96-hex tag + '|')
  }
}

int ModelCache::PutGuest(const std::string& key, const assets::Model& m, uint8_t group) {
  std::vector<std::string> warnings;
  const int id = fe_.AddModel(m, warnings, &guest_images_);
  if (group != kGroupFromLabel) fe_.SetModelGroup(id, group);   // S103 GSTP (merge with CTX2)
  ++uploads_;
  for (const std::string& w : warnings) LogOnce("warn#" + w, w);
  models_[key] = id;
  if (keep_cpu_ && id >= 0) cpu_[id] = CasterCopy(m), guest_cpu_.insert(id);   // S103 CS1B: a guest body casts into the shadow maps like a file wrestler (CS1.md 4.1; the game's map B draws the created body, sub_827E8080); RetireOther erases it with the model. S111 PIECELINK F6: no guest-only switch (the stream build's default was off): the one keep rule of Put / GetNamed
  return id;
}

void ModelCache::TrimGuestImages(const assets::Model& keep) {
  std::unordered_set<std::string> used;
  for (const assets::Texture& t : keep.textures) used.insert(FrontEnd::ImageKey(t));
  for (auto it = guest_images_.begin(); it != guest_images_.end();) {
    if (used.count(it->first)) ++it;
    else fe_.ReleaseImage(it->second), it = guest_images_.erase(it);
  }
}

int ModelCache::Find(const std::string& key) const {
  auto it = models_.find(key);
  return it == models_.end() ? -1 : it->second;
}

int ModelCache::PosedFind(const std::string& pool, size_t index) const {
  const auto it = pools_.find(pool);
  return it != pools_.end() && index < it->second.size() ? it->second[index] : -1;
}

int ModelCache::PosedModel(const std::string& pool, size_t index, const assets::Model& posed, bool textureless, uint8_t group) {
  std::vector<int>& v = pools_[pool];
  if (index < v.size() && v[index] >= 0) {
    if (fe_.UpdateModelVertices(v[index], posed)) return v[index];
    prop_cpu_.erase(v[index]);   // S103 MBC review 3: the CPU copy goes with the model
    retired_.push_back(v[index]);  // the shape changed (not expected): replace it
    v[index] = -1;
  }
  if (v.size() <= index) v.resize(index + 1, -1);
  const bool share = index > 0 && v[0] >= 0;
  if (textureless && !share) return -1;   // S103 C5: no images to build from and no owner to borrow them from
  std::vector<std::string> warnings;
  const int id = share ? fe_.AddModelSharing(posed, v[0], warnings) : fe_.AddModel(posed, warnings);
  if (group != kGroupFromLabel) fe_.SetModelGroup(id, group);   // S103 GSTP
  v[index] = id;
  ++uploads_;
  return id;
}

void ModelCache::RetireOther(const std::string& prefix, const std::string& keep) {
  const auto drop = [&](const std::string& key) { return key.rfind(prefix, 0) == 0 && key.find(keep) == std::string::npos; };
  for (auto it = models_.begin(); it != models_.end();) {
    if (drop(it->first)) { cpu_.erase(it->second); guest_cpu_.erase(it->second); retired_.push_back(it->second); it = models_.erase(it); }   // (S103 R2: cpu_ goes with the model)
    else ++it;
  }
  for (auto it = pools_.begin(); it != pools_.end();) {
    if (drop(it->first)) {
      for (size_t i = it->second.size(); i-- > 0;)  // sharers first, the image owner (slot 0) last
        if (it->second[i] >= 0) retired_.push_back(it->second[i]);
      for (int pid : it->second) prop_cpu_.erase(pid);   // S103 MBC
      it = pools_.erase(it);
    } else ++it;
  }
  for (auto it = stored_.begin(); it != stored_.end();) it = drop(it->first) ? stored_.erase(it) : std::next(it);
  for (auto it = state_.begin(); it != state_.end();) it = drop(it->first) ? state_.erase(it) : std::next(it);
  for (auto it = aux_.begin(); it != aux_.end();) it = drop(it->first) ? aux_.erase(it) : std::next(it);
}

void ModelCache::ReleaseRetired() {
  for (int id : retired_) fe_.ReleaseModel(id), ++retired_total_;
  retired_.clear();
}

std::vector<ModelCache::Missed> ModelCache::TakeMisses() {
  std::vector<Missed> out;
  out.swap(missed_);
  return out;
}

void ModelCache::MissFailed(const Missed& m, const std::string& error) {
  pending_.erase(m.key);
  failed_[m.key] = error.empty() ? "the loader gave up" : error;
  if (!m.stored) NoteNamedFail(m.pac, m.entry, m.variant, failed_[m.key]);   // S103 DLW
}

int ModelCache::PutLoaded(const Missed& m, const assets::Model& model) {
  pending_.erase(m.key);
  failed_.erase(m.key);
  if (m.stored) {  // the CPU copy only: an applier re-poses it; nothing of it is ever drawn as it is
    stored_[m.key] = model;
    return -1;
  }
  const int id = Put(m.key, model);
  if (keep_cpu_ && IsWrestlerPac(m.pac)) cpu_[id] = CasterCopy(model);   // S103 R2 (CS1B F1)
  return id;
}

void ModelCache::AdoptLoaded() {}  // offline: nothing loads in the background (lane S's loader thread fills this in)

// ---- the one space rule (SCENE_RECORD "One space rule for Live records", FRAME_CONTRACT 7) ----
// A Live record is in the game's space (file space, the game's camera); our loaders build meshes in the viewer's space (FinishModel mirrors Y). Converted as it goes
// into the frame: the camera takes S on the world side (view' = S * view; the eye's y negates), every object's world and every bone palette S X S (S = diag(1,-1,1)).
// The record itself is never copied or modified.
static void MirrorMat(Mat4& m) {
  assets::Matrix a;
  std::memcpy(a.data(), m.m, sizeof(float) * 16);
  a = assets::MirrorY(a);
  std::memcpy(m.m, a.data(), sizeof(float) * 16);
}

void BuildCtx::Emit(const Object& o) {
  frame.objects.push_back(o);
  if (convert_live) MirrorMat(frame.objects.back().world);
}

static bool IsCrowdEntry(const std::string& entry) {
  int num = 0, type = 0;
  return std::sscanf(entry.c_str(), "card:%d", &type) == 1 || (std::sscanf(entry.c_str(), "%d", &num) == 1 && num >= 20000 && num < 20200);
}

bool BuildFrame(const SceneTables& t, ModelCache& models, uint32_t families, uint32_t width, uint32_t height, SceneFrame& out,
                std::vector<ClassReport>& reports, std::string& error, uint32_t mode) {
  const bool warm = mode == kBuildWarm;
  const bool live = t.header.source == uint8_t(Source::Live);
  if (!warm && !t.has_camera) { error = "the record has no camera (a live record carries none yet: the caller supplies one)"; return false; }
  // reset `out` field by field: the vectors keep their capacity from the last frame
  out.objects.clear(), out.piece_alphas.clear(), out.absent_extras.clear(), out.object_engine.clear(), out.wrinkle_levels.clear(), out.change_colours.clear() /* S108 THR2 */, out.body_spheres.clear() /* S111 SPHEREOCC */, out.draw_places.clear() /* S111 PROPORDER */, out.params.clear(), out.param_names.clear(), out.light_sets.clear(), out.model_lights.clear(), out.stage_hooks.clear();
  out.entrance = out.entrance_bg = false;
  if (stage_look::Entrance(t)) out.entrance = out.entrance_bg = true;
  out.phase_word = -1;   // S103 D1: the phase word of the frame (the record's kSlPhase row), -1 = none
  card_gain::FromRows(t.stage_look, out.card_factor_valid, out.card_factor);   // S103 B3: the card factor row (the game's word * 0.001), absent = not read
  out.participants = out.ring_gate = out.plane_game_off = -1;
  for (const StageLookRow& r : t.stage_look) if (r.kind == kSlPhase) out.phase_word = int64_t(r.flags), out.participants = PhaseRowParticipants(r), out.ring_gate = PhaseRowRingGate(r), out.plane_game_off = PlaneGameOffOf(PhaseRowPlaneModule(r), out.ring_gate);  // S103 L: the stage look (STAGE_LIGHTING.md): an entrance is running -> the ring / prop / arena classes take their light-map variant (the game does)
  out.viewer_space = true;  // the objects are in viewer space: a Viewer record as written, a Live record converted by Emit()
  out.post = nullptr, out.maps = nullptr;
  out.names = &t;   // S103 P7 tween (review F1): the record's tables name an object by TEXT for the own blend key (set here, by the live draw / the host; never by the viewer)
  out.part_clocks = t.part_clocks.empty() ? nullptr : t.part_clocks.data(), out.part_clock_count = t.part_clocks.size();  // S103 E5: the UV-scroll clocks of the stage slots (front_end.cpp Ctx time)
  out.particles = t.particles.empty() ? nullptr : &t.particles;  // S103 F: the record's particle tables (a LiveParticles hook, owned by the caller, draws them)
  out.width = width, out.height = height;
  out.time_s = t.header.time_s;   // S103 RCL: the live record never sets header.time_s (0); the game's Time is the per-part clock (kScPartClocks), draws without one are counted (time_frozen)
  clear_pick::Pick(t.stage_look, live, game_clear::On(), out.clear);   // S103 CLRB: a live record clears to the game's own colour (the latch's kSlClearColor row; black without one), a viewer record keeps the slate
  out.camera = t.camera;
  if (t.camera.flags & kCamProjFromFov) {   // S103 S: the record carries the shown camera's view + FOV (the composer's copy, lane A); the projection is built for our target here
    // D3D-style row-vector perspective (clip = [p 1] * view * proj, depth 0..1). The game's view space is +x right, +y DOWN, +z forward (camera.md), so the
    // y scale is negative to put screen-down at clip -y. near / far from the record when given, else 10 / 300000 (the camera objects' own values seen in run
    // 7259) [C: lane R2 factored the capture's own c56-c59 ViewProj (brock_losswin f20/110/200): fov 42-44.7 deg, aspect 1.7778, near 10.0, far ~2.997e5,
    // a NEGATIVE y scale for a proper-rotation view; OFFLINE_RIG.md]. The shown camera's own near / far fields are not read: the record leaves them 0.
    const float aspect = height ? float(width) / float(height) : 16.f / 9.f;
    const float n = t.camera.near_z > 0.f ? t.camera.near_z : 10.f, f = t.camera.far_z > n ? t.camera.far_z : 300000.f;
    // S105 A2: Hor+ keeps the game's vertical FOV (aspect >= 16:9: the factor is 1, byte-identical); a TALLER target keeps the game's horizontal view and shows more above and below (Vert+, wide_frame.h TallScale).
    const float ys = (1.f / std::tan(t.camera.fov_y_deg * 3.14159265f / 360.f)) * wwe2k14::scene_host::wide_frame::TallScale(aspect), xs = ys / aspect;
    Mat4 p{};
    p.m[0] = xs, p.m[5] = -ys, p.m[10] = f / (f - n), p.m[11] = 1.f, p.m[14] = -n * f / (f - n), p.m[15] = 0.f;
    out.camera.proj = p;
    out.camera.flags &= ~uint32_t(kCamProjFromFov);
  }
  out.bones.clear();
  if (!warm) {
    if (live) {
      for (int k = 4; k < 8; ++k) out.camera.view.m[k] = -out.camera.view.m[k];
      out.camera.eye[1] = -out.camera.eye[1];
    }
    out.bones.assign(t.bones.begin(), t.bones.end());
    if (live) for (Mat4& b : out.bones) MirrorMat(b);
  }
  reports.clear();
  models.NewFrame();
  models.BeginRecord(t);
  guestmodel::AbsorbRecord(guestmodel::StateIn(models.GuestStateSlot()), t);   // S103 CW-2b (CW2_review bug 6): every record's guest-model bytes are taken here, whether or not an actor asks for them
  // the assets, parsed once for this record
  std::vector<AssetInfo>& info = models.InfoScratch();
  info.assign(t.assets.size(), AssetInfo());
  for (size_t i = 0; i < t.assets.size(); ++i) {
    const AssetRef& a = t.assets[i];
    if (a.kind != uint8_t(AssetKind::Model) || a.pac == 0) continue;
    const std::string_view pacv = t.Str(a.pac);
    if (pacv.size() < 12 || pacv.find("bg") == std::string_view::npos) continue;
    std::string pac(pacv);
    for (char& c : pac) if (c == '/') c = '\\';
    int arena = 0;
    if (std::sscanf(pac.c_str(), "pac\\bg\\bg%d.pac", &arena) != 1) continue;
    const std::string entry(t.Str(a.entry));
    int k = 0;
    if (std::sscanf(entry.c_str(), "slot %d", &k) == 1) { info[i].kind = AssetInfo::ArenaSlot, info[i].arena = arena, info[i].slot = k; }
    else if (IsCrowdEntry(entry)) { info[i].kind = AssetInfo::Crowd, info[i].arena = arena; }
  }
  BuildCtx ctx{t, models, out, families, reports};
  ctx.mode = mode;
  ctx.convert_live = live;
  ctx.bones_ptr = warm ? &t.bones : &out.bones;
  ctx.info_ptr = &info;
  // Every family's object count first (so a family with objects and no applier is still said).
  uint32_t counts[size_t(Family::Count)] = {};
  static thread_local std::vector<uint8_t> fam;
  fam.resize(t.objects.size());
  for (size_t i = 0; i < t.objects.size(); ++i) ++counts[fam[i] = uint8_t(FamilyOf(t, t.objects[i]))];
  ctx.fam_ptr = &fam;
  bool ok = true;
  for (const ClassEntry& e : Classes())
    if (!e.apply(ctx)) {
      if (ok) error = std::string("class ") + e.name + " could not build the frame";
      ok = false;
      if (!warm || Plants().warm_stops_at_miss) break;   // S110 ENTRBLACK round 5: a warm build (ask only) asks EVERY class's assets in the same build, so the props / wrestlers / crowd are not asked only after the arena arrives
    }
  models.EndRecord();
  if (!ok) return false;
  for (size_t f = 0; f < size_t(Family::Count); ++f) {
    if (!counts[f]) continue;
    ClassReport& r = ReportFor(ctx, Family(f));
    r.objects = counts[f];
    if (r.drawn + r.skipped == 0 && (families & (1u << f)) && !warm) { r.skipped = counts[f]; r.holes = counts[f]; r.why = "no applier for this family yet"; }
  }
  return true;
}

}  // namespace rdr_scene::rf
