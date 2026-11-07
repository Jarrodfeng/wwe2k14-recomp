Session 95: offline proof that a LEAN wrestler material equals the plain one (gpu_native_wrestler_lean).
Run from a scratch folder holding these scripts:  bash any.sh <body|head|skin|skin_plain|skin_decal|hair>
pp.py resolves #ifdef LEAN, gen_any.py makes 65,536 random records (gates forced to zero in 1/5..1/31 of them, fake textures with exact
zeros), mkh_n.py/mkharness.py (tools/shaders/harness) turn both into compute shaders, vkrun (build it, see README.md there) runs them,
cmp4.py prints the largest difference. Paths inside any.sh are absolute to Jarrod's PC; edit W and V.
