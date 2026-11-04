// mediapipe_c.h -- the slice of MediaPipe's Tasks C API that SayCheese uses.
//
// libmediapipe ships prebuilt inside the `mediapipe` Python wheel (Apache-2.0)
// without headers; these declarations mirror the ctypes structures in
// mediapipe/tasks/python (base_options_c.py, face_landmarker.py,
// containers/*_c.py), which call the same library. The library is loaded at
// runtime from next to the addon, so nothing here links against it.
#pragma once
#include <cstdint>

extern "C" {

struct MpBaseOptions {
    const char* model_asset_buffer;
    unsigned int model_asset_buffer_count;
    const char* model_asset_path;
    int file_descriptor;
    int delegate;          // 0 default, 1 CPU, 2 GPU
    int host_environment;
    int host_system;       // 1 Linux, 2 Mac, 3 Windows
    const char* host_version;
    const char* ca_bundle_path;
    const char* app_id;
    const char* app_version;
};

struct MpNormalizedLandmark {
    float x, y, z;
    bool has_visibility;
    float visibility;
    bool has_presence;
    float presence;
    const char* name;
};

struct MpNormalizedLandmarks {
    MpNormalizedLandmark* landmarks;
    uint32_t landmarks_count;
};

struct MpCategory {
    int index;
    float score;
    const char* category_name;
    const char* display_name;
};

struct MpCategories {
    MpCategory* categories;
    uint32_t categories_count;
};

struct MpMatrix {
    uint32_t rows;
    uint32_t cols;
    float* data;
};

struct MpFaceLandmarkerResult {
    MpNormalizedLandmarks* face_landmarks;
    uint32_t face_landmarks_count;
    MpCategories* face_blendshapes;
    uint32_t face_blendshapes_count;
    MpMatrix* facial_transformation_matrixes;
    uint32_t facial_transformation_matrixes_count;
};

typedef void (*MpFaceLandmarkerCallback)(int status, MpFaceLandmarkerResult* result, void* image, int64_t timestamp_ms);

struct MpFaceLandmarkerOptions {
    MpBaseOptions base_options;
    int running_mode;      // 1 image, 2 video, 3 live stream
    int num_faces;
    float min_face_detection_confidence;
    float min_face_presence_confidence;
    float min_tracking_confidence;
    bool output_face_blendshapes;
    bool output_facial_transformation_matrixes;
    MpFaceLandmarkerCallback result_callback;
};

struct MpRectF { float left, top, bottom, right; };

struct MpImageProcessingOptions {
    bool has_region_of_interest;
    MpRectF region_of_interest;
    int rotation_degrees;
};

enum { MP_IMAGE_SRGB = 1 };

// Every status function returns 0 on success and takes a trailing char** that
// receives an error message to release with MpErrorFree.
typedef int (*PFN_MpFaceLandmarkerCreate)(MpFaceLandmarkerOptions*, void** landmarker, char** error);
typedef int (*PFN_MpFaceLandmarkerDetectImage)(void* landmarker, void* image, MpImageProcessingOptions*, MpFaceLandmarkerResult*, char** error);
typedef void (*PFN_MpFaceLandmarkerCloseResult)(MpFaceLandmarkerResult*);
typedef int (*PFN_MpFaceLandmarkerClose)(void* landmarker, char** error);
typedef int (*PFN_MpImageCreateFromUint8Data)(int format, int width, int height, const uint8_t* data, int size, void** image, char** error);
typedef void (*PFN_MpImageFree)(void* image);
typedef void (*PFN_MpErrorFree)(void* error);

} // extern "C"
