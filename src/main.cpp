#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <EGL/egl.h>
#include <android/native_window.h>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

using EglCreateWindowSurfaceFn =
    EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);

using EglSwapBuffersFn =
    EGLBoolean (*)(EGLDisplay, EGLSurface);

using EglDestroySurfaceFn =
    EGLBoolean (*)(EGLDisplay, EGLSurface);

struct Settings {
    int resolutionPercent = 100;
    int appliedPercent = 100;
};

struct SurfaceState {
    ANativeWindow *window{};
    int baseWidth{};
    int baseHeight{};
};

std::mutex gStateMutex;
std::unordered_map<EGLSurface, SurfaceState> gWindows;
Settings gSettings;
std::filesystem::path gConfigPath;

EglCreateWindowSurfaceFn gCreateWindowSurfaceOriginal = nullptr;
EglSwapBuffersFn gSwapBuffersOriginal = nullptr;
EglDestroySurfaceFn gDestroySurfaceOriginal = nullptr;

pl::memory::HookHandle gCreateHook;
pl::memory::HookHandle gSwapHook;
pl::memory::HookHandle gDestroyHook;

std::atomic_bool gHooksInstalled{false};

bool parseInt(std::string_view text, int &out) {
    if (text.empty()) return false;
    int value{};
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    out = value;
    return true;
}

int clampPercent(int value) {
    return value < 25 ? 25 : value > 100 ? 100 : value;
}

void saveConfigLocked() {
    if (gConfigPath.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(gConfigPath.parent_path(), ec);

    const auto temporary = gConfigPath.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return;
        out << "{\n  \"resolution\": " << gSettings.resolutionPercent << "\n}\n";
    }

    std::filesystem::rename(temporary, gConfigPath, ec);
    if (ec) {
        std::filesystem::remove(gConfigPath, ec);
        ec.clear();
        std::filesystem::rename(temporary, gConfigPath, ec);
        if (ec) std::filesystem::remove(temporary, ec);
    }
}

void loadConfigLocked() {
    if (gConfigPath.empty() || !std::filesystem::exists(gConfigPath)) {
        saveConfigLocked();
        return;
    }

    std::ifstream in(gConfigPath);
    if (!in) return;

    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    const auto keyPos = text.find("\"resolution\"");
    if (keyPos == std::string::npos) return;
    const auto colon = text.find(':', keyPos);
    if (colon == std::string::npos) return;

    std::size_t start = colon + 1;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\n' ||
                                   text[start] == '\r' || text[start] == '\t')) {
        ++start;
    }
    std::size_t finish = start;
    while (finish < text.size() && text[finish] >= '0' && text[finish] <= '9') ++finish;

    int value{};
    if (parseInt(std::string_view(text).substr(start, finish - start), value)) {
        gSettings.resolutionPercent = clampPercent(value);
        gSettings.appliedPercent = gSettings.resolutionPercent;
    }
}

bool validWindow(ANativeWindow *window) {
    return window != nullptr;
}

void applyWindowResolution(ANativeWindow *window, int baseWidth, int baseHeight, int percent) {
    if (!validWindow(window) || baseWidth <= 0 || baseHeight <= 0) return;

    int targetWidth = (baseWidth * percent) / 100;
    int targetHeight = (baseHeight * percent) / 100;
    targetWidth = targetWidth < 1 ? 1 : targetWidth;
    targetHeight = targetHeight < 1 ? 1 : targetHeight;

    (void)ANativeWindow_setBuffersGeometry(window, targetWidth, targetHeight, 0);
}

void applyAllWindows() {
    std::vector<std::tuple<ANativeWindow *, int, int>> windows;
    int percent;
    {
        std::lock_guard lock(gStateMutex);
        gSettings.appliedPercent = gSettings.resolutionPercent;
        percent = gSettings.appliedPercent;
        saveConfigLocked();
        windows.reserve(gWindows.size());
        for (const auto &[surface, state] : gWindows) {
            (void)surface;
            if (state.window) {
                ANativeWindow_acquire(state.window);
                windows.emplace_back(state.window, state.baseWidth, state.baseHeight);
            }
        }
    }

    for (auto &[window, width, height] : windows) {
        applyWindowResolution(window, width, height, percent);
        ANativeWindow_release(window);
    }
}

void captureWindow(EGLSurface surface, ANativeWindow *window) {
    if (surface == EGL_NO_SURFACE || window == nullptr) return;

    std::lock_guard lock(gStateMutex);

    auto it = gWindows.find(surface);
    if (it != gWindows.end()) {
        if (it->second.window == window) return;
        ANativeWindow_release(it->second.window);
        gWindows.erase(it);
    }

    const int baseWidth = ANativeWindow_getWidth(window);
    const int baseHeight = ANativeWindow_getHeight(window);
    ANativeWindow_acquire(window);
    gWindows.emplace(surface, SurfaceState{window, baseWidth, baseHeight});
}

void releaseWindow(EGLSurface surface) {
    if (surface == EGL_NO_SURFACE) return;

    std::lock_guard lock(gStateMutex);
    auto it = gWindows.find(surface);
    if (it == gWindows.end()) return;

    ANativeWindow_release(it->second.window);
    gWindows.erase(it);
}

struct AcquiredWindowState {
    ANativeWindow *window{};
    int baseWidth{};
    int baseHeight{};
    int appliedPercent{};
};

AcquiredWindowState acquireWindowForSurface(EGLSurface surface) {
    std::lock_guard lock(gStateMutex);
    const auto it = gWindows.find(surface);
    if (it == gWindows.end() || it->second.window == nullptr) return {};
    ANativeWindow_acquire(it->second.window);
    return {it->second.window, it->second.baseWidth, it->second.baseHeight,
            gSettings.appliedPercent};
}

EGLSurface createWindowSurfaceDetour(
    EGLDisplay display,
    EGLConfig config,
    EGLNativeWindowType window,
    const EGLint *attributes) {

    if (!gCreateWindowSurfaceOriginal) return EGL_NO_SURFACE;

    const EGLSurface surface =
        gCreateWindowSurfaceOriginal(display, config, window, attributes);

    if (surface != EGL_NO_SURFACE && window != nullptr) {
        auto *nativeWindow = reinterpret_cast<ANativeWindow *>(window);
        captureWindow(surface, nativeWindow);
        std::lock_guard lock(gStateMutex);
        const auto it = gWindows.find(surface);
        if (it != gWindows.end()) {
            applyWindowResolution(nativeWindow, it->second.baseWidth,
                                  it->second.baseHeight, gSettings.appliedPercent);
        }
    }

    return surface;
}

EGLBoolean swapBuffersDetour(EGLDisplay display, EGLSurface surface) {
    auto windowState = acquireWindowForSurface(surface);
    if (windowState.window) {
        applyWindowResolution(windowState.window,
                              windowState.baseWidth,
                              windowState.baseHeight,
                              windowState.appliedPercent);
        ANativeWindow_release(windowState.window);
    }

    return gSwapBuffersOriginal
               ? gSwapBuffersOriginal(display, surface)
               : EGL_FALSE;
}

EGLBoolean destroySurfaceDetour(EGLDisplay display, EGLSurface surface) {
    const EGLBoolean result =
        gDestroySurfaceOriginal
            ? gDestroySurfaceOriginal(display, surface)
            : EGL_FALSE;

    releaseWindow(surface);
    return result;
}

void *eglSymbol(void *library, const char *name) {
    return library ? dlsym(library, name) : nullptr;
}

bool installEglHooks() {
    if (gHooksInstalled.load(std::memory_order_acquire)) return true;

    void *eglLibrary = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
    if (!eglLibrary) {
        eglLibrary = dlopen("libEGL.so", RTLD_NOW);
    }
    if (!eglLibrary) return false;

    const auto createTarget = eglSymbol(eglLibrary, "eglCreateWindowSurface");
    const auto swapTarget = eglSymbol(eglLibrary, "eglSwapBuffers");
    const auto destroyTarget = eglSymbol(eglLibrary, "eglDestroySurface");

    bool ok = createTarget && swapTarget && destroyTarget;

    if (ok) {
        gCreateHook = pl::memory::HookHandle(
            createTarget,
            reinterpret_cast<void *>(createWindowSurfaceDetour),
            reinterpret_cast<void **>(&gCreateWindowSurfaceOriginal));

        gSwapHook = pl::memory::HookHandle(
            swapTarget,
            reinterpret_cast<void *>(swapBuffersDetour),
            reinterpret_cast<void **>(&gSwapBuffersOriginal));

        gDestroyHook = pl::memory::HookHandle(
            destroyTarget,
            reinterpret_cast<void *>(destroySurfaceDetour),
            reinterpret_cast<void **>(&gDestroySurfaceOriginal));

        ok = gCreateHook.installed() && gSwapHook.installed() && gDestroyHook.installed();
    }

    dlclose(eglLibrary);

    if (ok) {
        gHooksInstalled.store(true, std::memory_order_release);
    }

    return ok;
}

void clearState() {
    std::lock_guard lock(gStateMutex);
    for (auto &[surface, state] : gWindows) {
        (void)surface;
        if (state.window) ANativeWindow_release(state.window);
    }
    gWindows.clear();
}

void setResolutionPercent(std::string_view value) {
    int parsed{};
    if (!parseInt(value, parsed)) return;

    std::lock_guard lock(gStateMutex);
    gSettings.resolutionPercent = clampPercent(parsed);
    saveConfigLocked();
}

void registerMenu() {
    pl::modmenu::ModuleBuilder builder(
        "resolutionchanger.resolution",
        "ResolutionChanger");

    builder.description(
              "Scale the Android rendering buffer resolution for optimization.")
        .modId("resolutionchanger")
        .defaultEnabled(true)
        .hideInHudEditor(true)
        .onConfigChanged([](std::string_view, std::string_view key, std::string_view value) {
            if (key == "resolution") {
                setResolutionPercent(value);
            } else if (key == "apply") {
                applyAllWindows();
            }
        })
        .config(
            "resolution",
            "Resolution",
            pl::modmenu::ConfigType::SliderInt,
            std::to_string(gSettings.resolutionPercent),
            "25",
            "100")
        .config(
            "apply",
            "Apply",
            pl::modmenu::ConfigType::Button)
        .registerModule();
}

class ResolutionChangerMod {
public:
    bool load(pl::mod::ModContext &context) {
        {
            std::lock_guard lock(gStateMutex);
            gConfigPath = context.configDir() / "config.json";
            loadConfigLocked();
        }

        const bool hooked = installEglHooks();
        registerMenu();
        return hooked;
    }

    bool enable(pl::mod::ModContext &) {
        return true;
    }

    bool disable(pl::mod::ModContext &) {
        return true;
    }

    bool unload(pl::mod::ModContext &) {
        clearState();
        gCreateHook.reset();
        gSwapHook.reset();
        gDestroyHook.reset();
        gHooksInstalled.store(false, std::memory_order_release);
        return true;
    }
};

} // namespace

PL_REGISTER_MOD(ResolutionChangerMod, ResolutionChangerMod{})
