r"""S102 lane G6 (review fixes 4 and 5): the GPU class table cannot drift, compile-time and in the python copies. 0 game runs.
  py -I tools\renderer\gpu_class_lint.py [--root DIR]
unit   builds tools\renderer\stamp_roles_test.cpp with -Werror=switch and runs it (every role but Unnamed / Other maps to a class; the scene-size pick gives frame_buffer to a 1280x720
       unnamed target next to a 736x1280 scene_color roster picture). Planted twins: the old pick (test plant 1) must fail; stamp_roles.h with `case LightStrip` deleted must NOT COMPILE.
lint   the python copies against the C++: stamp_roles.h GpuClassOfRole cases vs gpu_other_census.BUILTIN_TABLE, api.h GpuClassName vs the renderer's roles line (renderer.cpp) vs the
       CLASSES lists in gpu_stamps_check.py and gpu_other_check.py, the SurfaceRole enum vs SurfaceRoleName, the ROLE_NAMES lists of the census tools, and reader.h ScreenShapedRule vs
       stamp_roles.h ScreenShaped16x9. Planted twin: 'feedback' dropped from one CLASSES list must be reported.
Exit 0 = pass."""
import argparse, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'


sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)


def read(root, rel):
    return read_path(os.path.join(root, rel))


def snake(camel):
    return re.sub(r'(?<!^)([A-Z])', r'_\1', camel).lower()


def cpp_role_table(header):
    """GpuClassOfRole: {role snake name: class name} from `case SurfaceRole::X [: case SurfaceRole::Y]: return ... kGpuZ` (a plant ternary reads its LAST kGpu)."""
    body = header[header.index('inline uint8_t GpuClassOfRole'):header.index('inline bool ScreenShaped16x9')]
    cls = api_classes_by_enum
    out = {}
    for m in re.finditer(r'((?:case SurfaceRole::\w+:\s*)+)return ([^;]*);', body):
        roles = re.findall(r'SurfaceRole::(\w+)', m.group(1))
        klass = re.findall(r'kGpu(\w+)', m.group(2))[-1]
        for r in roles:
            out[snake(r)] = klass
    return out


api_classes_by_enum = None


def api_lists(api):
    enum = re.search(r'enum GpuClass : uint8_t \{([^}]*)\}', api).group(1)
    enum_names = [e.strip().split(' ')[0] for e in enum.split(',') if e.strip()]
    enum_names = [e[len('kGpu'):] for e in enum_names if e != 'kGpuClassCount']
    names = re.findall(r'"([a-z]+)"', api[api.index('inline const char* GpuClassName'):api.index('static_assert', api.index('inline const char* GpuClassName'))])
    return enum_names, names


def line_classes(renderer):
    m = re.search(r'by role, mean ms a frame over %llu frames: ([^=]*) = work', renderer)
    return re.findall(r'([a-z]+) %\.3f', m.group(1))


def py_list(text):
    m = re.search(r'^CLASSES = \[([^\]]*)\]', text, re.M)
    return re.findall(r"'([a-z]+)'", m.group(1))


def py_table(census):
    m = re.search(r'BUILTIN_TABLE = \{(.*?)\}', census, re.S)
    return dict(re.findall(r"'(\w+)': '(\w+)'", m.group(1)))


def lint(root, plant=None):
    global api_classes_by_enum
    problems = []
    api = read(root, 'src/renderer/api.h')
    enum_names, names = api_lists(api)
    if len(enum_names) != len(names):
        problems.append('api.h: GpuClass has %d values, GpuClassName %d names' % (len(enum_names), len(names)))
    api_classes_by_enum = None
    header = read(root, 'src/renderer_adapter/stamp_roles.h')
    table = cpp_role_table(header)
    names_h = read(root, 'src/renderer_adapter/names.h')
    enum_body = names_h[names_h.index('enum class SurfaceRole'):names_h.index('};', names_h.index('enum class SurfaceRole'))]
    enum_body = re.sub(r'//[^\n]*', '', enum_body).split('{', 1)[1]
    enum_roles = [e.split('=')[0].strip() for e in enum_body.split(',') if e.strip()]
    snake_enum = [snake(e) for e in enum_roles if e != 'Count']
    roles_str = re.findall(r'"([a-z_0-9]+)"', names_h[names_h.index('inline const char* SurfaceRoleName'):names_h.index('static_assert', names_h.index('inline const char* SurfaceRoleName'))])
    if sorted(snake_enum) != sorted(roles_str):
        problems.append('names.h: SurfaceRole enum %s vs SurfaceRoleName %s' % (snake_enum, roles_str))
    for r in snake_enum:
        if r not in table:
            problems.append('stamp_roles.h GpuClassOfRole has no case for role %s' % r)
    for r, c in table.items():
        if r not in ('unnamed', 'other', 'count') and c.lower() == 'other':
            problems.append('role %s maps to other' % r)
    census = read(root, 'tools/renderer/gpu_other_census.py')
    pt = py_table(census)
    for r, c in table.items():
        if r in ('unnamed', 'other', 'count'):
            continue
        # the census maps roles to the python class names: Color -> colour
        want = {'Color': 'colour'}.get(c, c.lower())
        if pt.get(r) != want:
            problems.append('gpu_other_census.BUILTIN_TABLE[%s] = %r, stamp_roles.h says %s' % (r, pt.get(r), want))
    for r in pt:
        if r not in table:
            problems.append('gpu_other_census.BUILTIN_TABLE has a role %s the C++ table does not' % r)
    renderer = read(root, 'src/renderer/renderer.cpp')
    line = line_classes(renderer)
    api_py = sorted({'colour' if n == 'Color' else n.lower() for n in enum_names})
    if sorted(line) != api_py:
        problems.append('renderer.cpp roles line classes %s vs api.h %s' % (sorted(line), api_py))
    if sorted(n for n in names) != api_py:
        problems.append('api.h GpuClassName %s vs GpuClass %s' % (sorted(names), api_py))
    for rel in ('tools/renderer/gpu_stamps_check.py', 'tools/renderer/gpu_other_check.py'):
        text = read(root, rel)
        if plant == rel:
            text = text.replace(", 'feedback'", '', 1)
        lst = py_list(text)
        if lst != line:
            problems.append('%s CLASSES %s is not the roles line order %s' % (rel, lst, line))
    for rel in ('tools/shadow/shadow_census.py', 'tools/post/post_census.py'):
        if not os.path.exists(os.path.join(root, rel)):
            problems.append('%s missing' % rel)
            continue
        m = re.search(r'^ROLE_NAMES = \[([^\]]*)\]', read(root, rel), re.M)
        got = re.findall(r"'([a-z_0-9]+)'", m.group(1)) if m else []
        if got != roles_str:
            problems.append('%s ROLE_NAMES %s is not SurfaceRoleName %s' % (rel, got, roles_str))
    reader = read(root, 'src/renderer_adapter/reader.h')
    a = re.search(r'inline bool ScreenShapedRule\(uint32_t width, uint32_t height\) \{(.*?)\n\}', reader, re.S).group(1)
    b = re.search(r'inline bool ScreenShaped16x9\(uint32_t width, uint32_t height\) \{(.*?)\n\}', header, re.S).group(1)
    if re.sub(r'\s+', '', a) != re.sub(r'\s+', '', b):
        problems.append('reader.h ScreenShapedRule and stamp_roles.h ScreenShaped16x9 differ')
    return problems


def unit(root, tmp):
    src = os.path.join(root, 'src')
    test = os.path.join(root, 'tools', 'renderer', 'stamp_roles_test.cpp')
    exe = os.path.join(tmp, 'stamp_roles_test.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-Werror=switch', '-I', src, test, '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL unit: did not compile\n' + b.stdout[-1500:] + b.stderr[-1500:])
        return False
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print('FAIL unit: the test failed')
        return False
    p = subprocess.run([exe, '1'], capture_output=True, text=True)
    if p.returncode == 0:
        print('FAIL planted twin: the old scene pick (the first scene_color record, a 736x1280 roster picture) was not caught')
        return False
    print('twin 1 (the old scene-size pick): caught')
    # twin 2: the header with `case LightStrip` deleted must not compile under -Werror=switch
    plant = os.path.join(tmp, 'plant_src')
    os.makedirs(os.path.join(plant, 'renderer_adapter'), exist_ok=True)
    os.makedirs(os.path.join(plant, 'renderer'), exist_ok=True)
    for rel in ('renderer_adapter/adapter_frame.h', 'renderer_adapter/names.h', 'renderer/api.h'):
        open(os.path.join(plant, rel), 'w', encoding='utf-8').write(read(root, 'src/' + rel))
    h = read(root, 'src/renderer_adapter/stamp_roles.h')
    line = '    case SurfaceRole::LightStrip: return rdr::kGpuPrelight;\n'
    if line not in h:
        print('FAIL twin 2: the LightStrip case line was not found')
        return False
    open(os.path.join(plant, 'renderer_adapter', 'stamp_roles.h'), 'w', encoding='utf-8').write(h.replace(line, ''))
    q = subprocess.run([CLANG, '-std=c++20', '-Werror=switch', '-fsyntax-only', '-I', plant, test], capture_output=True, text=True)
    if q.returncode == 0:
        print('FAIL planted twin: the header with `case LightStrip` deleted still compiles')
        return False
    print('twin 2 (case LightStrip deleted): does not compile, as it must')
    return True


# S103 GT review: a source that is built in BOTH builds may call SetImageClass only under `#if !WWE2K14_STREAM()`: in the stream build a class also steers the binder's per-frame levers
# (colour prepass, half transparents, arena blend-off), so naming an image there changes the picture / the cost. The scene-map images (shadow / rim / plane / prelight / circles) were named
# in both builds before S103 and are the known set below; the stream-free-only files (src/renderer_host/) are exempt. Every other call under src/renderer_scene must sit in a guarded region.
LEGACY_BOTH = {'live_maps.cpp', 'live_map_chain.cpp', 'live_map_plane.cpp', 'live_map_prelight.cpp', 'live_map_rim.cpp'}


def unguarded_set_image_class(text):
    """[line numbers] of SetImageClass( calls that are not inside a `#if !WWE2K14_STREAM()` region (a stack of #if / #else / #endif; `#else` of the guard flips it to stream-only)."""
    stack = []   # one entry per open #if: 'off' (the stream-free branch), 'on' (the stream branch), or None (any other condition)
    out = []
    for i, ln in enumerate(text.split('\n'), 1):
        t = ln.strip()
        if re.match(r'#\s*if', t):
            stack.append('off' if re.match(r'#\s*if\s+!\s*WWE2K14_STREAM\(\)', t) else 'on' if re.match(r'#\s*if\s+WWE2K14_STREAM\(\)', t) else None)
        elif re.match(r'#\s*else', t) and stack:
            stack[-1] = {'off': 'on', 'on': 'off'}.get(stack[-1])
        elif re.match(r'#\s*endif', t) and stack:
            stack.pop()
        elif 'SetImageClass(' in ln and not t.startswith('//') and 'off' not in stack:
            out.append(i)
    return out


def set_image_class_problems(root, plant=False):
    problems = []
    d = os.path.join(root, 'src', 'renderer_scene')
    for name in sorted(os.listdir(d)):
        if not name.endswith(('.cpp', '.h')) or name in LEGACY_BOTH:
            continue
        with open(os.path.join(d, name), encoding='utf-8', errors='replace') as f:
            text = f.read()
        if plant and name == 'front_end.cpp':   # twin: the stream-free guard around the new names removed
            text = text.replace('#if !WWE2K14_STREAM()', '#if 1')
        for ln in unguarded_set_image_class(text):
            problems.append('src/renderer_scene/%s:%d: SetImageClass outside #if !WWE2K14_STREAM() (a source built in both builds may name images only in the stream-free build)' % (name, ln))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    ok = True
    with tempfile.TemporaryDirectory(prefix='gpu_class_lint_') as tmp:
        ok &= unit(root, tmp)
    probs = lint(root)
    for p in probs:
        print('FAIL lint: ' + p)
    ok &= not probs
    if not probs:
        print('ok   lint: the python copies agree with the C++ (class table, class lists, role names, screen rule)')
    twin = lint(root, plant='tools/renderer/gpu_other_check.py')
    if not twin:
        print('FAIL planted twin: dropping feedback from a CLASSES list was not reported')
        ok = False
    else:
        print('twin 3 (feedback dropped from a CLASSES list): caught: ' + twin[0][:100])
    sp = set_image_class_problems(root)
    for q in sp:
        print('FAIL lint: ' + q)
    ok &= not sp
    if not sp:
        print('ok   lint: SetImageClass in src/renderer_scene is only called under !WWE2K14_STREAM() (besides the known map files %s)' % ', '.join(sorted(LEGACY_BOTH)))
    twin2 = set_image_class_problems(root, plant=True)
    if not twin2:
        print('FAIL planted twin: removing the stream-free guard in front_end.cpp was not reported')
        ok = False
    else:
        print('twin 4 (guard removed in front_end.cpp): caught: ' + twin2[0][:110])
    print('RESULT ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
