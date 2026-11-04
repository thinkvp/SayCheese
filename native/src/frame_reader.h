// frame_reader.h -- frame-accurate video decoding with FFmpeg, plus cropping
// and scaling decoded frames to RGB.
#pragma once
#include <string>

#include "image.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;

namespace saycheese {

class FrameReader {
public:
    FrameReader();
    ~FrameReader();
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;

    /** `threads` = decoder threads, 0 for one per core. */
    bool open(const std::string& path, std::string& error, int threads = 0);
    double fps() const { return fps_; }
    /** Seconds, or 0 when the container doesn't say. */
    double duration() const { return duration_; }

    /** After this, next() returns frames whose time is at or after `t` seconds
     *  (measured like ffmpeg's -ss: from the file's start time). */
    void seek(double t);

    /** The next decoded frame and its time in seconds, or nullptr at the end.
     *  The frame stays valid until the next call. */
    const AVFrame* next(double& t);

    /** Coarse scanning: skip frames nothing else refers to and the deblocking
     *  filter. Sampled frames can land a frame or two later; fine downscaled. */
    void setFastDecode(bool fast);

    int width() const;
    int height() const;

    /** Region [x0, x1) x [y0, y1) of `frame`, in source pixels, scaled to
     *  outW x outH RGB. `flags` are swscale flags (SWS_BICUBIC, SWS_AREA...). */
    void toRGB(const AVFrame* frame, int x0, int y0, int x1, int y1, int outW, int outH, int flags, Image& out);

private:
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* sws_ = nullptr;
    int stream_ = -1;
    double timeBase_ = 0;
    double startTime_ = 0;
    double fps_ = 25;
    double duration_ = 0;
    double target_ = -1;
    bool draining_ = false;
    bool finished_ = false;
};

/** Resizes an 8-bit grey plane with swscale. */
void resizeGray(const uint8_t* src, int w, int h, int stride, int outW, int outH, int flags, std::vector<uint8_t>& out);

} // namespace saycheese
