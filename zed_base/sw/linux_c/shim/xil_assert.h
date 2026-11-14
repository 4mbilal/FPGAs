/* xil_assert.h - Linux userspace shim: asserts print and abort */
#ifndef XIL_ASSERT_H
#define XIL_ASSERT_H
#include <stdio.h>
#include <stdlib.h>
#include "xil_types.h"
#define XIL_ASSERT_NONE 0U
#define XIL_ASSERT_OCCURRED 1U
#define Xil_AssertVoid(e)    do { if (!(e)) { fprintf(stderr, "Xil assert: %s (%s:%d)\n", #e, __FILE__, __LINE__); abort(); } } while (0)
#define Xil_AssertNonvoid(e) do { if (!(e)) { fprintf(stderr, "Xil assert: %s (%s:%d)\n", #e, __FILE__, __LINE__); abort(); } } while (0)
#define Xil_AssertVoidAlways()    do { fprintf(stderr, "Xil assert (%s:%d)\n", __FILE__, __LINE__); abort(); } while (0)
#define Xil_AssertNonvoidAlways() do { fprintf(stderr, "Xil assert (%s:%d)\n", __FILE__, __LINE__); abort(); } while (0)
#endif
