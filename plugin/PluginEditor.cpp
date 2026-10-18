#include "PluginEditor.h"

#include "SidPortalAssets.h"
#include "lcd_font.h"

#include <cmath>
#include <cstring>

namespace {

const juce::Colour kLcdBg(0xffa3bd3c), kLcdOff(0xff9db73a), kLcdOn(0xff1c2410), kLcdHot(0xffb4cc4e);
constexpr uint8_t kLedBits[4] = {0x40, 0x20, 0x10, 0x08};   // Sync, MIDI, Edit, Ext clk (active low)

juce::Image asset(const char* name)
{
    int size = 0;
    const char* data = SidPortalAssets::getNamedResource(name, size);
    return data ? juce::ImageCache::getFromMemory(data, size) : juce::Image();
}

juce::var load_layout()
{
    int size = 0;
    const char* data = SidPortalAssets::getNamedResource("layout_json", size);
    return data ? juce::JSON::parse(juce::String::fromUTF8(data, size)) : juce::var();
}

float num(const juce::var& v) { return float(double(v)); }

// The art is exported at 2x: draw each image into its 1x logical rectangle, so it stays
// sharp when the editor is enlarged or on a HiDPI screen.
float art_scale = 2.0f;

void draw_at(juce::Graphics& g, const juce::Image& img, float x, float y)
{
    g.drawImage(img, {x, y, img.getWidth() / art_scale, img.getHeight() / art_scale});
}

void draw_centred(juce::Graphics& g, const juce::Image& img, juce::Point<float> c)
{
    draw_at(g, img, c.x - img.getWidth() / art_scale / 2, c.y - img.getHeight() / art_scale / 2);
}
juce::Point<float> pt(const juce::var& v) { return {num(v[0]), num(v[1])}; }
juce::Rectangle<float> box(const juce::var& v)
{
    return juce::Rectangle<float>::leftTopRightBottom(num(v[0]), num(v[1]), num(v[2]), num(v[3]));
}

} // namespace

PanelView::PanelView(SidPortalProcessor& p) : proc_(p), layout_(load_layout())
{
    panel_ = asset("panel_png");
    knob_ = asset("knob_png");
    small_knob_ = asset("small_knob_png");
    encoder_ = asset("encoder_png");
    slider_ = asset("slider_png");
    gear_up_ = asset("gear_up_png");
    gear_down_ = asset("gear_down_png");
    character_ = asset("character_png");
    led_off_ = asset("led_off_png");
    led_on_ = asset("led_on_png");
    dimple_ = asset("dimple_png");
    subtitle_6581_ = asset("subtitle_6581_png");
    subtitle_8580_ = asset("subtitle_8580_png");
    if (layout_.hasProperty("scale")) art_scale = num(layout_["scale"]);
    for (int code = 0; code < 16; ++code) {
        keys_[code][0] = asset(("key" + juce::String(code) + "_up_png").toRawUTF8());
        keys_[code][1] = asset(("key" + juce::String(code) + "_down_png").toRawUTF8());
    }
    const auto& L = layout_;
    for (int i = 0; i < 4; ++i)
        knobs_.push_back({p.knob_param(i), pt(L["knobs"][i]), num(L["knob_r"]), &knob_});
    knobs_.push_back({p.volume_param(), pt(L["volume"]), num(L["small_knob_r"]), &small_knob_});
    knobs_.push_back({p.character_param(), pt(L["character"]), num(L["small_knob_r"]), &small_knob_});
    setSize(int(L["width"]), int(L["height"]));
    startTimerHz(30);
}

PanelView::~PanelView() { stopTimer(); }

bool PanelView::character_visible() const
{
    // reSID's character control moves the 6581's filter only; reSIDfp has one for each chip.
    return !(proc_.engine() == sidportal::SidEngineType::ReSid && proc_.chip_param()->getValue() > 0.5f);
}

void PanelView::timerCallback()
{
    if (++blink_ticks_ >= 12) { blink_ticks_ = 0; blink_phase_ = !blink_phase_; }
    snap_ = proc_.snapshot();
    repaint();
}

// --- Painting -------------------------------------------------------------------------

void PanelView::paint_lcd(juce::Graphics& g)
{
    auto glass = box(layout_["lcd_glass"]);
    g.setColour(kLcdBg);
    g.fillRect(glass);
    g.setColour(kLcdHot);
    g.fillRect(glass.withHeight(3.0f));
    // Whole-pixel grid as in the standalone: 3 px dots on a 4 px pitch.
    const int pitch = 4, dot = 3;
    const int ox = int(glass.getX() + (glass.getWidth() - 95 * pitch) / 2);
    const int oy = int(glass.getY() + (glass.getHeight() - 18 * pitch) / 2);

    uint8_t chars[32];
    bool on = snap_.on;
    if (snap_.ready) {
        std::memcpy(chars, snap_.chars, 32);
    } else {
        // Not up yet (or no firmware): say so on the display itself.
        auto problem = proc_.problem();
        juce::String l1 = problem.isEmpty() ? "  Starting up..." : "No firmware";
        juce::String l2 = problem.isEmpty() ? "" : "tools/roms.py";
        std::memset(chars, ' ', 32);
        for (int i = 0; i < 16 && i < l1.length(); ++i) chars[i] = uint8_t(l1[i]);
        for (int i = 0; i < 16 && i < l2.length(); ++i) chars[16 + i] = uint8_t(l2[i]);
        on = true;
    }
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 16; ++col) {
            uint8_t rows[8] = {};
            if (on) {
                const uint8_t code = chars[row * 16 + col];
                for (int py = 0; py < 8; ++py) {
                    if (code < 16) {   // CGRAM: bit 4 = leftmost column
                        uint8_t v = snap_.cgram[(code & 7) * 8 + py] & 0x1F, m = 0;
                        for (int b = 0; b < 5; ++b) if (v & (0x10 >> b)) m |= uint8_t(1 << b);
                        rows[py] = m;
                    } else {
                        rows[py] = kLcdFontA00[code * 8 + py];
                    }
                }
                if (snap_.ready && row == snap_.row && col == snap_.col) {
                    if (snap_.cursor) rows[7] = 0x1F;
                    if (snap_.blink && blink_phase_) std::memset(rows, 0x1F, 8);
                }
            }
            for (int py = 0; py < 8; ++py)
                for (int px = 0; px < 5; ++px) {
                    g.setColour((rows[py] >> px & 1) ? kLcdOn : kLcdOff);
                    g.fillRect(ox + (col * 6 + px) * pitch, oy + (row * 10 + py) * pitch, dot, dot);
                }
        }
}

void PanelView::paint_knob(juce::Graphics& g, const KnobSpot& k)
{
    draw_centred(g, *k.body, k.centre);
    const float a = juce::degreesToRadians(135.0f + k.param->getValue() * 270.0f);
    const float r0 = k.r * 0.30f, r1 = k.r * 0.86f;
    g.setColour(juce::Colour(0xfff2f2f2));
    g.drawLine({k.centre.x + r0 * std::cos(a), k.centre.y + r0 * std::sin(a),
                k.centre.x + r1 * std::cos(a), k.centre.y + r1 * std::sin(a)},
               k.r > 26 ? 3.0f : 2.5f);
}

void PanelView::paint(juce::Graphics& g)
{
    const auto& L = layout_;
    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    draw_at(g, panel_, 0, 0);

    const bool is8580 = proc_.chip_param()->getValue() > 0.5f;
    auto sub = pt(L["subtitle"]);
    draw_at(g, is8580 ? subtitle_8580_ : subtitle_6581_, sub.x, sub.y);

    paint_lcd(g);

    for (int i = 0; i < 4; ++i) {
        const bool lit = snap_.ready && !(snap_.leds & kLedBits[i]);
        draw_centred(g, lit ? led_on_ : led_off_, pt(L["leds"][i]));
    }

    for (int i = 0; i < 5; ++i) paint_knob(g, knobs_[size_t(i)]);
    if (character_visible()) {
        auto at = pt(L["character_legend_at"]);
        draw_at(g, character_, at.x, at.y);
        paint_knob(g, knobs_[5]);
    }

    // Encoder with its finger dimple.
    auto ec = pt(L["encoder"]);
    const float er = num(L["encoder_r"]);
    draw_centred(g, encoder_, ec);
    const float ea = juce::degreesToRadians(enc_angle_);
    const float d = er * 0.62f;
    draw_centred(g, dimple_, {ec.x + d * std::cos(ea), ec.y + d * std::sin(ea)});

    // Keypad.
    auto origin = pt(L["keypad_origin"]);
    const float pitch = num(L["key_pitch"]);
    for (int code = 0; code < 16; ++code) {
        const int row = code / 4, col = code % 4;
        draw_at(g, keys_[code][held_key_ == code ? 1 : 0], origin.x + col * pitch, origin.y + row * pitch);
    }

    // Chip switch slider and the gear.
    auto sw = pt(L["chip_switch"]);
    const float travel = num(L["switch_slot"][0]) / 2 - 17;
    const float sx = sw.x + (is8580 ? travel : -travel);
    draw_centred(g, slider_, {sx, sw.y});
    auto gc = pt(L["gear"]);
    auto& gear = gear_down_now_ ? gear_down_ : gear_up_;
    draw_centred(g, gear, gc);
}

// --- Mouse ----------------------------------------------------------------------------

int PanelView::knob_at(juce::Point<float> p) const
{
    for (size_t i = 0; i < knobs_.size(); ++i) {
        if (i == 5 && !character_visible()) continue;
        if (p.getDistanceFrom(knobs_[i].centre) <= knobs_[i].r + 4) return int(i);
    }
    return -1;
}

int PanelView::key_at(juce::Point<float> p) const
{
    auto origin = pt(layout_["keypad_origin"]);
    const float pitch = num(layout_["key_pitch"]), size = num(layout_["key_size"]);
    for (int code = 0; code < 16; ++code) {
        juce::Rectangle<float> r(origin.x + (code % 4) * pitch, origin.y + (code / 4) * pitch, size, size);
        if (r.contains(p)) return code;
    }
    return -1;
}

void PanelView::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.position;
    const auto& L = layout_;
    if (p.getDistanceFrom(pt(L["gear"])) <= 15) { open_menu(); return; }
    if (int code = key_at(p); code >= 0) {
        held_key_ = code;
        proc_.key(code, true);
        repaint();
        return;
    }
    auto sw = pt(L["chip_switch"]);
    const float w = num(L["switch_slot"][0]), h = num(L["switch_slot"][1]);
    if (juce::Rectangle<float>::leftTopRightBottom(sw.x - w / 2 - 6, sw.y - h / 2 - 22, sw.x + w / 2 + 6,
                                                   sw.y + h / 2 + 4).contains(p)) {
        // Clicking either half picks that side; clicking the slider flips it.
        const bool right = proc_.chip_param()->getValue() > 0.5f;
        const float travel = w / 2 - 17;
        bool target = p.x > sw.x;
        if (std::abs(p.x - sw.x - (right ? travel : -travel)) < 14) target = !right;
        auto* chip = proc_.chip_param();
        chip->beginChangeGesture();
        chip->setValueNotifyingHost(target ? 1.0f : 0.0f);
        chip->endChangeGesture();
        repaint();
        return;
    }
    if (int k = knob_at(p); k >= 0) {
        drag_knob_ = k;
        drag_start_y_ = p.y;
        drag_start_v_ = knobs_[size_t(k)].param->getValue();
        knobs_[size_t(k)].param->beginChangeGesture();
        return;
    }
    auto ec = pt(L["encoder"]);
    if (p.getDistanceFrom(ec) <= num(L["encoder_r"]) + 6) {
        drag_encoder_ = true;
        enc_last_ = juce::radiansToDegrees(std::atan2(p.y - ec.y, p.x - ec.x));
    }
}

void PanelView::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_knob_ >= 0) {
        const float scale = e.mods.isShiftDown() ? 0.25f : 1.0f;   // Shift: fine
        const float v = juce::jlimit(0.0f, 1.0f, drag_start_v_ + (drag_start_y_ - e.position.y) * scale / 255.0f);
        knobs_[size_t(drag_knob_)].param->setValueNotifyingHost(v);
        repaint();
    } else if (drag_encoder_) {
        auto ec = pt(layout_["encoder"]);
        const float a = juce::radiansToDegrees(std::atan2(e.position.y - ec.y, e.position.x - ec.x));
        const float d = std::fmod(a - enc_last_ + 540.0f, 360.0f) - 180.0f;
        enc_last_ = a;
        enc_angle_ += d;
        enc_acc_ += d;
        const int steps = int(enc_acc_ / 15.0f);   // 24 detents per turn
        if (steps) {
            enc_acc_ -= steps * 15.0f;
            proc_.encoder(steps);
        }
        repaint();
    }
}

void PanelView::mouseUp(const juce::MouseEvent&)
{
    if (held_key_ >= 0) { proc_.key(held_key_, false); held_key_ = -1; repaint(); }
    if (drag_knob_ >= 0) { knobs_[size_t(drag_knob_)].param->endChangeGesture(); drag_knob_ = -1; }
    drag_encoder_ = false;
}

void PanelView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (knob_at(e.position) == 5) {   // the filter character back to the centre
        auto* c = proc_.character_param();
        c->beginChangeGesture();
        c->setValueNotifyingHost(c->convertTo0to1(0.0f));
        c->endChangeGesture();
    }
}

void PanelView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int dir = w.deltaY > 0 ? 1 : (w.deltaY < 0 ? -1 : 0);
    if (!dir) return;
    if (int k = knob_at(e.position); k >= 0) {
        auto* p = knobs_[size_t(k)].param;
        p->beginChangeGesture();
        p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, p->getValue() + dir * 4.0f / 255.0f));
        p->endChangeGesture();
    } else if (e.position.getDistanceFrom(pt(layout_["encoder"])) <= num(layout_["encoder_r"]) + 6) {
        enc_angle_ += dir * 15.0f;
        proc_.encoder(dir);
    }
    repaint();
}

// --- Gear menu --------------------------------------------------------------------------

void PanelView::open_menu()
{
    gear_down_now_ = true;
    repaint();
    juce::PopupMenu engines;
    const bool fp = proc_.engine() == sidportal::SidEngineType::ReSidFp;
    engines.addItem(10, "reSIDfp (detailed filter model)", true, fp);
    engines.addItem(11, "reSID (classic)", true, !fp);
    juce::PopupMenu menu;
    menu.addItem(1, "Load SysEx...");
    menu.addSubMenu("SID emulation", engines);
    auto gc = pt(layout_["gear"]);
    auto area = localAreaToGlobal(juce::Rectangle<int>(int(gc.x) - 14, int(gc.y) - 14, 28, 28));
    juce::Component::SafePointer<PanelView> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(area), [self](int id) {
        if (!self) return;
        self->gear_down_now_ = false;
        self->repaint();
        if (id == 1) self->load_sysex();
        else if (id == 10) self->proc_.set_engine(sidportal::SidEngineType::ReSidFp);
        else if (id == 11) self->proc_.set_engine(sidportal::SidEngineType::ReSid);
    });
}

void PanelView::load_sysex()
{
    juce::File start(juce::String(SIDPORTAL_SOURCE_DIR) + "/patches");
    chooser_ = std::make_unique<juce::FileChooser>("Send SysEx to the SidPortal",
                                                   start.isDirectory() ? start : juce::File(), "*.syx");
    juce::Component::SafePointer<PanelView> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [self](const juce::FileChooser& fc) {
        if (!self) return;
        auto file = fc.getResult();
        if (!file.existsAsFile()) return;
        juce::MemoryBlock data;
        if (!file.loadFileAsData(data)) return;
        auto opts = juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::QuestionIcon)
                        .withTitle("Load SysEx")
                        .withMessage("Send " + file.getFileName() + " to the SidPortal?\n\n"
                                     "An all-patch dump replaces every patch in memory.")
                        .withButton("Send")
                        .withButton("Cancel")
                        .withAssociatedComponent(self);
        juce::AlertWindow::showAsync(opts, [self, data](int result) {
            if (!self || result != 1) return;
            const auto* b = static_cast<const uint8_t*>(data.getData());
            self->proc_.send_midi(std::vector<uint8_t>(b, b + data.getSize()));
        });
    });
}

// --- Editor ---------------------------------------------------------------------------

SidPortalEditor::SidPortalEditor(SidPortalProcessor& p) : AudioProcessorEditor(p), view_(p)
{
    addAndMakeVisible(view_);
    const int w = view_.getWidth(), h = view_.getHeight();
    setResizable(true, true);
    setResizeLimits(w / 2, h / 2, w * 2, h * 2);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio(double(w) / double(h));
    setSize(w, h);
}

void SidPortalEditor::resized()
{
    const float scale = float(getWidth()) / float(view_.getWidth());
    view_.setTransform(juce::AffineTransform::scale(scale));
}
