// S102 lane U1: prints src\renderer_adapter\start_failure_text.h's message for a reason and a card name (tools\bc\title_card_check.py).
//   start_failure_text_cli REASON CARD
#include <cstdio>

#include "../../src/renderer_adapter/start_failure_text.h"

int main(int argc, char** argv) {
  const std::string reason = argc > 1 ? argv[1] : "", card = argc > 2 ? argv[2] : "";
  std::printf("%s\n", rdr_adapter::start_failure::Text(reason, card).c_str());
  return 0;
}
