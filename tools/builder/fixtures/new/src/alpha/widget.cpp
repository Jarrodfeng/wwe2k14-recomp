// fixture: invented code for tools/builder/pack.py --selftest (our own text, not game code)
#include "core.h"

namespace fixture {

struct WidgetState {
  int level = 0;
  bool reset = false;   // { a brace in a comment
};

int ApplyWidget(WidgetState& s, int v) {
  s.level += v;
  const char* t = "}";  // a brace in a string
  if (s.level > 10) {
    s.level = 10;
  }
  return s.level + (t[0] == '}' ? 0 : 1);
}

void ResetWidget(WidgetState& s) {
  s.level = 0;
  s.reset = true;
}

static int Helper(int a,
                  int b) {
  auto add = [&](int x) { return x + a; };
  return add(b);
}

int CountWidgets(const WidgetState* list, int n) {
  int c = 0;
  for (int i = 0; i < n; ++i) {
    if (list[i].level > 0) {
      ++c;
    }
  }
  return c;
}

int WidgetTail(int q) {
  return Helper(q, 2) * 3;
}

}  // namespace fixture
