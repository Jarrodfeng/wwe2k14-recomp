// fixture: invented code whose braces a plain scan cannot balance (a macro hides one)
#define OPEN {
int Broken(int a) OPEN
  return a;
}
int Later(int b) {
  return b;
}
