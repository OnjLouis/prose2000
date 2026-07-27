#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef PROSE_DSP_BUILD
#define PROSE_API __declspec(dllexport)
#else
#define PROSE_API __declspec(dllimport)
#endif
#else
#define PROSE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct prose_dsp prose_dsp;

PROSE_API prose_dsp *prose_dsp_create(
	const uint8_t *packed_program,
	size_t packed_program_size,
	const uint8_t *data_rom,
	size_t data_rom_size);
PROSE_API void prose_dsp_destroy(prose_dsp *dsp);
PROSE_API void prose_dsp_reset(prose_dsp *dsp);
PROSE_API void prose_dsp_set_reset(prose_dsp *dsp, int asserted);
PROSE_API void prose_dsp_set_interrupt(prose_dsp *dsp, int asserted);
PROSE_API void prose_dsp_run(prose_dsp *dsp, uint32_t instructions);
PROSE_API uint8_t prose_dsp_status_read(prose_dsp *dsp);
PROSE_API uint8_t prose_dsp_data_read(prose_dsp *dsp);
PROSE_API void prose_dsp_data_write(prose_dsp *dsp, uint8_t value);
PROSE_API size_t prose_dsp_read_samples(prose_dsp *dsp, int16_t *output, size_t capacity);
PROSE_API uint16_t prose_dsp_program_counter(prose_dsp *dsp);
PROSE_API int prose_dsp_p0(prose_dsp *dsp);
PROSE_API int prose_dsp_p1(prose_dsp *dsp);
PROSE_API size_t prose_dsp_read_p0_events(prose_dsp *dsp, uint8_t *output, size_t capacity);
PROSE_API uint64_t prose_dsp_host_reads(prose_dsp *dsp);
PROSE_API uint64_t prose_dsp_ram_writes(prose_dsp *dsp);
PROSE_API uint64_t prose_dsp_ram_changes(prose_dsp *dsp);
PROSE_API uint16_t prose_dsp_last_output_source(prose_dsp *dsp);
PROSE_API uint16_t prose_dsp_last_output_pointer(prose_dsp *dsp);

#ifdef __cplusplus
}
#endif
