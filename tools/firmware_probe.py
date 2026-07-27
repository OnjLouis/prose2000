"""Boot and trace the Prose 2000 firmware without MAME.

This is deliberately a diagnostic harness, not the eventual NVDA engine. It
uses Unicorn for 8086 execution and supplies conservative placeholder behavior
for the board peripherals so we can identify the firmware's actual needs.
"""

from __future__ import annotations

import argparse
import collections
import ctypes
import os
import pathlib
import sys


def _load_unicorn():
	try:
		import unicorn
		return unicorn
	except ImportError:
		pass

	appdata = pathlib.Path(os.environ.get("APPDATA", ""))
	lib = appdata / "nvda" / "addons" / "monologue" / "synthDrivers" / "_monologue_engine" / "lib"
	arch = "x64" if sys.maxsize > 2**32 else "x86"
	os.environ.setdefault("LIBUNICORN_PATH", str(lib / "unicorn" / "lib" / arch))
	if str(lib) not in sys.path:
		sys.path.insert(0, str(lib))
	import unicorn
	return unicorn


unicorn = _load_unicorn()
from unicorn import Uc, UcError  # noqa: E402
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_SS, UC_X86_REG_SP  # noqa: E402


ROM_LAYOUT = (
	("v3.4.1__2000__2.u22", 0xC0000),
	("v3.4.1__2000__3.u45", 0xC0001),
	("v3.4.1__2000__0.u21", 0xE0000),
	("v3.4.1__2000__1.u44", 0xE0001),
)


class DspBridge:
	def __init__(self, dll_path: pathlib.Path, rom_dir: pathlib.Path):
		self.lib = ctypes.CDLL(str(dll_path))
		self.lib.prose_dsp_create.restype = ctypes.c_void_p
		self.lib.prose_dsp_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
		self.lib.prose_dsp_destroy.argtypes = [ctypes.c_void_p]
		self.lib.prose_dsp_set_reset.argtypes = [ctypes.c_void_p, ctypes.c_int]
		self.lib.prose_dsp_set_interrupt.argtypes = [ctypes.c_void_p, ctypes.c_int]
		self.lib.prose_dsp_run.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
		self.lib.prose_dsp_status_read.argtypes = [ctypes.c_void_p]
		self.lib.prose_dsp_status_read.restype = ctypes.c_uint8
		self.lib.prose_dsp_data_read.argtypes = [ctypes.c_void_p]
		self.lib.prose_dsp_data_read.restype = ctypes.c_uint8
		self.lib.prose_dsp_data_write.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
		self.lib.prose_dsp_read_samples.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t]
		self.lib.prose_dsp_read_samples.restype = ctypes.c_size_t
		program = (rom_dir / "v3.12__8-9-88__dsp_prog.u29").read_bytes()
		data = (rom_dir / "v3.12__8-9-88__dsp_data.u29").read_bytes()
		self._program = ctypes.create_string_buffer(program)
		self._data = ctypes.create_string_buffer(data)
		self.handle = self.lib.prose_dsp_create(self._program, len(program), self._data, len(data))
		if not self.handle:
			raise RuntimeError("prose_dsp_create failed")

	def close(self):
		if self.handle:
			self.lib.prose_dsp_destroy(self.handle)
			self.handle = None

	def set_reset(self, asserted: bool):
		self.lib.prose_dsp_set_reset(self.handle, asserted)

	def run_slice(self, instructions: int):
		self.lib.prose_dsp_run(self.handle, instructions)

	def pulse_interrupt(self):
		self.lib.prose_dsp_set_interrupt(self.handle, 1)
		self.lib.prose_dsp_set_interrupt(self.handle, 0)

	def status_read(self) -> int:
		return self.lib.prose_dsp_status_read(self.handle)

	def data_read(self) -> int:
		return self.lib.prose_dsp_data_read(self.handle)

	def data_write(self, value: int):
		self.lib.prose_dsp_data_write(self.handle, value)

	def sample_count(self) -> int:
		buffer = (ctypes.c_int16 * 1_000_000)()
		return self.lib.prose_dsp_read_samples(self.handle, buffer, len(buffer))


class Probe:
	MEM_SIZE = 0x100000
	UNICORN_MAP_SIZE = 0x200000
	UART_MIRROR = 0x341FC
	PIC_MIRROR = 0x341FC
	PARAM_MIRROR = 0x341FE
	DSP_MIRROR = 0x341FC
	RAM_MIRROR = 0x34000
	PERIPHERAL_PAGES = frozenset(0x3000 | bits for bits in (0, 0x4000, 0x10000, 0x14000, 0x20000, 0x24000, 0x30000, 0x34000))

	def __init__(self, rom_dir: pathlib.Path, trace_limit: int = 500, dsp_dll: pathlib.Path | None = None):
		self.uc = Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_16)
		# Unicorn's generic 16-bit x86 model can expose a 21-bit linear address
		# for FFFF:xxxx. A physical 8086 has only 20 address lines, so reserve
		# the extra range and explicitly mirror it onto the low megabyte.
		self.uc.mem_map(0, self.UNICORN_MAP_SIZE)
		# MAME's board map uses unmap_value_high: unpopulated address space reads
		# as an all-ones bus, not zero-filled RAM. Firmware error paths depend on
		# that distinction.
		open_bus = b"\xff" * 0x10000
		for base in range(0, self.UNICORN_MAP_SIZE, len(open_bus)):
			self.uc.mem_write(base, open_bus)
		self.trace_limit = trace_limit
		self.events: list[str] = []
		self.event_counts: collections.Counter[str] = collections.Counter()
		self.last_pcs: collections.deque[int] = collections.deque(maxlen=32)
		self.uart_rx: collections.deque[int] = collections.deque()
		self.uart_tx = bytearray()
		self.uart_mode_seen = False
		self.pic_writes: list[tuple[int, int]] = []
		self.param = 0
		self.dsp_data = 0
		self.dsp = DspBridge(dsp_dll, rom_dir) if dsp_dll else None
		self._cpu_instruction_counter = 0
		self._pending_restores: dict[int, bytes] = {}
		self._internal_write = False
		self._load_roms(rom_dir)
		self._map_peripherals()
		self.uc.hook_add(unicorn.UC_HOOK_MEM_READ | unicorn.UC_HOOK_MEM_FETCH, self._on_read)
		self.uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, self._on_write)
		self.uc.hook_add(unicorn.UC_HOOK_CODE, self._on_code)

	def _load_roms(self, rom_dir: pathlib.Path) -> None:
		for name, base in ROM_LAYOUT:
			data = (rom_dir / name).read_bytes()
			for index, value in enumerate(data):
				self.uc.mem_write(base + index * 2, bytes((value,)))

	def _map_peripherals(self) -> None:
		# The mirror masks place all board peripherals in one 4 KiB page,
		# repeated for every combination of address bits 14, 16 and 17.
		# True MMIO callbacks are important here: changing mapped memory from a
		# read hook can be observed by Unicorn as a separate CPU write.
		for mirror_bits in (0, 0x4000, 0x10000, 0x14000, 0x20000, 0x24000, 0x30000, 0x34000):
			for a20_alias in (0, self.MEM_SIZE):
				base = 0x3000 | mirror_bits | a20_alias
				self.uc.mem_unmap(base, 0x1000)
				self.uc.mmio_map(base, 0x1000, self._mmio_read, base, self._mmio_write, base)

	def _mmio_read(self, uc, offset: int, size: int, base: int) -> int:
		bus_address = (base + offset) & (self.MEM_SIZE - 1)
		result = 0xFF
		kind = "open_bus.read"
		if self._matches(bus_address, 0x3000, 0x3003, self.UART_MIRROR):
			register = self._offset(bus_address, self.UART_MIRROR, 0x3000)
			if register < 2:
				result = self.uart_rx.popleft() if self.uart_rx else 0
				kind = "uart.data.read"
			else:
				result = 0x05 | (0x02 if self.uart_rx else 0)
				kind = "uart.status.read"
		elif self._matches(bus_address, 0x3200, 0x3203, self.PIC_MIRROR):
			result = 0
			kind = "pic.read"
		elif self._matches(bus_address, 0x3400, 0x3400, self.PARAM_MIRROR):
			result = 0xFC
			kind = "switch.read"
		elif self._matches(bus_address, 0x3600, 0x3600, self.DSP_MIRROR):
			result = self.dsp.data_read() if self.dsp else self.dsp_data
			kind = "dsp.data.read"
		elif self._matches(bus_address, 0x3602, 0x3602, self.DSP_MIRROR):
			result = self.dsp.status_read() if self.dsp else 0x80
			kind = "dsp.status.read"
		value = result & 0xFF
		if size > 1:
			value |= ((1 << ((size - 1) * 8)) - 1) << 8
		self._record(kind, f"pc={self.pc:05x} address={bus_address:05x} size={size} value={value:0{size * 2}x}")
		return value

	def _mmio_write(self, uc, offset: int, size: int, value: int, base: int) -> None:
		bus_address = (base + offset) & (self.MEM_SIZE - 1)
		data = value & 0xFF
		if self._matches(bus_address, 0x3000, 0x3003, self.UART_MIRROR):
			register = self._offset(bus_address, self.UART_MIRROR, 0x3000)
			if register < 2:
				self.uart_tx.append(data)
				kind = "uart.data.write"
			else:
				kind = "uart.control.write"
		elif self._matches(bus_address, 0x3200, 0x3203, self.PIC_MIRROR):
			register = self._offset(bus_address, self.PIC_MIRROR, 0x3200)
			self.pic_writes.append((register, data))
			kind = "pic.write"
		elif self._matches(bus_address, 0x3401, 0x3401, self.PARAM_MIRROR):
			self.param = data
			if self.dsp:
				self.dsp.set_reset(not bool(self.param & 0x40))
			kind = "parameter.write"
		elif self._matches(bus_address, 0x3600, 0x3600, self.DSP_MIRROR):
			self.dsp_data = data
			if self.dsp:
				self.dsp.data_write(data)
			kind = "dsp.data.write"
		elif self._matches(bus_address, 0x3602, 0x3602, self.DSP_MIRROR):
			kind = "dsp.status.write"
		else:
			kind = "open_bus.write"
		self._record(kind, f"pc={self.pc:05x} address={bus_address:05x} size={size} value={data:02x}")

	def _record(self, kind: str, detail: str) -> None:
		self.event_counts[kind] += 1
		if len(self.events) < self.trace_limit:
			self.events.append(f"{kind}: {detail}")

	@staticmethod
	def _matches(address: int, start: int, end: int, mirror: int) -> bool:
		normal = address & ~mirror
		return start <= normal <= end

	@staticmethod
	def _offset(address: int, mirror: int, start: int) -> int:
		return (address & ~mirror) - start

	def _write_read_value(self, address: int, size: int, value: int, *, low_byte_device: bool = False) -> None:
		if low_byte_device and size > 1:
			# The board connects these peripherals only to D0-D7.  MAME's
			# umask16(0x00ff) therefore leaves every upper byte as open bus.
			value = (value & 0xFF) | (((1 << ((size - 1) * 8)) - 1) << 8)
		self._write_internal(address, int(value).to_bytes(size, "little", signed=False))

	def _write_internal(self, address: int, data: bytes) -> None:
		self._internal_write = True
		try:
			self.uc.mem_write(address, data)
		finally:
			self._internal_write = False

	def _on_read(self, uc, access, address, size, value, user_data) -> None:
		bus_address = address & (self.MEM_SIZE - 1)
		if (bus_address & ~0xFFF) in self.PERIPHERAL_PAGES:
			return
		if self._matches(bus_address, 0x0000, 0x2FFF, self.RAM_MIRROR):
			canonical = bus_address & ~self.RAM_MIRROR
			if address != canonical:
				self._write_internal(address, bytes(self.uc.mem_read(canonical, size)))
			return
		if self._matches(bus_address, 0x3000, 0x3003, self.UART_MIRROR):
			offset = self._offset(bus_address, self.UART_MIRROR, 0x3000)
			if offset < 2:
				result = self.uart_rx.popleft() if self.uart_rx else 0
				kind = "uart.data.read"
			else:
				# 8251: TXRDY | TXEMPTY, plus RXRDY when input is queued.
				result = 0x05 | (0x02 if self.uart_rx else 0)
				kind = "uart.status.read"
			self._write_read_value(address, size, result, low_byte_device=True)
			self._record(kind, f"pc={self.pc:05x} address={bus_address:05x} size={size} value={result:02x}")
			return

		if self._matches(bus_address, 0x3200, 0x3203, self.PIC_MIRROR):
			self._write_read_value(address, size, 0, low_byte_device=True)
			self._record("pic.read", f"pc={self.pc:05x} address={address:05x}")
			return

		if self._matches(bus_address, 0x3400, 0x3400, self.PARAM_MIRROR):
			self._write_read_value(address, size, 0xFC, low_byte_device=True)
			self._record("switch.read", f"pc={self.pc:05x}")
			return

		if self._matches(bus_address, 0x3600, 0x3600, self.DSP_MIRROR):
			result = self.dsp.data_read() if self.dsp else self.dsp_data
			self._write_read_value(address, size, result, low_byte_device=True)
			self._record("dsp.data.read", f"pc={self.pc:05x} value={result:02x}")
			return

		if self._matches(bus_address, 0x3602, 0x3602, self.DSP_MIRROR):
			result = self.dsp.status_read() if self.dsp else 0x80
			self._write_read_value(address, size, result, low_byte_device=True)
			self._record("dsp.status.read", f"pc={self.pc:05x} value={result:02x}")
			return

		if address != bus_address:
			self._write_internal(address, bytes(self.uc.mem_read(bus_address, size)))
		elif not (0xC0000 <= bus_address <= 0xFFFFF):
			self._write_read_value(address, size, (1 << (size * 8)) - 1)

	def _on_write(self, uc, access, address, size, value, user_data) -> None:
		if self._internal_write:
			return
		value &= (1 << (size * 8)) - 1
		bus_address = address & (self.MEM_SIZE - 1)
		if (bus_address & ~0xFFF) in self.PERIPHERAL_PAGES:
			return
		if self._matches(bus_address, 0x0000, 0x2FFF, self.RAM_MIRROR):
			canonical = bus_address & ~self.RAM_MIRROR
			encoded = value.to_bytes(size, "little")
			self._write_internal(canonical, encoded)
			self._write_internal(self.MEM_SIZE + canonical, encoded)
			return
		if self._matches(bus_address, 0x3000, 0x3003, self.UART_MIRROR):
			offset = self._offset(bus_address, self.UART_MIRROR, 0x3000)
			if offset < 2:
				self.uart_tx.append(value & 0xFF)
				self._record("uart.data.write", f"pc={self.pc:05x} address={bus_address:05x} size={size} value={value & 0xff:02x}")
			else:
				self._record("uart.control.write", f"pc={self.pc:05x} address={bus_address:05x} size={size} value={value & 0xff:02x}")
			return

		if self._matches(bus_address, 0x3200, 0x3203, self.PIC_MIRROR):
			offset = self._offset(bus_address, self.PIC_MIRROR, 0x3200)
			self.pic_writes.append((offset, value & 0xFF))
			self._record("pic.write", f"pc={self.pc:05x} offset={offset} value={value & 0xff:02x}")
			return

		if self._matches(bus_address, 0x3401, 0x3401, self.PARAM_MIRROR):
			self.param = value & 0xFF
			if self.dsp:
				self.dsp.set_reset(not bool(self.param & 0x40))
			self._record("parameter.write", f"pc={self.pc:05x} value={self.param:02x}")
			return

		if self._matches(bus_address, 0x3600, 0x3600, self.DSP_MIRROR):
			self.dsp_data = value & 0xFF
			if self.dsp:
				self.dsp.data_write(self.dsp_data)
			self._record("dsp.data.write", f"pc={self.pc:05x} value={self.dsp_data:02x}")
			return

		if self._matches(bus_address, 0x3602, 0x3602, self.DSP_MIRROR):
			self._record("dsp.status.write", f"pc={self.pc:05x} value={value & 0xff:02x}")
			return

		if address != bus_address:
			self._write_internal(bus_address, value.to_bytes(size, "little"))
		# Writes to ROM and unpopulated address space have no storage behind
		# them. Unicorn performs the write after this callback, so restore the
		# original bytes at the next instruction boundary.
		if 0xC0000 <= bus_address <= 0xFFFFF:
			self._pending_restores[address] = bytes(self.uc.mem_read(address, size))
		else:
			self._pending_restores[address] = b"\xff" * size

	def _on_code(self, uc, address, size, user_data) -> None:
		if self._pending_restores:
			for restore_address, original in self._pending_restores.items():
				self._write_internal(restore_address, original)
			self._pending_restores.clear()
		self.last_pcs.append(address)
		if self.dsp:
			self._cpu_instruction_counter += 1
			if self._cpu_instruction_counter >= 200:
				self.dsp.run_slice(self._cpu_instruction_counter)
				self.dsp.pulse_interrupt()
				self._cpu_instruction_counter = 0

	@property
	def pc(self) -> int:
		return ((self.uc.reg_read(UC_X86_REG_CS) << 4) + self.uc.reg_read(UC_X86_REG_IP)) & 0xFFFFF

	def run(self, instruction_count: int) -> None:
		self.uc.reg_write(UC_X86_REG_CS, 0xFFFF)
		self.uc.reg_write(UC_X86_REG_IP, 0)
		self.uc.reg_write(UC_X86_REG_SS, 0)
		self.uc.reg_write(UC_X86_REG_SP, 0)
		try:
			self.uc.emu_start(0xFFFF0, 0, count=instruction_count)
		except UcError as error:
			print(f"Emulation stopped: {error} at {self.pc:05x}")

	def report(self) -> None:
		print(f"Final PC: {self.pc:05x}")
		print("Event counts:")
		for key, count in self.event_counts.most_common():
			print(f"  {key}: {count}")
		print("Trace:")
		for event in self.events:
			print(f"  {event}")
		if self.uart_tx:
			print("UART output:", self.uart_tx.decode("latin1", errors="replace"))
		if self.dsp:
			print(f"Captured serial words: {self.dsp.sample_count()}")
		print("Last PCs:", " ".join(f"{pc:05x}" for pc in self.last_pcs))


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("rom_dir", type=pathlib.Path)
	parser.add_argument("--instructions", type=int, default=5_000_000)
	parser.add_argument("--trace-limit", type=int, default=500)
	parser.add_argument("--dsp-dll", type=pathlib.Path)
	args = parser.parse_args()
	probe = Probe(args.rom_dir, trace_limit=args.trace_limit, dsp_dll=args.dsp_dll)
	try:
		probe.run(args.instructions)
		probe.report()
	finally:
		if probe.dsp:
			probe.dsp.close()
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
