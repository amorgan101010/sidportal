#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

using sidportal::Board;
using sidportal::SidEngineType;

namespace {

enum EventKind : uint32_t { KeyDown = 1, KeyUp = 2, Encoder = 3 };

juce::File data_dir()
{
    const char* xdg = std::getenv("XDG_DATA_HOME");
    juce::File base = (xdg && *xdg) ? juce::File(xdg)
                                    : juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                          .getChildFile(".local/share");
    return base.getChildFile("sidportal");
}

// The firmware is never compiled in: the plugin looks where the standalone keeps it.
juce::String find_rom()
{
    juce::StringArray candidates;
    if (const char* env = std::getenv("SIDPORTAL_ROM")) candidates.add(env);
    candidates.add(data_dir().getChildFile("firmware.bin").getFullPathName());
    candidates.add(juce::String(SIDPORTAL_SOURCE_DIR) + "/roms/firmware.bin");
    for (auto& c : candidates)
        if (c.isNotEmpty() && juce::File(c).existsAsFile()) return c;
    return {};
}

std::vector<uint8_t> read_file(const juce::File& f)
{
    std::ifstream in(f.getFullPathName().toStdString(), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The factory bank, if the user has put one in patches/ (any .syx with "Presets" in its name).
juce::File presets_file()
{
    auto found = juce::File(juce::String(SIDPORTAL_SOURCE_DIR) + "/patches")
                     .findChildFiles(juce::File::findFiles, false, "*Presets*.syx");
    found.sort();
    return found.isEmpty() ? juce::File() : found.getFirst();
}

} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout SidPortalProcessor::layout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    for (int i = 1; i <= 4; ++i)
        l.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{"knob" + juce::String(i), 1},
                                                         "Knob " + juce::String(i), 0, 255, 128));
    juce::NormalisableRange<float> vol(0.0f, 3.0f);
    vol.setSkewForCentre(1.0f);
    l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"volume", 1}, "Volume", vol, 1.0f));
    l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"character", 1}, "Filter character",
                                                       juce::NormalisableRange<float>(-0.5f, 0.5f), 0.0f));
    l.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"chip8580", 1}, "SID 8580", false));
    return l;
}

SidPortalProcessor::SidPortalProcessor()
    : AudioProcessor(BusesProperties()
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Ext In", juce::AudioChannelSet::stereo(), false)),
      params(*this, nullptr, "SidPortal", layout())
{
    for (int i = 0; i < 4; ++i) knobs_[i] = params.getParameter("knob" + juce::String(i + 1));
    volume_ = params.getParameter("volume");
    character_ = params.getParameter("character");
    chip_ = params.getParameter("chip8580");
    rom_path_ = find_rom();
    // A fresh instance starts from the standalone's memory if there is one, else from a
    // cleared unit that gets the factory bank once it has booted.
    auto standalone = data_dir().getChildFile("nvram.bin");
    std::vector<uint8_t> ram;
    if (standalone.existsAsFile()) ram = read_file(standalone);
    start_boot(std::move(ram), !standalone.existsAsFile());
}

SidPortalProcessor::~SidPortalProcessor()
{
    ++generation_;
    if (boot_thread_.joinable()) boot_thread_.join();
}

// --- Boot -------------------------------------------------------------------------

void SidPortalProcessor::start_boot(std::vector<uint8_t> ram, bool send_presets)
{
    int gen = ++generation_;
    if (boot_thread_.joinable()) boot_thread_.join();   // it sees the new generation and stops
    {
        std::lock_guard<std::mutex> g(booting_mutex_);
        booting_ram_ = ram;
    }
    boot_thread_ = std::thread([this, gen, ram = std::move(ram), send_presets]() mutable {
        boot(gen, std::move(ram), send_presets);
    });
}

void SidPortalProcessor::boot(int gen, std::vector<uint8_t> ram, bool send_presets)
{
    if (rom_path_.isEmpty()) {
        std::lock_guard<std::mutex> g(problem_mutex_);
        problem_ = "Firmware not found: run tools/roms.py";
        return;
    }
    const double rate = rate_.load();
    auto b = std::make_unique<Board>(rate, 1000000.0, chip_->getValue() > 0.5f, engine());
    std::string err;
    if (!b->load_rom(rom_path_.toStdString(), &err)) {
        std::lock_guard<std::mutex> g(problem_mutex_);
        problem_ = juce::String(err);
        return;
    }
    if (ram.empty()) b->clear_nvram();
    else b->load_ram(ram.data(), ram.size());
    // Knobs where the parameters are, before the firmware first reads them.
    int knobs[4];
    for (int i = 0; i < 4; ++i) {
        knobs[i] = int(knobs_[i]->convertFrom0to1(knobs_[i]->getValue()));
        b->set_knob(i, uint8_t(knobs[i]));
    }
    b->set_filter_bias(character_->convertFrom0to1(character_->getValue()));
    b->reset();

    std::vector<int16_t> scratch(4096);
    const int chunk = std::min(4096, int(rate / 50));
    auto run = [&](double seconds) {
        for (double done = 0; done < seconds * rate; done += chunk) {
            if (generation_.load() != gen) return false;
            b->render(scratch.data(), chunk);
        }
        return true;
    };
    if (!run(12.5)) return;   // the boot screen
    if (send_presets) {
        auto syx = read_file(presets_file());
        for (uint8_t byte : syx) b->midi_in(byte);
        while (b->midi_in_pending() > 0)
            if (!run(0.1)) return;
        if (!run(0.5)) return;
    }
    b->take_midi_out();
    // The engine may have been changed from the gear menu while this was booting.
    if (b->sid_engine() != engine()) b->set_sid_engine(b->build_sid_engine(engine()));

    std::lock_guard<std::mutex> g(board_mutex_);
    if (generation_.load() != gen) return;
    b->set_sample_rate(rate_.load());
    board_.swap(b);
    for (int i = 0; i < 4; ++i) applied_knobs_[i] = knobs[i];
    applied_character_ = float(board_->filter_bias());
    applied_chip_ = chip_->getValue() > 0.5f;
    fade_ = 0;
    // b (the old board, if any) goes when this returns, outside the audio thread.
}

// --- Audio ------------------------------------------------------------------------

void SidPortalProcessor::prepareToPlay(double sampleRate, int)
{
    rate_ = sampleRate;
    std::lock_guard<std::mutex> g(board_mutex_);
    if (board_) board_->set_sample_rate(sampleRate);
}

bool SidPortalProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

void SidPortalProcessor::apply_params(Board& board, bool force)
{
    for (int i = 0; i < 4; ++i) {
        int v = int(knobs_[i]->convertFrom0to1(knobs_[i]->getValue()));
        if (force || v != applied_knobs_[i]) { board.set_knob(i, uint8_t(v)); applied_knobs_[i] = v; }
    }
    float c = character_->convertFrom0to1(character_->getValue());
    if (force || std::abs(c - applied_character_) > 1e-4f) { board.set_filter_bias(c); applied_character_ = c; }
    int chip = chip_->getValue() > 0.5f;
    if (force || chip != applied_chip_) { board.set_sid_model(chip == 1); applied_chip_ = chip; }
}

void SidPortalProcessor::parse_midi_out(uint8_t byte, int position, juce::MidiBuffer& out)
{
    if (byte >= 0xF8) { out.addEvent(juce::MidiMessage(byte), position); return; }   // realtime
    if (byte == 0xF0) { in_sysex_ = true; sysex_.assign(1, byte); return; }
    if (byte == 0xF7) {
        if (in_sysex_) {
            sysex_.push_back(byte);
            out.addEvent(juce::MidiMessage(sysex_.data(), int(sysex_.size())), position);
        }
        in_sysex_ = false;
        return;
    }
    if (byte & 0x80) {
        in_sysex_ = false;
        status_ = byte;
        have_ = 0;
        if (byte == 0xF6) { out.addEvent(juce::MidiMessage(byte), position); status_ = 0; }
        return;
    }
    if (in_sysex_) { sysex_.push_back(byte); return; }
    if (!status_) return;
    data_[have_++] = byte;
    const uint8_t hi = status_ & 0xF0;
    int need = (hi == 0xC0 || hi == 0xD0 || status_ == 0xF1 || status_ == 0xF3) ? 1 : 2;
    if (have_ < need) return;
    out.addEvent(need == 1 ? juce::MidiMessage(status_, data_[0]) : juce::MidiMessage(status_, data_[0], data_[1]),
                 position);
    have_ = 0;
    if (status_ >= 0xF0) status_ = 0;   // system common: no running status
}

void SidPortalProcessor::render(Board& board, juce::AudioBuffer<float>& buffer, int from, int to,
                                 const float* ext_l, const float* ext_r, juce::MidiBuffer& out)
{
    int16_t pcm[512], ext[512];
    const float vol = volume_->convertFrom0to1(volume_->getValue());
    const float fade_len = float(rate_.load() * 0.5);
    for (int pos = from; pos < to;) {
        const int n = std::min(512, to - pos);
        if (ext_l) {
            for (int k = 0; k < n; ++k) {
                float v = ext_r ? 0.5f * (ext_l[pos + k] + ext_r[pos + k]) : ext_l[pos + k];
                ext[k] = int16_t(std::clamp(v * 32767.0f, -32768.0f, 32767.0f));
            }
            board.ext_in(ext, size_t(n));
        }
        board.render(pcm, n);
        for (uint8_t byte : board.take_midi_out()) parse_midi_out(byte, pos, out);
        for (int k = 0; k < n; ++k) {
            // Fade in as the board comes up; 6 dB of headroom as in the standalone.
            float fade = std::min(1.0f, float(fade_++) / fade_len);
            float s = float(pcm[k]) * (vol * 0.5f / 32768.0f) * fade;
            for (int ch = 0; ch < getTotalNumOutputChannels(); ++ch) buffer.setSample(ch, pos + k, s);
        }
        pos += n;
    }
}

void SidPortalProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int frames = buffer.getNumSamples();

    // Ext In: the input bus, when the host has it enabled. Rendering reads each piece of
    // input before writing that piece of output, so in-place buffers are fine.
    const float* ext_l = nullptr;
    const float* ext_r = nullptr;
    if (auto* bus = getBus(true, 0); bus && bus->isEnabled() && getTotalNumInputChannels() > 0) {
        ext_l = buffer.getReadPointer(0);
        if (getTotalNumInputChannels() > 1) ext_r = buffer.getReadPointer(1);
    }

    std::unique_lock<std::mutex> lock(board_mutex_);
    if (!board_) {
        lock.unlock();
        buffer.clear();
        midi.clear();
        if (std::unique_lock<std::mutex> s(snapshot_mutex_, std::try_to_lock); s.owns_lock())
            snapshot_.ready = false;
        return;
    }
    Board& board = *board_;

    if (std::unique_lock<std::mutex> e(events_mutex_, std::try_to_lock); e.owns_lock()) {
        for (uint32_t ev : events_) {
            const uint32_t kind = ev >> 24;
            const int value = int(int32_t(ev << 8) >> 8);   // sign-extend the low 24 bits
            if (kind == KeyDown) board.key_down(value);
            else if (kind == KeyUp) board.key_up(value);
            else if (kind == Encoder) board.encoder(value);
        }
        events_.clear();
        for (uint8_t byte : midi_from_editor_) board.midi_in(byte);
        midi_from_editor_.clear();
    }
    apply_params(board, false);

    juce::MidiBuffer out;
    int pos = 0;
    for (const auto meta : midi) {
        const int at = std::clamp(meta.samplePosition, 0, frames);
        render(board, buffer, pos, at, ext_l, ext_r, out);
        pos = at;
        const uint8_t* raw = meta.data;
        for (int i = 0; i < meta.numBytes; ++i) board.midi_in(raw[i]);
    }
    render(board, buffer, pos, frames, ext_l, ext_r, out);
    for (int ch = getTotalNumOutputChannels(); ch < buffer.getNumChannels(); ++ch) buffer.clear(ch, 0, frames);
    midi.swapWith(out);

    if (std::unique_lock<std::mutex> s(snapshot_mutex_, std::try_to_lock); s.owns_lock()) {
        const auto& lcd = board.lcd();
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 16; ++c) snapshot_.chars[r * 16 + c] = lcd.visible(r, c);
        std::copy(lcd.cgram(), lcd.cgram() + 64, snapshot_.cgram);
        snapshot_.on = lcd.display_on();
        snapshot_.cursor = lcd.cursor_on();
        snapshot_.blink = lcd.blink_on();
        snapshot_.row = lcd.cursor_row();
        snapshot_.col = lcd.cursor_col();
        snapshot_.leds = board.leds();
        snapshot_.ready = true;
    }
}

// --- Editor side --------------------------------------------------------------------

void SidPortalProcessor::key(int code, bool down)
{
    std::lock_guard<std::mutex> g(events_mutex_);
    if (events_.size() < 1024) events_.push_back((down ? KeyDown : KeyUp) << 24 | uint32_t(code & 0x0F));
}

void SidPortalProcessor::encoder(int steps)
{
    std::lock_guard<std::mutex> g(events_mutex_);
    if (events_.size() < 1024) events_.push_back(Encoder << 24 | (uint32_t(steps) & 0xFFFFFF));
}

void SidPortalProcessor::send_midi(const std::vector<uint8_t>& bytes)
{
    std::lock_guard<std::mutex> g(events_mutex_);
    midi_from_editor_.insert(midi_from_editor_.end(), bytes.begin(), bytes.end());
}

void SidPortalProcessor::set_engine(SidEngineType type)
{
    if (int(type) == engine_.exchange(int(type))) return;
    // Build (and fill the tables of) the new engine here, not on the audio thread.
    auto e = sidportal::make_sid_engine(type);
    e->configure(false, 1000000.0, rate_.load());
    std::lock_guard<std::mutex> g(board_mutex_);
    if (board_) {
        if (board_->sample_rate() != rate_.load()) e->configure(false, 1000000.0, board_->sample_rate());
        board_->set_sid_engine(std::move(e));
    }
}

PanelSnapshot SidPortalProcessor::snapshot()
{
    std::lock_guard<std::mutex> g(snapshot_mutex_);
    return snapshot_;
}

juce::String SidPortalProcessor::problem() const
{
    std::lock_guard<std::mutex> g(problem_mutex_);
    return problem_;
}

juce::AudioProcessorEditor* SidPortalProcessor::createEditor() { return new SidPortalEditor(*this); }

// --- State ----------------------------------------------------------------------------
// The parameters, the engine and the battery-backed RAM (patches and settings).

void SidPortalProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = params.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    xml->setAttribute("engine", sidportal::sid_engine_name(engine()));
    juce::MemoryBlock ram;
    {
        std::lock_guard<std::mutex> g(board_mutex_);
        if (board_) ram.append(board_->ram(), 0x8000);
    }
    if (ram.isEmpty()) {
        std::lock_guard<std::mutex> g(booting_mutex_);
        if (!booting_ram_.empty()) ram.append(booting_ram_.data(), booting_ram_.size());
    }
    if (!ram.isEmpty()) xml->setAttribute("ram", ram.toBase64Encoding());
    copyXmlToBinary(*xml, destData);
}

void SidPortalProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (!xml || !xml->hasTagName(params.state.getType())) return;
    SidEngineType type;
    if (sidportal::parse_sid_engine(xml->getStringAttribute("engine", "residfp").toStdString(), &type))
        engine_ = int(type);
    juce::MemoryBlock ram;
    const bool have_ram = xml->hasAttribute("ram") && ram.fromBase64Encoding(xml->getStringAttribute("ram"));
    xml->removeAttribute("engine");
    xml->removeAttribute("ram");
    params.replaceState(juce::ValueTree::fromXml(*xml));
    if (!have_ram) return;   // keep whatever this instance booted with
    const auto* bytes = static_cast<const uint8_t*>(ram.getData());
    // A restored project boots its own memory: about 12 s of silence while it does.
    {
        std::lock_guard<std::mutex> g(board_mutex_);
        board_.reset();
    }
    start_boot(std::vector<uint8_t>(bytes, bytes + ram.getSize()), false);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SidPortalProcessor(); }
