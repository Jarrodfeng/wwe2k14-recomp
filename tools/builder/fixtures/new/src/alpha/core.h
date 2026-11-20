// fixture: invented header (alpha). Its basename is shared with src/beta/core.h on purpose (an ambiguous bare name).
#pragma once
namespace fixture {
constexpr int kAlphaLayout = 4;
inline int AlphaSize() { return kAlphaLayout * 8; }
}  // namespace fixture
