// The two hooks of PictureSource's lifetime events (Session 103, landing 2, piece PN3; docs\renderer\PICTURES_NO_STREAM.md 1, 5.2). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Included ONCE (wwe2k14_app.h): REX_HOOK_RAW defines the function override. Read only, dev only (picture_tap.h): off = one relaxed load, then the original, nothing else.
//   sub_822951F0(r3 = the out pair {key, cache}, r4 = the texture wrapper): the game's texture HANDLE BIRTH [C listing 0x822951F0-0x822952A4; 45 call sites, tools\scene\picture_creators_lint.py].
//   sub_82BAF210(r3 = the registry manager, r4 -> the key word): the registry's SLOT FREE [C listing 0x82BAF210-0x82BAF2E8: id = [r4], slot = id & 0xFFFF, entries {id, object} from [r3+8], matches only when the stored id
//   equals the key, then stores the next birth counter and returns the object (r3 != 0), else 0]. Every release path calls it (q callers 0x82BAF210: 15 sites in 13 functions, e.g. sub_82294FD0 for one handle,
//   sub_82BAA2E0 for whole texture-set arrays inline) and so does the one data word that points at it, so hooking the slot free is the whole death. A death is reported only for the TEXTURE registry's manager
//   ([0x8392DD9C] == r3) and a matched free. Replaces the hook on sub_82294FD0 (it covered 1 of 13 free paths; tools\scene\picture_creators_lint.py).
#pragma once
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "picture_tap.h"

REX_EXTERN(__imp__sub_822951F0);
REX_EXTERN(__imp__sub_82BAF210);

REX_HOOK_RAW(sub_822951F0) {
  const uint32_t pair = ctx.r3.u32, site = uint32_t(ctx.lr) - 4;   // (the call site of the creator: lr is the return address, one instruction past the `bl`)
  __imp__sub_822951F0(ctx, base);
  wwe2k14::picture_tap::OnBorn(pair, site);
}

REX_HOOK_RAW(sub_82BAF210) {
  const bool on = wwe2k14::picture_tap::Active();
  const uint32_t manager = ctx.r3.u32, key = on ? wwe2k14::picture_tap::KeyOfPtr(ctx.r4.u32) : 0u;   // (read before the original: it stores the next counter into the slot, not the key word, but nothing is assumed)
  __imp__sub_82BAF210(ctx, base);
  if (on) wwe2k14::picture_tap::OnSlotFreed(manager, key, ctx.r3.u32);
}
