/* xil_io.h - Linux userspace shim.
 * Xil_In32/Xil_Out32 take PHYSICAL addresses (as in bare metal) and are translated to the
 * UIO mappings of the PL IP. Frame buffer address registers written with a pointer into the
 * u-dma-buf mapping are converted to the physical address (see zb_linux.c). */
#ifndef XIL_IO_H
#define XIL_IO_H
#include "xil_types.h"
#include "xstatus.h"     /* the standalone xil_io.h also brings in XST_* */
u32  Xil_In32(UINTPTR addr);
void Xil_Out32(UINTPTR addr, u32 val);
static inline u16  Xil_In16(UINTPTR a) { return (u16)(Xil_In32(a & ~(UINTPTR)3) >> (8 * (a & 2))); }
static inline u8   Xil_In8(UINTPTR a)  { return (u8)(Xil_In32(a & ~(UINTPTR)3) >> (8 * (a & 3))); }
static inline u64  Xil_In64(UINTPTR a) { return (u64)Xil_In32(a) | ((u64)Xil_In32(a + 4) << 32); }
static inline void Xil_Out64(UINTPTR a, u64 v) { Xil_Out32(a, (u32)v); Xil_Out32(a + 4, (u32)(v >> 32)); }
#endif
