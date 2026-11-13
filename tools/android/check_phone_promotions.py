"""S101 lane 127: text check that the phone (Android) build carries every renderer promotion and its switch.

  py -I tools\\android\\check_phone_promotions.py [--root DIR] [--plant NAME]

Reads source text only (0 game runs, no device, no build). For every `wwe2k14_rdr_*_ours` cvar DEFINEd under src\\ it checks:
  1. the cvar is in the table below (a new promotion must be added here: its sources and whether it reads the game folder);
  2. every source of the promotion is in CMakeLists.txt's WWE2K14_SOURCES, which the ANDROID branch uses unchanged
     (add_library(wwe2k14 SHARED ${WWE2K14_SOURCES})), with no list(REMOVE_ITEM ... WWE2K14_SOURCES) and no `if(NOT ANDROID)` around a source;
  3. the source files exist and carry no `!defined(__ANDROID__)` / `#ifndef __ANDROID__` guard (a PC-only compile-out);
  4. the switch plumbing: reader.h reads the cvar into the live options; for a promotion that reads the player's files, wwe2k14_app.h's
     game-root condition names it (the phone's root is /proc/self/fd/N of the ISO; the same code path);
  5. the dev way to switch it on: src\\android_dev_flags.h exists, accepts the wwe2k14_rdr_ / wwe2k14_scene_ families, and wwe2k14_app.h calls it.
--plant NAME proves the check can fail: it applies one planted fault in memory and exits 0 only if the check then FAILS.
  drop-source    removes src/renderer_scene/arena_ours.cpp from the parsed source list
  drop-switch    removes wwe2k14_rdr_ring_ours from the parsed game-root condition
  drop-devflags  pretends android_dev_flags.h does not accept the rdr family
Exit 0 = pass (or, with --plant, the planted fault was caught); 1 = problems (printed).
"""
import argparse, re, sys
from pathlib import Path

# cvar -> (sources it needs in WWE2K14_SOURCES, header-only files it needs, reads the player's game files)
TABLE = {
    'wwe2k14_rdr_crowd_ours': (['src/renderer_scene/crowd_ours.cpp', 'src/assets/crowd_body.cpp'], [], True),
    'wwe2k14_rdr_arena_ours': (['src/renderer_scene/arena_ours.cpp', 'src/renderer_scene/arena_ours_core.cpp', 'src/assets/arena_game_space.cpp'], [], True),
    'wwe2k14_rdr_caw_ours': (['src/renderer_scene/caw_ours.cpp', 'src/assets/wrestler_roster.cpp'], ['src/renderer_scene/caw_core.h'], True),  # lane 153 (N3): no scene record; reads pac/edit
    'wwe2k14_rdr_wrestlers_ours': (['src/renderer_scene/wrestler_ours.cpp', 'src/assets/wrestler_roster.cpp'], [], True),
    'wwe2k14_rdr_ring_ours': (['src/renderer_scene/ring_ours.cpp', 'src/renderer_scene/ring_ours_core.cpp', 'src/assets/ring_game_space.cpp'], [], True),
    'wwe2k14_rdr_ui_ours': (['src/renderer_scene/ui_ours.cpp', 'src/renderer_scene/ui_ours_core.cpp'], [], True),
    'wwe2k14_rdr_ui_text_ours': (['src/renderer_scene/ui_text_ours.cpp', 'src/renderer_scene/ui_text_fonts.cpp'], [], True),
    'wwe2k14_rdr_flash_ours': (['src/renderer_scene/flash_ours.cpp', 'src/assets/flash_game_space.cpp'], [], True),
    'wwe2k14_rdr_crowd_props_ours': (['src/renderer_scene/crowd_props_ours.cpp', 'src/assets/crowd_prop_game_space.cpp'], [], True),  # lane 133
    'wwe2k14_rdr_props_ours': (['src/renderer_scene/props_ours.cpp', 'src/renderer_scene/props_ours_core.cpp', 'src/assets/prop_game_space.cpp'], [], True),
    'wwe2k14_rdr_shadow_casters_ours': (['src/renderer_scene/shadow_casters_ours.cpp', 'src/renderer_scene/shadow_casters_core.cpp', 'src/assets/chcommon_proxy.cpp'], [], True),
    'wwe2k14_rdr_screens_ours': (['src/renderer_scene/screens_ours.cpp', 'src/renderer_scene/screens_ours_core.cpp', 'src/assets/screen_game_space.cpp'], [], True),  # lane 115
    # lane 148: the UI's quads read no game files (the picture join is wwe2k14_rdr_ui_ours's); needs ui_ours
    'wwe2k14_rdr_ui_quads_ours': (['src/renderer_scene/ui_quads.cpp'], [], False),
    # lane 131: the movie planes read no game files (the decoder is the game's own; ours is the quad and the colour step)
    'wwe2k14_rdr_movie_ours': (['src/renderer_scene/movie_ours.cpp'], [], False),
    # lane 123: our shadow maps and chain (header-only in the binder; reads no game files)
    'wwe2k14_rdr_shadows_ours': (['src/renderer_adapter/live.cpp'], ['src/renderer_adapter/shadows_ours.h'], False),
    'wwe2k14_rdr_rim_ours': (['src/renderer_scene/rim_map_ours.cpp', 'src/renderer_scene/shadow_casters_core.cpp', 'src/assets/chcommon_proxy.cpp'], [], True),  # lane 124
    # lane 136: the circle blob shadows come from the scene record (the writer's three headers + the pure core); no game files
    'wwe2k14_rdr_circles_ours': (['src/renderer_scene/circle_shadows_ours.cpp'], ['src/renderer_scene/circle_shadow_core.h', 'src/scene_live/circle_hook.h', 'src/scene_live/circle_contrib.h', 'src/scene_live/circle_glue.h'], False),
    # lane 139: the plane shadow chain's constants come from the scene record (the writer's three headers + the pure core); no game files
    'wwe2k14_rdr_plane_shadow_ours': (['src/renderer_scene/plane_shadow_ours.cpp'], ['src/renderer_scene/plane_shadow_core.h', 'src/scene_live/plane_hook.h', 'src/scene_live/plane_contrib.h', 'src/scene_live/plane_glue.h', 'src/renderer/shaders/spv/rdr_plane_mask_vert.h', 'src/renderer/shaders/spv/rdr_plane_mask_frag.h', 'src/renderer/shaders/spv/rdr_plane_blur_vert.h', 'src/renderer/shaders/spv/rdr_plane_blur_frag.h'], False),  # lane 159: the plane shadow's own programs (the phone build compiles the same headers)
    # lane 135: the sweat map and skin pre-light meshes are rules (small_passes_core.h, header-only); no game files
    'wwe2k14_rdr_small_passes_ours': (['src/renderer_scene/small_passes_ours.cpp'], ['src/renderer_scene/small_passes_core.h'], False),
    # lane 151: the rim light map's reductions and blur by our own programs on the stream's draws; no game files, no scene record
    'wwe2k14_rdr_rim_chain_ours': (['src/renderer_scene/rim_chain_ours.cpp'], [], False),
    # lane 114: our post chain reads no game files (no game root needed)
    'wwe2k14_rdr_post_ours': (['src/renderer_scene/post_ours.cpp', 'src/renderer_scene/post_ours_core.cpp', 'src/renderer_scene/post_pass.cpp', 'src/renderer_scene/post_plan.cpp'], [], False),
    # the particle runs are header-only (binder.h / particle_runs.h inside live.cpp) and read no game files
    'wwe2k14_rdr_particles_ours': (['src/renderer_adapter/live.cpp'], ['src/renderer_adapter/particle_runs.h'], False),
}
# What every promotion's scene feed needs (the record writer's sources) is checked once.
COMMON_SOURCES = ['src/renderer_adapter/live.cpp', 'src/renderer_scene/live_scene.cpp', 'src/renderer_scene/record_io.cpp', 'src/renderer_scene/live_sinks.cpp']


def read(root, rel):
    p = root / rel
    if not p.is_file():
        raise SystemExit(f'FAIL cannot read {rel} (under {root})')
    return p.read_text(encoding='utf-8', errors='replace')


def cmake_sources(text):
    m = re.search(r'set\(WWE2K14_SOURCES(.*?)\n\)', text, re.S)
    if not m:
        raise SystemExit('FAIL CMakeLists.txt: no set(WWE2K14_SOURCES ... ) block')
    return [l.split('#')[0].strip() for l in m.group(1).splitlines() if l.split('#')[0].strip()]


def run(root, plant=''):
    problems = []
    cm = read(root, 'CMakeLists.txt')
    sources = cmake_sources(cm)
    if plant == 'drop-source':
        sources = [s for s in sources if s != 'src/renderer_scene/arena_ours.cpp']
    # 2a. the ANDROID branch builds the same list, nothing removed
    if not re.search(r'if\(ANDROID\).*?add_library\(wwe2k14 SHARED \$\{WWE2K14_SOURCES\}\)', cm, re.S):
        problems.append('CMakeLists.txt: the ANDROID branch does not build ${WWE2K14_SOURCES} as the shared library')
    if re.search(r'list\(\s*REMOVE_ITEM\s+WWE2K14_SOURCES', cm):
        problems.append('CMakeLists.txt: list(REMOVE_ITEM WWE2K14_SOURCES ...) drops sources from a build')
    # 1. every cvar defined is in the table
    defined = set()
    for p in (root / 'src').rglob('*'):
        if p.suffix in ('.h', '.cpp'):
            defined.update(re.findall(r'REXCVAR_DEFINE_BOOL\(\s*(wwe2k14_rdr_\w+_ours)\b', p.read_text(encoding='utf-8', errors='replace')))
    if not defined:
        problems.append('no `wwe2k14_rdr_*_ours` cvar found under src\\ (the check cannot read its input)')
    for c in sorted(defined - set(TABLE)):
        problems.append(f'{c}: defined for the PC but not in this check\'s TABLE (add its sources, then it is checked for the phone)')
    for c in sorted(set(TABLE) - defined):
        problems.append(f'{c}: in the TABLE but no longer defined (remove it from the table)')
    reader = read(root, 'src/renderer_adapter/reader.h')
    app = read(root, 'src/wwe2k14_app.h')
    m = re.search(r'Check H2.*?\n(.*?g_crowd_game_root = game_data_root\(\)\.string\(\);)', app, re.S)
    root_cond = m.group(1) if m else ''
    if not root_cond:
        problems.append('wwe2k14_app.h: the game-root condition (Check H2 block) was not found')
    if plant == 'drop-switch':
        root_cond = root_cond.replace('wwe2k14_rdr_ring_ours', 'wwe2k14_rdr_REMOVED')
    for c in sorted(defined & set(TABLE)):
        srcs, headers, needs_root = TABLE[c]
        for s in srcs:
            if s not in sources:
                problems.append(f'{c}: {s} is not in CMakeLists.txt WWE2K14_SOURCES (the phone library would not carry it)')
        for f in srcs + headers:
            t = read(root, f)
            if re.search(r'#\s*if\s+!\s*defined\(\s*__ANDROID__\s*\)|#\s*ifndef\s+__ANDROID__', t):
                problems.append(f'{c}: {f} compiles something out on Android')
        if not re.search(r'REXCVAR_GET\(\s*' + c + r'\s*\)', reader):
            problems.append(f'{c}: reader.h never reads the switch into the live options (REXCVAR_GET)')
        if needs_root and c not in root_cond:
            problems.append(f'{c}: not in wwe2k14_app.h\'s game-root condition (the promotion would not find the player\'s files)')
    for s in COMMON_SOURCES:
        if s not in sources:
            problems.append(f'scene feed: {s} is not in WWE2K14_SOURCES')
    # 5. the dev way to switch them on
    dev = read(root, 'src/android_dev_flags.h')
    fam = 'wwe2k14_rdr_' in dev and 'wwe2k14_scene_' in dev and plant != 'drop-devflags'
    if not fam:
        problems.append('android_dev_flags.h does not accept the wwe2k14_rdr_ / wwe2k14_scene_ families')
    if 'android_dev_flags::Apply()' not in app:
        problems.append('wwe2k14_app.h never calls android_dev_flags::Apply()')
    if 'IsDevBuild' not in dev:
        problems.append('android_dev_flags.h is not limited to +dev builds (a release APK would read the file)')
    return problems, sorted(defined)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument('--plant', default='')
    a = ap.parse_args()
    root = Path(a.root).resolve()
    problems, defined = run(root, a.plant)
    if a.plant:
        if problems:
            print(f'planted fault "{a.plant}" caught ({len(problems)} problem(s)): {problems[0]}')
            return 0
        print(f'FAIL planted fault "{a.plant}" was NOT caught: the check cannot fail')
        return 1
    if problems:
        for p in problems:
            print('PROBLEM', p)
        print(f'phone_promotions: {len(problems)} problem(s)')
        return 1
    print(f'phone_promotions: 0 problems, {len(defined)} promotion switches carried by the phone build ({", ".join(d[len("wwe2k14_rdr_"):-5] for d in defined)})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
