// Wrestlers through the scene front-end, from the live record alone (Session 103, lane B; docs\renderer\FRAME_CONTRACT.md 7,
// docs\agents\s103\briefs\B.md). The applier of Family::Wrestlers (record_frame.h ClassApplier): turns the record's wrestler objects
// (what src\scene_live\wrestler_contrib.cpp writes: "actor:slot:N" / "skel:XXXXXXXX", the asset ref pac\ch\chNNN.pac + the attire as
// `variant` (entry "model_key:NNNNN"), the palette in Object::bones_*, the light set id, the mesh mask, the tint) into front-end objects.
// No game, no SDK, no viewer: records, the front-end and the ModelCache only. Nothing here reads the stream.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "record_frame.h"

namespace rdr_scene::rf {

// The applier (the row in record_frame_classes.cpp is `{"wrestlers", Family::Wrestlers, ApplyWrestlers}`).
bool ApplyWrestlers(BuildCtx& ctx);

// Is this record object a wrestler-class object (a character skeleton the wrestler contributor wrote)? By id prefix, as FamilyOf does.
bool IsWrestlerObject(const SceneTables& t, const Object& o);

// What the last ApplyWrestlers call did, for the rig's report and the census (never silent about what stays at the file's value).
struct WrestlerStats {
  uint32_t key_sets = 0, key_sets_x = 0;   // S103 L7: record light sets with a key light / of those with |hl_dir.x| > 1e-3 (the right-axis sign of KeyLightWorld is only testable by those)
  uint32_t objects = 0;           // actor / skel objects in the record
  uint32_t drawn = 0;             // front-end objects made
  uint32_t pieces_guest_drawn = 0; // S103 DLW: own-skeleton pieces the owner cannot draw, drawn from their own guest rows (not holes)
  uint32_t skipped_invisible = 0; // no character slot (flags without kObjVisible): the game does not show them either
  uint32_t skipped_unnamed = 0;   // the record does not name the model (pac 0): lane A's link is missing for that skeleton
  uint32_t skipped_palette_mismatch = 0;  // the record's palette size differs from the loaded model's node count (a wrong link: refused, never drawn in the wrong pose; review B1)
  uint32_t pieces_no_owner_mesh = 0;      // S103 PZF2 (EB): linked pieces refused at draw because the owner's resident model has no mesh of that piece id (or the record does not name it): holes
  uint32_t pieces_detached = 0;           // S103 DTP: pieces the game detached from their owner (copy flag 0, kObjDetachedPiece): the owner mesh skips them (alpha 0 row) and they are drawn from their own guest rows; not an eyelid fade
  uint32_t pieces_by_owner = 0;         // S103 NRBF: of the own-skeleton pieces, the linked ones (kObjDrawnByOwner) whose owner body draws them and that the game drew this frame: not holes, no model of their own
  uint32_t pieces_alpha_carried = 0;      // S103 EYF: pieces drawn by their owner whose [model+252] row (MdlAlpha) was carried to the owner
  uint32_t pieces_alpha_missing = 0;      // S103 EYF: ... with no readable row: carried as alpha 0 (not drawn; fail closed)
  uint32_t pieces_unobserved = 0;        // S103 NRBF (F1): of pieces_by_owner, the ones on a frame with no walked evidence (OURS): hiding cannot be seen there; counted apart
  uint32_t pieces_hidden_owner_drawn = 0; // S103 NRBF (review F5): linked pieces the game did NOT draw this frame (no light ref at the stamp) while the owner body is drawn: the owner body would show them: HOLES (fail closed)
  uint32_t pieces_game_hidden = 0;        // S111 SOCKPIECE (piece_hidden.h): pieces the game hides (SUBM instance +20 == 0, kObjPieceHidden): an alpha-0 row on the owner (its merged mesh skips them), not drawn, not holes
  uint32_t own_rows_refused = 0;          // S111 PIECELINK: created superstars' SUBM pieces with no fresh latch palette of this frame (kObjOwnRowsRefused, owner link carried): not drawn, counted, never holes (also in skel_not_hole)
  uint32_t game_undrawn = 0;              // S111 PIECEHOLES: invisible slot bodies the game did not ask to draw this period (kObjGameUndrawn) on a frame with no walk: not drawn by the game, not holes (also in skel_not_hole)
  uint32_t pieces_only_owners = 0;        // S111 PIECEROUTE: slot bodies the game did not draw whose slot 22 drew their SUBM pieces (kObjPiecesOnly): drawn as their piece meshes only (also in drawn)
  uint32_t own_skeleton_pieces = 0;       // invisible skel: objects with a palette (own-skeleton pieces: hair, caps, glasses): not drawn yet, counted (review B6)
  uint32_t skel_not_hole = 0;             // S103 NR (A): of those, the ones that are NOT holes (the game did not draw them this frame, or the ring family draws them)
  uint32_t skipped_not_resident = 0;  // the ModelCache could not give the model (live: not loaded yet; offline: the load failed)
  uint32_t hair_pieces_absent = 0; // S103 W2B (review I): (body, piece) pairs this call whose spring-hair rows the record could not supply (meshes skipped, body drawn; ClassReport::absent_pieces)
  uint32_t no_palette = 0;        // drawn in the bind pose (no palette in the record)
  uint32_t light_by_id = 0;       // the object's light set id was found among the record's lights
  uint32_t light_state_missing = 0, light_state_rejected = 0;   // S103 E8: states registered but this wrestler has none (a hole) / a state that failed validation (the old set kept)
  uint32_t light_by_state = 0;    // S103 E8: lit by the light state the wrestler's model call read (the game's own constants)
  uint32_t light_default = 0;     // light set unknown or not in the record: the default set (LISTED, see default_set)
  int default_set = -1;           // the id used for light_default
  uint32_t mesh_masked = 0;       // objects whose record mask hides at least one mesh
  uint32_t body_scaled = 0;       // S103 W2: objects whose palette took the body scale (body_scale.h)
  uint32_t skipped_body_scale = 0;  // S103 W2: skipped for an unknown body scale (a hole)
  uint32_t body_scale_unknown = 0;  // S103 W2: no scale measured for the object yet (the palette stays unscaled)
  std::string first_skip_why;
  // The runtime values the record does not carry yet: they stay at the file's value, and are named here every call.
  std::string at_file_value;
};
const WrestlerStats& LastWrestlerStats();

// S111 SOCKSHADOW: the owner-mesh rows of ApplyWrestlers's pre-pass (an owner's Object is built later, before or after its pieces, so each row waits for it): per piece object of the record,
// its owner slot, piece id, alpha and `undrawn` (SceneFrame::PieceAlpha: true for a piece the game hides, piece_hidden.h, or detached, detached_piece.h; false for an EYF model-alpha row,
// piece_alpha.h). Honours Plants() (ignore_game_hide, ignore_detached_flag, piece_alpha_one) and counts into `st` (pieces_game_hidden, pieces_detached, pieces_alpha_carried / _missing).
// tools\scene\piece_cast_unit.cpp drives it with a synthetic record and plants the two ignore twins.
struct OwnerPieceRow { unsigned slot = 0, piece = 0; float alpha = 0.f; bool undrawn = false; };
void OwnerPieceRows(const SceneTables& t, std::vector<OwnerPieceRow>& out, WrestlerStats& st);
// SOCKSHADOW review: the attach (ApplyWrestlers, at the owner's main-loop step): the rows of owner slot `owner_slot` onto frame object `object` of `f` (SceneFrame::piece_alphas), `undrawn` with them,
// so the casters (LiveMapsHook::CasterMeshCasts) see it. tools\scene\piece_cast_unit.cpp drives it, so a row that loses `undrawn` on the way to the frame fails its caster check.
void AttachOwnerPieceRows(const std::vector<OwnerPieceRow>& rows, unsigned owner_slot, uint32_t object, SceneFrame& f);
std::string WrestlerStatsText();  // one line: "[rf][wrestlers] objects N drawn D skipped ...; at the file's value: ..."

// Planted faults (offline checks only: the game never sets these; the gate's twins do).
struct WrestlerPlants {
  bool no_palette = false;      // drop the palette (bind pose)
  bool ignore_light = false;    // ignore the light set id (the default rig)
  bool ignore_mask = false;     // ignore the mesh mask (every mesh shown)
  bool light_states_expected = false;   // S103 E8: the caller says light states are registered (bit 4 stamped): a visible wrestler with no state is a hole even when the record has none at all
  bool no_body_scale = false;   // S103 W2 twin: the old behaviour (the palette without the body scale)
  bool guest_flat = false;      // S103 CAWD debug plant: a guest part with no diffuse picture gets a flat magenta material colour (drawn-black vs not-drawn in one shot). Set by fview / the rig, or live by --wwe2k14_rdr_guest_flat (+dev builds only)
  bool piece_alpha_one = false; // S103 EYF twin: every piece at alpha 1 (the black eyes bug); set by eyes_unit / a dev plant, never by the game
  bool guest_piece_no_alpha = false;   // S103 SMF twin: a guest-routed piece at alpha 1 (the black eyes bug on CAW / DLC bodies); set by a dev plant, never by the game
  bool ignore_detached_flag = false;   // S103 DTP twin: kObjDetachedPiece is ignored (the owner's mesh keeps drawing a piece the game detached: the pad stays on the arm); set by wrestler_emit's selftest, never by the game
  bool refuse_body_on_absent_piece = false;   // S103 W2B twin: one piece the record cannot prove (no rows) refuses the whole body (the rule W2_ADV R2 rejected); set by w2b / wrestler_emit units, never by the game
  bool old_hole_rule = false; // S103 NR twin (review F2): every unlinked own-skeleton piece is a hole (the rule before NR); set by nr_unit, never by the game
  bool warm_stops_at_miss = false;   // S110 ENTRBLACK round 6 twin: a warm build stops at the first class that misses (the rule before round 5: nothing after the arena is asked); set by nr_unit, never by the game
  bool game_undrawn_is_hole = false;   // S111 PIECEHOLES twin: kObjGameUndrawn is ignored (a body the game did not draw is counted a hole again, the rule before); set by nr_unit, never by the game
  bool ignore_game_hide = false;   // S111 SOCKPIECE twin: kObjPieceHidden is ignored (the owner's merged mesh draws a piece the game hides: the sock at the entrance); never set by the game
};
WrestlerPlants& Plants();

}  // namespace rdr_scene::rf
