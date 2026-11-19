"""S103 SKY: each planted twin of sky_unit must fail exactly the named checks. Exit 0 = every twin was caught."""
import subprocess, sys, os
exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'tools', 'sky_unit.exe')
plants = {
    'row_rule': {'swap.desc_follows_body', 'rumble.rows_full_named', 'rumble.seat_reoccupied', 'remap.inactive_uses_1220'},   # the old row rule passes the real remap case
    'desc_1220': {'remap.follows_1224'},                                                          # the first SKY rule fails only the real remap
}
bad = False
for plant, want in plants.items():
    r = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, timeout=60)
    failed = {l.split(':')[0].split(' ', 1)[1] for l in r.stdout.splitlines() if l.startswith('FAIL')}
    if r.returncode != 1 or failed != want:
        print('planted', plant, 'not caught as expected: rc', r.returncode, 'failed', sorted(failed), 'want', sorted(want))
        bad = True
    else:
        print('planted', plant, 'caught:', sorted(failed))
sys.exit(1 if bad else 0)
