/* zb_linux.h - extras of the Linux port (u-dma-buf region for jig buffers). */
#ifndef ZB_LINUX_H
#define ZB_LINUX_H
#include "xil_types.h"
UINTPTR zb_dma_base(void);      /* CPU (virtual) address of /dev/udmabuf0 */
u32     zb_dma_size(void);
u32     zb_dma_phys(void);
#endif
