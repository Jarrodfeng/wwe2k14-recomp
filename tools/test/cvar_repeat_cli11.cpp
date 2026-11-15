// S102 cvar-parse: standalone proof of the repeated-switch bug in the SDK's cvar Init (our code, no game data).
// Mirrors cvar.cpp Init: A (bool flag), B and C (string options). Parse --A=true --B=x --B=y --C=z.
//   fixed  (policy TakeLast): A=true B=y C=z, no error (exit 0)
//   planted (--planted, policy Throw, the old code): C not applied, error printed (exit 0 if it reproduces the bug, 1 otherwise)
// Build/run: tools\test\cvar_repeat_cli11.ps1
#include <CLI/CLI.hpp>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
  bool planted = argc > 1 && !std::strcmp(argv[1], "--planted");
  std::string A = "unset", B = "unset", C = "unset";
  std::string err;
  CLI::App app{"", ""};
  app.allow_extras();
  app.add_flag_function("--A,!--no-A", [&](int64_t n) { A = n > 0 ? "true" : "false"; });
  for (auto* p : {&B, &C}) {
    auto* o = app.add_option_function<std::string>(p == &B ? "--B" : "--C", [p](const std::string& v) { *p = v; });
    if (!planted) o->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);
  }
  const char* args[] = {"t", "--A=true", "--B=x", "--B=y", "--C=z"};
  try {
    app.parse(5, args);
  } catch (const CLI::ParseError& e) {
    err = e.what();
  }
  std::printf("%s: A=%s B=%s C=%s error=[%s]\n", planted ? "planted(Throw)" : "fixed(TakeLast)", A.c_str(), B.c_str(),
              C.c_str(), err.c_str());
  if (planted) return (C == "unset" && !err.empty()) ? 0 : 1;  // the old bug must reproduce
  return (A == "true" && B == "y" && C == "z" && err.empty()) ? 0 : 1;
}
