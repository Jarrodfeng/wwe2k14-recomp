Offline pass tests (Session 82, harness builder) - iterate a new compute / merged pass without booting the game.

Parts
  run_compute.cpp / build.ps1   standalone Vulkan compute runner (Vulkan SDK + the repo's clang; built on first use into
                                out\native\compute_test\run_compute.exe): binds sampled images, storage images, SSBOs / UBOs,
                                dispatches, reads back, prints the dispatch time (GPU timestamps).
  compute_test.py               compiles your GLSL (glslc) or takes SPIR-V, runs it on dumped inputs, compares with the recorded
                                real output (max / mean difference, share over a tolerance, worst pixel): PASS / FAIL.
  dump_pass.ps1 / rdc_dump_pass.py   "capture a pass once": from a RenderDoc capture (texdiff.ps1 makes them, or the cvar
                                gpu_native_debug_rdc_now) dump one draw's / dispatch's sampled textures, read-write textures and
                                targets, raw, with a manifest (5 s, no game).

Loop
  1. Once per pass: & .\tools\native\compute_test\dump_pass.ps1 -Rdc <capture.rdc> -Ps <pixel shader hash from texdiff> [-Nth 0]
     (or -Draw N / -Event ID) -Out out\native\compute_test\<pass>. The manifest names every file, its size and format.
     (On a draw, the "in c" / "rw c" rows are the compute bindings left over from the last dispatch - ignore them.)
  2. Write <pass>.comp and a SPEC.json (format: compute_test.py's header): bind the dumped inputs (kind tex = sampled, nearest /
     clamp) and a storage image for the output with "expect": "out0.bin".
  3. py -I tools\native\compute_test\compute_test.py SPEC.json     (seconds; edit the shader, repeat)
  4. Only when it PASSES within the tolerance you accept (say how big it is in the report): wire it into the SDK and run the
     finished piece's owned_check.
  py -I tools\native\compute_test\compute_test.py --self-test   proves the chain (a 3x3 box blur vs numpy; max diff 1).

Limits (say them in a report rather than trust a green test blindly)
  - Depth formats (D32S8 etc.) are dumped raw but not bindable as depth: bind a colour copy (e.g. r32f) or test colour passes.
  - Compressed textures (BC1 ...) are dumped compressed: a test shader cannot sample them here; use passes on RGBA8 / float images.
  - One descriptor set (0), no mips / arrays / 3D, nearest sampling only, one push-constant block.
  - A dumped capture is one frame: the recorded output is that frame's, not a tolerance proof across the replay.
  - Game imagery: the dumps stay under out\ (local, gitignored); nothing from them is committed.
