r"""H2 / L3 SHARED-CACHE-STATE LINT (S103 L32F: the posed ring's slot state lived on ArenaSet, a process-wide per-arena object, but described ONE ModelCache's pools: the live and the mirror cache fought over it).

  py -I tools\gate\cache_state_lint.py [--root DIR]    exit 1 on a violation
  py -I tools\gate\cache_state_lint.py --selftest       plants the old ring_created / ring_scratch / ring_hash members back on a copy of rf_c.h

The structures held in the process-wide per-arena table (rf_c.h `struct ArenaSet`, rf_c_core.cpp `struct State` behind States()) are shared by every ModelCache (the live cache and the mirror cache each
build their own pools / images). A member there that describes a cache's pools is wrong: it must hang off the cache (ModelCache::RingStateSlot / ring_state keyed by the cache's address).
Flagged (members at the struct's top level, not functions):
  - a std::map / unordered_map / set keyed by std::string (a pool / slot key: `scratch[skey]`),
  - any member whose name or comment says pool / scratch / skey / created (a cache-pool bookkeeping word), unless it is keyed by the cache's address (const void* / ModelCache*), which is the fix.
BASELINE below = members that are shared on purpose, with the reason."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FILES = [('src/renderer_scene/rf_c.h', 'ArenaSet'), ('src/renderer_scene/rf_c_core.cpp', 'State')]
KEYED_STR = re.compile(r'std::(?:unordered_)?(?:map|set|multimap)<\s*std::string\b')
KEYED_CACHE = re.compile(r'std::(?:unordered_)?map<\s*(?:const\s+void\s*\*|const\s+ModelCache\s*\*|ModelCache\s*\*)')
WORDS = re.compile(r'\b(pool|pools|scratch|skey|created)\b|ring_(?:created|scratch|hash)\b', re.I)
BASELINE = {
    # (struct, member): reason
    ('ArenaSet', 'pcache'): 'the arenas own stage light-map pool pages (S103 L2: lightmap00 + span, copied from the arena pac): arena data, identical for every ModelCache, not one caches pools',
}


def struct_body(text, name):
    m = re.search(r'\bstruct\s+' + name + r'\s*\{', text)
    if not m:
        return None
    i, depth = m.end(), 1
    start = i
    while i < len(text) and depth:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return text[start:i - 1]


def members(body):
    """[(name, decl, comment)] of the top-level data members of a struct body: a declaration may span several lines (it ends at its ';' at brace depth 0);
    nested structs, inline function bodies and declarations with a '(' before any '=' (functions) are skipped."""
    out = []
    depth = 0
    cur, cm = '', ''
    for line in body.split('\n'):
        code, _, c = line.partition('//')
        cm += ' ' + c
        for ch in code:
            if ch == '{':
                depth += 1
                if depth == 1 and '=' not in cur:
                    cur += ' {'   # a nested struct / function body or a brace initializer: kept as text, the ';' decides
                continue
            if ch == '}':
                depth -= 1
                continue
            if depth > 0:
                continue
            if ch == ';':
                decl = cur.strip()
                head = decl.split('=')[0]
                if decl and '(' not in head and not decl.endswith('{') and not re.match(r'^(struct|class|enum|union|using|typedef|friend)\b', decl):
                    m = re.search(r'(\w+)\s*(?:\[[^\]]*\])?\s*(?:=.*)?$', decl, re.S)
                    if m:
                        out.append((m.group(1), re.sub(r'\s+', ' ', decl), cm))
                cur, cm = '', ''
            else:
                cur += ch
        if depth == 0 and not cur.strip():
            cm = ''
    return out


def lint_text(texts):
    bad = []
    for (path, sname), text in texts.items():
        body = struct_body(text, sname)
        if body is None:
            bad.append(f'{path}: struct {sname} not found (the lint needs updating)')
            continue
        # comments sitting on the lines above a member count too: join each member's own line comment only (the fix's own comments name pools on purpose)
        for name, decl, cm in members(body):
            if (sname, name) in BASELINE or KEYED_CACHE.search(decl):
                continue
            why = None
            if KEYED_STR.search(decl):
                why = 'is a container keyed by std::string (a pool / slot key: it describes one cache)'
            elif WORDS.search(name) or WORDS.search(cm):
                why = 'names pools / scratch / created slots'
            if why:
                bad.append(f'{path}: {sname}::{name} {why}; ArenaSet / State are shared by every ModelCache: hang it off the cache (ModelCache::RingStateSlot, keyed by the cache address)')
    return bad


def load(root):
    return {(p, s): open(os.path.join(root, p), encoding='utf-8', errors='replace').read() for p, s in FILES}


def selftest(root):
    ok = True
    texts = load(root)
    clean = lint_text(texts)
    print(f"  {'ok  ' if not clean else 'FAIL'} the current tree: {len(clean)} violation(s) {clean[:2]}")
    ok &= not clean
    key = ('src/renderer_scene/rf_c.h', 'ArenaSet')
    old = ('  std::map<std::string, assets::Model> ring_scratch;\n'
           '  std::map<std::string, bool> ring_created;   // pool -> its slot\'s front-end model exists (the scratch has no textures any more)\n'
           '  std::map<std::string, uint64_t> ring_hash;  // S103 C5: skey -> the hash of the rows the slot was last posed with\n')
    for label, plant in {'L32F: ring_created / ring_scratch / ring_hash on ArenaSet': old,
                         'a MULTI-LINE member (std::map<std::string,  newline  int> slot_img2;)': '  std::map<std::string,\n      int> slot_img2;\n',
                         'a plain pool map under another name (std::map<std::string, int> slot_img)': '  std::map<std::string, int> slot_img;\n',
                         'a member whose comment says pool (int ring_pool_n; // the cache pool count)': '  int ring_n = 0;  // the cache pool count\n'}.items():
        t = dict(texts)
        anchor = '  std::map<int, RingPiece> ring_piece;\n'
        if anchor not in t[key]:
            print('  FAIL the anchor line moved: update the selftest')
            ok = False
            continue
        t[key] = t[key].replace(anchor, anchor + plant)
        got = lint_text(t)
        good = bool(got)
        print(f"  {'ok  ' if good else 'FAIL'} {label}: {len(got)} flagged {[g.split(' ')[1] for g in got][:3]}")
        ok &= good
    print('selftest ' + ('PASSED' if ok else 'FAILED'))
    return ok


def main():
    a = sys.argv[1:]
    root = a[a.index('--root') + 1] if '--root' in a else ROOT
    if '--selftest' in a:
        sys.exit(0 if selftest(root) else 1)
    bad = lint_text(load(root))
    for b in bad:
        print('ERROR ' + b)
    print(f'cache_state_lint: {len(bad)} violation(s)')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
