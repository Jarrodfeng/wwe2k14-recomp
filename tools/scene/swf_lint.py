#!/usr/bin/env python3
"""S103 SWF lint (review SWF-1 / SWF-2): py -I tools\\scene\\swf_lint.py [--root DIR]
  1. every liquid_of( call in src\\renderer_scene passes a `.frame_object` (the index in frame.objects that SceneFrame::object_engine / piece_alphas name), never the dense objs index;
     any other direct use of `object_engine` must go through EngineObjectOf (liquid_join.h);
  2. no getenv of the liquid plants (WWE_RDR_NO_LIQUID, WWE_RDR_SWF_PLANT, WWE_RDR_SWF_*): plants are flags the rig sets (liquid_switch.h Plants).
Exit 1 on any finding."""
import os, re, sys
root = sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
bad = []
for dp, _, fs in os.walk(os.path.join(root, 'src')):
    for f in fs:
        if not f.endswith(('.cpp', '.h')):
            continue
        p = os.path.join(dp, f)
        txt = open(p, encoding='utf-8', errors='replace').read()
        for i, ln in enumerate(txt.splitlines(), 1):
            ln = ln.split('//')[0]   # code only
            if re.search(r'liquid_of\(', ln) and 'const auto liquid_of' not in ln and 'frame_object' not in ln:
                bad.append('%s:%d liquid_of( without frame_object: %s' % (p, i, ln.strip()[:100]))
            if re.search(r'getenv\("WWE_RDR_(NO_LIQUID|SWF_)', ln):
                bad.append('%s:%d getenv of a liquid plant: %s' % (p, i, ln.strip()[:100]))
            if 'object_engine' in ln and f not in ('front_end.h', 'rf_wrestlers.cpp', 'record_frame.cpp', 'liquid_join.h', 'live_map_sweat.cpp') and 'EngineObjectOf' not in ln and not (f == 'wrk_join.h' and ('value_type oe;' in ln or 'f.object_engine.push_back(oe)' in ln)):
                bad.append('%s:%d object_engine used outside the join: %s' % (p, i, ln.strip()[:100]))
            if f == 'front_end.cpp' and 'object_engine' in ln and 'EngineObjectOf' not in ln and 'object_engine.' not in ln and not ln.strip().startswith('//'):
                bad.append('%s:%d object_engine read without EngineObjectOf: %s' % (p, i, ln.strip()[:100]))
for b in bad:
    print('FAIL ' + b)
print('PASS  swf_lint: the liquid join uses the frame index, no liquid getenv' if not bad else 'FAIL  swf_lint: %d finding(s)' % len(bad))
sys.exit(1 if bad else 0)
