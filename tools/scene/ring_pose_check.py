#!/usr/bin/env python3
"""S103 lane C4 gate check: builds tools\\scene\\ring_pose_unit.cpp (src\\renderer_scene\\ring_pose.h: the file's slot -> node map and the CPU skin with a call's rows, hand-worked numbers), runs it, then its planted
twins (each must exit 1 and fail its NAMED check). 0 game runs.   py -I tools\\scene\\ring_pose_check.py [--root DIR]"""
import argparse
import os
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {'slot_identity': 'pose.slot_to_node', 'rigid_node_off': 'pose.rigid', 'mirror_off': 'pose.mirror_space', 'hash_short': 'pose.skip_unchanged', 'pool_by_resolved': 'pose.pool_names', 'pad_old': 'pose.instance_keys',
          'gpu_slot_off': 'gpu.matches_cpu', 'gpu_first_node': 'gpu.matches_cpu', 'gpu_weights_off': 'gpu.two_pair_rope'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'c4', 'pose')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'ring_pose_unit.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld', os.path.join(root, 'tools', 'scene', 'ring_pose_unit.cpp'), os.path.join(root, 'src', 'renderer_scene', 'char_effects.cpp'), os.path.join(root, 'src', 'renderer', 'shaders', 'spv', 'fx_spv_data.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL: the unit test failed')
        return 1
    bad = 0
    for plant, name in PLANTS.items():
        p = subprocess.run([exe, '--plant', plant], capture_output=True, text=True)
        ok = p.returncode != 0 and any(ln.startswith('FAIL') and name in ln for ln in p.stdout.splitlines())
        print('twin %-16s %s (%s)' % (plant, 'caught' if ok else 'NOT CAUGHT', name))
        bad += 0 if ok else 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
