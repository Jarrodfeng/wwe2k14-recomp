#!/usr/bin/env python3
"""picture_resolver_lint (S103 PN4F, from the Opus review of landing 2's picture wiring): the rules the review found broken, held as checks.

  D1  no pac read on the host (presentation) thread: `loader_.Load` / `pac::Open` / `UiFileLoader` are reachable only inside a function named *Worker (picture_resolver.h: LoadWorker); the host's
      callbacks (src\\renderer_host\\host_start.cpp, scene_host.cpp) and the resolver's Resolve / EndFrame / FileImageOf / Drain never touch the loader.
  D2  every file that makes a UiFileLoader and calls its Load also calls ReleasePacs (a batch of loads ends with the decoded pacs released: hundreds of MB).
  B1  every namespace-scope std::unique_ptr in host_start.cpp (it holds a renderer reference or is used by a thread) is reset / released in StopForGame or in a function StopForGame calls, in an order that puts the resolver
      before the host (the host destroys the renderer) and the host's own `on_stop` is wired.
  B4  the stream-free host's cache folders come from the runtime cache root: wwe2k14_app.h sets g_ui_index_root = cache_root() / "ui_index" and host_start.cpp uses it (no parent_path() of another folder).

  py -I tools\\scene\\picture_resolver_lint.py --root .      scan (exit 1 on a violation)
  py -I tools\\scene\\picture_resolver_lint.py --selftest    planted twins: each bad snippet MUST be flagged, the good ones must pass
"""
import argparse
import os
import re
import sys

RESOLVER = 'src/renderer_scene/picture_resolver.h'
HOST = ['src/renderer_host/host_start.cpp', 'src/renderer_host/scene_host.cpp']
APP = 'src/wwe2k14_app.h'
LOADER_USERS = ['src/renderer_scene/picture_resolver.h', 'src/renderer_scene/ui_ours.cpp']


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def functions(text):
    """[(name, start_line, body_text)] for every brace-delimited function / lambda body found by `name(...) ... {`."""
    out = []
    for m in re.finditer(r'(?:^|\n)[ \t]*(?:inline |static |void |bool |auto |[A-Za-z_:<>]+[ &*]+)*([A-Za-z_][\w:]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:->\s*[\w:<>]+\s*)?\{', text):
        name = m.group(1)
        if name in ('if', 'for', 'while', 'switch', 'catch', 'return'):
            continue
        i = m.end() - 1
        depth = 0
        j = i
        while j < len(text):
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((name.split('::')[-1], text.count('\n', 0, i) + 1, text[i:j + 1]))
    return out


LOADER_USE = re.compile(r'\bloader_\s*\.\s*Load\s*\(|\bpac::Open\s*\(|\bUiFileLoader\b|\bloader_\s*\.\s*ReleasePacs')


def check_resolver(text, name=RESOLVER):
    """D1 inside the resolver header: the loader is used only in *Worker functions (and the ctor initialiser / member declaration)."""
    probs = []
    t = strip_comments(text)
    fns = functions(t)
    for fname, line, body in fns:
        if LOADER_USE.search(body) and not fname.endswith('Worker') and fname not in ('PictureResolver', '~PictureResolver'):
            probs.append(f'{name}:{line}: `{fname}` touches the pac loader but is not a *Worker function (D1: a pac read on the host thread freezes the presentation)')
    for need in ('Resolve', 'EndFrame', 'FileImageOf', 'Drain'):
        if not any(f == need for f, _, _ in fns):
            probs.append(f'{name}: function {need} not found (the lint cannot check it)')
    if not any(f.endswith('Worker') and 'loader_.Load' in b for f, _, b in fns):
        probs.append(f'{name}: no *Worker function reads files any more (loader_.Load not found in one)')
    return probs


def check_host_callbacks(text, name):
    probs = []
    t = strip_comments(text)
    for m in re.finditer(r'\bloader\w*\s*[.>-]+\s*Load\s*\(|\bpac::Open\s*\(|\bUiFileLoader\b|\bBuildPacIndex\s*\(', t):
        line = t.count('\n', 0, m.start()) + 1
        ctx = t[max(0, m.start() - 1500):m.start()]
        # BuildPacIndex belongs to the index worker thread (a lambda given to std::thread); anything else in this file is a host-thread read
        if 'BuildPacIndex' in m.group(0) and 'std::thread' in ctx:
            continue
        probs.append(f'{name}:{line}: `{m.group(0).strip()}` in the host unit outside a thread lambda (D1)')
    return probs


def check_releases(text, name):
    """D2: a file that makes a UiFileLoader (or a resolver that owns one) and calls Load must call ReleasePacs too."""
    t = strip_comments(text)
    if re.search(r'\bLoad\s*\(', t) and re.search(r'\bUiFileLoader\b', t) and not re.search(r'\bReleasePacs\s*\(', t):
        return [f'{name}: uses UiFileLoader::Load but never calls ReleasePacs (D2: the decoded pacs stay resident)']
    return []


def check_host_start(text, name='src/renderer_host/host_start.cpp'):
    probs = []
    t = strip_comments(text)
    owners = re.findall(r'^std::unique_ptr<[^;]*?>\s+(g_\w+)\s*(?:;|\{)', t, flags=re.M)
    fns = {f: b for f, _, b in functions(t)}
    stop = fns.get('StopForGame', '')
    if not stop:
        return [f'{name}: StopForGame not found']
    called = [f for f in fns if f != 'StopForGame' and re.search(r'\b' + re.escape(f) + r'\s*\(', stop)]
    scope = stop + ''.join(fns[f] for f in called)
    for g in owners:
        if not re.search(r'\b' + g + r'\s*(?:\.|->)\s*(?:reset|release)\s*\(', scope):
            probs.append(f'{name}: namespace-scope unique_ptr `{g}` is not reset / released in StopForGame (or a function it calls): a static destructor would run against a stopped renderer (B1)')
    if 'g_resolver_owner' in owners and 'g_host' in owners:
        pos = {}
        for g in ('g_resolver_owner', 'g_host'):
            ms = [m.start() for f in ([stop] + [fns[c] for c in called]) for m in re.finditer(r'\b' + g + r'\s*(?:\.|->)\s*(?:reset|release|Stop)\s*\(', f)]
            pos[g] = min(ms) if ms else -1
    if not re.search(r'\bon_stop\s*=', t):
        probs.append(f'{name}: HostOptions::on_stop is not wired (the resolver must end before SceneHost resets the renderer; B1)')
    if re.search(r'parent_path\s*\(\s*\)', t):
        probs.append(f'{name}: a cache folder is derived with parent_path(); use the runtime cache root (g_ui_index_root; B4)')
    if not re.search(r'\bg_ui_index_root\b', t):
        probs.append(f'{name}: the ui_index cache does not come from g_ui_index_root (B4)')
    return probs


def check_app(text, name=APP):
    t = strip_comments(text)
    if not re.search(r'g_ui_index_root\s*=\s*\(\s*runtime\(\)\s*->\s*cache_root\(\)\s*/\s*"ui_index"\s*\)', t):
        return [f'{name}: g_ui_index_root is not set from runtime()->cache_root() / "ui_index" (B4)']
    return []


def check_scene_host_stop(text, name='src/renderer_host/scene_host.cpp'):
    t = strip_comments(text)
    m = re.search(r'void SceneHost::Stop\s*\(\s*\)\s*\{', t)
    if not m:
        return [f'{name}: SceneHost::Stop not found']
    body = t[m.start():]
    body = body[:body.index('\n}\n')] if '\n}\n' in body else body
    a, b, c = body.find('core_.Stop'), body.find('on_stop'), body.find('renderer_.reset')
    if min(a, b, c) < 0 or not (a < b < c):
        return [f'{name}: SceneHost::Stop must call core_.Stop, then opts_.on_stop, then renderer_.reset (B1)']
    return []


def scan(root):
    probs = []

    def rd(rel):
        with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
            return f.read()

    probs += check_resolver(rd(RESOLVER))
    for h in HOST:
        probs += check_host_callbacks(rd(h), h)
    for u in LOADER_USERS:
        probs += check_releases(rd(u), u)
    probs += check_host_start(rd(HOST[0]), HOST[0])
    probs += check_scene_host_stop(rd(HOST[1]), HOST[1])
    probs += check_app(rd(APP))
    return probs


GOOD_RESOLVER = """
class PictureResolver {
  UiFileLoader loader_;
};
inline PictureResolver::PictureResolver(int a) : loader_(a) {}
inline void PictureResolver::LoadWorker() { loader_.Load(e, 1, l, err); loader_.ReleasePacs(); }
inline void PictureResolver::Drain() { x(); }
inline rdr::ImageId PictureResolver::FileImageOf(uint32_t e) { return 0; }
inline rdr::TextureSlot PictureResolver::Resolve(const Quad2D& row) { return {}; }
inline void PictureResolver::EndFrame(uint64_t f) { }
"""
GOOD_HOST = """
std::unique_ptr<SceneHost> g_host;
std::unique_ptr<rdr_scene::PictureResolver> g_resolver_owner;
std::unique_ptr<rdr_scene::UiFileIndex> g_index;
void StopPictures() { g_resolver_owner.reset(); (void)g_index.release(); }
HostOptions BuildOptions() { HostOptions o; o.on_stop = &StopPictures; return o; }
static void StartPictures(SceneHost& host) { const fs::path dir = fs::path(wwe2k14::rdr_reader::g_ui_index_root); }
void StopForGame() {
  if (g_host) g_host->Stop();
  StopPictures();
  (void)g_host.release();
}
"""
GOOD_STOP = "void SceneHost::Stop() {\n  core_.Stop();\n  LogFinal();\n  if (opts_.on_stop) opts_.on_stop();\n  renderer_.reset();\n}\n"
GOOD_APP = '    wwe2k14::rdr_reader::g_ui_index_root = (runtime()->cache_root() / "ui_index").string();\n'


def selftest():
    cases = []   # (name, problems it must produce a substring of, or None for must-pass)
    cases.append(('good resolver', check_resolver(GOOD_RESOLVER), None))
    cases.append(('good host start', check_host_start(GOOD_HOST), None))
    cases.append(('good scene_host stop', check_scene_host_stop(GOOD_STOP), None))
    cases.append(('good app', check_app(GOOD_APP), None))
    cases.append(('good releases', check_releases('UiFileLoader l; l.Load(a); l.ReleasePacs();', 'x'), None))
    cases.append(('load in Resolve', check_resolver(GOOD_RESOLVER.replace('{ return {}; }', '{ loader_.Load(e, 1, l, err); return {}; }')), 'not a *Worker'))
    cases.append(('load in Drain', check_resolver(GOOD_RESOLVER.replace('{ x(); }', '{ loader_.Load(a, 1, l, e); }')), 'not a *Worker'))
    cases.append(('pac::Open in FileImageOf', check_resolver(GOOD_RESOLVER.replace('{ return 0; }', '{ pac::Open(a); return 0; }')), 'not a *Worker'))
    cases.append(('no worker read', check_resolver(GOOD_RESOLVER.replace('loader_.Load(e, 1, l, err); ', '')), 'no *Worker'))
    cases.append(('host callback loads', check_host_callbacks('o.picture = [](auto& r) { UiFileLoader l(g, f); return l.Load(e); };', 'h'), 'D1'))
    cases.append(('index worker allowed', check_host_callbacks('std::thread([] { BuildPacIndex(o, pac, e, err); });', 'h'), None))
    cases.append(('load without release', check_releases('UiFileLoader l; l.Load(a);', 'x'), 'ReleasePacs'))
    cases.append(('unique_ptr never reset', check_host_start(GOOD_HOST.replace('(void)g_host.release();', '')), 'g_host'))
    cases.append(('resolver not reset', check_host_start(GOOD_HOST.replace('g_resolver_owner.reset(); ', '')), 'g_resolver_owner'))
    cases.append(('index leaked into static dtor', check_host_start(GOOD_HOST.replace('(void)g_index.release();', '')), 'g_index'))
    cases.append(('on_stop not wired', check_host_start(GOOD_HOST.replace('o.on_stop = &StopPictures; ', '')), 'on_stop'))
    cases.append(('cache by parent_path', check_host_start(GOOD_HOST.replace('fs::path(wwe2k14::rdr_reader::g_ui_index_root)', 'fs::path(g_pipeline_store_root).parent_path() / "ui_index"')), 'parent_path'))
    cases.append(('app sets the cache elsewhere', check_app(GOOD_APP.replace('cache_root()', 'user_data_root()')), 'g_ui_index_root'))
    cases.append(('stop order', check_scene_host_stop(GOOD_STOP.replace('  if (opts_.on_stop) opts_.on_stop();\n', '')), 'on_stop'))
    cases.append(('stop after reset', check_scene_host_stop("void SceneHost::Stop() {\n  core_.Stop();\n  renderer_.reset();\n  if (opts_.on_stop) opts_.on_stop();\n}\n"), 'on_stop'))
    bad = 0
    for name, probs, want in cases:
        if want is None:
            ok = not probs
        else:
            ok = any(want in p for p in probs)
        print(('PASS' if ok else 'FAIL') + '  ' + name + ('' if ok else '  -> ' + str(probs)))
        bad += not ok
    print('%s: picture_resolver_lint selftest (%d cases)' % ('FAIL' if bad else 'PASS', len(cases)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    probs = scan(os.path.abspath(a.root))
    for p in probs:
        print('FAIL  ' + p)
    print('%s: picture_resolver_lint (%d problems)' % ('FAIL' if probs else 'PASS', len(probs)))
    return 1 if probs else 0


if __name__ == '__main__':
    sys.exit(main())
