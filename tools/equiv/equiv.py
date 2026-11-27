"""S111 EQUIV piece B: the equivalence tester's driver (docs\\research\\s111\\EQUIV0.md).

The game's own recompiled function and our port are called on the same inputs (equiv\\equiv_tests.cpp, the test DLL, run by piece A's headless
runner inside the game exe). This driver decides whether a test CAN be isolated, plans it, runs it, reads the
branch counts of the game function and steers inputs toward its unhit branches, and gives the verdict:
    PROVEN            the outputs agree on every case AND every branch of the game function ran both ways (clang's branch counts of its
                      generated file, the covmap exe)
    AGREES-UNCOVERED  the outputs agree, but these branches never ran one way (or no branch counts exist: an exe without covmap)
    REFUTED           a case differs: the shrunk case (fields off their defaults) and both outputs
    NOT-ISOLABLE      the closure holds a kernel import or one of our hooks the test does not stub, a stub needs the coverage tier, a
                      guest fault, or the call left the allowlist
Usage (py -I):
    equiv.py list                          the DLL's tests
    equiv.py plan TEST                     closure (q callees), refusals, the allowlist file, the input fields against q decomp --fields,
                                           the branches with how each is steered (no run). A target under one of our hooks that passes straight
                                           through to the original under named flags (tools\\equiv\\hook_passthrough.py) is allowed and the run
                                           passes those flags itself; any other hook is refused.
    equiv.py run TEST --exe EXE [--cases N] [--rounds K] [--max-runs M] [--expect VERDICT]   piece A's headless runner + branch counts +
                                           steering; exit 0 = PROVEN (or the --expect verdict: a planted twin's REFUTED in a proof file)
    equiv.py cov-build TEST[,TEST...] [--dry]   the one exe with the stub seam AND branch counts, for any test (tools\\equiv\\equiv_covexe.py:
                                           the commit's coverage lane exe's slot build + guest_variant_exe.py --variant covmap over it)
Options: --dll PATH (default out\\tools\\equiv\\wwe2k14_equiv_tests.dll of this checkout), --equiv-dir (default out\\harness\\equiv).
Inputs reach the DLL as WWE2K14_EQUIV_PINS="field=value;..." (branch steering). Contains nothing from the game.
"""
import argparse
import ctypes
import glob
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # py -I does not put the script's folder on the path
import hook_passthrough  # noqa: E402  (the table of our hooks that pass through to the original under flags)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
Q = os.path.join(REPO, 'tools', 'q', 'q.py')
DLL = os.path.join(REPO, 'out', 'tools', 'equiv', 'wwe2k14_equiv_tests.dll')
EQUIV_DIR = os.path.join(REPO, 'out', 'harness', 'equiv')
LLVM = r'C:\Program Files\LLVM\bin'
# The register-save helpers every prologue may call (q decomp's "helpers"): allowed in any closure.
HELPER = re.compile(r'^__(save|rest)(gpr|fpr|vmx)(lr)?_\d+$')
PLANT = os.environ.get('EQUIV_PLANT', '')   # tools\equiv\equiv_selftest.py's planted faults (each must fail its own check)


# ---------------------------------------------------------------------------------------------------------------- the test table
class Field:
    def __init__(self, name, kind, lo, hi, default):
        self.name, self.kind, self.lo, self.hi, self.default = name, kind, lo, hi, default


class TestDef:
    def __init__(self, name, target, twin_of, stubs, fields, what):
        self.name, self.target, self.twin_of, self.stubs, self.fields, self.what = name, target, twin_of, stubs, fields, what

    def field(self, name):
        return next((f for f in self.fields if f.name == name), None)


def parse_list(text):
    tests = {}
    for line in text.splitlines():
        parts = line.split('\t')
        if len(parts) < 6:
            continue
        name, target, twin, stubs, fields, what = parts[:6]
        fl = []
        for item in filter(None, fields.split(';')):
            n, spec = item.rsplit('=', 1)
            kind, lo, hi, d = spec.split(',')
            fl.append(Field(n, kind, float(lo), float(hi), float(d)))
        tests[name] = TestDef(name, int(target, 16), None if twin == '-' else twin,
                              [int(s, 16) for s in stubs.split(',') if s], fl, what)
    return tests


def load_tests(dll):
    if not os.path.exists(dll):
        raise SystemExit(f'no test DLL at {dll} (build it: pwsh -NoProfile -File tools\\equiv\\build_equiv.ps1 -Standalone)')
    lib = ctypes.CDLL(dll)
    fn = lib.wwe2k14_equiv_list
    fn.argtypes = [ctypes.c_char_p, ctypes.c_int]
    fn.restype = ctypes.c_int
    n = fn(None, 0)
    buf = ctypes.create_string_buffer(n + 1)
    fn(buf, n + 1)
    return parse_list(buf.value.decode('utf-8', 'replace'))


# ---------------------------------------------------------------------------------------------------------------- q
def q(*args):
    r = subprocess.run([sys.executable, Q] + list(args), capture_output=True, text=True, errors='replace', cwd=REPO)
    if r.returncode:
        raise SystemExit(f'q {" ".join(args)} failed ({r.returncode}): {(r.stderr or r.stdout)[-400:]}')
    return r.stdout


def q_json(*args):
    out = q(*args)
    return json.loads(out[out.index('{'):])


def fname(addr):
    return f'sub_{addr:08X}'


# ---------------------------------------------------------------------------------------------------------------- hooks we link
HOOK_DIRECT = re.compile(r'\bREX_HOOK(?:_RAW)?\(\s*(sub_[0-9A-Fa-f]{8})\b')
HOOK_MACRO_DEF = re.compile(r'^\s*#\s*define\s+(\w+)\s*\(')


def hooked_functions(texts):
    """{sub_X: file} for every function one of our files hooks: REX_HOOK_RAW(sub_X) / REX_HOOK(sub_X, ...) written out, and the uses of a
    macro whose body holds REX_HOOK_RAW (e.g. MY_HOOK(sub_X)). `texts` = {path: text}."""
    found, macros = {}, set()
    for path, text in texts.items():
        lines = text.splitlines()
        for i, line in enumerate(lines):
            m = HOOK_MACRO_DEF.match(line)
            if not m:
                continue
            body, j = line, i
            while body.rstrip().endswith('\\') and j + 1 < len(lines):
                j += 1
                body += '\n' + lines[j]
            if 'REX_HOOK' in body:
                macros.add(m.group(1))
    use = re.compile(r'\b(' + '|'.join(sorted(map(re.escape, macros))) + r')\(\s*(sub_[0-9A-Fa-f]{8})\b') if macros else None
    for path, text in texts.items():
        for m in HOOK_DIRECT.finditer(text):
            found.setdefault(m.group(1).upper().replace('SUB_', 'sub_'), path)
        if use:
            for m in use.finditer(text):
                found.setdefault(m.group(2).upper().replace('SUB_', 'sub_'), path)
    return found


def src_texts():
    out = {}
    for p in glob.glob(os.path.join(REPO, 'src', '**', '*.*'), recursive=True):
        if p.endswith(('.h', '.cpp', '.hpp', '.inl')):
            with open(p, encoding='utf-8', errors='replace') as f:
                out[os.path.relpath(p, REPO)] = f.read()
    return out


# ---------------------------------------------------------------------------------------------------------------- the closure
def callee_map(tree):
    """{func: [child funcs]} from q callees --json: q lists a function's callees once (later occurrences say "seen above" with none), so
    the union over every occurrence is the call graph whatever the order."""
    m, ind = {}, []

    def walk(node):
        kids = [c['func'] for c in node.get('callees', [])]
        if kids or node['func'] not in m:
            m.setdefault(node['func'], [])
            for k in kids:
                if k not in m[node['func']]:
                    m[node['func']].append(k)
        for s in node.get('sites', []) or []:
            if s.get('kind') not in ('call', 'tail'):
                ind.append((node['func'], s.get('site'), s.get('kind')))
        for c in node.get('callees', []):
            walk(c)

    root = {'func': tree['func'], 'callees': tree.get('callees', [])}
    walk(root)
    return m, ind


def closure(tree, stubs, hooked):
    """The functions a call of tree['func'] runs. Stubbed functions are not entered. Returns (funcs, stubbed reached, refusals): a refusal
    = (reason text) for a kernel import or a hooked function reached without a stub on the way."""
    m, indirect = callee_map(tree)
    stubset = {fname(a) for a in stubs}
    funcs, stubbed, refusals, seen = [tree['func']], set(), [], {tree['func']}
    stack = [(tree['func'], [tree['func']])]
    while stack:
        f, path = stack.pop()
        for c in m.get(f, []):
            if c in stubset:
                stubbed.add(c)
                continue
            if c.startswith('__imp__') and PLANT != 'closure_ignores_imports':
                refusals.append(f'kernel import {c} reached through {" -> ".join(path)} (stub {path[-1] if len(path) > 1 else c}, or a caller of it)')
                continue
            if c in hooked and PLANT != 'closure_ignores_hooks':
                refusals.append(f'{c} is hooked by us ({hooked[c]}), reached through {" -> ".join(path)}: the hook would run in the test; stub it or test the hook')
                continue
            if HELPER.match(c) or c in seen:
                continue
            seen.add(c)
            funcs.append(c)
            stack.append((c, path + [c]))
    if indirect:
        for f, site, kind in indirect:
            refusals.append(f'indirect call ({kind}) in {f} at {site}: the static tier stops at its target (SDK InvalidFunctionTrap); NOT isolable without that target in the allowlist')
    if tree.get('indirect_calls'):
        refusals.append(f'{tree["func"]} makes {tree["indirect_calls"]} indirect call(s) (bctrl): not isolable without their targets')
    return funcs, sorted(stubbed), refusals


# ---------------------------------------------------------------------------------------------------------------- branches and steering
CMP_OPS = ['>=u', '<=u', '==', '!=', '>=', '<=', '>u', '<u', '>', '<']
LINE_BLOCK = re.compile(r'^\s*(?:\}\s*)?(?:if|while)\s*\((.*)\)\s*;?\s*\{?\s*//\s*\+([0-9A-Fa-f]+)\b')
LINE_JUMP = re.compile(r'^\s*\+([0-9A-Fa-f]+)\s+if\s*\((.*)\)\s+(?:goto|return)\b')
CALL_ROW = re.compile(r'^\s*\+([0-9A-Fa-f]+)\s+(?:call|tail)#([0-9A-Fa-f]+)\s+(sub_[0-9A-Fa-f]{8}|__imp__\w+)')
NUM = re.compile(r'^-?(?:0x[0-9A-Fa-f]+|\d+(?:\.\d*)?(?:e-?\d+)?)(?:\s*\([^)]*\))?(?:\[0x[0-9A-Fa-f]+\])?$')


class Branch:
    def __init__(self, off, cond):
        self.off, self.cond = off, cond
        self.lhs = self.op = self.rhs = None
        self.split()

    def split(self):
        """The top-level compare `lhs OP rhs` (q decomp writes the operator between spaces)."""
        depth, s = 0, strip_parens(self.cond)
        for i, ch in enumerate(s):
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            elif ch == ' ' and depth == 0:
                for op in CMP_OPS:
                    if s.startswith(' ' + op + ' ', i):
                        self.lhs, self.op, self.rhs = s[:i].strip(), op, s[i + len(op) + 2:].strip()
                        return


def branches(lines):
    """Every conditional branch row of a q decomp listing: (offset, condition). Also {call# offset: callee} for ret# operands."""
    out, calls = [], {}
    for line in lines:
        m = CALL_ROW.match(line)
        if m:
            calls[int(m.group(2), 16)] = m.group(3)
        m = LINE_BLOCK.match(line)
        if m:
            out.append(Branch(int(m.group(2), 16), m.group(1)))
            continue
        m = LINE_JUMP.match(line)
        if m:
            out.append(Branch(int(m.group(1), 16), m.group(2)))
    return out, calls


def strip_parens(s):
    s = s.strip()
    while s.startswith('(') and s.endswith(')'):
        depth = 0
        for i, ch in enumerate(s):
            depth += ch == '('
            depth -= ch == ')'
            if depth == 0 and i < len(s) - 1:
                return s
        s = s[1:-1].strip()
    return s


def const_value(s):
    s = strip_parens(s)
    if not NUM.match(s):
        return None
    v = re.match(r'^-?(?:0x[0-9A-Fa-f]+|\d+(?:\.\d*)?(?:e-?\d+)?)', s).group(0)
    if v.lstrip('-').startswith('0x'):
        n = int(v, 16)
        return float(n - (1 << 32) if n >= 1 << 31 else n), True
    return float(v), ('.' not in v and 'e' not in v)


def operand(s, test, calls):
    """('field', name) / ('const', value, is_int) / ('no', why)."""
    s = strip_parens(s)
    c = const_value(s)
    if c is not None:
        return ('const',) + c
    m = re.match(r'^ret#([0-9A-Fa-f]+)$', s)
    if m:
        callee = calls.get(int(m.group(1), 16))
        if not callee:
            return ('no', f'{s}: no call row at that offset')
        nm = f'ret#{callee}'
        if test and test.field(nm):
            return ('field', nm)
        return ('no', f'{s} = the return of {callee}, not stubbed by the test (a callee return is steered only through its stub)')
    if test and test.field(s):
        return ('field', s)
    if re.search(r'\br\d+@[0-9A-Fa-f+]+', s):
        return ('no', f'{s}: a loop-carried / merged value (r@addr): not steerable by one field')
    if re.search(r'\bL\d+@', s):
        return ('no', f'{s}: a stack local (from bytes the function copied): not steerable by one field')
    if re.match(r'^\[[^\[\]]+\]$|^in_r\d+$|^\[\[0x[0-9A-Fa-f]+\]\+\d+\]$', s):
        return ('no', f'{s}: no input field of that name in the test')
    return ('no', f'{s}: computed from several values: not steerable by one field')


def neighbours(v, is_int, f):
    if is_int:
        vals = [v - 1, v, v + 1]
    else:
        x = struct.unpack('<f', struct.pack('<f', v))[0]
        vals = [math.nextafter(x, -math.inf), x, math.nextafter(x, math.inf)]
    if f is not None:
        vals = [min(max(x, f.lo), f.hi) for x in vals]
    out = []
    for x in vals:
        if x not in out:
            out.append(x)
    return out


def steer(branch, test, calls):
    """Pin sets that put the compare's input side on both sides of it, or (None, why)."""
    if branch.op is None:
        return None, f'{branch.cond}: not a compare of two values'
    a, b = operand(branch.lhs, test, calls), operand(branch.rhs, test, calls)
    if a[0] == 'const' and b[0] == 'field':
        a, b = b, a
    if a[0] == 'field' and b[0] == 'const':
        f = test.field(a[1])
        return [{a[1]: v} for v in neighbours(b[1], b[2] and (f is None or f.kind == 'int'), f)], None
    if a[0] == 'field' and b[0] == 'field':
        fa, fb = test.field(a[1]), test.field(b[1])
        return [{a[1]: v, b[1]: fb.default} for v in neighbours(fb.default, fb.kind == 'int', fa)], None
    whys = [x[1] for x in (a, b) if x[0] == 'no']
    return None, '; '.join(whys) or f'{branch.cond}: two constants'


# ---------------------------------------------------------------------------------------------------------------- branch counts (covmap exe)
def body_addresses(gen_lines, start_line, func_addr):
    """{line number: guest address} of one generated function body: each `// mnemonic` comment is one PowerPC instruction (4 bytes, in
    order from the function's address); a code line belongs to the comment above it. start_line = the DEFINE_REX_FUNC line (1-based)."""
    out, idx, cur = {}, -1, func_addr
    for n in range(start_line, len(gen_lines) + 1):
        line = gen_lines[n - 1]
        if n > start_line and (line.startswith('DEFINE_REX_FUNC(') or line.startswith('}')):
            out[n] = cur
            break
        if line.lstrip().startswith('// '):
            idx += 1
            cur = func_addr + 4 * idx
        out[n] = cur
    return out


# PowerPC branches that do not test a condition: every other b* mnemonic (beq, bne, blt, bdnz, beqlr, bnectr, bc, ...) is a conditional branch
UNCONDITIONAL = {'b', 'ba', 'bl', 'bla', 'blr', 'blrl', 'bctr', 'bctrl'}


def conditional_lines(gen_lines, start_line):
    """The line numbers of one generated function body that belong to a PowerPC conditional branch (its `// mnemonic` comment, then its code
    lines). S111 EQUIVTIER: llvm-cov also counts branches the build's macros hold (the coverage preset's REX_FUNC_PROLOGUE stub seam, its
    REX_STORE_* checks) and codegen expressions (cntlzw's `== 0 ?`); none of those is a branch of the game function."""
    out, cond = set(), False
    for n in range(start_line + 1, len(gen_lines) + 1):
        line = gen_lines[n - 1]
        if line.startswith('DEFINE_REX_FUNC(') or line.startswith('}'):
            break
        s = line.strip()
        if s.startswith('// '):
            m = s[3:].split(' ', 1)[0].rstrip('+-')
            cond = m.startswith('b') and m not in UNCONDITIONAL
            continue
        if cond:
            out.add(n)
    return out


def unhit_arms(export, func_addr, gen_path, gen_lines, start_line):
    """[(address, arm)] of the game function's branches that never ran one way, from `llvm-cov export` JSON; None when the function has no
    counts in it (not instrumented). Only the branches on a PowerPC conditional branch's lines count (conditional_lines)."""
    want = f'__imp__sub_{func_addr:08X}'
    lines = body_addresses(gen_lines, start_line, func_addr)
    cond = conditional_lines(gen_lines, start_line)
    gen_norm = os.path.normcase(os.path.abspath(gen_path)).replace('\\', '/')
    for data in export.get('data', []):
        for fn in data.get('functions', []):
            if want not in fn.get('name', ''):
                continue
            files = [os.path.normcase(os.path.abspath(p)).replace('\\', '/') for p in fn.get('filenames', [])]
            arms, total = [], 0
            for br in fn.get('branches', []):
                ls, true_n, false_n, file_id = br[0], br[4], br[5], br[6]
                if file_id < len(files) and files[file_id] != gen_norm:
                    continue
                if ls not in lines or ls not in cond:
                    continue
                total += 1
                if true_n == 0:
                    arms.append((lines[ls], 'taken'))
                if false_n == 0 and PLANT != 'coverage_ignores_false_arm':
                    arms.append((lines[ls], 'not taken'))
            return arms, total
    return None


def coverage(exe, profraws, out_dir, func_addr, gen_path, start_line):
    prof = os.path.join(out_dir, 'merged.profdata')
    r = subprocess.run([os.path.join(LLVM, 'llvm-profdata.exe'), 'merge', '-o', prof] + profraws, capture_output=True, text=True)
    if r.returncode:
        return None, f'llvm-profdata merge failed: {r.stderr[-300:]}'
    r = subprocess.run([os.path.join(LLVM, 'llvm-cov.exe'), 'export', exe, f'-instr-profile={prof}', '-format=text', '-skip-expansions', gen_path],
                       capture_output=True, text=True, errors='replace')
    if r.returncode:
        return None, f'llvm-cov export failed: {r.stderr[-300:]}'
    with open(gen_path, encoding='utf-8', errors='replace') as f:
        gen_lines = f.read().splitlines()
    got = unhit_arms(json.loads(r.stdout), func_addr, gen_path, gen_lines, start_line)
    if got is None:
        return None, f'no branch counts for sub_{func_addr:08X} in {exe} (its generated file not built with covmap)'
    return got, None


# ---------------------------------------------------------------------------------------------------------------- reports and the verdict
def parse_report(text):
    rep = {'result': None, 'equal': 0, 'why': '', 'shrunk': '', 'case': None, 'outs': [], 'pins_unknown': []}
    for line in text.splitlines():
        k, _, v = line.partition(' ')
        if k == 'result':
            rep['result'] = v.strip()
        elif k == 'equal':
            rep['equal'] = int(v)
        elif k == 'why':
            rep['why'] = v
        elif k == 'shrunk':
            rep['shrunk'] = v.strip()
        elif k == 'case':
            rep['case'] = int(v)
        elif k == 'out':
            rep['outs'].append(v)
        elif k == 'pin-unknown':
            rep['pins_unknown'].append(v)
    return rep


def verdict(test, reports, arms, cov_why, refusals=(), notes=()):
    """The one verdict line (and its detail lines). reports = the runs' parsed reports (first = unpinned); arms = (unhit [(addr, arm, why)],
    total branches) or None with cov_why."""
    if refusals:
        return 'NOT-ISOLABLE', [f'NOT-ISOLABLE {test}: {refusals[0]}'] + [f'  also: {r}' for r in refusals[1:]]
    for rep in reports:
        if rep['result'] == 'NOT-ISOLABLE':
            return 'NOT-ISOLABLE', [f'NOT-ISOLABLE {test}: {rep["why"]}']
        if rep['result'] not in ('EQUAL', 'DIFFER'):
            return 'NOT-ISOLABLE', [f'NOT-ISOLABLE {test}: the run gave no result ({rep["result"]})']
    for rep in reports:
        if rep['result'] == 'DIFFER':
            pins = rep.get('pins') or {}
            lines = [f'REFUTED {test}: shrunk {rep["shrunk"]}' + (f' (pinned {pins})' if pins else '')]
            lines += [f'  {o}' for o in rep['outs']]
            return 'REFUTED', lines
    n = sum(r['equal'] for r in reports)
    if PLANT == 'proven_ignores_coverage':
        return 'PROVEN', [f'PROVEN {test}: {n} cases equal']
    if arms is None:
        return 'AGREES-UNCOVERED', [f'AGREES-UNCOVERED {test}: {n} cases equal over {len(reports)} run(s); no branch counts: {cov_why}'] + list(notes)
    unhit, total = arms
    if not unhit and total > 0:
        return 'PROVEN', [f'PROVEN {test}: {n} cases equal over {len(reports)} run(s); every branch of the game function ran both ways ({total} branches)']
    if total == 0:
        return 'AGREES-UNCOVERED', [f'AGREES-UNCOVERED {test}: {n} cases equal; the branch counts list no branch of the function']
    lines = [f'AGREES-UNCOVERED {test}: {n} cases equal over {len(reports)} run(s); {len(unhit)} of {2 * total} branch arms never ran: '
             + ', '.join(f'0x{a:08X} {arm}' for a, arm, _ in unhit)]
    lines += [f'  0x{a:08X} {arm}: {why}' for a, arm, why in unhit]
    return 'AGREES-UNCOVERED', lines + list(notes)


def steer_loop(test, decomp_lines, func_addr, run_once, rounds=4, max_runs=24):
    """Runs the test unpinned, then re-runs with pins for each unhit branch arm until no new arm is hit (or `rounds` / `max_runs`).
    run_once(pins) -> (report dict, arms or None, cov_why). Returns (reports, arms with reasons, cov_why)."""
    br, calls = branches(decomp_lines)
    by_off = {b.off: b for b in br}
    reports, tried = [], set()
    rep, arms, why = run_once({})
    rep['pins'] = {}
    reports.append(rep)
    if arms is None or rep['result'] != 'EQUAL':
        return reports, (None if arms is None else ([(a, arm, '') for a, arm in arms[0]], arms[1])), why
    unhit, total = set(arms[0]), arms[1]
    for _ in range(rounds):
        progress = False
        for a, arm in sorted(unhit):
            b = by_off.get(a - func_addr)
            if not b:
                continue
            pins, _why = steer(b, test, calls)
            for p in pins or []:
                key = tuple(sorted(p.items()))
                if key in tried or len(reports) >= max_runs:
                    continue
                tried.add(key)
                rep, got, why = run_once(p)
                rep['pins'] = p
                reports.append(rep)
                if rep['result'] != 'EQUAL':
                    return reports, None, why
                if got is not None:
                    new = unhit & set(got[0])
                    if new != unhit:
                        progress = True
                    unhit = new
        if not progress:
            break
    out = []
    for a, arm in sorted(unhit):
        b = by_off.get(a - func_addr)
        if not b:
            out.append((a, arm, 'no q decomp row at that offset'))
            continue
        pins, w = steer(b, test, calls)
        if not pins:
            out.append((a, arm, f'not steerable: {w}'))
            continue
        ran = sum(tuple(sorted(p.items())) in tried for p in pins)
        out.append((a, arm, f'steered ({ran} of {len(pins)} pin sets run) but never ran: {b.cond}' if ran
                    else f'steerable ({len(pins)} pin sets), not run (--rounds / --max-runs): {b.cond}'))
    return reports, (out, total), why


# ---------------------------------------------------------------------------------------------------------------- the plan
def plan(test):
    tree = q_json('callees', fname(test.target), '--json', '--depth', '40')
    texts = src_texts()
    hooked = hooked_functions(texts)
    # S111 EQUIVHOOK: a hook listed in tools\equiv\hook_passthrough.py whose entry holds against the live hook text runs only the original
    # under its flags, so it does not refuse; the run passes those flags. Any other hook is refused as before.
    pass_ok, pass_bad = hook_passthrough.allowed(hooked, texts)
    hooked_eff = {f: p for f, p in hooked.items() if f not in pass_ok}
    if fname(test.target) in hooked_eff:
        hooked_self = [f'{fname(test.target)} itself is hooked by us ({hooked[fname(test.target)]}): the test would call our hook, not the game'
                       + ''.join(f'; its table entry fails: {w}' for w in pass_bad.get(fname(test.target), []))]
    else:
        hooked_self = []
    funcs, stubbed, refusals = closure(tree, test.stubs, hooked_eff)
    reached = {f: pass_ok[f] for f in funcs if f in pass_ok}
    flags, clash = hook_passthrough.merge_flags(reached)
    if clash:
        refusals.append(f'pass-through hooks in the closure need different flags: {clash}')
    d = q_json('decomp', fname(test.target), '--json')
    return {'tree': tree, 'funcs': funcs, 'stubbed': stubbed, 'refusals': hooked_self + refusals, 'decomp': d,
            'tier': 'coverage (stubs)' if test.stubs else 'static', 'flags': flags, 'passthrough': sorted(reached)}


def field_report(test, d):
    covered, other = [], []
    names = {f.name for f in test.fields}
    for r in d['summary'].get('reads', []):
        nm = '[' + r['addr'] + ']'
        if nm in names:
            covered.append(nm)
        elif '@' in r['addr'] or 'ret#' in r['addr']:
            other.append(f'{nm} (derived: a pointer the function computed)')
        else:
            other.append(f'{nm} (no field of that name: an image word, or reached through a field\'s pointer)')
    return covered, other


def write_allow(test, funcs):
    os.makedirs(EQUIV_DIR, exist_ok=True)
    path = os.path.join(EQUIV_DIR, f'{test.name}.allow')
    with open(path, 'w') as f:
        f.write(f'# {test.name}: the static closure of {fname(test.target)} (tools\\equiv\\equiv.py; one guest address per line)\n')
        for fn in funcs:
            m = re.match(r'^sub_([0-9A-F]{8})$', fn)
            if m:
                f.write(f'0x{m.group(1)}\n')
    return path


def cmd_plan(test, a):
    p = plan(test)
    print(f'test {test.name}: {test.what}')
    print(f'target {fname(test.target)}  tier {p["tier"]}  closure {len(p["funcs"])} functions  stubbed {", ".join(p["stubbed"]) or "-"}')
    print('closure: ' + ' '.join(p['funcs']))
    if p['passthrough']:
        print(f'pass-through hooks of ours in the closure (tools\\equiv\\hook_passthrough.py): {" ".join(p["passthrough"])}; run flags: '
              + ' '.join(f'--{k}={v}' for k, v in p['flags'].items()))
    for r in p['refusals']:
        print(f'REFUSED: {r}')
    if not p['refusals']:
        print(f'allowlist: {write_allow(test, p["funcs"])}')
    cov, other = field_report(test, p['decomp'])
    print(f'input fields read by the target ({len(cov)} with a test field): {" ".join(cov) or "-"}')
    for o in other:
        print(f'  read without a field: {o}')
    br, calls = branches(p['decomp']['lines'])
    print(f'branches: {len(br)}')
    for b in br:
        pins, why = steer(b, test, calls)
        print(f'  +{b.off:X}  {b.cond}  ->  ' + (f'steer {pins}' if pins else f'not steerable: {why}'))
    return 1 if p['refusals'] else 0


# ---------------------------------------------------------------------------------------------------------------- running
def cmd_run(test, a):
    p = plan(test)
    if p['refusals']:
        v, lines = verdict(test.name, [], None, '', p['refusals'])
        print('\n'.join(lines))
        return 1
    allow = write_allow(test, p['funcs'])
    d = p['decomp']
    m = re.match(r'^(wwe2k14_recomp\.(\d+)\.cpp):(\d+)$', d['listing'])
    gen = os.path.join(generated_of(a.exe), m.group(1)) if m else ''
    start = int(m.group(3)) if m else 0
    if gen and os.path.exists(gen):
        with open(gen, encoding='utf-8', errors='replace') as f:
            head = next((ln for i, ln in enumerate(f, 1) if i == start), '')
        if not head.startswith(f'DEFINE_REX_FUNC({fname(test.target)})'):
            raise SystemExit(f'equiv: {gen}:{start} is not {fname(test.target)}\'s DEFINE_REX_FUNC (q decomp read another generated copy)')
    work = os.path.join(EQUIV_DIR, f'{test.name}.runs')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    profraws = []

    runs = []

    def run_once(pins):
        env = dict(os.environ)
        env.pop('WWE2K14_EQUIV_PINS', None)
        if pins:
            env['WWE2K14_EQUIV_PINS'] = ';'.join(f'{k}={v!r}' for k, v in pins.items())
        # every run's report, branch counts (beside it, the exe's <out>.profraw) and log in this test's fresh folder: the exe's default report
        # folder depends on where the exe lives (<exe>\..\..\harness\equiv), and a report left from an earlier run must never be read
        n = len(runs)
        txt, log = os.path.join(work, f'run{n}.txt'), os.path.join(work, f'run{n}.log')
        raw = os.path.splitext(txt)[0] + '.profraw'
        cmd = [a.exe, f'--wwe2k14_equiv={test.name}', f'--wwe2k14_equiv_allow={allow}', f'--wwe2k14_equiv_dll={a.dll}',
               f'--wwe2k14_equiv_cases={a.cases}', f'--wwe2k14_equiv_out={txt}', f'--log_file={log}'] + [f'--{k}={v}' for k, v in p['flags'].items()]
        env['LLVM_PROFILE_FILE'] = raw   # the profile runtime truncates its default file (default.profraw in the current folder) at start-up
        try:
            rc = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=a.timeout).returncode
        except subprocess.TimeoutExpired:
            raise SystemExit(f'equiv: {test.name} run {n} passed --timeout {a.timeout} s (log {log})')
        runs.append(rc)
        if rc == 9:   # REX_FATAL (the static tier's InvalidFunctionTrap: an indirect call outside the allowlist) aborted the process
            fatal = [ln.strip() for ln in open(log, encoding='utf-8', errors='replace')] if os.path.exists(log) else []
            fatal = [ln for ln in fatal if '[FATAL]' in ln]
            return {'result': 'NOT-ISOLABLE', 'equal': 0, 'why': f'the exe aborted (exit 9): {fatal[-1] if fatal else "no [FATAL] line in " + log}'}, None, ''
        if rc not in (0, 1, 2, 3) or not os.path.exists(txt):
            # 5 = set-up, 6 = the test DLL: the tool failed, which says nothing about the game function
            raise SystemExit(f'equiv: {test.name} run {n}: exe exit {rc}, {"report " + txt if os.path.exists(txt) else "no report"}; log {log}')
        rep = parse_report(open(txt, encoding='utf-8', errors='replace').read())
        if not os.path.exists(raw):
            return rep, None, f'{raw} not written (an exe without the profile runtime: build it with equiv.py cov-build)'
        profraws.append(raw)
        got, why = coverage(a.exe, profraws, work, test.target, gen, start)
        return rep, got, why

    reports, arms, why = steer_loop(test, d['lines'], test.target, run_once, a.rounds, a.max_runs)
    v, lines = verdict(test.name, reports, arms, why)
    print('\n'.join(lines))
    print(f'runs: {len(runs)} headless (exit codes {runs}); reports, logs and branch counts in {work}')
    return 0 if v == (a.expect or 'PROVEN') else 1


def closure_tus(funcs):
    nums = set()
    out = q('multi', *[f'decomp {f} --fields' for f in funcs])
    for m in re.finditer(r'listing generated/default/wwe2k14_recomp\.(\d+)\.cpp:\d+', out):
        nums.add(int(m.group(1)))
    return sorted(nums)


def generated_of(exe):
    """The generated folder whose lines an exe's branch counts map to: a covmap exe's variant.json names the copy its build compiled (a lane
    slot's own); an exe without it compiled the main checkout's."""
    p = os.path.join(os.path.dirname(os.path.abspath(exe)), 'variant.json')
    if os.path.exists(p):
        with open(p, encoding='utf-8') as f:
            return json.load(f)['generated']
    return os.path.join(MAIN, 'generated', 'default')


def cmd_cov_build(tests, a):
    """One covered exe for one or more tests (TEST[,TEST...]: the union of their closures' generated files, stubbed callees included)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # py -I does not put the script's folder on the path
    import equiv_covexe
    funcs = []
    for test in tests:
        p = plan(test)
        funcs += [f for f in p['funcs'] + p['stubbed'] if f not in funcs]
    return equiv_covexe.build(tests, closure_tus(funcs), REPO, a.name, a.dry)


def main(argv=None):
    global EQUIV_DIR
    ap = argparse.ArgumentParser(description='S111 EQUIV driver')
    ap.add_argument('cmd', choices=['list', 'plan', 'run', 'cov-build'])
    ap.add_argument('test', nargs='?')
    ap.add_argument('--dll', default=DLL)
    ap.add_argument('--exe', default='')
    ap.add_argument('--equiv-dir', default='')
    ap.add_argument('--cases', type=int, default=200)
    ap.add_argument('--rounds', type=int, default=4)
    ap.add_argument('--max-runs', type=int, default=24)
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--name', default='')
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--expect', default='', choices=['', 'PROVEN', 'AGREES-UNCOVERED', 'REFUTED', 'NOT-ISOLABLE'])
    a = ap.parse_args(argv)
    if a.equiv_dir:
        EQUIV_DIR = os.path.abspath(a.equiv_dir)
    tests = load_tests(a.dll)
    if a.cmd == 'list':
        for t in tests.values():
            print(f'{t.name:28} {fname(t.target)}  {len(t.fields)} fields  stubs {len(t.stubs)}  {t.what}')
        return 0
    names = (a.test or '').split(',') if a.cmd == 'cov-build' else [a.test]
    for nm in names:
        if nm not in tests:
            raise SystemExit(f'unknown test {nm!r}: {", ".join(tests)}')
    if a.cmd == 'cov-build':
        return cmd_cov_build([tests[nm] for nm in names], a)
    t = tests[a.test]
    if a.cmd == 'plan':
        return cmd_plan(t, a)
    if a.cmd == 'run':
        if not a.exe:
            raise SystemExit('run needs --exe (piece A\'s headless runner; for branch counts the covmap exe of equiv.py cov-build)')
        return cmd_run(t, a)
    return 2


if __name__ == '__main__':
    sys.exit(main())
