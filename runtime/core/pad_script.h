#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/input.h"

// The SCRIPT file format shared by the headless harness (SCRIPT) and the runtime (XBE_PAD_SCRIPT).
// One press per line: "frame BUTTONS [frames held]", '#' comments, BUTTONS joined by '+'. A press of
// N frames is active at frames at .. at+N-1 (N=8 by default). Buttons:
// UP DOWN LEFT RIGHT START BACK L3 R3 A B X Y BLACK WHITE LT RT LS LSD LSL LSR RS RSD RSL RSR.
// An optional "=<value>" sets an analog amount: 0-255 for A..RT, a 0-32767 magnitude for the stick
// direction names (LS/LSL/...); without it they mean 255 and a full 32767 deflection.
namespace pad_script {

struct Press {
    double at;
    std::string buttons;
    double len;
};

std::vector<Press> Load(const char* path);  // an empty vector when path is unset or cannot be read
bool Active(const Press& p, double frame);
void Apply(const std::string& buttons, input::Pad& p);

// each '+'-separated name
template <class F>
void Each(std::string_view s, F f) {
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find('+', at);
        if (e == std::string_view::npos) e = s.size();
        if (e > at) f(s.substr(at, e - at));
        at = e + 1;
    }
}

// Records changes of port 0's final, guest-visible pad into XBE_PAD_CAPTURE, in the same format.
// Call once per poll, after GameInfo::pad, with the presented-frame count. Does nothing when the
// variable is unset, apart from one branch. The first poll of a state is its start frame; the frame
// where it changes again is the end, so a recorded press covers at .. at+len-1 like a script's.
void Capture(int port, const input::Pad& p, uint64_t frame);

}  // namespace pad_script
