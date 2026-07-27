// license:BSD-3-Clause
#include "prose_engine.h"
#include "prose_protocol.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
struct request
{
	prose_message type;
	uint32_t generation;
	std::string payload;
};

std::mutex output_mutex;

bool write_message(prose_message type, uint32_t generation, const void *data, uint32_t size)
{
	std::lock_guard lock(output_mutex);
	const prose_message_header header{
		prose_protocol_magic, static_cast<uint32_t>(type), generation, size};
	std::cout.write(reinterpret_cast<const char *>(&header), sizeof(header));
	if (size != 0)
		std::cout.write(static_cast<const char *>(data), size);
	std::cout.flush();
	return bool(std::cout);
}

bool write_text(prose_message type, uint32_t generation, const std::string &text)
{
	return write_message(type, generation, text.data(), static_cast<uint32_t>(text.size()));
}

bool write_audio(uint32_t generation, const int16_t *samples, size_t count)
{
	// Keep transport frames comfortably below the protocol ceiling. This is
	// framing only: the NVDA driver joins them before handing the utterance to
	// WavePlayer, so no playback boundary is introduced.
	constexpr size_t samples_per_frame = 128 * 1024;
	while (count != 0)
	{
		const size_t frame_count = std::min(count, samples_per_frame);
		if (!write_message(
			prose_message::audio,
			generation,
			samples,
			static_cast<uint32_t>(frame_count * sizeof(int16_t))))
			return false;
		samples += frame_count;
		count -= frame_count;
	}
	return true;
}
}

int main(int argc, char **argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	if (argc != 2)
		return 2;

	prose_engine engine;
	std::string error;
	if (!engine.load(std::filesystem::path(argv[1]), error))
	{
		write_text(prose_message::error, 0, error);
		return 1;
	}
	write_message(prose_message::ready, 0, nullptr, 0);

	std::mutex queue_mutex;
	std::condition_variable queue_changed;
	std::queue<request> requests;
	std::atomic<uint32_t> cancelled_generation{UINT32_MAX};
	std::atomic<bool> quitting{false};

	std::thread reader([&] {
		while (!quitting.load())
		{
			prose_message_header header{};
			if (!std::cin.read(reinterpret_cast<char *>(&header), sizeof(header)))
				break;
			if (header.magic != prose_protocol_magic || header.size > prose_protocol_max_payload)
				break;
			std::string payload(header.size, '\0');
			if (header.size != 0 && !std::cin.read(payload.data(), header.size))
				break;
			const auto type = static_cast<prose_message>(header.type);
			if (type == prose_message::cancel)
			{
				cancelled_generation.store(header.generation);
				continue;
			}
			if (type == prose_message::quit)
			{
				quitting.store(true);
				queue_changed.notify_all();
				break;
			}
			if (type == prose_message::speak)
			{
				std::lock_guard lock(queue_mutex);
				requests.push({type, header.generation, std::move(payload)});
				queue_changed.notify_one();
			}
		}
		quitting.store(true);
		queue_changed.notify_all();
	});

	while (!quitting.load())
	{
		request current{};
		{
			std::unique_lock lock(queue_mutex);
			queue_changed.wait(lock, [&] { return quitting.load() || !requests.empty(); });
			if (quitting.load())
				break;
			current = std::move(requests.front());
			requests.pop();
		}
		const auto cancelled = [&] {
			return quitting.load() || cancelled_generation.load() == current.generation;
		};
		if (cancelled())
		{
			write_message(prose_message::cancelled, current.generation, nullptr, 0);
			continue;
		}
		const bool ok = engine.synthesize(
			current.payload,
			[&](const int16_t *samples, size_t count) {
				return !cancelled() && write_audio(current.generation, samples, count);
			},
			cancelled,
			error);
		if (cancelled())
		{
			std::string reset_error;
			if (!engine.reset(reset_error))
			{
				write_text(prose_message::error, current.generation, reset_error);
				quitting.store(true);
			}
			else
			{
				write_message(prose_message::cancelled, current.generation, nullptr, 0);
			}
		}
		else if (ok)
		{
			write_message(prose_message::done, current.generation, nullptr, 0);
		}
		else
		{
			const std::string synthesis_error = error;
			std::string reset_error;
			if (!engine.reset(reset_error))
			{
				write_text(prose_message::error, current.generation, reset_error);
				quitting.store(true);
			}
			else
			{
				write_text(prose_message::error, current.generation, synthesis_error);
			}
		}
	}

	quitting.store(true);
	if (reader.joinable())
	{
		// stdin closes when the NVDA-side process object is terminated. A normal
		// quit message also wakes and completes the reader before this join.
		reader.join();
	}
	return 0;
}
