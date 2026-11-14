/* xil_types.h - Linux userspace shim for the Xilinx standalone BSP types */
#ifndef XIL_TYPES_H
#define XIL_TYPES_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t  u8;   typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t   s8;   typedef int16_t  s16; typedef int32_t  s32; typedef int64_t  s64;
typedef uintptr_t UINTPTR;
typedef intptr_t  INTPTR;
typedef char char8;
#ifndef TRUE
#define TRUE  1U
#define FALSE 0U
#endif
#define XIL_COMPONENT_IS_READY   0x11111111U
#define XIL_COMPONENT_IS_STARTED 0x22222222U
#define LOWER_32_BITS(n) ((u32)(n))
#define UPPER_32_BITS(n) ((u32)(((u64)(n)) >> 32))
#endif
