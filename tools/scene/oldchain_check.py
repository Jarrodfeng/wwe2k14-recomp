r"""S111 OLDCHAIN source check (0 game runs; docs\research\s111\OLDCHAIN.md): the wrestlers' old min-define shader pick is gone and nothing stands
in for it but the table pick.

    py -I tools\scene\oldchain_check.py [--root DIR]

Checks, on our code under --root:
  1 chooser  no call or declaration of the min-define chooser (ChooseCharEffect / ChooseCharEffectShadow / ChooseCharEffectImpl followed by `(`)
             in src\ or tools\assets\ych_census.cpp (comments excluded);
  2 switch   no g_char_table_pick / wwe2k14_rdr_char_table_pick / no_table_pick in src\ (comments excluded);
  3 hole     front_end.cpp: a failed table bind is the hole (`else char_hole = &cb, ++char_hole_draws_;`), the draw takes it before any other bind
             (`const Bound& b = char_b ? *char_b : char_hole ? *char_hole :`), and no wet bind or shadow-reader bind of the old chain is left
             (`if (liquid && !char_b)`, `shadow_bound =`);
  4 counted  BindMaterial counts each hole by its reason (`CharHoleOf(cc)`, `++char_hole_no_table_`, `++char_hole_no_row_`) and the census line
             (live_scene_draw.cpp) names char_hole_no_table, char_hole_no_row and char_hole_draws; the HOLE log line names the row (skinned / rigid)
             and the material (index, diffuse texture) and is said once per distinct hole (`hole_said.insert(`), not under a line cap;
  5 loud     the table build says NoTableLine on a failure (effect_table_build.h) and live_assets.cpp calls it (`read_rule::BuildEffectTable(impl_->files,`);
  6 viewer   the Arena Viewer's rdr backend publishes the table (renderer_rdr.cpp `PublishFromViewerFiles(ViewerGameFiles(),`);
  7 wet_unknown  the S103 L9 count of a character draw that is not a table pick and not wet is gone from src\ (comments excluded): every such
             draw is a hole now, counted char_hole_draws (review, S111 OLDCHAIN);
  8 skinned  the pass rule's "model is skinned" (CharPassOf's 2nd argument, the game's model +540) is the model's own flag
             (ModelData::skinned_model), never this frame's palette pointer: a skinned body with no palette keeps its odd pass
             (review, S111 OLDCHAIN: with the palette, a missing palette made every skinned mesh a hole).
Planted twin: the same checks on our code at OLD (s111 before the removal, read with git show) must each FAIL.
Exit 0 = every check passes and every twin is caught; 1 = a check failed; 2 = a twin was NOT caught.
"""
import argparse
import os
import re
import subprocess
import sys

OLD = '5bc8fc64a'   # s111 before OLDCHAIN: the chain, the switch and the silent table build still in place
FE = 'src/renderer_scene/front_end.cpp'
LSD = 'src/renderer_scene/live_scene_draw.cpp'
ETB = 'src/renderer_scene/effect_table_build.h'
LA = 'src/renderer_scene/live_assets.cpp'
RDR = 'src/assets/view/renderer_rdr.cpp'
YCH = 'tools/assets/ych_census.cpp'
FIXED = (FE, 'src/renderer_scene/front_end.h', 'src/renderer_scene/char_effects.cpp', 'src/renderer_scene/char_effects.h', 'src/start_settings.cpp',
         'src/renderer_scene/change_colour_draw.h', LSD, YCH)
CHOOSER = re.compile(r'\bChooseCharEffect(?:Shadow|Impl)?\s*\(')
SWITCH = re.compile(r'\b(?:g_char_table_pick|wwe2k14_rdr_char_table_pick|no_table_pick)\b')
WET_UNKNOWN = re.compile(r'\bwet_unknown_?\b')


def strip(t):
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def norm(t):
    return ' '.join(t.split())


def tree_reader(root):
    def read(p):
        try:
            with open(os.path.join(root, p), encoding='utf-8', errors='replace') as fh:
                return fh.read()
        except OSError:
            return ''

    def files():
        out = []
        for d, _dirs, fs in os.walk(os.path.join(root, 'src')):
            out += [os.path.relpath(os.path.join(d, f), root).replace('\\', '/') for f in fs if f.endswith(('.h', '.cpp', '.inc'))]
        return sorted(out) + [YCH]
    return read, files


def git_reader(root, ref):
    def read(p):
        r = subprocess.run(['git', '-C', root, 'show', f'{ref}:{p}'], capture_output=True, text=True, encoding='utf-8', errors='replace')
        return r.stdout if r.returncode == 0 else ''

    def files():
        return list(FIXED)
    return read, files


def hits(rx, texts):
    return [f'{p}:{t.count(chr(10), 0, m.start()) + 1}' for p, t in sorted(texts.items()) for m in rx.finditer(t)]


def checks(read, files):
    """-> [(name, None when it holds | why not)]"""
    out = []
    names = files()
    texts = {p: strip(read(p)) for p in names}
    h = hits(CHOOSER, texts)
    out.append(('1 chooser', f'the min-define chooser is still named at {", ".join(h[:4])}' if h else None))
    h = hits(SWITCH, {p: t for p, t in texts.items() if p.startswith('src/')})
    out.append(('2 switch', f'the table-pick switch is still named at {", ".join(h[:4])}' if h else None))
    fe = norm(strip(read(FE)))
    miss = [s for s in ('else char_hole = &cb, ++char_hole_draws_;', 'const Bound& b = char_b ? *char_b : char_hole ? *char_hole :') if norm(s) not in fe]
    left = [s for s in ('if (liquid && !char_b)', 'shadow_bound =') if norm(s) in fe]
    out.append(('3 hole', '; '.join([f'{FE} lacks `{s}`' for s in miss] + [f'{FE} still holds `{s}`' for s in left]) or None))
    lsd = read(LSD)
    miss = [s for s in ('CharHoleOf(cc)', '++char_hole_no_table_', '++char_hole_no_row_') if norm(s) not in fe]
    miss += [f'{LSD}: "{w}"' for w in (' char_hole_no_table ', ' char_hole_no_row ', ' char_hole_draws ') if f'"{w}"' not in lsd]
    miss += [f'{FE}: the HOLE log `{s}`' for s in ('hole_said.insert(', '(material %d, diffuse \'%s\')', 'skinned ? "skinned" : "rigid"') if norm(s) not in fe]
    out.append(('4 counted', ('missing: ' + ', '.join(miss)) if miss else None))
    etb, la = strip(read(ETB)), norm(strip(read(LA)))
    bad = ([] if 'NoTableLine(' in etb else [f'{ETB} has no NoTableLine']) + ([] if 'read_rule::BuildEffectTable(impl_->files,' in la else [f'{LA} does not call read_rule::BuildEffectTable(impl_->files, say)'])
    out.append(('5 loud', '; '.join(bad) or None))
    rdr = norm(strip(read(RDR)))
    out.append(('6 viewer', None if 'PublishFromViewerFiles(ViewerGameFiles(),' in rdr else f'{RDR} does not publish the table (PublishFromViewerFiles(ViewerGameFiles(), ...))'))
    h = hits(WET_UNKNOWN, {p: t for p, t in texts.items() if p.startswith('src/')})
    out.append(('7 wet_unknown', f'the S103 L9 count is still named at {", ".join(h[:4])}' if h else None))
    m = re.search(r'technique_pick::CharPassOf\(cl, ([^,]*),', fe)
    arg = m.group(1).strip() if m else None
    out.append(('8 skinned', None if arg == 'm.skinned_model' else f'{FE}: CharPassOf\'s model-skinned argument is `{arg}` (want the model\'s own flag `m.skinned_model`, never the palette)'))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    fails, missed = [], []
    now = checks(*tree_reader(root))
    for name, why in now:
        print(f'ok   {name}' if why is None else f'FAIL {name}: {why}')
        if why is not None:
            fails.append(name)
    old = checks(*git_reader(root, OLD))
    for name, why in old:
        if why is None:
            missed.append(name)
            print(f'NOT CAUGHT planted twin (our code at {OLD}): {name} passes on the old chain')
        else:
            print(f'ok   planted twin caught (our code at {OLD}): {name} ({why[:100]})')
    print(f'oldchain_check: {len(now)} checks, {len(fails)} failed; {len(missed)} planted twins not caught')
    return 2 if missed else 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
