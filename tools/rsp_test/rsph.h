// Minimal RSP test host around the cxd4 interpreter.
// RDRAM/DMEM/IMEM are kept in cxd4's layout (host-order 32-bit words), so use
// the accessors below, which take big-endian (N64 order) byte images.
#pragma once
#include <stdint.h>
#include <stddef.h>
#define RSPH_RDRAM_SIZE (8u << 20)
void rsph_init(void);
void rsph_load_file(const char *text_bin, const char *data_bin);
void rsph_write(uint8_t *mem, uint32_t addr, const void *be_bytes, size_t n);   // mem: rsph_rdram/dmem/imem
void rsph_read(const uint8_t *mem, uint32_t addr, void *be_bytes, size_t n);
void rsph_w32(uint8_t *mem, uint32_t addr, uint32_t v);
uint32_t rsph_r32(const uint8_t *mem, uint32_t addr);
int rsph_run(void);   // runs from PC 0 until break; returns 0 on halt
extern uint8_t *rsph_rdram, *rsph_dmem, *rsph_imem;
