#!/usr/bin/env python3
"""S103 PN23F lint (review A1): the picture source's DEATH hook is the registry's SLOT FREE, sub_82BAF210, which every release path calls.

  py -I tools\\scene\\picture_death_lint.py --root .

Re-derives from the recompiler's call graph (tools\\q, never from memory):
  1. `q callers 0x82BAF210`: the caller functions equal the acknowledged set below (a NEW free path is a reviewed change), every site is a plain `call` (a tail call / branch FAILS), the data words that point at the
     function equal the acknowledged set (an indirect caller still goes through the hook: it hooks the callee);
  2. every caller function that loads the registry global ([0x8392DD9C] = lis 0x8393 + lwz -8804) calls the slot free by a `bl 0x82baf210` in its own listing (tools\\analysis\\list_function.py, run in the main
     checkout where generated\\ lives): so the hooked function covers it;
  3. picture_hooks.h hooks sub_82BAF210 (and NOT a partial path such as sub_82294FD0), reads r3 (the manager) and the key at r4 BEFORE the original and reports r3 AFTER it; picture_tap.h OnSlotFreed reports a death only
     for the texture registry's manager and a matched free (r3 != 0).
  4. planted twins (self-test, in this run): a hook text on sub_82294FD0 only, a hook that does not read r3 after the call, and a tap without the manager check each FAIL their named rule.
Fails loudly when q or the listing cannot run: never a silent pass. 0 game runs."""
import argparse
import os
import re
import subprocess
import sys

# q callers 0x82BAF210 (S103, 15 sites in 13 functions); every one reaches the slot free through the hooked function.
ACK_CALLERS = {'sub_82294FD0', 'sub_82295050', 'sub_82297A40', 'sub_82297BE8', 'sub_823E7720', 'sub_823E7978', 'sub_823E7F48', 'sub_827CAFA8', 'sub_82813380', 'sub_82B114E0', 'sub_82BAA2E0', 'sub_82BAF160', 'sub_82BAF8D0'}
ACK_DATA_WORDS = {0x8220FEE0}   # a function-pointer word (an indirect caller): it calls the same function, so the hook covers it


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def hook_problems(hooks, tap):
    bad = []
    m = re.search(r'REX_HOOK_RAW\(sub_82BAF210\)\s*\{(.*?)\n\}', hooks, re.S)
    if not m:
        bad.append('hook: picture_hooks.h does not hook sub_82BAF210')
    else:
        body = m.group(1)
        call = body.find('__imp__sub_82BAF210(')
        if call < 0:
            bad.append('hook: the sub_82BAF210 hook does not call the original')
        else:
            before, after = body[:call], body[call:]
            if 'ctx.r3.u32' not in before or 'ctx.r4.u32' not in before:
                bad.append('hook: r3 (the manager) and r4 (the key pointer) are not read before the original')
            if 'ctx.r3.u32' not in after.split('__imp__sub_82BAF210(', 1)[1]:
                bad.append('hook: r3 after the original (the matched object) is not reported')
            if 'OnSlotFreed(' not in after:
                bad.append('hook: OnSlotFreed is not called after the original')
    if re.search(r'REX_HOOK_RAW\(sub_82294FD0\)', hooks):
        bad.append('hook: sub_82294FD0 is hooked: it is 1 of 13 free paths, the death is the slot free')
    t = re.search(r'inline void OnSlotFreed\((.*?)\n\}', tap, re.S)
    if not t:
        bad.append('tap: OnSlotFreed not found in picture_tap.h')
    else:
        body = t.group(1)
        if 'kTextureHandleGlobal' not in body or 'registry != manager' not in body:
            bad.append('tap: OnSlotFreed does not compare the manager with the texture registry [0x8392DD9C]')
        if '!freed' not in body:
            bad.append('tap: OnSlotFreed does not require a matched free (r3 != 0)')
    return bad


def run_q(root):
    r = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'q', 'q.py'), 'callers', '0x82BAF210', '--limit', '200'], capture_output=True, text=True, timeout=120, cwd=root)
    if r.returncode or 'sub_82BAF210:' not in r.stdout:
        print('FAIL: q callers did not run: %s %s' % (r.returncode, (r.stderr or r.stdout)[-300:]))
        return None
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    hooks = open(os.path.join(root, 'src', 'scene_live', 'picture_hooks.h'), encoding='utf-8').read()
    tap = open(os.path.join(root, 'src', 'scene_live', 'picture_tap.h'), encoding='utf-8').read()
    bad = []
    # 3 + 4: the hook text, and the planted twins
    bad += hook_problems(hooks, tap)
    twins = {
        'death hooked on sub_82294FD0 only': (hooks.replace('REX_HOOK_RAW(sub_82BAF210)', 'REX_HOOK_RAW(sub_82294FD0)'), tap, 'does not hook sub_82BAF210'),
        'matched object not reported': (hooks.replace('OnSlotFreed(manager, key, ctx.r3.u32)', 'OnSlotFreed(manager, key, 1)'), tap, 'r3 after the original'),
        'manager not compared': (hooks, tap.replace('registry != manager', 'false'), 'does not compare the manager'),
        'matched free not required': (hooks, tap.replace('!freed', 'false'), 'matched free'),
    }
    for name, (h, t, expect) in twins.items():
        got = hook_problems(h, t)
        ok = any(expect in g for g in got)
        print('%s twin "%s": %s' % ('PASS' if ok else 'FAIL', name, 'caught (%s)' % expect if ok else 'NOT caught: %s' % got))
        bad += [] if ok else ['twin %s not caught' % name]
    # 1: q callers
    out = run_q(root)
    if out is None:
        return 1
    funcs, kinds_bad, data = set(), [], set()
    for ln in out.splitlines():
        m = re.match(r'\s+(sub_[0-9A-F]{8})\s+(.*?)(\s+\[ran:.*)?$', ln)
        if m and not ln.strip().startswith('data words'):
            funcs.add(m.group(1))
            for s, kind in re.findall(r'0x([0-9A-F]{8}) (\w+)', m.group(2)):
                if kind != 'call':
                    kinds_bad.append('0x%s %s in %s' % (s, kind, m.group(1)))
        d = re.match(r'\s+data words pointing at it: \d+: (.*)', ln)
        if d:
            data = {int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{8}', d.group(1))}
        if 'vtable slot:' in ln:
            bad.append('q lists a vtable slot for sub_82BAF210 (an indirect caller by slot: review): %s' % ln.strip())
    head = re.search(r'sub_82BAF210: (\d+) call sites in (\d+) functions', out)
    if head and int(head.group(2)) != len(funcs):
        bad.append('q says %s caller functions, parsed %d' % (head.group(2), len(funcs)))
    for f in sorted(funcs - ACK_CALLERS):
        bad.append('NEW caller of the slot free: %s (review it, then add it to ACK_CALLERS)' % f)
    for f in sorted(ACK_CALLERS - funcs):
        bad.append('acknowledged caller %s is no longer a caller (update ACK_CALLERS)' % f)
    bad += ['caller site is not a plain call: %s' % k for k in kinds_bad]
    for d in sorted(data - ACK_DATA_WORDS):
        bad.append('a NEW data word points at sub_82BAF210: 0x%08X' % d)
    # 2: every caller that loads the registry calls the free by a bl in its own listing
    main_co = main_checkout(root)
    loaders = 0
    for f in sorted(funcs):
        r = subprocess.run([sys.executable, '-I', os.path.join(main_co, 'tools', 'analysis', 'list_function.py'), f[4:]], capture_output=True, text=True, timeout=120, cwd=main_co)
        if r.returncode or 'instructions' not in r.stdout:
            bad.append('list_function could not list %s: %s' % (f, (r.stderr or r.stdout)[-150:]))
            continue
        if '-8804(' in r.stdout:
            loaders += 1
            if 'bl 0x82baf210' not in r.stdout:
                bad.append('%s loads the registry but does not call the slot free by a bl (another free path?)' % f)
    for b in bad:
        print('FAIL: ' + b)
    print('%s: picture_death_lint: %d caller functions (q callers), %d load the registry [0x8392DD9C] and call the hooked slot free, data words %s' % ('FAIL' if bad else 'PASS', len(funcs), loaders, sorted('0x%08X' % d for d in data)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
