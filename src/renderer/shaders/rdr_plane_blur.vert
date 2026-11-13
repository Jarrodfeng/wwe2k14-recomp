#version 450
// Session 101 lane 159 (docs\renderer\SHADOW_PLANE.md): the plane shadow's blur, vertex stage. Our own shader, not a game shader.
// The stream's quad keeps its vertex stream (an attribute at location 2 whose x and z are plane coordinates); a plane point is projected three times, once as is and once pushed along
// x (the first pass) or z (the second) each way by a step, and the three clip positions go to the pixel stage as the blur's taps. One program for both passes: Axis says which.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 BlurStep;          // x: step between the taps, before the factor 5 (the record)
  vec4 PlaneMatrix[4];    // rows of the matrix from the shadow plane to clip space (the record)
  vec4 PlaneScale;        // x, z: scale of the plane coordinates, y: height of the plane (the record)
  vec4 Axis;              // xyz: the direction the taps are pushed along (1, 0, 0) or (0, 0, 1); w: -1 for the x pass (tap 1 behind), +1 for the z pass (tap 1 ahead) (the hook)
} vc;
layout(push_constant) uniform FxSystem { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 options; } fx_sys;
layout(location = 2) in vec3 in_plane_position;   // only x and z are used
layout(location = 0) out vec4 out_tap0;
layout(location = 1) out vec4 out_tap1;
layout(location = 2) out vec4 out_tap2;
invariant gl_Position;

vec4 project_plane_point(vec3 p) {
  vec4 v = vec4(p, 1.0);
  return vec4(dot(vc.PlaneMatrix[0], v), dot(vc.PlaneMatrix[1], v), dot(vc.PlaneMatrix[2], v), dot(vc.PlaneMatrix[3], v));
}

void main() {
  vec3 p = vec3(in_plane_position.x * vc.PlaneScale.x, vc.PlaneScale.y, in_plane_position.z * vc.PlaneScale.z);
  float step_size = 5.0 * vc.BlurStep.x;
  vec3 along = vc.Axis.xyz * step_size;
  out_tap1 = project_plane_point(p + along * vc.Axis.w);
  out_tap2 = project_plane_point(p - along * vc.Axis.w);
  out_tap0 = project_plane_point(p);
  vec4 clip = vec4(out_tap0.xy, -out_tap0.z, out_tap0.w);
  gl_Position = vec4(clip.xyz * fx_sys.ndc_scale.xyz + fx_sys.ndc_offset.xyz * clip.w, clip.w);
}
