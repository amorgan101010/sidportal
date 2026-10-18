// SidPortal host: headless scripting, WAV rendering, and the realtime
// ALSA engine driven by the Tk panel over stdin/stdout.
#include "board.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace sidportal;

namespace {

struct Options {
    std::string rom;
    std::string nvram;
    std::string script;
    std::string render;
    std::string audio_device = "default";
    std::string midi = "seq";
    double sample_rate = 48000;
    double sid_clock = 1000000;
    bool mos8580 = false;
    SidEngineType engine = SidEngineType::ReSidFp;
    bool realtime = false;
    bool fresh = false;
    bool log_io = false;
    bool trace_sid = false;
    float volume = 1.0f;
    int period = 256;
    std::string ext_in;   // ALSA capture device for Ext In (empty: none)
    float ext_gain = 1.0f;
    double filter_bias = 0.0;
    int bias_cc = -1;
};

void usage()
{
    std::puts(
        "usage: sidportal --rom FILE [options]\n"
        "  --nvram FILE        battery-backed RAM image (loaded at start, saved on exit)\n"
        "  --fresh             start with cleared RAM (factory reset on boot)\n"
        "  --script CMDS       run headless: ';'-separated commands (see below)\n"
        "  --render FILE.wav   with --script: write the audio to a WAV file\n"
        "  --realtime          run the ALSA engine; commands are read from stdin\n"
        "  --audio DEV         ALSA PCM device (default: default)\n"
        "  --period FRAMES     ALSA period size (default 256)\n"
        "  --midi seq|none     create an ALSA sequencer port (default seq)\n"
        "  --rate HZ           sample rate (default 48000)\n"
        "  --sid-clock HZ      SID clock (default 1000000)\n"
        "  --8580              emulate the MOS 8580 instead of the 6581\n"
        "  --engine NAME       SID emulation: residfp (default) or resid\n"
        "  --ext-in DEV        realtime: ALSA capture device feeding Ext In (through the filter)\n"
        "  --ext-gain F        Ext In gain (default 1.0)\n"
        "  --filter-bias V     filter character, -0.5 darker .. +0.5 brighter (reSID: 6581 only)\n"
        "  --bias-cc N         MIDI CC (any channel) controlling the filter character\n"
        "  --log-io            log accesses to unmapped I/O\n"
        "  --trace-sid         print SID register writes\n"
        "  --volume F          realtime output gain (default 1.0)\n"
        "commands: wait MS | lcd | press KEY | down KEY | up KEY | enc N | knob I V |\n"
        "          midi HEX.. | note N VEL MS | syx FILE | extin FILE.wav | extgain F |\n"
        "          filterbias V | model 6581|8580 | engine resid|residfp |\n"
        "          leds | reset | save");
}

int key_code(const std::string& k)
{
    static const std::pair<const char*, char> names[] = {
        {"exit", 'A'}, {"up", 'B'}, {"down", 'C'}, {"enter", 'D'}, {"left", '*'}, {"right", '#'}};
    char c = k.empty() ? 0 : char(std::toupper(static_cast<unsigned char>(k[0])));
    for (auto& [name, ch] : names)
        if (strcasecmp(k.c_str(), name) == 0) c = ch;
    const char* p = std::strchr(kKeyLegends, c);
    return (p && c) ? int(p - kKeyLegends) : -1;
}

std::string lcd_text(const Board& board)
{
    std::string s;
    for (int r = 0; r < 2; ++r) {
        s += '|';
        for (int c = 0; c < 16; ++c) {
            uint8_t ch = board.lcd().visible(r, c);
            s += (ch >= 0x20 && ch < 0x7F) ? char(ch) : (ch < 8 ? char('0' + ch) : '~');
        }
        s += "|\n";
    }
    return s;
}

std::vector<uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void write_wav(const std::string& path, const std::vector<int16_t>& pcm, int rate)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    uint32_t bytes = uint32_t(pcm.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(uint32_t(rate)); u32(uint32_t(rate * 2)); u16(2); u16(16);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), long(bytes));
}

// 16-bit PCM WAV -> mono samples at `rate` (linear resampling if the file differs).
std::vector<int16_t> read_wav_mono(const std::string& path, double rate, std::string* error)
{
    auto data = read_file(path);
    auto u16 = [&](size_t o) { return uint16_t(data[o] | data[o + 1] << 8); };
    auto u32 = [&](size_t o) { return uint32_t(u16(o) | uint32_t(u16(o + 2)) << 16); };
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) || std::memcmp(data.data() + 8, "WAVE", 4)) {
        *error = "not a WAV file";
        return {};
    }
    int channels = 0, bits = 0;
    uint32_t file_rate = 0;
    for (size_t o = 12; o + 8 <= data.size();) {
        uint32_t len = u32(o + 4);
        if (!std::memcmp(data.data() + o, "fmt ", 4)) {
            channels = u16(o + 10); file_rate = u32(o + 12); bits = u16(o + 22);
        } else if (!std::memcmp(data.data() + o, "data", 4)) {
            if (bits != 16 || channels < 1) { *error = "only 16-bit PCM WAV is supported"; return {}; }
            size_t frames = std::min<size_t>(len, data.size() - o - 8) / size_t(2 * channels);
            std::vector<float> mono(frames);
            for (size_t f = 0; f < frames; ++f) {
                int sum = 0;
                for (int c = 0; c < channels; ++c) sum += int16_t(u16(o + 8 + (f * size_t(channels) + size_t(c)) * 2));
                mono[f] = float(sum) / float(channels);
            }
            double step = double(file_rate) / rate;
            std::vector<int16_t> out;
            for (double p = 0; p + 1 < double(frames); p += step) {
                size_t k = size_t(p);
                double fr = p - double(k);
                out.push_back(int16_t(mono[k] + (mono[k + 1] - mono[k]) * fr));
            }
            return out;
        }
        o += 8 + len + (len & 1);
    }
    *error = "no data chunk";
    return {};
}

// --- Headless -----------------------------------------------------------------

struct Headless {
    Board& board;
    const Options& opt;
    std::vector<int16_t> pcm;

    void advance_ms(double ms)
    {
        if (opt.render.empty()) {
            board.run_cycles(uint64_t(ms * kEClockHz / 1000.0));
            return;
        }
        int frames = int(ms * opt.sample_rate / 1000.0);
        size_t at = pcm.size();
        pcm.resize(at + size_t(frames));
        board.render(pcm.data() + at, frames);
    }

    bool command(const std::string& line)
    {
        std::istringstream in(line);
        std::string cmd;
        if (!(in >> cmd)) return true;
        if (cmd == "wait") { double ms = 0; in >> ms; advance_ms(ms); }
        else if (cmd == "lcd") std::fputs(lcd_text(board).c_str(), stdout);
        else if (cmd == "leds") std::printf("leds %02X\n", board.leds());
        else if (cmd == "lcdraw") {
            for (int r = 0; r < 2; ++r) {
                std::printf("[");
                for (int c = 0; c < 40; ++c) {
                    uint8_t ch = board.lcd().visible(r, c);
                    std::putchar((ch >= 0x20 && ch < 0x7F) ? ch : '~');
                }
                std::printf("]\n");
            }
            std::printf("cursor %d,%d on=%d blink=%d\n", board.lcd().cursor_row(), board.lcd().cursor_col(),
                        board.lcd().cursor_on(), board.lcd().blink_on());
        }
        else if (cmd == "press" || cmd == "down" || cmd == "up") {
            std::string k; in >> k;
            int code = key_code(k);
            if (code < 0) { std::fprintf(stderr, "unknown key %s\n", k.c_str()); return false; }
            if (cmd != "up") board.key_down(code);
            if (cmd == "press") { advance_ms(60); board.key_up(code); advance_ms(80); }
            if (cmd == "up") board.key_up(code);
        }
        else if (cmd == "enc") { int n = 0; in >> n; board.encoder(n); advance_ms(10.0 * std::abs(n) + 60); }
        else if (cmd == "knob") { int i = 0, v = 0; in >> i >> v; board.set_knob(i - 1, uint8_t(v)); }
        else if (cmd == "midi") {
            std::string h;
            while (in >> h) board.midi_in(uint8_t(std::stoul(h, nullptr, 16)));
        }
        else if (cmd == "note") {
            int n = 60, vel = 100; double ms = 500; in >> n >> vel >> ms;
            board.midi_in(0x90); board.midi_in(uint8_t(n)); board.midi_in(uint8_t(vel));
            advance_ms(ms);
            board.midi_in(0x80); board.midi_in(uint8_t(n)); board.midi_in(0);
        }
        else if (cmd == "extin") {
            std::string path, err; in >> path;
            auto pcm = read_wav_mono(path, opt.sample_rate, &err);
            if (pcm.empty()) { std::fprintf(stderr, "extin %s: %s\n", path.c_str(), err.c_str()); return false; }
            board.ext_in_clear();
            board.ext_in(pcm.data(), pcm.size(), false);
        }
        else if (cmd == "extgain") { float g = 1; in >> g; board.set_ext_gain(g); }
        else if (cmd == "filterbias") { double v = 0; in >> v; board.set_filter_bias(v); }
        else if (cmd == "model") { std::string m; in >> m; board.set_sid_model(m == "8580"); }
        else if (cmd == "engine") {
            std::string m; in >> m;
            SidEngineType t;
            if (parse_sid_engine(m, &t)) board.set_sid_engine(board.build_sid_engine(t));
        }
        else if (cmd == "syx") {
            std::string path; in >> path;
            for (uint8_t b : read_file(path)) board.midi_in(b);
            while (board.midi_in_pending()) advance_ms(20);
        }
        else if (cmd == "reset") board.reset();
        else if (cmd == "save") { if (!opt.nvram.empty()) board.save_nvram(opt.nvram); }
        else if (cmd == "midiout") {
            for (uint8_t b : board.take_midi_out()) std::printf("%02X ", b);
            std::puts("");
        }
        else { std::fprintf(stderr, "unknown command %s\n", cmd.c_str()); return false; }
        return true;
    }
};

int run_headless(Board& board, const Options& opt)
{
    Headless h{board, opt, {}};
    std::string script = opt.script;
    size_t pos = 0;
    while (pos <= script.size()) {
        size_t semi = script.find(';', pos);
        if (semi == std::string::npos) semi = script.size();
        if (!h.command(script.substr(pos, semi - pos))) return 1;
        pos = semi + 1;
    }
    if (!opt.render.empty()) write_wav(opt.render, h.pcm, int(opt.sample_rate));
    if (board.crashed()) std::printf("crashed pc %04X\n", board.crash_pc());
    if (board.cpu().rx_dropped || board.cpu().rx_overruns)
        std::printf("midi in: %u bytes dropped (receiver off), %u overruns\n", board.cpu().rx_dropped, board.cpu().rx_overruns);
    std::fflush(stdout);
    return 0;
}

} // namespace

int run_realtime(Board& board, const Options& opt);

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--rom") opt.rom = next();
        else if (a == "--nvram") opt.nvram = next();
        else if (a == "--script") opt.script = next();
        else if (a == "--render") opt.render = next();
        else if (a == "--audio") opt.audio_device = next();
        else if (a == "--period") opt.period = std::stoi(next());
        else if (a == "--midi") opt.midi = next();
        else if (a == "--rate") opt.sample_rate = std::stod(next());
        else if (a == "--sid-clock") opt.sid_clock = std::stod(next());
        else if (a == "--8580") opt.mos8580 = true;
        else if (a == "--engine") {
            if (!parse_sid_engine(next(), &opt.engine)) { std::fprintf(stderr, "unknown engine\n"); return 2; }
        }
        else if (a == "--ext-in") opt.ext_in = next();
        else if (a == "--ext-gain") opt.ext_gain = std::stof(next());
        else if (a == "--filter-bias") opt.filter_bias = std::stod(next());
        else if (a == "--bias-cc") opt.bias_cc = std::stoi(next());
        else if (a == "--realtime") opt.realtime = true;
        else if (a == "--fresh") opt.fresh = true;
        else if (a == "--log-io") opt.log_io = true;
        else if (a == "--trace-sid") opt.trace_sid = true;
        else if (a == "--volume") opt.volume = std::stof(next());
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    if (opt.rom.empty()) { usage(); return 2; }

    Board board(opt.sample_rate, opt.sid_clock, opt.mos8580, opt.engine);
    std::string err;
    if (!board.load_rom(opt.rom, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (!opt.fresh && !opt.nvram.empty()) board.load_nvram(opt.nvram);
    board.log_io = opt.log_io;
    board.set_filter_bias(opt.filter_bias);
    if (opt.trace_sid) {
        board.sid_write_hook = [](void*, uint64_t cyc, int reg, uint8_t v) {
            std::printf("sid %10llu %02X=%02X\n", static_cast<unsigned long long>(cyc), reg, v);
        };
    }
    board.reset();

    int rc = opt.realtime ? run_realtime(board, opt) : run_headless(board, opt);
    if (!opt.nvram.empty() && rc == 0) board.save_nvram(opt.nvram);
    return rc;
}

// --- Realtime -------------------------------------------------------------------

// Protocol on stdin, one command per line:
//   down K | up K | enc N | knob I V | midi HEX.. | syx PATH | volume F | model 6581|8580 |
//   engine resid|residfp |
//   save | reset | quit
// and on stdout:
//   ready | lcd <32 char codes hex> <64 CGRAM bytes hex> <on> <cursor> <blink> <row> <col> |
//   leds <hex> | syx <bytes left> | error <text>

namespace {

std::atomic<bool> g_quit{false};

void on_signal(int) { g_quit = true; }

struct Realtime {
    Board& board;
    const Options& opt;
    std::mutex lock;           // guards board
    std::mutex out_lock;       // guards stdout
    std::mutex midi_out_lock;
    std::vector<uint8_t> midi_out;
    float volume = 1.0f;
    snd_seq_t* seq = nullptr;
    int in_port = -1, out_port = -1;
    std::thread capture;
    std::atomic<bool> capture_stop{false};
    std::atomic<int> bias_cc{-1};          // CC number controlling the 6581 filter character
    std::atomic<bool> learning{false};     // next incoming CC becomes bias_cc

    // Controllers handled by the host. Returns true if the event was consumed.
    bool host_cc(int cc, int value)
    {
        if (learning.exchange(false)) {
            bias_cc = cc;
            emit("biascc " + std::to_string(cc));
            return true;
        }
        if (cc != bias_cc) return false;
        double v = -0.5 + std::clamp(value, 0, 127) / 127.0;
        {
            std::lock_guard<std::mutex> g(lock);
            board.set_filter_bias(v);
        }
        char buf[48];
        std::snprintf(buf, sizeof buf, "filterbias %.3f", v);
        emit(buf);
        return true;
    }

    void emit(const std::string& line)
    {
        std::lock_guard<std::mutex> g(out_lock);
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    // --- Audio ---------------------------------------------------------------
    void audio_thread()
    {
        snd_pcm_t* pcm = nullptr;
        int err = snd_pcm_open(&pcm, opt.audio_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) { emit("error audio open: " + std::string(snd_strerror(err))); g_quit = true; return; }
        unsigned latency_us = unsigned(1e6 * opt.period * 3 / opt.sample_rate);
        err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                                 unsigned(opt.sample_rate), 1, latency_us);
        if (err < 0) { emit("error audio params: " + std::string(snd_strerror(err))); g_quit = true; return; }
        sched_param sp{};
        sp.sched_priority = 20;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   // best effort

        std::vector<int16_t> mono(size_t(opt.period));
        std::vector<int16_t> stereo(size_t(opt.period) * 2);
        auto start = std::chrono::steady_clock::now();
        int64_t frames_written = 0;
        int underruns = 0;
        while (!g_quit) {
            float vol;
            {
                std::lock_guard<std::mutex> g(lock);
                board.render(mono.data(), opt.period);
                auto out = board.take_midi_out();
                if (!out.empty()) {
                    std::lock_guard<std::mutex> m(midi_out_lock);
                    midi_out.insert(midi_out.end(), out.begin(), out.end());
                }
                vol = volume;
            }
            for (int i = 0; i < opt.period; ++i) {
                // Fade in over the first half second: reSID's output settles from a DC step.
                float fade = std::min(1.0f, float(frames_written + i) / float(opt.sample_rate * 0.5));
                // 6 dB of headroom: hot patches peak at reSID's full scale.
                float s = std::clamp(mono[size_t(i)] * vol * fade * 0.5f, -32768.0f, 32767.0f);
                stereo[size_t(2 * i)] = stereo[size_t(2 * i + 1)] = int16_t(s);
            }
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, stereo.data(), snd_pcm_uframes_t(opt.period));
            if (n == -EPIPE) std::fprintf(stderr, "audio underrun %d\n", ++underruns);
            if (n < 0) n = snd_pcm_recover(pcm, int(n), 1);
            if (n < 0) { emit("error audio write: " + std::string(snd_strerror(int(n)))); break; }
            // Devices that never block (null, some plugins) would let the machine run flat out.
            frames_written += opt.period;
            auto due = start + std::chrono::microseconds(int64_t(frames_written * 1e6 / opt.sample_rate));
            auto ahead = due - std::chrono::steady_clock::now();
            if (ahead > std::chrono::microseconds(latency_us)) std::this_thread::sleep_for(ahead - std::chrono::microseconds(latency_us));
        }
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }

    // --- Ext In ----------------------------------------------------------------
    void capture_thread(std::string device)
    {
        snd_pcm_t* pcm = nullptr;
        int err = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0) { emit("error ext in open " + device + ": " + snd_strerror(err)); return; }
        unsigned latency_us = unsigned(1e6 * opt.period * 2 / opt.sample_rate);
        int channels = 2;
        err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                                 unsigned(opt.sample_rate), 1, latency_us);
        if (err < 0) {
            channels = 1;
            err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                                     unsigned(opt.sample_rate), 1, latency_us);
        }
        if (err < 0) { emit("error ext in params: " + std::string(snd_strerror(err))); snd_pcm_close(pcm); return; }
        emit("extin " + device);
        std::vector<int16_t> buf(size_t(opt.period) * size_t(channels));
        std::vector<int16_t> mono(size_t(opt.period));
        while (!g_quit && !capture_stop) {
            snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf.data(), snd_pcm_uframes_t(opt.period));
            if (n < 0) n = snd_pcm_recover(pcm, int(n), 1);
            if (n < 0) { emit("error ext in read: " + std::string(snd_strerror(int(n)))); break; }
            for (snd_pcm_sframes_t f = 0; f < n; ++f)
                mono[size_t(f)] = channels == 2 ? int16_t((buf[size_t(2 * f)] + buf[size_t(2 * f + 1)]) / 2)
                                                : buf[size_t(f)];
            std::lock_guard<std::mutex> g(lock);
            board.ext_in(mono.data(), size_t(n));
        }
        snd_pcm_close(pcm);
        std::lock_guard<std::mutex> g(lock);
        board.ext_in_clear();
    }

    void set_ext_in(const std::string& device)
    {
        capture_stop = true;
        if (capture.joinable()) capture.join();
        capture_stop = false;
        if (device.empty() || device == "none") { emit("extin none"); return; }
        capture = std::thread([this, device] { capture_thread(device); });
    }

    // --- MIDI ----------------------------------------------------------------
    bool open_seq()
    {
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) return false;
        snd_seq_set_client_name(seq, "SidPortal");
        in_port = snd_seq_create_simple_port(seq, "SidPortal MIDI In",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        out_port = snd_seq_create_simple_port(seq, "SidPortal MIDI Out",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        return in_port >= 0 && out_port >= 0;
    }

    void midi_thread()
    {
        snd_midi_event_t* decoder = nullptr;
        snd_midi_event_t* encoder = nullptr;
        snd_midi_event_new(256, &decoder);
        snd_midi_event_new(65536, &encoder);
        snd_midi_event_no_status(decoder, 1);   // always send full status bytes
        int nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
        std::vector<pollfd> fds(static_cast<size_t>(nfds));
        snd_seq_poll_descriptors(seq, fds.data(), unsigned(nfds), POLLIN);
        while (!g_quit) {
            poll(fds.data(), nfds_t(nfds), 2);
            snd_seq_event_t* ev = nullptr;
            while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
                std::vector<uint8_t> bytes;
                if (ev->type == SND_SEQ_EVENT_CONTROLLER && host_cc(int(ev->data.control.param),
                                                                    int(ev->data.control.value)))
                    continue;   // a CC the emulator itself uses, not passed to the firmware
                if (ev->type == SND_SEQ_EVENT_SYSEX) {
                    auto* p = static_cast<uint8_t*>(ev->data.ext.ptr);
                    bytes.assign(p, p + ev->data.ext.len);
                } else {
                    uint8_t buf[256];
                    long n = snd_midi_event_decode(decoder, buf, sizeof buf, ev);
                    if (n > 0) bytes.assign(buf, buf + n);
                }
                if (!bytes.empty()) {
                    std::lock_guard<std::mutex> g(lock);
                    for (uint8_t b : bytes) board.midi_in(b);
                }
            }
            std::vector<uint8_t> out;
            {
                std::lock_guard<std::mutex> m(midi_out_lock);
                out.swap(midi_out);
            }
            for (uint8_t b : out) {
                snd_seq_event_t oev;
                snd_seq_ev_clear(&oev);
                if (snd_midi_event_encode_byte(encoder, b, &oev) == 1) {
                    snd_seq_ev_set_source(&oev, out_port);
                    snd_seq_ev_set_subs(&oev);
                    snd_seq_ev_set_direct(&oev);
                    snd_seq_event_output_direct(seq, &oev);
                }
            }
        }
        snd_midi_event_free(decoder);
        snd_midi_event_free(encoder);
    }

    // --- Panel state -----------------------------------------------------------
    void status_thread()
    {
        std::string last_lcd, last_leds, last_syx;
        bool crash_reported = false;
        auto last_save = std::chrono::steady_clock::now();
        while (!g_quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            char line[512];
            std::string lcd, leds, syx, crash;
            std::vector<uint8_t> ram;
            bool save_now = false;
            {
                std::lock_guard<std::mutex> g(lock);
                const Hd44780& l = board.lcd();
                std::string chars, cg;
                for (int r = 0; r < 2; ++r)
                    for (int c = 0; c < 16; ++c) { std::snprintf(line, 4, "%02X", l.visible(r, c)); chars += line; }
                for (int i = 0; i < 64; ++i) { std::snprintf(line, 4, "%02X", l.cgram()[i]); cg += line; }
                std::snprintf(line, sizeof line, "lcd %s %s %d %d %d %d %d", chars.c_str(), cg.c_str(),
                              l.display_on(), l.cursor_on(), l.blink_on(), l.cursor_row(), l.cursor_col());
                lcd = line;
                std::snprintf(line, sizeof line, "leds %02X", board.leds());
                leds = line;
                if (board.crashed() && !crash_reported) {
                    crash_reported = true;
                    std::snprintf(line, sizeof line, "error firmware crashed (pc %04X)", board.crash_pc());
                    crash = line;
                }
                std::snprintf(line, sizeof line, "syx %zu", board.midi_in_pending());
                syx = line;
                if (!opt.nvram.empty() && std::chrono::steady_clock::now() - last_save > std::chrono::seconds(30)) {
                    ram.assign(board.ram(), board.ram() + 0x8000);
                    save_now = true;
                }
            }
            if (lcd != last_lcd) { emit(lcd); last_lcd = lcd; }
            if (leds != last_leds) { emit(leds); last_leds = leds; }
            if (syx != last_syx) { emit(syx); last_syx = syx; }
            if (!crash.empty()) { emit(crash); std::fprintf(stderr, "%s\n", crash.c_str()); }
            if (save_now) {
                last_save = std::chrono::steady_clock::now();
                std::string tmp = opt.nvram + ".tmp";
                std::ofstream f(tmp, std::ios::binary);
                f.write(reinterpret_cast<const char*>(ram.data()), long(ram.size()));
                f.close();
                if (f) std::rename(tmp.c_str(), opt.nvram.c_str());
            }
        }
    }

    void command(const std::string& line)
    {
        std::istringstream in(line);
        std::string cmd;
        if (!(in >> cmd)) return;
        if (cmd == "extin") {   // before taking the lock: stopping capture joins a thread that uses it
            std::string dev; std::getline(in >> std::ws, dev);
            set_ext_in(dev);
            return;
        }
        if (cmd == "engine") {   // build the engine before taking the lock: its tables take a while
            std::string m; in >> m;
            SidEngineType t;
            if (!parse_sid_engine(m, &t)) { emit("error unknown engine " + m); return; }
            auto engine = board.build_sid_engine(t);
            std::lock_guard<std::mutex> g(lock);
            board.set_sid_engine(std::move(engine));
            return;
        }
        std::lock_guard<std::mutex> g(lock);
        if (cmd == "biascc") { int n = -1; in >> n; bias_cc = (n >= 0 && n < 128) ? n : -1; learning = false; return; }
        if (cmd == "learn") { std::string what; in >> what; learning = (what == "bias"); return; }
        if (cmd == "extgain") { float v = 1; in >> v; board.set_ext_gain(std::clamp(v, 0.0f, 8.0f)); return; }
        if (cmd == "filterbias") { double v = 0; in >> v; board.set_filter_bias(std::clamp(v, -2.0, 2.0)); return; }
        if (cmd == "down" || cmd == "up") {
            int k = -1; in >> k;
            if (k >= 0 && k < 16) { if (cmd == "down") board.key_down(k); else board.key_up(k); }
        }
        else if (cmd == "enc") { int n = 0; in >> n; board.encoder(n); }
        else if (cmd == "knob") { int i = 0, v = 0; in >> i >> v; board.set_knob(i - 1, uint8_t(std::clamp(v, 0, 255))); }
        else if (cmd == "midi") { std::string h; while (in >> h) board.midi_in(uint8_t(std::stoul(h, nullptr, 16))); }
        else if (cmd == "syx") {
            std::string path; std::getline(in >> std::ws, path);
            auto data = read_file(path);
            if (data.empty()) emit("error cannot read " + path);
            for (uint8_t b : data) board.midi_in(b);
        }
        else if (cmd == "volume") { float v = 1; in >> v; volume = std::clamp(v, 0.0f, 4.0f); }
        else if (cmd == "model") { std::string m; in >> m; board.set_sid_model(m == "8580"); }
        else if (cmd == "save") { if (!opt.nvram.empty()) board.save_nvram(opt.nvram); }
        else if (cmd == "reset") board.reset();
        else if (cmd == "stat") {
            char buf[160];
            std::snprintf(buf, sizeof buf, "stat %llu %zu %02X dropped %u overruns %u",
                          static_cast<unsigned long long>(board.cycles()), board.midi_in_pending(), board.sccr2(),
                          board.cpu().rx_dropped, board.cpu().rx_overruns);
            emit(buf);
        }
        else if (cmd == "quit") g_quit = true;
    }
};

} // namespace

int run_realtime(Board& board, const Options& opt)
{
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    Realtime rt{board, opt, {}, {}, {}, {}, opt.volume, nullptr, -1, -1, {}, {false}, {-1}, {false}};
    if (opt.midi == "seq" && !rt.open_seq()) {
        rt.emit("error cannot open the ALSA sequencer");
        return 1;
    }
    std::thread audio([&] { rt.audio_thread(); });
    std::thread status([&] { rt.status_thread(); });
    std::thread midi;
    if (rt.seq) midi = std::thread([&] { rt.midi_thread(); });
    rt.emit("ready");
    board.set_ext_gain(opt.ext_gain);
    rt.bias_cc = opt.bias_cc;
    if (!opt.ext_in.empty()) rt.set_ext_in(opt.ext_in);
    // stdin is read on its own thread so a closed pipe (panel gone) also ends the engine.
    std::thread input([&] {
        std::string line;
        while (!g_quit && std::getline(std::cin, line)) rt.command(line);
        g_quit = true;
    });
    input.detach();
    while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    audio.join();
    status.join();
    rt.capture_stop = true;
    if (rt.capture.joinable()) rt.capture.join();
    if (midi.joinable()) midi.join();
    if (rt.seq) snd_seq_close(rt.seq);
    return 0;
}
