// Session 61: XXH3_64bits for the shader tools (ctypes), from the SDK's own xxHash copy, so the tools compute the
// same hashes as the SDK's shader storage (pipeline_cache.cpp: XXH3_64bits over the guest-order microcode).
// Built by tools\shaders\build.ps1 to out\tools\xxh3.dll.
#define XXH_INLINE_ALL
#include "xxhash.h"

__declspec(dllexport) unsigned long long xxh3_64(const void* data, size_t size) {
  return XXH3_64bits(data, size);
}
