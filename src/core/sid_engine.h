// SID emulation engines behind one interface, so the board can switch between them at
// runtime: reSID (Dag Lem's original) and reSIDfp (libresidfp, with the more detailed
// 6581/8580 filter and combined-waveform models).
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace sidportal {

enum class SidEngineType { ReSid, ReSidFp };

const char* sid_engine_name(SidEngineType type);              // "resid" / "residfp"
bool parse_sid_engine(const std::string& name, SidEngineType* out);

class SidEngine {
public:
    virtual ~SidEngine() = default;
    virtual SidEngineType type() const = 0;
    // Chip model plus sampling setup (builds resampler tables: keep off the audio thread).
    virtual void configure(bool mos8580, double clock_hz, double sample_rate) = 0;
    // Chip model alone (cheap); also reapplies the chip character.
    virtual void set_model(bool mos8580) = 0;
    // Chip character, -0.5 (dark) .. +0.5 (bright). reSID shifts the 6581 filter's
    // cutoff DAC by this many volts (no effect on the 8580); reSIDfp moves the filter
    // curve of either chip across its 0..1 range.
    virtual void set_character(double c) = 0;
    virtual void reset() = 0;
    virtual void write(int reg, uint8_t value) = 0;
    virtual void input(int16_t value) = 0;   // EXT IN
    // Run `cycles` SID clocks, appending the output samples to `out`.
    virtual void clock(int cycles, std::deque<int16_t>& out) = 0;
};

std::unique_ptr<SidEngine> make_sid_engine(SidEngineType type);

} // namespace sidportal
