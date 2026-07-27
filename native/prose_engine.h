// license:BSD-3-Clause
#pragma once

#include "prose_board.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class prose_engine
{
public:
	using audio_callback = std::function<bool(const int16_t *, size_t)>;
	using cancel_callback = std::function<bool()>;

	bool load(const std::filesystem::path &rom_directory, std::string &error);
	bool reset(std::string &error);
	bool synthesize(
		const std::string &text,
		const audio_callback &on_audio,
		const cancel_callback &is_cancelled,
		std::string &error);

	static constexpr uint32_t sample_rate = 10'000;

private:
	static std::vector<u8> read_file(const std::filesystem::path &path);
	static int16_t convert_sample(int16_t raw);
	bool run_and_collect(
		s64 cycles,
		std::vector<int16_t> &pending,
		const audio_callback &on_audio,
		const cancel_callback &is_cancelled,
		size_t &silent_samples,
		bool &heard_audio,
		bool &pending_has_audio);
	bool flush_audio(std::vector<int16_t> &pending, const audio_callback &on_audio);

	std::array<std::vector<u8>, 4> m_cpu_roms;
	std::vector<u8> m_dsp_program;
	std::vector<u8> m_dsp_data;
	std::unique_ptr<prose_board> m_board;
};
