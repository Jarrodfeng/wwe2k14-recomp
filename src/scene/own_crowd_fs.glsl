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
  uvec4 alpha_info;  // Session 87: x the alpha test reference (float bits), y the test (the draw's own: bit 0 pass if less,
                     // 1 equal, 2 greater; 7 always; 5 not equal - as the guest epilogue's)
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
  // The draw's alpha test, as the guest path's epilogue applies it (the flat cards are cut-outs: pass if alpha >= 0.753;
  // the lit crowd's test is "always").
  uint test = scene.alpha_info.y;
  if (test != 7u) {
    float a = color.a, ref = uintBitsToFloat(scene.alpha_info.x);
    bool pass = test == 5u ? !(a == ref)
                           : ((test & 1u) != 0u && a < ref) || ((test & 2u) != 0u && a == ref) ||
                                 ((test & 4u) != 0u && a > ref);
    if (!pass) discard;
  }
  vec3 rgb = color.rgb * in_light.rgb;
#ifdef ENTRANCE
  rgb *= texture(entrance_map_tex, vec3(in_texcoord.zw, 0.0)).rgb * scene.light_map.z;
#endif
  out_color = vec4(rgb, color.a);
}
