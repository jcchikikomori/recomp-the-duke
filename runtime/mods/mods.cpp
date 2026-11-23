#include "mods/mods.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace mods {

static_assert(offsetof(RecompModApi, log) == 48, "RecompModApi v5 log ABI offset changed");

namespace {

constexpr size_t kMaxMods = 16;
constexpr size_t kMaxFrame = 16;
constexpr uint32_t kXbeBase = 0x00010000;
constexpr uint32_t kXbeCert = 0x118;
constexpr uint32_t kCertTitleId = 8;
constexpr uint32_t kCertTitleName = 0xC;

struct FrameHook {
    void (*fn)(void *);
    void *user;
};
std::vector<FrameHook> g_frame;

template <class T>
int Add(std::vector<T> &items, size_t limit, const T &item) {
    if (items.size() >= limit) return 0;
    items.push_back(item);
    return 1;
}

uint32_t TitleId() {
    uint32_t cert = kernel::R32(kXbeBase + kXbeCert);
    return cert ? kernel::R32(cert + kCertTitleId) : 0;
}

const char *TitleName() {
    static std::string name;
    static bool read = false;
    if (!read) {
        read = true;
        uint32_t cert = kernel::R32(kXbeBase + kXbeCert);
        for (uint32_t i = 0; cert && i < 40; i++) {
            uint32_t c = kernel::R32(cert + kCertTitleName + i * 2) & 0xFFFF;
            if (!c) break;
            if (c < 0x80) {
                name += char(c);
            } else if (c < 0x800) {
                name += char(0xC0 | c >> 6);
                name += char(0x80 | (c & 0x3F));
            } else {
                name += char(0xE0 | c >> 12);
                name += char(0x80 | (c >> 6 & 0x3F));
                name += char(0x80 | (c & 0x3F));
            }
        }
    }
    return name.empty() ? nullptr : name.c_str();
}

void *GuestPointer(uint32_t va) { return kernel::HostPtr(va); }

int OnFrame(void (*fn)(void *), void *user) { return fn ? Add(g_frame, kMaxFrame, FrameHook{fn, user}) : 0; }

uint32_t VertexConstants(uint32_t, uint32_t, float (*)[4]) { return 0; }

void Log(const char *line) { LOGR("[MOD] %s", line ? line : ""); }

int D3d9OnDevice(RecompModDeviceFn, void *) { return 0; }
int D3d9OnDraw(RecompModBeforeDraw, RecompModAfterDraw, void *) { return 0; }
int D3d9OnClear(RecompModClearFn, void *) { return 0; }
int D3d9OnPresent(RecompModDeviceFn, void *) { return 0; }
int D3d9VertexHlsl(const char *) { return 0; }
int D3d9RenderState(uint32_t, uint32_t *) { return 0; }
void D3d9SetProjection(const float[16]) {}
void D3d9RemakeWrittenTextures(int) {}
void D3d9SetTransform(uint32_t, const float[16]) {}
int D3d9OnCopy(RecompModCopyFn, void *) { return 0; }
uint32_t DrawVertexBuffer(uint32_t *data, uint32_t *stride, uint32_t *first) {
    if (data) *data = 0;
    if (stride) *stride = 0;
    if (first) *first = 0;
    return 0;
}
void D3d9FixedTransform(int, RecompModFixedFn, void *) {}

int Replaying() { return 0; }
int CaptureOnDraw(void (*fn)(void *), void *) { return fn ? 1 : 0; }
void CaptureNote(uint32_t, const void *, uint32_t) {}
int ReplayOnNote(void (*fn)(uint32_t, const void *, uint32_t, void *), void *) { return fn ? 1 : 0; }

RecompModApi MakeApi() {
    RecompModApi api{};
    api.version = RECOMP_MOD_API_VERSION;
    api.size = sizeof(RecompModApi);
    api.title_id = TitleId;
    api.title_name = TitleName;
    api.guest_pointer = GuestPointer;
    api.on_frame = OnFrame;
    api.vertex_constants = VertexConstants;
    api.log = Log;
    api.d3d9_on_device = D3d9OnDevice;
    api.d3d9_on_draw = D3d9OnDraw;
    api.d3d9_on_clear = D3d9OnClear;
    api.d3d9_on_present = D3d9OnPresent;
    api.d3d9_vertex_hlsl = D3d9VertexHlsl;
    api.d3d9_render_state = D3d9RenderState;
    api.d3d9_set_projection = D3d9SetProjection;
    api.d3d9_remake_written_textures = D3d9RemakeWrittenTextures;
    api.d3d9_set_transform = D3d9SetTransform;
    api.replaying = Replaying;
    api.capture_on_draw = CaptureOnDraw;
    api.capture_note = CaptureNote;
    api.replay_on_note = ReplayOnNote;
    api.d3d9_on_copy = D3d9OnCopy;
    api.draw_vertex_buffer = DrawVertexBuffer;
    api.d3d9_fixed_transform = D3d9FixedTransform;
    return api;
}
const RecompModApi g_api = MakeApi();

std::filesystem::path ModsDir() {
    if (const char *named = std::getenv("RECOMP_MOD_LIBS_DIR"); named && *named) return named;
    return app::ExeDir() / "mods";
}

#ifdef _WIN32
constexpr const char *kExt = ".dll";
#else
constexpr const char *kExt = ".so";
#endif

bool LoadOne(const std::filesystem::path &path) {
    std::string name = path.filename().string();
#ifdef _WIN32
    HMODULE module = LoadLibraryW(path.c_str());
    if (!module) {
        LOGR("[MOD] %s: could not load (%lu)", name.c_str(), GetLastError());
        return false;
    }
    auto init = reinterpret_cast<RecompModInitFn>(
        reinterpret_cast<void (*)()>(GetProcAddress(module, "recomp_mod_init")));
    auto unload = [&] { FreeLibrary(module); };
#else
    void *module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        LOGR("[MOD] %s: could not load (%s)", name.c_str(), dlerror());
        return false;
    }
    auto init = reinterpret_cast<RecompModInitFn>(dlsym(module, "recomp_mod_init"));
    auto unload = [&] { dlclose(module); };
#endif
    if (!init) {
        LOGR("[MOD] %s: no recomp_mod_init, not a mod", name.c_str());
        unload();
        return false;
    }
    bool keep = init(&g_api) != 0;
    if (!keep) unload();
    LOGR("[MOD] %s: %s", name.c_str(), keep ? "loaded" : "not for this game, unloaded");
    return keep;
}

}  // namespace

void Load() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char *enabled = std::getenv("RECOMP_MOD_LIBS"); enabled && enabled[0] == '0') return;

    std::filesystem::path dir = ModsDir();
    std::error_code ec;
    std::vector<std::filesystem::path> found;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (entry.is_regular_file(ec) && ext == kExt && found.size() < kMaxMods) found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    size_t kept = 0;
    for (const auto &path : found) kept += size_t(LoadOne(path));
    if (!found.empty()) LOGR("[MOD] %zu of %zu in %s loaded", kept, found.size(), dir.string().c_str());
}

void Frame() {
    for (const FrameHook &hook : g_frame) hook.fn(hook.user);
}

}  // namespace mods
