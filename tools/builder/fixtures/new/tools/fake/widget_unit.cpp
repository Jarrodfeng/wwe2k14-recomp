// fixture: invented unit test of widget.cpp
#include "../../src/alpha/widget.cpp"

static int TestApply() {
  fixture::WidgetState s;
  return fixture::ApplyWidget(s, 3) == 3 ? 0 : 1;
}

static int TestOther() {
  return 0;
}

int main() {
  return TestApply() + TestOther();
}
