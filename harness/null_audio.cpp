// the mixer pulled every 10 ms by the clock, its output dropped
#include <chrono>
#include <thread>
#include <vector>

#include "audio/audio.h"

namespace audio {

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        std::vector<float> b(kRate / 100 * 2);
        for (;;) {
            render(b.data(), kRate / 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
}

// dsound asks for the output shape; on a headless host there is no device, so the mix stays plain stereo
// (these are defined by xaudio2.cpp in a Windows build, which this harness leaves out)
bool IsSurround() { return false; }
bool IsMono() { return false; }

}  // namespace audio
