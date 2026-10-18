// The plugin's front panel: the standalone's artwork (exported by
// tools/export_plugin_assets.py) with the LCD, LEDs, knobs, encoder, keypad, chip switch
// and gear menu drawn live. The knobs, volume, filter character and chip are host
// parameters, so the DAW can automate them and map controllers to them.
#pragma once
#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

class PanelView final : public juce::Component, private juce::Timer {
public:
    explicit PanelView(SidPortalProcessor& p);
    ~PanelView() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

private:
    struct KnobSpot {
        juce::RangedAudioParameter* param;
        juce::Point<float> centre;
        float r;
        const juce::Image* body;
    };
    void timerCallback() override;
    int knob_at(juce::Point<float> p) const;
    int key_at(juce::Point<float> p) const;
    bool character_visible() const;
    void paint_lcd(juce::Graphics& g);
    void paint_knob(juce::Graphics& g, const KnobSpot& k);
    void open_menu();
    void load_sysex();

    SidPortalProcessor& proc_;
    juce::var layout_;
    juce::Image panel_, knob_, small_knob_, encoder_, slider_, gear_up_, gear_down_, character_;
    juce::Image led_off_, led_on_, dimple_, subtitle_6581_, subtitle_8580_;
    juce::Image keys_[16][2];
    std::vector<KnobSpot> knobs_;   // 0-3 realtime, 4 volume, 5 filter character
    PanelSnapshot snap_;
    bool blink_phase_ = false;
    int blink_ticks_ = 0;

    // Mouse state.
    int drag_knob_ = -1;
    float drag_start_y_ = 0, drag_start_v_ = 0;
    bool drag_encoder_ = false;
    float enc_last_ = 0, enc_acc_ = 0, enc_angle_ = -90;
    int held_key_ = -1;
    bool gear_down_now_ = false;
    std::unique_ptr<juce::FileChooser> chooser_;
};

class SidPortalEditor final : public juce::AudioProcessorEditor {
public:
    explicit SidPortalEditor(SidPortalProcessor& p);
    void resized() override;

private:
    PanelView view_;
};
