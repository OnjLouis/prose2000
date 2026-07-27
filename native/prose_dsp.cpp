// Standalone NEC uPD7720 core for the Prose 2000 research engine.
//
// The instruction execution and host-port behavior are adapted from MAME's
// src/devices/cpu/upd7725/upd7725.cpp:
//   license: BSD-3-Clause
//   copyright-holders: R. Belmont, byuu
//   original core by byuu (public domain)
//
// Prose-specific ROM unpacking and serial-output capture:
//   copyright-holders: Andre Louis and OpenAI

#include "prose_dsp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <new>

namespace {

uint8_t reverse8(uint8_t value) {
	value = static_cast<uint8_t>(((value & 0x55U) << 1) | ((value >> 1) & 0x55U));
	value = static_cast<uint8_t>(((value & 0x33U) << 2) | ((value >> 2) & 0x33U));
	return static_cast<uint8_t>((value << 4) | (value >> 4));
}

uint16_t bitswap16(uint16_t value, const std::array<unsigned, 16> &order) {
	uint16_t result = 0;
	for (const unsigned source : order) {
		result = static_cast<uint16_t>((result << 1) | ((value >> source) & 1U));
	}
	return result;
}

uint16_t byteSwap16(uint16_t value) {
	return static_cast<uint16_t>((value << 8) | (value >> 8));
}

struct Flag {
	bool s1 = false;
	bool s0 = false;
	bool c = false;
	bool z = false;
	bool ov1 = false;
	bool ov0 = false;

	void assign(uint16_t value) {
		s1 = (value & 0x20U) != 0;
		s0 = (value & 0x10U) != 0;
		c = (value & 0x08U) != 0;
		z = (value & 0x04U) != 0;
		ov1 = (value & 0x02U) != 0;
		ov0 = (value & 0x01U) != 0;
	}
};

struct Status {
	bool rqm = false;
	bool usf1 = false;
	bool usf0 = false;
	bool drs = false;
	bool dma = false;
	bool drc = false;
	bool soc = false;
	bool sic = false;
	bool ei = false;
	bool p1 = false;
	bool p0 = false;

	uint16_t value() const {
		return static_cast<uint16_t>(
			(rqm << 15) | (usf1 << 14) | (usf0 << 13) | (drs << 12) |
			(dma << 11) | (drc << 10) | (soc << 9) | (sic << 8) |
			(ei << 7) | (p1 << 1) | p0);
	}

	void assign(uint16_t value) {
		rqm = (value & 0x8000U) != 0;
		usf1 = (value & 0x4000U) != 0;
		usf0 = (value & 0x2000U) != 0;
		drs = (value & 0x1000U) != 0;
		dma = (value & 0x0800U) != 0;
		drc = (value & 0x0400U) != 0;
		soc = (value & 0x0200U) != 0;
		sic = (value & 0x0100U) != 0;
		ei = (value & 0x0080U) != 0;
		p1 = (value & 0x0002U) != 0;
		p0 = (value & 0x0001U) != 0;
	}
};

struct Registers {
	uint16_t pc = 0;
	std::array<uint16_t, 4> stack{};
	uint16_t rp = 0;
	uint16_t dp = 0;
	uint8_t sp = 0;
	int16_t k = 0;
	int16_t l = 0;
	int16_t m = 0;
	int16_t n = 0;
	int16_t a = 0;
	int16_t b = 0;
	Flag flaga{};
	Flag flagb{};
	uint16_t tr = 0;
	uint16_t trb = 0;
	Status sr{};
	uint16_t dr = 0;
	uint16_t si = 0;
	uint16_t so = 0;
	uint16_t idb = 0;
	bool siack = false;
	bool soack = false;
};

class Dsp {
public:
	bool load(const uint8_t *packed, size_t packedSize, const uint8_t *data, size_t dataSize) {
		if (!packed || packedSize != 0x600 || !data || dataSize != 0x400) {
			return false;
		}
		for (size_t instruction = 0; instruction < program_.size(); ++instruction) {
			const size_t offset = instruction * 3;
			const uint8_t first = reverse8(packed[offset]);
			const uint16_t rest = static_cast<uint16_t>((packed[offset + 1] << 8) | packed[offset + 2]);
			std::array<unsigned, 16> order{};
			if ((first & 0x80U) == 0) {
				order = {8, 9, 10, 15, 11, 12, 13, 14, 0, 1, 2, 3, 4, 5, 6, 7};
			} else if ((first & 0xC0U) == 0x80U) {
				order = {8, 9, 15, 15, 15, 10, 11, 12, 13, 14, 0, 1, 2, 3, 6, 7};
			} else {
				order = {8, 9, 10, 11, 12, 13, 14, 0, 1, 2, 3, 3, 4, 5, 6, 7};
			}
			program_[instruction] = (static_cast<uint32_t>(first) << 16) | bitswap16(rest, order);
		}
		for (size_t word = 0; word < dataRom_.size(); ++word) {
			// The 77P20 programmer dump walks ROM addresses downward. Restore
			// ascending address order while preserving the three low alignment
			// bits used when a 13-bit coefficient enters the 16-bit multiplier.
			const size_t source = dataRom_.size() - 1 - word;
			dataRom_[word] = static_cast<uint16_t>(
				data[source * 2] | (data[source * 2 + 1] << 8));
		}
		coldReset();
		return true;
	}

	void coldReset() {
		regs_ = Registers{};
		dataRam_.fill(0);
		irq_ = false;
		irqFiring_ = 0;
		resetAsserted_ = true;
		samples_.clear();
		p0Events_.clear();
		hostReads_ = 0;
		ramWrites_ = 0;
		ramChanges_ = 0;
		lastOutputSource_ = 0;
		lastOutputPointer_ = 0;
		servicingAudio_ = false;
		audioTriggeredAtBoundary_ = false;
	}

	void resetLine(bool asserted) {
		if (asserted) {
			regs_.pc = 0;
			regs_.sr.assign(0);
			regs_.flaga.assign(0);
			regs_.flagb.assign(0);
			regs_.siack = false;
			regs_.soack = false;
			irqFiring_ = 0;
			p0Events_.clear();
		}
		resetAsserted_ = asserted;
	}

	void interruptLine(bool asserted) {
		if (!irq_ && asserted && regs_.sr.ei) {
			irqFiring_ = 1;
			regs_.sr.ei = false;
		}
		irq_ = asserted;
	}

	void run(uint32_t instructions) {
		if (resetAsserted_) {
			return;
		}
		while (instructions-- != 0) {
			step();
		}
	}

	uint8_t statusRead() const { return static_cast<uint8_t>(regs_.sr.value() >> 8); }

	uint8_t dataRead() {
		if (!regs_.sr.drc) {
			if (!regs_.sr.drs) {
				regs_.sr.drs = true;
				return static_cast<uint8_t>(regs_.dr);
			}
			regs_.sr.rqm = false;
			regs_.sr.drs = false;
			return static_cast<uint8_t>(regs_.dr >> 8);
		}
		regs_.sr.rqm = false;
		return static_cast<uint8_t>(regs_.dr);
	}

	void dataWrite(uint8_t value) {
		if (!regs_.sr.drc) {
			if (!regs_.sr.drs) {
				regs_.sr.drs = true;
				regs_.dr = static_cast<uint16_t>((regs_.dr & 0xFF00U) | value);
				return;
			}
			regs_.sr.rqm = false;
			regs_.sr.drs = false;
			regs_.dr = static_cast<uint16_t>((value << 8) | (regs_.dr & 0x00FFU));
			return;
		}
		regs_.sr.rqm = false;
		regs_.dr = static_cast<uint16_t>((regs_.dr & 0xFF00U) | value);
	}

	size_t readSamples(int16_t *output, size_t capacity) {
		const size_t count = std::min(capacity, samples_.size());
		for (size_t index = 0; index < count; ++index) {
			output[index] = samples_.front();
			samples_.pop_front();
		}
		return count;
	}

	uint16_t pc() const { return regs_.pc; }
	bool p0() const { return regs_.sr.p0; }
	bool p1() const { return regs_.sr.p1; }
	size_t readP0Events(uint8_t *output, size_t capacity) {
		const size_t count = std::min(capacity, p0Events_.size());
		for (size_t index = 0; index < count; ++index) {
			output[index] = p0Events_.front();
			p0Events_.pop_front();
		}
		return count;
	}
	uint64_t hostReads() const { return hostReads_; }
	uint64_t ramWrites() const { return ramWrites_; }
	uint64_t ramChanges() const { return ramChanges_; }
	uint16_t lastOutputSource() const { return lastOutputSource_; }
	uint16_t lastOutputPointer() const { return lastOutputPointer_; }

private:
	void step() {
		// Enter the firmware's audio ISR only at its safe main-loop boundary.
		// An asynchronous timer interrupt races its synthesis state and corrupts
		// sibilants even though the nominal output rate remains correct.
		if (!servicingAudio_ && irqFiring_ == 0 && regs_.pc == 0x018 && !audioTriggeredAtBoundary_) {
			irqFiring_ = 1;
			regs_.sr.ei = false;
			servicingAudio_ = true;
			audioTriggeredAtBoundary_ = true;
		}
		uint32_t opcode = 0;
		if (irqFiring_ == 0) {
			opcode = program_[regs_.pc & 0x1FFU];
			regs_.pc = static_cast<uint16_t>((regs_.pc + 1) & 0x1FFU);
		} else if (irqFiring_ == 1) {
			opcode = 0;
			irqFiring_ = 2;
		} else {
			opcode = 0xA80400;
			irqFiring_ = 0;
		}

		switch (opcode >> 22) {
		case 0: execOp(opcode); break;
		case 1: execRt(opcode); break;
		case 2: execJp(opcode); break;
		case 3: execLd(opcode); break;
		default: break;
		}
		const int32_t result = static_cast<int32_t>(regs_.k) * regs_.l;
		regs_.m = static_cast<int16_t>(result >> 15);
		regs_.n = static_cast<int16_t>(result << 1);
		if (!servicingAudio_ && regs_.pc != 0x018)
			audioTriggeredAtBoundary_ = false;
	}

	uint16_t source(unsigned source) {
		switch (source) {
		case 0: return regs_.trb;
		case 1: return static_cast<uint16_t>(regs_.a);
		case 2: return static_cast<uint16_t>(regs_.b);
		case 3: return regs_.tr;
		case 4: return regs_.dp;
		case 5: return regs_.rp & 0x1FFU;
		case 6: return dataRom_[regs_.rp & 0x1FFU];
		case 7: return static_cast<uint16_t>(0x8000U - regs_.flaga.s1);
		case 8: ++hostReads_; regs_.sr.rqm = true; return regs_.dr;
		case 9: ++hostReads_; return regs_.dr;
		case 10: return regs_.sr.value();
		case 11: return regs_.si;
		case 12: return reverseWord(regs_.si);
		case 13: return static_cast<uint16_t>(regs_.k);
		case 14: return static_cast<uint16_t>(regs_.l);
		default: return dataRam_[regs_.dp & 0x7FU];
		}
	}

	static uint16_t reverseWord(uint16_t value) {
		uint16_t result = 0;
		for (unsigned bit = 0; bit < 16; ++bit) {
			result = static_cast<uint16_t>((result << 1) | ((value >> bit) & 1U));
		}
		return result;
	}

	void execOp(uint32_t opcode) {
		const unsigned pselect = (opcode >> 20) & 3U;
		const unsigned alu = (opcode >> 16) & 15U;
		const unsigned asl = (opcode >> 15) & 1U;
		const unsigned dpl = (opcode >> 13) & 3U;
		const unsigned dphm = (opcode >> 9) & 15U;
		const bool decrementRp = ((opcode >> 8) & 1U) != 0;
		const unsigned src = (opcode >> 4) & 15U;
		const unsigned dst = opcode & 15U;
		regs_.idb = source(src);

		if (alu != 0) {
			uint16_t p = 0;
			switch (pselect) {
			case 0: p = dataRam_[regs_.dp & 0x7FU]; break;
			case 1: p = regs_.idb; break;
			case 2: p = static_cast<uint16_t>(regs_.m); break;
			default: p = static_cast<uint16_t>(regs_.n); break;
			}
			const uint16_t q = static_cast<uint16_t>(asl ? regs_.b : regs_.a);
			Flag flag = asl ? regs_.flagb : regs_.flaga;
			const bool carry = asl ? regs_.flaga.c : regs_.flagb.c;
			uint16_t result = 0;
			switch (alu) {
			case 1: result = q | p; break;
			case 2: result = q & p; break;
			case 3: result = q ^ p; break;
			case 4: result = static_cast<uint16_t>(q - p); break;
			case 5: result = static_cast<uint16_t>(q + p); break;
			case 6: result = static_cast<uint16_t>(q - p - carry); break;
			case 7: result = static_cast<uint16_t>(q + p + carry); break;
			case 8: p = 1; result = static_cast<uint16_t>(q - 1); break;
			case 9: p = 1; result = static_cast<uint16_t>(q + 1); break;
			case 10: result = static_cast<uint16_t>(~q); break;
			case 11: result = static_cast<uint16_t>((q >> 1) | (q & 0x8000U)); break;
			case 12: result = static_cast<uint16_t>((q << 1) | (carry ? 1 : 0)); break;
			case 13: result = static_cast<uint16_t>((q << 2) | 3U); break;
			case 14: result = static_cast<uint16_t>((q << 4) | 15U); break;
			default: result = byteSwap16(q); break;
			}
			flag.s0 = (result & 0x8000U) != 0;
			flag.z = result == 0;
			if (!flag.ov1) flag.s1 = flag.s0;
			if (alu == 4 || alu == 5 || alu == 6 || alu == 7 || alu == 8 || alu == 9) {
				if ((alu & 1U) != 0) {
					flag.ov0 = ((q ^ result) & ~(q ^ p) & 0x8000U) != 0;
					flag.c = result < q;
				} else {
					flag.ov0 = ((q ^ result) & (q ^ p) & 0x8000U) != 0;
					flag.c = result > q;
				}
				flag.ov1 = (flag.ov0 && flag.ov1) ? (flag.s1 == flag.s0) : (flag.ov0 || flag.ov1);
			} else if (alu == 11) {
				flag.c = (q & 1U) != 0;
				flag.ov0 = flag.ov1 = false;
			} else if (alu == 12) {
				flag.c = (q >> 15) != 0;
				flag.ov0 = flag.ov1 = false;
			} else {
				flag.c = false;
				flag.ov0 = flag.ov1 = false;
			}
			if (asl) {
				regs_.b = static_cast<int16_t>(result);
				regs_.flagb = flag;
			} else {
				regs_.a = static_cast<int16_t>(result);
				regs_.flaga = flag;
			}
		}

		execLd((static_cast<uint32_t>(regs_.idb) << 6) | dst);
		if (dst != 4) {
			switch (dpl) {
			case 1: regs_.dp = static_cast<uint16_t>((regs_.dp & 0xF0U) + ((regs_.dp + 1) & 0x0FU)); break;
			case 2: regs_.dp = static_cast<uint16_t>((regs_.dp & 0xF0U) + ((regs_.dp - 1) & 0x0FU)); break;
			case 3: regs_.dp = static_cast<uint16_t>(regs_.dp & 0xF0U); break;
			default: break;
			}
			regs_.dp ^= static_cast<uint16_t>(dphm << 4);
			regs_.dp &= 0x7FU;
		}
		if (decrementRp && dst != 5)
			regs_.rp = static_cast<uint16_t>((regs_.rp - 1) & 0x1FFU);
	}

	void execRt(uint32_t opcode) {
		execOp(opcode);
		regs_.sp = static_cast<uint8_t>((regs_.sp - 1) & 0x03U);
		regs_.pc = regs_.stack[regs_.sp];
		if (servicingAudio_)
			servicingAudio_ = false;
	}

	void execJp(uint32_t opcode) {
		const uint16_t branch = static_cast<uint16_t>((opcode >> 13) & 0x1FFU);
		const uint16_t next = static_cast<uint16_t>((opcode >> 2) & 0x7FFU);
		const uint16_t shortTarget = static_cast<uint16_t>(next & 0x1FFU);
		const uint16_t longTarget = shortTarget;
		auto jump = [&](bool condition) { if (condition) regs_.pc = shortTarget; };
		switch (branch) {
		case 0x000: regs_.pc = regs_.so; return;
		case 0x080: jump(!regs_.flaga.c); return; case 0x082: jump(regs_.flaga.c); return;
		case 0x084: jump(!regs_.flagb.c); return; case 0x086: jump(regs_.flagb.c); return;
		case 0x088: jump(!regs_.flaga.z); return; case 0x08A: jump(regs_.flaga.z); return;
		case 0x08C: jump(!regs_.flagb.z); return; case 0x08E: jump(regs_.flagb.z); return;
		case 0x090: jump(!regs_.flaga.ov0); return; case 0x092: jump(regs_.flaga.ov0); return;
		case 0x094: jump(!regs_.flagb.ov0); return; case 0x096: jump(regs_.flagb.ov0); return;
		case 0x098: jump(!regs_.flaga.ov1); return; case 0x09A: jump(regs_.flaga.ov1); return;
		case 0x09C: jump(!regs_.flagb.ov1); return; case 0x09E: jump(regs_.flagb.ov1); return;
		case 0x0A0: jump(!regs_.flaga.s0); return; case 0x0A2: jump(regs_.flaga.s0); return;
		case 0x0A4: jump(!regs_.flagb.s0); return; case 0x0A6: jump(regs_.flagb.s0); return;
		case 0x0A8: jump(!regs_.flaga.s1); return; case 0x0AA: jump(regs_.flaga.s1); return;
		case 0x0AC: jump(!regs_.flagb.s1); return; case 0x0AE: jump(regs_.flagb.s1); return;
		case 0x0B0: jump((regs_.dp & 0x0FU) == 0); return;
		case 0x0B1: jump((regs_.dp & 0x0FU) != 0); return;
		case 0x0B2: jump((regs_.dp & 0x0FU) == 0x0F); return;
		case 0x0B3: jump((regs_.dp & 0x0FU) != 0x0F); return;
		case 0x0B4: jump(!regs_.siack); return; case 0x0B6: jump(regs_.siack); return;
		case 0x0B8: jump(!regs_.soack); return; case 0x0BA: jump(regs_.soack); return;
		case 0x0BC: jump(!regs_.sr.rqm); return; case 0x0BE: jump(regs_.sr.rqm); return;
		case 0x100: regs_.pc = longTarget; return;
		case 0x101: regs_.pc = longTarget; return;
		case 0x140:
			regs_.stack[regs_.sp] = regs_.pc; regs_.sp = static_cast<uint8_t>((regs_.sp + 1) & 0x03U);
			regs_.pc = longTarget; return;
		case 0x141:
			regs_.stack[regs_.sp] = regs_.pc; regs_.sp = static_cast<uint8_t>((regs_.sp + 1) & 0x03U);
			regs_.pc = longTarget; return;
		default: return;
		}
	}

	void execLd(uint32_t opcode) {
		const uint16_t immediate = static_cast<uint16_t>(opcode >> 6);
		const unsigned destination = opcode & 15U;
		regs_.idb = immediate;
		switch (destination) {
		case 0: break;
		case 1: regs_.a = static_cast<int16_t>(immediate); break;
		case 2: regs_.b = static_cast<int16_t>(immediate); break;
		case 3: regs_.tr = immediate; break;
		case 4: regs_.dp = immediate & 0x7FU; break;
		case 5: regs_.rp = immediate & 0x1FFU; break;
		case 6: regs_.dr = immediate; regs_.sr.rqm = true; break;
		case 7: {
			const uint16_t current = regs_.sr.value();
			const bool oldP0 = regs_.sr.p0;
			regs_.sr.assign(static_cast<uint16_t>((current & 0x907CU) | (immediate & ~0x907CU)));
			if (regs_.sr.p0 != oldP0)
				p0Events_.push_back(regs_.sr.p0 ? 1 : 0);
			break;
		}
		case 8: lastOutputSource_ = immediate; lastOutputPointer_ = regs_.dp; regs_.so = reverseWord(immediate); captureSerialOutput(); break;
		case 9: lastOutputSource_ = immediate; lastOutputPointer_ = regs_.dp; regs_.so = immediate; captureSerialOutput(); break;
		case 10: regs_.k = static_cast<int16_t>(immediate); break;
		case 11: regs_.k = static_cast<int16_t>(immediate); regs_.l = static_cast<int16_t>(dataRom_[regs_.rp & 0x1FFU]); break;
		case 12: regs_.l = static_cast<int16_t>(immediate); regs_.k = static_cast<int16_t>(dataRam_[(regs_.dp & 0x3FU) | 0x40U]); break;
		case 13: regs_.l = static_cast<int16_t>(immediate); break;
		case 14: regs_.trb = immediate; break;
		default: {
			const size_t address = regs_.dp & 0x7FU;
			++ramWrites_;
			if (dataRam_[address] != immediate)
				++ramChanges_;
			dataRam_[address] = immediate;
			break;
		}
		}
	}

	void captureSerialOutput() {
		samples_.push_back(static_cast<int16_t>(regs_.so));
	}

	Registers regs_{};
	std::array<uint32_t, 512> program_{};
	std::array<uint16_t, 512> dataRom_{};
	std::array<uint16_t, 128> dataRam_{};
	std::deque<int16_t> samples_{};
	std::deque<uint8_t> p0Events_{};
	bool irq_ = false;
	int irqFiring_ = 0;
	bool resetAsserted_ = true;
	bool servicingAudio_ = false;
	bool audioTriggeredAtBoundary_ = false;
	uint64_t hostReads_ = 0;
	uint64_t ramWrites_ = 0;
	uint64_t ramChanges_ = 0;
	uint16_t lastOutputSource_ = 0;
	uint16_t lastOutputPointer_ = 0;
};

} // namespace

struct prose_dsp { Dsp implementation; };

extern "C" {

prose_dsp *prose_dsp_create(const uint8_t *program, size_t programSize, const uint8_t *data, size_t dataSize) {
	auto *result = new (std::nothrow) prose_dsp;
	if (!result || !result->implementation.load(program, programSize, data, dataSize)) {
		delete result;
		return nullptr;
	}
	return result;
}

void prose_dsp_destroy(prose_dsp *dsp) { delete dsp; }
void prose_dsp_reset(prose_dsp *dsp) { if (dsp) dsp->implementation.coldReset(); }
void prose_dsp_set_reset(prose_dsp *dsp, int asserted) { if (dsp) dsp->implementation.resetLine(asserted != 0); }
void prose_dsp_set_interrupt(prose_dsp *dsp, int asserted) { if (dsp) dsp->implementation.interruptLine(asserted != 0); }
void prose_dsp_run(prose_dsp *dsp, uint32_t instructions) { if (dsp) dsp->implementation.run(instructions); }
uint8_t prose_dsp_status_read(prose_dsp *dsp) { return dsp ? dsp->implementation.statusRead() : 0; }
uint8_t prose_dsp_data_read(prose_dsp *dsp) { return dsp ? dsp->implementation.dataRead() : 0; }
void prose_dsp_data_write(prose_dsp *dsp, uint8_t value) { if (dsp) dsp->implementation.dataWrite(value); }
size_t prose_dsp_read_samples(prose_dsp *dsp, int16_t *output, size_t capacity) {
	return (dsp && output) ? dsp->implementation.readSamples(output, capacity) : 0;
}
uint16_t prose_dsp_program_counter(prose_dsp *dsp) { return dsp ? dsp->implementation.pc() : 0; }
int prose_dsp_p0(prose_dsp *dsp) { return dsp && dsp->implementation.p0(); }
int prose_dsp_p1(prose_dsp *dsp) { return dsp && dsp->implementation.p1(); }
size_t prose_dsp_read_p0_events(prose_dsp *dsp, uint8_t *output, size_t capacity) {
	return (dsp && output) ? dsp->implementation.readP0Events(output, capacity) : 0;
}
uint64_t prose_dsp_host_reads(prose_dsp *dsp) { return dsp ? dsp->implementation.hostReads() : 0; }
uint64_t prose_dsp_ram_writes(prose_dsp *dsp) { return dsp ? dsp->implementation.ramWrites() : 0; }
uint64_t prose_dsp_ram_changes(prose_dsp *dsp) { return dsp ? dsp->implementation.ramChanges() : 0; }
uint16_t prose_dsp_last_output_source(prose_dsp *dsp) { return dsp ? dsp->implementation.lastOutputSource() : 0; }
uint16_t prose_dsp_last_output_pointer(prose_dsp *dsp) { return dsp ? dsp->implementation.lastOutputPointer() : 0; }

}
