"""Session 81: one real download of the newest full release through the launcher's own update code (WinHTTP, GitHub's
redirect, the SHA-256 against GitHub's digest, the unpack), in a throwaway install folder under out\\s81_update\\real.
It stops before any swap and deletes everything it downloaded when it is done (Jarrod: "delete it immediately once it
works").  py tools\\launcher\\s81_real_download_check.py
"""
import os, shutil, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
R = REPO / "out" / "s81_update" / "real"
EXE = REPO / "out" / "build" / "win-amd64-release" / "wwe2k14_launcher.exe"
GH = r"C:\Program Files\GitHub CLI\gh.exe"

shutil.rmtree(R, ignore_errors=True)
inst = R / "inst"
(R / "appdata").mkdir(parents=True)
inst.mkdir()
shutil.copy2(EXE, inst / "wwe2k14_launcher.exe")
(inst / "wwe2k14.exe").write_text("stub")
rel = subprocess.run([GH, "api", "repos/Jarrodfeng/wwe2k14-recomp/releases/latest"], capture_output=True, text=True, check=True).stdout
(R / "release.json").write_text(rel, encoding="utf-8")
env = dict(os.environ, APPDATA=str(R / "appdata"), LOCALAPPDATA=str(R / "appdata"))
cmd = [str(inst / "wwe2k14_launcher.exe"), "--settings", str(R / "appdata" / "s.json"), "--no-save", "--no-open",
       "--update-file", str(R / "release.json"), "--update-version", "v0.0.1", "--show-update", "--wait", "20",
       "--update-install", "--wait", "9000", "--exit", "--log", str(R / "real.log")]
t0 = time.time()
p = subprocess.Popen(cmd, env=env, cwd=str(inst))
ok, peak = False, 0
while time.time() - t0 < 300:
    time.sleep(1)
    z = inst / "update_tmp" / "update.zip"
    if z.exists():
        peak = max(peak, z.stat().st_size)
    stage = inst / "update_tmp" / "stage"
    if stage.exists() and not z.exists():
        kids = [k for k in stage.iterdir() if k.is_dir()]
        if kids and (kids[0] / "wwe2k14.exe").exists() and (kids[0] / "wwe2k14_launcher.exe").exists():
            ok = True
            break
    if p.poll() is not None:
        break
print(f"{time.time() - t0:.0f} s; largest download seen {peak / 1e6:.1f} MB; ready (downloaded, checksum matched, unpacked): {ok}")
log = (R / "real.log").read_text(errors="replace") if (R / "real.log").exists() else ""
print("\n".join(l for l in log.splitlines() if "update" in l.lower())[-600:])
if p.poll() is None:
    p.kill()
time.sleep(1)
shutil.rmtree(R, ignore_errors=True)
print("deleted:", not R.exists())
sys.exit(0 if ok else 1)
