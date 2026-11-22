// pad 0 from a script, then GameInfo::pad as on Windows
// SCRIPT file: lines of "frame buttons [frames held]", held 8 frames by default, # comments; buttons joined by +:
// UP DOWN LEFT RIGHT START BACK A B X Y LT RT LS LSD LSL LSR (left stick up, down, left, right)
// cmd in the run folder: "buttons [frames held]" pressed once from the frame it is read, then removed
#include <cstdio>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "core/pad_script.h"
#include "harness.h"

namespace input {

namespace {

using pad_script::Press;

std::vector<Press> LoadScript() { return pad_script::Load(std::getenv("SCRIPT")); }

}  // namespace

Pad Poll(int port) {
    static const std::vector<Press> script = LoadScript();
    static Press live{-1, "", 0};
    Pad p;
    p.connected = port == 0;
    double t = double(harness::g_frames.load());
    if (port == 0) {
        std::string cmd = (app::ExeDir() / "cmd").string();
        if (FILE* f = std::fopen(cmd.c_str(), "r")) {
            char b[64] = {};
            double len = 3;
            if (std::fscanf(f, "%63s %lf", b, &len) >= 1) live = {t, b, len};
            std::fclose(f);
            std::remove(cmd.c_str());
        }
        for (const Press& s : script)
            if (pad_script::Active(s, t)) pad_script::Apply(s.buttons, p);
        if (pad_script::Active(live, t)) pad_script::Apply(live.buttons, p);
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    pad_script::Capture(port, p, uint64_t(t));
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
}

Pad KeyboardMouse() { return {}; }  // no keyboard here
void MouseDirect(bool) {}
bool MouseTake(float* dx, float* dy) {  // no mouse here
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    return false;
}
void LoadControls(const void*) {}  // no bindings here
void SaveControls(void*) {}

// no mouse either: the pointer is never over the picture, and the pad is what was used
Pointer GetPointer() { return {}; }
void PointerWant(bool) {}
Device LastUsed() { return Device::pad; }
std::string KeyName(int) { return ""; }
std::string ControlName(const char*) { return ""; }
bool CursorHidden() { return false; }
void CursorElsewhere(bool) {}
void KeysElsewhere(bool) {}

}  // namespace input
