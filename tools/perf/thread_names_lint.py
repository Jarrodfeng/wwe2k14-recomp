"""S102 lane H1: every thread WE create in src\\ must name itself.

  py -I tools\\perf\\thread_names_lint.py [--root DIR] [--list]     (exit 0 = clean, 1 = unnamed threads, 2 = could not read src)
  py -I tools\\perf\\thread_names_lint.py --selftest                (planted unnamed thread must be caught; named / exempt ones must pass)

A thread creation is `std::thread(`, `std::async(` (a real thread when launched async), or a bare `CreateThread(` / `_beginthreadex(` / `pthread_create(`.
Its argument list (balanced parentheses, so a multi-line lambda is the whole body) must contain `NameThread(` (src\\thread_name.h) or
`SetThreadDescription`, or the marker `thread-named-in:` followed by the function that names it (for a thread whose body is a named function).
Declarations (`std::thread t_;`, `std::vector<std::thread>`, `std::thread::id`) are not creations. `--list` prints every thread and its name (the
list lodged in docs\\renderer\\HEAT.md)."""
import re, sys, os, argparse, tempfile
from pathlib import Path

CREATE = re.compile(r'(?<![\w:])(?:std::thread|std::async|CreateThread|_beginthreadex|pthread_create)\s*\(')
NAME = re.compile(r'NameThread\(\s*L?"([^"]*)"')
OK = re.compile(r'NameThread\(|SetThreadDescription|thread-named-in:')
EXTS = {'.h', '.cpp', '.cc', '.c', '.hpp', '.inl'}


def span(text, open_idx):
    """Text from the '(' at open_idx to its matching ')' (strings and char literals skipped; comments are kept: they may hold the marker)."""
    depth = 0; i = open_idx; n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c == "'" and not (i > 0 and text[i - 1].isalnum()):
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return text[open_idx:i + 1]
        i += 1
    return text[open_idx:]


def scan(root):
    found = []
    files = [p for p in Path(root).rglob('*') if p.suffix in EXTS and p.is_file()]
    if not files:
        raise SystemExit(f'thread_names_lint: no source files under {root}: refusing to report "clean"')
    for p in sorted(files):
        text = p.read_text(encoding='utf-8', errors='replace')
        for m in CREATE.finditer(text):
            ls = text.rfind('\n', 0, m.start()) + 1
            if text[ls:m.start()].lstrip().startswith('//'):
                continue
            # a CreateThread name inside another identifier (ExCreateThread, FlightExCreateThread) is excluded by the lookbehind
            body = span(text, m.end() - 1)
            line = text.count('\n', 0, m.start()) + 1
            nm = NAME.search(body)
            found.append((str(p), line, m.group(0).split('(')[0].strip(), bool(OK.search(body)), nm.group(1) if nm else ('(in ' + re.search(r'thread-named-in:\s*(\S+)', body).group(1) + ')' if 'thread-named-in:' in body and re.search(r'thread-named-in:\s*(\S+)', body) else '')))
    return found


def selftest():
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / 'good.h').write_text('void f(){ std::thread([]{ wwe2k14::NameThread(L"ok_thread"); Run(); }).detach(); }\n'
                                        'std::thread t_;\nstd::vector<std::thread> v;\nauto id = std::this_thread::get_id(); std::thread::id x;\n'
                                        'auto h = CreateThread(nullptr, 0, &W::Main, this, 0, /* thread-named-in: W::Main */ nullptr);\n'
                                        'auto e = FlightExCreateThread(a, b);\n// std::thread([]{ x(); });\n')
        good = scan(d)
        bad_good = [g for g in good if not g[3]]
        (Path(d) / 'bad.h').write_text('void g(){\n  std::thread([]{\n    Work();\n  }).detach();\n}\n')
        planted = [g for g in scan(d) if not g[3]]
        ok = (not bad_good) and len(good) == 2 and len(planted) == 1 and planted[0][1] == 2
        print(f'selftest: named/exempt creations seen {len(good)} (want 2), unnamed in the good file {len(bad_good)} (want 0), planted unnamed caught {len(planted)} (want 1)')
        return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    src = Path(a.root) / 'src'
    if not src.is_dir():
        print(f'thread_names_lint: {src} not found'); sys.exit(2)
    found = scan(src)
    root = Path(a.root)
    if a.list:
        for f, line, kind, ok, nm in found:
            print(f'{os.path.relpath(f, root)}:{line}\t{kind}\t{nm or "UNNAMED"}')
    bad = [f for f in found if not f[3]]
    for f, line, kind, ok, nm in bad:
        print(f'UNNAMED {kind}: {os.path.relpath(f, root)}:{line}: add wwe2k14::NameThread(L"...") (src\\thread_name.h) as the first statement of its body')
    print(f'thread_names_lint: {len(found)} thread creations, {len(bad)} unnamed')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
