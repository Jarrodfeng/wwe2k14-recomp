#version 450
// yPartsBlur: a mesh (the part being blurred) moved by its local-to-world matrix, then the view-projection matrix.
// Matrices are rows. The vertex colour is passed on though the pixel shader does not use it.
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 g_fm44LocalWorld[4];
  vec4 g_fm44ViewProj[4];
} vc;
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColor;

void main() {
  vec4 p = vec4(inPosition, 1.0);
  vec4 world = vec4(dot(vc.g_fm44LocalWorld[0], p), dot(vc.g_fm44LocalWorld[1], p), dot(vc.g_fm44LocalWorld[2], p), dot(vc.g_fm44LocalWorld[3], p));
  gl_Position = vec4(dot(vc.g_fm44ViewProj[0], world), dot(vc.g_fm44ViewProj[1], world), dot(vc.g_fm44ViewProj[2], world), dot(vc.g_fm44ViewProj[3], world));
  vTexCoord = inTexCoord;
  vColor = inColor;
}
