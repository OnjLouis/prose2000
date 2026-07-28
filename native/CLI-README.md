# Prose 2000 command-line renderer

`prose_cli.exe` runs the original Prose 2000/2020 firmware and writes its
speech to a mono, 16-bit, 10 kHz WAV file. It is a standalone command-line
program and does not require NVDA.

The renderer does not include firmware. Supply the standard six-file `prose2k`
ROM set in a directory of your choice. The firmware remains proprietary to
Telesensory Systems Inc./Speech Plus and is not covered by this project's BSD
license.

## Usage

```text
prose_cli ROM_DIRECTORY [CYCLES] [TEXT|__TEST_MODE__] [WAV_PATH]
```

- `ROM_DIRECTORY` contains the six standard Prose ROM files.
- `CYCLES` defaults to `120000000`.
- `TEXT` is the text to render. The required carriage return is added
  automatically.
- `__TEST_MODE__` runs the firmware's repeating hardware self-test instead.
- `WAV_PATH` is the optional output file.

Example:

```powershell
prose_cli "C:\roms\prose2k" 120000000 "This is a Prose 2000 test." "C:\Temp\prose-test.wav"
```

Run `prose_cli --help` for command-line help or `prose_cli --version` to show
the program version. Board, UART and DSP diagnostics are written to the
console after each run.

## Project

Source, NVDA integration and release information:
https://github.com/OnjLouis/prose2000

