#version 450
// Session 87 (step 1 piece 2): our own vertex shader for the crowd's flat cards (the game's T_Audience, guest vertex
// shader D5E2FCC9F76BC40C, pixel shader EFC4EFCB5FC51945), the same way own_crowd_vs.glsl does the lit crowd. What the
// guest shader does was read from its microcode locally (never committed): the card's vertex (a 4-dword vertex: position
// xyz as floats, the sprite's coordinate as a half-float pair) is placed at the member's world matrix (the game's VS
// c76-c78), clipped by c56-c59; the vertex colour is white; the arena's lighting map is read per pixel at the member's
// world x / z (the first interpolator's zw: world * c255.x * c184.xy + c255.x - the card shader scales by c255.x where
// the lit ones use c255.z; both are the game's constants for the draw). The pixel shader is the entrances' own
// (own_crowd_entrance.frag: sprite x vertex colour x lighting map x c184.z, alpha the sprite's).
//
// Interface as the lit crowd's (SDK owned crowd, vulkan/native/own_crowd.cpp): no vertex inputs - each vertex pulled
// from our converted copy of the card mesh (binding 4: 4 dwords a vertex, byte-swapped, the half pair left packed) by the
// game's own index, each member's matrix from binding 5 by the instance; set 0 binding 0 the scene block.

#ifdef CROWD_VERTEX_INPUT
// Session 95 (P7, gpu_native_crowd_input): the attribute form, as own_crowd_vs.glsl's: binding 0 the card mesh, binding 1 the members per instance, the index already the guest's.
layout(location = 0) in uvec4 in_mesh;     // position xyz, the half pair
layout(location = 1) in uvec4 in_member0;  // c76
layout(location = 2) in uvec4 in_member1;  // c77
layout(location = 3) in uvec4 in_member2;  // c78
#else
layout(set = 0, binding = 4, std430) readonly buffer CrowdMesh { uint mesh[]; };
layout(set = 0, binding = 5, std430) readonly buffer CrowdMembers { vec4 members[]; };
#endif

layout(set = 0, binding = 0, std140) uniform CrowdScene {
  vec4 view_proj[4];     // c56-c59: world -> clip
  vec4 light_dir[4];     // c8-c11 (unused by the cards)
  vec4 light_color[4];   // c0-c3 (unused)
  vec4 light_floor;      // c74 (unused)
  vec4 up_axis;          // c180 (unused)
  vec4 ambient_low;      // c181 (unused)
  vec4 ambient_high;     // c182 (unused)
  vec4 light_map;        // c184: xy the map's scale per world unit, z its strength
  vec4 ambient_scale;    // c189 (unused)
  vec4 diffuse_scale;    // c190 (unused)
  vec4 half_k;           // c255: x = 0.5 for the cards (the game's own constant)
  vec4 ndc_scale;        // the host's clip-space conversion (the translator's ndc_scale / ndc_offset)
  vec4 ndc_offset;
  uvec4 index_info;      // x the index endian (xenos::Endian), y the base index, z / w the index clamp range
} scene;

layout(location = 0) out vec4 out_texcoord;  // xy the sprite, zw the lighting map
layout(location = 1) out vec4 out_light;     // the vertex colour: white

#ifndef CROWD_VERTEX_INPUT
uint GuestIndex(uint v) {
  uint endian = scene.index_info.x;
  if (endian == 1u || endian == 2u) v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
  if (endian == 2u || endian == 3u) v = (v >> 16) | (v << 16);
  return clamp((v + scene.index_info.y) & 0xFFFFFFu, scene.index_info.z, scene.index_info.w);
}
#endif

void main() {
#ifdef CROWD_VERTEX_INPUT
  vec3 in_position = uintBitsToFloat(in_mesh.xyz);
  vec2 in_texcoord = unpackHalf2x16(in_mesh.w).yx;
  vec4 p = vec4(in_position, 1.0);
  vec3 world = vec3(dot(uintBitsToFloat(in_member0), p), dot(uintBitsToFloat(in_member1), p), dot(uintBitsToFloat(in_member2), p));
#else
  uint v = GuestIndex(uint(gl_VertexIndex)) * 4u;
  vec3 in_position = vec3(uintBitsToFloat(mesh[v]), uintBitsToFloat(mesh[v + 1u]), uintBitsToFloat(mesh[v + 2u]));
  // The half pair: the sprite coordinate is (high half, low half) of the byte-swapped dword (as the lit crowd's).
  vec2 in_texcoord = unpackHalf2x16(mesh[v + 3u]).yx;
  uint m = uint(gl_InstanceIndex) * 3u;
  vec4 p = vec4(in_position, 1.0);
  vec3 world = vec3(dot(members[m], p), dot(members[m + 1u], p), dot(members[m + 2u], p));
#endif

  vec4 w = vec4(world, 1.0);
  vec4 clip = vec4(dot(scene.view_proj[0], w), dot(scene.view_proj[1], w), dot(scene.view_proj[2], w),
                   dot(scene.view_proj[3], w));
  gl_Position = vec4(clip.xyz * scene.ndc_scale.xyz + scene.ndc_offset.xyz * clip.w, clip.w);

  float k = scene.half_k.x;
  out_texcoord = vec4(in_texcoord, world.xz * k * scene.light_map.xy + k);
  out_light = vec4(1.0);
}
