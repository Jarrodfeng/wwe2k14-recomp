#version 450

//============================================================================================================
//
//
//                  Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// Snapdragon(TM) Game Super Resolution 1 (SGSR1), single-pass fragment shader.
// Source: https://github.com/SnapdragonStudios/snapdragon-gsr (SnapdragonGameStudios/snapdragon-gsr), branch main,
//   https://raw.githubusercontent.com/SnapdragonGameStudios/snapdragon-gsr/main/sgsr/v1/include/glsl/sgsr1_shader_mobile.frag
// License of the repository (LICENSE, BSD 3-Clause):
//
//   Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are met:
//
//   1. Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright notice,
//      this list of conditions and the following disclaimer in the documentation
//      and/or other materials provided with the distribution.
//
//   3. Neither the name of the copyright holder nor the names of its contributors
//      may be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
//   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//   POSSIBILITY OF SUCH DAMAGE.
//
//   SPDX-License-Identifier: BSD-3-Clause
//
// wwe2k14 port (Session 73) for the ReXGlue Vulkan presenter (present_effect=sgsr). Changes from the reference,
// declarations only; main() is the reference's body unchanged except where marked:
// - #version 450 (Vulkan GLSL) instead of 300 es; the reference's precision qualifiers are kept (mediump float).
// - Inputs come from the presenter's guest output effect interface: the source is a separate texture (binding 0) and
//   the presenter's immutable linear clamp sampler (binding 1), combined as ps0; the texture coordinate is derived from
//   gl_FragCoord with the output offset / 1 / output size push constants (the presenter's vertex shader only writes
//   the position), as the presenter's bilinear effect does.
// - ViewportInfo[0] = (1 / input width, 1 / input height, input width, input height) and EdgeSharpness (cvar
//   present_sgsr_edge_sharpness, reference default 2.0) are push constants.
// - The three highp vec2(...) constructor qualifiers in the textureGather offsets are dropped (not valid in
//   Vulkan GLSL; the operands are highp, so the result is too).
// - mode is const (textureGather's component argument must be a constant expression in Vulkan GLSL).
// Compile: py tools\perf\gen_spirv_header.py with --stage frag (vulkan_spirv/guest_output_sgsr_ps.h).
//
// Session 100 (settings parity S10a, the phone's SGSR UPSCALING in the new renderer's present): the SGSR 1 core above, as ported in
// the SDK (thirdparty/rexglue-sdk-src/src/ui/shaders/guest_output_sgsr.ps.glsl, Session 73), fed from our present's source coordinate
// (rdr_present.vert, already turned for the display's orientation) and finished by the game's display curve as rdr_present.frag.
precision mediump float;
precision highp int;

#define OperationMode 1
#define EdgeThreshold 8.0/255.0

// The present's constant block (renderer.cpp RecordPresent), read here from the fragment stage.
layout(set = 0, binding = 1, std140) uniform PresentConstants {
  highp vec4 rotation;
  highp vec4 uv_rect;
  highp vec4 viewport_info;  // 1 / source width, 1 / source height, source width, source height (texels of the whole image)
  highp vec4 sgsr;           // x: edge sharpness
} pc;
layout(set = 1, binding = 0) uniform mediump sampler2D source_image;
layout(set = 1, binding = 1) uniform sampler2D display_curve;
#define ps0 source_image
layout(location = 0) in highp vec2 in_uv;
layout(location = 0) out vec4 out_color;

float fastLanczos2(float x)
{
	float wA = x-4.0;
	float wB = x*wA-wA;
	wA *= wA;
	return wB*wA;
}
vec2 weightY(float dx, float dy,float c, float std)
{
	float x = ((dx*dx)+(dy* dy))* 0.55 + clamp(abs(c)*std, 0.0, 1.0);
	float w = fastLanczos2(x);
	return vec2(w, w * c);
}

void main()
{
	highp vec4 in_TEXCOORD0 = vec4(in_uv, 0.0, 0.0);
	highp vec4 ViewportInfo[1];
	ViewportInfo[0] = pc.viewport_info;
	float edgeThreshold = EdgeThreshold;
	float edgeSharpness = pc.sgsr.x;
	const int mode = OperationMode;  // wwe2k14 port: const (was int).

	vec4 color;
	if(mode == 1)
		color.xyz = textureLod(ps0,in_TEXCOORD0.xy,0.0).xyz;
	else
		color.xyzw = textureLod(ps0,in_TEXCOORD0.xy,0.0).xyzw;

	highp float xCenter;
	xCenter = abs(in_TEXCOORD0.x+-0.5);
	highp float yCenter;
	yCenter = abs(in_TEXCOORD0.y+-0.5);

	//todo: config the SR region based on needs
	//if ( mode!=4 && xCenter*xCenter+yCenter*yCenter<=0.4 * 0.4)
	if ( mode!=4)
	{
		highp vec2 imgCoord = ((in_TEXCOORD0.xy*ViewportInfo[0].zw)+vec2(-0.5,0.5));
		highp vec2 imgCoordPixel = floor(imgCoord);
		highp vec2 coord = (imgCoordPixel*ViewportInfo[0].xy);
		vec2 pl = (imgCoord+(-imgCoordPixel));
		vec4  left = textureGather(ps0,coord, mode);

		float edgeVote = abs(left.z - left.y) + abs(color[mode] - left.y)  + abs(color[mode] - left.z) ;
		if(edgeVote > edgeThreshold)
		{
			coord.x += ViewportInfo[0].x;

			vec4 right = textureGather(ps0,coord + vec2(ViewportInfo[0].x, 0.0), mode);
			vec4 upDown;
			upDown.xy = textureGather(ps0,coord + vec2(0.0, -ViewportInfo[0].y),mode).wz;
			upDown.zw  = textureGather(ps0,coord+ vec2(0.0, ViewportInfo[0].y), mode).yx;

			float mean = (left.y+left.z+right.x+right.w)*0.25;
			left = left - vec4(mean);
			right = right - vec4(mean);
			upDown = upDown - vec4(mean);
			color.w =color[mode] - mean;

			float sum = (((((abs(left.x)+abs(left.y))+abs(left.z))+abs(left.w))+(((abs(right.x)+abs(right.y))+abs(right.z))+abs(right.w)))+(((abs(upDown.x)+abs(upDown.y))+abs(upDown.z))+abs(upDown.w)));
			float std = 2.181818/sum;

			vec2 aWY = weightY(pl.x, pl.y+1.0, upDown.x,std);
			aWY += weightY(pl.x-1.0, pl.y+1.0, upDown.y,std);
			aWY += weightY(pl.x-1.0, pl.y-2.0, upDown.z,std);
			aWY += weightY(pl.x, pl.y-2.0, upDown.w,std);
			aWY += weightY(pl.x+1.0, pl.y-1.0, left.x,std);
			aWY += weightY(pl.x, pl.y-1.0, left.y,std);
			aWY += weightY(pl.x, pl.y, left.z,std);
			aWY += weightY(pl.x+1.0, pl.y, left.w,std);
			aWY += weightY(pl.x-1.0, pl.y-1.0, right.x,std);
			aWY += weightY(pl.x-2.0, pl.y-1.0, right.y,std);
			aWY += weightY(pl.x-2.0, pl.y, right.z,std);
			aWY += weightY(pl.x-1.0, pl.y, right.w,std);

			float finalY = aWY.y/aWY.x;

			float maxY = max(max(left.y,left.z),max(right.x,right.w));
			float minY = min(min(left.y,left.z),min(right.x,right.w));
			finalY = clamp(edgeSharpness*finalY, minY, maxY);

			float deltaY = finalY -color.w;

			//smooth high contrast input
			deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

			color.x = clamp((color.x+deltaY),0.0,1.0);
			color.y = clamp((color.y+deltaY),0.0,1.0);
			color.z = clamp((color.z+deltaY),0.0,1.0);
		}
	}

	// The game's display curve, as rdr_present.frag.
	ivec3 index = ivec3(clamp(color.rgb, 0.0, 1.0) * 255.0 + 0.5);
	out_color = vec4(texelFetch(display_curve, ivec2(index.r, 0), 0).r, texelFetch(display_curve, ivec2(index.g, 0), 0).g,
	                 texelFetch(display_curve, ivec2(index.b, 0), 0).b, 1.0);
}
