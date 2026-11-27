#!/usr/bin/env python3
r"""Shader hash -> variant name, and a variant's defines / samplers, from the fx.json files (src\renderer\shaders\fx) plus tools\solver\solve.py Effects.
Moved out of the deleted tools\scene\feature_diff.py (S111 FDDELETE); used by tools\harvest\choice_harvest.py and tools\scene\sampler_compare.py."""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import solve  # noqa: E402

FX_DIR = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')


class Fx:
    """variant name <-> defines, samplers, from the fx.json files; plus solve.Effects for hash resolution."""
    def __init__(self):
        self.eff = solve.Effects()
        self.defines = {}   # '<effect>_<variant>_<stage>' -> [defines]
        self.samplers = {}
        for fn in os.listdir(FX_DIR):
            if not fn.endswith('.fx.json'):
                continue
            d = json.load(open(os.path.join(FX_DIR, fn), encoding='utf-8'))
            for stage in ('vertex', 'fragment'):
                for v in d.get(stage, []):
                    nm = '%s_%s_%s' % (d['effect'], v['name'], stage)
                    self.defines[nm] = list(v.get('defines', []))
                    self.samplers[nm] = [s['slot'] for s in v.get('samplers', [])]

    def name_of(self, stage, h, prefer=None):
        c = self.eff.by_hash[stage].get(h, [])
        if not c:
            return None
        if prefer:
            for e, vn in c:
                if e == prefer:
                    return '%s_%s_%s' % (e, vn, stage)
        e, vn = c[0]
        return '%s_%s_%s' % (e, vn, stage)

    def effect_of(self, variant):
        return variant.rsplit('_', 2)[0] if variant else ''
