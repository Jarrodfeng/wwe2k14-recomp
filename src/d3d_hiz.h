// wwe2k14 - the library's HiZ helper 0x82CBD098 as the Clear (0x82CB4560, call at 0x82CB468C) and the Resolve core
// (0x82CAEA30, call at 0x82CAF50C) use it: r3 device, r4 = 0x100, r5 = the HiZ word (device+10560 with bit 0 / bit 2 / bit 5
// changed by the caller). Read in Session 87 (listing 0x82CBD098-0x82CBD424). Shared header: F1's draw flush calls the same
// helper with other bits in r4 (the main dirty mask) - that use is NOT covered here, see "Draw flush" below.
//
// With r4 = 0x100 (bit 23 of the 64-bit mask, nothing else), what the helper does:
//   - Reserves ring room (kick if near the end: 0x82CBD0BC-0x82CBD0C4), and returns r4 with 0x100 cleared in r3 (0x82CBD2FC);
//     both callers discard the result.
//   - Plain case (device byte 11071 bit 0x20 clear, 0x82CBD0FC-0x82CBD104): writes ONE register, RB_HIZCONTROL (0x2203), with
//     r5, bit 0 cleared when device+12284 & 0x000E0000 is 0 (0x82CBD2C4-0x82CBD2E4). Then, only if that cleared bit 0 AND device
//     byte 11068 bit 0x40 is set, a few more packets (a SET_BIN_MASK_HI-style type-3 and the tile fields at +13084 / +13088,
//     0x82CBD2EC-0x82CBD2F8 and 0x82CBD378-0x82CBD410).
//   - Tiling case (byte 11071 bit 0x20 set): loops over the tile list (+13124, +13468..) writing predicated HiZ words and
//     bin-mask packets (0x82CBD108-0x82CBD2C0).
//   - It stores NOTHING in the device except the ring write pointer (+48): no main / fetch / misc mask, no +10560, no flag byte
//     (the only 'std' / 'stb' in the function are the pointer at 0x82CBD414). So on the plain path dropping the call loses only
//     the RB_HIZCONTROL register write.
//   - The SDK never reads RB_HIZCONTROL (grep over src / include of s87-d3d: only the register table names it), so the dropped
//     register image needs no record field: it is dropped, with this reason.
//
// Draw flush (F1's 'prepass'): there r4 is the main dirty mask, the helper clears bits of it (the returned r3 is stored back into
// device+16) and may write the 0x2000 block's colour info and RB_COLOR_MASK - a different, larger contract; not mirrored here.
#pragma once

#include <cstdint>

namespace wwe2k14::d3d_hiz {

// True when the helper's call with r4 = 0x100 is the plain case above and its only effect is the dropped RB_HIZCONTROL write:
// byte 11071 bit 0x20 clear (no tiling HiZ list) and byte 11068 bit 0x40 clear (no bin-mask extras). False: the caller runs
// the original function (counted as its HiZ reason).
inline bool PlainCall(const uint8_t* base, uint32_t dev) { return !(base[dev + 11071] & 0x20) && !(base[dev + 11068] & 0x40); }

}  // namespace wwe2k14::d3d_hiz
