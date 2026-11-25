#!/usr/bin/env python3
"""cvar_shard_lint (S110 CVARSHARD): no header of src\\ holds a REXCVAR_DEFINE_*, and the generated shards src\\cvars\\cvars_<n>.cpp are what tools\\compile\\cvar_shard.py writes.

  py -I tools\\lint\\cvar_shard_lint.py [--root .]    exit 0 clean, 1 a finding (the fix is `py -I tools\\compile\\cvar_shard.py`: it moves a new definition to its shard and leaves a REXCVAR_DECLARE)

A cvar defined in a header is defined by every unit that includes the header, which is why src\\main.cpp had to be one unit (docs\\research\\s110\\MAINSPLIT.md). The judge is the tool's own
`--check` (tools\\compile\\cvar_shard.py); its selftest is `py -I tools\\compile\\cvar_shard.py --selftest`. Reads text only; writes nothing."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'compile'))
import cvar_shard  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    a = ap.parse_args()
    return cvar_shard.run(os.path.abspath(a.root), check=True)


if __name__ == '__main__':
    sys.exit(main())
