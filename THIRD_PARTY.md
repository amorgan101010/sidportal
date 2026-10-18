# Third-party components

## reSID (vendor/resid)

MOS 6581/8580 SID emulation by Dag Lem and contributors, from https://github.com/libsidplayfp/resid (master at 3bf8eff). Licensed under the GNU General Public License, version 2 or later; see `vendor/resid/COPYING`.

The repository was changed in three ways:
- `siddefs.h` was generated from `siddefs.h.in`.
- The `wave*.h` tables were generated from the `.dat` samples with the bundled `samp2src.pl`.
- The autotools files were removed.

## reSIDfp (vendor/residfp)

The reSIDfp SID emulation by Leandro Nini, Antti S. Lankila, Dag Lem and contributors, from libresidfp (https://github.com/libsidplayfp/libresidfp, master at c179138, version 1.3.0a). Licensed under the GNU General Public License, version 2 or later; see `vendor/residfp/COPYING`.

Only the library sources under `src/` are included. The following were left out:
- the unit tests;
- the resampler test program;
- the autotools and Doxygen files.

Two headers were generated from their `.in` templates with this project's settings: `siddefs-fp.h` (branch hints and inlining on) and `residfp/sidversion.h`.

## HD44780 A00 font (tools/assets/hd44780_a00.py)

The character ROM bitmap was rasterized from the Hitachi HD44780 datasheet by Uri Shaked for wokwi-elements (https://github.com/wokwi/wokwi-elements). It is under the MIT License, Copyright (c) 2020 Uri Shaked; see `tools/assets/LICENSE-wokwi-font-MIT.txt`.

## Orbitron font

The panel artwork's lettering is set in Orbitron by Matt McInerney and The League of Moveable Type (https://github.com/google/fonts/tree/main/ofl/orbitron), which is under the SIL Open Font License 1.1. The bundled images in `tools/assets/panel` contain text rendered in it.


## JUCE (the VST3 plugin only)

The plugin and its standalone are built with JUCE 8 (https://juce.com) from a local checkout; nothing from JUCE is vendored here. JUCE is used under the GNU Affero General Public License v3, so plugin binaries built from this project are distributed under AGPLv3 terms. The panel program and the engine don't use JUCE.

## Not included

The firmware, its factory patch banks, and the owner's manual are not part of this project.
