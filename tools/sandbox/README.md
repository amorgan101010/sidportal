# CI artifact sandboxes

These Docker sandboxes download and test artifacts uploaded by GitHub Actions.
They do not compile or package SidPortal. The Linux and Windows workflows build
the applications natively on their GitHub runners.

Run from the repository checkout with Docker and an authenticated GitHub CLI:

```sh
tools/sandbox/run.sh linux RUN_ID
tools/sandbox/run.sh windows RUN_ID
```

Replace `RUN_ID` with a completed Actions run ID that uploaded the requested
platform artifacts. An optional third argument selects a read-only test data
directory; by default the checkout is used. The package ZIP and SHA-256 file,
plus the platform's VST3 host checker, are downloaded under
`.sandbox-out/ci-run-RUN_ID/`.

The Linux sandbox verifies the checksum, opens the packaged JUCE standalone in
Xvfb and checks that its first-launch firmware picker appears. ALSA is routed to
a null sink. If test data includes `roms/firmware.bin`, it runs the VST3 audio
test and checks for non-silent output. Without firmware, the host checker still
verifies VST3 instantiation and callback processing. Firmware and presets are
mounted read-only and are never part of the build artifact.

The Windows sandbox runs the CI-built standalone and VST3 host checker under
Wine, checking the first-launch firmware picker, window creation, and plugin
processing. With local firmware it also checks for non-silent audio. This is a
useful Windows runtime smoke test, but does not replace testing in a native
Windows DAW.
