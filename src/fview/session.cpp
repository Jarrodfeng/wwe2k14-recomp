#include "session.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <cstring>

#include "../renderer/frame_builder.h"
#include "../renderer/player/png.h"  // lane 155: WritePng for --dump-frames
#include "../renderer_adapter/binder.h"
#include "../renderer_adapter/image_info_table.h"  // S101 lane 47
#include "../renderer_adapter/image_content_feed.h"  // S101 lane 66: the UI's image-content table, fed from the capture's contents
#include <map>
#include <set>
#include "../renderer_scene/crowd_ours.h"  // S101 lane 35
#include "../renderer_scene/post_ours.h"  // S101 lane PO
#include "../renderer_scene/crowd_floor_synth.h"  // S101 lane 82: test-only floor members
#include "../renderer_scene/record_io.h"
#include "../renderer_scene/circle_oracle.h"  // lane 136: the circle blobs' capture-side oracle
#include "../renderer_scene/plane_oracle.h"  // lane 139: the plane chain's capture-side oracle
#include "../renderer_scene/replace_factories.h"  // S101 lane 41
#include "../renderer_adapter/replace_test_hook.h"

#include <chrono>
#include "../renderer_adapter/lasting_prune.h"   // lane 307: D3_SKIP_LASTING=prune
#include <memory>
#include <thread>

namespace fview {

const char* OutcomeText(uint8_t outcome) {
  switch (rdr_adapter::BindOutcome(outcome)) {
    case rdr_adapter::BindOutcome::NotADraw: return "not a draw";
    case rdr_adapter::BindOutcome::LeftOut: return "left out (settings, shadow skip or isolate)";
    case rdr_adapter::BindOutcome::NoShader: return "not drawn: no shader for its hash";
    case rdr_adapter::BindOutcome::BadLayout: return "not drawn: vertex layout does not fit the effect";
    case rdr_adapter::BindOutcome::Bound: return "drawn";
    case rdr_adapter::BindOutcome::NoGeometry: return "not drawn: no geometry";
  }
  return "?";
}

struct Session::Impl {
  std::unique_ptr<rdr_adapter::Binder> binder;
  rdr::FrameBuilder builder;
  rdr::Frame full;  // the selected frame, bound (spans point into builder)
  uint32_t replayed = 0;  // frames played in full
  bool have_state = false;
  struct Override {
    std::vector<uint32_t> spirv, alpha;
    bool drop_twin = false;
  };
  std::map<int, Override> overrides;
  std::map<std::string, std::vector<HistoryEntry>> history_cache;  // key: frame / surface / x / y; dropped when the frame is bound again
  std::vector<HistoryEntry> empty_history;
  std::vector<rdr::Op> segment;  // the ops of the segment being rendered (the Frame handed to the renderer points into it)
  std::unique_ptr<rdr_scene::CrowdOurs> crowd;  // S101 lane 35: the crowd drawn by us (--crowd-ours)
  std::unique_ptr<rdr_scene::PostOurs> post;    // S101 lane PO: the post chain recorded by our own pass (--post-ours)
  std::unique_ptr<rdr_adapter::ReplaceTestHook> replace_test;  // S101 lane 41: --replace-test
  // S101 lane 47 (review D5): the supplier table is declared BEFORE the hooks that call it, so it is destroyed after them
  rdr_adapter::ImageInfoTable image_table;     // S101 lane 47: the live supplier's table, fed from the capture in the "live" modes
  std::string image_info_mode = "capture";   // S101 lane 47 (SetImageInfoMode)
  // S101 lane 66 (UI P2a): the content table the UI hook joins by (declared before the hooks that call its supplier), and the plant `stale_after_join`: after a 2D picture
  // was bound in an earlier frame, its bytes in the STREAM's image are replaced (the reader never re-read a CPU rewrite, S100's class); the table is not told, ours must not move.
  rdr_adapter::ImageContentTable content_table;
  rdr_adapter::LoadedTexture content_scratch;
  bool ui_on = false, stale_after_join = false;
  std::map<uint32_t, rdr_adapter::ContentRef> stale_latest;
  std::set<uint32_t> stale_seen, stale_done;
  uint64_t stale_patched = 0;
  std::vector<rdr_scene::ReplaceHookHandle> replace_handles;   // S101 lane 41: --arena-ours / --wrestlers-ours (the classes' hooks)
  std::vector<std::string> replace_kinds;                      // parallel to replace_handles
  bool thin_warned = false;
  uint32_t thin_resize_id = 0;
  std::string d3_ledger_out;   // lane 138
  bool d3_casters = false;     // lane 217 (E2b): --d3 casters (the caster class's own chain; the arena's is off)
  bool d3_ring = false, d3_arena_beside_ring = false;   // lane 229 (Ring D3 R3): --d3 ring (the ring chain, binder_ring_d3.inc), alone or beside the arena's
  bool d3_wrestlers = false;   // lane 227 (H2): --d3 wrestlers (the wrestler class's own chain; needs --wrestlers-ours --thin wrestlers; the arena's is off)
  bool d3_records_on = false, d3_records_late = false, d3_records_preload = false;   // lane 152 (review B1): the record arm delivers record i when it binds frame i (the live order); late = the old live pairing (plant); preload = every record up front (old)
  bool thin_on = false;                                        // lane 122: --thin (the reader emulation runs before every Bind)
  int thin_lag = 2;
  rdr_adapter::thin::PinPublisher thin_pub;                    // (declared before use by the binder; the session resets the binder first)
  bool crowd_floor = false;                     // S101 lane 82
  bool crowd_cards = false, crowd_cards_unchecked = false;  // S101 lane CD
  bool crowd_cards_thin = false;  // lane 175
  bool crowd_members_thin = false, crowd_members_probe = false;   // lane 272
  struct MemberCount { uint32_t seen, dropped; uint64_t world; };
  std::unordered_map<uint64_t, MemberCount> member_counts[2];   // lane 272: [site] hook frame -> (op-15 calls seen, dropped, world sum), the cut tool's sidecar
  struct ThinCount { uint32_t seen, dropped; uint64_t world; };
  std::unordered_map<uint64_t, ThinCount> thin_counts;   // lane 175: hook frame -> (card op-15 calls seen, dropped, world sum), the cut tool's sidecar
  bool hash_frames = false;                     // S101 lane CD
  std::set<uint32_t> dump_frames;               // lane 155: --dump-frames
  std::string dump_dir;
  uint32_t present_format = 0;
  std::vector<uint64_t> frame_hashes;
  std::unique_ptr<rdr_scene::FloorSynth> floor_synth;  // S101 lane 82: --crowd-floor-synth (test only)
  int crowd_offset = -1;                        // the pairing (lane 36): stream frame j takes record j-1; the planted fault uses 0
  int crowd_record_index = -1;
  // lane 136: --circles-oracle. The oracle file's constants are laid out as guest memory and played through the SHIPPED writer's tracker (circle_oracle.h) once per stream frame; the
  // entries (stamped one frame before the stream frame's guest frame, so the hook's offset search has something to find) go to the "circles" hook as a record.
  bool circle_oracle_on = false;
  rdr_scene::circles::oracle::File circle_file;
  rdr_scene::circles::oracle::FakeGuest circle_guest;
  rdr_scene::live::circles::Tracker circle_tracker;
  std::vector<int> circle_by_capture;   // capture frame -> index into circle_file.frames (-1 none)
  int circle_last_f = -1;
  std::shared_ptr<const rdr_scene::SceneTables> circle_last_record;
  // lane 139: --plane-oracle, like the circles': the oracle's constants played through the SHIPPED tracker once per stream frame, entries stamped one frame before the header frame.
  bool plane_oracle_on = false;
  rdr_scene::plane::oracle::File plane_file;
  rdr_scene::plane::oracle::FakeGuest plane_guest;
  rdr_scene::live::plane::Tracker plane_tracker;
  std::vector<int> plane_by_capture;
  int plane_last_f = -1;
  std::shared_ptr<const rdr_scene::SceneTables> plane_last_record;
  std::vector<uint32_t> crowd_by_frame;  // members drawn by us per replayed frame
  std::shared_ptr<const rdr_scene::SceneTables> crowd_record;  // the last record read (re-binding the same frame reads nothing)
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() {
  if (renderer_) renderer_->WaitIdle();
  impl_->binder.reset();
  impl_->crowd.reset();  // (the hook goes before the renderer; the binder no longer points at it)
  impl_->post.reset();   // S101 lane PO (same reason: its PostPass destroys images through the renderer; found by an intermittent exit crash under --validation)
  impl_->replace_test.reset();  // S101 lane 41 (same reason)
  impl_->replace_handles.clear();
}

// S101 lane 66: call right after ApplyContents(frame): the frame's content writes into the UI's content table (the same feeder the game's render thread uses), then the
// stale_after_join plant. A template only so it can name the private Impl.
template <class I>
static void FeedUi(I& im, const rdr_adapter::CaptureReader& capture, rdr::Renderer& renderer, const rdr_adapter::LoadedFrame& frame) {
  if (!im.ui_on) return;
  rdr_adapter::FeedImageContents(frame, capture, im.content_table, im.content_scratch);
  if (!im.stale_after_join) return;
  for (const rdr_adapter::ContentRef& ref : frame.contents) im.stale_latest[ref.image] = ref, im.stale_done.erase(ref.image);   // (new content: clean again)
  // Pictures bound in an EARLIER frame get stale bytes now (every byte inverted); the table is not told.
  for (const uint32_t id : im.stale_seen) {
    if (!im.stale_done.insert(id).second) continue;
    const auto ref = im.stale_latest.find(id);
    rdr_adapter::LoadedTexture tex;
    if (ref == im.stale_latest.end() || !capture.ReadTexture(ref->second, tex)) continue;
    std::vector<uint8_t> bad(tex.data, tex.data + tex.header.data_bytes);
    for (uint8_t& b : bad) b = uint8_t(~b);
    for (uint32_t s = 0; s < tex.header.subresource_count; ++s) {
      const rdr_adapter::Subresource& sub = tex.subresources[s];
      renderer.UpdateImage(im.binder->Mapped(id), sub.mip, sub.layer, bad.data() + sub.offset, size_t(sub.row_bytes) * sub.rows);
    }
    ++im.stale_patched;
  }
  for (uint32_t i = 0; i < frame.draws.count; ++i) {   // this frame's 2D pictures become "bound earlier" for the next frame
    const rdr_adapter::DrawOrigin* origin = frame.OriginOf(i);
    if (!origin || origin->op != 19 || (origin->call_site != 0x82BC5688u && origin->call_site != 0x82BC3784u && origin->call_site != 0x82BC8BECu)) continue;
    const rdr_adapter::Draw& d = frame.draws[i];
    for (uint32_t t = 0; t < d.texture_count && d.texture_first + t < frame.texture_slots.count; ++t)
      if (frame.texture_slots[d.texture_first + t].slot == 0 && frame.texture_slots[d.texture_first + t].image) im.stale_seen.insert(frame.texture_slots[d.texture_first + t].image);
  }
}

void Session::FeedCrowd(uint32_t f) {
  Impl& im = *impl_;
  if (!im.crowd && im.replace_handles.empty()) return;
  const int index = int(f) + im.crowd_offset;  // S101 lane 36: stream frame j is drawn with record j-1 (bit-exact, 178/178 frames): crowd_offset is -1 by default
  if (index != im.crowd_record_index) {
    im.crowd_record_index = index;
    im.crowd_record.reset();
    if (index >= 0 && uint32_t(index) < capture_->frame_count() && capture_->HasSceneRecord(uint32_t(index))) {
      auto tables = std::make_shared<rdr_scene::SceneTables>();
      std::string err;
      if (rdr_scene::ReadRecord(capture_->SceneRecordPath(uint32_t(index)).string(), *tables, err)) {
        if (im.floor_synth) im.floor_synth->Apply(f, *tables);  // S101 lane 82 (test only): the stream frame f's floor members into the copy of record f-1
        im.crowd_record = tables;
      }
      else std::fprintf(stderr, "[fview] scene record %d: %s\n", index, err.c_str());
    }
  }
  if (im.crowd) im.crowd->SetRecord(im.crowd_record);
  for (size_t hi = 0; hi < im.replace_handles.size(); ++hi) {  // S101 lane 41: the same record (j-1) for the classes' hooks
    rdr_scene::ReplaceHookHandle& h = im.replace_handles[hi];
    if (im.plane_oracle_on && im.replace_kinds[hi] == "plane_shadow") {   // lane 139: fed by the writer's tracker, joined by stamp
      if (int(f) != im.plane_last_f) {
        im.plane_last_f = int(f);
        im.plane_last_record.reset();
        const int oi = f < im.plane_by_capture.size() ? im.plane_by_capture[f] : -1;
        if (oi >= 0) {
          const rdr_scene::plane::oracle::Frame& fr = im.plane_file.frames[size_t(oi)];
          rdr_scene::plane::oracle::DriveFrame(im.plane_tracker, im.plane_guest, fr, fr.guest_frame - 1);
          std::vector<rdr_scene::PlaneFrame> frames;
          std::vector<rdr_scene::PlaneDraw> draws;
          im.plane_tracker.Take(frames, draws);
          auto tables = std::make_shared<rdr_scene::SceneTables>();
          rdr_scene::plane::oracle::FillRecord(*tables, frames, draws, fr.guest_frame);
          im.plane_last_record = tables;
        }
      }
      if (h.set_record) h.set_record(im.plane_last_record);
      continue;
    }
    if (im.circle_oracle_on && im.replace_kinds[hi] == "circles") {   // lane 136: the circle hook is fed by the writer's tracker, joined by stamp (not by this j-1 pairing)
      if (int(f) != im.circle_last_f) {
        im.circle_last_f = int(f);
        im.circle_last_record.reset();
        const int oi = f < im.circle_by_capture.size() ? im.circle_by_capture[f] : -1;
        if (oi >= 0) {
          const rdr_scene::circles::oracle::Frame& fr = im.circle_file.frames[size_t(oi)];
          rdr_scene::circles::oracle::DriveFrame(im.circle_tracker, im.circle_guest, fr, fr.guest_frame - 1);
          std::vector<rdr_scene::CircleShadow> entries;
          rdr_scene::CircleMesh mesh;
          bool mesh_present = false;
          im.circle_tracker.Take(entries, &mesh, mesh_present, true);
          auto tables = std::make_shared<rdr_scene::SceneTables>();
          rdr_scene::circles::oracle::FillRecord(*tables, entries, mesh_present ? &mesh : nullptr, fr.guest_frame);
          im.circle_last_record = tables;
        }
      }
      if (h.set_record) h.set_record(im.circle_last_record);
      continue;
    }
    if (h.set_record) h.set_record(im.crowd_record);
  }
}

bool Session::SetPlaneOracle(const std::string& path, std::string& error) {   // lane 139
  Impl& im = *impl_;
  if (!rdr_scene::plane::oracle::Load(path, im.plane_file, error)) return false;
  uint32_t top = 0;
  for (const auto& fr : im.plane_file.frames) top = std::max(top, fr.capture_frame + 1);
  im.plane_by_capture.assign(top, -1);
  for (size_t i = 0; i < im.plane_file.frames.size(); ++i) im.plane_by_capture[im.plane_file.frames[i].capture_frame] = int(i);
  im.plane_oracle_on = true;
  return true;
}

bool Session::SetCircleOracle(const std::string& path, std::string& error) {
  Impl& im = *impl_;
  if (!rdr_scene::circles::oracle::Load(path, im.circle_file, error)) return false;
  rdr_scene::circles::oracle::SetupMesh(im.circle_guest, im.circle_file);
  uint32_t top = 0;
  for (const auto& fr : im.circle_file.frames) top = std::max(top, fr.capture_frame + 1);
  im.circle_by_capture.assign(top, -1);
  for (size_t i = 0; i < im.circle_file.frames.size(); ++i) im.circle_by_capture[im.circle_file.frames[i].capture_frame] = int(i);
  im.circle_oracle_on = true;
  return true;
}

// S101 lane 99: one --replace-fault list goes to every enabled class, and the wrestlers' and the ring's parsers refuse a name they do not know. The shadow casters' faults are written
// "shadow.NAME" (shadow.palette_from_colour, shadow.no_verify ...): that class gets only those (prefix stripped), the others get the list without them.
static std::string FaultsFor(const std::string& kind, const std::string& list) {
  std::string mine, mine_rim, mine_circ, mine_plane, mine_small, mine_caw, mine_chain, rest;   // lane 153: mine_caw   // lane 124: mine_rim; lane 136: mine_circ; lane 139: mine_plane; lane 135: mine_small
  size_t at = 0;
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string w = list.substr(at, comma - at);
    at = comma + 1;
    if (w.empty()) continue;
    const bool shadow = w.rfind("shadow.", 0) == 0, rim = w.rfind("rim.", 0) == 0, circ = w.rfind("circles.", 0) == 0, pln = w.rfind("plane.", 0) == 0, small = w.rfind("small.", 0) == 0, caw = w.rfind("caw.", 0) == 0, chain = w.rfind("rimchain.", 0) == 0;   // per-hook prefixes: rim (124), circles (136), plane (139), small (135)
    std::string& to = chain ? mine_chain : caw ? mine_caw : small ? mine_small : shadow ? mine : rim ? mine_rim : circ ? mine_circ : pln ? mine_plane : rest;
    to += (to.empty() ? "" : ",") + (chain ? w.substr(9) : caw ? w.substr(4) : small ? w.substr(6) : shadow ? w.substr(7) : rim ? w.substr(4) : circ ? w.substr(8) : pln ? w.substr(6) : w);
  }
  return kind == "caw" ? mine_caw : kind == "shadow_casters" ? mine : kind == "rim_map" ? mine_rim : kind == "circles" ? mine_circ : kind == "plane_shadow" ? mine_plane : kind == "small_passes" ? mine_small : kind == "rim_chain" ? mine_chain : rest;
}

void Session::SetTestFsBlockCapacity(uint32_t bytes) {   // lane 151
  if (impl_->binder) impl_->binder->SetTestFsBlockCapacity(bytes);
}

void Session::SetTestSlotOverride(uint32_t from, uint32_t to) {   // lane 151
  if (impl_->binder) impl_->binder->SetTestSlotOverride(from, to);
}

bool Session::EnableReplace(const std::string& kind, const std::string& fault, const std::string& game_root, std::string& error) {
  Impl& im = *impl_;
  if (kind == "test") {
    rdr_adapter::ReplaceTestOptions o;
    if (!rdr_adapter::ReplaceTestOptions::Parse(fault, o, error)) return false;
    im.replace_test = std::make_unique<rdr_adapter::ReplaceTestHook>(*renderer_, o);
    im.binder->AddReplaceHook(im.replace_test.get(), &rdr_adapter::g_replace_test);
    rdr_adapter::g_replace_test = true;
    return true;
  }
  if (!rdr_scene::FindReplaceClass(kind)) {  // S101: every class in replace_factories.h kReplaceClasses (lane 70 found the hard-coded list)
    error = "unknown replace kind '" + kind + "'";
    return false;
  }
  if (!capture_ || (kind != "ui" && kind != "ui_text" && kind != "caw" /* lane 153: no scene record */ && kind != "shadow_casters" && kind != "movie" && kind != "rim_map" && kind != "circles" && kind != "plane_shadow" && kind != "small_passes" && kind != "rim_chain" /* lane 151: no record, no game file */ /* lane 135: no scene record (its meshes are rules, not joins) */ /* lane 139: oracle file or nothing */ /* lane 136: needs no record of the capture: the oracle file or nothing */ /* lane 124: like shadow_casters, runs on a capture without records (Bryan): nothing joins, nothing is replaced */ && capture_->scene_record_count() == 0)) {   // S101 lane 89: the text hook needs none either  // S101 lane 66: the UI needs no scene record (it joins by picture content)
    error = "the capture has no scene records (scene\\frame_NNNNNN.rsc): taken without --wwe2k14_scene_records";
    return false;
  }
  rdr_scene::InstallReplaceFactories();
  const rdr_scene::ReplaceClass* cls = rdr_scene::FindReplaceClass(kind);  // S101 ring contract: the class table
  const rdr_scene::ReplaceFactory factory = cls ? *cls->factory : nullptr;
  if (!factory) {
    error = "no " + kind + " class is registered in this build (replace_factories.h InstallReplaceFactories: the class adds its factory)";
    return false;
  }
  rdr_scene::ReplaceFactoryArgs args;
  args.renderer = renderer_.get();
  args.game_root = game_root;
  args.fault = FaultsFor(kind, fault);  // S101 lane 99: "shadow.NAME" items go to the shadow casters only
  args.image_content = im.content_table.Supplier();  // S101 lane 66: any class may ask for the content table (a hook that does sets wants_content: FeedUi then fills it)
  if (kind == "ui") im.stale_after_join = fault == "stale_after_join" || fault == "stale_stream";   // the UI's session plant
  if (im.image_info_mode == "capture") {  // S101 lane 40: the capture's image records (width / height behind a frame texture slot's image id) for the class's size checks
    const rdr_adapter::CaptureReader* cap = capture_.get();
    const uint32_t* resized = &im.thin_resize_id;  // lane 122 (TEST ONLY, --thin-swap-slot): from that frame the stream's picture A "has another size" (the hook's size check then refuses it); ids and facts stay
    args.image_info = [cap, resized](uint32_t id, uint32_t& w, uint32_t& h) {
      const rdr_adapter::ImageRecord* r = cap->FindImage(id);
      if (!r) return false;
      w = r->width + (*resized == id ? 1 : 0), h = r->height;
      return true;
    };
  } else if (im.image_info_mode == "live" || im.image_info_mode == "live-fault") {  // S101 lane 47: the live supplier, fed the way the render thread feeds it
    std::vector<rdr_adapter::ImageRecord> records = capture_->images();
    if (im.image_info_mode == "live-fault")
      for (rdr_adapter::ImageRecord& r : records) ++r.width;
    im.image_table.Set(records.data(), records.size());
    args.image_info = im.image_table.Supplier();
  }  // "none": args.image_info stays empty
  rdr_scene::ReplaceHookHandle handle = factory(args);
  if (handle.wants_content) im.ui_on = true;   // S101 lane 66: feed the content table next to every ApplyContents
  if (!handle.hook) {
    error = kind + ": " + (handle.error.empty() ? std::string("the class refused to start") : handle.error);
    return false;
  }
  im.replace_handles.push_back(std::move(handle));
  im.replace_kinds.push_back(kind);
  im.binder->AddReplaceHook(im.replace_handles.back().hook.get(), cls->enabled);  // (added in call order: main.cpp enables arena, wrestlers, ring in that order)
  *cls->enabled = true;
  return true;
}

bool Session::SetImageInfoMode(const std::string& mode, std::string& error) {
  if (mode != "capture" && mode != "live" && mode != "live-fault" && mode != "none") {
    error = "unknown --image-info '" + mode + "' (capture, live, live-fault, none)";
    return false;
  }
  impl_->image_info_mode = mode;
  return true;
}

bool Session::EnableParticles(const std::string& fault, std::string& error) {  // S101 lane 67
  rdr_adapter::particles::Fault f = rdr_adapter::particles::Fault::None;
  if (!fault.empty() && !rdr_adapter::particles::ParseFault(fault, f)) {
    error = "unknown --particles-fault '" + fault + "'";
    return false;
  }
  if (!impl_->binder) {
    error = "no binder yet";
    return false;
  }
  impl_->binder->SetParticleFault(f);
  rdr_adapter::g_particles_ours = true;
  return true;
}

std::string Session::ParticleReport() const { return impl_->binder ? impl_->binder->ParticleCensus() : std::string(); }

// ---- lane 122: D1 thin draws -----------------------------------------------------------------------------------------------------------------------------------------------
bool Session::EnableThin(const std::string& cls, const std::string& fault, int lag, int proof_k, int churn, std::string& error) {
  Impl& im = *impl_;
  std::vector<std::string> classes;   // lane 129 (KC): `arena`, `ring`, `wrestlers`, or a comma list of them
  for (size_t p = 0; p <= cls.size();) {
    size_t q = cls.find(',', p);
    if (q == std::string::npos) q = cls.size();
    classes.push_back(cls.substr(p, q - p));
    p = q + 1;
  }
  for (const std::string& c : classes)
    if (c != "arena" && c != "ring" && c != "wrestlers") {
      error = "--thin " + cls + ": the classes are arena, ring, wrestlers (comma separated)";
      return false;
    }
  rdr_adapter::thin::Fault f = rdr_adapter::thin::Fault::None;
  if (!rdr_adapter::thin::ParseFault(fault, f)) {
    error = "unknown --thin-fault '" + fault + "'";
    return false;
  }
  if (!im.binder) {
    error = "no binder yet";
    return false;
  }
  im.thin_pub.Reset();
  im.binder->SetThinPublisher(&im.thin_pub);
  im.binder->SetThinFault(f);
  im.binder->SetThinProofK(f == rdr_adapter::thin::Fault::NoProof ? 0u : proof_k < 0 ? rdr_adapter::thin::kProofK : uint32_t(proof_k));
  im.binder->SetThinChurn(uint32_t(churn));
  im.thin_lag = lag < 0 ? 2 : lag;
  im.thin_on = true;
  for (const std::string& c : classes) (c == "arena" ? rdr_adapter::thin::g_arena_thin : c == "ring" ? rdr_adapter::thin::g_ring_thin : rdr_adapter::thin::g_wrestlers_thin) = true;  // lane 129 (KC)
  return true;
}

bool Session::EnableD3(const std::string& cls_in, const std::string& mode, const std::string& fault, int lag, const std::string& ledger_in, const std::string& ledger_out, bool no_proof, std::string& error, bool verify, int record_offset) {   // lane 138
  Impl& im = *impl_;
  // lane 229 (Ring D3 R3): `ring` may stand alone (--d3 ring) or beside the arena (--d3 arena,ring); the ring chain is its own (binder_ring_d3.inc). The old single-class values are unchanged.
  std::string cls = cls_in;
  bool ring = false;
  if (const size_t rp = cls.find("ring"); rp != std::string::npos) {
    ring = true;
    cls.erase(rp, 4);
    while (!cls.empty() && cls.front() == ',') cls.erase(0, 1);
    while (!cls.empty() && cls.back() == ',') cls.pop_back();
  }
  if (ring && !im.binder) {
    error = "--d3 ring needs the binder";
    return false;
  }
  if (ring && !im.thin_on) {
    error = "--d3 ring needs --thin ring (the ring's sequences are learned from the thin draws' templates)";
    return false;
  }
  if (ring && !cls.empty() && cls != "arena") {   // N4: the valid sets are arena, casters, ring and arena,ring (the caster chain has no thin form; it does not run beside the ring)
    error = "--d3 " + cls_in + ": the valid sets are arena, casters, ring and arena,ring";
    return false;
  }
  if (cls.empty() && ring) {   // the ring chain alone
    rdr_adapter::d3::Mode rm = rdr_adapter::d3::Mode::Stream;
    if (mode == "sourced") rm = rdr_adapter::d3::Mode::Sourced;
    else if (mode == "record") rm = rdr_adapter::d3::Mode::Record;
    else if (!mode.empty() && mode != "stream") {
      error = "--d3-mode " + mode + ": stream, sourced or record";
      return false;
    }
    rdr_adapter::d3::Fault rf = rdr_adapter::d3::Fault::None;
    if (!rdr_adapter::d3::ParseFault(fault, rf)) {
      error = "unknown --d3-fault '" + fault + "'";
      return false;
    }
    im.d3_ring = true;
    im.binder->SetD3RingOffline(rm, rf, rm == rdr_adapter::d3::Mode::Sourced || no_proof, lag);
    im.binder->D3RSetRecordRoute(rm == rdr_adapter::d3::Mode::Record, verify, record_offset);
    im.d3_records_on = rm == rdr_adapter::d3::Mode::Record || verify;
    im.d3_records_late = rf == rdr_adapter::d3::Fault::RecordLate;
    im.d3_records_preload = std::getenv("D3_PRELOAD_RECORDS") != nullptr;
    if (im.d3_records_preload)
      for (uint32_t i = 0; capture_ && i < capture_->frame_count(); ++i) DeliverD3Record(i);
    if (!ledger_in.empty() && !im.binder->D3RLoadLedger(ledger_in)) {
      error = "--d3-ledger-in: cannot read " + ledger_in;
      return false;
    }
    im.d3_ledger_out = ledger_out;
    return true;
  }
  if (cls != "arena" && cls != "casters" && cls != "wrestlers") {
    error = "--d3 " + cls_in + ": the classes are arena, casters (lane 217), ring (lane 229; alone or arena,ring) and wrestlers (lane 227)";
    return false;
  }
  const bool casters = cls == "casters";
  const bool wrestlers = cls == "wrestlers";
  if (wrestlers && (!im.binder || !im.thin_on)) {
    error = "--d3 wrestlers needs --wrestlers-ours --thin wrestlers (the sequences are learned from the wrestlers' thin templates)";
    return false;
  }
  if (casters && !im.binder) {
    error = "--d3 casters needs the binder";
    return false;
  }
  if (!casters && !wrestlers && (!im.binder || !im.thin_on)) {
    error = "--d3 needs --thin arena (D3's sequences are learned from the thin draws' templates)";
    return false;
  }
  rdr_adapter::d3::Mode m = rdr_adapter::d3::Mode::Stream;
  if (mode == "sourced") m = rdr_adapter::d3::Mode::Sourced;
  else if (mode == "record") m = rdr_adapter::d3::Mode::Record;   // lane 152: the record route
  else if (!mode.empty() && mode != "stream") {
    error = "--d3-mode " + mode + ": stream (the packet carries the stream's registers), sourced (the record groups withheld: the readiness measurement) or record (the groups from the scene records)";
    return false;
  }
  rdr_adapter::d3::Fault f = rdr_adapter::d3::Fault::None;
  if (!rdr_adapter::d3::ParseFault(fault, f)) {
    error = "unknown --d3-fault '" + fault + "'";
    return false;
  }
  im.d3_casters = casters;
  if (ring) {   // lane 229: arena,ring together: the ring chain beside the arena's, same mode / fault / route
    im.d3_ring = true, im.d3_arena_beside_ring = true;
    im.binder->SetD3RingOffline(m, f, m == rdr_adapter::d3::Mode::Sourced || no_proof, lag);
    im.binder->D3RSetRecordRoute(m == rdr_adapter::d3::Mode::Record, verify, record_offset);
    if (!ledger_in.empty()) (void)im.binder->D3RLoadLedger(ledger_in + ".ring");
  }
  im.d3_wrestlers = wrestlers;
  if (wrestlers) {   // lane 227 (H2): the wrestler class has its own core, packets, marks and gate; the thin store (templates) is the D1 wrestler arm's
    im.binder->SetD3WrestlersOffline(m, f, m == rdr_adapter::d3::Mode::Sourced || no_proof, lag);
    im.binder->D3WSetRecordRoute(m == rdr_adapter::d3::Mode::Record, verify, record_offset);
  } else if (casters) {   // lane 217 (E2b): the caster class has no D1 form: no --thin, its own core, packets, marks and gate
    im.binder->SetD3CasterOffline(m, f, m == rdr_adapter::d3::Mode::Sourced || no_proof, lag);
    im.binder->D3CSetRecordRoute(m == rdr_adapter::d3::Mode::Record, verify, record_offset);
  } else {
    im.binder->SetD3Offline(m, f, m == rdr_adapter::d3::Mode::Sourced || no_proof, lag);
    im.binder->D3SetRecordRoute(m == rdr_adapter::d3::Mode::Record, verify, record_offset);   // lane 152
  }
  im.d3_records_on = m == rdr_adapter::d3::Mode::Record || verify;   // lane 152 (B1): records are delivered in LIVE ORDER as frames bind (DeliverD3Record), not preloaded
  im.d3_records_late = f == rdr_adapter::d3::Fault::RecordLate;
  im.d3_records_preload = std::getenv("D3_PRELOAD_RECORDS") != nullptr;   // diagnosis only: the old offline behaviour
  if (im.d3_records_preload)
    for (uint32_t i = 0; capture_ && i < capture_->frame_count(); ++i) DeliverD3Record(i);
  if (!ledger_in.empty() && !(wrestlers ? im.binder->D3WLoadLedger(ledger_in) : casters ? im.binder->D3CLoadLedger(ledger_in) : im.binder->D3LoadLedger(ledger_in))) {
    error = "--d3-ledger-in: cannot read " + ledger_in;
    return false;
  }
  im.d3_ledger_out = ledger_out;
  return true;
}

// lane 221 (SKIP_GAME_WORK_MAP.md 17.10): fview's stand-in for the live CasterExtra reader on a capture whose records predate it (run 7147's). ONLY with the environment variable D3_EMULATE_EXTRAS: the values are the
// STREAM's own (the rim draw's c56-59 un-transposed into the product, its c162, a map-A draw's collapsed row), so the record route is then tested for its JOIN and COMPOSITION (stamp pairing, transposition, the
// collapsed-slot set, the plants), NOT for the live reader's values: those are proved by the tracker unit and, live, by the next verify run's census (`product_eq_r`, tools\caster\caster_extras_check.py).
static void EmulateCasterExtras(rdr_adapter::CaptureReader& capture, rdr_scene::SceneTables& t) {
  if (t.caster_calls.empty() || !t.caster_extras.empty()) return;
  rdr_adapter::LoadedFrame frame;
  if (!capture.LoadFrame(0, frame)) return;
  const int64_t base = int64_t(frame.header.guest_frame);   // per capture (a second session in the process must not keep the first one's base)
  t.caster_extras.assign(t.caster_calls.size(), rdr_scene::CasterExtra{});
  int64_t loaded = -1;
  for (size_t ci = 0; ci < t.caster_calls.size(); ++ci) {
    const rdr_scene::CasterCall& c = t.caster_calls[ci];
    const int64_t idx = int64_t(c.frame) + 1 - base;   // hook stamp g-1 <-> stream frame g
    if (idx < 0 || idx >= int64_t(capture.frame_count())) continue;
    if (idx != loaded) {
      if (!capture.LoadFrame(uint32_t(idx), frame)) continue;
      loaded = idx;
    }
    rdr_scene::CasterExtra& x = t.caster_extras[ci];
    const auto bank_of = [&](uint32_t d) -> const uint32_t* { return d < frame.draws.count && frame.draws[d].vs_bank < frame.constant_banks.count ? frame.constant_banks[frame.draws[d].vs_bank].dwords : nullptr; };
    const auto flt = [](uint32_t w) { float f; std::memcpy(&f, &w, 4); return f; };
    if (c.pass == rdr_scene::kCpRim) {
      for (uint32_t d = 0; d < frame.draws.count; ++d) {
        const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
        const uint32_t* b = bank_of(d);
        if (!o || !b || o->pass_site != 0x82BFBD58u) continue;
        for (int i = 0; i < 4; ++i)
          for (int k = 0; k < 4; ++k) x.product[k * 4 + i] = flt(b[(56 + i) * 4 + k]);
        x.depth[0] = flt(b[162 * 4]), x.depth[1] = flt(b[162 * 4 + 1]);
        x.flags |= rdr_scene::kCxProduct | rdr_scene::kCxDepth;
        if (o->object == c.object) break;
      }
    } else if (c.pass == rdr_scene::kCpMap && c.collapse) {
      for (uint32_t d = 0; d < frame.draws.count && !(x.flags & rdr_scene::kCxPose); ++d) {
        const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
        const uint32_t* b = bank_of(d);
        if (!o || !b || o->pass_site != 0x82C04634u || o->object != c.object) continue;
        for (uint32_t slot = 0; slot < 21 && !(x.flags & rdr_scene::kCxPose); ++slot) {
          bool zero = true;
          for (int j = 0; j < 3 && zero; ++j)
            for (int k = 0; k < 3; ++k)
              if ((b[(76 + 3 * slot + j) * 4 + k] | 0x80000000u) != 0x80000000u) zero = false;
          if (!zero) continue;
          for (int j = 0; j < 3; ++j) x.pose_t[j] = flt(b[(76 + 3 * slot + j) * 4 + 3]);
          x.pose_t[3] = 1.f;
          x.flags |= rdr_scene::kCxPose;
        }
      }
    }
  }
}

// lane 226 (SKIP_GAME_WORK_MAP.md 17.11, E5): fview's stand-in for the plane tables and the plane entries' palette matrices on a capture whose records predate them (runs 7147 / 7149 recorded neither: the verify arm ran
// without the plane switch and the plane pass never read [model+200]). ONLY with D3_EMULATE_EXTRAS (or D3_EMULATE_PLANE): the values are the STREAM's own (the first plane draw's c0-c16 become the stamp's PlaneFrame, the call's first draw's
// c7 the CasterExtra entry snapshot (kCxPlanePos), the distinct 3-row triples of a call's palette slots its node matrices), so the record route is tested for its JOIN (stamp, object, part, nth = the call's ordinal), the slot -> node learning and the
// plants, NOT for the live values (the live reader is proved by hook.plane_mats and, live, by the next verify run's census; nothing here reads game memory).
static void EmulatePlane(rdr_adapter::CaptureReader& capture, rdr_scene::SceneTables& t) {
  bool any = false;
  for (const rdr_scene::CasterCall& c : t.caster_calls) any = any || c.pass == rdr_scene::kCpPlane;
  if (!any || !t.plane_frames.empty() || !t.caster_mats.empty()) return;
  rdr_adapter::LoadedFrame frame;
  if (!capture.LoadFrame(0, frame)) return;
  const int64_t base = int64_t(frame.header.guest_frame);
  int64_t loaded = -1;
  uint32_t last_stamp = 0xFFFFFFFFu;
  const auto flt = [](uint32_t w) { float f; std::memcpy(&f, &w, 4); return f; };
  for (size_t ci = 0; ci < t.caster_calls.size(); ++ci) {
    rdr_scene::CasterCall& c = t.caster_calls[ci];
    if (c.pass != rdr_scene::kCpPlane) continue;
    const int64_t idx = int64_t(c.frame) + 1 - base;
    if (idx < 0 || idx >= int64_t(capture.frame_count())) continue;
    if (idx != loaded) {
      if (!capture.LoadFrame(uint32_t(idx), frame)) continue;
      loaded = idx;
    }
    const auto bank_of = [&](uint32_t d) -> const uint32_t* { return d < frame.draws.count && frame.draws[d].vs_bank < frame.constant_banks.count ? frame.constant_banks[frame.draws[d].vs_bank].dwords : nullptr; };
    // the runs of this object at the plane site, by the binder's rule (consecutive draws of one object, increasing part index): the call's ordinal-th run
    std::vector<uint32_t> run;
    uint32_t seen = 0, last_obj = 0;
    int64_t last_part = -1;
    bool in_run = false;
    uint32_t first_plane = 0xFFFFFFFFu;
    for (uint32_t d = 0; d < frame.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
      if (!o || o->pass_site != 0x82BF4D38u || !o->object) {
        in_run = false;
        continue;
      }
      if (first_plane == 0xFFFFFFFFu) first_plane = d;
      if (!(in_run && o->object == last_obj && int64_t(o->part) > last_part)) {
        in_run = true;
        if (o->object == c.object) ++seen;
      }
      last_obj = o->object, last_part = o->part;
      if (o->object == c.object && seen == c.ordinal + 1) run.push_back(d);
    }
    if (run.empty()) continue;
    if (c.frame != last_stamp) {   // one PlaneFrame per stamp, from the first plane draw's constants
      last_stamp = c.frame;
      const uint32_t* b = bank_of(first_plane);
      if (b) {
        rdr_scene::PlaneFrame pf;
        pf.frame = c.frame, pf.have = rdr_scene::kPlaneHaveAll;
        for (uint32_t r = 0; r < 7; ++r) pf.fade[r] = flt(b[r * 4]);
        for (uint32_t k = 1; k < 4; ++k) pf.fade[6 + k] = flt(b[6 * 4 + k]);
        for (uint32_t k = 0; k < 4; ++k) pf.plane_normal[k] = flt(b[8 * 4 + k]);
        for (uint32_t k = 0; k < 16; ++k) pf.fit_matrix[k] = flt(b[9 * 4 + k]), pf.camera[k] = flt(b[13 * 4 + k]);
        t.plane_frames.push_back(pf);
      }
    }
    std::vector<rdr_scene::Mat4> nodes;
    std::map<std::array<uint32_t, 12>, size_t> seen_rows;
    for (uint32_t d : run) {
      const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
      const uint32_t* b = bank_of(d);
      if (!b) continue;
      if (ci < t.caster_extras.size() && !(t.caster_extras[ci].flags & rdr_scene::kCxPlanePos)) {   // lane 226 follow-up (S1): c7 is the entry snapshot in the call's extra (no PlaneDraw / nth join)
        for (uint32_t k = 0; k < 4; ++k) t.caster_extras[ci].pose_t[k] = flt(b[7 * 4 + k]);
        t.caster_extras[ci].flags |= rdr_scene::kCxPlanePos;
      }
      for (uint32_t s = 0; s < 21; ++s) {
        std::array<uint32_t, 12> key;
        for (uint32_t j = 0; j < 3; ++j)
          for (uint32_t k = 0; k < 4; ++k) key[j * 4 + k] = b[(76 + 3 * s + j) * 4 + k];
        if (!seen_rows.emplace(key, nodes.size()).second) continue;
        rdr_scene::Mat4 m{};
        for (uint32_t j = 0; j < 3; ++j)
          for (uint32_t k = 0; k < 4; ++k) std::memcpy(&m.m[k * 4 + j], &key[j * 4 + k], 4);
        m.m[15] = 1.f;
        nodes.push_back(m);
      }
    }
    if (nodes.empty() || nodes.size() > 0xFFFF) continue;
    c.flags |= rdr_scene::kCcMats, c.flags &= ~rdr_scene::kCcNodeHashes, c.node_first = uint32_t(t.caster_mats.size()), c.n_nodes = uint16_t(nodes.size());
    t.caster_mats.insert(t.caster_mats.end(), nodes.begin(), nodes.end());
  }
}

// lane 152 (B1): record i is read from the capture and handed to the binder's index (fview's stand-in for the live sink: the present's own record reaches the binder with the frame it was written at).
void Session::DeliverD3Record(uint32_t i) {
  Impl& im = *impl_;
  if (!capture_ || i >= capture_->frame_count() || !capture_->HasSceneRecord(i)) return;
  auto tables = std::make_shared<rdr_scene::SceneTables>();
  std::string err;
  if (!rdr_scene::ReadRecord(capture_->SceneRecordPath(i).string(), *tables, err)) {
    std::fprintf(stderr, "[fview] --d3-mode record: scene record %u: %s\n", i, err.c_str());
    return;
  }
  if (std::getenv("D3_EMULATE_EXTRAS")) EmulateCasterExtras(*capture_, *tables);   // lane 221
  if (std::getenv("D3_EMULATE_EXTRAS") || std::getenv("D3_EMULATE_PLANE")) EmulatePlane(*capture_, *tables);   // lane 226
  im.binder->D3AddRecord(tables);
  if (!im.d3_records_preload) im.binder->D3EvictRecords(i);   // (review S8) the live index keeps only the newest records
}

void Session::SetD3RingHookFromRecord(bool on) { if (impl_->binder && impl_->d3_ring) impl_->binder->D3RSetHookFromRecord(on); }   // lane 258

std::string Session::D3Report() {   // lane 138
  if (!impl_->binder) return {};
  if (impl_->d3_wrestlers) {   // lane 227
    if (!impl_->d3_ledger_out.empty()) impl_->binder->D3WSaveLedger(impl_->d3_ledger_out);
    return impl_->binder->D3WReportText();
  }
  if (impl_->d3_ring) {   // lane 229: the ring chain's block (+ the arena's when it runs beside it; the ring's ledger is <out>.ring then)
    if (!impl_->d3_ledger_out.empty()) impl_->binder->D3RSaveLedger(impl_->d3_ledger_out + (impl_->d3_arena_beside_ring ? ".ring" : ""));
    if (!impl_->d3_arena_beside_ring) return impl_->binder->D3RReport();
    if (!impl_->d3_ledger_out.empty()) impl_->binder->D3SaveLedger(impl_->d3_ledger_out);
    return impl_->binder->D3Census() + "\n" + impl_->binder->D3Readiness() + "\n" + impl_->binder->D3RecordReport() + "\n" + impl_->binder->D3RReport();
  }
  if (impl_->d3_casters) {   // lane 217
    if (!impl_->d3_ledger_out.empty()) impl_->binder->D3CSaveLedger(impl_->d3_ledger_out);
    return impl_->binder->D3CReport();
  }
  if (!impl_->d3_ledger_out.empty()) impl_->binder->D3SaveLedger(impl_->d3_ledger_out);
  return impl_->binder->D3Census() + "\n" + impl_->binder->D3Readiness() + "\n" + impl_->binder->D3RecordReport();   // lane 152: + the record route line and the reasons
}

std::string Session::ThinReport(const std::string& cls) const {  // lane 129 (KC): one line per class named
  if (!impl_->binder) return {};
  std::string text;
  for (size_t p = 0; p <= cls.size();) {
    size_t q = cls.find(',', p);
    if (q == std::string::npos) q = cls.size();
    const std::string c = cls.substr(p, q - p);
    p = q + 1;
    if (!text.empty()) text += "\n";
    text += impl_->binder->ThinCensus(c == "ring" ? rdr_adapter::thin::Cls::Ring : c == "wrestlers" ? rdr_adapter::thin::Cls::Wrestlers : rdr_adapter::thin::Cls::Arena);
  }
  return text;
}

namespace {
uint32_t SurfaceOf(const rdr_adapter::LoadedFrame& frame) {  // DefaultSurface() for a frame that is not the selected one
  uint32_t surface = 0;
  for (const rdr_adapter::Op& op : frame.ops)
    if (op.kind == rdr_adapter::OpKind::Present) surface = frame.presents[op.index].source.image;
  if (surface) return surface;
  for (const rdr_adapter::Op& op : frame.ops)
    if (op.kind == rdr_adapter::OpKind::SetTargets && frame.targets[op.index].color_count) surface = frame.targets[op.index].color[0].image;
  return surface;
}
}  // namespace

bool Session::ThinRun(uint32_t count, const std::string& out_dir, int swap_frame, std::string& error, int epoch_at, const std::string& isolate, int isolate_from, int vswap_frame, int release_frame) {
  Impl& im = *impl_;
  std::string trace_csv = "frame,op,fate,keep_why,thin,swapped\n";   // lane 129 review F7: the model-part draws' fate and the reason the hooks kept them, per frame and op (the census bar joins two arms' files)
  count = std::min(count, frame_count());
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  std::string csv = "frame,hash,draws,bound\n";
  std::string lite_csv = "frame,captured_lite,draws\n";   // lane 162
  uint64_t lite_total = 0;
  uint32_t lite_frames = 0;
  rdr_adapter::LoadedFrame frame;
  rdr::FrameBuilder builder;
  uint32_t swap_from = 0, swap_to = 0;  // the planted stream change: picture A -> B in slot 0 of the arena's draws
  std::unordered_set<uint32_t> swapped_draws;   // lane 129 review F4: this frame's draws whose vertex buffer was swapped
  std::unordered_set<uint32_t> arena_objects;  // the objects the arena hook replaced so far (swap only; both arms track them the same way)
  if (swap_frame >= 0) im.binder->TrackArenaObjects(&arena_objects);
  rdr_adapter::lasting::CopyStreaks fv_streaks;   // lane 307 (TEST ONLY): D3_SKIP_LASTING=prune = live.cpp's pruned lasting bind for a skipped frame
  rdr_adapter::lasting::Pruner fv_pruner;
  std::vector<uint8_t> fv_seed;
  std::vector<rdr_adapter::Op> fv_kept, fv_dropped;
  uint32_t fv_prune_frames = 0, fv_prune_blind = 0;
  for (uint32_t f = 0; f < count; ++f) {
    if (!capture_->LoadFrame(f, frame)) {
      error = "could not read frame " + std::to_string(f);
      return false;
    }
    if (swap_frame >= 0 && int(f) >= swap_frame) {
      const auto arena_draw = [&](uint32_t i) {
        const rdr_adapter::DrawOrigin* o = frame.OriginOf(i);
        return o && o->klass == rdr_adapter::DrawClass::ModelPart && o->object && (o->pass_site == 0x82C0A950u || o->pass_site == 0x82C0B94Cu) && arena_objects.count(o->object);
      };
      const auto slot0 = [&](uint32_t i) -> rdr_adapter::TextureSlot* {
        const rdr_adapter::Draw& d = frame.draws[i];
        for (uint32_t t = 0; t < d.texture_count; ++t)
          if (frame.texture_slots[d.texture_first + t].slot == 0 && frame.texture_slots[d.texture_first + t].image) return const_cast<rdr_adapter::TextureSlot*>(&frame.texture_slots[d.texture_first + t]);
        return nullptr;
      };
      if (!swap_from) {  // at the swap frame: A = the first arena draw's picture, B = the first other picture of a different size
        for (uint32_t i = 0; i < frame.draws.count && !swap_from; ++i) {
          if (!arena_draw(i) || !slot0(i)) continue;
          const rdr_adapter::ImageRecord* a = capture_->FindImage(slot0(i)->image);
          if (!a) continue;
          for (uint32_t j = i + 1; j < frame.draws.count; ++j) {
            if (!arena_draw(j) || !slot0(j)) continue;
            const rdr_adapter::ImageRecord* b = capture_->FindImage(slot0(j)->image);
            if (b && b->id != a->id && (b->width != a->width || b->height != a->height)) {
              swap_from = a->id, swap_to = b->id;
              break;
            }
          }
        }
        if (!swap_from) {
          error = "--thin-swap-slot: no arena draws with two pictures of different sizes in frame " + std::to_string(f);
          return false;
        }
      }
      for (uint32_t i = 0; i < frame.draws.count; ++i)
        (void)swap_to;  // (review 6: swapping the image id now makes a new key at once; the stream changes under the same id instead: its size)
      im.thin_resize_id = swap_from;
    }
    if ((frame.header.flags & rdr_adapter::kFrameHasThin) && !im.thin_warned) {
      im.thin_warned = true;
      std::fprintf(stderr, "[fview] WARNING: frame %u holds thin draws (kFrameHasThin): the arena's geometry was never built for them. They are drawn from templates learned from the capture's own full frames (--thin arena); a draw met before its key is learned is not drawn (counted \"not learned\"), and this capture cannot show the stream's arena.\n", f);
    }
    if (vswap_frame >= 0 && int(f) >= vswap_frame) {   // lane 129 review F4 (TEST ONLY, both arms): the stream's vertex buffer of some model parts is swapped for another of the same size under the same index buffer and images
      std::map<uint32_t, std::pair<uint64_t, uint64_t>> alt_for_size;   // vertex_bytes -> (the smallest, the largest) vertex_offset among the frame's colour-site draws of that size
      for (uint32_t i = 0; i < frame.draws.count; ++i) {
        const rdr_adapter::DrawOrigin* o = frame.OriginOf(i);
        if (!o || o->klass != rdr_adapter::DrawClass::ModelPart || !o->object || o->pass_site != 0x82C0A950u || (frame.draws[i].flags & rdr_adapter::kDrawInline)) continue;
        const rdr_adapter::Draw& d = frame.draws[i];
        const auto it = alt_for_size.find(d.vertex_bytes);
        if (it == alt_for_size.end()) alt_for_size[d.vertex_bytes] = {d.vertex_offset, d.vertex_offset};
        else it->second = {std::min(it->second.first, d.vertex_offset), std::max(it->second.second, d.vertex_offset)};
      }
      uint32_t swapped = 0;
      swapped_draws.clear();
      for (uint32_t i = 0; i < frame.draws.count; ++i) {
        const rdr_adapter::DrawOrigin* o = frame.OriginOf(i);
        if (!o || o->klass != rdr_adapter::DrawClass::ModelPart || !o->object || o->pass_site != 0x82C0A950u || (frame.draws[i].flags & rdr_adapter::kDrawInline) || (o->object ^ o->part) % 7) continue;
        rdr_adapter::Draw& d = const_cast<rdr_adapter::Draw&>(frame.draws[i]);
        const auto it = alt_for_size.find(d.vertex_bytes);
        if (it != alt_for_size.end() && it->second.first != it->second.second) d.vertex_offset = d.vertex_offset != it->second.first ? it->second.first : it->second.second, ++swapped, swapped_draws.insert(i);
      }
      if (int(f) == vswap_frame) std::printf("[fview] thin vertex swap from frame %d: %u draws of the first frame point at another vertex buffer of the same size\n", vswap_frame, swapped);
    }
    im.binder->ApplyContents(frame, *capture_);
    FeedUi(im, *capture_, *renderer_, frame);
    builder.Begin(f);
    std::vector<rdr_adapter::BindTrace> bind_trace;
    rdr_adapter::BindStats stats;
    stats.trace = &bind_trace;
    FeedCrowd(f);
    // lane 162 (diagnosis, TEST ONLY): the draws the LIVE reader already made lite (flag kDrawThin in the capture itself: J1 was recorded with arena thin on). Counted per frame
    // (lite.csv); with L162_DROP_LITE=1 they are dropped in EVERY arm (kDrawNoGeometry, thin flag cleared) so the arms are compared on the draws every arm can draw.
    std::vector<uint32_t> captured_lite;
    for (uint32_t i = 0; i < frame.draws.count; ++i)
      if (frame.draws[i].flags & rdr_adapter::kDrawThin) captured_lite.push_back(i);
    lite_total += captured_lite.size(), lite_frames += captured_lite.empty() ? 0 : 1;
    lite_csv += std::to_string(f) + "," + std::to_string(captured_lite.size()) + "," + std::to_string(frame.draws.count) + "\n";
    if (im.thin_on) im.binder->ThinEmulateReader(frame, im.thin_lag);
    if (std::getenv("L162_DROP_LITE")) {
      for (uint32_t i : captured_lite) {
        rdr_adapter::Draw& d = const_cast<rdr_adapter::Draw&>(frame.draws[i]);
        d.flags = uint8_t((d.flags & ~rdr_adapter::kDrawThin) | rdr_adapter::kDrawNoGeometry);
      }
    }
    if (im.d3_records_on && !im.d3_records_preload && (!im.d3_records_late || f >= 1)) DeliverD3Record(im.d3_records_late ? f - 1 : f);   // lane 152 (B1): the live order (late = the plant)
    im.binder->D3EmulateReader(frame);   // lane 138
    im.binder->D3CEmulateReader(frame);  // lane 217 (E2b): no-op unless --d3 casters
    im.binder->D3REmulateReader(frame);  // lane 229 (Ring D3 R3): no-op unless --d3 ring (after the arena's: the chains' markers are told apart by their call sites)
    im.binder->D3WEmulateReader(frame);  // lane 227 (H2): no-op unless --d3 wrestlers
    if (const char* sk = std::getenv("D3_SKIP"); sk && im.thin_on) {   // lane 152 (TEST ONLY): the live sink skips frames it does not bind (the 30 fps shape `every2`, backed-up `burst`); the hook already ran
      const bool skip = std::strcmp(sk, "every2") == 0 ? (f % 2) == 1 : (f % 23 >= 5 && f % 23 < 9) || (f % 47 >= 30 && f % 47 < 32);
      // lane 204 (P): D3_SKIP_LASTING=1 = live.cpp's order for a skipped frame WITH lasting work: no D3FrameSkipped, the frame's draws (markers included) are bound, the bind's BuildMini takes the packets;
      // D3_SKIP_LASTING=old = lane 152's order (D3FrameSkipped first, then the lasting bind): every marker of that bind is a hole (the Check K1 latch). Then the frame goes on to the normal bind below.
      const char* lasting_arm = std::getenv("D3_SKIP_LASTING");
      const bool prune_arm = lasting_arm && std::strcmp(lasting_arm, "prune") == 0;
      if (prune_arm) {
        fv_streaks.images = &capture_->images();
        fv_streaks.Note(frame);
      }
      if (skip && prune_arm) {   // lane 307: classify, prune, discard the dropped draws' packets, bind persist-only, render PersistentOnly, restore
        fv_streaks.Classify(frame, false, fv_seed, [](const rdr::CopyOp&) {});
        bool any = false;
        for (uint8_t v : fv_seed) any |= v != 0;
        rdr_adapter::lasting::Stats ps;
        const rdr_adapter::Table<rdr_adapter::Op> all_ops = frame.ops;
        if (!any) {   // no lasting work: live.cpp binds nothing and takes the whole frame's packets
          im.binder->D3FrameSkipped(frame);
          (void)builder.Finish();
          csv += std::to_string(f) + ",0,0,0\n";
          continue;
        }
        fv_pruner.Select(frame, fv_seed, fv_kept, fv_dropped, ps, false);
        frame.ops = rdr_adapter::Table<rdr_adapter::Op>{fv_dropped.data(), uint32_t(fv_dropped.size())};
        im.binder->D3FrameSkipped(frame);
        frame.ops = rdr_adapter::Table<rdr_adapter::Op>{fv_kept.data(), uint32_t(fv_kept.size())};
        rdr_adapter::BindStats pstats;
        builder.Begin(f);
        im.binder->frame_part_open = true;
        im.binder->persist_only_bind = true;
        im.binder->d3_skip_observe = ps.d3_blind() && !std::getenv("D3_PRUNE_NO_OBSERVE");   // plant D3_PRUNE_NO_OBSERVE: the guard missing (the review B2 failure)
        fv_prune_blind += im.binder->d3_skip_observe ? 1 : 0;
        ++fv_prune_frames;
        im.binder->Bind(frame, *capture_, builder, pstats);
        im.binder->d3_skip_observe = false;
        im.binder->persist_only_bind = false;
        im.binder->frame_part_open = false;
        frame.ops = all_ops;
        renderer_->RenderFrame(builder.Finish(), rdr::RenderMode::PersistentOnly);
        std::printf("[fview] D3_SKIP_LASTING=prune: %u pruned skipped frames so far, %u of them with no D3 / thin begin-end (no object pass kept)\n", fv_prune_frames, fv_prune_blind);
        csv += std::to_string(f) + ",0,0,0\n";
        continue;
      }
      if (skip && lasting_arm && *lasting_arm) {
        if (std::strcmp(lasting_arm, "old") == 0) im.binder->D3FrameSkipped(frame);
      } else if (skip) {
        if (!std::getenv("D3_SKIP_PLANT")) im.binder->D3FrameSkipped(frame);   // plant D3_SKIP_PLANT: a skipped frame's packets are left to be evicted (counted unconsumed)
        (void)builder.Finish();
        csv += std::to_string(f) + ",0,0,0\n";
        continue;
      }
    }
    // (--d3: the emulated hook turns the skipped objects' runs into markers)
    if (release_frame >= 0 && int(f) == release_frame) im.binder->TestReleasePart();   // review F2 (TEST ONLY): the wrestlers' hook destroys one part's buffers
    if (epoch_at >= 0 && int(f) == epoch_at) im.binder->TestForceEpoch();  // the arena "changed": every template is void from this frame
    if (!isolate.empty() && int(f) >= isolate_from) stats.isolate = isolate.c_str();  // review 3: an isolate bind (read-only for the thin store)
    im.binder->Bind(frame, *capture_, builder, stats);
    if (bind_trace.size() == frame.ops.count) {   // review F7
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        if (frame.ops[i].kind != rdr_adapter::OpKind::Draw || (!bind_trace[i].fate && !bind_trace[i].keep_why)) continue;
        const rdr_adapter::DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
        if (!o || o->klass != rdr_adapter::DrawClass::ModelPart || (o->pass_site != 0x82C0A950u && o->pass_site != 0x82C0B94Cu)) continue;
        trace_csv += std::to_string(f) + "," + std::to_string(i) + "," + std::to_string(bind_trace[i].fate) + "," + std::to_string(bind_trace[i].keep_why) + "," + std::to_string(bind_trace[i].thin) + "," + std::to_string(swapped_draws.count(frame.ops[i].index)) + "\n";
      }
    }
    renderer_->RenderFrame(builder.Finish());
    uint64_t hash = 1469598103934665603ull;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    const uint32_t surface = SurfaceOf(frame);
    if (surface && ReadSurface(surface, rgba, w, h)) {
      for (uint8_t b : rgba) hash = (hash ^ b) * 1099511628211ull;
      if (const char* save = std::getenv("D3_SAVE"); save && ((std::string(",") + save + ",").find("," + std::to_string(f) + ",") != std::string::npos)) {   // lane 138 (diagnosis): D3_SAVE=39,40 writes frame_N.ppm into the run folder
        std::ofstream ppm(std::filesystem::path(out_dir) / ("frame_" + std::to_string(f) + ".ppm"), std::ios::binary);
        ppm << "P6\n" << w << " " << h << "\n255\n";
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) ppm.write(reinterpret_cast<const char*>(&rgba[i]), 3);
      }
    } else {
      hash = 0;
    }
    csv += std::to_string(f) + "," + std::to_string(hash) + "," + std::to_string(stats.draws) + "," + std::to_string(stats.bound) + "\n";
  }
  renderer_->WaitIdle();
  im.replayed = count;
  im.binder->TrackArenaObjects(nullptr);
  std::ofstream out(std::filesystem::path(out_dir) / "frames.csv", std::ios::binary);
  out << csv;
  if (!out) {
    error = "cannot write " + out_dir + "\\frames.csv";
    return false;
  }
  std::ofstream tout(std::filesystem::path(out_dir) / "trace.csv", std::ios::binary);   // lane 129 review F7
  tout << trace_csv;
  std::printf("[fview] lane 162: draws the capture already carries lite (kDrawThin, made by the live reader): %llu in %u of %u frames%s\n", static_cast<unsigned long long>(lite_total), lite_frames, count, std::getenv("L162_DROP_LITE") ? " (DROPPED in this arm)" : "");
  std::ofstream lout(std::filesystem::path(out_dir) / "lite.csv", std::ios::binary);   // lane 162
  lout << lite_csv;
  return true;
}

bool Session::EnablePostOurs(const std::string& fault, const std::string& limits, float render_scale, std::string& error) {  // S101 lane PO
  Impl& im = *impl_;
  if (!im.binder || !renderer_) {
    error = "no binder yet";
    return false;
  }
  rdr_scene::PostOursOptions o;
  if (!rdr_scene::PostFaults::Parse(fault, o.faults, error)) return false;
  if (!limits.empty() && limits != "no_r32f_linear") {
    error = "unknown --post-limits '" + limits + "' (no_r32f_linear)";
    return false;
  }
  o.limits.no_r32f_linear = limits == "no_r32f_linear";
  o.limits.render_scale = render_scale;
  im.post = std::make_unique<rdr_scene::PostOurs>(*renderer_, o);
  im.binder->SetPostHook(im.post.get());
  rdr_adapter::g_post_ours = true;
  return true;
}

std::string Session::PostReport() const { return impl_->binder ? impl_->binder->PostCensus() : std::string(); }

std::string Session::ReplaceReport(const std::string& kind) const {
  const Impl& im = *impl_;
  if (!im.binder) return "";
  const std::string name = kind == "test" ? "test" : kind;
  std::string text = im.binder->ReplaceCensus(name.c_str());
  if (kind == "test" && im.replace_test) text += "\n[fview] [rdr][test] claims seen by the test hook " + std::to_string(im.replace_test->claims_seen());
  for (size_t i = 0; i < im.replace_handles.size(); ++i)
    if (im.replace_kinds[i] == kind && im.replace_handles[i].report) text += "\n[fview] [rdr][" + kind + "] " + im.replace_handles[i].report();
  return text;
}

uint64_t Session::replace_test_claims() const { return impl_->replace_test ? impl_->replace_test->claims_seen() : 0; }

bool Session::SetCrowdFloor(bool floor, const std::string& synth_file, std::string& error) {
  Impl& im = *impl_;
  im.crowd_floor = floor;
  if (!synth_file.empty()) {
    if (!floor) {
      error = "--crowd-floor-synth needs --crowd-floor";
      return false;
    }
    im.floor_synth = std::make_unique<rdr_scene::FloorSynth>();
    if (!im.floor_synth->Load(synth_file, error)) return false;
  }
  return true;
}

void Session::SetHashFrames(bool on) { impl_->hash_frames = on; }  // S101 lane CD
void Session::SetDumpFrames(const std::vector<uint32_t>& frames, const std::string& dir) {  // lane 155
  impl_->dump_frames.insert(frames.begin(), frames.end());
  impl_->dump_dir = dir;
}
const std::vector<uint64_t>& Session::FrameHashes() const { return impl_->frame_hashes; }
uint32_t Session::PresentFormat() const { return impl_->present_format; }

bool Session::SetCrowdMembersThin(bool thin, bool probe, const std::string& counts_file, std::string& error) {  // lane 272
  impl_->crowd_members_thin = thin;
  impl_->crowd_members_probe = probe || thin;
  if (counts_file.empty()) return true;   // no sidecar: no frame is thin (the entry needs the tally)
  std::ifstream in(counts_file);
  if (!in) {
    error = "cannot read --crowd-members-thin-counts " + counts_file;
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    unsigned long long f = 0, world = 0;
    unsigned site = 0, seen = 0, dropped = 0;
    if (std::sscanf(line.c_str(), "%llu,%u,%u,%u,%llu", &f, &site, &seen, &dropped, &world) == 5 && site < 2) impl_->member_counts[site][f] = {seen, dropped, world};
  }
  return true;
}

bool Session::SetCrowdCardsThin(bool thin, const std::string& counts_file, std::string& error) {  // lane 175
  impl_->crowd_cards_thin = thin;
  if (!thin) return true;
  if (counts_file.empty()) return true;   // no sidecar: no frame is thin (the entry needs the tally)
  std::ifstream in(counts_file);
  if (!in) {
    error = "cannot read --crowd-cards-thin-counts " + counts_file;
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    unsigned long long f = 0, world = 0;
    unsigned seen = 0, dropped = 0;
    if (std::sscanf(line.c_str(), "%llu,%u,%u,%llu", &f, &seen, &dropped, &world) == 4) impl_->thin_counts[f] = {seen, dropped, world};
  }
  return true;
}

void Session::SetCrowdCards(bool cards, bool unchecked) {  // S101 lane CD
  impl_->crowd_cards = cards;
  impl_->crowd_cards_unchecked = unchecked;
}

bool Session::EnableCrowdOurs(const std::string& game_root, const std::string& fault, std::string& error) {
  Impl& im = *impl_;
  if (!capture_ || capture_->scene_record_count() == 0) {
    error = "the capture has no scene records (scene\\frame_NNNNNN.rsc): taken without --wwe2k14_scene_records";
    return false;
  }
  rdr_scene::CrowdOursOptions o;
  o.game_root = game_root;
  o.verify_pairing = true;  // (offline: the pairing proof costs a pass over the frame's draws)
  if (const char* ve = std::getenv("RDR_CROWD_VERIFY_EVERY")) {  // lane 270 (timing only): the live cadence (verify every Nth frame) instead of every frame
    o.verify_pairing = false;
    o.verify_every = uint32_t(std::atoi(ve));
  }
  o.floor = im.crowd_floor;  // S101 lane 82
  o.cards = im.crowd_cards;  // S101 lane CD
  o.cards_unchecked = im.crowd_cards_unchecked;
  o.cards_thin = im.crowd_cards_thin && im.crowd_cards;  // lane 175
  o.members_thin = im.crowd_members_thin;   // lane 272
  o.member_probe = im.crowd_members_probe;
  if (o.member_probe) {
    o.member_counts = [&im](uint64_t f, int site, uint32_t& seen, uint32_t& dropped, uint64_t& world) {
      const auto it = im.member_counts[site].find(f);
      if (it == im.member_counts[site].end()) return false;
      seen = it->second.seen, dropped = it->second.dropped, world = it->second.world;
      return true;
    };
  }
  if (o.cards_thin) {
    o.thin_counts = [&im](uint64_t f, uint32_t& seen, uint32_t& dropped, uint64_t& world) {
      const auto it = im.thin_counts.find(f);
      if (it == im.thin_counts.end()) return false;
      seen = it->second.seen, dropped = it->second.dropped, world = it->second.world;
      return true;
    };
  }
  if (fault.rfind("cards_", 0) == 0 && fault != "cards_as_bodies" && !im.crowd_cards) {
    error = "--crowd-fault " + fault + " needs --crowd-cards";
    return false;
  }
  if (fault.rfind("floor_", 0) == 0 && !im.crowd_floor) {
    error = "--crowd-fault " + fault + " needs --crowd-floor";
    return false;
  }
  if (fault == "bind_pose") o.fault_bind_pose = true;
  else if (fault == "cache_tex_stale") o.fault_cache_tex_stale = true;    // lane 270: the texture cache key leaves the picture out
  else if (fault == "identity_vp") o.fault_identity_viewproj = true;
  else if (fault == "no_cards") o.fault_no_cards = true;
  else if (fault == "double_mesh") o.fault_double_mesh = true;
  else if (fault == "undrawable_type") o.fault_undrawable_first = true;
  else if (fault == "lod0_palette") o.fault_lod0_palette = true;
  else if (fault == "record_missing_member") o.fault_record_missing_member = true, o.verify_pairing = false;  // S101 lane 82: verify OFF (a non-verified live frame)
  else if (fault == "floor_lod1") o.fault_floor_lod1 = true;                   // S101 lane 82: the floor members as the LOD 1 body
  else if (fault == "floor_type_shift") o.fault_floor_type_shift = true;       // ... as the next person type
  else if (fault == "floor_first_texture") o.fault_floor_first_texture = true; // ... all with the first floor draw's slot 0
  else if (fault == "floor_dup") o.fault_floor_dup = true;                     // ... emitted twice
  else if (fault == "floor_drop_no_draw") o.fault_floor_drop_no_draw = true;   // ... claimed and dropped, never drawn
  else if (fault == "hide_ours") o.hide_ours = true;  // test only: the sheet's mask (our draws not recorded, the stream's block draws dropped)
  else if (fault == "cards_as_bodies") o.cards_as_bodies = true;  // the first design, for comparison
  else if (fault == "cards_one_picture") o.cards_fault = rdr_scene::cards::Fault::OnePicture;        // S101 lane CD: lane 35's one texture for every member
  else if (fault == "cards_type_shift") o.cards_fault = rdr_scene::cards::Fault::TypeShift;          // the picture of type T + 1
  else if (fault == "cards_atlas_picture") o.cards_fault = rdr_scene::cards::Fault::AtlasPicture;    // the 512 x 512 page as the picture
  else if (fault == "cards_order_swap") o.cards_fault = rdr_scene::cards::Fault::OrderSwap;          // two cards of a type swapped
  else if (fault == "cards_order_reversed") o.cards_fault = rdr_scene::cards::Fault::OrderReversed;  // CONTROL
  else if (fault == "cards_missing_card") o.cards_fault = rdr_scene::cards::Fault::MissingCard;      // the first record card removed
  else if (fault == "cards_drop_unjoined") o.cards_fault = rdr_scene::cards::Fault::DropUnjoined;    // an unjoined type's cards dropped (a hole)
  else if (fault == "cards_no_cards") o.cards_fault = rdr_scene::cards::Fault::NoCards;              // CONTROL: the record holds no cards (the fallback rule)
  else if (fault == "cards_blob_drift") o.cards_fault = rdr_scene::cards::Fault::BlobDrift;          // review F2: the expected quad differs from the stream's blob
  else if (fault == "cards_state_drift") o.cards_fault = rdr_scene::cards::Fault::StateDrift;        // ... a card draw with another program than the first
  else if (fault == "cards_gap") o.cards_fault = rdr_scene::cards::Fault::CardGap;   // lane 223
  else if (fault == "cards_inject_early") o.cards_fault = rdr_scene::cards::Fault::InjectEarly;      // ... injection before the page copy
  else if (fault == "cards_position_all") o.cards_fault = rdr_scene::cards::Fault::PositionAll;      // every type joined by bake position: byte-equal
  else if (fault == "cards_position_reversed") o.cards_fault = rdr_scene::cards::Fault::PositionReversed;  // ... reversed: fails the picture
  else if (fault == "cards_keep_first") o.cards_fault = rdr_scene::cards::Fault::KeepFirst;          // the first stream card stays the game's
  else if (fault == "cards_thin_first_shift") o.cards_fault = rdr_scene::cards::Fault::ThinFirstOfTypeShift;  // lane 175
  else if (fault == "cards_thin_type_missing") o.cards_fault = rdr_scene::cards::Fault::ThinTypeMissing;      // lane 175
  else if (fault == "cards_thin_trust_count") o.cards_fault = rdr_scene::cards::Fault::ThinTrustCount;        // lane 175 CONTROL
  else if (fault == "cards_unjoin_type") o.cards_fault = rdr_scene::cards::Fault::UnjoinType;        // TWIN: a type unjoined and kept (picture = the stream's)
  else if (fault == "cards_uv_swapped") o.fault_cards_uv_swapped = true;
  else if (fault == "cards_copy_swap_lost") o.fault_cards_copy_swap_lost = true;
  else if (fault == "cards_no_alpha_test") o.fault_cards_no_alpha_test = true;
  else if (fault == "member_one_material") o.fault_member_one_material = true;   // lane 272: every class takes the frame's first kept draw's bank and picture
  else if (fault == "member_own_sd") o.fault_member_own_sd = true;                  // lane 272: the thin sd choice dropped
  else if (fault == "member_no_tally") o.fault_member_no_tally = true;            // lane 272: the tally check is skipped
  else if (fault == "offset") im.crowd_offset = 0;  // planted: the record one frame off the correct pairing (j, not j-1); the correct one is j-1
  else if (!fault.empty()) {
    error = "unknown --crowd-fault '" + fault + "' (bind_pose, identity_vp, no_cards, offset, double_mesh, cards_as_bodies, lod0_palette, undrawable_type, hide_ours, record_missing_member, floor_lod1, floor_type_shift, floor_first_texture, floor_dup, floor_drop_no_draw; with --crowd-cards: cards_one_picture, cards_type_shift, cards_atlas_picture, cards_order_swap, cards_order_reversed, cards_missing_card, cards_no_cards, cards_blob_drift, cards_state_drift, cards_inject_early, cards_position_all, cards_position_reversed, cards_keep_first, cards_drop_unjoined, cards_unjoin_type, cards_uv_swapped, cards_copy_swap_lost, cards_no_alpha_test)";
    return false;
  }
  im.crowd = std::make_unique<rdr_scene::CrowdOurs>(*renderer_, o);
  im.binder->SetCrowdHook(im.crowd.get());
  rdr_adapter::g_crowd_ours = true;
  return true;
}

bool Session::PrimeCrowd(double seconds, std::string& message) {
  Impl& im = *impl_;
  if (!im.crowd) return true;
  const auto start = std::chrono::steady_clock::now();
  const uint32_t limit = std::min<uint32_t>(frame_count(), 600);
  for (uint32_t f = 0; f < limit; ++f) {
    FeedCrowd(f);
    im.crowd->FrameReady();
    if (im.crowd->arena() < 0) continue;  // no crowd in this record
    while (!im.crowd->FrameReady()) {
      if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) {
        message = "the crowd's bodies for arena " + std::to_string(im.crowd->arena()) + " were not loaded in " + std::to_string(int(seconds)) + " s (game folder wrong?)";
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    message = "crowd bodies for arena " + std::to_string(im.crowd->arena()) + " ready after frame " + std::to_string(f) + "'s record";
    im.crowd_record_index = -2;  // (the next Feed reads its own frame's record again)
    return true;
  }
  message = "no scene record in the first " + std::to_string(limit) + " frames has a crowd";
  return false;
}

std::vector<uint32_t> Session::CrowdMembersByFrame() const { return impl_->crowd_by_frame; }

std::string Session::CrowdReport() const {
  const Impl& im = *impl_;
  if (!im.binder) return "";
  const rdr_adapter::CrowdCounters& c = im.binder->crowd_counters();
  char line[320];
  std::snprintf(line, sizeof line, "[rdr][crowd] ours frames %llu, fallback %llu, stream draws dropped %llu, members drawn %llu, ms ours %.2f (stream draws kept for members ours could not draw: %llu), persist-only %llu", static_cast<unsigned long long>(c.ours_frames),
                static_cast<unsigned long long>(c.fallback_frames), static_cast<unsigned long long>(c.draws_dropped), static_cast<unsigned long long>(c.members_drawn), c.ms_ours, static_cast<unsigned long long>(c.draws_kept), static_cast<unsigned long long>(c.persist_only));
  std::string text = line;
  if (im.crowd && im.crowd->InjectsCards()) {  // S101 lane CD: the binder's side of the cards census
    char cl[320];
    std::snprintf(cl, sizeof cl, "\n[rdr][crowd] cards: frames ours %llu, fallback %llu, card draws dropped %llu, cards drawn %llu, kept %llu, ms ours %.2f", static_cast<unsigned long long>(c.card_frames_ours),
                  static_cast<unsigned long long>(c.card_frames_fallback), static_cast<unsigned long long>(c.card_draws_dropped), static_cast<unsigned long long>(c.cards_drawn), static_cast<unsigned long long>(c.card_draws_kept), c.card_ms_ours);
    text += cl;
  }
  return text + (im.crowd ? "\n[fview] " + im.crowd->Report() : std::string());
}
std::string Session::StageViewReport() const {  // S101 lane 59
  const Impl& im = *impl_;
  if (!im.binder) return "";
  char line[160];
  std::snprintf(line, sizeof line, "[rdr][stage_view] frames %llu, missed %llu, last target %u", static_cast<unsigned long long>(im.binder->stage_view_frames()), static_cast<unsigned long long>(im.binder->stage_view_missed()),
                im.binder->stage_view_target());
  return line;
}
uint64_t Session::crowd_frames_ours() const { return impl_->binder ? impl_->binder->crowd_counters().ours_frames : 0; }
uint64_t Session::crowd_frames_fallback() const { return impl_->binder ? impl_->binder->crowd_counters().fallback_frames : 0; }
uint64_t Session::crowd_draws_dropped() const { return impl_->binder ? impl_->binder->crowd_counters().draws_dropped : 0; }

bool Session::Open(const std::string& folder, const rdr::RendererOptions& options, std::string& error) {
  folder_ = folder;
  capture_ = std::make_unique<rdr_adapter::CaptureReader>();
  if (!capture_->Open(folder)) {
    error = "could not open capture " + folder + " (needs images.bin and frames\\frame_000000.bin)";
    return false;
  }
  renderer_ = rdr::Renderer::Create(options, error);
  if (!renderer_) return false;
  impl_->binder = std::make_unique<rdr_adapter::Binder>(*renderer_);
  impl_->binder->CreateImages(*capture_);
  if (std::getenv("L162_CAPTURED_ONLY")) impl_->binder->SetThinCapturedOnly(true);   // lane 162 (TEST ONLY): only the capture's own lite draws are lite
  impl_->binder->SetThinOffline(true);  // lane 122: a capture replay learns its thin keys from its own full frames
  return true;
}

uint32_t Session::frame_count() const { return capture_ ? capture_->frame_count() : 0; }
uint64_t Session::validation_errors() const { return renderer_ ? renderer_->validation_errors() : 0; }
uint64_t Session::stale_contents() const { return impl_->binder ? impl_->binder->stale_contents() : 0; }
uint64_t Session::stale_subres() const { return impl_->binder ? impl_->binder->stale_subres() : 0; }
bool Session::validation_active() const { return renderer_ && renderer_->validation_active(); }
uint32_t Session::renderer_op_count() const { return impl_->full.ops.count; }

namespace {
void Add(ReplayTotals& totals, const rdr_adapter::BindStats& stats) {
  ++totals.frames;
  totals.draws += stats.draws;
  totals.bound += stats.bound;
  totals.no_shader += stats.no_shader;
  totals.no_geometry += stats.no_geometry;
  totals.bad_layout += stats.bad_layout;
  totals.high_slot += stats.high_slot;
}
}  // namespace

bool Session::ReplayThrough(uint32_t count, ReplayTotals& totals, std::string& error) {
  Impl& im = *impl_;
  count = std::min(count, frame_count());
  if (im.ui_on) im.content_table.Clear();  // S101 lane 66: the contents are fed again from frame 0
  rdr_adapter::LoadedFrame frame;
  rdr::FrameBuilder builder;
  for (uint32_t f = 0; f < count; ++f) {
    if (!capture_->LoadFrame(f, frame)) {
      error = "could not read frame " + std::to_string(f);
      return false;
    }
    im.binder->ApplyContents(frame, *capture_);
    FeedUi(im, *capture_, *renderer_, frame);  // S101 lane 66
    builder.Begin(f);
    rdr_adapter::BindStats stats;
    FeedCrowd(f);  // S101 lane 35
    const uint64_t members_before = im.binder->crowd_counters().members_drawn;
    if (im.thin_on) im.binder->ThinEmulateReader(frame, im.thin_lag);  // lane 122
    im.binder->Bind(frame, *capture_, builder, stats);
    if (im.crowd_by_frame.size() <= f) im.crowd_by_frame.resize(size_t(f) + 1, 0);
    im.crowd_by_frame[f] = uint32_t(im.binder->crowd_counters().members_drawn - members_before);
    renderer_->RenderFrame(builder.Finish());
    Add(totals, stats);
    if (im.hash_frames || im.dump_frames.count(f)) {  // S101 lane CD: the presented surface of this frame
      renderer_->WaitIdle();
      uint32_t surface = 0;
      for (const rdr_adapter::Op& op : frame.ops)
        if (op.kind == rdr_adapter::OpKind::Present) surface = frame.presents[op.index].source.image;
      uint64_t hash = 0;
      std::vector<uint8_t> rgba;
      uint32_t w = 0, h = 0;
      if (surface && ReadSurface(surface, rgba, w, h)) {
        hash = 0xcbf29ce484222325ull;
        for (uint8_t byte : rgba) hash = (hash ^ byte) * 0x100000001b3ull;
      }
      im.frame_hashes.push_back(hash);
      if (im.dump_frames.count(f) && !rgba.empty()) {  // lane 155: the picture of a chosen frame, for the magnitude of a difference the hash only counts
        char name[64];
        std::snprintf(name, sizeof name, "/frame_%05u.png", f);
        std::filesystem::create_directories(im.dump_dir);
        rdr::WritePng((im.dump_dir + name).c_str(), rgba.data(), w, h);
      }
      for (const rdr_adapter::ImageRecord& r : capture_->images())
        if (r.id == surface) im.present_format = r.format;  // review: the presented surface's storage format (rdr::Format; 8-bit = RGBA8 1 / BGRA8 3)
    }
  }
  renderer_->WaitIdle();
  im.replayed = count;
  return true;
}

// S101 lane 99: every frame played in order (the hooks keep their state across frames, as live), and after each the picture (the Present source) and the given images read back and hashed.
// One CSV line a frame: frame,picture_hash,image_hash... (FNV-1a 64 over the RGBA bytes ReadImage returns; the same bytes in two arms = the same hash).
bool Session::SweepHashes(const std::vector<uint32_t>& images, int slot, std::string& csv, std::string& error) {
  Impl& im = *impl_;
  if (im.ui_on) im.content_table.Clear();
  rdr_adapter::LoadedFrame frame;
  rdr::FrameBuilder builder;
  const auto fnv = [](const std::vector<uint8_t>& b, uint32_t w, uint32_t h) {
    uint64_t x = 1469598103934665603ull ^ (uint64_t(w) << 32 | h);
    for (uint8_t c : b) x = (x ^ c) * 1099511628211ull;
    return x;
  };
  csv = "frame,picture";
  for (uint32_t id : images) csv += ",img" + std::to_string(id);
  if (slot >= 0) csv += ",slot" + std::to_string(slot) + "_images,slot" + std::to_string(slot) + "_hash";
  csv += "\n";
  for (uint32_t f = 0; f < frame_count(); ++f) {
    if (!capture_->LoadFrame(f, frame)) {
      error = "could not read frame " + std::to_string(f);
      return false;
    }
    im.binder->ApplyContents(frame, *capture_);
    FeedUi(im, *capture_, *renderer_, frame);
    builder.Begin(f);
    rdr_adapter::BindStats stats;
    FeedCrowd(f);
    im.binder->D3CEmulateReader(frame);   // lane 228: a --d3 casters sweep hashes the shadow-map / rim-map images WITH the markers (no-op unless --d3 casters)
    im.binder->Bind(frame, *capture_, builder, stats);
    rdr::Frame full = builder.Finish();
    full.keep_contents = true;   // nothing the game marks dead at the frame's end is dropped: the surfaces are read after
    renderer_->RenderFrame(full);
    uint32_t present = 0;
    for (const rdr_adapter::Op& op : frame.ops)
      if (op.kind == rdr_adapter::OpKind::Present) present = frame.presents[op.index].source.image;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    char line[64];
    std::snprintf(line, sizeof line, "%u,", f);
    csv += line;
    const auto one = [&](uint32_t id) {
      rgba.clear();
      w = h = 0;
      if (id && ReadSurface(id, rgba, w, h)) std::snprintf(line, sizeof line, "%016llx", (unsigned long long)fnv(rgba, w, h));
      else std::snprintf(line, sizeof line, "unreadable");
      csv += line;
    };
    one(present);
    for (uint32_t id : images) csv += ",", one(id);
    if (slot >= 0) {   // every distinct image the frame's draws bind at that fetch slot (slot 12: the shadow readers' maps and the plane mask), one combined hash
      std::set<uint32_t> ids;
      for (uint32_t t = 0; t < frame.texture_slots.count; ++t)
        if (frame.texture_slots[t].slot == slot && frame.texture_slots[t].image) ids.insert(frame.texture_slots[t].image);
      uint64_t combined = 1469598103934665603ull;
      for (uint32_t id : ids) {
        rgba.clear();
        w = h = 0;
        const uint64_t one_hash = ReadSurface(id, rgba, w, h) ? fnv(rgba, w, h) : 0xDEADull;
        combined = (combined ^ id) * 1099511628211ull;
        combined = (combined ^ one_hash) * 1099511628211ull;
      }
      std::snprintf(line, sizeof line, ",%zu,%016llx", ids.size(), (unsigned long long)combined);
      csv += line;
    }
    csv += "\n";
  }
  renderer_->WaitIdle();
  return true;
}

// The frames before `f`, played in full (from frame 0 again when going back).
bool Session::ReplayTo(uint32_t f, std::string& error) {
  Impl& im = *impl_;
  if (f < im.replayed) im.replayed = 0;
  if (im.ui_on && im.replayed == 0) im.content_table.Clear();  // S101 lane 66: replaying from frame 0 feeds the contents again
  ReplayTotals ignore;
  rdr_adapter::LoadedFrame frame;
  rdr::FrameBuilder builder;
  while (im.replayed < f) {
    const uint32_t k = im.replayed;
    if (!capture_->LoadFrame(k, frame)) {
      error = "could not read frame " + std::to_string(k);
      return false;
    }
    im.binder->ApplyContents(frame, *capture_);
    FeedUi(im, *capture_, *renderer_, frame);  // S101 lane 66
    builder.Begin(k);
    rdr_adapter::BindStats stats;
    FeedCrowd(k);  // S101 lane 35
    if (im.thin_on) im.binder->ThinEmulateReader(frame, im.thin_lag);  // lane 122
    im.binder->Bind(frame, *capture_, builder, stats);
    renderer_->RenderFrame(builder.Finish());
    Add(ignore, stats);
    ++im.replayed;
  }
  return true;
}

bool Session::StepFrame(uint32_t f, std::string& error) {  // S101 lane 123
  Impl& im = *impl_;
  if (!capture_->LoadFrame(f, frame_)) {
    error = "could not read frame " + std::to_string(f);
    return false;
  }
  im.binder->ApplyContents(frame_, *capture_);
  FeedCrowd(f);  // lane 136: the replace hooks' records (the circle hook is fed here; with no hook and no crowd it returns at once)
  im.builder.Begin(f);
  rdr_adapter::BindStats stats;
  im.binder->Bind(frame_, *capture_, im.builder, stats);
  renderer_->RenderFrame(im.builder.Finish());
  return true;
}

bool Session::ReadPresented(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  const rdr::PresentOp present = impl_->binder->last_present();
  if (present.source.image == rdr::kNoImage) return false;
  return renderer_->ReadImage(present.source, rgba, width, height);
}
const rdr_adapter::shadows_ours::Plan& Session::ShadowsPlan() const { return impl_->binder->shadows_plan(); }
std::string Session::ShadowsReport() const { return impl_->binder->ShadowsOursReport(); }
std::string Session::CopyAliasReport() const { return impl_->binder ? impl_->binder->CopyAliasReport() : std::string(); }  // S101 lane 190
uint32_t Session::ShadowsOursWidth(uint32_t image) const { return impl_->binder->ShadowsOursWidth(image); }
uint32_t Session::ShadowsOursHeight(uint32_t image) const { return impl_->binder->ShadowsOursHeight(image); }

bool Session::SelectFrame(uint32_t f, std::string& error) {
  Impl& im = *impl_;
  if (f >= frame_count()) {
    error = "frame " + std::to_string(f) + " is not in the capture (" + std::to_string(frame_count()) + " frames)";
    return false;
  }
  if (frame_bound_ && f == frame_index_) return true;
  if (!ReplayTo(f, error)) return false;
  if (!capture_->LoadFrame(f, frame_)) {
    error = "could not read frame " + std::to_string(f);
    return false;
  }
  frame_index_ = f;
  im.binder->ApplyContents(frame_, *capture_);
  FeedUi(im, *capture_, *renderer_, frame_);  // S101 lane 66
  im.have_state = true;
  if (!BindFrame(error)) return false;
  im.history_cache.clear();
  rendered_through_ = -1;
  dirty_ = false;
  return true;
}

bool Session::BindFrame(std::string& error) {
  Impl& im = *impl_;
  if (!im.have_state) {
    error = "no frame selected";
    return false;
  }
  std::vector<rdr_adapter::BindTrace> bt;
  rdr_adapter::BindStats stats;
  stats.trace = &bt;
  im.builder.Begin(frame_index_);
  FeedCrowd(frame_index_);  // S101 lane 35
  if (im.thin_on) im.binder->ThinEmulateReader(frame_, im.thin_lag);  // lane 122
  const uint64_t nl143 = im.binder->thin_not_learned();  // lane 143
  (void)im.binder->TakeThinNotLearned();  // lane 214: only this bind's draws are listed
  im.binder->Bind(frame_, *capture_, im.builder, stats);
  if (im.thin_on && (frame_.header.flags & rdr_adapter::kFrameHasThin)) {  // lane 143: the census's guard: how much of THIS bound frame could not be drawn (a lite draw whose key was not learned from the capture's earlier frames)
    uint32_t lite = 0;
    for (uint32_t i = 0; i < frame_.draws.count; ++i) lite += (frame_.draws[i].flags & rdr_adapter::kDrawThin) ? 1u : 0u;
    std::printf("[fview] thin frame %u: %u lite draws, %llu not learned (drawn by nothing)\n", frame_index_, lite, static_cast<unsigned long long>(im.binder->thin_not_learned() - nl143));
    for (const auto& n : im.binder->TakeThinNotLearned()) std::printf("[fview] thin frame %u: not learned: object 0x%08X part %u pass site 0x%08X\n", frame_index_, n[0], n[1], n[2]);  // lane 214
  }
  im.full = im.builder.Finish();
  if (bt.size() != frame_.ops.count) {
    error = "the binder traced " + std::to_string(bt.size()) + " ops for a frame of " + std::to_string(frame_.ops.count);
    return false;
  }
  trace_.resize(bt.size());
  for (size_t i = 0; i < bt.size(); ++i) {
    OpTrace& t = trace_[i];
    t.frame_ops = bt[i].frame_ops;
    t.vs = bt[i].vs;
    t.ps = bt[i].ps;
    t.outcome = uint8_t(bt[i].outcome);
    t.alpha_requested = bt[i].alpha_requested;
    t.alpha_twin = bt[i].alpha_twin;
    t.program = bt[i].program;
    t.fate = bt[i].fate;
    t.keep_why = bt[i].keep_why;  // S101 lane 84
    t.geom_ours = bt[i].geom_ours;  // lane 126
  }
  frame_bound_ = true;
  return true;
}

bool Session::Rebind(std::string& error) {
  if (!BindFrame(error)) return false;
  impl_->history_cache.clear();
  dirty_ = true;  // the surfaces hold the old programs' results
  return true;
}

// Puts the surfaces back to the start of the selected frame: the frames before it are played again.
bool Session::ResetFrame(std::string& error) {
  Impl& im = *impl_;
  im.replayed = 0;
  if (!ReplayTo(frame_index_, error)) return false;
  im.binder->ApplyContents(frame_, *capture_);
  FeedUi(im, *capture_, *renderer_, frame_);  // S101 lane 66
  if (!BindFrame(error)) return false;
  rendered_through_ = -1;
  dirty_ = false;
  return true;
}

bool Session::RenderThrough(int op) {
  Impl& im = *impl_;
  if (!frame_bound_) return false;
  if (op >= int(trace_.size())) op = int(trace_.size()) - 1;
  if (legacy_prefix_) {  // the old behaviour (test only): ops 0..op again, over what is there
    if (op < 0 || trace_[size_t(op)].frame_ops == 0) return true;
    rdr::Frame cut = im.full;
    cut.ops.count = trace_[size_t(op)].frame_ops;
    cut.keep_contents = true;
    renderer_->RenderFrame(cut);
    rendered_through_ = op;
    return true;
  }
  if (dirty_ || op < rendered_through_) {
    std::string error;
    if (!ResetFrame(error)) return false;
  }
  if (op < 0 || op == rendered_through_) return true;
  const uint32_t from = rendered_through_ >= 0 ? trace_[size_t(rendered_through_)].frame_ops : 0;
  const uint32_t to = trace_[size_t(op)].frame_ops;
  rendered_through_ = op;
  if (to <= from) return true;
  // The segment [from, to) of the renderer's ops. A segment that starts inside a pass begins with the targets in force there.
  im.segment.clear();
  if (from > 0 && im.full.ops[from].kind != rdr::OpKind::Targets) {
    for (uint32_t i = from; i-- > 0;) {
      if (im.full.ops[i].kind == rdr::OpKind::Targets) {
        im.segment.push_back(im.full.ops[i]);
        break;
      }
    }
  }
  for (uint32_t i = from; i < to; ++i) im.segment.push_back(im.full.ops[i]);
  rdr::Frame cut = im.full;
  cut.ops.data = im.segment.data();
  cut.ops.count = uint32_t(im.segment.size());
  cut.keep_contents = true;  // nothing the game marks dead at the frame's end is dropped: the surfaces are read after
  renderer_->RenderFrame(cut);
  return true;
}

rdr::ImageId Session::MappedImage(uint32_t image) const { return impl_->binder ? impl_->binder->Mapped(image) : rdr::kNoImage; }  // S101 lane 34

bool Session::ReadSurface(uint32_t image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  rdr::ImageRef ref;
  ref.image = impl_->binder->Mapped(image);
  if (ref.image == rdr::kNoImage) return false;
  return renderer_->ReadImage(ref, rgba, width, height);
}

bool Session::ReadTexel(uint32_t image, int x, int y, uint8_t raw[8], uint32_t& raw_bytes, uint8_t rgba[4]) {
  rdr::ImageRef ref;
  ref.image = impl_->binder->Mapped(image);
  if (ref.image == rdr::kNoImage) return false;
  return renderer_->ReadTexelRaw(ref, x, y, raw, raw_bytes, rgba);
}

bool Session::ReadTexels(std::vector<TexelQuery>& queries) {
  std::vector<rdr::Renderer::TexelRead> reads(queries.size());
  for (size_t i = 0; i < queries.size(); ++i) {
    reads[i].image.image = impl_->binder->Mapped(queries[i].surface);
    reads[i].x = queries[i].x;
    reads[i].y = queries[i].y;
  }
  std::vector<rdr::Renderer::TexelRead> ask;
  std::vector<size_t> index;
  for (size_t i = 0; i < reads.size(); ++i)
    if (reads[i].image.image != rdr::kNoImage) ask.push_back(reads[i]), index.push_back(i);
  if (!renderer_->ReadTexelsRaw(ask)) return false;
  for (size_t k = 0; k < ask.size(); ++k) {
    TexelQuery& q = queries[index[k]];
    q.ok = ask[k].ok;
    std::memcpy(q.raw, ask[k].raw, 8);
    q.raw_bytes = ask[k].raw_bytes;
  }
  return true;
}

bool Session::ReadPixel(uint32_t image, int x, int y, uint8_t out[4]) {
  uint8_t raw[8];
  uint32_t bytes = 0;
  return ReadTexel(image, x, y, raw, bytes, out);
}

uint32_t Session::DefaultSurface() const {
  uint32_t surface = 0;
  for (const rdr_adapter::Op& op : frame_.ops) {
    if (op.kind == rdr_adapter::OpKind::Present) surface = frame_.presents[op.index].source.image;
  }
  if (surface) return surface;
  for (const rdr_adapter::Op& op : frame_.ops) {
    if (op.kind == rdr_adapter::OpKind::SetTargets && frame_.targets[op.index].color_count) surface = frame_.targets[op.index].color[0].image;
  }
  return surface;
}

namespace {
bool RectHas(const rdr::Rect& r, int x, int y) {
  return x >= r.x && y >= r.y && int64_t(x) < int64_t(r.x) + r.width && int64_t(y) < int64_t(r.y) + r.height;
}
}  // namespace

const std::vector<HistoryEntry>& Session::PixelHistory(uint32_t surface, int x, int y, int through_op) {
  Impl& im = *impl_;
  const std::string key = std::to_string(frame_index_) + "/" + std::to_string(surface) + "/" + std::to_string(x) + "/" + std::to_string(y) + "/" + std::to_string(through_op);
  const auto cached = im.history_cache.find(key);
  if (cached != im.history_cache.end()) return cached->second;
  std::vector<HistoryEntry> entries;  // oldest first
  if (!frame_bound_) return im.empty_history;

  // 1. The ops that reach the pixel, from the tables alone (mip 0 / layer 0 of the surface only; a clear only for the colour slots it names).
  uint32_t slots = 0;  // the colour slots of the current targets that hold the surface (mip 0, layer 0)
  const uint32_t ops_end = through_op >= 0 ? std::min<uint32_t>(frame_.ops.count, uint32_t(through_op) + 1) : frame_.ops.count;
  for (uint32_t i = 0; i < ops_end; ++i) {
    const rdr_adapter::Op& op = frame_.ops[i];
    switch (op.kind) {
      case rdr_adapter::OpKind::SetTargets: {
        slots = 0;
        const rdr::TargetsOp& t = frame_.targets[op.index];
        for (uint32_t c = 0; c < t.color_count; ++c)
          if (t.color[c].image == surface && t.color[c].mip == 0 && t.color[c].layer == 0) slots |= 1u << c;
        break;
      }
      case rdr_adapter::OpKind::Clear: {
        const rdr::ClearOp& c = frame_.clears[op.index];
        if (!(c.flags & rdr::kClearColor) || !(slots & c.color_mask)) break;
        if (!c.full && !RectHas(c.rect, x, y)) break;
        HistoryEntry e;
        e.op = i, e.kind = "clear", e.index = op.index;
        e.note = c.full ? "full clear" : "rect clear";
        entries.push_back(e);
        break;
      }
      case rdr_adapter::OpKind::Copy: {
        const rdr::CopyOp& c = frame_.copies[op.index];
        if (c.dest.image != surface || c.dest.mip != 0 || c.dest.layer != 0 || !RectHas(c.dest_rect, x, y)) break;
        HistoryEntry e;
        e.op = i, e.kind = "copy", e.index = op.index;
        e.note = "copy from image " + std::to_string(c.source.image);
        entries.push_back(e);
        break;
      }
      case rdr_adapter::OpKind::Draw: {
        if (!slots) break;
        const rdr_adapter::Draw& d = frame_.draws[op.index];
        const rdr_adapter::DrawDynamic& dyn = frame_.dynamics[d.dynamic];
        if (dyn.flags & rdr_adapter::kDynEmptyViewport) break;
        if (!RectHas(dyn.host.scissor, x, y)) break;
        if (!(dyn.flags & rdr_adapter::kDynClipOff)) {
          const float* v = dyn.host.viewport;
          if (float(x) + 0.5f < v[0] || float(y) + 0.5f < v[1] || float(x) + 0.5f >= v[0] + v[2] || float(y) + 0.5f >= v[1] + v[3]) break;
        }
        HistoryEntry e;
        e.op = i, e.kind = "draw", e.index = op.index;
        e.drawn = OpDrawn(trace_[i]);  // S101 lane 58
        if (!e.drawn) e.note = OutcomeText(trace_[i].outcome);
        entries.push_back(e);
        break;
      }
      default: break;
    }
  }

  // 2. The pixel after each of them that was drawn: the surfaces go back to the frame's start, then the frame is rendered forward one entry at a time, so
  // every op is applied once. The value before the first entry is only known when a clear or copy that covers the pixel comes before the first draw.
  int first = -1;
  for (size_t k = 0; k < entries.size(); ++k)
    if (entries[k].drawn) {
      first = int(k);
      break;
    }
  if (first >= 0) {
    bool covered_first = false;  // a covering clear / copy comes before the first draw: the start value is the game's own
    for (size_t k = size_t(first); k < entries.size(); ++k) {
      if (entries[k].kind == "clear" || entries[k].kind == "copy") covered_first = true;
      if (entries[k].kind == "draw") break;
    }
    bool known = false;
    uint8_t previous[8] = {};
    uint8_t previous_rgba[4] = {};
    uint32_t previous_bytes = 0;
    RenderThrough(-1);
    known = ReadTexel(surface, x, y, previous, previous_bytes, previous_rgba);
    bool first_draw_seen = false;
    for (size_t k = size_t(first); k < entries.size(); ++k) {
      HistoryEntry& e = entries[k];
      if (!e.drawn) continue;
      RenderThrough(int(e.op));
      uint8_t now[8] = {};
      uint8_t now_rgba[4] = {};
      uint32_t now_bytes = 0;
      const bool now_known = ReadTexel(surface, x, y, now, now_bytes, now_rgba);
      const bool stale_start = e.kind == "draw" && !first_draw_seen && !covered_first;  // nothing clears the pixel before this first draw
      if (e.kind == "draw") first_draw_seen = true;
      e.value_known = known && now_known && !stale_start;
      if (e.value_known) {
        std::memcpy(e.before, previous_rgba, 4);
        std::memcpy(e.after, now_rgba, 4);
        std::memcpy(e.raw_before, previous, 8);
        std::memcpy(e.raw_after, now, 8);
        e.raw_bytes = now_bytes;
        e.wrote = previous_bytes != now_bytes || std::memcmp(previous, now, 8) != 0;
        if (e.kind == "draw") e.note = e.wrote ? "wrote" : "covered, unchanged";
      } else if (e.kind == "draw") {
        e.note = stale_start ? "covers the pixel; its value before is unknown (nothing clears the surface before this draw)" : "covers the pixel (value not readable: surface format)";
      }
      std::memcpy(previous, now, 8);
      std::memcpy(previous_rgba, now_rgba, 4);
      previous_bytes = now_bytes;
      known = now_known;
    }
  }
  std::reverse(entries.begin(), entries.end());  // newest first
  return im.history_cache.emplace(key, std::move(entries)).first->second;
}

int Session::VariantCount() const { return int(std::size(rdr::fx::kVariants)); }
const char* Session::VariantName(int variant) const {
  return variant >= 0 && variant < VariantCount() ? rdr::fx::kVariants[variant].name : "(none)";
}
int Session::FindVariantByName(const std::string& name) const { return rdr_adapter::VariantByName(name.c_str()); }
bool Session::VariantHasAlphaTwin(int variant) const {
  if (variant < 0 || variant >= VariantCount() || !rdr::fx::kVariants[variant].spirv_alpha) return false;
  const auto found = impl_->overrides.find(variant);
  return found == impl_->overrides.end() || !found->second.drop_twin;  // a reload that dropped the twin: the table the binder uses has none
}

uint64_t Session::VariantSpirvHash(int variant, bool alpha) const {
  if (variant < 0 || variant >= VariantCount()) return 0;
  const uint32_t* words = nullptr;
  size_t count = 0;
  const auto found = impl_->overrides.find(variant);
  if (found != impl_->overrides.end() && !(alpha ? found->second.alpha : found->second.spirv).empty()) {
    const auto& v = alpha ? found->second.alpha : found->second.spirv;
    words = v.data(), count = v.size();
  } else {
    const rdr::fx::Variant& v = rdr::fx::kVariants[variant];
    words = alpha ? v.spirv_alpha : v.spirv;
    count = (alpha ? v.bytes_alpha : v.bytes) / 4;
  }
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; words && i < count; ++i) {
    for (int b = 0; b < 4; ++b) h = (h ^ ((words[i] >> (8 * b)) & 0xFF)) * 0x100000001b3ull;
  }
  return h;
}

void Session::SetOverride(int variant, std::vector<uint32_t> spirv, std::vector<uint32_t> alpha, bool drop_twin) {
  if (spirv.empty() && alpha.empty() && !drop_twin) impl_->overrides.erase(variant);
  else impl_->overrides[variant] = {spirv, alpha, drop_twin};
  impl_->binder->SetVariantOverride(variant, std::move(spirv), std::move(alpha), drop_twin);
}

std::vector<std::string> Session::TakeOverrideErrors() { return impl_->binder->TakeOverrideErrors(); }

std::string Session::CheckLayout(int variant, const VariantLayout& layout) const {
  if (variant < 0 || variant >= VariantCount()) return "not in the compiled table";
  const rdr::fx::Variant& v = rdr::fx::kVariants[variant];
  std::string why;
  const auto add = [&](const std::string& s) { why += (why.empty() ? "" : "; ") + s; };
  if (int(v.constant_count) != int(layout.ranges.size())) add("constant ranges " + std::to_string(v.constant_count) + " compiled vs " + std::to_string(layout.ranges.size()) + " rebuilt");
  else
    for (size_t i = 0; i < layout.ranges.size(); ++i) {
      const rdr::fx::ConstantRange& c = v.constants[i];
      if (c.reg != layout.ranges[i][0] || c.count != layout.ranges[i][1] || c.offset != layout.ranges[i][2]) {
        add("constant range " + std::to_string(i) + " (register/count/offset) differs");
        break;
      }
    }
  if (int(v.block_bytes) != layout.block_bytes) add("constant block " + std::to_string(v.block_bytes) + " vs " + std::to_string(layout.block_bytes) + " bytes");
  if (int(v.bools_offset) != layout.bools) add("bools offset differs");
  if (int(v.attribute_count) != int(layout.attributes.size())) add("attribute count differs");
  else
    for (size_t i = 0; i < layout.attributes.size(); ++i) {
      const rdr::fx::Attribute& a = v.attributes[i];
      if (a.fetch != layout.attributes[i][0] || a.location != layout.attributes[i][1] || a.integer != layout.attributes[i][2]) {
        add("attribute " + std::to_string(i) + " differs");
        break;
      }
    }
  if (int(v.sampler_count) != int(layout.samplers.size())) add("sampler count differs");
  else
    for (size_t i = 0; i < layout.samplers.size(); ++i) {
      const rdr::fx::Sampler& s = v.samplers[i];
      if (s.slot != layout.samplers[i][0] || s.kind != layout.samplers[i][1]) {
        add("sampler " + std::to_string(i) + " differs");
        break;
      }
    }
  return why;
}

}  // namespace fview
