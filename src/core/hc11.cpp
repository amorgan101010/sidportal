#include "hc11.h"

namespace sidportal {

namespace {

// Register offsets in the $1000 block.
enum : uint8_t {
    PORTA = 0x00, CFORC = 0x0B, OC1M = 0x0C, OC1D = 0x0D, TCNTH = 0x0E, TCNTL = 0x0F,
    TIC1 = 0x10, TOC1 = 0x16, TCTL1 = 0x20, TCTL2 = 0x21, TMSK1 = 0x22, TFLG1 = 0x23,
    TMSK2 = 0x24, TFLG2 = 0x25, PACTL = 0x26, BAUD = 0x2B, SCCR1 = 0x2C, SCCR2 = 0x2D,
    SCSR = 0x2E, SCDR = 0x2F, ADCTL = 0x30, ADR1 = 0x31, OPTION = 0x39,
};

enum : uint8_t { TDRE = 0x80, TC = 0x40, RDRF = 0x20, IDLE = 0x10, OR_ = 0x08 };

// Base E-clock cycles for page-1 opcodes. Prefixed (page 2/3/4) forms take one more.
constexpr uint8_t kCycles[256] = {
    // 0x00
    2, 2, 41, 41, 3, 3, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2,
    // 0x10
    2, 2, 6, 6, 6, 6, 2, 2, 0, 2, 0, 2, 7, 7, 7, 7,
    // 0x20
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    // 0x30
    3, 3, 4, 4, 3, 3, 3, 3, 5, 5, 3, 12, 4, 10, 14, 14,
    // 0x40
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    // 0x50
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    // 0x60
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6,
    // 0x70
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6,
    // 0x80
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 4, 6, 3, 3,
    // 0x90
    3, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 5, 5, 4, 4,
    // 0xA0
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5,
    // 0xB0
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 6, 5, 5,
    // 0xC0
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 3, 0, 3, 2,
    // 0xD0
    3, 3, 3, 5, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4,
    // 0xE0
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
    // 0xF0
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
};

} // namespace

Hc11::Hc11(Hc11Bus& bus) : bus_(bus) { reset(); }

void Hc11::reset()
{
    for (auto& r : regs_) r = 0;
    regs_[BAUD] = 0x04;
    regs_[OPTION] = 0x10;
    regs_[0x3C] = 0x05;   // HPRIO: IRQ highest
    regs_[0x3D] = 0x01;   // INIT: registers at $1000, RAM at $0000
    regs_[0x3F] = 0x0C;   // CONFIG
    for (int i = 0; i < 5; ++i) { regs_[TOC1 + 2 * i] = 0xFF; regs_[TOC1 + 2 * i + 1] = 0xFF; }
    port_a_out_ = 0;
    tcnt_ = 0;
    prescale_acc_ = rti_acc_ = 0;
    tflg1_ = tflg2_ = 0;
    scsr_ = TDRE | TC;
    tx_busy_ = tx_buffer_full_ = false;
    rx_timer_ = 0;
    adc_running_ = false;
    adctl_ = 0;
    sci_listening_ = false;
    waiting_ = stopped_ = false;
    ccr = S | X | I;
    pc_ = rd16(0xFFFE);
}

// --- Bus --------------------------------------------------------------------

uint8_t Hc11::rd(uint16_t address)
{
    if ((address & 0xFFC0) == 0x1000) return reg_read(uint8_t(address & 0x3F));
    return bus_.read(address);
}

void Hc11::wr(uint16_t address, uint8_t value)
{
    if ((address & 0xFFC0) == 0x1000) { reg_write(uint8_t(address & 0x3F), value); return; }
    bus_.write(address, value);
}

uint8_t Hc11::reg_read(uint8_t r)
{
    switch (r) {
    case PORTA: {
        // PA0-PA2 are inputs (idle low), PA3-PA6 outputs, PA7 an output when DDRA7 is set.
        uint8_t out_mask = (regs_[PACTL] & 0x80) ? 0xF8 : 0x78;
        return uint8_t(port_a_out_ & out_mask);
    }
    case CFORC: return 0;
    case TCNTH: return uint8_t(tcnt_ >> 8);
    case TCNTL: return uint8_t(tcnt_);
    case TIC1: case TIC1 + 1: case TIC1 + 2: case TIC1 + 3: case TIC1 + 4: case TIC1 + 5: {
        uint16_t v = tic_[(r - TIC1) / 2];
        return (r - TIC1) & 1 ? uint8_t(v) : uint8_t(v >> 8);
    }
    case TFLG1: return tflg1_;
    case TFLG2: return tflg2_;
    case SCSR: scsr_read_ = true; return scsr_;
    case SCDR:
        if (scsr_read_) scsr_ &= uint8_t(~(RDRF | OR_ | IDLE));
        scsr_read_ = false;
        return rx_data_;
    case ADCTL: return uint8_t((adc_running_ ? 0 : 0x80) | (adctl_ & 0x3F));
    case ADR1: case ADR1 + 1: case ADR1 + 2: case ADR1 + 3:
        if (adctl_ & 0x20) {
            // Continuous scan: results always current.
            int base = (adctl_ & 0x10) ? (adctl_ & 0x04) : (adctl_ & 0x07);
            int ch = (adctl_ & 0x10) ? base + (r - ADR1) : base;
            return adc_in_[ch & 7];
        }
        return adr_[r - ADR1];
    default: return regs_[r];
    }
}

void Hc11::reg_write(uint8_t r, uint8_t v)
{
    switch (r) {
    case PORTA: {
        // Bits driven by an output-compare function ignore port writes.
        uint8_t oc_mask = 0;
        uint8_t tctl1 = regs_[TCTL1];
        if (tctl1 & 0xC0) oc_mask |= 0x40;
        if (tctl1 & 0x30) oc_mask |= 0x20;
        if (tctl1 & 0x0C) oc_mask |= 0x10;
        if (tctl1 & 0x03) oc_mask |= 0x08;
        uint8_t old = port_a_out_;
        port_a_out_ = uint8_t((port_a_out_ & oc_mask) | (v & ~oc_mask & 0xF8));
        regs_[PORTA] = v;
        if (port_a_out_ != old) bus_.port_a_changed(port_a_out_);
        break;
    }
    case CFORC:
        for (int oc = 0; oc < 5; ++oc)
            if (v & (0x80 >> oc)) apply_oc_pin(oc);
        break;
    case TCNTH: case TCNTL: break;   // read-only in normal modes
    case TIC1: case TIC1 + 1: case TIC1 + 2: case TIC1 + 3: case TIC1 + 4: case TIC1 + 5: break;
    case TFLG1: tflg1_ &= uint8_t(~v); break;
    case TFLG2: tflg2_ &= uint8_t(~v); break;
    case SCSR: break;
    case SCDR:
        scsr_read_ = false;
        if (!(regs_[SCCR2] & 0x08)) break;   // transmitter disabled
        scsr_ &= uint8_t(~(TDRE | TC));
        if (!tx_busy_) {
            tx_busy_ = true;
            tx_shift_ = v;
            tx_timer_ = sci_frame_cycles;
            scsr_ |= TDRE;                    // moved straight into the shifter
        } else {
            tx_buffer_ = v;
            tx_buffer_full_ = true;
        }
        break;
    case SCCR2:
        regs_[r] = v;
        if (v & 0x20) sci_listening_ = true;
        break;
    case ADCTL:
        adctl_ = v & 0x3F;
        adc_running_ = true;
        adc_timer_ = 128;
        break;
    case ADR1: case ADR1 + 1: case ADR1 + 2: case ADR1 + 3: break;
    case BAUD: {
        regs_[r] = v;
        static const int scp[4] = {1, 3, 4, 13};
        int bit = 16 * scp[(v >> 4) & 3] * (1 << (v & 7));
        sci_frame_cycles = bit * ((regs_[SCCR1] & 0x10) ? 11 : 10);
        break;
    }
    default: regs_[r] = v; break;
    }
}

// --- Peripherals --------------------------------------------------------------

void Hc11::input_capture(int which)
{
    if (which < 1 || which > 3) return;
    uint8_t edges = uint8_t(regs_[TCTL2] >> (2 * (3 - which)) & 3);
    if (!edges) return;
    tic_[which - 1] = tcnt_;
    tflg1_ |= uint8_t(0x04 >> (which - 1));
}

void Hc11::apply_oc_pin(int oc)
{
    uint8_t old = port_a_out_;
    if (oc == 0) {
        // OC1 drives any of PA7-PA3 selected by OC1M to the OC1D levels.
        uint8_t m = regs_[OC1M] & 0xF8;
        port_a_out_ = uint8_t((port_a_out_ & ~m) | (regs_[OC1D] & m));
    } else {
        uint8_t bit = uint8_t(0x80 >> oc);              // OC2->PA6 ... OC5->PA3
        int action = regs_[TCTL1] >> (2 * (4 - oc)) & 3;
        if (action == 1) port_a_out_ ^= bit;
        else if (action == 2) port_a_out_ &= uint8_t(~bit);
        else if (action == 3) port_a_out_ |= bit;
    }
    if (port_a_out_ != old) bus_.port_a_changed(port_a_out_);
}

void Hc11::timer_advance(int cyc)
{
    static const int prescales[4] = {1, 4, 8, 16};
    int prescale = prescales[regs_[TMSK2] & 3];
    prescale_acc_ += cyc;
    while (prescale_acc_ >= prescale) {
        prescale_acc_ -= prescale;
        ++tcnt_;
        if (tcnt_ == 0) tflg2_ |= 0x80;   // TOF
        for (int oc = 0; oc < 5; ++oc) {
            uint16_t toc = uint16_t(regs_[TOC1 + 2 * oc] << 8 | regs_[TOC1 + 2 * oc + 1]);
            if (toc == tcnt_) {
                tflg1_ |= uint8_t(0x80 >> oc);
                apply_oc_pin(oc);
            }
        }
    }
    // Real-time interrupt: E/2^13 scaled by RTR1:RTR0.
    int rti_period = 8192 << (regs_[PACTL] & 3);
    rti_acc_ += cyc;
    while (rti_acc_ >= rti_period) {
        rti_acc_ -= rti_period;
        tflg2_ |= 0x40;
    }
}

void Hc11::sci_advance(int cyc)
{
    // Queued bytes arrive at wire speed whether or not the receiver is listening; one that
    // completes while RE is off is lost, as on the real chip.
    if (!rx_queue_.empty()) {
        rx_timer_ += cyc;
        if (rx_timer_ >= sci_frame_cycles) {
            rx_timer_ -= sci_frame_cycles;
            uint8_t v = rx_queue_.front();
            rx_queue_.pop_front();
            if (!(regs_[SCCR2] & 0x04) || !sci_listening_) {
                // Receiver off, or the firmware has not yet enabled its receive interrupt
                // since power-on: the MIDI cable counts as unplugged. (The firmware restarts
                // itself on a broken message, and a byte latched during boot makes one.)
                ++rx_dropped;
            } else if (scsr_ & RDRF) {
                scsr_ |= OR_;   // overrun: the new byte is lost
                ++rx_overruns;
            } else {
                rx_data_ = v;
                scsr_ |= RDRF;
            }
            if (rx_queue_.empty()) rx_timer_ = 0;
        }
    }
    if (tx_busy_) {
        tx_timer_ -= cyc;
        if (tx_timer_ <= 0) {
            bus_.sci_transmit(tx_shift_);
            if (tx_buffer_full_) {
                tx_shift_ = tx_buffer_;
                tx_buffer_full_ = false;
                tx_timer_ += sci_frame_cycles;
                scsr_ |= TDRE;
            } else {
                tx_busy_ = false;
                if (scsr_ & TDRE) scsr_ |= TC;
            }
        }
    }
}

void Hc11::advance(int cyc)
{
    cycles_ += uint64_t(cyc);
    timer_advance(cyc);
    sci_advance(cyc);
    if (adc_running_) {
        adc_timer_ -= cyc;
        if (adc_timer_ <= 0) {
            for (int i = 0; i < 4; ++i) {
                int ch = (adctl_ & 0x10) ? ((adctl_ & 0x04) + i) : (adctl_ & 0x07);
                adr_[i] = adc_in_[ch & 7];
            }
            adc_running_ = (adctl_ & 0x20) != 0;
            adc_timer_ = 128;
            if (!adc_running_) adc_timer_ = 0;
        }
    }
}

int Hc11::pending_vector()
{
    if (ccr & I) return -1;
    if (irq_line_ && !(regs_[OPTION] & 0x20)) return 0xFFF2;
    uint8_t tmsk2 = regs_[TMSK2];
    if ((tflg2_ & 0x40) && (tmsk2 & 0x40)) return 0xFFF0;   // RTI
    uint8_t t1 = uint8_t(tflg1_ & regs_[TMSK1]);
    if (t1 & 0x04) return 0xFFEE;   // IC1
    if (t1 & 0x02) return 0xFFEC;   // IC2
    if (t1 & 0x01) return 0xFFEA;   // IC3
    if (t1 & 0x80) return 0xFFE8;   // OC1
    if (t1 & 0x40) return 0xFFE6;   // OC2
    if (t1 & 0x20) return 0xFFE4;   // OC3
    if (t1 & 0x10) return 0xFFE2;   // OC4
    if (t1 & 0x08) return 0xFFE0;   // OC5
    if ((tflg2_ & 0x80) && (tmsk2 & 0x80)) return 0xFFDE;   // TOF
    if ((tflg2_ & 0x20) && (tmsk2 & 0x20)) return 0xFFDC;   // PAOV
    if ((tflg2_ & 0x10) && (tmsk2 & 0x10)) return 0xFFDA;   // PAI
    uint8_t sccr2 = regs_[SCCR2];
    if (((sccr2 & 0x80) && (scsr_ & TDRE)) || ((sccr2 & 0x40) && (scsr_ & TC)) ||
        ((sccr2 & 0x20) && (scsr_ & (RDRF | OR_))) || ((sccr2 & 0x10) && (scsr_ & IDLE)))
        return 0xFFD6;
    return -1;
}

void Hc11::take_interrupt(uint16_t vector)
{
    if (!waiting_) {
        push16(pc_);
        push16(y);
        push16(x);
        push(a);
        push(b);
        push(ccr);
    }
    waiting_ = false;
    stopped_ = false;
    ccr |= I;
    pc_ = rd16(vector);
}

int Hc11::step()
{
    // After an RTI the CPU executes one instruction of the interrupted program before it
    // services another interrupt; without this a flag nobody clears locks the machine.
    int vec = after_rti_ ? -1 : pending_vector();
    after_rti_ = false;
    if (vec >= 0) {
        int cyc = waiting_ ? 2 : 14;
        take_interrupt(uint16_t(vec));
        advance(cyc);
        return cyc;
    }
    if (waiting_ || stopped_) {
        advance(4);
        return 4;
    }
    uint8_t op = fetch();
    int page = 1;
    if (op == 0x18) { page = 2; op = fetch(); }
    else if (op == 0x1A) { page = 3; op = fetch(); }
    else if (op == 0xCD) { page = 4; op = fetch(); }
    int cyc = execute(op, page);
    advance(cyc);
    return cyc;
}

// --- ALU ----------------------------------------------------------------------

uint8_t Hc11::add8(uint8_t l, uint8_t r, int carry)
{
    unsigned res = unsigned(l) + r + unsigned(carry);
    uint8_t v = uint8_t(res);
    flag(H, ((l & 0x0F) + (r & 0x0F) + carry) > 0x0F);
    flag(C, res > 0xFF);
    flag(V, (~(l ^ r) & (l ^ v) & 0x80) != 0);
    set_nz8(v);
    return v;
}

uint8_t Hc11::sub8(uint8_t l, uint8_t r, int carry)
{
    int res = int(l) - int(r) - carry;
    uint8_t v = uint8_t(res);
    flag(C, res < 0);
    flag(V, ((l ^ r) & (l ^ v) & 0x80) != 0);
    set_nz8(v);
    return v;
}

uint16_t Hc11::add16(uint16_t l, uint16_t r)
{
    uint32_t res = uint32_t(l) + r;
    uint16_t v = uint16_t(res);
    flag(C, res > 0xFFFF);
    flag(V, (~(l ^ r) & (l ^ v) & 0x8000) != 0);
    set_nz16(v);
    return v;
}

uint16_t Hc11::sub16(uint16_t l, uint16_t r)
{
    int32_t res = int32_t(l) - int32_t(r);
    uint16_t v = uint16_t(res);
    flag(C, res < 0);
    flag(V, ((l ^ r) & (l ^ v) & 0x8000) != 0);
    set_nz16(v);
    return v;
}

uint8_t Hc11::rmw(int op, uint8_t v)
{
    uint8_t r = v;
    switch (op) {
    case 0x0: r = uint8_t(0 - v); flag(C, r != 0); flag(V, r == 0x80); set_nz8(r); break;   // NEG
    case 0x3: r = uint8_t(~v); flag(C, true); flag(V, false); set_nz8(r); break;           // COM
    case 0x4: r = uint8_t(v >> 1); flag(C, v & 1); set_nz8(r); flag(V, v & 1); break;      // LSR
    case 0x6: r = uint8_t(v >> 1 | (ccr & C ? 0x80 : 0)); flag(C, v & 1); set_nz8(r);
              flag(V, bool(ccr & N) != bool(ccr & C)); break;                              // ROR
    case 0x7: r = uint8_t(v >> 1 | (v & 0x80)); flag(C, v & 1); set_nz8(r);
              flag(V, bool(ccr & N) != bool(ccr & C)); break;                              // ASR
    case 0x8: r = uint8_t(v << 1); flag(C, v & 0x80); set_nz8(r);
              flag(V, bool(ccr & N) != bool(ccr & C)); break;                              // ASL
    case 0x9: r = uint8_t(v << 1 | (ccr & C ? 1 : 0)); flag(C, v & 0x80); set_nz8(r);
              flag(V, bool(ccr & N) != bool(ccr & C)); break;                              // ROL
    case 0xA: r = uint8_t(v - 1); flag(V, v == 0x80); set_nz8(r); break;                    // DEC
    case 0xC: r = uint8_t(v + 1); flag(V, v == 0x7F); set_nz8(r); break;                    // INC
    case 0xD: flag(V, false); flag(C, false); set_nz8(v); break;                            // TST
    case 0xF: r = 0; ccr = uint8_t((ccr & ~(N | V | C)) | Z); break;                        // CLR
    }
    return r;
}

// --- Instructions ---------------------------------------------------------------

int Hc11::execute(uint8_t op, int page)
{
    uint16_t& ix = (page == 2 || page == 4) ? y : x;   // index register for ,X/,Y modes
    uint16_t& xr = (page == 2 || page == 3) ? y : x;   // register for LDX/STX/CPX/INX... family
    int cyc = kCycles[op] + (page > 1 ? 1 : 0);

    auto branch = [&](bool cond) {
        int8_t rel = int8_t(fetch());
        if (cond) pc_ = uint16_t(pc_ + rel);
    };
    auto illegal = [&]() {
        ++illegal_count;
        illegal_pc = uint16_t(pc_ - 1);
        push16(pc_); push16(y); push16(x); push(a); push(b); push(ccr);
        ccr |= I;
        pc_ = rd16(0xFFF8);
        return 14;
    };

    // Inherent and bit-manipulation opcodes ($00-$5F).
    if (op < 0x60) {
        switch (op) {
        case 0x00: return illegal();                 // TEST (only legal in test mode)
        case 0x01: break;                            // NOP
        case 0x02: {                                 // IDIV
            uint16_t num = d();
            if (x == 0) { x = 0xFFFF; flag(C, true); }
            else {
                uint16_t q = uint16_t(num / x), rem = uint16_t(num % x);
                x = q; set_d(rem); flag(C, false);
            }
            flag(V, false); flag(Z, x == 0);
            break;
        }
        case 0x03: {                                 // FDIV
            uint16_t num = d();
            if (x == 0) { x = 0xFFFF; flag(C, true); flag(V, true); }
            else if (x <= num) { x = 0xFFFF; flag(V, true); flag(C, false); }
            else {
                uint32_t n = uint32_t(num) << 16;
                uint16_t q = uint16_t(n / x), rem = uint16_t(n % x);
                x = q; set_d(rem); flag(V, false); flag(C, false);
            }
            flag(Z, x == 0);
            break;
        }
        case 0x04: { uint16_t v = d(); flag(C, v & 1); v >>= 1; set_d(v); set_nz16(v); flag(V, ccr & C); break; }   // LSRD
        case 0x05: { uint16_t v = d(); flag(C, v & 0x8000); v = uint16_t(v << 1); set_d(v); set_nz16(v);
                     flag(V, bool(ccr & N) != bool(ccr & C)); break; }   // ASLD
        case 0x06: ccr = uint8_t((a & ~X) | (ccr & a & X)); break;      // TAP (X can't be set)
        case 0x07: a = ccr; break;                                       // TPA
        case 0x08: ++xr; flag(Z, xr == 0); break;                        // INX/INY
        case 0x09: --xr; flag(Z, xr == 0); break;                        // DEX/DEY
        case 0x0A: flag(V, false); break;
        case 0x0B: flag(V, true); break;
        case 0x0C: flag(C, false); break;
        case 0x0D: flag(C, true); break;
        case 0x0E: flag(I, false); break;
        case 0x0F: flag(I, true); break;
        case 0x10: a = sub8(a, b, 0); break;                             // SBA
        case 0x11: sub8(a, b, 0); break;                                 // CBA
        case 0x12: case 0x13: {                                          // BRSET/BRCLR dir
            uint8_t m = rd(fetch());
            uint8_t mask = fetch();
            branch(op == 0x12 ? (m & mask) == mask : (m & mask) == 0);
            break;
        }
        case 0x14: case 0x15: {                                          // BSET/BCLR dir
            uint16_t ea = fetch();
            uint8_t mask = fetch();
            uint8_t v = rd(ea);
            v = op == 0x14 ? uint8_t(v | mask) : uint8_t(v & ~mask);
            set_nz8(v); flag(V, false);
            wr(ea, v);
            break;
        }
        case 0x16: b = a; set_nz8(b); flag(V, false); break;             // TAB
        case 0x17: a = b; set_nz8(a); flag(V, false); break;             // TBA
        case 0x19: {                                                     // DAA
            uint8_t cf = 0;
            uint8_t lsn = a & 0x0F, msn = a >> 4;
            if ((ccr & H) || lsn > 9) cf |= 0x06;
            if ((ccr & C) || msn > 9 || (msn > 8 && lsn > 9)) cf |= 0x60;
            unsigned t = unsigned(a) + cf;
            a = uint8_t(t);
            if (cf & 0x60) flag(C, true);
            set_nz8(a);
            break;
        }
        case 0x1B: a = add8(a, b, 0); break;                             // ABA
        case 0x1C: case 0x1D: {                                          // BSET/BCLR ind
            uint16_t ea = uint16_t(ix + fetch());
            uint8_t mask = fetch();
            uint8_t v = rd(ea);
            v = op == 0x1C ? uint8_t(v | mask) : uint8_t(v & ~mask);
            set_nz8(v); flag(V, false);
            wr(ea, v);
            break;
        }
        case 0x1E: case 0x1F: {                                          // BRSET/BRCLR ind
            uint8_t m = rd(uint16_t(ix + fetch()));
            uint8_t mask = fetch();
            branch(op == 0x1E ? (m & mask) == mask : (m & mask) == 0);
            break;
        }
        case 0x20: branch(true); break;
        case 0x21: branch(false); break;
        case 0x22: branch(!(ccr & (C | Z))); break;
        case 0x23: branch(ccr & (C | Z)); break;
        case 0x24: branch(!(ccr & C)); break;
        case 0x25: branch(ccr & C); break;
        case 0x26: branch(!(ccr & Z)); break;
        case 0x27: branch(ccr & Z); break;
        case 0x28: branch(!(ccr & V)); break;
        case 0x29: branch(ccr & V); break;
        case 0x2A: branch(!(ccr & N)); break;
        case 0x2B: branch(ccr & N); break;
        case 0x2C: branch(bool(ccr & N) == bool(ccr & V)); break;
        case 0x2D: branch(bool(ccr & N) != bool(ccr & V)); break;
        case 0x2E: branch(!(ccr & Z) && bool(ccr & N) == bool(ccr & V)); break;
        case 0x2F: branch((ccr & Z) || bool(ccr & N) != bool(ccr & V)); break;
        case 0x30: xr = uint16_t(sp + 1); break;                         // TSX/TSY
        case 0x31: ++sp; break;                                          // INS
        case 0x32: a = pull(); break;
        case 0x33: b = pull(); break;
        case 0x34: --sp; break;                                          // DES
        case 0x35: sp = uint16_t(xr - 1); break;                         // TXS/TYS
        case 0x36: push(a); break;
        case 0x37: push(b); break;
        case 0x38: xr = pull16(); break;                                 // PULX/PULY
        case 0x39: pc_ = pull16(); break;                                // RTS
        case 0x3A: xr = uint16_t(xr + b); break;                         // ABX/ABY
        case 0x3B:                                                       // RTI
            {
                uint8_t newccr = pull();
                // X can be cleared but not set by RTI.
                ccr = uint8_t((newccr & ~X) | (ccr & newccr & X));
            }
            b = pull(); a = pull(); x = pull16(); y = pull16(); pc_ = pull16();
            after_rti_ = true;
            break;
        case 0x3C: push16(xr); break;                                    // PSHX/PSHY
        case 0x3D: { uint16_t r = uint16_t(a * b); set_d(r); flag(C, r & 0x80); break; }   // MUL
        case 0x3E:                                                       // WAI
            push16(pc_); push16(y); push16(x); push(a); push(b); push(ccr);
            waiting_ = true;
            break;
        case 0x3F:                                                       // SWI
            push16(pc_); push16(y); push16(x); push(a); push(b); push(ccr);
            ccr |= I;
            pc_ = rd16(0xFFF6);
            break;
        default:
            if (op >= 0x40) {
                int sub = op & 0x0F;
                if (sub == 1 || sub == 2 || sub == 5 || sub == 0xB || sub == 0xE) return illegal();
                uint8_t& r = op < 0x50 ? a : b;
                uint8_t v = rmw(sub, r);
                if (sub != 0xD) r = v;
                break;
            }
            return illegal();
        }
        return cyc;
    }

    // Memory read-modify-write ($60-$7F).
    if (op < 0x80) {
        int sub = op & 0x0F;
        uint16_t ea = op < 0x70 ? uint16_t(ix + fetch()) : fetch16();
        if (sub == 0xE) { pc_ = ea; return cyc; }                        // JMP
        if (sub == 1 || sub == 2 || sub == 5 || sub == 0xB) return illegal();
        uint8_t v = rmw(sub, rd(ea));
        if (sub != 0xD) wr(ea, v);
        return cyc;
    }

    // Accumulator/memory instructions ($80-$FF).
    int mode = (op >> 4) & 3;   // 0 imm, 1 dir, 2 ind, 3 ext
    int sub = op & 0x0F;
    bool is_b = op >= 0xC0;
    bool wide = (sub == 0x3) || (sub == 0xC) || (sub == 0xE) || (is_b && sub == 0xD) ||
                (!is_b && sub == 0xF) || (is_b && sub == 0xF);

    // Special immediate-row opcodes.
    if (mode == 0) {
        if (!is_b && sub == 0xD) { int8_t rel = int8_t(fetch()); push16(pc_); pc_ = uint16_t(pc_ + rel); return cyc; }   // BSR
        if (!is_b && sub == 0xF) { uint16_t t = d(); set_d(xr); xr = t; return cyc; }   // XGDX/XGDY
        if (!is_b && sub == 0x7) return illegal();
        if (is_b && sub == 0x7) return illegal();
        if (is_b && sub == 0xD) return illegal();                        // (prefix byte; unreachable)
        if (is_b && sub == 0xF) {                                        // STOP
            if (!(ccr & S)) stopped_ = true;
            return cyc;
        }
    }

    uint16_t ea = 0;
    switch (mode) {
    case 0: ea = pc_; pc_ = uint16_t(pc_ + (wide ? 2 : 1)); break;
    case 1: ea = fetch(); break;
    case 2: ea = uint16_t(ix + fetch()); break;
    case 3: ea = fetch16(); break;
    }

    bool cpd = (page == 3 || page == 4);
    uint8_t& acc = is_b ? b : a;
    switch (sub) {
    case 0x0: acc = sub8(acc, rd(ea), 0); break;
    case 0x1: sub8(acc, rd(ea), 0); break;
    case 0x2: acc = sub8(acc, rd(ea), ccr & C ? 1 : 0); break;
    case 0x3:
        if (is_b) set_d(add16(d(), rd16(ea)));                           // ADDD
        else if (cpd) sub16(d(), rd16(ea));                              // CPD
        else set_d(sub16(d(), rd16(ea)));                                // SUBD
        break;
    case 0x4: acc &= rd(ea); set_nz8(acc); flag(V, false); break;
    case 0x5: set_nz8(uint8_t(acc & rd(ea))); flag(V, false); break;
    case 0x6: acc = rd(ea); set_nz8(acc); flag(V, false); break;
    case 0x7: wr(ea, acc); set_nz8(acc); flag(V, false); break;
    case 0x8: acc ^= rd(ea); set_nz8(acc); flag(V, false); break;
    case 0x9: acc = add8(acc, rd(ea), ccr & C ? 1 : 0); break;
    case 0xA: acc |= rd(ea); set_nz8(acc); flag(V, false); break;
    case 0xB: acc = add8(acc, rd(ea), 0); break;
    case 0xC:
        if (is_b) { uint16_t v = rd16(ea); set_d(v); set_nz16(v); flag(V, false); }   // LDD
        else sub16(xr, rd16(ea));                                        // CPX/CPY
        break;
    case 0xD:
        if (is_b) { uint16_t v = d(); wr16(ea, v); set_nz16(v); flag(V, false); }     // STD
        else { push16(pc_); pc_ = ea; }                                  // JSR
        break;
    case 0xE:
        if (is_b) { xr = rd16(ea); set_nz16(xr); }                       // LDX/LDY
        else { sp = rd16(ea); set_nz16(sp); }                            // LDS
        flag(V, false);
        break;
    case 0xF:
        if (is_b) { wr16(ea, xr); set_nz16(xr); }                        // STX/STY
        else { wr16(ea, sp); set_nz16(sp); }                             // STS
        flag(V, false);
        break;
    }
    return cyc;
}

} // namespace sidportal
