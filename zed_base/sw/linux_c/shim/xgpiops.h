/* xgpiops.h - Linux userspace shim: PS GPIO (MIO/EMIO) through zynq_gpio (libgpiod).
 * Pin numbers are the bare-metal ones (EMIO 54..117 = line offsets 54..117). */
#ifndef XGPIOPS_H
#define XGPIOPS_H
#include "xil_types.h"
#include "xstatus.h"
typedef struct { UINTPTR BaseAddr; } XGpioPs_Config;
typedef struct { XGpioPs_Config Config; void *chip; void *lines[118]; u8 dir[118]; } XGpioPs;
XGpioPs_Config *XGpioPs_LookupConfig(UINTPTR base);
int  XGpioPs_CfgInitialize(XGpioPs *g, XGpioPs_Config *c, UINTPTR base);
void XGpioPs_SetDirectionPin(XGpioPs *g, u32 pin, u32 out);
void XGpioPs_SetOutputEnablePin(XGpioPs *g, u32 pin, u32 en);
void XGpioPs_WritePin(XGpioPs *g, u32 pin, u32 v);
u32  XGpioPs_ReadPin(XGpioPs *g, u32 pin);
#endif
