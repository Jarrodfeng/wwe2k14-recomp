#!/usr/bin/env python3
"""one_definition_lint (lane EXEB-B1, S108; docs\\research\\s108\\EXEB_B_plan.md): the one-definition hazards of splitting src\\main.cpp into hook modules.

A game-side module used to be a header pulled into wwe2k14_app.h, so every strong definition (a hook body, a cvar, a mid-asm function) lived in the one TU main.cpp. Once a module
is its own .cpp, two things can go wrong silently or only at link:
  duplicate_hook  a guest function hooked (REX_HOOK_RAW / REX_HOOK / REX_STUB* / REX_EXPORT*, directly or through a macro of ours) by two or more units of one game build: a duplicate
                  symbol at link, or one hook silently winning over the other. Asked of the real preprocessor (tools\\handback\\cvar_pp.py: clang -E -fdirectives-only with each
                  unit's own build flags, the same text the cvar judge reads).
  header_static   a src\\**\\*.h with a namespace-scope `static` object or function, or an anonymous namespace: every unit that includes it gets its own copy (a hook module's
                  state would split in two). A source scan: comments and strings blanked, braces counted, only namespace levels are looked at.

  py -I tools\\lint\\one_definition_lint.py [--root .]   judge the tree (exit 0 clean, 1 a finding, 2 a unit or the whole judge is BLOCKED)
  py -I tools\\lint\\one_definition_lint.py --selftest    the planted twins (clang VFS overlays; nothing is written to src\\) and the real tree (MUST be clean):
      T1 a header with REX_HOOK_RAW(sub_825C1888) {} (a function blood_keep hooks) included by src\\start_settings.cpp: duplicate_hook in ON and OFF
      T2 a header with a namespace-scope `static int`: header_static
      T3 a header with an anonymous namespace: header_static
      C1 a header with a hook of a function nothing else hooks, included by one unit: clean (control)
      C2 a header whose `static` is a class member / a function-local: clean (control)
      S1-S4 (pure Python, first): cvar_pp.scan_hooks on planted texts: a direct hook, a hook through a macro parameter, a macro that calls a hook macro with the name
            pasted by ## (nf/reg_save.h), and the non-hooks (the SDK's own macro, another directive, a call after #undef)
"""
# entry: findings
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'handback'))
import cvar_pp as hb  # noqa: E402  (tools\handback\cvar_pp.py: judge_tree)

PLANTED_HOOK = 'src/odr_planted_hook.h'
PLANTED_STATIC = 'src/odr_planted.h'
HOOKED = 'sub_825C1888'   # hooked by src\blood_keep.cpp (before B1: through blood_keep.h in main.cpp)


# ------------------------------------------------------------------------------------------------------------------------------ header_static

def _blank(text):
    """text with comments, string and character literals blanked (newlines kept, so line numbers hold)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '//':
            j = text.find('\n', i)
            j = n if j < 0 else j
            while text[j - 1:j] == '\\' and j < n:   # a line comment continued by a backslash
                j = text.find('\n', j + 1)
                j = n if j < 0 else j
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif two == '/*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c == 'R' and text[i:i + 2] == 'R"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            m = re.compile(r'R"([^()\\ ]*)\(').match(text, i)
            if m:
                end = text.find(')' + m.group(1) + '"', m.end())
                j = n if end < 0 else end + len(m.group(1)) + 2
                out.append('""' + re.sub(r'[^\n]', ' ', text[i + 2:j]))
                i = j
            else:
                out.append(c)
                i += 1
        elif c in '"\'':
            if c == "'" and i and (text[i - 1].isalnum() and text[i - 1] not in 'uUL8'):   # a digit separator: 1'000
                out.append(c)
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r'[^\n]', ' ', text[i + 1:j - 1]) + c)
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def _drop_directives(text):
    """each preprocessor line (with its continuations) blanked, newlines kept."""
    lines = text.split('\n')
    out, cont = [], False
    for ln in lines:
        if cont or ln.lstrip().startswith('#'):
            cont = ln.rstrip().endswith('\\')
            out.append(' ' * len(ln))
        else:
            out.append(ln)
    return '\n'.join(out)


NS_OPEN = re.compile(r'(?:^|[\s;{}])namespace\b[^;{}=]*$')
EXTERN_C = re.compile(r'(?:^|[\s;{}])extern\s*""\s*$')
STATIC_TOK = re.compile(r'(?<![\w$])static(?![\w$])')


def header_static_findings(text):
    """[(line, what)] for one header's text: a namespace-scope `static` declaration / definition, or an anonymous namespace."""
    code = _drop_directives(_blank(text))
    found = []
    stack = []        # one bool per open brace: True = a namespace (or extern "C") level
    stmt, stmt_line, line = [], 0, 1

    def flush(kind_pos):
        s = ''.join(stmt)
        d = re.search(r'[(={\[]', s)
        head = s[:d.start()] if d else s   # a `static` after the declarator's first ( = { [ is not the storage class
        if all(stack) and STATIC_TOK.search(head):
            is_fn = bool(d) and d.group(0) == '('
            # a read-only static OBJECT (`static const T x`, `static constexpr`) is a constant every unit may copy: no state to split (a pointer to const is not one)
            const_obj = not is_fn and (re.search(r'\bconstexpr\b', head) or (re.search(r'\bconst\b', head) and '*' not in head))
            if not const_obj:
                lead = len(s) - len(s.lstrip())
                found.append((stmt_line + s[lead:].count('\n', 0, max(0, s.index('static') - lead)), 'namespace-scope static ' + ('function' if is_fn else 'object') + ': ' + ' '.join(s.split())[:80]))
        stmt.clear()
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if c == '\n':
            line += 1
        if c in ';{}':
            if c == '{':
                s = ''.join(stmt)
                is_ns = bool(NS_OPEN.search(s)) or bool(EXTERN_C.search(s))
                if all(stack):
                    if is_ns and re.search(r'(?:^|[\s;{}])namespace\s*(?:\[\[[^\]]*\]\]\s*)?$', s):
                        found.append((line, 'anonymous namespace'))
                    elif not is_ns:
                        flush(i)
                stack.append(is_ns)
                stmt.clear()
            elif c == '}':
                if stack:
                    stack.pop()
                stmt.clear()
            else:
                if all(stack):
                    flush(i)
                stmt.clear()
            stmt_line = 0
        else:
            if not stmt_line and not c.isspace():
                stmt_line = line
            stmt.append(c)
        i += 1
    return found


def header_texts(root, overlay):
    """{relative path: text} of every src\\**\\*.h, with the overlay's planted texts served in place."""
    out = {}
    base = os.path.join(root, 'src')
    for dp, _dn, fn in os.walk(base):
        for f in fn:
            if f.endswith('.h'):
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, root).replace('\\', '/')
                try:
                    out[rel] = open(p, encoding='utf-8', errors='replace', newline='').read()
                except OSError:
                    pass
    for k, v in (overlay or {}).items():
        if k.startswith('src/') and k.endswith('.h'):
            out[k] = v
    return out


# ------------------------------------------------------------------------------------------------------------------------------ the judgement

def findings(root, overlay=None):
    """([(key, message)], blocked [(unit, mode, why)], notes). `overlay` {path relative to root: planted text}. RuntimeError when no judgement is possible."""
    ov = {os.path.join(root, k): v for k, v in (overlay or {}).items()}
    out, blocked, not_judged = hb.judge_tree(root, overlay=ov or None)
    found = []
    for mode in (1, 0):
        mtxt = 'ON' if mode else 'OFF'
        hooked = {}
        for u in sorted(out[mode]):
            for fn in out[mode][u].get('hook', {}):
                hooked.setdefault(fn, []).append(u)
        for fn in sorted(hooked):
            if len(hooked[fn]) > 1:
                found.append((['duplicate_hook', mtxt, fn, len(hooked[fn])], '%s is hooked by %d units of the %s game build (a duplicate symbol at link, or one hook silently over the other): %s'
                              % (fn, len(hooked[fn]), mtxt, ', '.join(hooked[fn]))))
    for rel, text in sorted(header_texts(root, overlay).items()):
        for ln, what in header_static_findings(text):
            found.append((['header_static', rel, ln], '%s:%d: %s (every unit that includes the header gets its own copy: use `inline`, or move it into a .cpp)' % (rel, ln, what)))
    notes = ['%s (%s) not judged: %s' % (u, 'ON' if m else 'OFF', why) for u, m, why in not_judged]
    return found, blocked, notes


def twins(root):
    ss = open(os.path.join(root, 'src/start_settings.cpp'), encoding='utf-8', errors='replace', newline='').read()
    class _Inc:
        def __mod__(self, h):
            return ss + '\n#include "%s"\n' % h
    inc = _Inc()
    hook_h = '#pragma once\n#include <rex/hook.h>\nREX_HOOK_RAW(%s) {}\n'
    # (name, overlay, rule wanted (None = clean), required modes)
    return [
        ('T1 a header hooking %s, included by src/start_settings.cpp (a second unit that hooks it)' % HOOKED,
         {PLANTED_HOOK: hook_h % HOOKED, 'src/start_settings.cpp': inc % 'odr_planted_hook.h'}, 'duplicate_hook', ('ON', 'OFF')),
        ('T2 a header with a namespace-scope `static int`', {PLANTED_STATIC: '#pragma once\nnamespace wwe2k14::planted {\nstatic int g_planted = 0;\n}\n'}, 'header_static', None),
        ('T3 a header with an anonymous namespace', {PLANTED_STATIC: '#pragma once\nnamespace wwe2k14::planted {\nnamespace {\nint g_planted = 0;\n}\n}\n'}, 'header_static', None),
        ('C1 control: a header hooking a function nothing else hooks, included by one unit',
         {PLANTED_HOOK: hook_h % 'sub_DEADBEEF', 'src/start_settings.cpp': inc % 'odr_planted_hook.h'}, None, None),
        ('C2 control: `static` as a class member and in a function body',
         {PLANTED_STATIC: '#pragma once\nnamespace wwe2k14::planted {\nstruct S { static int a; static constexpr int b = 1; static void F(); };\ninline int G() { static int c = 0; return ++c; }\n'
                          'static_assert(sizeof(int) == 4);\n}  // namespace wwe2k14::planted\n// static int in_a_comment;\nconst char* kS = "static int in_a_string;";\n'}, None, None),
    ]


SCAN_TWINS = [   # (name, preprocessed text, the hook names cvar_pp.scan_hooks must give, with their counts); pure Python, no clang
    ('S1 a direct hook', b'REX_HOOK_RAW(sub_A1) {\n}\n', {'sub_A1': 1}),
    ('S2 a hook through a macro parameter (draw_origin.h S101_UI_SCOPE shape)',
     b'#define UI_SCOPE(fn, tex_off) \\\n  REX_HOOK_RAW(fn) { \\\n    __imp__##fn(ctx, base); \\\n  }\nUI_SCOPE(sub_B1, 0)\nUI_SCOPE(sub_B2, 8)\n', {'sub_B1': 1, 'sub_B2': 1}),
    ('S3 a macro calling a hook macro, the name pasted with ## (nf/reg_save.h WWE2K14_REG_HOOKS shape)',
     b'#define REG_HOOK(name, fn, n) \\\n  REX_EXTERN(__imp__##name##n); \\\n  REX_HOOK_RAW(name##n) { \\\n  }\n'
     b'#define REG_HOOKS(n) \\\n  REG_HOOK(__savegprlr_, SaveGpr, n) \\\n  REG_HOOK(__restfpr_, RestFpr, n)\nREG_HOOKS(14)\nREG_HOOKS(15)\n',
     {'__savegprlr_14': 1, '__restfpr_14': 1, '__savegprlr_15': 1, '__restfpr_15': 1}),
    ('S4 the SDK hook macro itself, a hook in another directive and a call after #undef are not hooks',
     b'#define REX_HOOK_RAW(name) extern "C" REX_FUNC(name)\n#define W(f) REX_HOOK_RAW(f) {}\n#undef W\nW(sub_C1)\n#if 0 // REX_HOOK_RAW(sub_C2)\n#endif\n', {}),
]


def scan_selftest():
    """failures of the pure-Python twins of cvar_pp.scan_hooks (the hook names the duplicate_hook rule is built on)."""
    bad = 0
    for name, text, want in SCAN_TWINS:
        got = hb.scan_hooks(text)
        ok = got == want
        print('%s  %s -> %s' % ('ok  ' if ok else 'FAIL', name, sorted(got.items()) if not ok else '%d name(s)' % len(got)))
        bad += not ok
    return bad


def selftest(root):
    bad = scan_selftest()
    for name, overlay, rule, modes in twins(root):
        try:
            got, blocked, _n = findings(root, overlay)
        except RuntimeError as e:
            print('BLOCKED: %s' % e)
            return 2
        mine = [(k, m) for k, m in got if k[0] == rule] if rule else got
        if rule is None:
            ok = not got
        else:
            ok = bool(mine) and all(k[0] == rule for k, _m in got)
            if rule == 'duplicate_hook':
                ok = ok and sorted({k[1] for k, _m in mine}) == sorted(modes) and all(k[2] == HOOKED for k, _m in mine)
            else:
                ok = ok and all(k[1] == PLANTED_STATIC for k, _m in mine)
        ok = ok and not blocked
        print('%s  %s -> %d finding(s)%s' % ('ok  ' if ok else 'FAIL', name, len(got), ('; BLOCKED %d' % len(blocked)) if blocked else ''))
        if got and not ok or (got and rule):
            print('        e.g. ' + got[0][1][:230])
        bad += not ok
    try:
        got, blocked, notes = findings(root)
    except RuntimeError as e:
        print('BLOCKED: %s' % e)
        return 2
    ok = not got and not blocked
    print('%s  real tree: %d finding(s), %d unit-build(s) blocked, %d not judged' % ('ok  ' if ok else 'FAIL', len(got), len(blocked), len(notes)))
    for _k, m in got[:5]:
        print('        ' + m[:230])
    bad += not ok
    print('PASS: every planted twin is caught, the controls and the tree are clean' if not bad else 'FAIL: %d selftest case(s)' % bad)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.selftest:
        return selftest(root)
    try:
        got, blocked, notes = findings(root)
    except RuntimeError as e:
        print('BLOCKED: the judge cannot preprocess the game units: %s' % e)
        return 2
    for n in notes:
        print('note: ' + n)
    for u, m, e in blocked:
        print('BLOCKED: %s [%s]: %s' % (u, 'ON' if m else 'OFF', e))
    for _k, m in got:
        print('error: ' + m)
    print('one_definition_lint: %d finding(s), %d unit-build(s) blocked, %d not judged' % (len(got), len(blocked), len(notes)))
    return 1 if got else 2 if blocked else 0


if __name__ == '__main__':
    sys.exit(main())
