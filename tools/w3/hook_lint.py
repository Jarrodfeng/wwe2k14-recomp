r"""S102 W3 review lints on the game's hooks.

 Lint 1 (F1): a switch the table (src\renderer_adapter\rdr_switches.h) lists as StartOnly must not be read with REXCVAR_GET(<name>) inside a REX_HOOK / REX_HOOK_RAW body:
   a hook runs every call, so a mid-run flip would take effect while the table logs it "refused".  (PerFrame switches are read once per replay / frame through rdr_sw::BoolSeen.)
 Lint 2 (F3): every REXCVAR_GET name in the sub_82BB53D0 hook body (src\replay_watch.h) must be in the condition of the sub_82BB5BE8 hook's `kFast` (the bookkeeping the
   fast path must agree with) or in the allowlist {wwe2k14_replay_call_check}. A new cvar read there would change what a record costs or does without the fast path knowing.

  py -I tools\w3\hook_lint.py [--root DIR]        exit 1 on a violation
  py -I tools\w3\hook_lint.py --selftest          planted twins: (a) the W3 code as first committed (fc934dc9) must fail lint 1; (b) a REXCVAR_GET(wwe2k14_x) added to the
                                                  53D0 hook body must fail lint 2; (c) the current tree must pass both (exit 1 if a twin is missed)
"""
import argparse
import os
import re
import subprocess
import sys

ALLOW_53D0 = {'wwe2k14_replay_call_check'}
HOOK = re.compile(r'\bREX_HOOK(?:_RAW)?\(\s*(\w+)\s*\)\s*\{')


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def hook_bodies(text):
    """[(name, body)] for each REX_HOOK / REX_HOOK_RAW(name) { ... } (braces matched; string and char literals skipped)."""
    text = strip_comments(text)
    out = []
    for m in HOOK.finditer(text):
        i, depth = m.end(), 1
        while i < len(text) and depth:
            c = text[i]
            if c in '"\'':
                q = c
                i += 1
                while i < len(text) and text[i] != q:
                    i += 2 if text[i] == '\\' else 1
            else:
                depth += (c == '{') - (c == '}')
            i += 1
        out.append((m.group(1), text[m.end():i - 1]))
    return out


def startonly_cvars(table_text):
    return set(re.findall(r'X\(\w+,\s*"(\w+)",\s*StartOnly', table_text))


def cvar_reads(body):
    return set(re.findall(r'REXCVAR_GET\(\s*(\w+)\s*\)', body))


def lint1(bodies, startonly):
    return ['hook %s reads StartOnly switch %s with REXCVAR_GET' % (n, c) for n, b in bodies for c in sorted(cvar_reads(b) & startonly)]


def lint2(watch_text):
    bodies = dict(hook_bodies(watch_text))
    if 'sub_82BB53D0' not in bodies or 'sub_82BB5BE8' not in bodies:
        return ['replay_watch.h: the sub_82BB5BE8 / sub_82BB53D0 hooks were not found']
    m = re.search(r'const bool kFast\s*=([^;]*);', bodies['sub_82BB5BE8'])
    if not m:
        return ['replay_watch.h: the sub_82BB5BE8 hook has no `const bool kFast = ...;`']
    allowed = cvar_reads(m.group(1)) | ALLOW_53D0
    extra = cvar_reads(bodies['sub_82BB53D0']) - allowed
    return ['the sub_82BB53D0 hook reads %s, which is neither in kFast\'s condition (%s) nor allowlisted' % (c, ', '.join(sorted(allowed))) for c in sorted(extra)]


def tree_texts(root):
    watch = open(os.path.join(root, 'src', 'replay_watch.h'), encoding='utf-8').read()
    table = open(os.path.join(root, 'src', 'renderer_adapter', 'rdr_switches.h'), encoding='utf-8').read()
    return watch, table


def all_hook_bodies(root):
    out = []
    for d, _, files in os.walk(os.path.join(root, 'src')):
        for f in files:
            if f.endswith(('.h', '.cpp', '.inc', '.hpp')):
                try:
                    out += hook_bodies(open(os.path.join(d, f), encoding='utf-8', errors='replace').read())
                except OSError:
                    pass
    return out


def run(root):
    watch, table = tree_texts(root)
    so = startonly_cvars(table)
    if not so:
        print('hook_lint: no StartOnly entries found in the table (the lint reads nothing: not a pass)')
        return 2
    problems = lint1(all_hook_bodies(root), so) + lint2(watch)
    for p in problems:
        print('hook_lint: ' + p)
    print('hook_lint: %s (%d StartOnly names, %d hooks scanned)' % ('FAIL' if problems else 'PASS', len(so), len(all_hook_bodies(root))))
    return 1 if problems else 0


def git_show(root, rev, path):
    r = subprocess.run(['git', '-C', root, 'show', '%s:%s' % (rev, path)], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def selftest(root):
    ok = True
    # (a) the W3 code as first committed read two table-StartOnly switches inside hooks
    old_watch = git_show(root, 'fc934dc9', 'src/replay_watch.h')
    old_table = git_show(root, 'fc934dc9', 'src/renderer_adapter/rdr_switches.h')
    if old_watch is None or old_table is None:
        print('selftest: commit fc934dc9 is not in this repository: the historic twin cannot run (FAIL)')
        return 1
    hit = lint1(hook_bodies(old_watch), startonly_cvars(old_table))
    print('selftest (a) fc934dc9 replay_watch.h: %d violation(s) %s' % (len(hit), 'caught' if hit else 'MISSED'))
    ok &= bool(hit)
    # (b) a new cvar read in the 53D0 hook body
    watch, table = tree_texts(root)
    planted = re.sub(r'(REX_HOOK_RAW\(sub_82BB53D0\)\s*\{)', r'\1 if (REXCVAR_GET(wwe2k14_x)) {}', watch, count=1)
    hit = lint2(planted) if planted != watch else None
    print('selftest (b) REXCVAR_GET(wwe2k14_x) added to the 53D0 hook: %s' % ('DID NOT APPLY' if hit is None else ('caught' if hit else 'MISSED')))
    ok &= bool(hit)
    # (c) the tree passes
    clean = lint1(hook_bodies(watch), startonly_cvars(table)) + lint2(watch)
    print('selftest (c) current replay_watch.h: %s' % ('clean' if not clean else 'VIOLATIONS ' + '; '.join(clean)))
    ok &= not clean
    print('selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    return selftest(a.root) if a.selftest else run(a.root)


if __name__ == '__main__':
    sys.exit(main())
