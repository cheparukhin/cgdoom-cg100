/* tests/gint-host: stub of <gint/mmu.h> */
#pragma once
#include <stdint.h>
uint32_t mmu_translate(uint32_t page, uint32_t *size);
