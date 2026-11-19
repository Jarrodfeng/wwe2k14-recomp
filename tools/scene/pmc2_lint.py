"""S103 PMC2 lint: the wiring of the props-map census fixes (docs/agents/s103/PMH.md). Source-only, 0 game runs.
  py -I tools/scene/pmc2_lint.py            exit 0 = ok
  py -I tools/scene/pmc2_lint.py --plant N  exit 0 only when the lint FAILS: no_row (the latch never emits the kPlDegenerate row) / no_flag (BuildMapsInput never reads it) /
                                            raw_hole (RecordCircles counts a hole itself on a degenerate frame) / bad_label (the census still prints one `bad`)
Checks: the latch emits the degenerate row; BuildMapsInput reads it; RecordCircles goes through maps_census (no raw `++counts_.holes` outside the classified hole);
Record counts the props hole through HolesWithPropsHole; the census prints props_light_degenerate, non_finite / no_range and the circle_module branches."""
import os, sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def text(rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def main():
    plant = sys.argv[sys.argv.index('--plant') + 1] if '--plant' in sys.argv else ''
    contrib, maps, circ = text('src/scene_live/inputs_contrib.h'), text('src/renderer_scene/live_maps.cpp'), text('src/renderer_scene/live_map_circles.cpp')
    if plant == 'no_row':
        contrib = contrib.replace('kPlDegenerate', 'kPlRead')
    if plant == 'no_flag':
        maps = maps.replace('kPlDegenerate', 'kPlRead')
    if plant == 'raw_hole':
        circ = circ.replace('maps_census::ClassifyCircleInput', 'ClassifyOld') + '\n  ++counts_.holes;'
    if plant == 'bad_label':
        contrib = contrib.replace('non_finite', 'bad')
    problems = []
    if 'kPlDegenerate' not in contrib or 'f->light_degenerate' not in contrib:
        problems.append('inputs_contrib.h: the latch does not emit the kPlDegenerate row from f->light_degenerate')
    if 'kPlDegenerate' not in maps or 'out.light_degenerate' not in maps:
        problems.append('live_maps.cpp: BuildMapsInput does not read the kPlDegenerate row into MapsInput::light_degenerate')
    if 'maps_census::ClassifyCircleInput' not in circ:
        problems.append('live_map_circles.cpp: RecordCircles does not classify through maps_census')
    if circ.count('++counts_.holes') != 1:
        problems.append('live_map_circles.cpp: expected exactly one `++counts_.holes` (the classified hole), found %d' % circ.count('++counts_.holes'))
    if 'HolesWithPropsHole' not in maps or '++props_holes_, ++counts_.holes' in maps:
        problems.append('live_maps.cpp: the props hole must count through HolesWithPropsHole (one missing image = one hole)')
    if 'props_light_degenerate' not in maps:
        problems.append('live_maps.cpp: the census does not print props_light_degenerate')
    for tok in ('non_finite', 'no_range', 'circle_module unreadable', 'module_ptr', 'view_proj_read', 'degenerate (module+2240 non-finite'):
        if tok not in contrib:
            problems.append('inputs_contrib.h: the census lacks `%s`' % tok)
    if ', bad " +' in contrib:
        problems.append('inputs_contrib.h: the props_light census still prints one `bad`')
    for p in problems:
        print('FAIL', p)
    if plant:
        print('plant %s: %s' % (plant, 'caught' if problems else 'NOT caught'))
        return 0 if problems else 1
    print('ok' if not problems else 'FAILED')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
