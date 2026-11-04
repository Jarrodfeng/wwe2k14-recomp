// wwe2k14 - the game exe's own copy of the D3D layer's inline-draw record types (Session 87, F1 integrator). The SDK's
// include\rex\graphics\d3d_inline_draw.h is the other copy; the exe never includes it (a changed SDK header never forces an
// exe / SDK re-pin: the exe is built against the pinned SDK install). The two are tied by the token's layout, which the
// SDK reports as the int capability cvar gpu_d3d_inline_abi (the hooks take inline draws only when it is present).
#pragma once

#include <cstdint>
#include <vector>

namespace wwe2k14::d3d_types {

// The SDK's int capability cvar gpu_d3d_inline_abi must equal this (bump it here AND in the SDK's d3d_inline_draw.cpp when the
// token layout changes); the hooks compare the value, not just the cvar's presence.
inline constexpr int32_t kInlineAbi = 1;
inline constexpr uint32_t kInlineTokenOpcode = 0x7E;  // F2's copy token is 0x7D, F1's indexed-draw token 0x7C
inline constexpr uint32_t kInlineTokenMagic = 0xD3D30001u;

struct RegisterRun {
  uint32_t first_register = 0;
  uint32_t count = 0;
  uint32_t value_offset = 0;  // into InlineDrawRecord::values
};

struct ShaderBind {
  uint32_t guest_address = 0;  // the IM_LOAD address form (physical), 0 = inline_words
  uint32_t dword_count = 0;
  std::vector<uint32_t> inline_words;
  bool present() const { return dword_count != 0; }
};

enum InlineFlags : uint32_t {
  kInlineTiled = 1u << 0,
  kInlineHasPixelShader = 1u << 1,
  kInlineHasVertexShader = 1u << 2,
  kInlineUseGuestVertexMemory = 1u << 3,
};

struct InlineDrawRecord {
  uint32_t sequence = 0;
  uint32_t flags = 0;
  uint32_t primitive_type = 0;
  uint32_t vertex_count = 0;
  uint32_t stride_bytes = 0;
  uint32_t viz_query_word = 0;
  std::vector<RegisterRun> runs;
  std::vector<uint32_t> values;
  ShaderBind vertex_shader;
  ShaderBind pixel_shader;
  uint32_t slot31_words[4] = {};
  std::vector<uint8_t> vertices;
  uint32_t guest_vertex_address = 0;
};

}  // namespace wwe2k14::d3d_types
