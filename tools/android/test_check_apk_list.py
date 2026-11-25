#!/usr/bin/env python3
"""S110 ANDROIDREADY twin: check_apk.shader_list_fails on fixture bytes (no real APK), and GameActivity.java installs the list before SDL starts.
Planted: an APK with no assets/shader_list.txt (every APK before this lane went unchecked for it) and one packed from another list must FAIL.
  py -I tools\\android\\test_check_apk_list.py
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_apk as c

L = c.SHADER_LIST
tree = b'S 0123456789ABCDEF VS 0 exact\nH rdr 3 1 0\nR 00\n'
cases = [
    ({L}, tree, tree, True),                                        # packed from this tree
    (set(), None, tree, False),                                     # planted: the list not packed (what nothing checked before)
    ({L}, b'S 0123456789ABCDEF VS 0 exact\n', tree, False),         # planted: packed from another (older) list
    (set(), None, None, True),                                      # a tree with no list: nothing to carry
]
bad = 0
for names, packed, t, ok in cases:
    got = not c.shader_list_fails(names, packed, t)
    if got != ok:
        bad += 1
        print(f'FAIL names={sorted(names)} packed={packed!r}: expected {"pass" if ok else "fail"}')

# S110 ANDROIDREADY review: the zip side (check_shader_list reads the entry from a real APK-shaped zip) and the wiring (main calls it):
# without these a check_apk.py that never runs the check, or reads the wrong entry, still passed the cases above.
import inspect
import tempfile
import zipfile
with tempfile.TemporaryDirectory() as d:
    tree_file = Path(d) / 'shader_list.txt'
    tree_file.write_bytes(tree)
    for i, (packed, ok) in enumerate(((tree, True), (None, False), (tree + b'R 01\n', False))):
        apk = Path(d) / f'a{i}.apk'
        with zipfile.ZipFile(apk, 'w') as z:
            z.writestr('classes.dex', b'')
            if packed is not None:
                z.writestr(L, packed)
        if (not c.check_shader_list(str(apk), tree_file)) != ok:
            bad += 1
            print(f'FAIL check_shader_list on a zip with packed={packed!r}: expected {"pass" if ok else "fail"}')
if 'check_shader_list(args.apk' not in inspect.getsource(c.main):
    bad += 1
    print('FAIL check_apk.main does not call check_shader_list(args.apk, ...): every APK goes unchecked for the list')

# The phone side: GameActivity.onCreate installs the list BEFORE super.onCreate (SDL starts the native thread there: Import / StoreInit read it).
java = (HERE.parent.parent / 'android' / 'java' / 'io' / 'github' / 'jarrodfeng' / 'wwe2k14' / 'GameActivity.java').read_text(encoding='utf-8')
call = java.find('if (!fromArgsFile) installShaderList(files);')
sup = java.find('super.onCreate(savedInstanceState);')
if call < 0 or sup < 0 or call > sup:
    bad += 1
    print('FAIL GameActivity.java: installShaderList(files) is not called before super.onCreate')
if 'getAssets().open("shader_list.txt")' not in java or 'new File(files, "shader_list.txt")' not in java:
    bad += 1
    print('FAIL GameActivity.java: installShaderList does not copy assets/shader_list.txt to <files>/shader_list.txt')

print('check_apk shader list twin: ' + ('FAIL' if bad else f'ok ({len(cases)} cases + the GameActivity order)'))
sys.exit(1 if bad else 0)
