// wwe2k14 - S111 BLOODMOD / BLOODMOD2: the permanent blood MOD's game hooks (docs/research/s111/BLOODMOD.md, BLOODMOD2.md; the store is src/renderer_scene/floor_blood_keep.h). A unit of its own (the
// blood_keep.cpp pattern); both builds, Android too.
// sub_82697888 constructs the game's floor-splat pool (the cDRB component: 30 particles, all free); the scene set-up sub_824EC6A8 is its one caller and the scene exit sub_824EA2D8 destroys the pool.
// After the original has run, the scene count goes up (only while the mod is on): the drawer empties its kept splats when the count moves. Nothing of the game's own changes (r3, the new pool, is left as the
// original returned it).
// sub_8235BFB8 answers the RESTART MATCH confirm (its one caller, the pause answer step sub_8235DC30, case 1): r4 = 0 is YES ([flow+88] = 1, the close; the flow then reaches the restart handler sub_827C22A8),
// 1 is NO (the pause list again). After the original has run, a YES moves the same count: the abandoned match's kept splats go. Nothing of the game's own changes.
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "renderer_scene/floor_blood_switch.h"

REX_EXTERN(__imp__sub_82697888);  // the floor-splat pool's constructor (r3 = the new component)

REX_HOOK_RAW(sub_82697888) {
  __imp__sub_82697888(ctx, base);
  rdr_scene::floor_blood::NotePoolMade();
}

REX_EXTERN(__imp__sub_8235BFB8);  // the pause flow's RESTART MATCH confirm answer (r3 = the pause flow, r4 = the answer: 0 YES, 1 NO)

REX_HOOK_RAW(sub_8235BFB8) {
  const bool yes = ctx.r4.u32 == 0;   // read before the original (it does not keep r4)
  __imp__sub_8235BFB8(ctx, base);
  if (yes) rdr_scene::floor_blood::NoteMatchRestart();
}
