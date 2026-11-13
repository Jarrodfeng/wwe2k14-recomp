// wwe2k14 - lane 283: what lookup.cpp (the cache lookups, linked by the game) shares with audio_import.cpp (the converter, launcher only).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "audio_import.h"

namespace audio_import::detail {

#if defined(__ANDROID__)
inline constexpr uintmax_t kMaxSourceBytes = 300ull << 20;  // a phone: a 300 MB source is refused before it is read
#else
inline constexpr uintmax_t kMaxSourceBytes = 1ull << 30;  // a 1 GB source is refused before it is read into memory
#endif

// A path as UTF-8 text for keys and messages (path::string() throws for names the ANSI code page cannot hold).
std::string U8(const std::filesystem::path& p);
bool ReadAll(const std::filesystem::path& p, std::vector<uint8_t>& out, std::string* err = nullptr);
uint64_t Fnv1a64(const std::vector<uint8_t>& b);
std::string KeyOf(uint64_t content, double target);
Imported Hit(const std::filesystem::path& cache_dir, const std::string& key);  // the cache entry's numbers (ok = both files exist)

// The in-process memo: (path, cache dir, target) -> the answer, valid while the file's size and mtime are unchanged.
std::string MemoKey(const std::filesystem::path& src, const std::filesystem::path& cache_dir, double target);
bool MemoGet(const std::string& key, uintmax_t size, std::filesystem::file_time_type mtime, Imported& out);
void MemoPut(const std::string& key, uintmax_t size, std::filesystem::file_time_type mtime, const Imported& r);

// <cache>\index.txt: written by the converter after every Import, read by the game.
//   status "ok" (key = the cache entry), "plain" (plays as it is; note), "refused" (err)
void IndexRecord(const std::filesystem::path& cache_dir, const std::filesystem::path& src, uintmax_t size,
                 std::filesystem::file_time_type mtime, double target, const Imported& r);

void IndexPrune(const std::filesystem::path& cache_dir);  // drops entries of deleted songs (once per pass)

Song SongFrom(const Imported& r, const std::filesystem::path& src);

}  // namespace audio_import::detail
