// S103 P9 (docs\renderer\COMPILE_OUT_LIST.md A20 / A23): the app's owner-mode globals, kept in EVERY build. They used to be defined in the stream reader (reader.h), so a
// build without the reader could not even name them. They are plain data the app fills at start (window handle, the folders the scene classes read) for whatever hosts the
// render thread: the stream's LiveSink today (src\renderer_adapter\live.cpp), landing 2's own host later (P7). Same names, same namespace as before: nothing that read them changes.
#pragma once

#include <string>

namespace wwe2k14::rdr_reader {

// S101 lane 35: the game files folder the crowd's bodies are read from (set at start by wwe2k14_app.h from game_data_root(); read by the render thread).
inline std::string g_crowd_game_root;
inline std::string g_caw_cache_root;   // lane 153: set from runtime()->cache_root() / caw in wwe2k14_app.h (beside the shader list import)
inline std::string g_pipeline_store_root;  // lane 276: runtime()->cache_root() / renderer_pipelines, set in wwe2k14_app.h; used only with --wwe2k14_rdr_pipeline_store
inline std::string g_ui_index_root;       // S103 PN4F (B4): runtime()->cache_root() / ui_index, set in wwe2k14_app.h: the stream-free host's picture file index (paths and hashes only) is cached here

inline void* g_owner_window = nullptr;   // stage B: the game window's native handle (HWND / ANativeWindow*), set by the app in OnPostSetup
inline void* g_owner_instance = nullptr;  // Windows: the HINSTANCE
inline void* (*g_owner_window_source)(void*) = nullptr;  // lane 235 (Android): the window's handle right now, set by the app
inline void* g_owner_window_source_user = nullptr;

}  // namespace wwe2k14::rdr_reader
