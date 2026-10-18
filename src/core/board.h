// The emulated main board: MC68HC11 at E = 2 MHz, 32 KB battery-backed SRAM at
// $0000-$7FFF, 32 KB flash at $8000-$FFFF, and an I/O window at $1040-$10FF:
//   $1040-$105F  MOS 6581 SID (write only)
//   $1060/$1061  HD44780 LCD instruction/data registers
//   $1080        keypad encoder: bit 4 = key down, bits 0-3 = key code
// The four knobs are on the HC11 A/D (PE0-PE3), the rotary encoder pulses IC1 (up)
// or IC2 (down), MIDI is the SCI, and the four LEDs are port A outputs.
#pragma once
#include "hc11.h"
#include "hd44780.h"
#include "sid_engine.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace sidportal {

constexpr double kEClockHz = 2000000.0;

// Panel key codes as the keypad encoder reports them; the firmware's table at $A14F
// maps code n to "123A456B789C*0#D"[n]. A = EXIT, B = UP, C = DOWN, D = ENTER,
// * = LEFT, # = RIGHT.
constexpr const char* kKeyLegends = "123A456B789C*0#D";

class Board : public Hc11Bus {
public:
    explicit Board(double sample_rate = 48000.0, double sid_clock_hz = 1000000.0, bool mos8580 = false,
                   SidEngineType engine = SidEngineType::ReSidFp);

    // Load the 32 KB program image for $8000-$FFFF (accepts the 128 KB EPROM dumps too).
    bool load_rom(const std::string& path, std::string* error);
    bool load_nvram(const std::string& path);
    bool save_nvram(const std::string& path) const;
    void clear_nvram();
    // RAM image from memory (plugin state); shorter images leave the rest cleared.
    void load_ram(const uint8_t* data, size_t size);
    void reset();

    // Run the machine until `frames` mono samples are ready and copy them to `out`.
    void render(int16_t* out, int frames);
    // Run for a number of E cycles, discarding audio beyond a small backlog.
    void run_cycles(uint64_t cycles);

    // Panel inputs.
    void key_down(int code);
    void key_up(int code);
    void encoder(int steps);          // + = clockwise (UP), - = counter-clockwise
    void set_knob(int knob, uint8_t value) { cpu_.set_adc_input(knob & 3, value); }
    void midi_in(uint8_t byte) { cpu_.sci_receive(byte); }
    // Ext In: audio at the output sample rate, fed to the SID's EXT IN pin (which the
    // firmware routes through the filter). Samples queue behind a small fixed latency;
    // when the queue runs dry the input goes silent and resynchronises.
    // bound_latency: drop the oldest samples if the queue exceeds 4x the latency target
    // (live capture); off for preloaded files.
    void ext_in(const int16_t* samples, size_t count, bool bound_latency = true);
    void ext_in_clear();
    void set_ext_gain(float gain) { ext_gain_ = gain; }
    size_t ext_in_pending() const { return ext_.size(); }
    size_t midi_in_pending() const { return cpu_.sci_pending(); }

    // Outputs.
    std::vector<uint8_t> take_midi_out();
    const Hd44780& lcd() const { return lcd_; }
    uint8_t leds() const { return cpu_.port_a(); }
    Hc11& cpu() { return cpu_; }
    const uint8_t* ram() const { return ram_.data(); }
    uint64_t cycles() const { return cpu_.cycles(); }
    uint8_t sccr2() { return cpu_.peek_reg(0x2D); }
    // Chip and engine switches fade out, switch while silent and fade back in.
    void set_sid_model(bool mos8580);
    bool sid_model_8580() const { return pending_model_ >= 0 ? pending_model_ == 1 : mos8580_; }
    // A configured engine for this board's clock and rate. Slow (tables): call it off the
    // audio thread, then hand the result to set_sid_engine, which replays the registers.
    std::unique_ptr<SidEngine> build_sid_engine(SidEngineType type) const;
    void set_sid_engine(std::unique_ptr<SidEngine> engine);
    SidEngineType sid_engine() const { return pending_engine_ ? pending_engine_->type() : sid_->type(); }
    // Chip character, -0.5 (dark) .. +0.5 (bright); see SidEngine::set_character. Real
    // 6581 filters differ from chip to chip by about this much.
    void set_filter_bias(double volts);
    double filter_bias() const { return filter_bias_; }
    double sid_clock_hz() const { return sid_clock_hz_; }
    double sample_rate() const { return sample_rate_; }
    // Change the output rate without rebooting (plugin hosts): rebuilds the resampler,
    // drops queued audio and Ext In.
    void set_sample_rate(double rate);

    // Debug: called for each SID register write (cycle, register, value).
    void (*sid_write_hook)(void* ctx, uint64_t cycle, int reg, uint8_t value) = nullptr;
    void* sid_write_ctx = nullptr;
    // Set when the firmware runs off the rails (PC outside ROM or an illegal opcode).
    bool crashed() const { return crash_pc_ >= 0; }
    int crash_pc() const { return crash_pc_; }
    // Debug: log unmapped I/O accesses.
    bool log_io = false;

    // Hc11Bus
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void sci_transmit(uint8_t value) override { midi_out_.push_back(value); }

private:
    void step();
    void flush_sid();

    Hc11 cpu_;
    Hd44780 lcd_;
    std::unique_ptr<SidEngine> sid_;
    std::array<uint8_t, 0x20> sid_regs_{};   // last value written to each register
    bool mos8580_ = false;
    std::array<uint8_t, 0x8000> ram_{};
    std::array<uint8_t, 0x8000> rom_{};
    double sample_rate_;
    double sid_clock_hz_;
    double filter_bias_ = 0.0;
    double sid_ratio_;        // SID cycles per E cycle
    double sid_debt_ = 0.0;   // SID cycles owed for elapsed CPU time
    std::deque<int16_t> audio_;
    // Ext In timeline: positions are in output samples since reset, derived from SID cycles.
    float ext_sample(double pos);
    std::deque<float> ext_;
    double ext_base_ = 0;           // timeline position of ext_.front()
    bool ext_resync_ = true;
    float ext_gain_ = 1.0f;
    uint64_t sid_cycles_ = 0;       // SID cycles clocked since reset
    static constexpr int kExtLatency = 768;   // samples of slack for capture jitter
    std::vector<uint8_t> midi_out_;

    // Keypad: a key stays down for at least two scan periods so short clicks register.
    int crash_pc_ = -1;
    int64_t unmute_at_ = -1;     // output sample index where the power-on mute ends
    int64_t samples_out_ = 0;
    // Chip switch: fade out from down_from_, switch at switch_at_ (E cycles), fade in.
    void schedule_switch();
    void apply_switch();
    int pending_model_ = -1;
    std::unique_ptr<SidEngine> pending_engine_;
    uint64_t switch_at_ = 0;
    int64_t down_from_ = -1;
    int64_t unmute_before_ = -1;
    bool switch_done_ = false;
    int held_key_ = -1;
    int pending_release_ = -1;
    uint64_t key_down_at_ = 0;
    // Encoder pulses still to deliver, spaced so the firmware sees each one.
    int encoder_pending_ = 0;
    uint64_t encoder_next_ = 0;
};

} // namespace sidportal
