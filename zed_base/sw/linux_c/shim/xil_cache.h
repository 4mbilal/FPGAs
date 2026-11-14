/* xil_cache.h - Linux userspace shim: cache maintenance on the u-dma-buf region
 * (the buffer is mapped cached; these call the u-dma-buf sync interface). */
#ifndef XIL_CACHE_H
#define XIL_CACHE_H
#include "xil_types.h"
void Xil_DCacheFlushRange(UINTPTR addr, u32 len);
void Xil_DCacheInvalidateRange(UINTPTR addr, u32 len);
static inline void Xil_DCacheFlush(void) {}
static inline void Xil_DCacheEnable(void) {}
static inline void Xil_DCacheDisable(void) {}
static inline void Xil_ICacheEnable(void) {}
#endif
