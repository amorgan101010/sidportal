// Instruction-level checks for the 68HC11 core: prefixed (Y) forms, 16-bit compares,
// bit branches, IDIV/FDIV, interrupt stacking order and TFLG1 write-1-to-clear.
#include "hc11.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace sidportal;

namespace {

struct Mem : Hc11Bus {
    uint8_t m[0x10000];
    Mem() { std::memset(m, 0, sizeof m); m[0xFFFE] = 0x80; m[0xFFFF] = 0x00; }
    uint8_t read(uint16_t a) override { return m[a]; }
    void write(uint16_t a, uint8_t v) override { m[a] = v; }
};

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

void load(Mem& mem, uint16_t at, std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) mem.m[at++] = b;
}

} // namespace

int main()
{
    {
        Mem mem;
        Hc11 cpu(mem);
        load(mem, 0x8000, {
            0x8E, 0x1F, 0xFF,             // lds #$1FFF
            0x18, 0xCE, 0x12, 0x34,       // ldy #$1234
            0x18, 0x08,                   // iny
            0x1A, 0x83, 0x12, 0x00,       // cpd #$1200 (D = 0)
            0xCC, 0x00, 0x07,             // ldd #7
            0xCE, 0x00, 0x02,             // ldx #2
            0x02,                         // idiv -> X=3, D=1
            0x18, 0x3C,                   // pshy
            0x38,                         // pulx -> X=$1235
            0x86, 0x81,                   // ldaa #$81
            0x97, 0x40,                   // staa $40
            0x12, 0x40, 0x81, 0x02,       // brset $40,#$81,+2
            0x86, 0x00,                   // (skipped)
            0x15, 0x40, 0x01,             // bclr $40,#$01
            0x1A, 0xB3, 0x00, 0x40,       // cpd $40 (just exercises page 3 ext)
            0x01,                         // nop
        });
        cpu.reset();
        for (int i = 0; i < 3; ++i) cpu.step();
        check(cpu.y == 0x1235, "ldy/iny");
        cpu.step();
        check((cpu.ccr & 0x01) != 0, "cpd sets carry when D < operand");
        for (int i = 0; i < 3; ++i) cpu.step();
        check(cpu.x == 3 && cpu.b == 1, "idiv");
        cpu.step(); cpu.step();
        check(cpu.x == 0x1235 && cpu.sp == 0x1FFF, "pshy/pulx");
        cpu.step(); cpu.step(); cpu.step();
        check(cpu.pc() == 0x8021, "brset taken");
        cpu.step();
        check(mem.m[0x40] == 0x80, "bclr");
    }
    {
        // Output compare interrupt through the vector, and BSET on TFLG1 clearing all flags.
        Mem mem;
        Hc11 cpu(mem);
        mem.m[0xFFE6] = 0x90; mem.m[0xFFE7] = 0x00;   // OC2 vector
        load(mem, 0x8000, {
            0x8E, 0x1F, 0xFF,             // lds #$1FFF
            0xCE, 0x10, 0x00,             // ldx #$1000
            0xCC, 0x00, 0x10,             // ldd #$0010
            0xED, 0x18,                   // std TOC2
            0x86, 0x40, 0xA7, 0x22,       // ldaa #$40; staa TMSK1
            0x0E,                         // cli
            0x20, 0xFE,                   // bra *
        });
        load(mem, 0x9000, {
            0x1C, 0x23, 0x40,             // bset TFLG1,#$40  (clears every set flag)
            0x7C, 0x00, 0x50,             // inc $0050
            0x3B,                         // rti
        });
        cpu.reset();
        for (int i = 0; i < 2000; ++i) cpu.step();
        check(mem.m[0x50] >= 1, "OC2 interrupt taken");
        check(cpu.sp == 0x1FFF, "stack balanced after rti");
    }
    {
        // FDIV: $4000 / $8000 = 0.5 -> $8000
        Mem mem;
        Hc11 cpu(mem);
        load(mem, 0x8000, {0xCC, 0x40, 0x00, 0xCE, 0x80, 0x00, 0x03});
        cpu.reset();
        cpu.step(); cpu.step(); cpu.step();
        check(cpu.x == 0x8000, "fdiv");
    }
    {
        // A pending interrupt whose handler never clears its flag must still let the main
        // program run one instruction per RTI (the firmware's boot SCI handler is a bare RTI).
        Mem mem;
        Hc11 cpu(mem);
        mem.m[0xFFE6] = 0x90; mem.m[0xFFE7] = 0x00;   // OC2 vector -> RTI, flag never cleared
        mem.m[0x9000] = 0x3B;
        load(mem, 0x8000, {
            0x8E, 0x1F, 0xFF,             // lds #$1FFF
            0xCE, 0x10, 0x00,             // ldx #$1000
            0xCC, 0x00, 0x10, 0xED, 0x18, // TOC2 = $0010
            0x86, 0x40, 0xA7, 0x22,       // enable OC2
            0x0E,                         // cli
            0x7C, 0x00, 0x60,             // loop: inc $0060
            0x20, 0xFB,                   // bra loop
        });
        cpu.reset();
        for (int i = 0; i < 5000; ++i) cpu.step();
        check(mem.m[0x60] > 10, "main program advances during an interrupt storm");
    }
    if (failures == 0) std::puts("hc11: all checks passed");
    return failures ? 1 : 0;
}
