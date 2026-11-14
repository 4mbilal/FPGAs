/* xiicps.h - Linux userspace shim: PS I2C through /dev/i2c-N (i2c-dev).
 * XIICPS_REP_START_OPTION: the next send is held and issued together with the following
 * receive as one combined transfer (repeated start), as the bare-metal driver does. */
#ifndef XIICPS_H
#define XIICPS_H
#include "xil_types.h"
#include "xstatus.h"
#define XIICPS_7_BIT_ADDR_OPTION 0x01U
#define XIICPS_10_BIT_ADDR_OPTION 0x02U
#define XIICPS_SLAVE_MON_OPTION  0x04U
#define XIICPS_REP_START_OPTION  0x08U
typedef struct { UINTPTR BaseAddress; } XIicPs_Config;
typedef struct { XIicPs_Config Config; int fd; u32 opts; int held; u8 hbuf[64]; int hlen; u16 haddr; } XIicPs;
XIicPs_Config *XIicPs_LookupConfig(UINTPTR base);
int  XIicPs_CfgInitialize(XIicPs *i, XIicPs_Config *c, UINTPTR base);
int  XIicPs_SetSClk(XIicPs *i, u32 hz);
int  XIicPs_MasterSendPolled(XIicPs *i, u8 *buf, s32 n, u16 addr);
int  XIicPs_MasterRecvPolled(XIicPs *i, u8 *buf, s32 n, u16 addr);
int  XIicPs_SetOptions(XIicPs *i, u32 o);
int  XIicPs_ClearOptions(XIicPs *i, u32 o);
u32  XIicPs_GetOptions(XIicPs *i);
#define XIicPs_BusIsBusy(i) 0
#endif
