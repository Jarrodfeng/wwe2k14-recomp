// S102 G2 (the output pass on the Surface): the two output levers' switches and counters, plain atomics so the standalone renderer (rplay, fview) and the game build share
// one place. Both levers are PerFrame: the reader (reader.h) writes them every frame from the cvars; RecordPresent reads them where the present is drawn.
// Both default off (0 / false). docs\renderer\OUTPUT_PASS.md.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr {

// NOTE (review F4): in the game these two atomics stay 0. live.cpp reads the S0-4 table (IntSeen, per guest frame) and hands the values to the frame
// (FrameBuilder::SetOutput -> Frame::out_cap_h / out_bilinear); the render thread reads only the frame. rplay / fview have no table: they set these
// atomics from their flags and FrameBuilder::Begin latches them.
// (b) wwe2k14_rdr_low_output_cap: the present is drawn at most this many pixels tall (the width follows the panel's aspect) and a bilinear blit stretches it over the panel.
// 0 = off. A panel not taller than the cap is never touched.
inline std::atomic<uint32_t> g_out_cap_h{0};
// (c) wwe2k14_rdr_low_present_bilinear: the upscaling present is a plain bilinear sample instead of SGSR (options.present_sgsr is ignored while it is set).
inline std::atomic<bool> g_out_bilinear{false};
// TEST ONLY (rplay --output-cap-plant): the cap is asked for and never applied. The offline check must fail on it (a lever that is "on" but does nothing).
inline std::atomic<bool> g_out_cap_plant{false};
// TEST ONLY (rplay / fview --output-blit-plant): the cap's blit writes only the capped-size corner of the panel (dst = draw_extent_). The pixel check must FAIL on it.
inline std::atomic<bool> g_out_blit_plant{false};
// TEST ONLY (--output-makecap-fail-plant): the capped target cannot be made (as if BindNew failed). The cap must give up once, not retry every frame.
inline std::atomic<bool> g_out_makecap_fail_plant{false};
inline std::atomic<uint64_t> g_out_makecap_tries{0};  // MakeCap attempts (the F7 check reads it)
// The LOW tier as resolved at start (reader.h, once; low_levers.h IsLow). live.cpp decides the two PerFrame levers with it (the S0-4 table values are raw cvars).
inline std::atomic<bool> g_low_tier{false};
// Counters the offline check reads: presents drawn, and of those how many drew at the capped size / through SGSR.
inline std::atomic<uint64_t> g_out_frames{0};
inline std::atomic<uint64_t> g_out_cap_frames{0};
inline std::atomic<uint64_t> g_out_sgsr_frames{0};

// S102 R24-A (docs\renderer\PRESENT_COPY.md): wwe2k14_rdr_present_fetch, PerFrame, default off. When the picture is shown exactly 1:1 (the picture is the panel's size, the viewport covers it, no turn,
// no SGSR / DLSS / cap) the present's fragment shader reads its pixel with one texelFetch at gl_FragCoord (rdr_present_fetch.frag: no UV, no filter, no SGSR maths) and applies the display curve exactly as
// rdr_present.frag does. The game reads the table (live.cpp) and hands the value to the frame (FrameBuilder::SetPresentFetch -> Frame::present_fetch); rplay / fview set this atomic from --present-fetch
// and FrameBuilder::Begin latches it. The render thread reads only the frame.
inline std::atomic<bool> g_present_fetch{false};
// TEST ONLY (rplay / fview --present-fetch-plant 2): the fit test is skipped (the fetch shader runs on a picture that is not 1:1). The pixel check must FAIL on it.
inline std::atomic<int> g_present_fetch_plant{0};
// TEST ONLY (--display-curve-s100 / --display-curve-file F): the tool applies a real display curve (the game's is never the identity: LIVE_ADAPTER.md), captures do not carry one. --overlay-plant: an overlay is drawn.
inline std::atomic<bool> g_display_curve_s100{false};
inline std::atomic<bool> g_overlay_plant{false};
// Census slots: relaxed atomics in fixed slots (no name, string, map or lock on the per-frame path; the line is built every 600 frames).
enum PresentFetchSlot : int { kPcFrames, kPcTaken, kPcDeclFit, kPcDeclSgsrDlss, kPcDeclCap, kPcSlots };
inline std::atomic<uint64_t> g_present_fetch_count[kPcSlots];


}  // namespace rdr
