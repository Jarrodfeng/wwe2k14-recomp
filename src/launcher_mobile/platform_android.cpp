// wwe2k14 phone launcher - the phone's platform (Session 71): every call goes to LauncherActivity.java over JNI (the
// j* methods there), and LauncherActivity answers through nativeEvent below. The calls run on SDL's main thread, which
// SDL has attached to Java; LauncherActivity moves anything that needs Android's UI thread there itself.
#include <SDL3/SDL.h>
#include <jni.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>

#include "mobile_platform.h"

namespace mlauncher::platform {

namespace {

std::string g_version;  // for the update check's answer
fs::path g_files;

// The activity and its class for one call (local references, released at the end).
struct Java {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  jclass cls = env && activity ? env->GetObjectClass(activity) : nullptr;
  ~Java() {
    if (!env) return;
    if (env->ExceptionCheck()) env->ExceptionDescribe(), env->ExceptionClear();
    if (cls) env->DeleteLocalRef(cls);
    if (activity) env->DeleteLocalRef(activity);
  }
  explicit operator bool() const { return cls != nullptr; }
  jmethodID Method(const char* name, const char* sig) {
    jmethodID m = env->GetMethodID(cls, name, sig);
    if (!m) {
      env->ExceptionClear();
      SDL_Log("wwe2k14 launcher: LauncherActivity.%s%s missing", name, sig);
    }
    return m;
  }
  jstring Str(const std::string& s) { return env->NewStringUTF(s.c_str()); }
  jobjectArray Strings(const std::vector<std::string>& v) {
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray a = env->NewObjectArray(jsize(v.size()), string_class, nullptr);
    for (size_t i = 0; i < v.size(); ++i) {
      jstring s = Str(v[i]);
      env->SetObjectArrayElement(a, jsize(i), s);
      env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(string_class);
    return a;
  }
  std::string Utf8(jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
  }
  // A void method taking one string.
  void CallStr(const char* name, const std::string& arg) {
    if (!*this) return;
    if (jmethodID m = Method(name, "(Ljava/lang/String;)V")) {
      jstring s = Str(arg);
      env->CallVoidMethod(activity, m, s);
      env->DeleteLocalRef(s);
    }
  }
  void CallVoid(const char* name) {
    if (!*this) return;
    if (jmethodID m = Method(name, "()V")) env->CallVoidMethod(activity, m);
  }
};

int g_game_fd = -1;  // the launcher's own read of the picked game file

// The newest Android release in GitHub's release list (tags "android-vX.Y.Z"; pre-releases count: the Android builds
// are playtest pre-releases, Jarrod 2026-10-28) against ours. PC releases (vX.Y.Z) are never offered to the phone.
launcher::UpdateInfo ReadAndroidRelease(const std::string& body, const std::string& current) {
  static constexpr const char* kPrefix = "android-";
  auto number = [](const std::string& tag) { return tag.rfind(kPrefix, 0) == 0 ? tag.substr(8) : std::string(); };
  launcher::UpdateInfo u;
  u.state = launcher::UpdateInfo::kLatest;
  try {
    const auto list = nlohmann::json::parse(body);
    std::string best;
    for (const auto& r : list) {
      const std::string tag = r.value("tag_name", "");
      if (r.value("draft", false) || number(tag).empty()) continue;
      if (best.empty() || launcher::IsNewer(number(tag), number(best))) {
        best = tag;
        u.tag = tag, u.name = r.value("name", ""), u.url = r.value("html_url", ""), u.notes = r.value("body", "");
      }
    }
    if (!best.empty() && launcher::IsNewer(number(best), number(current))) u.state = launcher::UpdateInfo::kAvailable;
  } catch (const std::exception&) {
    u.state = launcher::UpdateInfo::kFailed, u.error = "GitHub's answer could not be read";
  }
  if (u.state == launcher::UpdateInfo::kAvailable && u.url.rfind("https://github.com/", 0) != 0)
    u.state = launcher::UpdateInfo::kFailed, u.error = "unexpected release address";
  return u;
}

}  // namespace

void Init(int, char**) {
  Java j;
  if (j) {
    if (jmethodID m = j.Method("jFilesDir", "()Ljava/lang/String;")) {
      jstring s = static_cast<jstring>(j.env->CallObjectMethod(j.activity, m));
      g_files = fs::u8path(j.Utf8(s));
      if (s) j.env->DeleteLocalRef(s);
    }
  }
  if (g_files.empty()) g_files = fs::u8path(SDL_GetAndroidExternalStoragePath() ? SDL_GetAndroidExternalStoragePath() : ".");
#ifdef WWE2K14_VERSION
  g_version = WWE2K14_VERSION;
#endif
}

fs::path FilesDir() { return g_files; }
const char* Name() { return "android"; }
// liblauncher.so sits in the app's native library folder: its own path tells where (no JNI call needed).
fs::path NativeLibDir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&NativeLibDir), &info) || !info.dli_fname) return {};
  return fs::path(info.dli_fname).parent_path();
}

fs::path DriversDir() {
  static fs::path dir;  // asked once: it does not change while the app runs
  if (dir.empty()) {
    Java j;
    if (j)
      if (jmethodID m = j.Method("jDriversDir", "()Ljava/lang/String;")) {
        jstring s = static_cast<jstring>(j.env->CallObjectMethod(j.activity, m));
        dir = fs::u8path(j.Utf8(s));
        if (s) j.env->DeleteLocalRef(s);
      }
  }
  return dir;
}

void ImportDriver(const fs::path& drivers_dir) { Java().CallStr("jImportDriver", U8(drivers_dir)); }

void PickGameFile() { Java().CallVoid("jPickGame"); }

std::string OpenGameFile(const std::string& address) {
  if (address.rfind("content://", 0) != 0) {  // a plain path (the USB folder)
    std::error_code ec;
    return fs::exists(fs::u8path(address), ec) ? address : std::string();
  }
  if (g_game_fd >= 0) close(g_game_fd), g_game_fd = -1;
  Java j;
  if (!j) return {};
  jmethodID m = j.Method("jOpenFd", "(Ljava/lang/String;)I");
  if (!m) return {};
  jstring s = j.Str(address);
  g_game_fd = j.env->CallIntMethod(j.activity, m, s);
  j.env->DeleteLocalRef(s);
  if (g_game_fd < 0) return {};
  return "/proc/self/fd/" + std::to_string(g_game_fd);
}

void AddDlc(const fs::path& dlc_dir) { Java().CallStr("jAddDlc", U8(dlc_dir)); }

void Share(const std::vector<fs::path>& files) {
  Java j;
  if (!j) return;
  std::vector<std::string> paths;
  for (const auto& f : files) paths.push_back(U8(f));
  if (jmethodID m = j.Method("jShare", "([Ljava/lang/String;)V")) {
    jobjectArray a = j.Strings(paths);
    j.env->CallVoidMethod(j.activity, m, a);
    j.env->DeleteLocalRef(a);
  }
}

void ExportSaves(const fs::path& save_dir) { Java().CallStr("jExportSaves", U8(save_dir)); }
void ImportSaves(const fs::path& save_dir) { Java().CallStr("jImportSaves", U8(save_dir)); }

void CheckForUpdate(const std::string& version) {
  g_version = version;
  // The repo's newest releases (the PC's and the Android port's share one list); ReadAndroidRelease picks the
  // Android ones. "latest" would be the PC's full release.
  Java().CallStr("jCheckUpdate", "https://api.github.com/repos/Jarrodfeng/wwe2k14-recomp/releases?per_page=30");
}

void OpenUrl(const std::string& url) { Java().CallStr("jOpenUrl", url); }

void StartGame(const std::vector<std::string>& args, const std::string& game_address, const TouchChoice& touch) {
  Java j;
  if (!j) return;
  jmethodID m = j.Method("jStartGame", "([Ljava/lang/String;Ljava/lang/String;[I)V");
  if (!m) return;
  jobjectArray a = j.Strings(args);
  jstring g = j.Str(game_address);
  const jint t[4] = {touch.mode, touch.size, touch.opacity, touch.vibrate ? 1 : 0};
  jintArray ta = j.env->NewIntArray(4);
  j.env->SetIntArrayRegion(ta, 0, 4, t);
  j.env->CallVoidMethod(j.activity, m, a, g, ta);
  j.env->DeleteLocalRef(ta);
  j.env->DeleteLocalRef(g);
  j.env->DeleteLocalRef(a);
}

void EditTouchLayout(int size, int opacity) {
  Java j;
  if (!j) return;
  jmethodID m = j.Method("jEditTouch", "([I)V");
  if (!m) return;
  const jint t[2] = {size, opacity};
  jintArray ta = j.env->NewIntArray(2);
  j.env->SetIntArrayRegion(ta, 0, 2, t);
  j.env->CallVoidMethod(j.activity, m, ta);
  j.env->DeleteLocalRef(ta);
}

void Quit() { Java().CallVoid("jQuit"); }

void ScreenSize(int& w, int& h) {
  w = h = 0;
  Java j;
  if (!j) return;
  jmethodID m = j.Method("jScreenSize", "()[I");
  if (!m) return;
  jintArray a = static_cast<jintArray>(j.env->CallObjectMethod(j.activity, m));
  if (!a) return;
  jint v[2] = {};
  if (j.env->GetArrayLength(a) == 2) j.env->GetIntArrayRegion(a, 0, 2, v);
  j.env->DeleteLocalRef(a);
  w = std::max(v[0], v[1]), h = std::min(v[0], v[1]);
}

}  // namespace mlauncher::platform

// LauncherActivity.nativeEvent(int kind, boolean ok, String a, String b): kind is Event::Kind; for kUpdate, a is
// GitHub's answer and b its HTTP status.
extern "C" JNIEXPORT void JNICALL Java_io_github_jarrodfeng_wwe2k14_LauncherActivity_nativeEvent(
    JNIEnv* env, jclass, jint kind, jboolean ok, jstring a, jstring b) {
  using namespace mlauncher::platform;
  auto utf8 = [env](jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
  };
  Event e;
  e.kind = static_cast<Event::Kind>(kind);
  e.ok = ok, e.a = utf8(a), e.b = utf8(b);
  if (e.kind == Event::kUpdate) {
    const int status = std::atoi(e.b.c_str());
    if (status == 404) e.update.state = launcher::UpdateInfo::kLatest;
    else if (status == 200) e.update = ReadAndroidRelease(e.a, g_version);
    else e.update.state = launcher::UpdateInfo::kFailed, e.update.error = status ? "GitHub answered " + e.b : "GitHub could not be reached";
    e.a.clear();
  }
  Post(std::move(e));
}
