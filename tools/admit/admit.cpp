// admit: the memory admission gate every compile on this PC is started through (lane CQA, 2026-11-20; docs\agents\admit.md).
//
// Why (docs\research\harness\CQ2.md): in Session 103 several compiles at once took available memory to 0.2 GB / 0.0 GB, and "LLVM out of
// memory" was the COMMIT limit (RAM + page file = 45.9 GB here; one q refresh at -j 28 committed 48 GB). The older limits each read memory
// once and could not see what a job started a second earlier was about to use. admit keeps one machine-wide record of who holds what
// (OS-locked slot files, like the gate's GPU slots in tools\gate\sched.py GpuSlots) and counts the reservations of jobs younger than the
// pending window as still to come. Reservations per kind are measured (docs\research\harness\CQM.md).
//
//   admit run --kind K [--reserve-gb X] [--reserve-commit-gb Y] [--timeout S] [--on-timeout run|fail] [--state DIR] [--config F] -- PROGRAM ARGS...
//   admit hold --kind K --want N [--min M] [--per-gb X] [--per-commit-gb Y] [--timeout S] [--max-s S]
//        -> stdout "granted M" + "marker V"; held until stdin EOF (a pipe) / the parent exits; a timeout = exit 75 and no "granted"
//   admit hold ... --pid P --detach [--max-s S]   -> the two lines at once; a detached helper holds until P exits, `admit release`, or --max-s
//   admit release --pid P [--kind K] | admit status | admit selftest [--keep] [--target EXE] [--plant no_pending|no_expiry]
//   No shim mode (CQ_ADV claims 2e / c): explicit call sites only.
//
// Never blocks a build: any internal error prints "[admit] ERROR <what>: running without admission" and runs the command.
// Built by tools\admit\build_admit.ps1 (clang++ by absolute path; never through admit).
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
#include <map>
#include <string>
#include <utility>
#include <vector>

using std::string;
using std::vector;
using std::wstring;

static wstring g_self;                 // full path of this exe
static volatile LONG g_child_running = 0;
static const int EXIT_TIMEOUT_FAIL = 75;
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
static wstring default_state() {
    // One fixed folder per Windows user, whichever checkout's exe runs: <LocalAppData>\wwe2k14-admit, outside out\ (which is cleared). The known
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
    unsigned long pid = 0, for_pid = 0;
    unsigned long long for_ct = 0;   // the owner's creation time: a dead owner's pid reused later is not it
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
            hd.for_ct = strtoull(j.str("for_ct", "0").c_str(), nullptr, 10);   // a string: a FILETIME does not fit a double exactly
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
    double pend_p = 0, pend_c = 0;
    string reason;   // memory / commit / slots / lock / senior: also written to the waiter's ticket
    string why;      // the sentence for the waiting line when the reason is slots or senior
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
};
// pending = what live holders have reserved but may not use yet. A HOLD's reservation counts for its whole life (CQA review 4, item 2): a build
// lasts minutes and a running compiler sits near half its peak, so after a minute a second pool would be admitted on a dip and both would peak
// together. Only a `run` record (one process whose use does show up) and a Python holder count for the pending window alone.
static View view_of(const Ctx& c) {
    View v;
    v.hs = scan(c);
    for (auto& h : v.hs) {
        bool counts = c.plant_no_expiry || h.mode.rfind("hold", 0) == 0 || h.age_s < c.cfg.window_s;
        if (!c.plant_no_pending && counts) { v.pend_p += h.phys; v.pend_c += h.commit; }
    }
    return v;
}
// how many workers the numbers allow now (>= 1 = admit); reason says why not
static long long decide(const Ctx& c, const Want& w, const Mem& m, const View& v, string& reason) {
    double bp = m.phys - v.pend_p - c.cfg.floor_phys, bc = m.commit - v.pend_c - c.cfg.floor_commit;
    long long nbp = w.phys > 0 ? (long long)std::floor(bp / w.phys + 1e-9) : w.want;
    long long nbc = w.commit > 0 ? (long long)std::floor(bc / w.commit + 1e-9) : w.want;
    long long budget = std::min<long long>({(long long)w.want, nbp, nbc});
    long long target = v.hs.empty() ? std::max<long long>(w.min, budget) : budget;   // a lone job is admitted at once (with at least its minimum)
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
                     "\"parent\":%lu,\"for_pid\":%lu,\"for_ct\":\"%llu\",\"workers\":%d}",   // the creation time as a string: > 2^53
                     GetCurrentProcessId(), json_esc(w.kind).c_str(), w.phys, w.commit, tick, now_ft(), local_stamp().c_str(), w.mode.c_str(),
                     json_esc(w.prog).c_str(), pp, w.for_pid, pid_created(w.for_pid), (int)t.slots.size());
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
// 1 = admitted (t holds the slots), 0 = wait (s.reason / s.why say why), -1 = internal error (err). force: the timeout-run path, take any one
// slot so the job stays visible to the others (index up to max_slots + 64), whatever the numbers and the queue.
static int attempt(Ctx& c, const Want& w, Taken& t, Snap& s, string& err, bool force) {
    s = Snap{};
    Mem m;
    if (!read_mem(c, m, err)) return -1;   // memory, holders and waiters are read outside the lock
    s.mem = m;
    unsigned long long g0 = read_gen(c);
    View v = view_of(c);
    s.holders = (int)v.hs.size();
    s.pend_p = v.pend_p;
    s.pend_c = v.pend_c;
    long long target = force ? 1 : evaluate(c, w, m, v, s);
    if (target < 1) return 0;
    GLock g = glock_take(c, GLOCK_WAIT_MS, err);
    if (g == GL_ERROR) return -1;
    if (g == GL_BUSY) { s.reason = "lock"; return 0; }
    unsigned long long g1 = read_gen(c);
    if (g1 != g0 && !force) {   // a take since our scan: check again on the current holders
        v = view_of(c);
        s.holders = (int)v.hs.size();
        s.pend_p = v.pend_p;
        s.pend_c = v.pend_c;
        target = evaluate(c, w, m, v, s);
        if (target < 1) { glock_release(c); return 0; }
    }
    int slots = std::max(1, c.cfg.kinds.at(w.kind).slots);
    take_slots(c, w, t, target, force ? slots + 64 : slots);
    if ((int)t.slots.size() < (force ? 1 : w.min)) {   // fewer free slots than the minimum: take none
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
    string extra = s.why.empty() ? "" : "; " + s.why;
    return fmt("[admit] waiting: %s needs %.1f GB + floor %.1f (commit %.1f + %.1f); available %.1f GB, commit left %.1f GB, holders %d (pending %.1f GB, commit %.1f GB)%s",
               w.kind.c_str(), w.phys, c.cfg.floor_phys, w.commit, c.cfg.floor_commit, s.mem.phys, s.mem.commit, s.holders, s.pend_p, s.pend_c, extra.c_str());
}

enum Outcome { ADMITTED, TIMEOUT_RUN, TIMEOUT_FAIL, ERROR_OPEN, NESTED, NESTED_HOLD, OFF, ORPHANED, OBSERVED };
static const char* outcome_name(Outcome o) {
    switch (o) {
        case ADMITTED: return "admitted";
        case TIMEOUT_RUN: return "timeout_run";
        case TIMEOUT_FAIL: return "timeout_fail";
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
};
static void admit_wait(Ctx& c, Want& w, double timeout_s, bool on_timeout_fail, Admission& a) {
    double t0 = qpc_s(), last = 0;
    w.t0_tick = now_tick();
    if (c.test && !getenv_w(L"WWE_ADMIT_TEST_TICK0").empty()) w.t0_tick = wcstoull(getenv_w(L"WWE_ADMIT_TEST_TICK0").c_str(), nullptr, 10);   // selftest: a shared tick
    w.runlock = !getenv_w(L"WWE2K14_RUNLOCK_TOKEN").empty();
    unsigned seed = (unsigned)(GetCurrentProcessId() * 2654435761u) ^ (unsigned)GetTickCount();
    auto rnd = [&seed] { seed = seed * 1103515245u + 12345u; return (double)((seed >> 8) & 0xFFFF) / 65535.0; };
    double limit = timeout_s * (1.0 + 0.10 * rnd());   // +0-10 %: a cohort that started together never times out in one instant
    bool said = false, said_big = false, owner_tried = false, owner_gone = false;
    HANDLE ticket = INVALID_HANDLE_VALUE, owner = nullptr;
    unsigned long owner_pid = 0;
    int tries = 0;
    for (;;) {
        string err;
        int r = attempt(c, w, a.t, a.s, err, false);
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
            say_err(fmt("[admit] ORPHANED: the caller (pid %lu) is gone: not running (exit %d)", owner_pid, EXIT_TIMEOUT_FAIL));
            a.out = ORPHANED;
            break;
        }
        if (ticket == INVALID_HANDLE_VALUE) ticket = ticket_open(c);
        if (ticket != INVALID_HANDLE_VALUE) ticket_write(ticket, w.kind, w.phys * w.min, w.commit * w.min, w.t0_tick, w.runlock, a.s.reason);
        if (!said_big && w.phys * w.min + c.cfg.floor_phys > a.s.mem.total_phys) {
            said_big = true;
            say_err(fmt("[admit] note: %s needs %.1f GB + floor %.1f, more than this PC's %.1f GB: nobody will wait for it", w.kind.c_str(), w.phys * w.min,
                        c.cfg.floor_phys, a.s.mem.total_phys));
        }
        if (el >= 2.0 && !said) { say_err(wait_line(c, w, a.s)); said = true; last = el; }
        else if (said && el - last >= 30.0) { say_err(wait_line(c, w, a.s)); last = el; }
        if (el >= limit) {
            string why = wait_line(c, w, a.s).substr(17);
            if (on_timeout_fail) {
                say_err(fmt("[admit] TIMEOUT after %.0f s: not running (exit %d); %s", timeout_s, EXIT_TIMEOUT_FAIL, why.c_str()));
                a.out = TIMEOUT_FAIL;
                break;
            }
            say_err(fmt("[admit] TIMEOUT after %.0f s: running without admission; %s", timeout_s, why.c_str()));
            // stay visible to the others in an overflow slot; a busy lock is tried again for up to 5 s (CQA review 3, item 6)
            double until = qpc_s() + 5.0;
            for (;;) {
                Snap s2;
                string e2;
                if (attempt(c, w, a.t, s2, e2, true) != 0 || s2.reason != "lock" || qpc_s() > until) break;
                Sleep(100);
            }
            a.out = TIMEOUT_RUN;
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
    int holders = -1, workers = 1;
    bool has_exit = false;
    unsigned long exit_code = 0;
};
static string numf(double v, const char* f = "%.2f") { return v < 0 ? "-" : fmt(f, v); }
static const char* HISTORY_HEADER =
    "time\tkind\tpid\twaited_s\treserve_phys_gb\treserve_commit_gb\tavail_phys_gb\tcommit_left_gb\tholders\toutcome\texit_code\theld_s\tpeak_job_mb\tpeak_scope\tmode\tworkers\tended\t"
    "queue_mode\twould_grant\twould_reason\tprog\n";
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
    line += fmt("%s\t%s\t%lu\t%.3f\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\n", local_stamp().c_str(), r.kind.c_str(), GetCurrentProcessId(),
                r.waited, numf(r.rp).c_str(), numf(r.rc).c_str(), numf(r.avail).c_str(), numf(r.commit).c_str(), r.holders < 0 ? "-" : fmt("%d", r.holders).c_str(),
                r.outcome.c_str(), r.has_exit ? fmt("%lu", r.exit_code).c_str() : "-", numf(r.held, "%.3f").c_str(),
                r.scope == "job" || r.scope == "partial" ? numf(r.peak_mb, "%.1f").c_str() : "-", r.scope.c_str(), r.mode.c_str(), r.workers, r.ended.c_str(),
                r.qmode.c_str(), r.would_grant < 0 ? "-" : fmt("%lld", r.would_grant).c_str(), r.would_reason.c_str(), prog.c_str());
    DWORD n = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &n, nullptr);
    CloseHandle(h);
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
    Opts o = parse_opts(2, {L"--kind", L"--reserve-gb", L"--reserve-commit-gb", L"--timeout", L"--on-timeout", L"--state", L"--config"});
    if (!o.has_tail) {
        say_err("usage: admit run --kind K [--reserve-gb X] [--reserve-commit-gb Y] [--timeout S] [--on-timeout run|fail] -- PROGRAM ARGS...");
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
        hr.outcome = "observed";
        hr.would_grant = would;
        hr.would_reason = a.s.reason;
        return exec_tail(c, w, a, o.tail, hr, true, true);
    }
    double timeout = timeout_opt >= 0 ? timeout_opt : c.cfg.timeout_s;
    wstring ot = o.kv.count(L"--on-timeout") ? o.kv.at(L"--on-timeout") : L"run";
    if (ot != L"run" && ot != L"fail") { say_err("[admit] ERROR --on-timeout " + u8(ot) + " is not run|fail: using run"); ot = L"run"; }
    admit_wait(c, w, timeout, ot == L"fail", a);
    hr.waited = a.waited;
    hr.rp = w.phys;
    hr.rc = w.commit;
    hr.avail = a.s.mem.phys;
    hr.commit = a.s.mem.commit;
    hr.holders = a.s.holders;
    hr.outcome = outcome_name(a.out);
    if (a.out == TIMEOUT_FAIL || a.out == ORPHANED) {   // nothing is started
        hr.has_exit = true;
        hr.exit_code = EXIT_TIMEOUT_FAIL;
        hr.ended = a.out == ORPHANED ? "orphaned" : "timeout_fail";
        history(c, hr);
        return EXIT_TIMEOUT_FAIL;
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
        say_err(line);
    }
    CloseHandle(r);
    if (granted) { CloseHandle(h.h); return 0; }
    DWORD code = 0;
    WaitForSingleObject(h.h, 5000);
    GetExitCodeProcess(h.h, &code);
    CloseHandle(h.h);
    if (code == (DWORD)EXIT_TIMEOUT_FAIL) return EXIT_TIMEOUT_FAIL;
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
            } else {
                double timeout = timeout_opt >= 0 ? timeout_opt : c.cfg.timeout_s;
                w.watch_pid = helper ? w.for_pid : 0;   // a waiting helper's caller is P (its front exits by design); otherwise the parent
                // a hold that times out grants NOTHING: exit 75 and no "granted" line; the caller runs its plain count (CQA addendum A3)
                admit_wait(c, w, timeout, true, a);
                hr.outcome = outcome_name(a.out);
                hr.waited = a.waited;
                hr.avail = a.s.mem.phys;
                hr.commit = a.s.mem.commit;
                hr.holders = a.s.holders;
                if (a.out == TIMEOUT_FAIL || a.out == ORPHANED) {
                    hr.kind = w.kind;
                    hr.has_exit = true;
                    hr.exit_code = EXIT_TIMEOUT_FAIL;
                    hr.workers = 0;
                    hr.ended = a.out == ORPHANED ? "orphaned" : "timeout_fail";
                    history(c, hr);
                    return EXIT_TIMEOUT_FAIL;
                }
                if (a.out == ADMITTED) grant = (int)a.t.slots.size();
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
static unsigned long long file_time(const wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}
static int cmd_status() {
    Opts o = parse_opts(2, {L"--state", L"--config"});
    Ctx c;
    string err;
    setup(c, opt_str(o, L"--state"), opt_str(o, L"--config"), err);
    wstring em = getenv_w(L"WWE_ADMIT_MODE");
    say_out("admit status: mode " + c.cfg.mode + (em == L"observe" || em == L"enforce" ? " (from WWE_ADMIT_MODE)" : c.cfg.path.empty() ? " (default)" : " (from the config)") +
            (c.cfg.mode == "observe" ? ": nothing waits or is narrowed; history logs what enforce would have done\n" : ": askers wait for memory\n"));
    wstring src = c.cfg.path.empty() ? L"" : join(dir_of(c.cfg.path), L"admit.cpp");
    unsigned long long te = file_time(g_self), ts = src.empty() ? 0 : file_time(src);
    say_out("  exe      " + u8(g_self) + (!ts ? ": source admit.cpp not found beside the config" : te < ts ? ": STALE (older than " + u8(src) + "): rebuild with tools\\admit\\build_admit.ps1" : ": up to date with " + u8(src)) + "\n");
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
           C_QMODE, C_WOULD, C_WREASON, C_PROG, C_N };
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
    Case c{"2 a lone job below the floor is admitted at once"};
    wstring sd = set_case(st, L"c2", 24, 60, 900, "1.0 1.0");
    string e;
    long rc = run_admit(st, L"run --kind compile --timeout 5 --on-timeout fail -- cmd /c exit 0", L"c2", &e);
    auto rows = hist_rows(sd);
    c.need(rc == 0, fmt("rc %ld", rc));
    c.need(e.empty(), "stderr: " + e);
    c.need(rows.size() == 1 && rows[0][C_OUTCOME] == "admitted" && atof(rows[0][C_WAITED].c_str()) < 0.1, "not admitted at once");
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
    Case c{"7 timeout: run goes on with the loud line; fail exits 75 and starts nothing"};
    wstring sd = set_case(st, L"c7", 1, 60, 900, "100 100");
    WJ a = wait_job(st, L"c7a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    string e;
    double secs = 0;
    long rc = run_admit(st, L"run --kind compile --timeout 1 --on-timeout run -- cmd /c exit 3", L"c7b", &e, &secs);
    c.need(rc == 3, fmt("timeout-run rc %ld", rc));
    c.need(e.find("[admit] TIMEOUT after 1 s: running without admission") != string::npos, "no TIMEOUT run line: " + e);
    c.need(secs >= 0.9, fmt("timeout-run returned after %.2f s", secs));
    wstring touched = join(st.root, L"c7_touched");
    string e2;
    rc = run_admit(st, L"run --kind compile --timeout 1 --on-timeout fail -- " + q(g_self) + L" __touch " + q(touched), L"c7c", &e2);
    c.need(rc == EXIT_TIMEOUT_FAIL, fmt("timeout-fail rc %ld, want 75", rc));
    c.need(!is_file(touched), "timeout-fail started the command");
    c.need(e2.find("[admit] TIMEOUT after 1 s: not running (exit 75)") != string::npos, "no TIMEOUT fail line: " + e2);
    finish(a);
    auto rows = hist_rows(sd);
    c.need(has_row(rows, C_OUTCOME, "timeout_run") && has_row(rows, C_OUTCOME, "timeout_fail"), "history lacks timeout_run / timeout_fail");
    c.info = first_line(e);
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
    rc = run_admit(st, L"run --kind compile --timeout 20 --on-timeout fail -- cmd /c exit 4", L"c8c", &e);
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
    long rc = run_admit(st, L"run --kind compile --timeout 10 --on-timeout fail -- " + q(g_target) + L" run --kind compile --timeout 10 --on-timeout fail -- " +
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
    Case c{"12 interop with Python msvcrt.locking (the gate's lock), both ways"};
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
    long rc = run_admit(st, L"run --kind compile --timeout 1 --on-timeout fail -- cmd /c exit 0", L"c12a", &e);
    c.need(rc == EXIT_TIMEOUT_FAIL, fmt("admit took the slot python holds (rc %ld)", rc));
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
        long rc = run_admit(st, L"run --kind compile --timeout 3 --on-timeout fail -- cmd /c exit 0", L"c15a", &e);
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
    P p = spawn(q(g_self) + L" __later --go " + g1 + L" -- " + q(g_target) + L" run --kind compile --timeout 3 --on-timeout fail -- " + q(g_self) + L" __count " + q(cnt),
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
    Case c{"28 with LOCALAPPDATA unset the state folder is still <LocalAppData>\\wwe2k14-admit (the known folder)"};
    static const GUID kLocalAppData = {0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
    PWSTR p = nullptr;
    wstring want;
    if (SUCCEEDED(SHGetKnownFolderPath(kLocalAppData, 0, nullptr, &p)) && p) want = join(p, L"wwe2k14-admit");
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
static void case34(ST& st) {
    Case c{"34 a hold that times out exits 75, says TIMEOUT, and prints no 'granted' (both forms)"};
    set_case(st, L"c34", 1, 60, 900, "100 100");
    WJ a = wait_job(st, L"c34a", L"run --kind compile");
    c.need(signalled(a.started, LONG_MS), "A never started");
    SH h = stdin_hold(st, L"hold --kind compile --want 2 --timeout 1", L"c34s");
    long rc = stdin_hold_end(h);
    string e = slurp(join(st.root, L"c34s.err"));
    c.need(rc == EXIT_TIMEOUT_FAIL && h.granted.empty() && e.find("TIMEOUT") != string::npos,
           fmt("stdin form: rc %ld, stdout '%s', stderr '%s'", rc, h.granted.c_str(), first_line(e).c_str()));
    WJ p = plain_wait(st);
    signalled(p.started, LONG_MS);
    string out;
    bool eof = false;
    rc = run_piped(st, L"hold --kind compile --want 2 --timeout 1 --pid " + wnum(p.p.pid) + L" --detach", L"c34d", out, eof);
    string e2 = slurp(join(st.root, L"c34d.err"));
    c.need(rc == EXIT_TIMEOUT_FAIL && out.find("granted") == string::npos && e2.find("TIMEOUT") != string::npos,
           fmt("detached form: rc %ld, stdout '%s', stderr '%s'", rc, first_line(out).c_str(), first_line(e2).c_str()));
    finish(p);
    finish(a);
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
    long rc = run_admit(st, L"run --kind compile --timeout 3 --on-timeout fail -- cmd /c exit 0", L"c36r", &e);
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
    rc = run_piped(st, L"hold --kind parse --want 8 --timeout 2 --pid " + wnum(p2.p.pid) + L" --detach", L"c37b", out, eof);
    c.need(rc == EXIT_TIMEOUT_FAIL && out.find("granted") == string::npos,
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
    bool keep = false;
    g_target = g_self;
    for (int i = 2; i < argc; i++) {
        wstring a = argv[i];
        if (a == L"--keep") keep = true;
        else if (a == L"--target" && i + 1 < argc) g_target = full(argv[++i]);
        else if (a == L"--plant" && i + 1 < argc) { st.plant = true; st.plant_name = u8(argv[++i]); }
    }
    for (const wchar_t* v : {L"WWE_ADMIT_HELD", L"WWE_NO_ADMIT", L"WWE_ADMIT_PLANT", L"WWE_ADMIT_DIR", L"WWE_ADMIT_CONFIG", L"WWE2K14_RUNLOCK_TOKEN"})
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
    say_out("admit selftest (temp " + u8(st.root) + (g_target != g_self ? ", TARGET " + u8(g_target) : "") + (st.plant ? ", PLANTED: " + st.plant_name : "") + ")\n");
    double t0 = qpc_s();
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
    case34(st);
    case35(st);
    case36(st);
    case37(st);
    case38(st);
    case39(st);
    case40(st);
    case41(st);
    case_overhead(st);
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
            "  admit run --kind K [--reserve-gb X] [--reserve-commit-gb Y] [--timeout S] [--on-timeout run|fail] [--state DIR] [--config F] -- PROGRAM ARGS...\n"
            "  admit hold --kind K --want N [--min M] [--per-gb X] [--per-commit-gb Y] [--timeout S] [--max-s S] [--pid P [--detach]]\n"
            "  admit release --pid P [--kind K]\n"
            "  admit status [--state DIR] [--config F]\n"
            "  admit selftest [--keep] [--target EXE] [--plant no_pending|no_expiry]");
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
    if (sub == L"__wait") return child_wait(argc, argv);
    if (sub == L"__echo" && argc >= 3) return child_echo(argv[2]);
    if (sub == L"__count" && argc >= 3) return child_count(argv[2]);
    if (sub == L"__touch" && argc >= 3) return write_file(argv[2], "x") ? 0 : 3;
    if (sub == L"__parent") return child_parent();
    if (sub == L"__later") return child_later();
    if (sub == L"__jobinfo" && argc >= 3) return child_jobinfo(argv[2]);
    if (sub == L"__memcost") return child_memcost();
    usage();
    return 2;
}
