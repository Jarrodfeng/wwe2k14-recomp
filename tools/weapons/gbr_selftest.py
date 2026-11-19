"""Lane GBR selftest: runs out/weapons/gbr_test.exe (build it first: tools/weapons/build_gbr.ps1).
The clean run must pass every check; each planted twin must fail exactly its named check.
  py -I tools/weapons/gbr_selftest.py [--root .]
"""
import os
import subprocess
import sys

PLANTS = {
    "gm_all_models": "gm.non_member",         # the gm membership test dropped: any own-submit model is a set object
    "gm_no_recheck": "gm.address_reuse",      # the per-call [obj+48] == model / [obj+44] == key re-check dropped
    "gm_ignore_pass": "gm.pass_zero",         # the thread pass index (sub_82BA77A8 == 0) not tested
    "blood_include_dead": "blood.live_records",  # a dead (alpha 0) decal record is given a call
    "event_no_clear": "event.scene_clear",    # the event list is not cleared when the stage holder changes
    "event_no_prune": "event.stale_prune",    # an event model with a stale vtable word stays listed
    "no_hold": "plan.stage_hold",             # a latched family with no rows draws nothing (no hold)
    "gm_dead_alpha": "plan.gm_blood",         # a dead (alpha not > 0) splat / set row is drawn
    "release_drop_on_busy": "release.retry",  # a busy release is forgotten, not retried
    "want_low_bit": "gm.want_flags",          # the old 0x400 want flag collides with gm keys >= 1024
    "gm_rescan_empty": "gm.empty_pulls",      # an empty set is rescanned every frame
    "scene_sig_loaded": "event.loaded_keeps", # the holder loaded flag is part of the scene signature
    "first_write_clears": "event.first_write_keeps",   # S103 SMF (BUG2 7): a 0 -> X first write of the manager / holder pointer clears the event list
    "gm_no_fallthrough": "gm.fallthrough",    # a failed gm member returns instead of reaching the event lookup
    "hold_ignore_sig": "plan.hold_holder_change",  # a structure is held across a holder change
}


def run(exe, *args):
    p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout


def main():
    root = "."
    if "--root" in sys.argv:
        root = sys.argv[sys.argv.index("--root") + 1]
    exe = os.path.join(root, "out", "weapons", "gbr_test.exe")
    if not os.path.exists(exe):
        print("FAIL: %s is not built (tools/weapons/build_gbr.ps1)" % exe)
        return 1
    bad = 0
    rc, out = run(exe)
    if rc != 0 or "FAIL  " in out:
        print(out)
        print("FAIL: the clean run did not pass")
        bad += 1
    else:
        print("PASS clean run: %d checks" % out.count("PASS "))
    for plant, check in PLANTS.items():
        rc, out = run(exe, "--plant", plant)
        failed = [ln.split()[1] for ln in out.splitlines() if ln.startswith("FAIL  ")]
        if rc == 1 and failed == [check]:
            print("PASS plant %s fails exactly %s" % (plant, check))
        else:
            print(out)
            print("FAIL: plant %s must fail exactly %s, got rc %d, failed %s" % (plant, check, rc, failed))
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
