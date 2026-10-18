# SidPortal

![The SidPortal panel](screenshots/panel.png)

SidPortal is a Linux desktop emulator of the Elektron SidStation, a MOS 6581 synthesizer module from 1999. It runs the SidStation's original OS r34 firmware on an emulated 68HC11, produces sound with reSIDfp or reSID, plays in realtime from ALSA MIDI, and has a front panel with the LCD, LEDs, knobs, rotary encoder and keypad, plus host controls for the volume, the SID chip and the filter character.

**Firmware is not included.** You must supply your own dump of the SidStation OS r34 firmware (either 128 KB EPROM dump, or the 32 KB program). The import tool recognises it by SHA-1 and stores it as `roms/firmware.bin`.

**Disclaimer:** SidPortal is an independent project. It is not affiliated with, endorsed by or sponsored by Elektron. Elektron and SidStation are trademarks of their respective owners, and are used here only to say which hardware SidPortal emulates.

## Requirements

You need the following:

- Linux;
- a C++20 compiler;
- CMake 3.20 or newer;
- the ALSA development headers;
- Python 3 with Tkinter.

The packages are as follows:

- **Arch:** `base-devel cmake alsa-lib python tk`
- **Debian or Ubuntu:** `build-essential cmake libasound2-dev python3-tk`

The VST3 also needs a JUCE 8 checkout; see below.

## Build and run

```sh
python3 tools/roms.py --import-zip /path/to/firmware_dumps.zip
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
python3 tools/gui.py
```

The panel starts the realtime engine and creates an ALSA sequencer client **SidPortal** with the ports **SidPortal MIDI In** and **SidPortal MIDI Out**. Choose a source under **Setup… > MIDI Input** in the menu behind the gear button at the top right of the panel (a right-click anywhere on the panel opens it too), or connect one with `aconnect` or a patchbay. The Setup dialog also holds the keyboard channel, the SID emulation, the audio output, Ext In and the filter-character CC. The firmware listens on its base channel, which is 1 by default; to change it, go to System > MIDI.

The battery-backed RAM, which holds the patches and settings, lives in `$XDG_DATA_HOME/sidportal/nvram.bin`. Panel settings live in `$XDG_CONFIG_HOME/sidportal/config.json`. The engine saves the RAM every 30 seconds and on exit.

A fresh unit has no patches, just as the hardware does after a memory reset. On the first run the panel sends the factory bank once boot finishes, if `patches/` holds one (a `.syx` file with "Presets" in its name). **Load SysEx…** in the gear menu sends any other bank. An all-patch bank replaces every patch in memory.

`tools/install_desktop.sh` adds SidPortal to the desktop's application menu. It writes `~/.local/share/applications/sidportal.desktop` pointing at this checkout and installs the icon from `tools/assets/icons`.

## VST3 plugin

If a JUCE 8 checkout sits next to this one (`../JUCE`), or wherever `-DJUCE_DIR=` points, the build also produces a VST3 at `build/SidPortalPlugin_artefacts/Release/VST3/SidPortal.vst3`, plus a standalone version of it. Copy the VST3 to `~/.vst3`. It runs the same emulated board as the panel, drawn with the same artwork. On Debian or Ubuntu, JUCE also needs `libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev`.

![The SidPortal VST3](screenshots/plugin.png)

- **Firmware:** the plugin loads it at runtime from `$SIDPORTAL_ROM`, `$XDG_DATA_HOME/sidportal/firmware.bin` or this project's `roms/`. It is never built into the plugin.
- **Memory:** each instance keeps its own battery-backed RAM in the project. A new instance starts from a copy of the standalone panel's memory, or from a cleared unit that receives the factory bank. Projects never write back to the standalone's memory. Booting is emulated faster than realtime, so an instance is ready about two seconds after it loads.
- **Parameters:** the four knobs, Volume, Filter character and the SID chip, so the host can automate them and MIDI-learn controllers onto them.
- **Gear menu:** Load SysEx… and the SID emulation engine.
- **MIDI:** MIDI from the host goes to the firmware at its sample position. The firmware's MIDI out, which carries SysEx dumps and the knobs' CCs, comes back to the host.
- **Ext In:** an optional stereo input bus named "Ext In", off by default. Enable it in the host and its audio goes into the SID's external input, as Ext In does on the panel.

`ctest` runs two plugin checks. The first, `sidportal_vst3_hostcheck`, loads the built bundle the way a DAW does. The second, `sidportal_plugin_smoke`, does the following:
- boots a cleared instance;
- plays a note;
- switches engines and chips;
- saves the state and restores it into a second instance;
- feeds the Ext In bus;
- changes the sample rate.

## Look

The panel's printing is set in Orbitron, the nearest open font to the hardware's wide, rounded-square lettering. The keypad keys keep a plain sans, like the hardware's. The artwork is bundled in `tools/assets/panel`, so neither the panel nor the build needs any image libraries. The knob pointers, LEDs and encoder dimple are images too, because Tk's own canvas lines and circles have no antialiasing. Each knob size has a sprite sheet with one frame per position. The plugin uses a set at twice the panel size, so it stays sharp when the plugin window is enlarged.


## Playing it

- **Keypad:** A is EXIT, B is UP, C is DOWN, D is ENTER, `*` is LEFT and `#` is RIGHT. The digits select menu items.
- **Realtime knobs:** drag vertically, hold Shift for fine control, or use the mouse wheel. They map to the values on screen from left to right. In patch play they are the patch's direct controllers DC1–DC4, which the firmware also sends and receives as **CC 16–19**, so set a hardware controller to those CCs to use it in their place.
- **Encoder:** drag around it or use the mouse wheel.
- **Computer keyboard:**
  - `0`–`9` are the digit keys.
  - The arrow keys are UP/DOWN/LEFT/RIGHT.
  - Return is ENTER; BackSpace or Escape is EXIT.
  - `-` and `=` turn the encoder.
  - Hold `A` `S` `D` `F` and tap `-` or `=` to turn knobs 1–4.
- **Keyboard strip:** the strip below the panel plays notes on the keyboard channel set in Setup.

The right-hand column of the panel holds controls the unit never had:

- **Output > Volume** sets the output level.
- **SID** is a slide switch: it picks the 6581 (the original chip) or the 8580. The logo's subtitle follows it.
- **Filter** sets the chip character.

**SID emulation** under **Setup…** picks the engine:

- **reSIDfp** (the default) comes from libresidfp, the engine in sidplayfp. It models the filters of both chips in more detail, including the 6581's distortion, and has per-chip filter curves.
- **reSID** is Dag Lem's original engine.

Switching engines takes effect live, without rebooting the machine: the new engine picks up the current SID registers, so held notes keep sounding. Switching chips or engines fades the sound out and back in over about a third of a second, because changing chips steps the output's DC level, which would otherwise come through as a loud thump. The emulated machine boots for about 12 seconds before the top menu appears, just like the unit. The output stays muted until the firmware has initialised the SID, which skips the thump the real unit makes at power-on. MIDI sent during boot is ignored. On the real unit, a note sent at that point leaves a half-received message, and the firmware then restarts itself as soon as the next status byte arrives.

**Ext In** works like the jack on the unit: audio from the capture device chosen under **Setup… > Ext in**, such as `pipewire` or a card's `sysdefault`, goes into the SID's external input, which the firmware routes through the filter. The filter type, cutoff, resonance and filter envelope all shape it, and note-ons and CCs can trigger the envelope. **Ext in gain** sets the input level. From the command line, use `--ext-in DEV --ext-gain F`, or the headless `extin FILE.wav` command.

The **Filter** knob, marked dark and bright, sets the chip character. Real SID filters differ from chip to chip: the same cutoff setting sounds darker on some units and brighter on others. Double-clicking the knob returns it to the centre. How it works depends on the engine:

- **reSIDfp:** it moves the filter curve of either chip from one end of its range to the other.
- **reSID:** it shifts the 6581's cutoff DAC voltage (the filter bias) between −0.5 and +0.5 V. It has no effect on the 8580 under reSID, so the knob is hidden then. You can assign it a MIDI CC under **Setup… > Filter character CC**: type a number, or press **Learn** and move a control. The CC works on any channel, maps 0–127 across the range, and is not passed on to the firmware. From the command line, use `--filter-bias V` (−0.5 to +0.5), `--bias-cc N`, `--engine resid|residfp`, or the `filterbias V`, `model 6581|8580` and `engine NAME` commands.

A faint 16 Hz buzz at idle is authentic. The 6581's oscillators leak through even with their envelopes closed, as the manual describes.

## Command line

```sh
build/sidportal --rom roms/firmware.bin --realtime --audio default
build/sidportal --rom roms/firmware.bin --fresh --script "wait 12000;syx patches/Presets.syx;press enter;wait 500;lcd"
build/sidportal --rom roms/firmware.bin --nvram ram.bin --render out.wav --script "wait 12000;press enter;wait 500;press enter;note 57 100 800"
build/sidportal --help
```

Headless scripts are `;`-separated commands:

- `wait MS`
- `lcd`
- `press KEY` (also `down KEY` and `up KEY`)
- `enc N`
- `knob I V`
- `midi HEX..`
- `note N VEL MS`
- `syx FILE`
- `leds`
- `filterbias V`, `model 6581|8580` and `engine resid|residfp`
- `save`

Run `ctest --test-dir build --output-on-failure` for the tests. The firmware test needs the ROM, and its SysEx checks also need the factory bank in `patches/`.

## How it works

The emulated board is a 68HC11 with a 2 MHz E clock and the following memory and I/O map:

| Address | Device |
| --- | --- |
| `$0000–$7FFF` | 32 KB battery-backed RAM |
| `$8000–$FFFF` | Firmware |
| `$1040` | SID, clocked at 1 MHz |
| `$1060` / `$1061` | HD44780 LCD (instruction / data) |
| `$1080` | Keypad encoder latch |

The rest of the panel and MIDI go through the HC11's on-chip peripherals:

- **Knobs:** the HC11 A/D converter.
- **Rotary encoder:** pulses input capture 1 (up) or 2 (down).
- **MIDI:** the SCI at 31250 baud.
- **LEDs:** port A.

The firmware's update engine runs from output compare 2. Every SID write is applied at its own CPU cycle, because the instrument's character comes from those 50–200 Hz register updates. `src/core` holds the CPU, the LCD and the board. `src/main.cpp` is the host: headless scripting, WAV rendering, and the ALSA engine. `tools/gui.py` is the panel.

## Status

The emulator boots, menus and editing work, it receives and sends SysEx, it plays from MIDI, and the LEDs work. Some parts are not emulated:

- the flash-upgrade mode;
- the ASID mode.

It has not been compared against a recording of real hardware, so the sound is the emulation engine's model of the chip, not a measured match for any particular unit.

## License

GPL-3.0-or-later; see [LICENSE](LICENSE). Vendored code keeps its own license; see [THIRD_PARTY.md](THIRD_PARTY.md).
