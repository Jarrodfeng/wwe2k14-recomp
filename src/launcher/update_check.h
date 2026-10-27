// Session 31 (Jarrod): the launcher checks GitHub for a newer release when it starts.
//
// Jarrod's choices: notify only (the player opens the release page and downloads it); on every start, on by default,
// a Settings toggle turns it off; only FULL releases count (playtest pre-releases are ignored), so the GitHub API's
// "latest release" is exactly the question: GET https://api.github.com/repos/Jarrodfeng/wwe2k14-recomp/releases/latest
// returns the newest release that is neither a draft nor a pre-release (404 when there is none yet: "no update").
// One request, no personal data (GitHub sees the address like any web page); WinHTTP, part of Windows.
//
// Versions: "vMAJOR.MINOR.PATCH[-suffix][+dev]". Newer = a higher number; at the same number a release without a
// suffix is newer than one with it (v0.1.5 after v0.1.5-playtest), as in semantic versioning.

// Session 71 (the Android launcher): the version rules and ReadRelease are shared; only CheckForUpdate (WinHTTP) is
// Windows. The phone asks GitHub through Java (src\launcher_mobile\platform_android.cpp) and reads the answer here.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#include <array>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

namespace launcher {

struct UpdateInfo {
  enum State { kNone, kChecking, kLatest, kAvailable, kFailed } state = kNone;
  std::string tag, name, url, notes, error;
};

// Session 36 (Jarrod): three or four numbers, so a hotfix such as v0.1.9.1 counts as newer than v0.1.9 (a missing
// fourth number is 0). Launchers up to v0.1.8 read only the first three: a four-number version is invisible to them.
struct VersionParts {
  std::array<int, 4> n{};
  bool suffix = false;  // "-playtest" and the like: a pre-release of that number
  bool ok = false;
};
inline VersionParts ParseVersion(std::string v) {
  VersionParts p;
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
  const size_t plus = v.find('+');
  if (plus != std::string::npos) v.resize(plus);
  const size_t dash = v.find('-');
  p.suffix = dash != std::string::npos;
  if (p.suffix) v.resize(dash);
  p.ok = std::sscanf(v.c_str(), "%d.%d.%d.%d", &p.n[0], &p.n[1], &p.n[2], &p.n[3]) >= 3;
  return p;
}
inline bool IsNewer(const std::string& remote, const std::string& local) {
  const VersionParts r = ParseVersion(remote), l = ParseVersion(local);
  if (!r.ok || !l.ok) return false;
  if (r.n != l.n) return r.n > l.n;
  return !r.suffix && l.suffix;
}

// The release JSON (GitHub's answer, or a test file) against our version.
inline UpdateInfo ReadRelease(const std::string& body, const std::string& current) {
  UpdateInfo u;
  try {
    const auto j = nlohmann::json::parse(body);
    if (j.value("draft", false) || j.value("prerelease", false)) return u.state = UpdateInfo::kLatest, u;
    u.tag = j.value("tag_name", ""), u.name = j.value("name", ""), u.url = j.value("html_url", "");
    u.notes = j.value("body", "");
    u.state = !u.tag.empty() && IsNewer(u.tag, current) ? UpdateInfo::kAvailable : UpdateInfo::kLatest;
  } catch (const std::exception&) {
    u.state = UpdateInfo::kFailed, u.error = "GitHub's answer could not be read";
  }
  if (u.state == UpdateInfo::kAvailable && u.url.rfind("https://github.com/", 0) != 0)
    u.state = UpdateInfo::kFailed, u.error = "unexpected release address";  // only ever open GitHub pages
  return u;
}

#ifdef _WIN32
// Asks GitHub (blocking, a few seconds at most: run it off the UI thread).
inline UpdateInfo CheckForUpdate(const std::string& current) {
  UpdateInfo u;
  u.state = UpdateInfo::kFailed;
  const std::wstring agent = L"wwe2k14-launcher/" + std::wstring(current.begin(), current.end());
  HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return u.error = "no internet access", u;
  WinHttpSetTimeouts(s, 4000, 4000, 4000, 6000);
  HINTERNET c = WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/repos/Jarrodfeng/wwe2k14-recomp/releases/latest", nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                  : nullptr;
  std::string body;
  DWORD status = 0;
  if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                              DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(r, nullptr)) {
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    for (DWORD avail = 0; WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < (4u << 20);) {
      std::string chunk(avail, '\0');
      DWORD got = 0;
      if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
      body.append(chunk, 0, got);
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  if (status == 404) return u.state = UpdateInfo::kLatest, u;  // no full release yet
  if (status != 200) return u.error = status ? "GitHub answered " + std::to_string(status) : "GitHub could not be reached", u;
  return ReadRelease(body, current);
}
#endif  // _WIN32

}  // namespace launcher
