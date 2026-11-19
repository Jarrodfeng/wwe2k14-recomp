// S103 SPBF (review M1): the one decision "is this crowd figure drawn" of the stream-free crowd (live_crowd.cpp add_member), pure, so a unit test (tools\crowd\crowd_density_test.cpp) can prove that CROWD DENSITY
// applies to the far CARDS as well as the members (the stream binder culls both: binder.h CrowdRank, crowd_cards_core.cpp). The first stream-free version returned from the card branch before the density test.
// No game or SDK includes.
#pragma once

namespace rdr_scene::crowd_density {

enum class Figure { Skipped, Culled, Drawn };   // Skipped: a test switch left this kind out (counted as a skip); Culled: the player's CROWD DENSITY (counted, not a skip); Drawn

// `rank()` is the figure's 0-99 density rank (crowd_rank.h); it is asked only when density < 100 (100 = every figure: no test, no cost) and only for a figure not already skipped.
template <class RankFn>
inline Figure Decide(bool card, bool no_cards, bool no_members, int density, RankFn rank) {
  if (card ? no_cards : no_members) return Figure::Skipped;
  if (density < 100 && int(rank()) >= density) return Figure::Culled;
  return Figure::Drawn;
}

}  // namespace rdr_scene::crowd_density
