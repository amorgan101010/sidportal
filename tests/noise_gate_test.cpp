#include "noise_gate.h"
#include <cassert>
#include <cmath>
using sidportal::NoiseGate;
int main() {
    NoiseGate g;
    g.set_sample_rate(48000);
    g.set_threshold_db(-50);
    // Disabled is an exact pass-through, including low-level samples.
    for (int i = 0; i < 1000; ++i) assert(g.process(int16_t(7)) == 7);
    g.set_enabled(true);
    float prev = 1.0f;
    for (int i = 0; i < 30000; ++i) prev = g.process(0.001f);
    assert(std::abs(prev) < 0.00001f);
    // Opening ramps rather than jumping, then reaches unity for a strong note.
    float first = g.process(0.2f);
    assert(first > 0.0f && first < 0.2f);
    float last = first;
    for (int i = 0; i < 6000; ++i) last = g.process(0.2f);
    assert(last > 0.195f);
    // Hysteresis and hold preserve the sound through short dips.
    for (int i = 0; i < 500; ++i) g.process(0.0f);
    assert(g.process(0.2f) > 0.18f);
    // Sustained quiet signal closes smoothly.
    float closing = g.process(0.001f);
    assert(closing > 0.0f);
    for (int i = 0; i < 40000; ++i) closing = g.process(0.001f);
    assert(std::abs(closing) < 0.00001f);
    // A low threshold lets intentional quiet notes and SID noise waveforms through.
    g.set_threshold_db(-80);
    for (int i = 0; i < 10000; ++i) last = g.process((i & 1) ? 0.001f : -0.001f);
    assert(std::abs(last) > 0.0009f);
    g.set_enabled(false);
    assert(g.process(0.001f) == 0.001f);
    g.set_sample_rate(96000);
    g.set_enabled(true);
    for (int i = 0; i < 20000; ++i) last = g.process(0.2f);
    assert(last > 0.19f);
}
