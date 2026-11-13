#version 450
// Settings parity S10b (DLSS, the wrestlers' own motion): ours, not a game effect. Paired with the character vertex shaders' VELOCITY twins
// (tools\renderer\compile_shaders.py), which give this tick's clip position and the previous tick's (its bones and camera) for each vertex.
// Writes, for the surface this pass sees: xy the move in the game's NDC (previous minus this, y up, no jitter: both positions are before the
// jitter), z its depth (as the scene's depth buffer holds it), w 1 = a wrestler pixel. rdr_dlss_motion.frag uses it where the scene's depth is
// this surface's (nothing in front), the camera's motion elsewhere.
layout(location = 14) in vec4 fx_vel_now;
layout(location = 15) in vec4 fx_vel_prev;
layout(location = 0) out vec4 out_motion;

void main() {
  vec2 now = fx_vel_now.w != 0.0 ? fx_vel_now.xy / fx_vel_now.w : vec2(0.0);
  vec2 prev = fx_vel_prev.w != 0.0 ? fx_vel_prev.xy / fx_vel_prev.w : now;
  out_motion = vec4(prev - now, gl_FragCoord.z, 1.0);
}
