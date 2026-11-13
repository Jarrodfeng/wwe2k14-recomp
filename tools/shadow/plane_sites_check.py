r"""Lane 159: the plane shadow writer's listening sites, proven against the GAME'S CODE, not against our own table. Offline, 0 game runs, reads the local generated code (never committed).

  py -I tools\shadow\plane_sites_check.py            check src\scene_live\plane_sites.h against the listings of the three upload functions
  py -I tools\shadow\plane_sites_check.py --plant NAME   NAME = foreign_lr | shifted_lr | dropped_row | wrong_reg | wrong_rows | f1_range ; exits 0 only if the check then FAILS

Why: lane 139's first writer listened at return addresses that never uploaded the mask's rows (the 18 op-21 calls of sub_82BF22C8 are engine-parameter slots), and the unit oracle replayed the very
table the writer used, so it was circular and passed. This check is the independent half: for every row of the committed table it reads the call in the listing of the function the table names
(py tools\analysis\list_function.py) and requires
  1. the instruction at lr - 4 is `bl 0x82bb27d8` (the op-21 recorder);
  2. the last `li r4,N` / `li r6,N` before that call are the row's first register and row count;
  3. lr lies inside one of the upload functions (derived: the direct callees of sub_827EED58 / sub_827EEAE0 that call the recorder; today sub_827EC600, sub_827EE190, sub_827ECD10), and the SET of recorder calls in those functions is exactly the table's lr set (a missed upload
     fails, as does an extra row);
  4. no row lies in sub_82BF22C8 (lane 139's F1: the function whose range the first version accepted).
Needs the main checkout's generated\default (RDR_MAIN_CHECKOUT overrides)."""
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RECORDER = 0x82BB27D8
ROOTS = {'sub_827EED58': 0x827EED58, 'sub_827EEAE0': 0x827EEAE0}   # the plane module's mask draw and blur draw: the upload functions are DERIVED from their callees (review N6), not listed by hand
FOREIGN = 'sub_82BF22C8'


def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return Path(os.environ['RDR_MAIN_CHECKOUT'])
    try:
        common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return ROOT


MAIN = main_checkout()


def listing(addr):
    """[(address, text)] of the function from the local generated code."""
    r = subprocess.run([sys.executable, '-I', str(MAIN / 'tools' / 'analysis' / 'list_function.py'), '%08X' % addr], capture_output=True, text=True, cwd=str(MAIN), timeout=300)
    rows = []
    for line in r.stdout.splitlines():
        m = re.match(r'0x([0-9A-F]{8})\s+(.*)', line)
        if m:
            rows.append((int(m.group(1), 16), m.group(2).strip()))
    return rows


def callees(rows):
    """Direct `bl` targets of a listing (not the recorder itself)."""
    out = []
    for _, t in rows:
        m = re.match(r'bl 0x([0-9a-f]+)$', t)
        if m and int(m.group(1), 16) != RECORDER and int(m.group(1), 16) not in out:
            out.append(int(m.group(1), 16))
    return out


def derive_functions():
    """{name: listing} of the direct callees of the plane module's two draws that call the recorder."""
    found = {}
    for root in ROOTS.values():
        for target in callees(listing(root)):
            rows = listing(target)
            if rows and recorder_calls(rows):
                found['sub_%08X' % target] = rows
    return found


def parse_table(text):
    """The rows of kSites: (lr, reg, rows)."""
    out = []
    for m in re.finditer(r'\{0x([0-9A-Fa-f]{8}),\s*(\d+),\s*(\d+),\s*Chain::\w+,\s*What::\w+', text):
        out.append((int(m.group(1), 16), int(m.group(2)), int(m.group(3))))
    return out


def recorder_calls(rows):
    """[(call_address, reg, rows)] of every `bl recorder` in a listing: reg / rows = the last li r4 / li r6 before the call (within 14 instructions)."""
    calls = []
    for i, (a, t) in enumerate(rows):
        m = re.match(r'bl 0x([0-9a-f]+)$', t)
        if not m or int(m.group(1), 16) != RECORDER:
            continue
        r4 = r6 = None
        for j in range(i - 1, max(i - 15, -1), -1):
            tt = rows[j][1]
            if tt.startswith(('bl ', 'b ', 'blr', 'bctr')):   # the previous call or branch ends the setup of this one
                break
            if r4 is None:
                x = re.match(r'li r4,(-?\d+)$', tt)
                if x:
                    r4 = int(x.group(1))
            if r6 is None:
                x = re.match(r'li r6,(-?\d+)$', tt)
                if x:
                    r6 = int(x.group(1))
        calls.append((a, r4, r6))
    return calls


def check(table, listings, foreign_calls):
    problems = []
    calls = {}
    for name, rows in listings.items():
        for a, r4, r6 in recorder_calls(rows):
            calls[a + 4] = (name, r4, r6)
    lrs = [t[0] for t in table]
    if len(set(lrs)) != len(lrs):
        problems.append('the table has a return address twice')
    for lr, reg, rows in table:
        if lr in foreign_calls:
            problems.append('row 0x%08X lies in %s, the function lane 139 first listened to (F1): it uploads engine-parameter slots, not the plane constants' % (lr, FOREIGN))
            continue
        if lr not in calls:
            problems.append('row 0x%08X: no `bl 0x82bb27d8` at 0x%08X in %s' % (lr, lr - 4, ' / '.join(listings)))
            continue
        name, r4, r6 = calls[lr]
        if r4 != reg:
            problems.append('row 0x%08X (%s): the listing sets r4 = %s, the table says register %d' % (lr, name, r4, reg))
        if r6 != rows:
            problems.append('row 0x%08X (%s): the listing sets r6 = %s, the table says %d rows' % (lr, name, r6, rows))
    for lr in sorted(set(calls) - set(lrs)):
        problems.append('the listing of %s has a recorder call (return 0x%08X, r4 %s, r6 %s) the table does not listen to' % (calls[lr][0], lr, calls[lr][1], calls[lr][2]))
    return problems


def main():
    plant = ''
    if '--plant' in sys.argv:
        plant = sys.argv[sys.argv.index('--plant') + 1]
    text = (ROOT / 'src' / 'scene_live' / 'plane_sites.h').read_text(encoding='utf-8')
    table = parse_table(text)
    if len(table) < 15:
        print('FAIL cannot read the table (%d rows)' % len(table))
        return 1
    listings = derive_functions()
    if len(listings) < 3:
        print('FAIL derived only %d upload function(s) from the call graph (generated code missing under %s?): %s' % (len(listings), MAIN, ', '.join(listings)))
        return 1
    foreign = {a + 4 for a, _, _ in recorder_calls(listing(0x82BF22C8))}
    if not foreign:
        print('FAIL no listing for %s' % FOREIGN)
        return 1
    if plant == 'foreign_lr':      # the F1 mistake: one row points at an upload of the engine-parameter function
        table[3] = (sorted(foreign)[2], table[3][1], table[3][2])
    elif plant == 'f1_range':      # the F1 mistake as the first version wrote it: every row in the foreign function
        table = [(sorted(foreign)[i % len(foreign)], r, n) for i, (lr, r, n) in enumerate(table)]
    elif plant == 'shifted_lr':    # a return address one instruction late
        table[5] = (table[5][0] + 4, table[5][1], table[5][2])
    elif plant == 'dropped_row':   # a real upload nobody listens to
        table = table[:7] + table[8:]
    elif plant == 'wrong_reg':
        table[9] = (table[9][0], table[9][1] + 1, table[9][2])
    elif plant == 'wrong_rows':
        table[8] = (table[8][0], table[8][1], table[8][2] + 1)
    elif plant:
        print('unknown plant %s' % plant)
        return 2
    problems = check(table, listings, foreign)
    if plant:
        baseline = check(parse_table(text), listings, foreign)
        problems = [p for p in problems if p not in baseline]   # only what the plant added counts as caught
    calls_total = sum(len(recorder_calls(r)) for r in listings.values())
    if plant:
        if problems:
            print('plant %s caught (%d problem(s)): %s' % (plant, len(problems), problems[0]))
            return 0
        print('FAIL plant %s was NOT caught: the check cannot fail' % plant)
        return 1
    for p in problems:
        print('FAIL ' + p)
    if problems:
        return 1
    print('ok    plane_sites: all %d table rows are recorder calls at the register / row count the listings set; the three upload functions hold %d recorder calls, exactly the table; no row in %s (%d calls)' %
          (len(table), calls_total, FOREIGN, len(foreign)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
