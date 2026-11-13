// Session 101 lane 2 (fview): what a draw (or any op) of the selected frame is, by name: the effect and variants the binder chose, the state the game
// set and what the bound program applies, the textures, the constants. One struct, written as text for the window and as JSON for the headless modes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "effects.h"
#include "json_lite.h"
#include "session.h"

namespace fview {

// The names lane 1 will record in a version-2 capture (src\renderer_adapter\names.h, docs\renderer\NAMES.md). Until a frame carries them each is
// "unnamed"; when names.h lands, each of these three is the one place that reads it.
std::string DrawOrigin(const rdr_adapter::LoadedFrame& frame, uint32_t draw);
std::string SurfaceRole(const rdr_adapter::CaptureReader& capture, uint32_t image);
std::string TextureProvenance(const rdr_adapter::LoadedFrame& frame, uint32_t image);

struct TextureInfo {
  uint32_t slot = 0, image = 0;
  bool has_record = false;
  rdr_adapter::ImageRecord record;
  std::string format, source, flags, role, provenance;
  std::string sampler;
  uint8_t swizzle[4] = {};
};

struct ConstantInfo {
  std::string stage;  // "vs" / "ps"
  std::string field;  // the name from the effect description; "c<N>" for a raw register
  int reg = 0, count = 0;
  bool named = true;
  std::vector<float> values;  // count * 4
};

struct DrawInfo {
  uint32_t op = 0, draw = 0;
  std::string outcome;
  bool bound = false;
  uint64_t vs_hash = 0, ps_hash = 0;
  std::string vs_variant, ps_variant;  // the registry names, e.g. yAudience_lm_uv_vertex
  std::string effect;                  // from the description folder; "" when it has none for the variant
  std::string origin;                  // lane 1's name
  // Alpha test.
  bool alpha_game = false;      // the game's DrawFixed
  std::string alpha_func;
  float alpha_ref = 0;
  bool twin_compiled = false;   // the registry holds an alpha-test build of the pixel variant
  bool twin_used = false;       // the binder drew with it
  int json_alpha = -1;          // the description's "alpha_test" for the pixel variant: -1 no description, 0 false, 1 true
  bool program_alpha = false;   // what the bound program applies
  bool mismatch = false;        // game ON, program OFF
  bool stale_table = false;     // the description's alpha_test disagrees with the compiled table's twin
  std::string alpha_line;       // "alpha test: game ON, program OFF" ...
  // State.
  std::vector<std::string> state;  // "blend0 ...", "depth ..." lines
  std::string viewport, scissor, targets;
  std::vector<TextureInfo> textures;
  std::vector<ConstantInfo> constants;
};

// `effects` may be null (no descriptions: names are the registry's).
bool DescribeDraw(const Session& session, const EffectIndex* effects, uint32_t op, DrawInfo& out, std::string& error);
std::vector<std::string> DrawLines(const DrawInfo& info);  // the window's draw panel and the case text
void WriteDrawJson(JsonWriter& w, const DrawInfo& info);
const char* OpKindName(rdr_adapter::OpKind kind);
// The effect of a registry variant name ("" when the descriptions have none).
std::string EffectOf(const EffectIndex* effects, const std::string& variant);

}  // namespace fview
