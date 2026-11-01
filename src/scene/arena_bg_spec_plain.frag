#version 450
#extension GL_GOOGLE_include_directive : require
#include "xenos_ps.glsl"
#include "arena_ps.glsl"
// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the background's material with a highlight the texture does not
// mask (the game's yBG_Default, guest pixel shader 748BF8B0868777D3; one draw on the entrance stage), through the
// shader swap (src\scene_swap.h): as arena_bg_spec.frag but the highlight (the highlight colour c200 times c201.x,
// times the normal against the half vector of the view and the light c8, floored at c255.x and raised to c202.x,
// scaled per vertex by the first interpolator's z) is not multiplied by the colour texture's alpha, and the alpha out
// is the texture's alpha times the vertex alpha. The colour texture is read at the shader's own LOD bias (c248.x);
// the vertex colour is the sixth interpolator. As elsewhere: unsigned texture, no exponent adjust, no fetch-constant
// LOD bias, no rounding nudge on the coordinate; the console's 0 x infinity = 0 is kept in the power (PowAbs).
// Float constants (set 1 binding 2): 8, 200, 201, 202, 248, 255.

layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[6]; } fc;
#define C8   fc.c[0]  // xyz: the light's direction
#define C200 fc.c[1]  // xyz: highlight colour
#define C201 fc.c[2]  // x: highlight strength
#define C202 fc.c[3]  // x: highlight power
#define C248 fc.c[4]  // x: LOD bias of the colour texture
#define C255 fc.c[5]  // x: floor of the highlight's dot product (the compiler's literal bank)

layout(set = 3, binding = 1000) uniform texture2DArray t_color;  // fetch constant 0
layout(set = 3, binding = 1200) uniform sampler s_color;

layout(location = 0) in vec4 in_uv;      // xy: the colour's coordinate; z: the highlight's scale
layout(location = 1) in vec4 in_normal;  // xyz: the surface normal
layout(location = 2) in vec4 in_view;    // xyz: towards the camera
layout(location = 5) in vec4 in_color;   // the vertex colour

void main() {
  vec4 color = texture(sampler2DArray(t_color, s_color), vec3(in_uv.xy, 0.0), C248.x);
  float highlight = ArenaHighlight(in_normal.xyz, in_view.xyz, C8.xyz, C255.x, C202.x);
  vec3 spec = ((C200.xyz * C201.x) * highlight) * in_uv.z;
  XeOutputColor(vec4(color.rgb * in_color.rgb + spec, color.a * in_color.a));
}
