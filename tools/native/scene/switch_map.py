"""The render-command replay switch's cases (Session 50, docs\\native\\SCENE_MAP.md).

For the interpreter sub_82BB53D0 (one record per call; db render_cmd_dispatch): every opcode's case label from the
recompiler's switch (generated code, read locally - nothing of it is printed but labels and addresses) and the calls
its case block makes before it leaves (from tools\\analysis\\list_function.py), with the names the function database
and tools\\q know for them. Prints a Markdown table: op | case address | calls (address = db name).

    py tools\\native\\scene\\switch_map.py [--fn 0x82BB53D0] [--md OUT]
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))


def run(args):
    return subprocess.run([sys.executable] + args, cwd=ROOT, capture_output=True, text=True).stdout


def generated_switch(fn):
    """{case: label address} from the generated code's switch in fn (the first switch of the function)."""
    out = run([os.path.join('tools', 'q', 'q.py'), 'fn', fn])
    m = re.search(r'(generated[\\/][^:]+\.cpp):(\d+)', out)
    if not m:
        sys.exit(f'no generated location for {fn}')
    path, line = os.path.join(ROOT, m.group(1)), int(m.group(2))
    cases = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()
    i = line - 1
    while i < len(lines) and 'switch (' not in lines[i]:
        i += 1
    case = None
    for j in range(i + 1, min(i + 2000, len(lines))):
        t = lines[j].strip()
        mc = re.match(r'case (\d+):', t)
        if mc:
            case = int(mc.group(1))
            continue
        mg = re.match(r'goto loc_([0-9A-F]+);', t)
        if mg and case is not None:
            cases[case] = int(mg.group(1), 16)
            case = None
            continue
        if t.startswith('default:') or t == '}':
            break
    return cases


def listing(fn):
    """[(address, text)] of fn's instructions and labels."""
    out = run([os.path.join('tools', 'analysis', 'list_function.py'), fn])
    rows = []
    for t in out.splitlines():
        m = re.match(r'0x([0-9A-F]{8})\s+(.*)', t.strip())
        if m:
            rows.append((int(m.group(1), 16), m.group(2)))
    return rows


def names():
    """{address: name} from every db\\*.json entry (our annotations)."""
    found = {}
    for name in os.listdir(os.path.join(ROOT, 'db')):
        if not name.endswith('.json'):
            continue
        try:
            data = json.load(open(os.path.join(ROOT, 'db', name), encoding='utf-8'))
        except Exception:
            continue
        entries = data if isinstance(data, list) else data.get('functions', data.get('entries', []))
        if isinstance(entries, dict):
            entries = list(entries.values())
        for e in entries:
            if isinstance(e, dict) and 'address' in e and 'name' in e:
                try:
                    found.setdefault(int(str(e['address']), 16), e['name'])
                except ValueError:
                    pass
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fn', default='0x82BB53D0')
    ap.add_argument('--md', default='')
    a = ap.parse_args()
    cases = generated_switch(a.fn)
    rows = listing(a.fn)
    index = {addr: k for k, (addr, _) in enumerate(rows)}
    db = names()
    by_label = {}
    for op, label in cases.items():
        by_label.setdefault(label, []).append(op)
    lines = ['| op | case | calls (address = db name) |', '|---|---|---|']
    for op in sorted(cases):
        label = cases[op]
        calls = []
        k = index.get(label)
        # Follow the block until it leaves: an unconditional branch out, a return, or 40 instructions.
        steps = 0
        while k is not None and k < len(rows) and steps < 40:
            addr, text = rows[k]
            m = re.match(r'bl 0x([0-9a-f]+)', text)
            if m:
                t = int(m.group(1), 16)
                calls.append(f'0x{t:08X}' + (f' = {db[t]}' if t in db else ''))
            if text.startswith('bctrl'):
                calls.append('(indirect call)')
            if re.match(r'b 0x', text) or text.startswith('blr') or text.startswith('bctr') and not text.startswith('bctrl'):
                break
            k += 1
            steps += 1
        shared = by_label[label]
        note = '' if len(shared) == 1 else f' (shared with {", ".join(str(o) for o in shared if o != op)})'
        if label == cases.get(0) and op != 1:
            pass
        lines.append(f'| {op} | 0x{label:08X}{note} | {"; ".join(calls) if calls else "-"} |')
    text = '\n'.join(lines)
    if a.md:
        open(a.md, 'w', encoding='utf-8').write(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
