"""Session 81: tests of the launcher's self-update against a fake local release (no GitHub, no real files touched).

  py tools\\launcher\\s81_update_test.py [ok|locked|badsum|failshot|all] [--keep]

Everything happens under out\\s81_update (outside the repo's game_data, so the launcher is not a "development build"
there). The test launcher is a copy of the built one in a fake install folder; APPDATA points inside out\\s81_update
too, so nobody's real launcher settings are read or written (the new launcher the helper starts inherits it).

  ok       full path: offer, download, ready (shots), restart, swap, new launcher verifies and removes the backup;
           saves/ and other files not in the zip survive; a file the last update installed and this zip dropped goes
  locked   a file of the install is held open: the swap fails, every old file is back, the launcher shows why
  badsum   a download that does not match the digest: nothing changes
  failshot the UPDATE FAILED dialog from a result file (picture)
"""
import hashlib, json, os, shutil, subprocess, sys, time, zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "out" / "build" / "win-amd64-release"
ROOT = REPO / "out" / "s81_update"
EXE = BUILD / "wwe2k14_launcher.exe"
NEW_VER = "v9.9.9"
fails = []


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        fails.append(what)


def make_install(name, with_manifest=True):
    inst = ROOT / name
    shutil.rmtree(inst, ignore_errors=True)
    inst.mkdir(parents=True)
    shutil.copy2(EXE, inst / "wwe2k14_launcher.exe")
    (inst / "wwe2k14.exe").write_text("old game")
    (inst / "README.md").write_text("old readme")
    (inst / "old_only.txt").write_text("shipped by the last update, dropped by this one")
    (inst / "saves").mkdir()
    (inst / "saves" / "keep.txt").write_text("a save")
    (inst / "mine.txt").write_text("not from any zip")
    if with_manifest:
        (inst / "update_manifest.json").write_text(json.dumps(
            {"version": "v0.0.1", "files": ["wwe2k14_launcher.exe", "wwe2k14.exe", "README.md", "old_only.txt"]}))
    return inst


def make_release(name, good_digest=True):
    zpath = ROOT / f"wwe2k14-recomp-{NEW_VER}.zip"
    top = f"wwe2k14-recomp-{NEW_VER}"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(EXE, f"{top}/wwe2k14_launcher.exe")
        z.writestr(f"{top}/wwe2k14.exe", "new game")
        z.writestr(f"{top}/README.md", "new readme")
        z.writestr(f"{top}/newfile.txt", "new")
        z.writestr(f"{top}/data/ids/x.json", "{}")
        z.writestr(f"{top}/padding.bin", os.urandom(3_000_000))  # a visible progress bar
    sha = hashlib.sha256(zpath.read_bytes()).hexdigest()
    rel = {
        "tag_name": NEW_VER, "name": NEW_VER, "draft": False, "prerelease": False,
        "html_url": "https://github.com/Jarrodfeng/wwe2k14-recomp/releases/tag/" + NEW_VER,
        "body": "# What's new\n\n**Self-updating launcher**\nThe launcher can now install updates by itself. See the "
                "[release page](https://github.com/Jarrodfeng/wwe2k14-recomp) for details, and `update_log.txt`.\n\n"
                "- Downloads are checked against GitHub's **SHA-256** before anything changes\n"
                "- Old files are removed only after the new launcher has started\n"
                "- A failed update puts everything back and says why, with a long line that has to wrap around the "
                "edge of the notes box to prove the layout works\n\n> Saves, settings and mods are never touched.\n\n"
                "```\nupdate_backup\\  update_journal.json\n```\n\n---\n\nThanks for playing.",
        "assets": [{"name": zpath.name, "size": zpath.stat().st_size,
                    "browser_download_url": "file:///" + str(zpath).replace("\\", "/"),
                    "digest": "sha256:" + (sha if good_digest else "0" * 64)}],
    }
    out = ROOT / f"{name}.json"
    out.write_text(json.dumps(rel))
    return out


def env():
    e = dict(os.environ)
    e["APPDATA"] = str(ROOT / "appdata")
    e["LOCALAPPDATA"] = str(ROOT / "appdata")
    (ROOT / "appdata").mkdir(parents=True, exist_ok=True)
    return e


def run_launcher(inst, rel_json, args, timeout=90):
    cmd = [str(inst / "wwe2k14_launcher.exe"), "--settings", str(ROOT / "settings.json"), "--no-save", "--no-open",
           "--update-file", str(rel_json), "--update-version", "v0.2.15"] + args
    return subprocess.run(cmd, env=env(), timeout=timeout, cwd=str(inst))


def kill_launchers():
    subprocess.run(["taskkill", "/F", "/IM", "wwe2k14_launcher.exe"], capture_output=True)
    subprocess.run(["taskkill", "/F", "/IM", "wwe2k14_update_helper.exe"], capture_output=True)


def wait_gone(path, secs):
    t = time.time()
    while time.time() - t < secs:
        if not path.exists():
            return True
        time.sleep(0.5)
    return False


def test_ok():
    inst = make_install("ok")
    rel = make_release("ok_rel")
    log = ROOT / "ok_launcher.log"
    run_launcher(inst, rel, ["--show-update", "--wait", "25", "--shot", str(ROOT / "ok_1_offer.bmp"),
                             "--update-install", "--wait", "6", "--shot", str(ROOT / "ok_2_progress.bmp"),
                             "--wait", "200", "--shot", str(ROOT / "ok_3_ready.bmp"),
                             "--update-finish", "--wait", "40", "--log", str(log), "--exit"])
    # the launcher quits for the helper; the helper swaps and starts the new launcher, which verifies by itself
    t = time.time()
    while time.time() - t < 30 and not (inst / "update_backup").exists():
        time.sleep(0.2)  # the helper has not swapped yet
    check((inst / "update_backup").exists() or (inst / "update_journal.json").exists() or (inst / "newfile.txt").exists(), "the swap happened")
    check(wait_gone(inst / "update_backup", 60), "backup folder removed once the new launcher started")
    check(wait_gone(inst / "update_journal.json", 10), "journal removed")
    check((inst / "README.md").read_text() == "new readme", "README replaced")
    check((inst / "wwe2k14.exe").read_text() == "new game", "game exe replaced")
    check((inst / "newfile.txt").exists() and (inst / "data" / "ids" / "x.json").exists(), "new files added")
    check(not (inst / "old_only.txt").exists(), "file the last update shipped and this one dropped is gone")
    check((inst / "saves" / "keep.txt").read_text() == "a save", "saves untouched")
    check((inst / "mine.txt").read_text() == "not from any zip", "files not in the zip untouched")
    man = json.loads((inst / "update_manifest.json").read_text())
    check(man["version"] == NEW_VER and "newfile.txt" in man["files"], "manifest lists the new files")
    check(not (inst / "update_tmp").exists(), "download and staging folder removed")
    ul = (inst / "update_log.txt").read_text() if (inst / "update_log.txt").exists() else ""
    check("swap done" in ul and "verified" in ul, "update_log.txt tells swap done and verified")
    kill_launchers()


def test_locked():
    inst = make_install("locked")
    rel = make_release("locked_rel")
    held = open(inst / "README.md", "r+")  # a handle without delete sharing: the rename fails
    try:
        run_launcher(inst, rel, ["--show-update", "--wait", "25", "--update-install", "--wait", "200",
                                 "--update-finish", "--wait", "40", "--log", str(ROOT / "locked_launcher.log"), "--exit"])
        time.sleep(6)  # the helper tries, rolls back, restarts the launcher, which reads the result
    finally:
        held.close()
        kill_launchers()
    check((inst / "README.md").read_text() == "old readme", "old README still there")
    check((inst / "wwe2k14.exe").read_text() == "old game", "old game exe put back")
    check((inst / "old_only.txt").exists(), "stale file put back")
    check(not (inst / "newfile.txt").exists(), "no new file left behind")
    check(not (inst / "update_backup").exists() and not (inst / "update_journal.json").exists(), "backup and journal cleaned after the rollback")
    ul = (inst / "update_log.txt").read_text() if (inst / "update_log.txt").exists() else ""
    check("FAILED" in ul, "update_log.txt says it failed: " + ul.strip().splitlines()[-2:].__repr__())
    check(not (inst / "update_result.json").exists(), "the restarted launcher read the failure result")


def test_badsum():
    inst = make_install("badsum")
    rel = make_release("badsum_rel", good_digest=False)
    log = ROOT / "badsum_launcher.log"
    run_launcher(inst, rel, ["--show-update", "--wait", "25", "--update-install", "--wait", "200",
                             "--shot", str(ROOT / "badsum_fail.bmp"), "--log", str(log), "--exit"])
    lt = log.read_text() if log.exists() else ""
    check("checksum" in lt, "the log names the checksum failure")
    check((inst / "README.md").read_text() == "old readme" and not (inst / "newfile.txt").exists(), "nothing changed")
    check(not (inst / "update_tmp").exists(), "the bad download was removed")


def test_failshot():
    inst = make_install("failshot")
    rel = make_release("failshot_rel")
    (inst / "update_result.json").write_text(json.dumps(
        {"ok": False, "version": NEW_VER, "error": "a file is in use or locked: wwe2k14.exe (The process cannot access the file)"}))
    run_launcher(inst, rel, ["--wait", "30", "--shot", str(ROOT / "failshot.bmp"), "--exit", "--log", str(ROOT / "failshot.log")])
    check(not (inst / "update_result.json").exists(), "result file consumed")


if __name__ == "__main__":
    which = [a for a in sys.argv[1:] if not a.startswith("--")] or ["all"]
    ROOT.mkdir(parents=True, exist_ok=True)
    if not EXE.exists():
        sys.exit("build the launcher first")
    kill_launchers()
    for name, fn in [("ok", test_ok), ("locked", test_locked), ("badsum", test_badsum), ("failshot", test_failshot)]:
        if "all" in which or name in which:
            print(f"--- {name}")
            fn()
    print("FAILED: " + "; ".join(fails) if fails else "ALL PASS")
    sys.exit(1 if fails else 0)
