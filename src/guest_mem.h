#pragma once
// Pure inline: no hook, no cvar, no static (tools/lint/one_definition_lint.py). Moved verbatim from src/scenario.h (EXEB-B1) so a game-fix unit can read guest memory
// without pulling in the scenario module.
#include <cstdint>

#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

namespace wwe2k14::scenario {
inline uint8_t* Guest(uint32_t address) { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address); }
inline uint32_t U32(uint32_t address) { return rex::memory::load_and_swap<uint32_t>(Guest(address)); }
inline uint8_t U8(uint32_t address) { return *Guest(address); }
}  // namespace wwe2k14::scenario
