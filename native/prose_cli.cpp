// license:BSD-3-Clause
#include "prose_board.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>

namespace {

constexpr const char *VERSION = "1.1.0";
constexpr s64 DEFAULT_CYCLES = 120'000'000;

void print_usage(std::ostream &stream)
{
	stream <<
		"Prose 2000 command-line renderer " << VERSION << "\n\n"
		"Usage:\n"
		"  prose_cli ROM_DIRECTORY [CYCLES] [TEXT|__TEST_MODE__] [WAV_PATH]\n\n"
		"The ROM directory must contain the standard six-file prose2k ROM set.\n"
		"CYCLES defaults to 120000000. Text is terminated automatically.\n"
		"The optional WAV output is mono, 16-bit PCM at 10000 Hz.\n";
}

static uint16_t reverse_bits(uint16_t value)
{
	uint16_t result = 0;
	for (unsigned bit = 0; bit < 16; ++bit)
		result |= uint16_t(((value >> bit) & 1u) << (15 - bit));
	return result;
}

static bool write_wav(const std::filesystem::path &path, const std::vector<int16_t> &raw_samples)
{
	std::ofstream stream(path, std::ios::binary);
	if (!stream)
		return false;
	const uint32_t sample_rate = 10'000;
	const uint32_t data_size = uint32_t(raw_samples.size() * sizeof(int16_t));
	const auto write_u16 = [&stream](uint16_t value) {
		stream.put(char(value));
		stream.put(char(value >> 8));
	};
	const auto write_u32 = [&stream](uint32_t value) {
		for (unsigned shift = 0; shift < 32; shift += 8)
			stream.put(char(value >> shift));
	};
	stream.write("RIFF", 4);
	write_u32(36 + data_size);
	stream.write("WAVEfmt ", 8);
	write_u32(16);
	write_u16(1);
	write_u16(1);
	write_u32(sample_rate);
	write_u32(sample_rate * sizeof(int16_t));
	write_u16(sizeof(int16_t));
	write_u16(16);
	stream.write("data", 4);
	write_u32(data_size);
	for (int16_t raw : raw_samples)
	{
		const uint16_t dac = (reverse_bits(uint16_t(raw)) >> 1) & 0x0fff;
		// The AM6012 uses offset binary; the firmware's neutral level is
		// 0x7f0. A gain of 15 preserves the complete 12-bit range after
		// removing that offset without overflowing signed 16-bit PCM.
		const int16_t pcm = static_cast<int16_t>((int32_t(dac) - 0x7f0) * 15);
		write_u16(uint16_t(pcm));
	}
	return bool(stream);
}

static std::vector<u8> read_file(const std::filesystem::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // anonymous namespace

int main(int argc, char **argv)
{
	if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h"))
	{
		print_usage(std::cout);
		return 0;
	}
	if (argc == 2 && std::string(argv[1]) == "--version")
	{
		std::cout << VERSION << '\n';
		return 0;
	}
	if (argc < 2)
	{
		print_usage(std::cerr);
		return 2;
	}
	const std::filesystem::path directory(argv[1]);
	const s64 cycles = argc > 2 ? std::stoll(argv[2]) : DEFAULT_CYCLES;
	std::array<std::vector<u8>, 4> cpu_roms = {
		read_file(directory / "v3.4.1__2000__2.u22"),
		read_file(directory / "v3.4.1__2000__3.u45"),
		read_file(directory / "v3.4.1__2000__0.u21"),
		read_file(directory / "v3.4.1__2000__1.u44")};
	prose_board board;
	if (!board.load_cpu_roms(cpu_roms)
		|| !board.load_dsp_roms(
			read_file(directory / "v3.12__8-9-88__dsp_prog.u29"),
			read_file(directory / "v3.12__8-9-88__dsp_data.u29")))
	{
		std::cerr << "invalid or incomplete ROM set\n";
		return 1;
	}
	if (argc > 3 && std::string(argv[3]) == "__TEST_MODE__")
		board.set_switches(0xbc);
	board.reset();
	s64 elapsed = board.run_cycles(std::min<s64>(cycles, 30'000'000));
	if (argc > 3 && std::string(argv[3]) != "__TEST_MODE__")
	{
		std::string text(argv[3]);
		if (text.empty() || (text.back() != '\r' && text.back() != '\n'))
			text.push_back('\r');
		// Model 9600 baud, 8N1: approximately 8,333 CPU clocks per byte.
		for (const char value : text)
		{
			board.queue_text(std::string(1, value));
			const s64 serial_cycles = std::min<s64>(8'333, cycles - elapsed);
			if (serial_cycles <= 0)
				break;
			elapsed += board.run_cycles(serial_cycles);
		}
		if (cycles > elapsed)
			elapsed += board.run_cycles(cycles - elapsed);
	}
	else if (cycles > elapsed)
	{
		elapsed += board.run_cycles(cycles - elapsed);
	}
	std::vector<int16_t> samples;
	board.pull_dsp_samples(samples, 1'000'000);
	if (argc > 4 && !write_wav(argv[4], samples))
	{
		std::cerr << "could not write WAV file\n";
		return 1;
	}
	if (!samples.empty())
	{
		const auto [minimum, maximum] = std::minmax_element(samples.begin(), samples.end());
		unsigned low_bits = 0;
		u16 source_minimum = 0xffff;
		u16 source_maximum = 0;
		std::array<size_t, 4096> histogram{};
		for (int16_t sample : samples)
		{
			low_bits |= unsigned(uint16_t(sample) & 0x000f);
			u16 source = 0;
			u16 bits = u16(sample);
			for (unsigned bit = 0; bit < 16; ++bit)
				source |= u16(((bits >> bit) & 1u) << (15 - bit));
			source_minimum = std::min(source_minimum, source);
			source_maximum = std::max(source_maximum, source);
			++histogram[source & 0x0fff];
		}
		const auto mode = std::max_element(histogram.begin(), histogram.end());
		std::cout << "sample_min=" << *minimum << " sample_max=" << *maximum
			<< " low_nibble_or=" << low_bits << " source_min=" << source_minimum
			<< " source_max=" << source_maximum << " source_mode="
			<< std::distance(histogram.begin(), mode) << ':' << *mode << '\n';
	}
	std::cout << "cycles=" << elapsed << " pc=" << std::hex << std::setw(5)
		<< std::setfill('0') << board.physical_pc() << std::dec
		<< " parameter_writes=" << board.parameter_writes()
		<< " parameter=" << std::hex << unsigned(board.parameter()) << std::dec
		<< " dsp_writes=" << board.dsp_data_writes()
		<< " dsp_samples=" << samples.size()
		<< " dsp_pc=" << std::hex << board.dsp_pc()
		<< " dsp_status=" << unsigned(board.dsp_status()) << std::dec
		<< " dsp_host_reads=" << board.dsp_host_reads()
		<< " dsp_ram_writes=" << board.dsp_ram_writes()
		<< " dsp_ram_changes=" << board.dsp_ram_changes()
		<< " dsp_output_source=" << std::hex << board.dsp_last_output_source()
		<< " dsp_output_dp=" << board.dsp_last_output_pointer() << std::dec
		<< " uart_output=" << board.uart_output().size()
		<< " uart_reads=" << board.uart_data_reads()
		<< " uart_pending=" << board.uart_pending() << "\nrecent_pc=";
	for (u32 pc : board.recent_pcs())
		std::cout << ' ' << std::hex << std::setw(5) << std::setfill('0') << pc;
	std::cout << std::dec << '\n';
	const u32 queue_base = ((u32(board.data_segment()) << 4) + 0xee0c) & 0xfffff;
	std::cout << "ds=" << std::hex << board.data_segment() << " queue="
		<< unsigned(board.peek(queue_base)) << ':' << unsigned(board.peek(queue_base + 2))
		<< std::dec << '\n';
	for (u32 vector : {0x20u, 0x21u})
	{
		const u32 base = vector * 4;
		const u16 ip = u16(board.peek(base) | (u16(board.peek(base + 1)) << 8));
		const u16 cs = u16(board.peek(base + 2) | (u16(board.peek(base + 3)) << 8));
		std::cout << "vector" << std::hex << vector << '=' << cs << ':' << ip << '\n';
	}
	std::cout << "dsp_write_values=";
	const auto &writes = board.dsp_write_values();
	for (size_t index = 0; index < std::min<size_t>(writes.size(), 128); ++index)
		std::cout << ' ' << std::hex << unsigned(writes[index]);
	if (writes.size() > 128)
		std::cout << " ...";
	std::cout << std::dec << '\n';
	std::cout << "pic_writes=";
	for (const auto &[port, value] : board.pic_writes())
		std::cout << ' ' << std::hex << unsigned(port) << ':' << unsigned(value);
	std::cout << std::dec << '\n';
	std::cout << "uart_output=";
	for (u8 value : board.uart_output())
		std::cout << ' ' << std::hex << unsigned(value);
	std::cout << std::dec << '\n';
	return 0;
}
