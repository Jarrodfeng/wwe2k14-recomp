// Session 80 (OWN_RENDERER_PLAN.md: the crowd fully ours). Our own crowd vertex shader - not a swap of the game's: our
// own inputs (the member mesh converted once into a host vertex buffer, each member's world matrix as instance data,
// the scene's lights in one uniform block), the crowd's lighting written as plain maths. What the lighting does was
// read from the game's crowd vertex shaders (183CC64A4C4E96AC match, C1028A64EFCF8229 entrances; microcode read
// locally, never committed): per vertex, the member's world position and normal; a sky / ground ambient blended by the
// normal against the up axis; four directional lights clamped at a floor; all of it times the arena's lighting map,
// read at the member's world x / z (a top-down map). Shared body of own_crowd_match.vert / own_crowd_entrance.vert
// (ENTRANCE: the entrances read the lighting map per pixel - its coordinate goes to the pixel shader in
// out_texcoord.zw - and the vertex colour is the light alone).
//
// Interface (SDK owned crowd, vulkan/native/own_crowd.h): no vertex inputs - each vertex is pulled from our converted
// copy of the member mesh (binding 4) by the game's own index (the draw's index buffer, swapped and offset as the
// translator does: index_info), each member's world matrix from binding 5 by the instance; set 0 binding 0 the
// scene block (the game's own constants for the draw, by register: noted per member); binding 1 the lighting map
// (the match's vertex read; the entrances read it in the pixel shader).

// The member mesh: 8 floats a vertex (position xyz, normal xyz, the colour texture's coordinate already in the order
// the pixel shader samples), converted once from the game's 7-dword big-endian vertices.
layout(set = 0, binding = 4, std430) readonly buffer CrowdMesh { float mesh[]; };
// The members drawn: 3 vec4 each, the world matrix rows (the game's VS c76, c77, c78).
layout(set = 0, binding = 5, std430) readonly buffer CrowdMembers { vec4 members[]; };

layout(set = 0, binding = 0, std140) uniform CrowdScene {
  vec4 view_proj[4];     // c56-c59: world -> clip
  vec4 light_dir[4];     // c8-c11: the four lights' directions (towards the light), paired with light_color
  vec4 light_color[4];   // c0-c3
  vec4 light_floor;      // c74: each light's lower clamp (x: light 0 ... w: light 3)
  vec4 up_axis;          // c180: the axis the ambient blend measures the normal against
  vec4 ambient_low;      // c181: ambient for normals facing along up_axis
  vec4 ambient_high;     // c182: ambient for normals facing against it
  vec4 light_map;        // c184: xy the map's scale per world unit, z its strength
  vec4 ambient_scale;    // c189
  vec4 diffuse_scale;    // c190
  vec4 half_k;           // c255: z = 0.5 (the game's own constant)
  vec4 ndc_scale;        // the host's clip-space conversion (the translator's ndc_scale / ndc_offset)
  vec4 ndc_offset;
  uvec4 index_info;      // x the index endian (xenos::Endian), y the base index, z / w the index clamp range
} scene;
#ifndef ENTRANCE
layout(set = 0, binding = 1) uniform sampler2DArray light_map_tex;  // fetch constant 16
#endif

layout(location = 0) out vec4 out_texcoord;  // xy the colour texture, zw the lighting map (entrances)
layout(location = 1) out vec4 out_light;

uint GuestIndex(uint v) {
  uint endian = scene.index_info.x;
  if (endian == 1u || endian == 2u) v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
  if (endian == 2u || endian == 3u) v = (v >> 16) | (v << 16);
  return clamp((v + scene.index_info.y) & 0xFFFFFFu, scene.index_info.z, scene.index_info.w);
}

void main() {
  uint v = GuestIndex(uint(gl_VertexIndex)) * 8u;
  vec3 in_position = vec3(mesh[v], mesh[v + 1u], mesh[v + 2u]);
  vec3 in_normal = vec3(mesh[v + 3u], mesh[v + 4u], mesh[v + 5u]);
  vec2 in_texcoord = vec2(mesh[v + 6u], mesh[v + 7u]);
  uint m = uint(gl_InstanceIndex) * 3u;
  vec4 in_world_x = members[m], in_world_y = members[m + 1u], in_world_z = members[m + 2u];
  vec4 p = vec4(in_position, 1.0);
  vec3 world = vec3(dot(in_world_x, p), dot(in_world_y, p), dot(in_world_z, p));
  vec3 normal = vec3(dot(in_world_x.xyz, in_normal), dot(in_world_y.xyz, in_normal), dot(in_world_z.xyz, in_normal));

  vec4 w = vec4(world, 1.0);
  vec4 clip = vec4(dot(scene.view_proj[0], w), dot(scene.view_proj[1], w), dot(scene.view_proj[2], w),
                   dot(scene.view_proj[3], w));
  gl_Position = vec4(clip.xyz * scene.ndc_scale.xyz + scene.ndc_offset.xyz * clip.w, clip.w);

  // The arena's lighting map, top-down at the member's world x / z.
  float k = scene.half_k.z;
  vec2 map_uv = world.xz * k * scene.light_map.xy + k;

  // Ambient: blended between the two ambient colours by how the normal faces the up axis.
  float facing = k - k * dot(normal, scene.up_axis.xyz);
  vec3 ambient = mix(scene.ambient_low.rgb, scene.ambient_high.rgb, facing) * scene.ambient_scale.rgb;

  // Four directional lights, each clamped at its floor.
  vec3 diffuse = vec3(0.0);
  for (int i = 0; i < 4; ++i) {
    diffuse += max(dot(normal, scene.light_dir[i].xyz), scene.light_floor[i]) * scene.light_color[i].rgb;
  }

  vec3 light = ambient + diffuse * scene.diffuse_scale.rgb;
#ifdef ENTRANCE
  out_texcoord = vec4(in_texcoord, map_uv);
  out_light = vec4(light, 1.0);
#else
  vec4 map = textureLod(light_map_tex, vec3(map_uv, 0.0), 0.0);
  out_texcoord = vec4(in_texcoord, 0.0, 0.0);
  out_light = vec4(map.rgb * scene.light_map.z * light, scene.light_map.z * map.a);
#endif
}
