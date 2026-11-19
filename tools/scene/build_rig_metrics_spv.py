#!/usr/bin/env python3
r"""Builds tools\scene\rig_metrics_spv.h (kSpv_rig_metrics_comp) from tools\scene\rig_metrics.comp: the offline rig's GPU picture-compare shader (S103 PS1). Our own shader, our own header (committed).
Same tools and target as tools\renderer\compile_shaders.py: glslangValidator -V --target-env vulkan1.1, spirv-opt -O --strip-debug, spirv-val.

  py -I tools\scene\build_rig_metrics_spv.py [--check]

--check: exit 1 when the header is not what the source compiles to (a gate check: s103_L.rig_metrics_spv).
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'rig_metrics.comp')
OUT = os.path.join(HERE, 'rig_metrics_spv.h')
SDK_ROOT = r'C:\VulkanSDK'
SDK = os.path.join(SDK_ROOT, sorted(os.listdir(SDK_ROOT))[-1], 'Bin')


def run(tool, *args):
    done = subprocess.run([os.path.join(SDK, tool), *args], capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit('%s failed:\n%s%s' % (tool, done.stdout, done.stderr))


def words():
    with tempfile.TemporaryDirectory() as tmp:
        raw, opt = os.path.join(tmp, 'raw.spv'), os.path.join(tmp, 'opt.spv')
        run('glslangValidator.exe', '-V', '--target-env', 'vulkan1.1', '-o', raw, SRC)
        run('spirv-opt.exe', '-O', '--strip-debug', '--target-env=vulkan1.1', '-o', opt, raw)
        run('spirv-val.exe', '--target-env', 'vulkan1.1', opt)
        data = open(opt, 'rb').read()
    return list(struct.unpack('<%dI' % (len(data) // 4), data))


def header(ws):
    rows = []
    for i in range(0, len(ws), 8):
        rows.append(', '.join('0x%08x' % w for w in ws[i:i + 8]) + ',')
    return ('// Generated from rig_metrics.comp by tools\\scene\\build_rig_metrics_spv.py. Our own shader.\n#pragma once\n#include <cstdint>\n'
            'inline constexpr uint32_t kSpv_rig_metrics_comp[] = {\n' + '\n'.join(rows) + '\n};\n')


def main():
    text = header(words())
    if '--check' in sys.argv:
        have = open(OUT, encoding='utf8', newline='').read().replace('\r\n', '\n') if os.path.exists(OUT) else ''
        ok = have == text
        print('%s  rig_metrics_spv.h is %s' % ('PASS' if ok else 'FAIL', 'current' if ok else 'STALE: py -I tools\\scene\\build_rig_metrics_spv.py'))
        return 0 if ok else 1
    open(OUT, 'w', encoding='utf8', newline='\n').write(text)
    print('wrote %s (%d words)' % (OUT, len(re.findall('0x', text))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
