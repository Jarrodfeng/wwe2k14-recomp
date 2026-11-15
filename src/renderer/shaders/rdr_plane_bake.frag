#version 450
// S102 lane L2 (docs\renderer\SHADOWS_OURS.md, "S102 L2"): the plane shadow bake's fullscreen passes, pixel stage. Our own shader, not a game shader.
//   Mode.x = 0 / 1: soften the bake along x / z (a true gaussian of sigma `Mode.w` TEXELS, every texel tapped out to 3 sigma: a sparse comb of taps would copy a thin line into several); reads map_a.
//   Mode.x = 2: the composite. The pixel is a point of the game's screen-fit map (the space the ring mat reads: the camera's clip space taken through PlaneInv). The ground-plane point that projects
//   there is found by inverting that projection on the plane (a 2x2 solve: M is the frame's PlaneInv * CameraViewProj, the plane is y = Bake.w), and the baked rope shadow is read at that point.
//   The result is max(the game's blurred wrestler map at the same place (map_a), the bake (map_b)): the game's own rule for the mat is 1 - map.x, so a max of two darknesses is the union of shadows.
layout(set = 0, binding = 1, std140) uniform FragmentConstants {
  vec4 Mode;   // x: 0 soften x, 1 soften z, 2 composite; y: texel step of the SOURCE (1 / its edge); z: 1 = min instead of max, 2 = ignore the bake (planted faults); w: gaussian sigma in texels (soften)
  vec4 Unused0;   // (the camera rows M moved to the vertex block so the frame pump can interpolate them: flat varyings below)
  vec4 Unused1;
  vec4 Unused2;
  vec4 Bake;   // x, y: the bake's centre on the plane (x, z); z: 1 / half extent; w: the plane's height
  vec4 Misc;   // x: shift of the bake read in uv (planted half-texel fault); y: 1 = read the game's wrestler map, 0 = ignore it (planted fault); z: gain on the bake
} fc;
layout(set = 1, binding = 0) uniform sampler2D map_a;   // slot 0: the source (soften) / the game's blurred wrestler map (composite)
layout(set = 1, binding = 1) uniform sampler2D map_b;   // slot 1: the softened bake (composite)
layout(location = 0) in vec2 in_clip;
layout(location = 1) flat in vec4 in_row_x;   // M row 0: the screen fit's clip x of a world point (x, y, z, 1)
layout(location = 2) flat in vec4 in_row_y;   // M row 1
layout(location = 3) flat in vec4 in_row_w;   // M row 3
layout(location = 0) out vec4 out_color;

vec2 clip_to_uv(vec2 c) { return vec2(0.5 * c.x + 0.5, 0.5 - 0.5 * c.y); }

void main() {
  vec2 uv = clip_to_uv(in_clip);
  int mode = int(fc.Mode.x + 0.5);
  if (mode < 2) {
    vec2 step_uv = (mode == 0 ? vec2(1.0, 0.0) : vec2(0.0, 1.0)) * fc.Mode.y;
    float sigma = max(fc.Mode.w, 0.3);
    int radius = int(min(ceil(3.0 * sigma), 40.0));
    float sum = 0.0, weight = 0.0;
    for (int k = -radius; k <= radius; ++k) {
      float w = exp(-0.5 * float(k * k) / (sigma * sigma));
      sum += w * texture(map_a, uv + step_uv * float(k)).x;
      weight += w;
    }
    sum /= weight;
    out_color = vec4(sum, sum, sum, 1.0);
    return;
  }
  float wrestler = fc.Misc.y > 0.5 ? texture(map_a, uv).x : 0.0;
  vec2 ndc = vec2(in_clip.x, in_clip.y);
  float py = fc.Bake.w;
  float cw = in_row_w.y * py + in_row_w.w;
  float ax = in_row_x.x - ndc.x * in_row_w.x, bx = in_row_x.z - ndc.x * in_row_w.z, cx = in_row_x.y * py + in_row_x.w - ndc.x * cw;
  float ay = in_row_y.x - ndc.y * in_row_w.x, by = in_row_y.z - ndc.y * in_row_w.z, cy = in_row_y.y * py + in_row_y.w - ndc.y * cw;
  float det = ax * by - bx * ay;
  float bake = 0.0;
  if (abs(det) > 1.0e-12) {
    vec2 xz = vec2(bx * cy - cx * by, cx * ay - ax * cy) / det;   // ax x + bx z = -cx; ay x + by z = -cy
    vec2 bn = (xz - fc.Bake.xy) * fc.Bake.z;
    float w_at = in_row_w.x * xz.x + in_row_w.z * xz.y + cw;   // the clip w of that ground point: a point behind the camera (w <= 0) projects to the screen too, and must not be shadowed
    if (w_at > 0.0 && abs(bn.x) <= 1.0 && abs(bn.y) <= 1.0) bake = texture(map_b, clip_to_uv(bn) + vec2(fc.Misc.x)).x * fc.Misc.z;
  }
  if (fc.Mode.z > 1.5) bake = 0.0;
  float v = (fc.Mode.z > 0.5 && fc.Mode.z < 1.5) ? min(wrestler, bake) : max(wrestler, bake);
  out_color = vec4(v, v, v, 1.0);
}
