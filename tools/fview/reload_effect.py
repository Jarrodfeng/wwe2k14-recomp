#!/usr/bin/env python3
"""Session 101 lane 2 (fview live reload): rebuilds ONE effect from its sources into loose .spv files, without touching the checked-in headers.
The same compiler path as tools\\renderer\\compile_shaders.py (its functions, imported), with its folders pointed at --fx-dir and its output at --out;
the variants are compiled in parallel, so a reload takes seconds.

  py -I tools\\fview\\reload_effect.py --effect yAudience --fx-dir DIR --out OUTDIR [--only IDENT[,IDENT]]

Writes OUTDIR\\<effect>_<variant>_<vertex|fragment>.spv and, for a fragment variant that has an alpha-test twin, ....alpha.spv. Prints one JSON line:
{"effect":..., "files":[...], "failed":[...]} ; exit 1 when anything failed (a shader that does not compile is said loudly, never skipped).
"""
import argparse
import concurrent.futures
import importlib.util
import json
import os
import re
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))


def load_compiler(fx_dir):
    spec = importlib.util.spec_from_file_location('compile_shaders', os.path.join(ROOT, 'tools', 'renderer', 'compile_shaders.py'))
    cs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cs)
    cs.FX = os.path.abspath(fx_dir)
    return cs


# ---- S101 lane 24: a compile error names the SOURCE file and line -------------------------------------------------------------
# compile_text() gets the wrapped text (includes pasted in, a prologue added) and glslang reports a line of THAT text. The offending line (with the one before it)
# is looked up in the sources the wrapper was made from, so the message says "yAudience.frag:23" instead of "shader.frag:211".
SOURCE_EXT = ('.frag', '.vert', '.glsl')


def install_error_locator(cs):
    original = cs.compile_text
    files = []
    for folder in (cs.FX, cs.SOURCE):
        if os.path.isdir(folder):
            for name in sorted(os.listdir(folder)):
                if name.endswith(SOURCE_EXT) and os.path.isfile(os.path.join(folder, name)):
                    files.append(os.path.join(folder, name))
    cache = {}

    def lines_of(path):
        if path not in cache:
            with open(path, encoding='utf-8', errors='replace') as f:
                cache[path] = [x.strip() for x in f.read().split('\n')]
        return cache[path]

    def locate(text_lines, number):
        if not 1 <= number <= len(text_lines):
            return None
        here = text_lines[number - 1].strip()
        before = text_lines[number - 2].strip() if number >= 2 else None
        best = None
        for path in files:
            src = lines_of(path)
            for i, line in enumerate(src):
                if line == here:
                    if before is None or (i > 0 and src[i - 1] == before):
                        return os.path.basename(path), i + 1
                    if best is None:
                        best = (os.path.basename(path), i + 1)
        return best

    def compile_text(text, defines, stage):
        try:
            return original(text, defines, stage)
        except RuntimeError as error:
            message = str(error)
            text_lines = text.replace('\r\n', '\n').split('\n')
            out = []
            for m in re.finditer(r'(ERROR|WARNING): [^\n]*?:(\d+): ([^\n]*)', message):
                where = locate(text_lines, int(m.group(2)))
                out.append(f'{where[0]}:{where[1]}: {m.group(3)}' if where else f'(generated line {m.group(2)}) {m.group(3)}')
            raise RuntimeError('; '.join(out) if out else message.strip()[:600]) from None

    cs.compile_text = compile_text


def write_words(path, words):
    with open(path, 'wb') as f:
        f.write(struct.pack(f'<{len(words)}I', *words))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--effect', required=True)
    ap.add_argument('--fx-dir', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--only', default='')
    # S101 lane 24 (live reload in the viewer): --write-layouts also writes OUTDIR\<ident>.json (the variant's layout, the format SCENE_FRONTEND.md "Live reload"
    # describes) so the viewer can check a rebuilt variant against its compiled table; a failure is also said on stderr as "FAILED <ident>: file:line: message".
    ap.add_argument('--write-layouts', action='store_true')
    args = ap.parse_args()
    json_path = os.path.join(args.fx_dir, args.effect + '.fx.json')
    if not os.path.isfile(json_path):
        print(json.dumps({'effect': args.effect, 'files': [], 'failed': ['no such effect description: ' + json_path]}))
        return 1
    cs = load_compiler(args.fx_dir)
    install_error_locator(cs)  # S101 lane 24
    os.makedirs(args.out, exist_ok=True)
    with open(json_path, encoding='utf-8') as f:
        spec = json.load(f)
    effect = spec['effect']
    only = {x for x in args.only.split(',') if x}

    def source_of(variant, extension):
        return os.path.join(cs.FX, variant.get('source', effect) + extension)

    # Also printed: "layouts", what the registry would hold for each variant (constant ranges with offsets, attributes, samplers, block size), so fview can refuse an
    # override whose layout differs from the compiled table; and a target_16 that differs from the checked-in description is a failure (the registry does not record it).
    layouts = {}
    layout_jobs = []
    root_spec = None
    root_json = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx', args.effect + '.fx.json')
    if os.path.isfile(root_json) and os.path.abspath(root_json) != os.path.abspath(json_path):
        with open(root_json, encoding='utf-8') as f:
            root_spec = json.load(f)
    layout_failed = []
    kinds = {'2d': 0, 'cube': 1, 'volume': 2, '3d': 2}

    def layout_of(v, path, defines, block):
        offsets, size = cs.block_layout(path, defines, block)
        ranges = [[c['register'], c['count'], offsets[c['field']]] for c in v.get('constants', []) if c['field'] in offsets]
        return {'ranges': ranges, 'attributes': [[a['fetch'], a['location'], 1 if a.get('integer') else 0] for a in v.get('attributes', [])],
                'samplers': [[s['slot'], kinds.get(s.get('kind', '2d'), 0)] for s in v.get('samplers', [])], 'block_bytes': size, 'bools': offsets.get('Bools', 0xFFFF)}

    jobs = []  # (ident, path, words-producer)
    vertex_variants = list(spec.get('vertex', []))
    if effect.startswith('yCh_'):
        vertex_variants += [{**v, 'name': v['name'] + '_velocity', 'defines': list(v.get('defines', [])) + ['VELOCITY'], 'hashes': []} for v in spec.get('vertex', [])]
    for v in vertex_variants:
        ident = f'{effect}_{v["name"]}_vertex'
        if only and ident not in only:
            continue
        defines = v.get('defines', [])
        layout_jobs.append((ident, lambda v=v, defines=defines: layout_of(v, source_of(v, '.vert'), defines, 'VertexConstants')))
        integer_locations = {a['location'] for a in v.get('attributes', []) if a.get('integer')}
        jobs.append((ident, lambda v=v, defines=defines, integer_locations=integer_locations: cs.compile_text(
            cs.wrapped_source(source_of(v, '.vert'), 'vert', False, None, integer_locations, defines=defines), defines, 'vert')))
    for v in spec.get('fragment', []):
        ident = f'{effect}_{v["name"]}_fragment'
        if only and ident not in only:
            continue
        defines = v.get('defines', [])
        target_16 = v.get('target_16')
        layout_jobs.append((ident, lambda v=v, defines=defines: layout_of(v, source_of(v, '.frag'), defines, 'FragmentConstants')))
        if root_spec is not None:
            before = next((r for r in root_spec.get('fragment', []) if r.get('name') == v['name']), None)
            if before is not None and before.get('target_16') != target_16:
                layout_failed.append(f'{ident}: target_16 {target_16!r} differs from the checked-in description ({before.get("target_16")!r}): rebuild the registry (tools/renderer/compile_shaders.py)')
        jobs.append((ident, lambda v=v, defines=defines, target_16=target_16: cs.compile_text(
            cs.wrapped_source(source_of(v, '.frag'), 'frag', False, target_16, defines=defines), defines, 'frag')))
        if v.get('alpha_test', True):
            jobs.append((ident + '.alpha', lambda v=v, defines=defines, target_16=target_16: cs.compile_text(
                cs.wrapped_source(source_of(v, '.frag'), 'frag', True, target_16, defines=defines), defines, 'frag')))
    files, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        futures = [(ident, pool.submit(job)) for ident, job in jobs]
        layout_futures = [(ident, pool.submit(job)) for ident, job in layout_jobs]
        for ident, future in futures:
            try:
                words = future.result()
                path = os.path.join(args.out, ident + '.spv')
                write_words(path, words)
                files.append(ident + '.spv')
            except Exception as error:  # a shader that fails to build is reported, with the compiler's text
                failed.append(f'{ident}: {error}')
        for ident, future in layout_futures:
            try:
                layouts[ident] = future.result()
            except Exception as error:
                failed.append(f'{ident}: layout: {error}')
    failed += layout_failed
    if args.write_layouts:
        for ident, layout in layouts.items():
            with open(os.path.join(args.out, ident + '.json'), 'w', encoding='utf-8', newline='\n') as f:
                json.dump({'ranges': layout['ranges'], 'attributes': layout['attributes'], 'samplers': layout['samplers'], 'block_bytes': layout['block_bytes'], 'bools': layout['bools']}, f, separators=(',', ':'))
    if args.write_layouts:  # a fragment variant whose description says "alpha_test": false has no alpha-test twin: a marker file says so (the viewer's folder loader reads it)
        for v in spec.get('fragment', []):
            ident = f'{effect}_{v["name"]}_fragment'
            if not v.get('alpha_test', True) and ident + '.spv' in files and not (only and ident not in only):
                open(os.path.join(args.out, ident + '.notwin'), 'w').close()
    for line in failed:
        print('FAILED ' + line.replace('\n', ' '), file=sys.stderr)
    print(json.dumps({'effect': effect, 'files': files, 'failed': failed, 'layouts': layouts}))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
