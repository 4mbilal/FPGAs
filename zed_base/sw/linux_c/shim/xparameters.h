/* xparameters.h - zed_base (v6 hardware) addresses for the Linux port.
 * Must match the Vivado address editor (and the UIO nodes in system-user.dtsi). */
#ifndef XPARAMETERS_H
#define XPARAMETERS_H
#define XPAR_VFB_RD_BASEADDR          0x43C00000U
#define XPAR_VTC_BASEADDR             0x43C10000U
#define XPAR_VFB_WR_BASEADDR          0x43C20000U
#define XPAR_CLK_WIZ_PIX_BASEADDR     0x43C30000U
#define XPAR_VFB_RD2_BASEADDR         0x43C40000U
#define XPAR_PROC_SLOT_BASEADDR       0x43D00000U
#define XPAR_AXI_GPIO_LEDSW_BASEADDR  0x41200000U
#define XPAR_AXI_GPIO_BTNS_BASEADDR   0x41210000U
#define XPAR_XGPIOPS_0_BASEADDR       0xE000A000U
#define XPAR_XIICPS_0_BASEADDR        0xE0004000U
#define XPAR_XIICPS_1_BASEADDR        0xE0005000U
#define XPAR_XUARTPS_0_BASEADDR       0xE0001000U
#define STDIN_BASEADDRESS             0xE0001000U
#define XPAR_CPU_CORE_CLOCK_FREQ_HZ   666666687U
#endif
