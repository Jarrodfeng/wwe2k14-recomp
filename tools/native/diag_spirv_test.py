"""Session 95: offline test of the diagnostic run's SPIR-V passes. No game, no GPU.

    py tools\\native\\diag_spirv_test.py --sdk C:\\...\\thirdparty\\sdk-wt\\s95-diag2 [--gpu]

Builds tools\\native\\diag_spirv_test.cpp with diag_spirv.cpp, compiles a few vertex shaders of our own (the module shapes the game's
modules have: a gl_PerVertex block with and without gl_PointSize, interpolator arrays, early returns, a loose gl_Position written in
assembly form, gl_VertexIndex absent) with glslang, patches each, and runs spirv-val on the patched module and on the fragment modules.
Then links them by name of location: the fragment module's inputs must be the locations the patch reports.

--gpu also runs the capture on this PC's GPU (no game): tools\\native\\diag_capture_gpu_test.cpp, our own test shader through the patch, drawn as the
diagnostic run draws it, every vertex's position bits read back and compared, under the validation layer.
"""
import argparse
import glob
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CASES = {
    "plain": """#version 450
layout(location=0) out vec4 a; layout(location=1) out vec2 b;
void main(){ a=vec4(gl_VertexIndex); b=vec2(1.0); gl_Position=vec4(float(gl_VertexIndex),0.0,0.0,1.0); }""",
    "pointsize": """#version 450
layout(location=0) out vec4 a;
void main(){ a=vec4(1.0); gl_PointSize=3.0; gl_Position=vec4(float(gl_VertexIndex),0.0,0.0,1.0); }""",
    "interp_array": """#version 450
layout(location=0) out vec4 interp[12];
void main(){ for(int i=0;i<12;++i) interp[i]=vec4(i); gl_Position=vec4(float(gl_VertexIndex)); }""",
    "early_return": """#version 450
layout(location=0) out vec4 a;
void main(){ a=vec4(0.0); gl_Position=vec4(1.0); if(gl_VertexIndex==3) { gl_Position=vec4(2.0); return; } gl_Position=vec4(3.0); }""",
    "no_vertex_index": """#version 450
layout(location=0) in vec4 p; layout(location=0) out vec4 a;
void main(){ a=p; gl_Position=p; }""",
    "clip_distance": """#version 450
out float gl_ClipDistance[2];
layout(location=0) out vec4 a;
void main(){ a=vec4(1.0); gl_ClipDistance[0]=1.0; gl_ClipDistance[1]=0.0; gl_Position=vec4(1.0); }""",
    "ssbo_fetch": """#version 450
layout(set=0,binding=0,std430) readonly buffer M { uint d[]; } mem;
layout(set=1,binding=0) uniform U { vec4 c[4]; mat4 m; } u;
layout(location=0) out vec4 a;
void main(){ uint i=uint(gl_VertexIndex)*8u; vec4 p=vec4(uintBitsToFloat(mem.d[i]),uintBitsToFloat(mem.d[i+1u]),uintBitsToFloat(mem.d[i+2u]),1.0);
 a=u.c[1]; gl_Position=u.m*p; }""",
}

# The --gpu test's shader: a position whose bits depend on the index only (bits are the point; diag_capture_gpu_test.cpp expects these).
GPU_SHADER = """#version 450
layout(location=0) out vec4 a;
void main(){ uint raw=uint(gl_VertexIndex);
 uint i=((raw>>24)&255u)|((raw>>8)&65280u);
 vec4 p=vec4(uintBitsToFloat(raw),uintBitsToFloat(raw^0x3F800000u),uintBitsToFloat(0x7FC00000u|(i&7u)),uintBitsToFloat(1u));
 a=p; gl_Position=p; }"""

# A module with gl_Position as a loose variable (glslang always makes a block): assembled by hand.
LOOSE_ASM = """
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %pos
OpDecorate %pos BuiltIn Position
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%ptr = OpTypePointer Output %v4
%pos = OpVariable %ptr Output
%one = OpConstant %float 1
%c = OpConstantComposite %v4 %one %one %one %one
%main = OpFunction %void None %fn
%l = OpLabel
OpStore %pos %c
OpReturn
OpFunctionEnd
"""


def tool(name):
    c = sorted(glob.glob(r"C:\VulkanSDK\*\Bin\%s.exe" % name), reverse=True)
    if c:
        return c[0]
    p = shutil.which(name)
    if not p:
        raise SystemExit(name + " not found (Vulkan SDK)")
    return p


def run(*args):
    return subprocess.run([str(a) for a in args], capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--gpu", action="store_true")
    args = ap.parse_args()
    native = Path(args.sdk) / "src" / "graphics" / "vulkan" / "native"
    here = Path(__file__).resolve().parent
    glslang, spirv_val, spirv_as = tool("glslangValidator"), tool("spirv-val"), tool("spirv-as")
    clang = shutil.which("clang++") or r"C:\Program Files\LLVM\bin\clang++.exe"
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        exe = t / "diag_spirv_test.exe"
        r = run(clang, "-std=c++20", "-O1", "-I", native, here / "diag_spirv_test.cpp", native / "diag_spirv.cpp", "-o", exe)
        if r.returncode:
            print(r.stdout, r.stderr)
            raise SystemExit("build failed")
        modules = {}
        for name, src in CASES.items():
            (t / (name + ".vert")).write_text(src)
            r = run(glslang, "-V", t / (name + ".vert"), "-o", t / (name + ".spv"))
            if r.returncode:
                print("glslang failed on", name, r.stdout)
                fails += 1
                continue
            modules[name] = t / (name + ".spv")
        (t / "loose.spvasm").write_text(LOOSE_ASM)
        r = run(spirv_as, "--target-env", "vulkan1.0", t / "loose.spvasm", "-o", t / "loose.spv")
        if r.returncode:
            print("spirv-as failed", r.stderr)
            fails += 1
        else:
            modules["loose_position"] = t / "loose.spv"
        for name, spv in modules.items():
            o, cf, sf = t / (name + ".o.spv"), t / (name + ".cf.spv"), t / (name + ".sf.spv")
            r = run(exe, spv, o, cf, sf, 16)
            line = r.stdout.strip()
            if r.returncode:
                print("FAIL %-16s patch: %s" % (name, line))
                fails += 1
                continue
            bad = [(n, run(spirv_val, "--target-env", "vulkan1.0", f)) for n, f in (("vertex", o), ("capture fs", cf), ("solo fs", sf))]
            bad = [(n, x.stderr.strip() or x.stdout.strip()) for n, x in bad if x.returncode]
            if bad:
                print("FAIL %-16s %s" % (name, bad))
                fails += 1
            else:
                print("ok   %-16s %s" % (name, line))
        # refused: no room
        r = run(exe, modules["interp_array"], t / "x.spv", t / "y.spv", t / "z.spv", 13)
        print(("ok   " if r.returncode == 3 else "FAIL ") + "no room: " + r.stdout.strip())
        fails += r.returncode != 3
        if args.gpu:
            sdk_root = Path(tool("spirv-val")).parent.parent
            gexe = t / "diag_capture_gpu_test.exe"
            r = run(clang, "-std=c++20", "-O1", "-I", native, "-I", sdk_root / "Include", here / "diag_capture_gpu_test.cpp", native / "diag_spirv.cpp",
                    sdk_root / "Lib" / "vulkan-1.lib", "-o", gexe)
            if r.returncode:
                print(r.stdout, r.stderr)
                raise SystemExit("gpu test build failed")
            (t / "gpu.vert").write_text(GPU_SHADER)
            r = run(glslang, "-V", t / "gpu.vert", "-o", t / "gpu.spv")
            if r.returncode:
                print(r.stdout)
                raise SystemExit("glslang failed on the gpu shader")
            r = run(gexe, t / "gpu.spv")
            print(r.stdout.strip())
            fails += r.returncode != 0
    print("diag_spirv_test: %s" % ("PASS" if not fails else "%d FAILED" % fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
