// face.cpp -- see face.h. MediaPipe's face landmarker comes from libmediapipe,
// loaded at runtime from next to the addon (see mediapipe_c.h).
#include "face.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "eye_model.h"
#include "frame_reader.h"
#include "mediapipe_c.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include <libswscale/swscale.h>
}

namespace saycheese {
namespace {

constexpr int kSharpnessFaceHeight = 256;
// Eye crop side as a fraction of the distance between the eye centres. Sizing
// from eye width instead over-zooms the far eye in a three-quarter view.
constexpr double kEyeCropScale = 0.75;

PFN_MpFaceLandmarkerCreate mpCreate;
PFN_MpFaceLandmarkerDetectImage mpDetect;
PFN_MpFaceLandmarkerCloseResult mpCloseResult;
PFN_MpImageCreateFromUint8Data mpImageFromData;
PFN_MpImageFree mpImageFree;
PFN_MpErrorFree mpErrorFree;

void* loadLibrary(const std::string& path, std::string& error) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], n);
    // Altered search path: the library's own dependencies resolve from its folder.
    HMODULE h = LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) error = "couldn't load " + path + " (error " + std::to_string(GetLastError()) + ")";
    return h;
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) error = dlerror();
    return h;
#endif
}

void* symbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

std::string takeError(char* msg) {
    std::string s = msg ? msg : "unknown error";
    if (msg && mpErrorFree) mpErrorFree(msg);
    return s;
}

double blendOf(const Face& f, const char* name) {
    auto it = f.blend.find(name);
    return it == f.blend.end() ? 0.0 : it->second;
}

int reflect101(int i, int n) {
    if (n == 1) return 0;
    if (i < 0) return -i;
    if (i >= n) return 2 * n - 2 - i;
    return i;
}

} // namespace

// ---------------------------------------------------------------------------
// Face
// ---------------------------------------------------------------------------

void Face::shift(double dx, double dy) {
    for (auto& v : x) v += dx;
    for (auto& v : y) v += dy;
    box.x0 += dx;
    box.x1 += dx;
    box.y0 += dy;
    box.y1 += dy;
}

Expression Face::expression() const {
    Expression e;
    e.smile = (blendOf(*this, "mouthSmileLeft") + blendOf(*this, "mouthSmileRight")) / 2;
    e.squint = (blendOf(*this, "cheekSquintLeft") + blendOf(*this, "cheekSquintRight") +
                blendOf(*this, "eyeSquintLeft") + blendOf(*this, "eyeSquintRight")) / 4;
    e.blink = std::max(blendOf(*this, "eyeBlinkLeft"), blendOf(*this, "eyeBlinkRight"));
    e.jaw = blendOf(*this, "jawOpen");
    e.pucker = std::max(blendOf(*this, "mouthPucker"), blendOf(*this, "mouthFunnel"));
    // Yaw proxy: nose tip position between the two face-edge landmarks.
    if (x.size() > 454) {
        double lx = x[234], rx = x[454], nx = x[1];
        e.yaw = std::abs(rx - lx) > 1e-3 ? (nx - lx) / (rx - lx) : 0.5;
    }
    return e;
}

bool squareRegion(int w, int h, const BBox& b, double scale, int& x0, int& y0, int& x1, int& y1) {
    double cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
    double side = std::max(b.x1 - b.x0, b.y1 - b.y0) * scale;
    if (side < 16) return false;
    x0 = static_cast<int>(std::max(0.0, cx - side / 2));
    y0 = static_cast<int>(std::max(0.0, cy - side / 2));
    x1 = static_cast<int>(std::min(double(w), cx + side / 2));
    y1 = static_cast<int>(std::min(double(h), cy + side / 2));
    return x1 - x0 >= 16 && y1 - y0 >= 16;
}

double sharpness(const Image& img, const Face& face) {
    int x0 = std::max(0, static_cast<int>(std::lround(face.box.x0)));
    int y0 = std::max(0, static_cast<int>(std::lround(face.box.y0)));
    int x1 = std::min(img.w, static_cast<int>(std::lround(face.box.x1)));
    int y1 = std::min(img.h, static_cast<int>(std::lround(face.box.y1)));
    if (x1 - x0 < 8 || y1 - y0 < 8) return 0;
    int w = x1 - x0, h = y1 - y0;
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        const uint8_t* src = img.row(y0 + y) + x0 * 3;
        for (int x = 0; x < w; x++) gray[y * w + x] = grayOf(src + x * 3);
    }
    // Resize to a fixed face height (area when shrinking, bicubic when growing).
    double scale = double(kSharpnessFaceHeight) / h;
    int ow = std::max(1, static_cast<int>(std::lround(w * scale)));
    int oh = kSharpnessFaceHeight;
    std::vector<uint8_t> g;
    resizeGray(gray.data(), w, h, w, ow, oh, scale < 1 ? SWS_AREA : SWS_BICUBIC, g);

    // 3x3 Gaussian (sigma 0.8: [1 2 1]/4 each way) keeps sensor noise from
    // reading as detail; then the 4-neighbour Laplacian. Borders reflect-101.
    std::vector<uint8_t> blur(g.size());
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            int s = 0;
            for (int i = -1; i <= 1; i++) {
                const uint8_t* r = g.data() + reflect101(y + i, oh) * ow;
                int wy = i == 0 ? 2 : 1;
                s += wy * (r[reflect101(x - 1, ow)] + 2 * r[x] + r[reflect101(x + 1, ow)]);
            }
            blur[y * ow + x] = static_cast<uint8_t>((s + 8) >> 4);
        }
    }
    double sum = 0, sum2 = 0;
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            double l = double(blur[reflect101(y - 1, oh) * ow + x]) + blur[reflect101(y + 1, oh) * ow + x] +
                       blur[y * ow + reflect101(x - 1, ow)] + blur[y * ow + reflect101(x + 1, ow)] -
                       4.0 * blur[y * ow + x];
            sum += l;
            sum2 += l * l;
        }
    }
    double n = double(ow) * oh;
    double mean = sum / n;
    return sum2 / n - mean * mean;
}

// ---------------------------------------------------------------------------
// FaceAnalyzer
// ---------------------------------------------------------------------------

bool FaceAnalyzer::init(const std::string& libDir, const std::string& modelPath, std::string& error) {
    std::lock_guard<std::mutex> g(lock_);
    if (landmarker_) return true;
    // The library is shared by every analyzer; each has its own landmarker.
    static std::mutex libLock;
    static void* lib = nullptr;
    std::lock_guard<std::mutex> lg(libLock);
#ifdef _WIN32
    const char* libName = "/libmediapipe.dll";
#else
    const char* libName = "/libmediapipe.dylib";
#endif
    if (!lib && !(lib = loadLibrary(libDir + libName, error))) return false;
    mpCreate = reinterpret_cast<PFN_MpFaceLandmarkerCreate>(symbol(lib, "MpFaceLandmarkerCreate"));
    mpDetect = reinterpret_cast<PFN_MpFaceLandmarkerDetectImage>(symbol(lib, "MpFaceLandmarkerDetectImage"));
    mpCloseResult = reinterpret_cast<PFN_MpFaceLandmarkerCloseResult>(symbol(lib, "MpFaceLandmarkerCloseResult"));
    mpImageFromData = reinterpret_cast<PFN_MpImageCreateFromUint8Data>(symbol(lib, "MpImageCreateFromUint8Data"));
    mpImageFree = reinterpret_cast<PFN_MpImageFree>(symbol(lib, "MpImageFree"));
    mpErrorFree = reinterpret_cast<PFN_MpErrorFree>(symbol(lib, "MpErrorFree"));
    if (!mpCreate || !mpDetect || !mpCloseResult || !mpImageFromData || !mpImageFree) {
        error = "libmediapipe is missing the face landmarker functions";
        return false;
    }
    MpFaceLandmarkerOptions o = {};
    o.base_options.model_asset_path = modelPath.c_str();
    o.base_options.delegate = 1;  // CPU
#ifdef _WIN32
    o.base_options.host_system = 3;
#else
    o.base_options.host_system = 2;
#endif
    o.running_mode = 1;  // still images: samples are far apart in time
    o.num_faces = 2;
    o.min_face_detection_confidence = 0.4f;
    o.min_face_presence_confidence = 0.4f;
    o.min_tracking_confidence = 0.5f;
    o.output_face_blendshapes = true;
    char* err = nullptr;
    if (mpCreate(&o, &landmarker_, &err) != 0) {
        error = "face landmarker: " + takeError(err);
        landmarker_ = nullptr;
        return false;
    }
    return true;
}

std::vector<Face> FaceAnalyzer::detect(const Image& img) {
    std::vector<Face> faces;
    if (!landmarker_ || img.empty()) return faces;
    std::lock_guard<std::mutex> g(lock_);
    void* image = nullptr;
    char* err = nullptr;
    if (mpImageFromData(MP_IMAGE_SRGB, img.w, img.h, img.px.data(), static_cast<int>(img.px.size()), &image, &err) != 0) {
        takeError(err);
        return faces;
    }
    MpFaceLandmarkerResult res = {};
    if (mpDetect(landmarker_, image, nullptr, &res, &err) == 0) {
        uint32_t n = std::min(res.face_landmarks_count, res.face_blendshapes_count);
        for (uint32_t i = 0; i < n; i++) {
            const MpNormalizedLandmarks& lms = res.face_landmarks[i];
            Face f;
            f.x.resize(lms.landmarks_count);
            f.y.resize(lms.landmarks_count);
            f.box = {1e9, 1e9, -1e9, -1e9};
            for (uint32_t k = 0; k < lms.landmarks_count; k++) {
                f.x[k] = lms.landmarks[k].x * img.w;
                f.y[k] = lms.landmarks[k].y * img.h;
                f.box.x0 = std::min(f.box.x0, f.x[k]);
                f.box.y0 = std::min(f.box.y0, f.y[k]);
                f.box.x1 = std::max(f.box.x1, f.x[k]);
                f.box.y1 = std::max(f.box.y1, f.y[k]);
            }
            const MpCategories& c = res.face_blendshapes[i];
            for (uint32_t k = 0; k < c.categories_count; k++)
                if (c.categories[k].category_name) f.blend[c.categories[k].category_name] = c.categories[k].score;
            faces.push_back(std::move(f));
        }
        mpCloseResult(&res);
    } else {
        takeError(err);
    }
    mpImageFree(image);
    return faces;
}

bool FaceAnalyzer::find(const Image& img, const BBox* hint, Face& out) {
    auto largest = [&](std::vector<Face>& faces, int ox, int oy) {
        auto it = std::max_element(faces.begin(), faces.end(),
                                   [](const Face& a, const Face& b) { return a.area() < b.area(); });
        out = std::move(*it);
        out.shift(ox, oy);
    };
    int x0, y0, x1, y1;
    if (hint && squareRegion(img.w, img.h, *hint, 2.4, x0, y0, x1, y1)) {
        std::vector<Face> faces = detect(img.crop(x0, y0, x1, y1));
        if (!faces.empty()) {
            largest(faces, x0, y0);
            return true;
        }
    }
    std::vector<Face> faces = detect(img);
    if (!faces.empty()) {
        largest(faces, 0, 0);
        return true;
    }
    // Interview framing often puts the subject off-centre and small; square
    // tiles give the short-range detector a closer look.
    int side = std::min(img.w, img.h);
    for (double frac : {0.0, 0.5, 1.0}) {
        int ox = img.w > img.h ? static_cast<int>((img.w - side) * frac) : 0;
        int oy = img.h > img.w ? static_cast<int>((img.h - side) * frac) : 0;
        faces = detect(img.crop(ox, oy, ox + side, oy + side));
        if (!faces.empty()) {
            largest(faces, ox, oy);
            return true;
        }
        if (img.w == img.h) break;
    }
    return false;
}

void FaceAnalyzer::eyesOpen(const Image& img, const Face& face, double& right, double& left) {
    right = left = 1.0;
    if (face.x.size() < 400) return;
    double cx[2] = {(face.x[33] + face.x[133]) / 2, (face.x[362] + face.x[263]) / 2};
    double cy[2] = {(face.y[33] + face.y[133]) / 2, (face.y[362] + face.y[263]) / 2};
    double side = std::hypot(cx[0] - cx[1], cy[0] - cy[1]) * kEyeCropScale;
    if (side < 4) return;
    double inv = side / 32;  // source pixels per crop pixel
    double probs[2];
    for (int e = 0; e < 2; e++) {
        // 32x32 crop centred on the eye, bilinear, black outside the frame;
        // the model was trained on infrared (grey) eye crops.
        uint8_t gray[32 * 32];
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                double sx = (x - 16) * inv + cx[e], sy = (y - 16) * inv + cy[e];
                int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
                double fx = sx - ix, fy = sy - iy;
                double c[3] = {0, 0, 0};
                for (int j = 0; j < 2; j++) {
                    for (int i = 0; i < 2; i++) {
                        int px = ix + i, py = iy + j;
                        if (px < 0 || py < 0 || px >= img.w || py >= img.h) continue;
                        double wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                        const uint8_t* p = img.row(py) + px * 3;
                        for (int k = 0; k < 3; k++) c[k] += wgt * p[k];
                    }
                }
                uint8_t rgb[3] = {static_cast<uint8_t>(std::lround(c[0])), static_cast<uint8_t>(std::lround(c[1])),
                                  static_cast<uint8_t>(std::lround(c[2]))};
                gray[y * 32 + x] = grayOf(rgb);
            }
        }
        probs[e] = eyeOpenProbability(gray);
    }
    right = probs[0];
    left = probs[1];
}

} // namespace saycheese
