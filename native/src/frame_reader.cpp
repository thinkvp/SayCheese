// frame_reader.cpp -- see frame_reader.h.
#include "frame_reader.h"

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace saycheese {

FrameReader::FrameReader() = default;

FrameReader::~FrameReader() {
    sws_freeContext(sws_);
    av_frame_free(&frame_);
    av_packet_free(&pkt_);
    avcodec_free_context(&dec_);
    avformat_close_input(&fmt_);
}

bool FrameReader::open(const std::string& path, std::string& error, int threads) {
    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
        error = "can't open the file";
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        error = "can't read the file's streams";
        return false;
    }
    stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_ < 0) {
        error = "no video stream";
        return false;
    }
    AVStream* st = fmt_->streams[stream_];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        error = std::string("unsupported codec (") + avcodec_get_name(st->codecpar->codec_id) + ")";
        return false;
    }
    dec_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec_, st->codecpar);
    dec_->thread_count = threads;
    dec_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(dec_, codec, nullptr) < 0) {
        error = std::string("can't start the ") + codec->name + " decoder";
        return false;
    }
    timeBase_ = av_q2d(st->time_base);
    // ffmpeg -ss counts from the container's start time, and so do these times.
    startTime_ = fmt_->start_time == AV_NOPTS_VALUE ? 0 : fmt_->start_time / double(AV_TIME_BASE);
    double rate = av_q2d(st->avg_frame_rate);
    if (!(rate > 1 && rate < 400)) rate = av_q2d(st->r_frame_rate);
    fps_ = (rate > 1 && rate < 400) ? rate : 25;
    if (fmt_->duration != AV_NOPTS_VALUE) duration_ = fmt_->duration / double(AV_TIME_BASE);
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    return true;
}

void FrameReader::setFastDecode(bool fast) {
    if (!dec_) return;
    dec_->skip_frame = fast ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    dec_->skip_loop_filter = fast ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
}

int FrameReader::width() const { return dec_ ? dec_->width : 0; }
int FrameReader::height() const { return dec_ ? dec_->height : 0; }

void FrameReader::seek(double t) {
    int64_t ts = static_cast<int64_t>((std::max(0.0, t) + startTime_) / timeBase_);
    if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(fmt_, stream_, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_);
    target_ = t;
    draining_ = false;
    finished_ = false;
}

const AVFrame* FrameReader::next(double& t) {
    const double halfFrame = 0.5 / fps_;
    while (!finished_) {
        int r = avcodec_receive_frame(dec_, frame_);
        if (r == 0) {
            int64_t pts = frame_->best_effort_timestamp;
            t = (pts == AV_NOPTS_VALUE ? 0 : pts * timeBase_) - startTime_;
            if (t < target_ - halfFrame) continue;  // decoded on the way to the seek target
            return frame_;
        }
        if (r == AVERROR_EOF) {
            finished_ = true;
            break;
        }
        if (r != AVERROR(EAGAIN)) {
            finished_ = true;
            break;
        }
        if (draining_) {
            finished_ = true;
            break;
        }
        // Feed the decoder.
        for (;;) {
            int rr = av_read_frame(fmt_, pkt_);
            if (rr < 0) {
                avcodec_send_packet(dec_, nullptr);
                draining_ = true;
                break;
            }
            if (pkt_->stream_index == stream_) {
                avcodec_send_packet(dec_, pkt_);
                av_packet_unref(pkt_);
                break;
            }
            av_packet_unref(pkt_);
        }
    }
    return nullptr;
}

void FrameReader::toRGB(const AVFrame* f, int x0, int y0, int x1, int y1, int outW, int outH, int flags, Image& out) {
    auto fmt = static_cast<AVPixelFormat>(f->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    x0 = std::clamp(x0, 0, f->width);
    y0 = std::clamp(y0, 0, f->height);
    x1 = std::clamp(x1, x0, f->width);
    y1 = std::clamp(y1, y0, f->height);

    // Crop by offsetting plane pointers (what ffmpeg's crop filter does), with
    // the origin snapped to the chroma grid.
    const uint8_t* planes[4] = {f->data[0], f->data[1], f->data[2], f->data[3]};
    bool whole = x0 == 0 && y0 == 0 && x1 == f->width && y1 == f->height;
    bool planar = desc && (desc->flags & AV_PIX_FMT_FLAG_PLANAR) && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
    if (planar) {
        x0 &= ~((1 << desc->log2_chroma_w) - 1);
        y0 &= ~((1 << desc->log2_chroma_h) - 1);
        for (int c = 0; c < desc->nb_components; c++) {
            const AVComponentDescriptor& comp = desc->comp[c];
            bool chroma = (c == 1 || c == 2) && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
            int px = chroma ? (x0 >> desc->log2_chroma_w) : x0;
            int py = chroma ? (y0 >> desc->log2_chroma_h) : y0;
            planes[comp.plane] = f->data[comp.plane] + py * f->linesize[comp.plane] + px * comp.step;
        }
    } else if (whole) {
        // nothing to crop
    } else if (fmt != AV_PIX_FMT_RGB24) {
        // Packed formats (rare for camera codecs): convert the whole frame to
        // RGB first, then crop that.
        Image full;
        toRGB(f, 0, 0, f->width, f->height, f->width, f->height, SWS_POINT, full);
        AVFrame rgb = {};
        rgb.format = AV_PIX_FMT_RGB24;
        rgb.width = full.w;
        rgb.height = full.h;
        rgb.data[0] = full.px.data();
        rgb.linesize[0] = full.w * 3;
        rgb.color_range = AVCOL_RANGE_JPEG;
        toRGB(&rgb, x0, y0, x1, y1, outW, outH, flags, out);
        return;
    } else {
        planes[0] = f->data[0] + y0 * f->linesize[0] + x0 * 3;
    }
    int w = std::max(1, x1 - x0), h = std::max(1, y1 - y0);

    sws_ = sws_getCachedContext(sws_, w, h, fmt, outW, outH, AV_PIX_FMT_RGB24, flags, nullptr, nullptr, nullptr);
    if (!sws_) return;
    // Use the frame's own YUV matrix and range, as ffmpeg's scale filter does.
    int* inv = nullptr;
    int srcRange = 0, *table = nullptr, dstRange = 0, brightness = 0, contrast = 0, saturation = 0;
    sws_getColorspaceDetails(sws_, &inv, &srcRange, &table, &dstRange, &brightness, &contrast, &saturation);
    int cs = f->colorspace == AVCOL_SPC_UNSPECIFIED ? SWS_CS_DEFAULT : f->colorspace;
    bool full = f->color_range == AVCOL_RANGE_JPEG;
    sws_setColorspaceDetails(sws_, sws_getCoefficients(cs), full, table, 1, brightness, contrast, saturation);

    out.resize(outW, outH);
    uint8_t* dst[4] = {out.px.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {outW * 3, 0, 0, 0};
    sws_scale(sws_, planes, f->linesize, 0, h, dst, dstStride);
}

void resizeGray(const uint8_t* src, int w, int h, int stride, int outW, int outH, int flags, std::vector<uint8_t>& out) {
    SwsContext* s = sws_getContext(w, h, AV_PIX_FMT_GRAY8, outW, outH, AV_PIX_FMT_GRAY8, flags, nullptr, nullptr, nullptr);
    out.resize(static_cast<size_t>(outW) * outH);
    const uint8_t* in[4] = {src, nullptr, nullptr, nullptr};
    int inStride[4] = {stride, 0, 0, 0};
    uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {outW, 0, 0, 0};
    sws_scale(s, in, inStride, 0, h, dst, dstStride);
    sws_freeContext(s);
}

} // namespace saycheese
