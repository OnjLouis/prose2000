# Prose 2000 native tools

The `native` directory builds the isolated NVDA host, the ROM-free Prose 2000
command-line renderer and a DSP library used by both programs. The NVDA add-on
does not require users to run the command-line renderer.

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
- `CYCLES` is the emulated CPU-cycle budget. It defaults to `120000000`, which
  allows the original firmware to boot and finish a typical short utterance.
- `TEXT` is ASCII text sent to the emulated serial port. The renderer adds the
  required carriage return automatically.
- `__TEST_MODE__` selects the firmware's repeating hardware self-test instead
  of sending text.
- `WAV_PATH` writes captured mono, 16-bit, 10 kHz audio to that file.

Example:

```powershell
prose_cli "C:\roms\prose2k" 120000000 "This is a Prose 2000 test." "C:\Temp\prose-test.wav"
```

Use `prose_cli --help` for command-line help and `prose_cli --version` for the
program version. The remaining console output contains board, UART and DSP
diagnostics that can help emulator development and regression testing.

The original Prose firmware is proprietary to Telesensory Systems Inc./Speech
Plus and is not covered by this project's BSD license.
