// SidPortal as a VST3 instrument: the same Board the standalone runs, driven by
// the host's audio and MIDI. MIDI in goes to the SCI at its sample position, the
// firmware's MIDI out (SysEx dumps, the knobs' CCs) comes back to the host, and an
// optional "Ext In" input bus feeds the SID's external input.
//
// Threads: the audio thread owns the board. Boots (first load, state restore) run on
// a worker with a board of their own and swap it in when the firmware is up, so the
// audio thread never waits 12 s. The editor talks to the board through a small event
// queue and reads a panel snapshot written at the end of each block.
#pragma once
#include "board.h"
#include "noise_gate.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

struct PanelSnapshot {
    uint8_t chars[32] = {};
    uint8_t cgram[64] = {};
    bool on = false, cursor = false, blink = false;
    int row = 0, col = 0;
    uint8_t leds = 0xFF;   // port A, active low
    bool ready = false;
};

class SidPortalProcessor final : public juce::AudioProcessor {
public:
    SidPortalProcessor();
    ~SidPortalProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SidPortal"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Editor side (message thread).
    void key(int code, bool down);
    void encoder(int steps);
    void reset_firmware();
    void send_midi(const std::vector<uint8_t>& bytes);
    void set_engine(sidportal::SidEngineType type);
    sidportal::SidEngineType engine() const { return sidportal::SidEngineType(engine_.load()); }
    bool needs_firmware() const { return rom_path_.isEmpty(); }
    bool install_firmware(const juce::File& source, juce::String& error);
    PanelSnapshot snapshot();
    juce::String problem() const;   // why there is no sound (no firmware), or empty

    juce::AudioProcessorValueTreeState params;
    juce::RangedAudioParameter* knob_param(int i) { return knobs_[i]; }
    juce::RangedAudioParameter* volume_param() { return volume_; }
    juce::RangedAudioParameter* character_param() { return character_; }
    juce::RangedAudioParameter* chip_param() { return chip_; }
    juce::RangedAudioParameter* gate_param() { return gate_enabled_; }
    juce::RangedAudioParameter* gate_threshold_param() { return gate_threshold_; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    void start_boot(std::vector<uint8_t> ram, bool send_presets);
    void boot(int generation, std::vector<uint8_t> ram, bool send_presets);
    void apply_params(sidportal::Board& board, bool force);
    void render(sidportal::Board& board, juce::AudioBuffer<float>& buffer, int from, int to,
                const float* ext_l, const float* ext_r, juce::MidiBuffer& out);
    void parse_midi_out(uint8_t byte, int position, juce::MidiBuffer& out);

    std::mutex board_mutex_;                       // board_ and its swap
    std::unique_ptr<sidportal::Board> board_;
    std::atomic<double> rate_{48000.0};
    std::atomic<int> engine_{int(sidportal::SidEngineType::ReSidFp)};

    std::thread boot_thread_;
    std::atomic<int> generation_{0};
    std::vector<uint8_t> booting_ram_;             // RAM a running boot started from (state saves)
    std::mutex booting_mutex_;
    juce::String rom_path_, problem_;
    mutable std::mutex problem_mutex_;

    std::mutex events_mutex_;                       // editor -> audio
    std::vector<uint32_t> events_;                 // kind << 24 | value
    std::vector<uint8_t> midi_from_editor_;

    std::mutex snapshot_mutex_;                     // audio -> editor
    PanelSnapshot snapshot_;

    juce::RangedAudioParameter* knobs_[4] = {};
    juce::RangedAudioParameter* volume_ = nullptr;
    juce::RangedAudioParameter* character_ = nullptr;
    juce::RangedAudioParameter* chip_ = nullptr;
    juce::RangedAudioParameter* gate_enabled_ = nullptr;
    juce::RangedAudioParameter* gate_threshold_ = nullptr;
    sidportal::NoiseGate noise_gate_;
    int applied_knobs_[4] = {-1, -1, -1, -1};
    float applied_character_ = -9.0f;
    int applied_chip_ = -1;
    int64_t fade_ = 0;                              // samples since the board came up

    // Firmware MIDI out -> whole messages.
    std::vector<uint8_t> sysex_;
    bool in_sysex_ = false;
    uint8_t status_ = 0, data_[2] = {};
    int have_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SidPortalProcessor)
};
