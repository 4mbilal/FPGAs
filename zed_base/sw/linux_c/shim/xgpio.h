/* xgpio.h - Linux userspace shim: AXI GPIO through the kernel gpio-xilinx driver (libgpiod).
 * Channel 1 = lines 0..W1-1, channel 2 = lines W1.. (W1 = 8 for a 16-line dual-channel chip). */
#ifndef XGPIO_H
#define XGPIO_H
#include "xil_types.h"
#include "xstatus.h"
typedef struct { UINTPTR BaseAddress; void *chip; void *lines[32]; int nlines, w1; u32 dir[3]; } XGpio;
int  XGpio_Initialize(XGpio *g, UINTPTR base);
void XGpio_SetDataDirection(XGpio *g, unsigned ch, u32 in_mask);
u32  XGpio_DiscreteRead(XGpio *g, unsigned ch);
void XGpio_DiscreteWrite(XGpio *g, unsigned ch, u32 v);
#endif
