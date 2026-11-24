// admit: the memory admission gate every compile on this PC is started through (lane CQA, 2026-11-20; docs\agents\admit.md).
//
// Why (docs\research\harness\CQ2.md): in Session 103 several compiles at once took available memory to 0.2 GB / 0.0 GB, and "LLVM out of
// memory" was the COMMIT limit (RAM + page file = 45.9 GB here; one q refresh at -j 28 committed 48 GB). The older limits each read memory
// once and could not see what a job started a second earlier was about to use. admit keeps one machine-wide record of who holds what
// (OS-locked slot files) and counts the reservations of jobs younger than the
// pending window as still to come. Reservations per kind are measured (docs\research\harness\CQM.md).
//
//   admit run --kind K [--reserve-gb X] [--reserve-commit-gb Y] [--timeout S] [--state DIR] [--config F] -- PROGRAM ARGS...
//        (S109 GATE: no production caller; the selftest's vehicle for the rule, and the rule is the same as hold's)
//   admit hold --kind K --want N [--min M] [--per-gb X] [--per-commit-gb Y] [--timeout S] [--max-s S]
//        -> stdout "granted M <word>" + "marker V"; held until stdin EOF (a pipe) / the parent exits
//   admit hold ... --pid P --detach [--max-s S]   -> the two lines at once; a detached helper holds until P exits, `admit release`, or --max-s
//   admit release --pid P [--kind K] | admit status | admit selftest [--keep] [--target EXE] [--plant no_pending|no_expiry] [--only-gate]
//   No shim mode (CQ_ADV claims 2e / c): explicit call sites only.
//
// S109 GATE (docs\research\s109\GATE_plan.md): an ask waits at most min(--timeout, admit.json timeout_s = 180 s), for ANY reason. At 2 s of waiting it
// writes a `waiting` history row (a waiter its caller kills is still on record). At the bound it asks once more at a minimum of 1 worker (a grant below
// the asker's minimum = `granted N narrowed`); if not even that fits above the floor it is REFUSED: exit 75, a `refused` row, stdout `refused <line>`,
// and one loud stderr line naming the need, the memory, every queue holder with its age, pending, and the largest committers outside the queue.
// Nothing ever runs unadmitted after a wait (the S106 forced slot, its endless extension and --on-timeout are gone).
// Never blocks a build on its OWN error: any internal error prints "[admit] ERROR <what>: running without admission" and runs the command.
// Built by tools\admit\build_admit.ps1 (clang++ by absolute path; never through admit). The callers rebuild it from the main checkout's admit.cpp when
// its sidecar admit.exe.sha256 differs (build_admit.ps1 -IfStale): this stands in for a Python port of the gate, the structural answer once the
// exe's speed (the reason for C++: `admit run` wrapping single compiles) has no production caller.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using std::string;
using std::vector;
using std::wstring;

static wstring g_self;                 // full path of this exe
static volatile LONG g_child_running = 0;
static const int EXIT_REFUSED = 75;   // not admitted within the bound (S109 GATE), or the caller is gone (orphaned): nothing ran
static const DWORD GLOCK_WAIT_MS = 1000;   // one try at the machine lock; a busy lock is "keep waiting", never an error
static const long long HISTORY_CAP = 16LL << 20;   // history.tsv rotates to history.old.tsv past 16 MB (~100k rows)

// ---------------------------------------------------------------- small helpers
static string u8(const wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static wstring w16(const string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static string fmt(const char* f, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    return string(buf, (size_t)n);
}
static wstring wnum(unsigned long long v) { return w16(fmt("%llu", v)); }
static void write_handle(HANDLE h, const string& s) {
    if (!h || h == INVALID_HANDLE_VALUE || s.empty()) return;
    DWORD n = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr);
}
static void say_err(const string& line) { write_handle(GetStdHandle(STD_ERROR_HANDLE), line + "\n"); }
static void say_out(const string& s) { write_handle(GetStdHandle(STD_OUTPUT_HANDLE), s); }
static wstring getenv_w(const wchar_t* n) {
    DWORD k = GetEnvironmentVariableW(n, nullptr, 0);
    if (!k) return {};
    wstring v(k, L'\0');
    DWORD m = GetEnvironmentVariableW(n, v.data(), k);
    v.resize(m);
    return v;
}
static bool env_on(const wchar_t* n) { return getenv_w(n) == L"1"; }
static string winerr(DWORD e) {
    char* m = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPSTR)&m, 0, nullptr);
    string s = m ? m : "";
    if (m) LocalFree(m);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
    return fmt("error %lu: %s", e, s.c_str());
}
static wstring lower(wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static wstring dir_of(const wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == wstring::npos ? L"." : p.substr(0, k);
}
static wstring base_of(const wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == wstring::npos ? p : p.substr(k + 1);
}
static wstring ext_of(const wstring& p) {   // ".cmd" (lower case) or ""
    wstring b = base_of(p);
    size_t k = b.rfind(L'.');
    return k == wstring::npos ? L"" : lower(b.substr(k));
}
static wstring stem_lower(const wstring& p) {   // "clang++" from "C:\x\Clang++.exe"
    wstring b = lower(base_of(p));
    size_t k = b.rfind(L'.');
    return k == wstring::npos ? b : b.substr(0, k);
}
static wstring join(const wstring& a, const wstring& b) {
    if (a.empty()) return b;
    wchar_t l = a.back();
    return (l == L'\\' || l == L'/') ? a + b : a + L"\\" + b;
}
static wstring full(const wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    wstring r(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, r.data(), nullptr);
    r.resize(n);
    return r;
}
static wstring canon(const wstring& p) {   // full, long names, lower case, no trailing slash: for path comparisons only
    wstring f = full(p);
    DWORD n = GetLongPathNameW(f.c_str(), nullptr, 0);
    if (n) {
        wstring r(n, L'\0');
        n = GetLongPathNameW(f.c_str(), r.data(), n);
        if (n) { r.resize(n); f = r; }
    }
    f = lower(f);
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    return f;
}
static bool is_file(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool is_dir(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool make_dirs(const wstring& p) {
    if (is_dir(p)) return true;
    wstring up = dir_of(p);
    if (up != p && up.size() > 2 && !is_dir(up)) make_dirs(up);
    CreateDirectoryW(p.c_str(), nullptr);
    return is_dir(p);
}
static bool read_file(const wstring& p, string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    return true;
}
static bool write_file(const wstring& p, const string& data) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &n, nullptr);
    CloseHandle(h);
    return ok && n == data.size();
}
static double qpc_s() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
static unsigned long long now_tick() { return GetTickCount64(); }   // ms since boot: monotonic and the same in every process
static unsigned long long now_ft() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static string local_stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return fmt("%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}
static bool has_console() {
    DWORD pl[2];
    return GetConsoleProcessList(pl, 2) > 0;
}
static unsigned long long proc_created(HANDLE h) {
    FILETIME c{}, x1, x2, x3;
    if (!GetProcessTimes(h, &c, &x1, &x2, &x3)) return 0;
    return ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime;
}

// ---------------------------------------------------------------- a small JSON reader (config, slot records)
struct JV {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    string s;
    vector<JV> a;
    vector<std::pair<string, JV>> o;
    const JV* get(const char* k) const {
        if (t != OBJ) return nullptr;
        for (auto& p : o)
            if (p.first == k) return &p.second;
        return nullptr;
    }
    double num(const char* k, double d) const {
        auto* v = get(k);
        return v && v->t == NUM ? v->n : d;
    }
    string str(const char* k, const string& d) const {
        auto* v = get(k);
        return v && v->t == STR ? v->s : d;
    }
};
struct JP {
    const string& s;
    size_t i = 0;
    string err;
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }
    bool fail(const char* m) {
        if (err.empty()) err = fmt("%s at byte %zu", m, i);
        return false;
    }
    bool str(string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected a string");
        i++;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("bad \\u escape");
                    unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool val(JV& v, int depth) {
        if (depth > 32) return fail("nested too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            v.t = JV::OBJ;
            i++;
            ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            for (;;) {
                ws();
                string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                i++;
                JV x;
                if (!val(x, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(x));
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.t = JV::ARR;
            i++;
            ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            for (;;) {
                JV x;
                if (!val(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.t = JV::STR; return str(v.s); }
        if (s.compare(i, 4, "true") == 0) { v.t = JV::BOOL; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v.t = JV::BOOL; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { v.t = JV::NUL; i += 4; return true; }
        const char* b = s.c_str() + i;
        char* e = nullptr;
        double d = strtod(b, &e);
        if (e == b || !std::isfinite(d)) return fail("bad value");
        v.t = JV::NUM;
        v.n = d;
        i += (size_t)(e - b);
        return true;
    }
};
static bool json_parse(const string& text, JV& v, string& err) {
    string s = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.substr(3) : text;
    JP p{s};
    if (!p.val(v, 0)) { err = p.err; return false; }
    p.ws();
    if (p.i != s.size()) { err = fmt("trailing data at byte %zu", p.i); return false; }
    return true;
}
static string json_esc(const string& s) {
    string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) o += fmt("\\u%04x", c);
        else o += (char)c;
    }
    return o;
}

// ---------------------------------------------------------------- config, state, memory
struct Kind {
    double phys = 2.0, commit = 2.0;
    int slots = 32;
    vector<string> covers;   // kinds a hold of this kind covers besides itself ("cc.*" = prefix)
};
struct Config {
    std::map<string, Kind> kinds;
    double floor_phys = 4, floor_commit = 4, window_s = 60, timeout_s = 900, starve_s = 30;
    string default_kind = "cc.tool";
    string mode = "observe";   // observe (decide once, log it, admit at once) | enforce (wait); env WWE_ADMIT_MODE overrides
    vector<string> warnings;   // ignored keys and dropped kinds: said on stderr, never a reason to fall open
    wstring path;
};
// A kind name becomes part of a slot file name "<kind>.<n>" (parse_slot_name): letters, digits, '_', '-', '.' only.
static bool kind_name_ok(const string& n) {
    if (n.empty() || n.size() > 48 || n.front() == '.' || n.back() == '.') return false;
    for (char ch : n)
        if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.')) return false;
    return true;
}
// Lenient where it is safe (CQA review 4, item 6): an unknown key or a wrong-typed value is a warning and is ignored (the default stays); a bad
// kind (name, type or field) drops only that kind (asks for it get default_kind with a warning). Only an unreadable / unparsable file, or no
// usable kind at all, is an error: the caller falls open and every grant says "open".
static bool load_config(const wstring& path, Config& c, string& err) {
    string text;
    string where = "config " + u8(path) + ": ";
    if (!read_file(path, text)) { err = "config " + u8(path) + " unreadable"; return false; }
    JV root;
    string e;
    if (!json_parse(text, root, e)) { err = where + e; return false; }
    if (root.t != JV::OBJ) { err = where + "not an object"; return false; }
    auto warn = [&](const string& w) { c.warnings.push_back(where + w); };
    auto number = [&](const JV& v, const string& key, double& out) {
        if (v.t != JV::NUM || v.n < 0) return false;
        out = v.n;
        return true;
    };
    const JV* ks = nullptr;
    for (auto& [key, v] : root.o) {
        if (!key.empty() && key[0] == '_') continue;
        bool ok = true;
        if (key == "floor_phys_gb") ok = number(v, key, c.floor_phys);
        else if (key == "floor_commit_gb") ok = number(v, key, c.floor_commit);
        else if (key == "pending_window_s") ok = number(v, key, c.window_s);
        else if (key == "timeout_s") ok = number(v, key, c.timeout_s);
        else if (key == "starve_s") ok = number(v, key, c.starve_s);
        else if (key == "default_kind") ok = v.t == JV::STR && (c.default_kind = v.s, true);
        else if (key == "mode") ok = v.t == JV::STR && (v.s == "observe" || v.s == "enforce") && (c.mode = v.s, true);
        else if (key == "kinds") ok = v.t == JV::OBJ && (ks = &v, true);
        else { warn("unknown key '" + key + "' ignored"); continue; }
        if (!ok) warn("'" + key + "' has a bad value: ignored (default kept)");
    }
    if (ks)
        for (auto& [name, v] : ks->o) {
            if (!name.empty() && name[0] == '_') continue;
            if (!kind_name_ok(name)) { warn("kind name '" + name + "' (letters, digits, _ - . only): kind dropped"); continue; }
            if (v.t != JV::OBJ) { warn("kind '" + name + "' is not an object: kind dropped"); continue; }
            Kind k;
            string bad;
            for (auto& [key, x] : v.o) {
                if (!key.empty() && key[0] == '_') continue;
                string kk = "kinds." + name + "." + key;
                double d = 0;
                if (key == "reserve_phys_gb") { if (!number(x, kk, k.phys)) bad = kk; }
                else if (key == "reserve_commit_gb") { if (!number(x, kk, k.commit)) bad = kk; }
                else if (key == "max_slots") {
                    if (!number(x, kk, d) || d < 1 || d > 4096 || d != std::floor(d)) bad = kk;
                    else k.slots = (int)d;
                } else if (key == "covers") {
                    if (x.t != JV::ARR) bad = kk;
                    else
                        for (auto& y : x.a)
                            if (y.t == JV::STR) k.covers.push_back(y.s);
                            else bad = kk;
                } else {
                    warn("unknown key '" + kk + "' ignored");
                }
            }
            if (!bad.empty()) { warn("'" + bad + "' has a bad value: kind '" + name + "' dropped"); continue; }
            c.kinds[name] = k;
        }
    if (c.kinds.empty()) { err = where + "no usable kinds"; return false; }
    if (!c.kinds.count(c.default_kind)) {
        string pick = c.kinds.count("cc.tool") ? "cc.tool" : c.kinds.begin()->first;
        warn("default_kind '" + c.default_kind + "' is not a kind: using " + pick);
        c.default_kind = pick;
    }
    c.path = path;
    return true;
}
static bool kind_covers(const Config& c, const string& hold_kind, const string& run_kind) {
    if (hold_kind == run_kind) return true;
    auto it = c.kinds.find(hold_kind);
    if (it == c.kinds.end()) return false;
    for (auto& p : it->second.covers) {
        if (p == run_kind) return true;
        if (!p.empty() && p.back() == '*' && run_kind.compare(0, p.size() - 1, p, 0, p.size() - 1) == 0) return true;
    }
    return false;
}
static wstring find_up(const wstring& start, const wstring& rel) {
    wstring d = start;
    for (int i = 0; i < 10; i++) {
        wstring p = join(d, rel);
        if (is_file(p)) return p;
        wstring up = dir_of(d);
        if (up == d || up.size() < 3) break;
        d = up;
    }
    return {};
}
// S106 0C: the main checkout of the repo this exe sits in. `.git` a directory = that checkout; `.git` a file ("gitdir: <main>\.git\worktrees\X") = a
// worktree, whose main checkout is the part before "\.git\worktrees\". "" = no repo above the exe.
static wstring main_checkout_of(const wstring& start) {
    wstring d = start;
    for (int i = 0; i < 12; i++) {
        wstring g = join(d, L".git");
        DWORD a = GetFileAttributesW(g.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) {
            if (a & FILE_ATTRIBUTE_DIRECTORY) return d;
            string t;
            if (read_file(g, t) && t.rfind("gitdir:", 0) == 0) {
                wstring gd = w16(t.substr(7));
                while (!gd.empty() && (gd.back() == L'\n' || gd.back() == L'\r' || gd.back() == L' ')) gd.pop_back();
                while (!gd.empty() && gd.front() == L' ') gd.erase(0, 1);
                for (auto& ch : gd) if (ch == L'/') ch = L'\\';
                if (!(gd.size() > 1 && (gd[1] == L':' || (gd[0] == L'\\' && gd[1] == L'\\')))) gd = full(join(d, gd));   // S106 0CF (review F11): a relative `gitdir:` is relative to this folder
                size_t k = gd.find(L"\\.git\\worktrees\\");
                if (k != wstring::npos) return gd.substr(0, k);
            }
            return d;
        }
        wstring above = dir_of(d);
        if (above == d || above.size() < 3) break;
        d = above;
    }
    return {};
}
static wstring default_state() {
    // S106 0C: ONE folder for every tool and every worktree: <main checkout>\out\harness\admit (the PowerShell tool and the Bash tool see different
    // copies of <LocalAppData> (a Claude Code sandbox quirk): S105 ran two queues that could not see each other's holds, docs\research\s106\0C.md).
    wstring root = main_checkout_of(dir_of(g_self));
    if (!root.empty()) return join(join(join(root, L"out"), L"harness"), L"admit");
    // last fallback only (an exe outside any repo): one folder per Windows user, <LocalAppData>\wwe2k14-admit. The known
    // folder first (a process started with a bare environment has no LOCALAPPDATA), then the variable; "" = neither: the caller falls open.
    static const GUID kLocalAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};   // FOLDERID_LocalAppData
    PWSTR p = nullptr;
    wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(kLocalAppData, 0, nullptr, &p)) && p && *p) r = join(p, L"wwe2k14-admit");
    if (p) CoTaskMemFree(p);
    if (r.empty()) {
        wstring la = getenv_w(L"LOCALAPPDATA");
        if (!la.empty()) r = join(la, L"wwe2k14-admit");
    }
    return r;
}
static bool under_temp(const wstring& p) {
    wchar_t t[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 1, t);
    if (!n) return false;
    wstring tp = canon(wstring(t, n));
    wstring pp = canon(p);
    return tp.size() > 3 && pp.size() > tp.size() && pp.compare(0, tp.size(), tp) == 0 && (pp[tp.size()] == L'\\');
}

struct Ctx {
    wstring state;
    Config cfg;
    bool test = false;               // WWE_ADMIT_TEST=1 AND the state folder is under %TEMP%: the fake memory file and plants are honoured
    bool plant_no_pending = false;   // selftest planted faults: the pending window off / young-holder ages never expire
    bool plant_no_expiry = false;
    bool plant_no_tiebreak = false;  // two seniors with the same start tick: no pid tie-break (the round-3 freeze)
    bool plant_no_outer_alone = false, plant_no_inner_alone = false;   // S106 C5F note 4: the alone pause looked at before / inside the machine lock (a plant drops one look)
    HANDLE glock = INVALID_HANDLE_VALUE;
    string cfg_err, state_err;
};
static bool setup(Ctx& c, const wstring& state_opt, const wstring& cfg_opt, string& err) {
    wstring cf = !cfg_opt.empty() ? cfg_opt : getenv_w(L"WWE_ADMIT_CONFIG");
    if (cf.empty()) cf = find_up(dir_of(g_self), L"tools\\admit\\admit.json");
    if (cf.empty()) c.cfg_err = "config tools\\admit\\admit.json not found above " + u8(dir_of(g_self));
    else load_config(full(cf), c.cfg, c.cfg_err);
    wstring em = getenv_w(L"WWE_ADMIT_MODE");
    if (em == L"observe" || em == L"enforce") c.cfg.mode = u8(em);
    else if (!em.empty()) c.cfg.warnings.push_back("WWE_ADMIT_MODE '" + u8(em) + "' is not observe|enforce: ignored");
    for (auto& w : c.cfg.warnings) say_err("[admit] warning: " + w);

    wstring st = !state_opt.empty() ? state_opt : getenv_w(L"WWE_ADMIT_DIR");
    if (st.empty()) st = default_state();
    if (st.empty()) {
        c.state_err = "no state folder: neither SHGetKnownFolderPath(LocalAppData) nor LOCALAPPDATA gave one";
    } else if (st = full(st); !make_dirs(st)) {
        c.state_err = "state folder " + u8(st) + " cannot be created";
    } else {
        c.state = st;
        make_dirs(join(st, L"waiters"));
        c.glock = CreateFileW(join(st, L"admit.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (c.glock == INVALID_HANDLE_VALUE) c.state_err = "state folder " + u8(st) + " not writable (" + winerr(GetLastError()) + ")";
    }
    c.test = !c.state.empty() && env_on(L"WWE_ADMIT_TEST") && under_temp(c.state);
    wstring plant = getenv_w(L"WWE_ADMIT_PLANT");
    c.plant_no_pending = c.test && plant == L"no_pending";
    c.plant_no_expiry = c.test && plant == L"no_expiry";
    c.plant_no_tiebreak = c.test && plant == L"no_tiebreak";
    c.plant_no_outer_alone = c.test && (plant == L"no_outer_alone" || plant == L"no_alone_looks");
    c.plant_no_inner_alone = c.test && plant == L"no_alone_looks";
    err = !c.state_err.empty() ? c.state_err : c.cfg_err;
    return err.empty();
}

struct Mem {
    double phys = -1, commit = -1, total_phys = -1;
    bool fake = false;
};
// Commit left = GlobalMemoryStatusEx ullAvailPageFile (the system's commit limit minus its commit charge, the figure GetPerformanceInfo's
// CommitLimit - CommitTotal gives). Measured on this PC (admit __memcost, 2026-11-20 ~02:10): GlobalMemoryStatusEx 1.0 us / call, GetPerformanceInfo
// 32.6 us / call, both 30.07 GB at that moment; one cheap call gives all three numbers.
static bool read_mem(const Ctx& c, Mem& m, string& err) {
    const double GB = 1024.0 * 1024.0 * 1024.0;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (!GlobalMemoryStatusEx(&ms)) { err = "GlobalMemoryStatusEx " + winerr(GetLastError()); return false; }
    m.total_phys = (double)ms.ullTotalPhys / GB;
    if (c.test) {   // selftest: "<avail_phys_gb> <commit_left_gb>" in <state>\fake_mem.txt (the total stays the real one)
        string t;
        if (read_file(join(c.state, L"fake_mem.txt"), t)) {
            double a = 0, b = 0;
            if (sscanf(t.c_str(), "%lf %lf", &a, &b) == 2) { m.phys = a; m.commit = b; m.fake = true; return true; }
        }
    }
    m.phys = (double)ms.ullAvailPhys / GB;
    m.commit = (double)ms.ullAvailPageFile / GB;
    return true;
}

// ---------------------------------------------------------------- the machine-wide lock and the slot files
enum GLock { GL_TAKEN, GL_BUSY, GL_ERROR };
static GLock glock_take(Ctx& c, DWORD ms, string& err) {
    if (c.glock == INVALID_HANDLE_VALUE) { err = "no machine lock handle"; return GL_ERROR; }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) { err = "CreateEvent " + winerr(GetLastError()); return GL_ERROR; }
    GLock r = GL_TAKEN;
    if (!LockFileEx(c.glock, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0) CancelIoEx(c.glock, &ov);
            DWORD n = 0;
            if (!GetOverlappedResult(c.glock, &ov, &n, TRUE)) r = GL_BUSY;   // cancelled: someone holds it; try again on the next poll
        } else {
            err = "machine lock " + winerr(e);
            r = GL_ERROR;
        }
    }
    CloseHandle(ov.hEvent);
    return r;
}
static void glock_release(Ctx& c) {
    OVERLAPPED ov{};
    UnlockFileEx(c.glock, 0, 1, 0, &ov);
}
// A slot file "<kind>.<index>" in the state folder: byte 0 is the OS lock (LockFileEx; the CRT's msvcrt.locking takes the same Win32 lock), a
// JSON record from byte 1. Opened sharing read + write (Python's os.open is deny-none; CQ_ADV claim 5), never delete.
static HANDLE slot_open(const wstring& p, bool create) {
    return CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}
static bool try_lock0(HANDLE h) {
    OVERLAPPED ov{};
    return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) != FALSE;
}
static void unlock0(HANDLE h) {
    OVERLAPPED ov{};
    UnlockFileEx(h, 0, 1, 0, &ov);
}
static string read_record(HANDLE h) {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 1) return {};
    DWORD want = (DWORD)std::min<long long>(sz.QuadPart - 1, 8192);
    string s(want, '\0');
    OVERLAPPED ov{};
    ov.Offset = 1;
    DWORD got = 0;
    if (!ReadFile(h, s.data(), want, &got, &ov)) return {};
    s.resize(got);
    return s;
}
static void write_record(HANDLE h, const string& js) {
    LARGE_INTEGER one{};
    one.QuadPart = 1;
    SetFilePointerEx(h, one, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    DWORD n = 0;
    WriteFile(h, js.data(), (DWORD)js.size(), &n, nullptr);
}
static bool parse_slot_name(const wstring& name, string& kind, int& idx) {
    size_t dot = name.rfind(L'.');
    if (dot == wstring::npos || dot == 0 || dot + 1 >= name.size() || name.size() - dot > 6) return false;
    for (size_t i = dot + 1; i < name.size(); i++)
        if (name[i] < L'0' || name[i] > L'9') return false;
    for (size_t i = 0; i < dot; i++) {
        wchar_t ch = name[i];
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'_' || ch == L'-' || ch == L'.')) return false;
    }
    kind = u8(name.substr(0, dot));
    idx = _wtoi(name.c_str() + dot + 1);
    return true;
}

struct Holder {
    string kind, label, mode;
    int index = 0;
    unsigned long pid = 0, for_pid = 0, parent = 0;   // S106 0C: parent = the process that started this admit (the owner of a stdin-form hold)
    unsigned long long for_ct = 0;  // the owner's creation time: a dead owner's pid reused later is not it
    unsigned long long parent_ct = 0;   // S106 0CF: the creation time of `parent` (the owner of a stdin-form hold)
    double phys = 0, commit = 0, age_s = 0;
    int workers = 1;
};
static vector<Holder> scan(const Ctx& c) {
    vector<Holder> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(join(c.state, L"*.*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return out;
    unsigned long long ft_now = now_ft(), tk_now = now_tick();
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        string kind;
        int idx = 0;
        if (!parse_slot_name(fd.cFileName, kind, idx)) continue;
        HANDLE h = slot_open(join(c.state, fd.cFileName), false);
        bool live = false;
        string rec;
        if (h == INVALID_HANDLE_VALUE) {
            live = GetLastError() == ERROR_SHARING_VIOLATION;   // opened by someone without sharing: count it
        } else {
            if (try_lock0(h)) unlock0(h);
            else { live = true; rec = read_record(h); }
            CloseHandle(h);
        }
        if (!live) continue;
        Holder hd;
        hd.kind = kind;
        hd.index = idx;
        auto k = c.cfg.kinds.find(kind);
        if (k != c.cfg.kinds.end()) { hd.phys = k->second.phys; hd.commit = k->second.commit; }
        unsigned long long wt = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        hd.age_s = wt < ft_now ? (double)(ft_now - wt) / 1e7 : 0;   // a record without a tick (a Python holder): the file's write time
        JV j;
        string e;
        if (!rec.empty() && json_parse(rec, j, e) && j.t == JV::OBJ) {
            hd.pid = (unsigned long)j.num("pid", 0);
            hd.for_pid = (unsigned long)j.num("for_pid", 0);
            hd.parent = (unsigned long)j.num("parent", 0);   // S106 0C
            hd.for_ct = strtoull(j.str("for_ct", "0").c_str(), nullptr, 10);   // a string: a FILETIME does not fit a double exactly
            hd.parent_ct = strtoull(j.str("parent_ct", "0").c_str(), nullptr, 10);   // S106 0CF
            hd.phys = j.num("phys_gb", hd.phys);
            hd.commit = j.num("commit_gb", hd.commit);
            hd.workers = (int)j.num("workers", 1);
            hd.mode = j.str("mode", "");
            double st = j.num("start_tick", 0);
            if (st > 0) hd.age_s = (double)tk_now > st ? ((double)tk_now - st) / 1000.0 : 0;
            hd.label = j.str("prog", j.str("check", ""));
        }
        out.push_back(hd);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out;
}

// ---------------------------------------------------------------- waiters (fairness)
// A waiter registers <state>\waiters\<pid>.ticket after its first refused attempt: byte 0 locked, delete-on-close (a dead waiter's ticket
// vanishes with its handle), a JSON record from byte 1, rewritten every poll with the waiter's need (one worker) and its last refusal reason.
// A waiter older than starve_s, or one whose caller holds the run lock (WWE2K14_RUNLOCK_TOKEN), is senior. A younger waiter gives way to a
// senior ONLY when the senior was last refused on memory or commit, its need can ever fit this PC, and admitting the younger one would take
// from a dimension the senior is short of (CQA review 3, blocker 2). Order among seniors: run lock, then the oldest tick, then the lowest pid.
struct Waiter {
    unsigned long pid = 0;
    string kind, reason;
    double phys = 0, commit = 0, waited_s = 0;
    bool runlock = false;
    unsigned long long start_tick = 0;
};
static HANDLE ticket_open(const Ctx& c) {
    wstring p = join(join(c.state, L"waiters"), wnum(GetCurrentProcessId()) + L".ticket");
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    if (!try_lock0(h)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    return h;
}
static void ticket_write(HANDLE h, const string& kind, double phys, double commit, unsigned long long start_tick, bool runlock, const string& reason) {
    write_record(h, fmt("{\"pid\":%lu,\"kind\":\"%s\",\"phys_gb\":%.3f,\"commit_gb\":%.3f,\"start_tick\":%llu,\"runlock\":%d,\"reason\":\"%s\"}", GetCurrentProcessId(),
                        json_esc(kind).c_str(), phys, commit, start_tick, runlock ? 1 : 0, reason.c_str()));
}
static vector<Waiter> scan_waiters(const Ctx& c) {
    vector<Waiter> out;
    wstring dir = join(c.state, L"waiters");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(join(dir, L"*.ticket").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return out;
    unsigned long long tk = now_tick();
    do {
        HANDLE h = CreateFileW(join(dir, fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;   // being deleted: its waiter is gone
        string rec;
        bool live = !try_lock0(h);
        if (!live) unlock0(h);
        else rec = read_record(h);
        CloseHandle(h);
        JV j;
        string e;
        if (!live || rec.empty() || !json_parse(rec, j, e) || j.t != JV::OBJ) continue;
        Waiter w;
        w.pid = (unsigned long)j.num("pid", 0);
        w.kind = j.str("kind", "");
        w.reason = j.str("reason", "");
        w.phys = j.num("phys_gb", 0);
        w.commit = j.num("commit_gb", 0);
        w.runlock = j.num("runlock", 0) != 0;
        w.start_tick = (unsigned long long)j.num("start_tick", 0);
        w.waited_s = tk > w.start_tick ? (double)(tk - w.start_tick) / 1000.0 : 0;
        out.push_back(w);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out;
}

// ---------------------------------------------------------------- admission
struct Want {
    string kind, mode = "run", prog;
    double phys = 0, commit = 0;   // per worker
    int want = 1;
    int min = 1;                   // hold --min: never grant fewer workers than this (CQA addendum A4)
    unsigned long for_pid = 0;     // hold: the process the reservation is for (--pid); 0 = none named
    unsigned long watch_pid = 0;   // whose exit makes a waiter an orphan (0 = the parent)
    unsigned long long t0_tick = 0;
    bool runlock = false;
};
struct Snap {
    Mem mem;
    int holders = 0;
    double pend_p = 0, pend_c = 0, use_p = 0;   // S106 0C: use_p = what the holds' owner trees use now
    string reason;   // memory / commit / slots / lock / senior: also written to the waiter's ticket
    string why;      // the sentence for the waiting line when the reason is slots or senior
    vector<Holder> hs;   // S109 GATE: the live holders the decision saw (named in a refusal)
};
struct Slot {
    HANDLE h;
    wstring path;
};
// Releasing deletes the slot file too (best effort: a file someone else has open stays), so the folder holds about as many files as live
// holders and a scan stays cheap (CQA review 3, item 5).
struct Taken {
    vector<Slot> slots;
    void release() {
        for (auto& s : slots) {
            unlock0(s.h);
            CloseHandle(s.h);
            DeleteFileW(s.path.c_str());
        }
        slots.clear();
    }
};
static unsigned long parent_pid() {
    // NtQueryInformationProcess(ProcessBasicInformation) costs microseconds; a toolhelp snapshot of every process costs milliseconds
    struct PBI {
        LONG ExitStatus;
        PVOID Peb;
        ULONG_PTR Affinity;
        LONG BasePriority;
        ULONG_PTR Pid;
        ULONG_PTR ParentPid;
    } pbi{};
    typedef LONG(WINAPI * NtQIP)(HANDLE, int, PVOID, ULONG, PULONG);
    static NtQIP fn = (NtQIP)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (fn && fn(GetCurrentProcess(), 0, &pbi, sizeof pbi, nullptr) == 0) return (unsigned long)pbi.ParentPid;
    return 0;
}
static unsigned long long pid_created(unsigned long pid) {
    HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!h) return 0;
    unsigned long long ct = proc_created(h);
    CloseHandle(h);
    return ct;
}
// The order among seniors: a run-lock holder first, then the oldest start tick, then the lowest pid (ticks move every ~15.6 ms, so a burst
// shares one: without the pid two seniors could each give way to the other forever; CQA review 3, blocker 1).
static bool senior_before(const Ctx& c, bool rl_a, unsigned long long st_a, unsigned long pid_a, bool rl_b, unsigned long long st_b, unsigned long pid_b) {
    if (rl_a != rl_b) return rl_a;
    if (st_a != st_b) return st_a < st_b;
    if (c.plant_no_tiebreak) return false;
    return pid_a < pid_b;
}
// The generation is bumped (under the lock) at every take. A check whose scan, made outside the lock, predates the last take scans again under
// the lock; otherwise the lock covers only the take itself (CQA review 3, item 5). Releases do not bump it: a stale holder only makes a check
// stricter. Python holders do not bump it either (they are caught by their slot lock, not by this re-check).
static unsigned long long read_gen(const Ctx& c) {
    string t;
    return read_file(join(c.state, L"generation"), t) ? strtoull(t.c_str(), nullptr, 10) : 0;
}
struct View {
    vector<Holder> hs;
    double pend_p = 0, pend_c = 0;
    double use_p = 0, use_c = 0;   // S106 0C: what the holds' owner trees were measured using (already out of the available memory)
};
// S106 0C: the measured use of a hold's owner tree (the processes under the owner, not the owner itself), so that `pending` counts only the UNUSED part
// of a reservation: `ullAvailPhys` already excludes what a running pool uses, and counting its whole reservation as well counted it twice
// (docs\research\s106\0C.md: 141 of 174 waiting states in S105 had memory free and were refused only by that). WorkingSetSize for physical,
// PrivateUsage for commit. false = not measurable (the owner is gone, cannot be opened, or is another process with the same pid): the caller then
// counts the full reservation, as before. Test mode (the selftest, WWE_ADMIT_TEST under %TEMP%): `<state>\fake_use.txt` lines "<pid> <phys_gb> <commit_gb>";
// an owner not listed (or no file) uses 0, so the selftest never depends on what its own processes happen to use.
struct ProcTree {
    std::map<unsigned long, vector<unsigned long>> kids;
};
static void snap_tree(ProcTree& t) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W ent{};
    ent.dwSize = sizeof ent;
    if (Process32FirstW(s, &ent)) do { if (ent.th32ProcessID != ent.th32ParentProcessID) t.kids[ent.th32ParentProcessID].push_back(ent.th32ProcessID); } while (Process32NextW(s, &ent));
    CloseHandle(s);
}
// S106 0CF (review B2): the physical figure of one process = its PRIVATE working set. WorkingSetSize counts the clang image, DLLs and mapped headers in every
// process that maps them, so a sum over a pool's processes overstated its use and understated `pending` (the floor guarantee). priv_ws < 0 = the counter is
// not available (Windows before 11 lacks PrivateWorkingSetSize): then min(WorkingSetSize, PrivateUsage). Never more than the working set.
static double pick_private_phys(double ws, double priv_usage, double priv_ws) {
    if (priv_ws >= 0) return std::min(ws, priv_ws);
    return std::min(ws, priv_usage);
}
struct ProcUse {
    bool ok = false;                 // false = not readable: the process and everything under it is not counted (the safe direction)
    unsigned long long ct = 0;       // its creation time
    double phys = 0, commit = 0;     // GB
};
// The tree walk, apart from the process table so a selftest can feed it a fake one (review B1). Toolhelp's th32ParentProcessID is the creator's pid AT
// CREATION and is never updated: when the owner's pid was recycled, a process created long before the owner still names it. A process is counted only if
// it was created AT OR AFTER its parent (the owner's creation time first, then each child's own for the next level): a real child cannot be older. A
// process whose creation time cannot be read is not counted, nor anything under it. `exclude` = every live holder's own pid and every other hold owner
// (review F6): what is not the pool's is never credited to the hold. owner_ct 0 = unknown: nothing under the owner can be told from a reused pid, nothing counted.
static void walk_tree(const ProcTree& t, const std::function<ProcUse(unsigned long)>& use, unsigned long owner, unsigned long long owner_ct,
                      const std::set<unsigned long>& exclude, double& phys, double& commit) {
    phys = commit = 0;
    if (!owner_ct) return;
    vector<std::pair<unsigned long, unsigned long long>> todo{{owner, owner_ct}};
    std::set<unsigned long> seen{owner};
    while (!todo.empty()) {
        auto cur = todo.back();
        todo.pop_back();
        auto it = t.kids.find(cur.first);
        if (it == t.kids.end()) continue;
        for (unsigned long k : it->second) {
            if (seen.count(k) || exclude.count(k)) continue;   // (a reused pid can close a loop)
            seen.insert(k);
            ProcUse u = use(k);
            if (!u.ok || !u.ct || u.ct < cur.second) continue;   // unreadable, or older than its recorded parent: a recycled pid, not a child
            phys += u.phys;
            commit += u.commit;
            todo.push_back({k, u.ct});
        }
    }
}
static ProcUse real_use(unsigned long pid) {
    const double GB = 1024.0 * 1024.0 * 1024.0;
    ProcUse u;
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return u;
    u.ct = proc_created(hp);
    PROCESS_MEMORY_COUNTERS_EX2 pm{};
    pm.cb = sizeof pm;
    if (GetProcessMemoryInfo(hp, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) {
        u.phys = pick_private_phys((double)pm.WorkingSetSize, (double)pm.PrivateUsage, (double)pm.PrivateWorkingSetSize) / GB;
        u.commit = (double)pm.PrivateUsage / GB;
        u.ok = true;
    } else {
        PROCESS_MEMORY_COUNTERS_EX p1{};
        p1.cb = sizeof p1;
        if (GetProcessMemoryInfo(hp, (PROCESS_MEMORY_COUNTERS*)&p1, sizeof p1)) {
            u.phys = pick_private_phys((double)p1.WorkingSetSize, (double)p1.PrivateUsage, -1) / GB;
            u.commit = (double)p1.PrivateUsage / GB;
            u.ok = true;
        }
    }
    CloseHandle(hp);
    return u;
}
static bool tree_use(const Ctx& c, const ProcTree& t, const std::set<unsigned long>& exclude, unsigned long owner, unsigned long long want_ct, double& phys,
                     double& commit) {
    phys = commit = 0;
    if (c.test) {
        // `<state>\fake_tree.txt` (a fake process table): lines "<pid> <ppid> <ct> <phys_gb> <commit_gb>"; the owner's own row gives its creation time
        // (1 when it has none). Without it, `<state>\fake_use.txt` lines "<pid> <phys_gb> <commit_gb>" (the owner's use, no tree).
        string f;
        if (read_file(join(c.state, L"fake_tree.txt"), f)) {
            ProcTree ft;
            std::map<unsigned long, ProcUse> rows;
            unsigned long long owner_ct = 1;
            size_t p = 0;
            while (p < f.size()) {
                size_t e = f.find('\n', p);
                if (e == string::npos) e = f.size();
                unsigned long pid = 0, ppid = 0;
                unsigned long long ct = 0;
                double a = 0, b = 0;
                if (sscanf(f.substr(p, e - p).c_str(), "%lu %lu %llu %lf %lf", &pid, &ppid, &ct, &a, &b) == 5) {
                    ProcUse u;
                    u.ok = true;
                    u.ct = ct;
                    u.phys = a;
                    u.commit = b;
                    rows[pid] = u;
                    if (pid == owner) owner_ct = ct;
                    else ft.kids[ppid].push_back(pid);
                }
                p = e + 1;
            }
            walk_tree(ft, [&rows](unsigned long k) { auto i = rows.find(k); return i == rows.end() ? ProcUse{} : i->second; }, owner, owner_ct, exclude, phys, commit);
            return owner != 0;
        }
        if (read_file(join(c.state, L"fake_use.txt"), f)) {
            size_t p = 0;
            while (p < f.size()) {
                size_t e = f.find('\n', p);
                if (e == string::npos) e = f.size();
                unsigned long who = 0;
                double a = 0, b = 0;
                if (sscanf(f.substr(p, e - p).c_str(), "%lu %lf %lf", &who, &a, &b) == 3 && who == owner) { phys = a; commit = b; }
                p = e + 1;
            }
        }
        return owner != 0;
    }
    if (!owner) return false;
    if (!want_ct) return false;   // S106 0CFF (review B-b): no recorded creation time (every hold written before 0CF) = unknown identity: the pid may be a stranger's now, its tree is not walked and the reservation counts in full
    HANDLE ho = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
    if (!ho) return false;
    DWORD code = 0;
    unsigned long long owner_ct = proc_created(ho);
    bool alive = GetExitCodeProcess(ho, &code) && code == STILL_ACTIVE && owner_ct == want_ct;
    CloseHandle(ho);
    if (!alive) return false;
    walk_tree(t, real_use, owner, owner_ct, exclude, phys, commit);
    return true;
}
// pending = what live holders have reserved but may not use yet. A HOLD's reservation counts for its whole life (CQA review 4, item 2): a build
// lasts minutes and a running compiler sits near half its peak, so after a minute a second pool would be admitted on a dip and both would peak
// together. Only a `run` record (one process whose use does show up) and a Python holder count for the pending window alone.
static View view_of(const Ctx& c) {
    View v;
    v.hs = scan(c);
    // S106 0C: a HOLD slot counts its reservation minus what its owner's process tree is measured using (spread over the owner's live slots)
    std::map<unsigned long, int> slots_of;   // owner pid -> live hold slots
    auto owner_of = [](const Holder& h) { return h.for_pid ? h.for_pid : h.parent; };
    for (auto& h : v.hs)
        if (h.mode.rfind("hold", 0) == 0 && owner_of(h)) slots_of[owner_of(h)]++;
    ProcTree tree;
    std::set<unsigned long> exclude;   // S106 0CF (review F6): every live holder's own pid and every hold owner: not the pool's use
    for (auto& h : v.hs) exclude.insert(h.pid);
    for (auto& so : slots_of) exclude.insert(so.first);
    std::map<unsigned long, std::pair<double, double>> used;   // owner -> measured (phys, commit); absent = not measurable
    if (!slots_of.empty() && !c.test) snap_tree(tree);
    for (auto& so : slots_of) {
        double up = 0, uc = 0;
        unsigned long long ct = 0;
        for (auto& h : v.hs) {   // the creation time the hold recorded for its owner (for_pid), or for its parent (a stdin-form hold: review B1)
            if (h.for_pid == so.first && h.for_ct) ct = h.for_ct;
            else if (!h.for_pid && h.parent == so.first && h.parent_ct) ct = h.parent_ct;
        }
        std::set<unsigned long> ex = exclude;
        ex.erase(so.first);   // (the owner itself is the walk's root: other owners stay excluded)
        if (tree_use(c, tree, ex, so.first, ct, up, uc)) { used[so.first] = {up, uc}; v.use_p += up; v.use_c += uc; }
    }
    for (auto& h : v.hs) {
        bool is_hold = h.mode.rfind("hold", 0) == 0;
        bool counts = c.plant_no_expiry || is_hold || h.age_s < c.cfg.window_s;
        if (c.plant_no_pending || !counts) continue;
        auto u = is_hold ? used.find(owner_of(h)) : used.end();
        if (u != used.end()) {
            int n = std::max(1, slots_of[owner_of(h)]);
            v.pend_p += std::max(0.0, h.phys - u->second.first / n);
            v.pend_c += std::max(0.0, h.commit - u->second.second / n);
        } else {
            v.pend_p += h.phys;
            v.pend_c += h.commit;
        }
    }
    return v;
}
// how many workers the numbers allow now (>= 1 = admit); reason says why not
static long long decide(const Ctx& c, const Want& w, const Mem& m, const View& v, string& reason) {
    double bp = m.phys - v.pend_p - c.cfg.floor_phys, bc = m.commit - v.pend_c - c.cfg.floor_commit;
    long long nbp = w.phys > 0 ? (long long)std::floor(bp / w.phys + 1e-9) : w.want;
    long long nbc = w.commit > 0 ? (long long)std::floor(bc / w.commit + 1e-9) : w.want;
    long long budget = std::min<long long>({(long long)w.want, nbp, nbc});
    // S106 0C: a lone job is admitted at once only if its minimum fits the PC's free memory (budget >= min already says so) or can never fit it (nobody
    // can wait for that: the senior_cap rule's test); otherwise it waits like anyone else (the old rule admitted it into a PC with memory short)
    bool never_fits = w.phys * w.min + c.cfg.floor_phys > m.total_phys;
    long long target = v.hs.empty() && never_fits ? std::max<long long>(w.min, budget) : budget;
    if (target < w.min) { reason = nbc < w.min ? "commit" : "memory"; return 0; }
    return target;
}
// The most workers this asker may take without cutting into a senior that waits on memory or commit (see "waiters" above): its take must
// leave free - pending >= the senior's need + floor in each dimension (CQA review 4, item 3: the check used to be made at the minimum while the
// grant was the whole budget). LLONG_MAX = no such senior; below the asker's minimum = give way to `out`.
static long long senior_cap(const Ctx& c, const Want& w, const Mem& m, const View& v, Waiter& out) {
    vector<Waiter> ws = scan_waiters(c);
    unsigned long me = GetCurrentProcessId();
    double my_wait = w.t0_tick && now_tick() > w.t0_tick ? (double)(now_tick() - w.t0_tick) / 1000.0 : 0;
    bool me_senior = w.runlock || my_wait > c.cfg.starve_s;
    long long cap = LLONG_MAX;
    for (auto& x : ws) {
        if (x.pid == me || !(x.runlock || x.waited_s > c.cfg.starve_s)) continue;
        // refused on slots, the lock, or itself giving way: not a reason to wait (the no_tiebreak plant restores the round-2 rule: any senior)
        if (x.reason != "memory" && x.reason != "commit" && !(c.plant_no_tiebreak && x.reason == "senior")) continue;
        if (x.phys + c.cfg.floor_phys > m.total_phys) continue;       // can never fit this PC: nobody waits for it
        if (me_senior && senior_before(c, w.runlock, w.t0_tick, me, x.runlock, x.start_tick, x.pid)) continue;   // I go first
        long long cx = LLONG_MAX;
        if (w.phys > 0) cx = std::min(cx, (long long)std::floor((m.phys - v.pend_p - x.phys - c.cfg.floor_phys) / w.phys + 1e-9));
        if (w.commit > 0) cx = std::min(cx, (long long)std::floor((m.commit - v.pend_c - x.commit - c.cfg.floor_commit) / w.commit + 1e-9));
        if (c.plant_no_tiebreak && x.reason == "senior") cx = -1;
        if (cx < cap || (cx == cap && cap < w.min && senior_before(c, x.runlock, x.start_tick, x.pid, out.runlock, out.start_tick, out.pid))) {
            cap = cx;
            out = x;
        }
    }
    return cap;
}
// The decision enforce would make now, without taking anything: workers (0 = wait), with s.reason / s.why when 0. Shared by enforce (attempt)
// and observe (observe_take), so the logged would_grant is exactly what enforce would have done at that instant.
static long long evaluate(const Ctx& c, const Want& w, const Mem& m, const View& v, Snap& s) {
    Waiter sx;
    long long cap = senior_cap(c, w, m, v, sx);
    if (cap < w.min) {
        s.reason = "senior";
        s.why = fmt("senior waiter pid %lu (%s, waited %.0f s, refused on %s%s) goes first", sx.pid, sx.kind.c_str(), sx.waited_s, sx.reason.c_str(),
                    sx.runlock ? ", holds the run lock" : "");
        return 0;
    }
    long long target = decide(c, w, m, v, s.reason);
    return target < 1 ? 0 : std::min(target, cap);
}
static void write_slots(Ctx& c, const Want& w, Taken& t, unsigned long long gen) {
    unsigned long long tick = now_tick();
    unsigned long pp = parent_pid();
    string rec = fmt("{\"pid\":%lu,\"kind\":\"%s\",\"phys_gb\":%.3f,\"commit_gb\":%.3f,\"start_tick\":%llu,\"start_ft\":%llu,\"start\":\"%s\",\"mode\":\"%s\",\"prog\":\"%s\","
                     "\"parent\":%lu,\"parent_ct\":\"%llu\",\"for_pid\":%lu,\"for_ct\":\"%llu\",\"workers\":%d}",   // the creation time as a string: > 2^53
                     GetCurrentProcessId(), json_esc(w.kind).c_str(), w.phys, w.commit, tick, now_ft(), local_stamp().c_str(), w.mode.c_str(),
                     json_esc(w.prog).c_str(), pp, pid_created(pp), w.for_pid, pid_created(w.for_pid), (int)t.slots.size());
    for (auto& sl : t.slots) write_record(sl.h, rec);
    write_file(join(c.state, L"generation"), fmt("%llu", gen + 1));
}
static void take_slots(const Ctx& c, const Want& w, Taken& t, long long n, int limit) {
    for (int i = 0; i < limit && (long long)t.slots.size() < n; i++) {
        wstring p = join(c.state, w16(w.kind) + L"." + wnum((unsigned)i));
        HANDLE h = slot_open(p, true);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (try_lock0(h)) t.slots.push_back({h, p});
        else CloseHandle(h);
    }
}
// S106 C5: the "alone" pause. A run that must be alone (tools\runlock.ps1 -Alone) holds byte 0 of <main checkout>\out\harness\runlock\alone.lock and writes
// {pid, token, name, since, until} from byte 1 (until = epoch seconds). While the lock is held admit grants NOTHING (reason "alone"), the forced path
// included; the holder's own process tree (WWE2K14_RUNLOCK_TOKEN == the record's token) passes, and a record whose `until` has passed is ignored (a hung
// holder cannot hold compiles past its own deadline). Liveness is the OS lock (freed when the holder dies), never the record. Tests: env WWE_ALONE_FILE
// (honoured only in test mode, state folder under %TEMP% with WWE_ADMIT_TEST=1).
static wstring alone_file_path(const Ctx& c) {
    if (c.test) {
        wstring e = getenv_w(L"WWE_ALONE_FILE");
        if (!e.empty()) return full(e);
    }
    wstring root = main_checkout_of(dir_of(g_self));
    if (root.empty()) return {};
    return join(join(join(join(root, L"out"), L"harness"), L"runlock"), L"alone.lock");
}
// S106 C5F note 1: the probe takes a SHARED lock on byte 0 (the holder's exclusive lock refuses it; two probers never collide with each other), and a lock
// is a pause only with a parseable record: a token, a future `until`, a live pid (and pid_start, when the record has one). A locked file with an empty or
// unparseable record, or a record whose process is gone, is NOT a pause (a clean exit truncates the record; a hung writer must not hold compiles forever).
static bool probe_shared0(HANDLE h) {   // true = no exclusive holder (we could take byte 0 shared, and let it go)
    OVERLAPPED ov{};
    if (!LockFileEx(h, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) return false;
    OVERLAPPED ov2{};
    UnlockFileEx(h, 0, 1, 0, &ov2);
    return true;
}
static const long long FILETIME_TO_DOTNET_TICKS = 504911232000000000LL;   // 0001-01-01 -> 1601-01-01 in 100 ns (the record's pid_start is DateTime.Ticks, UTC)
static bool alone_record_live(unsigned long pid, double pid_start) {
    if (!pid) return false;
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!ph) return false;
    bool alive = WaitForSingleObject(ph, 0) == WAIT_TIMEOUT;
    if (alive && pid_start > 0) {
        unsigned long long ct = proc_created(ph);
        double want = pid_start - (double)FILETIME_TO_DOTNET_TICKS;
        if (ct && std::fabs((double)ct - want) > 1e7) alive = false;   // the pid was reused (within 1 s; a double holds the ticks to ~100 ticks)
    }
    CloseHandle(ph);
    return alive;
}
static bool alone_paused(const Ctx& c, string& why) {
    wstring f = alone_file_path(c);
    if (f.empty()) return false;
    HANDLE h = slot_open(f, false);
    if (h == INVALID_HANDLE_VALUE) return false;   // no file, or one opened without sharing: no pause we can read
    bool paused = false;
    string rec;
    if (!probe_shared0(h)) { paused = true; rec = read_record(h); }
    CloseHandle(h);
    if (!paused) return false;
    JV j;
    string e;
    if (rec.empty() || !json_parse(rec, j, e) || j.t != JV::OBJ) return false;   // locked, no readable record: not a pause
    double until = j.num("until", 0);
    unsigned long pid = (unsigned long)j.num("pid", 0);
    string name = j.str("name", "?");
    string token = j.str("token", "");
    if (name.size() > 200) name.resize(200);
    double now = (double)now_ft() / 1e7 - 11644473600.0;
    if (token.empty() || until <= 0) return false;   // not a record this tool writes
    if (until <= now) return false;   // expired: the holder outlived its own deadline
    if (!alone_record_live(pid, j.num("pid_start", 0))) return false;   // the holder's process is gone (or the pid is another process now)
    string mine = u8(getenv_w(L"WWE2K14_RUNLOCK_TOKEN")), mine2 = u8(getenv_w(L"WWE_ALONE_TOKEN"));
    if (token == mine || (!mine2.empty() && token == mine2)) return false;   // the alone run's own tree (S106 C5F note 3: WWE_ALONE_TOKEN = the dedicated token of the alone run)
    string when = "an unknown time";
    {
        FILETIME ft;
        unsigned long long t100 = (unsigned long long)((until + 11644473600.0) * 1e7);
        ft.dwLowDateTime = (DWORD)(t100 & 0xFFFFFFFFULL);
        ft.dwHighDateTime = (DWORD)(t100 >> 32);
        FILETIME lft;
        SYSTEMTIME sys;
        if (FileTimeToLocalFileTime(&ft, &lft) && FileTimeToSystemTime(&lft, &sys)) when = fmt("%02d:%02d:%02d", sys.wHour, sys.wMinute, sys.wSecond);
    }
    why = fmt("held off by an alone run '%s' pid %lu until %s: nothing is granted while it runs", name.c_str(), pid, when.c_str());
    return true;
}
// 1 = admitted (t holds the slots), 0 = wait (s.reason / s.why say why), -1 = internal error (err). S109 GATE: there is no forced take; a grant
// always fits above the floor by the rule (evaluate).
static int attempt(Ctx& c, const Want& w, Taken& t, Snap& s, string& err) {
    s = Snap{};
    {   // S106 C5: an alone run's pause grants nothing
        string aw;
        if (!c.plant_no_outer_alone && alone_paused(c, aw)) { s.reason = "alone"; s.why = aw; return 0; }
    }
    Mem m;
    if (!read_mem(c, m, err)) return -1;   // memory, holders and waiters are read outside the lock
    s.mem = m;
    unsigned long long g0 = read_gen(c);
    View v = view_of(c);
    s.holders = (int)v.hs.size();
    s.pend_p = v.pend_p;
    s.pend_c = v.pend_c;
    s.use_p = v.use_p;
    s.hs = v.hs;
    long long target = evaluate(c, w, m, v, s);
    if (target < 1) return 0;
    GLock g = glock_take(c, GLOCK_WAIT_MS, err);
    if (g == GL_ERROR) return -1;
    if (g == GL_BUSY) { s.reason = "lock"; return 0; }
    {   // S106 C5F note 4: the pause is looked at again INSIDE the machine lock: an alone run that took its pause after the look above still keeps this grant out
        string aw;
        if (!c.plant_no_inner_alone && alone_paused(c, aw)) { glock_release(c); s.reason = "alone"; s.why = aw; return 0; }
    }
    unsigned long long g1 = read_gen(c);
    if (g1 != g0) {   // a take since our scan: check again on the current holders
        v = view_of(c);
        s.holders = (int)v.hs.size();
        s.pend_p = v.pend_p;
        s.pend_c = v.pend_c;
        s.use_p = v.use_p;
        s.hs = v.hs;
        target = evaluate(c, w, m, v, s);
        if (target < 1) { glock_release(c); return 0; }
    }
    int slots = std::max(1, c.cfg.kinds.at(w.kind).slots);
    take_slots(c, w, t, target, slots);
    if ((int)t.slots.size() < w.min) {   // fewer free slots than the minimum: take none
        t.release();
        s.reason = "slots";
        s.why = w.min > 1 ? fmt("fewer than %d of the %d %s slots free", w.min, slots, w.kind.c_str()) : fmt("all %d %s slots held", slots, w.kind.c_str());
    } else {
        write_slots(c, w, t, g1);
    }
    glock_release(c);
    return t.slots.empty() ? 0 : 1;
}
// Observe mode (CQA review 4, item 1): the decision enforce would make, read ONCE (same reads, same rule; free slots counted without taking
// them), then admitted at once with the FULL want: no waiting, no ticket, no timeout, no minimum. The slots are still taken (spare numbers past
// max_slots when needed) and released as usual, so later askers see the real concurrency. would = enforce's worker count (0 = it would wait).
static void observe_take(Ctx& c, const Want& w, Taken& t, Snap& s, long long& would) {
    s = Snap{};
    string err;
    Mem m;
    if (read_mem(c, m, err)) s.mem = m;
    View v = view_of(c);
    s.holders = (int)v.hs.size();
    s.pend_p = v.pend_p;
    s.pend_c = v.pend_c;
    s.use_p = v.use_p;
    int slots = std::max(1, c.cfg.kinds.at(w.kind).slots);
    would = s.mem.phys < 0 ? 0 : evaluate(c, w, m, v, s);
    if (s.mem.phys < 0) s.reason = "error";
    if (would >= 1) {
        int free = 0;
        for (int i = 0; i < slots; i++) {
            wstring p = join(c.state, w16(w.kind) + L"." + wnum((unsigned)i));
            if (!is_file(p)) { free++; continue; }
            HANDLE h = slot_open(p, false);
            if (h == INVALID_HANDLE_VALUE) continue;
            if (try_lock0(h)) { unlock0(h); free++; }
            CloseHandle(h);
        }
        would = std::min<long long>(would, free);
        if (would < w.min) { would = 0; s.reason = "slots"; }
    }
    if (would >= 1) s.reason = "ok";
    GLock g = glock_take(c, GLOCK_WAIT_MS, err);   // serialises the take with enforce askers; a busy lock does not hold an observed job back
    take_slots(c, w, t, w.want, slots + 64);
    if (!t.slots.empty()) write_slots(c, w, t, read_gen(c));
    if (g == GL_TAKEN) glock_release(c);
}
static string wait_line(const Ctx& c, const Want& w, const Snap& s) {
    if (s.reason == "lock")
        return fmt("[admit] waiting: %s: the machine lock %s\\admit.lock is busy (another admit holds it)", w.kind.c_str(), u8(c.state).c_str());
    if (s.reason == "alone") return fmt("[admit] waiting: %s: %s", w.kind.c_str(), s.why.c_str());   // S106 C5
    string extra = s.why.empty() ? "" : "; " + s.why;
    return fmt("[admit] waiting: %s needs %.1f GB + floor %.1f (commit %.1f + %.1f); available %.1f GB, commit left %.1f GB, holders %d (pending %.1f GB, commit %.1f GB)%s",
               w.kind.c_str(), w.phys, c.cfg.floor_phys, w.commit, c.cfg.floor_commit, s.mem.phys, s.mem.commit, s.holders, s.pend_p, s.pend_c, extra.c_str());
}
// S109 GATE: the largest committers on the PC (PrivateUsage), for a refusal only: "name pid G GB" x n, the queue's own holders marked. Test mode reads
// <state>\fake_top.txt lines "<name> <pid> <commit_gb>" instead (the selftest never depends on what really runs).
static string top_committers(const Ctx& c, const vector<Holder>& hs, size_t n) {
    struct PU { string name; unsigned long pid; double gb; };
    vector<PU> all;
    const double GB = 1024.0 * 1024.0 * 1024.0;
    if (c.test) {
        string f;
        if (read_file(join(c.state, L"fake_top.txt"), f)) {
            size_t p = 0;
            while (p < f.size()) {
                size_t e = f.find('\n', p);
                if (e == string::npos) e = f.size();
                char nm[128] = {0};
                unsigned long pid = 0;
                double gb = 0;
                if (sscanf(f.substr(p, e - p).c_str(), "%127s %lu %lf", nm, &pid, &gb) == 3) all.push_back({nm, pid, gb});
                p = e + 1;
            }
        }
    } else {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof pe;
            if (Process32FirstW(snap, &pe)) do {
                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (!hp) continue;
                PROCESS_MEMORY_COUNTERS_EX pm{};
                pm.cb = sizeof pm;
                if (GetProcessMemoryInfo(hp, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) all.push_back({u8(pe.szExeFile), pe.th32ProcessID, (double)pm.PrivateUsage / GB});
                CloseHandle(hp);
            } while (Process32NextW(snap, &pe));
            CloseHandle(snap);
        }
    }
    std::sort(all.begin(), all.end(), [](const PU& a, const PU& b) { return a.gb > b.gb; });
    string out;
    for (size_t i = 0; i < all.size() && i < n; i++) {
        bool queued = false;
        for (auto& h : hs) queued = queued || h.pid == all[i].pid || h.for_pid == all[i].pid || h.parent == all[i].pid;
        out += fmt("%s%s pid %lu %.1f GB%s", i ? ", " : "", all[i].name.c_str(), all[i].pid, all[i].gb, queued ? " (a queue holder)" : "");
    }
    return out.empty() ? "unreadable" : out;
}
// S109 GATE: the one loud line of a refusal: the need, the memory, every queue holder with its age, pending, the largest committers, the reason.
static string refusal_text(const Ctx& c, const Want& w, const Snap& s, double waited) {
    string holders;
    for (size_t i = 0; i < s.hs.size(); i++) {
        const Holder& h = s.hs[i];
        holders += fmt("%s%s.%d pid %lu '%s' for %.0f s", i ? ", " : "", h.kind.c_str(), h.index, h.pid, h.label.c_str(), h.age_s);
    }
    string why = s.reason == "alone" || s.reason == "lock" ? wait_line(c, w, s).substr(17) : s.reason + (s.why.empty() ? "" : ": " + s.why);
    return fmt("REFUSED after %.0f s: %s needs %d x %.1f GB + floor %.1f (commit %d x %.1f + %.1f); available %.1f GB, commit left %.1f GB; queue holders %d%s%s "
               "(pending %.1f GB, commit %.1f GB); largest commit on the PC: %s; reason %s. Nothing ran: report it, do not retry in a loop",
               waited, w.kind.c_str(), w.min, w.phys, c.cfg.floor_phys, w.min, w.commit, c.cfg.floor_commit, s.mem.phys, s.mem.commit, (int)s.hs.size(),
               holders.empty() ? "" : ": ", holders.c_str(), s.pend_p, s.pend_c, top_committers(c, s.hs, 3).c_str(), why.c_str());
}

enum Outcome { ADMITTED, NARROWED, REFUSED, ERROR_OPEN, NESTED, NESTED_HOLD, OFF, ORPHANED, OBSERVED };   // S109 GATE: narrowed / refused (history rows `waiting` too)
static const char* outcome_name(Outcome o) {
    switch (o) {
        case ADMITTED: return "admitted";
        case NARROWED: return "narrowed";
        case REFUSED: return "refused";
        case ERROR_OPEN: return "error_open";
        case NESTED: return "nested";
        case NESTED_HOLD: return "nested_hold";
        case ORPHANED: return "orphaned";
        case OBSERVED: return "observed";
        default: return "off";
    }
}
struct Admission {
    Outcome out = ADMITTED;
    Taken t;
    Snap s;
    double waited = 0;
    string refusal;   // S109 GATE: the refusal line (without "[admit] ") when out == REFUSED
};
// S109 GATE: an ask waits at most min(timeout_s, admit.json timeout_s), for any reason (an alone run's pause included). on_wait is called ONCE, at 2 s
// of waiting (the caller writes the `waiting` history row). At the bound: one last ask at a minimum of 1 worker (NARROWED when below the asker's
// minimum), else REFUSED with the loud line (a.refusal). Nothing is forced, and the bound is never extended.
static void admit_wait(Ctx& c, Want& w, double timeout_s, Admission& a, const std::function<void(const Admission&)>& on_wait) {
    double t0 = qpc_s(), last = 0;
    w.t0_tick = now_tick();
    if (c.test && !getenv_w(L"WWE_ADMIT_TEST_TICK0").empty()) w.t0_tick = wcstoull(getenv_w(L"WWE_ADMIT_TEST_TICK0").c_str(), nullptr, 10);   // selftest: a shared tick
    w.runlock = !getenv_w(L"WWE2K14_RUNLOCK_TOKEN").empty();
    unsigned seed = (unsigned)(GetCurrentProcessId() * 2654435761u) ^ (unsigned)GetTickCount();
    auto rnd = [&seed] { seed = seed * 1103515245u + 12345u; return (double)((seed >> 8) & 0xFFFF) / 65535.0; };
    double limit = std::max(0.0, std::min(timeout_s, c.cfg.timeout_s));   // the config caps every caller: no caller can buy a long silent wait
    bool said = false, said_big = false, owner_tried = false, owner_gone = false;
    HANDLE ticket = INVALID_HANDLE_VALUE, owner = nullptr;
    unsigned long owner_pid = 0;
    int tries = 0;
    for (;;) {
        string err;
        int r = attempt(c, w, a.t, a.s, err);
        double el = qpc_s() - t0;
        a.waited = el;
        if (r < 0) {
            say_err("[admit] ERROR " + err + ": running without admission");
            a.out = ERROR_OPEN;
            break;
        }
        if (r == 1) { a.out = ADMITTED; break; }
        // An orphan (the caller is gone: a cancelled build) leaves without running (CQA review 3, item 7). The handle is opened on the first
        // refusal, so an admitted-at-once job never pays for it; a pid reused after the caller died has a later creation time.
        if (!owner_tried) {
            owner_tried = true;
            owner_pid = w.watch_pid ? w.watch_pid : parent_pid();
            owner = owner_pid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner_pid) : nullptr;
            if (!owner && owner_pid && GetLastError() == ERROR_INVALID_PARAMETER) owner_gone = true;   // no such process (access denied: cannot watch)
            if (owner && proc_created(owner) > proc_created(GetCurrentProcess())) owner_gone = true;
        }
        if (owner_gone || (owner && WaitForSingleObject(owner, 0) == WAIT_OBJECT_0)) {
            say_err(fmt("[admit] ORPHANED: the caller (pid %lu) is gone: not running (exit %d)", owner_pid, EXIT_REFUSED));
            a.out = ORPHANED;
            break;
        }
        if (ticket == INVALID_HANDLE_VALUE) ticket = ticket_open(c);
        if (ticket != INVALID_HANDLE_VALUE) ticket_write(ticket, w.kind, w.phys * w.min, w.commit * w.min, w.t0_tick, w.runlock, a.s.reason);
        if (!said_big && a.s.mem.total_phys > 0 && w.phys * w.min + c.cfg.floor_phys > a.s.mem.total_phys) {   // (S106 C5: an alone pause refuses before memory is read: total -1 = unknown, no note)
            said_big = true;
            say_err(fmt("[admit] note: %s needs %.1f GB + floor %.1f, more than this PC's %.1f GB: nobody will wait for it", w.kind.c_str(), w.phys * w.min,
                        c.cfg.floor_phys, a.s.mem.total_phys));
        }
        if (el >= 2.0 && !said) {
            say_err(wait_line(c, w, a.s) + fmt("; refused after %.0f s unless admitted", limit));
            said = true;
            last = el;
            if (on_wait) on_wait(a);   // S109 GATE: the `waiting` row, so a waiter its caller kills is still on record
        } else if (said && el - last >= 30.0) { say_err(wait_line(c, w, a.s)); last = el; }
        if (el >= limit) {
            // S109 GATE: one last ask at a minimum of 1 worker (the full rule: floor, seniors, slots); a busy machine lock is tried again for up to 5 s
            Want w1 = w;
            w1.min = 1;
            double until = qpc_s() + 5.0;
            Snap s2;
            int r2 = 0;
            for (;;) {
                string e2;
                r2 = attempt(c, w1, a.t, s2, e2);
                if (r2 != 0 || s2.reason != "lock" || qpc_s() > until) break;
                Sleep(100);
            }
            a.waited = qpc_s() - t0;
            if (r2 < 0) {
                say_err("[admit] ERROR at the bound: running without admission");
                a.out = ERROR_OPEN;
                break;
            }
            if (r2 == 1) {
                a.s = s2;
                a.out = (int)a.t.slots.size() < w.min ? NARROWED : ADMITTED;
                break;
            }
            a.s = s2;
            a.refusal = refusal_text(c, w, s2, a.waited);
            say_err("[admit] " + a.refusal);
            a.out = REFUSED;
            break;
        }
        // poll: 250 ms, backing off to 1 s, +-20 % jitter so many waiters do not poll in step
        tries++;
        double base = std::min(1000.0, 250.0 * (1.0 + 0.25 * std::min(tries, 12)));
        Sleep((DWORD)(base * (0.8 + 0.4 * rnd())));
    }
    if (ticket != INVALID_HANDLE_VALUE) CloseHandle(ticket);
    if (owner) CloseHandle(owner);
}

// ---------------------------------------------------------------- the nested marker and the ancestry check
// WWE_ADMIT_HELD = "<admit pid> <its creation time> <kind>": it counts only while that admit (run) or hold process is alive (pid + creation
// time match). A long-lived server started inside an admitted job inherits it: while that admit lives the server's compiles count as nested,
// so the wiring clears the marker where the sccache server starts; once the admit has exited the marker is dead and they are admitted again.
static wstring held_marker(const string& kind) {
    return w16(fmt("%lu %llu %s", GetCurrentProcessId(), proc_created(GetCurrentProcess()), kind.c_str()));
}
// True when the marker's process is alive AND its kind covers `kind` in this config (CQA review 3, blocker 4: a child of a cc.tool run
// that asks for link.game_exe is not inside it).
static bool held_covers(const Config& cfg, const string& kind, unsigned long* holder = nullptr) {
    wstring v = getenv_w(L"WWE_ADMIT_HELD");
    if (v.empty()) return false;
    unsigned long pid = 0;
    unsigned long long ct = 0;
    wchar_t mk[64] = {0};
    if (swscanf(v.c_str(), L"%lu %llu %63ls", &pid, &ct, mk) != 3 || !pid) return false;
    if (!kind_covers(cfg, u8(mk), kind)) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool live = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE && proc_created(h) == ct;
    CloseHandle(h);
    if (live && holder) *holder = pid;
    return live;
}
// How many workers the process `pid` holds (its slot records' "workers"); 1 when it holds none (a nested or fail-open admission).
static int holder_workers(const Ctx& c, unsigned long pid) {
    int n = 0;
    for (auto& h : scan(c))
        if (h.pid == pid) n = std::max(n, h.workers);
    return std::max(1, n);
}
// Automatic route (CQA review B2b): this process descends from the --pid P of a live hold that covers its kind. ONLY --pid holds nest by
// ancestry (CQA addendum A1): a stdin-form hold's parent may take several holds for several children, so its descendants are not "inside" any
// one of them; a child is placed under a stdin-form hold only by its marker line. Walks the parent chain with a creation-time check (a reused
// pid ends the walk, and P's recorded creation time must match). Only runs when a live --pid hold exists, so a plain compile pays one scan.
// workers: the covering hold's worker count (a nested hold is granted at most that: CQA addendum A2). A second hold for the same P and kind is
// caught before this by the same-owner rule (CQA review 4, item 5).
static bool nested_in_hold(const Ctx& c, const string& kind, string& which, int& workers) {
    vector<Holder> hs = scan(c);
    vector<const Holder*> holds;
    for (auto& h : hs)
        if (h.mode.rfind("hold", 0) == 0 && h.for_pid && kind_covers(c.cfg, h.kind, kind)) holds.push_back(&h);
    if (holds.empty()) return false;
    std::map<unsigned long, unsigned long> parent_of;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do parent_of[pe.th32ProcessID] = pe.th32ParentProcessID;
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    unsigned long cur = GetCurrentProcessId();
    unsigned long long cur_ct = proc_created(GetCurrentProcess());
    for (int depth = 0; depth < 64; depth++) {
        auto it = parent_of.find(cur);
        if (it == parent_of.end() || it->second == 0 || it->second == cur) break;
        unsigned long pp = it->second;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pp);
        if (!h) break;
        unsigned long long ct = proc_created(h);
        CloseHandle(h);
        if (!ct || ct > cur_ct) break;   // started after its "child": the pid was reused, not our ancestor
        for (auto* hd : holds)   // the owner match also needs the creation time the hold recorded: a dead owner's reused pid is not it
            if (pp == hd->for_pid && (!hd->for_ct || hd->for_ct == ct)) {
                which = fmt("%s hold of pid %lu for pid %lu", hd->kind.c_str(), hd->pid, hd->for_pid);
                workers = hd->workers;
                return true;
            }
        cur = pp;
        cur_ct = ct;
    }
    return false;
}

// ---------------------------------------------------------------- history
struct HistRow {
    string kind, outcome, mode, prog, scope = "none", ended = "-";
    string qmode = "-", would_reason = "-";   // the queue's mode (observe / enforce) and, in observe mode, what enforce would have done
    long long would_grant = -1;
    double waited = 0, rp = -1, rc = -1, avail = -1, commit = -1, held = -1, peak_mb = -1;
    double pending = -1, held_use = -1;   // S106 0C: the last decision's pending and measured held use (GB), for the next replay
    int holders = -1, workers = 1;
    bool has_exit = false;
    unsigned long exit_code = 0;
};
static string numf(double v, const char* f = "%.2f") { return v < 0 ? "-" : fmt(f, v); }
static const char* HISTORY_HEADER =
    "time\tkind\tpid\twaited_s\treserve_phys_gb\treserve_commit_gb\tavail_phys_gb\tcommit_left_gb\tholders\toutcome\texit_code\theld_s\tpeak_job_mb\tpeak_scope\tmode\tworkers\tended\t"
    "queue_mode\twould_grant\twould_reason\tprog\tpending_gb\theld_use_gb\n";   // S106 0C: the last two (appended: older readers keep their columns)
static bool history_header_current(const wstring& p) {   // the first line, read alone (the file may be 16 MB)
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return true;
    char buf[512];
    DWORD n = 0;
    ReadFile(h, buf, sizeof buf, &n, nullptr);
    CloseHandle(h);
    size_t len = strlen(HISTORY_HEADER);
    return n == 0 || (n >= len && memcmp(buf, HISTORY_HEADER, len) == 0);
}
static void history(const Ctx& c, const HistRow& r) {
    if (c.state.empty()) return;
    HANDLE h = CreateFileW(join(c.state, L"history.tsv").c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    // rotate past the size cap (keep one old file; a racing writer may lose a row, never the file), or when the columns changed
    if (sz.QuadPart > HISTORY_CAP || (sz.QuadPart > 0 && !history_header_current(join(c.state, L"history.tsv")))) {
        CloseHandle(h);
        MoveFileExW(join(c.state, L"history.tsv").c_str(), join(c.state, L"history.old.tsv").c_str(), MOVEFILE_REPLACE_EXISTING);
        h = CreateFileW(join(c.state, L"history.tsv").c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        GetFileSizeEx(h, &sz);
    }
    string prog = r.prog;
    for (auto& ch : prog)
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    string line;
    if (sz.QuadPart == 0) line = HISTORY_HEADER;
    line += fmt("%s\t%s\t%lu\t%.3f\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", local_stamp().c_str(), r.kind.c_str(), GetCurrentProcessId(),
                r.waited, numf(r.rp).c_str(), numf(r.rc).c_str(), numf(r.avail).c_str(), numf(r.commit).c_str(), r.holders < 0 ? "-" : fmt("%d", r.holders).c_str(),
                r.outcome.c_str(), r.has_exit ? fmt("%lu", r.exit_code).c_str() : "-", numf(r.held, "%.3f").c_str(),
                r.scope == "job" || r.scope == "partial" ? numf(r.peak_mb, "%.1f").c_str() : "-", r.scope.c_str(), r.mode.c_str(), r.workers, r.ended.c_str(),
                r.qmode.c_str(), r.would_grant < 0 ? "-" : fmt("%lld", r.would_grant).c_str(), r.would_reason.c_str(), prog.c_str(), numf(r.pending).c_str(), numf(r.held_use).c_str());
    DWORD n = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &n, nullptr);
    CloseHandle(h);
}
// S109 GATE: the row a wait writes at 2 s (outcome `waiting`, ended `waiting:<reason>`, workers 0): a waiter killed later is still on record; the
// row at the wait's end (admitted / narrowed / refused / orphaned) follows it with the same pid.
static HistRow waiting_row(const HistRow& base, const Admission& a) {
    HistRow r = base;
    r.outcome = "waiting";
    r.ended = "waiting:" + (a.s.reason.empty() ? string("?") : a.s.reason);
    r.waited = a.waited;
    r.avail = a.s.mem.phys;
    r.commit = a.s.mem.commit;
    r.holders = a.s.holders;
    r.pending = a.s.pend_p;
    r.held_use = a.s.use_p;
    r.workers = 0;
    return r;
}

// ---------------------------------------------------------------- the command line (verbatim tail)
struct Tok {
    wstring s;
    size_t b = 0, e = 0;
};
static vector<Tok> tokenize(const wstring& c) {   // the CRT's rules (argv[0]: quotes only; then backslash / quote rules), with each token's span
    vector<Tok> v;
    size_t i = 0, n = c.size();
    {
        Tok t;
        if (i < n && c[i] == L'"') {
            i++;
            while (i < n && c[i] != L'"') t.s += c[i++];
            if (i < n) i++;
        } else {
            while (i < n && c[i] != L' ' && c[i] != L'\t') t.s += c[i++];
        }
        t.e = i;
        v.push_back(t);
    }
    for (;;) {
        while (i < n && (c[i] == L' ' || c[i] == L'\t')) i++;
        if (i >= n) break;
        Tok t;
        t.b = i;
        bool inq = false;
        while (i < n) {
            wchar_t ch = c[i];
            if (!inq && (ch == L' ' || ch == L'\t')) break;
            if (ch == L'\\') {
                size_t k = i;
                while (k < n && c[k] == L'\\') k++;
                size_t nb = k - i;
                if (k < n && c[k] == L'"') {
                    t.s.append(nb / 2, L'\\');
                    if (nb % 2) { t.s += L'"'; i = k + 1; }
                    else i = k;
                } else {
                    t.s.append(nb, L'\\');
                    i = k;
                }
                continue;
            }
            if (ch == L'"') {
                if (inq && i + 1 < n && c[i + 1] == L'"') { t.s += L'"'; i += 2; continue; }
                inq = !inq;
                i++;
                continue;
            }
            t.s += ch;
            i++;
        }
        t.e = i;
        v.push_back(t);
    }
    return v;
}
struct Opts {
    std::map<wstring, wstring> kv;
    bool has_tail = false;
    wstring tail;
    string err;
};
static Opts parse_opts(size_t first, const vector<wstring>& valued, const vector<wstring>& flags = {}) {
    Opts o;
    wstring cl = GetCommandLineW();
    vector<Tok> t = tokenize(cl);
    for (size_t i = first; i < t.size(); i++) {
        if (cl.compare(t[i].b, t[i].e - t[i].b, L"--") == 0) {
            size_t p = t[i].e;
            while (p < cl.size() && (cl[p] == L' ' || cl[p] == L'\t')) p++;
            o.tail = cl.substr(p);
            o.has_tail = !o.tail.empty();
            break;
        }
        const wstring& a = t[i].s;
        if (std::find(valued.begin(), valued.end(), a) != valued.end()) {
            if (i + 1 >= t.size()) { o.err = "option " + u8(a) + " needs a value"; continue; }
            o.kv[a] = t[++i].s;
        } else if (std::find(flags.begin(), flags.end(), a) != flags.end()) {
            o.kv[a] = L"1";
        } else if (o.err.empty()) {
            o.err = "unknown option " + u8(a);
        }
    }
    return o;
}
static double opt_num(const Opts& o, const wchar_t* k, double d, string& err) {
    auto it = o.kv.find(k);
    if (it == o.kv.end()) return d;
    wchar_t* e = nullptr;
    double v = wcstod(it->second.c_str(), &e);
    if (e == it->second.c_str() || *e || !std::isfinite(v) || v < 0) { err = "bad value '" + u8(it->second) + "' for " + u8(k); return d; }
    return v;
}
static wstring opt_str(const Opts& o, const wchar_t* k) { return o.kv.count(k) ? o.kv.at(k) : L""; }

// ---------------------------------------------------------------- starting the program
// What a direct start would run: CreateProcess(NULL, tail) for programs it can start; a .cmd / .bat FOUND by an explicit search (its own name,
// or a bare name through PATHEXT in the current folder then PATH) through `cmd /d /s /c "<tail>"` (cmd re-parses that line: % expansion, ^, &,
// |, <, >); a found .ps1 through `pwsh -NoProfile -File`. Nothing else is ever handed to cmd: any other start failure is an error line and exit
// 9009 (not found) or 127, and nothing runs (CQA review 3, blocker 3: a generic cmd retry could run a built-in or turn data into commands).
static wstring cmd_exe() {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    return n ? join(wstring(sys, n), L"cmd.exe") : L"cmd.exe";
}
static wstring find_program(const wstring& name) {
    bool has_dir = name.find_first_of(L"\\/:") != wstring::npos;
    vector<wstring> exts;
    if (ext_of(name).empty()) {
        wstring pe = getenv_w(L"PATHEXT");
        if (pe.empty()) pe = L".COM;.EXE;.BAT;.CMD";
        size_t a = 0;
        while (a <= pe.size()) {
            size_t b = pe.find(L';', a);
            if (b == wstring::npos) b = pe.size();
            if (b > a) exts.push_back(pe.substr(a, b - a));
            a = b + 1;
        }
    } else {
        exts.push_back(L"");
    }
    vector<wstring> dirs;
    if (has_dir) dirs.push_back(L"");
    else {
        wchar_t cwd[MAX_PATH];
        DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
        if (n) dirs.push_back(wstring(cwd, n));
        wstring path = getenv_w(L"PATH");
        size_t a = 0;
        while (a <= path.size()) {
            size_t b = path.find(L';', a);
            if (b == wstring::npos) b = path.size();
            if (b > a) dirs.push_back(path.substr(a, b - a));
            a = b + 1;
        }
    }
    for (auto& d : dirs)
        for (auto& e : exts) {
            wstring p = d.empty() ? name + e : join(d, name + e);
            if (is_file(p)) return full(p);
        }
    return {};
}
struct Launch {
    wstring app, cmdline, prog_stem;
};
static Launch plan_launch(const wstring& tail) {
    Launch l;
    vector<Tok> t = tokenize(tail);
    wstring name = t.empty() ? wstring() : t[0].s;
    wstring rest = t.empty() ? wstring() : tail.substr(t[0].e);
    l.cmdline = tail;
    l.prog_stem = stem_lower(name);
    wstring ext = ext_of(name);
    wstring found;
    if (ext.empty()) {   // the common case, a program by bare name: if CreateProcess's own search finds an .exe, start exactly that way
        wchar_t sp[MAX_PATH];
        if (SearchPathW(nullptr, name.c_str(), L".exe", MAX_PATH, sp, nullptr) == 0) found = find_program(name);
    } else if (ext == L".cmd" || ext == L".bat" || ext == L".ps1") {
        found = find_program(name);
    }
    wstring fext = ext_of(found);
    if (fext == L".cmd" || fext == L".bat") {
        l.app = cmd_exe();
        l.cmdline = L"cmd.exe /d /s /c \"\"" + found + L"\"" + rest + L"\"";
        l.prog_stem = L"cmd";
    } else if (fext == L".ps1") {
        l.cmdline = L"pwsh -NoProfile -File \"" + found + L"\"" + rest;
        l.prog_stem = L"pwsh";
        say_err("[admit] note: " + u8(name) + " is a PowerShell script: starting it with pwsh -NoProfile -File");
    }
    return l;
}
// false = not started; code is then 9009 (not found, as cmd reports it) or 127
static bool start_suspended(const Launch& l, DWORD flags, PROCESS_INFORMATION& pi, unsigned long& code) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    for (HANDLE h : {si.hStdInput, si.hStdOutput, si.hStdError})
        if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    vector<wchar_t> buf(l.cmdline.begin(), l.cmdline.end());
    buf.push_back(0);
    if (CreateProcessW(l.app.empty() ? nullptr : l.app.c_str(), buf.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi)) return true;
    DWORD e = GetLastError();
    code = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 9009 : 127;
    say_err(fmt("[admit] ERROR cannot start %s: %s: nothing run (exit %lu)", u8(l.cmdline.substr(0, 200)).c_str(), winerr(e).c_str(), code));
    return false;
}

// ---------------------------------------------------------------- the child
static BOOL WINAPI ctrl_handler(DWORD t) {
    // while a child runs it gets the same Ctrl+C (shared console) and decides; admit waits for it and passes its exit code on
    if ((t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) && g_child_running) return TRUE;
    return FALSE;
}
struct ChildResult {
    bool started = false;
    unsigned long code = 0;
    double peak_mb = -1;
    string scope = "none";   // job (the measurement), partial (pwsh / powershell / cmd, or processes left behind), unknown (sccache), none
};
// in_job: run the child in a kill-on-close job while admit waits on it (a killed admit takes it down) and read the job's peak commit. After a
// NORMAL exit nothing is terminated: kill-on-close is cleared before the handle closes, so whatever the child left running (the sccache server
// and the compiles it runs for other builds) lives on (CQA review B1). Residual: an admit killed while a daemon that started under it is
// serving others takes that daemon down. in_job false (WWE_NO_ADMIT, nested): the plain start, no job.
static ChildResult run_child(const wstring& tail, bool in_job) {
    ChildResult r;
    Launch l = plan_launch(tail);
    HANDLE job = nullptr;
    if (in_job) {
        job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) { CloseHandle(job); job = nullptr; }
        }
    }
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (job ? CREATE_SUSPENDED : 0) | (has_console() ? 0 : CREATE_NO_WINDOW);
    InterlockedExchange(&g_child_running, 1);
    if (!start_suspended(l, flags, pi, r.code)) {
        InterlockedExchange(&g_child_running, 0);
        if (job) CloseHandle(job);
        return r;
    }
    r.started = true;
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) { CloseHandle(job); job = nullptr; }
    if (flags & CREATE_SUSPENDED) ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    InterlockedExchange(&g_child_running, 0);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.code = code;
    CloseHandle(pi.hProcess);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li, nullptr)) r.peak_mb = (double)li.PeakJobMemoryUsed / (1024.0 * 1024.0);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc{};
        bool left = QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acc, sizeof acc, nullptr) && acc.ActiveProcesses > 0;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION z{};   // LimitFlags 0: closing the handle kills nothing
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &z, sizeof z) && left)
            say_err("[admit] ERROR could not clear kill-on-close (" + winerr(GetLastError()) + "): processes the child left running end with this admit");
        CloseHandle(job);
        wstring st = l.prog_stem;
        if (st == L"sccache") r.scope = "unknown";   // the compile runs in the sccache server, outside this job
        else if (st == L"pwsh" || st == L"powershell" || st == L"cmd" || left) r.scope = "partial";   // the Store pwsh starts children outside jobs (CQM F1)
        else r.scope = "job";
    }
    return r;
}

// ---------------------------------------------------------------- run
static int exec_tail(Ctx& c, const Want& w, Admission& a, const wstring& tail, HistRow hr, bool in_job, bool set_marker) {
    if (set_marker) SetEnvironmentVariableW(L"WWE_ADMIT_HELD", held_marker(w.kind).c_str());
    double t0 = qpc_s();
    ChildResult r = run_child(tail, in_job);
    hr.held = qpc_s() - t0;
    a.t.release();
    hr.has_exit = true;
    hr.exit_code = r.code;
    hr.peak_mb = r.peak_mb;
    hr.scope = r.scope;
    hr.ended = r.started ? "child_exit" : "not_started";
    history(c, hr);
    return (int)r.code;
}
static wstring state_if_exists() {
    wstring st = getenv_w(L"WWE_ADMIT_DIR");
    if (st.empty()) st = default_state();
    if (st.empty()) return L"";
    st = full(st);
    return is_dir(st) ? st : L"";
}
// The kind to use: the named one, else default_kind; an unknown name is a warning and default_kind (not a reason to fall open).
static void settle_kind(const Ctx& c, Want& w) {
    if (w.kind.empty()) { w.kind = c.cfg.default_kind; return; }
    if (c.cfg.kinds.count(w.kind)) return;
    say_err("[admit] warning: unknown kind '" + w.kind + "' (config " + u8(c.cfg.path) + "): using " + c.cfg.default_kind);
    w.kind = c.cfg.default_kind;
}
static int cmd_run() {
    Opts o = parse_opts(2, {L"--kind", L"--reserve-gb", L"--reserve-commit-gb", L"--timeout", L"--state", L"--config"});
    if (!o.has_tail) {
        say_err("usage: admit run --kind K [--reserve-gb X] [--reserve-commit-gb Y] [--timeout S] -- PROGRAM ARGS...");
        return 2;
    }
    string err = o.err;
    Want w;
    w.kind = u8(opt_str(o, L"--kind"));
    w.mode = "run";
    {
        vector<Tok> t = tokenize(o.tail);
        w.prog = t.empty() ? "" : u8(base_of(t[0].s));
    }
    w.phys = opt_num(o, L"--reserve-gb", -1, err);
    w.commit = opt_num(o, L"--reserve-commit-gb", o.kv.count(L"--reserve-gb") ? w.phys : -1, err);
    double timeout_opt = opt_num(o, L"--timeout", -1, err);
    HistRow hr;
    hr.kind = w.kind.empty() ? "-" : w.kind;
    hr.mode = "run";
    hr.prog = w.prog;
    Ctx c;
    Admission a;
    if (env_on(L"WWE_NO_ADMIT")) {   // silent, the plain start; a history row only if the state folder already exists
        c.state = state_if_exists();
        hr.outcome = "off";
        return exec_tail(c, w, a, o.tail, hr, false, false);
    }
    string serr;
    bool ok = setup(c, opt_str(o, L"--state"), opt_str(o, L"--config"), serr);
    if (ok && !err.empty()) { ok = false; serr = err; }   // a bad option: fail open, loudly
    err = serr;
    if (!ok) {
        say_err("[admit] ERROR " + (err.empty() ? c.state_err + c.cfg_err : err) + ": running without admission");
        hr.outcome = "error_open";
        return exec_tail(c, w, a, o.tail, hr, true, true);
    }
    settle_kind(c, w);
    hr.kind = w.kind;
    hr.qmode = c.cfg.mode;
    if (held_covers(c.cfg, w.kind)) {   // inside an admitted job or a hold of a covering kind (the marker): no second reservation, the plain start
        hr.outcome = "nested";
        return exec_tail(c, w, a, o.tail, hr, false, false);
    }
    string which;
    int cover_workers = 0;
    if (nested_in_hold(c, w.kind, which, cover_workers)) {   // a descendant of a live --pid hold that covers this kind: no second reservation
        hr.outcome = "nested_hold";
        hr.prog = w.prog + " (in " + which + ")";
        return exec_tail(c, w, a, o.tail, hr, false, true);
    }
    const Kind& k = c.cfg.kinds.at(w.kind);
    if (w.phys < 0) w.phys = k.phys;
    if (w.commit < 0) w.commit = k.commit;
    hr.rp = w.phys;
    hr.rc = w.commit;
    if (c.cfg.mode == "observe") {   // decide once, log what enforce would have done, start at once
        long long would = 0;
        observe_take(c, w, a.t, a.s, would);
        hr.avail = a.s.mem.phys;
        hr.commit = a.s.mem.commit;
        hr.holders = a.s.holders;
        hr.pending = a.s.pend_p;
        hr.held_use = a.s.use_p;
        hr.outcome = "observed";
        hr.would_grant = would;
        hr.would_reason = a.s.reason;
        return exec_tail(c, w, a, o.tail, hr, true, true);
    }
    double timeout = timeout_opt >= 0 ? timeout_opt : c.cfg.timeout_s;
    admit_wait(c, w, timeout, a, [&](const Admission& x) { history(c, waiting_row(hr, x)); });
    hr.waited = a.waited;
    hr.rp = w.phys;
    hr.rc = w.commit;
    hr.avail = a.s.mem.phys;
    hr.commit = a.s.mem.commit;
    hr.holders = a.s.holders;
    hr.pending = a.s.pend_p;
    hr.held_use = a.s.use_p;
    hr.outcome = outcome_name(a.out);
    if (a.out == REFUSED || a.out == ORPHANED) {   // nothing is started
        hr.has_exit = true;
        hr.exit_code = EXIT_REFUSED;
        hr.ended = a.out == ORPHANED ? "orphaned" : "refused:" + a.s.reason;
        history(c, hr);
        return EXIT_REFUSED;
    }
    return exec_tail(c, w, a, o.tail, hr, true, true);
}

// ---------------------------------------------------------------- spawning with an explicit handle list (hold's helper, the selftest)
struct P {
    HANDLE h = nullptr;
    DWORD pid = 0;
};
// only in / out / err are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): a helper must never hold a caller's pipe open. job: started suspended,
// put in that job, then resumed.
static P spawn_ex(const wstring& cmdline, HANDLE in, HANDLE out, HANDLE err, DWORD extra_flags, HANDLE job = nullptr) {
    P p;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul_in = nullptr, nul_out = nullptr;
    if (!in) in = nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (!out) out = nul_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (!err) err = out;
    HANDLE list[3];
    int nl = 0;
    for (HANDLE h : {in, out, err}) {
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        if (std::find(list, list + nl, h) == list + nl) list[nl++] = h;
    }
    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
    vector<char> abuf(sz);
    auto al = (LPPROC_THREAD_ATTRIBUTE_LIST)abuf.data();
    InitializeProcThreadAttributeList(al, 1, 0, &sz);
    UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, nl * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    si.lpAttributeList = al;
    vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi{};
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | extra_flags | (job ? CREATE_SUSPENDED : 0);
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi)) {
        if (job) { AssignProcessToJobObject(job, pi.hProcess); ResumeThread(pi.hThread); }
        CloseHandle(pi.hThread);
        p.h = pi.hProcess;
        p.pid = pi.dwProcessId;
    }
    DeleteProcThreadAttributeList(al);
    if (nul_in) CloseHandle(nul_in);
    if (nul_out) CloseHandle(nul_out);
    return p;
}
static wstring q(const wstring& s) { return L"\"" + s + L"\""; }
static bool read_line_blocking(HANDLE r, string& line) {   // false at EOF with nothing read
    line.clear();
    char ch;
    DWORD n = 0;
    bool any = false;
    while (ReadFile(r, &ch, 1, &n, nullptr) && n == 1) {
        any = true;
        if (ch == '\n') return true;
        if (ch != '\r') line += ch;
    }
    return any;
}

// ---------------------------------------------------------------- hold
static DWORD WINAPI stdin_thread(LPVOID ev) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char buf[256];
    DWORD n = 0;
    for (;;)
        if (!ReadFile(in, buf, sizeof buf, &n, nullptr) || n == 0) break;
    SetEvent((HANDLE)ev);
    return 0;
}
static wstring release_event_name(unsigned long holder_pid) { return L"Local\\wwe_admit_rel_" + wnum(holder_pid); }
// Waits for the release event (`admit release --pid P`, or a newer hold of the same kind for the same P), the owner's exit (P = --pid, else
// the parent), stdin's end-of-file when stdin is a pipe, and max_s. Returns why it ended.
static string hold_until_released(unsigned long for_pid, bool watch_stdin, HANDLE rel, double max_s) {
    vector<HANDLE> waits;
    vector<string> names;
    if (rel) { waits.push_back(rel); names.push_back("released"); }
    if (watch_stdin) {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE th = CreateThread(nullptr, 0, stdin_thread, ev, 0, nullptr);
        if (th) CloseHandle(th);
        waits.push_back(ev);
        names.push_back("stdin_eof");
    }
    unsigned long pp = for_pid ? for_pid : parent_pid();
    HANDLE ph = pp ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pp) : nullptr;
    if (!ph) return for_pid ? "owner_gone" : "parent_gone";
    if (proc_created(ph) > proc_created(GetCurrentProcess())) { CloseHandle(ph); return for_pid ? "owner_gone" : "parent_gone"; }   // a reused pid
    waits.push_back(ph);
    names.push_back(for_pid ? "owner_exit" : "parent_exit");
    DWORD ms = max_s > 0 ? (DWORD)std::min(max_s * 1000.0, 4.0e9) : INFINITE;
    DWORD r = WaitForMultipleObjects((DWORD)waits.size(), waits.data(), FALSE, ms);
    CloseHandle(ph);
    if (r == WAIT_TIMEOUT) return "max_s";
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + names.size()) return names[r - WAIT_OBJECT_0];
    return "wait_failed";
}
// A second hold of the same kind for the same P (a wired script started with `&` inside another wired script's process: same $PID) NESTS
// under the first (CQA review 4, item 5): granted at most its workers, nothing reserved, and the `admit release --pid P` it makes is absorbed by
// a counter <state>\nested.<P>.<kind>.cnt instead of ending the outer hold. The outer helper deletes the counter when it ends.
static wstring nest_counter(const Ctx& c, unsigned long P, const string& kind) { return join(c.state, L"nested." + wnum(P) + L"." + w16(kind) + L".cnt"); }
static int nest_add(Ctx& c, unsigned long P, const string& kind, int delta) {   // returns the count before the change
    string err;
    GLock g = glock_take(c, GLOCK_WAIT_MS, err);
    string t;
    wstring f = nest_counter(c, P, kind);
    int n = read_file(f, t) ? atoi(t.c_str()) : 0;
    int m = std::max(0, n + delta);
    if (m > 0) write_file(f, fmt("%d", m));
    else DeleteFileW(f.c_str());
    if (g == GL_TAKEN) glock_release(c);
    return n;
}
static int same_owner_workers(const Ctx& c, unsigned long P, const string& kind) {   // 0 = P holds no hold of this kind
    int n = 0;
    for (auto& h : scan(c))
        if (h.mode.rfind("hold", 0) == 0 && h.for_pid == P && h.kind == kind && h.pid != GetCurrentProcessId()) n = std::max(n, h.workers);
    return n;
}
// Ends ONE hold of process P per kind per call (records with for_pid == P, mode hold*, the given kind or any): a nested hold's release is
// absorbed first (enforce); otherwise the OLDEST holder of that kind is released (observe can hold several for one P and kind: one per call,
// oldest first; enforce holds at most one). Its own release event is signalled, waiting for it only when the event was opened.
static int release_for(Ctx& c, unsigned long P, const string& kind, int& absorbed) {
    std::map<string, std::pair<unsigned long, double>> oldest;   // kind -> (holder pid, age)
    for (auto& h : scan(c))
        if (h.mode.rfind("hold", 0) == 0 && h.for_pid == P && (kind.empty() || h.kind == kind) && h.pid && h.pid != GetCurrentProcessId()) {
            auto it = oldest.find(h.kind);
            if (it == oldest.end() || h.age_s > it->second.second) oldest[h.kind] = {h.pid, h.age_s};
        }
    int n = 0;
    absorbed = 0;
    for (auto& [k, ph_age] : oldest) {
        if (nest_add(c, P, k, -1) > 0) { absorbed++; continue; }   // a nested hold's release: the outer one stays
        unsigned long pid = ph_age.first;
        HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, release_event_name(pid).c_str());
        if (!ev) continue;
        HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
        SetEvent(ev);
        CloseHandle(ev);
        n++;
        if (ph) { WaitForSingleObject(ph, 5000); CloseHandle(ph); }   // its slots are free when it has exited
    }
    return n;
}
// `hold --detach`: start the helper (this exe, `hold ... --as-helper --pid P`), relay its lines, print its grant and marker, and return at
// once. The helper gets a private pipe (never the caller's stdout: a PowerShell capture would wait on it) and lives on, holding.
static int hold_front(const Opts& o, unsigned long owner, int want, const string& kind) {
    if (env_on(L"WWE_NO_ADMIT")) {   // the marker is judged by the helper, which loads the config (kind coverage)
        say_out(fmt("granted %d open\n", want));
        say_out("marker " + u8(held_marker(kind.empty() ? "-" : kind)) + "\n");
        return 0;
    }
    if (!owner) owner = parent_pid();
    wstring cl = GetCommandLineW();
    vector<Tok> t = tokenize(cl);
    wstring cmd = q(g_self) + L" hold" + cl.substr(t[1].e) + L" --as-helper" + (o.kv.count(L"--pid") ? L"" : L" --pid " + wnum(owner));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) {
        say_err("[admit] ERROR hold: pipe " + winerr(GetLastError()) + ": running without admission");
        say_out(fmt("granted %d open\n", want));
        return 0;
    }
    P h = spawn_ex(cmd, nullptr, w, w, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP);
    CloseHandle(w);
    if (!h.h) {
        CloseHandle(r);
        say_err("[admit] ERROR hold: cannot start the helper (" + winerr(GetLastError()) + "): running without admission");
        say_out(fmt("granted %d open\n", want));
        return 0;
    }
    string line;
    bool granted = false;
    while (read_line_blocking(r, line)) {
        if (line.rfind("granted ", 0) == 0) { say_out(line + "\n"); granted = true; continue; }
        if (line.rfind("marker ", 0) == 0) { say_out(line + "\n"); break; }
        if (line.rfind("refused ", 0) == 0) { say_out(line + "\n"); continue; }   // S109 GATE: the refusal's stdout line, for the caller
        say_err(line);
    }
    CloseHandle(r);
    if (granted) { CloseHandle(h.h); return 0; }
    DWORD code = 0;
    WaitForSingleObject(h.h, 5000);
    GetExitCodeProcess(h.h, &code);
    CloseHandle(h.h);
    if (code == (DWORD)EXIT_REFUSED) return EXIT_REFUSED;
    say_err(fmt("[admit] ERROR hold: the helper ended without a grant (exit %lu): running without admission", code));
    say_out(fmt("granted %d open\n", want));
    return 0;
}
// "granted N <word>" (CQA review 4, item 4): held = a real reservation; nested = inside a covering admission, nothing new held; observed =
// observe mode, held for the record but never narrowed; open = an error / WWE_NO_ADMIT path, nothing held.
static const char* grant_word(const string& outcome) {
    if (outcome == "admitted") return "held";
    if (outcome == "nested" || outcome == "nested_hold") return "nested";
    if (outcome == "observed") return "observed";
    if (outcome == "narrowed") return "narrowed";   // S109 GATE: granted at the bound below the asker's minimum (still above the floor)
    return "open";
}
static int cmd_hold() {
    Opts o = parse_opts(2, {L"--kind", L"--want", L"--min", L"--per-gb", L"--per-commit-gb", L"--timeout", L"--state", L"--config", L"--pid", L"--max-s"},
                        {L"--detach", L"--as-helper"});
    string err = o.err;
    Want w;
    w.kind = u8(opt_str(o, L"--kind"));
    w.mode = "hold";
    w.want = (int)std::min(4096.0, std::max(1.0, opt_num(o, L"--want", 1, err)));   // clamped before the cast (a huge value is not UB)
    w.min = (int)std::min((double)w.want, std::max(1.0, opt_num(o, L"--min", 1, err)));
    w.phys = opt_num(o, L"--per-gb", -1, err);
    w.commit = opt_num(o, L"--per-commit-gb", -1, err);
    w.for_pid = (unsigned long)opt_num(o, L"--pid", 0, err);
    bool detach = o.kv.count(L"--detach") > 0, helper = o.kv.count(L"--as-helper") > 0;
    double max_s = opt_num(o, L"--max-s", detach ? 3600 : 0, err);   // a detached hold ends after an hour at the latest (CQA review bug 8)
    double timeout_opt = opt_num(o, L"--timeout", -1, err);
    if (detach && !helper) return hold_front(o, w.for_pid, w.want, w.kind);
    HistRow hr;
    hr.mode = detach ? "hold_detach" : "hold";
    hr.prog = w.for_pid ? fmt("hold for pid %lu", w.for_pid) : "hold";
    w.mode = hr.mode;
    w.prog = hr.prog;
    Ctx c;
    Admission a;
    int grant = w.want;
    double t0 = qpc_s();
    HANDLE rel = CreateEventW(nullptr, TRUE, FALSE, release_event_name(GetCurrentProcessId()).c_str());
    string which;
    wstring marker = held_marker(w.kind.empty() ? "-" : w.kind);
    if (env_on(L"WWE_NO_ADMIT")) {
        hr.outcome = "off";
        c.state = state_if_exists();
    } else {
        string serr;
        bool ok = setup(c, opt_str(o, L"--state"), opt_str(o, L"--config"), serr);
        if (ok && !err.empty()) { ok = false; serr = err; }
        err = serr;
        if (ok) settle_kind(c, w);
        hr.qmode = c.cfg.mode;
        marker = held_marker(w.kind.empty() ? "-" : w.kind);
        int sw = 0, cw = 0;
        unsigned long cover = 0;
        bool observe = ok && c.cfg.mode == "observe";   // observe never narrows: no nesting by any route, every hold is observed in full
        if (!ok) {
            say_err("[admit] ERROR " + (err.empty() ? c.state_err + c.cfg_err : err) + ": running without admission");
            hr.outcome = "error_open";
        } else if (!observe && held_covers(c.cfg, w.kind, &cover)) {   // inside a covering admission (the marker)
            // no reservation; at most the covering admission's workers (CQA addendum A2); the covering marker is passed on
            grant = std::min(w.want, holder_workers(c, cover));
            hr.outcome = "nested_hold";
            hr.prog += fmt(" (marker of pid %lu)", cover);
            marker = getenv_w(L"WWE_ADMIT_HELD");
        } else if (!observe && w.for_pid && (sw = same_owner_workers(c, w.for_pid, w.kind)) > 0) {   // P already holds this kind: nest, absorb its release
            grant = std::min(w.want, sw);
            hr.outcome = "nested_hold";
            hr.prog += " (same owner)";
            nest_add(c, w.for_pid, w.kind, +1);
        } else if (!observe && nested_in_hold(c, w.kind, which, cw)) {
            grant = std::min(w.want, std::max(1, cw));
            hr.outcome = "nested_hold";
            hr.prog += " (in " + which + ")";
        } else {
            const Kind& k = c.cfg.kinds.at(w.kind);
            if (w.phys < 0) w.phys = k.phys;
            if (w.commit < 0) w.commit = k.commit;
            w.min = std::min(w.min, std::max(1, k.slots));   // a minimum above the kind's slots could never be met
            hr.rp = w.phys;
            hr.rc = w.commit;
            if (observe) {   // decide once, log what enforce would have done, grant the full want
                long long would = 0;
                observe_take(c, w, a.t, a.s, would);
                hr.outcome = "observed";
                hr.would_grant = would;
                hr.would_reason = a.s.reason;
                hr.avail = a.s.mem.phys;
                hr.commit = a.s.mem.commit;
                hr.holders = a.s.holders;
                hr.pending = a.s.pend_p;
                hr.held_use = a.s.use_p;
            } else {
                double timeout = timeout_opt >= 0 ? timeout_opt : c.cfg.timeout_s;
                // S109 GATE: a hold not admitted within its bound is REFUSED (exit 75, stdout `refused <line>`), or NARROWED below its minimum when one fits
                w.watch_pid = helper ? w.for_pid : 0;   // a waiting helper's caller is P (its front exits by design); otherwise the parent
                admit_wait(c, w, timeout, a, [&](const Admission& x) {
                    HistRow b = hr;
                    b.kind = w.kind;
                    history(c, waiting_row(b, x));
                });
                hr.outcome = outcome_name(a.out);
                hr.waited = a.waited;
                hr.avail = a.s.mem.phys;
                hr.commit = a.s.mem.commit;
                hr.holders = a.s.holders;
                hr.pending = a.s.pend_p;
                hr.held_use = a.s.use_p;
                if (a.out == REFUSED || a.out == ORPHANED) {
                    hr.kind = w.kind;
                    hr.has_exit = true;
                    hr.exit_code = EXIT_REFUSED;
                    hr.workers = 0;
                    hr.ended = a.out == ORPHANED ? "orphaned" : "refused:" + a.s.reason;
                    history(c, hr);
                    if (a.out == REFUSED) say_out("refused " + a.refusal + "\n");
                    return EXIT_REFUSED;
                }
                if (a.out == ADMITTED || a.out == NARROWED) grant = (int)a.t.slots.size();
            }
        }
    }
    hr.kind = w.kind.empty() ? "-" : w.kind;
    hr.workers = grant;
    bool watch_stdin = !detach;
    if (watch_stdin && GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_PIPE) {   // NUL or a console: EOF would come at once, or never
        watch_stdin = false;
        say_err(fmt("[admit] hold: stdin is not a pipe: holding until the parent (pid %lu) exits%s (from PowerShell use --pid $PID --detach)",
                    w.for_pid ? w.for_pid : parent_pid(), w.for_pid ? " or admit release" : ""));
    }
    say_out(fmt("granted %d %s\n", grant, grant_word(hr.outcome)));
    say_out("marker " + u8(marker) + "\n");
    if (helper) {   // the front has its lines: let go of the private pipe so nothing lingers on it
        HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE), se = GetStdHandle(STD_ERROR_HANDLE);
        SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
        SetStdHandle(STD_ERROR_HANDLE, nullptr);
        if (so && so != INVALID_HANDLE_VALUE) CloseHandle(so);
        if (se && se != INVALID_HANDLE_VALUE && se != so) CloseHandle(se);
        if (a.t.slots.empty()) {   // nothing reserved (fail-open / nested): nothing to keep; the row says so
            hr.ended = "nothing_held";
            history(c, hr);
            return 0;
        }
    }
    hr.ended = hold_until_released(w.for_pid, watch_stdin, rel, max_s);
    bool held = !a.t.slots.empty();
    a.t.release();
    if (held && w.for_pid) DeleteFileW(nest_counter(c, w.for_pid, w.kind).c_str());   // the outer hold ends: no nested release is left to absorb
    hr.held = qpc_s() - t0 - hr.waited;
    hr.has_exit = true;
    hr.exit_code = 0;
    history(c, hr);
    return 0;
}
static int cmd_release() {
    Opts o = parse_opts(2, {L"--pid", L"--kind", L"--state", L"--config"});
    string err = o.err;
    unsigned long P = (unsigned long)opt_num(o, L"--pid", 0, err);
    if (!P || !err.empty()) { say_err("usage: admit release --pid P [--kind K]" + (err.empty() ? "" : " (" + err + ")")); return 2; }
    Ctx c;
    setup(c, opt_str(o, L"--state"), opt_str(o, L"--config"), err);
    if (c.state.empty()) { say_err("[admit] ERROR " + c.state_err + ": nothing released"); return 0; }
    int absorbed = 0;
    int n = release_for(c, P, u8(opt_str(o, L"--kind")), absorbed);
    say_out(fmt("released %d hold(s) for pid %lu%s\n", n, P, absorbed ? fmt(" (%d nested release(s) absorbed: the outer hold stays)", absorbed).c_str() : ""));
    return 0;
}

// ---------------------------------------------------------------- status
static int cmd_status() {
    Opts o = parse_opts(2, {L"--state", L"--config"});
    Ctx c;
    string err;
    setup(c, opt_str(o, L"--state"), opt_str(o, L"--config"), err);
    wstring em = getenv_w(L"WWE_ADMIT_MODE");
    say_out("admit status: mode " + c.cfg.mode + (em == L"observe" || em == L"enforce" ? " (from WWE_ADMIT_MODE)" : c.cfg.path.empty() ? " (default)" : " (from the config)") +
            (c.cfg.mode == "observe" ? ": nothing waits or is narrowed; history logs what enforce would have done\n" : fmt(": askers wait for memory at most %.0f s, then are refused (exit 75)\n", c.cfg.timeout_s)));
    // S109 GATE: staleness is judged by CONTENT (the sidecar's SHA-256 of the admit.cpp it was built from) by tools\admit\status.py, and repaired by the
    // callers (build_admit.ps1 -IfStale); a file time said STALE after a one-line label change and said nothing useful
    string stamp;
    read_file(g_self + L".sha256", stamp);
    while (!stamp.empty() && (stamp.back() == '\n' || stamp.back() == '\r' || stamp.back() == ' ')) stamp.pop_back();
    say_out("  exe      " + u8(g_self) + (stamp.empty() ? " (no source stamp: built before S109 GATE; the callers rebuild it)" : " (built from admit.cpp sha256 " + stamp.substr(0, 12) + "; py -I tools\\admit\\status.py compares)") + "\n");
    say_out("  state    " + (c.state.empty() ? "(none: " + c.state_err + ")" : u8(c.state)) + "\n");
    say_out("  config   " + (c.cfg.path.empty() ? "(none: " + c.cfg_err + ")" : u8(c.cfg.path)) + "\n");
    Mem m;
    string e;
    if (!c.state.empty() && read_mem(c, m, e)) say_out(fmt("  memory   available %.1f GB, commit left %.1f GB%s\n", m.phys, m.commit, m.fake ? " (FAKE: selftest file)" : ""));
    else say_out("  memory   unreadable: " + e + "\n");
    say_out(fmt("  rule     floor %.1f GB phys, %.1f GB commit; pending: holds for life, runs %.0f s; senior after %.0f s; timeout %.0f s; default kind %s\n",
                c.cfg.floor_phys, c.cfg.floor_commit, c.cfg.window_s, c.cfg.starve_s, c.cfg.timeout_s, c.cfg.default_kind.c_str()));
    for (auto& [n, k] : c.cfg.kinds) say_out(fmt("  kind     %-15s %4.1f GB phys, %4.1f GB commit, %d slots\n", n.c_str(), k.phys, k.commit, k.slots));
    View v = c.state.empty() ? View{} : view_of(c);
    say_out(fmt("  holders  %zu (pending %.1f GB phys, %.1f GB commit)\n", v.hs.size(), v.pend_p, v.pend_c));
    for (auto& h : v.hs)
        say_out(fmt("    %s.%d  pid %lu  %s  age %.0f s  %.1f / %.1f GB  %s\n", h.kind.c_str(), h.index, h.pid, h.mode.c_str(), h.age_s, h.phys, h.commit, h.label.c_str()));
    vector<Waiter> ws = c.state.empty() ? vector<Waiter>{} : scan_waiters(c);
    say_out(fmt("  waiters  %zu\n", ws.size()));
    for (auto& x : ws) say_out(fmt("    pid %lu  %s  waited %.0f s%s\n", x.pid, x.kind.c_str(), x.waited_s, x.runlock ? "  (run lock)" : ""));
    return 0;
}

// ---------------------------------------------------------------- hidden child modes (selftest)
static int child_echo(const wchar_t* file) { return write_file(file, u8(GetCommandLineW())) ? 0 : 3; }
static int child_wait(int argc, wchar_t** argv) {
    wstring st, go, pidf;
    int code = 0;
    for (int i = 2; i + 1 < argc; i++) {
        wstring a = argv[i];
        if (a == L"--start") st = argv[++i];
        else if (a == L"--go") go = argv[++i];
        else if (a == L"--pidfile") pidf = argv[++i];
        else if (a == L"--exit") code = _wtoi(argv[++i]);
    }
    if (!pidf.empty()) write_file(pidf, fmt("%lu", GetCurrentProcessId()));
    if (!st.empty()) {
        HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, st.c_str());
        if (e) { SetEvent(e); CloseHandle(e); }
    }
    if (!go.empty()) {
        HANDLE g = OpenEventW(SYNCHRONIZE, FALSE, go.c_str());
        if (!g) return 98;
        DWORD w = WaitForSingleObject(g, 120000);   // an upper bound only: the selftest signals it
        CloseHandle(g);
        if (w != WAIT_OBJECT_0) return 99;
    }
    return code;
}
static int child_count(const wchar_t* file) {
    Ctx c;
    string err;
    setup(c, L"", L"", err);
    size_t n = c.state.empty() ? 0 : scan(c).size();
    return write_file(file, fmt("%zu", n)) ? 0 : 3;
}
static bool plain_start(const wstring& cmdline, PROCESS_INFORMATION& pi) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);
    return CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, has_console() ? 0 : CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
}
static int child_parent() {   // __parent [--go EVT] -- CMD: start CMD (no job of its own), wait for EVT (if any), exit 0
    Opts o = parse_opts(2, {L"--go"});
    if (!o.has_tail) return 2;
    PROCESS_INFORMATION pi{};
    if (!plain_start(o.tail, pi)) return 97;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    HANDLE g = o.kv.count(L"--go") ? OpenEventW(SYNCHRONIZE, FALSE, o.kv.at(L"--go").c_str()) : nullptr;
    if (g) { WaitForSingleObject(g, 120000); CloseHandle(g); }
    return 0;
}
static int child_later() {   // __later --go EVT -- CMD: wait for EVT, then run CMD and exit with its code
    Opts o = parse_opts(2, {L"--go"});
    if (!o.has_tail) return 2;
    HANDLE g = OpenEventW(SYNCHRONIZE, FALSE, opt_str(o, L"--go").c_str());
    if (!g) return 98;
    DWORD w = WaitForSingleObject(g, 120000);
    CloseHandle(g);
    if (w != WAIT_OBJECT_0) return 99;
    PROCESS_INFORMATION pi{};
    if (!plain_start(o.tail, pi)) return 97;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 96;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
static int child_memcost() {   // __memcost: microseconds per call of the two commit readers (1000 calls each; no load)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof pi;
    double t0 = qpc_s();
    for (int i = 0; i < 1000; i++) GlobalMemoryStatusEx(&ms);
    double t1 = qpc_s();
    for (int i = 0; i < 1000; i++) GetPerformanceInfo(&pi, sizeof pi);
    double t2 = qpc_s();
    const double GB = 1024.0 * 1024.0 * 1024.0;
    say_out(fmt("GlobalMemoryStatusEx %.1f us/call (avail page file %.2f GB); GetPerformanceInfo %.1f us/call (limit - total %.2f GB)\n", (t1 - t0) * 1000.0,
                (double)ms.ullAvailPageFile / GB, (t2 - t1) * 1000.0, (double)(pi.CommitLimit - pi.CommitTotal) * pi.PageSize / GB));
    return 0;
}
static int child_jobinfo(const wchar_t* file) {   // is my parent in my innermost job? (no job of admit's own = yes, inside the selftest's job)
    struct {
        JOBOBJECT_BASIC_PROCESS_ID_LIST h;
        ULONG_PTR more[1023];
    } list{};
    unsigned long pp = parent_pid();
    bool in = false;
    DWORD n = 0;
    if (QueryInformationJobObject(nullptr, JobObjectBasicProcessIdList, &list, sizeof list, nullptr)) {
        n = list.h.NumberOfProcessIdsInList;
        for (DWORD i = 0; i < n; i++)
            if ((unsigned long)list.h.ProcessIdList[i] == pp) in = true;
    }
    return write_file(file, fmt("parent_in_job=%d n=%lu", in ? 1 : 0, n)) ? 0 : 3;
}

// ---------------------------------------------------------------- selftest
static wstring g_target;   // the admit exe under test (default: this exe; --target runs the cases against another build)
static HANDLE g_stjob = nullptr;
struct ST {
    wstring root, helper;
    int fails = 0, cases = 0, evn = 0;
    bool plant = false;
    string plant_name;
    string hist_line;
};
static HANDLE out_file(const wstring& p) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    return CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS, 0, nullptr);
}
static P spawn(const wstring& cmdline, HANDLE in, HANDLE out, HANDLE err) {
    return spawn_ex(cmdline, in, out, err, has_console() ? 0 : CREATE_NO_WINDOW, g_stjob);
}
static long wait_exit(P& p, DWORD ms) {   // exit code, or -1 (timed out: killed) / -2 (never started)
    if (!p.h) return -2;
    if (WaitForSingleObject(p.h, ms) != WAIT_OBJECT_0) { TerminateProcess(p.h, 1); WaitForSingleObject(p.h, 5000); CloseHandle(p.h); p.h = nullptr; return -1; }
    DWORD c = 0;
    GetExitCodeProcess(p.h, &c);
    CloseHandle(p.h);
    p.h = nullptr;
    return (long)c;
}
static string slurp(const wstring& p) {
    string s;
    read_file(p, s);
    return s;
}
static int count_of(const string& s, const string& needle) {
    int n = 0;
    for (size_t k = s.find(needle); k != string::npos; k = s.find(needle, k + 1)) n++;
    return n;
}
static string first_line(const string& s) { return s.substr(0, s.find('\n')); }
static wstring g_cfg_path;
static wstring write_cfg(ST& st, const wstring& name, const string& body) {
    wstring p = join(st.root, name + L".json");
    write_file(p, body);
    return p;
}
static string std_cfg(int compile_slots, double window, double timeout, double starve = 30) {
    return fmt("{\"floor_phys_gb\": 4, \"floor_commit_gb\": 4, \"pending_window_s\": %g, \"timeout_s\": %g, \"starve_s\": %g, \"default_kind\": \"compile\", \"kinds\": {"
               "\"compile\": {\"reserve_phys_gb\": 2.0, \"reserve_commit_gb\": 2.5, \"max_slots\": %d}, "
               "\"parse\": {\"reserve_phys_gb\": 1.7, \"reserve_commit_gb\": 1.7, \"max_slots\": 16}, "
               "\"big\": {\"reserve_phys_gb\": 11.0, \"reserve_commit_gb\": 11.0, \"max_slots\": 4}, "
               "\"link\": {\"reserve_phys_gb\": 2.0, \"reserve_commit_gb\": 2.0, \"max_slots\": 1}, "
               "\"huge\": {\"reserve_phys_gb\": 1000.0, \"reserve_commit_gb\": 1000.0, \"max_slots\": 4}}}",
               window, timeout, starve, compile_slots);
}
static wstring set_case_cfg(ST& st, const wstring& name, const string& cfg, const char* fake) {
    wstring sd = join(st.root, name);
    make_dirs(sd);
    if (fake) write_file(join(sd, L"fake_mem.txt"), fake);
    g_cfg_path = write_cfg(st, name, cfg);
    SetEnvironmentVariableW(L"WWE_ADMIT_DIR", sd.c_str());
    SetEnvironmentVariableW(L"WWE_ADMIT_CONFIG", g_cfg_path.c_str());
    return sd;
}
static wstring set_case(ST& st, const wstring& name, int compile_slots, double window, double timeout, const char* fake) {
    return set_case_cfg(st, name, std_cfg(compile_slots, window, timeout), fake);
}
static void set_fake(const wstring& sd, const char* fake) { write_file(join(sd, L"fake_mem.txt"), fake); }
static int live_count(const wstring& sd, vector<Holder>* out = nullptr) {
    Ctx c;
    c.state = sd;
    load_config(g_cfg_path, c.cfg, c.cfg_err);
    vector<Holder> hs = scan(c);
    if (out) *out = hs;
    return (int)hs.size();
}
// runs `<target> ARGS` with stdout + stderr into <root>\TAG.err; returns the exit code (-1: killed after ms)
static long run_admit(ST& st, const wstring& args, const wstring& tag, string* errtext = nullptr, double* secs = nullptr, DWORD ms = 60000) {
    wstring ef = join(st.root, tag + L".err");
    HANDLE e = out_file(ef);
    double t0 = qpc_s();
    P p = spawn(q(g_target) + L" " + args, nullptr, e, e);
    CloseHandle(e);
    long rc = wait_exit(p, ms);
    if (secs) *secs = qpc_s() - t0;
    if (errtext) *errtext = slurp(ef);
    return rc;
}
struct WJ {
    P p;
    HANDLE started = nullptr, go = nullptr;
    wstring err, pidf;
};
static std::pair<wstring, wstring> ev_names(ST& st) {
    wstring b = L"Local\\wwe_admit_" + wnum(GetCurrentProcessId()) + L"_" + wnum((unsigned)++st.evn);
    return {b + L"_s", b + L"_g"};
}
// `<target> ADMIT_ARGS -- <self> __wait ...`: an admitted job whose child waits for its go event (started = the child runs)
static WJ wait_job(ST& st, const wstring& tag, const wstring& admit_args) {
    WJ j;
    auto [s, g] = ev_names(st);
    j.started = CreateEventW(nullptr, TRUE, FALSE, s.c_str());
    j.go = CreateEventW(nullptr, TRUE, FALSE, g.c_str());
    j.err = join(st.root, tag + L".err");
    j.pidf = join(st.root, tag + L".pid");
    HANDLE e = out_file(j.err);
    j.p = spawn(q(g_target) + L" " + admit_args + L" -- " + q(g_self) + L" __wait --start " + s + L" --go " + g + L" --pidfile " + q(j.pidf), nullptr, e, e);
    CloseHandle(e);
    return j;
}
static WJ plain_wait(ST& st) {   // a process P with no admit: it waits for its go event
    WJ j;
    auto [s, g] = ev_names(st);
    j.started = CreateEventW(nullptr, TRUE, FALSE, s.c_str());
    j.go = CreateEventW(nullptr, TRUE, FALSE, g.c_str());
    j.p = spawn(q(g_self) + L" __wait --start " + s + L" --go " + g, nullptr, nullptr, nullptr);
    return j;
}
static bool signalled(HANDLE ev, DWORD ms) { return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0; }
static long finish(WJ& j, DWORD ms = 30000) {
    SetEvent(j.go);
    long rc = wait_exit(j.p, ms);
    CloseHandle(j.started);
    CloseHandle(j.go);
    return rc;
}
// waits (generous bound) until FILE holds NEEDLE, or EV (if given) is signalled first; 1 = text seen, 2 = event first, 0 = neither
static int wait_text_or(const wstring& file, const string& needle, HANDLE ev, DWORD ms) {
    double t0 = qpc_s();
    for (;;) {
        if (slurp(file).find(needle) != string::npos) return 1;
        if (ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0) return 2;
        if ((qpc_s() - t0) * 1000 > ms) return 0;
        Sleep(50);
    }
}
struct Case {
    string name, info;
    vector<string> fails;
    void need(bool ok, const string& what) {
        if (!ok) fails.push_back(what);
    }
};
static void report(ST& st, Case& c) {
    st.cases++;
    if (c.fails.empty()) {
        say_out(fmt("PASS  case %s%s%s\n", c.name.c_str(), c.info.empty() ? "" : ": ", c.info.c_str()));
        return;
    }
    st.fails++;
    say_out(fmt("FAIL  case %s%s%s\n", c.name.c_str(), c.info.empty() ? "" : ": ", c.info.c_str()));
    for (auto& f : c.fails) say_out("        - " + f + "\n");
}
enum Col { C_TIME, C_KIND, C_PID, C_WAITED, C_RP, C_RC, C_AVAIL, C_COMMIT, C_HOLDERS, C_OUTCOME, C_EXIT, C_HELD, C_PEAK, C_SCOPE, C_MODE, C_WORKERS, C_ENDED,
           C_QMODE, C_WOULD, C_WREASON, C_PROG, C_PENDING, C_HELDUSE, C_N };
static vector<vector<string>> hist_rows(const wstring& sd) {
    vector<vector<string>> rows;
    string t = slurp(join(sd, L"history.tsv"));
    size_t a = 0;
    bool first = true;
    while (a < t.size()) {
        size_t b = t.find('\n', a);
        if (b == string::npos) b = t.size();
        string line = t.substr(a, b - a);
        a = b + 1;
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        vector<string> cols;
        size_t x = 0;
        for (;;) {
            size_t y = line.find('\t', x);
            cols.push_back(line.substr(x, y == string::npos ? string::npos : y - x));
            if (y == string::npos) break;
            x = y + 1;
        }
        cols.resize(std::max<size_t>(cols.size(), C_N));
        rows.push_back(cols);
    }
    return rows;
}
static bool has_row(const vector<vector<string>>& rows, int col, const string& v) {
    for (auto& r : rows)
        if (r[col] == v) return true;
    return false;
}
static string read_line_pipe(HANDLE r, DWORD ms) {
    string s;
    double t0 = qpc_s();
    while ((qpc_s() - t0) * 1000 < ms) {
        DWORD avail = 0;
        if (!PeekNamedPipe(r, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail) {
            char ch;
            DWORD n = 0;
            if (!ReadFile(r, &ch, 1, &n, nullptr) || !n) break;
            if (ch == '\n') return s;
            if (ch != '\r') s += ch;
            continue;
        }
        Sleep(20);
    }
    return s;
}
static bool wait_live(const wstring& sd, int want, DWORD ms) {
    double t0 = qpc_s();
    for (;;) {
        if (live_count(sd) == want) return true;
        if ((qpc_s() - t0) * 1000 > ms) return false;
        Sleep(50);
    }
}
static bool wait_row(const wstring& sd, int col, const string& v, DWORD ms) {
    double t0 = qpc_s();
    for (;;) {
        if (has_row(hist_rows(sd), col, v)) return true;
        if ((qpc_s() - t0) * 1000 > ms) return false;
        Sleep(50);
    }
}
static const DWORD LONG_MS = 30000;   // generous upper bounds: a busy PC must not fail a case

static void case1(ST& st) {
    Case c{"1 null run, exit codes, argument fidelity, history, peak_scope"};
    wstring sd = set_case(st, L"c1", 24, 60, 900, "50 50");
    string e;
    double secs = 0;
    long rc = run_admit(st, L"run --kind compile -- cmd /c exit 0", L"c1a", &e, &secs);
    c.need(rc == 0, fmt("cmd /c exit 0 gave %ld", rc));
    c.need(e.empty(), "stderr not empty: " + e);
    rc = run_admit(st, L"run --kind compile -- cmd /c exit 7", L"c1b");
    c.need(rc == 7, fmt("exit 7 came back as %ld", rc));
    wstring echo = join(st.root, L"c1_echo.txt");
    wstring tail = q(g_self) + L" __echo " + q(echo) +
                   L" plain \"two words\" \"q\\\"uote\" 100% %PATH% ^caret a&b|c <x> c:\\dir\\ \"trail\\\\\" back\\\\\\\"q 'single' \"\" caf\u00e9 \"a\"\"b\" end";
    rc = run_admit(st, L"run --kind compile -- " + tail, L"c1c");
    string got = slurp(echo);
    c.need(rc == 0 && got == u8(tail), "short line not verbatim:\n          sent " + u8(tail) + "\n          got  " + got);
    wstring tail2 = tail + L" " + wstring(30000 - tail.size() - 1, L'x');
    DeleteFileW(echo.c_str());
    rc = run_admit(st, L"run --kind compile -- " + tail2, L"c1d");
    got = slurp(echo);
    c.need(rc == 0 && got == u8(tail2), fmt("30,000-character line not verbatim (rc %ld, got %zu bytes, sent %zu)", rc, got.size(), tail2.size()));
    wstring scc = join(st.root, L"sccache.exe");   // a program named sccache: its compile would run in the server, outside the job
    CopyFileW(g_self.c_str(), scc.c_str(), FALSE);
    rc = run_admit(st, L"run --kind compile -- " + q(scc) + L" __touch " + q(join(st.root, L"c1_scc")), L"c1e");
    c.need(rc == 0, fmt("sccache-named program rc %ld", rc));
    auto rows = hist_rows(sd);
    c.need(rows.size() == 5, fmt("history rows %zu, want 5", rows.size()));
    for (size_t i = 0; i + 1 < rows.size() && i < 4; i++) {   // rows 0-1 run cmd (scope partial by rule), rows 2-3 a plain exe (job: a measurement)
        const char* want = i < 2 ? "partial" : "job";
        c.need(rows[i][C_SCOPE] == want && atof(rows[i][C_PEAK].c_str()) > 0 && rows[i][C_ENDED] == "child_exit",
               fmt("row %zu: peak '%s' scope '%s' ended '%s' (want a number, %s, child_exit)", i, rows[i][C_PEAK].c_str(), rows[i][C_SCOPE].c_str(), rows[i][C_ENDED].c_str(), want));
    }
    if (rows.size() == 5) c.need(rows[4][C_SCOPE] == "unknown" && rows[4][C_PEAK] == "-", "the sccache row: scope '" + rows[4][C_SCOPE] + "' peak '" + rows[4][C_PEAK] + "' (want unknown, -)");
    if (!rows.empty()) {
        double w = atof(rows[0][C_WAITED].c_str());
        c.need(w < 0.1, fmt("lone job waited %.3f s", w));
        c.info = fmt("waited %.4f s, wall %.0f ms, peak %s MB", w, secs * 1000, rows[0][C_PEAK].c_str());
    }
    string t = slurp(join(sd, L"history.tsv"));
    size_t nl = t.find('\n');
    if (nl != string::npos) {
        size_t nl2 = t.find('\n', nl + 1);
        st.hist_line = t.substr(0, nl) + "\n" + t.substr(nl + 1, nl2 == string::npos ? string::npos : nl2 - nl - 1);
    }
    report(st, c);
}
static void case2(ST& st) {
    // S106 0C: was "a lone job below the floor is admitted at once": that admitted a job into a PC with its memory short (the old lone-job rule)
    Case c{"2 a lone job whose minimum does not fit the free memory WAITS (it is admitted at once only when it fits or can never fit this PC: case 24)"};
    wstring sd = set_case(st, L"c2", 24, 60, 900, "1.0 1.0");
    string e;
    long rc = run_admit(st, L"run --kind compile --timeout 2 -- cmd /c exit 0", L"c2", &e);
    auto rows = hist_rows(sd);
    c.need(rc == EXIT_REFUSED, fmt("rc %ld, want 75 (it must wait, not run)", rc));
    c.need(e.find("[admit] waiting:") != string::npos && e.find("; refused after 2 s unless admitted") != string::npos, "no waiting line naming its bound: " + e);
    c.need(rows.size() == 2 && rows[0][C_OUTCOME] == "waiting" && rows[1][C_OUTCOME] == "refused", "the rows are not waiting + refused (S109 GATE)");
    set_fake(sd, "30 30");   // the same lone job with room: admitted at once
    rc = run_admit(st, L"run --kind compile --timeout 5 -- cmd /c exit 0", L"c2b", &e);
    rows = hist_rows(sd);
    c.need(rc == 0 && rows.size() == 3 && rows[2][C_OUTCOME] == "admitted" && atof(rows[2][C_WAITED].c_str()) < 0.1, "with room it was not admitted at once");
    if (!rows.empty()) c.info = "available " + rows[0][C_AVAIL] + " GB, commit " + rows[0][C_COMMIT] + " GB, waited " + rows[0][C_WAITED] + " s";
    report(st, c);
}
static void case3(ST& st) {
    Case c{"3 two jobs, memory for one (the first one's use has shown up): the second waits, says so once, runs when the first exits"};
    wstring sd = set_case(st, L"c3", 24, 0, 900, "7 7");
    WJ a = wait_job(st, L"c3a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    set_fake(sd, "5 5");
    WJ b = wait_job(st, L"c3b", L"run --kind compile");
    int w = wait_text_or(b.err, "[admit] waiting:", b.started, LONG_MS);
    c.need(w == 1, w == 2 ? "B was admitted while memory was short" : "B printed no waiting line");
    long ra = finish(a);
    set_fake(sd, "7 7");
    c.need(signalled(b.started, LONG_MS), "B not admitted after A exited");
    long rb = finish(b);
    string be = slurp(b.err);
    c.need(ra == 0 && rb == 0, fmt("exit codes %ld %ld", ra, rb));
    c.need(count_of(be, "[admit] waiting:") == 1, fmt("B's waiting lines: %d", count_of(be, "[admit] waiting:")));
    c.info = "B said: " + first_line(be);
    report(st, c);
}
static void case3b(ST& st) {
    Case c{"3b two jobs, one slot: the second waits for the slot"};
    set_case(st, L"c3b", 1, 60, 900, "100 100");
    WJ a = wait_job(st, L"c3ba", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    WJ b = wait_job(st, L"c3bb", L"run --kind compile");
    int w = wait_text_or(b.err, "all 1 compile slots held", b.started, LONG_MS);
    c.need(w == 1, w == 2 ? "B was admitted with the only slot held" : "B's line does not name the slots");
    finish(a);
    c.need(signalled(b.started, LONG_MS), "B not admitted after A exited");
    finish(b);
    report(st, c);
}
// case 4 and its planted twin: returns 1 when B printed its waiting line before being admitted (the right behaviour), 2 when B was admitted first
static int case4_run(ST& st, const wchar_t* tag, const wchar_t* plant, string& bsaid) {
    set_case(st, tag, 24, 60, 900, "7 7");
    if (plant) SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", plant);
    WJ a = wait_job(st, wstring(tag) + L"a", L"run --kind compile");
    signalled(a.started, LONG_MS);
    WJ b = wait_job(st, wstring(tag) + L"b", L"run --kind compile");
    int w = wait_text_or(b.err, "[admit] waiting:", b.started, LONG_MS);
    bsaid = first_line(slurp(b.err));
    finish(a);
    signalled(b.started, LONG_MS);
    finish(b);
    if (plant) SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", st.plant ? w16(st.plant_name).c_str() : nullptr);
    return w;
}
static void case4(ST& st) {
    Case c{"4 the pending window: two jobs a second apart, numbers that do not drop fit one: the second waits"};
    string said;
    int w = case4_run(st, L"c4", nullptr, said);
    c.need(w == 1, w == 2 ? "B was admitted on the same memory reading (the Session 103 fault)" : "B printed no waiting line");
    c.need(said.find("pending 2.0 GB") != string::npos, "B's line does not count A as pending: " + said);
    c.info = "B said: " + said;
    report(st, c);
}
static void case4_planted(ST& st) {
    Case c{"4P planted fault (pending window off): case 4 must FAIL"};
    string said;
    int w = case4_run(st, L"c4p", L"no_pending", said);
    c.need(w == 2, "case 4 still passed with the pending window switched off: the check proves nothing");
    c.info = w == 2 ? "with the plant B was admitted before any waiting line: case 4 fails, as it must" : "";
    report(st, c);
}
static void case5_11(ST& st) {
    Case c{"5 + 11a commit short: hold --want 28 --per-commit-gb 1.7 gets <= 10; a marker line; closing stdin releases"};
    wstring sd = set_case(st, L"c5", 24, 60, 900, "100 21");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE inR, inW, outR, outW;
    CreatePipe(&inR, &inW, &sa, 0);
    CreatePipe(&outR, &outW, &sa, 0);
    HANDLE ef = out_file(join(st.root, L"c5.err"));
    P h = spawn(q(g_target) + L" hold --kind parse --want 28 --per-commit-gb 1.7", inR, outW, ef);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(ef);
    string line = read_line_pipe(outR, LONG_MS), mline = read_line_pipe(outR, LONG_MS);
    int m = -1;
    sscanf(line.c_str(), "granted %d", &m);
    c.need(m == 10, "granted line '" + line + "', want 'granted 10' ((21 - 4) / 1.7)");
    c.need(mline.rfind("marker ", 0) == 0, "no marker line: '" + mline + "'");
    int live = live_count(sd);
    c.need(live == m, fmt("live slots %d, granted %d", live, m));
    CloseHandle(inW);
    long rc = wait_exit(h, LONG_MS);
    c.need(rc == 0, fmt("hold exit %ld after stdin closed", rc));
    c.need(wait_live(sd, 0, LONG_MS), fmt("%d slots still held after hold exited", live_count(sd)));
    c.need(wait_row(sd, C_ENDED, "stdin_eof", LONG_MS), "no history row ended 'stdin_eof'");
    CloseHandle(outR);
    c.info = fmt("granted %d of 28", m);
    report(st, c);
}
static void case11b(ST& st) {
    Case c{"11b hold: the parent's death releases the reservation"};
    wstring sd = set_case(st, L"c11", 24, 60, 900, "100 100");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE inR, inW, outR, outW;
    CreatePipe(&inR, &inW, &sa, 0);
    CreatePipe(&outR, &outW, &sa, 0);
    auto [s, g] = ev_names(st);
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, g.c_str());
    HANDLE ef = out_file(join(st.root, L"c11.err"));
    P par = spawn(q(g_self) + L" __parent --go " + g + L" -- " + q(g_target) + L" hold --kind parse --want 2", inR, outW, ef);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(ef);
    string line = read_line_pipe(outR, LONG_MS);
    c.need(line == "granted 2 held", "granted line: '" + line + "'");
    c.need(live_count(sd) == 2, fmt("live %d, want 2", live_count(sd)));
    TerminateProcess(par.h, 1);
    wait_exit(par, LONG_MS);
    c.need(wait_live(sd, 0, LONG_MS), "slots still held after the parent died (stdin still open)");
    CloseHandle(inW);
    CloseHandle(outR);
    CloseHandle(go);
    report(st, c);
}
static void case6(ST& st) {
    Case c{"6 a killed holder stops counting at once (not after the 60 s window); a killed admit kills its child"};
    wstring sd = set_case(st, L"c6", 24, 60, 900, "7 7");
    WJ a = wait_job(st, L"c6a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    unsigned long cpid = strtoul(slurp(a.pidf).c_str(), nullptr, 10);
    HANDLE child = cpid ? OpenProcess(SYNCHRONIZE, FALSE, cpid) : nullptr;
    c.need(child != nullptr, "A's child pid unreadable");
    WJ b = wait_job(st, L"c6b", L"run --kind compile");
    int w = wait_text_or(b.err, "[admit] waiting:", b.started, LONG_MS);
    c.need(w == 1, "B was not held back by A's pending reservation");
    double t0 = qpc_s();
    TerminateProcess(a.p.h, 1);
    bool bs = signalled(b.started, LONG_MS);
    double dt = qpc_s() - t0;
    c.need(bs && dt < 30.0, fmt("B admitted %.2f s after A was killed (the window is 60 s)", dt));
    c.need(child && WaitForSingleObject(child, LONG_MS) == WAIT_OBJECT_0, "A's child still alive after A was killed");
    if (child) CloseHandle(child);
    wait_exit(a.p, LONG_MS);
    CloseHandle(a.started);
    CloseHandle(a.go);
    finish(b);
    c.info = fmt("B admitted %.2f s after A was killed", dt);
    report(st, c);
}
static void case7(ST& st) {
    Case c{"7 the bound (S109 GATE): a run not admitted in its 1 s is REFUSED (exit 75), starts nothing, and the loud line names the holder"};
    wstring sd = set_case(st, L"c7", 1, 60, 900, "100 100");
    WJ a = wait_job(st, L"c7a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    wstring touched = join(st.root, L"c7_touched");
    string e2;
    double secs = 0;
    long rc = run_admit(st, L"run --kind compile --timeout 1 -- " + q(g_self) + L" __touch " + q(touched), L"c7c", &e2, &secs);
    c.need(rc == EXIT_REFUSED, fmt("refused rc %ld, want 75", rc));
    c.need(!is_file(touched), "a refused ask started the command");
    c.need(secs >= 0.9 && secs < 15.0, fmt("the refusal came after %.2f s (bound 1 s)", secs));
    c.need(e2.find("[admit] REFUSED after ") != string::npos && e2.find(" s: compile needs") != string::npos && e2.find("compile.0 pid ")!= string::npos && e2.find("reason slots") != string::npos,
           "the REFUSED line does not name the need, the holder and the reason: " + e2);
    finish(a);
    auto rows = hist_rows(sd);
    c.need(has_row(rows, C_OUTCOME, "refused") && !has_row(rows, C_OUTCOME, "timeout_run"), "history lacks the refused row");
    c.info = first_line(e2);
    report(st, c);
}
static void case8(ST& st) {
    Case c{"8 fail-open: unwritable state / unreadable config / a bad --timeout run the command with the ERROR line; WWE_NO_ADMIT=1 runs directly"};
    wstring afile = join(st.root, L"c8_a_file");
    write_file(afile, "x");
    SetEnvironmentVariableW(L"WWE_ADMIT_DIR", join(afile, L"sub").c_str());
    SetEnvironmentVariableW(L"WWE_ADMIT_CONFIG", write_cfg(st, L"c8", std_cfg(24, 60, 900)).c_str());
    string e;
    long rc = run_admit(st, L"run --kind compile -- cmd /c exit 5", L"c8a", &e);
    c.need(rc == 5, fmt("unwritable state: rc %ld", rc));
    c.need(e.find("[admit] ERROR") != string::npos && e.find("running without admission") != string::npos, "unwritable state: no ERROR line: " + e);
    string e1 = first_line(e);
    set_case(st, L"c8b", 1, 60, 900, "100 100");
    SetEnvironmentVariableW(L"WWE_ADMIT_CONFIG", join(st.root, L"missing.json").c_str());
    rc = run_admit(st, L"run --kind compile -- cmd /c exit 6", L"c8b", &e);
    c.need(rc == 6 && e.find("[admit] ERROR config") != string::npos, fmt("missing config: rc %ld, ", rc) + e);
    SetEnvironmentVariableW(L"WWE_ADMIT_CONFIG", g_cfg_path.c_str());
    rc = run_admit(st, L"run --kind compile --timeout nan -- cmd /c exit 8", L"c8n", &e);
    c.need(rc == 8 && e.find("[admit] ERROR bad value 'nan'") != string::npos, fmt("--timeout nan: rc %ld, ", rc) + e);
    WJ a = wait_job(st, L"c8h", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "holder never started");
    SetEnvironmentVariableW(L"WWE_NO_ADMIT", L"1");
    rc = run_admit(st, L"run --kind compile --timeout 20 -- cmd /c exit 4", L"c8c", &e);
    SetEnvironmentVariableW(L"WWE_NO_ADMIT", nullptr);
    c.need(rc == 4 && e.empty(), fmt("WWE_NO_ADMIT: rc %ld, stderr '%s'", rc, e.c_str()));
    finish(a);
    c.info = e1;
    report(st, c);
}
static void case9(ST& st) {
    Case c{"9 nested: a command inside an admitted job that calls admit again makes no second reservation"};
    wstring sd = set_case(st, L"c9", 1, 60, 900, "100 100");
    wstring cnt = join(st.root, L"c9_count.txt");
    string e;
    long rc = run_admit(st, L"run --kind compile --timeout 10 -- " + q(g_target) + L" run --kind compile --timeout 10 -- " +
                                q(g_self) + L" __count " + q(cnt),
                        L"c9", &e);
    string n = slurp(cnt);
    c.need(rc == 0, fmt("rc %ld (75 = the inner admit waited for the outer's slot)", rc));
    c.need(n == "1", "live holders seen inside: '" + n + "', want 1");
    c.need(has_row(hist_rows(sd), C_OUTCOME, "nested"), "no 'nested' history row");
    c.info = "holders inside = " + n;
    report(st, c);
}
// `<target> ARGS` with stdout on a pipe (as a PowerShell capture has it): rc, the text, and whether the pipe reached EOF once the front exited
static long run_piped(ST& st, const wstring& args, const wstring& tag, string& out, bool& eof) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE r = nullptr, w = nullptr;
    CreatePipe(&r, &w, &sa, 0);
    HANDLE e = out_file(join(st.root, tag + L".err"));
    P p = spawn(q(g_target) + L" " + args, nullptr, w, e);
    CloseHandle(w);
    CloseHandle(e);
    long rc = wait_exit(p, LONG_MS);
    out.clear();
    eof = false;
    double t1 = qpc_s();
    while (qpc_s() - t1 < 10.0) {
        DWORD av = 0;
        if (!PeekNamedPipe(r, nullptr, 0, nullptr, &av, nullptr)) { eof = GetLastError() == ERROR_BROKEN_PIPE; break; }
        if (av) {
            char buf[256];
            DWORD n = 0;
            if (ReadFile(r, buf, std::min<DWORD>(av, sizeof buf), &n, nullptr)) out.append(buf, n);
        } else {
            Sleep(20);
        }
    }
    CloseHandle(r);
    return rc;
}
static int granted_of(const string& out) {
    int m = -1;
    sscanf(out.c_str(), "granted %d", &m);
    return m;
}
static void case10(ST& st) {
    Case c{"10 hold --pid P --detach: the grant comes back while P lives; held until P exits; release ends it; a second hold of the same kind NESTS under the first, another kind reserves its own"};
    wstring sd = set_case(st, L"c10", 24, 60, 900, "100 100");
    WJ p1 = plain_wait(st);
    c.need(signalled(p1.started, LONG_MS), "P1 never started");
    string out;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind parse --want 3 --pid " + wnum(p1.p.pid) + L" --detach", L"c10a", out, eof);
    c.need(rc == 0 && granted_of(out) == 3 && out.find("\nmarker ") != string::npos, fmt("first hold: rc %ld, stdout '%s'", rc, out.c_str()));
    c.need(eof, "the caller's stdout pipe stayed open after the front returned (a PowerShell capture would hang)");
    c.need(WaitForSingleObject(p1.p.h, 0) == WAIT_TIMEOUT, "P1 ended before the front returned");
    c.need(live_count(sd) == 3, fmt("live %d after the first hold, want 3", live_count(sd)));
    rc = run_piped(st, L"hold --kind compile --want 1 --pid " + wnum(p1.p.pid) + L" --detach", L"c10k", out, eof);
    c.need(rc == 0 && granted_of(out) == 1 && live_count(sd) == 4, fmt("a compile hold for the same P: live %d, want 4 (a different kind reserves its own)", live_count(sd)));
    rc = run_piped(st, L"hold --kind parse --want 5 --pid " + wnum(p1.p.pid) + L" --detach", L"c10b", out, eof);
    c.need(rc == 0 && out.rfind("granted 3 nested", 0) == 0, fmt("second parse hold for the same P: rc %ld, stdout '%s' (want granted 3 nested: at most the first's)", rc, first_line(out).c_str()));
    c.need(live_count(sd) == 4, fmt("live %d after a second parse hold for the same P, want 4 (it reserves nothing)", live_count(sd)));
    SetEvent(p1.go);
    wait_exit(p1.p, LONG_MS);
    c.need(wait_live(sd, 0, LONG_MS), fmt("%d slots still held after P exited", live_count(sd)));
    CloseHandle(p1.started);
    CloseHandle(p1.go);
    WJ p2 = plain_wait(st);
    c.need(signalled(p2.started, LONG_MS), "P2 never started");
    rc = run_piped(st, L"hold --kind parse --want 2 --pid " + wnum(p2.p.pid) + L" --detach", L"c10c", out, eof);
    c.need(rc == 0 && granted_of(out) == 2 && live_count(sd) == 2, fmt("hold for P2: rc %ld, stdout '%s', live %d", rc, out.c_str(), live_count(sd)));
    rc = run_piped(st, L"release --pid " + wnum(p2.p.pid), L"c10r", out, eof);
    c.need(rc == 0 && out.find("released 1 hold(s)") != string::npos, fmt("release: rc %ld, '%s'", rc, out.c_str()));
    c.need(live_count(sd) == 0, fmt("live %d right after release, want 0", live_count(sd)));
    c.need(p2.p.h && WaitForSingleObject(p2.p.h, 0) == WAIT_TIMEOUT, "P2 is not alive after release (release must not touch P)");
    finish(p2);
    c.need(wait_row(sd, C_ENDED, "released", LONG_MS) && wait_row(sd, C_ENDED, "owner_exit", LONG_MS), "history lacks ended 'released' / 'owner_exit'");
    // as a build script does it: the owner itself takes two holds (no --pid: the owner is the front's parent); the second nests under the first
    wstring cnt = join(st.root, L"c10_count.txt");
    wstring script = q(g_target) + L" hold --kind parse --want 3 --detach >NUL & " + q(g_target) + L" hold --kind parse --want 2 --detach >NUL & " + q(g_self) +
                     L" __count " + q(cnt);
    P owner = spawn(L"cmd.exe /d /s /c \"" + script + L"\"", nullptr, nullptr, nullptr);
    long orc = wait_exit(owner, LONG_MS);
    c.need(orc == 0 && slurp(cnt) == "3", fmt("owner with two holds: rc %ld, live slots seen '%s', want 3 (the second nested in the first)", orc, slurp(cnt).c_str()));
    c.need(wait_live(sd, 0, LONG_MS), "the owner's hold outlived the owner");
    report(st, c);
}
static void case12(ST& st) {
    Case c{"12 interop with Python msvcrt.locking (the former Python scheduler's lock), both ways"};
    wstring sd = set_case(st, L"c12", 1, 60, 900, "100 100");
    c.need(!st.helper.empty(), "tools\\admit\\lock_interop.py not found above the exe");
    if (st.helper.empty()) { report(st, c); return; }
    wstring slot0 = join(sd, L"compile.0");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE inR, inW, outR, outW;
    CreatePipe(&inR, &inW, &sa, 0);
    CreatePipe(&outR, &outW, &sa, 0);
    HANDLE ef = out_file(join(st.root, L"c12py.err"));
    P py = spawn(L"py -I " + q(st.helper) + L" hold " + q(slot0), inR, outW, ef);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(ef);
    c.need(py.h != nullptr, "py did not start");
    string line = read_line_pipe(outR, LONG_MS);
    c.need(line == "locked", "python said '" + line + "' (stderr: " + slurp(join(st.root, L"c12py.err")) + ")");
    vector<Holder> hs;
    int n = live_count(sd, &hs);
    c.need(n == 1 && hs[0].label == "interop", fmt("admit sees %d live holders", n) + (n ? " (label " + hs[0].label + ")" : ""));
    string e;
    long rc = run_admit(st, L"run --kind compile --timeout 1 -- cmd /c exit 0", L"c12a", &e);
    c.need(rc == EXIT_REFUSED, fmt("admit took the slot python holds (rc %ld)", rc));
    CloseHandle(inW);
    c.need(wait_exit(py, LONG_MS) == 0, "python hold did not exit");
    CloseHandle(outR);
    c.need(wait_live(sd, 0, LONG_MS), "python's slot still live after it exited");
    WJ a = wait_job(st, L"c12h", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "admit holder never started");
    wstring pf = join(st.root, L"c12try.out");
    HANDLE o = out_file(pf);
    P t = spawn(L"py -I " + q(st.helper) + L" try " + q(slot0), nullptr, o, o);
    CloseHandle(o);
    wait_exit(t, LONG_MS);
    string busy = slurp(pf);
    c.need(busy.rfind("busy", 0) == 0, "python try on admit's slot said '" + busy + "'");
    finish(a);
    o = out_file(pf);
    t = spawn(L"py -I " + q(st.helper) + L" try " + q(slot0), nullptr, o, o);
    CloseHandle(o);
    wait_exit(t, LONG_MS);
    string fr = slurp(pf);
    c.need(fr.rfind("free", 0) == 0, "python try after admit exited said '" + fr + "'");
    report(st, c);
}
// 13: a holder older than the window stops counting as pending (planted twin: ages never expire)
static int case13_run(ST& st, const wchar_t* tag, const wchar_t* plant) {
    wstring sd = set_case(st, tag, 24, 2, 900, "7 7");
    if (plant) SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", plant);
    WJ a = wait_job(st, wstring(tag) + L"a", L"run --kind compile");
    signalled(a.started, LONG_MS);
    WJ b = wait_job(st, wstring(tag) + L"b", L"run --kind compile");
    bool bs = signalled(b.started, 15000);   // A's 2 s window passes while A still holds: B must get in well within 15 s
    int r = bs ? 1 : 0;
    finish(a);
    signalled(b.started, LONG_MS);
    finish(b);
    if (plant) SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", st.plant ? w16(st.plant_name).c_str() : nullptr);
    return r;
}
static void case13(ST& st) {
    Case c{"13 a holder older than the pending window stops counting as pending (window 2 s, A still holding)"};
    c.need(case13_run(st, L"c13", nullptr) == 1, "B was never admitted while A held, though A was older than the window");
    Case p{"13P planted fault (ages never expire): case 13 must FAIL"};
    p.need(case13_run(st, L"c13p", L"no_expiry") == 0, "case 13 still passed with expiry switched off: the check proves nothing");
    report(st, c);
    report(st, p);
}
static void case14(ST& st) {
    Case c{"14 after a normal exit nothing the child left is killed (a daemon and its compiles live on); peak_scope partial"};
    wstring sd = set_case(st, L"c14", 24, 60, 900, "100 100");
    auto [s, g] = ev_names(st);
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, s.c_str()), go = CreateEventW(nullptr, TRUE, FALSE, g.c_str());
    wstring pidf = join(st.root, L"c14.pid");
    string e;
    long rc = run_admit(st, L"run --kind compile -- " + q(g_self) + L" __parent -- " + q(g_self) + L" __wait --start " + s + L" --go " + g + L" --pidfile " + q(pidf), L"c14", &e);
    c.need(rc == 0, fmt("rc %ld", rc));
    c.need(signalled(started, LONG_MS), "the daemon never started (killed with the job?)");
    unsigned long dpid = strtoul(slurp(pidf).c_str(), nullptr, 10);
    HANDLE dh = dpid ? OpenProcess(SYNCHRONIZE, FALSE, dpid) : nullptr;
    c.need(dh && WaitForSingleObject(dh, 500) == WAIT_TIMEOUT, "the daemon died with admit's job after a normal exit");
    SetEvent(go);
    if (dh) { WaitForSingleObject(dh, LONG_MS); CloseHandle(dh); }
    auto rows = hist_rows(sd);
    c.need(rows.size() == 1 && rows[0][C_SCOPE] == "partial", "history scope '" + (rows.empty() ? string("?") : rows[0][C_SCOPE]) + "', want partial (processes left behind)");
    CloseHandle(started);
    CloseHandle(go);
    report(st, c);
}
static void case15(ST& st) {
    Case c{"15 a run inside a hold takes no second reservation: by the marker line, and by ancestry (the hold's --pid)"};
    wstring sd = set_case(st, L"c15", 1, 60, 900, "100 100");
    // (a) marker: the hold is started through an intermediate so it is NOT an ancestor of the run (ancestry cannot help)
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE inR, inW, outR, outW;
    CreatePipe(&inR, &inW, &sa, 0);
    CreatePipe(&outR, &outW, &sa, 0);
    auto [s0, g0] = ev_names(st);
    HANDLE go0 = CreateEventW(nullptr, TRUE, FALSE, g0.c_str());
    HANDLE ef = out_file(join(st.root, L"c15h.err"));
    P par = spawn(q(g_self) + L" __parent --go " + g0 + L" -- " + q(g_target) + L" hold --kind compile --want 1", inR, outW, ef);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(ef);
    string gl = read_line_pipe(outR, LONG_MS), ml = read_line_pipe(outR, LONG_MS);
    c.need(gl == "granted 1 held", "hold: '" + gl + "'");
    c.need(ml.rfind("marker ", 0) == 0, "no marker line: '" + ml + "'");
    string e;
    if (ml.rfind("marker ", 0) == 0) {
        SetEnvironmentVariableW(L"WWE_ADMIT_HELD", w16(ml.substr(7)).c_str());
        long rc = run_admit(st, L"run --kind compile --timeout 3 -- cmd /c exit 0", L"c15a", &e);
        SetEnvironmentVariableW(L"WWE_ADMIT_HELD", nullptr);
        c.need(rc == 0, fmt("marker route: rc %ld (75 = it waited for the hold's own slot)", rc));
    }
    SetEvent(go0);
    CloseHandle(inW);
    wait_exit(par, LONG_MS);
    CloseHandle(outR);
    CloseHandle(go0);
    c.need(wait_live(sd, 0, LONG_MS), "the marker-route hold did not end");
    // (b) ancestry: P holds (--pid P --detach), then P itself runs admit run
    auto [s1, g1] = ev_names(st);
    HANDLE go1 = CreateEventW(nullptr, TRUE, FALSE, g1.c_str());
    wstring cnt = join(st.root, L"c15_count.txt");
    HANDLE ef2 = out_file(join(st.root, L"c15p.err"));
    P p = spawn(q(g_self) + L" __later --go " + g1 + L" -- " + q(g_target) + L" run --kind compile --timeout 3 -- " + q(g_self) + L" __count " + q(cnt),
                nullptr, ef2, ef2);
    CloseHandle(ef2);
    string out;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind compile --want 1 --pid " + wnum(p.pid) + L" --detach", L"c15d", out, eof);
    c.need(rc == 0 && granted_of(out) == 1, fmt("detached hold: rc %ld '%s'", rc, out.c_str()));
    SetEvent(go1);
    long prc = wait_exit(p, LONG_MS);
    c.need(prc == 0, fmt("ancestry route: the run inside P returned %ld (75 = it waited for P's own slot)", prc));
    c.need(slurp(cnt) == "1", "holders seen inside: '" + slurp(cnt) + "', want 1 (the hold only)");
    c.need(wait_live(sd, 0, LONG_MS), "P's hold did not end when P exited");
    auto rows = hist_rows(sd);
    c.need(has_row(rows, C_OUTCOME, "nested") && has_row(rows, C_OUTCOME, "nested_hold"), "history lacks 'nested' (marker) / 'nested_hold' (ancestry)");
    CloseHandle(go1);
    report(st, c);
}
static void case16(ST& st) {
    Case c{"16 a .cmd called by its bare name runs (PATHEXT, cmd /d /s /c), arguments and exit code intact"};
    wstring sd = set_case(st, L"c16", 24, 60, 900, "100 100");
    wstring bin = join(st.root, L"bin16");
    make_dirs(bin);
    wstring outf = join(st.root, L"c16_out.txt");
    write_file(join(bin, L"zzadmit16.cmd"), "@echo %*> \"" + u8(outf) + "\"\r\n@exit /b 9\r\n");
    wstring old_path = getenv_w(L"PATH");
    SetEnvironmentVariableW(L"PATH", (bin + L";" + old_path).c_str());
    string e;
    long rc = run_admit(st, L"run --kind compile -- zzadmit16 a \"b c\"", L"c16", &e);
    SetEnvironmentVariableW(L"PATH", old_path.c_str());
    string got = slurp(outf);
    while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
    c.need(rc == 9, fmt("rc %ld, want 9 (127 = it could not start)", rc) + (e.empty() ? "" : "; " + first_line(e)));
    c.need(got == "a \"b c\"", "the script saw '" + got + "'");
    c.need(e.find("cannot start") == string::npos, "a 'cannot start' line (the .cmd must be found, not retried): " + first_line(e));
    report(st, c);
}
static void case17(ST& st, bool token) {
    Case c{token ? "17b a waiter holding the run lock (WWE2K14_RUNLOCK_TOKEN) is senior at once: a small job does not overtake it"
                 : "17 a senior waiter (waited > starve_s) is not overtaken by a small job"};
    wstring sd = set_case_cfg(st, token ? L"c17b" : L"c17", std_cfg(24, 60, 900, token ? 600 : 1), "16 16");
    WJ a = wait_job(st, token ? L"c17ba" : L"c17a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    if (token) SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", L"selftest");
    WJ b = wait_job(st, token ? L"c17bb" : L"c17b", L"run --kind big");
    if (token) SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", nullptr);
    int wb = wait_text_or(b.err, "[admit] waiting:", b.started, LONG_MS);   // printed at 2 s: by then B is senior (starve 1 s / the token)
    c.need(wb == 1, "big job B did not wait for A");
    WJ cj = wait_job(st, token ? L"c17bc" : L"c17c", L"run --kind compile");
    int wc = wait_text_or(cj.err, "senior waiter", cj.started, LONG_MS);
    c.need(wc == 1, wc == 2 ? "small job C overtook the senior waiter B" : "C's line does not name the senior waiter");
    finish(a);
    c.need(signalled(b.started, LONG_MS), "B not admitted after A exited");
    c.need(WaitForSingleObject(cj.started, 0) == WAIT_TIMEOUT, "C was admitted before B");
    finish(b);
    c.need(signalled(cj.started, LONG_MS), "C not admitted after B exited");
    finish(cj);
    c.info = "C said: " + first_line(slurp(cj.err));
    report(st, c);
}
static void case18(ST& st) {
    Case c{"18 a busy machine lock is waited out (no ERROR, no unadmitted start)"};
    wstring sd = set_case(st, L"c18", 24, 60, 900, "100 100");
    HANDLE lk = CreateFileW(join(sd, L"admit.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    c.need(lk != INVALID_HANDLE_VALUE && try_lock0(lk), "could not take the machine lock for the test");
    wstring touched = join(st.root, L"c18_touched"), ef = join(st.root, L"c18.err");
    HANDLE e = out_file(ef);
    P p = spawn(q(g_target) + L" run --kind compile --timeout 120 -- " + q(g_self) + L" __touch " + q(touched), nullptr, e, e);
    CloseHandle(e);
    int w = wait_text_or(ef, "machine lock", nullptr, LONG_MS);   // the waiting line comes at >= 2 s: at least one 1 s lock try has timed out
    string said = slurp(ef);
    c.need(w == 1 && said.find("ERROR") == string::npos, "while the lock was held admit said: '" + first_line(said) + "' (want a waiting line, no ERROR)");
    c.need(!is_file(touched), "the command ran while the machine lock was held");
    unlock0(lk);
    CloseHandle(lk);
    long rc = wait_exit(p, LONG_MS);
    c.need(rc == 0 && is_file(touched), fmt("after the lock was freed: rc %ld, ran %d", rc, is_file(touched) ? 1 : 0));
    c.need(slurp(ef).find("ERROR") == string::npos, "an ERROR line: " + slurp(ef));
    c.info = first_line(said);
    report(st, c);
}
static void case19(ST& st) {
    Case c{"19 stdin-form hold with stdin = NUL keeps its reservation until the parent exits (and says so)"};
    wstring sd = set_case(st, L"c19", 24, 60, 900, "100 100");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE outR, outW;
    CreatePipe(&outR, &outW, &sa, 0);
    auto [s, g] = ev_names(st);
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, g.c_str());
    wstring ef = join(st.root, L"c19.err");
    HANDLE e = out_file(ef);
    P par = spawn(q(g_self) + L" __parent --go " + g + L" -- " + q(g_target) + L" hold --kind parse --want 1", nullptr, outW, e);
    CloseHandle(outW);
    CloseHandle(e);
    string line = read_line_pipe(outR, LONG_MS);
    c.need(line == "granted 1 held", "hold: '" + line + "'");
    c.need(wait_text_or(ef, "stdin is not a pipe", nullptr, LONG_MS) == 1, "no 'stdin is not a pipe' line: " + slurp(ef));
    c.need(live_count(sd) == 1, fmt("live %d with the parent alive, want 1", live_count(sd)));
    TerminateProcess(par.h, 1);
    wait_exit(par, LONG_MS);
    c.need(wait_live(sd, 0, LONG_MS), "still held after the parent died");
    CloseHandle(outR);
    CloseHandle(go);
    report(st, c);
}
static void case20(ST& st) {
    Case c{"20 a detached hold ends at --max-s while P lives, and history says so"};
    wstring sd = set_case(st, L"c20", 24, 60, 900, "100 100");
    WJ p = plain_wait(st);
    c.need(signalled(p.started, LONG_MS), "P never started");
    string out;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind parse --want 2 --pid " + wnum(p.p.pid) + L" --detach --max-s 2", L"c20", out, eof);
    c.need(rc == 0 && granted_of(out) == 2, fmt("hold: rc %ld '%s'", rc, out.c_str()));
    c.need(slurp(join(st.root, L"c20.err")).find("ERROR") == string::npos, "an ERROR line: " + slurp(join(st.root, L"c20.err")));
    c.need(wait_row(sd, C_ENDED, "max_s", LONG_MS), "no history row ended 'max_s'");
    c.need(live_count(sd) == 0, fmt("live %d after max-s", live_count(sd)));
    c.need(WaitForSingleObject(p.p.h, 0) == WAIT_TIMEOUT, "P ended (the expiry must not depend on P)");
    finish(p);
    report(st, c);
}
static void case21(ST& st) {
    Case c{"21 WWE_NO_ADMIT=1 and a nested run start the child plainly: no job of admit's own"};
    set_case(st, L"c21", 24, 60, 900, "100 100");
    wstring f1 = join(st.root, L"c21_off.txt"), f2 = join(st.root, L"c21_nested.txt"), f3 = join(st.root, L"c21_admitted.txt");
    SetEnvironmentVariableW(L"WWE_NO_ADMIT", L"1");
    long rc = run_admit(st, L"run --kind compile -- " + q(g_self) + L" __jobinfo " + q(f1), L"c21a");
    SetEnvironmentVariableW(L"WWE_NO_ADMIT", nullptr);
    SetEnvironmentVariableW(L"WWE_ADMIT_HELD", held_marker("compile").c_str());   // the selftest itself is a live "holder"
    long rc2 = run_admit(st, L"run --kind compile -- " + q(g_self) + L" __jobinfo " + q(f2), L"c21b");
    SetEnvironmentVariableW(L"WWE_ADMIT_HELD", nullptr);
    long rc3 = run_admit(st, L"run --kind compile -- " + q(g_self) + L" __jobinfo " + q(f3), L"c21c");
    c.need(rc == 0 && slurp(f1).rfind("parent_in_job=1", 0) == 0, "WWE_NO_ADMIT: the child is in a job of admit's own (" + slurp(f1) + ")");
    c.need(rc2 == 0 && slurp(f2).rfind("parent_in_job=1", 0) == 0, "nested: the child is in a job of admit's own (" + slurp(f2) + ")");
    c.need(rc3 == 0 && slurp(f3).rfind("parent_in_job=0", 0) == 0, "control: an admitted child should be in admit's own job (" + slurp(f3) + ")");
    report(st, c);
}
// 22: two seniors with the SAME start tick (forced) resolve by pid; planted twin (no pid tie-break, yield to any senior) freezes
static bool case22_run(ST& st, const wchar_t* tag, const wchar_t* plant) {
    set_case_cfg(st, tag, std_cfg(24, 60, 900, 600), "7 7");
    if (plant) SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", plant);
    WJ a = wait_job(st, wstring(tag) + L"a", L"run --kind compile");
    signalled(a.started, LONG_MS);
    SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", L"selftest");   // both senior at once (a run-lock build's compiles all inherit the token)
    SetEnvironmentVariableW(L"WWE_ADMIT_TEST_TICK0", L"12345");       // and both on one tick
    WJ b = wait_job(st, wstring(tag) + L"b", L"run --kind compile");
    WJ cj = wait_job(st, wstring(tag) + L"c", L"run --kind compile");
    SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", nullptr);
    SetEnvironmentVariableW(L"WWE_ADMIT_TEST_TICK0", nullptr);
    wait_text_or(b.err, "[admit] waiting:", b.started, LONG_MS);
    wait_text_or(cj.err, "[admit] waiting:", cj.started, LONG_MS);
    finish(a);
    HANDLE both[2] = {b.started, cj.started};
    bool one = WaitForMultipleObjects(2, both, FALSE, 15000) < WAIT_OBJECT_0 + 2;   // A is gone: one of the two must get in
    if (plant) SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", st.plant ? w16(st.plant_name).c_str() : nullptr);
    finish(b);
    finish(cj);
    return one;
}
static void case22(ST& st) {
    Case c{"22 two seniors on the same tick do not give way to each other forever (pid tie-break)"};
    c.need(case22_run(st, L"c22", nullptr), "neither senior was admitted within 15 s after the holder left (the queue froze)");
    Case p{"22P planted fault (no pid tie-break, yield to any senior): case 22 must FAIL"};
    p.need(!case22_run(st, L"c22p", L"no_tiebreak"), "case 22 still passed with the tie-break removed: the check proves nothing");
    report(st, c);
    report(st, p);
}
static void case23(ST& st) {
    Case c{"23 a senior refused on SLOTS does not stall another kind"};
    set_case_cfg(st, L"c23", std_cfg(24, 60, 900, 1), "100 100");
    WJ a = wait_job(st, L"c23a", L"run --kind link");
    c.need(signalled(a.started, LONG_MS), "link A never started");
    WJ l2 = wait_job(st, L"c23l", L"run --kind link");
    c.need(wait_text_or(l2.err, "all 1 link slots held", l2.started, LONG_MS) == 1, "the second link did not wait for the slot");
    WJ cc = wait_job(st, L"c23c", L"run --kind compile");
    c.need(signalled(cc.started, 15000), "a compile was stalled behind a link waiting for a SLOT: " + first_line(slurp(cc.err)));
    finish(cc);
    finish(a);
    signalled(l2.started, LONG_MS);
    finish(l2);
    report(st, c);
}
static void case24(ST& st) {
    Case c{"24 a senior whose need can never fit this PC stalls nobody (and says so once)"};
    set_case_cfg(st, L"c24", std_cfg(24, 60, 900, 1), "100 100");
    WJ a = wait_job(st, L"c24a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    WJ h = wait_job(st, L"c24h", L"run --kind huge");
    c.need(wait_text_or(h.err, "nobody will wait for it", h.started, LONG_MS) == 1, "the huge job did not say it can never fit");
    wait_text_or(h.err, "[admit] waiting:", h.started, LONG_MS);   // by now it is senior (starve 1 s)
    WJ cc = wait_job(st, L"c24c", L"run --kind compile");
    c.need(signalled(cc.started, 15000), "a compile was stalled behind a job that can never fit: " + first_line(slurp(cc.err)));
    c.need(count_of(slurp(h.err), "nobody will wait for it") == 1, "the can-never-fit line was not said exactly once");
    finish(cc);
    finish(a);
    signalled(h.started, LONG_MS);   // alone, it is admitted at once (a lone job always is)
    finish(h);
    report(st, c);
}
static void case25(ST& st) {
    Case c{"25 a younger job that would cut into a memory-refused senior's need gives way; one that would not is admitted"};
    set_case_cfg(st, L"c25", std_cfg(24, 60, 900, 1), "30 16");   // plenty of physical memory, commit short for the senior
    WJ a = wait_job(st, L"c25a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    WJ s = wait_job(st, L"c25s", L"run --kind big");
    c.need(wait_text_or(s.err, "[admit] waiting:", s.started, LONG_MS) == 1, "the senior (big, commit 11) did not wait");
    WJ y1 = wait_job(st, L"c25y1", L"run --kind compile");   // takes 2.5 GB of the commit the senior is short of
    c.need(wait_text_or(y1.err, "senior waiter", y1.started, LONG_MS) == 1, "the younger job that cuts into the senior's commit did not give way");
    WJ y2 = wait_job(st, L"c25y2", L"run --kind compile --reserve-gb 1 --reserve-commit-gb 0");   // takes no commit: leaves the senior its need
    c.need(signalled(y2.started, 15000), "a younger job that takes nothing the senior is short of was held back: " + first_line(slurp(y2.err)));
    c.need(WaitForSingleObject(y1.started, 0) == WAIT_TIMEOUT, "the cutting job was admitted before the senior");
    finish(y2);
    finish(a);
    c.need(signalled(s.started, LONG_MS), "the senior was not admitted after A left");
    finish(s);
    signalled(y1.started, LONG_MS);
    finish(y1);
    report(st, c);
}
static void case26(ST& st) {
    Case c{"26 a marker of a kind that does not cover the run is not 'nested'"};
    wstring sd = set_case(st, L"c26", 24, 60, 900, "100 100");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE inR, inW, outR, outW;
    CreatePipe(&inR, &inW, &sa, 0);
    CreatePipe(&outR, &outW, &sa, 0);
    auto [s0, g0] = ev_names(st);
    HANDLE go0 = CreateEventW(nullptr, TRUE, FALSE, g0.c_str());
    HANDLE ef = out_file(join(st.root, L"c26h.err"));
    P par = spawn(q(g_self) + L" __parent --go " + g0 + L" -- " + q(g_target) + L" hold --kind parse --want 1", inR, outW, ef);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(ef);
    string gl = read_line_pipe(outR, LONG_MS), ml = read_line_pipe(outR, LONG_MS);
    c.need(ml.rfind("marker ", 0) == 0, "no marker line from the parse hold: '" + ml + "'");
    if (ml.rfind("marker ", 0) == 0) {
        SetEnvironmentVariableW(L"WWE_ADMIT_HELD", w16(ml.substr(7)).c_str());
        long rc = run_admit(st, L"run --kind compile -- cmd /c exit 0", L"c26r");
        SetEnvironmentVariableW(L"WWE_ADMIT_HELD", nullptr);
        auto rows = hist_rows(sd);
        string outc = rows.empty() ? "?" : rows.back()[C_OUTCOME];
        c.need(rc == 0 && outc == "admitted", "a compile run under a parse marker: outcome '" + outc + "', want admitted (its own reservation)");
    }
    SetEvent(go0);
    CloseHandle(inW);
    wait_exit(par, LONG_MS);
    CloseHandle(outR);
    CloseHandle(go0);
    report(st, c);
}
static void case27(ST& st) {
    Case c{"27 an orphaned waiter (its caller is gone) leaves without running"};
    wstring sd = set_case(st, L"c27", 1, 60, 900, "100 100");
    WJ a = wait_job(st, L"c27a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    wstring touched = join(st.root, L"c27_touched"), ef = join(st.root, L"c27.err");
    HANDLE e = out_file(ef);
    auto [s0, g0] = ev_names(st);
    HANDLE go0 = CreateEventW(nullptr, TRUE, FALSE, g0.c_str());
    P par = spawn(q(g_self) + L" __parent --go " + g0 + L" -- " + q(g_target) + L" run --kind compile --timeout 120 -- " + q(g_self) + L" __touch " + q(touched),
                  nullptr, e, e);
    CloseHandle(e);
    c.need(wait_text_or(ef, "[admit] waiting:", nullptr, LONG_MS) == 1, "the run never waited");
    TerminateProcess(par.h, 1);   // the caller is cancelled
    wait_exit(par, LONG_MS);
    c.need(wait_row(sd, C_OUTCOME, "orphaned", LONG_MS), "no 'orphaned' history row: the waiter did not notice its caller was gone");
    finish(a);   // the slot frees: an orphan that were still waiting would run now
    c.need(wait_text_or(ef, "ORPHANED", nullptr, 2000) == 1 && !is_file(touched), "the orphan ran its command, or did not say ORPHANED");
    CloseHandle(go0);
    report(st, c);
}
static void case28(ST& st) {
    // S106 0C: the default state is the MAIN checkout's out\harness\admit (one folder for every tool and worktree); the LocalAppData folder is the last fallback only
    Case c{"28 with LOCALAPPDATA unset the state folder is still <main checkout>\\out\\harness\\admit (LocalAppData only for an exe outside any repo)"};
    static const GUID kLocalAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
    PWSTR p = nullptr;
    wstring want;
    wstring mroot = main_checkout_of(dir_of(g_target));
    {   // S106 0CF (review F11): the expectation comes from git itself (`--git-common-dir`), not from main_checkout_of again; skipped when git is not available
        FILE* gp = _wpopen((L"git -C \"" + dir_of(g_target) + L"\" rev-parse --path-format=absolute --git-common-dir 2>nul").c_str(), L"r");
        char gb[2048] = {0};
        if (gp) {
            if (fgets(gb, sizeof gb, gp)) {
                string gl = gb;
                while (!gl.empty() && (gl.back() == '\n' || gl.back() == '\r' || gl.back() == ' ')) gl.pop_back();
                wstring gw = w16(gl);
                for (auto& ch : gw) if (ch == L'/') ch = L'\\';
                if (gw.size() > 5 && _wcsicmp(gw.c_str() + gw.size() - 5, L"\\.git") == 0) {
                    wstring gm = gw.substr(0, gw.size() - 5);
                    if (!gm.empty() && canon(gm) != canon(mroot)) c.need(false, "main_checkout_of said " + u8(mroot) + " but git's common dir says " + u8(gm));
                }
            }
            _pclose(gp);
        }
    }
    if (!mroot.empty()) want = join(join(join(mroot, L"out"), L"harness"), L"admit");
    else if (SUCCEEDED(SHGetKnownFolderPath(kLocalAppData, 0, nullptr, &p)) && p) want = join(p, L"wwe2k14-admit");
    if (p) CoTaskMemFree(p);
    wstring la = getenv_w(L"LOCALAPPDATA"), wd = getenv_w(L"WWE_ADMIT_DIR");
    SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
    SetEnvironmentVariableW(L"WWE_ADMIT_DIR", nullptr);
    string out;
    run_admit(st, L"status", L"c28", &out);
    SetEnvironmentVariableW(L"LOCALAPPDATA", la.c_str());
    SetEnvironmentVariableW(L"WWE_ADMIT_DIR", wd.c_str());
    c.need(!want.empty() && out.find("state    " + u8(want) + "\n") != string::npos, "status said: " + out.substr(out.find("state"), out.find('\n', out.find("state")) - out.find("state")));
    report(st, c);
}
static void case29(ST& st) {
    Case c{"29 a program that is not found runs nothing (no cmd retry) and returns 9009"};
    set_case(st, L"c29", 24, 60, 900, "100 100");
    wstring touched = join(st.root, L"c29_touched");
    string e;
    long rc = run_admit(st, L"run --kind compile -- zz_admit_nope_29 x & " + q(g_self) + L" __touch " + q(touched), L"c29", &e);
    c.need(rc == 9009, fmt("rc %ld, want 9009", rc));
    c.need(!is_file(touched), "the data after '&' was run as a command (a cmd retry)");
    c.need(e.find("nothing run") != string::npos && e.find("through cmd") == string::npos, "stderr: " + first_line(e));
    c.info = first_line(e);
    report(st, c);
}
// a stdin-form hold started by the selftest itself: returns the process, the write end of its stdin (close to release) and its two lines
struct SH {
    P p;
    HANDLE inW = nullptr, outR = nullptr;
    string granted, marker;
};
static SH stdin_hold(ST& st, const wstring& args, const wstring& tag) {
    SH h;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE inR, outW;
    CreatePipe(&inR, &h.inW, &sa, 0);
    CreatePipe(&h.outR, &outW, &sa, 0);
    HANDLE ef = out_file(join(st.root, tag + L".err"));
    h.p = spawn(q(g_target) + L" " + args, inR, outW, ef);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(ef);
    h.granted = read_line_pipe(h.outR, LONG_MS);
    h.marker = read_line_pipe(h.outR, 2000);
    return h;
}
static long stdin_hold_end(SH& h) {
    CloseHandle(h.inW);
    long rc = wait_exit(h.p, LONG_MS);
    CloseHandle(h.outR);
    return rc;
}
static void case30(ST& st) {
    Case c{"30 a bad config key does not open the queue: unknown keys warn and are ignored, a bad kind is dropped alone; only an unreadable file opens it (grants say 'open')"};
    wstring sd = set_case(st, L"c30", 24, 60, 900, "100 100");
    string bad = std_cfg(24, 60, 900);
    bad.replace(bad.find("\"reserve_phys_gb\": 2.0"), 22, "\"reserve_phys_GB\": 2.0");   // a typo inside a kind
    bad.replace(bad.find("\"parse\""), 7, "\"pa rse\"");                                  // a bad kind name
    bad.replace(bad.find("\"floor_phys_gb\""), 15, "\"colour\": 1, \"floor_phys_gb\"");     // an unknown top-level key
    string e;
    SetEnvironmentVariableW(L"WWE_ADMIT_CONFIG", write_cfg(st, L"c30bad", bad).c_str());
    long rc = run_admit(st, L"run --kind compile -- cmd /c exit 3", L"c30a", &e);
    auto rows = hist_rows(sd);
    c.need(rc == 3 && !rows.empty() && rows.back()[C_OUTCOME] == "admitted", "the queue was opened by a bad key: '" + first_line(e) + "'");
    c.need(e.find("reserve_phys_GB") != string::npos && e.find("colour") != string::npos && e.find("pa rse") != string::npos && e.find("running without admission") == string::npos,
           "the warnings do not name the three keys: " + e);
    rc = run_admit(st, L"run --kind parse -- cmd /c exit 0", L"c30b", &e);   // the dropped kind: default_kind with a warning
    rows = hist_rows(sd);
    c.need(rc == 0 && e.find("warning: unknown kind 'parse'") != string::npos && rows.back()[C_KIND] == "compile" && rows.back()[C_OUTCOME] == "admitted",
           "asking for the dropped kind: '" + e + "'");
    SetEnvironmentVariableW(L"WWE_ADMIT_CONFIG", join(st.root, L"no_such_config.json").c_str());   // unreadable: the one case that opens the queue
    SH h = stdin_hold(st, L"hold --kind compile --want 3", L"c30h");
    stdin_hold_end(h);
    c.need(h.granted == "granted 3 open", "an unreadable config: the hold said '" + h.granted + "', want 'granted 3 open'");
    SetEnvironmentVariableW(L"WWE_ADMIT_CONFIG", g_cfg_path.c_str());
    c.info = first_line(slurp(join(st.root, L"c30a.err")));
    report(st, c);
}
static void case31(ST& st) {
    Case c{"31 a huge --want is clamped, not undefined: granted as many as the slots allow"};
    set_case(st, L"c31", 24, 60, 900, "100 100");
    WJ p = plain_wait(st);
    c.need(signalled(p.started, LONG_MS), "P never started");
    string out;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind parse --want 1e12 --pid " + wnum(p.p.pid) + L" --detach", L"c31", out, eof);
    c.need(rc == 0 && granted_of(out) == 16, fmt("--want 1e12: rc %ld, '%s' (want granted 16: the parse slots)", rc, first_line(out).c_str()));
    finish(p);
    report(st, c);
}
static void case32(ST& st) {
    Case c{"32 one process takes two stdin-form holds (for two children): both reserve, neither is nested by ancestry"};
    wstring sd = set_case(st, L"c32", 24, 60, 900, "100 100");
    SH h1 = stdin_hold(st, L"hold --kind parse --want 2", L"c32a");
    c.need(h1.granted == "granted 2 held", "first hold: '" + h1.granted + "'");
    Sleep(1000);   // the second is taken a second later, as a caller's next check would
    SH h2 = stdin_hold(st, L"hold --kind parse --want 3", L"c32b");
    c.need(h2.granted == "granted 3 held", "second hold: '" + h2.granted + "'");
    c.need(live_count(sd) == 5, fmt("live slots %d, want 5 (2 + 3: both reserved)", live_count(sd)));
    stdin_hold_end(h1);
    stdin_hold_end(h2);
    c.need(wait_row(sd, C_OUTCOME, "admitted", LONG_MS) && !has_row(hist_rows(sd), C_OUTCOME, "nested_hold") && !has_row(hist_rows(sd), C_OUTCOME, "nested"),
           "a hold was recorded as nested");
    report(st, c);
}
static void case33(ST& st) {
    Case c{"33 a nested --pid hold wanting 16 under a covering hold of 8 is granted 8 (no new reservation)"};
    wstring sd = set_case(st, L"c33", 24, 60, 900, "100 100");
    auto [s0, g0] = ev_names(st);
    HANDLE go0 = CreateEventW(nullptr, TRUE, FALSE, g0.c_str());
    wstring outf = join(st.root, L"c33_inner.txt");
    // P waits, then (through cmd, which stays alive while the inner front runs) takes a 16-wide hold for its own child process tree
    P p = spawn(q(g_self) + L" __later --go " + g0 + L" -- cmd.exe /d /s /c \"" + q(g_target) + L" hold --kind parse --want 16 --detach > " + q(outf) + L"\"",
                nullptr, nullptr, nullptr);
    string out;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind parse --want 8 --pid " + wnum(p.pid) + L" --detach", L"c33o", out, eof);
    c.need(rc == 0 && granted_of(out) == 8, fmt("covering hold: rc %ld '%s'", rc, first_line(out).c_str()));
    SetEvent(go0);
    long prc = wait_exit(p, LONG_MS);
    string inner = slurp(outf);
    c.need(prc == 0 && granted_of(inner) == 8, fmt("inner hold: rc %ld, '%s' (want granted 8)", prc, first_line(inner).c_str()));
    c.need(wait_live(sd, 0, LONG_MS), "slots still held after P exited");
    c.need(wait_row(sd, C_OUTCOME, "nested_hold", LONG_MS), "no 'nested_hold' history row");
    CloseHandle(go0);
    report(st, c);
}
static void case35(ST& st) {
    Case c{"35 hold --min 4: room for 3 waits, room for 5 grants 5 (never fewer than the minimum)"};
    wstring sd = set_case(st, L"c35", 24, 60, 900, "13.5 13.5");   // with A's 1.7 pending: (13.5 - 1.7 - 4) / 2.5 commit = 3 compile workers
    WJ a = wait_job(st, L"c35a", L"run --kind parse");
    c.need(signalled(a.started, LONG_MS), "A never started");
    WJ p = plain_wait(st);
    signalled(p.started, LONG_MS);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE r = nullptr, w = nullptr;
    CreatePipe(&r, &w, &sa, 0);
    wstring ef = join(st.root, L"c35.err");
    HANDLE e = out_file(ef);
    P f = spawn(q(g_target) + L" hold --kind compile --want 8 --min 4 --pid " + wnum(p.p.pid) + L" --detach", nullptr, w, e);
    CloseHandle(w);
    CloseHandle(e);
    int wl = wait_text_or(ef, "[admit] waiting:", f.h, LONG_MS);   // f.h is signalled if the front returned (granted early)
    c.need(wl == 1, "with room for 3 the hold did not wait (min 4)");
    set_fake(sd, "18.5 18.5");   // (18.5 - 1.7 - 4) / 2.5 = 5
    string line = read_line_pipe(r, LONG_MS);
    c.need(line == "granted 5 held", "after room for 5 appeared: '" + line + "'");
    wait_exit(f, LONG_MS);
    CloseHandle(r);
    finish(p);
    finish(a);
    report(st, c);
}
static string last_row_col(const wstring& sd, int col) {
    auto rows = hist_rows(sd);
    return rows.empty() ? "?" : rows.back()[col];
}
static void case36(ST& st) {
    Case c{"36 observe mode never waits: a run enforce would refuse (slots) starts at once; a hold enforce would refuse (memory) gets its full want; both log would_grant 0 + the reason"};
    wstring sd = set_case(st, L"c36", 1, 60, 900, "100 100");
    WJ a = wait_job(st, L"c36a", L"run --kind compile");   // enforce: holds the only compile slot
    c.need(signalled(a.started, LONG_MS), "A never started");
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"observe");
    string e;
    long rc = run_admit(st, L"run --kind compile --timeout 3 -- cmd /c exit 0", L"c36r", &e);
    c.need(rc == 0, fmt("observed run: rc %ld (75 = it waited)", rc));
    c.need(last_row_col(sd, C_OUTCOME) == "observed" && last_row_col(sd, C_QMODE) == "observe" && last_row_col(sd, C_WOULD) == "0" && last_row_col(sd, C_WREASON) == "slots",
           "run row: outcome '" + last_row_col(sd, C_OUTCOME) + "' queue_mode '" + last_row_col(sd, C_QMODE) + "' would '" + last_row_col(sd, C_WOULD) + "' reason '" +
               last_row_col(sd, C_WREASON) + "'");
    set_fake(sd, "5 5");   // A's run is pending (2 / 2.5): nothing fits
    SH h = stdin_hold(st, L"hold --kind parse --want 6 --min 2 --timeout 2", L"c36h");
    c.need(h.granted == "granted 6 observed", "observed hold: '" + h.granted + "' (want the full 6, observed)");
    stdin_hold_end(h);
    wait_row(sd, C_MODE, "hold", LONG_MS);   // both memory and commit are short here: either reason is right
    c.need(last_row_col(sd, C_WOULD) == "0" && (last_row_col(sd, C_WREASON) == "memory" || last_row_col(sd, C_WREASON) == "commit"),
           "hold row: would '" + last_row_col(sd, C_WOULD) + "' reason '" + last_row_col(sd, C_WREASON) + "'");
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"enforce");
    finish(a);
    report(st, c);
}
static void case37(ST& st) {
    Case c{"37 a hold's reservation counts for its whole life (not just the 1 s window here): a second hold is refused in enforce and logs would_grant 0 in observe"};
    wstring sd = set_case(st, L"c37", 24, 1, 900, "16 16");
    WJ p1 = plain_wait(st), p2 = plain_wait(st), p3 = plain_wait(st);
    signalled(p1.started, LONG_MS);
    signalled(p2.started, LONG_MS);
    signalled(p3.started, LONG_MS);
    string out;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind parse --want 7 --pid " + wnum(p1.p.pid) + L" --detach", L"c37a", out, eof);   // 7 x 1.7 = 11.9 of 16 - 4
    c.need(rc == 0 && out.rfind("granted 7 held", 0) == 0, "first hold: '" + first_line(out) + "'");
    Sleep(2000);   // the first hold is now older than the 1 s pending window
    rc = run_piped(st, L"hold --kind parse --want 8 --timeout 2 --pid " + wnum(p2.p.pid) + L" --detach", L"c37b", out, eof);   // refused = it waits its 2 s, then exit 75 (S109 GATE: never forced; 0.1 GB is left above the floor)
    c.need(rc == EXIT_REFUSED && out.rfind("refused REFUSED after ", 0) == 0,
           fmt("enforce: the second hold was admitted (rc %ld, '%s'): the first one's 11.9 GB stopped counting after the window", rc, first_line(out).c_str()));
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"observe");
    rc = run_piped(st, L"hold --kind parse --want 8 --pid " + wnum(p3.p.pid) + L" --detach", L"c37c", out, eof);
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"enforce");
    c.need(rc == 0 && out.rfind("granted 8 observed", 0) == 0, "observe: '" + first_line(out) + "'");
    auto rows = hist_rows(sd);   // the observed hold's row is written when it ends: end it
    finish(p3);
    c.need(wait_row(sd, C_QMODE, "observe", LONG_MS), "no observe row");
    rows = hist_rows(sd);
    bool found = false;
    for (auto& r : rows)
        if (r[C_QMODE] == "observe") found = r[C_WOULD] == "0" && (r[C_WREASON] == "memory" || r[C_WREASON] == "commit");
    c.need(found, "the observed hold did not log would_grant 0 / memory or commit");
    finish(p1);
    finish(p2);
    report(st, c);
}
static void case38(ST& st) {
    Case c{"38 a large-want pool that does not cut a senior at its minimum is capped so the senior's need + floor stays free"};
    set_case_cfg(st, L"c38", std_cfg(24, 60, 900, 1), "30 16");   // the senior (big 11 / 11) is short of commit
    WJ a = wait_job(st, L"c38a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    WJ s = wait_job(st, L"c38s", L"run --kind big");
    c.need(wait_text_or(s.err, "[admit] waiting:", s.started, LONG_MS) == 1, "the senior did not wait");
    WJ p = plain_wait(st);
    signalled(p.started, LONG_MS);
    string out;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind parse --want 16 --per-gb 1 --per-commit-gb 0 --pid " + wnum(p.p.pid) + L" --detach", L"c38y", out, eof);
    c.need(rc == 0 && granted_of(out) == 13, fmt("the pool: rc %ld, '%s' (want 13: 30 - 2 pending - 11 senior - 4 floor; its budget was 16)", rc, first_line(out).c_str()));
    finish(p);
    finish(a);
    signalled(s.started, LONG_MS);
    finish(s);
    report(st, c);
}
static void case39(ST& st) {
    Case c{"39 the grant line says what was granted: held / nested / observed / open"};
    set_case(st, L"c39", 24, 60, 900, "100 100");
    SH h1 = stdin_hold(st, L"hold --kind parse --want 2", L"c39a");
    c.need(h1.granted == "granted 2 held", "held: '" + h1.granted + "'");
    if (h1.marker.rfind("marker ", 0) == 0) {
        SetEnvironmentVariableW(L"WWE_ADMIT_HELD", w16(h1.marker.substr(7)).c_str());
        SH h2 = stdin_hold(st, L"hold --kind parse --want 5", L"c39b");
        SetEnvironmentVariableW(L"WWE_ADMIT_HELD", nullptr);
        stdin_hold_end(h2);
        c.need(h2.granted == "granted 2 nested", "nested: '" + h2.granted + "' (want 2: the covering hold's workers)");
    }
    stdin_hold_end(h1);
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"observe");
    SH h3 = stdin_hold(st, L"hold --kind parse --want 4", L"c39c");
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"enforce");
    stdin_hold_end(h3);
    c.need(h3.granted == "granted 4 observed", "observed: '" + h3.granted + "'");
    SetEnvironmentVariableW(L"WWE_NO_ADMIT", L"1");
    SH h4 = stdin_hold(st, L"hold --kind parse --want 3", L"c39d");
    SetEnvironmentVariableW(L"WWE_NO_ADMIT", nullptr);
    stdin_hold_end(h4);
    c.need(h4.granted == "granted 3 open", "open: '" + h4.granted + "'");
    report(st, c);
}
static void case40(ST& st) {
    Case c{"40 a second hold for the same pid and kind nests under the first; its release is absorbed and the outer hold stays"};
    wstring sd = set_case(st, L"c40", 24, 60, 900, "100 100");
    WJ p = plain_wait(st);
    signalled(p.started, LONG_MS);
    string out;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind parse --want 3 --pid " + wnum(p.p.pid) + L" --detach", L"c40a", out, eof);
    c.need(rc == 0 && out.rfind("granted 3 held", 0) == 0, "outer: '" + first_line(out) + "'");
    rc = run_piped(st, L"hold --kind parse --want 2 --pid " + wnum(p.p.pid) + L" --detach", L"c40b", out, eof);
    c.need(rc == 0 && out.rfind("granted 2 nested", 0) == 0, "inner: '" + first_line(out) + "' (want granted 2 nested)");
    c.need(live_count(sd) == 3, fmt("live %d after the inner hold, want 3 (the outer's, untouched)", live_count(sd)));
    rc = run_piped(st, L"release --pid " + wnum(p.p.pid), L"c40r1", out, eof);   // the inner script's release
    c.need(rc == 0 && live_count(sd) == 3, fmt("live %d after the inner release, want 3 (absorbed): '%s'", live_count(sd), first_line(out).c_str()));
    rc = run_piped(st, L"release --pid " + wnum(p.p.pid), L"c40r2", out, eof);   // the outer script's release
    c.need(rc == 0 && live_count(sd) == 0, fmt("live %d after the outer release, want 0", live_count(sd)));
    finish(p);
    report(st, c);
}
static void case41(ST& st) {
    Case c{"41 observe never nests: the same pid asking again, a child of it, and a marker holder each get their FULL want, observed; release ends one per call, oldest first"};
    wstring sd = set_case(st, L"c41", 24, 60, 900, "100 100");
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"observe");
    auto [s0, g0] = ev_names(st);
    HANDLE go0 = CreateEventW(nullptr, TRUE, FALSE, g0.c_str());
    wstring outf = join(st.root, L"c41_child.txt");
    // P waits, then (through cmd, alive while the inner front runs) asks for 8 for its own child tree
    P p = spawn(q(g_self) + L" __later --go " + g0 + L" -- cmd.exe /d /s /c \"" + q(g_target) + L" hold --kind parse --want 8 --detach > " + q(outf) + L"\"",
                nullptr, nullptr, nullptr);
    string out, mline;
    bool eof = false;
    long rc = run_piped(st, L"hold --kind parse --want 4 --pid " + wnum(p.pid) + L" --detach", L"c41a", out, eof);
    c.need(rc == 0 && out.rfind("granted 4 observed", 0) == 0, "outer: '" + first_line(out) + "'");
    size_t mk = out.find("\nmarker ");
    if (mk != string::npos) mline = out.substr(mk + 8, out.find('\n', mk + 1) - mk - 8);
    rc = run_piped(st, L"hold --kind parse --want 16 --pid " + wnum(p.pid) + L" --detach", L"c41b", out, eof);
    c.need(rc == 0 && out.rfind("granted 16 observed", 0) == 0, "the same pid asking again: '" + first_line(out) + "' (want granted 16 observed: observe never narrows)");
    if (!mline.empty()) {
        SetEnvironmentVariableW(L"WWE_ADMIT_HELD", w16(mline).c_str());
        SH h = stdin_hold(st, L"hold --kind parse --want 5", L"c41m");
        SetEnvironmentVariableW(L"WWE_ADMIT_HELD", nullptr);
        stdin_hold_end(h);
        c.need(h.granted == "granted 5 observed", "under the outer hold's marker: '" + h.granted + "' (want granted 5 observed)");
    } else {
        c.need(false, "the outer hold printed no marker");
    }
    SetEvent(go0);
    c.need(wait_text_or(outf, "granted", nullptr, LONG_MS) == 1, "the child of P never answered");
    string child = slurp(outf);
    c.need(child.rfind("granted 8 observed", 0) == 0, "a child of P: '" + first_line(child) + "' (want granted 8 observed)");
    long prc = wait_exit(p, LONG_MS);   // P exits once the child's front returned: P's two holds end with it
    (void)prc;
    // a fresh owner with two observed holds of one kind: release ends one per call, oldest first
    WJ p2 = plain_wait(st);
    signalled(p2.started, LONG_MS);
    run_piped(st, L"hold --kind parse --want 3 --pid " + wnum(p2.p.pid) + L" --detach", L"c41c", out, eof);
    run_piped(st, L"hold --kind parse --want 5 --pid " + wnum(p2.p.pid) + L" --detach", L"c41d", out, eof);
    c.need(wait_live(sd, 8, LONG_MS), fmt("live %d with P2's two observed holds, want 8 (3 + 5)", live_count(sd)));
    run_piped(st, L"release --pid " + wnum(p2.p.pid), L"c41r1", out, eof);
    c.need(live_count(sd) == 5, fmt("live %d after the first release, want 5 (the oldest, 3, ended)", live_count(sd)));
    run_piped(st, L"release --pid " + wnum(p2.p.pid), L"c41r2", out, eof);
    c.need(live_count(sd) == 0, fmt("live %d after the second release, want 0", live_count(sd)));
    finish(p2);
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"enforce");
    CloseHandle(go0);
    report(st, c);
}
// S106 0C: pending counts only the UNUSED part of a hold's reservation (the owner tree's measured use cancels it); a fresh hold counts in full; a
// partly used one only in part (the UNSAFE-direction twins: full use is the only thing that lets the next job in)
static void case42(ST& st) {
    Case c{"42 a hold's pending is its reservation minus its owner's measured use: fresh = full, partly used = the rest, fully used = 0"};
    wstring sd = set_case(st, L"c42", 24, 60, 900, "10 10");
    SH h = stdin_hold(st, L"hold --kind parse --want 3", L"c42h");   // 3 x 1.7 = 5.1 GB reserved, owner = this process
    c.need(h.granted == "granted 3 held", "the hold: '" + h.granted + "'");
    string e;
    const wstring ask = L"run --kind compile --timeout 2 -- cmd /c exit 0";   // needs 2.0 + floor 4 (and 2.5 + 4 commit)
    long rc = run_admit(st, ask, L"c42a", &e);   // no use listed: the whole 5.1 is pending: 10 - 5.1 - 4 = 0.9 < 2
    c.need(rc == EXIT_REFUSED, fmt("a fresh hold (no use): rc %ld, want 75 (waits)", rc));
    write_file(join(sd, L"fake_use.txt"), fmt("%lu 1.0 1.0\n", GetCurrentProcessId()));   // 1.0 of 5.1 used: 4.1 pending: 10 - 4.1 - 4 = 1.9 < 2
    rc = run_admit(st, ask, L"c42b", &e);
    c.need(rc == EXIT_REFUSED, fmt("a partly used hold (1.0 of 5.1): rc %ld, want 75 (waits)", rc));
    write_file(join(sd, L"fake_use.txt"), fmt("%lu 5.1 5.1\n", GetCurrentProcessId() + 1));   // use listed for ANOTHER process: this owner still counts in full
    rc = run_admit(st, ask, L"c42c", &e);
    c.need(rc == EXIT_REFUSED, fmt("use of another pid: rc %ld, want 75 (waits)", rc));
    write_file(join(sd, L"fake_use.txt"), fmt("%lu 5.1 5.1\n", GetCurrentProcessId()));   // all of it in use (and already out of the available figure): 0 pending
    rc = run_admit(st, ask, L"c42d", &e);
    auto rows = hist_rows(sd);
    c.need(rc == 0 && has_row(rows, C_OUTCOME, "admitted"), fmt("a fully used hold: rc %ld, want 0 (admitted)", rc));
    if (!rows.empty() && rows.back().size() > (size_t)C_HELDUSE) c.need(rows.back()[C_PENDING] != "-" && rows.back()[C_HELDUSE] != "-", "the history row has no pending_gb / held_use_gb");
    stdin_hold_end(h);
    report(st, c);
}
// S109 GATE: a hold whose minimum is not met within its bound asks once more at a minimum of 1: a free slot with memory above the floor = `narrowed`
static void case43(ST& st) {
    Case c{"43 at its bound a hold whose minimum cannot be met is granted what fits (granted 1 narrowed: one free slot), never forced past a slot or the floor"};
    wstring sd = set_case(st, L"c43", 2, 60, 900, "100 100");
    WJ a = wait_job(st, L"c43a", L"run --kind compile");   // one of the kind's two slots: a minimum of 2 cannot be met
    c.need(signalled(a.started, LONG_MS), "A never started");
    SH h = stdin_hold(st, L"hold --kind compile --want 8 --min 2 --timeout 1", L"c43h");
    c.need(h.granted == "granted 1 narrowed", "the hold said '" + h.granted + "', want 'granted 1 narrowed' (one free slot of two)");
    c.need(live_count(sd) == 2, fmt("live slots %d, want 2 (A's and the narrowed one)", live_count(sd)));
    stdin_hold_end(h);
    finish(a);
    report(st, c);
}
// S106 0CF (review B1): a hold's use is the use of the processes that really are under its owner. The fake process table (<state>\fake_tree.txt, rows
// "<pid> <ppid> <creation time> <phys_gb> <commit_gb>") plants a process that was created BEFORE the owner yet names it as its parent (a recycled pid):
// it is not counted, so the next ask waits; a real child (created after), a real grandchild, and the planted older grandchild each get the verdict.
static void case44(ST& st) {
    Case c{"44 a hold's measured use counts only processes created AFTER their parent (a recycled owner pid is not a child); a holder's own pid is not the pool's"};
    wstring sd = set_case(st, L"c44", 24, 60, 900, "10 10");
    SH h = stdin_hold(st, L"hold --kind parse --want 3", L"c44h");   // 5.1 GB reserved, owner = this process
    c.need(h.granted == "granted 3 held", "the hold: '" + h.granted + "'");
    unsigned long me = GetCurrentProcessId();
    const wstring ask = L"run --kind compile --timeout 2 -- cmd /c exit 0";   // needs 2.0 + floor 4: waits while 5.1 is pending
    string e;
    auto tree = [&](const string& rows) { write_file(join(sd, L"fake_tree.txt"), rows); };
    tree(fmt("%lu 1 1000 0 0\n7001 %lu 500 5.1 5.1\n", me, me));   // created at 500, the owner at 1000: an unrelated process with a recycled parent pid
    long rc = run_admit(st, ask, L"c44a", &e);
    c.need(rc == EXIT_REFUSED, fmt("a process OLDER than the owner that names it as parent was counted: rc %ld, want 75 (waits)", rc));
    tree(fmt("%lu 1 1000 0 0\n7001 %lu 2000 5.1 5.1\n", me, me));   // a real child, fully using the reservation
    rc = run_admit(st, ask, L"c44b", &e);
    c.need(rc == 0, fmt("a real child using all of it: rc %ld, want 0 (admitted)", rc));
    tree(fmt("%lu 1 1000 0 0\n7001 %lu 2000 0 0\n7002 7001 1500 5.1 5.1\n", me, me));   // a grandchild older than its parent
    rc = run_admit(st, ask, L"c44c", &e);
    c.need(rc == EXIT_REFUSED, fmt("a grandchild older than its parent was counted: rc %ld, want 75", rc));
    tree(fmt("%lu 1 1000 0 0\n7001 %lu 2000 0 0\n7002 7001 2500 5.1 5.1\n", me, me));   // a real grandchild
    rc = run_admit(st, ask, L"c44d", &e);
    c.need(rc == 0, fmt("a real grandchild using all of it: rc %ld, want 0 (admitted)", rc));
    tree(fmt("%lu 1 1000 0 0\n%lu %lu 2000 5.1 5.1\n", me, (unsigned long)h.p.pid, me));   // the hold's own admit process (a child of the owner): not the pool's use
    rc = run_admit(st, ask, L"c44e", &e);
    c.need(rc == EXIT_REFUSED, fmt("a live holder's own process was credited to the hold: rc %ld, want 75", rc));
    stdin_hold_end(h);
    report(st, c);
}
// S106 0CF (review B5) / S109 GATE: at the bound a hold gets what fits above the floor, never its whole minimum into a full PC
static void case45(ST& st) {
    Case c{"45 at the bound a grant is what fits: min 4 x 1.7 GB into 2 GB above the floor gives granted 1 narrowed, not 4"};
    set_case(st, L"c45", 24, 60, 900, "6 6");   // 6 GB free, floor 4: room for ONE 1.7 GB worker; the minimum of 4 never fits, so the hold reaches its bound
    SH h = stdin_hold(st, L"hold --kind parse --want 8 --min 4 --timeout 1", L"c45h");
    c.need(h.granted == "granted 1 narrowed", "the hold said '" + h.granted + "', want 'granted 1 narrowed' (the minimum 4, cut to what fits)");
    stdin_hold_end(h);
    set_case(st, L"c45b", 24, 60, 900, "100 100");
    SH h2 = stdin_hold(st, L"hold --kind parse --want 8 --min 4 --timeout 1", L"c45h2");   // plenty of memory: admitted at once, never forced
    c.need(h2.granted == "granted 8 held", "with memory free: '" + h2.granted + "'");
    stdin_hold_end(h2);
    report(st, c);
}
// S106 0CF (review B2): the counter choice. Shared pages (images, DLLs) are in every process's WorkingSetSize: the private figure is used.
static void case46(ST& st) {
    Case c{"46 the physical use of a process is its private working set (shared pages are not counted once per process)"};
    (void)st;
    c.need(pick_private_phys(100, 30, 20) == 20, "with PrivateWorkingSetSize 20 of a working set of 100: want 20");
    c.need(pick_private_phys(100, 30, -1) == 30, "without that counter: min(working set 100, PrivateUsage 30) = 30");
    c.need(pick_private_phys(100, 300, -1) == 100, "PrivateUsage above the working set (paged out): the working set, 100");
    c.need(pick_private_phys(10, 30, 50) == 10, "never more than the working set");
    c.need(pick_private_phys(100, 30, 0) == 0, "a private working set of 0 is 0, not a fall-back");
    report(st, c);
}
// S106 0CF (review F11): the REAL tree_use path (no fake file): a hold whose owner (this process) has a child that really allocates ~256 MB. A single
// allocation, held for a few seconds; killed at the end.
static void case47(ST& st) {
    Case c{"47 real mode: the use of a hold owner's child process is measured (GetProcessMemoryInfo works with the access the walk asks for)"};
    wstring sd = set_case(st, L"c47", 24, 60, 900, "100 100");
    SH h = stdin_hold(st, L"hold --kind parse --want 3", L"c47h");
    c.need(h.granted == "granted 3 held", "the hold: '" + h.granted + "'");
    wstring of = join(st.root, L"c47alloc.out");
    HANDLE oh = out_file(of);
    P ch = spawn(q(g_self) + L" __alloc 256 8", nullptr, oh, oh);
    CloseHandle(oh);
    c.need(wait_text_or(of, "ready", nullptr, LONG_MS) == 1, "the allocating child never said ready");
    Ctx cx;
    cx.state = sd;
    load_config(g_cfg_path, cx.cfg, cx.cfg_err);
    cx.test = false;   // the real reader
    View v = view_of(cx);
    c.need(v.use_p >= 0.2 && v.use_p < 4.0, fmt("held_use_gb %.3f, want >= 0.2 (a child holding 256 MB) and sane", v.use_p));
    c.info = fmt("held_use_gb %.3f", v.use_p);
    if (ch.h) { TerminateProcess(ch.h, 1); WaitForSingleObject(ch.h, 5000); CloseHandle(ch.h); }
    stdin_hold_end(h);
    report(st, c);
}
// S106 0CFF (review B-a): a forced grant is capped by what is FREE above the floor AFTER the pending reservations (read under the lock): a hold that
// timed out into a PC whose pools are reserved but not yet allocated used to get its whole minimum on top of them (three of min 4 x 3 GB = 36 GB)
static void case48(ST& st) {
    Case c{"48 the bound's grant counts pending reservations: 12 GB free, 5.1 GB reserved, min 4 x 1.7 GB gives granted 1 narrowed; a third (1.2 GB left) is REFUSED"};
    set_case(st, L"c48", 24, 60, 900, "12 12");   // free 12, floor 4: 8 GB above it; the first hold reserves 5.1 and uses nothing yet
    SH h1 = stdin_hold(st, L"hold --kind parse --want 3", L"c48h1");
    c.need(h1.granted == "granted 3 held", "the first hold: '" + h1.granted + "'");
    SH h2 = stdin_hold(st, L"hold --kind parse --want 8 --min 4 --timeout 1", L"c48h2");
    c.need(h2.granted == "granted 1 narrowed", "the second hold said '" + h2.granted + "', want 'granted 1 narrowed' (8 - 5.1 pending = 2.9 above the floor = one 1.7 GB worker; 4 would be 6.8 GB)");
    SH h3 = stdin_hold(st, L"hold --kind parse --want 8 --min 4 --timeout 1", L"c48h3");   // the lock-held read sees h1 AND h2: 12 - 6.8 - 4 = 1.2 < 1.7
    c.need(h3.granted.rfind("refused REFUSED after ", 0) == 0, "a third hold said '" + h3.granted + "', want the refusal (S109 GATE: never forced past the floor)");
    stdin_hold_end(h3);
    stdin_hold_end(h2);
    stdin_hold_end(h1);
    report(st, c);
}
// S106 0CFF (review B-b): a hold record with no recorded creation time (every hold written before 0CF) names an owner pid that may since belong to a
// STRANGER: unknown identity counts the reservation in full, the stranger's tree is never walked. Real mode (no fake file): this process owns the hold
// and has a child that really holds 256 MB; with the creation time stripped from the record the use must be 0.
static void case49(ST& st) {
    Case c{"49 a hold record with creation time 0: its owner's tree is not walked (the reservation counts in full); with the time it is measured"};
    wstring sd = set_case(st, L"c49", 24, 60, 900, "100 100");
    SH h = stdin_hold(st, L"hold --kind parse --want 3", L"c49h");
    c.need(h.granted == "granted 3 held", "the hold: '" + h.granted + "'");
    wstring of = join(st.root, L"c49alloc.out");
    HANDLE oh = out_file(of);
    P ch = spawn(q(g_self) + L" __alloc 256 8", nullptr, oh, oh);
    CloseHandle(oh);
    c.need(wait_text_or(of, "ready", nullptr, LONG_MS) == 1, "the allocating child never said ready");
    Ctx cx;
    cx.state = sd;
    load_config(g_cfg_path, cx.cfg, cx.cfg_err);
    cx.test = false;   // the real reader
    View v0 = view_of(cx);
    c.need(v0.use_p >= 0.2, fmt("control: with its creation time the hold's use is %.3f, want >= 0.2", v0.use_p));
    vector<Holder> hs;
    live_count(sd, &hs);
    bool done = false;
    for (auto& h0 : hs) {   // every slot of the hold carries the record
        wstring f = join(sd, w16(h0.kind) + L"." + wnum((unsigned)h0.index));
        HANDLE fh = CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh != INVALID_HANDLE_VALUE) {
            string rec = read_record(fh);
            size_t a = rec.find("\"parent_ct\":\"");
            if (a != string::npos) {
                size_t b = rec.find('"', a + 13);
                if (b != string::npos) { rec.replace(a + 13, b - (a + 13), "0"); write_record(fh, rec); done = true; }
            }
            CloseHandle(fh);
        }
    }
    c.need(done, "could not rewrite the hold record");
    View v1 = view_of(cx);
    c.need(v1.use_p == 0.0, fmt("no creation time recorded: use %.3f, want 0 (a stranger's tree is not walked)", v1.use_p));
    c.need(v1.pend_p >= 5.0, fmt("pending %.3f, want the whole 5.1 GB reservation", v1.pend_p));
    if (ch.h) { TerminateProcess(ch.h, 1); WaitForSingleObject(ch.h, 5000); CloseHandle(ch.h); }
    stdin_hold_end(h);
    report(st, c);
}
// S109 GATE (was S106 0CFF F-a, the forced take's `open`): a last ask whose machine lock stays busy for its 5 s of retries is REFUSED, the lock named
static void case50(ST& st) {
    Case c{"50 the bound's last ask with the machine lock busy throughout is REFUSED (exit 75), the line naming the lock; nothing is granted open"};
    wstring sd = set_case(st, L"c50", 2, 60, 900, "100 100");
    WJ a = wait_job(st, L"c50a", L"run --kind compile");   // one of the kind's two slots: a minimum of 2 cannot be met, so the hold times out
    c.need(signalled(a.started, LONG_MS), "A never started");
    Ctx cx;
    cx.state = sd;
    cx.glock = CreateFileW(join(sd, L"admit.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    string err;
    c.need(glock_take(cx, 5000, err) == GL_TAKEN, "the selftest could not take the machine lock");
    SH h = stdin_hold(st, L"hold --kind compile --want 8 --min 2 --timeout 1", L"c50h");   // never gets the lock: the last ask's retries run out after 5 s
    c.need(h.granted.rfind("refused REFUSED after ", 0) == 0 && h.granted.find("admit.lock is busy") != string::npos,
           "the hold said '" + h.granted + "', want a refusal naming the busy machine lock");
    glock_release(cx);
    CloseHandle(cx.glock);
    stdin_hold_end(h);
    finish(a);
    report(st, c);
}
// S109 GATE (EXEB-B1, S108 12:54-13:28; was S106 0CFF F-b "keeps waiting", the endless extension): commit short, nothing in the queue holding anything.
// The hold waits its 3 s bound, writes its `waiting` row at 2 s, and is REFUSED: exit 75, stdout `refused ...`, the loud line naming the memory, the
// empty queue and the largest committer outside it (fake_top.txt). Planted twin: the pre-GATE exe (--target) waits on and fails every check.
static void case51(ST& st) {
    Case c{"51 commit short with 0 holders: a hold is REFUSED at its 3 s bound (exit 75, `refused` on stdout, `waiting` + `refused` rows, the outside committer named)"};
    wstring sd = set_case(st, L"c51", 24, 60, 900, "5 5");
    write_file(join(sd, L"fake_top.txt"), "wwe2k14.exe 4242 17.5\nchrome.exe 777 2.0\n");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE inR, inW, outR, outW;
    CreatePipe(&inR, &inW, &sa, 0);
    CreatePipe(&outR, &outW, &sa, 0);
    HANDLE ef = out_file(join(st.root, L"c51h.err"));
    double t0 = qpc_s();
    P p = spawn(q(g_target) + L" hold --kind parse --want 3 --min 1 --timeout 3", inR, outW, ef);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(ef);
    string line = read_line_pipe(outR, 15000);   // the bound is 3 s: 15 s is the old code's tell (it waits on, says nothing)
    double secs = qpc_s() - t0;
    c.need(line.rfind("refused REFUSED after ", 0) == 0, "the hold said '" + line + "', want `refused REFUSED after ...`");
    c.need(secs >= 2.5 && secs < 12.0, fmt("the answer came after %.1f s (bound 3 s)", secs));
    long rc = wait_exit(p, 10000);
    c.need(rc == EXIT_REFUSED, fmt("exit %ld, want 75", rc));
    string e = slurp(join(st.root, L"c51h.err"));
    c.need(e.find("[admit] REFUSED after ") != string::npos && e.find("commit left 5.0 GB") != string::npos && e.find("queue holders 0") != string::npos &&
               e.find("wwe2k14.exe pid 4242 17.5 GB") != string::npos && e.find("reason commit") != string::npos,
           "the loud line lacks the memory / the empty queue / the outside committer / the reason: " + e.substr(0, 400));
    auto rows = hist_rows(sd);
    c.need(rows.size() == 2 && rows[0][C_OUTCOME] == "waiting" && rows[1][C_OUTCOME] == "refused", fmt("%zu rows, want `waiting` then `refused`", rows.size()));
    CloseHandle(inW);
    CloseHandle(outR);
    c.info = e.substr(0, 160);
    report(st, c);
}
// S109 GATE: the config caps every caller's limit: --timeout 900 with timeout_s 2 is refused at about 2 s (no caller can buy a long silent wait)
static void case55(ST& st) {
    Case c{"55 the config's timeout_s caps the caller: --timeout 900 against timeout_s 2 is refused in about 2 s, not 900"};
    set_case(st, L"c55", 24, 60, 2, "5 5");
    string e;
    double secs = 0;
    long rc = run_admit(st, L"run --kind compile --timeout 900 -- cmd /c exit 0", L"c55", &e, &secs);
    c.need(rc == EXIT_REFUSED && secs >= 1.5 && secs < 12.0, fmt("rc %ld after %.1f s, want 75 after about 2 s", rc, secs));
    report(st, c);
}
// S109 GATE: a queue holder is named in a refusal with its slot, pid, program and age
static void case56(ST& st) {
    Case c{"56 a refusal names every queue holder (kind.slot pid 'prog' for N s) and the pending reservation"};
    wstring sd = set_case(st, L"c56", 24, 60, 900, "8 8");   // 4 GB above the floor; the first hold reserves 3 x 1.7 = 5.1 of it
    SH h1 = stdin_hold(st, L"hold --kind parse --want 3 --min 1", L"c56h1");
    c.need(h1.granted.rfind("granted ", 0) == 0, "the first hold: '" + h1.granted + "'");
    string e;
    long rc = run_admit(st, L"run --kind parse --timeout 2 -- cmd /c exit 0", L"c56", &e);
    c.need(rc == EXIT_REFUSED, fmt("rc %ld, want 75", rc));
    c.need(e.find("parse.0 pid ") != string::npos && e.find("'hold' for ") != string::npos && e.find("queue holders ") != string::npos && e.find("pending ") != string::npos,
           "the refusal does not name the holder: " + e.substr(0, 400));
    stdin_hold_end(h1);
    report(st, c);
}
// S109 GATE: a waiter its caller kills (no end row can be written) is still on record: its `waiting` row from 2 s
static void case57(ST& st) {
    Case c{"57 a waiter killed after 2 s leaves its `waiting` row (pid, kind, the reason) in history.tsv"};
    wstring sd = set_case(st, L"c57", 24, 60, 900, "5 5");
    HANDLE ef = out_file(join(st.root, L"c57.err"));
    P p = spawn(q(g_target) + L" run --kind compile --timeout 60 -- cmd /c exit 0", nullptr, ef, ef);
    CloseHandle(ef);
    bool seen = wait_row(sd, C_OUTCOME, "waiting", 10000);
    if (p.h) { TerminateProcess(p.h, 1); WaitForSingleObject(p.h, 5000); }
    c.need(seen, "no `waiting` row within 10 s of a wait that began at once");
    auto rows = hist_rows(sd);
    bool mine = false;
    for (auto& r : rows) mine = mine || (r[C_OUTCOME] == "waiting" && r[C_PID] == fmt("%lu", p.pid) && r[C_KIND] == "compile" && r[C_ENDED] == "waiting:commit");
    c.need(mine, "the waiting row lacks the killed waiter's pid / kind / reason");
    if (p.h) CloseHandle(p.h);
    report(st, c);
}
static int child_alloc(int mb, int sec) {   // __alloc MB SECONDS: commit and touch MB megabytes once, say ready, hold them
    char* m = (char*)VirtualAlloc(nullptr, (SIZE_T)mb << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m) return 3;
    memset(m, 1, (size_t)mb << 20);
    say_out("ready\n");
    Sleep((DWORD)sec * 1000);
    return 0;
}
// entry: attempt (cases 52 / 53 drive `hold` and `run` through admit_wait -> attempt, the production path)
// S106 C5: the alone pause (docs\research\s106\C5.md). The selftest holds the pause file's byte-0 lock itself (an in-process handle), exactly what runlock.ps1 -Alone does.
static HANDLE alone_hold(const wstring& f, double until_in_s, const char* token) {
    HANDLE h = slot_open(f, true);
    if (h == INVALID_HANDLE_VALUE) return h;
    if (!try_lock0(h)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    double now = (double)now_ft() / 1e7 - 11644473600.0;
    write_record(h, fmt("{\"pid\":%lu,\"token\":\"%s\",\"name\":\"selftest alone\",\"since\":%.0f,\"until\":%.0f}", GetCurrentProcessId(), token, now, now + until_in_s));
    return h;
}
// 52: a pause grants nothing (the stdin-form hold and the run-form); S109 GATE: its time counts toward the bound, so both are REFUSED naming the alone
// run; closing the pause grants at once
static void case52(ST& st) {
    Case c{"52 an alone pause grants nothing (hold, run): both are REFUSED at their 1 s bound naming the run; closing it grants"};
    wstring sd = set_case(st, L"c52", 24, 60, 900, "100 100");
    wstring af = join(sd, L"alone.lock");
    SetEnvironmentVariableW(L"WWE_ALONE_FILE", af.c_str());
    HANDLE ph = alone_hold(af, 600, "tok52");
    c.need(ph != INVALID_HANDLE_VALUE, "the selftest could not take the pause");
    SH h;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE inR, outW;
    CreatePipe(&inR, &h.inW, &sa, 0);
    CreatePipe(&h.outR, &outW, &sa, 0);
    HANDLE ef = out_file(join(st.root, L"c52h.err"));
    h.p = spawn(q(g_target) + L" hold --kind compile --want 1 --min 1 --timeout 1", inR, outW, ef);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(ef);
    wstring touched = join(sd, L"c52_touched");
    HANDLE e = out_file(join(st.root, L"c52r.err"));
    P run = spawn(q(g_target) + L" run --kind compile --timeout 1 -- " + q(g_self) + L" __touch " + q(touched), nullptr, e, e);
    CloseHandle(e);
    // S109 GATE: the pause counts toward the bound like any reason: both asks are REFUSED at about 1 s, naming the alone run; nothing ran
    h.granted = read_line_pipe(h.outR, 15000);
    c.need(h.granted.rfind("refused REFUSED after ", 0) == 0 && h.granted.find("held off by an alone run 'selftest alone'") != string::npos,
           "the paused hold said '" + h.granted + "', want a refusal naming the alone run");
    c.need(wait_exit(run, 15000) == EXIT_REFUSED && !is_file(touched), "the paused run-form was not refused at its bound, or it ran its command");
    stdin_hold_end(h);
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);   // closing the handle frees byte 0
    SH h2 = stdin_hold(st, L"hold --kind compile --want 1 --min 1 --timeout 5", L"c52h2");
    c.need(h2.granted == "granted 1 held", "after the pause ended a hold said '" + h2.granted + "', want 'granted 1 held'");
    stdin_hold_end(h2);
    SetEnvironmentVariableW(L"WWE_ALONE_FILE", join(st.root, L"alone_none.lock").c_str());
    report(st, c);
}
// 53: a paused ask ends at its bound (75, never forced); the run's own token passes; an expired pause is ignored
static void case53(ST& st) {
    Case c{"53 a pause ends the ask at its bound (75, not forced); the alone run's own token passes; an expired pause is ignored"};
    wstring sd = set_case(st, L"c53", 24, 60, 900, "100 100");
    wstring af = join(sd, L"alone.lock");
    SetEnvironmentVariableW(L"WWE_ALONE_FILE", af.c_str());
    HANDLE ph = alone_hold(af, 600, "tok53");
    string e;
    double waited53 = 0;
    long rc = run_admit(st, L"run --kind compile --timeout 2 -- cmd /c exit 0", L"c53a", &e, &waited53);
    c.need(rc == EXIT_REFUSED, fmt("a paused fail-form ask: rc %ld, want 75", rc));
    c.need(waited53 >= 1.5, fmt("a paused fail-form ask ended after %.1f s: it was refused at once instead of waiting its limit", waited53));
    SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", L"tok53");
    rc = run_admit(st, L"run --kind compile --timeout 2 -- cmd /c exit 0", L"c53b", &e, &waited53);
    c.need(rc == 0, fmt("the alone run's own token was held off: rc %ld", rc));
    SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", L"other");
    rc = run_admit(st, L"run --kind compile --timeout 2 -- cmd /c exit 0", L"c53c", &e);
    c.need(rc == EXIT_REFUSED, fmt("another run's token passed the pause: rc %ld, want 75", rc));
    SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", nullptr);
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    ph = alone_hold(af, -30, "tok53x");   // the lock is held but the record's `until` has passed: a hung holder
    rc = run_admit(st, L"run --kind compile --timeout 2 -- cmd /c exit 0", L"c53d", &e);
    c.need(rc == 0, fmt("an expired pause held compiles off: rc %ld", rc));
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    // a holder that dies without cleaning up (python's msvcrt lock, killed): the OS frees byte 0 and the ask is granted
    if (!st.helper.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
        HANDLE inR, inW, outR, outW;
        CreatePipe(&inR, &inW, &sa, 0);
        CreatePipe(&outR, &outW, &sa, 0);
        HANDLE ef = out_file(join(st.root, L"c53py.err"));
        P py = spawn(L"py -I " + q(st.helper) + L" hold " + q(af) + L" alone", inR, outW, ef);   // S106 C5F: the pause record carries a token and a future until
        CloseHandle(inR);
        CloseHandle(outW);
        CloseHandle(ef);
        string line = read_line_pipe(outR, LONG_MS);
        c.need(line == "locked", "python said '" + line + "'");
        rc = run_admit(st, L"run --kind compile --timeout 1 -- cmd /c exit 0", L"c53e", &e);
        c.need(rc == EXIT_REFUSED, fmt("a python-held pause did not hold compiles off: rc %ld", rc));
        wstring kill = L"taskkill /F /T /PID " + wnum(py.pid);
        P k = spawn(kill, nullptr, nullptr, nullptr);
        wait_exit(k, LONG_MS);
        CloseHandle(inW);
        CloseHandle(outR);
        double t0 = qpc_s();
        rc = run_admit(st, L"run --kind compile --timeout 5 -- cmd /c exit 0", L"c53f", &e);
        c.need(rc == 0 && qpc_s() - t0 < 5.0, fmt("after the holder was killed the pause still held compiles off: rc %ld", rc));
    }
    SetEnvironmentVariableW(L"WWE_ALONE_FILE", join(st.root, L"alone_none.lock").c_str());
    report(st, c);
}
// S106 C5F note 1 / 4: what is a pause. A locked pause file is a pause only with a parseable record (token, future until, live pid [+ pid_start]); the probe
// never takes the exclusive byte (two probers, or a prober and a holder's own scan, never make each other "paused"); the pause is looked at again inside the
// machine lock.
static HANDLE alone_hold_raw(const wstring& f, const string& raw) {   // takes byte 0 (exclusive, like the holder) and writes RAW from byte 1 as the record
    HANDLE h = slot_open(f, true);
    if (h == INVALID_HANDLE_VALUE) return h;
    if (!try_lock0(h)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    if (!raw.empty()) write_record(h, raw);
    else { LARGE_INTEGER one{}; one.QuadPart = 1; SetFilePointerEx(h, one, nullptr, FILE_BEGIN); SetEndOfFile(h); }
    return h;
}
static void case54(ST& st) {
    Case c{"54 a pause needs a parseable live record: empty / unparseable / dead-pid / reused-pid records are not pauses; the probe is shared; the in-lock look refuses"};
    wstring sd = set_case(st, L"c54", 24, 60, 900, "100 100");
    wstring af = join(sd, L"alone.lock");
    SetEnvironmentVariableW(L"WWE_ALONE_FILE", af.c_str());
    const wstring ask = L"run --kind compile --timeout 2 -- cmd /c exit 0";
    string e;
    double now = (double)now_ft() / 1e7 - 11644473600.0;
    unsigned long me = GetCurrentProcessId();
    long long my_start = (long long)pid_created(me) + 504911232000000000LL;
    HANDLE ph;
    long rc;
    // a) a live holder with an EMPTY record (a clean exit truncates it; a failed write leaves it): not a pause
    ph = alone_hold_raw(af, "");
    c.need(ph != INVALID_HANDLE_VALUE, "the selftest could not take the pause (a)");
    rc = run_admit(st, ask, L"c54a", &e);
    c.need(rc == 0, fmt("a locked pause file with an empty record held compiles off: rc %ld, want 0", rc));
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    // b) unparseable
    ph = alone_hold_raw(af, "{\"pid\": 12, \"token\": ");
    rc = run_admit(st, ask, L"c54b", &e);
    c.need(rc == 0, fmt("a locked pause file with an unparseable record held compiles off: rc %ld, want 0", rc));
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    // c) a record with a token and a future until whose process is gone
    {
        wstring cmd0 = L"cmd /c exit 0";
        P dead = spawn(cmd0, nullptr, nullptr, nullptr);
        wait_exit(dead, LONG_MS);
        ph = alone_hold_raw(af, fmt("{\"pid\":%lu,\"token\":\"t54c\",\"name\":\"gone\",\"since\":%.0f,\"until\":%.0f}", (unsigned long)dead.pid, now, now + 600));
        rc = run_admit(st, ask, L"c54c", &e);
        c.need(rc == 0, fmt("a pause whose holder process is gone held compiles off: rc %ld, want 0", rc));
        if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    }
    // d) a live pid with a pid_start that is not that process's start (the pid was reused): not a pause; the same record with the right pid_start IS one
    ph = alone_hold_raw(af, fmt("{\"pid\":%lu,\"pid_start\":%lld,\"token\":\"t54d\",\"name\":\"reused\",\"since\":%.0f,\"until\":%.0f}", me, my_start + 600000000LL, now, now + 600));
    rc = run_admit(st, ask, L"c54d", &e);
    c.need(rc == 0, fmt("a pause with another process's start time held compiles off: rc %ld, want 0", rc));
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    ph = alone_hold_raw(af, fmt("{\"pid\":%lu,\"pid_start\":%lld,\"token\":\"t54e\",\"name\":\"%s\",\"since\":%.0f,\"until\":%.0f}", me, my_start, string(500, 'n').c_str(), now, now + 600));
    rc = run_admit(st, ask, L"c54e", &e);
    c.need(rc == EXIT_REFUSED, fmt("a live pause with the right pid_start did not hold compiles off (the detector cannot fail): rc %ld, want 75", rc));
    c.need(e.find(string(201, 'n')) == string::npos, "a 500-character pause name was printed whole (capped at 200)");
    // f) the in-lock look: the OUTER look dropped (a plant: the pause began after it), the lock look must still refuse; both dropped (the twin) grants
    SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", L"no_outer_alone");
    rc = run_admit(st, ask, L"c54f", &e);
    c.need(rc == EXIT_REFUSED, fmt("with the outer look dropped the in-lock look let a grant through: rc %ld, want 75", rc));
    SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", L"no_alone_looks");
    rc = run_admit(st, ask, L"c54g", &e);
    c.need(rc == 0, fmt("both looks dropped must grant (the plant's detector): rc %ld, want 0", rc));
    SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", st.plant ? w16(st.plant_name).c_str() : nullptr);
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    // g) two probers at once, no exclusive holder: a SHARED lock on byte 0 (another admit probing) must not make a pause out of a leftover valid record
    ph = alone_hold_raw(af, fmt("{\"pid\":%lu,\"pid_start\":%lld,\"token\":\"t54h\",\"name\":\"left\",\"since\":%.0f,\"until\":%.0f}", me, my_start, now, now + 600));
    if (ph != INVALID_HANDLE_VALUE) { unlock0(ph); }
    HANDLE other = slot_open(af, false);
    OVERLAPPED ov{};
    bool shared_ok = other != INVALID_HANDLE_VALUE && LockFileEx(other, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov);   // the other prober's shared byte
    c.need(shared_ok, "the selftest could not take a shared byte 0");
    rc = run_admit(st, ask, L"c54h", &e);
    c.need(rc == 0, fmt("a prober holding byte 0 shared made a pause out of a record nobody holds: rc %ld, want 0", rc));
    if (other != INVALID_HANDLE_VALUE) { OVERLAPPED o2{}; UnlockFileEx(other, 0, 1, 0, &o2); CloseHandle(other); }
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    // h) the dedicated alone token (S106 C5F note 3): WWE_ALONE_TOKEN = the record's token passes even when WWE2K14_RUNLOCK_TOKEN is the OUTER holder's; the outer
    // token alone (a sibling of the alone run inside the outer holder's tree) does not
    ph = alone_hold_raw(af, fmt("{\"pid\":%lu,\"pid_start\":%lld,\"token\":\"t54i\",\"name\":\"dedicated\",\"since\":%.0f,\"until\":%.0f}", me, my_start, now, now + 600));
    SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", L"outer-holder");
    rc = run_admit(st, ask, L"c54i", &e);
    c.need(rc == EXIT_REFUSED, fmt("a sibling carrying only the outer holder's token passed the pause: rc %ld, want 75", rc));
    SetEnvironmentVariableW(L"WWE_ALONE_TOKEN", L"t54i");
    rc = run_admit(st, ask, L"c54j", &e);
    c.need(rc == 0, fmt("the alone run's dedicated token was held off: rc %ld, want 0", rc));
    SetEnvironmentVariableW(L"WWE_ALONE_TOKEN", nullptr);
    SetEnvironmentVariableW(L"WWE2K14_RUNLOCK_TOKEN", nullptr);
    if (ph != INVALID_HANDLE_VALUE) CloseHandle(ph);
    // i) the capability query (note 5)
    {
        string out_cap;
        long rcc = run_admit(st, L"capabilities", L"c54k", &out_cap);
        c.need(rcc == 0, fmt("`admit capabilities` exit %ld, want 0", rcc));
    }
    SetEnvironmentVariableW(L"WWE_ALONE_FILE", join(st.root, L"alone_none.lock").c_str());
    report(st, c);
}
static bool g_no_real = false;   // S109 GATE: --no-real skips the cases that allocate real memory or read the real PC (47, 49, overhead)
static void run_new_cases(ST& st) {
    case52(st);
    case53(st);
    case54(st);
    case44(st);
    case45(st);
    case46(st);
    if (!g_no_real) case47(st);
    case48(st);
    if (!g_no_real) case49(st);
    case50(st);
    case51(st);
    case55(st);
    case56(st);
    case57(st);
}
// S109 GATE: the cases of the bounded wait and the refusal (`--only-gate`; with `--target <pre-GATE exe>` every one of them fails: the planted twin)
static void run_gate_cases(ST& st) {
    case2(st);
    case7(st);
    case37(st);
    case43(st);
    case45(st);
    case48(st);
    case50(st);
    case51(st);
    case52(st);
    case55(st);
    case56(st);
    case57(st);
}
static void case_overhead(ST& st) {
    Case c{"overhead: admit run -- cmd /c exit 0 vs cmd /c exit 0, median of 50 each (bar 30 ms, judged only when the CPU was under 50 % busy)"};
    set_case(st, L"cov", 24, 60, 900, nullptr);   // no fake file: the real memory reader
    FILETIME i0, k0, u0, i1, k1, u1;
    GetSystemTimes(&i0, &k0, &u0);
    vector<double> d, a;
    for (int i = 0; i < 50; i++) {
        double t0 = qpc_s();
        P p = spawn(L"cmd /c exit 0", nullptr, nullptr, nullptr);
        wait_exit(p, LONG_MS);
        d.push_back(qpc_s() - t0);
        t0 = qpc_s();
        p = spawn(q(g_target) + L" run --kind compile -- cmd /c exit 0", nullptr, nullptr, nullptr);
        wait_exit(p, LONG_MS);
        a.push_back(qpc_s() - t0);
    }
    GetSystemTimes(&i1, &k1, &u1);
    auto u64 = [](FILETIME f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    double idle = (double)(u64(i1) - u64(i0)), total = (double)(u64(k1) - u64(k0) + u64(u1) - u64(u0));   // kernel time includes idle
    double busy = total > 0 ? 100.0 * (1.0 - idle / total) : 0;
    std::sort(d.begin(), d.end());
    std::sort(a.begin(), a.end());
    double md = (d[24] + d[25]) / 2 * 1000, ma = (a[24] + a[25]) / 2 * 1000;
    bool quiet = busy < 50.0;
    if (quiet) c.need(ma - md <= 30, fmt("overhead %.1f ms", ma - md));
    auto rows = hist_rows(join(st.root, L"cov"));
    c.need(rows.size() == 50, fmt("history rows %zu, want 50", rows.size()));
    c.info = fmt("direct %.1f ms, through admit %.1f ms, overhead %.1f ms; CPU %.0f %% busy: %s", md, ma, ma - md, busy, quiet ? "judged" : "NOT judged (busy PC)");
    report(st, c);
}
static void rmtree(const wstring& d) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(join(d, L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            wstring p = join(d, n);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) rmtree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(d.c_str());
}
static int cmd_selftest(int argc, wchar_t** argv) {
    ST st;
    bool keep = false, only_new = false, only_alone = false, only_gate = false;
    g_target = g_self;
    for (int i = 2; i < argc; i++) {
        wstring a = argv[i];
        if (a == L"--keep") keep = true;
        else if (a == L"--only-gate") only_gate = true;   // S109 GATE: the bounded wait and the refusal
        else if (a == L"--no-real") g_no_real = true;     // S109 GATE: fake memory only (skips 47, 49 and the overhead case)
        else if (a == L"--only-new") only_new = true;   // S106 0CF: just the new cases (44 and up)
        else if (a == L"--only-alone") only_alone = true;   // S106 C5F: cases 52 - 54
        else if (a == L"--target" && i + 1 < argc) g_target = full(argv[++i]);
        else if (a == L"--plant" && i + 1 < argc) { st.plant = true; st.plant_name = u8(argv[++i]); }
    }
    for (const wchar_t* v : {L"WWE_ADMIT_HELD", L"WWE_NO_ADMIT", L"WWE_ADMIT_PLANT", L"WWE_ADMIT_DIR", L"WWE_ADMIT_CONFIG", L"WWE2K14_RUNLOCK_TOKEN", L"WWE_ALONE_FILE"})
        SetEnvironmentVariableW(v, nullptr);
    SetEnvironmentVariableW(L"WWE_ADMIT_TEST", L"1");
    SetEnvironmentVariableW(L"WWE_ADMIT_MODE", L"enforce");   // the waiting rules are tested in enforce; the observe cases set observe themselves
    if (st.plant) SetEnvironmentVariableW(L"WWE_ADMIT_PLANT", w16(st.plant_name).c_str());
    // every child of the selftest (and everything they start) is in this kill-on-close job: nothing outlives the selftest
    g_stjob = CreateJobObjectW(nullptr, nullptr);
    if (g_stjob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_stjob, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    wchar_t t[MAX_PATH + 2];
    GetTempPathW(MAX_PATH + 1, t);
    st.root = join(canon(t), L"wwe_admit_selftest_" + wnum(GetCurrentProcessId()));
    rmtree(st.root);
    if (!make_dirs(st.root)) { say_out("selftest: cannot create " + u8(st.root) + "\n"); return 1; }
    st.helper = find_up(dir_of(g_self), L"tools\\admit\\lock_interop.py");
    SetEnvironmentVariableW(L"WWE_ALONE_FILE", join(st.root, L"alone_none.lock").c_str());   // S106 C5: a real alone run's pause must not hold the selftest's asks off
    say_out("admit selftest (temp " + u8(st.root) + (g_target != g_self ? ", TARGET " + u8(g_target) : "") + (st.plant ? ", PLANTED: " + st.plant_name : "") + ")\n");
    double t0 = qpc_s();
    if (only_gate) run_gate_cases(st);
    else if (only_alone) { case52(st); case53(st); case54(st); }
    else if (only_new) run_new_cases(st);
    else {
    case1(st);
    case2(st);
    case3(st);
    case3b(st);
    case4(st);
    if (!st.plant) case4_planted(st);
    case5_11(st);
    case6(st);
    case7(st);
    case8(st);
    case9(st);
    case10(st);
    case11b(st);
    case12(st);
    case13(st);
    case14(st);
    case15(st);
    case16(st);
    case17(st, false);
    case17(st, true);
    case18(st);
    case19(st);
    case20(st);
    case21(st);
    case22(st);
    case23(st);
    case24(st);
    case25(st);
    case26(st);
    case27(st);
    case28(st);
    case29(st);
    case30(st);
    case31(st);
    case32(st);
    case33(st);
    case35(st);
    case36(st);
    case37(st);
    case38(st);
    case39(st);
    case40(st);
    case41(st);
    case42(st);
    case43(st);
    run_new_cases(st);
    if (!g_no_real) case_overhead(st);
    }
    if (!st.hist_line.empty()) say_out("history (case 1, first row):\n" + st.hist_line + "\n");
    say_out(fmt("selftest: %d of %d cases passed in %.1f s%s\n", st.cases - st.fails, st.cases, qpc_s() - t0, st.fails ? "  -> FAIL" : "  -> PASS"));
    if (g_stjob) { TerminateJobObject(g_stjob, 1); CloseHandle(g_stjob); g_stjob = nullptr; }   // pass or fail: nothing of ours left running
    Sleep(200);
    if (!keep) rmtree(st.root);
    return st.fails ? 1 : 0;
}

// ---------------------------------------------------------------- main
static void usage() {
    say_err("admit: the compile admission gate (docs\\agents\\admit.md)\n"
            "  admit run --kind K [--reserve-gb X] [--reserve-commit-gb Y] [--timeout S] [--state DIR] [--config F] -- PROGRAM ARGS...\n"
            "  admit hold --kind K --want N [--min M] [--per-gb X] [--per-commit-gb Y] [--timeout S] [--max-s S] [--pid P [--detach]]\n"
            "  admit release --pid P [--kind K]\n"
            "  admit status [--state DIR] [--config F]\n"
            "  admit selftest [--keep] [--target EXE] [--plant no_pending|no_expiry] [--only-gate] [--no-real]");
}
int wmain(int argc, wchar_t** argv) {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    g_self = wstring(buf, n);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    if (argc < 2) { usage(); return 2; }
    wstring sub = argv[1];
    if (sub == L"run") return cmd_run();
    if (sub == L"hold") return cmd_hold();
    if (sub == L"release") return cmd_release();
    if (sub == L"status") return cmd_status();
    if (sub == L"selftest") return cmd_selftest(argc, argv);
    if (sub == L"capabilities") { say_out("alone_pause 2\n"); return 0; }   // S106 C5F note 5: what this exe honours (runlock.ps1 -Alone asks; an older exe answers its usage, exit 2)
    if (sub == L"__wait") return child_wait(argc, argv);
    if (sub == L"__echo" && argc >= 3) return child_echo(argv[2]);
    if (sub == L"__count" && argc >= 3) return child_count(argv[2]);
    if (sub == L"__touch" && argc >= 3) return write_file(argv[2], "x") ? 0 : 3;
    if (sub == L"__parent") return child_parent();
    if (sub == L"__later") return child_later();
    if (sub == L"__jobinfo" && argc >= 3) return child_jobinfo(argv[2]);
    if (sub == L"__memcost") return child_memcost();
    if (sub == L"__alloc" && argc >= 4) return child_alloc(_wtoi(argv[2]), _wtoi(argv[3]));
    usage();
    return 2;
}
