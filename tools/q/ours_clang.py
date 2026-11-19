"""libclang (ctypes) walker for `q ours` (tools/q/build_ours.py runs it per translation unit, in worker processes).

Parses ONE translation unit with the flags of a compile database and returns plain rows about OUR code only (src\\, tools\\,
probes\\ under the repo root): symbol definitions, references (calls, reads, writes, plain references) with the function they
sit in, the function extents, and the repo files the unit includes. No game code is involved: the index covers our sources.

Read / write is decided from the AST context of a reference to a global / static / field: the left side of =, op=, ++ / --,
the object of an atomic store / exchange / fetch_* / compare_exchange*, of a container mutator (push_back, clear, ...) or of
operator= is a write; everything else a read. Known limits: a variable passed by non-const reference or pointer to a function
that writes it shows as 'addr' (address / reference taken), not 'write'.
"""
import ctypes as C
import os
import re
import sys

LIBCLANG = r'C:\Program Files\LLVM\bin\libclang.dll'


class CXCursor(C.Structure):
    _fields_ = [('kind', C.c_int), ('xdata', C.c_int), ('data', C.c_void_p * 3)]


class CXSourceLocation(C.Structure):
    _fields_ = [('ptr', C.c_void_p * 2), ('int_data', C.c_uint)]


class CXSourceRange(C.Structure):
    _fields_ = [('ptr', C.c_void_p * 2), ('begin', C.c_uint), ('end', C.c_uint)]


class CXString(C.Structure):
    _fields_ = [('data', C.c_void_p), ('flags', C.c_uint)]


VISITOR = C.CFUNCTYPE(C.c_int, CXCursor, CXCursor, C.c_void_p)
INCL = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p, C.c_uint, C.c_void_p)

# CXCursorKind
K_STRUCT, K_UNION, K_CLASS, K_ENUM, K_FIELD, K_ENUMCONST, K_FUNC, K_VAR, K_PARM = 2, 3, 4, 5, 6, 7, 8, 9, 10
K_TYPEDEF, K_CXXMETHOD, K_NAMESPACE, K_LINKAGE, K_CTOR, K_DTOR, K_CONV = 20, 21, 22, 23, 24, 25, 26
K_FUNCTMPL, K_CLASSTMPL, K_TYPEREF, K_TEMPLATEREF = 30, 31, 43, 45
K_UNEXPOSED_EXPR, K_DECLREF, K_MEMBERREF, K_CALL, K_PAREN, K_UNARY, K_SUBSCRIPT, K_BINARY, K_COMPOUNDASSIGN = 100, 101, 102, 103, 111, 112, 113, 114, 115
K_CONDOP, K_CSTYLECAST, K_INITLIST = 116, 117, 119
K_CASESTMT = 203
K_TU = 350
K_CASTS = {117, 124, 125, 126, 127, 128}
FUNC_KINDS = {K_FUNC, K_CXXMETHOD, K_CTOR, K_DTOR, K_CONV, K_FUNCTMPL}
TYPE_KINDS = {K_STRUCT, K_UNION, K_CLASS, K_ENUM, K_CLASSTMPL}
SCOPE_PARENTS = {K_NAMESPACE, K_TU, K_CLASS, K_STRUCT, K_UNION, K_CLASSTMPL, K_LINKAGE}
# CXBinaryOperatorKind / CXUnaryOperatorKind (clang-c/Index.h, LLVM 17+; verified by build_ours.py --selftest)
BIN_ASSIGN, BIN_COMPOUND = 22, set(range(23, 33))
BIN_CMPISH = {10, 11, 12, 13, 14, 15, 16, 8, 9, 17, 18, 19}      # <=>, < > <= >= == !=, << >>, & ^ |
UN_INCDEC, UN_ADDR = {1, 2, 3, 4}, 5
WRITE_METHODS = re.compile(r'^(store|exchange|fetch_\w+|compare_exchange_\w+|test_and_set|clear|push_back|emplace\w*|insert\w*|'
                           r'erase|resize|assign|reset|swap|pop_back|pop_front|push_front|fill|set\w*)$')
ASSIGN_OPS = {'operator=': 'w', 'operator+=': 'rw', 'operator-=': 'rw', 'operator*=': 'rw', 'operator/=': 'rw', 'operator|=': 'rw',
              'operator&=': 'rw', 'operator^=': 'rw', 'operator<<=': 'rw', 'operator>>=': 'rw', 'operator%=': 'rw',
              'operator++': 'rw', 'operator--': 'rw'}
MODEMAP = {'r': 'read', 'w': 'write', 'rw': 'rw', 'cmp': 'read', 'addr': 'addr', 'c': 'read', 'cw': 'read', 'skip': 'read'}
PASS_THROUGH = {K_UNEXPOSED_EXPR, K_PAREN} | K_CASTS

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(LIBCLANG)
        L.clang_createIndex.restype = C.c_void_p
        L.clang_parseTranslationUnit2.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(C.c_char_p), C.c_int, C.c_void_p, C.c_uint,
                                                   C.c_uint, C.POINTER(C.c_void_p)]
        L.clang_getTranslationUnitCursor.argtypes = [C.c_void_p]
        L.clang_getTranslationUnitCursor.restype = CXCursor
        L.clang_visitChildren.argtypes = [CXCursor, VISITOR, C.c_void_p]
        L.clang_getCursorKind.argtypes = [CXCursor]
        L.clang_getCursorLocation.argtypes = [CXCursor]
        L.clang_getCursorLocation.restype = CXSourceLocation
        L.clang_getExpansionLocation.argtypes = [CXSourceLocation, C.POINTER(C.c_void_p), C.POINTER(C.c_uint), C.POINTER(C.c_uint),
                                                  C.POINTER(C.c_uint)]
        L.clang_getFileName.argtypes = [C.c_void_p]
        L.clang_getFileName.restype = CXString
        L.clang_getCString.argtypes = [CXString]
        L.clang_getCString.restype = C.c_char_p
        L.clang_disposeString.argtypes = [CXString]
        L.clang_getCursorUSR.argtypes = [CXCursor]
        L.clang_getCursorUSR.restype = CXString
        L.clang_getCursorSpelling.argtypes = [CXCursor]
        L.clang_getCursorSpelling.restype = CXString
        L.clang_getCursorReferenced.argtypes = [CXCursor]
        L.clang_getCursorReferenced.restype = CXCursor
        L.clang_getCursorSemanticParent.argtypes = [CXCursor]
        L.clang_getCursorSemanticParent.restype = CXCursor
        L.clang_isCursorDefinition.argtypes = [CXCursor]
        L.clang_getCursorBinaryOperatorKind.argtypes = [CXCursor]
        L.clang_getCursorUnaryOperatorKind.argtypes = [CXCursor]
        L.clang_getCursorExtent.argtypes = [CXCursor]
        L.clang_getCursorExtent.restype = CXSourceRange
        L.clang_getRangeEnd.argtypes = [CXSourceRange]
        L.clang_getRangeEnd.restype = CXSourceLocation
        L.clang_hashCursor.argtypes = [CXCursor]
        L.clang_Cursor_isNull.argtypes = [CXCursor]
        L.clang_Cursor_getStorageClass.argtypes = [CXCursor]
        L.clang_getInclusions.argtypes = [C.c_void_p, INCL, C.c_void_p]
        L.clang_disposeTranslationUnit.argtypes = [C.c_void_p]
        L.clang_getNumDiagnostics.argtypes = [C.c_void_p]
        L.clang_getDiagnostic.argtypes = [C.c_void_p, C.c_uint]
        L.clang_getDiagnostic.restype = C.c_void_p
        L.clang_getDiagnosticSeverity.argtypes = [C.c_void_p]
        L.clang_formatDiagnostic.argtypes = [C.c_void_p, C.c_uint]
        L.clang_formatDiagnostic.restype = CXString
        L.clang_disposeDiagnostic.argtypes = [C.c_void_p]
        _lib = L
    return _lib


def s(L, cx):
    v = L.clang_getCString(cx)
    r = v.decode('utf-8', 'replace') if v else ''
    L.clang_disposeString(cx)
    return r


class Walker:
    def __init__(self, root, main_file, overrides=None):
        self.overrides = overrides or {}    # rel -> real path to read the text of (a branch overlay parses the main checkout's paths through a VFS overlay)
        self.L = lib()
        self.root = os.path.normcase(os.path.abspath(root)).replace('\\', '/').rstrip('/') + '/'
        self.main_file = main_file
        self.file_cache = {}
        self.sym_cache = {}     # hashCursor -> (usr, qname, name, kind, keep) | None
        self.syms = {}          # usr -> row
        self.known = {}         # usr -> row of a symbol only referenced (declared elsewhere)
        self.refs = set()
        self.funcs = {}         # usr -> (file, line, endline)
        self.text_cache = {}
        self.ncursors = 0

    # ---- files / locations
    def rel(self, fileptr):
        """repo-relative path (forward slashes) of OUR file, '' for the SDK, None for anything else (system headers, std)."""
        if not fileptr:
            return None
        if fileptr in self.file_cache:
            return self.file_cache[fileptr]
        p = s(self.L, self.L.clang_getFileName(fileptr))
        n = os.path.abspath(p).replace('\\', '/')       # the case is kept (build_ours maps it to the file's real case)
        nl = n.lower()
        r = None
        if nl.startswith(self.root):
            r0 = n[len(self.root):]
            if r0.lower().startswith(('src/', 'tools/', 'probes/')):
                r = r0
            # out\ and the rest of the repo (third_party, thirdparty, generated): not ours
        elif '/rexglue-sdk' in nl:
            r = ''
        self.file_cache[fileptr] = r
        return r

    def loc(self, cur):
        f, ln, col, off = C.c_void_p(), C.c_uint(), C.c_uint(), C.c_uint()
        self.L.clang_getExpansionLocation(self.L.clang_getCursorLocation(cur), C.byref(f), C.byref(ln), C.byref(col), C.byref(off))
        return self.rel(f.value), ln.value, col.value

    def kind(self, cur):
        return self.L.clang_getCursorKind(cur)

    def children(self, cur):
        out = []

        def cb(c, p, d):
            out.append(CXCursor.from_buffer_copy(c))
            return 1
        self.L.clang_visitChildren(cur, VISITOR(cb), None)
        return out

    # ---- symbols
    def qname(self, cur):
        parts = []
        c = cur
        for _ in range(12):
            k = self.kind(c)
            if k == K_TU:
                break
            sp = s(self.L, self.L.clang_getCursorSpelling(c))
            if sp:
                parts.append(sp)
            c = self.L.clang_getCursorSemanticParent(c)
            if self.L.clang_Cursor_isNull(c):
                break
        return '::'.join(reversed(parts))

    def is_global_var(self, cur):
        pk = self.kind(self.L.clang_getCursorSemanticParent(cur))
        if pk in SCOPE_PARENTS:
            return True
        return self.L.clang_Cursor_getStorageClass(cur) == 3      # a static local

    def symbol(self, cur):
        """(usr, qname, name, kind, in_scope) of a referenced / declared cursor; None for locals, parameters, system things."""
        h = self.L.clang_hashCursor(cur)
        if h in self.sym_cache:
            return self.sym_cache[h]
        r = None
        k = self.kind(cur)
        if k in FUNC_KINDS or k in (K_FIELD, K_ENUMCONST, K_VAR) or k in TYPE_KINDS or k == K_TYPEDEF:
            f = self.loc(cur)[0]
            keep = f is not None
            if keep and k == K_VAR and not self.is_global_var(cur):
                keep = False
            if keep:
                usr = s(self.L, self.L.clang_getCursorUSR(cur))
                if usr:
                    name = s(self.L, self.L.clang_getCursorSpelling(cur))
                    r = (usr, self.qname(cur), name, k, f)
        self.sym_cache[h] = r
        return r

    # ---- recording
    def add_ref(self, sym, kind, cur, func):
        f, ln, col = self.loc(cur)
        if f:   # references only from OUR files
            self.refs.add((sym[0], kind, f, ln, col, func))
            self.known.setdefault(sym[0], (sym[0], sym[1], sym[2], sym[3], sym[4], 0, 0))

    def decl_sym(self, cur, isdef):
        sym = self.symbol(cur)
        if not sym or not sym[4]:
            return sym
        usr, qn, name, k, f = sym
        f, ln, col = self.loc(cur)
        old = self.syms.get(usr)
        if old is None or (isdef and not old[6]):
            self.syms[usr] = (usr, qn, name, k, f, ln, 1 if isdef else 0)
        return sym

    # ---- walking
    def top(self, cur):
        for c in self.children(cur):
            self.decl(c, '')

    def decl(self, cur, func):
        k = self.kind(cur)
        f = self.loc(cur)[0]
        if f is None or (f == '' and k not in (K_NAMESPACE, K_LINKAGE)):
            return
        self.ncursors += 1
        if k in (K_NAMESPACE, K_LINKAGE):
            for c in self.children(cur):
                self.decl(c, func)
        elif k in TYPE_KINDS:
            if f:
                self.decl_sym(cur, self.L.clang_isCursorDefinition(cur))
            for c in self.children(cur):
                self.decl(c, func)
        elif k in FUNC_KINDS:
            isdef = self.L.clang_isCursorDefinition(cur)
            sym = self.decl_sym(cur, isdef) if f else None
            if sym and isdef and f:
                ext_end = self.L.clang_getExpansionLocation
                e = self.L.clang_getRangeEnd(self.L.clang_getCursorExtent(cur))
                ff, ln, col, off = C.c_void_p(), C.c_uint(), C.c_uint(), C.c_uint()
                ext_end(e, C.byref(ff), C.byref(ln), C.byref(col), C.byref(off))
                self.funcs[sym[0]] = (f, self.loc(cur)[1], ln.value)
                self.refs.add((sym[0], 'def', f, self.loc(cur)[1], self.loc(cur)[2], ''))
                for c in self.children(cur):
                    self.expr(c, 'r', sym[0])
            elif f:
                for c in self.children(cur):
                    self.expr(c, 'r', sym[0] if sym else func)
        elif k in (K_VAR, K_FIELD):
            sym = self.decl_sym(cur, 1) if f and (k == K_FIELD or self.is_global_var(cur)) else None
            for c in self.children(cur):
                self.expr(c, 'r', func)
        elif k == K_ENUMCONST:
            if f:
                self.decl_sym(cur, 1)
        elif k == K_TYPEDEF:
            pass
        else:
            for c in self.children(cur):
                self.decl(c, func)

    def ref_target(self, cur):
        r = self.L.clang_getCursorReferenced(cur)
        if self.L.clang_Cursor_isNull(r):
            return None, None
        return r, self.symbol(r)

    def expr(self, cur, mode, func):
        L = self.L
        k = self.kind(cur)
        self.ncursors += 1
        if k == K_DECLREF:
            r, sym = self.ref_target(cur)
            if sym:
                rk = sym[3]
                if rk in FUNC_KINDS:
                    self.add_ref(sym, 'call' if mode in ('c', 'cw') else 'ref', cur, func)
                elif rk == K_ENUMCONST:
                    self.add_ref(sym, 'read' if mode == 'cmp' else 'write', cur, func)
                elif rk in (K_VAR, K_FIELD):
                    m = MODEMAP.get(mode, 'read')
                    self.add_ref(sym, m, cur, func)
                else:
                    self.add_ref(sym, 'ref', cur, func)
            return
        kids = self.children(cur)
        if k == K_MEMBERREF:   # MemberRefExpr: the method / field / static, then the object
            r, sym = self.ref_target(cur)
            sub = 'r'
            if r is not None and self.kind(r) in FUNC_KINDS:      # also a system method (std::atomic::store): the object is written
                sub = 'w' if mode == 'cw' else 'r'
            if sym:
                rk = sym[3]
                if rk in FUNC_KINDS:
                    self.add_ref(sym, 'call' if mode in ('c', 'cw') else 'ref', cur, func)
                elif rk == K_FIELD or rk == K_VAR:
                    m = MODEMAP.get(mode, 'read')
                    self.add_ref(sym, m, cur, func)
                    sub = mode if mode in ('w', 'rw') else ('addr' if mode == 'addr' else 'r')
            for i, c in enumerate(kids):
                self.expr(c, sub if i == 0 else 'r', func)
            return
        if k == K_CALL:
            r = L.clang_getCursorReferenced(cur)
            name = ''
            rk = 0
            sym = None
            if not L.clang_Cursor_isNull(r):
                name = s(L, L.clang_getCursorSpelling(r))
                rk = self.kind(r)
                sym = self.symbol(r)
            if rk == K_CTOR and sym:
                self.add_ref(sym, 'call', cur, func)
                for c in kids:
                    self.expr(c, 'r', func)
                return
            if name in ASSIGN_OPS and len(kids) >= 2:
                self.expr(kids[0], 'skip', func)
                self.expr(kids[1], ASSIGN_OPS[name], func)
                for c in kids[2:]:
                    self.expr(c, 'r', func)
                if sym:
                    self.add_ref(sym, 'call', cur, func)
                return
            if name in ('operator[]', 'operator*', 'operator->') and len(kids) >= 2:
                self.expr(kids[0], 'skip', func)
                self.expr(kids[1], mode if mode in ('w', 'rw') else 'r', func)
                for c in kids[2:]:
                    self.expr(c, 'r', func)
                if sym:
                    self.add_ref(sym, 'call', cur, func)
                return
            first = 'cw' if (rk in FUNC_KINDS and WRITE_METHODS.match(name or '')) else 'c'
            for i, c in enumerate(kids):
                self.expr(c, first if i == 0 else 'r', func)
            return
        if k == K_BINARY or k == K_COMPOUNDASSIGN:
            op = L.clang_getCursorBinaryOperatorKind(cur)
            if op == BIN_ASSIGN and len(kids) == 2:
                self.expr(kids[0], 'w', func)
                self.expr(kids[1], 'r', func)
            elif (op in BIN_COMPOUND or k == K_COMPOUNDASSIGN) and len(kids) == 2:
                self.expr(kids[0], 'rw', func)
                self.expr(kids[1], 'r', func)
            else:
                m = 'cmp' if op in BIN_CMPISH else 'r'
                for c in kids:
                    self.expr(c, m, func)
            return
        if k == K_UNARY:
            op = L.clang_getCursorUnaryOperatorKind(cur)
            m = 'rw' if op in UN_INCDEC else ('addr' if op == UN_ADDR else 'r')
            for c in kids:
                self.expr(c, m, func)
            return
        if k == K_SUBSCRIPT:
            for i, c in enumerate(kids):
                self.expr(c, mode if (i == 0 and mode in ('w', 'rw', 'addr')) else 'r', func)
            return
        if k in PASS_THROUGH:
            for i, c in enumerate(kids):
                self.expr(c, mode if (i == 0 and mode != 'skip') else ('skip' if mode == 'skip' else 'r'), func)
            return
        if k == K_CASESTMT:
            for i, c in enumerate(kids):
                self.expr(c, 'cmp' if i == 0 else 'r', func)
            return
        if k == K_TYPEREF or k == K_TEMPLATEREF:
            r, sym = self.ref_target(cur)
            if sym and mode != 'skip':
                self.add_ref(sym, 'ref', cur, func)
            return
        if k in (K_VAR, K_FIELD) or k in FUNC_KINDS or k in TYPE_KINDS:   # a local decl / local class / lambda body
            self.decl(cur, func)
            return
        if mode == 'skip':
            mode = 'r'
        for c in kids:
            self.expr(c, 'r', func)

    # ---- file text scans (switches, log lines)
    def lines(self, rel):
        if rel not in self.text_cache:
            try:
                with open(self.overrides.get(rel) or os.path.join(self.root, rel), encoding='utf-8', errors='replace') as f:
                    self.text_cache[rel] = f.read().split('\n')
            except OSError:
                self.text_cache[rel] = []
        return self.text_cache[rel]


CVAR_RE = re.compile(r'REXCVAR_GET\(\s*([A-Za-z_0-9]+)\s*\)')
LOG_RE = re.compile(r'(?:REXLOG_\w+|printf|fprintf|LOG\w*)\s*\(\s*(?:stderr\s*,\s*)?"((?:[^"\\]|\\.){0,160})')


def scan_function_text(w, funcs):
    """funcs: usr -> (file, line, endline). Returns usr -> (cvars, logs) read from the source lines of the function."""
    out = {}
    for usr, (f, a, b) in funcs.items():
        L = w.lines(f)
        text = '\n'.join(L[max(a - 1, 0):min(b, len(L))])
        cv = sorted(set(CVAR_RE.findall(text)))
        lg = []
        for m in LOG_RE.finditer(text):
            t = m.group(1)
            if t not in lg:
                lg.append(t[:100])
        if cv or lg:
            out[usr] = (cv, lg[:12])
    return out


def clean_args(args, drop_includes=False):
    """Compile-database arguments (compiler first) -> libclang arguments: no compiler, -o, -c, source file, PCH plumbing."""
    out = []
    i = 1
    n = len(args)
    while i < n:
        a = args[i]
        if a in ('-o', '-MF', '-MT'):
            i += 2
            continue
        if a in ('-c', '-MD', '-MMD'):
            i += 1
            continue
        if a == '-Xclang' and i + 1 < n:
            nx = args[i + 1]
            if nx in ('-emit-pch', ) or nx.startswith('--dependent-lib'):
                i += 2
                continue
            if nx in ('-include-pch', '-include') and i + 3 < n:
                if nx == '-include' and not drop_includes:
                    out += ['-include', args[i + 3]]
                i += 4
                continue
        if a == '-x' and i + 1 < n and args[i + 1] == 'c++-header':
            i += 2
            continue
        if not a.startswith('-') and re.search(r'\.(cpp|cc|cxx|c|rc)$', a):
            i += 1
            continue
        out.append(a)
        i += 1
    return out


def parse_tu(root, src_abs, args, overrides=None):
    """-> dict(rows). args: cleaned libclang arguments (without the source file). overrides: {rel: real file} for text scans (see Walker)."""
    import time
    t0 = time.time()
    L = lib()
    idx = L.clang_createIndex(0, 0)
    tu = C.c_void_p()
    full = list(args) + ['-w', '-fno-delayed-template-parsing', '-Wno-everything', '-x', 'c++']
    arr = (C.c_char_p * len(full))(*[a.encode() for a in full])
    # CXTranslationUnit_KeepGoing 0x200 | DetailedPreprocessingRecord 0x1 not needed
    err = L.clang_parseTranslationUnit2(idx, src_abs.encode(), arr, len(full), None, 0, 0x200, C.byref(tu))
    if err != 0 or not tu.value:
        return {'error': f'parse failed ({err})', 'secs': time.time() - t0}
    w = Walker(root, src_abs, overrides)
    incs = set()

    def icb(fp, stack, n, cd):
        r = w.rel(fp)
        if r:
            incs.add(r)
    cb = INCL(icb)
    L.clang_getInclusions(tu, cb, None)
    n_err = 0
    first_err = []
    for i in range(L.clang_getNumDiagnostics(tu)):
        d = L.clang_getDiagnostic(tu, i)
        if L.clang_getDiagnosticSeverity(d) >= 3:
            n_err += 1
            if len(first_err) < 3:
                first_err.append(s(L, L.clang_formatDiagnostic(d, 0))[:200])
        L.clang_disposeDiagnostic(d)
    w.top(L.clang_getTranslationUnitCursor(tu))
    extra = scan_function_text(w, w.funcs)
    L.clang_disposeTranslationUnit(tu)
    for u, r in w.known.items():
        w.syms.setdefault(u, r)
    return {'syms': list(w.syms.values()), 'refs': list(w.refs), 'funcs': w.funcs, 'extra': extra, 'incs': sorted(incs),
            'errors': n_err, 'first_err': first_err, 'cursors': w.ncursors, 'secs': time.time() - t0}


if __name__ == '__main__':
    # smoke: py ours_clang.py ROOT FILE [flags...]
    import json
    root, src = sys.argv[1], sys.argv[2]
    r = parse_tu(root, src, sys.argv[3:])
    print(json.dumps({k: (len(v) if isinstance(v, (list, dict)) else v) for k, v in r.items()}, default=str))
