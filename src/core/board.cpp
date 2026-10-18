#include "board.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace sidportal {

namespace {
// Keypad scan (TOC4) runs every 3906 timer ticks = 15.6 ms; hold keys for 3 scans.
constexpr uint64_t kMinKeyHoldCycles = 3 * 3906 * 8;
// Space encoder pulses so each lands in its own capture interrupt (2 ms)...
constexpr uint64_t kEncoderPulseCycles = 4000;
// ...and hold them back while the firmware's 8-entry key-event ring ($1365, head $136D,
// tail $136E) is filling up, so a fast spin never overflows it and loses detents.
bool event_queue_has_room(const std::array<uint8_t, 0x8000>& ram)
{
    return ((ram[0x136D] - ram[0x136E]) & 7) < 4;
}
} // namespace

Board::Board(double sample_rate, double sid_clock_hz, bool mos8580, SidEngineType engine)
    : cpu_(*this), sid_(make_sid_engine(engine)), mos8580_(mos8580),
      sample_rate_(sample_rate), sid_clock_hz_(sid_clock_hz)
{
    sid_ratio_ = sid_clock_hz_ / kEClockHz;
    sid_->configure(mos8580_, sid_clock_hz_, sample_rate_);
    rom_.fill(0xFF);
}

void Board::set_sid_model(bool mos8580)
{
    pending_model_ = mos8580 ? 1 : 0;
    schedule_switch();
}

std::unique_ptr<SidEngine> Board::build_sid_engine(SidEngineType type) const
{
    auto engine = make_sid_engine(type);
    engine->configure(false, sid_clock_hz_, sample_rate_);
    return engine;
}

void Board::set_sid_engine(std::unique_ptr<SidEngine> engine)
{
    if (!engine) return;
    pending_engine_ = std::move(engine);
    schedule_switch();
}

void Board::schedule_switch()
{
    if (cpu_.cycles() == 0) { apply_switch(); return; }
    // Switching chips or engines steps the output's DC level (a thump), so fade out
    // over 15 ms, switch while silent, and fade back in once the step has settled.
    switch_at_ = cpu_.cycles() + uint64_t(0.015 * kEClockHz);
    if (down_from_ < 0) {
        down_from_ = samples_out_ + int64_t(audio_.size());
        unmute_before_ = unmute_at_;
    }
    switch_done_ = false;
}

void Board::apply_switch()
{
    if (pending_model_ >= 0) mos8580_ = pending_model_ == 1;
    pending_model_ = -1;
    if (pending_engine_) {
        sid_ = std::move(pending_engine_);
        sid_->reset();
        sid_->set_model(mos8580_);
        sid_->set_character(filter_bias_);
        // Replay the registers, each voice's control register (gate) last so its
        // envelope starts with the right ADSR.
        for (int reg = 0; reg <= 0x18; ++reg)
            if (reg != 0x04 && reg != 0x0B && reg != 0x12) sid_->write(reg, sid_regs_[size_t(reg)]);
        for (int reg : {0x04, 0x0B, 0x12}) sid_->write(reg, sid_regs_[size_t(reg)]);
    } else {
        sid_->set_model(mos8580_);
        sid_->set_character(filter_bias_);
    }
}

void Board::set_sample_rate(double rate)
{
    if (rate == sample_rate_) return;
    // Output-sample positions (mute and fade marks) are kept as they are: they only
    // decide when the sound comes up, and a rate change never happens mid-boot in practice.
    sample_rate_ = rate;
    sid_->configure(mos8580_, sid_clock_hz_, sample_rate_);
    sid_->set_character(filter_bias_);
    if (pending_engine_) pending_engine_->configure(mos8580_, sid_clock_hz_, sample_rate_);
    audio_.clear();
    ext_in_clear();
}

void Board::set_filter_bias(double volts)
{
    filter_bias_ = volts;
    sid_->set_character(volts);
}

bool Board::load_rom(const std::string& path, std::string* error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t offset;
    if (data.size() == 0x8000) offset = 0;
    else if (data.size() == 0x20000) {
        // The 128 KB EPROM dumps hold the program either at $8000 (plain) or at $0000 (reloc).
        bool upper = data[0xFFFE] == 0x80;
        offset = upper ? 0x8000 : 0;
    } else {
        if (error) *error = "unexpected ROM size " + std::to_string(data.size());
        return false;
    }
    std::copy(data.begin() + long(offset), data.begin() + long(offset) + 0x8000, rom_.begin());
    if (rom_[0x7FFE] != 0x80 || rom_[0x7FFF] != 0x00) {
        if (error) *error = "reset vector is not $8000; not the OS r34 firmware?";
        return false;
    }
    return true;
}

bool Board::load_nvram(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(ram_.data()), long(ram_.size()));
    return f.gcount() == long(ram_.size());
}

bool Board::save_nvram(const std::string& path) const
{
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(ram_.data()), long(ram_.size()));
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

void Board::clear_nvram() { ram_.fill(0); }

void Board::load_ram(const uint8_t* data, size_t size)
{
    ram_.fill(0);
    std::copy(data, data + std::min(size, ram_.size()), ram_.begin());
}

void Board::reset()
{
    lcd_.reset();
    sid_->reset();
    sid_regs_.fill(0);
    audio_.clear();
    sid_debt_ = 0;
    held_key_ = pending_release_ = -1;
    encoder_pending_ = 0;
    crash_pc_ = -1;
    cpu_.illegal_count = 0;
    unmute_at_ = -1;
    samples_out_ = 0;
    down_from_ = -1;
    unmute_before_ = -1;
    if (pending_model_ >= 0 || pending_engine_) apply_switch();
    switch_done_ = false;
    sid_cycles_ = 0;
    ext_.clear();
    ext_resync_ = true;
    cpu_.reset();
}

// --- Bus ----------------------------------------------------------------------

uint8_t Board::read(uint16_t address)
{
    if (address >= 0x8000) return rom_[address - 0x8000];
    if (address >= 0x1040 && address < 0x1100) {
        if (address < 0x1060) return 0;   // SID is write-only here
        if (address < 0x1080) return lcd_.read(address & 1);
        if (address < 0x10A0) return held_key_ >= 0 ? uint8_t(0x10 | held_key_) : 0;
        if (log_io) std::fprintf(stderr, "io read  %04X (pc %04X)\n", address, cpu_.pc());
        return 0xFF;
    }
    return ram_[address];
}

void Board::write(uint16_t address, uint8_t value)
{
    if (address >= 0x8000) return;   // flash: programming sequence not modelled
    if (address >= 0x1040 && address < 0x1100) {
        if (address < 0x1060) {
            flush_sid();
            int reg = address & 0x1F;
            if (sid_write_hook) sid_write_hook(sid_write_ctx, cpu_.cycles(), reg, value);
            sid_regs_[size_t(reg)] = value;
            sid_->write(reg, value);
            if (reg == 0x18 && (value & 0x0F) && unmute_at_ < 0)
                unmute_at_ = samples_out_ + int64_t(audio_.size()) + int64_t(0.3 * sample_rate_);
        } else if (address < 0x1080) {
            lcd_.write(address & 1, value);
        } else if (log_io) {
            std::fprintf(stderr, "io write %04X=%02X (pc %04X)\n", address, value, cpu_.pc());
        }
        return;
    }
    ram_[address] = value;
}

// --- Timing ---------------------------------------------------------------------

void Board::flush_sid()
{
    int owed = int(sid_debt_);
    if (owed <= 0) return;
    sid_debt_ -= owed;
    if (ext_.empty() && ext_resync_) {
        sid_->input(0);
        sid_cycles_ += uint64_t(owed);
        sid_->clock(owed, audio_);
        return;
    }
    // Feed EXT IN in 8-cycle steps, linearly interpolated, so the 1 MHz SID does not
    // see a 48 kHz staircase.
    const double samples_per_cycle = sample_rate_ / sid_clock_hz_;
    while (owed > 0) {
        int chunk = owed < 8 ? owed : 8;
        float v = ext_sample(double(sid_cycles_) * samples_per_cycle) * ext_gain_;
        sid_->input(int16_t(std::clamp(v, -32768.0f, 32767.0f)));
        sid_cycles_ += uint64_t(chunk);
        owed -= chunk;
        sid_->clock(chunk, audio_);
    }
}

float Board::ext_sample(double pos)
{
    double i = pos - ext_base_;
    if (i < 0) return 0.0f;                       // still inside the latency slack
    while (i >= 1.0 && ext_.size() > 1) { ext_.pop_front(); ext_base_ += 1.0; i -= 1.0; }
    if (ext_.size() < 2) {                        // ran dry: go quiet and resync on new data
        ext_.clear();
        ext_resync_ = true;
        return 0.0f;
    }
    return float(ext_[0] + (ext_[1] - ext_[0]) * i);
}

void Board::ext_in(const int16_t* samples, size_t count, bool bound_latency)
{
    double now = double(sid_cycles_) * sample_rate_ / sid_clock_hz_;
    if (ext_resync_ || ext_.empty()) {
        ext_.clear();
        ext_base_ = now + kExtLatency;
        ext_resync_ = false;
    }
    ext_.insert(ext_.end(), samples, samples + count);
    // Bound the latency if capture runs faster than playback (clock drift).
    double ahead = ext_base_ + double(ext_.size()) - now;
    while (bound_latency && ahead > 4 * kExtLatency && ext_.size() > 1) { ext_.pop_front(); ext_base_ += 1.0; ahead -= 1.0; }
}

void Board::ext_in_clear()
{
    ext_.clear();
    ext_resync_ = true;
    sid_->input(0);
}

void Board::step()
{
    uint64_t now = cpu_.cycles();
    if (pending_release_ >= 0 && now - key_down_at_ >= kMinKeyHoldCycles) {
        if (held_key_ == pending_release_) held_key_ = -1;
        pending_release_ = -1;
    }
    if (encoder_pending_ != 0 && now >= encoder_next_ && event_queue_has_room(ram_)) {
        cpu_.input_capture(encoder_pending_ > 0 ? 1 : 2);
        encoder_pending_ += encoder_pending_ > 0 ? -1 : 1;
        encoder_next_ = now + kEncoderPulseCycles;
    }
    if ((pending_model_ >= 0 || pending_engine_) && now >= switch_at_) {
        flush_sid();
        apply_switch();
        switch_done_ = true;
        // Still waiting for the firmware's first volume write (boot): keep that mute.
        if (unmute_before_ >= 0)
            unmute_at_ = samples_out_ + int64_t(audio_.size()) + int64_t(0.12 * sample_rate_);
    }
    if (crash_pc_ < 0 && cpu_.pc() < 0x8000) crash_pc_ = cpu_.pc();
    int cyc = cpu_.step();
    if (crash_pc_ < 0 && cpu_.illegal_count) crash_pc_ = cpu_.illegal_pc;
    sid_debt_ += cyc * sid_ratio_;
    if (sid_debt_ >= 64) flush_sid();
}

void Board::render(int16_t* out, int frames)
{
    while (audio_.size() < size_t(frames)) step();
    const float fade_in = float(0.2 * sample_rate_);
    const float fade_out = float(0.015 * sample_rate_);
    auto up = [&](int64_t k, int64_t at) {
        return (at < 0 || k < at) ? 0.0f : std::min(1.0f, float(k - at) / fade_in);
    };
    for (int i = 0; i < frames; ++i, ++samples_out_) {
        // Mute until the firmware first sets the SID volume (power-on), plus 0.3 s for the
        // 6581's DC step to settle, then fade in. A chip switch fades out, mutes and
        // fades back in the same way.
        const int64_t k = samples_out_;
        float gain;
        if (down_from_ >= 0 && k >= down_from_) {
            gain = 0.0f;
            if (float(k - down_from_) < fade_out)
                gain = up(k, unmute_before_) * (1.0f - float(k - down_from_) / fade_out);
            if (switch_done_ && unmute_at_ > down_from_ && k >= unmute_at_) {
                gain = up(k, unmute_at_);
                if (gain >= 1.0f) down_from_ = -1;
            }
        } else {
            gain = up(k, down_from_ >= 0 ? unmute_before_ : unmute_at_);
        }
        out[i] = int16_t(float(audio_.front()) * gain);
        audio_.pop_front();
    }
}

void Board::run_cycles(uint64_t count)
{
    uint64_t end = cpu_.cycles() + count;
    while (cpu_.cycles() < end) {
        step();
        if (audio_.size() > 8192) audio_.erase(audio_.begin(), audio_.end() - 4096);
    }
}

// --- Panel ------------------------------------------------------------------------

void Board::key_down(int code)
{
    held_key_ = code & 0x0F;
    pending_release_ = -1;
    key_down_at_ = cpu_.cycles();
}

void Board::key_up(int code)
{
    if (held_key_ != (code & 0x0F)) return;
    pending_release_ = held_key_;
}

void Board::encoder(int steps) { encoder_pending_ += steps; }

std::vector<uint8_t> Board::take_midi_out()
{
    std::vector<uint8_t> out;
    out.swap(midi_out_);
    return out;
}

} // namespace sidportal
