// fixture: invented code (shapes the brace scan must follow: review findings 6-9)
extern "C" {
int CFunc(int a) {
  return a + 1;
}
}

#if USE_A
int Pick(int a) {
#else
int Pick(int a, int b) {
#endif
  return a;
}

int After(int x) {
  return x * 2;
}

struct Holder {
  Holder(int a) : x_(a), y_{a} {
    x_ += 1;
    y_ += 2;
  }
  int x_;
  int y_;
};

REX_HOOK_RAW(sub_82000010, 4) {
  return 1;
}

REX_HOOK_RAW(sub_82000020, 8) {
  return 2;
}

TEST(Widget, Clamps) {
  return;
}

TEST(Widget, Resets) {
  return;
}
