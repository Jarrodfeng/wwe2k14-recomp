#!/usr/bin/env python3
"""S105 ADEV twin: check_apk.release_version_fails on fixture bytes (no real .so). Planted: today's string `v0.4.0+dev` must FAIL."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_apk as c

V = 'v0.4.0'
cases = [
    (b'xx\0wwe2k14 version v0.4.0+dev\0yy', False),   # the planted twin (what a test APK and the pre-fix release carried)
    (b'xx\0v0.4.0\0yy', True),                        # a release
    (b'xx\0unrelated\0', False),                      # no version at all: cannot tell
    (b'v0.4.0\0v0.4.0+dev\0', False),                 # both present: still a dev build
]
bad = 0
for data, ok in cases:
    got = not c.release_version_fails(data, V)
    if got != ok:
        bad += 1
        print(f'FAIL {data!r}: expected {"pass" if ok else "fail"}')
print('check_apk release twin: ' + ('FAIL' if bad else f'ok ({len(cases)} cases)'))
sys.exit(1 if bad else 0)
