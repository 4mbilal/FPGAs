/* xstatus.h - Linux userspace shim */
#ifndef XSTATUS_H
#define XSTATUS_H
#include "xil_types.h"
#define XST_SUCCESS            0L
#define XST_FAILURE            1L
#define XST_DEVICE_NOT_FOUND   2L
#define XST_DEVICE_BLOCK_NOT_FOUND 3L
#define XST_INVALID_VERSION    4L
#define XST_DEVICE_IS_STARTED  5L
#define XST_DEVICE_IS_STOPPED  6L
#define XST_FIFO_ERROR         7L
#define XST_RESET_ERROR        8L
#define XST_DMA_ERROR          9L
#define XST_NOT_POLLED         10L
#define XST_FIFO_NO_ROOM       11L
#define XST_BUFFER_TOO_SMALL   12L
#define XST_NO_DATA            13L
#define XST_REGISTER_ERROR     14L
#define XST_INVALID_PARAM      15L
#define XST_NOT_SGDMA          16L
#define XST_LOOPBACK_ERROR     17L
#define XST_NO_CALLBACK        18L
#define XST_NO_FEATURE         19L
#define XST_NOT_INTERRUPT      20L
#define XST_DEVICE_BUSY        21L
#define XST_ERROR_COUNT_MAX    22L
#define XST_IS_STARTED         23L
#define XST_IS_STOPPED         24L
#define XST_DATA_LOST          26L
#define XST_RECV_ERROR         27L
#define XST_SEND_ERROR         28L
#define XST_NOT_ENABLED        29L
#define XST_NO_ACCESS          30L
#define XST_TIMEOUT            31L
#endif
