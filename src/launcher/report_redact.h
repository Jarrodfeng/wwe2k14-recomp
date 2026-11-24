// wwe2k14 launchers - the ONE redaction both problem reports use (S108 lane MOB): the PC launcher (launcher_main.cpp Redact) and the phone launcher
// (launcher_mobile\mobile_main.cpp ShareZip, the diagnostic zip, the one-by-one fallback). Standard C++ only: no Windows, no Android. The platform says
// what to hide by filling `Rules`; this file knows no environment variable and no folder. Unit: tools\report\redact_unit.cpp (run_redact_unit.py).
//
// What it hides
//   PC (profile set):   the profile folder (C:\Users\NAME -> %USERPROFILE%, also lower-case and with / slashes) and the user name as a whole word anywhere
//                       -> USER (these two passes are the launcher's old Redact, byte for byte); another user's profile (X:\Users\OTHER\... -> OTHERUSER;
//                       Public / Default / All Users stay); a UNC path (\\host\share\dir\file -> \\<host>\<share>\file); an email-shaped string.
//   phone:              the app's and the shared-storage paths (/data/user/N/<pkg>/..., /data/data/<pkg>/..., /storage/emulated/N/..., /storage/<volume id>/...,
//                       /sdcard/..., /mnt/user/N/...) -> <path>/NAME (the last name kept, a folder's too: .../NAME/ -> <path>/NAME/; a bare root -> <path>);
//                       content:// addresses -> content://AUTHORITY/<uri>/NAME;
//                       an email-shaped string; a MAC address; an IPv4 address (not 0.x.x.x, not 127.x.x.x).
//   NOT hidden: hardware and build names (maker, model, SoC, GPU, driver, Android version, RAM, display): they identify hardware, not a person. IPv4 is
//   off on the PC on purpose: a driver version (23.40.27.6) has the shape of one.
// Every pass is one linear scan (no regex); Redact(Redact(x)) == Redact(x); RedactFile does a file of any size in ~2 MiB of memory.
// A BINARY file (S108 review, D1: last_game_exit_trace.bin, a native crash's tombstone in protobuf form, or an ANR's text traces) takes
// RedactBinary / RedactFileBinary instead: the same path, content-uri, email and MAC rules (IPv4 off: random bytes can look like one), but each
// hidden span is overwritten IN PLACE with filler of the same byte length ("<path>xxxx/NAME"), so no length prefix or offset moves and the file's
// size never changes; a span is only taken inside text (printable ASCII / valid UTF-8). A file that parses as protobuf is walked field by field,
// so each string is masked on its own (a printable tag or length byte next to it is never part of a span); anything else is masked run by run of
// text bytes. KindOf picks the pass by an explicit list of text types (as the PC's diagnostic zip, launcher_main.cpp DiagMakeZip).
#pragma once

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace report_redact {

struct Rules {
  std::string profile;  // PC: C:\Users\NAME ("" = no profile passes)
  bool other_profiles = false, unc = false, email = false;
  bool android_paths = false, uris = false, ips = false, macs = false;
};

// One hidden span of a pass's input: [b, e) and what the text redaction writes in its place (the binary pass fits it to e - b bytes).
struct Span {
  size_t b, e;
  std::string rep;
};

inline Rules PcRules(std::string profile) {
  Rules r;
  r.profile = std::move(profile), r.other_profiles = r.unc = r.email = true;
  return r;
}
inline Rules PhoneRules() {
  Rules r;
  r.android_paths = r.uris = r.email = r.ips = r.macs = true;
  return r;
}

namespace detail {

inline std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
inline bool Alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
inline bool Alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
inline bool Digit(char c) { return c >= '0' && c <= '9'; }
inline bool Hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// ---- the PC's two old passes, verbatim from launcher_main.cpp (S25 release check: "C--Users-NAME-..." in a temp folder name)
inline std::string Profile(std::string t, const std::string& prof) {
  for (const std::string& needle : {prof, Lower(prof), [&] {  // also with forward slashes
         std::string x = prof;
         for (char& c : x)
           if (c == '\\') c = '/';
         return x;
       }()}) {
    if (needle.size() < 4) continue;
    for (size_t at = 0; (at = t.find(needle, at)) != std::string::npos;) t.replace(at, needle.size(), "%USERPROFILE%");
  }
  // The user name as a whole word anywhere (case-insensitive). Bounded by non-alphanumerics so a short name doesn't eat parts of ordinary words.
  const size_t slash = prof.find_last_of('\\');
  const std::string user = Lower(slash == std::string::npos ? prof : prof.substr(slash + 1));
  if (user.size() >= 3) {
    const std::string low = Lower(t);
    std::string out;
    size_t from = 0;
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (size_t at = low.find(user); at != std::string::npos; at = low.find(user, at + 1)) {
      const bool left = at == 0 || !word(low[at - 1]), right = at + user.size() >= low.size() || !word(low[at + user.size()]);
      if (!left || !right || at < from) continue;
      out.append(t, from, at - from).append("USER");
      from = at + user.size();
    }
    t = out.append(t, from, std::string::npos);
  }
  return t;
}

// ---- another user's profile: <drive>:\Users\NAME (also / and JSON's doubled backslashes) -> NAME = OTHERUSER
inline std::string OtherProfiles(const std::string& t) {
  if (t.size() < 12) return t;
  const std::string low = Lower(t);
  std::string out;
  size_t from = 0;
  for (size_t at = low.find("users"); at != std::string::npos; at = low.find("users", at + 1)) {
    if (at < from || at == 0) continue;  // (S108 review: nothing before index 0 to read; a text that starts with "Users" is not a drive path)
    size_t before = 0;  // the separator before "Users": 1 or 2 characters
    if (low[at - 1] == '/') before = 1;
    else if (low[at - 1] == '\\') before = (at >= 2 && low[at - 2] == '\\') ? 2 : 1;
    if (!before || at < before + 2 || low[at - before - 1] != ':' || !Alpha(low[at - before - 2])) continue;
    if (at >= before + 3 && Alnum(low[at - before - 3])) continue;  // the drive letter is a whole word ("xC:\Users")
    size_t n0 = at + 5;
    if (n0 < low.size() && low[n0] == '/') n0 += 1;
    else if (n0 + 1 < low.size() && low[n0] == '\\' && low[n0 + 1] == '\\') n0 += 2;
    else if (n0 < low.size() && low[n0] == '\\') n0 += 1;
    else continue;
    size_t e = n0;
    while (e < t.size() && std::string("\\/\"'<>|*?:\r\n\t").find(t[e]) == std::string::npos && t[e] != '\0') ++e;
    size_t name_end = e;
    if (!(e < t.size() && (t[e] == '\\' || t[e] == '/'))) {  // not followed by a folder: a space ends the name
      const size_t sp = t.find(' ', n0);
      if (sp != std::string::npos && sp < e) name_end = sp;
    }
    if (name_end == n0 || name_end - n0 > 64) continue;
    const std::string name = low.substr(n0, name_end - n0);
    if (name == "public" || name == "default" || name == "default user" || name == "all users" || name == "user" || name == "otheruser") continue;
    if (name[0] == '%' || name[0] == '<' || name[0] == '$') continue;
    out.append(t, from, n0 - from).append("OTHERUSER");
    from = name_end;
  }
  if (from == 0) return t;
  return out.append(t, from, std::string::npos);
}

// ---- UNC: \\host\share\dir\file -> \\<host>\<share>\file (and JSON's \\\\host\\share)
inline std::string Unc(const std::string& t) {
  if (t.find("\\\\") == std::string::npos) return t;
  std::string out;
  size_t from = 0;
  for (size_t i = t.find('\\'); i != std::string::npos; i = t.find('\\', i)) {
    if (i < from) {
      i = from;
      continue;
    }
    size_t run = i;
    while (run < t.size() && t[run] == '\\') ++run;
    const size_t n = run - i;
    const char prev = i ? t[i - 1] : ' ';
    const bool boundary = i == 0 || std::string(" \t\r\n\"'(=,[").find(prev) != std::string::npos;
    if ((n != 2 && n != 4) || !boundary || run >= t.size() || !Alnum(t[run])) {
      i = run;
      continue;
    }
    const size_t sepn = n == 4 ? 2 : 1;
    size_t h = run;
    while (h < t.size() && (Alnum(t[h]) || t[h] == '.' || t[h] == '-' || t[h] == '_' || t[h] == '$')) ++h;
    const std::string sep(sepn, '\\');
    // the rest of the token (up to a character a path cannot hold)
    size_t e = h;
    while (e < t.size() && static_cast<unsigned char>(t[e]) > ' ' && std::string("\"'<>|*?,;)]}").find(t[e]) == std::string::npos) ++e;
    std::string rep = std::string(n, '\\') + "<host>";
    if (t.compare(h, sepn, sep) == 0 && h + sepn < e) {
      const std::string rest = t.substr(h + sepn, e - h - sepn);  // share\dir\file
      const size_t cut = rest.rfind(sep);
      rep += sep + "<share>";
      if (cut != std::string::npos && cut + sepn < rest.size()) rep += sep + rest.substr(cut + sepn);
    }
    out.append(t, from, i - from).append(rep);
    from = e;
    i = e;
  }
  if (from == 0) return t;
  return out.append(t, from, std::string::npos);
}

// ---- an email-shaped string: local@domain.tld (tld = 2+ letters, not a file extension: "func@module.dll")
inline bool FileExt(const std::string& tld) {
  static const char* const kExt[] = {"dll", "exe", "so", "sys", "pdb", "txt", "log", "json", "cpp", "h", "hpp", "cc", "dat", "bin", "png", "jpg", "csv", "xml", "ini", "zip", "apk", "lib", "obj", "md", "py", "java"};
  const std::string l = Lower(tld);
  for (const char* e : kExt)
    if (l == e) return true;
  return false;
}
inline std::string Emails(const std::string& t, std::vector<Span>* spans = nullptr) {
  if (t.find('@') == std::string::npos) return t;
  std::string out;
  size_t from = 0;
  for (size_t at = t.find('@'); at != std::string::npos; at = t.find('@', at + 1)) {
    size_t b = at;
    while (b > from && at - b < 64 && (Alnum(t[b - 1]) || std::string("._%+-").find(t[b - 1]) != std::string::npos)) --b;
    if (b == at) continue;
    size_t e = at + 1, last_dot = std::string::npos;
    while (e < t.size() && (Alnum(t[e]) || t[e] == '-' || (t[e] == '.' && e + 1 < t.size() && Alnum(t[e + 1])))) {
      if (t[e] == '.') last_dot = e;
      ++e;
    }
    if (last_dot == std::string::npos || e - last_dot - 1 < 2) continue;
    bool letters = true;
    for (size_t k = last_dot + 1; k < e; ++k) letters = letters && Alpha(t[k]);
    if (!letters || FileExt(t.substr(last_dot + 1, e - last_dot - 1))) continue;
    out.append(t, from, b - from).append("<email>");
    if (spans) spans->push_back({b, e, "<email>"});
    from = e;
    at = e - 1;
  }
  if (from == 0) return t;
  return out.append(t, from, std::string::npos);
}

// ---- a MAC address: 6 hex pairs with ':' or '-'
inline std::string Macs(const std::string& t, std::vector<Span>* spans = nullptr) {
  if (t.size() < 17) return t;
  std::string out;
  size_t from = 0;
  for (size_t i = 0; i + 17 <= t.size(); ++i) {
    if (!Hex(t[i]) || (i && (Alnum(t[i - 1]) || t[i - 1] == ':' || t[i - 1] == '-'))) continue;
    if (!Hex(t[i + 1]) || (t[i + 2] != ':' && t[i + 2] != '-')) continue;
    const char sep = t[i + 2];
    bool ok = true;
    for (int g = 0; g < 6 && ok; ++g) {
      const size_t p = i + size_t(g) * 3;
      ok = Hex(t[p]) && Hex(t[p + 1]) && (g == 5 || t[p + 2] == sep);
    }
    if (!ok) continue;
    const size_t e = i + 17;
    if (e < t.size() && (Alnum(t[e]) || ((t[e] == ':' || t[e] == '-') && e + 1 < t.size() && Hex(t[e + 1])))) continue;
    out.append(t, from, i - from).append("<mac>");
    if (spans) spans->push_back({i, e, "<mac>"});
    from = e;
    i = e - 1;
  }
  if (from == 0) return t;
  return out.append(t, from, std::string::npos);
}

// ---- an IPv4 address (not 0.x.x.x / 127.x.x.x)
inline std::string Ips(const std::string& t) {
  if (t.size() < 7) return t;
  std::string out;
  size_t from = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    if (!Digit(t[i]) || (i && (Alnum(t[i - 1]) || t[i - 1] == '.' || t[i - 1] == '_'))) continue;
    size_t p = i;
    int oct[4] = {};
    bool ok = true;
    for (int k = 0; k < 4 && ok; ++k) {
      size_t d = 0;
      int v = 0;
      while (p < t.size() && Digit(t[p]) && d < 4) v = v * 10 + (t[p++] - '0'), ++d;
      ok = d >= 1 && d <= 3 && v <= 255;
      oct[k] = v;
      if (ok && k < 3) ok = p < t.size() && t[p] == '.', ++p;
    }
    if (!ok) {
      while (i < t.size() && (Digit(t[i]) || t[i] == '.')) ++i;  // skip this number-and-dots run
      continue;
    }
    const bool more = p < t.size() && (Alnum(t[p]) || (t[p] == '.' && p + 1 < t.size() && Digit(t[p + 1])));
    if (more || oct[0] == 0 || oct[0] == 127) {
      i = p;
      continue;
    }
    out.append(t, from, i - from).append("<ip>");
    from = p;
    i = p - 1;
  }
  if (from == 0) return t;
  return out.append(t, from, std::string::npos);
}

// ---- Android paths -> <path>/NAME
struct Root {
  const char* prefix;
  int bare;  // a token with this many path parts or fewer is only a root (a user number, a package, a volume id): -> <path>
};
inline int BareDepth(const std::string& tok, const Root& r) {
  if (std::string(r.prefix) == "/storage/") {
    const size_t s = tok.find('/', 9);
    const std::string second = tok.substr(9, s == std::string::npos ? std::string::npos : s - 9);
    return (second == "emulated" || second == "self") ? 3 : 2;
  }
  return r.bare;
}
// `loose` (binary runs only): a path is also taken after a letter, digit, '-', '.' or '_' (inside a binary a length or tag byte sits right before a string
// and can be one of those: the strict boundary would leave the path in; a false hit only costs filler).
inline std::string AndroidPaths(const std::string& t, std::vector<Span>* spans = nullptr, bool loose = false) {
  static const Root kRoots[] = {{"/data/user/", 4}, {"/data/user_de/", 4}, {"/data/data/", 3},   {"/data/media/", 3},      {"/storage/", 3},
                                {"/sdcard", 1},     {"/mnt/sdcard", 2},     {"/mnt/user/", 3},   {"/mnt/media_rw/", 2},    {"/mnt/runtime/", 5},
                                {"/mnt/pass_through/", 5}, {"/mnt/shell/", 3}};
  if (t.find('/') == std::string::npos) return t;
  std::string out;
  size_t from = 0;
  for (size_t i = t.find('/'); i != std::string::npos; i = t.find('/', i + 1)) {
    if (i < from) continue;
    if (i && !loose) {
      const char p = t[i - 1];
      if (Alnum(p) || p == '_' || p == '-' || p == '.') continue;
      if (p == '/' && !(i >= 2 && (t[i - 2] == '/'))) continue;  // "file:///sdcard"
    }
    const Root* hit = nullptr;
    for (const Root& r : kRoots) {
      const size_t n = std::char_traits<char>::length(r.prefix);
      if (t.compare(i, n, r.prefix) == 0) {
        hit = &r;
        break;
      }
    }
    if (!hit) continue;
    // (an apostrophe inside a word, "John's Mix", is part of a name; at a word's edge it is a quote)
    auto stop = [&](size_t k) {
      const char c = t[k];
      if (c == '\'') return !(k && k + 1 < t.size() && Alnum(t[k - 1]) && Alnum(t[k + 1]));
      return static_cast<unsigned char>(c) < ' ' || std::string("\"<>|*?,;)]}`").find(c) != std::string::npos;
    };
    size_t e = i;
    while (e < t.size() && !stop(e) && t[e] != ' ') ++e;
    while (e < t.size() && t[e] == ' ') {  // a folder with a space in its name: the next word holds a '/' (and is not a new absolute path)
      size_t w = e + 1;
      while (w < t.size() && t[w] != ' ' && !stop(w)) ++w;
      if (w == e + 1 || w - e > 256 || t[e + 1] == '/' || t.find('/', e + 1) >= w) break;
      e = w;
    }
    size_t end = e;
    while (end > i + 1 && (t[end - 1] == '.' || t[end - 1] == ':' || t[end - 1] == '!')) --end;
    const std::string tok = t.substr(i, end - i);
    int parts = 0;
    for (size_t k = 0; k < tok.size();) {
      const size_t s = tok.find('/', k);
      if (s == std::string::npos) {
        ++parts;
        break;
      }
      if (s > k) ++parts;
      k = s + 1;
    }
    std::string rep = "<path>";
    if (parts > BareDepth(tok, *hit)) {
      // a folder path ending in '/' keeps its last folder's name and the '/' (S108 review: ".../drivers/v849/" -> "<path>/v849/", the custom driver's name)
      size_t name_end = tok.size();
      while (name_end > 1 && tok[name_end - 1] == '/') --name_end;
      const size_t slash = tok.rfind('/', name_end - 1);
      rep += "/" + tok.substr(slash + 1);
    }
    out.append(t, from, i - from).append(rep);
    if (spans) spans->push_back({i, end, rep});
    from = end;
    i = end - 1;
  }
  if (from == 0) return t;
  return out.append(t, from, std::string::npos);
}

// ---- content://AUTHORITY/... -> content://AUTHORITY/<uri>/NAME
inline std::string Uris(const std::string& t, std::vector<Span>* spans = nullptr) {
  static const std::string kKey = "content://";
  if (t.find(kKey) == std::string::npos) return t;
  std::string out;
  size_t from = 0;
  for (size_t at = t.find(kKey); at != std::string::npos; at = t.find(kKey, at + 1)) {
    if (at < from) continue;
    const size_t auth = at + kKey.size();
    size_t e = auth;
    auto stop = [&](char c) { return static_cast<unsigned char>(c) <= ' ' || std::string("\"'>,;)]}`").find(c) != std::string::npos; };
    while (e < t.size() && !stop(t[e])) ++e;
    const size_t slash = t.find('/', auth);
    if (slash == std::string::npos || slash >= e) continue;  // no path: nothing in it
    if (t.compare(slash + 1, 5, "<uri>") == 0) continue;     // already done
    size_t end = e;
    while (end > slash + 1 && (t[end - 1] == '.' || t[end - 1] == ':' || t[end - 1] == '!')) --end;
    const std::string rest = t.substr(slash + 1, end - slash - 1);
    size_t cut = rest.rfind('/');
    const size_t enc = Lower(rest).rfind("%2f");
    size_t name_at = cut == std::string::npos ? std::string::npos : cut + 1;
    if (enc != std::string::npos && (name_at == std::string::npos || enc + 3 > name_at)) name_at = enc + 3;
    const std::string name = name_at == std::string::npos ? std::string() : rest.substr(name_at);
    out.append(t, from, slash - from).append("/<uri>");
    if (!name.empty()) out.append("/").append(name);
    if (spans) spans->push_back({slash, end, name.empty() ? std::string("/<uri>") : "/<uri>/" + name});
    from = end;
    at = end - 1;
  }
  if (from == 0) return t;
  return out.append(t, from, std::string::npos);
}

// ---- the binary-safe pass (S108 review, D1): same spans as the text passes, each overwritten in place with filler of its own length

// The text character at s[p] (p < e): its byte count, 0 = not text. Text = printable ASCII, a tab, a valid UTF-8 sequence; `lines` also takes \n \r.
inline size_t TextAt(const std::string& s, size_t p, size_t e, bool lines) {
  const unsigned char c = static_cast<unsigned char>(s[p]);
  if ((c >= 0x20 && c < 0x7F) || c == '\t' || (lines && (c == '\n' || c == '\r'))) return 1;
  const size_t n = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
  if (!n || p + n > e) return 0;
  for (size_t k = 1; k < n; ++k)
    if ((static_cast<unsigned char>(s[p + k]) & 0xC0) != 0x80) return 0;
  return n;
}

// The span's text replacement fitted to exactly its own length: the marker ("<path>"), 'x' padding, then the kept tail ("/NAME") where the text
// rule keeps one; a span too short for both: 'x' then the tail (or all 'x').
inline void Fill(std::string& s, const Span& x) {
  const size_t len = x.e - x.b, m = x.rep.find('>');
  const std::string pre = m == std::string::npos ? x.rep : x.rep.substr(0, m + 1);
  std::string suf = m == std::string::npos ? std::string() : x.rep.substr(m + 1);
  if (suf.size() > len) suf.clear();
  const std::string f = pre.size() + suf.size() <= len ? pre + std::string(len - pre.size() - suf.size(), 'x') + suf
                                                       : std::string(len - suf.size(), 'x') + suf;
  s.replace(x.b, len, f);  // f.size() == len: nothing after the span moves
}

// One piece of text (every byte of it text): the phone's passes in the text order, each one's spans filled in place. IPv4 is not run here.
inline void MaskText(std::string& piece, const Rules& r, bool loose = false) {
  std::vector<Span> sp;
  auto pass = [&](std::string (*fn)(const std::string&, std::vector<Span>*)) {
    sp.clear();
    fn(piece, &sp);
    for (const Span& x : sp) Fill(piece, x);
  };
  if (r.uris) pass(Uris);
  if (r.android_paths) {
    sp.clear();
    AndroidPaths(piece, &sp, loose);
    for (const Span& x : sp) Fill(piece, x);
  }
  if (r.email) pass(Emails);
  if (r.macs) pass(Macs);
}

// s[b, e): each run of text bytes (lines are runs too: \n and \r end one) masked on its own; every other byte is left as it is.
inline void MaskRuns(std::string& s, size_t b, size_t e, const Rules& r) {
  for (size_t p = b; p < e;) {
    size_t q = p, n;
    while (q < e && (n = TextAt(s, q, e, false)) != 0) q += n;
    if (q > p) {
      std::string piece = s.substr(p, q - p);
      MaskText(piece, r, true);  // (loose: the bytes before a run are not known to be a boundary)
      s.replace(p, piece.size(), piece);
    }
    p = q < e ? q + 1 : e;
  }
}

// ---- protobuf: a tombstone (Android 12+) is one. s[b, e) as fields; `ld` gets each length-delimited field's content [b, e).
inline bool Varint(const std::string& s, size_t& p, size_t e, uint64_t& v) {
  v = 0;
  for (int k = 0; k < 10 && p < e; ++k) {
    const unsigned char c = static_cast<unsigned char>(s[p++]);
    v |= uint64_t(c & 0x7F) << (7 * k);
    if (!(c & 0x80)) return true;
  }
  return false;
}
inline bool PbFields(const std::string& s, size_t b, size_t e, std::vector<std::pair<size_t, size_t>>* ld) {
  if (b >= e) return false;  // (an empty message is not one)
  for (size_t p = b; p < e;) {
    uint64_t key = 0, v = 0;
    if (!Varint(s, p, e, key) || (key >> 3) == 0) return false;
    switch (key & 7) {
      case 0:
        if (!Varint(s, p, e, v)) return false;
        break;
      case 1:
        if (e - p < 8) return false;
        p += 8;
        break;
      case 5:
        if (e - p < 4) return false;
        p += 4;
        break;
      case 2:
        if (!Varint(s, p, e, v) || v > e - p) return false;
        if (ld) ld->push_back({p, p + size_t(v)});
        p += size_t(v);
        break;
      default:
        return false;  // (groups and unknown wire types: not protobuf as Android writes it)
    }
  }
  return true;
}
inline bool AllText(const std::string& s, size_t b, size_t e) {
  for (size_t p = b, n; p < e; p += n)
    if ((n = TextAt(s, p, e, true)) == 0) return false;
  return true;
}
// Each length-delimited field: text -> masked as one piece (its own bounds: a printable tag or length byte beside it is never in a span); a
// message -> walked; other bytes (a memory dump) -> masked run by run inside the field.
inline void PbMask(std::string& s, size_t b, size_t e, const Rules& r, int depth) {
  std::vector<std::pair<size_t, size_t>> ld;
  PbFields(s, b, e, &ld);
  for (const auto& [fb, fe] : ld) {
    if (fb == fe) continue;
    if (AllText(s, fb, fe)) {
      std::string piece = s.substr(fb, fe - fb);
      MaskText(piece, r);
      s.replace(fb, piece.size(), piece);
    } else if (depth < 32 && PbFields(s, fb, fe, nullptr)) {
      PbMask(s, fb, fe, r, depth + 1);
    } else {
      MaskRuns(s, fb, fe, r);
    }
  }
}

}  // namespace detail

inline std::string Redact(std::string t, const Rules& r) {
  if (!r.profile.empty()) t = detail::Profile(std::move(t), r.profile);
  if (r.other_profiles) t = detail::OtherProfiles(t);
  if (r.unc) t = detail::Unc(t);
  if (r.uris) t = detail::Uris(t);
  if (r.android_paths) t = detail::AndroidPaths(t);
  if (r.email) t = detail::Emails(t);
  if (r.macs) t = detail::Macs(t);
  if (r.ips) t = detail::Ips(t);
  return t;
}

// `in` redacted into `out` in pieces of about 1 MiB cut at a line end (else at a space): memory stays near 2 MiB whatever the file's size.
// false when `in` cannot be read or `out` cannot be written.
inline bool RedactFile(const std::filesystem::path& in, const std::filesystem::path& out, const Rules& r) {
  std::ifstream f(in, std::ios::binary);
  std::ofstream o(out, std::ios::binary | std::ios::trunc);
  if (!f || !o) return false;
  constexpr size_t kBlock = size_t(1) << 20;
  std::string buf, block(kBlock, '\0');
  bool more = true;
  while (more) {
    f.read(block.data(), std::streamsize(kBlock));
    const size_t got = size_t(f.gcount());
    buf.append(block, 0, got);
    more = got == kBlock;
    size_t cut = buf.size();
    if (more) {
      size_t nl = buf.rfind('\n');
      if (nl == std::string::npos && buf.size() >= kBlock) nl = buf.rfind(' ');
      if (nl != std::string::npos) cut = nl + 1;
      else if (buf.size() < 2 * kBlock) continue;  // no line end and no space yet: read on; at 2 MiB it is cut anywhere (nothing to protect in such text)
    }
    const std::string piece = Redact(buf.substr(0, cut), r);
    o.write(piece.data(), std::streamsize(piece.size()));
    buf.erase(0, cut);
  }
  o.flush();
  return bool(o);
}

// ---- binary-safe (S108 review, D1). The result has the input's exact size: only bytes inside a hidden text span differ. The phone's rules
// (paths, content uris, email, MAC); the PC's profile / UNC passes and IPv4 are not run on binary.
inline std::string RedactBinary(std::string s, const Rules& r) {
  // (the walk keeps a list of every string field: over 8 MiB - a tombstone is tens of KB - a file of 2-byte fields would need hundreds of MB, so it is masked by runs)
  if (s.size() <= (size_t(8) << 20) && detail::PbFields(s, 0, s.size(), nullptr)) detail::PbMask(s, 0, s.size(), r, 0);
  else detail::MaskRuns(s, 0, s.size(), r);
  return s;
}

// `in` -> `out` through RedactBinary. Up to 64 MiB the file is held whole (a protobuf is walked as one); a bigger one is masked run by run in
// pieces of about 1 MiB cut after a control byte (a run of text never spans one). false when `in` cannot be read, `out` cannot be written,
// or the sizes would differ.
inline bool RedactFileBinary(const std::filesystem::path& in, const std::filesystem::path& out, const Rules& r) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(in, ec);
  if (ec) return false;
  std::ifstream f(in, std::ios::binary);
  std::ofstream o(out, std::ios::binary | std::ios::trunc);
  if (!f || !o) return false;
  uintmax_t written = 0;
  if (size <= (uintmax_t(64) << 20)) {
    std::string s(size_t(size), '\0');
    f.read(s.data(), std::streamsize(s.size()));
    if (size_t(f.gcount()) != s.size()) return false;
    const std::string m = RedactBinary(std::move(s), r);
    o.write(m.data(), std::streamsize(m.size()));
    written = m.size();
  } else {
    constexpr size_t kBlock = size_t(1) << 20;
    std::string buf, block(kBlock, '\0');
    bool more = true;
    while (more) {
      f.read(block.data(), std::streamsize(kBlock));
      const size_t got = size_t(f.gcount());
      buf.append(block, 0, got);
      more = got == kBlock;
      size_t cut = buf.size();
      if (more) {
        size_t k = buf.size();
        while (k > 0 && (static_cast<unsigned char>(buf[k - 1]) >= 0x20 || buf[k - 1] == '\t')) --k;  // (a tab is text)
        if (k > 0) cut = k;
        else if (buf.size() < 2 * kBlock) continue;  // (2 MiB with no control byte: cut anywhere, as RedactFile does)
      }
      detail::MaskRuns(buf, 0, cut, r);
      o.write(buf.data(), std::streamsize(cut));
      written += cut;
      buf.erase(0, cut);
    }
  }
  o.flush();
  return bool(o) && written == size;
}

// Which pass a file takes, by an explicit list of TEXT types (the PC's diagnostic zip's list, launcher_main.cpp DiagMakeZip); pictures are
// left as they are; everything else (last_game_exit_trace.bin, an unknown type) is binary.
enum class Kind { kText, kPicture, kBinary };
inline Kind KindOf(const std::filesystem::path& p) {
  std::string ext;
  const std::filesystem::path e = p.extension();  // (held: a range-for over a temporary path's native() dangles before C++23)
  for (const auto ch : e.native()) ext += (ch > 0 && ch < 128) ? char(std::tolower(int(ch))) : '?';
  for (const char* t : {".txt", ".json", ".jsonl", ".csv", ".log", ".tsv"})
    if (ext == t) return Kind::kText;
  for (const char* t : {".png", ".jpg", ".jpeg"})
    if (ext == t) return Kind::kPicture;
  return Kind::kBinary;
}
// `in` -> `out` by its kind: text -> RedactFile, binary -> RedactFileBinary, a picture -> copied as it is.
inline bool RedactFileByKind(const std::filesystem::path& in, const std::filesystem::path& out, const Rules& r) {
  switch (KindOf(in)) {
    case Kind::kText: return RedactFile(in, out, r);
    case Kind::kBinary: return RedactFileBinary(in, out, r);
    case Kind::kPicture: break;
  }
  std::error_code ec;
  std::filesystem::copy_file(in, out, std::filesystem::copy_options::overwrite_existing, ec);
  return !ec;
}

}  // namespace report_redact
