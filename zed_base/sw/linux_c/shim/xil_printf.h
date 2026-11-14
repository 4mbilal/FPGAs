/* xil_printf.h - Linux userspace shim */
#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H
#include <stdio.h>
#define xil_printf(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define print(s) xil_printf("%s", s)
#endif
