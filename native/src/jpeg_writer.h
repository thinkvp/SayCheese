// jpeg_writer.h -- writes an RGB image as a JPEG with FFmpeg's MJPEG encoder.
#pragma once
#include <string>

#include "image.h"

namespace saycheese {

/** `quality` is the MJPEG qscale (2 best .. 31 worst). */
bool writeJpeg(const Image& img, const std::string& path, int quality, std::string& error);

} // namespace saycheese
