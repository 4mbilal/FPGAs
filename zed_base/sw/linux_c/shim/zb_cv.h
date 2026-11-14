/* zb_cv.h - OpenCV software kernels for the Linux jig (C interface). */
#ifndef ZB_CV_H
#define ZB_CV_H
#include "xil_types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Canny edges on an XRGB8888 frame in place: w x h pixels, line stride in pixels.
 * Output: white edges on black (grey written to R, G and B).
 * Thresholds: env ZB_CANNY_LO / ZB_CANNY_HI (default 50 / 150), ZB_CANNY_BLUR=0 to skip
 * the 3x3 Gaussian pre-blur. */
void k_canny(u32 *p, int w, int h, int stride_px);
#ifdef __cplusplus
}
#endif
#endif
