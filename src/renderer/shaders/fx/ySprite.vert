#version 450
// ySprite vertex stage: a screen sprite (UI, text, 2D art). See docs\renderer\effects\ySprite.md.
// Options: FX_VERTEX_COLOR (colour comes from the vertex), FX_TINT (multiply by the Color constant),
//          FX_GRADIENT_COORD (a second texture coordinate pair for the gradation), FX_CHANNEL_SELECT (text channel masks).

layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 PosScale;
  vec4 fUseProjMatrix;
  vec4 matProj[4];
#if defined(FX_TINT) || !defined(FX_VERTEX_COLOR)
  vec4 Color;
#endif
#ifdef FX_CHANNEL_SELECT
  vec4 XfmChannelSelector;
#endif
} vc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
#ifdef FX_VERTEX_COLOR
layout(location = 2) in vec4 inColor;
#endif
#ifdef FX_GRADIENT_COORD
layout(location = 3) in vec2 inGradientCoord;
#endif

#ifdef FX_GRADIENT_COORD
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec2 vGradientCoord;
layout(location = 2) out vec4 vColor;
#elif defined(FX_CHANNEL_SELECT)
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vChannelSelect;
layout(location = 2) out vec4 vColor;
layout(location = 3) out float vAlphaMaskMode;
#else
layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColor;
#endif



// Two ways to place the sprite: fUseProjMatrix == 0 treats the position as pixel-style units scaled by PosScale
// (origin shifted by half a unit, y flipped); anything else runs the projection matrix.
vec4 spritePosition(vec3 p) {
  if (vc.fUseProjMatrix.x == 0.0) {
    vec2 scaled = (p.xy - 0.5) * vc.PosScale.xy;
    return vec4(scaled.x - 1.0, 1.0 - scaled.y, p.z, 1.0);
  }
  vec4 q = vec4(p, 1.0);
  return vec4(dot(vc.matProj[0], q), dot(vc.matProj[1], q), dot(vc.matProj[2], q), dot(vc.matProj[3], q));
}

void main() {
  gl_Position = spritePosition(inPosition);
  vTexCoord = inTexCoord;
#ifdef FX_GRADIENT_COORD
  vGradientCoord = inGradientCoord;
#ifdef FX_VERTEX_COLOR
  vColor = inColor;
#else
  vColor = vc.Color;
#endif
#elif defined(FX_CHANNEL_SELECT)
  // A channel whose selector value is in (0, 0.5] is an alpha-mask channel: it is sent on as 1 and raises the mask flag.
  bvec4 isMask = bvec4(vc.XfmChannelSelector.x > 0.0 && vc.XfmChannelSelector.x <= 0.5,
                       vc.XfmChannelSelector.y > 0.0 && vc.XfmChannelSelector.y <= 0.5,
                       vc.XfmChannelSelector.z > 0.0 && vc.XfmChannelSelector.z <= 0.5,
                       vc.XfmChannelSelector.w > 0.0 && vc.XfmChannelSelector.w <= 0.5);
  vChannelSelect = vec4(isMask.x ? 1.0 : vc.XfmChannelSelector.x, isMask.y ? 1.0 : vc.XfmChannelSelector.y,
                        isMask.z ? 1.0 : vc.XfmChannelSelector.z, isMask.w ? 1.0 : vc.XfmChannelSelector.w);
  vColor = vc.Color;
  vAlphaMaskMode = any(isMask) ? 1.0 : 0.0;
#elif defined(FX_VERTEX_COLOR) && defined(FX_TINT)
  vColor = inColor * vc.Color;
#elif defined(FX_VERTEX_COLOR)
  vColor = inColor;
#else
  vColor = vc.Color;
#endif
}
