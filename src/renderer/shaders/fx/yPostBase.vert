#version 450
// yPostBase: full-screen quad. FX_UV also passes the texture coordinate on (the fill passes without a texture have none).
layout(location = 0) in vec3 inPosition;
#ifdef FX_UV
layout(location = 1) in vec2 inTexCoord;
layout(location = 0) out vec2 vTexCoord;
#endif

void main() {
  gl_Position = vec4(inPosition, 1.0);
#ifdef FX_UV
  vTexCoord = inTexCoord;
#endif
}
