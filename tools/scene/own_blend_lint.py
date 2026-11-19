#!/usr/bin/env python3
"""S103 P7 tween lint (review of lane TW, F1 / F3 / F8): the rules of the host's own blend tags, read from the tree (nothing is run).

  py -I tools\\scene\\own_blend_lint.py [--root DIR]      scan (exit 1 on a violation)
  py -I tools\\scene\\own_blend_lint.py --selftest        the planted twins: each bad snippet MUST be flagged

Rules:
  1. SetBlendTags( is called only under src\\renderer_host\\ (the stream build never switches tags on; the definition in src\\renderer\\frame_builder.h is the one other place).
  2. every fb.Draw / builder.Draw / b.Draw site of src\\renderer_scene\\ is either tagged or on tools\\scene\\own_blend_sites.txt as `untagged` with a reason; a file's site count must match.
  3. an own blend key never takes a raw Object::id (a byte offset into the record's string blob: not stable between ticks) or another per-record table index: the 14 lines before every
     TagOwnBlend( call in src\\ must not read `.id` / `.params_first` / `.bones_first` / `.light_set` / `.mesh_first` of a record object (use the id's TEXT: OwnNameHash(names->Str(id))).
  4. a cvar named by a string in an OFF-only unit (tools\\scene\\keep_stream_off_sources.txt: GetFlagInfo / GetFlagByName / SetFlagByName / Query / flag_text) is DEFINED by a unit the
     OFF build compiles: REXCVAR_DEFINE_* in an OFF-only unit, or in a file that is not a stream header (tools\\scene\\keep_stream_headers.txt).
"""
import argparse
import os
import re
import sys

SKIP_DIRS = {'out', 'generated', 'thirdparty', 'game_data', '.git'}


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def src_files(root, sub, exts=('.cpp', '.h', '.inc')):
    base = os.path.join(root, sub)
    for d, dirs, files in os.walk(base):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for n in files:
            if n.endswith(exts):
                yield os.path.join(d, n)


def rel(root, p):
    return os.path.relpath(p, root).replace('\\', '/')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def check_set_blend_tags(texts):
    bad = []
    for path, text in texts:
        if path.startswith('src/renderer_host/') or path == 'src/renderer/frame_builder.h':
            continue
        if re.search(r'\bSetBlendTags\s*\(', strip_comments(text)):
            bad.append(f'{path}: SetBlendTags( outside src/renderer_host/')
    return bad


DRAW = re.compile(r'\b(?:fb|builder|b)\.Draw\s*\(')


def load_sites(root):
    sites = {}
    for ln in read(os.path.join(root, 'tools/scene/own_blend_sites.txt')).splitlines():
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        parts = [p.strip() for p in ln.split('|')]
        if len(parts) < 4:
            sites[ln] = None
            continue
        sites[parts[0]] = (parts[1], int(parts[2]) if parts[2].isdigit() else -1, parts[3] if len(parts) > 3 else '')
    return sites


def check_draw_sites(texts, sites):
    bad = []
    seen = set()
    for path, text in texts:
        if not path.startswith('src/renderer_scene/'):
            continue
        n = len(DRAW.findall(strip_comments(text)))
        if not n:
            continue
        seen.add(path)
        row = sites.get(path)
        if not row:
            bad.append(f'{path}: {n} Draw site(s) and no row in own_blend_sites.txt (tag them with TagOwnBlend or list them untagged with a reason)')
            continue
        kind, count, reason = row
        if count != n:
            bad.append(f'{path}: {n} Draw site(s), own_blend_sites.txt says {count} (a new site must be tagged or listed)')
        if kind == 'untagged' and not reason:
            bad.append(f'{path}: untagged row without a reason')
        if kind == 'tagged':
            calls = len(re.findall(r'\bTagOwnBlend\s*\(', strip_comments(text)))
            if calls < count:
                bad.append(f'{path}: {count} tagged site(s) listed but only {calls} TagOwnBlend call(s)')
        if kind not in ('tagged', 'untagged'):
            bad.append(f'{path}: row kind must be tagged or untagged, got {kind!r}')
    for p in sites:
        if p not in seen:
            bad.append(f'{p}: listed in own_blend_sites.txt but has no Draw site any more (remove the row)')
    return bad


RAW = re.compile(r'\.(?:id|params_first|bones_first|light_set|mesh_first)\b')


def check_raw_ids(texts):
    bad = []
    for path, text in texts:
        if path == 'src/renderer_scene/own_blend.h':
            continue
        lines = strip_comments(text).split('\n')
        for i, ln in enumerate(lines):
            if not re.search(r'\bTagOwnBlend\s*\(', ln):
                continue
            window = lines[max(0, i - 14):i + 1]
            for j, w in enumerate(window):
                if RAW.search(w):
                    bad.append(f'{path}:{i - len(window) + j + 2}: a raw record id / table index within 14 lines of TagOwnBlend ({w.strip()[:90]})')
    return bad


def off_sources(root):
    out = []
    for ln in read(os.path.join(root, 'tools/scene/keep_stream_off_sources.txt')).splitlines():
        ln = ln.strip()
        if ln and not ln.startswith('#'):
            out.append(ln.replace('\\', '/'))
    return out


NAME_USE = re.compile(r'(?:GetFlagInfo|GetFlagByName|SetFlagByName|Query<[^>]*>|flag_text)\s*\(\s*"(wwe2k14_[A-Za-z0-9_]+)"')
DEFINE = re.compile(r'REXCVAR_DEFINE_[A-Z0-9_]+\s*\(\s*(wwe2k14_[A-Za-z0-9_]+)')


def not_stream_only_lines(text):
    """Lines of text that are NOT inside a `#if WWE2K14_STREAM()` branch (those exist only when the stream is kept)."""
    out, stack = [], []   # stack of "this branch is stream-only"
    for ln in text.split('\n'):
        m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', ln)
        if m:
            d, rest = m.group(1), re.sub(r'\s|//.*', '', m.group(2))
            if d == 'if':
                stack.append([rest.startswith('WWE2K14_STREAM()'), rest.startswith('!WWE2K14_STREAM()'), rest.startswith('WWE2K14_STREAM()')])
            elif d in ('ifdef', 'ifndef'):
                stack.append([False, False, False])
            elif d == 'else' and stack:
                stack[-1][2] = stack[-1][1]
            elif d == 'elif' and stack:
                stack[-1][2] = False
            elif d == 'endif' and stack:
                stack.pop()
            continue
        if not any(s[2] for s in stack):
            out.append(ln)
    return '\n'.join(out)


def check_off_cvars(root, texts):
    bad = []
    off = off_sources(root)
    headers = set()
    for ln in read(os.path.join(root, 'tools/scene/keep_stream_headers.txt')).splitlines():
        ln = ln.strip()
        if ln and not ln.startswith('#'):
            headers.add('src/' + ln.replace('\\', '/'))
    by_path = dict(texts)
    defined = set()
    for path, text in texts:
        if path in headers:
            continue
        body = not_stream_only_lines(text) if path in off else text
        for m in DEFINE.finditer(strip_comments(body)):
            defined.add(m.group(1))
    for path in off:
        text = by_path.get(path)
        if text is None:
            continue
        for m in NAME_USE.finditer(strip_comments(not_stream_only_lines(text))):
            if m.group(1) not in defined:
                bad.append(f'{path}: reads cvar {m.group(1)} by name, but no unit the OFF build compiles defines it (define it in host_start.cpp / owner_cvars_off.cpp under #if !WWE2K14_STREAM())')
    return bad


def selftest():
    fails = 0

    def expect(name, bad):
        nonlocal fails
        if not bad:
            print(f'SELFTEST FAIL: {name} was not flagged')
            fails += 1
        else:
            print(f'selftest ok: {name} flagged')
    expect('SetBlendTags outside the host', check_set_blend_tags([('src/renderer_scene/x.cpp', 'void f(rdr::FrameBuilder& fb){ fb.SetBlendTags(true); }')]))
    expect('an untagged unlisted Draw site', check_draw_sites([('src/renderer_scene/new_pass.cpp', 'void f(){ fb.Draw(op); }')], {}))
    expect('a raw Object::id in an own key', check_raw_ids([('src/renderer_scene/x.cpp', 'uint64_t what = uint64_t(oi.id) * 3;\nTagOwnBlend(fb, op, v, 1, 64, what);')]))
    ok = check_raw_ids([('src/renderer_scene/x.cpp', 'uint64_t what = oi.name_hash;\nTagOwnBlend(fb, op, v, 1, 64, what);')])
    if ok:
        print('SELFTEST FAIL: a name-hash key was flagged', ok)
        fails += 1
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return 1 if selftest() else 0
    root = os.path.abspath(a.root)
    texts = [(rel(root, p), read(p)) for p in src_files(root, 'src')]
    bad = []
    bad += check_set_blend_tags(texts)
    bad += check_draw_sites(texts, load_sites(root))
    bad += check_raw_ids(texts)
    bad += check_off_cvars(root, texts)
    for b in bad:
        print('FAIL:', b)
    print(f'own_blend_lint: {len(texts)} files, {len(bad)} problem(s)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
