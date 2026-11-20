// fixture: invented code (mover), the version the fake note was written against
namespace fixture {

int StartMover(int x) {
  return x + 1;
}

int StepMover(int x) {
  int y = x * 2;
  y += 7;
  return y;
}

int TickMover(int t) {
  int a = t;
  a -= 1;
  a *= 3;
  return a;
}

}  // namespace fixture
