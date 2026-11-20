// Quads2DPlants (Session 103, lane I1): the planted twins of the 2D record's unit proof (tools\scene\i1_unit.cpp, `--plant NAME`). Present ONLY in a build with -DRDR_Q2D_PLANTS (the unit test's): in every
// other build Q2D_PLANT(n) is the constant false and the branches it guards are removed by the compiler. Each plant breaks one production function in one way; the unit check named for it must FAIL.
#pragma once

#ifdef RDR_Q2D_PLANTS
namespace rdr_scene::q2d {
inline int g_plant = 0;
}
#define Q2D_PLANT(n) (::rdr_scene::q2d::g_plant == (n))
#else
#define Q2D_PLANT(n) false
#endif
// 1 tap_no_colour      OnOp21 never reads c1                          -> [TAP CONSTANTS]
// 2 tap_vertex_order   OnOp19 exchanges corners 1 and 2               -> [TAP VERTICES]
// 3 tap_ticket_reuse   the call-order ticket repeats                  -> [TAP ORDER]
// 4 tap_no_flush       a movie row misses its flush stamp             -> [TAP FLUSH]
// 5 drain_unsorted     Drain does not sort by seq                     -> [TAP ORDER]
// 6 blocks_no_half     BlocksOf drops the half unit (w = 0)           -> [BLOCKS EXACT]
// 7 blocks_blend       BlendOf maps alpha blend to additive           -> [DRAW STATE]
// 8 draw_reversed      Append draws the rows last to first            -> [DRAW ORDER]
// 9 parse_seq          Quads2DProblem does not check ascending seq    -> [RECORD SEQ]
// 10 tap_late_colour   a quad takes the constants when its scope closes, not at its op-19 -> [TAP CONSTANTS] (two quads, two colours)
// 11 sprite_no_hsl     BlocksOf drops a sprite row's hue-shift constant (the plain program draws) -> [SPRITE HSL]
// 12 pic_key_shift     the picture table answers the NEXT key's picture                           -> [PICTURE KEY]
// 13 glyph_no_selector the shadow never reads c4 (a glyph row has no channel selector)          -> [GLYPH TAP]
// 14 glyph_stale_regs  a glyph row takes c0 / c1 / c28 from its scope only, not from the thread's register file -> [GLYPH TAP]
// 15 glyph_key_off     a glyph row reads its sheet's key at object + 400 (the sprite's offset), not + 416 -> [GLYPH TAP]
// 16 glyph_blocks_sel  BlocksOf writes a zero channel selector into a glyph row's fragment block      -> [GLYPH BLOCKS]
// 17 glyph_prim_any    BlocksOf draws a glyph row whatever its primitive word (the check for strip / quad list is gone) -> [GLYPH BLOCKS]
// 18 glyph_stamp_ignored the register shadow's frame stamp is not checked (an old frame's c0 makes a complete row) -> [GLYPH STALE]
// 19 hole_dropped       NoteHole records nothing (a draw seen but not recorded leaves no per-frame trace)        -> [GLYPH HOLES]
// (pic_no_swap is guest_tex::g_tex_plant 3, the guest reader's own plant: the endian swap is skipped -> [PICTURE GUEST])
// 25 row_unscoped_ok    IsHoleSite excludes Row sites again (an unscoped Row-site draw makes no row and no hole)                 -> [HOLE SITES]   (l2a_unit --plant row_unscoped_ok)
// 26 pair_target_ignored ScanStream does not check a row-site draw's colour target                                              -> [PAIR SCAN]
// 27 pair_interleave_ignored ScanStream does not refuse a kept op between row-site draws                                         -> [PAIR SCAN]
// 28 pair_count_ignored ScanStream does not compare the row-site draw count with the rows                                         -> [PAIR SCAN]
// 29 pair_blend_ignored ScanStream does not compare row i's blend with the i-th row-site draw's                                   -> [PAIR SCAN]
// 30 m_c4_ignored       a glyph-kind scope does not note the channel selector upload (a selector-carrying 0x82BC8BEC draw becomes a plain sprite row)         -> [P3B M SITE]
// 31 m_variant_ignored  a glyph-kind scope does not note the c2 / c3 pixel-constant uploads (a hue-shifted / blurred 0x82BC8BEC draw becomes a plain sprite row) -> [P3B M SITE]
// 33 proj_not_taken      the tap does not read the matrix block c29..c32 (scope or register file): a matrix-placed row has no matrix and is refused (a hole)          -> [P3C TAP]
// 36 matrix_partial_kept an upload that overlaps c29..c32 without being the whole block keeps the old matrix (scope and register file)                                -> [P3C TAP]   (review F1)
// 37 glyph_matrix_always a glyph row takes the register file's matrix even when its c28 = 0                                                                              -> [P3C TAP]   (review F3)
// 34 proj_transposed     BlocksOf puts the matrix in the vertex block transposed (rows for columns)                                                                  -> [P3C BLOCKS]   (the rig fault of the same name does it on the row: the picture check)
// 35 proj_c28_ignored    BlocksOf places a matrix-placed row by the pixel-unit rule (c28 not looked at)                                                              -> [P3C BLOCKS]
// 32 strip_order_ignored BlocksOf takes a strip's (prim 6) vertices in the draw's order, not the perimeter order (rect + sprite)                                -> [P3B BLOCKS]
// 38 scissor_not_filled  FillScissor leaves the row without scissor state (no kQhScissor: Quad2DRowProblem refuses it, a hole)                                               -> [SC TAP]   (sc_unit)
// 39 scissor_enable_ignored FillScissor takes the scissor as OFF whatever the state object's +124 says (a menu list unclipped)                                                -> [SC TAP]
// 40 scissor_y_flipped   FillScissor measures the box from the bottom edge (720 - bottom)                                                                                  -> [SC TAP]
// 41 scissor_ignored     ScissorOf (the drawer) ignores the row's scissor: every row is drawn over the whole target                                                         -> [SC DRAW]
// 42 scissor_y_flipped_draw ScissorOf measures the box from the bottom edge                                                                                              -> [SC DRAW]
// 43 target_not_filled   FillTarget leaves the row without target / viewport state (no kQhTarget: a hole)                                                                    -> [SC TARGET]
// 44 target_always_frame FillTarget reads every target as the 1280x720 frame (an offscreen page row is drawn on the frame)                                                   -> [SC TARGET]
// 45 rt_holder_direct    OnSetRenderTarget reads the surface header at r5 + 36 (r5 taken as the surface, not the slot HOLDING it: the pre-review bug)                      -> [SC RT HOLDER]   (sc_unit; review F1)
// 46 reset_ignored       the reset recorders (op 47, op 48) and the SetRenderTarget slot-0 scissor invalidation do nothing (the thread's scissor / target stay as before)  -> [SC RESET]   (review F2)
// 47 final_by_size       FillTarget calls a 1280x720 surface the frame whatever it is (judged by size, not by identity with the default back buffer)                       -> [SC TARGET]   (review F3)
// 48 scissor_viewport_ignored ScissorOf does not intersect the box with the row's viewport                                                                               -> [SC DRAW]   (review F4)
// 49 scissor_root_fixed  ScissorOf scales by a fixed 1280x720 root, not by the row's target size                                                                            -> [SC DRAW]   (review F4)
// 50 check_empty_ignored Check does not count a zero-area scissor row as skipped (Append does)                                                                               -> [SC DRAW]   (review F5)
// 51 cross_target_ignored  the dev cross-check (CrossCheckStream) does not compare a row's target class with the stream draw's target                                          -> [SC CROSS]   (sc_unit)
// 53 frame_reset_skipped OnFrameBoundary does not reset the thread's target / scissor at a new guest frame (the match's sticky pass_offscreen, a fresh thread's 100 % unknown)          -> [SC FRAME]   (sc_unit; PFR)
// 54 save_ignored        the op-46 save table is neither written nor consulted (an op-45 restore reads the guest's empty / stale holder: the null-holder hole)                            -> [SC SAVE]   (sc_unit; PFR)
// 52 seed_off            SeedTargetIfUnseen does nothing (a thread that never ran op 45 / 47 keeps an unknown target: every boot / title / menu row is a hole)   -> [SC SEED]   (sc_unit; PBS)
// 60 scene_z_ignored    Append with Context::scene_depth takes no depth state from the row (test / write off, z not flipped)                                          -> [SCS DEPTH]
// 65 rectcolor_leaf_hole  T_RectColor (0x82BC4404) is a Leaf2D hole again (the old table: the 30 Years narration caption's dark box never draws)  -> [A8 SITE] / [A8 TAP]   (a8_unit; the S105 A8 past-bug re-find)
// 66 vcol_alpha_dropped   BlocksOf makes every corner of a T_RectColor row opaque (the 30 / 180 alpha gradient is lost)  -> [A8 BLOCKS]
// 67 vcol_prim_any      BlocksOf / Quad2DRowProblem accept a T_RectColor row whatever its primitive (the strip too: laid out as a quad list)  -> [A8 PRIM]   (a8_unit; S105 A8 review F1)
// 64 yuv_site_hole      the no-alpha movie sprite site 0x82BC799C is a Leaf2D hole again (the 30 Years season video draws black)  -> [SVY ROW]   (rfx_unit; the S103 SVY past-bug re-find)
// 61 address_ignored    AddressModes ignores the row's address code (today's forced clamp: the film-grain flipbook smears)  -> [RFX TILE]   (rfx_unit; the S103 RFX past-bug re-find)
// 62 code7_wrap         AddressModes does not remap the game's code 7 to clamp (7 tiles)  -> [RFX CODE7]
// 63 unknown_wrap      AddressModes tiles code 3 / an unknown code instead of clamping  -> [RFX UNKNOWN]
