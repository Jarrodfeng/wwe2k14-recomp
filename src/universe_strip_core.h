#pragma once
// S110 UNISTRIP (docs\research\s110\UNISTRIP.md "The fix"): the pure half of the strip of a handed-over seat from a match of the Universe's current card
// that the game's own card builder will NOT rebuild. Std only (unit: tools\seats\handover_unit.cpp). The game side is src\universe_strip.h.
// A card match is 252 bytes: +0 = type, +2 = participant count, +3 = status (2 = result recorded), +4.. = a byte-wise flag bit array (bit n = byte
// +4 + n / 8, mask 1 << n % 8). Ordinary kind: participant k (k < 6) is a 12-byte record at +16 + 12k, its id (u16) first and its byte +8 at +24 + 12k
// (bit 0x40 = "replace me", read by the game's random fill sub_82B90820). Bare-id kinds (types 20..23: 40 ids; the sub_82B79F00 kind: 16 ids) hold bare
// u16 ids at +16 + 2k (sub_82B7A400 +C8..+128).
#include <cstdint>
#include <set>

namespace wwe2k14::universe_strip {

constexpr int kMaxSlots = 6;         // sub_82B7A400 +88..+C4: the ordinary kind copies 12 bytes for k < 6
constexpr int kRumbleSlots = 40;     // types 20..23 (sub_82B7A400 +E4)
constexpr int kBareKindSlots = 16;   // the sub_82B79F00 kind (sub_82B7A400 +E4)
constexpr int kMatches = 10;         // the 10 matches of the current card
constexpr int kBuilderMatches = 7;   // N of the card builder sub_82B6B350: sub_82583608 returns 7; the custom generator's ctor sub_82BA6E38 sets [gen+15412] = 7
constexpr uint8_t kStatusResult = 2;
constexpr uint8_t kReplaceBit = 0x40;

constexpr uint32_t IdOffset(int k) { return 16u + 12u * uint32_t(k); }
constexpr uint32_t FlagByte(int k) { return 24u + 12u * uint32_t(k); }
constexpr uint32_t BareIdOffset(int k) { return 16u + 2u * uint32_t(k); }
// How many participant ids a match of this kind can hold (sub_82B7A400 refuses k at or past it).
constexpr int SlotCap(bool rumble_type, bool bare_kind) { return rumble_type ? kRumbleSlots : bare_kind ? kBareKindSlots : kMaxSlots; }

// true = the game's card builder (mode 1) will not rebuild this match: a SPECIAL match (bit 3; its auto ordinal is -1), or an auto match at an auto ordinal
// >= kBuilderMatches (the builder walks auto ordinals 0..N-1 only, sub_82B6B350 +D8..+148). `auto_ordinal` = the game's sub_82B7B210(&card, i).
inline bool LeftByBuilder(bool special, int auto_ordinal) { return special || auto_ordinal >= kBuilderMatches; }

// true = this slot must be flagged (+0x40) and refilled by the game's replacer.
//   status           u8[m + 3]; a played match is a result, never changed
//   left_by_builder  LeftByBuilder(): a match the game's builder rebuilds is left to the game
//   bare_id_kind     types 20..23 (sub_822BA578) or sub_82B79F00(m): the participants are bare u16 ids; the game has no replacer for them
//   slot, count      the participant index and u8[m + 2]
inline bool StripSlot(uint8_t status, bool left_by_builder, bool bare_id_kind, int slot, int count, uint16_t id, const std::set<int>& gone) {
  if (status == kStatusResult) return false;
  if (!left_by_builder) return false;
  if (bare_id_kind) return false;
  if (slot < 0 || slot >= kMaxSlots || slot >= count) return false;
  return gone.count(int(id)) != 0;
}

// After the game's replacer ran on a slot we flagged: the byte +8 to keep. Replaced (the id changed): the game's own byte (it kept +8 & 63). Not replaced
// (sub_82B8FA88 found nobody and sub_82B90820 wrote nothing, +268): the byte from before the strip, so our 0x40 does not stay behind for the game's
// match-finish fill (sub_82B6CC38 +64 runs sub_82B90820 before the result) to act on after the match was played.
inline uint8_t FlagAfterReplace(uint8_t before_strip, uint8_t now, uint16_t id_before, uint16_t id_now) { return id_now == id_before ? before_strip : now; }

// S110 UNIDIAG (docs\research\s110\UNICARDREAD.md): which of four states the walk of the current card found; the summary line says it.
//   reached  matches for which sub_82B731E0 returned a match (0 = no current card)
//   booked   of those, matches with a participant count > 0 (0 = an empty card: a Universe not entered yet)
//   named    slots naming a seat answered gone
enum class CardState { NoCard, Empty, NoneNamed, Named };
inline CardState Summarise(int reached, int booked, int named) {
  if (reached == 0) return CardState::NoCard;
  if (booked == 0) return CardState::Empty;
  if (named == 0) return CardState::NoneNamed;
  return CardState::Named;
}
// The verdict printed on a per-slot "card before strip" line (same inputs as StripSlot, whose result is `strip`).
inline const char* SlotVerdictText(uint8_t status, bool left_by_builder, bool bare, bool strip) {
  if (status == kStatusResult) return "played (kept)";
  if (bare) return "bare-id kind (kept, no game replacer)";
  if (!left_by_builder) return "left to the game's builder";
  return strip ? "strip" : "kept";
}

}  // namespace wwe2k14::universe_strip
