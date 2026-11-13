// The crowd's far cards, drawn by us (Session 101, lane CD = C1; docs\renderer\CROWD_CARDS_PROMOTION_MAP.md, CROWD_CARDS.md): the GPU-free half. From a frame's ops
// and the scene record it builds the PLAN of one frame: which record cards are drawn (grouped by person type, in the stream's own order), the picture of each type
// (the copy destination of that type's bake pass, joined to the type by the texture the type's stands blocks bind), the stream card draws that stay the game's
// (lane 82's rule: no hole), and the proof counts (pairing, order, picture). No GPU, game or SDK includes: crowd_cards.cpp turns a Plan into instanced draws.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "../renderer_adapter/capture_reader.h"
#include "record_io.h"

namespace rdr_scene::cards {

inline constexpr uint32_t kTypes = 20;
inline constexpr uint32_t kAmbiguous = 0xFFFFFFFFu;

// Planted faults (each must fail its named check, tools\crowd\crowd_cards_check.py). The drawer's own plants (uv_swapped, copy_swap_lost, no_alpha_test) live in CardsDrawer.
enum class Fault : uint8_t {
  None,
  OnePicture,      // every card bound to the first stream card's picture (lane 35's "one texture for every member")
  TypeShift,       // the picture of type T + 1
  AtlasPicture,    // the 512 x 512 light-map page taken as the picture
  OrderSwap,       // the first two cards of the first run with two or more swapped
  OrderReversed,   // CONTROL: every run reversed (moves at most overlapping alpha-edge texels)
  MissingCard,     // the first record card removed (its stream draw must stay)
  NoCards,         // CONTROL of the fallback rule: the record is read as holding no cards (a stretch of record without them): the whole frame stays the stream's
  DropUnjoined,    // the first card's type is made unjoinable and its cards are DROPPED instead of kept (a hole)
  UnjoinType,      // TWIN of DropUnjoined: the same type made unjoinable, its cards kept the game's (no hole, the picture is the stream's)
  BlobDrift,       // the expected quad differs from the stream's blob by one coordinate (the blob check must refuse the frame)
  StateDrift,      // the last stream card draw is read as having another program (the same-state check must refuse the frame)
  InjectEarly,     // the first card op is read as sitting at the page copy (injection before the light-map page / bake copies: the pass-order check must refuse the frame)
  CardGap,         // lane 223: one op more than the card run holds (a non-card op, e.g. a picture rewrite, read as sitting inside the cards): the contiguity check must refuse the frame
  PositionAll,     // every type joined by its bake's position (the block join skipped): must stay byte-equal
  PositionReversed,// the k-th bake taken as type 19 - k: must fail the picture check
  KeepFirst,       // the first stream card is kept the game's and not drawn (the kept-first attribution case): only alpha edges may move
  // lane 175 (THIN: the game recorded only the first card of each type's loop, card_thin_core.h):
  ThinFirstOfTypeShift,  // the kept card of a type is paired with the SECOND record card of the type (the first-of-type check must refuse the frame: a hole)
  ThinTypeMissing,       // the record is read as holding one more type than the stream kept (the type-presence check must refuse the frame: a hole)
  ThinTrustCount,        // CONTROL of the count rule: the thin branch is entered on a frame whose stream cards number more than the types (must not happen: refused as `not thin`)
};

// The card quad as the vertex blob holds it: 4 vertices of 16 bytes (position R32G32B32_SFLOAT, uv R16G16_SFLOAT), the feet at y 0 and v 1, the head at y -250 and v 0.
void CardQuad(uint8_t out[64]);

struct Instance {
  float c[12];  // the card's world in the c76-c78 form: row r = c[4 r .. 4 r + 3] (the record's Mat4 transposed, translation in the 4th column)
};

struct Run {  // one instanced draw: the cards of one person type, in the stream's order
  uint32_t type = 0;
  uint32_t picture = 0;           // the capture image id bound as slot 0
  uint32_t first = 0, count = 0;  // into Plan::instances
};

struct Input {
  const rdr_adapter::LoadedFrame* frame = nullptr;
  const rdr_scene::SceneTables* record = nullptr;
  const std::unordered_set<uint64_t>* bake_vs_hashes = nullptr;  // vertex program hashes of the bake's vertex effect (yAudDefault skinned_lit_lm_vertex)
  int density = 100;                                              // g_crowd_density: the percent of members drawn
  int (*rank)(uint32_t w3, uint32_t w7, uint32_t w11) = nullptr;  // binder.h CrowdRank
  Fault fault = Fault::None;
  uint32_t page_slot = 13;   // lane 219: where the stream's first card draw reads the light-map page: 13 (lm_uv) or 16 (lm_vertex), from its vertex program
  bool enforce = true;   // false (test only, fview --crowd-cards-unchecked): a bad frame is drawn anyway and a mismatching card is not kept, so a planted fault shows in the picture
  uint64_t thin_world = 0;                   // lane 175: the SUM of the 12-word hashes of every card op-21 the game recorded this frame (card_thin_core.h CountRing)
  uint32_t thin_seen = 0, thin_dropped = 0;  // lane 175 (review B1): the hook's tally for this frame: card op-15 calls the game made, and how many it dropped. dropped > 0 = the frame is THIN
  bool thin = false;     // lane 175: a frame whose stream holds only the first card of each type's loop may be drawn from the record (card_thin_core.h); false: such a frame is refused
  bool verify = true;    // this frame's whole-frame proof acts (offline every frame; live every Nth): a bad frame is NOT drawn
  std::function<bool(uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out)> read_geometry;  // the capture's / live store's geometry bytes (the blob and index checks)
};

struct Plan {
  bool usable = false;               // false: nothing is drawn, every stream card draw stays (see `why`)
  const char* why = "";              // the reason for !usable (counted by the caller)
  std::vector<Instance> instances;   // type-major
  std::vector<Run> runs;             // ascending type
  std::unordered_set<uint32_t> keep; // stream card draw indices that stay the game's
  uint32_t light_map_image = 0;      // the first stream card's light-map page: slot 13 (lm_uv) or vertex slot 16 (lm_vertex), by Input::page_slot
  // Counts of this frame (the caller sums them):
  uint32_t record_cards = 0;         // visible card objects of the record
  uint64_t record_world_sum = 0;     // lane 175 (SF2): the sum of the record cards' 12-word hashes (what the hook's tally `world` must equal)
  uint32_t stream_cards = 0;         // the frame's draws at the card site
  uint32_t drawn = 0;                // instances
  uint32_t density_skipped = 0;      // cards left out by CROWD DENSITY (their stream draws are dropped)
  uint32_t unaccounted = 0;          // stream card draws no record card pairs with (kept)
  uint32_t unpaired_record = 0;      // record cards no stream draw pairs with (not drawn)
  uint32_t joined_by_position = 0;   // types joined by their bake's position (no stands block of the type this frame)
  uint32_t joined_by_stream = 0;     // lane 223: types whose picture is the one the stream's own cards show (not joined through a bake: any frame)
  uint32_t bake_count = 0;           // lane 223: bakes (64 x 64 pass + copy) found in the frame
  bool bakes_scanned = false;        // lane 223: the bake scan ran (the frame had cards and a record): only then is bake_count a cadence
  uint32_t unjoined_types = 0;       // types with cards whose picture could not be joined (their cards are kept)
  uint32_t unjoined_cards = 0;
  uint32_t picture_mismatch = 0;     // paired cards whose stream slot-0 image is not the picture we would bind
  uint32_t inversions = 0;           // adjacent stream cards whose instance order descends (our drawn order against the stream's)
  uint32_t record_order_differs = 0; // types whose cards the record lists in another order than the stream draws them (census only: we sort by the stream)
  bool thin = false;                 // lane 175: this frame was drawn THIN (the stream held only the first card of each type's loop)
  bool thin_hole = false;            // lane 175: a thin frame ours could not draw (its dropped cards are gone and nothing replaces them): counted, required 0
  const char* thin_why = "";         // lane 175: why a thin frame was refused
  uint32_t thin_dropped = 0;         // lane 175: record cards without a stream draw in a thin frame (the game's dropped op-15 records, counted from the record)
  const char* state_why = "";        // bad_state: which same-state / blob / contiguity / pass-order check failed
  uint32_t holes = 0;                // stream card draws neither drawn, kept nor density-dropped (must be 0)
  bool bad_pairing = false, bad_order = false, bad_picture = false, bad_state = false;  // the whole-frame proofs (verify frames)
};

// Builds the plan of one frame. Pure: reads the frame's draws / ops / constants and the record; touches nothing else.
void BuildPlan(const Input& in, Plan& plan);

// lane 270: the phase table of BuildPlan (env RDR_CROWD_PHASES=1; empty otherwise)
std::string PhaseReport();

// A record object's card type (the asset entry "card:TT"), -1 when it is not a card.
int CardType(const rdr_scene::SceneTables& t, const rdr_scene::Object& o);

}  // namespace rdr_scene::cards
