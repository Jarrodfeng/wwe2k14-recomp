#version 450
// S103 FXF: the game's yAfterImage (post effect 4; docs\research\s103\FXP.md "Adversary" E, docs\systems\post_effects.md section 10). The game draws the PREVIOUS output of the post step over the new one as a
// 2D sprite of the display's size: centre S+232 / +236 (display pixels), scale S+240 about the centre, tint S+256..+264 / 255, alpha floor(S+228 * 255) / 255, blend S+272 (0 = SRC_ALPHA / INV_SRC_ALPHA,
// 1 = SRC_ALPHA / ONE). Our own shader: one full-target triangle (rdr_fullscreen.vert), no vertex inputs. Slot 0 = this frame's picture (the chain's output), slot 1 = the history (last frame's output of this pass).
// The sprite's texel alpha is treated as 1 [U, FXP.md]. Outside the sprite's rectangle the picture is unchanged.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 a;   // x, y: the centre as a fraction of the picture; z: scale; w: alpha (already floor(S+228 * 255) / 255, 0 on a first frame)
  vec4 b;   // x, y, z: tint (0..1); w: 1 = additive
  vec4 c;   // x, y: the target size in pixels
} fc;
layout(set = 1, binding = 0) uniform sampler2D picture;
layout(set = 1, binding = 1) uniform sampler2D history;
layout(location = 0) out vec4 out_colour;
layout(location = 1) out vec4 out_history;   // the same picture into the history image (review 1: no per-frame copy)

void main() {
  vec4 dst = texelFetch(picture, ivec2(gl_FragCoord.xy), 0);
  // S105 A1: the DRAWN size (gl_FragCoord is in drawn pixels; a scaled scene is 2x the logical fc.c at 1440p). fc.c.z = 1 is the rig-only planted old divide (by the logical size).
  vec2 p = gl_FragCoord.xy / (fc.c.z > 0.5 ? fc.c.xy : vec2(textureSize(picture, 0)));
  vec2 u = (p - fc.a.xy) / max(fc.a.z, 1e-4) + 0.5;
  bool inside = all(greaterThanEqual(u, vec2(0.0))) && all(lessThanEqual(u, vec2(1.0)));
  vec3 src = texture(history, u).rgb * fc.b.xyz;
  vec3 rgb = fc.b.w > 0.5 ? dst.rgb + src * fc.a.w : mix(dst.rgb, src, fc.a.w);
  out_colour = vec4(inside ? rgb : dst.rgb, dst.a);
  out_history = out_colour;
}
