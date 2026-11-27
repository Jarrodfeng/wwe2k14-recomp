#!/usr/bin/env python3
"""start_parity_lint (S103 SPB; docs\\research\\s103\\SP2_start_parity.md section 8): a setting one start path applies must be applied by every start path that runs the same game.

Two builds, two start paths (the manifest tools\\scene\\start_paths.txt names every one): the stream build (ON: reader.h StartLive + live.cpp LiveSink::Start) and the stream-free build (OFF:
host_start.cpp BuildOptions + StartForGame). start_settings.cpp ApplyStartSettings counts for both. Text scan, no compile, about a second.

  py -I tools\\scene\\start_parity_lint.py --root .             scan the tree (exit 1 on an error)
  py -I tools\\scene\\start_parity_lint.py --selftest           the planted twins: each bad snippet MUST be flagged, each marked one must pass
  py -I tools\\scene\\start_parity_lint.py --root . --refind REV   the past-bug re-find: the start files of REV (git show) MUST report the known drift (presence globals, gpu_timestamps, present_fill)

Rules (each error is `file:line: Rn text`):
  R1 target parity   a global (`g_x = ..` / `g_x.store(..)` / `ns::g_x.member = ..`) stored in a start path of one build and not in a start path of the other. Exempt: the symbol is defined only
                     in a stream-only header / unit (outside the include closure of the units the stream-free build compiles), or the store carries `// START-ONLY(ON|OFF|pc|android): why`
                     (the line or the 1 above); `// NOT-READ-YET(OFF): why` marks an ON store the OFF build owes (counted as debt).
  R2 read parity    a cvar / SDK flag READ (REXCVAR_GET(x), GetFlagByName("x"), flag("x"), Query<T>("x")) in one build's start path and not the other's, where the cvar exists in the stream-free
                     build (defined in a kept unit, or an SDK flag). Same markers.
  R3 default drift  an R1 target initialised in a kept header (`inline std::atomic<bool> g_x{false}`) whose store takes REXCVAR_GET(c) with another default: the build that never stores it keeps the header value.
  R4 option fields  a `ro.field =` (rdr::RendererOptions) assigned in an ON path and not in an OFF path (or the reverse): same markers, on the assignment line.
  R6 dev gate       a target stored behind `dev &&` / IsDevBuild() in one build's path and unconditionally in the other's.
  R7 rig stores     a rig / tool (manifest `rig` rows) that stores to a start-path target needs `// RIG-ONLY: why`.
  R8 cvar copies    a cvar defined twice (the stream's definition and the stream-free copy) must have the same type and default.
  R9 manifest       a function named StartForGame / StartLive / LiveSink::Start in a start file with no manifest row.
Preprocessor: `#if !WWE2K14_STREAM()` regions count for OFF only, `#if WWE2K14_STREAM()` for ON only (the `#else` flips).
"""
import argparse
import fnmatch
import itertools
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
from read_unit import read_path, part_files  # a split file is read with its parts expanded (tools/split/read_unit.py)

BLOCK = re.compile(r'//\s*START-ONLY-(BEGIN|END)(?:\(([A-Za-z, ]+)\))?\s*:?\s*(.*)')
MARK = re.compile(r'//\s*(START-ONLY\(([A-Za-z, ]+)\)|NOT-READ-YET\((OFF)\)|RIG-ONLY)\s*:?\s*(.*)')
STORE = re.compile(r'\b((?:\w+::)*g_\w+(?:\.\w+)*)\s*(?:=(?![=])|\.store\s*\()')
OPT = re.compile(r'\bro\.(\w+)\s*=(?![=])')
GET = re.compile(r'REXCVAR_GET\s*\(\s*(\w+)\s*\)')
LIT = re.compile(r'"((?:wwe2k14_|gpu_|present_)\w+|vsync|resolution_scale|anisotropic_override)"')   # a whole string literal naming a flag: a read (or a Set) of it
FLAGSTR = re.compile(r'(?:GetFlagByName|GetFlagInfo|flag|flag_text|Query<\w+>|SetFlagByName|num|FlagText|FlagNum)\s*\(\s*"(\w+)"')
DEFINE = re.compile(r'REXCVAR_DEFINE_(\w+)\s*\(\s*(\w+)\s*,\s*([^,]+?)\s*,')
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
SHARD = re.compile(r'^src/cvars/cvars_\d+\.cpp$')   # S110 CVARSHARD: generated (tools\compile\cvar_shard.py); each group starts with `//@ header | namespace | condition`
SHARD_MARK = re.compile(r'^//@ [^|\n]*\| [^|\n]* \| (.*)$', re.M)


def shard_group_in_off(text, pos):
    """True when the shard group holding the definition at `pos` is compiled by the stream-free (OFF) build: its condition holds with WWE2K14_STREAM() = 0 for some value of the other defined() tests."""
    ms = list(SHARD_MARK.finditer(text, 0, pos))
    cond = ms[-1].group(1).strip() if ms else '-'
    if cond in ('-', ''):
        return True
    atoms = sorted(set(re.findall(r'defined\(\s*(\w+)\s*\)', cond)))[:6]
    for bits in itertools.product((0, 1), repeat=len(atoms)):
        e = cond.replace('WWE2K14_STREAM()', '0')
        for a, b in zip(atoms, bits):
            e = re.sub(r'defined\(\s*%s\s*\)' % re.escape(a), str(b), e)
        e = e.replace('&&', ' and ').replace('||', ' or ').replace('!', ' not ')
        if not re.fullmatch(r'[\d\s()a-z]*', e):
            return True   # a condition this does not model: judged kept (the old behaviour)
        try:
            if eval(e, {'__builtins__': {}}, {}):   # noqa: S307 (digits, parentheses, and / or / not only: checked above)
                return True
        except (SyntaxError, NameError):
            return True
    return False


def strip(text):
    """code text with comments and string / char literals blanked (newlines and columns kept)."""
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
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            if j < n and text[j] == c:   # closed on this line; an unterminated literal stops BEFORE its newline (a consumed newline shifted every later line number, so R7 markers were missed)
                j += 1
            seg = text[i:j]
            body = ''.join('\n' if ch == '\n' else ' ' for ch in seg[1:-1])
            out.append(c + body + c if len(seg) >= 2 and seg[-1] == c else ''.join('\n' if ch == '\n' else ' ' for ch in seg))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def markers(raw_lines):
    """line number (1-based) -> list of (kind, sides, why)."""
    out = {}
    open_block = None
    for no, ln in enumerate(raw_lines, 1):
        b = BLOCK.search(ln)
        if b:
            if b.group(1) == 'BEGIN':
                open_block = ([s.strip() for s in (b.group(2) or "ON").split(",")], b.group(3))
            else:
                open_block = None
            continue
        if open_block:
            out.setdefault(no, []).append(('SO', open_block[0], '__block__'))   # (the block covers its own lines only: the look-back in marked() skips these)
        m = MARK.search(ln)
        if m:
            kind = 'RIG' if m.group(1) == 'RIG-ONLY' else ('NRY' if m.group(3) else 'SO')
            sides = [s.strip() for s in (m.group(2) or 'ON').split(',')] if kind == 'SO' else ['ON']
            out.setdefault(no, []).append((kind, sides, m.group(4)))
    return out


def marked(mk, line, side):
    for kind, sides, _ in mk.get(line, []):   # the line itself (a marker comment on it, or inside a START-ONLY-BEGIN / END block)
        if kind in ('SO', 'NRY') and (side in sides or 'both' in sides):
            return kind
    for ln in range(max(1, line - 1), line):   # S103 SPBF L5: the ONE line above (a marker covers its own line and the next, nothing more)
        for kind, sides, why in mk.get(ln, []):
            if kind in ('SO', 'NRY') and (side in sides or 'both' in sides) and why != '__block__':
                return kind
    return None


def rig_marked(mk, line):
    return any(kind == 'RIG' for ln in range(max(1, line - 1), line + 1) for kind, _, _ in mk.get(ln, []))   # S103 SPBF L5: the line or the one above


def regions(text_raw, row):
    """[(first_line, last_line)] of every function matching the row's regexp (brace matched on the stripped code)."""
    code = strip(text_raw)
    out = []
    for m in re.finditer(row['regex'], code):
        j = code.find('{', m.end())
        if j < 0:
            continue
        depth, k = 0, j
        while k < len(code):
            if code[k] == '{':
                depth += 1
            elif code[k] == '}':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        out.append((code.count('\n', 0, m.start()) + 1, code.count('\n', 0, k) + 1))
    return out, code


def line_builds(code_lines, first, last, base):
    """per line: the set of builds active ({'ON','OFF'} subset), following #if !WWE2K14_STREAM() / #if WWE2K14_STREAM() / #else / #endif."""
    stack = []   # entries: [builds_or_None, flipped_builds_or_None]
    res = {}
    for no in range(first, last + 1):
        ln = code_lines[no - 1].strip()
        if ln.startswith('#'):
            d = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', ln)
            if d:
                kw, rest = d.group(1), d.group(2)
                if kw in ('if', 'ifdef', 'ifndef'):
                    if 'WWE2K14_STREAM()' in rest:
                        neg = bool(re.search(r'!\s*WWE2K14_STREAM\(\)', rest))
                        stack.append([{'OFF'} if neg else {'ON'}, {'ON'} if neg else {'OFF'}])
                    else:
                        stack.append([None, None])
                elif kw == 'else' and stack:
                    stack[-1] = [stack[-1][1], stack[-1][0]]
                elif kw == 'endif' and stack:
                    stack.pop()
        active = set(base)
        for b, _ in stack:
            if b is not None:
                active &= b
        res[no] = active
    return res


def load_manifest(path):
    rows, rigs = [], []
    for ln in open(path, encoding='utf-8'):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        p = [x.strip() for x in ln.split('|')]
        if p[0] == 'rig':
            rigs.append(p[1])
        else:
            rows.append({'name': p[0], 'file': p[1], 'regex': p[2], 'builds': set(x.strip() for x in p[3].split(',')), 'platform': p[4]})
    return rows, rigs


def collect(read, rows):
    """-> (stores, opts, reads, notes). Each: key -> list of dicts {file,line,builds,mk,text}."""
    stores, opts, reads, notes = {}, {}, {}, []
    for row in rows:
        raw = read(row['file'])
        if raw is None:
            notes.append('skipped manifest row %s: %s does not exist in this tree' % (row['name'], row['file']))
            continue
        rawl = raw.split('\n')
        mk = markers(rawl)
        regs, code = regions(raw, row)
        codel = code.split('\n')
        if not regs:
            notes.append('manifest row %s matches nothing in %s' % (row['name'], row['file']))   # check() turns this into an ERROR (S103 SPBF L5)
        for first, last in regs:
            lb = line_builds(codel, first, last, row['builds'])
            for no in range(first, last + 1):
                ln = codel[no - 1]
                builds = lb[no]
                if not builds:
                    continue
                stmt = ' '.join(codel[max(0, no - 3):no])
                rec = lambda: {'file': row['file'], 'line': no, 'builds': builds, 'mk': mk, 'text': ln, 'stmt': stmt, 'raw': rawl[no - 1]}
                for m in STORE.finditer(ln):
                    key = m.group(1)
                    stores.setdefault(key, []).append(dict(rec(), rhs=ln[m.end():]))
                for m in OPT.finditer(ln):
                    opts.setdefault(m.group(1), []).append(rec())
                for m in GET.finditer(ln):
                    reads.setdefault(m.group(1), []).append(rec())
                for m in LIT.finditer(rawl[no - 1].split('//')[0]):
                    reads.setdefault(m.group(1), []).append(rec())
    return stores, opts, reads, notes


def base_of(key):
    return re.match(r'(?:\w+::)*(g_\w+)', key).group(1)


def side_sets(occs):
    on = [o for o in occs if 'ON' in o['builds']]
    off = [o for o in occs if 'OFF' in o['builds']]
    return on, off


def check(read, rows, rigs, kept_defines, stream_only_sym, header_init, cvar_defaults, rig_files, cvar_defs_all, alias_targets=(), header_both=lambda sym: False):
    """returns (errors, debts, notes). Pure over its inputs (the selftest feeds in-memory ones)."""
    errors, debts = [], []
    stores, opts, reads, notes = collect(read, rows)
    for n in notes:
        if 'matches nothing' in n:   # S103 SPBF L5: a row that matches no function checks nothing: an error, not a note
            errors.append('tools/scene/start_paths.txt: R9 ' + n + ' (a renamed start function? fix the row)')

    def verdict(occs, side, key, rule, what):
        """an occurrence in `side`'s paths only: marked, or an error. Returns True when fine."""
        bad = False
        for o in occs:
            kind = marked(o['mk'], o['line'], side)
            if kind == 'NRY':
                debts.append('%s:%d: NOT-READ-YET(OFF) %s' % (o['file'], o['line'], key))
            elif not kind:
                errors.append('%s:%d: %s %s `%s` is set by the %s start only (the %s start path never applies it): fix it in start_settings.cpp / the other path, or mark the line `// START-ONLY(%s): why`'
                              % (o['file'], o['line'], rule, what, key, side, 'OFF' if side == 'ON' else 'ON', side))
                bad = True
        return not bad
    # R1
    for key, occs in sorted(stores.items()):
        on, off = side_sets(occs)
        if bool(on) == bool(off):
            continue
        if stream_only_sym(key):
            continue
        side = 'ON' if on else 'OFF'
        verdict(on or off, side, key, 'R1', 'target')
    # R6
    for key, occs in sorted(stores.items()):
        on, off = side_sets(occs)
        if on and off:
            gate = lambda os_: any(re.search(r'\bdev\b|IsDevBuild', o['stmt'] + o['rhs']) for o in os_)
            if gate(on) != gate(off):
                errors.append('%s:%d: R6 dev gate `%s` is stored behind a dev test in one build\'s start path and not in the other\'s' % (occs[0]['file'], occs[0]['line'], key))
    # R2
    for name, occs in sorted(reads.items()):
        on, off = side_sets(occs)
        if bool(on) == bool(off):
            continue
        if name in cvar_defs_all and not kept_defines(name):
            continue   # a stream-only cvar: it does not exist in the stream-free build (removed_switches.h covers kept readers)
        verdict(on or off, 'ON' if on else 'OFF', name, 'R2', 'read')
    # R4
    for field, occs in sorted(opts.items()):
        on, off = side_sets(occs)
        if bool(on) != bool(off):
            verdict(on or off, 'ON' if on else 'OFF', 'ro.' + field, 'R4', 'renderer option')
    # R3: the R1 targets the OFF build never stores, with a header default that differs from the cvar's
    for key, occs in sorted(stores.items()):
        on, off = side_sets(occs)
        if on and not off:
            for o in on:
                m = GET.search(o['rhs'])
                init = header_init(base_of(key))
                if m and init is not None and m.group(1) in cvar_defaults and cvar_defaults[m.group(1)] != init:
                    errors.append('%s:%d: R3 default drift: `%s` starts as %s in its header but --%s defaults to %s; the build that never stores it keeps %s' % (o['file'], o['line'], key, init, m.group(1), cvar_defaults[m.group(1)], init))
    # R10 (S103 RGP review F1): a store only the stream-free start makes (START-ONLY(OFF)) whose header default is a plain true / false equal to the cvar's default: the stream build keeps that value with no switch.
    #      The header must differ by build (`{!WWE2K14_STREAM()}`) or carry `// BOTH-BUILDS: why` on its line.
    for key, occs in sorted(stores.items()):
        on, off = side_sets(occs)
        if off and not on:
            for o in off:
                if marked(o['mk'], o['line'], 'OFF') != 'SO':
                    continue
                m = GET.search(o['rhs'])
                init = header_init(base_of(key))
                if m and init in ('true', 'false') and cvar_defaults.get(m.group(1)) == init and not header_both(base_of(key)):
                    errors.append('%s:%d: R10 `%s` is stored by the stream-free start only but its header default (%s) is a plain literal equal to the cvar default: the stream build runs it with no switch. Make the header `{!WWE2K14_STREAM()}` or mark the header line `// BOTH-BUILDS: why`' % (o['file'], o['line'], key, init))
    # R7: rigs
    def two(k):
        return '::'.join(k.split('.')[0].split('::')[-2:])
    targets = {two(k) for k in stores if not stream_only_sym(k)}
    alias_bases = set(alias_targets)
    for rel, text in rig_files:
        rawl = text.split('\n')
        mk = markers(rawl)
        code = strip(text).split('\n')
        for no, ln in enumerate(code, 1):
            for m in STORE.finditer(ln):
                if (two(m.group(1)) in targets or base_of(m.group(1)) in alias_bases) and not rig_marked(mk, no):
                    errors.append('%s:%d: R7 rig store `%s` is a start-path target; mark it `// RIG-ONLY: why` (a rig that sets what the game start sets hides a start path that forgot it)' % (rel, no, m.group(1)))
    return errors, debts, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--refind', default='')
    ap.add_argument('--manifest', default='')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    root = os.path.abspath(a.root)
    rows, rigs = load_manifest(a.manifest or os.path.join(root, 'tools', 'scene', 'start_paths.txt'))

    def read_tree(rel):
        p = os.path.join(root, rel)
        return read_path(p) if os.path.isfile(p) else None

    def read_rev(rel):
        r = subprocess.run(['git', '-C', root, 'show', '%s:%s' % (a.refind, rel)], capture_output=True, text=True, encoding='utf-8', errors='replace')
        return r.stdout.replace('\r\n', '\n') if r.returncode == 0 else None
    read = read_rev if a.refind else read_tree

    # the tree-wide facts (kept closure, definitions) always come from the working tree: kept-ness of a symbol does not change between the revs compared
    src = {}
    parts = part_files(os.path.join(root, 'src'))   # a split parent is read with its parts expanded; its part files are not read again
    for d, _, fs in os.walk(os.path.join(root, 'src')):
        for f in fs:
            if f.endswith(('.h', '.cpp', '.inc')):
                p = os.path.join(d, f)
                if Path(p).resolve() in parts:
                    continue
                src[os.path.relpath(p, root).replace('\\', '/')] = read_path(p)
    cm = open(os.path.join(root, 'CMakeLists.txt'), encoding='utf-8').read()
    blk = re.search(r'set\(WWE2K14_SOURCES(.*?)\n\s*\)', cm, re.S)
    units = set(re.findall(r'^\s*(src/\S+?\.cpp)\s*(?:#.*)?$', blk.group(1) if blk else '', re.M))

    def listed(name):
        return {l.strip() for l in open(os.path.join(root, 'tools', 'scene', name), encoding='utf-8') if l.strip().startswith('src/')}
    units = (units - listed('keep_stream_sources.txt')) | listed('keep_stream_off_sources.txt')
    stream_headers = {'src/' + l.strip() for l in open(os.path.join(root, 'tools', 'scene', 'keep_stream_headers.txt'), encoding='utf-8') if l.strip() and not l.startswith('#')}

    def includes_off(text):
        """the quoted includes the stream-free build sees: not those under `#if WWE2K14_STREAM()` (the ON half), not the stream headers themselves."""
        out, stack = [], []
        for ln in text.split(chr(10)):
            s = ln.strip()
            d = re.match(r'#\s*(if|ifdef|ifndef|else|endif|elif)\b(.*)', s)
            if d:
                kw, rest = d.group(1), d.group(2)
                if kw in ('if', 'ifdef', 'ifndef'):
                    if 'WWE2K14_STREAM()' in rest:
                        stack.append('off' if re.search(r'!\s*WWE2K14_STREAM\(\)', rest) else 'on')
                    else:
                        stack.append(None)
                elif kw == 'else' and stack:
                    stack[-1] = {'on': 'off', 'off': 'on', None: None}[stack[-1]]
                elif kw == 'endif' and stack:
                    stack.pop()
                continue
            if 'on' in stack:
                continue
            m = re.match(r'#\s*include\s*"([^"]+)"', s)
            if m:
                out.append(m.group(1))
        return out
    closure, todo = set(), [u for u in units if u in src]
    while todo:
        f = todo.pop()
        if f in closure:
            continue
        closure.add(f)
        for inc in includes_off(src.get(f, '')):
            for cand in (os.path.normpath(os.path.join(os.path.dirname(f), inc)).replace('\\', '/'), 'src/' + inc):
                if cand in src and cand not in closure:
                    if cand not in stream_headers:
                        todo.append(cand)
                    break
    defs = {}
    defaults = {}
    r8_exempt = set()
    for rel, text in src.items():
        for m in DEFINE.finditer(text):
            # S110 CVARSHARD: the definitions sit in src\cvars\cvars_<n>.cpp, each group under the condition its header was included under: a group the stream-free build does not compile is not "defined" there
            defs.setdefault(m.group(2), []).append(rel if not SHARD.match(rel) or shard_group_in_off(text, m.start()) else rel + '#stream')
            defaults.setdefault(m.group(2), []).append((rel, m.group(1), m.group(3).strip()))
            eol = text.find('\n', m.end())
            if 'R8-OFF-DEFAULT' in text[m.start():len(text) if eol < 0 else eol]:   # S103 UPL: a stream-free copy that ships another default on purpose (the marker names why, on the definition's last line)
                r8_exempt.add(m.group(2))

    def kept_defines(name):
        return any(f in closure for f in defs.get(name, []))
    decl = re.compile(r'^[^\n/]*\binline\b[^;{}()\n]*?\b(g_\w+)\s*(=\s*(?:true|false)\b|\{\s*(?:true|false)\b|=|\{|;|\[)', re.M)
    sym_both = set()
    sym_files = {}   # one pass over src: g_name -> {file: initial literal or None}   (`inline T& g_x = other::g_quality.field` counts as this file's)
    for rel, text in src.items():
        if 'inline' not in text or 'g_' not in text:
            continue
        for m in decl.finditer(text):
            lit = re.search(r'\b(true|false)\b', m.group(2))
            sym_files.setdefault(m.group(1), {}).setdefault(rel, lit.group(1) if lit else None)
            eol = text.find(chr(10), m.end())
            if 'BOTH-BUILDS' in text[text.rfind(chr(10), 0, m.start()) + 1:eol if eol > 0 else len(text)]:
                sym_both.add(m.group(1))
            for extra in re.finditer(r',\s*(g_\w+)\s*(?:=|\{|;|,|\[)', text[m.end():eol if eol > 0 else len(text)]):   # `inline bool g_a = false, g_b = false;`
                sym_files.setdefault(extra.group(1), {}).setdefault(rel, None)

    def defined_in(sym):
        return list(sym_files.get(sym, {}))

    def stream_only_sym(key):
        sym = base_of(key)
        files = defined_in(sym)
        quals = key.split('::')[:-1]
        if quals and len(files) > 1:   # `ns::g_x`: the definition that sits in `namespace ns`
            narrowed = [f for f in files if re.search(r'namespace\s+(?:[\w:]*::)?' + re.escape(quals[-1]) + r'\b', src[f])]
            files = narrowed or files
        if not files:
            return False   # unknown definition: judged like a kept one (an error unless marked) - never silently exempt
        return all(f not in closure for f in files)

    def header_init(sym):
        for rel, init in sym_files.get(sym, {}).items():
            if init:
                return init
        return None
    cvar_defaults = {}
    for name, lst in defaults.items():
        cvar_defaults[name] = lst[0][2]
    rig_files = []
    for pat in rigs:
        d = os.path.join(root, os.path.dirname(pat))
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            rel = os.path.dirname(pat) + '/' + f
            if fnmatch.fnmatch(rel, pat) and os.path.isfile(os.path.join(root, rel)):
                rig_files.append((rel, open(os.path.join(root, rel), encoding='utf-8', errors='replace', newline='').read().replace('\r\n', '\n')))
    aliases = set()
    for text in src.values():   # `inline T& g_x = wwe2k14::start::g_quality.field`: the binder's old globals, now PlayerQuality (a rig that sets one sets the player's quality)
        aliases.update(re.findall(r'inline\s+[^;=(]*?&\s*(g_\w+)\s*=\s*wwe2k14::start::', text))
    errors, debts, notes = check(read, rows, rigs, kept_defines, stream_only_sym, header_init, cvar_defaults, rig_files, set(defs), aliases, header_both=lambda sym: sym in sym_both)
    # R8: a cvar defined twice
    for name, lst in sorted(defaults.items()):
        if name not in r8_exempt and len(lst) > 1 and any(f == 'src/start_settings.cpp' for f, _, _ in lst) and len({(t, d) for _, t, d in lst}) > 1:
            errors.append('%s: R8 cvar `%s` is defined twice with different type / default: %s' % (lst[0][0], name, '; '.join('%s %s %s' % x for x in lst)))
    # R9: a start function with no manifest row
    for rel in ('src/renderer_adapter/live.cpp', 'src/renderer_adapter/reader.h', 'src/renderer_host/host_start.cpp'):
        text = read_tree(rel) if not a.refind else read(rel)
        if not text:
            continue
        covered = {r['file'] for r in rows}
        for m in re.finditer(r'^\s*(?:inline\s+)?(?:bool|void)\s+((?:\w+::)?(?:StartForGame|StartLive|Start))\s*\(', strip(text), re.M):
            if rel not in covered:
                errors.append('%s: R9 `%s` is a start function and %s has no row in tools\\scene\\start_paths.txt' % (rel, m.group(1), rel))
    for n in notes:
        print('note: ' + n)
    for d in debts:
        print('debt: ' + d)
    if a.refind:
        got = ' '.join(errors)
        expect = ['presence::g_crowd', 'presence::g_particles', 'presence::g_crowd_screen_trust', 'ro.gpu_timestamps', 'ro.present_fill']
        for e in errors:
            print(e)
        miss = [x for x in expect if x + '`' not in got]   # (a key is reported fully qualified: rdr_scene::live::presence::g_crowd)
        print('REFIND %s: %d findings; expected %s; %s' % (a.refind, len(errors), ', '.join(expect), ('NOT reported: ' + ', '.join(miss)) if miss else 'all reported'))
        return 1 if miss else 0
    for e in errors:
        print(e)
    print('%s: %d errors, %d NOT-READ-YET debts' % ('FAIL' if errors else 'PASS', len(errors), len(debts)))
    return 1 if errors else 0


def selftest():
    kept = {'g_plant_a': 'false'}
    files = {
        'on.cpp': 'void PlantOn() {\n  g_plant_a.store(REXCVAR_GET(plant_cv));\n  ro.plant_field = 1;\n  ro.shared = 1;\n}\n',
        'off.cpp': 'void PlantOff() {\n  ro.shared = 1;\n}\n',
    }
    marked_on = 'void PlantOn() {\n  // START-ONLY(ON): the plant says why\n  g_plant_a.store(REXCVAR_GET(plant_cv));\n  // START-ONLY(ON): second reason\n  ro.plant_field = 1;\n  ro.shared = 1;\n}\n'
    rows = [{'name': 'on', 'file': 'on.cpp', 'regex': 'void PlantOn', 'builds': {'ON'}, 'platform': 'both'},
            {'name': 'off', 'file': 'off.cpp', 'regex': 'void PlantOff', 'builds': {'OFF'}, 'platform': 'both'}]
    args = dict(rigs=[], kept_defines=lambda n: True, stream_only_sym=lambda s: False, header_init=lambda s: kept.get(s), cvar_defaults={'plant_cv': 'true'}, cvar_defs_all={'plant_cv'})
    bad = []

    def run(fs, rig=None):
        return check(lambda rel: fs.get(rel), rows, rig_files=rig or [], **args)[0]

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            bad.append(what)
    e = run(files)
    need(any('R1' in x and 'g_plant_a' in x for x in e), '(a) a store the OFF start forgot is reported (R1)')
    need(any('R3' in x and 'g_plant_a' in x for x in e), '(a) its header default drift (false vs the cvar\'s true) is reported (R3)')
    need(any('R4' in x and 'plant_field' in x for x in e), '(c) a renderer option field missing in the OFF start is reported (R4)')
    need(not any('ro.shared' in x or 'shared' in x for x in e), 'a field both starts set is not reported')
    e = run(dict(files, **{'on.cpp': marked_on}))
    need(not [x for x in e if 'R1' in x or 'R4' in x], '(b) the same stores with START-ONLY(ON) markers pass')
    e = run(files, rig=[('rig.cpp', 'void R() {\n  g_plant_a.store(true);\n}\n')])
    need(any('R7' in x and 'rig.cpp' in x for x in e), '(d) an unmarked rig store of a start-path target is reported (R7)')
    e = run(dict(files, **{'on.cpp': marked_on}), rig=[('rig.cpp', 'void R() {\n  // RIG-ONLY: the rig has no start\n  g_plant_a.store(true);\n}\n')])
    need(not [x for x in e if 'R7' in x], '(d) a marked rig store passes')
    off_gate = {'off.cpp': 'void PlantOff() {\n  g_plant_a.store(dev && REXCVAR_GET(plant_cv));\n  ro.shared = 1;\n}\n'}
    e = run(dict(files, **off_gate))
    need(any('R6' in x for x in e), '(e) a dev gate in one build\'s path and none in the other\'s is reported (R6)')
    e = run(dict(files, **{'off.cpp': 'void PlantOff() {\n  g_plant_a.store(REXCVAR_GET(plant_cv));\n  ro.shared = 1;\n  ro.plant_field = 1;\n}\n'}))
    need(not [x for x in e if 'R1' in x or 'R4' in x], '(f) a store both starts make is not reported')
    # S103 SPBF L5 twins
    blk = 'void PlantOn() {\n  // START-ONLY-BEGIN(ON): a block\n  g_plant_a.store(REXCVAR_GET(plant_cv));\n  // START-ONLY-END\n  ro.plant_field = 1;\n  ro.shared = 1;\n}\n'
    e = run(dict(files, **{'on.cpp': blk}))
    need(any('R1' in x and 'g_plant_a' in x for x in e) is False and any('R4' in x and 'plant_field' in x for x in e),
         '(g) a START-ONLY block covers its own lines only: the line right after END is reported (R4 plant_field), the block store passes')
    two_above = 'void PlantOn() {\n  // START-ONLY(ON): one line covers the next line only\n  g_plant_a.store(REXCVAR_GET(plant_cv));\n  int pad = 0;\n  ro.plant_field = 1;\n  ro.shared = 1;\n}\n'
    e = run(dict(files, **{'on.cpp': two_above}))
    need(any('R4' in x and 'plant_field' in x for x in e) and not any('R1' in x for x in e), '(i) a marker covers its own line and the next, not three lines on: the field two lines below is reported')
    rows_bad = rows + [{'name': 'ghost', 'file': 'off.cpp', 'regex': 'void RenamedAway', 'builds': {'OFF'}, 'platform': 'both'}]
    e = check(lambda rel: dict(files).get(rel), rows_bad, rig_files=[], **args)[0]
    need(any('matches nothing' in x for x in e), '(h) a manifest row that matches no function is an ERROR')
    r10_files = {'on.cpp': 'void PlantOn() {\n  ro.shared = 1;\n}\n', 'off.cpp': 'void PlantOff() {\n  // START-ONLY(OFF): stream-free only\n  g_plant_b.store(REXCVAR_GET(plant_cv));\n  ro.shared = 1;\n}\n'}
    r10_args = dict(args, header_init=lambda s: 'true', cvar_defaults={'plant_cv': 'true'})
    e = check(lambda rel: r10_files.get(rel), rows, rig_files=[], **r10_args)[0]
    need(any('R10' in x and 'g_plant_b' in x for x in e), '(j) a START-ONLY(OFF) store whose header default is a plain literal equal to the cvar default is reported (R10; the g_ring_gpu_skin line before its fix)')
    e = check(lambda rel: r10_files.get(rel), rows, rig_files=[], header_both=lambda s: True, **r10_args)[0]
    need(not any('R10' in x for x in e), '(k) the same with a BOTH-BUILDS header marker passes')
    e = check(lambda rel: r10_files.get(rel), rows, rig_files=[], **dict(r10_args, header_init=lambda s: None))[0]
    need(not any('R10' in x for x in e), '(l) a header that is not a plain literal ({!WWE2K14_STREAM()}) passes')
    print('FAIL: %d' % len(bad) if bad else 'PASS: every planted twin behaved')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
