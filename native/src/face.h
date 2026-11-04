// face.h -- face finding and measurement, ported from engine/analyzer.py
// (Face, FaceAnalyzer, square_crop, sharpness).
#pragma once
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "image.h"

namespace saycheese {

struct Expression {
    double smile = 0, squint = 0, blink = 0, jaw = 0, pucker = 0, yaw = 0.5;
};

struct BBox {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct Face {
    std::vector<double> x, y;  // 478 landmarks, pixels in the analysed image
    std::unordered_map<std::string, double> blend;
    BBox box;

    double height() const { return std::max(1.0, box.y1 - box.y0); }
    double area() const { return (box.x1 - box.x0) * (box.y1 - box.y0); }
    void shift(double dx, double dy);
    Expression expression() const;
};

/** The square region around `box` scaled by `scale`, clamped to the image;
 *  false when it would be too small to analyse. */
bool squareRegion(int w, int h, const BBox& box, double scale, int& x0, int& y0, int& x1, int& y1);

/** Variance of the Laplacian over the face at a fixed face height. */
double sharpness(const Image& img, const Face& face);

class FaceAnalyzer {
public:
    /** Loads libmediapipe from `libDir` and the landmarker model. */
    bool init(const std::string& libDir, const std::string& modelPath, std::string& error);
    bool ready() const { return landmarker_ != nullptr; }

    std::vector<Face> detect(const Image& img);
    /** Largest face, trying a zoomed crop around `hint` first, then the whole
     *  frame, then square tiles for small off-centre faces. */
    bool find(const Image& img, const BBox* hint, Face& out);
    /** Probability each eye (right, left) is open, from the eye classifier. */
    void eyesOpen(const Image& img, const Face& face, double& right, double& left);

private:
    std::mutex lock_;
    void* landmarker_ = nullptr;
};

} // namespace saycheese
