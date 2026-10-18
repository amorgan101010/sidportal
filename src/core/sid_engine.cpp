#include "sid_engine.h"

#include "sid.h"
#include "residfp/residfp.h"

#include <algorithm>

namespace sidportal {

const char* sid_engine_name(SidEngineType type)
{
    return type == SidEngineType::ReSidFp ? "residfp" : "resid";
}

bool parse_sid_engine(const std::string& name, SidEngineType* out)
{
    if (name == "resid") { *out = SidEngineType::ReSid; return true; }
    if (name == "residfp") { *out = SidEngineType::ReSidFp; return true; }
    return false;
}

namespace {

class ReSidEngine final : public SidEngine {
public:
    SidEngineType type() const override { return SidEngineType::ReSid; }
    void configure(bool mos8580, double clock_hz, double sample_rate) override
    {
        set_model(mos8580);
        sid_.set_sampling_parameters(clock_hz, reSID::SAMPLE_RESAMPLE, sample_rate);
    }
    void set_model(bool mos8580) override
    {
        sid_.set_chip_model(mos8580 ? reSID::MOS8580 : reSID::MOS6581);
        sid_.set_voice_mask(0x0F);   // reSID masks EXT IN out of the mix by default
        sid_.adjust_filter_bias(character_);
    }
    void set_character(double c) override { character_ = c; sid_.adjust_filter_bias(c); }
    void reset() override { sid_.reset(); sid_.set_voice_mask(0x0F); }
    void write(int reg, uint8_t value) override { sid_.write(reSID::reg8(reg), value); }
    void input(int16_t value) override { sid_.input(value); }
    void clock(int cycles, std::deque<int16_t>& out) override
    {
        short buf[512];
        reSID::cycle_count left = cycles;
        while (left > 0) {
            int n = sid_.clock(left, buf, 512);
            out.insert(out.end(), buf, buf + n);
        }
    }

private:
    reSID::SID sid_;
    double character_ = 0.0;
};

class ReSidFpEngine final : public SidEngine {
public:
    SidEngineType type() const override { return SidEngineType::ReSidFp; }
    void configure(bool mos8580, double clock_hz, double sample_rate) override
    {
        set_model(mos8580);
        sid_.setSamplingParameters(clock_hz, reSIDfp::RESAMPLE, sample_rate);
        // clock() writes ceil(cycles * rate / clock) samples with no bound: size the
        // chunks so they always fit the 512-sample buffer.
        max_chunk_ = std::max(1, int(400.0 * clock_hz / sample_rate));
    }
    void set_model(bool mos8580) override
    {
        mos8580_ = mos8580;
        sid_.setChipModel(mos8580 ? reSIDfp::CSG8580 : reSIDfp::MOS6581);
        set_character(character_);
    }
    void set_character(double c) override
    {
        character_ = c;
        // libresidfp documents 0 as dark for both curves, but measured, the 8580's runs the
        // other way (its integrator bias falls as the curve rises, lowering the cutoff).
        if (mos8580_) sid_.setFilter8580Curve(std::clamp(0.5 - c, 0.0, 1.0));
        else sid_.setFilter6581Curve(std::clamp(0.5 + c, 0.0, 1.0));
    }
    void reset() override { sid_.reset(); }
    void write(int reg, uint8_t value) override { sid_.write(reg, value); }
    void input(int16_t value) override { sid_.input(value); }
    void clock(int cycles, std::deque<int16_t>& out) override
    {
        short buf[512];
        while (cycles > 0) {
            int chunk = std::min(cycles, max_chunk_);
            int n = sid_.clock(unsigned(chunk), buf);
            out.insert(out.end(), buf, buf + n);
            cycles -= chunk;
        }
    }

private:
    reSIDfp::residfp sid_;
    bool mos8580_ = false;
    double character_ = 0.0;
    int max_chunk_ = 4096;
};

} // namespace

std::unique_ptr<SidEngine> make_sid_engine(SidEngineType type)
{
    if (type == SidEngineType::ReSidFp) return std::make_unique<ReSidFpEngine>();
    return std::make_unique<ReSidEngine>();
}

} // namespace sidportal
