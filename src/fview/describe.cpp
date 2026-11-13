#include "describe.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace fview {

// S101 names (docs\renderer\NAMES.md): the draw's origin section, when the frame has one. Call sites stay as addresses: q.py fn ADDR names
// the function (the origin table classifies only the functions db\ids\origin_table.json lists).
std::string DrawOrigin(const rdr_adapter::LoadedFrame& frame, uint32_t draw) {
  const rdr_adapter::DrawOrigin* o = frame.OriginOf(draw);
  if (!o) return "unnamed (this frame carries no names)";
  if (!o->op) return "unnamed (no tag reached this draw)";
  char buf[256];
  const char* technique = frame.String(o->technique);
  std::snprintf(buf, sizeof(buf), "%s%s%s, op %u, call site 0x%08X%s", rdr_adapter::DrawClassName(o->klass), *technique ? " " : "", technique,
                unsigned(o->op), o->call_site, o->klass == rdr_adapter::DrawClass::Unnamed ? " (not in the origin table)" : "");
  std::string s = buf;
  if (o->pass_site) {
    std::snprintf(buf, sizeof(buf), ", pass 0x%08X, object 0x%08X part %u", o->pass_site, o->object, o->part);
    s += buf;
  }
  // S101 lane18: a 2D draw's widget / screen ("widget unnamed" for a 2D class in a capture made before the field existed)
  const char* widget = frame.String(o->widget);
  if (*widget) s += std::string(", widget [") + widget + "]";
  else if (o->klass == rdr_adapter::DrawClass::Sprite || o->klass == rdr_adapter::DrawClass::Text || o->klass == rdr_adapter::DrawClass::Movie) s += ", widget unnamed";
  // S101 U1: a glyph draw's whole run string (kSecTextRuns, sorted by draw; absent in frames made without labels)
  for (uint32_t i = 0; i < frame.text_runs.count; ++i) {
    const rdr_adapter::TextRun& r = frame.text_runs[i];
    if (r.draw != draw) continue;
    s += std::string(", run \"") + frame.String(r.string) + "\" (" + std::to_string(r.units) + " units" + ((r.flags & rdr_adapter::kRunTruncated) ? ", truncated" : "") + ")";
    break;
  }
  return s;
}
// Lane 1's names (names.h): the role the image's creator declared, with its flags; old captures carry none.
std::string SurfaceRole(const rdr_adapter::CaptureReader& capture, uint32_t image) {
  const rdr_adapter::ImageRecord* r = capture.FindImage(image);
  if (!r || (r->role == 0 && r->role_flags == 0)) return "unnamed (no names in this capture)";
  std::string s = rdr_adapter::SurfaceRoleName(rdr_adapter::SurfaceRole(r->role));
  if (r->role == 0) s = "unnamed";
  std::string extra;
  const auto add = [&](const char* t) { extra += extra.empty() ? "" : ", ", extra += t; };
  if (r->role_flags & rdr_adapter::kRoleDeclared) add("declared");
  if (r->role_flags & rdr_adapter::kRoleGuessed) add(r->role == 0 ? "guessed by the 16:9 rule" : "guessed");
  if (r->role_flags & rdr_adapter::kRoleScales) add("scales");
  if (r->role_flags & rdr_adapter::kRoleLasting) add("lasting");
  return extra.empty() ? s : s + " (" + extra + ")";
}
std::string TextureProvenance(const rdr_adapter::LoadedFrame& frame, uint32_t image) {
  for (uint32_t i = 0; i < frame.provenance.count; ++i) {
    const rdr_adapter::TextureProvenance& p = frame.provenance[i];
    if (p.image != image) continue;
    std::string s = rdr_adapter::ProvenanceName(p.source);
    if (p.source == rdr_adapter::Provenance::File) s += std::string(": ") + frame.String(p.file) + " / " + frame.String(p.file_name);
    else if (p.source == rdr_adapter::Provenance::Gpu) s += ": copy of image " + std::to_string(p.source_image);
    else if (p.source == rdr_adapter::Provenance::Cpu) s += std::string(": ") + frame.String(p.writer);
    if (p.flags & rdr_adapter::kProvContentByReference) s += " (by reference)";
    if (p.flags & rdr_adapter::kProvGuessed) s += " (guessed)";
    return s;
  }
  return frame.provenance.count ? "unnamed" : "unnamed (no names in this capture)";
}

const char* OpKindName(rdr_adapter::OpKind kind) {
  switch (kind) {
    case rdr_adapter::OpKind::SetTargets: return "targets";
    case rdr_adapter::OpKind::Clear: return "clear";
    case rdr_adapter::OpKind::Draw: return "draw";
    case rdr_adapter::OpKind::Copy: return "copy";
    case rdr_adapter::OpKind::Present: return "present";
  }
  return "?";
}

namespace {

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...) {
  char text[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  return text;
}

const char* kFormatNames[] = {"Undefined", "RGBA8", "RGBA8_SNORM", "BGRA8", "R8", "R8_SNORM", "RG8", "RGBA4", "R5G6B5", "A1RGB5", "RGB10A2", "RGBA16_SNORM",
                              "RGBA16_UNORM", "RG16_UNORM", "RG16_SNORM", "R16_UNORM", "RGBA16F", "RG16F", "R16F", "RGBA32F", "RG32F", "R32F", "R32_UINT",
                              "Depth", "BC1", "BC2", "BC3", "BC4", "BC5", "ASTC_4x4", "EAC_R11", "EAC_RG11"};
std::string FormatName(uint32_t f) { return f < std::size(kFormatNames) ? kFormatNames[f] : Fmt("format%u", f); }

const char* CompareName(uint32_t c) {
  static const char* names[] = {"NEVER", "LESS", "EQUAL", "LEQUAL", "GREATER", "NOTEQUAL", "GEQUAL", "ALWAYS"};
  return c < 8 ? names[c] : "?";
}
const char* BlendFactor(uint32_t f) {
  static const char* names[] = {"ZERO", "ONE", "SRC_COLOR", "1-SRC_COLOR", "DST_COLOR", "1-DST_COLOR", "SRC_ALPHA", "1-SRC_ALPHA", "DST_ALPHA", "1-DST_ALPHA",
                                "CONST_COLOR", "1-CONST_COLOR", "CONST_ALPHA", "1-CONST_ALPHA", "SRC_ALPHA_SATURATE"};
  return f < 15 ? names[f] : "?";
}
const char* BlendOp(uint32_t o) {
  static const char* names[] = {"ADD", "SUBTRACT", "REV_SUBTRACT", "MIN", "MAX"};
  return o < 5 ? names[o] : "?";
}
const char* CullName(uint32_t c) {
  static const char* names[] = {"none", "front", "back", "both"};
  return c < 4 ? names[c] : "?";
}
const char* TopologyName(uint32_t t) {
  static const char* names[] = {"points", "lines", "line strip", "triangles", "triangle strip", "triangle fan"};
  return t < 6 ? names[t] : "other";
}
const char* AddressName(uint32_t a) {
  static const char* names[] = {"repeat", "mirror", "clamp", "border", "mirror-clamp"};
  return a < 5 ? names[a] : "?";
}

std::string FlagsText(uint8_t flags) {
  std::string s;
  const auto add = [&](const char* t) { s += s.empty() ? "" : "|", s += t; };
  if (flags & rdr_adapter::kImageTiled) add("tiled");
  if (flags & rdr_adapter::kImageCopyDest) add("copy-dest");
  if (flags & rdr_adapter::kImageMoviePlane) add("movie");
  if (flags & rdr_adapter::kImageCopySwap) add("copy-swap");
  if (flags & rdr_adapter::kImageBorder) add("border");
  return s.empty() ? "-" : s;
}

}  // namespace

std::string EffectOf(const EffectIndex* effects, const std::string& variant) {
  if (!effects) return {};
  const FxVariant* v = effects->Find(variant);
  return v ? v->effect : std::string();
}

bool DescribeDraw(const Session& session, const EffectIndex* effects, uint32_t op, DrawInfo& out, std::string& error) {
  const rdr_adapter::LoadedFrame& frame = session.frame();
  if (op >= frame.ops.count || frame.ops[op].kind != rdr_adapter::OpKind::Draw) {
    error = "op " + std::to_string(op) + " is not a draw";
    return false;
  }
  const OpTrace& trace = session.trace()[op];
  const rdr_adapter::Draw& draw = frame.draws[frame.ops[op].index];
  const rdr_adapter::DrawFixed& fixed = frame.fixeds[draw.fixed];
  const rdr_adapter::DrawDynamic& dyn = frame.dynamics[draw.dynamic];
  out = DrawInfo{};
  out.op = op;
  out.draw = frame.ops[op].index;
  out.outcome = OutcomeText(trace.outcome);
  out.bound = trace.outcome == kOutcomeBound;
  out.vs_hash = draw.vs_hash;
  out.ps_hash = draw.ps_hash;
  out.vs_variant = trace.vs >= 0 ? session.VariantName(trace.vs) : "(none)";
  out.ps_variant = trace.ps >= 0 ? session.VariantName(trace.ps) : "(none)";
  out.effect = EffectOf(effects, trace.ps >= 0 ? out.ps_variant : out.vs_variant);
  if (out.effect.empty()) out.effect = EffectOf(effects, out.vs_variant);
  out.origin = DrawOrigin(frame, out.draw);

  // The alpha test: what the game asked for, and what the program the binder bound applies.
  out.alpha_game = fixed.alpha_test != 0;
  out.alpha_func = CompareName(fixed.alpha_func);
  out.alpha_ref = fixed.alpha_ref;
  out.twin_compiled = trace.ps >= 0 && session.VariantHasAlphaTwin(trace.ps);
  out.twin_used = out.bound && trace.alpha_twin;
  if (effects && trace.ps >= 0) {
    if (const FxVariant* v = effects->Find(out.ps_variant)) out.json_alpha = v->alpha_test ? 1 : 0;
  }
  out.program_alpha = out.twin_used;  // what the binder bound (the trace records the twin it used)
  out.mismatch = out.bound && out.alpha_game && !out.program_alpha;
  // The description on disk against the compiled table: it makes the twin the binder uses (or not), so a description that disagrees is stale (rebuild the registry).
  out.stale_table = trace.ps >= 0 && out.json_alpha >= 0 && ((out.json_alpha == 0 && out.twin_compiled) || (out.json_alpha == 1 && !out.twin_compiled));
  if (!out.alpha_game) out.alpha_line = "alpha test: game OFF";
  else if (!out.bound) out.alpha_line = "alpha test: game ON (draw not drawn)";
  else out.alpha_line = std::string("alpha test: game ON, program ") + (out.program_alpha ? "ON" : "OFF");
  if (out.stale_table) out.alpha_line += std::string(" (STALE TABLE: the description says alpha_test ") + (out.json_alpha ? "true" : "false") + ", the compiled table " + (out.twin_compiled ? "has" : "has no") + " alpha-test twin: run tools\\renderer\\compile_shaders.py)";

  // State.
  const rdr::DrawState& s = fixed.state;
  for (int t = 0; t < rdr::kMaxColorTargets; ++t) {
    const rdr::BlendState& b = s.blend[t];
    if (t > 0 && !b.enable && b.write_mask == s.blend[0].write_mask) continue;
    if (b.enable)
      out.state.push_back(Fmt("blend%d: color %s * %s %s %s * %s, alpha %s * %s %s %s * %s, write mask %X", t, BlendFactor(b.src_color), "src", BlendOp(b.color_op), BlendFactor(b.dst_color),
                              "dst", BlendFactor(b.src_alpha), "src", BlendOp(b.alpha_op), BlendFactor(b.dst_alpha), "dst", b.write_mask));
    else
      out.state.push_back(Fmt("blend%d: off, write mask %X", t, b.write_mask));
  }
  out.state.push_back(Fmt("depth: test %s write %s compare %s", s.depth_test ? "on" : "off", s.depth_write ? "on" : "off", CompareName(s.depth_compare)));
  out.state.push_back(Fmt("stencil: %s; cull %s; topology %s; depth clamp %s", s.stencil_test ? "on" : "off", CullName(s.cull), TopologyName(s.topology), s.depth_clamp ? "on" : "off"));
  out.state.push_back(Fmt("source primitive %u, %u %s, flags %02X%s", unsigned(draw.source_prim), draw.index_bytes ? draw.index_count : draw.vertex_count,
                          draw.index_bytes ? "indices" : "vertices", unsigned(draw.flags), (draw.flags & rdr_adapter::kDrawInline) ? " (inline)" : ""));
  out.viewport = Fmt("viewport %g,%g %gx%g depth %g..%g%s", dyn.host.viewport[0], dyn.host.viewport[1], dyn.host.viewport[2], dyn.host.viewport[3], dyn.host.viewport[4], dyn.host.viewport[5],
                     (dyn.flags & rdr_adapter::kDynClipOff) ? " (clip off: pixel-space)" : "");
  out.scissor = Fmt("scissor %d,%d %ux%u", dyn.host.scissor.x, dyn.host.scissor.y, dyn.host.scissor.width, dyn.host.scissor.height);

  // The targets in force at this op.
  const rdr::TargetsOp* targets = nullptr;
  for (uint32_t i = 0; i <= op; ++i)
    if (frame.ops[i].kind == rdr_adapter::OpKind::SetTargets) targets = &frame.targets[frame.ops[i].index];
  if (targets) {
    out.targets = "colour";
    for (uint32_t c = 0; c < targets->color_count; ++c) out.targets += Fmt(" %u", targets->color[c].image);
    if (!targets->color_count) out.targets += " (none)";
    if (targets->depth.image) out.targets += Fmt(", depth %u", targets->depth.image);
  }

  // Textures.
  for (uint32_t t = 0; t < draw.texture_count; ++t) {
    const rdr_adapter::TextureSlot& slot = frame.texture_slots[draw.texture_first + t];
    TextureInfo info;
    info.slot = slot.slot;
    info.image = slot.image;
    std::memcpy(info.swizzle, slot.swizzle, 4);
    if (const rdr_adapter::ImageRecord* r = session.capture().FindImage(slot.image)) {
      info.has_record = true;
      info.record = *r;
      info.format = FormatName(r->format);
      info.source = r->source == rdr_adapter::kSourceSurface ? "surface" : "texture";
      info.flags = FlagsText(r->flags);
    }
    info.role = SurfaceRole(session.capture(), slot.image);
    info.provenance = TextureProvenance(frame, slot.image);
    if (slot.sampler < frame.samplers.count) {
      const rdr::SamplerDesc& sm = frame.samplers[slot.sampler];
      info.sampler = Fmt("%s/%s mip %u, %s,%s,%s, aniso %u, lod %g..%g bias %g", sm.min_filter ? "linear" : "nearest", sm.mag_filter ? "linear" : "nearest", unsigned(sm.mip_mode),
                         AddressName(sm.address_u), AddressName(sm.address_v), AddressName(sm.address_w), unsigned(sm.max_anisotropy), sm.min_lod, sm.max_lod, sm.lod_bias);
    }
    out.textures.push_back(std::move(info));
  }

  // Constants, by name where the description has them.
  const auto add_constants = [&](const char* stage, const std::string& variant, uint32_t bank_index) {
    if (bank_index >= frame.constant_banks.count) return;
    const uint32_t* bank = frame.constant_banks[bank_index].dwords;
    const FxVariant* v = effects ? effects->Find(variant) : nullptr;
    std::vector<bool> covered(256, false);
    if (v) {
      for (const FxConstant& c : v->constants) {
        if (c.reg < 0 || c.count < 0 || c.reg + c.count > 256) continue;
        ConstantInfo info;
        info.stage = stage, info.field = c.field, info.reg = c.reg, info.count = c.count;
        for (int r = 0; r < c.count; ++r) {
          covered[size_t(c.reg + r)] = true;
          for (int k = 0; k < 4; ++k) {
            float f;
            std::memcpy(&f, bank + (c.reg + r) * 4 + k, 4);
            info.values.push_back(f);
          }
        }
        out.constants.push_back(std::move(info));
      }
    }
    if (!v) {  // no description: the raw registers that hold something, as many as fit a panel
      int shown = 0;
      for (int r = 0; r < 256 && shown < 32; ++r) {
        bool any = false;
        for (int k = 0; k < 4; ++k) any |= bank[r * 4 + k] != 0;
        if (!any) continue;
        ConstantInfo info;
        info.stage = stage, info.field = Fmt("c%d", r), info.reg = r, info.count = 1, info.named = false;
        for (int k = 0; k < 4; ++k) {
          float f;
          std::memcpy(&f, bank + r * 4 + k, 4);
          info.values.push_back(f);
        }
        out.constants.push_back(std::move(info));
        ++shown;
      }
    }
  };
  add_constants("vs", out.vs_variant, draw.vs_bank);
  if (trace.ps >= 0) add_constants("ps", out.ps_variant, draw.ps_bank);
  return true;
}

std::vector<std::string> DrawLines(const DrawInfo& d) {
  std::vector<std::string> lines;
  lines.push_back(Fmt("op %u  draw %u  %s", d.op, d.draw, d.outcome.c_str()));
  lines.push_back("effect: " + (d.effect.empty() ? std::string("(no description)") : d.effect));
  lines.push_back(Fmt("vs %016llX  %s", static_cast<unsigned long long>(d.vs_hash), d.vs_variant.c_str()));
  lines.push_back(Fmt("ps %016llX  %s", static_cast<unsigned long long>(d.ps_hash), d.ps_variant.c_str()));
  lines.push_back("origin: " + d.origin);
  lines.push_back(d.alpha_line);
  if (d.alpha_game)
    lines.push_back(Fmt("  game: func %s ref %.4g; program: twin compiled %s, used %s, fx.json alpha_test %s", d.alpha_func.c_str(), double(d.alpha_ref), d.twin_compiled ? "yes" : "no",
                        d.twin_used ? "yes" : "no", d.json_alpha < 0 ? "(no description)" : d.json_alpha ? "true" : "false"));
  if (d.mismatch) lines.push_back("  MISMATCH: the game tests alpha here and the bound program does not");
  lines.push_back("targets: " + d.targets);
  lines.push_back(d.viewport);
  lines.push_back(d.scissor);
  for (const std::string& s : d.state) lines.push_back(s);
  for (const TextureInfo& t : d.textures) {
    if (t.has_record)
      lines.push_back(Fmt("tex slot %u: image %u %ux%u %s %s [%s] game fmt %u, mips %u; role %s, provenance %s", t.slot, t.image, t.record.width, t.record.height, t.format.c_str(), t.source.c_str(),
                          t.flags.c_str(), unsigned(t.record.game_format), unsigned(t.record.mips), t.role.c_str(), t.provenance.c_str()));
    else
      lines.push_back(Fmt("tex slot %u: image %u (no record: reads empty)", t.slot, t.image));
    if (!t.sampler.empty()) lines.push_back("    sampler " + t.sampler);
  }
  for (const ConstantInfo& c : d.constants) {
    std::string v;
    for (size_t i = 0; i < c.values.size(); ++i) v += Fmt("%s%.5g", i % 4 ? " " : (i ? " | " : ""), double(c.values[i]));
    lines.push_back(Fmt("%s %s%s c%d%s: %s", c.stage.c_str(), c.field.c_str(), c.named ? "" : " (raw)", c.reg, c.count > 1 ? Fmt("..c%d", c.reg + c.count - 1).c_str() : "", v.c_str()));
  }
  return lines;
}

void WriteDrawJson(JsonWriter& w, const DrawInfo& d) {
  w.Begin('{');
  w.Key("op"), w.Int(d.op);
  w.Key("draw"), w.Int(d.draw);
  w.Key("outcome"), w.Str(d.outcome);
  w.Key("effect"), w.Str(d.effect);
  w.Key("origin"), w.Str(d.origin);
  w.Key("vs_hash"), w.Str(Fmt("%016llX", static_cast<unsigned long long>(d.vs_hash)));
  w.Key("ps_hash"), w.Str(Fmt("%016llX", static_cast<unsigned long long>(d.ps_hash)));
  w.Key("vs_variant"), w.Str(d.vs_variant);
  w.Key("ps_variant"), w.Str(d.ps_variant);
  w.Key("alpha_test");
  w.Begin('{');
  w.Key("game"), w.Bool(d.alpha_game);
  w.Key("func"), w.Str(d.alpha_func);
  w.Key("ref"), w.Num(double(d.alpha_ref));
  w.Key("twin_compiled"), w.Bool(d.twin_compiled);
  w.Key("twin_used"), w.Bool(d.twin_used);
  w.Key("fx_json_alpha_test"), d.json_alpha < 0 ? w.Null() : w.Bool(d.json_alpha != 0);
  w.Key("program"), w.Bool(d.program_alpha);
  w.Key("mismatch"), w.Bool(d.mismatch);
  w.Key("stale_table"), w.Bool(d.stale_table);
  w.Key("line"), w.Str(d.alpha_line);
  w.End();
  w.Key("targets"), w.Str(d.targets);
  w.Key("viewport"), w.Str(d.viewport);
  w.Key("scissor"), w.Str(d.scissor);
  w.Key("state");
  w.Begin('[');
  for (const std::string& s : d.state) w.Str(s);
  w.End();
  w.Key("textures");
  w.Begin('[');
  for (const TextureInfo& t : d.textures) {
    w.Begin('{');
    w.Key("slot"), w.Int(t.slot);
    w.Key("image"), w.Int(t.image);
    if (t.has_record) {
      w.Key("width"), w.Int(t.record.width);
      w.Key("height"), w.Int(t.record.height);
      w.Key("format"), w.Str(t.format);
      w.Key("source"), w.Str(t.source);
      w.Key("flags"), w.Str(t.flags);
      w.Key("game_format"), w.Int(t.record.game_format);
    }
    w.Key("role"), w.Str(t.role);
    w.Key("provenance"), w.Str(t.provenance);
    w.Key("sampler"), w.Str(t.sampler);
    w.End();
  }
  w.End();
  w.Key("constants");
  w.Begin('[');
  for (const ConstantInfo& c : d.constants) {
    w.Begin('{');
    w.Key("stage"), w.Str(c.stage);
    w.Key("field"), w.Str(c.field);
    w.Key("named"), w.Bool(c.named);
    w.Key("register"), w.Int(c.reg);
    w.Key("count"), w.Int(c.count);
    w.Key("values");
    w.Begin('[');
    for (float f : c.values) w.Num(double(f));
    w.End();
    w.End();
  }
  w.End();
  w.End();
}

}  // namespace fview
