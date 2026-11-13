#version 450
// yHalation: light bloom drawn over the frame. Options:
//   TRAIL_SPHERE  a soft round blob (centre, radius, colour; width / height corrects the aspect) added to the frame
//   FLARE         frame (slot 0) + a light glow and a coloured flare, both shaped as a falling-off cross (1 - |dx|/w)(1 - |dy|/h)
//                 around g_f2LightCenterPos, + the trail texture (slot 1) scaled by g_fTrailIntensity
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
#ifdef TRAIL_SPHERE
  vec4 g_f2TrailSpherePos;
  vec4 g_fTrailSphereRadius;
  vec4 g_f3TrailSphereColor;
  vec4 g_fWidthDivHeight;
#else
  vec4 g_f2LightCenterPos;
  vec4 g_fLightWidth;
  vec4 g_fLightHeight;
  vec4 g_fLightIntensity;
  vec4 g_fFlareWidth;
  vec4 g_fFlareHeight;
  vec4 g_fFlareIntensity;
  vec4 g_f3FlareColor;
  vec4 g_fTrailIntensity;
#endif
} fc;
layout(set = 1, binding = 0) uniform sampler2D textures[16];
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
#ifdef TRAIL_SPHERE
  vec3 frame = texture(textures[0], vTexCoord).rgb;
  vec2 d = vTexCoord - fc.g_f2TrailSpherePos.xy;
  d.x *= fc.g_fWidthDivHeight.x;
  float falloff = min(length(d) / fc.g_fTrailSphereRadius.x, 1.0);
  outColor = vec4(frame + (1.0 - falloff) * fc.g_f3TrailSphereColor.xyz, 1.0);
#else
  vec3 trail = texture(textures[1], vTexCoord).rgb;
  vec3 frame = texture(textures[0], vTexCoord).rgb;
  vec2 d = abs(vTexCoord - fc.g_f2LightCenterPos.xy);
  vec2 light = 1.0 - min(d / vec2(fc.g_fLightWidth.x, fc.g_fLightHeight.x), 1.0);
  vec2 flare = 1.0 - min(d / vec2(fc.g_fFlareWidth.x, fc.g_fFlareHeight.x), 1.0);
  float lightAmount = fc.g_fLightIntensity.x * fc.g_fLightIntensity.x * light.x * light.y;
  float flareAmount = fc.g_fFlareIntensity.x * fc.g_fFlareIntensity.x * flare.x * flare.y;
  outColor = vec4(frame + lightAmount + flareAmount * fc.g_f3FlareColor.xyz + trail * fc.g_fTrailIntensity.x, 1.0);
#endif
}
