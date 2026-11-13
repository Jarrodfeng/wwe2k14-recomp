#!/usr/bin/env python3
r"""Session 100: one game shader's listing, or only how it differs from another. Near-identical shaders are hidden behind one lead in the packets; this shows what they change.

  py tools\shaders\harness\show_shader.py <effect> <hash> [--against HASH]

Terminal output only: a listing is game code and never goes into the repo (rule 1).
"""
import argparse
import difflib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_effect as ce


def listing(effect, h):
    kind, code, literals = ce.uc.get(effect, [h])[h]
    text = re.sub(r'/\*.*?\*/', '', ce.disassemble(code, kind))
    return [line.strip() for line in text.split('\n') if line.strip()], literals, sorted(ce.uc.named_float_registers(h))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('effect')
    ap.add_argument('hash')
    ap.add_argument('--against')
    args = ap.parse_args()
    ce.ensure_tools()
    lines, literals, named = listing(args.effect, int(args.hash, 16))
    print('named float registers:', named)
    print('literals:', literals)
    if not args.against:
        print('\n'.join(lines))
        return
    other, _, other_named = listing(args.effect, int(args.against, 16))
    print('registers only in this one:', sorted(set(named) - set(other_named)), ' only in the other:', sorted(set(other_named) - set(named)))
    print('\n'.join(l for l in difflib.unified_diff(other, lines, 'against', 'this', lineterm='', n=1)))


if __name__ == '__main__':
    main()
