/* xuartps_hw.h - Linux userspace shim: the "UART" is the controlling terminal (stdin). */
#ifndef XUARTPS_HW_H
#define XUARTPS_HW_H
#include "xil_types.h"
#define XUARTPS_FIFO_OFFSET 0x30U
int zb_stdin_ready(void);
int zb_stdin_getc(void);
#define XUartPs_IsReceiveData(base)   zb_stdin_ready()
#define XUartPs_ReadReg(base, reg)    ((u32)zb_stdin_getc())
#endif
