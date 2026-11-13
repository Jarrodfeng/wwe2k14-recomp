// wwe2k14 - Session 100: observers on native-factory hooks. A hook owned by src\nf\*.h (one translation unit) calls the observer, if one is
// set, before its own work; other code sets the pointer without including the hook's header (that would define the hook twice).
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/hook.h>

namespace wwe2k14::nf_observers {

using Observer = void (*)(PPCContext& ctx, uint8_t* base);

// sub_82CA8B40, the library's SetTexture (r3 device, r4 stage, r5 texture header or null, r6 dirty bit): the adapter reader's bind trace.
inline std::atomic<Observer> g_set_texture{nullptr};

}  // namespace wwe2k14::nf_observers
