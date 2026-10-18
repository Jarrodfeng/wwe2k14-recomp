#pragma once
// Session 26: read game files through the game's own file system (GAME:), so our features work the same whether the
// game files are a folder or a disc image (.iso). Never read them from the host folder directly: an ISO has none.
// Paths are guest paths, e.g. "game:\\movies\\titantron\\343.bik2".

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/runtime.h>
#include <rex/thread/mutex.h>

namespace wwe2k14::game_files {

inline rex::filesystem::Entry* Resolve(std::string_view guest_path) {
  auto* rt = rex::Runtime::instance();
  auto* fs = rt ? rt->file_system() : nullptr;
  return fs ? fs->ResolvePath(guest_path) : nullptr;
}

// Reads up to out.size() bytes at offset; returns the byte count (0 if the file is missing or the read failed).
inline size_t Read(std::string_view guest_path, std::span<uint8_t> out, size_t offset = 0) {
  auto* entry = Resolve(guest_path);
  if (!entry || out.empty()) return 0;
  rex::filesystem::File* file = nullptr;
  if (XFAILED(entry->Open(rex::filesystem::FileAccess::kGenericRead, &file)) || !file) return 0;
  size_t n = 0;
  if (XFAILED(file->ReadSync(out, offset, &n))) n = 0;
  file->Destroy();
  return n;
}

// Names of the files (not folders) in a game folder.
inline std::vector<std::string> ListFiles(std::string_view guest_dir) {
  std::vector<std::string> names;
  auto* dir = Resolve(guest_dir);
  if (!dir) return names;
  // The folder device can add children while a guest thread opens a file; the VFS does that under this lock.
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  for (const auto& child : dir->children())
    if (!(child->attributes() & rex::filesystem::kFileAttributeDirectory)) names.push_back(child->name());
  return names;
}

}  // namespace wwe2k14::game_files
