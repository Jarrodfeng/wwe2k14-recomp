"""S105 C2LA F1: the planted twin of submodel_remap_unit: with --plant skip_noise the unit MUST fail (exit 1, the noise row FAIL)."""
import subprocess
import sys

import os
exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "tools", "submodel_remap_unit.exe")
r = subprocess.run([exe, "--plant", "skip_noise"], capture_output=True, text=True)
out = r.stdout
ok = r.returncode != 0 and "FAIL noise is mat_noize" in out
print(out)
print("twin fails as it must" if ok else "TWIN DID NOT FAIL: the unit cannot see a missing noise remap")
sys.exit(0 if ok else 1)
