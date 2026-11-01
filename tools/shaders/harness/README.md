# Shader arithmetic harness (Session 80, OWN_RENDERER_PLAN.md)

Checks one of our replacement pixel shaders (`src\scene\*.frag`) against the SDK translator's own SPIR-V of the game
shader it replaces, on the GPU: both are turned into compute shaders that read their interpolators and float constants
from a buffer (textures replaced by smooth fake functions), run over thousands of random inputs, and compared.
It proves the arithmetic and the channel mapping only - texture formats, sign modes, LOD and derivatives are checked
by same-frame shots. Written by the agents that drafted `wrestler_body.frag` / `wrestler_body_entrance.frag`.

- `mkharness.py mine|ref <src> <out.comp>` - our shader or the spirv-cross text of the translation -> a compute shader
  (`*_ent.py`: the entrance variant's 38-constant layout). Per-shader constants (record size, constant count) are at the
  top of each script: adapt them for a new material.
- `gen_inputs.py` - random input records; `vkrun.cpp` - a minimal Vulkan compute runner (build it yourself; no binary
  is committed); `compare2.py` - relative error per output; `trace.py` - a symbolic trace of a microcode listing (reads
  the local dumps at run time).
- Inputs come from the local, never-committed dumps (`out\s60` / `out\s66` / `out\s70`); nothing from the game is in here.
