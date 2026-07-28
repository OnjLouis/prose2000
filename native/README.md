# Prose diagnostic tools

The `native` directory builds the isolated NVDA host and two diagnostic tools.
These tools are intended for emulator development and regression testing; the
NVDA add-on does not require users to run them.

## Build

Configure and build with CMake using a C++20 compiler:

```powershell
cmake -S native -B build -G Ninja
cmake --build build
```

The build produces `ProseHost`, `prose_cli`, and the `prose_dsp` library.

## prose_cli

Usage:

```text
prose_cli ROM_DIRECTORY [CYCLES] [TEXT|__TEST_MODE__] [WAV_PATH]
```

- `ROM_DIRECTORY` is required and must contain the six ROM files named in
  `addon/synthDrivers/prose2000.py`.
- `CYCLES` is the emulated CPU-cycle budget. It defaults to `20000000`.
- `TEXT` is ASCII text sent to the emulated serial port. End it with sentence
  punctuation so the original firmware flushes the utterance.
- `__TEST_MODE__` selects the firmware's repeating hardware self-test instead
  of sending text.
- `WAV_PATH` writes captured mono, 16-bit, 10 kHz audio to that file.

Example:

```powershell
prose_cli "C:\roms\prose2k" 40000000 "This is a Prose 2000 test." "C:\Temp\prose-test.wav"
```

The remaining console output contains board, UART and DSP diagnostics. It is
primarily useful when comparing firmware execution or investigating emulator
timing; it is not a user-facing speech interface.

The original Prose firmware is proprietary to Telesensory Systems Inc./Speech
Plus and is not covered by this project's BSD license.
