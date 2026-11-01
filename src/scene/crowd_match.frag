#version 450
// Session 80 (SCENE_SHADERS_PLAN.md step 1; Jarrod: "Our own scene shaders"): the match crowd's material, in place of
// guest pixel shader ED94EC2F6D52E14F - the member's colour texture times the colour its vertex shader lit (sky /
// ground ambient, four directional lights, the arena's lighting map), with the texture's own alpha.
// Interface (src\scene_swap.h, SDK vulkan/native/shader_swap.h): inputs 0 and 1 = the guest's first two interpolators
// (xy: the texture coordinate; rgb: the lit colour), set 3 = the guest textures under canonical bindings (1000 + fetch
// constant images, 1200 + samplers). No float constants.

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0: the member's colour
layout(set = 3, binding = 1200) uniform sampler s_color;

layout(location = 0) in vec4 in_texcoord;
layout(location = 1) in vec4 in_light;
layout(location = 0) out vec4 out_color;

void main() {
  // (2D textures are bound as 2D array views, layer 0: src\post\combine.glsl.)
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_texcoord.xy, 0.0));
  out_color = vec4(color.rgb * in_light.rgb, color.a);
}
