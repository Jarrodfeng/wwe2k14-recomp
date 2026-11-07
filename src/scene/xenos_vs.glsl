// Session 87 (stage 4, gpu_native_vs_swap; D3D_LAYER_PLAN.md "the four stages"): the guest-facing half of our own
// vertex shaders - what the SDK's vertex swap (vulkan/native/shader_swap.h BuildVertex) hands a replacement: the shared
// memory the vertex data is read from, the translator's system constants (set 1 binding 0; only the members used are
// declared), the fetch constants (binding 4), and the clip-space step the translation does last. Our own code, following
// the SDK translator's vertex prologue (the vertex index, the fetch address, the byte order) and epilogue (the position).
// Include before the shader's own constants block (set 1 binding 1, packed in register order for the registers the
// shader was written for) and after nothing else.

// Session 93 (the per-draw storage window): the shared memory is ONE storage buffer binding on every GPU. On a GPU whose
// maxStorageBufferRange is below 512 MiB the SDK binds a WINDOW of it per draw (vulkan_shared_memory_window) and passes the
// window's first dword in the system constants (offset 512, the translator's shared_memory_window_base_dwords; 0 whenever the
// window mode is off); every address is shifted by it here, as the translator's LoadUint32FromSharedMemory does. An address
// outside the window reads 0 (an explicit range check). One compile serves every GPU (Session 92's split variants _s2 / _s4 are gone).
layout(set = 0, binding = 0, std430) readonly buffer XeSharedMemory { uint shared_memory[]; };
layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;                      // bit 8: x, y divided by w; bit 9: z divided by w; bit 10: w is not a reciprocal
  layout(offset = 8) uint vertex_index_endian;        // the host index buffer's byte order (xenos::Endian)
  layout(offset = 12) uint line_loop_closing_index;
  layout(offset = 16) int vertex_base_index;
  layout(offset = 128) float ndc_scale_x;
  layout(offset = 132) float ndc_scale_y;
  layout(offset = 136) float ndc_scale_z;
  layout(offset = 144) float ndc_offset_x;
  layout(offset = 148) float ndc_offset_y;
  layout(offset = 152) float ndc_offset_z;
  layout(offset = 192) uint vertex_texture_signs;      // texture_swizzled_signs[1].x: fetch constant 16's four channels, 2 bits each (xenos_vs_tex.glsl)
  layout(offset = 496) uint vertex_index_min;
  layout(offset = 500) uint vertex_index_max;
  layout(offset = 512) uint shared_memory_window_base_dwords;  // Session 93: the shared memory window's first dword (0 unless the host windows the memory)
} xe_system;

uint XeSharedMemoryLoad(int address) {  // an address outside the (window of the) buffer reads 0: robustBufferAccess v1 alone does not promise that
  uint r = uint(address) - xe_system.shared_memory_window_base_dwords;
  return r < uint(shared_memory.length()) ? shared_memory[r] : 0u;
}

layout(set = 1, binding = 4, std140) uniform XeFetchConstants { uvec4 fetch_constants[48]; } xe_fetch;

// As the translation's module: the position and the interpolators are invariant (the same expression gives the same bits in
// every shader, so a mesh drawn by this and by the game's own shader never cracks).
// The per-vertex output block is {gl_Position} only, exactly as the translation declares it for every draw BuildVertex accepts (the
// translator adds ClipDistance / CullDistance members only for user clip planes or a vertex kill, which BuildVertex refuses), so a
// geometry stage the pipeline puts after the vertex shader (rectangle lists, point expansion) sees the block it expects.
out gl_PerVertex { vec4 gl_Position; };
invariant gl_Position;

// The console's big-endian dwords as the host reads them from the shared memory: every vertex fetch constant is 8-in-32
// here (the translation's baked endian modes, which BuildVertex requires), i.e. all four bytes reversed.
uint XeSwap8in32(uint v) {
  v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  return (v >> 16) | (v << 16);
}

// The guest vertex index the draw is on: the host index (a closing index of a line loop reading vertex 0), put in the
// guest's byte order, plus the draw's base index, clamped to the draw's range.
#if defined(XE_SKIN_ONCE)
// Session 95 (gpu_native_skin_once): the SDK binds the skinned rows so that gl_VertexIndex is the row itself, and takes this variant only when the draw needs no clamp, byte order or base.
uint XeGuestVertexIndex() { return uint(gl_VertexIndex); }
#elif defined(XE_VERTEX_INPUT)
// Session 95 (vertex input, the _vi variant: tools/shaders/build_vertex_input_vs.py): the vertex comes from the vertex buffer the SDK binds at the
// fetch constant's address (the console's own bytes: no host copy), and the draw's index is gl_VertexIndex as it is, the guest index itself (the SDK
// takes this variant only for an index range that needs no clamp, byte order or base: vertex_index_min..max, vertex_base_index and the endian are not read).
uint XeGuestVertexIndex() { return uint(gl_VertexIndex); }
#else
uint XeGuestVertexIndex() {
  uint v = uint(gl_VertexIndex);
  if (v == xe_system.line_loop_closing_index) v = 0u;
  uint endian = xe_system.vertex_index_endian;
  if (endian == 1u || endian == 2u) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (endian == 2u || endian == 3u) v = (v >> 16) | (v << 16);
  return clamp((v + uint(xe_system.vertex_base_index)) & 0xFFFFFFu, xe_system.vertex_index_min, xe_system.vertex_index_max);
}
#endif

#if defined(XE_SKIN_ONCE)
// Session 95 (gpu_native_skin_once, the _so variant: tools/shaders/build_skin_once.py): one vertex skinned this frame by src/scene/skin_once.comp, three rows of a real vertex buffer:
// xe_s0 = world position + the texture coordinate dword; xe_s1 = the blended normal + the colour dword; xe_s2 = the unit tangent. XE_SO_UV / XE_SO_QUAD = the dwords of the two raw values in
// this shader's own vertex layout (at = 0 here): nothing else of the raw vertex is read.
layout(location = 0) in uvec4 xe_s0;
layout(location = 1) in uvec4 xe_s1;
layout(location = 2) in uvec4 xe_s2;
int XeVertexAddress(uint index) { return 0; }
int XeVertexAddress(uint index, int stride, int fetch_constant) { return 0; }
uint XeFetchDword(int address) { return address == XE_SO_UV ? xe_s0.w : (address == XE_SO_QUAD ? xe_s1.w : 0u); }
#elif defined(XE_VERTEX_INPUT)
// The vertex's XE_VI_DWORDS dwords as vertex attributes, cut by the rule of rex/graphics/pipeline/shader/vertex_input_layout.h (ViAttributes): uvec4 chunks
// at locations 0.., then a uvec2 and / or a uint tail, never past the vertex. The bytes are the console's (big-endian dwords): XeFetchDword swaps them as the
// shared memory path does (every vertex fetch constant is 8-in-32). A shader declares all of its locations; the optimiser drops the ones it never reads, and
// the pipeline's attributes cover the whole vertex (legal Vulkan). XeVertexAddress is 0: the fetch address is the vertex's own first dword here, and a word
// read past the vertex returns 0 (build_vertex_input_vs.py fails the build when a shader reads one).
#define XE_VI_Q (XE_VI_DWORDS / 4)
#define XE_VI_R (XE_VI_DWORDS % 4)
#if XE_VI_Q >= 1
layout(location = 0) in uvec4 xe_vb0;
#endif
#if XE_VI_Q >= 2
layout(location = 1) in uvec4 xe_vb1;
#endif
#if XE_VI_Q >= 3
layout(location = 2) in uvec4 xe_vb2;
#endif
// (literal locations: the harness and the tools read them without a preprocessor)
#if XE_VI_R >= 2
#if XE_VI_Q == 0
layout(location = 0) in uvec2 xe_vbt2;
#elif XE_VI_Q == 1
layout(location = 1) in uvec2 xe_vbt2;
#else
layout(location = 2) in uvec2 xe_vbt2;
#endif
#endif
#if XE_VI_R == 1
#if XE_VI_Q == 0
layout(location = 0) in uint xe_vbt1;
#elif XE_VI_Q == 1
layout(location = 1) in uint xe_vbt1;
#else
layout(location = 2) in uint xe_vbt1;
#endif
#endif
#if XE_VI_R == 3
#if XE_VI_Q == 0
layout(location = 1) in uint xe_vbt1;
#elif XE_VI_Q == 1
layout(location = 2) in uint xe_vbt1;
#else
layout(location = 3) in uint xe_vbt1;
#endif
#endif
int XeVertexAddress(uint index) { return 0; }
int XeVertexAddress(uint index, int stride, int fetch_constant) { return 0; }
uint XeFetchDword(int address) {
  uint a = uint(address);
#if XE_VI_Q >= 1
  if (a < 4u) return XeSwap8in32(xe_vb0[a]);
#endif
#if XE_VI_Q >= 2
  if (a < 8u) return XeSwap8in32(xe_vb1[a - 4u]);
#endif
#if XE_VI_Q >= 3
  if (a < 12u) return XeSwap8in32(xe_vb2[a - 8u]);
#endif
#if XE_VI_R >= 2
  if (a < uint(XE_VI_Q * 4 + 2)) return XeSwap8in32(xe_vbt2[a - uint(XE_VI_Q * 4)]);
#endif
#if XE_VI_R == 1
  if (a == uint(XE_VI_Q * 4)) return XeSwap8in32(xe_vbt1);
#endif
#if XE_VI_R == 3
  if (a == uint(XE_VI_Q * 4 + 2)) return XeSwap8in32(xe_vbt1);
#endif
  return 0u;
}
#else
// The vertex's first dword in the shared memory: the arena meshes' one vertex stream (the library puts it in the last vertex
// fetch constant, 95 = dwords 190 / 191 of the fetch constants) with 8-dword (32-byte) vertices.
int XeVertexAddress(uint index) {
  return int(xe_fetch.fetch_constants[47].z >> 2u) + int(index) * 8;
}

// The same for a stream of another stride (dwords a vertex) in a vertex fetch constant 0-95 (95 = [47].z, 0 = [0].x, ...).
int XeVertexAddress(uint index, int stride, int fetch_constant) {
  uvec4 pair = xe_fetch.fetch_constants[fetch_constant >> 1];
  uint base = (fetch_constant & 1) != 0 ? pair.z : pair.x;
  return int(base >> 2u) + int(index) * stride;
}

uint XeFetchDword(int address) { return XeSwap8in32(XeSharedMemoryLoad(address)); }
#endif
float XeFetchFloat(int address) { return uintBitsToFloat(XeFetchDword(address)); }

// The translation's last step: the guest position (its w a reciprocal unless flagged) into the host's clip space.
void XeOutputPosition(vec4 p) {
  float inv_w = ((xe_system.flags & 1024u) != 0u) ? p.w : 1.0 / p.w;
  vec2 xy = ((xe_system.flags & 256u) != 0u) ? p.xy * inv_w : p.xy;
  float z = ((xe_system.flags & 512u) != 0u) ? p.z * inv_w : p.z;
  precise vec3 scaled = vec3(xy, z) * vec3(xe_system.ndc_scale_x, xe_system.ndc_scale_y, xe_system.ndc_scale_z);
  precise vec3 offset = vec3(xe_system.ndc_offset_x, xe_system.ndc_offset_y, xe_system.ndc_offset_z) * inv_w;
  precise vec3 ndc = scaled + offset;
  gl_Position = vec4(ndc, inv_w);
}
