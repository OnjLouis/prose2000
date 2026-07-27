// license:BSD-3-Clause
#include "prose_board.h"

#include <algorithm>
#include <cstring>

constexpr u32 RAM_MIRROR = 0x34000;
constexpr u32 UART_MIRROR = 0x341fc;
constexpr u32 PARAM_MIRROR = 0x341fe;
constexpr u32 DSP_MIRROR = 0x341fc;

class prose_program_space final : public address_space
{
public:
	explicit prose_program_space(prose_board &board) : m_board(board) { }
	u8 read_byte(offs_t address) override { return m_board.read_memory(u32(address)); }
	void write_byte(offs_t address, u8 value) override { m_board.write_memory(u32(address), value); }
private:
	prose_board &m_board;
};

class prose_io_space final : public address_space
{
public:
	u8 read_byte(offs_t) override { return 0xff; }
	void write_byte(offs_t, u8) override { }
};

prose_board::prose_board()
{
	m_cpu_rom.fill(0xff);
	m_cpu = std::make_unique<prose_cpu>(m_config, "prose_cpu", nullptr, CPU_HZ);
	m_program_space = std::make_unique<prose_program_space>(*this);
	m_io_space = std::make_unique<prose_io_space>();
	m_cpu->shim_set_space(AS_PROGRAM, m_program_space.get());
	m_cpu->shim_set_space(AS_IO, m_io_space.get());
	m_cpu->set_irq_acknowledge_callback(*this, &prose_board::pic_ack);
	m_machine.set_cpu(m_cpu.get());
	running_machine::set_cycles_per_second(double(CPU_HZ));
	m_cpu->shim_start();
}

prose_board::~prose_board()
{
	if (m_dsp)
		prose_dsp_destroy(m_dsp);
}

bool prose_board::load_cpu_roms(const std::array<std::vector<u8>, 4> &roms)
{
	for (const auto &rom : roms)
		if (rom.size() != 0x10000)
			return false;
	for (size_t i = 0; i < 0x10000; ++i)
	{
		m_cpu_rom[i * 2] = roms[0][i];
		m_cpu_rom[i * 2 + 1] = roms[1][i];
		m_cpu_rom[0x20000 + i * 2] = roms[2][i];
		m_cpu_rom[0x20000 + i * 2 + 1] = roms[3][i];
	}
	return true;
}

bool prose_board::load_dsp_roms(const std::vector<u8> &program, const std::vector<u8> &data)
{
	if (m_dsp)
		prose_dsp_destroy(m_dsp);
	m_dsp = prose_dsp_create(program.data(), program.size(), data.data(), data.size());
	return m_dsp != nullptr;
}

void prose_board::reset()
{
	m_ram.fill(0);
	m_parameter = 0;
	m_dsp_data = 0;
	m_uart_mode = 0;
	m_uart_command = 0;
	m_pic_vector_base = 0x20;
	m_pic_mask = 0xff;
	m_pic_requests = 0;
	m_pic_in_service = 0;
	m_pic_init_step = 0;
	m_pic_needs_icw4 = false;
	m_pic_auto_eoi = false;
	m_pic_read_isr = false;
	m_uart_input.clear();
	m_uart_output.clear();
	m_pic_writes.clear();
	m_dsp_write_values.clear();
	m_recent_pcs.clear();
	m_parameter_writes = 0;
	m_dsp_data_writes = 0;
	m_uart_data_reads = 0;
	m_dsp_clock_remainder = 0;
	m_dsp_pic_line = false;
	m_serial_input_paused = false;
	m_cpu->shim_reset();
	if (m_dsp)
	{
		prose_dsp_reset(m_dsp);
		prose_dsp_set_reset(m_dsp, 1);
	}
}

s64 prose_board::run_cycles(s64 cycles)
{
	constexpr s64 slice = 256;
	s64 elapsed = 0;
	while (elapsed < cycles)
	{
		const s64 amount = std::min(slice, cycles - elapsed);
		const s64 consumed = m_machine.run_cycles(amount);
		run_dsp(consumed);
		elapsed += consumed;
		m_recent_pcs.push_back(physical_pc());
		if (m_recent_pcs.size() > 32)
			m_recent_pcs.pop_front();
		if (consumed <= 0)
			break;
	}
	return elapsed;
}

void prose_board::queue_text(const std::string &text)
{
	for (unsigned char value : text)
		m_uart_input.push_back(value);
	set_pic_request(1, !m_uart_input.empty());
}

void prose_board::set_pic_request(u8 line, bool asserted)
{
	if (asserted)
		m_pic_requests |= u8(1u << line);
	else
		m_pic_requests &= u8(~(1u << line));
	update_pic_line();
}

void prose_board::update_pic_line()
{
	u8 eligible = m_pic_requests & u8(~m_pic_mask);
	for (u8 line = 0; line < 8; ++line)
	{
		if (m_pic_in_service & (1u << line))
		{
			eligible &= u8((1u << line) - 1u);
			break;
		}
	}
	m_cpu->set_input_line(0, eligible ? ASSERT_LINE : CLEAR_LINE);
}

int prose_board::pic_ack(device_t &, int)
{
	u8 pending = m_pic_requests & u8(~m_pic_mask);
	for (u8 line = 0; line < 8; ++line)
	{
		if (m_pic_in_service & (1u << line))
		{
			pending &= u8((1u << line) - 1u);
			break;
		}
	}
	for (u8 line = 0; line < 8; ++line)
	{
		if (pending & (1u << line))
		{
			m_pic_requests &= u8(~(1u << line));
			if (!m_pic_auto_eoi)
				m_pic_in_service |= u8(1u << line);
			update_pic_line();
			return m_pic_vector_base + line;
		}
	}
	return m_pic_vector_base + 7;
}

void prose_board::write_pic(u8 port, u8 value)
{
	m_pic_writes.emplace_back(port, value);
	if (port == 0)
	{
		if (value & 0x10)
		{
			m_pic_init_step = 1;
			m_pic_needs_icw4 = (value & 0x01) != 0;
			m_pic_mask = 0;
			m_pic_requests = 0;
			m_pic_in_service = 0;
		}
		else if (value & 0x20)
		{
			if (value & 0x40)
				m_pic_in_service &= u8(~(1u << (value & 7u)));
			else
			{
				for (u8 line = 0; line < 8; ++line)
				{
					if (m_pic_in_service & (1u << line))
					{
						m_pic_in_service &= u8(~(1u << line));
						break;
					}
				}
			}
			update_pic_line();
		}
		else if ((value & 0x1a) == 0x0a)
			m_pic_read_isr = (value & 0x01) != 0;
		return;
	}
	if (m_pic_init_step == 1)
	{
		m_pic_vector_base = value & 0xf8;
		m_pic_init_step = m_pic_needs_icw4 ? 2 : 0;
	}
	else if (m_pic_init_step == 2)
	{
		m_pic_auto_eoi = (value & 0x02) != 0;
		m_pic_init_step = 0;
	}
	else
	{
		m_pic_mask = value;
		if (m_uart_command & 0x01)
			m_pic_requests |= 0x0c;
		update_pic_line();
	}
}

void prose_board::run_dsp(s64 cpu_cycles)
{
	if (!m_dsp || cpu_cycles <= 0)
		return;
	// The original uPD7720 takes two 8 MHz input clocks per 250 ns
	// instruction. Preserve a remainder because the 8086 often yields an odd
	// number of clock cycles between peripheral updates.
	const u64 dsp_clocks = u64(cpu_cycles) * DSP_HZ / CPU_HZ + m_dsp_clock_remainder;
	u32 remaining = u32(dsp_clocks / 2);
	m_dsp_clock_remainder = u32(dsp_clocks & 1);
	prose_dsp_run(m_dsp, remaining);
	consume_dsp_pic_events();
}

void prose_board::consume_dsp_pic_events()
{
	uint8_t events[64];
	for (;;)
	{
		const size_t count = prose_dsp_read_p0_events(m_dsp, events, std::size(events));
		for (size_t index = 0; index < count; ++index)
		{
			const bool state = (m_parameter & 0x01) && events[index];
			// The board presents the DSP request to the PIC as active-low.
			if (!state && m_dsp_pic_line)
			{
				m_pic_requests |= 0x01;
				update_pic_line();
			}
			m_dsp_pic_line = state;
		}
		if (count < std::size(events))
			break;
	}
}

bool prose_board::matches(u32 address, u32 start, u32 end, u32 mirror)
{
	const u32 normal = address & ~mirror;
	return normal >= start && normal <= end;
}

u32 prose_board::offset(u32 address, u32 start, u32 mirror)
{
	return (address & ~mirror) - start;
}

u8 prose_board::read_memory(u32 address)
{
	address &= 0xfffff;
	if (matches(address, 0x0000, 0x2fff, RAM_MIRROR))
		return m_ram[address & ~RAM_MIRROR];
	if (matches(address, 0x3000, 0x3003, UART_MIRROR))
	{
		if (offset(address, 0x3000, UART_MIRROR) < 2)
		{
			if (m_uart_input.empty())
				return 0;
			const u8 value = m_uart_input.front();
			m_uart_input.pop_front();
			++m_uart_data_reads;
			set_pic_request(1, !m_uart_input.empty());
			return value;
		}
		// TX ready, transmitter empty and DSR asserted.  The serial terminal
		// attached in the original system drives DSR high.
		return u8(0x85 | (m_uart_input.empty() ? 0 : 0x02));
	}
	if (matches(address, 0x3200, 0x3203, UART_MIRROR))
		return offset(address, 0x3200, UART_MIRROR) < 2
			? (m_pic_read_isr ? m_pic_in_service : m_pic_requests)
			: m_pic_mask;
	if (matches(address, 0x3400, 0x3400, PARAM_MIRROR))
		return m_switches;
	if (matches(address, 0x3600, 0x3600, DSP_MIRROR))
		return m_dsp ? prose_dsp_data_read(m_dsp) : m_dsp_data;
	if (matches(address, 0x3602, 0x3602, DSP_MIRROR))
		return m_dsp ? prose_dsp_status_read(m_dsp) : 0x80;
	if (address >= 0xc0000)
		return m_cpu_rom[address - 0xc0000];
	return 0xff;
}

void prose_board::write_memory(u32 address, u8 value)
{
	address &= 0xfffff;
	if (matches(address, 0x0000, 0x2fff, RAM_MIRROR))
	{
		m_ram[address & ~RAM_MIRROR] = value;
		return;
	}
	if (matches(address, 0x3000, 0x3003, UART_MIRROR))
	{
		if (offset(address, 0x3000, UART_MIRROR) < 2)
		{
			m_uart_output.push_back(value);
			if (value == 0x13)
				m_serial_input_paused = true;
			else if (value == 0x11)
				m_serial_input_paused = false;
			m_pic_requests |= 0x08;
			update_pic_line();
		}
		else if (!m_uart_mode)
			m_uart_mode = value;
		else
		{
			m_uart_command = value;
			if (value & 0x01)
			{
				m_pic_requests |= 0x0c;
				update_pic_line();
			}
		}
		return;
	}
	if (matches(address, 0x3401, 0x3401, PARAM_MIRROR))
	{
		m_parameter = value;
		++m_parameter_writes;
		if (m_dsp)
		{
			prose_dsp_set_reset(m_dsp, (value & 0x40) ? 0 : 1);
			consume_dsp_pic_events();
		}
		return;
	}
	if (matches(address, 0x3200, 0x3203, UART_MIRROR))
	{
		write_pic(offset(address, 0x3200, UART_MIRROR) < 2 ? 0 : 1, value);
		return;
	}
	if (matches(address, 0x3600, 0x3600, DSP_MIRROR))
	{
		m_dsp_data = value;
		++m_dsp_data_writes;
		m_dsp_write_values.push_back(value);
		if (m_dsp)
			prose_dsp_data_write(m_dsp, value);
	}
}

size_t prose_board::pull_dsp_samples(std::vector<int16_t> &output, size_t limit)
{
	if (!m_dsp || !limit)
		return 0;
	const size_t old_size = output.size();
	output.resize(old_size + limit);
	const size_t count = prose_dsp_read_samples(m_dsp, output.data() + old_size, limit);
	output.resize(old_size + count);
	return count;
}
