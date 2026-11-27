#!/usr/bin/env python3
r"""S111 CDLINTS (WHOLEDIFF_PLAN A.6.1): every `REX_HOOK_RAW(sub_X)` body calls `__imp__sub_X` exactly once on each path, or leaves before it with a stated reason.

  py -I tools\lint\hook_original_lint.py [--root DIR] [--selftest] [--table]

Why: a raw hook REPLACES the game function; the original runs only if the body calls `__imp__sub_X(ctx, base)`. A path that forgets it silently drops the game's work (a skip nobody
decided), a path that calls it twice runs the game's side effects twice. Both are invisible in a build that compiles. The rule, per `REX_HOOK_RAW(sub_X) { ... }` body in src\ (only the
literal `sub_X` forms; the macro-generated ones, e.g. nf\reg_save.h, are not read):
  * every straight path through the body (brace-depth walk: if / else, loops and switch bodies may run or not, a `return` ends the path, `#if` / `#else` arms are each walked) holds
    exactly ONE call of `__imp__sub_X(`;
  * a path that returns BEFORE the call (0 calls so far) carries a line `// SKIP-ORIGINAL: <reason>` (a reason of 12+ characters) on the return's line, the line above it, or the
    header line (and the line above that) of an enclosing `if` / `else` / loop block that holds it;
  * a body whose every path skips the original (a full replacement) carries the same marker on the `REX_HOOK_RAW` line, the line above it, or any line inside the body's last statement's lines.
A hook whose body only hands on to a helper that holds the call carries `// ORIGINAL-IN: <Helper>` (hook line or the line above): the lint finds the helper's definition in the same file, requires the hook body to call it, and judges the HELPER's body by the same rules (its skips marked at its own return lines or above its definition). A missing helper, or a hook that never calls it, is a finding.
A path that ends with 2 or more calls is a finding unless the hook carries `// TWICE-ORIGINAL: <reason>` (12+ characters) on the `REX_HOOK_RAW` line or the line above it (a deliberate re-run of the original). The walk is textual (comments and strings masked), not a compile: a call inside a lambda or a local struct counts as
made on the path that declares it; a loop body counts as run once or not at all.
Every run also feeds planted twins (synthetic, and real hooks of the tree changed so that they must fire) and requires each to fire.
Exit 0 = pass, 1 = a finding (or a twin that did not fire), 2 = src missing. 0 game runs."""
import argparse
import os
import re
import sys

MARK = re.compile(r'SKIP-ORIGINAL:[ \t]*(?P<why>[^\n]*)')
MARK2 = re.compile(r'TWICE-ORIGINAL:[ \t]*(?P<why>[^\n]*)')
ORIG_IN = re.compile(r'ORIGINAL-IN:[ \t]*(?P<name>\w*)')


def find_helper(code, name):
    """(def_offset, body_start, body_end) of the function `name` defined in code (a definition: a return type before the name, a `{` after the parameter list), or None."""
    for m in re.finditer(r'(?<![\w:.>])%s\s*\(' % re.escape(name), code):
        pre = code[code.rfind('\n', 0, m.start()) + 1:m.start()]
        if not re.match(r'^\s*(?:(?:inline|static|constexpr)\s+)*[\w:<>]+[\s*&]+$', pre):
            continue
        d, i = 0, m.end() - 1
        while i < len(code):
            if code[i] == '(':
                d += 1
            elif code[i] == ')':
                d -= 1
                if d == 0:
                    break
            i += 1
        j = i + 1
        while True:
            while j < len(code) and code[j].isspace():
                j += 1
            w = re.match(r'(const|noexcept)\b', code[j:j + 10])
            if not w:
                break
            j += len(w.group(0))
        if j >= len(code) or code[j] != '{':
            continue
        d, k = 0, j
        while k < len(code):
            if code[k] == '{':
                d += 1
            elif code[k] == '}':
                d -= 1
                if d == 0:
                    break
            k += 1
        return m.start(), j + 1, k
    return None
HOOK = re.compile(r'\bREX_HOOK_RAW\s*\(\s*(sub_[0-9A-Fa-f]+)\s*\)\s*\{')
KEYS = ('if', 'for', 'while', 'switch', 'do', 'else')


def mask(t):
    """comments and string / char literals blanked to spaces (newlines and offsets kept)."""
    out, i, n = [], 0, len(t)
    while i < n:
        c = t[i]
        two = t[i:i + 2]
        if two == '//':
            j = t.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif two == '/*':
            j = t.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', t[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and t[j] != c and t[j] != '\n':
                j += 2 if t[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r'[^\n]', ' ', t[i + 1:j - 1]) + (c if j - 1 > i else ''))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def cond_variants(code, cap=8):
    """the text with each #if group's arms chosen one at a time: a list of texts (directive lines blanked, offsets kept). Nested groups: the arms of the outer one decide first."""
    lines = code.split('\n')
    # build a tree-free enumeration: each directive group -> list of arm line-sets; enumerate the product (capped)
    groups, stack = [], []   # group = list of (start_line, end_line) arms
    for i, ln in enumerate(lines):
        m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b', ln)
        if not m:
            continue
        k = m.group(1)
        if k in ('if', 'ifdef', 'ifndef'):
            stack.append([i + 1])   # arm starts after this line
            stack[-1].append(None)
        elif k in ('elif', 'else') and stack:
            stack[-1].append(i)
        elif k == 'endif' and stack:
            g = stack.pop()
            starts = [g[0]] + [x + 1 for x in g[2:]]
            ends = [x for x in g[2:]] + [i]
            groups.append([(a, b) for a, b in zip(starts, ends)])
    variants = [set()]   # each: the set of line indexes dropped
    for g in groups:
        if len(g) < 2:
            continue
        nv = []
        for v in variants:
            for keep in range(len(g)):
                d = set(v)
                for j, (a, b) in enumerate(g):
                    if j != keep:
                        d.update(range(a, b))
                nv.append(d)
        variants = nv[:cap * 4]
    outs = []
    for v in variants[:cap]:
        outs.append('\n'.join('' if (i in v or re.match(r'\s*#', ln)) else ln for i, ln in enumerate(lines)))
    return outs or [code]


class P:
    def __init__(self, s, base):
        self.s, self.base = s, base   # base: absolute offset of s[0] in the file text (for line numbers)

    def skip_ws(self, i):
        while i < len(self.s) and self.s[i].isspace():
            i += 1
        return i

    def balanced(self, i, o, c):
        d = 0
        while i < len(self.s):
            if self.s[i] == o:
                d += 1
            elif self.s[i] == c:
                d -= 1
                if d == 0:
                    return i
            i += 1
        return len(self.s) - 1

    def word_at(self, i):
        m = re.match(r'[A-Za-z_]\w*', self.s[i:i + 40])
        return m.group(0) if m else ''

    def one(self, i):
        """one statement or block at i: (node, next_i)."""
        i = self.skip_ws(i)
        if i >= len(self.s):
            return None, i
        c = self.s[i]
        if c == '{':
            j = self.balanced(i, '{', '}')
            return ('block', self.seq(i + 1, j), self.base + i), j + 1
        w = self.word_at(i)
        if w == 'if' or w in ('for', 'while', 'switch'):
            k = self.skip_ws(i + len(w))
            if self.word_at(k) == 'constexpr':
                k = self.skip_ws(k + len('constexpr'))
            if k < len(self.s) and self.s[k] == '(':
                e = self.balanced(k, '(', ')')
                cond = self.s[k:e + 1]
                body, j = self.one(e + 1)
                if w == 'if':
                    j2 = self.skip_ws(j)
                    els = None
                    if self.word_at(j2) == 'else':
                        els, j = self.one(j2 + 4)
                    return ('if', cond, body, els, self.base + i), j
                return ('loop', cond, body, self.base + i), j
        if w == 'try':
            body, j = self.one(i + 3)
            catches = []
            while True:
                j2 = self.skip_ws(j)
                if self.word_at(j2) != 'catch':
                    break
                k = self.skip_ws(j2 + 5)
                e = self.balanced(k, '(', ')')
                cb, j = self.one(e + 1)
                catches.append(cb)
            return ('try', body, catches, self.base + i), j
        if w == 'do':
            body, j = self.one(i + 2)
            j = self.skip_ws(j)
            if self.word_at(j) == 'while':
                k = self.skip_ws(j + 5)
                e = self.balanced(k, '(', ')')
                j = e + 1
                j = self.skip_ws(j)
                if j < len(self.s) and self.s[j] == ';':
                    j += 1
            return ('block', [body] if body else [], self.base + i), j
        # a simple statement: to the ';' at depth 0 (braces of a lambda / local struct count as depth)
        d, j = 0, i
        while j < len(self.s):
            ch = self.s[j]
            if ch in '({[':
                d += 1
            elif ch in ')}]':
                d -= 1
            elif ch == ';' and d <= 0:
                break
            j += 1
        return ('stmt', self.s[i:j + 1], self.base + i), j + 1

    def seq(self, i, end):
        sub = P(self.s[:end], self.base)
        out = []
        while True:
            i = sub.skip_ws(i)
            if i >= end:
                break
            node, j = sub.one(i)
            if node is None or j <= i:
                break
            out.append(node)
            i = j
        return out


def inner_masked(t):
    """a simple statement with the contents of its nested {...} removed (a lambda's or local struct's returns are not the hook's)."""
    out, d = [], 0
    for ch in t:
        if ch == '{':
            d += 1
            out.append(ch)
        elif ch == '}':
            d -= 1
            out.append(ch)
        else:
            out.append(ch if d == 0 else (' ' if ch != '\n' else '\n'))
    return ''.join(out)


def walk(nodes, counts, call, exits, hdrs):
    """counts: set of call counts (0, 1, 2 = two or more) on the paths reaching here; returns the counts of the paths that fall through. exits += (count, offset of the return, hdr offsets)."""
    for nd in nodes:
        if not counts:
            break
        if nd is None:
            continue
        k = nd[0]
        if k == 'stmt':
            txt = nd[1]
            n = len(call.findall(txt))
            m = re.search(r'\breturn\b', inner_masked(txt))
            if m:
                for c in counts:
                    exits.append((min(2, c + n), nd[2] + m.start(), tuple(hdrs)))
                counts = set()
            else:
                counts = {min(2, c + n) for c in counts}
        elif k == 'block':
            counts = walk(nd[1], counts, call, exits, hdrs + [nd[2]])
        elif k == 'if':
            n = len(call.findall(nd[1]))
            counts = {min(2, c + n) for c in counts}
            a = walk([nd[2]], set(counts), call, exits, hdrs + [nd[4]])
            b = walk([nd[3]], set(counts), call, exits, hdrs + [nd[4]]) if nd[3] else set(counts)
            counts = a | b
        elif k == 'try':   # the catch arms start from the count before the try (a throw can leave the body anywhere)
            out = walk([nd[1]], set(counts), call, exits, hdrs + [nd[3]])
            for cb in nd[2]:
                out |= walk([cb], set(counts), call, exits, hdrs + [nd[3]])
            counts = out
        elif k == 'loop':
            n = len(call.findall(nd[1]))
            counts = {min(2, c + n) for c in counts}
            counts = walk([nd[2]], set(counts), call, exits, hdrs + [nd[3]]) | counts
    return counts


def hook_bodies(text):
    """[(sub, header_offset, body_start, body_end)] of every literal REX_HOOK_RAW(sub_X) { ... } in text (offsets in text; comments / strings masked for the brace match)."""
    code = mask(text)
    out = []
    for m in HOOK.finditer(code):
        d, i = 0, m.end() - 1
        while i < len(code):
            if code[i] == '{':
                d += 1
            elif code[i] == '}':
                d -= 1
                if d == 0:
                    break
            i += 1
        out.append((m.group(1), m.start(), m.end(), i))
    return code, out


def judge_text(path, text):
    """([findings], [(path, sub, state)]) for one file's text."""
    bad, table = [], []
    code, hooks = hook_bodies(text)
    raw = text.split('\n')
    starts = [0]
    for ln in text.split('\n'):
        starts.append(starts[-1] + len(ln) + 1)

    def line_of(off):
        lo, hi = 0, len(starts) - 1
        while lo < hi - 1:
            mid = (lo + hi) // 2
            if starts[mid] <= off:
                lo = mid
            else:
                hi = mid
        return lo + 1

    def marked(ln):   # a SKIP-ORIGINAL line with a reason on 1-based line ln or the one above
        for x in (ln, ln - 1):
            if 1 <= x <= len(raw):
                m = MARK.search(raw[x - 1])
                if m and len(m.group('why').strip()) >= 12:
                    return True
        return False

    def thin(ln):
        for x in (ln, ln - 1):
            if 1 <= x <= len(raw):
                m = MARK.search(raw[x - 1])
                if m and len(m.group('why').strip()) < 12:
                    return True
        return False

    for sub, h0, b0, b1 in hooks:
        call = re.compile(r'\b__imp__%s\b' % sub)   # a call, or the original handed on as a value (Run<__imp__sub_X>, Wrap(__imp__sub_X, ...)): the wrapper is not read
        body = code[b0:b1]
        hl = line_of(h0)
        found = set()
        oi = None   # `// ORIGINAL-IN: Helper` on the hook line or above it: the original is called inside Helper (same file), which is judged by the same rule instead of the hook body
        for x in (hl, hl - 1):
            if 1 <= x <= len(raw):
                mo = ORIG_IN.search(raw[x - 1])
                if mo:
                    oi = mo.group('name')
        if oi is not None:
            helper = find_helper(code, oi) if oi else None
            if not oi or not re.search(r'(?<![\w.>])%s\s*\(' % re.escape(oi), body):   # (a qualified call, ns::Helper(, counts)
                found.add((hl, '%s (line %d): `// ORIGINAL-IN: %s` but the hook body does not call that helper' % (sub, hl, oi or '<name>')))
            elif not helper:
                found.add((hl, '%s (line %d): `// ORIGINAL-IN: %s` but no definition of %s is in this file' % (sub, hl, oi, oi)))
            else:
                h0, b0, b1 = helper
                body = code[b0:b1]
                hl = line_of(h0)
        whole = marked(hl) or marked(line_of(b1 - 1) )
        twice_ok = False
        for x in (hl, hl - 1):
            if 1 <= x <= len(raw):
                m2 = MARK2.search(raw[x - 1])
                if m2 and len(m2.group('why').strip()) >= 12:
                    twice_ok = True
        calls_total = 0
        for var in cond_variants(body):
            nodes = P(var, b0).seq(0, len(var))
            calls_total += len(call.findall(var))
            exits = []
            end = walk(nodes, {0}, call, exits, [])
            for c, off, hdrs in exits:
                ln = line_of(off)
                if c >= 2:
                    if not twice_ok:
                        found.add((ln, '%s (line %d): a path reaches this return with %s called more than once (a deliberate re-run: `// TWICE-ORIGINAL: <reason>` on the hook line or above it)' % (sub, ln, '__imp__' + sub)))
                elif c == 0 and not whole and not marked(ln) and not any(marked(line_of(hh)) for hh in hdrs):
                    why = ' (the SKIP-ORIGINAL reason is under 12 characters)' if thin(ln) else ''
                    found.add((ln, '%s (line %d): returns before __imp__%s with no `// SKIP-ORIGINAL: <reason>` on the return line, the line above or its if-header%s' % (sub, ln, sub, why)))
            if 2 in end and not twice_ok:
                found.add((line_of(b1), '%s (line %d): a path reaches the end of the body with __imp__%s called more than once (a deliberate re-run: `// TWICE-ORIGINAL: <reason>` on the hook line or above it)' % (sub, line_of(b1), sub)))
            if 0 in end and not whole:
                found.add((line_of(b1), '%s (line %d): a path reaches the end of the body without calling __imp__%s (add the call, or `// SKIP-ORIGINAL: <reason>` on the hook line / above it for a full replacement)' % (sub, line_of(b1), sub)))
        for _ln, msg in sorted(found):
            bad.append('%s: %s' % (path, msg))
        table.append((path, sub, 'MISSING' if found else ('replaces' if whole and not calls_total else 'once')))
    return bad, table


def judge(files):
    bad, table = [], []
    for p in sorted(files):
        b, t = judge_text(p, files[p])
        bad += b
        table += t
    return bad, table


# ---- planted twins -----------------------------------------------------------------------------------------------------------------------------------------------------------
def hk(body, sub='sub_82000010'):
    return 'REX_HOOK_RAW(%s) {\n%s\n}\n' % (sub, body)


CALL = '  __imp__sub_82000010(ctx, base);\n'
HELP_ONCE = 'inline void Help(PPCContext& ctx, uint8_t* base) {\n  if (X()) {\n    __imp__sub_82000010(ctx, base);\n    return;\n  }\n  __imp__sub_82000010(ctx, base);\n}\n'
GOOD = {
    'a_once.h': hk('  Note(ctx);\n' + CALL + '  After();'),
    'b_if_else.h': hk('  if (X()) {\n    __imp__sub_82000010(ctx, base);\n  } else {\n    __imp__sub_82000010(ctx, base);\n  }'),
    'c_skip_above.h': hk('  // SKIP-ORIGINAL: the boot screens are skipped whole\n  if (Skip()) return;\n' + CALL),
    'd_skip_trailing.h': hk('  if (Skip()) return;   // SKIP-ORIGINAL: the boot screens are skipped whole\n' + CALL),
    'e_skip_in_block_header.h': hk('  if (Skip()) {   // SKIP-ORIGINAL: the scene is OURS this frame, nothing of the original runs\n    Note();\n    return;\n  }\n' + CALL),
    'f_return_after.h': hk(CALL + '  if (Y()) return;\n  Tail();'),
    'g_return_call.h': hk('  return __imp__sub_82000010(ctx, base);'),
    'h_replace.h': '// SKIP-ORIGINAL: the game function is replaced whole, ours does its work\n' + hk('  Ours(ctx);'),
    'i_preproc.h': hk('#if A()\n  Pre();\n#else\n  Pre2();\n#endif\n' + CALL),
    'j_lambda_return.h': hk('  auto f = [&](int x) { if (x) return 1; return 2; };\n' + CALL),
    'k_nested_calls.h': hk('  Scope s(ctx);\n  if (s.skipped()) {\n    __imp__sub_82000010(ctx, base);\n    return;\n  }\n  __imp__sub_82000010(ctx, base);'),
    'l_not_a_hook.h': 'void Helper(Ctx& ctx) { if (X) return; }\n',
    'm_twice_marked.h': '// TWICE-ORIGINAL: the original is re-run once with the setting forced\n' + hk(CALL + CALL),
    'n_original_in.h': HELP_ONCE + '// ORIGINAL-IN: Help\n' + hk('  Help(ctx, base);'),
    'o_original_in_above_hook_line.h': HELP_ONCE + hk('  Help(ctx, base);').replace('REX_HOOK_RAW', '// ORIGINAL-IN: Help\nREX_HOOK_RAW', 1),
    'p_original_in_helper_skip.h': ('inline void Help(PPCContext& ctx, uint8_t* base) {\n  if (Native()) {\n    return;   // SKIP-ORIGINAL: native code replaces the game function by switch\n  }\n'
                                    '  __imp__sub_82000010(ctx, base);\n}\n') + '// ORIGINAL-IN: Help\n' + hk('  Help(ctx, base);'),
}
TWINS = [
    ('ORIGINAL-IN: the helper never calls the original', {'a.h': 'inline void Help(PPCContext& ctx, uint8_t* base) {\n  Ours();\n}\n// ORIGINAL-IN: Help\n' + hk('  Help(ctx, base);')}),
    ('ORIGINAL-IN: the helper has a path with no call', {'a.h': 'inline void Help(PPCContext& ctx, uint8_t* base) {\n  if (X()) return;\n  __imp__sub_82000010(ctx, base);\n}\n// ORIGINAL-IN: Help\n' + hk('  Help(ctx, base);')}),
    ('ORIGINAL-IN: the helper calls the original twice', {'a.h': 'inline void Help(PPCContext& ctx, uint8_t* base) {\n' + CALL + CALL + '}\n// ORIGINAL-IN: Help\n' + hk('  Help(ctx, base);')}),
    ('ORIGINAL-IN: the helper is not in the file', {'a.h': '// ORIGINAL-IN: Missing\n' + hk('  Missing(ctx, base);')}),
    ('ORIGINAL-IN: the hook body never calls the helper', {'a.h': HELP_ONCE + '// ORIGINAL-IN: Help\n' + hk('  Ours(ctx);')}),
    ('ORIGINAL-IN: a thin marker does not count as the helper name', {'a.h': HELP_ONCE + '// ORIGINAL-IN:\n' + hk('  Help(ctx, base);')}),
    ('an early return with no marker', {'a.h': hk('  if (Skip()) return;\n' + CALL)}),
    ('an early return with a thin reason', {'a.h': hk('  if (Skip()) return;   // SKIP-ORIGINAL: no\n' + CALL)}),
    ('a marker two lines above the return (not adjacent)', {'a.h': hk('  // SKIP-ORIGINAL: the boot screens are skipped whole\n  Note();\n  if (Skip()) return;\n' + CALL)}),
    ('the call twice in a row', {'a.h': hk(CALL + CALL)}),
    ('the call twice with a thin TWICE-ORIGINAL reason', {'a.h': '// TWICE-ORIGINAL: no\n' + hk(CALL + CALL)}),
    ('a SKIP-ORIGINAL marker does not excuse the call twice', {'a.h': '// SKIP-ORIGINAL: the game function is replaced whole, ours does its work\n' + hk(CALL + CALL)}),
    ('the call twice, one in an if', {'a.h': hk('  if (X()) {\n    __imp__sub_82000010(ctx, base);\n  }\n' + CALL)}),
    ('the call in a branch only (the else path never calls it)', {'a.h': hk('  if (X()) {\n    __imp__sub_82000010(ctx, base);\n  }')}),
    ('no call at all, no marker', {'a.h': hk('  Ours(ctx);')}),
    ('a return after the call and a second call after that', {'a.h': hk(CALL + '  if (Y()) { __imp__sub_82000010(ctx, base); return; }')}),
    ('the call in only one #if arm', {'a.h': hk('#if A()\n' + CALL + '#else\n  Pre2();\n#endif')}),
    ('another function\'s original does not count', {'a.h': hk('  __imp__sub_82000020(ctx, base);')}),
    ('the call only in a comment', {'a.h': hk('  // __imp__sub_82000010(ctx, base);\n  Ours();')}),
]


def selftest(files):
    out = []
    bad, _t = judge(GOOD)
    if bad:
        out.append('selftest: the good synthetic set fails: %s' % bad)
    for name, fs in TWINS:
        if not judge(fs)[0]:
            out.append('selftest: the planted twin "%s" did not fire' % name)
    # real-tree plants: a hook that is clean today, changed so that it must fire
    clean = None
    for p in sorted(files):
        b, t = judge_text(p, files[p])
        if t and not b and any(re.search(r'^[ \t]*__imp__%s\(ctx, base\);[ \t]*$' % s, files[p], re.M) for _p, s, st in t if st == 'once'):
            for _p, s, st in t:
                m = re.search(r'^([ \t]*)__imp__%s\(ctx, base\);[ \t]*$' % s, files[p], re.M)
                if st == 'once' and m and files[p].count(m.group(0)) == 1:
                    clean = (p, s, m)
                    break
        if clean:
            break
    if not clean:
        out.append('selftest: no real hook with a plain `__imp__sub_X(ctx, base);` line found (the tree moved: update the real-tree plants)')
    else:
        p, s, m = clean
        t = files[p]
        for name, new in (('the call doubled', m.group(0) + '\n' + m.group(0)),
                          ('the call cut', ''),
                          ('an early return planted before the call', '  if (planted_skip) return;\n' + m.group(0))):
            planted = dict(files)
            planted[p] = t.replace(m.group(0), new, 1)
            if not judge_text(p, planted[p])[0]:
                out.append('selftest: real-tree plant (%s in %s of %s) did not fire' % (name, s, p))
    return out


def read_src(root):
    files = {}
    for dp, _d, fns in os.walk(os.path.join(root, 'src')):
        for fn in sorted(fns):
            if fn.endswith(('.h', '.cpp', '.inl', '.hpp')):
                p = os.path.join(dp, fn)
                with open(p, encoding='utf-8', errors='replace') as f:
                    t = f.read()
                if 'REX_HOOK_RAW' in t:
                    files[os.path.relpath(p, root).replace('\\', '/')] = t
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true', help='only the planted twins')
    ap.add_argument('--table', action='store_true', help='print every hook and its state')
    a = ap.parse_args()
    if not os.path.isdir(os.path.join(a.root, 'src')):
        print('hook_original_lint: no src under %s' % a.root)
        return 2
    files = read_src(a.root)
    st = selftest(files)
    for s in st:
        print('FAIL ' + s)
    if a.selftest:
        print('hook_original_lint selftest: %s' % ('FAIL' if st else 'every twin fires (%d synthetic, 3 on the real tree)' % len(TWINS)))
        return 1 if st else 0
    bad, table = judge(files)
    for b in bad:
        print('FAIL ' + b)
    if a.table:
        for p, s, state in table:
            print('%-8s %s %s' % (state, s, p))
    print('hook_original_lint: %s (%d REX_HOOK_RAW(sub_X) bodies in %d files; %d findings; %d synthetic + 3 real-tree twins, %d did not fire)' % (
        'FAIL (%d)' % (len(bad) + len(st)) if bad or st else 'pass', len(table), len(files), len(bad), len(TWINS), len(st)))
    return 1 if bad or st else 0


if __name__ == '__main__':
    sys.exit(main())
