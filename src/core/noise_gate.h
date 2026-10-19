// Host output effect. This never writes SID registers or changes the firmware envelope.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sidportal {

class NoiseGate {
public:
    void set_sample_rate(double rate) {
        rate_ = rate > 0 ? rate : 48000.0;
        detector_attack_ = coefficient(0.001);
        detector_release_ = coefficient(0.030);
        gain_attack_ = coefficient(0.003);
        gain_release_ = coefficient(0.060);
        hold_samples_ = int(0.040 * rate_);
        hold_ = 0;
    }
    void set_enabled(bool enabled) {
        if (enabled == enabled_) return;
        enabled_ = enabled;
        gain_ = enabled ? 0.0f : 1.0f;
        detector_ = 0.0f;
        hold_ = 0;
        open_ = false;
    }
    void set_threshold_db(float db) {
        threshold_db_ = std::clamp(db, -90.0f, -18.0f);
        open_level_ = std::pow(10.0f, threshold_db_ / 20.0f);
        close_level_ = open_level_ * 0.708f; // 3 dB hysteresis
    }
    bool enabled() const { return enabled_; }
    float threshold_db() const { return threshold_db_; }
    float process(float sample) {
        if (!enabled_) return sample; // bit-identical bypass
        const float level = std::abs(sample);
        detector_ += (level - detector_) * (level > detector_ ? detector_attack_ : detector_release_);
        if (detector_ < 1e-10f) detector_ = 0.0f;
        if (detector_ >= open_level_) { open_ = true; hold_ = hold_samples_; }
        else if (open_) {
            if (detector_ >= close_level_) hold_ = hold_samples_;
            else if (hold_ > 0) --hold_;
            else open_ = false;
        }
        const float target = open_ ? 1.0f : 0.0f;
        gain_ += (target - gain_) * (open_ ? gain_attack_ : gain_release_);
        if (gain_ < 1e-6f) gain_ = 0.0f;
        return sample * gain_;
    }
    int16_t process(int16_t sample) {
        if (!enabled_) return sample;
        return int16_t(std::clamp(process(float(sample) / 32768.0f) * 32768.0f, -32768.0f, 32767.0f));
    }
private:
    float coefficient(double seconds) const { return float(1.0 - std::exp(-1.0 / (seconds * rate_))); }
    double rate_ = 48000.0;
    float threshold_db_ = -60.0f, open_level_ = 0.001f, close_level_ = 0.000708f;
    float detector_ = 0.0f, gain_ = 1.0f;
    float detector_attack_ = 0.0206f, detector_release_ = 0.000694f;
    float gain_attack_ = 0.00692f, gain_release_ = 0.000347f;
    int hold_samples_ = 1920, hold_ = 0;
    bool open_ = false, enabled_ = false;
};
} // namespace sidportal
