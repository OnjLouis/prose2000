"""Exercise the standalone Prose uPD7720 core and inspect serial output."""

from __future__ import annotations

import argparse
import ctypes
import pathlib
import statistics


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("dll", type=pathlib.Path)
	parser.add_argument("rom_dir", type=pathlib.Path)
	parser.add_argument("--instructions", type=int, default=2_000_000)
	args = parser.parse_args()

	lib = ctypes.CDLL(str(args.dll))
	lib.prose_dsp_create.restype = ctypes.c_void_p
	lib.prose_dsp_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
	lib.prose_dsp_destroy.argtypes = [ctypes.c_void_p]
	lib.prose_dsp_set_reset.argtypes = [ctypes.c_void_p, ctypes.c_int]
	lib.prose_dsp_set_interrupt.argtypes = [ctypes.c_void_p, ctypes.c_int]
	lib.prose_dsp_run.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
	lib.prose_dsp_status_read.argtypes = [ctypes.c_void_p]
	lib.prose_dsp_status_read.restype = ctypes.c_uint8
	lib.prose_dsp_program_counter.argtypes = [ctypes.c_void_p]
	lib.prose_dsp_program_counter.restype = ctypes.c_uint16
	lib.prose_dsp_read_samples.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t]
	lib.prose_dsp_read_samples.restype = ctypes.c_size_t

	program = (args.rom_dir / "v3.12__8-9-88__dsp_prog.u29").read_bytes()
	data = (args.rom_dir / "v3.12__8-9-88__dsp_data.u29").read_bytes()
	program_buffer = ctypes.create_string_buffer(program)
	data_buffer = ctypes.create_string_buffer(data)
	handle = lib.prose_dsp_create(program_buffer, len(program), data_buffer, len(data))
	if not handle:
		raise RuntimeError("prose_dsp_create failed")

	try:
		lib.prose_dsp_set_reset(handle, 0)
		remaining = args.instructions
		# 8 MHz clock, four clocks per instruction, 10 kHz external INT:
		# one interrupt edge per 200 executed instructions.
		while remaining:
			step = min(200, remaining)
			lib.prose_dsp_run(handle, step)
			remaining -= step
			lib.prose_dsp_set_interrupt(handle, 1)
			lib.prose_dsp_set_interrupt(handle, 0)

		capacity = 4_000_000
		output = (ctypes.c_int16 * capacity)()
		count = lib.prose_dsp_read_samples(handle, output, capacity)
		values = list(output[:count])
		print(f"PC: {lib.prose_dsp_program_counter(handle):04x}")
		print(f"Status: {lib.prose_dsp_status_read(handle):02x}")
		print(f"Serial words: {count}")
		if values:
			print(f"Range: {min(values)} to {max(values)}")
			print(f"Mean: {statistics.fmean(values):.2f}")
			print("First 64:", " ".join(str(value) for value in values[:64]))
	finally:
		lib.prose_dsp_destroy(handle)
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
