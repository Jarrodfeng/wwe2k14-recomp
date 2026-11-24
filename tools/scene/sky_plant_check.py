"""S103 SKY: each planted twin of sky_unit must fail exactly the named checks. Exit 0 = every twin was caught."""
import subprocess, sys, os
exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'tools', 'sky_unit.exe')
plants = {
    'row_rule': {'swap.desc_follows_body', 'rumble.rows_full_named', 'rumble.seat_reoccupied', 'remap.inactive_uses_1220', 'relay.before', 'relay.key_follows_model_desc'},   # the old row rule passes the real remap case; S110: it has no key in the 30 Years relay (row 5 is empty)
    'desc_1220': {'remap.follows_1224', 'relay.before', 'relay.key_follows_model_desc', 'bad.freed_descriptor'},                              # the first SKY rule fails the real remap; review: it never reads desc+52
    'scale_by_slot': {'scale.follows_1224', 'scale.not_from_slot_record'},                        # S110 MGRFINGERS: the old scale read (D[slot position]+12); no remap = same value, so inactive_equals_slot passes
    'desc_index': {'relay.key_follows_model_desc', 'relay.scale_held', 'relay.scale_not_from_other_record', 'bad.freed_descriptor'},   # S110 DESCKEY: the key through the re-laid match record (+1224), the latch erased by a refused read; review: it latches another body's record and never reads desc+52
    'no_key_check': {'relay.scale_not_from_other_record'},                                        # S110 DESCKEY review: DescScale's D_s+0 == desc+92 check dropped: another body's scale is latched
    'whole_key': {'kept.scale_latched_across_attire'},                                            # S110 DESCKEY review F3: the whole-key compare refuses a kept model's character scale
    'no_loaded_check': {'bad.freed_descriptor'},                                                  # S110 DESCKEY review: CharModelDesc's desc+52 refusal dropped: a freed descriptor names a key
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
