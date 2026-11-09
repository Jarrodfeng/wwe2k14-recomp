// Session 98 (Arena Viewer phone port, stage C): the MODS panel's platform pieces on the phone (ui_mods.h RegisterModsPlatform; the
// Windows ones are mods_win32.cpp). No video frame decode and no folder window here: the panel says so where it would use them
// (mods_lookup.h answers "no video decoder on this platform"). The frame cache is the app's own files folder (never the mods folder
// or the game files).
// Session 98 (phone test: "the character importing appears greyed out"): the mods folder is <files>/mods (viewer_main.cpp), and
// IMPORT CHARACTER uses Android's own file picker (ViewerActivity.jPickCharacter, the Storage Access Framework): Java copies the
// picked file (a content:// stream) to <files>/viewer/import/<its name>, and this side hands that path to the same import as the PC
// (char_mods.h Import: a .pac, or the .pac members of a .zip, copied into <mods>/characters). The copy in viewer/import is a
// stand-in for the file the PC reads in place; it is removed at the next pick and at the next start.
// REMOVE on the phone: there is no Recycle Bin, so the file moves to <files>/viewer/removed/ (the last removed copy of each name).
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <jni.h>

#include "../mods_lookup.h"
#include "ui_mods.h"

namespace view {
namespace ui {

namespace {
namespace fs = std::filesystem;

fs::path FilesDir() {
  const char* f = std::getenv("WWE2K14_FILES_DIR");
  return f && *f ? fs::u8path(f) : fs::path();
}
fs::path ImportDir() { return FilesDir().empty() ? fs::path() : FilesDir() / "viewer" / "import"; }
fs::path RemovedDir() { return FilesDir().empty() ? fs::path() : FilesDir() / "viewer" / "removed"; }

void ClearImportDir() {
  std::error_code ec;
  if (!ImportDir().empty()) fs::remove_all(ImportDir(), ec);
}

// The picker's answer (ViewerActivity.nativeCharPicked, on a Java thread) for the waiting SDL main thread. Each pick has its own
// request id; an answer to another id (a pick that timed out here, then finished in Java) is ignored.
std::mutex g_mu;
std::condition_variable g_cv;
int g_want = 0;  // the id being waited for (0 = none)
bool g_done = false;
std::string g_path, g_err;

// kind 0 = one character file (into viewer/import, cleared at each pick); Session 98 (Superstar Importer): 1 = several character files,
// 2 = a PNG / JPG picture, both into their own new folder <files>/importer/picked/<n> (kept: a pack's source pac must outlive the next
// pick). The answer for kind 1 is the paths joined by '\n'.
std::string PickFiles(int kind) {
  static int next_id = 0;
  assets::mods_lookup::Plat().pick_error.clear();
  if (ImportDir().empty()) return {};
  fs::path dest = ImportDir();
  std::error_code ec;
  if (kind == 0) ClearImportDir();
  else dest = FilesDir() / "importer" / "picked" / std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
  fs::create_directories(dest, ec);
  const int id = ++next_id;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_want = id, g_done = false, g_path.clear(), g_err.clear();
  }
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !act) return {};
  jclass cls = env->GetObjectClass(act);
  jmethodID m = cls ? env->GetMethodID(cls, "jPickFiles", "(Ljava/lang/String;II)V") : nullptr;
  bool called = false;
  if (m) {
    const std::string dir = assets::mods_lookup::U8(dest);
    jstring s = env->NewStringUTF(dir.c_str());
    env->CallVoidMethod(act, m, s, jint(id), jint(kind));
    env->DeleteLocalRef(s);
    called = !env->ExceptionCheck();
  }
  if (env->ExceptionCheck()) env->ExceptionDescribe(), env->ExceptionClear();
  if (cls) env->DeleteLocalRef(cls);
  env->DeleteLocalRef(act);
  if (!called) {
    SDL_Log("wwe2k14 viewer: ViewerActivity.jPickFiles missing");
    assets::mods_lookup::Plat().pick_error = "the file picker could not be opened";
    return {};
  }
  // The picker is Android's own activity over this one; this (SDL's main) thread waits for its answer. The UI thread is never blocked.
  std::unique_lock<std::mutex> lk(g_mu);
  const bool answered = g_cv.wait_for(lk, std::chrono::minutes(15), [] { return g_done; });
  g_want = 0;
  if (!answered) {
    assets::mods_lookup::Plat().pick_error = "the file picker timed out";
    return {};
  }
  if (!g_err.empty()) SDL_Log("wwe2k14 viewer: character pick: %s", g_err.c_str());
  assets::mods_lookup::Plat().pick_error = g_err;
  return g_path;
}

std::string PickCharacter() { return PickFiles(0); }
std::string PickImage() { return PickFiles(2); }
std::vector<std::string> PickCharacters() {
  std::vector<std::string> v;
  const std::string all = PickFiles(1);
  size_t a = 0;
  while (a < all.size()) {
    size_t b = all.find('\n', a);
    if (b == std::string::npos) b = all.size();
    if (b > a) v.push_back(all.substr(a, b - a));
    a = b + 1;
  }
  return v;
}

// A Java string as real UTF-8 (GetStringUTFChars gives modified UTF-8: characters past U+FFFF as two 3-byte halves).
std::string Utf8(JNIEnv* env, jstring s) {
  std::string o;
  if (!s) return o;
  const jsize n = env->GetStringLength(s);
  const jchar* c = env->GetStringChars(s, nullptr);
  if (!c) return o;
  for (jsize i = 0; i < n; ++i) {
    uint32_t cp = c[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && c[i + 1] >= 0xDC00 && c[i + 1] < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (c[++i] - 0xDC00);
    else if (cp >= 0xD800 && cp < 0xE000) cp = '_';  // a lone half
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) o += char(0xC0 | (cp >> 6)), o += char(0x80 | (cp & 0x3F));
    else if (cp < 0x10000) o += char(0xE0 | (cp >> 12)), o += char(0x80 | ((cp >> 6) & 0x3F)), o += char(0x80 | (cp & 0x3F));
    else o += char(0xF0 | (cp >> 18)), o += char(0x80 | ((cp >> 12) & 0x3F)), o += char(0x80 | ((cp >> 6) & 0x3F)), o += char(0x80 | (cp & 0x3F));
  }
  env->ReleaseStringChars(s, c);
  return o;
}

bool MoveToRemoved(const fs::path& file, std::string& err) {
  if (RemovedDir().empty()) return err = "no app folder", false;
  std::error_code ec;
  fs::create_directories(RemovedDir(), ec);
  const fs::path to = RemovedDir() / file.filename();
  fs::remove(to, ec);
  ec.clear();
  fs::rename(file, to, ec);
  if (ec) return err = "could not move it: " + ec.message(), false;
  return true;
}
}  // namespace

void RegisterModsPlatform() {
  assets::mods_lookup::Platform& p = assets::mods_lookup::Plat();
  if (FilesDir().empty()) return;
  p.cache_dir = FilesDir() / "viewer" / "mod_frames";
  ClearImportDir();  // (a copy left by the last run)
  p.pick_character = PickCharacter;
  p.pick_characters = PickCharacters;  // Session 98 (Superstar Importer)
  p.pick_image = PickImage;
  p.recycle = MoveToRemoved;
  p.recycle_name = "the app's removed folder";
}

}  // namespace ui
}  // namespace view

// ViewerActivity.nativeCharPicked(id, path, error): the picked file's copy ("" = cancelled or failed; error says why).
extern "C" JNIEXPORT void JNICALL Java_io_github_jarrodfeng_wwe2k14_ViewerActivity_nativeCharPicked(JNIEnv* env, jclass, jint id, jstring path, jstring err) {
  const std::string p = view::ui::Utf8(env, path), e = view::ui::Utf8(env, err);
  {
    std::lock_guard<std::mutex> lk(view::ui::g_mu);
    if (id != view::ui::g_want || view::ui::g_want == 0) {
      SDL_Log("wwe2k14 viewer: character pick answer %d ignored (waiting for %d)", int(id), view::ui::g_want);
      return;
    }
    view::ui::g_path = p, view::ui::g_err = e, view::ui::g_done = true;
  }
  view::ui::g_cv.notify_all();
}
