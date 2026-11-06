r"""Session 94: regenerate the three transcode SPIR-V headers from texture_transcode_cs.glsl (the SDK worktree in
TRANSCODE_SDK, default thirdparty\sdk-wt\s94-tex2). Extra defines per header: pass --extra DEFINE for variants."""
import os, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
SDK = Path(os.environ.get('TRANSCODE_SDK') or ROOT / 'thirdparty' / 'sdk-wt' / 's94-tex2')
glsl = SDK / 'src/graphics/shaders/vulkan_glsl/texture_transcode_cs.glsl'
for define, name in [('TRANSCODE_BC1_ASTC', 'texture_load_dxt1_astc4x4_cs'), ('TRANSCODE_BC1_ASTC,BC1_ALPHA_FIX', 'texture_load_dxt1_astc4x4_alphafix_cs'), ('TRANSCODE_BC4_EAC', 'texture_load_dxt5a_eac_r11_cs'),
                     ('TRANSCODE_BC5_EAC', 'texture_load_dxn_eac_rg11_cs')]:
    out = SDK / f'src/graphics/shaders/vulkan_spirv/{name}.h'
    subprocess.run([sys.executable, str(ROOT / 'tools/perf/gen_spirv_header.py'), str(glsl), name, define, str(out),
                    '--session', 'Session 94'], check=True)
