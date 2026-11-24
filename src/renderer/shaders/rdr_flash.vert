#version 450
// S110 CUTGREY: the crowd camera flash sprite, drawn AFTER the post chain (FrontEnd::DrawFlashesAfterPost). Our own shader (docs\research\s110\CUTGREY_BUILD.md).
// It does what the game's yFlashDefault_base_vertex does for this model (docs\renderer\effects\yFlashDefault.md): the model's vertex through the world and the view-projection, and the
// light of the flash: two lights along +x and -x with the flash's colour, the colour as the ambient (sky = ground = the colour), clamped below at 0, so
//   lit = colour * (1 + abs(n.x)) * material, n = the world-space unit normal (docs\formats\crowd_flash.md "What one draw carries").
// The result is multiplied by the vertex colour (all four channels), as the game's `outColor = vec4(diffuse * MatDifCol + ambient, alpha) * inColor`.
// The depth test is NOT here: the game tests ONE depth read at the sprite's centre and hides the sprite whole; the fragment stage does the same read (rdr_flash.frag).
layout(set = 0, binding = 0, std140) uniform VertexConstants {
  vec4 view_proj[4];   // rows: clip.i = dot(view_proj[i], p)
  vec4 world[3];       // rows: p'.i = dot(world[i], vec4(p, 1))
  vec4 colour;         // rgb = the flash's colour (the light and the ambient); w unused
  vec4 mat_amb;        // MatAmbCol: rgb ambient factor, w alpha factor
  vec4 mat_dif;        // MatDifCol: rgb diffuse factor, w alpha factor
  vec4 alpha;          // x = MdlAlpha (c185.x)
} vc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUv;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

void main() {
  vec4 p = vec4(inPosition, 1.0);
  vec4 w = vec4(dot(vc.world[0], p), dot(vc.world[1], p), dot(vc.world[2], p), 1.0);
  gl_Position = vec4(dot(vc.view_proj[0], w), dot(vc.view_proj[1], w), dot(vc.view_proj[2], w), dot(vc.view_proj[3], w));
  vec3 n = vec3(dot(vc.world[0].xyz, inNormal), dot(vc.world[1].xyz, inNormal), dot(vc.world[2].xyz, inNormal));
  const float len = length(n);
  n = len > 1e-12 ? n / len : vec3(0.0);
  const float diffuse = max(n.x, 0.0) + max(-n.x, 0.0);   // the two lights along +x / -x, no floor below 0
  const vec3 rgb = vc.colour.rgb * diffuse * vc.mat_dif.rgb + vc.colour.rgb * vc.mat_amb.rgb;
  const float a = vc.mat_amb.w * vc.mat_dif.w * vc.alpha.x;
  outUv = inUv;
  outColor = vec4(rgb, a) * inColor;
}
