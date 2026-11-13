#!/usr/bin/env python3
r"""Session 100: says how a hand-written shader differs from the game's, in a few lines, so nobody has to guess and rerun.

  py tools\shaders\harness\explain_failure.py <effect> <hash> [--variant NAME]

Runs the same comparison as check_effect.py for one game shader (the variant holding the hash, or --variant for an unsolved one) and reports, per wrong output: swapped or
negated channels, a constant factor or offset, one-minus, on which inputs it goes wrong (only when a constant or interpolator is zero, negative, above 1, or a bool
bit is set), and any single input the difference follows. Numbers and our own field names only. Nothing is written.
"""
import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_effect as ce

COMPONENTS = 'xyzw'


def input_features(stage, var, inputs, consts, rows, named):
    """[(name, values per compared input)] for every input that can matter."""
    features = []
    if stage == 'fragment':
        for loc in range(16):
            for c in range(4):
                features.append(('interpolator %d.%s' % (loc, COMPONENTS[c]), inputs[rows, loc, c]))
    else:
        for a in var.get('attributes', []):
            for c in range(4):
                column = 4 * a['fetch'] + c
                if column < inputs.shape[1]:
                    features.append(('vertex input %d.%s' % (a['location'], COMPONENTS[c]), inputs[rows, column]))
    sets = rows // ce.SET_INV
    floats = consts[:, :1024].astype(np.uint32).view(np.float32)
    field_of = {}
    for entry in var.get('constants', []):
        for k in range(entry.get('count', 1)):
            field_of[entry['register'] + k] = '%s[%d]' % (entry['field'], k) if entry.get('count', 1) > 1 else entry['field']
    for reg in sorted(named):
        for c in range(4):
            features.append(('%s.%s (c%d)' % (field_of.get(reg, 'unnamed'), COMPONENTS[c], reg), floats[sets, reg * 4 + c]))
    words = consts[sets, 1024:1032].astype(np.uint32)
    for bit in range(256):
        features.append(('bool %d' % bit, ((words[:, bit >> 5] >> (bit & 31)) & 1).astype(np.float32)))
    return features


def close(a, b):
    return np.abs(a - b) <= 1e-3 * np.maximum(np.maximum(np.abs(a), np.abs(b)), 0.1)


def describe_component(label, column, a, b, wrong, features):
    print('  %s: wrong on %.1f%% of the inputs' % (label, 100.0 * wrong.mean()))
    target = b[:, column]
    for other in range(a.shape[1]):
        if close(a[:, other], target).mean() > 0.98:
            print('    the game writes our .%s here' % COMPONENTS[other])
        if close(-a[:, other], target).mean() > 0.98:
            print('    the game writes our -.%s here' % COMPONENTS[other])
    ours = a[:, column]
    if close(1.0 - ours, target).mean() > 0.98:
        print('    the game writes 1 minus ours')
    usable = np.abs(ours) > 1e-3
    if usable.sum() > 16:
        factor = float(np.median(target[usable] / ours[usable]))
        if close(ours * factor, target).mean() > 0.98:
            print('    the game = ours x %.5g' % factor)
    offset = float(np.median(target - ours))
    if close(ours + offset, target).mean() > 0.98:
        print('    the game = ours + %.5g' % offset)
    conditions = []
    base = max(float(wrong.mean()), 1e-9)
    for name, values in features:
        for text, predicate in (('is 0', values == 0.0), ('is negative', values < 0.0), ('is above 1', values > 1.0), ('is on', values == 1.0)):
            if text == 'is on' and not name.startswith('bool'):
                continue
            if text != 'is on' and name.startswith('bool'):
                continue
            chosen = int(predicate.sum())
            if not chosen:
                continue
            hit = int((predicate & wrong).sum())
            recall, precision = hit / max(int(wrong.sum()), 1), hit / chosen
            if recall >= 0.9 and precision >= 1.5 * base and precision > 0.3:
                conditions.append((precision, recall, name, text))
    for precision, recall, name, text in sorted(conditions, reverse=True)[:4]:
        print('    wrong when %s %s (%.0f%% of the wrong inputs; %.0f%% of those inputs are wrong)' % (name, text, 100.0 * recall, 100.0 * precision))
    difference = target - ours
    if np.std(difference) > 0:
        follows = []
        for name, values in features:
            if name.startswith('bool') or np.std(values) == 0:
                continue
            r = float(np.corrcoef(values, difference)[0, 1])
            if abs(r) > 0.95:
                follows.append((abs(r), name, float(np.polyfit(values, difference, 1)[0])))
        for r, name, slope in sorted(follows, reverse=True)[:3]:
            print('    the difference follows %s (slope %.4g, correlation %.3f): likely a missing or extra term' % (name, slope, r))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('effect')
    ap.add_argument('hash')
    ap.add_argument('--variant')
    args = ap.parse_args()
    fxdir = os.path.join(ce.REPO, 'src', 'renderer', 'shaders', 'fx')
    spec = json.load(open(os.path.join(fxdir, args.effect + '.fx.json'), encoding='utf-8'))
    h = int(args.hash, 16)
    found = None
    for stage in ('vertex', 'fragment'):
        for var in spec.get(stage, []):
            if args.hash.upper() in var.get('hashes', []) or var.get('name') == args.variant:
                found = (stage, var)
    ce.ensure_tools()
    ucodes = ce.uc.get(args.effect, [h])
    if args.variant and spec.get('solve'):   # a variant the solver built but no game shader uses yet
        import itertools
        import solve_variants as sv
        stage = 'vertex' if ucodes[h][0] == 'VS' else 'fragment'
        template = spec['solve'].get(stage, {})
        options = template.get('options', [])
        for count in range(len(options) + 1):
            for defines in itertools.combinations(options, count):
                built = sv.build_variant(fxdir, args.effect, stage, template, defines)
                if built and built['name'] == args.variant:
                    found = (stage, built)
    if not found:
        raise SystemExit('no variant holds that hash: pass --variant NAME')
    stage, var = found
    check = ce.check_vertex if stage == 'vertex' else ce.check_fragment
    worst, output, problems, _, inputs, consts = check(fxdir, var.get('source', args.effect), var, h, ucodes[h], np.random.default_rng(1 + (h & 0xFFFF)), 4096 * 4, True, False)
    print('%s against variant %s: worst %.3e at %s %s' % (args.hash, var['name'], worst, output, problems or ''))
    last = ce.LAST
    keep = ~last['skip']
    rows = last['rows'][keep]
    features = input_features(stage, var, inputs, consts, rows, ce.uc.named_float_registers(h))
    any_wrong = False
    for slot, name in enumerate(last['names']):
        a, b = last['mine'][keep, slot], last['ref'][keep, slot]
        with np.errstate(all='ignore'):
            scale = np.maximum(np.maximum(np.abs(a).max(axis=1, keepdims=True), np.abs(b).max(axis=1, keepdims=True)), 0.1)
            wrong = (np.abs(a - b) / scale > 1e-3) & np.isfinite(b)
        for column in range(4):
            if wrong[:, column].mean() > 0.0005:
                any_wrong = True
                describe_component('%s.%s' % (name, COMPONENTS[column]), column, np.nan_to_num(a), np.nan_to_num(b), wrong[:, column], features)
    if not any_wrong:
        print('no output is wrong on more than 0.05% of the inputs')


if __name__ == '__main__':
    main()
