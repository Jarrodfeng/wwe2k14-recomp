// Shared by the transparent-capable character effects (yCh_trans, yCh_TransTiling, yCh_Reflect, yCh_Anisotropic, yCh_Aniso_trans).
// Needs fx_common.glsl and fx_character.glsl included first. See docs\renderer\effects\yCh_trans.md.
#ifndef FX_TRANS_GLSL
#define FX_TRANS_GLSL

// The sky-to-ground tint along the normal. This family measures it against the opposite sign to yCh_Ls: 1 where the normal faces away from the sky.
vec3 trans_hemisphere(vec3 n, vec3 skyDir, vec3 skyCol, vec3 groundCol) {
  return mix(skyCol, groundCol, 0.5 - 0.5 * dot(n, skyDir));
}

// Key-light highlight: the half vector of normal and view against the key light, a Fresnel edge, the rim distance strength and the light colour.
vec3 trans_key_highlight(vec3 n, vec3 v, float nv, vec3 keyDir, vec3 keyColor, float gain, float fresnelPower, float strength) {
  float highlight = max(dot(fx_normalize(0.5 * (n + v)), keyDir), 0.0);
  return keyColor * (gain * highlight * fx_pow(1.0 - abs(nv), fresnelPower) * strength);
}

// Screen-space rim weight from the rim map: the map's depth term, raised to a power, edge-on strength and gloss.
float trans_rim_weight(vec2 rimMap, float depthFade, vec4 depthParam, vec4 rimParam, float gloss, float nv) {
  float base = max(rimMap.y - depthFade, 0.0) * depthParam.z + rimMap.x;
  return fx_pow(base, rimParam.x) * (1.0 - abs(nv)) * ((gloss - 1.0) * rimParam.y + 1.0) * rimParam.z;
}

// Diffuse colour d with a key highlight k laid on top: the rim weight blends towards a bright-edge version, then the specular (clamped to 1) pushes the diffuse down and adds itself.
vec3 trans_compose(vec3 d, vec3 k, float rim, vec3 specular) {
  vec3 edge = 0.75 * (d + k) + 0.25 * (d * (1.0 + k));
  return mix(d, edge, rim) * (1.0 - min(specular, vec3(1.0))) + specular;
}

#endif
