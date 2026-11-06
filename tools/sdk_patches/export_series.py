"""Export our ReXGlue SDK changes as a verified chain of diffs (Session 25).

The SDK fork (thirdparty\\rexglue-sdk-src, branch wwe2k14-local) is a separate git repository that this repo ignores, so
these diffs are the only copy of our SDK changes that travel with the project. The older patches\\rexglue-sdk\\00NN-*.patch
files are kept as the historical record, but applied in order they do NOT rebuild the branch (some are summaries,
commits 87-103 are missing, and 35 merge commits can't be expressed as per-commit patches).

This writes patches\\rexglue-sdk\\full\\NN-<session>.diff: tree-to-tree diffs between the last pinned SDK build of each
session (tools\\sdk_pins.json), so merges are included. Then it checks the chain: upstream base + every diff in order
must give exactly the branch head's tree (a temporary git index; no files in the SDK checkout are touched).

  py tools\\sdk_patches\\export_series.py           export + verify (run after any SDK change you keep)
  py tools\\sdk_patches\\export_series.py --check   verify the committed diffs only
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

R = Path(__file__).resolve().parents[2]
SDK = R / 'thirdparty' / 'rexglue-sdk-src'
OUT = R / 'patches' / 'rexglue-sdk' / 'full'
BASE = 'f5337cd'          # upstream rexglue/rexglue-sdk commit the fork starts from
BRANCH = 'wwe2k14-local'
# The last pinned build of each session (tools\sdk_pins.json), in order. Add a line per new session.
STEPS = [
    ('sessions03-07c', 'd2512163c84bfe1dfdbd6ae62964650b72dd25bc'),
    ('session08', '728667b8531b99288e51cde46f9f6c528580ae79'),
    ('session14', '32fae132798bd73ef98386ccaf9fe4148efa560b'),
    ('session15', 'f77004c2896740c30b958f0c2aeb39bac085f7b9'),
    ('session16', '69a47ab172944d4bf2546f57d621dfc84e179683'),
    ('session18', '4057aa9912c8303530458578f79c6562c607dc63'),
    ('session19', '2d9a84f0718e5c19148dfdc54a9d49c0570711cf'),
    ('session20', 'a63272c9e3a9bcda8c637fff647722cb68217b48'),
    ('session21', 'fef3684baf86d68e9f2563a2d9d3b74ea9906889'),
    ('session22', '63b87cfa2981455d61b5e22f1908f70da0780041'),
    ('session23', 'cf61391d3b4090206fc4b37708312b0c7a26b2b9'),
    ('session24', '6a2666fe6dadfe587bf50c4e84caa0b69858e538'),
    ('session26', '2aaec24b364c7d5494f6469089fd85047628a793'),
    ('session26b', '7412a803920fa9ecf587caa8a69d71db3f687a50'),
    ('session27', '8794393499be25948763faa8c0b0831566a901d3'),
    ('session27_5', 'e5ebb5bbbb41c2b970f378ad60a72ce37f1fcefc'),
    ('session30', '0750a5bd1b4a53b37b4f06a17ae6bfbc1a1985cd'),
    ('session32', 'b82208db3eaf6fd068298a57c1a6fc9f7e22efe6'),
    ('session33', '5cdb5475bb5c2c71b122b0752e9a93a4a654e5b9'),
    ('session37', '48c1e6cc7ff26a3f1bd688011b431a9529eaf26a'),
    ('sessions38-53', 'e0f72778d351da15341cf1b7e963d4f39ee68e4e'),
    ('session54', 'ec136eb164d9aef10fb99d8e1cb7087f28691f13'),
    ('session54b', '50c9137a0a1f166989d82696ed97407ced02d1e1'),
    ('session55', '1ee937091702f4780716c3c85116896b88d1559a'),
    ('session56', '3d91313879f72362de6c0df157a0f7bc1cec7fde'),
    ('session57', '3313bb8a7ca2200a4dec568cc904bd14740311ef'),
    ('session58', '0ef0000aad71fb8def873b61c54af35c856b88ac'),
    ('session59', 'e03ea01780e426550d82fcb446c72dedc8d2084c'),
    ('session60', '8bd883ab9ea10777ca96c9d8aff4ca028d961958'),
    ('session61', 'c33dc62ba9175bcf34ceb3571bf972eaed5ff85b'),
    ('session63-64-android', '61ca96d9f003e2f4cb3f3d6c4e780269f16f3e6d'),
    ('session66-native2', '12af82b49813c74787e7cf4a96910a5a6522b31c'),
    ('session69-redblock-stutter', '0bdef435e92b7a58faba341258300d7c2e293cef'),
    ('session70-precompile', 'f3d44bdd38c2ad06c2f59ba7aaf0d90e0fc152e1'),
    ('session71-android', 'd8053f39911e0bd7b7de2ef0ee2f01ea4b1b0015'),
    ('session72-post', 'e1abce8b4fb2f41afc8e0d52ccb0c2430748994b'),
    ('session72-passes', '49465d381ad6f5631a6101a7ee4a14d707be04f9'),
    ('session73-gpu', 'aa1fd81859b7211c2805e1c92a328acb6ce13d99'),
    ('session74-batch', 'a0a1d5646a2eee31fa6fc6ebcc0212257caf4c74'),
    ('session76-silent-audio', '898fdc87b8191844d6f252cc15f5774c970df515'),
    ('session76-ftz-diag', '102f2d9b226b4eb1c3f5ad4b8016a438c141e82a'),
    ('session77-xdet', '4eacaff4e593ab0cfb9b3f279909f61a8848ac3e'),
    ('session78-native-frame', '6fd63c048f0d458364fbfceaf4a7104194b239df'),
    ('session79-nfc', 'd1fd953739776718a7795e3219d9cbb1bc2e29f7'),
    ('session79-nfd', '752e026ac3d641109427edfdca5e6feae9a48a25'),
    ('session79-glr', '3426e5f62b7a0edfc738e9efb1b218c900948011'),
    ('session79-crowd', '38a27fb9926604400713ff8045d3258c9423fb0f'),
    ('session79-aliascensus', 'f05f55b137a1681baed2936024d71c6c1d609228'),
    ('session79-cubefaces', '5ef4e50020b6030857d4649add9bbb7ec58839cd'),
    ('session79-cubeonly', 'd700b31f538a91fb157b86166d8e72167f063af1'),
    ('session79-aliasdirect', 'f1a55720cb5a8a2443ce667f1819c964c03301ba'),
    ('session79-defermem', '4b81d3e0133ff637c67a61e4e393c06e4a78b837'),
    ('session79-reviewfixes', '647b4595196bd0c33d8dc30c53cb426fedc87b32'),
    ('session79-breakstack', 'b9bccc407c86b5c24a432b73534056670b0ec53a'),
    ('session79-readbacknarrow', '3595d66e25e4a6566c7b47de6d910394c342588d'),
    ('session79-texlog', '892e60c03c62d32b3ec36ce03c0bb1b2a01de010'),
    ('session79-readbackgap', 'c4519efdd469fabcdacfd230361eac1d95250098'),
    ('session79-deferdepth', '6aa7858ab64dcc4a40ea9da569658d4056eea6e1'),
    ('session79-deferfull', '91d4dc71ba23b5ea07c806079ec35786fbddc885'),
    ('session79-aliasrgba16', 'b7f0b78aefb9bc4d77062c18bafbe7db313cb544'),
    ('session79-aliaslazy', '55e7849356b7323e31f5e83e41576e08a1d360f5'),
    ('session79-reviewfixes2', '3123f634c23b196faa5bc5beebdfea3abfa1de14'),
    ('session80-bootpipes', '895cc9045ed85ba4b2aa79efe6cb1fe3efd51764'),
    ('session80-half', 'f7190db59e14ca628f7226aad7318bb0fdc6bcb1'),
    ('session80-siblings', 'e799310c06be03971abeafeef8d3de959583411f'),
    ('session80-raise', 'b73d515b77479e586a60999c5fd33562b9d73477'),
    ('session80-shadowhalf', '392e193720f319e84f68212f44f04b9fcaef84e0'),
    ('session80-shadowstagger', 'e0b6ee238fac4564ec928fce392605030cdf9bbd'),
    ('session80-swapepilogue', '6aa9e602c4593989c4a41480ac2e48105bd0a445'),
    ('session80-swapskip', '3c28cfe1ec5192211f485b5751519dbbc5ef6235'),
    ('session80-owncrowd', 'cf2b2dec208ffb33e1143aeb52b1d1abccfd30ef'),
    ('session80-anyconst', '9fd14512b4643207c3c9e64ba82f541680757ad0'),
    ('session80-framestruct', '329487a26dbf34cf07c2365f91b34a20a4d090c3'),
    ('session80-copygraph', 'cab86cb015377293efaaf3495d90f9414e532a61'),
    ('session80-deadpass', '0af816bdd6d682a1e011167c7c87d63e46d923c7'),
    ('session80-servecopy', '83df32f5c0c39e9a7945837804d7ac43978eb587'),
    ('session80-arena', 'e7c82b0ffce664cf2af0f8e825b71891f079fb8a'),
    ('session80-arena2', '538f7ca2b66fe16109e91843a9384cd5d27a2a8d'),
    ('session80-clears', 'b945e920c27874bb3470e4ba385ea7594e9c2297'),
    ('session80-probe', '491a1e001aa085c041f70f067e00623232ec6d5f'),
    ('session80-bounds', 'bd5a9730eb6b1d5627f3988cf5bab9d4615c569c'),
    ('session80-serveahead', 'bc6a731980dfd2283b31cf65770fc2583b5dc86b'),
    ('session80-boundsfast', 'a55876b32186a1b04da41de50b025a4b292fdf76'),
    ('session80-ownsteps', '561b4bfe0d173a87538e5481361eafcc02115415'),
    ('session80-ownpasses', '623cc9842d1351a8146a5ba7f78be002e5114729'),
    ('session82-owned', 'a0ff9cf71b48c2d62ab9356bde0601a58ca6f05f'),
    ('session83-bugs', 'ded47622450c334d617894368ef60bd22a68bdb7'),  # gpu_wide_post_vs (b2991f8), keyboard stick keys
    ('session84-android', 'f527e636ba3f4293770d9315207e783e0012eec1'),  # owned frame on host RT: clear depth range, viewport tracking
    ('session85-settings', '86722be5bd70ef6fe989cdee2621a0f40252ec05'),  # gpu_dynamic_shadows / gpu_depth_of_field / gpu_scene_crowd_density
    ('session87-d3d-layer', '243729cc23d08370777ccb77babac344fabc9367'),  # the D3D layer: no ring, own input, own shaders, textures by object, chains (switches off)
    ('session89-posix-event', '958c1a923d9581f870a7957971337050fec872ef'),  # POSIX auto-reset events keep Win32 semantics (the phone pinfall freeze)
    ('session90-ownership', '224e5d66781f79003f19c2d4f62a4e4c4efe70af'),  # TRUE NATIVE 100% on the bench: carried passes, crowd restore, guest memory 0 bytes, read-back write-back, stable thinning, Adreno interface, load-op flag scope
    ('session92-whole-game', '2ec4d65863cd7ebfbf2e0090110ca305d10212de'),  # TRUE NATIVE across the whole game: frozen window, M0 counters, stencil volumes / ladder, lighting cubes, Jericho burst + held swaps, damage-map resets, M2a owned-only latch (off)
    ('session92-free', '8b81e2c45a28a341874b7031ff89cedc37ce5a7d'),  # free from the 360: M2 no truth path / EDRAM, M3 resolves + present as data, M4 the cut, M5 sync, M6 the translation store, split-aware vertex shaders
    ('session93-perf-compat', '3e88dc162e1017dbdff8cc206e577a25f2b5800e'),  # per-draw storage windows (Adreno 720), quirks table, BC1->ASTC, tiler passes, typed memo reuse, lean crowd, inline staging, display-timing pacing, M6 phone precompile tail, EDS (off), boot barrier fix
    ('session93-cull-merge', 'decd7e4923adc40687c76e7e042e5baf1732eaf0'),  # phase-0 census (gpu_d3d_phase0_census), the frustum cull of our arena draws (gpu_d3d_cull_ours), the real crowd merge (first-member state + checkpoint markers), all off
    ('session93-mali-post', '0ba44dde857cd134ea11b0ffd955b85d4872c8c2'),  # TRUE NATIVE no longer needs RGBA32F blending (Mali-G720 r49p1 black screen); post warnings name the missing part
    ('session93-hoist-barrier', 'a89047e045acf7242d1e8438b966a0043a918211'),  # the hoist stream's closing barrier names every read (the stuck reversal indicator on Adreno with storage windows); compute writes carry SHADER_WRITE
    ('session94-speedrun', '20594b5913f0e48757124302e64b7b872afbe73f'),  # Session 94: boot self-check of required formats + features, no-GS typed draws, format lists / transient depth / tiler scene reset, pre-rotation, BC-less textures round 2, lean bind, instancing (off), direct uploads (off), phone footprint, dynamic resolution under TRUE NATIVE (off), Vulkan 1.1 layout features, decoupling multiview + blend (off)
]


def git(*args, env=None, text=True):
    r = subprocess.run(['git', '-C', str(SDK), *args], capture_output=True, text=text, env=env)
    if r.returncode:
        sys.exit(f'git {" ".join(args)} failed: {r.stderr if text else r.stderr.decode(errors="replace")}')
    return r.stdout


def verify():
    files = sorted(OUT.glob('*.diff'))
    head_tree = git('rev-parse', f'{BRANCH}^{{tree}}').strip()
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / 'index'))
        git('read-tree', BASE, env=env)
        for f in files:
            r = subprocess.run(['git', '-C', str(SDK), 'apply', '--cached', '--binary', str(f)], capture_output=True,
                               text=True, env=env)
            if r.returncode:
                sys.exit(f'FAIL: {f.name} does not apply: {r.stderr.strip()[:300]}')
        tree = git('write-tree', env=env).strip()
    if tree != head_tree:
        sys.exit(f'FAIL: base + {len(files)} diffs = tree {tree}, but {BRANCH} is {head_tree}')
    head = git('rev-parse', '--short', BRANCH).strip()
    print(f'PASS: upstream {BASE} + {len(files)} diffs = {BRANCH} ({head}) exactly (tree {tree[:12]})')


def export():
    head = git('rev-parse', BRANCH).strip()
    if STEPS[-1][1] != head:
        sys.exit(f'{BRANCH} is at {head[:7]}, the last step is {STEPS[-1][1][:7]}: add a STEPS line for the new SDK state')
    prev = BASE
    for _, c in STEPS:  # each step must build on the one before
        if subprocess.run(['git', '-C', str(SDK), 'merge-base', '--is-ancestor', prev, c]).returncode:
            sys.exit(f'{prev[:7]} is not an ancestor of {c[:7]}')
        prev = c
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob('*.diff'):
        old.unlink()
    prev = BASE
    for i, (label, c) in enumerate(STEPS, 1):
        diff = git('diff', '--binary', '--full-index', prev, c, text=False)
        subjects = git('log', '--first-parent', '--format=  %h %s', f'{prev}..{c}').rstrip()
        header = (f'# ReXGlue SDK changes for WWE 2K14 ({label}): {prev[:12]}..{c[:12]} on {BRANCH}\n'
                  f'# Apply in order on upstream rexglue/rexglue-sdk {BASE} (git apply). Commits (first parent):\n'
                  + ''.join(f'#{line}\n' for line in subjects.splitlines()) + '\n')
        path = OUT / f'{i:02d}-{label}.diff'
        path.write_bytes(header.encode() + diff)
        print(f'{path.name}: {len(diff) // 1024} KB, {len(subjects.splitlines())} commits')
        prev = c
    verify()


if __name__ == '__main__':
    verify() if '--check' in sys.argv else export()
