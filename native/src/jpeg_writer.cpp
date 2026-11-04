// jpeg_writer.cpp -- see jpeg_writer.h.
#include "jpeg_writer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace saycheese {

bool writeJpeg(const Image& img, const std::string& path, int quality, std::string& error) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        error = "no JPEG encoder";
        return false;
    }
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;
    bool ok = false;
    do {
        enc->width = img.w;
        enc->height = img.h;
        enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
        enc->color_range = AVCOL_RANGE_JPEG;
        enc->time_base = {1, 25};
        enc->flags |= AV_CODEC_FLAG_QSCALE;
        enc->global_quality = FF_QP2LAMBDA * quality;
        if (avcodec_open2(enc, codec, nullptr) < 0) { error = "can't start the JPEG encoder"; break; }

        frame->format = AV_PIX_FMT_YUVJ420P;
        frame->width = img.w;
        frame->height = img.h;
        if (av_frame_get_buffer(frame, 0) < 0) { error = "out of memory"; break; }
        frame->quality = enc->global_quality;
        frame->pict_type = AV_PICTURE_TYPE_I;
        sws = sws_getContext(img.w, img.h, AV_PIX_FMT_RGB24, img.w, img.h, AV_PIX_FMT_YUVJ420P,
                             SWS_BICUBIC, nullptr, nullptr, nullptr);
        const uint8_t* src[4] = {img.px.data(), nullptr, nullptr, nullptr};
        int srcStride[4] = {img.w * 3, 0, 0, 0};
        sws_scale(sws, src, srcStride, 0, img.h, frame->data, frame->linesize);

        if (avcodec_send_frame(enc, frame) < 0 || avcodec_receive_packet(enc, pkt) < 0) { error = "JPEG encoding failed"; break; }
        std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
        out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
        if (!out) { error = "can't write " + path; break; }
        ok = true;
    } while (false);
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    return ok;
}

} // namespace saycheese
