// license:BSD-3-Clause
#pragma once

#include <cstdint>

constexpr uint32_t prose_protocol_magic = 0x4b325250; // PR2K
constexpr uint32_t prose_protocol_max_payload = 1U << 20;

enum class prose_message : uint32_t
{
	speak = 1,
	cancel = 2,
	quit = 3,
	ready = 101,
	audio = 102,
	done = 103,
	error = 104,
	cancelled = 105,
};

struct prose_message_header
{
	uint32_t magic;
	uint32_t type;
	uint32_t generation;
	uint32_t size;
};

static_assert(sizeof(prose_message_header) == 16);
