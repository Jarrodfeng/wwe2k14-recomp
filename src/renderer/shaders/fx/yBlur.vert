#version 450
// Screen blurs and texture merges, vertex stage. See docs\renderer\effects\yBlur.md.
// Options: FX_MASKED (the quad is turned into pixel space with VertexConvert and switched off where a mask texture, read per vertex, is zero).
//          FX_PLANE_X / FX_PLANE_Z (plane-shadow blur: a point on the shadow plane is projected three times, once as is and once pushed along x (or z)
//          each way by a step; the three projected positions go to the pixel stage as taps).
#if defined(FX_PLANE_X) || defined(FX_PLANE_Z)
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 BlurStep;           // x: step between the taps (scaled by 5)
  vec4 PlaneMatrix[4];     // rows of the matrix from the shadow plane to clip space
  vec4 PlaneScale;         // x, z: scale of the plane coordinates, y: height of the plane
} vc;
layout(location = 2) in vec3 inPlanePosition;   // only x and z are used
layout(location = 0) out vec4 vTap0;
layout(location = 1) out vec4 vTap1;
layout(location = 2) out vec4 vTap2;

vec4 projectPlanePoint(vec3 p) {
  vec4 v = vec4(p, 1.0);
  return vec4(dot(vc.PlaneMatrix[0], v), dot(vc.PlaneMatrix[1], v), dot(vc.PlaneMatrix[2], v), dot(vc.PlaneMatrix[3], v));
}

void main() {
  vec3 p = vec3(inPlanePosition.x * vc.PlaneScale.x, vc.PlaneScale.y, inPlanePosition.z * vc.PlaneScale.z);
  float step = 5.0 * vc.BlurStep.x;
#ifdef FX_PLANE_X
  vec3 along = vec3(step, 0.0, 0.0);
  vTap1 = projectPlanePoint(p - along);
  vTap2 = projectPlanePoint(p + along);
#else
  vec3 along = vec3(0.0, 0.0, step);
  vTap1 = projectPlanePoint(p + along);
  vTap2 = projectPlanePoint(p - along);
#endif
  vTap0 = projectPlanePoint(p);
  gl_Position = vec4(vTap0.xy, -vTap0.z, vTap0.w);
}
#else
#if defined(FX_MASKED)
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 VertexConvert;      // xy scale, zw offset of the quad position
} vc;
layout(set = 1, binding = 1) uniform sampler2D textures[4];   // textures[0] is the game's vertex texture slot 16
layout(location = 0) in vec2 inPosition;
#else
layout(location = 0) in vec4 inPosition;
#endif
layout(location = 1) in vec2 inTexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
#if defined(FX_MASKED)
  float on = textureLod(textures[0], inTexCoord, 0.0).x != 0.0 ? 1.0 : 0.0;
  gl_Position = vec4((inPosition * vc.VertexConvert.xy + vc.VertexConvert.zw) * on, 1.0, on);
#else
  gl_Position = inPosition;
#endif
  vTexCoord = inTexCoord;
}
#endif
