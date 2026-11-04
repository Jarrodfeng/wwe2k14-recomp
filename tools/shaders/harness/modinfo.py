#!/usr/bin/env python3
"""Session 87 (stage 4): prints the vertex fetch part of the SDK translation of a guest vertex shader (a --dump_shaders module,
through spirv-cross): the fetch constant, the stride, and each attribute load with the register it lands in, so a replacement
can be written for its layout. Our own tool; reads local dumps at run time.
  modinfo.py <hash> [module-suffix-substring]     (hash = the guest vertex shader's, e.g. 0BBBF514D70CF0F4)
"""
import glob, os, re, subprocess, sys

DUMP = r'C:\Users\Jarrod\wwe2k14-recomp\out\s70\shader_dump'
SDK = r'C:\VulkanSDK'

def cross_bin():
    for d in sorted(os.listdir(SDK), reverse=True):
        p = os.path.join(SDK, d, 'Bin', 'spirv-cross.exe')
        if os.path.exists(p):
            return p
    raise SystemExit('no spirv-cross')

def module_for(h, sub=''):
    files = sorted(glob.glob(os.path.join(DUMP, 'shader_%s_*.vk.bin.vert' % h)))
    files = [f for f in files if sub in f]
    if not files:
        raise SystemExit('no module for ' + h)
    # prefer the native FMA one if the substring did not pick
    for f in files:
        if '_e00000002' in f:
            return f
    return files[0]

def main():
    h = sys.argv[1]
    f = module_for(h, sys.argv[2] if len(sys.argv) > 2 else '')
    text = subprocess.run([cross_bin(), '--version', '450', '--vulkan-semantics', f], capture_output=True, text=True).stdout
    print('module', os.path.basename(f))
    m = re.search(r'float_constants\[(\d+)\]', text)
    print('float constants:', m.group(1) if m else 'none (or dynamic)')
    body = text[text.index('void main()'):]
    for line in body.splitlines():
        s = line.strip()
        if 'fetch_constants[' in s and 'vfetch' in s or 'xe_var_vfetch_address =' in s:
            print('  ', s[:200])
        elif 'shared_memory[' in s or 'unpackHalf2x16' in s or 'bitfieldExtract(_' in s and 'ivec' in s:
            print('    ', s[:230])
        elif re.match(r'xe_var_registers\[\d\] = ', s) and ('_' in s) and 'vec4(0.0)' not in s and len(s) < 120 and 'float_constants' not in s and '_' in s.split('=')[1]:
            pass
        if s.startswith('precise ') or s.startswith('gl_Position'):
            break

if __name__ == '__main__':
    main()
