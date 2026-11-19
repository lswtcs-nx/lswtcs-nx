// log129: FFmpeg's WMV3/VC-1 decoder for the game's movies (the title-idle trailer and any other WMV the player opens).
// kernel_stubs.cpp feeds it the compressed frames it pulls through the game's own data callback and copies its pictures
// into the buffers the game asks for. Plain C API so the runtime does not need FFmpeg's headers.
#include <stdint.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"

typedef struct LswVc1 {
    AVCodecContext* ctx;
    AVPacket* pkt;
    AVFrame* frame;    // the latest picture
    AVFrame* scratch;
    int have;          // a picture was decoded since the stream began
} LswVc1;

void lsw_vc1_close(void* h) {
    LswVc1* d = (LswVc1*)h;
    if (!d) return;
    avcodec_free_context(&d->ctx);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    av_frame_free(&d->scratch);
    av_free(d);
}

// fourcc: 'WMV3' or 'WVC1'. extradata: the sequence header the game's decoder read at its start.
void* lsw_vc1_open(uint32_t fourcc, const uint8_t* extradata, int extradata_size, int width, int height) {
    const AVCodec* codec = avcodec_find_decoder(fourcc == 0x57564331u ? AV_CODEC_ID_VC1 : AV_CODEC_ID_WMV3);
    if (!codec) return NULL;
    LswVc1* d = (LswVc1*)av_mallocz(sizeof *d);
    if (!d) return NULL;
    d->ctx = avcodec_alloc_context3(codec);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->scratch = av_frame_alloc();
    if (!d->ctx || !d->pkt || !d->frame || !d->scratch) { lsw_vc1_close(d); return NULL; }
    d->ctx->width = width;
    d->ctx->height = height;
    d->ctx->codec_tag = fourcc;
    d->ctx->thread_count = 1;
    if (extradata_size > 0) {
        d->ctx->extradata = (uint8_t*)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!d->ctx->extradata) { lsw_vc1_close(d); return NULL; }
        memcpy(d->ctx->extradata, extradata, extradata_size);
        d->ctx->extradata_size = extradata_size;
    }
    if (avcodec_open2(d->ctx, codec, NULL) < 0) { lsw_vc1_close(d); return NULL; }
    return d;
}

// One compressed frame. Returns 1 when a new picture came out, 0 when none did (the decoder holds it back, or the
// frame was a skipped one), a negative FFmpeg error otherwise.
int lsw_vc1_decode(void* h, const uint8_t* data, int size) {
    LswVc1* d = (LswVc1*)h;
    if (!d) return -1;
    if (av_new_packet(d->pkt, size) < 0) return -1;
    memcpy(d->pkt->data, data, size);
    int r = avcodec_send_packet(d->ctx, d->pkt);
    av_packet_unref(d->pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) return r;
    int got = 0;
    while (avcodec_receive_frame(d->ctx, d->scratch) == 0) {
        av_frame_unref(d->frame);
        av_frame_move_ref(d->frame, d->scratch);
        got = 1;
        d->have = 1;
    }
    return got;
}

// The latest picture as I420 (Y, then U, then V, each plane packed) or YV12 (V before U), width x height.
// Returns 0 when there is no picture yet.
int lsw_vc1_copy_planar(void* h, uint8_t* dst, int width, int height, int v_first) {
    LswVc1* d = (LswVc1*)h;
    if (!d || !d->have || d->frame->format != AV_PIX_FMT_YUV420P) return 0;
    const AVFrame* f = d->frame;
    const int w = width < f->width ? width : f->width, hh = height < f->height ? height : f->height;
    for (int y = 0; y < hh; ++y) memcpy(dst + (size_t)y * width, f->data[0] + (size_t)y * f->linesize[0], w);
    uint8_t* u = dst + (size_t)width * height;
    uint8_t* v = u + (size_t)(width / 2) * (height / 2);
    if (v_first) { uint8_t* t = u; u = v; v = t; }
    for (int y = 0; y < hh / 2; ++y) {
        memcpy(u + (size_t)y * (width / 2), f->data[1] + (size_t)y * f->linesize[1], w / 2);
        memcpy(v + (size_t)y * (width / 2), f->data[2] + (size_t)y * f->linesize[2], w / 2);
    }
    return 1;
}
