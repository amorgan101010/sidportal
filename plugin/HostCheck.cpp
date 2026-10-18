// Load the built SidPortal.vst3 through JUCE's VST3 host code, as a DAW would: scan,
// instantiate, process with MIDI, and round-trip the state. Usage: HostCheck PATH.vst3
#include <juce_audio_processors/juce_audio_processors.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::puts("usage: HostCheck PATH.vst3"); return 2; }
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile(found, argv[1]);
    if (found.isEmpty()) { std::puts("FAIL no plugin found in the bundle"); return 1; }
    auto& desc = *found[0];
    std::printf("ok   found %s by %s (%s), %d in / %d out\n", desc.name.toRawUTF8(),
                desc.manufacturerName.toRawUTF8(), desc.category.toRawUTF8(), desc.numInputChannels,
                desc.numOutputChannels);
    juce::String err;
    auto plugin = format.createInstanceFromDescription(desc, 48000.0, 512, err);
    if (!plugin) { std::printf("FAIL instantiate: %s\n", err.toRawUTF8()); return 1; }
    plugin->enableAllBuses();
    plugin->setPlayConfigDetails(plugin->getTotalNumInputChannels(), 2, 48000.0, 512);
    plugin->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buf(std::max(2, plugin->getTotalNumInputChannels()), 512);
    double peak = 0;
    int midi_out = 0;
    // ~8 s at realtime pace (a fresh instance boots and loads the factory bank in about
    // 2 s), with a note every second after the first four.
    for (int block = 0; block < 94 * 8; ++block) {
        juce::MidiBuffer midi;
        if (block > 376 && block % 94 == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 57, uint8_t(110)), 0);
        if (block > 376 && block % 94 == 60) midi.addEvent(juce::MidiMessage::noteOff(1, 57), 0);
        buf.clear();
        plugin->processBlock(buf, midi);
        midi_out += midi.getNumEvents();
        peak = std::max(peak, double(buf.getMagnitude(0, 0, 512)));
        std::this_thread::sleep_for(std::chrono::microseconds(10600));
    }
    std::printf("%s processed 8 s: peak %.4f, %d MIDI events out\n", peak > 0.001 ? "ok  " : "FAIL", peak, midi_out);
    juce::MemoryBlock state;
    plugin->getStateInformation(state);
    auto second = format.createInstanceFromDescription(desc, 44100.0, 256, err);
    if (!second) { std::printf("FAIL second instance: %s\n", err.toRawUTF8()); return 1; }
    second->setStateInformation(state.getData(), int(state.getSize()));
    std::printf("%s state round trip: %d bytes\n", state.getSize() > 32768 ? "ok  " : "FAIL", int(state.getSize()));
    plugin->releaseResources();
    plugin.reset();
    second.reset();
    std::puts("ok   unloaded");
    return state.getSize() > 32768 && peak > 0.001 ? 0 : 1;
}
