#!/usr/bin/env python3
r"""register_switch_lint (S103 PWI, review of lane PW-1/PW-2 finding 1): a live scene contributor registered in OnPostSetup (src\wwe2k14_app.h) costs the game thread work EVERY frame (a table pull at the latch, a
guest read at every model call). The player build turns --wwe2k14_scene_records / --wwe2k14_scene_light_states on through the promotions lists (OnPreSetup, src\bundle3.h), so a reader registered under one of those
switches runs for every player. Rule: every `Register*(` call in OnPostSetup sits under an `if (...)` whose REXCVAR_GET switches all default FALSE and none of them is in the promotions lists, unless
tools\scene\register_switch_allow.txt gives that callee a reason. (`!WWE2K14_STREAM() || REXCVAR_GET(x)` is the pattern for "on in the stream-free build, a switch in the stream build".)
A callee that is not on the allowlist and not under such a switch is an error; an allowlisted callee that is no longer called is an error too (the list stays true).

  py -I tools\scene\register_switch_lint.py --root .        scan the tree (exit 1 on an error)
  py -I tools\scene\register_switch_lint.py --selftest      planted twins: each bad snippet MUST be flagged, each good one must pass (incl. the two registrations the review found, as they were)

Text scan, no compile, about a second."""
import argparse
import os
import re
import sys
import warnings

warnings.simplefilter('ignore')   # preprocess() evals #if expressions it cannot always read (an unknown one counts as true)

APP = 'src/wwe2k14_app.h'
ALLOW = 'tools/scene/register_switch_allow.txt'
CALL = re.compile(r'\b((?:\w+::)*Register\w*|(?:\w+::)*scene_live::Install\w*)\s*\(')   # PW review 9: the scene contributors' Install* (scene_live::InstallArena) match too; other Install()s (crash handlers, mods, keybinds) are not contributors
GET = re.compile(r'REXCVAR_GET\s*\(\s*(\w+)\s*\)')
DEFINE = re.compile(r'REXCVAR_DEFINE_BOOL\s*\(\s*(\w+)\s*,\s*(true|false)\s*,')
LIT = re.compile(r'"(wwe2k14_\w+)"')


def strip(text):
    """code text with comments and string / char literals blanked (newlines and columns kept); preprocessor lines blanked."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '//':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif two == '/*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(c + ' ' * (j - i - 2) + c if j - i >= 2 else c)
            i = j
        else:
            out.append(c)
            i += 1
    return preprocess(''.join(out))


def preprocess(text, stream=True, android=False):
    """blank the directive lines and the lines of inactive #if branches: the player's PC stream build (WWE2K14_STREAM() true, not Android); an unknown condition counts as true."""
    def ev(expr):
        e = re.sub(r'defined\s*\(\s*__ANDROID__\s*\)', 'True' if android else 'False', expr)
        e = re.sub(r'WWE2K14_STREAM\s*\(\s*\)', 'True' if stream else 'False', e)
        e = re.sub(r'defined\s*\(\s*\w+\s*\)', 'True', e)
        e = e.replace('&&', ' and ').replace('||', ' or ')
        e = re.sub(r'!(?!=)', ' not ', e)
        e = re.sub(r'\b(?!True\b|False\b|and\b|or\b|not\b)\w+\b', 'True', e)
        try:
            return bool(eval(e, {'__builtins__': {}}, {}))
        except Exception:
            return True
    out, stack = [], []   # stack of (this branch active, any branch taken, parent active)
    active = True
    for l in text.split('\n'):
        t = l.strip()
        if t.startswith('#'):
            d = re.match(r'#\s*(\w+)\s*(.*)', t)
            k, rest = (d.group(1), d.group(2)) if d else ('', '')
            if k in ('if', 'ifdef', 'ifndef'):
                cond = ev(rest) if k == 'if' else (ev('defined(%s)' % rest.strip()) if k == 'ifdef' else not ev('defined(%s)' % rest.strip()))
                stack.append((active and cond, cond, active))
                active = active and cond
            elif k == 'elif' and stack:
                _, taken, parent = stack.pop()
                cond = parent and not taken and ev(rest)
                stack.append((cond, taken or cond, parent))
                active = cond
            elif k == 'else' and stack:
                _, taken, parent = stack.pop()
                cond = parent and not taken
                stack.append((cond, True, parent))
                active = cond
            elif k == 'endif' and stack:
                _, _, parent = stack.pop()
                active = parent
            out.append(' ' * len(l))
        else:
            out.append(l if active else ' ' * len(l))
    return '\n'.join(out)


def body_of(code, marker):
    """the text of the function whose header contains `marker` (up to its matching closing brace); '' when absent."""
    k = code.find(marker)
    if k < 0:
        return '', 0
    j = code.find('{', k)
    depth, i = 0, j
    while i < len(code):
        if code[i] == '{':
            depth += 1
        elif code[i] == '}':
            depth -= 1
            if depth == 0:
                return code[j + 1:i], j + 1
        i += 1
    return code[j + 1:], j + 1


def balanced(code, i):
    """code[i] == '(' -> (the text inside, the index after the closing paren)."""
    depth, j = 0, i
    while j < len(code):
        if code[j] == '(':
            depth += 1
        elif code[j] == ')':
            depth -= 1
            if depth == 0:
                return code[i + 1:j], j + 1
        j += 1
    return code[i + 1:], len(code)


def calls_with_guards(body, first_line=1):
    """[(callee, [conditions of every enclosing `if (...)`, innermost last], line)] for every Register*( call in `body` (a stripped function body)."""
    res = []
    stack = []            # per open brace: its `if` condition or None
    pending = None        # the condition of an `if (...)` seen, waiting for its `{` or its single statement
    single = None         # (condition) of an `if (...) statement;` until that statement's `;`
    single_depth = 0
    i, n = 0, len(body)
    line = first_line
    while i < n:
        c = body[i]
        if c == '\n':
            line += 1
        m = re.compile(r'\bif\s*\(').match(body, i)
        if m and (i == 0 or not (body[i - 1].isalnum() or body[i - 1] == '_')):
            cond, j = balanced(body, m.end() - 1)
            k = j
            while k < n and body[k] in ' \t\r\n':
                k += 1
            line += body.count('\n', i, k)
            if k < n and body[k] == '{':
                pending = cond
            else:
                single, single_depth = cond, len(stack)
            i = k
            continue
        if c == '{':
            stack.append(pending)
            pending = None
        elif c == '}':
            if stack:
                stack.pop()
        elif c == ';' and single is not None and len(stack) == single_depth:
            single = None
        m3 = CALL.match(body, i)
        if m3 and (i == 0 or not (body[i - 1].isalnum() or body[i - 1] == '_' or body[i - 1] == ':')):
            conds = [s for s in stack if s is not None]
            if single is not None:
                conds.append(single)
            res.append((m3.group(1), conds, line))
            i = m3.end()
            continue
        i += 1
    return res


def adequate(cond, defaults, promoted):
    """a condition guards a registration when it evaluates FALSE in the player's stream build with the promotions applied: every REXCVAR_GET(x) is true when x is promoted, else its default (an unknown switch
    counts as true: not adequate); WWE2K14_STREAM() is true. `!WWE2K14_STREAM() || REXCVAR_GET(x)` (x default false, unpromoted) is false there; `REXCVAR_GET(scene_records)` (promoted) is true."""
    def sub(m):
        nm = m.group(1)
        return 'True' if nm in promoted or defaults.get(nm, True) else 'False'
    e = GET.sub(sub, cond)
    e = re.sub(r'WWE2K14_STREAM\s*\(\s*\)', 'True', e)
    e = re.sub(r'\btrue\b', 'True', e)
    e = re.sub(r'\bfalse\b', 'False', e)
    e = e.replace('&&', ' and ').replace('||', ' or ')
    e = re.sub(r'!(?!=)', ' not ', e)
    if re.sub(r'True|False|and|or|not|[()\s]', '', e):
        return False   # something we cannot evaluate (a function call, a variable): not adequate
    try:
        return eval(e, {'__builtins__': {}}, {}) is False
    except Exception:
        return False


def lint(app_text, defaults, promoted, allow):
    """-> (errors, used_allow): errors = ['file:line: text']"""
    code = strip(app_text)
    body, start = body_of(code, 'void OnPostSetup()')
    errors, used = [], set()
    if not body:
        return ['%s: OnPostSetup() not found (the lint reads nothing: refusing)' % APP], used
    calls = calls_with_guards(body, code.count('\n', 0, start) + 1)
    if not calls:
        errors.append('%s: no Register*( call found in OnPostSetup (the lint reads nothing: refusing)' % APP)
    for callee, conds, line in calls:
        if any(adequate(c, defaults, promoted) for c in conds):
            continue
        if callee in allow:
            used.add(callee)
            continue
        why = 'unguarded' if not conds else 'guarded only by ' + ' / '.join('(%s)' % re.sub(r'\s+', ' ', c).strip()[:90] for c in conds)
        errors.append('%s:%d: %s is registered %s: put it under a switch that defaults false and is not in the promotions lists (`!WWE2K14_STREAM() || REXCVAR_GET(x)` for "on in the stream-free build"), or '
                      'give it a reasoned line in %s' % (APP, line, callee, why, ALLOW))
    return errors, used


DIRECT = re.compile(r'(?<![\w:])(?:\w+::)*Register\s*\(\s*&')
HEADER = re.compile(r'^ {0,2}(?! *(?:if|for|while|switch|else|namespace|struct|class|auto|do)\b)[^\n#;{}]*?\b(\w+)\s*\([^;{}]*\)[^;{}]*\{\s*$', re.M)


def lint_direct(texts):
    """Every direct contributor registration (`Register(&x)`, the scene contributor registry) must sit inside a function named Register* / Install*, so the OnPostSetup scan above sees every way a
    contributor gets in. texts = {file: source}; -> [error]. (The registry's own definition and its self-test are exempt.)"""
    errs = []
    for f, t in texts.items():
        if 'live_scene' in f:
            continue
        code = strip(t)
        for m in DIRECT.finditer(code):
            name = None
            for fm in HEADER.finditer(code, 0, m.start()):   # the nearest function header (a line at indent 0-2 ending in `{`) before the call
                name = fm.group(1)
            if name is None or not re.match(r'(Register|Install)', name):
                errs.append('%s:%d: a direct contributor registration inside `%s`: move it into a function named Register* / Install* (OnPostSetup\'s switch check only sees those)' % (f, code.count('\n', 0, m.start()) + 1, name))
    return errs


def read(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def load_allow(text):
    allow = {}
    for ln in text.splitlines():
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        parts = [p.strip() for p in ln.split('|')]
        if len(parts) < 2 or not parts[1]:
            raise SystemExit('FAIL: %s: a line needs `callee | reason`: %r' % (ALLOW, ln))
        allow[parts[0]] = parts[1]
    return allow


def tree_inputs(root):
    app = read(root, APP)
    defaults = {}
    for dp, _, fns in os.walk(os.path.join(root, 'src')):
        for fn in fns:
            if fn.endswith(('.h', '.cpp')):
                try:
                    t = open(os.path.join(dp, fn), encoding='utf-8', errors='replace').read()
                except OSError:
                    continue
                for nm, dv in DEFINE.findall(t):
                    defaults[nm] = dv == 'true'
    pre, _ = body_of(app, 'void OnPreSetup(')
    promoted = set()
    for m in re.finditer(r'for\s*\(\s*const\s+char\*\s+name\s*:\s*\{(.*?)\}\s*\)', pre, re.S):
        k = pre.rfind('if (', 0, m.start())
        cond_line = pre[k:pre.find('\n', k)] if k >= 0 else ''
        if 'wwe2k14_s101_promotions' in cond_line:   # the two promotions blocks only (the K3 skips_auto list is an arming for a flag that is off by default)
            promoted.update(LIT.findall(m.group(1)))
    promoted.update(LIT.findall(read(root, 'src/bundle3.h')))
    return app, defaults, promoted


def selftest():
    bad = 0

    def case(name, text, want_errors, defaults=None, promoted=None, allow=None):
        nonlocal bad
        d = {'wwe2k14_rdr_weapons': False, 'wwe2k14_scene_light_states': False, 'wwe2k14_scene_records': False, 'wwe2k14_new_on': True, 'wwe2k14_rdr_ring_d3': False}
        d.update(defaults or {})
        p = {'wwe2k14_scene_light_states', 'wwe2k14_scene_records'} | set(promoted or [])
        app = 'class A {\n  void OnPostSetup() override {\n' + text + '\n  }\n};\n'
        errs, _ = lint(app, d, p, allow or {})
        ok = (len(errs) > 0) == want_errors
        print('%s  %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  -> ' + ' | '.join(errs)))
        bad += 0 if ok else 1

    # the two registrations the review found, exactly as they were: both must be flagged
    case('review: PW-2 under the promoted light-state switch', '    if (REXCVAR_GET(wwe2k14_scene_records)) {\n      if (REXCVAR_GET(wwe2k14_scene_light_states)) rdr_scene::live::weaponrows::RegisterWeaponRows();\n    }', True)
    case('review: PW-1 unguarded inside scene_records', '    if (REXCVAR_GET(wwe2k14_scene_records)) {\n      wwe2k14::scene_weapons_glue::Register();\n    }', True)
    case('a bare unguarded call', '    wwe2k14::x::RegisterThing();', True)
    case('a default-true switch', '    if (REXCVAR_GET(wwe2k14_new_on)) x::RegisterThing();', True)
    case('a promoted switch', '    if (REXCVAR_GET(wwe2k14_rdr_weapons)) x::RegisterThing();', True, promoted=['wwe2k14_rdr_weapons'])
    case('a new unguarded call in a block', '    if (REXCVAR_GET(wwe2k14_scene_records)) {\n      a::Register();\n      if (REXCVAR_GET(wwe2k14_rdr_weapons)) b::RegisterOk();\n    }', True)
    case('good: single statement under a false, unpromoted switch', '    if (REXCVAR_GET(wwe2k14_scene_records)) {\n      if (!WWE2K14_STREAM() || REXCVAR_GET(wwe2k14_rdr_weapons)) b::RegisterOk();\n    }', False)
    case('good: an && of a false unpromoted switch and a promoted one (the ring)', '    if (REXCVAR_GET(wwe2k14_rdr_ring_d3) && REXCVAR_GET(wwe2k14_scene_light_states)) {\n      rdr_scene::live::ringrows::RegisterRingRows();\n    }', False)   # an && of a false unpromoted switch and a promoted one is false for the player (the ring's real condition)
    case('good: block under a false, unpromoted switch only', '    if (REXCVAR_GET(wwe2k14_rdr_ring_d3)) {\n      rdr_scene::live::ringrows::RegisterRingRows();\n    }', False)
    case('an || with a promoted switch is on for every player', '    if (REXCVAR_GET(wwe2k14_rdr_weapons) || REXCVAR_GET(wwe2k14_scene_light_states)) x::RegisterThing();', True)
    case('a condition it cannot evaluate is not adequate', '    if (some_runtime_flag) x::RegisterThing();', True)
    case('good: an allowlisted callee with a reason', '    a::RegisterOld();', False, allow={'a::RegisterOld': 'legacy'})
    case('Install: a scene_live::Install* contributor under a promoted switch is flagged (InstallArena as it was)', '    if (REXCVAR_GET(wwe2k14_scene_records)) {\n      wwe2k14::scene_live::InstallArena(root);\n    }', True)
    for name, src, want in (('direct: a Register(&X) inside a helper not named Register*/Install* is flagged', 'inline void SetUp() {\n  Register(&Rows());\n}\n', True),
                            ('direct: inside RegisterThing is fine', 'inline void RegisterThing() {\n  if (!f) Register(&Rows());\n}\n', False),
                            ('direct: inside InstallThing is fine', 'inline void InstallThing() {\n  live::Register(&Rows());\n}\n', False)):
        ok = bool(lint_direct({'src/x.h': src})) == want
        print('%s  %s' % ('PASS' if ok else 'FAIL', name))
        bad += 0 if ok else 1
    # the unused-allowlist rule and the empty-read refusal
    errs, used = lint('class A { void OnPostSetup() override { } };', {}, set(), {})
    ok = bool(errs)
    print('%s  an OnPostSetup with no Register call is refused (the lint reads nothing)' % ('PASS' if ok else 'FAIL'))
    bad += 0 if ok else 1
    errs, used = lint('class A { void Other() { } };', {}, set(), {})
    ok = bool(errs)
    print('%s  a source with no OnPostSetup is refused' % ('PASS' if ok else 'FAIL'))
    bad += 0 if ok else 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        bad = selftest()
        print('register_switch_lint selftest: %s' % ('ALL PASS' if not bad else '%d FAILED' % bad))
        return 1 if bad else 0
    root = os.path.abspath(a.root)
    app, defaults, promoted = tree_inputs(root)
    if len(promoted) < 20:
        print('FAIL: only %d promoted switches read from OnPreSetup / bundle3.h (the lint reads nothing useful: refusing)' % len(promoted))
        return 1
    allow = load_allow(read(root, ALLOW))
    errors, used = lint(app, defaults, promoted, allow)
    srcs = {}
    for dp, _, fns in os.walk(os.path.join(root, 'src')):
        for fn in fns:
            if fn.endswith(('.h', '.cpp')):
                p = os.path.join(dp, fn)
                srcs[os.path.relpath(p, root).replace('\\', '/')] = open(p, encoding='utf-8', errors='replace').read()
    errors += lint_direct(srcs)
    for callee in sorted(set(allow) - used):
        errors.append('%s: %s is allowlisted but is no longer an unguarded registration in OnPostSetup: remove the line' % (ALLOW, callee))
    for e in errors:
        print('ERROR ' + e)
    print('register_switch_lint: %d error(s); %d allowlisted legacy callees, %d promoted switches, %d switch defaults read' % (len(errors), len(used), len(promoted), len(defaults)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
