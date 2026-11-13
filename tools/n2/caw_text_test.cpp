// S101 lane 118 (N2 / L-6): runs the SHIPPED reader (src/caw_text.h ReadCawState + Flash + Line) over a capture folder and prints one line a frame.
//   caw_text_test.exe CAPTURE_DIR [--plant ps_bank|min_r|white|strip_runs] [--json]
//   --json  prints the live verb's JSON (src/caw_text.h Json(), exactly what `!caw since` answers) one object a line instead of the text form;
//           tools\n2\caw_driver_selftest.py feeds those objects to the driver (learn / page_entry / run) so a field the game never writes is caught offline
// The planted twins are Options of the shipped reader, never a copy of it. No game, no SDK. Built by tools\n2\build_caw_text_test.ps1, driven by
// tools\n2\caw_text_selftest.py (which compares every line with tools\n2\caw_read.py's).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "../../src/caw_text.h"

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: caw_text_test CAPTURE_DIR [--plant NAME]\n"); return 2; }
  wwe2k14::caw::Options opt;
  bool json = false;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--json")) json = true;
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) {
      const std::string p = argv[++i];
      if (p == "ps_bank") opt.ps_bank = true;
      else if (p == "min_r") opt.min_r = 0.3f;
      else if (p == "white") opt.white = true;
      else if (p == "strip_runs") opt.strip_runs = true;
      else { std::fprintf(stderr, "unknown plant %s\n", p.c_str()); return 2; }
    }
  }
  if (argc > 1 && !std::strcmp(argv[1], "--unit")) {   // the colour rule on fixed vectors (S101 lane 118 review: a dim red is still hot, greys and amber never are)
    const wwe2k14::caw::Options o;
    const float dim[3] = {0.12f, 0.0f, 0.0f}, grey[3] = {0.392f, 0.392f, 0.392f}, amber[3] = {0.941f, 0.627f, 0.0f}, white[3] = {1, 1, 1}, red[3] = {0.92f, 0.04f, 0.02f}, floor_[3] = {0.165f, 0.03f, 0.02f};
    std::printf("dim_red=%d grey=%d amber=%d white=%d red=%d pulse_floor=%d\n", wwe2k14::caw::IsHot(dim, o), wwe2k14::caw::IsHot(grey, o), wwe2k14::caw::IsHot(amber, o), wwe2k14::caw::IsHot(white, o),
                wwe2k14::caw::IsHot(red, o), wwe2k14::caw::IsHot(floor_, o));
    return 0;
  }
  rdr_adapter::CaptureReader reader;
  if (!reader.Open(argv[1])) { std::fprintf(stderr, "cannot open capture %s\n", argv[1]); return 2; }
  wwe2k14::caw::State prev;
  bool have_prev = false;
  for (uint32_t n = 0; n < reader.frame_count(); ++n) {
    rdr_adapter::LoadedFrame frame;
    if (!reader.LoadFrame(n, frame)) { std::fprintf(stderr, "cannot read frame %u\n", n); return 2; }
    wwe2k14::caw::State s = wwe2k14::caw::ReadCawState(frame, [&](uint64_t o, uint32_t b, std::vector<uint8_t>& out) { return reader.ReadGeometry(o, b, out); }, opt);
    if (have_prev) s.flash = wwe2k14::caw::Flash(prev, s);
    if (json) std::printf("%s\n", wwe2k14::caw::Json(s).c_str());
    else std::printf("%u\t%s\t%d\n", n, wwe2k14::caw::Line(s).c_str(), s.flash ? 1 : 0);
    prev = s, have_prev = true;
  }
  return 0;
}
