"""cite_decls: the TOP-LEVEL declaration a line sits in (a table, enum, constant, struct, #define), for cite_by_name.

A function body is found by qsym's spans; a line in a constant table, a vocabulary array or a one-line constant sits in no span.  This
finds the file-scope statement that holds the line and the name it declares:  `static const int kTable[] = { ... };` -> kTable,
`struct Cfg { ... };` -> Cfg, `#define NAME ...` -> NAME, `REXCVAR_DEFINE_BOOL(my_cvar, ...)` -> my_cvar, a Python module-level
`NAME = {...}` -> NAME.  Namespace braces are not nesting.  [(start line, end line, name)], lines 1-based.
"""
import ast
import bisect
import re
from pathlib import Path

NS_HEAD = re.compile(r'\s*(?:inline\s+)?namespace\b|\s*extern\s*"')
KEYWORDS = {"if", "for", "while", "switch", "return", "static", "const", "constexpr", "inline", "extern", "volatile", "struct", "class", "enum", "union",
            "template", "typename", "using", "typedef", "unsigned", "signed", "static_assert", "alignas", "sizeof"}
TYPE_HEAD = re.compile(r'\b(?:struct|class|union|enum(?:\s+(?:class|struct))?)\s+(?:\[\[[^\]]*\]\]\s*)?(\w+)')
CALL = re.compile(r'(~?\w+(?:::~?\w+)*)\s*\(')
IDENT = re.compile(r'[A-Za-z_]\w*')
ASSIGN = re.compile(r'(?<![=!<>+\-*/|&^%])=(?!=)')


def decl_name(head):
    h = re.sub(r'template\s*<[^<>]*(?:<[^<>]*>[^<>]*)*>', ' ', head)
    cut = re.search(r'[{;]', h)
    if cut:
        h = h[:cut.start()]
    m = TYPE_HEAD.search(h)
    if m and not ASSIGN.search(h[:m.start()]):
        return m.group(1)
    eq = ASSIGN.search(h)
    pre = h[:eq.start()] if eq else h
    cm = CALL.search(pre)
    if cm and cm.group(1) not in KEYWORDS and "[" not in pre[:cm.start()]:
        name = cm.group(1)
        if re.fullmatch(r'[A-Z][A-Z0-9_]*', name):   # a macro: its first argument is the thing declared (REXCVAR_DEFINE_BOOL(cvar_name, ...))
            arg = IDENT.match(h[cm.end():].lstrip())
            return arg.group(0) if arg else name
        return name
    seg = re.split(r'[\[={]', pre)[0]
    ids = [x for x in IDENT.findall(seg) if x not in KEYWORDS]
    return ids[-1] if ids else ""


def c_top_decls(text, masked):
    """Declarations of C-like `text` (masked = the same text with comments / literals blanked, same offsets)."""
    code = re.sub(r'(?m)^[ \t]*#(?:[^\n\\]|\\[^\n]|\\\n)*', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), masked)
    nl = [m.start() for m in re.finditer('\n', code)]

    def line_of(pos):
        return bisect.bisect_right(nl, pos - 1) + 1 if pos > 0 else 1
    out = []
    stack, depth, head, open0 = [], 0, 0, 0

    def finish(h, e):
        seg = code[h:e + 1]
        s = seg.lstrip()
        if not s.strip(" \t\r\n;"):
            return
        name = decl_name(s)
        if name:
            out.append((line_of(h + len(seg) - len(s)), line_of(e), name))
    for m in re.finditer(r'[{};]', code):
        c, pos = m.group(), m.start()
        if c == "{":
            if depth == 0 and NS_HEAD.match(code[head:pos]):
                stack.append("ns")
                head = pos + 1
                continue
            if depth == 0:
                open0 = pos
            stack.append("blk")
            depth += 1
        elif c == "}":
            if not stack:
                continue
            k = stack.pop()
            if k == "ns":
                head = pos + 1
                continue
            depth -= 1
            if depth == 0:
                hd = code[head:open0]
                nxt = code[pos + 1:pos + 40].lstrip()
                if ASSIGN.search(hd) or nxt.startswith((";", ",")):
                    continue   # `= { ... };` and `struct X { ... };` end at the semicolon
                finish(head, pos)
                head = pos + 1
        elif depth == 0:
            finish(head, pos)
            head = pos + 1
    return out


def define_at(lines, n):
    """(start, end, NAME) of the #define (with its continuation lines) holding 1-based line n, else None."""
    s = n
    while s > 1 and lines[s - 2].rstrip().endswith("\\"):
        s -= 1
    m = re.match(r'\s*#\s*define\s+(\w+)', lines[s - 1])
    if not m:
        return None
    e = s
    while e < len(lines) and lines[e - 1].rstrip().endswith("\\"):
        e += 1
    return (s, e, m.group(1)) if s <= n <= e else None


def py_top_decls(path):
    try:
        tree = ast.parse(Path(path).read_text(encoding="utf-8", errors="replace"))
    except (SyntaxError, ValueError):
        return []
    out = []
    for node in tree.body:
        tg = []
        if isinstance(node, ast.Assign):
            tg = [t for t in node.targets if isinstance(t, ast.Name)]
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            tg = [node.target]
        if tg:
            out.append((node.lineno, node.end_lineno or node.lineno, tg[0].id))
    return out
