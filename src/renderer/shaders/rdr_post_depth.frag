#version 450
// Session 101 lane 34 (the post pass on the viewer's scenes): the viewer renders with an ordinary 0..1 depth; the game's post effects (yDownScale_dof, yGlareEtc)
// read the GAME's depth, 9.92 / view w (small = far: ~0.002 for the far crowd, ~0.007 near the camera; docs\systems\post_effects.md; the vk viewer's GameDepth()).
// This rewrites the first into the second: the view w of a viewer depth d is nq / (q - d) (q = the projection's m[10], nq = -m[14]; view\render_shared.h
// PostParams), so game depth = 9.92 * (q - d) / nq.
// One full-target triangle (rdr_fullscreen.vert); the output is an R32F image the post effects sample as their depth texture.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 proj;   // x: q, y: nq, z: the game's near plane constant 9.92
} fc;
layout(set = 1, binding = 0) uniform sampler2D viewer_depth;
layout(location = 0) out vec4 out_depth;

void main() {
  float d = texelFetch(viewer_depth, ivec2(gl_FragCoord.xy), 0).x;
  float w = fc.proj.y / max(fc.proj.x - d, 1e-7);
  out_depth = vec4(fc.proj.z / max(w, 1e-3), 0.0, 0.0, 1.0);
}
