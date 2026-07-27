# Prose 2000 for NVDA

Purpose-built emulation of the Telesensory Systems/Speech Plus Prose 2000/2020
speech synthesizer for modern NVDA.

The project does not embed or launch MAME. Its intended architecture is a small
engine containing only the hardware needed by Prose 2000:

- Intel 8086 firmware execution.
- Minimal 8251 UART and 8259 interrupt-controller behavior.
- NEC uPD7720 speech-DSP execution.
- Serial DSP output capture as PCM audio.
- A small isolated host that streams 10 kHz PCM to an NVDA synth driver.

The source build expects a directory containing the standard `prose2k` ROM
set. Firmware redistribution must be considered separately from the
BSD-licensed emulator source before a public release.

## Features

The native engine boots the original 8086 and uPD7720 firmware, accepts serial
text and reconstructs the board's 12-bit DAC output. `ProseHost.exe` runs
outside NVDA and communicates through a bounded binary protocol. NVDA remains
responsive while speech is generated, and can cancel or replace the host if it
stops responding.

The add-on currently provides speech, pause, cancellation, Say All completion
and NVDA audio-device routing. Rate, Rate Boost and Volume are exposed through
NVDA. Its DSP scheduler enters the firmware audio routine at the synthesis
loop's safe boundary, avoiding the timer race that corrupts sibilants.

The packaged add-on checks releases at
https://github.com/OnjLouis/prose2000 once a day. A manual check is available
from NVDA's Tools menu; network checks run outside NVDA's main thread.

SoundWave 1.2.1 and later can render Prose 2000 directly through an independent
host process without altering NVDA's active synthesizer or global audio path.

## Building

Configure `native` with CMake and build the `ProseHost` target. The diagnostic
`prose_cli` and `prose_dsp` targets are intentionally retained for firmware and
audio regression testing, but are not packaged in the NVDA add-on.

## Research basis

MAME's `tsispch.cpp` documents the board memory map and ROM layout under the
BSD-3-Clause license. MAME currently marks Prose 2000 as not working and without
sound because its uPD7725 core does not expose the serial output and clock used
by the board's DAC. This project implements that missing path directly instead
of distributing MAME.

## License

The emulator and NVDA integration source are available under the BSD 3-Clause
License. See `LICENSE` for the full terms and retained attribution.
