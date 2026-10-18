"""Validate crash triage against the historical runs whose outcome is known (Session 8, task 13).

    py tools\\triage\\validate.py [--runs 1-69] [--md OUT.md] [--no-events]
    py tools\\triage\\validate.py --batch 200-274          (no known table: just the verdict per run; table in
                                                          out\\harness\\triage\\batch_200-274.md unless --md)

The known outcome of each run comes from PLAYBOOK.md (Sessions 2-4) and, where PLAYBOOK does not describe a
run individually, from the Windows event log (an Application event 1000 for wwe2k14.exe = the process crashed; no
entry = it did not). The source of each known class is printed next to it so a reader can check it. The triage
verdict is compared on the class only; 'no_crash' matches clean_exit, script_failure, script_stall or ended_abruptly.
Culprits are compared where PLAYBOOK names one (most historical host offsets cannot be resolved any more: the map
of that build is gone, and triage says so instead of resolving them against today's map).
Writes the table to out\\harness\\triage\\validation.md (game-derived: addresses and function names).
"""
import argparse
import datetime as dt
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage as T  # noqa: E402

P2 = 'PLAYBOOK S2'
P3 = 'PLAYBOOK S3'
P4 = 'PLAYBOOK S4'
EV = 'event log: no error entry'
# run: (known class, culprit named by PLAYBOOK or None, source)
KNOWN = {
    1: ('startup_error', None, 'log (1 line: --game_data_root not provided); not in PLAYBOOK'),
    2: ('missing_function', '0x834C5610', f'{P2} "first crash address 0x834C5610"'),
    3: ('missing_function', '0x834C5610', f'{P2} hints round 1 from wwe2k14_003'),
    4: ('missing_function', '0x82D22E30', f'{P2} round 2 from wwe2k14_004, "0x82D22E30 also a live crash"'),
    5: ('missing_function', None, f'{P2} "one per run" (rounds 004-008)'),
    6: ('missing_function', None, f'{P2} round 3 from wwe2k14_006'),
    7: ('missing_function', '0x82BB6020', f'{P2} round 4 "after crash 0x82BB6020"'),
    8: ('missing_function', '0x8331EA70', f'{P2} hints_live: 0x8331EA70 from wwe2k14_008'),
    9: ('switch_out_of_range', 'sub_835A9728', 'config/switch_tables.toml: wwe2k14_009.log, fault offset 0x535AA7'),
    10: ('no_crash', None, f'{P2} table dump run (--wwe2k14_dump_words); {EV}'),
    11: ('switch_out_of_range', 'sub_835AD638', f'{P2} 2nd ud2, wwe2k14_011.log, fault offset 0x4E1F338'),
    12: ('no_crash', None, f'{P2} "dumped all 66 tables in one launch (wwe2k14_012.log)"'),
    13: ('missing_function', '0x823F16E0', f'{P2} 0x823F16E0 (vcall-thunk slot)'),
    14: ('device_lost', None, f'{P2} M3 crash: ucrtbase 0xC0000409 without FATAL, D3D12 GPU faults (event 153)'),
    15: ('no_crash', None, f'{P2} memory findings run 015; {EV}'),
    16: ('no_crash', None, f'{P2} memory findings run 016; {EV}'),
    17: ('device_lost', None, f'{P2} A/B run: crash in Punk\'s entrance = D3D12 device lost (abort in rexgpu-xenos)'),
    18: ('no_crash', None, f'not in PLAYBOOK; {EV}'),
    19: ('no_crash', None, f'not in PLAYBOOK; {EV}'),
    20: ('device_lost', None, f'{P2}/{P3} first Vulkan match test, device lost'),
    21: ('device_lost', None, f'{P3} original policy 5/5 crashed (020-023, 025)'),
    22: ('device_lost', None, f'{P3} original policy 5/5 crashed'),
    23: ('device_lost', None, f'{P3} original policy 5/5 crashed; first checkpoint report = resolve (copy)'),
    24: ('no_crash', None, f'{P3} fixed policy survived: first full match, clean exit'),
    25: ('device_lost', None, f'{P3} A/B with the original policy: crashed, over the cap at frames 8428/8429'),
    26: ('guest_access_violation', 'sub_82CE3868', f'{P3} survived device loss; Lettering crash found in 026/027/028'),
    27: ('guest_access_violation', 'sub_82CE3868', f'{P3} Lettering, --log_level=debug'),
    28: ('guest_access_violation', 'sub_82CE3868', f'{P3} Lettering with wwe2k14_mount_cache'),
    29: ('no_crash', None, f'{P3} title-screen frame timing measurement; {EV}'),
    30: ('guest_access_violation', 'sub_82CE3868', f'{P3} narrowed down with the new harness (wwe2k14_030)'),
}
for n in list(range(31, 45)) + [46, 47]:
    KNOWN[n] = ('no_crash', None, f'{P3} Lettering investigation step runs; {EV}')
for n in (45, 48, 49, 50, 51):
    KNOWN[n] = ('guest_access_violation', 'sub_82CE3868' if n == 48 else None,
                f'{P3} Lettering repro / hook runs; event log 1000 0xC0000005' + (' (048 = repro boot->crash 63 s)' if n == 48 else ''))
KNOWN[52] = ('no_crash', None, f'{P3} fix verified: Lettering opens, clean exit')
KNOWN[53] = ('no_crash', None, f'{P3} regression run after the fix (PASS); {EV}')
for n in range(54, 60):
    KNOWN[n] = ('no_crash', None, f'{P4} runs 054-059 (slowdown A/Bs), none crashed; {EV}')

NO_CRASH = {'clean_exit', 'script_failure', 'ended_abruptly', 'script_stall'}


def matches(known, got):
    return got in NO_CRASH if known == 'no_crash' else got == known


def parse_range(s):
    out = []
    for part in s.split(','):
        a, _, b = part.partition('-')
        out += list(range(int(a), int(b or a) + 1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', default='1-59')
    ap.add_argument('--batch', help='verdicts only, no known table (e.g. 200-274)')
    ap.add_argument('--md', default=None, help='table output (default out\\harness\\triage\\validation.md, or '
                                                'batch_<range>.md with --batch so the validation table is kept)')
    ap.add_argument('--no-events', action='store_true')
    a = ap.parse_args()
    if a.md is None:
        name = ('batch_' + a.batch.replace(',', '_') + '.md') if a.batch else 'validation.md'
        a.md = os.path.join(T.REPO, 'out', 'harness', 'triage', name)
    runs = parse_range(a.batch or a.runs)
    idx, image, dbe = T.CodeIndex(), T.Image(), T.load_db_entries()
    logs = os.path.join(T.EXE_DIR, 'logs')
    present = [n for n in runs if os.path.exists(os.path.join(logs, f'wwe2k14_{n:03d}.log'))]
    if not a.no_events and present:  # one event-log query for the whole span
        parts = T.find_parts(str(present[0]), T.EXE_DIR)[1]
        with open(parts[0], encoding='utf-8', errors='replace') as f:
            t0 = T.parse_ts(T.TS_RE.match(f.readline()).group(1))
        t1 = dt.datetime.fromtimestamp(os.path.getmtime(os.path.join(logs, f'wwe2k14_{present[-1]:03d}.log')))
        T.preload_events(t0 - dt.timedelta(minutes=5), t1 + dt.timedelta(minutes=10))
    rows, ok, total, misses = [], 0, 0, []
    for n in present:
        v = T.triage(str(n), T.EXE_DIR, not a.no_events, None, idx, image, dbe)
        got, conf, cul = v['class'], v['confidence'], v['culprit'] or ''
        if a.batch:
            rows.append(f'| {n:03d} | {got} | {conf} | {cul} | {(v["details"][0] if v["details"] else "")[:110]} |')
            print(f'{n:03d}  {got:<24} {conf:<7} {cul[:60]}')
            continue
        known = KNOWN.get(n)
        if not known:
            continue
        kc, kcul, src = known
        m = matches(kc, got)
        total += 1
        ok += m
        cul_cmp = ''
        if kcul:
            cul_cmp = 'same' if kcul.lower() in cul.lower() else ('unresolved (map stale)' if not v['map']['fresh']
                                                                 and not cul.lower().startswith('0x') else 'differs')
            if cul_cmp == 'same' and got == 'missing_function':
                cul_cmp = 'same (read from the FATAL line: not an independent check)'
        if not m:
            misses.append((n, kc, got, v['details'][:2]))
        rows.append(f'| {n:03d} | {kc} | {got} ({conf}) | {"yes" if m else "**NO**"} | {kcul or ""} | '
                    f'{cul or ""} | {cul_cmp} | {src} |')
        print(f'{n:03d}  known {kc:<24} triage {got:<24} {conf:<7} {"MATCH" if m else "MISS "}  '
              f'culprit {cul[:50]}{"  [PLAYBOOK: " + kcul + ", " + cul_cmp + "]" if kcul else ""}')
    os.makedirs(os.path.dirname(a.md), exist_ok=True)
    with open(a.md, 'w', encoding='utf-8', newline='\n') as f:
        if a.batch:
            f.write('| run | triage class | conf | culprit | first detail |\n|---|---|---|---|---|\n')
        else:
            f.write(f'Triage validation, {dt.datetime.now():%Y-%m-%d %H:%M}: {ok}/{total} classes match.\n\n')
            f.write('| run | known class | triage verdict | match | PLAYBOOK culprit | triage culprit | culprit '
                    'check | source of the known class |\n|---|---|---|---|---|---|---|---|\n')
        f.write('\n'.join(rows) + '\n')
    if not a.batch:
        print(f'\n{ok}/{total} classes match; table: {a.md}')
        for n, kc, got, det in misses:
            print(f'MISS {n:03d}: known {kc}, triage {got}: {" / ".join(det)[:300]}')
    return 0 if a.batch or ok == total else 1


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(errors='replace')
    except AttributeError:
        pass
    sys.exit(main())
