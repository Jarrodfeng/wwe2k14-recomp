// Session 72 (gpu_native_shader_swap): the shared body of our replacements for the game's final post pass - the match
// combine (guest pixel shader 1233F68C15FDF948) and the entrance combine (4E2CD8031D93DE3E, ENTRANCE defined: the same
// plus bloom and a star glare added at the end). Included by the *_combine_*.frag files; our own code, driven by the
// game's own constants (what each one does: docs\systems\post_effects.md).
//
// ORIGINAL: what the game's pass does, step for step (the console FXAA, the depth of field blend, the depth tint, and in
// entrances bloom + glare), so a same-frame pair against the game's own shader shows only rounding differences. The
// base for doing the post chain in fewer passes later.
// ENHANCED: the same, but the background depth of field is a lens-like disc blur ("bokeh") gathered from the full-size
// scene in place of 80% of a quarter-size blur, only from pixels that are themselves in the blurred background (no
// halo of the in-focus wrestlers around them), with bright spots spreading into discs.
//
// Interface (src\post_swap.h, SDK vulkan/native/shader_swap.h): input 0 = the guest's first interpolator (xy: this
// pixel's texture coordinate, zw: the coordinate half a texel up-left, for the FXAA's diagonal taps), set 1 binding 2 =
// the guest shader's float constants packed in register order, set 3 = the guest textures under canonical bindings
// (1000 + fetch constant images, 1200 + samplers).

// The registers the guest shader uses, in order (post_swap.h registers the same lists).
#ifdef ENTRANCE
// c20 c21 c27 c29 c30 c31 c32 c250 c251 c252 c253 c254 c255
layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[13]; } fc;
#define C31 fc.c[5]
#define C32 fc.c[6]
#define C250 fc.c[7]
#define C251 fc.c[8]
#define C252 fc.c[9]
#define C253 fc.c[10]
#define C254 fc.c[11]
#define C255 fc.c[12]
#else
// c20 c21 c27 c29 c30 c250 c251 c252 c253 c254 c255
layout(set = 1, binding = 2, std140) uniform XeFloatConstants { vec4 c[11]; } fc;
#define C250 fc.c[5]
#define C251 fc.c[6]
#define C252 fc.c[7]
#define C253 fc.c[8]
#define C254 fc.c[9]
#define C255 fc.c[10]
#endif
#define C20 fc.c[0]
#define C21 fc.c[1]
#define C27 fc.c[2]
#define C29 fc.c[3]
#define C30 fc.c[4]

layout(set = 3, binding = 1000) uniform texture2DArray t_scene;   // fetch constant 0: the scene's colour
layout(set = 3, binding = 1002) uniform texture2DArray t_blur_a;  // 2: quarter-size blur for what is right at the camera
layout(set = 3, binding = 1003) uniform texture2DArray t_depth;   // 3: the scene's depth (larger = nearer)
layout(set = 3, binding = 1006) uniform texture2DArray t_blur_b;  // 6: quarter-size blur for the background
layout(set = 3, binding = 1200) uniform sampler s_scene;
layout(set = 3, binding = 1202) uniform sampler s_blur_a;
layout(set = 3, binding = 1203) uniform sampler s_depth;
layout(set = 3, binding = 1206) uniform sampler s_blur_b;
#ifdef ENTRANCE
layout(set = 3, binding = 1007) uniform texture2DArray t_bloom;   // 7: bloom
layout(set = 3, binding = 1008) uniform texture2DArray t_glare;   // 8: the star glare
layout(set = 3, binding = 1207) uniform sampler s_bloom;
layout(set = 3, binding = 1208) uniform sampler s_glare;
#endif

// Session 73: the texture cache binds every 2D texture as a 2D *array* view (layer 0), as the translated shaders
// declare them; reading one through a plain 2D type is undefined (validation VUID-vkCmdDraw-viewType-07752: worked
// on NVIDIA and the Adreno 830 by luck). These read layer 0 of the array.
vec4 Lod(texture2DArray t, sampler s, vec2 uv) { return textureLod(sampler2DArray(t, s), vec3(uv, 0.0), 0.0); }
ivec2 Size(texture2DArray t, sampler s) { return textureSize(sampler2DArray(t, s), 0).xy; }
vec4 Fetch(texture2DArray t, sampler s, ivec2 p) { return texelFetch(sampler2DArray(t, s), ivec3(p, 0), 0); }

layout(location = 0) in vec4 in_texcoord;
layout(location = 0) out vec4 out_color;

vec3 Tap(vec2 p) { return Lod(t_scene, s_scene, p).rgb; }
// The game's luma: c252 holds the weights for blue, red, green.
float Luma(vec3 rgb) { return dot(rgb, C252.yzx); }

// The console FXAA as the game runs it: four diagonal taps around the pixel give the edge direction, then two taps
// along it (c253: about half a texel each way) or four (c254 adds about a texel each way); the four-tap result unless
// its luma leaves the neighbourhood's range.
vec3 ConsoleFxaa(vec2 uv, vec2 nw) {
  float lNW = Luma(Tap(nw));
  float lNE = Luma(Tap(nw + vec2(C251.x, 0.0)));
  float lSW = Luma(Tap(nw + vec2(0.0, C251.y)));
  float lSE = Luma(Tap(nw + C251.xy));
  float lM = Luma(Tap(uv));
  float reduce = max((lNW + lNE + lSW + lSE) * C251.z, C250.y);
  float dNS = (lNW + lNE) - (lSW + lSE);
  float dWE = (lNW + lSW) - (lNE + lSE);
  float rcpMin = 1.0 / (min(abs(dNS), abs(dWE)) + reduce);
  vec2 dir = min(max(vec2(-dNS, dWE) * rcpMin, C250.ww), C250.zz);
  vec3 inner = Tap(uv + dir * C253.yx) + Tap(uv + dir * C253.zw);
  vec3 outer = Tap(uv + dir * C254.xy) + Tap(uv + dir * C254.zw);
  vec3 two = inner * C251.w;
  vec3 four = inner * C250.x + outer * C250.x;
  float lFour = Luma(four);
  float lMin = min(min(min(lSW, lSE), min(lNW, lNE)), lM);
  float lMax = max(max(max(lSW, lSE), max(lNW, lNE)), lM);
  return (lMin > lFour || lFour > lMax) ? two : four;
}

#ifdef ENHANCED
// The disc blur: radius at full blur as a share of the picture's height (5 pixels at 720p, 10 at 1440p), taps on a golden-angle
// spiral, and how much more a bright tap counts (bright spots spread into discs, as through a lens). 32 taps alone cost
// 2.3x the game's pass at 1440p (run 4535); 16 taps plus the game's quarter-size blur for the inside.
const int kBokehTaps = 16;
const float kBokehInside = 6.0;  // how many taps the game's quarter-size blur counts as
const float kBokehRadius = 0.007;
const float kBokehHighlight = 6.0;

ivec2 g_size;
// How blurred the background is at a depth, 0 to 1: the game's own ramp for its background blend (c21.y, c20.y) up to
// its cap (c20.w: the game animates it, 0.5 to 0.8 seen; about 0 where it does not blur), full blur at 0.8.
float FarBlur(float depth) {
  float z = C252.w - depth;
  return min(clamp((z - C21.y) * C20.y + C21.z, 0.0, max(C20.w, 0.0)) * 1.25, 1.0);
}

vec3 Bokeh(vec2 uv, vec3 centre, float blur) {
  float radius = blur * kBokehRadius * float(g_size.y);
  float maxRadius = kBokehRadius * float(g_size.y);
  vec3 sum = centre;
  float weights = 1.0;
  // The game's own quarter-size background blur (already made each frame) stands in for the disc's inside, so few
  // full-size taps are enough; its alpha is 1 where it holds blurred background (0 at in-focus pixels it passed
  // through).
  vec4 inside = Lod(t_blur_b, s_blur_b, uv);
  sum += inside.rgb * (kBokehInside * inside.a);
  weights += kBokehInside * inside.a;
  // Golden-angle spiral: each tap turns by the golden angle and steps out so the taps cover the disc evenly.
  const float kGoldenCos = -0.7373688780783197;
  const float kGoldenSin = 0.6754902942615238;
  // Each pixel's spiral is 15% larger or smaller (interleaved gradient noise, Jimenez 2014), which smears the faint
  // copies of the picture that few taps leave. Turning the spiral per pixel instead hides them better but cost 0.1 ms
  // more at 1440p (run 4540: neighbouring pixels no longer read neighbouring texels).
  radius *= 0.85 + 0.3 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  vec2 dir = vec2(1.0, 0.0);
  vec2 size = vec2(g_size);
  // Taps by the radius in pixels (a 5-pixel disc at 720p gets 8, a 10-pixel one at 1440p 16).
  int taps = int(clamp(ceil(radius * 1.6), 4.0, float(kBokehTaps)));
  for (int i = 0; i < taps; ++i) {
    float r = sqrt((float(i) + 0.5) / float(taps)) * radius;
    vec2 p = uv * size + dir * r;
    // Colour through the game's (linear) sampler: each tap averages its 2x2 texels, which keeps the disc smooth.
    vec3 c = Tap(p / size);
    float tapBlur = FarBlur(Fetch(t_depth, s_depth, clamp(ivec2(p), ivec2(0), g_size - 1)).x);
    // A tap counts where its own blur reaches this pixel: the in-focus wrestlers (no blur) stay out of the background.
    float w = clamp(tapBlur * maxRadius - r + 1.0, 0.0, 1.0);
    w *= 1.0 + kBokehHighlight * max(Luma(c) - 0.6, 0.0);
    sum += c * w;
    weights += w;
    dir = vec2(dir.x * kGoldenCos - dir.y * kGoldenSin, dir.x * kGoldenSin + dir.y * kGoldenCos);
  }
  return sum / weights;
}

// Cubic B-spline scaling up of a small texture through its linear sampler: four bilinear taps placed so their blend
// gives the 4x4 texel B-spline (smooth, no texel grid).
vec3 BSpline(texture2DArray t, sampler s, vec2 uv) {
  vec2 size = vec2(Size(t, s));
  vec2 p = uv * size - 0.5;
  vec2 i = floor(p);
  vec2 f = p - i;
  vec2 f2 = f * f;
  vec2 f3 = f2 * f;
  vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
  vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
  vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
  vec2 w3 = f3 / 6.0;
  vec2 g0 = w0 + w1;
  vec2 g1 = w2 + w3;
  vec2 c0 = (i - 0.5 + w1 / g0) / size;
  vec2 c1 = (i + 1.5 + w3 / g1) / size;
  return g0.y * (g0.x * Lod(t, s, vec2(c0.x, c0.y)).rgb +
                 g1.x * Lod(t, s, vec2(c1.x, c0.y)).rgb) +
         g1.y * (g0.x * Lod(t, s, vec2(c0.x, c1.y)).rgb +
                 g1.x * Lod(t, s, vec2(c1.x, c1.y)).rgb);
}
#endif

void main() {
  vec2 uv = in_texcoord.xy;
  vec3 color = ConsoleFxaa(uv, in_texcoord.zw);
  float one = C252.w;
  float depth = Lod(t_depth, s_depth, uv).x;
  float z = one - depth;
  if (C21.x > z) {
    // Right in front of the camera: toward the near blur.
    float f = clamp((C21.x - z) * C20.x + C21.z, C21.z, one);
    color += (Lod(t_blur_a, s_blur_a, uv).rgb - color) * f;
  } else {
#ifdef ENHANCED
    g_size = Size(t_scene, s_scene);
    float blur = FarBlur(depth);
    if (blur > 0.0) {
      // Fades in over the first pixel of radius, so the edge of the blurred range does not step.
      color = mix(color, Bokeh(uv, color, blur), clamp(blur * kBokehRadius * float(g_size.y), 0.0, 1.0));
    }
#else
    // The background: toward the far blur, capped (c20.w).
    float f = clamp((z - C21.y) * C20.y + C21.z, C21.z, C20.w);
    color += (Lod(t_blur_b, s_blur_b, uv).rgb - color) * f;
#endif
  }
  // The depth tint toward c27, by a ramp on depth.
  float span = one - C30.x;
  float k = C29.x * (1.0 / span) * max(span - depth, C255.x);
  color += (C27.xyz - color) * k;
#ifdef ENTRANCE
  // Bloom and the star glare, added by their strengths (often 0: then not read at all). Both are 1/8 size (160x90 at
  // 720p); Enhanced scales them up with a smooth cubic filter instead of a bilinear one, which shows the texel grid.
#ifdef ENHANCED
  if (C31.x != 0.0) color += BSpline(t_bloom, s_bloom, uv) * C31.x;
  if (C32.x != 0.0) color += BSpline(t_glare, s_glare, uv) * C32.x;
#else
  if (C31.x != 0.0) color += Lod(t_bloom, s_bloom, uv).rgb * C31.x;
  if (C32.x != 0.0) color += Lod(t_glare, s_glare, uv).rgb * C32.x;
#endif
#endif
  out_color = vec4(color, 1.0);
}
