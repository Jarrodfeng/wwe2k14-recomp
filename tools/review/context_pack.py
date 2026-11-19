r"""H2 CONTEXT PACK: one markdown file a reviewer starts from (S103: reviews re-found, by reading, what a grep of the callers / readers would have shown at once).

  py -I tools\review\context_pack.py BRANCH [--base REF] [--out FILE] [--max-kb 400] [--no-q]
  py -I tools\review\context_pack.py --selftest

BRANCH is diffed against the merge-base with --base (default main). The pack (default out\review\<branch>_pack.md) holds:
  1. the diff stat (per file, +/-) and the commit list;
  2. each CHANGED FUNCTION (from the hunk headers and the added definitions) with its CALLERS in our code (grep over src\ and tools\, definition lines and the changed files' own hunks left out);
  3. each CHANGED STRUCT FIELD (an added / removed member declaration in a header) with its READERS and WRITERS in our code (grep: a write is `field =`, `field++`, `.push_back`, `.emplace`, `.insert`, `.clear()`);
  3b. each NEW CONTAINER member (an added std::map / unordered_map / set / vector / deque / list / queue field) with its insert sites and its erase / clear / pop / swap sites
      (S103 H3: a container with inserts and no shrink site is the "unbounded cache" finding of L31A and L33F F2, listed for the reviewer to read at once);
  4. each GAME ADDRESS (0x82xxxxxx) and sub_XXXXXXXX named in the diff: `q callers` (tools\q, under 1 s; skipped with --no-q or when q is not usable) and, when its sqlite exists, fields.py `func`.
  5. `q ours diff BRANCH` (S103 QB, tools\q\ours_overlay.py): the clang-AST answer: changed functions / variables, their callers, readers and writers in the BASE index
     and in the BRANCH, and every caller / reader that exists in only one of the ON / OFF builds (builds the branch overlay once, ~1 min; skipped with --no-q).
EVERY GREP READS THE BRANCH, NOT THE CHECKOUT (S103 H3: `git grep -P REV`; the old pack grepped whatever the reviewer's working tree held, so a function the branch
calls showed 0 callers in a checkout from before the merge). Every section is capped (lines per symbol, symbols per section, total --max-kb): what was cut is said in the pack. The pack is DATA for the reviewer: read the lines it points at before trusting a count."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SRC_EXT = ('.cpp', '.h', '.hpp', '.cc', '.inc', '.py', '.ps1', '.json', '.glsl', '.txt')
SCAN_EXT = ('.cpp', '.h', '.hpp', '.cc', '.inc', '.py', '.ps1')
PER_SYMBOL = 10
MAX_SYMBOLS = 60
SKIP_NAMES = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'main', 'operator', 'catch', 'else'}
FUNC_HDR = re.compile(r'([A-Za-z_~][\w:~]*)\s*\([^;]*$')
DEF_LINE = re.compile(r'^\+\s*(?:[\w:<>,\*&\s~]+?\s)?(?:(\w+)::)?(~?[A-Za-z_]\w{3,})\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{?\s*$')
FIELD_LINE = re.compile(r'^[+-]\s+(?!return|using|typedef|friend|static_assert|#)(?:[\w:<>,\*&\s]+?[\s\*&>])(\w+)\s*(?:\[[^\]]*\])?\s*(?:=\s*[^;(]*)?;\s*(?://.*)?$')
ADDR = re.compile(r'\b(0x82[0-9A-Fa-f]{6})\b|\bsub_(82[0-9A-Fa-f]{6})\b')


def run(cmd, cwd=ROOT, timeout=60):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace', timeout=timeout)
        return r.stdout
    except (OSError, subprocess.TimeoutExpired):
        return ''


def git(*a, root=ROOT):
    return run(['git', '-C', root] + list(a), timeout=120)


def parse_diff(diff):
    """{file: {'hunks': [(header, [lines])]}} from `git diff -U0`."""
    files, cur, hunk = {}, None, None
    for ln in diff.split('\n'):
        if ln.startswith('diff --git'):
            cur = ln.split(' b/', 1)[-1]
            files[cur] = []
            hunk = None
        elif ln.startswith('@@') and cur:
            m = re.match(r'@@ [^@]*@@\s*(.*)$', ln)
            hunk = (m.group(1) if m else '', [])
            files[cur].append(hunk)
        elif hunk is not None and (ln.startswith('+') or ln.startswith('-')) and not ln.startswith(('+++', '---')):
            hunk[1].append(ln)
    return files


def changed_functions(files):
    """{name: set(files)}: the function a hunk sits in (hunk header) and definitions the diff adds."""
    out = {}
    for f, hunks in files.items():
        if not f.endswith(('.cpp', '.h', '.hpp', '.cc', '.inc')):
            continue
        for header, lines in hunks:
            m = FUNC_HDR.search(header)
            if m:
                n = m.group(1).split('::')[-1]
                if n not in SKIP_NAMES and len(n) > 2:
                    out.setdefault(n, set()).add(f)
            for ln in lines:
                if ln.startswith('+'):
                    d = DEF_LINE.match(ln)
                    if d and d.group(2) not in SKIP_NAMES:
                        out.setdefault(d.group(2), set()).add(f)
    return out


def changed_fields(files):
    """{field: set(files)} from member-like lines added / removed in headers."""
    out = {}
    for f, hunks in files.items():
        if not f.endswith(('.h', '.hpp')):
            continue
        for header, lines in hunks:
            if not re.search(r'\b(struct|class)\b', header) and not any('struct' in x or 'class' in x for x in lines):
                continue
            for ln in lines:
                if '(' in ln.split('//')[0] and '=' not in ln.split('//')[0].split('(')[0]:
                    continue   # a function declaration, not a member
                m = FIELD_LINE.match(ln)
                if m and m.group(1) not in SKIP_NAMES and len(m.group(1)) > 2:
                    out.setdefault(m.group(1), set()).add(f)
    return out


CONT_TYPES = r'(?:std::)?(?:unordered_map|unordered_set|unordered_multimap|multimap|multiset|map|set|vector|deque|list|queue|stack|priority_queue|flat_map|flat_set)'
CONT_LINE = re.compile(r'^\+\s*((?:(?:static|inline|mutable|thread_local)\s+)*)(' + CONT_TYPES + r')\s*<.*>\s+(\w+)\s*(?:\{[^;]*\}|=[^;]*)?;')
GROW_RX = r'\b{n}\b\s*(?:\[|(?:\.|->)\s*(?:push_back|emplace|emplace_back|insert|try_emplace|insert_or_assign|push|push_front|resize|assign|append)\s*\()'
SHRINK_RX = (r'\b{n}\b\s*(?:\.|->)\s*(?:erase|clear|pop_front|pop_back|pop|swap|extract|shrink_to_fit|reset)\s*\(|erase_if\s*\([^;]*\b{n}\b|'
             r'\b{n}\b\s*=\s*(?:\{{\s*\}}|std::\w+<[^;]*>\s*(?:\{{\s*\}}|\(\s*\))|\w+<[^;]*>\s*(?:\{{\s*\}}|\(\s*\)))|std::swap\s*\([^;]*\b{n}\b')


def new_containers(files):
    """{name: (type, file)}: container members / statics ADDED by the diff (headers; a .cpp only for `static` or a trailing-underscore name). Tables (const) are skipped."""
    out = {}
    for f, hunks in files.items():
        if not f.endswith(('.h', '.hpp', '.cpp', '.cc', '.inc')):
            continue
        for header, lines in hunks:
            for ln in lines:
                if not ln.startswith('+'):
                    continue
                code = ln.split('//')[0]
                if re.search(r'\b(const|constexpr)\b', code.split('<')[0]):
                    continue
                m = CONT_LINE.match(code)
                if not m:
                    continue
                name = m.group(3)
                if f.endswith(('.cpp', '.cc')) and not ('static' in m.group(1) or name.endswith('_')):
                    continue
                if len(name) > 2 and name not in SKIP_NAMES:
                    out.setdefault(name, (m.group(2), f))
    return out


class Tree:
    """Greps of `git grep -P -n -I REV -- src tools`: the content of REV (a branch or commit), never the working tree."""

    def __init__(self, root, rev):
        self.root = root
        self.rev = rev
        probe = subprocess.run(['git', '-C', root, 'grep', '-P', '-q', '-e', 'x', rev, '--', 'src'], capture_output=True, text=True, errors='replace')
        if probe.returncode not in (0, 1):
            raise SystemExit(f'git grep -P failed on {rev} ({probe.stderr.strip()[:200]}): the pack cannot read the branch')

    def find(self, rx):
        pat = rx.pattern if hasattr(rx, 'pattern') else rx
        out = run(['git', '-C', self.root, 'grep', '-P', '-n', '-I', '--no-color', '-e', pat, self.rev, '--', 'src', 'tools'], self.root, 120)
        hits, pre = [], self.rev + ':'
        for ln in out.split('\n'):
            if not ln.startswith(pre):
                continue
            m = re.match(r'(.+?):(\d+):(.*)$', ln[len(pre):])
            if not m:
                continue
            rel = m.group(1)
            if not rel.endswith(SCAN_EXT) or '/spv/' in rel or rel.startswith('out/'):
                continue
            hits.append((rel, int(m.group(2)), m.group(3).strip()))
        return hits


WRITE_RX = '{n}\\s*(?:\\[[^\\]]*\\])?\\s*(?:=(?!=)|\\+=|-=|\\|=|&=|\\+\\+|--)|\\b{n}\\.(?:push_back|emplace|emplace_back|insert|clear|resize|assign|erase)\\(|(?:\\+\\+|--){n}\\b'


def callers(tree, name, own_files):
    rx = re.compile(r'(?<![\w])' + re.escape(name) + r'\s*\(')
    out = []
    for rel, i, ln in tree.find(rx):
        if ln.startswith(('//', '*', '#')) or re.match(r'^[\w:<>,\*&\s~]+\b' + re.escape(name) + r'\s*\([^;]*\)\s*(const\s*)?\{?$', ln) and not ln.rstrip().endswith(';'):
            continue   # a comment, or a definition line
        out.append((rel, i, ln))
    return out


def readers_writers(tree, name, near=()):
    # member access only (`.name` / `->name`): a bare word matches every local variable of that name; hits next to the changed files come first
    rx = re.compile(r'(?:\.|->)' + re.escape(name) + r'(?![\w])')
    wr = re.compile(WRITE_RX.replace('{n}', re.escape(name)))
    reads, writes = [], []
    for rel, i, ln in tree.find(rx):
        if ln.startswith(('//', '*')):
            continue
        (writes if wr.search(ln) else reads).append((rel, i, ln))
    dirs = {os.path.dirname(x) for x in near}
    key = lambda h: (os.path.dirname(h[0]) not in dirs, h[0], h[1])
    return sorted(reads, key=key), sorted(writes, key=key)


def fmt(hits, limit):
    out = [f'    - `{rel}:{i}` {ln[:150]}' for rel, i, ln in hits[:limit]]
    if len(hits) > limit:
        out.append(f'    - ... {len(hits) - limit} more not shown (cap {limit})')
    return out


def ours_diff_section(branch, root, per=8):
    """S103 QB: the branch overlay of `q ours` (tools/q/ours_overlay.py): the branch's changed functions / variables with their callers,
    readers and writers in the base index and in the branch, and every caller / reader that exists in only one of the ON / OFF builds.
    Needs the main checkout's out\\harness\\index\\ours.db; says so and skips when it is missing (the pack never fails on it)."""
    import json
    qpy = os.path.join(ROOT, 'tools', 'q', 'q.py')
    if not os.path.exists(qpy):
        return ['(skipped: tools\\q\\q.py not found)']
    try:
        r = subprocess.run([sys.executable, '-I', qpy, 'ours', '--branch', branch, 'diff', '--json', '--root', root, '--limit', '60'],
                           cwd=root, capture_output=True, text=True, errors='replace', timeout=300)
    except (OSError, subprocess.TimeoutExpired) as e:
        return [f'(skipped: q ours diff did not finish: {e.__class__.__name__}; run it by hand: py tools\\q\\q.py ours --branch {branch} diff)']
    try:
        d = json.loads(r.stdout)
    except ValueError:
        return [f'(skipped: q ours diff failed: {(r.stderr or r.stdout).strip()[-300:]})']
    if 'error' in d:
        return [f'(skipped: {d["error"][:300]})']
    out = [f'overlay {d["overlay"]} in {d["overlay_secs"]} s ({d["units_reparsed"]} units re-parsed); {d["git_changed_files"]} files changed by the branch; '
           f'rows come from the base index (main checkout) and the branch overlay.']
    if d.get('base_drift_files'):
        out.append(f'NOTE the base index differs from the main checkout in {len(d["base_drift_files"])} changed files: body compares there are unreliable.')
    out.append(f'**CROSS-BUILD FLAGS ({len(d["flags"])})**' + (':' if d['flags'] else ': none'))
    out += [f'- {f}' for f in d['flags'][:40]]
    out += ['', '**Changed symbols**', '']
    if not d['changed']:
        out.append('(no changed function / variable found in the changed files)')
    for e in d['changed'][:60]:
        out.append(f'- {e["change"]}: `{e["qname"]}` ({e["kind"]}, `{e["file"]}:{e["line"]}`) builds base {e["builds_base"] or "-"} -> branch {e["builds_branch"] or "-"}')
        for g in e.get('globals_changed', [])[:per]:
            out.append(f'    - globals: +{",".join(g["new_access"]) or "-"} -{",".join(g["dropped_access"]) or "-"} `{g["global"]}` ({g["builds"]}){"  FLAG: " + g["flag"] if g["flag"] else ""}')
        for kind in ('callers', 'writers', 'readers'):
            rows = e.get(kind, [])
            for s in rows[:per]:
                out.append(f'    - {s["what"]}: `{s["func"]}` `{s["file"]}:{",".join(str(x) for x in s["lines"][:3])}` base {s["base"] or "-"} / branch {s["branch"] or "-"}'
                           f'{"  [" + s["change"] + "]" if s["change"] else ""}{"  FLAG: " + s["flag"] if s["flag"] else ""}')
            if len(rows) > per:
                out.append(f'    - ... {len(rows) - per} more {kind} not shown')
    if d['touched_globals']:
        out += ['', '**Touched globals** (every writer / reader, base vs branch)', '']
        for t in d['touched_globals'][:20]:
            out.append(f'- `{t["global"]}`')
            for kind in ('writers', 'readers'):
                for s in t[kind][:per]:
                    out.append(f'    - {s["what"]}: `{s["func"]}` `{s["file"]}:{",".join(str(x) for x in s["lines"][:3])}` base {s["base"] or "-"} / branch {s["branch"] or "-"}'
                               f'{"  [" + s["change"] + "]" if s["change"] else ""}{"  FLAG: " + s["flag"] if s["flag"] else ""}')
    out.append('')
    out.append('Limits: ' + '; '.join(d['limits']) + '.')
    return out


def build(branch, base, root, max_kb, use_q):
    mb = git('merge-base', branch, base, root=root).strip() or base
    stat = git('diff', '--stat=120', mb, branch, root=root)
    commits = git('log', '--oneline', f'{mb}..{branch}', root=root).strip().split('\n')
    diff = git('diff', '-U0', '--no-color', mb, branch, '--', 'src', 'tools', 'CMakeLists.txt', 'config', root=root)
    files = parse_diff(diff)
    tree = Tree(root, branch)
    md = [f'# Context pack: {branch} (base {mb[:10]} = merge-base with {base})', '',
          'Start the review here. Counts are `git grep` over our src\\ and tools\\ AT THE BRANCH (not the checkout); read the lines before trusting them.', '',
          '## 1. Diff stat', '', '```', stat.strip()[-6000:], '```', '', f'Commits: {len([c for c in commits if c])}', '']
    funcs = changed_functions(files)
    md += ['## 2. Changed functions and their callers', '']
    for k, (n, fs) in enumerate(sorted(funcs.items())):
        if k >= MAX_SYMBOLS:
            md.append(f'- ... {len(funcs) - MAX_SYMBOLS} more changed functions not shown (cap {MAX_SYMBOLS})')
            break
        hits = [h for h in callers(tree, n, fs) if h[0] not in fs or True]
        md.append(f'- `{n}` ({", ".join(sorted(os.path.basename(x) for x in fs))}): {len(hits)} call site(s)')
        md += fmt(hits, PER_SYMBOL)
    fields = changed_fields(files)
    md += ['', '## 3. Changed struct fields and their readers / writers', '']
    for k, (n, fs) in enumerate(sorted(fields.items())):
        if k >= MAX_SYMBOLS:
            md.append(f'- ... {len(fields) - MAX_SYMBOLS} more changed fields not shown (cap {MAX_SYMBOLS})')
            break
        rd, wr = readers_writers(tree, n, fs)
        md.append(f'- `{n}` ({", ".join(sorted(os.path.basename(x) for x in fs))}): {len(rd)} read(s), {len(wr)} write(s)')
        if wr:
            md.append('  - writers:')
            md += fmt(wr, PER_SYMBOL // 2)
        if rd:
            md.append('  - readers:')
            md += fmt(rd, PER_SYMBOL // 2)
    conts = new_containers(files)
    md += ['', '## 3b. New containers and where each shrinks', '',
           'An insert site with no erase / clear / pop / swap site is unbounded unless the whole object is replaced: read the sites before judging. (Name-based grep: a local of the same name shows up too.)']
    if not conts:
        md.append('(none added)')
    for k, (n, (ty, f)) in enumerate(sorted(conts.items())):
        if k >= 30:
            md.append(f'- ... {len(conts) - 30} more new containers not shown (cap 30)')
            break
        grow = tree.find(re.compile(GROW_RX.replace('{n}', re.escape(n))))
        shrink = tree.find(re.compile(SHRINK_RX.replace('{{', '{').replace('}}', '}').replace('{n}', re.escape(n))))
        grow = [h for h in grow if not h[2].startswith('//')]
        verdict = 'NO erase / clear / pop / swap site found: UNBOUNDED unless replaced wholesale' if not shrink else f'{len(shrink)} erase / clear site(s)'
        md.append(f'- `{n}` ({ty}, {os.path.basename(f)}): {len(grow)} insert site(s); {verdict}')
        if shrink:
            md.append('  - shrink sites:')
            md += fmt(shrink, PER_SYMBOL // 2)
        if grow:
            md.append('  - insert sites:')
            md += fmt(grow, PER_SYMBOL // 2)
    addrs = sorted({(m.group(1) or '0x' + m.group(2)).upper().replace('0X', '0x') for ln in diff.split('\n') if ln.startswith('+') for m in ADDR.finditer(ln)})
    md += ['', '## 4. Game addresses named in the diff', '']
    if not addrs:
        md.append('(none)')
    for a in addrs[:25]:
        md.append(f'- `{a}`')
        if use_q:
            out = run([sys.executable, '-I', os.path.join(root, 'tools', 'q', 'q.py'), 'callers', a], root, 30).strip().split('\n')
            md += [f'    {x[:200]}' for x in out[:6] if x]
    if len(addrs) > 25:
        md.append(f'- ... {len(addrs) - 25} more addresses not shown (cap 25)')
    md += ['', '## 5. `q ours diff`: changed symbols, callers / readers / writers in BASE and BRANCH, ON / OFF flags (clang AST, exact)', '']
    md += ours_diff_section(branch, root) if use_q else ['(skipped: --no-q)']
    text = '\n'.join(md) + '\n'
    cap = max_kb * 1024
    if len(text.encode()) > cap:
        text = text.encode()[:cap].decode(errors='ignore') + f'\n\n(pack cut at {max_kb} KB: --max-kb to raise)\n'
    return text, len(funcs), len(fields), len(addrs), len(conts)


def main():
    a = sys.argv[1:]
    if '--selftest' in a:
        sys.exit(0 if selftest() else 1)

    def opt(n, d=None):
        return a[a.index(n) + 1] if n in a else d
    branch = next((x for x in a if not x.startswith('--') and a.index(x) == 0), None)
    if not branch:
        print(__doc__)
        sys.exit(2)
    out = opt('--out') or os.path.join(ROOT, 'out', 'review', re.sub(r'[^\w.-]', '_', branch) + '_pack.md')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    text, nf, nfl, na, nc = build(branch, opt('--base', 'main'), ROOT, int(opt('--max-kb', '400')), '--no-q' not in a)
    open(out, 'w', encoding='utf-8').write(text)
    print(f'context pack: {out}  ({len(text) // 1024} KB; {nf} functions, {nfl} fields, {nc} new containers, {na} game addresses)')


def selftest():
    import tempfile
    d = tempfile.mkdtemp(prefix='ctxpack_')

    def g(*x):
        return subprocess.run(['git', '-C', d, '-c', 'user.name=t', '-c', 'user.email=t@t'] + list(x), capture_output=True, text=True)

    def w(rel, body):
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w').write(body)
    g('init', '-q', '-b', 'main')
    w('src/a.h', 'struct Box {\n  int width = 1;\n};\nint Measure(const Box& b);\n')
    w('src/a.cpp', '#include "a.h"\nint Measure(const Box& b) {\n  return b.width;\n}\nint Other() { return Measure(Box{}); }\n')
    w('src/use.cpp', '#include "a.h"\nvoid Use(Box& b) { b.width = 3; int m = Measure(b); }\n')
    g('add', '-A')
    g('commit', '-q', '-m', 'base')
    g('checkout', '-q', '-b', 'feat')
    w('src/a.h', '#include <unordered_map>\nstruct Box {\n  int width = 1;\n  int height = 2;\n  std::unordered_map<int, int> memo;\n  std::vector<int> tidy;\n};\nint Measure(const Box& b);\n')
    w('src/a.cpp', '#include "a.h"\nint Measure(const Box& b) {\n  return b.width * b.height;  // sub_82BAAEE8\n}\nint Other() { return Measure(Box{}); }\n')
    w('src/use.cpp', '#include "a.h"\nvoid Use(Box& b) { b.width = 3; b.height = 4; b.memo[1] = 2; b.tidy.push_back(1); b.tidy.clear(); int m = Measure(b); }\n')
    g('commit', '-qam', 'feat')
    text, nf, nfl, na, nc = build('feat', 'main', d, 400, False)
    g('checkout', '-q', 'main')   # the checkout is now the OLD tree: the pack must still read the branch
    text_old_checkout = build('feat', 'main', d, 400, False)[0]
    ok = True
    checks = [('reads the BRANCH, not the checkout: callers still found with main checked out', 'use.cpp:2' in text_old_checkout and '`Measure`' in text_old_checkout),
              ('new container memo listed with an insert site and NO shrink site (unbounded)', '`memo`' in text and 'UNBOUNDED' in text),
              ('new container tidy listed with its clear site', '`tidy`' in text and 'shrink sites' in text),
              ('changed function Measure listed with callers in use.cpp / a.cpp', '`Measure`' in text and 'use.cpp:2' in text and 'a.cpp:5' in text),
              ('changed field height listed with its writer in use.cpp and reader in a.cpp', '`height`' in text and 'writers:' in text and 'use.cpp:2' in text),
              ('game address named in the diff is listed', '0x82BAAEE8' in text),
              ('diff stat present', 'src/a.cpp' in text and 'Diff stat' in text),
              ('section 5 (q ours diff) is in the pack, and says so when skipped', '## 5.' in text and 'skipped' in text.split('## 5.')[1]),
              ('cap is respected', len(build('feat', 'main', d, 1, False)[0].encode()) <= 1024 + 200)]
    for label, good in checks:
        print(f"  {'ok  ' if good else 'FAIL'} {label}")
        ok &= bool(good)
    print('selftest ' + ('PASSED' if ok else 'FAILED'))
    return ok


if __name__ == '__main__':
    main()
