// fixture: invented code (mover), the version the fake note was written against
namespace fixture {

// three lines added after the note was written,
// so every line below moved down by three
// (the drift case "L10 -> L13")
int StartMover(int x) {
  return x + 1;
}

int StepMover(int x) {
  int y = x * 2;
  y += 7;
  return y;
}

int TickMover(int t) {
  const int base = t;
  const int scaled = (base - 1) * 3;
  return scaled;
}

}  // namespace fixture
