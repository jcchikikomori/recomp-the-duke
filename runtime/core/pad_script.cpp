// The pad script format used by the headless harness (SCRIPT) and by the runtime (XBE_PAD_SCRIPT),
// and the capture of a real session back into that format (XBE_PAD_CAPTURE).
#include "core/pad_script.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

#include "core/log.h"

namespace pad_script {

namespace {

std::vector<Press> LoadImpl(const char* path) {
    std::vector<Press> v;
    FILE* f = path ? std::fopen(path, "r") : nullptr;
    if (!f) return v;
    char line[200];
    while (std::fgets(line, sizeof line, f)) {
        char b[128];
        double t, len = 8;
        if (line[0] != '#' && std::sscanf(line, "%lf %127s %lf", &t, b, &len) >= 2) v.push_back({t, b, len});
    }
    std::fclose(f);
    LOGI("pad script: %s, %zu presses", path, v.size());
    return v;
}

// a name's optional "=<value>" is an analog amount, not part of the name
std::pair<std::string_view, long> Split(std::string_view token) {
    size_t eq = token.find('=');
    std::string_view name = token.substr(0, eq);
    long value = -1;  // no '=' means full deflection
    if (eq != std::string_view::npos) {
        std::string text(token.substr(eq + 1));
        value = std::strtol(text.c_str(), nullptr, 10);
    }
    return {name, value};
}

int16_t Stick(long magnitude, int sign) {
    long v = std::clamp(magnitude, long(0), long(32767));
    return int16_t(sign * v);
}

// the currently held state, as script tokens. Only nonzero fields have a token; an unadorned name
// means full deflection, so a captured script uses the familiar names wherever it can.
std::vector<std::string> Names(const input::Pad& p) {
    std::vector<std::string> v;
    constexpr const char* kButtons[] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "L3", "R3"};
    for (int i = 0; i < 8; i++)
        if (p.buttons & (1 << i)) v.push_back(kButtons[i]);
    constexpr const char* kAnalog[] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"};
    for (int i = 0; i < 8; i++) {
        if (!p.analog[i]) continue;
        v.push_back(p.analog[i] == 255 ? kAnalog[i] : std::string(kAnalog[i]) + "=" + std::to_string(p.analog[i]));
    }
    auto stick = [&](int16_t value, const char* positive, const char* negative) {
        if (!value) return;
        const char* name = value > 0 ? positive : negative;
        long amount = std::abs(long(value));
        v.push_back(amount == 32767 ? name : std::string(name) + "=" + std::to_string(amount));
    };
    stick(p.ly, "LS", "LSD");
    stick(p.lx, "LSR", "LSL");
    stick(p.ry, "RS", "RSD");
    stick(p.rx, "RSR", "RSL");
    return v;
}

// the capture is one process-wide state: poll can come from the guest or the window thread
struct CaptureState {
    struct Hold {
        std::string name;
        uint64_t start;
    };

    std::mutex lock;
    FILE* file = nullptr;
    std::vector<Hold> held;  // the last reported state, one entry per active token
    uint64_t frame = 0;

    CaptureState() {
        const char* path = std::getenv("XBE_PAD_CAPTURE");
        if (!path || !*path) return;
        file = std::fopen(path, "w");
        if (!file) {
            LOGE("pad capture: cannot write %s", path);
            return;
        }
        LOGR("pad capture: %s", path);
        std::fprintf(file, "# captured from presented frames; frame BUTTONS [frames held]\n");
        std::fflush(file);
    }

    ~CaptureState() {
        if (!file) return;
        // a press still held when the game closes: give it the frames observed so far
        for (const Hold& h : held) {
            uint64_t len = frame >= h.start ? frame - h.start + 1 : 1;
            std::fprintf(file, "%llu %s %llu\n", (unsigned long long)h.start, h.name.c_str(),
                         (unsigned long long)len);
        }
        std::fclose(file);
    }

    void Record(const input::Pad& p, uint64_t at) {
        std::lock_guard guard(lock);
        frame = std::max(frame, at);
        std::vector<std::string> now = Names(p);
        std::vector<Hold> next;
        bool changed = false;
        // a token that is gone (or whose value changed) ends where the new state was first seen
        for (const Hold& h : held) {
            if (std::find(now.begin(), now.end(), h.name) != now.end()) continue;
            if (at > h.start) {
                std::fprintf(file, "%llu %s %llu\n", (unsigned long long)h.start, h.name.c_str(),
                             (unsigned long long)(at - h.start));
                changed = true;
            }
        }
        for (const std::string& name : now) {
            auto it = std::find_if(held.begin(), held.end(), [&](const Hold& h) { return h.name == name; });
            if (it != held.end()) {
                next.push_back(*it);
                continue;
            }
            next.push_back({name, at});
            changed = true;
        }
        held.swap(next);
        if (changed) std::fflush(file);  // a crash repro is useful even if the game is killed
    }
};

CaptureState& State() {
    static CaptureState state;
    return state;
}

}  // namespace

std::vector<Press> Load(const char* path) { return LoadImpl(path); }

bool Active(const Press& p, double frame) { return frame >= p.at && frame < p.at + p.len; }

void Apply(const std::string& buttons, input::Pad& p) {
    Each(buttons, [&](std::string_view token) {
        auto [name, value] = Split(token);
        std::string n(name);
        auto amount = [&] { return uint8_t(value < 0 ? 255 : std::clamp(value, long(0), long(255))); };
        auto deflection = [&] { return Stick(value < 0 ? 32767 : value, 1); };
        if (n == "UP") p.buttons |= 1;
        else if (n == "DOWN") p.buttons |= 2;
        else if (n == "LEFT") p.buttons |= 4;
        else if (n == "RIGHT") p.buttons |= 8;
        else if (n == "START") p.buttons |= 16;
        else if (n == "BACK") p.buttons |= 32;
        else if (n == "L3") p.buttons |= 64;
        else if (n == "R3") p.buttons |= 128;
        else if (n == "A") p.analog[0] = amount();
        else if (n == "B") p.analog[1] = amount();
        else if (n == "X") p.analog[2] = amount();
        else if (n == "Y") p.analog[3] = amount();
        else if (n == "BLACK") p.analog[4] = amount();
        else if (n == "WHITE") p.analog[5] = amount();
        else if (n == "LT") p.analog[6] = amount();
        else if (n == "RT") p.analog[7] = amount();
        else if (n == "LS") p.ly = deflection();
        else if (n == "LSD") p.ly = -deflection();
        else if (n == "LSL") p.lx = -deflection();
        else if (n == "LSR") p.lx = deflection();
        else if (n == "RS") p.ry = deflection();
        else if (n == "RSD") p.ry = -deflection();
        else if (n == "RSL") p.rx = -deflection();
        else if (n == "RSR") p.rx = deflection();
        // KEY:... is a keyboard script for code that reads keys itself, not a pad field
    });
}

void Capture(int port, const input::Pad& p, uint64_t frame) {
    CaptureState& s = State();  // one getenv/fopen on the first call, one branch afterwards
    if (!s.file || port != 0) return;
    s.Record(p, frame);
}

}  // namespace pad_script
