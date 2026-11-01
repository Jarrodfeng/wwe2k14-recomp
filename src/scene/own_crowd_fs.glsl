// Session 80 (OWN_RENDERER_PLAN.md: the crowd fully ours). Our own crowd pixel shader body: the member's colour texture
// times the colour the vertex shader lit, with the texture's own alpha; ENTRANCE: times the arena's lighting map read
// here at the coordinate the vertex shader passed (out_texcoord.zw) and scaled by its strength (c184.z).
// Shared by own_crowd_match.frag / own_crowd_entrance.frag; set 0 as own_crowd_vs.glsl's (binding 2 the colour
// texture, fetch constant 0; binding 3 the entrances' lighting map, fetch constant 13).

layout(set = 0, binding = 0, std140) uniform CrowdScene {
  vec4 view_proj[4];
  vec4 light_dir[4];
  vec4 light_color[4];
  vec4 light_floor;
  vec4 up_axis;
  vec4 ambient_low;
  vec4 ambient_high;
  vec4 light_map;
  vec4 ambient_scale;
  vec4 diffuse_scale;
  vec4 half_k;
  vec4 ndc_scale;
  vec4 ndc_offset;
  uvec4 index_info;
} scene;
layout(set = 0, binding = 2) uniform sampler2DArray color_tex;
#ifdef ENTRANCE
layout(set = 0, binding = 3) uniform sampler2DArray entrance_map_tex;
#endif

layout(location = 0) in vec4 in_texcoord;
layout(location = 1) in vec4 in_light;
layout(location = 0) out vec4 out_color;

void main() {
  vec4 color = texture(color_tex, vec3(in_texcoord.xy, 0.0));
  vec3 rgb = color.rgb * in_light.rgb;
#ifdef ENTRANCE
  rgb *= texture(entrance_map_tex, vec3(in_texcoord.zw, 0.0)).rgb * scene.light_map.z;
#endif
  out_color = vec4(rgb, color.a);
}
