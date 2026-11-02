// Session 81 (Jarrod): the launcher installs its own updates (Windows).
//
// Jarrod's choices (interview 2026-11-02): click-through (nothing downloads until the player presses UPDATE NOW); only
// what the release zip holds is replaced (saves, settings, mods and caches live elsewhere or are never in the zip);
// the old files stay until the new launcher has started properly and are then removed; any failure puts the old files
// back and tells the player why (log + dialog + REPORT A PROBLEM). SHA-256 against GitHub's own digest, no signing yet.
//
// The steps:
//   1. Download   the release's .zip into <install>\update_tmp\ (WinHTTP, progress, cancel), SHA-256 against the digest.
//   2. Unpack     with Windows' own tar.exe into <install>\update_tmp\stage\ (same drive: the swap is renames only).
//   3. Hand over  the launcher copies itself to %TEMP%, starts that copy with --apply-update and quits (a running exe
//                 cannot replace itself; renaming it away is fine).
//   4. Swap       the helper waits for the launcher, then for every file of the zip: the old file (if any) moves to
//                 <install>\update_backup\, the new one moves in. Files the LAST update installed that this zip no
//                 longer has move to the backup too (update_manifest.json). The plan is written first
//                 (update_journal.json) so a half-done swap can always be undone. Any error: everything goes back.
//   5. Verify     the helper restarts the launcher; once the new launcher has drawn its window it deletes the backup
//                 (update_backup, journal, tmp). If it never got that far three starts in a row, the old files return.
// Results go to update_result.json (read once by the launcher, which shows a failure) and update_log.txt (the helper's
// own notes; the report zip carries it).
#pragma once

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace launcher::upd {

namespace fs = std::filesystem;

inline constexpr const char* kAllowedPrefix = "https://github.com/Jarrodfeng/wwe2k14-recomp/releases/download/";

inline std::wstring W(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
inline std::string N(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
inline std::string P(const fs::path& p) { return N(p.wstring()); }

inline fs::path TmpDir(const fs::path& install) { return install / L"update_tmp"; }
inline fs::path BackupDir(const fs::path& install) { return install / L"update_backup"; }
inline fs::path JournalPath(const fs::path& install) { return install / L"update_journal.json"; }
inline fs::path ManifestPath(const fs::path& install) { return install / L"update_manifest.json"; }
inline fs::path ResultPath(const fs::path& install) { return install / L"update_result.json"; }
inline fs::path LogPath(const fs::path& install) { return install / L"update_log.txt"; }

inline void LogLine(const fs::path& install, const std::string& line) {
  std::ofstream f(LogPath(install), std::ios::app);
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char ts[32];
  std::tm tmv{};
  localtime_s(&tmv, &t);
  std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
  f << ts << "  " << line << "\n";
}

inline nlohmann::json ReadJson(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return nullptr;
  return nlohmann::json::parse(f, nullptr, false);
}
inline void WriteJson(const fs::path& p, const nlohmann::json& j) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << j.dump(1);
}

inline std::string Sha256File(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  std::string out;
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
    std::vector<char> buf(1 << 20);
    while (f.read(buf.data(), std::streamsize(buf.size())) || f.gcount() > 0)
      BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), ULONG(f.gcount()), 0);
    unsigned char d[32];
    if (BCryptFinishHash(h, d, sizeof(d), 0) == 0) {
      char hex[65];
      for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", d[i]);
      out = hex;
    }
    BCryptDestroyHash(h);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return out;
}

inline bool Writable(const fs::path& dir) {
  const fs::path probe = dir / L"update_write_test.tmp";
  std::ofstream f(probe, std::ios::binary);
  const bool ok = bool(f);
  f.close();
  std::error_code ec;
  fs::remove(probe, ec);
  return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// 1. Download

struct Progress {
  std::atomic<unsigned long long> done{0}, total{0};
  std::atomic<bool> cancel{false};
};

// `test_files`: a file:///PATH address is copied instead (the tests' fake releases); otherwise only the repository's own
// GitHub release downloads are fetched. Returns an empty string, or what went wrong (plain English).
inline std::string Download(const std::string& url, const fs::path& dest, Progress& pr, bool test_files, const std::string& version) {
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  if (url.rfind("file:///", 0) == 0) {
    if (!test_files) return "unexpected download address";
    const fs::path src = fs::path(W(url.substr(8)));
    pr.total = fs::file_size(src, ec);
    if (ec) return "the test file is missing";
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    std::vector<char> buf(256 * 1024);
    while (in.read(buf.data(), std::streamsize(buf.size())) || in.gcount() > 0) {
      if (pr.cancel) return "cancelled";
      out.write(buf.data(), in.gcount());
      pr.done += unsigned(in.gcount());
      std::this_thread::sleep_for(std::chrono::milliseconds(120));  // so a test's progress bar is visible
    }
    return {};
  }
  if (url.rfind(kAllowedPrefix, 0) != 0) return "unexpected download address";

  URL_COMPONENTS uc{sizeof(uc)};
  wchar_t host[256]{}, path[2048]{};
  uc.lpszHostName = host, uc.dwHostNameLength = DWORD(std::size(host));
  uc.lpszUrlPath = path, uc.dwUrlPathLength = DWORD(std::size(path));
  const std::wstring wurl = W(url);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return "the download address could not be read";

  const std::wstring agent = L"wwe2k14-launcher/" + W(version);
  HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return "no internet access";
  WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
  HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  std::string err;
  if (!r || !WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, nullptr)) {
    err = "GitHub could not be reached";
  } else {
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      err = "GitHub answered " + std::to_string(status);
    } else {
      wchar_t len[32]{};
      sz = sizeof(len);
      if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &sz, WINHTTP_NO_HEADER_INDEX))
        pr.total = _wtoi64(len);
      std::ofstream out(dest, std::ios::binary | std::ios::trunc);
      if (!out) err = "the download could not be saved";
      std::vector<char> buf(256 * 1024);
      while (err.empty()) {
        if (pr.cancel) {
          err = "cancelled";
          break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &got)) {
          err = "the download was interrupted";
          break;
        }
        if (!got) break;
        out.write(buf.data(), got);
        if (!out) {
          err = "the disk is full or not writable";
          break;
        }
        pr.done += got;
      }
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return err;
}

// ---------------------------------------------------------------------------------------------------------------
// 2. Unpack (Windows' tar.exe reads zips). Returns the folder holding the release's files, or an error in `err`.

inline fs::path Unpack(const fs::path& zip, const fs::path& stage, std::string& err) {
  std::error_code ec;
  fs::remove_all(stage, ec);
  fs::create_directories(stage, ec);
  wchar_t sys[MAX_PATH]{};
  GetSystemDirectoryW(sys, MAX_PATH);
  const fs::path tar = fs::path(sys) / L"tar.exe";
  if (!fs::exists(tar, ec)) return err = "Windows' tar.exe is missing", fs::path();
  std::wstring cmd = L"\"" + tar.wstring() + L"\" -x -f \"" + zip.wstring() + L"\" -C \"" + stage.wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return err = "the package could not be unpacked", fs::path();
  const DWORD w = WaitForSingleObject(pi.hProcess, 300000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (w != WAIT_OBJECT_0 || code != 0) return err = "the package could not be unpacked", fs::path();
  // the release zips hold one folder (wwe2k14-recomp-vX.Y.Z); its contents are the files
  fs::path root = stage;
  std::vector<fs::path> kids;
  for (const auto& e : fs::directory_iterator(stage, ec)) kids.push_back(e.path());
  if (kids.size() == 1 && fs::is_directory(kids[0], ec)) root = kids[0];
  if (!fs::is_regular_file(root / L"wwe2k14_launcher.exe", ec) || !fs::is_regular_file(root / L"wwe2k14.exe", ec))
    return err = "the package is missing the game or launcher program", fs::path();
  return root;
}

// ---------------------------------------------------------------------------------------------------------------
// 4. Swap and its undo. Paths are relative to the install folder, "/" separated.

inline std::vector<std::string> FilesUnder(const fs::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(root, ec))
    if (e.is_regular_file(ec)) out.push_back(fs::relative(e.path(), root, ec).generic_string());
  return out;
}

inline bool SafeRel(const std::string& rel) {
  return !rel.empty() && rel.find("..") == std::string::npos && rel.find(':') == std::string::npos && rel[0] != '/';
}

// Puts everything the journal lists back as it was. Safe to run twice and after a swap that died halfway.
inline void Restore(const fs::path& install, std::string& problems) {
  const nlohmann::json j = ReadJson(JournalPath(install));
  if (j.is_discarded() || j.is_null()) return;
  const fs::path backup = BackupDir(install);
  std::error_code ec;
  for (const auto& r : j.value("added", std::vector<std::string>{})) {
    if (!SafeRel(r)) continue;
    fs::remove(install / fs::path(W(r)), ec);
  }
  for (const auto& r : j.value("backed_up", std::vector<std::string>{})) {
    if (!SafeRel(r)) continue;
    const fs::path from = backup / fs::path(W(r)), to = install / fs::path(W(r));
    if (!fs::exists(from, ec)) continue;
    fs::create_directories(to.parent_path(), ec);
    fs::remove(to, ec);
    fs::rename(from, to, ec);
    if (ec) problems += (problems.empty() ? "" : "; ") + std::string("could not put back ") + r;
  }
  if (problems.empty()) {
    fs::remove_all(backup, ec);
    fs::remove_all(TmpDir(install), ec);
    fs::remove(JournalPath(install), ec);
  }
}

// Throws std::runtime_error (plain English) after the caller restores.
inline void Swap(const fs::path& install, const fs::path& stage_root, const std::string& version) {
  std::error_code ec;
  const std::vector<std::string> fresh = FilesUnder(stage_root);
  std::set<std::string> fresh_set(fresh.begin(), fresh.end());
  std::vector<std::string> backed_up, added, stale;
  for (const auto& r : fresh) {
    if (!SafeRel(r)) throw std::runtime_error("the package holds an unexpected path");
    (fs::exists(install / fs::path(W(r)), ec) ? backed_up : added).push_back(r);
  }
  const nlohmann::json old = ReadJson(ManifestPath(install));
  if (old.is_object())
    for (const auto& r : old.value("files", std::vector<std::string>{}))
      if (SafeRel(r) && !fresh_set.count(r) && fs::exists(install / fs::path(W(r)), ec)) stale.push_back(r), backed_up.push_back(r);

  fs::remove_all(BackupDir(install), ec);
  fs::create_directories(BackupDir(install), ec);
  WriteJson(JournalPath(install), {{"version", version}, {"phase", "swapping"}, {"attempts", 0}, {"backed_up", backed_up}, {"added", added}});

  for (const auto& r : backed_up) {
    const fs::path from = install / fs::path(W(r)), to = BackupDir(install) / fs::path(W(r));
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) throw std::runtime_error("a file is in use or locked: " + r + " (" + ec.message() + ")");
  }
  for (const auto& r : fresh) {
    const fs::path from = stage_root / fs::path(W(r)), to = install / fs::path(W(r));
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) throw std::runtime_error("could not move in " + r + " (" + ec.message() + ")");
  }
  WriteJson(ManifestPath(install), {{"version", version}, {"files", fresh}});
  WriteJson(JournalPath(install), {{"version", version}, {"phase", "verify"}, {"attempts", 0}, {"backed_up", backed_up}, {"added", added}});
  fs::remove_all(TmpDir(install), ec);
}

inline bool WaitForProcess(DWORD pid, DWORD ms) {
  if (!pid) return true;
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h) return true;  // already gone
  const DWORD w = WaitForSingleObject(h, ms);
  CloseHandle(h);
  return w == WAIT_OBJECT_0;
}

inline void StartLauncher(const fs::path& install) {
  const fs::path exe = install / L"wwe2k14_launcher.exe";
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const std::wstring wd = install.wstring();
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, wd.c_str(), &si, &pi))
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
}

// The helper process: argv = [--apply-update INSTALL STAGE_ROOT VERSION PID] or [--restore-update INSTALL PID].
// Never shows a window; returns the process exit code.
inline int RunHelper(const std::vector<std::wstring>& a) {
  const bool apply = a.size() >= 5 && a[0] == L"--apply-update";
  if (!apply && !(a.size() >= 3 && a[0] == L"--restore-update")) return 2;
  const fs::path install = a[1];
  const std::string version = apply ? N(a[3]) : "";
  const DWORD pid = DWORD(_wtoi(a[apply ? 4 : 2].c_str()));
  LogLine(install, std::string(apply ? "update to " + version : "restore") + ": helper started, waiting for the launcher (pid " + std::to_string(pid) + ")");
  if (!WaitForProcess(pid, 60000)) {
    WriteJson(ResultPath(install), {{"ok", false}, {"version", version}, {"error", "the launcher did not close in time"}});
    LogLine(install, "FAILED: the launcher did not close in time");
    StartLauncher(install);
    return 1;
  }
  Sleep(400);
  std::string problems;
  if (!apply) {
    Restore(install, problems);
    WriteJson(ResultPath(install), {{"ok", false}, {"version", version}, {"error", problems.empty() ? "the new version did not start properly; the previous files were put back" : problems}});
    LogLine(install, "restore: " + (problems.empty() ? std::string("done") : problems));
    StartLauncher(install);
    return problems.empty() ? 0 : 1;
  }
  try {
    Swap(install, fs::path(a[2]), version);
    LogLine(install, "swap done; starting the new launcher");
    WriteJson(ResultPath(install), {{"ok", true}, {"version", version}});
    StartLauncher(install);
    return 0;
  } catch (const std::exception& e) {
    LogLine(install, std::string("FAILED: ") + e.what() + "; putting the old files back");
    Restore(install, problems);
    if (!problems.empty()) LogLine(install, "restore problems: " + problems);
    WriteJson(ResultPath(install), {{"ok", false}, {"version", version}, {"error", std::string(e.what()) + (problems.empty() ? "" : " (and " + problems + ")")}});
    StartLauncher(install);
    return 1;
  }
}

// 3. Hands over: this launcher's exe is copied to %TEMP% and started as the helper; the caller then quits.
inline std::string StartHelper(const fs::path& install, const fs::path& stage_root, const std::string& version) {
  wchar_t self[MAX_PATH * 2]{}, tmp[MAX_PATH]{};
  GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  GetTempPathW(MAX_PATH, tmp);
  const fs::path helper = fs::path(tmp) / L"wwe2k14_update_helper.exe";
  std::error_code ec;
  fs::copy_file(self, helper, fs::copy_options::overwrite_existing, ec);
  if (ec) return "the update helper could not be prepared (" + ec.message() + ")";
  std::wstring cmd = L"\"" + helper.wstring() + L"\" --apply-update \"" + install.wstring() + L"\" \"" + stage_root.wstring() +
                     L"\" " + W(version) + L" " + std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, tmp, &si, &pi))
    return "the update helper could not be started";
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return {};
}

inline std::string StartRestoreHelper(const fs::path& install) {
  wchar_t self[MAX_PATH * 2]{}, tmp[MAX_PATH]{};
  GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  GetTempPathW(MAX_PATH, tmp);
  const fs::path helper = fs::path(tmp) / L"wwe2k14_update_helper.exe";
  std::error_code ec;
  fs::copy_file(self, helper, fs::copy_options::overwrite_existing, ec);
  if (ec) return "the update helper could not be prepared (" + ec.message() + ")";
  std::wstring cmd = L"\"" + helper.wstring() + L"\" --restore-update \"" + install.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, tmp, &si, &pi))
    return "the update helper could not be started";
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return {};
}

}  // namespace launcher::upd
