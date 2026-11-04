// eye_model.cpp -- forward pass of open-closed-eye-0001:
//   conv3x3(3->10) maxpool2 relu, conv3x3(10->20) maxpool2 relu,
//   conv3x3(20->50), conv1x1(50->2), maxpool4, softmax.
// Input is OpenCV's blobFromImage(crop, 1/255, mean 127) of a grey crop
// replicated into three channels, as engine/analyzer.py fed it.
#include "eye_model.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "eye_model_weights.h"

namespace saycheese {
namespace {

// Valid (unpadded) convolution, stride 1. in: C x H x W, w: K x C x k x k.
std::vector<float> conv(const std::vector<float>& in, int C, int H, int W, const float* w, const float* bias, int K,
                        int k, int& outH, int& outW) {
    outH = H - k + 1;
    outW = W - k + 1;
    std::vector<float> out(static_cast<size_t>(K) * outH * outW);
    for (int o = 0; o < K; o++) {
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                float s = bias ? bias[o] : 0.0f;
                for (int c = 0; c < C; c++) {
                    const float* wk = w + ((o * C + c) * k) * k;
                    const float* src = in.data() + (static_cast<size_t>(c) * H + y) * W + x;
                    for (int i = 0; i < k; i++)
                        for (int j = 0; j < k; j++) s += wk[i * k + j] * src[i * W + j];
                }
                out[(static_cast<size_t>(o) * outH + y) * outW + x] = s;
            }
        }
    }
    return out;
}

// Max pool with kernel = stride = p, floor mode; optional ReLU after.
std::vector<float> pool(const std::vector<float>& in, int C, int H, int W, int p, bool relu, int& outH, int& outW) {
    outH = H / p;
    outW = W / p;
    std::vector<float> out(static_cast<size_t>(C) * outH * outW);
    for (int c = 0; c < C; c++) {
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                float m = -1e30f;
                for (int i = 0; i < p; i++)
                    for (int j = 0; j < p; j++)
                        m = std::max(m, in[(static_cast<size_t>(c) * H + y * p + i) * W + x * p + j]);
                out[(static_cast<size_t>(c) * outH + y) * outW + x] = relu ? std::max(0.0f, m) : m;
            }
        }
    }
    return out;
}

} // namespace

double eyeOpenProbability(const uint8_t gray[32 * 32]) {
    std::vector<float> x(3 * 32 * 32);
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 32 * 32; i++) x[c * 1024 + i] = (gray[i] - 127.0f) / 255.0f;

    int h = 32, w = 32, oh, ow;
    x = conv(x, 3, h, w, k_conv1_weight, k_conv1_bias, 10, 3, oh, ow);
    x = pool(x, 10, oh, ow, 2, true, h, w);
    x = conv(x, 10, h, w, k_conv2_weight, k_conv2_bias, 20, 3, oh, ow);
    x = pool(x, 20, oh, ow, 2, true, h, w);
    x = conv(x, 20, h, w, k_conv3_weight, k_conv3_bias, 50, 3, oh, ow);
    x = conv(x, 50, oh, ow, k_conv4_weight, nullptr, 2, 1, h, w);
    x = pool(x, 2, h, w, 4, false, oh, ow);  // -> 2 x 1 x 1
    // Softmax; the zoo docs list [open, closed] but index 1 is open in practice.
    double e0 = std::exp(static_cast<double>(x[0])), e1 = std::exp(static_cast<double>(x[1]));
    return e1 / (e0 + e1);
}

} // namespace saycheese
