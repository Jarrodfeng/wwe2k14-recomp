#!/usr/bin/env python3
"""Session 95 (piece LIFE): what the built APK must say, read back from the APK itself.

  py tools\\android\\check_apk.py <apk> --build-tools <folder>

Checks (exit 1 and one FAIL line each when wrong):
  - every activity's configChanges holds all the bits a running game must survive (SDL cannot rebuild its activity);
  - android:hasFragileUserData is on: uninstalling asks to keep the data (the saves live in the app's files folder);
  - versionCode is the one android\\VERSION gives and above 18, the last hand-set code: a lower one fails to install as an update;
  - versionName is android\\VERSION;
  - the native libraries the activities load are in lib/arm64-v8a (Session 98: libviewer.so for ViewerActivity too);
  - the signing key's SHA-256 is tools\\android\\signing_fingerprint.txt: an APK from another key fails to install as an update,
    and the only way out for a player is to uninstall. --no-signature skips this one (an unsigned APK).
"""
import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
LAST_HAND_SET_CODE = 18
ACTIVITIES = ('LauncherActivity', 'GameActivity', 'TouchEditActivity', 'ViewerActivity')  # Session 98: + the Arena Viewer
# Session 98 (Arena Viewer phone port, stage C): the native libraries every APK must carry (each activity's getLibraries()).
LIBRARIES = ('libc++_shared.so', 'librexruntime.so', 'libmain.so', 'liblauncher.so', 'libviewer.so')
# android.content.pm.ActivityInfo CONFIG_* (the values aapt writes for the manifest's names).
CONFIG_BITS = {
    'mcc': 0x1, 'mnc': 0x2, 'locale': 0x4, 'touchscreen': 0x8, 'keyboard': 0x10, 'keyboardHidden': 0x20, 'navigation': 0x40,
    'orientation': 0x80, 'screenLayout': 0x100, 'uiMode': 0x200, 'screenSize': 0x400, 'smallestScreenSize': 0x800,
    'density': 0x1000, 'layoutDirection': 0x2000, 'colorMode': 0x4000, 'grammaticalGender': 0x8000,
    'fontWeightAdjustment': 0x10000000, 'fontScale': 0x40000000,
}


def version_code(version: str) -> int:
    """android-vMAJOR.MINOR.PATCH -> major*10000 + minor*100 + patch (the same sum as build_apk.ps1)."""
    m = re.fullmatch(r'android-v(\d+)\.(\d+)\.(\d+)', version)
    if not m:
        raise ValueError(f'android\\VERSION is not android-vMAJOR.MINOR.PATCH: {version!r}')
    major, minor, patch = (int(g) for g in m.groups())
    if minor > 99 or patch > 99:
        raise ValueError(f'android\\VERSION: minor and patch must stay under 100: {version!r}')
    return major * 10000 + minor * 100 + patch


def parse_manifest(text: str):
    """aapt's xmltree dump -> [(element, {attribute: raw value})] in file order."""
    elements = []
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r'E: (\S+)', s)
        if m:
            elements.append((m.group(1), {}))
            continue
        m = re.match(r'A: (?:http://schemas\.android\.com/apk/res/)?(?:android:)?([A-Za-z]+)(?:\(0x[0-9a-f]+\))?=(.*)', s)
        if m and elements:
            elements[-1][1][m.group(1)] = m.group(2)
    return elements


def int_value(raw: str):
    m = re.search(r'\(type 0x[0-9a-f]+\)(0x[0-9a-f]+)', raw)
    return int(m.group(1), 16) if m else None


def str_value(raw: str):
    m = re.match(r'"([^"]*)"', raw)
    return m.group(1) if m else None


def known_fingerprint() -> str:
    path = HERE / 'signing_fingerprint.txt'
    if not path.is_file():
        return ''
    for line in path.read_text(encoding='ascii').splitlines():
        line = line.strip().lower()
        if re.fullmatch(r'[0-9a-f]{64}', line):
            return line
    return ''


def check_manifest(text: str, version: str) -> list:
    fails = []
    elements = parse_manifest(text)
    manifest = next((a for e, a in elements if e == 'manifest'), {})
    application = next((a for e, a in elements if e == 'application'), {})
    want_code = version_code(version)
    code = int_value(manifest.get('versionCode', ''))
    if code != want_code:
        fails.append(f'versionCode is {code}, android\\VERSION gives {want_code}')
    if code is None or code <= LAST_HAND_SET_CODE:
        fails.append(f'versionCode {code} is not above {LAST_HAND_SET_CODE}: it would not install as an update')
    name = str_value(manifest.get('versionName', ''))
    if name != version:
        fails.append(f'versionName is {name}, android\\VERSION says {version}')
    if int_value(application.get('hasFragileUserData', '')) in (None, 0):
        fails.append('android:hasFragileUserData is not on')
    want_bits = 0
    for bit in CONFIG_BITS.values():
        want_bits |= bit
    for activity in ACTIVITIES:
        found = [a for e, a in elements if e == 'activity' and (str_value(a.get('name', '')) or '').endswith('.' + activity)]
        if len(found) != 1:
            fails.append(f'{activity}: {len(found)} activities in the manifest')
            continue
        bits = int_value(found[0].get('configChanges', '')) or 0
        missing = [n for n, b in CONFIG_BITS.items() if not bits & b]
        if missing:
            fails.append(f'{activity}: configChanges lacks {"|".join(missing)}')
    return fails


def check_libraries(apk: str) -> list:
    with zipfile.ZipFile(apk) as z:
        names = set(z.namelist())
    return [f'lib/arm64-v8a/{n} is not in the APK' for n in LIBRARIES if f'lib/arm64-v8a/{n}' not in names]


def check_signature(apk: str, tools: Path) -> list:
    out = subprocess.run([str(tools / 'apksigner.bat'), 'verify', '--print-certs', apk], capture_output=True, text=True, shell=False)
    m = re.search(r'certificate SHA-256 digest: ([0-9a-f]{64})', out.stdout)
    if out.returncode or not m:
        return [f'apksigner verify failed: {(out.stdout + out.stderr).strip()[:300]}']
    actual = m.group(1)
    print(f'signing key SHA-256: {actual}')
    known = known_fingerprint()
    if not known:
        return ['tools\\android\\signing_fingerprint.txt has no fingerprint']
    if actual != known:
        return [f'signed with another key: {actual}, tools\\android\\signing_fingerprint.txt says {known}']
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('apk')
    ap.add_argument('--build-tools', required=True, help="the Android SDK's build-tools folder (aapt, apksigner)")
    ap.add_argument('--no-signature', action='store_true')
    ap.add_argument('--version-file', default=str(ROOT / 'android' / 'VERSION'))
    args = ap.parse_args()
    tools = Path(args.build_tools)
    dump = subprocess.run([str(tools / 'aapt.exe'), 'dump', 'xmltree', args.apk, 'AndroidManifest.xml'],
                          capture_output=True, text=True)
    if dump.returncode:
        print(f'check_apk: FAIL aapt: {dump.stderr.strip()[:300]}')
        return 1
    version = Path(args.version_file).read_text(encoding='ascii').strip()
    fails = check_manifest(dump.stdout, version)
    fails += check_libraries(args.apk)
    if not args.no_signature:
        fails += check_signature(args.apk, tools)
    for f in fails:
        print(f'check_apk: FAIL {f}')
    if not fails:
        print(f'check_apk: ok ({version}, versionCode {version_code(version)}, configChanges and hasFragileUserData as asked, '
              f'{len(LIBRARIES)} native libraries present)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
