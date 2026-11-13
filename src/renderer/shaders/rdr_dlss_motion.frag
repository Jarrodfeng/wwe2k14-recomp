#version 450
// Settings parity S10b: DLSS's motion vectors, the camera's (static world): each pixel's position rebuilt from the scene's depth in the game's
// own clip space (its ViewProj, D3D style: y up, z = depth), taken to the last frame's clip space by clip_to_prev = ViewProj(previous) *
// inverse(ViewProj(this)), and the difference in render pixels, previous minus this, the jitter removed (what DLSS wants: mvecScale makes it
// normalised). Where a wrestler is the visible surface, its own motion instead (object_motion: the binder's velocity pass, yVelocity.frag).
layout(set = 0, binding = 1, std140) uniform MotionConstants {
  vec4 clip_to_prev[4];  // rows: prev_clip = M * clip
  vec4 size;             // render width, height, 1 / width, 1 / height
  vec4 jitter;           // xy: this frame's jitter in render pixels; z: 1 = object_motion holds the wrestlers' own motion
} mc;
layout(set = 1, binding = 0) uniform sampler2D scene_depth;
layout(set = 1, binding = 1) uniform sampler2D object_motion;  // yVelocity.frag: xy NDC move, z depth, w 1 = a wrestler
layout(location = 0) out vec4 out_motion;

void main() {
  vec2 pixel = gl_FragCoord.xy;  // pixel centres, top-left origin (the game's window)
  float depth = texelFetch(scene_depth, ivec2(pixel), 0).r;
  vec2 p = pixel - mc.jitter.xy;
  // The wrestlers' own motion where a wrestler is the visible surface (its depth is the scene's here: nothing in front of it).
  if (mc.jitter.z > 0.5) {
    vec4 object = texelFetch(object_motion, ivec2(pixel), 0);
    if (object.w > 0.5 && abs(depth - object.z) <= max(1e-3 * abs(object.z), 1e-6)) {
      out_motion = vec4(object.x * 0.5 * mc.size.x, -object.y * 0.5 * mc.size.y, 0.0, 0.0);
      return;
    }
  }
  vec4 clip = vec4(p.x * 2.0 * mc.size.z - 1.0, 1.0 - p.y * 2.0 * mc.size.w, depth, 1.0);
  vec4 prev = vec4(dot(mc.clip_to_prev[0], clip), dot(mc.clip_to_prev[1], clip), dot(mc.clip_to_prev[2], clip), dot(mc.clip_to_prev[3], clip));
  if (abs(prev.w) < 1e-6) {
    out_motion = vec4(0.0);
    return;
  }
  vec2 ndc = prev.xy / prev.w;
  vec2 prev_pixel = vec2((ndc.x + 1.0) * 0.5 * mc.size.x, (1.0 - ndc.y) * 0.5 * mc.size.y);
  out_motion = vec4(prev_pixel - p, 0.0, 0.0);
}
