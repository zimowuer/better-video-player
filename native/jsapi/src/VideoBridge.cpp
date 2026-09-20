// 更好的视频播放器 · 原生 video 模块
// FFmpeg(4.4, libavcodec 58) 解码 + libswscale 缩放 + /dev/fb0 直写（竖屏 270° 旋转）+ ALSA 出声。
// 所有第三方库运行时 dlopen（头文件 vendored 于 native/jsapi/ffmpeg-include，无链接期依赖）；
// 目标固件缺库时 play 返回明确错误，应用可优雅降级。
// 音视频事实来源：真机探测 profile（profiles/youdao-dictpen-coco1826.md）。
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VideoBridge.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <linux/fb.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <string>

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
}

namespace {

// ---------------------------------------------------------------------------
// 函数指针类型（dlopen 后解析；符号缺失即判定库不可用）
// ---------------------------------------------------------------------------

#define FNP(name) typedef decltype(&name) pfn_##name

FNP(av_frame_alloc); FNP(av_frame_free);
FNP(av_packet_alloc); FNP(av_packet_free); FNP(av_packet_unref);
FNP(av_strerror);
FNP(avformat_open_input); FNP(avformat_find_stream_info); FNP(avformat_close_input);
FNP(av_read_frame); FNP(av_seek_frame);
FNP(av_find_best_stream);
FNP(avcodec_find_decoder); FNP(avcodec_alloc_context3);
FNP(avcodec_parameters_to_context); FNP(avcodec_open2);
FNP(avcodec_send_packet); FNP(avcodec_receive_frame);
FNP(avcodec_flush_buffers); FNP(avcodec_free_context);
FNP(sws_getContext); FNP(sws_scale); FNP(sws_freeContext);
FNP(swr_alloc_set_opts); FNP(swr_init); FNP(swr_convert); FNP(swr_free);
FNP(av_frame_unref);

// ALSA 没有头文件，手动声明签名（C ABI 稳定）
typedef void snd_pcm_t;
typedef int (*pfn_snd_pcm_open)(snd_pcm_t**, const char*, int, int);
typedef int (*pfn_snd_pcm_set_params)(snd_pcm_t*, int, int, unsigned int, unsigned int, int, unsigned int);
typedef long (*pfn_snd_pcm_writei)(snd_pcm_t*, const void*, unsigned long);
typedef int (*pfn_snd_pcm_prepare)(snd_pcm_t*);
typedef int (*pfn_snd_pcm_drop)(snd_pcm_t*);
typedef int (*pfn_snd_pcm_close)(snd_pcm_t*);
typedef const char* (*pfn_snd_strerror)(int);

struct Libs {
    void *avformat, *avcodec, *avutil, *swscale, *swresample, *alsa;
    pfn_av_frame_alloc av_frame_alloc_;
    pfn_av_frame_free av_frame_free_;
    pfn_av_packet_alloc av_packet_alloc_;
    pfn_av_packet_free av_packet_free_;
    pfn_av_packet_unref av_packet_unref_;
    pfn_av_strerror av_strerror_;
    pfn_avformat_open_input avformat_open_input_;
    pfn_avformat_find_stream_info avformat_find_stream_info_;
    pfn_avformat_close_input avformat_close_input_;
    pfn_av_read_frame av_read_frame_;
    pfn_av_seek_frame av_seek_frame_;
    pfn_av_find_best_stream av_find_best_stream_;
    pfn_avcodec_find_decoder avcodec_find_decoder_;
    pfn_avcodec_alloc_context3 avcodec_alloc_context3_;
    pfn_avcodec_parameters_to_context avcodec_parameters_to_context_;
    pfn_avcodec_open2 avcodec_open2_;
    pfn_avcodec_send_packet avcodec_send_packet_;
    pfn_avcodec_receive_frame avcodec_receive_frame_;
    pfn_avcodec_flush_buffers avcodec_flush_buffers_;
    pfn_avcodec_free_context avcodec_free_context_;
    pfn_sws_getContext sws_getContext_;
    pfn_sws_scale sws_scale_;
    pfn_sws_freeContext sws_freeContext_;
    pfn_swr_alloc_set_opts swr_alloc_set_opts_;
    pfn_swr_init swr_init_;
    pfn_swr_convert swr_convert_;
    pfn_swr_free swr_free_;
    pfn_av_frame_unref av_frame_unref_;
    pfn_snd_pcm_open snd_pcm_open_;
    pfn_snd_pcm_set_params snd_pcm_set_params_;
    pfn_snd_pcm_writei snd_pcm_writei_;
    pfn_snd_pcm_prepare snd_pcm_prepare_;
    pfn_snd_pcm_drop snd_pcm_drop_;
    pfn_snd_pcm_close snd_pcm_close_;
    pfn_snd_strerror snd_strerror_;
    bool ok;
    std::string error;

    Libs() : ok(false) { memset(this, 0, sizeof(*this)); }
};

template <typename T>
static bool bindSym(T& slot, void* handle, const char* name, std::string& err) {
    slot = reinterpret_cast<T>(dlsym(handle, name));
    if (!slot) { err = std::string("symbol-missing:") + name; return false; }
    return true;
}

bool loadLibsOnce(Libs& L) {
    if (L.ok) return true;
    if (!L.error.empty()) return false;

    const char* avfmtNames[] = {"libavformat.so.58", "libavformat.so", NULL};
    const char* avcodecNames[] = {"libavcodec.so.58", "libavcodec.so", NULL};
    const char* avutilNames[] = {"libavutil.so.56", "libavutil.so", NULL};
    const char* swsNames[] = {"libswscale.so.5", "libswscale.so", NULL};
    const char* swrNames[] = {"libswresample.so.3", "libswresample.so", NULL};
    const char* alsaNames[] = {"libasound.so.2", "libasound.so", NULL};

    for (int i = 0; avfmtNames[i] && !L.avformat; i++) L.avformat = dlopen(avfmtNames[i], RTLD_NOW | RTLD_GLOBAL);
    for (int i = 0; avutilNames[i] && !L.avutil; i++) L.avutil = dlopen(avutilNames[i], RTLD_NOW | RTLD_GLOBAL);
    for (int i = 0; avcodecNames[i] && !L.avcodec; i++) L.avcodec = dlopen(avcodecNames[i], RTLD_NOW | RTLD_GLOBAL);
    for (int i = 0; swsNames[i] && !L.swscale; i++) L.swscale = dlopen(swsNames[i], RTLD_NOW | RTLD_GLOBAL);
    for (int i = 0; swrNames[i] && !L.swresample; i++) L.swresample = dlopen(swrNames[i], RTLD_NOW | RTLD_GLOBAL);
    for (int i = 0; alsaNames[i] && !L.alsa; i++) L.alsa = dlopen(alsaNames[i], RTLD_NOW | RTLD_GLOBAL);

    if (!L.avformat || !L.avcodec || !L.avutil || !L.swscale || !L.swresample) {
        L.error = "ffmpeg-missing";
        return false;
    }

#define BIND(field, handle, name) \
    do { if (!bindSym(field, handle, name, L.error)) return false; } while (0)

    BIND(L.av_frame_alloc_, L.avutil, "av_frame_alloc");
    BIND(L.av_frame_free_, L.avutil, "av_frame_free");
    BIND(L.av_packet_alloc_, L.avcodec, "av_packet_alloc");
    BIND(L.av_packet_free_, L.avcodec, "av_packet_free");
    BIND(L.av_packet_unref_, L.avcodec, "av_packet_unref");
    BIND(L.av_strerror_, L.avutil, "av_strerror");

    BIND(L.avformat_open_input_, L.avformat, "avformat_open_input");
    BIND(L.avformat_find_stream_info_, L.avformat, "avformat_find_stream_info");
    BIND(L.avformat_close_input_, L.avformat, "avformat_close_input");
    BIND(L.av_read_frame_, L.avformat, "av_read_frame");
    BIND(L.av_seek_frame_, L.avformat, "av_seek_frame");
    BIND(L.av_find_best_stream_, L.avformat, "av_find_best_stream");

    BIND(L.avcodec_find_decoder_, L.avcodec, "avcodec_find_decoder");
    BIND(L.avcodec_alloc_context3_, L.avcodec, "avcodec_alloc_context3");
    BIND(L.avcodec_parameters_to_context_, L.avcodec, "avcodec_parameters_to_context");
    BIND(L.avcodec_open2_, L.avcodec, "avcodec_open2");
    BIND(L.avcodec_send_packet_, L.avcodec, "avcodec_send_packet");
    BIND(L.avcodec_receive_frame_, L.avcodec, "avcodec_receive_frame");
    BIND(L.avcodec_flush_buffers_, L.avcodec, "avcodec_flush_buffers");
    BIND(L.avcodec_free_context_, L.avcodec, "avcodec_free_context");

    BIND(L.sws_getContext_, L.swscale, "sws_getContext");
    BIND(L.sws_scale_, L.swscale, "sws_scale");
    BIND(L.sws_freeContext_, L.swscale, "sws_freeContext");

    BIND(L.swr_alloc_set_opts_, L.swresample, "swr_alloc_set_opts");
    BIND(L.swr_init_, L.swresample, "swr_init");
    BIND(L.swr_convert_, L.swresample, "swr_convert");
    BIND(L.swr_free_, L.swresample, "swr_free");
    BIND(L.av_frame_unref_, L.avutil, "av_frame_unref");

    if (L.alsa) {
        bool alsaOk = true;
        alsaOk = alsaOk && bindSym(L.snd_pcm_open_, L.alsa, "snd_pcm_open", L.error);
        alsaOk = alsaOk && bindSym(L.snd_pcm_set_params_, L.alsa, "snd_pcm_set_params", L.error);
        alsaOk = alsaOk && bindSym(L.snd_pcm_writei_, L.alsa, "snd_pcm_writei", L.error);
        alsaOk = alsaOk && bindSym(L.snd_pcm_close_, L.alsa, "snd_pcm_close", L.error);
        L.snd_pcm_prepare_ = reinterpret_cast<pfn_snd_pcm_prepare>(dlsym(L.alsa, "snd_pcm_prepare"));
        L.snd_pcm_drop_ = reinterpret_cast<pfn_snd_pcm_drop>(dlsym(L.alsa, "snd_pcm_drop"));
        L.snd_strerror_ = reinterpret_cast<pfn_snd_strerror>(dlsym(L.alsa, "snd_strerror"));
        if (!alsaOk) { L.alsa = NULL; L.error.clear(); }  // ALSA 缺失按无声视频处理
    }

    L.ok = true;
    return true;
}

// ---------------------------------------------------------------------------
// fb0 直写（物理 254x800 竖屏，逻辑横屏 800x254，cfg.json direction=270）
// ---------------------------------------------------------------------------

struct FbWriter {
    int fd;
    uint8_t* mem;
    size_t memSize;
    int stridePx;    // 一行像素数（含 padding，真机 256）
    int physW;       // 可见物理宽（真机 254）
    int physH;       // 可见物理高（真机 800）
    int virtH;       // 虚拟高（双缓冲 = 2*physH）
    int landscapeW;  // 逻辑宽（真机 800）
    bool valid;
    std::string error;

    FbWriter() : fd(-1), mem(NULL), memSize(0), stridePx(0), physW(0), physH(0), virtH(0),
                 landscapeW(800), valid(false) {}

    bool open() {
        if (valid) return true;
        fd = ::open("/dev/fb0", O_RDWR);
        if (fd < 0) { error = "fb0-open-failed"; return false; }
        fb_var_screeninfo vinfo;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0) { error = "fb-vinfo-failed"; ::close(fd); fd = -1; return false; }
        fb_fix_screeninfo finfo;
        if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) { error = "fb-finfo-failed"; ::close(fd); fd = -1; return false; }
        if (vinfo.bits_per_pixel != 32) { error = "fb-not-32bpp"; ::close(fd); fd = -1; return false; }
        physW = (int)vinfo.xres;
        physH = (int)vinfo.yres;
        stridePx = (int)(finfo.line_length / 4);
        int virtY = (int)vinfo.yres_virtual;
        if (virtY <= 0) virtY = physH;
        virtH = virtY;
        memSize = finfo.smem_len > 0 ? (size_t)finfo.smem_len : (size_t)stridePx * (size_t)virtY * 4;
        mem = (uint8_t*)mmap(NULL, memSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mem == MAP_FAILED) { error = "fb-mmap-failed"; mem = NULL; ::close(fd); fd = -1; return false; }
        valid = true;
        return true;
    }

    void close() {
        if (mem) { munmap(mem, memSize); mem = NULL; }
        if (fd >= 0) { ::close(fd); fd = -1; }
        valid = false;
    }

    uint32_t* visibleBase() {
        fb_var_screeninfo vinfo;
        int yoff = 0;
        if (fd >= 0 && ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0) yoff = (int)vinfo.yoffset;
        return (uint32_t*)(mem + (size_t)yoff * (size_t)stridePx * 4);
    }

    // 逻辑横屏 BGRA 缓冲 (w x h) 写到逻辑矩形 (lx, ly, w, h)。
    // direction=270 映射：物理列 = ly + j，物理行 = (landscapeW-1) - (lx + i)。
    // 运行时是双缓冲（yres_virtual = 2 * yres）并按 yoffset 平移，因此两个 buffer 都写，
    // 保证运行时切缓冲后画面仍在。
    void blitLandscape(const uint8_t* src, int srcStridePx, int lx, int ly, int w, int h) {
        if (!valid || w <= 0 || h <= 0 || !src) return;
        const int flip = landscapeW;
        for (int buf = 0; buf * physH < virtH; buf++) {
            uint32_t* dst = (uint32_t*)(mem + (size_t)buf * (size_t)physH * (size_t)stridePx * 4);
            for (int j = 0; j < h; j++) {
                const int col = ly + j;
                if (col < 0 || col >= physW) continue;
                const uint32_t* srow = (const uint32_t*)(src + (size_t)j * (size_t)srcStridePx * 4);
                uint32_t* dcol = dst + col;
                for (int i = 0; i < w; i++) {
                    const int row = flip - 1 - (lx + i);
                    if (row < 0 || row >= physH) continue;
                    dcol[(size_t)row * (size_t)stridePx] = srow[i];
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// 播放会话
// ---------------------------------------------------------------------------

static Libs g_libs;

struct AudioOut {
    void* pcm;
    unsigned int outRate;
    bool ok;

    AudioOut() : pcm(NULL), outRate(48000), ok(false) {}
};

static void audioClose(AudioOut& ao) {
    if (ao.pcm && g_libs.snd_pcm_close_) {
        if (g_libs.snd_pcm_drop_) g_libs.snd_pcm_drop_(ao.pcm);
        g_libs.snd_pcm_close_(ao.pcm);
    }
    ao.pcm = NULL;
    ao.ok = false;
}

static bool audioOpen(AudioOut& ao, int srcRate, double rateF) {
    ao.pcm = NULL;
    ao.ok = false;
    if (!g_libs.snd_pcm_open_) return false;
    int err = g_libs.snd_pcm_open_(&ao.pcm, "default", 0 /*playback*/, 0 /*block*/);
    if (err < 0) { ao.pcm = NULL; return false; }
    ao.outRate = (unsigned int)((double)srcRate * rateF + 0.5);
    if (ao.outRate < 8000) ao.outRate = (unsigned int)srcRate;
    // S16_LE, 交错, 2ch, 软重采样允许, 500ms 缓冲（大缓冲减少视频 blit 阻塞导致的 underrun 爆音）
    err = g_libs.snd_pcm_set_params_(ao.pcm, 2 /*S16_LE*/, 3 /*RW_INTERLEAVED*/,
                                     2, ao.outRate, 1, 500000);
    if (err < 0) { audioClose(ao); return false; }
    // 预填 150ms 静音，让缓冲一开始就有深度，避免 blit 阻塞导致 underrun 爆音
    if (g_libs.snd_pcm_writei_) {
        int prefill = (int)(ao.outRate * 0.15);
        int16_t* silence = (int16_t*)calloc((size_t)prefill * 2, sizeof(int16_t));
        if (silence) {
            g_libs.snd_pcm_writei_(ao.pcm, silence, (unsigned long)prefill);
            free(silence);
        }
    }
    ao.ok = true;
    return true;
}

static void applyVolume(int16_t* samples, int frames, int volume) {
    if (volume >= 100) return;
    const double gain = (double)(volume * volume) / 10000.0;
    const int n = frames * 2;
    for (int i = 0; i < n; i++) {
        double v = (double)samples[i] * gain;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        samples[i] = (int16_t)v;
    }
}

struct Session {
    volatile bool active;
    volatile bool paused;
    volatile bool eos;
    volatile bool stopFlag;
    volatile int ratePermillage;  // 1000 = 1.0x
    volatile int volume;          // 0..100
    std::string mediaPath;
    std::string lastError;

    int videoW, videoH;
    int targetW, targetH;
    int64_t durationMs;

    FbWriter fb;
    int rectLX, rectLY, rectW, rectH;   // JS 指定的视频区域（逻辑横屏坐标）
    int blitX, blitY;                   // 实际 blit 原点（区域内居中后）

    pthread_t thread;
    bool threadStarted;

    pthread_mutex_t mutex;
    volatile bool seekPending;
    volatile int64_t seekTargetMs;
    volatile bool viewDirty;   // setView 后需要重算缩放与 blit 区域

    double basePosMs;      // 时钟基准（seek 目标 / 0）
    double contentPosMs;   // 内容时间（UI 进度，倍速后）
    double wallBaseMs;
    bool clockByAudio;

    Session()
        : active(false), paused(false), eos(false), stopFlag(false),
          ratePermillage(1000), volume(70), videoW(0), videoH(0), targetW(0), targetH(0),
          durationMs(0), rectLX(0), rectLY(0), rectW(0), rectH(0), blitX(0), blitY(0), threadStarted(false),
          seekPending(false), seekTargetMs(0), viewDirty(false), basePosMs(0), contentPosMs(0), wallBaseMs(0), clockByAudio(false) {
        pthread_mutex_init(&mutex, NULL);
    }

    double nowMs() {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
    }
};

static Session* g_session = NULL;
static pthread_mutex_t g_sessionMutex = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// 解码线程
// ---------------------------------------------------------------------------

struct DecodeCtx {
    AVFormatContext* fmt;
    AVCodecContext* vctx;
    AVCodecContext* actx;
    int vStream;
    int aStream;
    AVFrame* frame;
    AVPacket* pkt;
    SwsContext* sws;
    int swsSrcFormat;  // sws 按该输入像素格式建立
    int swsSrcW;
    SwrContext* swr;
    int swrInFormat;      // swr 按该输入格式建立
    int swrInRate;
    uint64_t swrInLayout;
    int swrOutRate;
    AVRational vTimeBase;

    DecodeCtx() : fmt(NULL), vctx(NULL), actx(NULL), vStream(-1), aStream(-1),
                  frame(NULL), pkt(NULL), sws(NULL), swsSrcFormat(-1), swsSrcW(0), swr(NULL),
                  swrInFormat(-1), swrInRate(0), swrInLayout(0), swrOutRate(0) {
        vTimeBase.num = 0; vTimeBase.den = 0;
    }
};

static void decodeCtxClose(DecodeCtx& d) {
    if (d.sws) { g_libs.sws_freeContext_(d.sws); d.sws = NULL; }
    if (d.swr) { g_libs.swr_free_(&d.swr); d.swr = NULL; }
    if (d.vctx) g_libs.avcodec_free_context_(&d.vctx);
    if (d.actx) g_libs.avcodec_free_context_(&d.actx);
    if (d.frame) g_libs.av_frame_free_(&d.frame);
    if (d.pkt) g_libs.av_packet_free_(&d.pkt);
    if (d.fmt) g_libs.avformat_close_input_(&d.fmt);
}

// swr 重建（输入格式/率/声道布局或倍速变化时）
static bool ensureSwr(DecodeCtx& d, AVFrame* frame, int ratePermillage) {
    const int wantRate = (int)((double)frame->sample_rate * ((double)ratePermillage / 1000.0) + 0.5);
    if (d.swr && d.swrInFormat == frame->format && d.swrInRate == frame->sample_rate &&
        d.swrInLayout == (frame->channel_layout ? frame->channel_layout : 3) &&
        d.swrOutRate == wantRate) {
        return true;
    }
    if (d.swr) {
        g_libs.swr_free_(&d.swr);
        d.swr = NULL;
    }
    uint64_t inLayout = frame->channel_layout;
    if (inLayout == 0) inLayout = frame->channels == 1 ? 1 : 3;  // MONO / STEREO
    const int outRate = (int)((double)frame->sample_rate * ((double)ratePermillage / 1000.0) + 0.5);
    d.swr = g_libs.swr_alloc_set_opts_(NULL,
                                       3 /*stereo*/, (AVSampleFormat)AV_SAMPLE_FMT_S16, outRate,
                                       (int64_t)inLayout, (AVSampleFormat)frame->format, frame->sample_rate,
                                       0, NULL);
    if (!d.swr) return false;
    if (g_libs.swr_init_(d.swr) < 0) { g_libs.swr_free_(&d.swr); d.swr = NULL; return false; }
    d.swrInFormat = frame->format;
    d.swrInRate = frame->sample_rate;
    d.swrInLayout = inLayout;
    d.swrOutRate = outRate;
    return true;
}

static void* decodeThread(void* arg) {
    Session& s = *reinterpret_cast<Session*>(arg);
    DecodeCtx d;
    AudioOut ao;
    uint8_t* scaleBuf = NULL;
    int scaleBufSize = 0;
    int16_t* outSamples = NULL;
    size_t outSamplesBytes = 0;
    bool opened = false;
    int capMaxW = 0;   // JS 画质上限（首帧探测后设置）
    int capMaxH = 0;

    // 目标尺寸（降画质）与 blit 原点：可重复计算（setView 切换视频区域时重算）
    //   可用区域 = JS 传入的视频矩形 rectW×rectH（逻辑横屏坐标），缺省全屏 800×254
    //   取「原始尺寸、画质上限、可用区域」三者最小并等比缩放，不放大，区域内居中。
    auto recomputeTarget = [&]() {
        const int availW = (s.rectW > 0 && s.rectW <= 800) ? s.rectW : 800;
        const int availH = (s.rectH > 0 && s.rectH <= 254) ? s.rectH : 254;
        const int capW = capMaxW > 0 ? capMaxW : s.videoW;
        const int capH = capMaxH > 0 ? capMaxH : s.videoH;
        int tw = s.videoW > 0 ? s.videoW : capW;
        int th = s.videoH > 0 ? s.videoH : capH;
        if (capW > 0 && capW < tw) { th = th * capW / tw; tw = capW; }
        if (capH > 0 && capH < th) { tw = tw * capH / th; th = capH; }
        if (tw > availW) { th = th * availW / tw; tw = availW; }
        if (th > availH) { tw = tw * availH / th; th = availH; }
        if (tw % 2) tw--;
        if (th % 2) th--;
        s.targetW = tw > 0 ? tw : 0;
        s.targetH = th > 0 ? th : 0;
        s.blitX = s.rectLX + (availW - s.targetW) / 2;
        s.blitY = s.rectLY + (availH - s.targetH) / 2;
    };

    // ---- 打开 ----
    if (g_libs.avformat_open_input_(&d.fmt, s.mediaPath.c_str(), NULL, NULL) != 0) {
        s.lastError = "open-failed";
        goto done;
    }
    if (g_libs.avformat_find_stream_info_(d.fmt, NULL) < 0) {
        s.lastError = "stream-info-failed";
        goto done;
    }
    d.vStream = g_libs.av_find_best_stream_(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    d.aStream = g_libs.av_find_best_stream_(d.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (d.vStream < 0 && d.aStream < 0) { s.lastError = "no-stream"; goto done; }
    if (d.fmt->duration > 0) s.durationMs = d.fmt->duration / 1000;

    if (d.vStream >= 0) {
        AVStream* st = d.fmt->streams[d.vStream];
        const AVCodec* codec = g_libs.avcodec_find_decoder_(st->codecpar->codec_id);
        if (codec) {
            d.vctx = g_libs.avcodec_alloc_context3_(codec);
            g_libs.avcodec_parameters_to_context_(d.vctx, st->codecpar);
            if (g_libs.avcodec_open2_(d.vctx, codec, NULL) == 0) {
                s.videoW = st->codecpar->width;
                s.videoH = st->codecpar->height;
                d.vTimeBase = st->time_base;
                if (s.durationMs <= 0 && st->duration > 0) {
                    s.durationMs = (int64_t)((double)st->duration * 1000.0 * (double)st->time_base.num / (double)st->time_base.den);
                }
            } else {
                g_libs.avcodec_free_context_(&d.vctx);
                d.vctx = NULL;
            }
        }
        if (!d.vctx) d.vStream = -1;
    }
    if (d.aStream >= 0) {
        AVStream* st = d.fmt->streams[d.aStream];
        const AVCodec* codec = g_libs.avcodec_find_decoder_(st->codecpar->codec_id);
        if (codec) {
            d.actx = g_libs.avcodec_alloc_context3_(codec);
            g_libs.avcodec_parameters_to_context_(d.actx, st->codecpar);
            if (g_libs.avcodec_open2_(d.actx, codec, NULL) != 0) {
                g_libs.avcodec_free_context_(&d.actx);
                d.actx = NULL;
            }
        }
        if (!d.actx) d.aStream = -1;
    }
    if (d.vStream < 0 && d.aStream < 0) { s.lastError = "no-decoder"; goto done; }

    d.frame = g_libs.av_frame_alloc_();
    d.pkt = g_libs.av_packet_alloc_();
    if (!d.frame || !d.pkt) { s.lastError = "alloc-failed"; goto done; }

    capMaxW = s.targetW;
    capMaxH = s.targetH;
    recomputeTarget();
    if (d.vStream >= 0 && s.targetW > 0 && s.targetH > 0) {
        scaleBufSize = s.targetW * s.targetH * 4;
        scaleBuf = (uint8_t*)malloc((size_t)scaleBufSize);
        if (!scaleBuf) { s.lastError = "alloc-failed"; goto done; }
    }
    if (d.aStream >= 0) {
        outSamplesBytes = (size_t)(192000 * 2 * 2);  // 1s@48k 立体声 s16 上限
        outSamples = (int16_t*)malloc(outSamplesBytes);
        // 不在此处预打开 ALSA：等第一帧音频拿到真实采样率再开，
        // 避免 close+reopen 产生的启动咔哒声。
    }

    s.clockByAudio = false;
    s.basePosMs = 0;
    s.wallBaseMs = s.nowMs();
    s.contentPosMs = 0;
    opened = true;

    // ---- 主循环 ----
    {
        bool videoEof = false, audioEof = false;
        bool hasPendingVideo = false;
        double pendingPtsMs = -1;
        bool sentVFlush = false, sentAFlush = false;
        int64_t playedSrcSamples = 0;   // 已送入 swr 的源采样数（进度推算用）
        bool frameReady = false;        // 缩放缓冲里已有可重绘的帧
        double lastBlitMs = 0;

        while (!s.stopFlag) {
            // seek
            if (s.seekPending) {
                pthread_mutex_lock(&s.mutex);
                const int64_t target = s.seekTargetMs;
                s.seekPending = false;
                pthread_mutex_unlock(&s.mutex);
                const int64_t ts = target * 1000;  // ms -> AV_TIME_BASE(us)
                if (d.fmt) g_libs.av_seek_frame_(d.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
                if (d.vctx) g_libs.avcodec_flush_buffers_(d.vctx);
                if (d.actx) g_libs.avcodec_flush_buffers_(d.actx);
                hasPendingVideo = false;
                videoEof = audioEof = false;
                sentVFlush = sentAFlush = false;
                s.eos = false;
                s.basePosMs = (double)target;
                s.wallBaseMs = s.nowMs();
                s.contentPosMs = (double)target;
                playedSrcSamples = 0;
            }

            if (s.viewDirty) {
                s.viewDirty = false;
                recomputeTarget();
                if (d.sws) { g_libs.sws_freeContext_(d.sws); d.sws = NULL; }
                if (scaleBuf && s.targetW > 0 && s.targetH > 0 &&
                    s.targetW * s.targetH * 4 != scaleBufSize) {
                    free(scaleBuf);
                    scaleBufSize = s.targetW * s.targetH * 4;
                    scaleBuf = (uint8_t*)malloc((size_t)scaleBufSize);
                }
                frameReady = false;
            }

            if (s.paused) {
                if (frameReady && s.fb.valid && s.targetW > 0 && s.nowMs() - lastBlitMs > 400) {
                    s.fb.blitLandscape(scaleBuf, s.targetW, s.blitX, s.blitY, s.targetW, s.targetH);
                    lastBlitMs = s.nowMs();
                }
                s.wallBaseMs = s.nowMs();  // 冻结墙钟
                usleep(20000);
                continue;
            }

            bool fedPacket = false;

            // 喂一个包
            if ((!videoEof || !audioEof) && !s.seekPending) {
                int ret = g_libs.av_read_frame_(d.fmt, d.pkt);
                if (ret == 0) {
                    if (d.pkt->stream_index == d.vStream && d.vctx) {
                        g_libs.avcodec_send_packet_(d.vctx, d.pkt);
                    } else if (d.pkt->stream_index == d.aStream && d.actx) {
                        g_libs.avcodec_send_packet_(d.actx, d.pkt);
                    }
                    g_libs.av_packet_unref_(d.pkt);
                    fedPacket = true;
                } else if (ret == AVERROR_EOF) {
                    if (!videoEof && d.vctx && !sentVFlush) { g_libs.avcodec_send_packet_(d.vctx, NULL); sentVFlush = true; }
                    if (!audioEof && d.actx && !sentAFlush) { g_libs.avcodec_send_packet_(d.actx, NULL); sentAFlush = true; }
                    videoEof = audioEof = true;
                    fedPacket = true;
                } else {
                    videoEof = audioEof = true;
                }
            }

            // 收视频帧（只保留最新的可呈现帧，旧帧丢弃 = 追帧）
            if (d.vctx) {
                while (true) {
                    int ret = g_libs.avcodec_receive_frame_(d.vctx, d.frame);
                    if (ret == 0) {
                        double ptsMs = -1;
                        if (d.frame->pts != AV_NOPTS_VALUE && d.vTimeBase.den > 0) {
                            ptsMs = (double)d.frame->pts * 1000.0 * (double)d.vTimeBase.num / (double)d.vTimeBase.den;
                        }
                        if (scaleBuf && s.targetW > 0 && s.targetH > 0) {
                            // 源格式首帧才确定：sws 未建或格式变化时（重）建
                            if (!d.sws || d.swsSrcFormat != d.frame->format || d.swsSrcW != s.videoW) {
                                if (d.sws) g_libs.sws_freeContext_(d.sws);
                                d.sws = g_libs.sws_getContext_(s.videoW, s.videoH, (AVPixelFormat)d.frame->format,
                                                               s.targetW, s.targetH, AV_PIX_FMT_BGRA,
                                                               SWS_BILINEAR, NULL, NULL, NULL);
                                d.swsSrcFormat = d.frame->format;
                                d.swsSrcW = s.videoW;
                            }
                            if (d.sws) {
                                uint8_t* dst[4] = { scaleBuf, NULL, NULL, NULL };
                                int dstStride[4] = { s.targetW * 4, 0, 0, 0 };
                                if (g_libs.sws_scale_(d.sws, d.frame->data, d.frame->linesize, 0, s.videoH, dst, dstStride) > 0) {
                                    if (ptsMs >= 0) {
                                        if (!hasPendingVideo || ptsMs >= pendingPtsMs) {
                                            pendingPtsMs = ptsMs;
                                            hasPendingVideo = true;
                                        }
                                        // 旧帧直接丢弃（缩小后的缓冲被下一帧覆盖前不呈现）
                                    } else {
                                        pendingPtsMs = s.contentPosMs;
                                        hasPendingVideo = true;
                                    }
                                }
                            }
                        }
                        g_libs.av_frame_unref_(d.frame);
                        continue;
                    }
                    break;  // EAGAIN / EOF
                }
            }

            // 收音频帧并播放（ALSA 阻塞写 = 主时钟）
            if (d.actx) {
                while (true) {
                    int ret = g_libs.avcodec_receive_frame_(d.actx, d.frame);
                    if (ret == 0) {
                        const int inFrames = d.frame->nb_samples;
                        if (inFrames > 0 && outSamples && !s.seekPending) {
                            if (ensureSwr(d, d.frame, s.ratePermillage)) {
                                if (d.frame->sample_rate > 0 && !ao.ok) {
                                    audioClose(ao);
                                    audioOpen(ao, d.frame->sample_rate, (double)s.ratePermillage / 1000.0);
                                }
                                const uint8_t* in[8] = { NULL };
                                in[0] = d.frame->extended_data[0];
                                if ((d.frame->format == AV_SAMPLE_FMT_U8P || d.frame->format == AV_SAMPLE_FMT_S16P ||
                                     d.frame->format == AV_SAMPLE_FMT_S32P || d.frame->format == AV_SAMPLE_FMT_FLTP ||
                                     d.frame->format == AV_SAMPLE_FMT_DBLP) && d.frame->channels > 1) {
                                    // planar 多声道：swr 可从 extended_data 连续平面读取
                                    in[1] = d.frame->extended_data[1];
                                    in[2] = d.frame->extended_data[2];
                                    in[3] = d.frame->extended_data[3];
                                    in[4] = d.frame->extended_data[4];
                                    in[5] = d.frame->extended_data[5];
                                    in[6] = d.frame->extended_data[6];
                                    in[7] = d.frame->extended_data[7];
                                }
                                int outFrames = g_libs.swr_convert_(d.swr, (uint8_t**)&outSamples,
                                                                    (int)(outSamplesBytes / 4),
                                                                    in, inFrames);
                                // swr 建立后：按实际源率与当前倍速校准 ALSA（倍速变化时也重开）
                                if (d.swr && d.frame->sample_rate > 0 && (!ao.ok || ao.outRate != (unsigned int)d.swrOutRate)) {
                                    audioClose(ao);
                                    audioOpen(ao, d.frame->sample_rate, (double)s.ratePermillage / 1000.0);
                                }
                                if (outFrames > 0 && ao.ok && g_libs.snd_pcm_writei_) {
                                    applyVolume(outSamples, outFrames, s.volume);
                                    int16_t* p = outSamples;
                                    int remain = outFrames;
                                    int failStreak = 0;
                                    while (remain > 0 && !s.stopFlag && !s.seekPending && !s.paused) {
                                        long wrote = g_libs.snd_pcm_writei_(ao.pcm, p, (unsigned long)remain);
                                        if (wrote > 0) {
                                            p += wrote * 2;
                                            remain -= (int)wrote;
                                            failStreak = 0;
                                        } else if (wrote == -11 /*EAGAIN*/) {
                                            usleep(4000);
                                        } else if (++failStreak <= 10) {
                                            /* xrun: drop stale buffer then prepare to restart clean */
                                            if (g_libs.snd_pcm_drop_) g_libs.snd_pcm_drop_(ao.pcm);
                                            if (g_libs.snd_pcm_prepare_ && g_libs.snd_pcm_prepare_(ao.pcm) == 0) continue;
                                            ao.ok = false; break;
                                        } else {
                                            ao.ok = false;  // 放弃音频，转墙钟
                                            break;
                                        }
                                    }
                                }
                                playedSrcSamples += inFrames;
                                // 进度：basePos + 已解码源采样 / 源率（内容时间），墙钟同步
                                if (d.frame->sample_rate > 0) {
                                    s.contentPosMs = s.basePosMs + (double)playedSrcSamples / (double)d.frame->sample_rate * 1000.0;
                                    s.wallBaseMs = s.nowMs();
                                    s.clockByAudio = true;
                                }
                            }
                        }
                        g_libs.av_frame_unref_(d.frame);
                        continue;
                    }
                    break;
                }
            }

            // 呈现视频帧
            if (hasPendingVideo) {
                double clock;
                if (s.clockByAudio) {
                    clock = s.contentPosMs + (s.nowMs() - s.wallBaseMs) * ((double)s.ratePermillage / 1000.0);
                } else {
                    clock = s.basePosMs + (s.nowMs() - s.wallBaseMs) * ((double)s.ratePermillage / 1000.0);
                    s.contentPosMs = clock;
                }
                if (pendingPtsMs <= clock + 20.0) {
                    if (s.fb.valid && s.targetW > 0 && s.targetH > 0) {
                        s.fb.blitLandscape(scaleBuf, s.targetW, s.blitX, s.blitY, s.targetW, s.targetH);
                        frameReady = true;
                        lastBlitMs = s.nowMs();
                    }
                    hasPendingVideo = false;
                } else {
                    usleep(3000);
                }
            } else if (!fedPacket) {
                if (frameReady && s.fb.valid && s.targetW > 0 && s.nowMs() - lastBlitMs > 250) {
                    // UI 重绘可能把视频区刷黑，周期性重绘上一帧兜底
                    s.fb.blitLandscape(scaleBuf, s.targetW, s.blitX, s.blitY, s.targetW, s.targetH);
                    lastBlitMs = s.nowMs();
                }
                if ((videoEof || d.vStream < 0) && (audioEof || d.aStream < 0)) {
                    if (!s.eos) s.eos = true;
                    usleep(50000);
                } else {
                    usleep(2000);
                }
            }
        }
    }

done:
    (void)opened;
    audioClose(ao);
    decodeCtxClose(d);
    if (scaleBuf) free(scaleBuf);
    if (outSamples) free(outSamples);
    s.active = false;
    return NULL;
}

// ---------------------------------------------------------------------------
// 会话控制
// ---------------------------------------------------------------------------

static void sessionStopAndFree(Session* s) {
    if (!s) return;
    s->stopFlag = true;
    if (s->threadStarted) {
        pthread_join(s->thread, NULL);
        s->threadStarted = false;
    }
    /* Clear ENTIRE framebuffer to black so no video pixels linger after exit */
    if (s->fb.valid && s->fb.mem && s->fb.memSize > 0) {
        memset(s->fb.mem, 0, s->fb.memSize);
    }
    s->fb.close();
    delete s;
}

// ---------------------------------------------------------------------------
// JS 胶水
// ---------------------------------------------------------------------------

JSValue retOk(JSContext* ctx) {
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, true));
    return r;
}

JSValue retErr(JSContext* ctx, const char* msg) {
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, r, "error", JS_NewString(ctx, msg ? msg : "unknown"));
    return r;
}

bool getPathArg(JSContext* ctx, int argc, JSValueConst* argv, std::string& path) {
    if (argc < 1 || !JS_IsObject(argv[0])) return false;
    JSValue v = JS_GetPropertyStr(ctx, argv[0], "path");
    if (JS_IsException(v)) return false;
    const char* c = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!c) return false;
    path.assign(c);
    JS_FreeCString(ctx, c);
    if (path.empty() || path.size() > 768) return false;
    for (size_t i = 0; i < path.size(); i++) {
        const unsigned char ch = (unsigned char)path[i];
        if (ch < 0x20 && ch != '\t') return false;
    }
    return true;
}

int getIntArg(JSContext* ctx, JSValueConst obj, const char* name, int def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int out = def;
    if (!JS_IsException(v) && !JS_IsUndefined(v) && !JS_IsNull(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

JSValue videoPlay(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return retErr(ctx, "bad-args");
    std::string path;
    if (!getPathArg(ctx, argc, argv, path)) return retErr(ctx, "bad-path");
    if (!loadLibsOnce(g_libs)) return retErr(ctx, g_libs.error.c_str());

    pthread_mutex_lock(&g_sessionMutex);
    if (g_session) { sessionStopAndFree(g_session); g_session = NULL; }

    Session* s = new Session();
    s->mediaPath = path;
    s->rectLX = getIntArg(ctx, argv[0], "rectX", 0);
    s->rectLY = getIntArg(ctx, argv[0], "rectY", 0);
    s->rectW = getIntArg(ctx, argv[0], "rectW", 0);
    s->rectH = getIntArg(ctx, argv[0], "rectH", 0);
    s->targetW = getIntArg(ctx, argv[0], "maxW", 0);
    s->targetH = getIntArg(ctx, argv[0], "maxH", 0);
    s->ratePermillage = getIntArg(ctx, argv[0], "ratePermillage", 1000);
    if (s->ratePermillage < 250) s->ratePermillage = 250;
    if (s->ratePermillage > 4000) s->ratePermillage = 4000;
    s->volume = getIntArg(ctx, argv[0], "volume", 70);
    if (s->volume < 0) s->volume = 0;
    if (s->volume > 100) s->volume = 100;

    if (!s->fb.open()) {
        const std::string err = s->fb.error;
        delete s;
        pthread_mutex_unlock(&g_sessionMutex);
        return retErr(ctx, err.c_str());
    }

    s->stopFlag = false;
    s->active = true;
    if (pthread_create(&s->thread, NULL, decodeThread, s) != 0) {
        s->active = false;
        s->fb.close();
        delete s;
        pthread_mutex_unlock(&g_sessionMutex);
        return retErr(ctx, "thread-failed");
    }
    s->threadStarted = true;
    g_session = s;
    pthread_mutex_unlock(&g_sessionMutex);

    // 等媒体打开（最多 4 秒），把探测结果带回 JS
    for (int i = 0; i < 400; i++) {
        if (g_session != s) break;
        if (!s->lastError.empty() || s->videoW > 0 || s->durationMs > 0 || !s->active) break;
        usleep(10000);
    }
    JSValue r = JS_NewObject(ctx);
    if (g_session != s || !s->lastError.empty()) {
        const std::string err = !s->lastError.empty() ? s->lastError : std::string("open-timeout");
        JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, r, "error", JS_NewString(ctx, err.c_str()));
        return r;  // 失败会话由下次 play/stop 清理
    }
    JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, r, "videoW", JS_NewInt32(ctx, s->videoW));
    JS_SetPropertyStr(ctx, r, "videoH", JS_NewInt32(ctx, s->videoH));
    JS_SetPropertyStr(ctx, r, "targetW", JS_NewInt32(ctx, s->targetW));
    JS_SetPropertyStr(ctx, r, "targetH", JS_NewInt32(ctx, s->targetH));
    JS_SetPropertyStr(ctx, r, "durationMs", JS_NewFloat64(ctx, (double)s->durationMs));
    return r;
}

static Session* sessionRef() {
    pthread_mutex_lock(&g_sessionMutex);
    Session* s = g_session;
    pthread_mutex_unlock(&g_sessionMutex);
    return s;
}

JSValue videoPause(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Session* s = sessionRef();
    if (!s) return retErr(ctx, "not-playing");
    s->paused = true;
    return retOk(ctx);
}

JSValue videoResume(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Session* s = sessionRef();
    if (!s) return retErr(ctx, "not-playing");
    s->paused = false;
    s->wallBaseMs = s->nowMs();
    return retOk(ctx);
}

JSValue videoStop(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    pthread_mutex_lock(&g_sessionMutex);
    Session* s = g_session;
    g_session = NULL;
    pthread_mutex_unlock(&g_sessionMutex);
    sessionStopAndFree(s);
    return retOk(ctx);
}

JSValue videoSeek(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Session* s = sessionRef();
    if (!s) return retErr(ctx, "not-playing");
    double ms = 0;
    if (argc >= 1) JS_ToFloat64(ctx, &ms, argv[0]);
    if (ms < 0) ms = 0;
    pthread_mutex_lock(&s->mutex);
    s->seekTargetMs = (int64_t)ms;
    s->seekPending = true;
    pthread_mutex_unlock(&s->mutex);
    return retOk(ctx);
}

JSValue videoSetRate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Session* s = sessionRef();
    if (!s) return retErr(ctx, "not-playing");
    double rate = 1.0;
    if (argc >= 1) JS_ToFloat64(ctx, &rate, argv[0]);
    if (rate < 0.25) rate = 0.25;
    if (rate > 4.0) rate = 4.0;
    s->ratePermillage = (int)(rate * 1000.0 + 0.5);
    // swr 会在下一帧按新率重建（ensureSwr 检测 ratePermillage 变化）
    return retOk(ctx);
}

JSValue videoSetView(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Session* s = sessionRef();
    if (!s) return retErr(ctx, "not-playing");
    if (argc < 1 || !JS_IsObject(argv[0])) return retErr(ctx, "bad-args");
    JSValueConst o = argv[0];
    s->rectLX = getIntArg(ctx, o, "rectX", s->rectLX);
    s->rectLY = getIntArg(ctx, o, "rectY", s->rectLY);
    const int w = getIntArg(ctx, o, "rectW", s->rectW);
    const int h = getIntArg(ctx, o, "rectH", s->rectH);
    if (w > 0) s->rectW = w;
    if (h > 0) s->rectH = h;
    s->viewDirty = true;
    return retOk(ctx);
}

JSValue videoSetVolume(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Session* s = sessionRef();
    if (!s) return retErr(ctx, "not-playing");
    int v = 70;
    if (argc >= 1) JS_ToInt32(ctx, &v, argv[0]);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s->volume = v;
    return retOk(ctx);
}

JSValue videoGetPosition(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Session* s = sessionRef();
    JSValue r = JS_NewObject(ctx);
    const double ms = s ? s->contentPosMs : 0.0;
    JS_SetPropertyStr(ctx, r, "ms", JS_NewFloat64(ctx, ms));
    JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, !!s));
    return r;
}

JSValue videoGetDuration(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Session* s = sessionRef();
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "ms", JS_NewFloat64(ctx, s ? (double)s->durationMs : 0.0));
    JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, !!s));
    return r;
}

JSValue videoGetStatus(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Session* s = sessionRef();
    JSValue r = JS_NewObject(ctx);
    if (!s) {
        JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, r, "playing", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, r, "eos", JS_NewBool(ctx, false));
        JS_SetPropertyStr(ctx, r, "videoW", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, r, "videoH", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, r, "durationMs", JS_NewFloat64(ctx, 0.0));
        return r;
    }
    JS_SetPropertyStr(ctx, r, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, r, "playing", JS_NewBool(ctx, s->active && !s->paused && !s->eos));
    JS_SetPropertyStr(ctx, r, "paused", JS_NewBool(ctx, s->paused));
    JS_SetPropertyStr(ctx, r, "eos", JS_NewBool(ctx, s->eos));
    JS_SetPropertyStr(ctx, r, "videoW", JS_NewInt32(ctx, s->videoW));
    JS_SetPropertyStr(ctx, r, "videoH", JS_NewInt32(ctx, s->videoH));
    JS_SetPropertyStr(ctx, r, "targetW", JS_NewInt32(ctx, s->targetW));
    JS_SetPropertyStr(ctx, r, "targetH", JS_NewInt32(ctx, s->targetH));
    JS_SetPropertyStr(ctx, r, "ratePermillage", JS_NewInt32(ctx, s->ratePermillage));
    JS_SetPropertyStr(ctx, r, "volume", JS_NewInt32(ctx, s->volume));
    JS_SetPropertyStr(ctx, r, "durationMs", JS_NewFloat64(ctx, (double)s->durationMs));
    return r;
}

int videoModuleInit(JSContext* ctx, JSModuleDef* m) {
    JSValue v = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, v, "play", JS_NewCFunction(ctx, videoPlay, "play", 1));
    JS_SetPropertyStr(ctx, v, "pause", JS_NewCFunction(ctx, videoPause, "pause", 0));
    JS_SetPropertyStr(ctx, v, "resume", JS_NewCFunction(ctx, videoResume, "resume", 0));
    JS_SetPropertyStr(ctx, v, "stop", JS_NewCFunction(ctx, videoStop, "stop", 0));
    JS_SetPropertyStr(ctx, v, "seek", JS_NewCFunction(ctx, videoSeek, "seek", 1));
    JS_SetPropertyStr(ctx, v, "setRate", JS_NewCFunction(ctx, videoSetRate, "setRate", 1));
    JS_SetPropertyStr(ctx, v, "setVolume", JS_NewCFunction(ctx, videoSetVolume, "setVolume", 1));
    JS_SetPropertyStr(ctx, v, "setView", JS_NewCFunction(ctx, videoSetView, "setView", 1));
    JS_SetPropertyStr(ctx, v, "getPosition", JS_NewCFunction(ctx, videoGetPosition, "getPosition", 0));
    JS_SetPropertyStr(ctx, v, "getDuration", JS_NewCFunction(ctx, videoGetDuration, "getDuration", 0));
    JS_SetPropertyStr(ctx, v, "getStatus", JS_NewCFunction(ctx, videoGetStatus, "getStatus", 0));
    JS_SetModuleExport(ctx, m, "default", v);
    return 0;
}

}  // namespace

JSModuleDef* video_module_load(JSContext* ctx, const char* moduleName) {
    if (!moduleName || strcmp(moduleName, "video") != 0) return NULL;
    JSModuleDef* m = JS_NewCModule(ctx, moduleName, videoModuleInit);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
    return m;
}
