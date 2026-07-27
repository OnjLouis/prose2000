// license:BSD-3-Clause
#pragma once

#include "emu.h"
#include "i86.h"
#include "prose_dsp.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class prose_board;

class prose_cpu final : public i8086_cpu_device
{
public:
	prose_cpu(const machine_config &config, const char *tag, device_t *owner, u32 clock)
		: i8086_cpu_device(config, tag, owner, clock) { }

	u32 physical_pc() const { return ((u32(m_sregs[CS]) << 4) + m_ip) & 0xfffff; }
	u16 segment(int index) const { return m_sregs[index]; }
};

class prose_board
{
public:
	static constexpr u32 CPU_HZ = 8'000'000;
	static constexpr u32 DSP_HZ = 8'000'000;
	static constexpr size_t CPU_ROM_SIZE = 0x40000;

	prose_board();
	~prose_board();

	bool load_cpu_roms(const std::array<std::vector<u8>, 4> &roms);
	bool load_dsp_roms(const std::vector<u8> &program, const std::vector<u8> &data);
	void reset();
	s64 run_cycles(s64 cycles);
	void queue_text(const std::string &text);
	void set_switches(u8 value) { m_switches = value; }

	u32 physical_pc() const { return m_cpu->physical_pc(); }
	u16 data_segment() const { return m_cpu->segment(3); }
	u8 peek(u32 address) { return read_memory(address); }
	const std::deque<u32> &recent_pcs() const { return m_recent_pcs; }
	const std::vector<u8> &uart_output() const { return m_uart_output; }
	const std::vector<std::pair<u8, u8>> &pic_writes() const { return m_pic_writes; }
	u64 parameter_writes() const { return m_parameter_writes; }
	u8 parameter() const { return m_parameter; }
	bool talking() const { return (m_parameter & 0x02U) == 0; }
	u64 dsp_data_writes() const { return m_dsp_data_writes; }
	u16 dsp_pc() const { return m_dsp ? prose_dsp_program_counter(m_dsp) : 0; }
	u8 dsp_status() const { return m_dsp ? prose_dsp_status_read(m_dsp) : 0; }
	u64 dsp_host_reads() const { return m_dsp ? prose_dsp_host_reads(m_dsp) : 0; }
	u64 dsp_ram_writes() const { return m_dsp ? prose_dsp_ram_writes(m_dsp) : 0; }
	u64 dsp_ram_changes() const { return m_dsp ? prose_dsp_ram_changes(m_dsp) : 0; }
	u16 dsp_last_output_source() const { return m_dsp ? prose_dsp_last_output_source(m_dsp) : 0; }
	u16 dsp_last_output_pointer() const { return m_dsp ? prose_dsp_last_output_pointer(m_dsp) : 0; }
	const std::vector<u8> &dsp_write_values() const { return m_dsp_write_values; }
	u64 uart_data_reads() const { return m_uart_data_reads; }
	size_t uart_pending() const { return m_uart_input.size(); }
	bool serial_input_paused() const { return m_serial_input_paused; }
	size_t pull_dsp_samples(std::vector<int16_t> &output, size_t limit);

private:
	friend class prose_program_space;
	friend class prose_io_space;

	u8 read_memory(u32 address);
	void write_memory(u32 address, u8 value);
	void run_dsp(s64 cpu_cycles);
	void consume_dsp_pic_events();
	void write_pic(u8 port, u8 value);
	void update_pic_line();
	void set_pic_request(u8 line, bool asserted);
	int pic_ack(device_t &, int);
	static bool matches(u32 address, u32 start, u32 end, u32 mirror);
	static u32 offset(u32 address, u32 start, u32 mirror);

	running_machine m_machine;
	machine_config m_config{m_machine};
	std::unique_ptr<prose_cpu> m_cpu;
	std::unique_ptr<address_space> m_program_space;
	std::unique_ptr<address_space> m_io_space;
	std::array<u8, 0x3000> m_ram{};
	std::array<u8, CPU_ROM_SIZE> m_cpu_rom{};
	prose_dsp *m_dsp = nullptr;
	u8 m_parameter = 0;
	u8 m_switches = 0xfc;
	u8 m_dsp_data = 0;
	u8 m_uart_mode = 0;
	u8 m_uart_command = 0;
	u8 m_pic_vector_base = 0x20;
	u8 m_pic_mask = 0xff;
	u8 m_pic_requests = 0;
	u8 m_pic_in_service = 0;
	u8 m_pic_init_step = 0;
	bool m_pic_needs_icw4 = false;
	bool m_pic_auto_eoi = false;
	bool m_pic_read_isr = false;
	std::deque<u8> m_uart_input;
	std::vector<u8> m_uart_output;
	std::vector<std::pair<u8, u8>> m_pic_writes;
	std::vector<u8> m_dsp_write_values;
	std::deque<u32> m_recent_pcs;
	u64 m_parameter_writes = 0;
	u64 m_dsp_data_writes = 0;
	u64 m_uart_data_reads = 0;
	u32 m_dsp_clock_remainder = 0;
	bool m_dsp_pic_line = false;
	bool m_serial_input_paused = false;
};
