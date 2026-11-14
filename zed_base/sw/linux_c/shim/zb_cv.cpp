// zb_cv.cpp - OpenCV software kernels for the Linux jig. See zb_cv.h.
#include <cstdlib>
#include <opencv2/imgproc.hpp>
#include "zb_cv.h"

static double env_d(const char *name, double def)
{
    const char *s = std::getenv(name);
    return s ? std::atof(s) : def;
}

extern "C" void k_canny(u32 *p, int w, int h, int stride_px)
{
    static const double lo = env_d("ZB_CANNY_LO", 50), hi = env_d("ZB_CANNY_HI", 150);
    static const bool blur = env_d("ZB_CANNY_BLUR", 1) != 0;
    static cv::Mat gray, edges;                           // reused between frames
    cv::Mat frame(h, w, CV_8UC4, p, (size_t)stride_px * 4);   // B,G,R,X bytes, no copy
    cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
    if (blur) cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0);
    cv::Canny(gray, edges, lo, hi);
    cv::cvtColor(edges, frame, cv::COLOR_GRAY2BGRA);      // same size/type: written in place
}
