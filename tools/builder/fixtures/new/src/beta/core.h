// fixture: invented header (beta). Same basename as src/alpha/core.h.
#pragma once
namespace fixture {
struct BetaRow {
  int a = 0;
  int b = 0;
  int c = 0;
  int layout = 2;
};
inline int BetaSum(const BetaRow& r) { return r.a + r.b + r.c; }
}  // namespace fixture
