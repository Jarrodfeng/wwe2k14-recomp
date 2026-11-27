r"""S102 lane K2: the unit proof of the Pruner's no-group mode and the wiring lints. 0 game runs, no GPU.

  py -I tools\renderer\nogroup_unit.py [--root DIR]

1. src\renderer_adapter\lasting_prune_test.cpp (clang++ -std=c++20): TestNoGroup keeps exactly the passes that feed a lasting copy when `group` is false (object passes left out counted, kept_mask exact, d3_blind unchanged),
   and keeps all object passes when it is true. It must pass.
2. Twins, each must FAIL (and each text plant checks it applied first):
   --plant-half        the group rule applied to about half of the object passes only
   text plant          `group ||` replaced by `true ||` in lasting_prune.h (the switch cannot remove the rule)
   text plant          the kept_mask write removed (the binder's ordinal view would read an all-zero mask)
3. Wiring lints (each run on the real files, which must pass, and on a mutated copy, which must fail): the switch exists (PerFrame, default 0) and has its cvar (default false); live.cpp reads it through BoolSeen with the
   frame it works on, hands the pruner `group = !switch`, gives the binder the full ops and the kept mask only around the lasting Bind and clears them; binder.h swaps the ops for the chains' End calls and restores them;
   D3Core::Observe skips only through observe_keep; the census line names nogroup."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
FAILS = []


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)


def read(root, rel):
    return read_path(os.path.join(root, rel))


def build_and_run(root, out, name, src_text_override=None, args=()):
    """Compile the test (against the real tree, or a scratch copy of lasting_prune.h with an override) and run it. Returns (compiled, exit code, output)."""
    exe = os.path.join(out, name + '.exe')
    inc = ['-I', os.path.join(root, 'src')]
    if src_text_override is not None:   # a scratch tree: only lasting_prune.h differs; its siblings come from the real renderer_adapter via -I order
        scratch = os.path.join(out, name + '_src')
        os.makedirs(os.path.join(scratch, 'renderer_adapter'), exist_ok=True)
        with open(os.path.join(scratch, 'renderer_adapter', 'lasting_prune.h'), 'w', encoding='utf-8') as f:
            f.write(src_text_override)
        inc = ['-I', scratch, '-I', os.path.join(root, 'src', 'renderer_adapter'), '-I', os.path.join(root, 'src')]
    test_cpp = os.path.join(root, 'src', 'renderer_adapter', 'lasting_prune_test.cpp')
    if src_text_override is not None:   # the test must sit next to the scratch lasting_prune.h (a quoted include looks in the includer's own folder first)
        test_cpp = os.path.join(scratch, 'renderer_adapter', 'lasting_prune_test.cpp')
        with open(os.path.join(root, 'src', 'renderer_adapter', 'lasting_prune_test.cpp'), encoding='utf-8') as f:
            body = f.read()
        with open(test_cpp, 'w', encoding='utf-8') as f:
            f.write(body)
    cmd = [CLANG, '-std=c++20', '-Wno-deprecated-declarations'] + inc + [test_cpp, '-o', exe]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if p.returncode:
        return False, p.returncode, (p.stdout + p.stderr)[-1500:]
    r = subprocess.run([exe] + list(args), capture_output=True, text=True, timeout=120)
    return True, r.returncode, r.stdout + r.stderr


def lint(files):
    """Returns the list of problems in the wiring texts (a dict name -> text)."""
    bad = []
    sw, rd, lv, bd, d3b = files['rdr_switches.h'], files['reader.h'], files['live.cpp'], files['binder.h'], files['d3_binder.h']
    if not re.search(r'X\(LastingNoGroup, "wwe2k14_rdr_lasting_nogroup", PerFrame, 0\)', sw):
        bad.append('rdr_switches.h: LastingNoGroup is not a PerFrame switch with default 0')
    if not re.search(r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_lasting_nogroup, false,', rd):
        bad.append('reader.h: the cvar wwe2k14_rdr_lasting_nogroup (default false) is missing')
    if not re.search(r'BoolSeen\(rdr_sw::kLastingNoGroup, false, message\.guest_frame\)', lv):
        bad.append('live.cpp: the switch is not read through BoolSeen with message.guest_frame')
    if 'pruner.group = !nogroup_now;' not in lv:
        bad.append('live.cpp: the pruner is not told group = !switch')
    if not re.search(r'pruned_now && nogroup_now\) binder\.ord_full_ops = &all_ops, binder\.ord_keep = &pruner\.kept_mask;', lv):
        bad.append('live.cpp: the binder is not given the full ops and the kept mask for the lasting bind')
    if not re.search(r'pruner\.lasting_targets = rdr_sw::BoolSeen\(rdr_sw::kLastingNoGroupTargets, false, message\.guest_frame\)', lv):
        bad.append('live.cpp: the lasting-target sub-switch is not read through BoolSeen with message.guest_frame')
    if 'states for the Surface arm' not in lv:
        bad.append('live.cpp: the census line does not carry the switch states (review F8)')
    if 'pruner.images = &source.images();' not in lv:
        bad.append('live.cpp: the pruner is not given the image table (lasting render targets)')
    if 'binder.ord_full_ops = nullptr, binder.ord_keep = nullptr;' not in lv:
        bad.append('live.cpp: the ordinal view is not cleared after the lasting Bind')
    if 'nogroup %s: lasting binds with an object pass' not in lv:
        bad.append('live.cpp: the [rdr][skip_cost] census line does not name nogroup')
    if not (re.search(r'mutable_frame\.ops = full;', bd) and re.search(r'mutable_frame\.ops = bound_ops;', bd) and bd.count('observe_keep = nullptr') >= 4):
        bad.append('binder.h: the End calls do not swap the ops for the full view and back (or leave an observe_keep set)')
    if not re.search(r'if \(observe_keep && !\(r\.first_op < observe_keep->size\(\) && \(\*observe_keep\)\[r\.first_op\]\)\)', d3b):
        bad.append('d3_binder.h: Observe does not skip the runs the bind left out through observe_keep')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    out = os.path.join(root, 'out', 'k2', 'unit')
    os.makedirs(out, exist_ok=True)
    ok, rc, text = build_and_run(root, out, 'honest')
    say(ok and rc == 0 and 'all passed' in text and 'nogroup: object passes kept 1 of 3, left out 2, grouped 0' in text, 'lasting_prune_test (TestNoGroup: only the passes that feed a lasting copy are kept without the group rule; all three with it): exit %s' % rc)
    if not (ok and rc == 0):
        print(text)
    ok, rc, text = build_and_run(root, out, 'plant_attach', args=('--plant-attach',))
    say(ok and rc != 0 and 'FAIL' in text, 'twin --plant-attach (a kept pass own colour / depth attachments are not marked needed: review F1): CAUGHT (exit %s, %d FAIL lines)' % (rc, text.count('FAIL ')))
    ok, rc, text = build_and_run(root, out, 'plant_lasting', args=('--plant-lasting',))
    say(ok and rc != 0 and 'FAIL' in text, 'twin --plant-lasting (an object pass that draws to a lasting render target is dropped without the group rule: the J1 failure): CAUGHT (exit %s, %d FAIL lines)' % (rc, text.count('FAIL ')))
    ok, rc, text = build_and_run(root, out, 'plant_half', args=('--plant-half',))
    say(ok and rc != 0 and 'FAIL' in text, 'twin --plant-half (the group rule applied to about half of the object passes): CAUGHT (exit %s, %d FAIL lines)' % (rc, text.count('FAIL ')))
    src = read(root, 'src/renderer_adapter/lasting_prune.h')
    for name, a, b in (('group_forced', 'const bool apply = group ||', 'const bool apply = true ||'), ('mask_unwritten', '          kept_mask[i] = 1;\n', ''),
                      ('attach_off', 'if (!group && !plant_no_attach && t) {', 'if (false && !group && !plant_no_attach && t) {'),
                      ('lasting_target_off', 'if (!group && lasting_targets && !has_seed && t && sensitive_[s] && !plant_no_lasting_target && TargetsLasting(*t))', 'if (false && !group && lasting_targets && !has_seed && t && sensitive_[s] && !plant_no_lasting_target && TargetsLasting(*t))')):
        pl = src.replace(a, b)
        if pl == src:
            say(False, 'twin %s DID NOT APPLY (the text it replaces is gone)' % name)
            continue
        ok, rc, text = build_and_run(root, out, 'plant_' + name, src_text_override=pl)
        say(ok and rc != 0, 'twin %s: CAUGHT (exit %s, %d FAIL lines)' % (name, rc, text.count('FAIL ')))
    files = {n: read(root, p) for n, p in (('rdr_switches.h', 'src/renderer_adapter/rdr_switches.h'), ('reader.h', 'src/renderer_adapter/reader.h'), ('live.cpp', 'src/renderer_adapter/live.cpp'),
                                           ('binder.h', 'src/renderer_adapter/binder.h'), ('d3_binder.h', 'src/renderer_adapter/d3_binder.h'))}
    bad = lint(files)
    say(not bad, 'wiring lints on the real files: %d problem(s)%s' % (len(bad), (': ' + '; '.join(bad)) if bad else ''))
    for fname, a, b in (('live.cpp', 'message.guest_frame);   // S102 K2 (R16 piece C)', 'message.guest_frame + 0);   // S102 K2 (R16 piece C)'),
                        ('live.cpp', 'pruner.group = !nogroup_now;', 'pruner.group = true;'),
                        ('binder.h', 'mutable_frame.ops = bound_ops;', ''),
                        ('live.cpp', 'pruner.images = &source.images();', ''),
                        ('live.cpp', 'pruner.lasting_targets = rdr_sw::BoolSeen(rdr_sw::kLastingNoGroupTargets, false, message.guest_frame);', 'pruner.lasting_targets = true;'),
                        ('d3_binder.h', 'if (observe_keep && !(r.first_op', 'if (false && !(r.first_op')):
        mutated = dict(files)
        if a not in mutated[fname]:
            say(False, 'lint twin on %s DID NOT APPLY (%r is gone)' % (fname, a[:40]))
            continue
        mutated[fname] = mutated[fname].replace(a, b, 1)
        mb = lint(mutated)
        say(len(mb) > 0, 'lint twin (%s: %s): CAUGHT (%s)' % (fname, a[:44].strip(), mb[0] if mb else 'nothing'))
    print('nogroup_unit: %d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
