// Plugin smoke test: boot the processor as a host would, play a note, save and restore
// the state into a second instance, and change the sample rate. Runs against a private,
// empty data directory, so it boots a cleared unit and sends it the factory bank.
#include "PluginProcessor.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

extern juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace {

int failures = 0;

void check(bool ok, const char* what, const juce::String& detail = {})
{
    std::printf("%s %s %s\n", ok ? "ok  " : "FAIL", what, detail.toRawUTF8());
    failures += !ok;
}

void set_environment_variable(const char* name, const char* value)
{
#if JUCE_WINDOWS
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct Host {
    std::unique_ptr<juce::AudioProcessor> plugin{createPluginFilter()};
    SidPortalProcessor& p = *dynamic_cast<SidPortalProcessor*>(plugin.get());
    double rate = 48000;
    int block = 512;
    int midi_out = 0;
    float noise = 0.0f;   // amplitude of white noise on the Ext In bus
    juce::Random rnd{1};

    void prepare(double r, int b)
    {
        rate = r;
        block = b;
        plugin->setPlayConfigDetails(0, 2, r, b);
        plugin->prepareToPlay(r, b);
    }

    // Process `ms` of audio; returns the RMS of the output. `events` go in the first block.
    double run(int ms, juce::MidiBuffer events = {})
    {
        juce::AudioBuffer<float> buf(2, block);
        double sum = 0;
        int64_t n = 0;
        for (int64_t done = 0; done < int64_t(ms * rate / 1000); done += block) {
            juce::MidiBuffer midi;
            midi.swapWith(events);
            buf.clear();
            if (noise > 0)
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block; ++i) buf.setSample(ch, i, noise * (rnd.nextFloat() * 2 - 1));
            plugin->processBlock(buf, midi);
            midi_out += midi.getNumEvents();
            for (int i = 0; i < block; ++i, ++n) sum += double(buf.getSample(0, i)) * buf.getSample(0, i);
        }
        return n ? std::sqrt(sum / double(n)) : 0.0;
    }

    bool wait_ready(int seconds)
    {
        for (int i = 0; i < seconds * 50; ++i) {
            run(20);
            if (p.snapshot().ready) return true;
            if (p.problem().isNotEmpty()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    juce::String lcd()
    {
        auto s = p.snapshot();
        juce::String out;
        for (int i = 0; i < 32; ++i) out += juce::String::charToString(juce::juce_wchar(s.chars[i] >= 32 ? s.chars[i] : '?'));
        return out;
    }

    void press(int code)
    {
        p.key(code, true);
        run(100);
        p.key(code, false);
        run(400);
    }

    double note(int ms)
    {
        juce::MidiBuffer on;
        on.addEvent(juce::MidiMessage::noteOn(1, 57, uint8_t(100)), 100);
        double rms = run(ms, on);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::noteOff(1, 57), 0);
        run(300, off);
        return rms;
    }

    void sysex(const std::vector<juce::uint8>& bytes)
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::createSysExMessage(bytes.data(), int(bytes.size())), 0);
        run(20, midi);
    }
};

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    auto tmp = juce::File::createTempFile("sidportal-smoke");
    tmp.createDirectory();
    auto isolated_data_dir = tmp.getChildFile("data");
    isolated_data_dir.createDirectory();
    set_environment_variable("SIDPORTAL_DATA_DIR", isolated_data_dir.getFullPathName().toRawUTF8());

    juce::MemoryBlock state;
    juce::String first_lcd;
    {
        Host h;
        h.prepare(48000, 512);
        auto t0 = std::chrono::steady_clock::now();
        bool up = h.wait_ready(60);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(up, "boots (fresh unit, factory bank sent)", juce::String(secs, 1) + " s; " + h.p.problem());
        if (!up) return 1;
        h.run(500);
        check(h.lcd().contains("Patch"), "top menu on the LCD", h.lcd());
        h.press(15);   // ENTER: patch play
        h.press(15);
        first_lcd = h.lcd();
        double rms = h.note(1000);
        check(rms > 0.003, "a MIDI note sounds", "rms " + juce::String(rms, 4) + " lcd " + first_lcd);
        h.plugin->getStateInformation(state);
        check(state.getSize() > 32768, "state carries the RAM", juce::String(int(state.getSize())) + " bytes");

        // Engine and chip switches while running.
        h.p.set_engine(sidportal::SidEngineType::ReSid);
        h.p.chip_param()->setValueNotifyingHost(1.0f);
        h.run(800);   // past the switch's fade
        double rms2 = h.note(1000);
        check(rms2 > 0.003, "still sounds on reSID / 8580", "rms " + juce::String(rms2, 4));
        h.p.chip_param()->setValueNotifyingHost(0.0f);
        h.p.set_engine(sidportal::SidEngineType::ReSidFp);

        // Host SysEx reaches the same firmware SCI path as realtime ALSA MIDI.
        h.press(3); // Exit patch play to the top menu.
        if (!h.lcd().contains("System")) h.press(3);
        check(h.lcd().contains("System"), "returns to the top menu before ASID", h.lcd());
        h.press(11); // DOWN to System
        h.press(11); // DOWN to ASID
        h.press(15); // ENTER
        const std::vector<juce::uint8> update = {
            0x2D, 0x4E, 0x33, 0, 0, 0x03, 0x21, 0, 0, 0,
            0x56, 0x1C, 0x09, 0x70, 0x0F, 0x21};
        h.sysex(update);
        h.sysex(update);
        std::vector<juce::uint8> text{0x2D, 0x4F};
        const char* label = "ASID FRAME TEST";
        for (int i = 0; i < 32; ++i)
            text.push_back(i < 15 ? uint8_t(label[i]) : uint8_t(' '));
        h.sysex(text);
        for (int i = 0; i < 16; ++i) h.sysex(update);
        double asidRms = h.run(500);
        check(h.lcd().contains("ASID FRAME TEST"), "host ASID SysEx reaches the firmware", h.lcd());
        check(asidRms > 0.001, "host ASID SysEx produces SID audio", "rms " + juce::String(asidRms, 4));
    }
    {
        Host h;
        h.plugin->setStateInformation(state.getData(), int(state.getSize()));
        h.prepare(44100, 256);
        bool up = h.wait_ready(60);
        check(up, "restored instance boots");
        h.run(300);
        h.press(15);   // the firmware always boots to its menu; patch play shows the patch again
        h.press(15);
        check(h.lcd() == first_lcd, "restored memory comes back with the same patch", h.lcd());
        // Ext In: enable the input bus, open the cutoff (CC 27), and feed it noise.
        auto layout = h.plugin->getBusesLayout();
        layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
        check(h.plugin->setBusesLayout(layout), "Ext In bus can be enabled");
        h.plugin->setPlayConfigDetails(2, 2, h.rate, h.block);
        juce::MidiBuffer cc;
        cc.addEvent(juce::MidiMessage::controllerEvent(1, 27, 127), 0);
        double quiet = h.run(1000, cc);
        h.noise = 0.5f;
        h.run(300);
        double fed = h.run(1000);
        h.noise = 0.0f;
        check(fed > 2 * quiet && fed > 0.002, "Ext In noise comes out through the filter",
              "rms " + juce::String(quiet, 4) + " -> " + juce::String(fed, 4));
        h.prepare(96000, 1024);
        double rms = h.note(800);
        check(rms > 0.003, "sounds after a sample-rate change to 96 kHz", "rms " + juce::String(rms, 4));
    }
    tmp.deleteRecursively();
    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
