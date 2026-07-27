// license:BSD-3-Clause
#include "prose_engine.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace {
constexpr s64 boot_cycles = 30'000'000;
constexpr s64 serial_byte_cycles = 8'333;
constexpr s64 run_slice_cycles = 8'000;
constexpr size_t output_chunk_samples = 1'000;
constexpr size_t completion_silence_samples = 10'000;
constexpr s64 completion_producer_quiet_cycles = prose_board::CPU_HZ * 3;
constexpr size_t audio_threshold = 2;

uint16_t reverse_bits(uint16_t value)
{
	uint16_t result = 0;
	for (unsigned bit = 0; bit < 16; ++bit)
		result |= uint16_t(((value >> bit) & 1U) << (15 - bit));
	return result;
}

}

std::vector<u8> prose_engine::read_file(const std::filesystem::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool prose_engine::load(const std::filesystem::path &directory, std::string &error)
{
	m_cpu_roms = {
		read_file(directory / "v3.4.1__2000__2.u22"),
		read_file(directory / "v3.4.1__2000__3.u45"),
		read_file(directory / "v3.4.1__2000__0.u21"),
		read_file(directory / "v3.4.1__2000__1.u44")};
	m_dsp_program = read_file(directory / "v3.12__8-9-88__dsp_prog.u29");
	m_dsp_data = read_file(directory / "v3.12__8-9-88__dsp_data.u29");
	return reset(error);
}

bool prose_engine::reset(std::string &error)
{
	auto board = std::make_unique<prose_board>();
	if (!board->load_cpu_roms(m_cpu_roms)
		|| !board->load_dsp_roms(m_dsp_program, m_dsp_data))
	{
		error = "The Prose firmware set is incomplete or invalid.";
		return false;
	}
	board->reset();
	if (board->run_cycles(boot_cycles) < boot_cycles)
	{
		error = "The Prose firmware did not finish its startup sequence.";
		return false;
	}
	std::vector<int16_t> discarded;
	while (board->pull_dsp_samples(discarded, 16'384) != 0)
		discarded.clear();
	m_board = std::move(board);
	error.clear();
	return true;
}

int16_t prose_engine::convert_sample(int16_t raw)
{
	// The uPD7720 produces a 13-bit sample. The physical board discarded its
	// least-significant bit for the 12-bit AM6012 DAC, but retaining it here
	// avoids recreating that hardware quantisation in 16-bit PCM.
	const int32_t sample = reverse_bits(uint16_t(raw)) & 0x1fff;
	return static_cast<int16_t>((sample - 0x0fe0) * 15 / 2);
}

bool prose_engine::flush_audio(
	std::vector<int16_t> &pending,
	const audio_callback &on_audio)
{
	if (pending.empty())
		return true;
	if (!on_audio(pending.data(), pending.size()))
		return false;
	pending.clear();
	return true;
}

bool prose_engine::run_and_collect(
	s64 cycles,
	std::vector<int16_t> &pending,
	const audio_callback &on_audio,
	const cancel_callback &is_cancelled,
	size_t &silent_samples,
	bool &heard_audio,
	bool &pending_has_audio)
{
	if (is_cancelled())
		return false;
	m_board->run_cycles(cycles);
	std::vector<int16_t> raw;
	for (;;)
	{
		raw.clear();
		if (m_board->pull_dsp_samples(raw, 4'096) == 0)
			break;
		for (const int16_t value : raw)
		{
			const int16_t pcm = convert_sample(value);
			pending.push_back(pcm);
			if (std::abs(int(pcm)) > int(audio_threshold))
			{
				heard_audio = true;
				pending_has_audio = true;
				silent_samples = 0;
			}
			else if (heard_audio)
			{
				++silent_samples;
			}
			if (pending.size() >= output_chunk_samples && pending_has_audio)
			{
				if (!flush_audio(pending, on_audio))
					return false;
				pending_has_audio = false;
			}
		}
	}
	return !is_cancelled();
}

bool prose_engine::synthesize(
	const std::string &input,
	const audio_callback &on_audio,
	const cancel_callback &is_cancelled,
	std::string &error)
{
	if (!m_board)
	{
		error = "The Prose engine is not initialized.";
		return false;
	}
	std::string text = input;
	// The firmware buffers fragments until it receives sentence punctuation.
	// NVDA commonly submits labels, characters and other unpunctuated text as
	// complete utterances, so terminate those fragments before sending CR.
	const auto last_non_space = text.find_last_not_of(" \t\r\n");
	if (last_non_space != std::string::npos
		&& text[last_non_space] != '.'
		&& text[last_non_space] != '?'
		&& text[last_non_space] != '!')
		// Two stops also disambiguate single letters, which the firmware treats
		// as an unfinished abbreviation when followed by only one stop.
		text.insert(last_non_space + 1, "..");
	if (text.empty() || (text.back() != '\r' && text.back() != '\n'))
		text.push_back('\r');

	std::vector<int16_t> pending;
	pending.reserve(output_chunk_samples);
	size_t silent_samples = 0;
	bool heard_audio = false;
	bool pending_has_audio = false;
	u64 dsp_writes = m_board->dsp_data_writes();
	s64 producer_quiet_cycles = 0;
	const s64 maximum_cycles = prose_board::CPU_HZ
		* std::max<s64>(15, static_cast<s64>(text.size() / 2));
	s64 elapsed = 0;

	for (const unsigned char value : text)
	{
		while (m_board->serial_input_paused())
		{
			if (elapsed >= maximum_cycles)
			{
				error = "The Prose firmware exceeded the utterance time limit.";
				return false;
			}
			if (!run_and_collect(run_slice_cycles, pending, on_audio, is_cancelled,
				silent_samples, heard_audio, pending_has_audio))
				return false;
			elapsed += run_slice_cycles;
		}
		m_board->queue_text(std::string(1, char(value)));
		if (!run_and_collect(serial_byte_cycles, pending, on_audio, is_cancelled,
			silent_samples, heard_audio, pending_has_audio))
			return false;
		elapsed += serial_byte_cycles;
	}

	while (elapsed < maximum_cycles)
	{
		if (!run_and_collect(run_slice_cycles, pending, on_audio, is_cancelled,
			silent_samples, heard_audio, pending_has_audio))
			return false;
		elapsed += run_slice_cycles;
		const u64 current_dsp_writes = m_board->dsp_data_writes();
		if (current_dsp_writes != dsp_writes)
		{
			dsp_writes = current_dsp_writes;
			producer_quiet_cycles = 0;
		}
		else
		{
			producer_quiet_cycles += run_slice_cycles;
		}
		if (producer_quiet_cycles >= completion_producer_quiet_cycles
			&& (!heard_audio || silent_samples >= completion_silence_samples)
			&& !m_board->talking()
			&& (m_board->parameter() & 0x0fU) == 0x0bU
			&& m_board->uart_pending() == 0)
			break;
	}
	if (!heard_audio)
	{
		// Punctuation-only and other control utterances can legitimately produce
		// no waveform. The firmware still completed the request successfully.
		error.clear();
		return true;
	}
	if (elapsed >= maximum_cycles)
	{
		error = "The Prose firmware exceeded the utterance time limit.";
		return false;
	}
	// Pending samples containing no speech are the completion guard, not part of
	// the utterance. Do not make NVDA play a second of trailing silence.
	if (pending_has_audio && !flush_audio(pending, on_audio))
		return false;
	error.clear();
	return true;
}
