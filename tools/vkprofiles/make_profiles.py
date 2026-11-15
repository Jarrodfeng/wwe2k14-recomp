#!/usr/bin/env python3
r"""S102 lane U3: writes the simulated-card profiles for the Khronos profiles layer (VK_LAYER_KHRONOS_profiles) next to this file.

  py -I tools\vkprofiles\make_profiles.py [--out DIR] [--check]     (--check: fail if the committed JSONs differ from what this writes)

SOURCES AND CONFIDENCE (CLAUDE.md rule 4): the numbers are what public gpuinfo-style reports (vulkan.gpuinfo.org device reports for the card families,
the vendors' published limits) show for each family, written from memory of those reports. They were NOT fetched during this lane (a web search did
not reach a device report): every value is [unconfirmed against a real report]. The profiles exist for the SHAPE of each family (the feature and
format holes the renderer's floor and fallbacks must survive), not exact limits. A profile only lowers what the real device offers; the layer cannot add.

  amd_vega_680m  AMD Radeon Vega / 680M class: Vulkan 1.3, BC yes, ASTC no, no D24S8 (AMD has no 24-bit depth), D32S8 yes.
  intel_uhd_620  Intel UHD 620 (the Surface Pro 6): Vulkan 1.2 here (1.3 on current drivers), BC yes, ASTC no, D24S8 not offered, D32S8 yes.
  mali_g615      Arm Mali-G615 (the black-title player's phone class): Vulkan 1.1 (Android baseline), BC NO (the hole that matters), D32S8 yes, no D24S8. The real card has ASTC + ETC2, but the layer cannot ADD what this PC's 4090 lacks (vkCreateDevice refuses it), so the profile leaves ASTC out: the ASTC path is NOT covered by this lane.
  plant_no_rgba16f  planted twin: the Intel profile with RGBA16F render + sample removed. The renderer's floor must refuse it, loudly.
"""
import argparse
import json
import sys
from pathlib import Path

F = 'VK_FORMAT_FEATURE_'
SAMPLED, BLIT_SRC, BLIT_DST, LINEAR, SRC, DST, COLOR, BLEND, DEPTH = (F + n for n in (
    'SAMPLED_IMAGE_BIT', 'BLIT_SRC_BIT', 'BLIT_DST_BIT', 'SAMPLED_IMAGE_FILTER_LINEAR_BIT', 'TRANSFER_SRC_BIT', 'TRANSFER_DST_BIT',
    'COLOR_ATTACHMENT_BIT', 'COLOR_ATTACHMENT_BLEND_BIT', 'DEPTH_STENCIL_ATTACHMENT_BIT'))

TEX = [SAMPLED, BLIT_SRC, LINEAR, SRC, DST]                 # a sampled, filterable, copyable picture format
RENDER = TEX + [COLOR, BLEND, BLIT_DST]                     # + render and blend
DEPTHF = [SAMPLED, SRC, DST, DEPTH, BLIT_SRC]


def fmt(optimal, vertex=True):
    return {'VkFormatProperties': {'linearTilingFeatures': [SRC, DST], 'optimalTilingFeatures': sorted(optimal),
                                   'bufferFeatures': [F + 'VERTEX_BUFFER_BIT'] if vertex else []}}


# Every colour format src/renderer maps to (grep VK_FORMAT_ in src/renderer). A format missing from a profile is UNSUPPORTED to the layer, so this list is the
# set the profiles keep; the planted twin removes one. Plain: render + blend + filter. Plain32: 32-bit float (no filter, no blend on most cards). Int: no filter / blend.
PLAIN = ['R8G8B8A8_UNORM', 'R8G8B8A8_SNORM', 'B8G8R8A8_UNORM', 'R8_UNORM', 'R8_SNORM', 'R8G8_UNORM', 'R5G6B5_UNORM_PACK16',
         'A1R5G5B5_UNORM_PACK16', 'A2B10G10R10_UNORM_PACK32', 'R16G16B16A16_SNORM', 'R16G16B16A16_UNORM', 'R16G16_UNORM', 'R16G16_SNORM', 'R16_UNORM', 'R16_SNORM',
         'R16G16B16A16_SFLOAT', 'R16G16_SFLOAT', 'R16_SFLOAT']
PLAIN32 = ['R32G32B32A32_SFLOAT', 'R32G32_SFLOAT', 'R32_SFLOAT']
INTS = ['R8G8B8A8_UINT', 'R8G8B8A8_SINT', 'R8G8_UINT', 'R8G8_SINT', 'R8_UINT', 'R8_SINT', 'A8B8G8R8_UINT_PACK32', 'A8B8G8R8_SINT_PACK32', 'A2B10G10R10_UINT_PACK32',
        'R16G16B16A16_UINT', 'R16G16B16A16_SINT', 'R16G16_UINT', 'R16G16_SINT', 'R16_UINT', 'R16_SINT', 'R32G32B32A32_UINT',
        'R32G32B32A32_SINT', 'R32G32B32_UINT', 'R32G32B32_SINT', 'R32G32_UINT', 'R32G32_SINT', 'R32_UINT', 'R32_SINT']


# Formats a vertex stream reads (real cards offer vertex fetch for these; not for 5-6-5 / 4-4-4-4 / 1-5-5-5).
VERTEX = {'R8G8B8A8_UNORM', 'R8G8B8A8_SNORM', 'R8_UNORM', 'R8_SNORM', 'R8G8_UNORM', 'A2B10G10R10_UNORM_PACK32', 'R16G16B16A16_SNORM', 'R16G16B16A16_UNORM', 'R16G16_UNORM',
          'R16G16_SNORM', 'R16_UNORM', 'R16_SNORM', 'R16G16B16A16_SFLOAT', 'R16G16_SFLOAT', 'R16_SFLOAT', 'R8G8B8A8_UINT', 'R8G8_UINT', 'R8_UINT', 'R16G16B16A16_UINT',
          'R16G16_UINT', 'R16_UINT', 'R32G32B32A32_UINT', 'R32G32B32_UINT', 'R32G32_UINT', 'R32_UINT'}


def formats(bc, astc, d24, rgba16f=True):
    f = {}
    for n in PLAIN:
        f['VK_FORMAT_' + n] = fmt(RENDER, n in VERTEX)
    f['VK_FORMAT_B4G4R4A4_UNORM_PACK16'] = fmt(TEX, False)   # no render on the desktop card this runs on, so none in the profiles
    for n in PLAIN32:
        f['VK_FORMAT_' + n] = fmt([SAMPLED, BLIT_SRC, SRC, DST, COLOR, BLIT_DST], True)
    for n in INTS:
        f['VK_FORMAT_' + n] = fmt([] if n.startswith('R32G32B32_') else [SAMPLED, SRC, DST, COLOR], n in VERTEX)
    f['VK_FORMAT_R32G32B32_SFLOAT'] = fmt([], True)   # vertex input only (the game's position streams)
    if not rgba16f:
        f['VK_FORMAT_R16G16B16A16_SFLOAT'] = fmt([SRC, DST])
    f['VK_FORMAT_D32_SFLOAT_S8_UINT'] = fmt(DEPTHF, False)
    f['VK_FORMAT_D24_UNORM_S8_UINT'] = fmt(DEPTHF if d24 else [SRC, DST], False)
    if bc:
        for n in ('BC1_RGBA_UNORM_BLOCK', 'BC2_UNORM_BLOCK', 'BC3_UNORM_BLOCK', 'BC4_UNORM_BLOCK', 'BC5_UNORM_BLOCK'):
            f['VK_FORMAT_' + n] = fmt(TEX, False)
    if astc:
        for n in ('ASTC_4x4_UNORM_BLOCK', 'EAC_R11_UNORM_BLOCK', 'EAC_R11G11_UNORM_BLOCK', 'ETC2_R8G8B8A8_UNORM_BLOCK'):
            f['VK_FORMAT_' + n] = fmt(TEX, False)
    return f


def limits(**kw):
    base = {'maxImageDimension2D': 16384, 'maxBoundDescriptorSets': 8, 'maxColorAttachments': 8, 'maxUniformBufferRange': 65536,
            'maxPushConstantsSize': 128, 'maxPerStageDescriptorSamplers': 1048576, 'maxSamplerAnisotropy': 16.0}
    base.update(kw)
    return base


def profile(name, api, label, lim, bc, astc, d24, depth_clamp=True, rgba16f=True):
    cap = {
        'features': {'VkPhysicalDeviceFeatures': {'textureCompressionBC': bc, 'textureCompressionASTC_LDR': astc, 'depthClamp': depth_clamp, 'samplerAnisotropy': True, 'independentBlend': True}},
        'properties': {'VkPhysicalDeviceProperties': {'limits': lim}},
        'formats': formats(bc, astc, d24, rgba16f),
    }
    return {'$schema': 'https://schema.khronos.org/vulkan/profiles-0.8.2-304.json#', 'capabilities': {'caps': cap},
            'profiles': {name: {'version': 1, 'api-version': api, 'label': label,
                                'description': 'Our own S102 approximation of a card family (tools/vkprofiles/make_profiles.py); values unconfirmed.', 'capabilities': ['caps']}}}


# What the renderer's `[rdr] device:` line must say under each profile (cards_check.py parses it). The layer applies the API version, the features (BC / ASTC) and the
# limits / formats; it does NOT simulate deviceName or driverVersion (tried: the log still showed the 4090's), so the checkable fingerprint is Vulkan version + BC + ASTC.
# Intel is given 1.2.0 (not 1.3.0) so the check can tell the two desktop profiles apart; Gen9 drivers have reported both.
META = {
    'amd_vega_680m': {'vulkan': '1.3.0', 'bc': 1, 'astc': 0},
    'intel_uhd_620': {'vulkan': '1.2.0', 'bc': 1, 'astc': 0},
    'mali_g615': {'vulkan': '1.1.0', 'bc': 0, 'astc': 0},
}
# maxUniformBufferRange: a profile can only LOWER what the real device offers (this PC's RTX 4090 reports 65536), so no value above 65536 is written (a "greater than
# device" layer warning fails the check). Intel's real 128 MB range is therefore not modelled.
PROFILES = {
    'amd_vega_680m': profile('VP_WWE_amd_vega_680m', '1.3.0', 'AMD Radeon Vega / 680M class', limits(), bc=True, astc=False, d24=False),
    'intel_uhd_620': profile('VP_WWE_intel_uhd_620', META['intel_uhd_620']['vulkan'], 'Intel UHD 620 (Surface Pro 6)', limits(), bc=True, astc=False, d24=False),
    'mali_g615': profile('VP_WWE_mali_g615', '1.1.0', 'Arm Mali-G615 (Android, Vulkan 1.1)', limits(maxPerStageDescriptorSamplers=500000), bc=False, astc=False, d24=False),
    'plant_no_rgba16f': profile('VP_WWE_plant_no_rgba16f', '1.3.0', 'PLANTED FAULT: Intel UHD 620 without RGBA16F render', limits(), bc=True, astc=False, d24=False, rgba16f=False),
    # PLANTED FAULT: a push-constant range (32 bytes) below the 64 the renderer pushes but above nothing the floor checks: only the validation layer, sitting ABOVE the
    # profiles layer, can see it.
    # PLANTED FAULT: a limit RAISED above what the real device offers (the layer can only lower): it must produce a PROFILES WARNING that cards_check fails on.
    'plant_raised_limit': profile('VP_WWE_plant_raised_limit', '1.3.0', 'PLANTED FAULT: maxUniformBufferRange 128 MB', limits(maxUniformBufferRange=134217728), bc=True, astc=False, d24=False),
    'plant_small_push': profile('VP_WWE_plant_small_push', '1.3.0', 'PLANTED FAULT: maxPushConstantsSize 32', limits(maxPushConstantsSize=32), bc=True, astc=False, d24=False),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(Path(__file__).resolve().parent))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    bad = 0
    for name, p in PROFILES.items():
        path = Path(a.out) / f'{name}.json'
        text = json.dumps(p, indent=1, sort_keys=True) + '\n'
        if a.check:
            if not path.exists() or path.read_text() != text:
                print(f'FAIL  {path} differs from the generator')
                bad += 1
        else:
            path.write_text(text)
            print('wrote', path)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
