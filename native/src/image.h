// image.h -- an 8-bit RGB image, the common currency between decoding and analysis.
#pragma once
#include <cstdint>
#include <vector>

namespace saycheese {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // RGB24, rows packed (stride = 3 * w)

    void resize(int width, int height) {
        w = width;
        h = height;
        px.resize(static_cast<size_t>(w) * h * 3);
    }
    uint8_t* row(int y) { return px.data() + static_cast<size_t>(y) * w * 3; }
    const uint8_t* row(int y) const { return px.data() + static_cast<size_t>(y) * w * 3; }
    bool empty() const { return w <= 0 || h <= 0; }

    /** Copy of [x0, x1) x [y0, y1); the rectangle must lie inside the image. */
    Image crop(int x0, int y0, int x1, int y1) const {
        Image out;
        out.resize(x1 - x0, y1 - y0);
        for (int y = y0; y < y1; y++) {
            const uint8_t* src = row(y) + x0 * 3;
            std::copy(src, src + out.w * 3, out.row(y - y0));
        }
        return out;
    }
};

/** OpenCV's BGR2GRAY weights (fixed point, rounded), applied to RGB. */
inline uint8_t grayOf(const uint8_t* rgb) {
    return static_cast<uint8_t>((rgb[0] * 4899 + rgb[1] * 9617 + rgb[2] * 1868 + 8192) >> 14);
}

} // namespace saycheese
