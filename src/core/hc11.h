// Motorola MC68HC11 CPU core with the on-chip peripherals the unit uses:
// the free-running timer (TOC1-5, TIC1-3, RTI, TOF), the SCI, the A/D converter
// and port A. Registers sit at $1000-$103F (INIT is never remapped by the firmware).
#pragma once
#include <cstdint>
#include <deque>

namespace sidportal {

class Hc11Bus {
public:
    virtual ~Hc11Bus() = default;
    // External bus access (everything outside the $1000-$103F register block).
    virtual uint8_t read(uint16_t address) = 0;
    virtual void write(uint16_t address, uint8_t value) = 0;
    // A byte left the SCI transmitter (MIDI Out).
    virtual void sci_transmit(uint8_t value) { (void)value; }
    // Port A output pins changed (LEDs).
    virtual void port_a_changed(uint8_t value) { (void)value; }
};

class Hc11 {
public:
    explicit Hc11(Hc11Bus& bus);

    void reset();
    // Execute one instruction (or take one interrupt); returns E-clock cycles used.
    int step();

    uint64_t cycles() const { return cycles_; }

    // Host inputs.
    void set_adc_input(int channel, uint8_t value) { adc_in_[channel & 7] = value; }
    // Edge on an input-capture pin: 1 = PA2/IC1, 2 = PA1/IC2, 3 = PA0/IC3.
    void input_capture(int which);
    // Queue a byte for the SCI receiver; it arrives after one frame time.
    void sci_receive(uint8_t value) { rx_queue_.push_back(value); }
    size_t sci_pending() const { return rx_queue_.size(); }
    void set_irq_line(bool asserted) { irq_line_ = asserted; }

    uint8_t port_a() const { return port_a_out_; }
    uint16_t pc() const { return pc_; }
    uint8_t peek_reg(uint8_t r) const { return regs_[r & 0x3F]; }
    // Diagnostics: illegal opcodes executed and the PC of the last one.
    uint32_t illegal_count = 0;
    uint32_t rx_dropped = 0;    // bytes that arrived with the receiver off
    uint32_t rx_overruns = 0;   // bytes lost because the firmware had not read the last one
    uint16_t illegal_pc = 0;
    void set_pc(uint16_t pc) { pc_ = pc; }

    // Register state for debugging/tests.
    uint8_t a = 0, b = 0, ccr = 0xD0;
    uint16_t x = 0, y = 0, sp = 0;

    // Frame length in E cycles (10 bits at 31250 baud with E = 2 MHz is 640).
    int sci_frame_cycles = 640;

private:
    enum : uint8_t { C = 0x01, V = 0x02, Z = 0x04, N = 0x08, I = 0x10, H = 0x20, X = 0x40, S = 0x80 };

    uint8_t rd(uint16_t a);
    void wr(uint16_t a, uint8_t v);
    uint16_t rd16(uint16_t a) { return uint16_t(rd(a) << 8 | rd(uint16_t(a + 1))); }
    void wr16(uint16_t a, uint16_t v) { wr(a, uint8_t(v >> 8)); wr(uint16_t(a + 1), uint8_t(v)); }
    uint8_t fetch() { return rd(pc_++); }
    uint16_t fetch16() { uint16_t v = rd16(pc_); pc_ += 2; return v; }
    void push(uint8_t v) { wr(sp--, v); }
    void push16(uint16_t v) { push(uint8_t(v)); push(uint8_t(v >> 8)); }
    uint8_t pull() { return rd(++sp); }
    uint16_t pull16() { uint16_t h = pull(); return uint16_t(h << 8 | pull()); }

    uint8_t reg_read(uint8_t r);
    void reg_write(uint8_t r, uint8_t v);

    void advance(int cycles);
    void timer_advance(int cycles);
    void sci_advance(int cycles);
    void apply_oc_pin(int oc);
    int pending_vector();
    void take_interrupt(uint16_t vector);
    int execute(uint8_t op, int page);

    void set_nz8(uint8_t v) { ccr = uint8_t((ccr & ~(N | Z)) | (v & 0x80 ? N : 0) | (v ? 0 : Z)); }
    void set_nz16(uint16_t v) { ccr = uint8_t((ccr & ~(N | Z)) | (v & 0x8000 ? N : 0) | (v ? 0 : Z)); }
    void flag(uint8_t f, bool on) { ccr = on ? uint8_t(ccr | f) : uint8_t(ccr & ~f); }
    uint16_t d() const { return uint16_t(a << 8 | b); }
    void set_d(uint16_t v) { a = uint8_t(v >> 8); b = uint8_t(v); }

    uint8_t add8(uint8_t l, uint8_t r, int carry);
    uint8_t sub8(uint8_t l, uint8_t r, int carry);
    uint16_t add16(uint16_t l, uint16_t r);
    uint16_t sub16(uint16_t l, uint16_t r);
    uint8_t rmw(int op, uint8_t v);

    Hc11Bus& bus_;
    uint16_t pc_ = 0;
    uint64_t cycles_ = 0;
    bool after_rti_ = false;
    bool sci_listening_ = false;   // receive interrupt enabled at least once since reset
    bool waiting_ = false;   // WAI: registers stacked, waiting for an interrupt
    bool stopped_ = false;
    bool irq_line_ = false;

    // Registers.
    uint8_t regs_[0x40] = {};
    uint8_t port_a_out_ = 0;
    uint16_t tcnt_ = 0;
    int prescale_acc_ = 0;
    int rti_acc_ = 0;
    uint8_t tflg1_ = 0, tflg2_ = 0;
    uint16_t tic_[3] = {};
    // SCI.
    std::deque<uint8_t> rx_queue_;
    int rx_timer_ = 0;
    uint8_t scsr_ = 0xC0;
    uint8_t rx_data_ = 0;
    bool tx_busy_ = false;
    int tx_timer_ = 0;
    uint8_t tx_shift_ = 0;
    bool tx_buffer_full_ = false;
    uint8_t tx_buffer_ = 0;
    bool scsr_read_ = false;
    // ADC.
    uint8_t adc_in_[8] = {};
    uint8_t adr_[4] = {};
    int adc_timer_ = 0;
    bool adc_running_ = false;
    uint8_t adctl_ = 0;
};

} // namespace sidportal
