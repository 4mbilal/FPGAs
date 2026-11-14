/* xenv.h - Linux userspace shim */
#ifndef XENV_H
#define XENV_H
#include <string.h>
#define XENV_MEM_COPY(d, s, n) memcpy(d, s, n)
#define XENV_MEM_FILL(d, v, n) memset(d, v, n)
#endif
