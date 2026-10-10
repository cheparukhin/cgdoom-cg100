/* tests/gint-host: stub of <gint/dma.h> */
#ifndef STUB_DMA
#define STUB_DMA
#include <gint/gint.h>
#include <stdbool.h>
typedef enum { DMA_32B = 4 } dma_size_t;
typedef enum { DMA_FIXED = 0, DMA_INC = 1 } dma_address_t;
static inline void dma_transfer_wait(int c) { (void)c; }
static inline bool dma_transfer_async(int c, dma_size_t s, unsigned b, void const *src, dma_address_t sm, void *dst, dma_address_t dm, gint_call_t cb) { (void)c;(void)s;(void)b;(void)src;(void)sm;(void)dst;(void)dm;(void)cb; return true; }
#define GINT_CALL_NULL ((gint_call_t){0, 0})
#endif
