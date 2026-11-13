r"""Planted faults for the judge's acceptance test (Session 101 lane 20): a temp copy of the effect folder with a known past bug put back.

  py -I tools\judge\plant.py alpha-false DEST_DIR     Session 100's far crowd: "alpha_test": false on every fragment variant of the 29 effects the template
                                                      infected (SESSION100_SUMMARY.md, 14:23 entry; RENDERER_ARC.md 2.1 "Effect descriptions (json)")
  py -I tools\judge\plant.py color-scale DEST_DIR EFFECT    a second fault, in another class: the effect's pixel shaders return half their colour (a .frag edit)
"""
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
BASE = HERE / 'src' / 'renderer' / 'shaders' / 'fx'

# SESSION100_SUMMARY.md, the 14:23 entry: "yAudience and 28 others had alpha_test false".
S100_EFFECTS = ['yAfterImage', 'yAntialiasing', 'yAudDefault', 'yAudience', 'yCircleShadow', 'yCombineTex', 'yComposition', 'yCopyFrBuffer', 'yDefocus', 'yDownScale',
                'yEmboss', 'yGamma', 'yHalation', 'yMonotone', 'yParticle', 'yPartsBlur', 'yPostBase', 'yRect', 'yRetroFilm', 'yRimLightMap', 'yScaleDownFB',
                'yScaleDownZ', 'yShadowMap', 'yShadowMapEdge', 'ySoftParticle', 'ySprite', 'yToneMap', 'yVelocity', 'yZPass']


def copy_base(dest: Path):
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(BASE, dest)


def alpha_false(dest: Path, effects=S100_EFFECTS):
    copy_base(dest)
    for e in effects:
        jp = dest / f'{e}.fx.json'
        spec = json.loads(jp.read_text(encoding='utf-8'))
        for v in spec.get('fragment', []):
            v['alpha_test'] = False
        jp.write_text(json.dumps(spec, indent=1), encoding='utf-8')


def color_scale(dest: Path, effect: str):
    """Half brightness: `outColor.rgb *= 0.5` before the closing brace of the effect's .frag (ySprite.frag and yRect.frag end with main())."""
    copy_base(dest)
    fp = dest / f'{effect}.frag'
    text = fp.read_text(encoding='utf-8')
    marker = text.rfind('}')
    if marker < 0 or 'outColor' not in text:
        sys.exit(f'FAIL {fp}: no closing brace / no outColor: this fault cannot be planted there')
    fp.write_text(text[:marker] + '  outColor.rgb = outColor.rgb * 0.5;\n' + text[marker:], encoding='utf-8')


def noop(dest: Path, effect: str):
    """A difference that changes no shader (a comment in the .frag): the judge must refuse it (\"the plant applies nothing\")."""
    copy_base(dest)
    fp = dest / f'{effect}.frag'
    fp.write_text(fp.read_text(encoding='utf-8') + '\n// judge selftest: a comment changes no SPIR-V\n', encoding='utf-8')


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'alpha-false':
        alpha_false(Path(sys.argv[2]))
        print(f'planted alpha_test:false in {len(S100_EFFECTS)} effects under {sys.argv[2]}')
    elif len(sys.argv) >= 4 and sys.argv[1] == 'color-scale':
        color_scale(Path(sys.argv[2]), sys.argv[3])
        print(f'planted half brightness in {sys.argv[3]}.frag under {sys.argv[2]}')
    else:
        sys.exit(__doc__)
