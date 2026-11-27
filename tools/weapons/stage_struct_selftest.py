"""Lane CGF selftest: runs out/weapons/stage_struct_test.exe (build it first: tools/weapons/build_stage_struct.ps1).
The clean run must pass every check; each planted twin must fail exactly its named check.
  py -I tools/weapons/stage_struct_selftest.py [--root .]
"""
import os
import subprocess
import sys

PLANTS = {
    "stage_one_family": "stage.both_families",  # S111 CAGESIDE review: the S103 one-family rule (own only when a structure has own rows)
    "stage_instance_per_family": "stage.pose_slots",  # S111 CAGESIDE review: a model's own and slot-32 calls numbered apart (they share a pose slot)
    "stage_zwrite_on": "stage.zwrite_off",       # S111 CAGESIDE review: a cage slot-32 row keeps its material's depth write
    "stage_no_recheck": "stage.address_reuse",  # the per-call [H+76+4k] == model re-check dropped
    "stage_no_cap": "stage.cage_rows_cap2",     # a cage slot-32 call not capped at its 2 palette rows
    "stage_alpha252": "stage.alpha_f1",         # a slot-32 call's alpha read from [model+252], not the call's f1
    "stage_first_only": "stage.every_call",     # the event branch's first-call-per-frame rule: 3 wall calls -> 1 row
    "stage_no_rigid_bound": "stage.rigid_bound",  # a rigid piece's palette offset is not bounded to the 2-row stack palette
    "stage_alpha_always": "stage.alpha_other_lr",  # an unknown caller's f1 is taken as MdlAlpha
    "stage_ignore_switch": "stage.switch_off",  # --wwe2k14_rdr_stage_structs = off ignored
}


def run(exe, *args):
    p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout


def main():
    root = "."
    if "--root" in sys.argv:
        root = sys.argv[sys.argv.index("--root") + 1]
    exe = os.path.join(root, "out", "weapons", "stage_struct_test.exe")
    if not os.path.exists(exe):
        print("FAIL: %s is not built (tools/weapons/build_stage_struct.ps1)" % exe)
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
