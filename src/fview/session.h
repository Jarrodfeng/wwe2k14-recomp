// Session 101 lane 2 (fview): a capture opened for looking. It plays the capture through the REAL binder and renderer (as rplay does) and answers
// "what did op N do": the surface after any op, the draws that wrote a pixel, what the binder chose for a draw.
//
// How "after op N" works: the selected frame is bound once into an rdr::Frame; the binder's trace (BindStats::trace) says how many renderer ops
// each capture op became. The renderer is fed the frame in SEGMENTS (keep_contents: nothing dropped): going forward renders only the ops since the last
// render, so blended and additive draws are applied exactly once; going back (or after a reload) first puts the surfaces back to the frame's start by playing
// the frames before it again, then renders forward. Limit: a surface the game does not clear in the frame starts from whatever the earlier frames' replay left
// (frame 0: the last render's leftovers); the pixel history marks such a first draw's values unknown.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../renderer/renderer.h"
#include "../renderer_adapter/capture_reader.h"
#include "../renderer_adapter/shadows_ours.h"  // S101 lane 123

namespace fview {

// One capture op as the binder handled it (binder.h BindTrace, copied so this header needs no binder).
struct OpTrace {
  uint32_t frame_ops = 0;
  int32_t vs = -1, ps = -1;  // rdr::fx::kVariants indices the binder chose
  uint8_t outcome = 0;       // rdr_adapter::BindOutcome
  uint8_t alpha_requested = 0, alpha_twin = 0;
  uint16_t program = 0;
  uint8_t geom_ours = 0;     // lane 126: BindTrace::geom_ours
  uint8_t keep_why = 0;      // S101 lane 84: BindTrace::keep_why
  uint8_t fate = 0;          // S101 lane 58: rdr_adapter::kFate* (0 stream, 1 replaced, 2 crowd ours, 3 arena ours, 4 wrestlers ours, 5 crowd injected)
};

inline constexpr uint8_t kOutcomeBound = 4;  // rdr_adapter::BindOutcome::Bound
const char* OutcomeText(uint8_t outcome);
// S101 lane 58: a draw counts as drawn when the binder bound it OR it is the op that ran InjectCrowd (kFateCrowdInjected: dropped itself, but our whole crowd's ops follow it, so its pixels are ours). Other dropped crowd draws write nothing.
inline bool OpDrawn(const OpTrace& t) { return t.outcome == kOutcomeBound || t.fate == 5; }

struct HistoryEntry {
  uint32_t op = 0;
  std::string kind;      // "draw", "clear", "copy"
  uint32_t index = 0;    // the draw / clear / copy table index
  bool drawn = true;     // false: the binder left the draw out (outcome says why), so it wrote nothing
  bool value_known = false;
  bool wrote = false;    // the pixel's value changed (the raw texel bytes differ: exact for every format)
  uint8_t before[4] = {}, after[4] = {};  // display RGBA8 (16F: magnitude clamped to 0..1)
  uint8_t raw_before[8] = {}, raw_after[8] = {};
  uint32_t raw_bytes = 0;
  std::string note;      // why not drawn / what covered it
};

struct ReplayTotals {
  uint32_t frames = 0, draws = 0, bound = 0, no_shader = 0, no_geometry = 0, bad_layout = 0, high_slot = 0;
};

class Session {
 public:
  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool Open(const std::string& folder, const rdr::RendererOptions& options, std::string& error);
  const std::string& folder() const { return folder_; }
  const rdr_adapter::CaptureReader& capture() const { return *capture_; }
  rdr::Renderer& renderer() { return *renderer_; }
  uint32_t frame_count() const;
  uint64_t validation_errors() const;
  uint64_t stale_contents() const;  // lane 195: contents the binder skipped (format rule + SubresourceFits)
  uint64_t stale_subres() const;
  bool validation_active() const;  // the validation layer is really loaded (--validation asked for it)

  // S101 lane 35 (the crowd's first promotion): draw the crowd from the scene records (scene\frame_NNNNNN.rsc beside the frames) instead of the stream's block / card
  // draws. `fault`: "" or one of bind_pose, identity_vp, no_cards (cards dropped), offset (the record one frame behind), as the acceptance test's planted twins.
  // PrimeCrowd waits (up to `seconds`) for the first frame with a crowd record to have its bodies loaded, so the replay that follows is not a fallback one.
  bool EnableCrowdOurs(const std::string& game_root, const std::string& fault, std::string& error);
  // S101 lane 82: call BEFORE EnableCrowdOurs. floor: the seated floor people are ours too (CrowdOursOptions::floor); synth_file (test only, may be empty): floor members added to a copy of each record
  // read (crowd_floor_synth.h). The planted floor faults (floor_lod1, floor_type_shift, floor_first_texture, floor_dup, floor_drop_no_draw) are `fault` names of EnableCrowdOurs and need floor.
  bool SetCrowdFloor(bool floor, const std::string& synth_file, std::string& error);
  // S101 lane CD: call BEFORE EnableCrowdOurs. cards: the far cards are ours (CrowdOursOptions::cards; the planted faults cards_* are `fault` names of EnableCrowdOurs and need cards); unchecked: test only.
  void SetCrowdCards(bool cards, bool unchecked);
  bool SetCrowdMembersThin(bool thin, bool probe, const std::string& counts_file, std::string& error);  // lane 272 (member_thin_core.h): fview --crowd-members-thin [--crowd-members-probe] [--crowd-members-thin-counts FILE: hook_frame,site,seen,dropped,world per line]
  bool SetCrowdCardsThin(bool thin, const std::string& counts_file, std::string& error);  // lane 175 (card_thin_core.h): fview --crowd-cards-thin [--crowd-cards-thin-counts FILE: hook_frame,seen,dropped per line]
  // S101 lane CD: ReplayThrough hashes the presented surface of every frame (FNV-1a of its bytes) into FrameHashes() (the byte-equal proofs of the promotions).
  void SetHashFrames(bool on);
  void SetDumpFrames(const std::vector<uint32_t>& frames, const std::string& dir);  // lane 155: with --replay, write the presented surface of these frames as PNG (dir/frame_NNNNN.png)
  const std::vector<uint64_t>& FrameHashes() const;
  uint32_t PresentFormat() const;  // rdr::Format of the last hashed presented surface
  bool PrimeCrowd(double seconds, std::string& message);
  std::vector<uint32_t> CrowdMembersByFrame() const;  // per replayed frame: members drawn by us (ReplayThrough / ReplayTo fill it)
  std::string CrowdReport() const;  // the binder's counters + the hook's
  std::string StageViewReport() const;  // S101 lane 59: "[rdr][stage_view] frames N, missed M"
  uint64_t crowd_frames_ours() const;
  uint64_t crowd_frames_fallback() const;
  uint64_t crowd_draws_dropped() const;

  // S101 lane 41 (the generic in-place replace seam, draw_replace_hook.h): switch a hook on. kind: "test" (the transparent test hook; `fault` is its comma list, see
  // replace_test_hook.h), "arena" or "wrestlers" (the class's factory, replace_factories.h; refused with a message while no class registered one). Call before the
  // first frame is selected. ReplaceReport(kind) is the binder's census line for the hook (+ the class's own report).
  bool EnableReplace(const std::string& kind, const std::string& fault, const std::string& game_root, std::string& error);
  std::string ReplaceReport(const std::string& kind) const;
  void SetTestFsBlockCapacity(uint32_t bytes);   // lane 151 (TEST ONLY, --rim-chain-fs-capacity): the replace seam's pixel block buffer is this many bytes (the pack must refuse when the block does not fit)
  void SetTestSlotOverride(uint32_t from, uint32_t to);   // lane 151 (TEST ONLY, --rim-chain-override): the rim reductions' input image FROM is read as TO in this session
  // lane 136: play the circle blob shadows' record from a circles oracle file (tools\shadow\circles_oracle.py dump) through the shipped writer's tracker (circle_oracle.h), per frame, into the "circles" hook.
  // lane 139: the same for the plane shadow chain (tools\shadow\plane_oracle.py dump, src\renderer_scene\plane_oracle.h).
  bool SetPlaneOracle(const std::string& path, std::string& error);
  bool SetCircleOracle(const std::string& path, std::string& error);
  // S101 lane 67 (particle runs as one draw, binder.h / particle_runs.h): switch on, with a planted fault name or empty. false + error: unknown fault. The census line at the end.
  bool EnableParticles(const std::string& fault, std::string& error);
  // S101 lane PO (the post chain, post_hook.h): fault = PostFaults::Parse's list, limits = "" or "no_r32f_linear", render_scale = --scale. Needs neither scene records nor the game files.
  bool EnablePostOurs(const std::string& fault, const std::string& limits, float render_scale, std::string& error);
  std::string PostReport() const;
  std::string ParticleReport() const;
  // lane 122 (D1 thin draws, renderer_adapter\thin_draw.h): switch the class's thin draws on (needs its --*-ours hook enabled). The reader is EMULATED on the capture: before each frame is bound the
  // pins published `lag` binds ago decide which keyed draws go lite and the fields a lite reader would not fill are blanked. `fault`: a thin::Fault name; proof_k: the proof rule's K (-1 = 32,
  // 0 = never); churn: TEST ONLY, every N-th frame one pinned key is unpinned (the handshake's workout).
  bool EnableThin(const std::string& cls, const std::string& fault, int lag, int proof_k, int churn, std::string& error);
  // Binds and renders frames [0, count) once each (no re-binds) and writes out_dir\frames.csv: frame, 64-bit hash of the presented picture, draws, bound. swap_frame >= 0 (TEST ONLY, either arm):
  // from that frame on, the arena draws that bind picture A in slot 0 bind picture B (another size) in the capture itself: the stream really changed under a pinned key.
  // epoch_at >= 0 (TEST ONLY): at that frame the hook's assets count as replaced (templates cleared). isolate (TEST ONLY): from frame isolate_from the binds are isolate binds with that name.
  bool ThinRun(uint32_t count, const std::string& out_dir, int swap_frame, std::string& error, int epoch_at = -1, const std::string& isolate = std::string(), int isolate_from = 0, int vswap_frame = -1, int release_frame = -1);   // lane 129: vswap_frame (review F4) and release_frame (review F2) are TEST ONLY
  bool EnableD3(const std::string& cls, const std::string& mode, const std::string& fault, int lag, const std::string& ledger_in, const std::string& ledger_out, bool no_proof, std::string& error, bool verify = false, int record_offset = -1);   // lane 138: D3 skip the object (d3_binder.h); needs --thin arena. lane 152: mode `record` = the record route (the capture's scene records), verify = the (10) verify arm
  void SetD3RingHookFromRecord(bool on);   // lane 258: --d3-hook-from-record (after EnableD3)
  void DeliverD3Record(uint32_t i);   // lane 152 (B1)
  std::string D3Report();                                                                                        // lane 138: the census + readiness lines
  std::string ThinReport(const std::string& cls = "arena") const;  // lane 129 (KC): the class's census line
  // S101 lane 47: where a class's image_info (width / height per capture image id) comes from. "capture" (default): the capture reader directly. "live": the live path's thread-safe
  // ImageInfoTable (image_info_table.h), fed from the capture's image records through the same Set() the render thread uses; must answer exactly like "capture". "live-fault": the same
  // table with every width one too big (a planted fault: the size checks must then refuse). "none": no supplier (the old live behaviour: sizes unchecked, the stream's images stay).
  // Call before EnableReplace.
  bool SetImageInfoMode(const std::string& mode, std::string& error);
  uint64_t replace_test_claims() const;  // the test hook's own tally of claimed draws (the independent side of replaced + kept == claimed)

  // rplay's replay: frames [0, count) in full, one after the other, accumulating the binder's totals.
  bool ReplayThrough(uint32_t count, ReplayTotals& totals, std::string& error);
  // S101 lane 99: every frame in order, then the picture and the given images hashed per frame (CSV text in `csv`): the offline A/B of a promotion over a whole capture in one process.
  bool SweepHashes(const std::vector<uint32_t>& images, int slot, std::string& csv, std::string& error);   // slot >= 0: also one combined hash of every distinct image bound at that fetch slot of any draw

  // S101 lane 123 (--shadows-check): frame `f` bound and rendered in full on its own (the frames before it are the caller's to have stepped, in order); frame() is it afterwards.
  bool StepFrame(uint32_t f, std::string& error);
  bool ReadPresented(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);  // the last present's source, RGBA8
  const rdr_adapter::shadows_ours::Plan& ShadowsPlan() const;
  std::string ShadowsReport() const;
  std::string CopyAliasReport() const;   // S101 lane 190 (copy_alias.h): the elide census, printed by fview at the end of a replay
  uint32_t ShadowsOursWidth(uint32_t image) const;   // 0: the stream's image
  uint32_t ShadowsOursHeight(uint32_t image) const;

  // Makes `f` the selected frame: the frames before it are played, then it is bound (with the trace) but not yet rendered.
  bool SelectFrame(uint32_t f, std::string& error);
  uint32_t frame_index() const { return frame_index_; }
  const rdr_adapter::LoadedFrame& frame() const { return frame_; }
  const std::vector<OpTrace>& trace() const { return trace_; }
  // Binds the selected frame again (after SetOverride changed a program); the surfaces are put back before the next render.
  bool Rebind(std::string& error);

  // Leaves every surface as it is after op `op` of the selected frame (op < 0: the frame's start). Forward = only the ops since the last call are rendered;
  // backward = the surfaces are put back first (class comment). False: no frame, or the frames could not be replayed.
  bool RenderThrough(int op);
  // Mip 0 of a capture image as RGBA8 (formats ReadImage supports). False: not readable. (16F surfaces are scaled by their peak: display only.)
  bool ReadSurface(uint32_t image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
  // S101 lane 34: the renderer image a capture image id is bound to now (kNoImage: none). post_proof hands it to the post pass as its input.
  rdr::ImageId MappedImage(uint32_t image) const;
  bool ReadPixel(uint32_t image, int x, int y, uint8_t out[4]);
  // One texel as the GPU holds it: raw bytes (compare these) and a display RGBA8.
  bool ReadTexel(uint32_t image, int x, int y, uint8_t raw[8], uint32_t& raw_bytes, uint8_t rgba[4]);
  // S101 lane 20: many texels in one submit (capture image ids; mip 0 / layer 0). `ok` false: not readable.
  struct TexelQuery {
    uint32_t surface = 0;
    int x = 0, y = 0;
    uint8_t raw[8] = {};
    uint32_t raw_bytes = 0;
    bool ok = false;
  };
  bool ReadTexels(std::vector<TexelQuery>& queries);
  // Renderer ops (the binder's) the selected frame became, and how many of them the last render covered.
  uint32_t renderer_op_count() const;
  // TEST ONLY (fview --legacy-prefix): render ops 0..N over whatever the surfaces hold, never putting them back. This is the behaviour the pixel history had before
  // the fix; the selftest turns it on to prove its drift check can fail.
  void set_legacy_prefix(bool on) { legacy_prefix_ = on; }
  int rendered_through() const { return rendered_through_; }

  // The draws, clears and copies that touch (x, y) of `surface` in the selected frame, newest first, each with the pixel before and after.
  // `through_op` >= 0: the history as of that op (later ops are ignored): "who put this pixel there before the post passes reused the surface".
  const std::vector<HistoryEntry>& PixelHistory(uint32_t surface, int x, int y, int through_op = -1);
  // The surface a query without --surface means: the last present's source, else the last colour target.
  uint32_t DefaultSurface() const;

  // The binder's registry (rdr::fx), by index and name.
  int VariantCount() const;
  const char* VariantName(int variant) const;
  int FindVariantByName(const std::string& name) const;
  bool VariantHasAlphaTwin(int variant) const;
  // FNV-1a of the SPIR-V the binder uses for the variant now (the override if one is set), `alpha`: the alpha-test twin.
  uint64_t VariantSpirvHash(int variant, bool alpha) const;
  // Replaces a variant's SPIR-V (live reload); empty both = back to the compiled one. `drop_twin`: the rebuilt variant has no alpha-test twin (its description
  // says "alpha_test": false), so the compiled one is not used either. Call Rebind() to see it.
  void SetOverride(int variant, std::vector<uint32_t> spirv, std::vector<uint32_t> alpha, bool drop_twin);
  // A reloaded variant's layout (from tools/fview/reload_effect.py) against the compiled table's: "" when they are the same, else what differs.
  struct VariantLayout {
    std::vector<std::array<int, 3>> ranges;      // constant register, count, offset
    std::vector<std::array<int, 3>> attributes;  // fetch, location, integer
    std::vector<std::array<int, 2>> samplers;    // slot, kind
    int block_bytes = 0, bools = 0xFFFF;
  };
  std::string CheckLayout(int variant, const VariantLayout& layout) const;
  // Reloaded shaders the driver refused since the last call.
  std::vector<std::string> TakeOverrideErrors();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string folder_;
  std::unique_ptr<rdr_adapter::CaptureReader> capture_;
  std::unique_ptr<rdr::Renderer> renderer_;
  rdr_adapter::LoadedFrame frame_;
  std::vector<OpTrace> trace_;
  uint32_t frame_index_ = 0;
  bool frame_bound_ = false;
  int rendered_through_ = -1;  // the last capture op of the selected frame rendered (-1: none)
  bool legacy_prefix_ = false;
  bool dirty_ = false;         // the renderer's surfaces no longer match "rendered_through_" (a reload): reset before rendering
  bool ReplayTo(uint32_t f, std::string& error);
  void FeedCrowd(uint32_t f);  // S101 lane 35: the scene record for frame f to the crowd hook
  bool BindFrame(std::string& error);
  bool ResetFrame(std::string& error);
};

}  // namespace fview
