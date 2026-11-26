// S111 GUESTVTX (docs\research\s111\GUESTVTX.md): the vertex field rules our two decoders share.
// The game packs every model vertex once at load (sub_82BD4BE0, one call per vertex) from the JBOY mesh record's planar streams into the
// GPU vertex. Our stock loader (jboy.h LoadJboy) decodes the FILE record; the created-superstar path (renderer_scene\guest_model_draw.h
// DecodeVertices) decodes the game's PACKED vertex out of its vertex buffer. The two layouts are not the same bytes (10:11:11 normals, half
// UVs, the skin block by the flag word in the packed one), so each keeps its own field reader; the RULES that turn a field into what our
// effects read are here, once, and both call them.
#pragma once

#include <cstdint>

#include "load_plants.h"   // load_detail::VdecOldPlant (the planted twin; the unit sets it, the game exe never does)

namespace assets::vdecode {

// The colour word as the game's vertex fetch delivers it. w = the word's value: the file's S0 bytes 24..27 read big-endian, which is also
// the GPU vertex dword the packer copied (sub_82BD4BE0 +250..+264) after the fetch's 8-in-32 swap (the guest reader's host-order dword).
// The skin shaders read it as (byte 25, 26, 27, 24) / 255 (x = the first lobe's gain, w = the packed ID-map / wrinkle slot).
// Returns Vertex::color (RGBA8 in memory order: R = byte 25, G = byte 26, B = byte 27, A = byte 24).
inline uint32_t GameColour(uint32_t w) { return ((w >> 16) & 0xFFu) | (w & 0xFF00u) | ((w & 0xFFu) << 16) | (w & 0xFF000000u); }

// The weight our vertex keeps for a skin pair. A 1-pair mesh is the packer's flag 6 branch (sub_82BD4BE0 +1B0..+1D0): it stores the
// pair's slot byte alone and never reads the weight, and the 1-bone draw binds the vertex whole to that slot. So a 1-pair vertex weighs
// 1.0 whatever the file's weight is (a file weight 0 must not drop the pair); 2..4 pairs keep the file's weight.
// pairs = GamePairCount below. VdecOldPlant = the old rule (the file's weight: a weight 0 fell to palette[0]).
inline float GamePairWeight(uint32_t pairs, float w) { return pairs == 1 && !load_detail::VdecOldPlant() ? 1.0f : w; }

// The skin-pair count of a mesh record: the record's +0x5C, the count the game builds the part's vertex flag word from (sub_82BDB460 +D58: the
// skin-pair table entry for it, only when the body has nodes; a body with none gets 0x52, no skin block, and the count is not read) and its packer
// reads S1 by, 8 bytes a pair (sub_82BD4BE0: vertex << 3 for 1 pair .. << 5 for 4). Equal to the S1 stride / 8 on every tiled mesh record of every
// pac on the player's disc (vertex_decode_unit scan, S111 GUESTVTX review). nodes = the body's node count (with none, no skin is read: the S1
// stride / 8 is kept, every pair falls to the loader's no-node fallback as before). VdecOldPlant = the old rule (the S1 stride / 8 always).
inline uint32_t GamePairCount(uint32_t rec_pairs, uint32_t s1_stride, uint32_t nodes) { return nodes == 0 || load_detail::VdecOldPlant() ? s1_stride / 8 : rec_pairs; }

}  // namespace assets::vdecode
