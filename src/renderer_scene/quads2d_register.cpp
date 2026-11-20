// Quads2DRegister (Session 103, lane I1): the 2D drawer's programs, made once on a renderer. Apart from quads2d_draw.cpp so the offline unit proof (no renderer linked) can use Append. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "quads2d_draw.h"

#include "../renderer/shaders/spv/rdr_movie_frag.h"
#include "../renderer/shaders/spv/rdr_movie_vert.h"
#include "../renderer/shaders/spv/rdr_uiglyph_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_flat_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_hsl_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_proj_vert.h"
#include "../renderer/shaders/spv/rdr_uiquad_vcol_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_vcol_proj_vert.h"
#include "../renderer/shaders/spv/rdr_uiquad_vcol_vert.h"
#include "../renderer/shaders/spv/rdr_uiquad_vert.h"

namespace rdr_scene::q2d {

Programs Register(rdr::Renderer& renderer, bool switch_on) {
  Programs p;
  if (!switch_on) return p;
  rdr::ProgramDesc d;
  d.name = "rdr_uiquad_flat";
  d.vertex_spirv = kSpv_rdr_uiquad_vert;
  d.vertex_bytes = sizeof(kSpv_rdr_uiquad_vert);
  d.fragment_spirv = kSpv_rdr_uiquad_flat_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_uiquad_flat_frag);
  p.rect = renderer.AddProgram(d);
  d.name = "rdr_movie";
  d.vertex_spirv = kSpv_rdr_movie_vert;
  d.vertex_bytes = sizeof(kSpv_rdr_movie_vert);
  d.fragment_spirv = kSpv_rdr_movie_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_movie_frag);
  p.movie = renderer.AddProgram(d);
  d.name = "rdr_uiquad";   // S103 I2: the sprite and its hue-shifted twin (the same vertex program as the rect)
  d.vertex_spirv = kSpv_rdr_uiquad_vert;
  d.vertex_bytes = sizeof(kSpv_rdr_uiquad_vert);
  d.fragment_spirv = kSpv_rdr_uiquad_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_uiquad_frag);
  p.sprite = renderer.AddProgram(d);
  d.name = "rdr_uiquad_hsl";
  d.fragment_spirv = kSpv_rdr_uiquad_hsl_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_uiquad_hsl_frag);
  p.sprite_hsl = renderer.AddProgram(d);
  d.name = "rdr_uiglyph";   // S103 I3: the text glyph (channel select + mask), the same vertex program
  d.fragment_spirv = kSpv_rdr_uiglyph_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_uiglyph_frag);
  p.glyph = renderer.AddProgram(d);
  d.name = "rdr_uiquad_flat_proj";   // S103 P3c: the matrix variant (rdr_uiquad_proj.vert) of the flat rect, the picture sprite and the glyph: the menu's matrix-placed rows
  d.vertex_spirv = kSpv_rdr_uiquad_proj_vert;
  d.vertex_bytes = sizeof(kSpv_rdr_uiquad_proj_vert);
  d.fragment_spirv = kSpv_rdr_uiquad_flat_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_uiquad_flat_frag);
  p.rect_proj = renderer.AddProgram(d);
  d.name = "rdr_uiquad_proj";
  d.fragment_spirv = kSpv_rdr_uiquad_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_uiquad_frag);
  p.sprite_proj = renderer.AddProgram(d);
  d.name = "rdr_uiglyph_proj";
  d.fragment_spirv = kSpv_rdr_uiglyph_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_uiglyph_frag);
  p.glyph_proj = renderer.AddProgram(d);
  d.name = "rdr_uiquad_vcol_proj";   // S105 A8: the vertex-colour rect (T_RectColor), matrix-placed and pixel-placed
  d.vertex_spirv = kSpv_rdr_uiquad_vcol_proj_vert;
  d.vertex_bytes = sizeof(kSpv_rdr_uiquad_vcol_proj_vert);
  d.fragment_spirv = kSpv_rdr_uiquad_vcol_frag;
  d.fragment_bytes = sizeof(kSpv_rdr_uiquad_vcol_frag);
  p.rect_color_proj = renderer.AddProgram(d);
  d.name = "rdr_uiquad_vcol";
  d.vertex_spirv = kSpv_rdr_uiquad_vcol_vert;
  d.vertex_bytes = sizeof(kSpv_rdr_uiquad_vcol_vert);
  p.rect_color = renderer.AddProgram(d);
  p.layout = renderer.AddVertexLayout(rdr::VertexLayoutDesc{});   // no vertex buffer: the corners come from the constants and gl_VertexIndex
  return p;
}

}  // namespace rdr_scene::q2d
