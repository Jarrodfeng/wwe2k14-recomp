// S111 EQUIV piece B: stb_image's implementation for the test DLL alone (the asset headers' pac.h decodes with its zlib), in its own unit so a
// change to equiv_tests.cpp never recompiles it. The DLL exports none of it. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>
