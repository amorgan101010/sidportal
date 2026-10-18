// Hitachi HD44780 character LCD controller (2 x 16, 8-bit bus). The unit maps
// the instruction register at $1060 and the data register at $1061.
#pragma once
#include <cstdint>
#include <cstring>

namespace sidportal {

class Hd44780 {
public:
    Hd44780() { reset(); }

    void reset()
    {
        std::memset(ddram_, 0x20, sizeof ddram_);
        std::memset(cgram_, 0, sizeof cgram_);
        ac_ = 0;
        increment_ = true;
        shift_on_write_ = false;
        display_on_ = cursor_on_ = blink_on_ = false;
        shift_ = 0;
        cgram_mode_ = false;
    }

    void write(bool data, uint8_t v)
    {
        ++version_;
        if (data) { write_data(v); return; }
        if (v & 0x80) { cgram_mode_ = false; ac_ = v & 0x7F; }
        else if (v & 0x40) { cgram_mode_ = true; ac_ = v & 0x3F; }
        else if (v & 0x20) { /* function set: always 8-bit, 2 lines */ }
        else if (v & 0x10) {
            bool display = v & 0x08, right = v & 0x04;
            if (display) shift_ = (shift_ + (right ? 39 : 1)) % 40;   // right: contents move right
            else move_ac(right);
        }
        else if (v & 0x08) { display_on_ = v & 0x04; cursor_on_ = v & 0x02; blink_on_ = v & 0x01; }
        else if (v & 0x04) { increment_ = v & 0x02; shift_on_write_ = v & 0x01; }
        else if (v & 0x02) { ac_ = 0; shift_ = 0; cgram_mode_ = false; }
        else if (v & 0x01) {
            std::memset(ddram_, 0x20, sizeof ddram_);
            ac_ = 0; shift_ = 0; increment_ = true; cgram_mode_ = false;
        }
    }

    uint8_t read(bool data)
    {
        if (!data) return ac_ & 0x7F;   // busy flag always clear
        uint8_t v = cgram_mode_ ? cgram_[ac_ & 0x3F] : ddram_[ddram_index(ac_)];
        move_ac(increment_);
        return v;
    }

    // Character code shown at (row, column), honouring the display shift.
    uint8_t visible(int row, int col) const
    {
        int pos = (col + shift_) % 40;
        return ddram_[row * 40 + pos];
    }
    const uint8_t* cgram() const { return cgram_; }
    bool display_on() const { return display_on_; }
    bool cursor_on() const { return cursor_on_; }
    bool blink_on() const { return blink_on_; }
    // Cursor position as (row, column) on screen, or -1 when off screen.
    int cursor_row() const { return (ac_ & 0x40) ? 1 : 0; }
    int cursor_col() const { return ((ac_ & 0x3F) - shift_ + 40) % 40; }
    uint32_t version() const { return version_; }

private:
    static int ddram_index(uint8_t ac) { return ((ac & 0x40) ? 40 : 0) + ((ac & 0x3F) % 40); }

    void write_data(uint8_t v)
    {
        if (cgram_mode_) cgram_[ac_ & 0x3F] = v;
        else ddram_[ddram_index(ac_)] = v;
        move_ac(increment_);
        if (shift_on_write_ && !cgram_mode_) shift_ = (shift_ + (increment_ ? 1 : 39)) % 40;
    }

    void move_ac(bool up)
    {
        if (cgram_mode_) { ac_ = uint8_t((ac_ + (up ? 1 : -1)) & 0x3F); return; }
        int line = (ac_ & 0x40) ? 1 : 0;
        int pos = ac_ & 0x3F;
        if (up) { if (++pos >= 40) { pos = 0; line ^= 1; } }
        else { if (--pos < 0) { pos = 39; line ^= 1; } }
        ac_ = uint8_t(line * 0x40 + pos);
    }

    uint8_t ddram_[80];
    uint8_t cgram_[64];
    uint8_t ac_;
    bool increment_, shift_on_write_;
    bool display_on_, cursor_on_, blink_on_;
    int shift_;
    bool cgram_mode_;
    uint32_t version_ = 0;
};

} // namespace sidportal
