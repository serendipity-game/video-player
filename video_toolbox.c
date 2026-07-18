// player_fixed.c - Intel macOS VideoToolbox 硬解修复版
// 编译命令：
// gcc player_fixed.c -o player $(pkg-config --cflags --libs libavformat libavcodec libswscale libswresample libavutil sdl2) -lpthread -lm -O2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>

#include <SDL2/SDL.h>

#define VIDEO_QUEUE_SIZE 6
#define PACKET_QUEUE_SIZE 150
#define NAME "APlayer"

// ================== 队列 ==================
typedef struct {
    AVPacket *pkt[PACKET_QUEUE_SIZE];
    int rindex, windex, size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    atomic_int abort_request;
} PacketQueue;

// ================== 上下文 ==================
typedef struct {
    AVFormatContext *fmt_ctx;
    AVCodecContext *video_ctx;
    AVCodecContext *audio_ctx;
    int video_stream_idx, audio_stream_idx;
    AVFrame *video_frame, *audio_frame;
    struct SwsContext *sws_ctx;
    SwrContext *swr_ctx;
    AVBufferRef *hw_device_ctx;

    PacketQueue video_pkt_queue, audio_pkt_queue;
    AVFrame *video_queue[VIDEO_QUEUE_SIZE];
    int vq_ridx, vq_widx, vq_size;
    pthread_mutex_t vq_mutex;
    pthread_cond_t vq_cond;

    double audio_clock, video_clock;
    atomic_int audio_clock_set;
    atomic_int quit, paused, demux_quit;

    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;

    int width, height;
    AVRational video_time_base;
    double audio_clock_last_time, last_video_pts, frame_duration;
    int sws_src_w, sws_src_h;
    enum AVPixelFormat sws_src_fmt;

    pthread_mutex_t seek_mutex;
    atomic_int seek_request;
    double seek_target;
    
    int frames_decoded, frames_rendered, frames_dropped;
} PlayerContext;

// ================== 队列操作 ==================
static void pktq_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    atomic_store(&q->abort_request, 0);
}
static void pktq_flush(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    for (int i = 0; i < PACKET_QUEUE_SIZE; i++) if (q->pkt[i]) av_packet_free(&q->pkt[i]);
    q->rindex = q->windex = q->size = 0;
    pthread_mutex_unlock(&q->mutex);
}
static void pktq_destroy(PacketQueue *q) { pktq_flush(q); pthread_mutex_destroy(&q->mutex); pthread_cond_destroy(&q->cond); }

static int pktq_put(PacketQueue *q, AVPacket *pkt) {
    pthread_mutex_lock(&q->mutex);
    if (atomic_load(&q->abort_request) || q->size >= PACKET_QUEUE_SIZE) { pthread_mutex_unlock(&q->mutex); return -1; }
    q->pkt[q->windex] = av_packet_clone(pkt);
    if (!q->pkt[q->windex]) { pthread_mutex_unlock(&q->mutex); return -1; }
    q->windex = (q->windex + 1) % PACKET_QUEUE_SIZE; q->size++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}
static int pktq_get(PacketQueue *q, AVPacket *pkt, int block) {
    pthread_mutex_lock(&q->mutex);
    while (q->size == 0 && !atomic_load(&q->abort_request)) {
        if (!block) { pthread_mutex_unlock(&q->mutex); return -1; }
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    if (atomic_load(&q->abort_request)) { pthread_mutex_unlock(&q->mutex); return -1; }
    av_packet_move_ref(pkt, q->pkt[q->rindex]);
    av_packet_free(&q->pkt[q->rindex]);
    q->rindex = (q->rindex + 1) % PACKET_QUEUE_SIZE; q->size--;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static void vframe_enqueue(PlayerContext *ctx, AVFrame *frame) {
    pthread_mutex_lock(&ctx->vq_mutex);
    if (ctx->vq_size < VIDEO_QUEUE_SIZE) {
        ctx->video_queue[ctx->vq_widx] = frame;
        ctx->vq_widx = (ctx->vq_widx + 1) % VIDEO_QUEUE_SIZE;
        ctx->vq_size++;
        pthread_cond_signal(&ctx->vq_cond);
    } else {
        ctx->frames_dropped++;
        av_frame_free(&frame);
    }
    pthread_mutex_unlock(&ctx->vq_mutex);
}
static AVFrame *vframe_dequeue(PlayerContext *ctx) {
    pthread_mutex_lock(&ctx->vq_mutex);
    while (ctx->vq_size == 0 && !atomic_load(&ctx->quit))
        pthread_cond_wait(&ctx->vq_cond, &ctx->vq_mutex);
    AVFrame *f = NULL;
    if (ctx->vq_size > 0) {
        f = ctx->video_queue[ctx->vq_ridx];
        ctx->vq_ridx = (ctx->vq_ridx + 1) % VIDEO_QUEUE_SIZE;
        ctx->vq_size--;
    }
    pthread_mutex_unlock(&ctx->vq_mutex);
    return f;
}

static double get_master(PlayerContext *ctx) {
    if (atomic_load(&ctx->audio_clock_set)) {
        double now = av_gettime_relative() / 1000000.0;
        double elapsed = now - ctx->audio_clock_last_time;
        if (elapsed < 0) elapsed = 0;
        if (elapsed > 0.15) elapsed = 0.15;
        return ctx->audio_clock + elapsed;
    }
    return 0.0;
}

// ================== Demux 线程 ==================
static void *demux_thread(void *arg) {
    PlayerContext *ctx = arg;
    AVPacket pkt = {0};
    while (!atomic_load(&ctx->demux_quit)) {
        if (atomic_load(&ctx->paused)) { av_usleep(10000); continue; }
        if (atomic_load(&ctx->seek_request)) {
            pthread_mutex_lock(&ctx->seek_mutex);
            double target = ctx->seek_target;
            int64_t ts = (int64_t)(target / av_q2d(ctx->video_time_base));
            avformat_seek_file(ctx->fmt_ctx, -1, INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
            pktq_flush(&ctx->video_pkt_queue); pktq_flush(&ctx->audio_pkt_queue);
            avcodec_flush_buffers(ctx->video_ctx); avcodec_flush_buffers(ctx->audio_ctx);
            ctx->audio_clock = target; ctx->audio_clock_last_time = av_gettime_relative()/1000000.0;
            ctx->video_clock = target; ctx->last_video_pts = target; ctx->frame_duration = 0.04;
            atomic_store(&ctx->seek_request, 0);
            pthread_mutex_unlock(&ctx->seek_mutex);
            continue;
        }
        int ret = av_read_frame(ctx->fmt_ctx, &pkt);
        if (ret < 0) { if (ret != AVERROR_EOF) fprintf(stderr, "demux err: %d\n", ret); break; }
        if (pkt.stream_index == ctx->video_stream_idx) {
            while (pktq_put(&ctx->video_pkt_queue, &pkt) < 0 && !atomic_load(&ctx->demux_quit)) av_usleep(1000);
        } else if (pkt.stream_index == ctx->audio_stream_idx) {
            while (pktq_put(&ctx->audio_pkt_queue, &pkt) < 0 && !atomic_load(&ctx->demux_quit)) av_usleep(1000);
        }
        av_packet_unref(&pkt);
    }
    atomic_store(&ctx->video_pkt_queue.abort_request, 1);
    atomic_store(&ctx->audio_pkt_queue.abort_request, 1);
    pthread_cond_broadcast(&ctx->video_pkt_queue.cond);
    pthread_cond_broadcast(&ctx->audio_pkt_queue.cond);
    av_packet_unref(&pkt);
    return NULL;
}

// ================== 视频解码线程（硬解修复版） ==================
static void *video_decode_thread(void *arg) {
    PlayerContext *ctx = arg;
    AVPacket pkt = {0};
    int fail = 0;
    fprintf(stderr, "[VIDEO] Decode thread started (HW: %s)\n", ctx->hw_device_ctx ? "VideoToolbox" : "Software");

    while (!atomic_load(&ctx->quit)) {
        if (atomic_load(&ctx->paused)) { av_usleep(10000); continue; }
        if (pktq_get(&ctx->video_pkt_queue, &pkt, 1) < 0) break;

        int ret = avcodec_send_packet(ctx->video_ctx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                if (++fail > 5) { avcodec_flush_buffers(ctx->video_ctx); fail = 0; }
            }
            continue;
        }
        fail = 0;

        while ((ret = avcodec_receive_frame(ctx->video_ctx, ctx->video_frame)) == 0) {
            ctx->frames_decoded++;
            if (ctx->video_frame->pts != AV_NOPTS_VALUE) ctx->video_clock = ctx->video_frame->pts * av_q2d(ctx->video_time_base);
            
            AVFrame *display_frame = ctx->video_frame;
            AVFrame *sw_frame = NULL;

            // 🚀 修复：动态获取硬件帧的真实软格式，安全导出到内存
            if (ctx->video_frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                if (!ctx->video_frame->hw_frames_ctx) {
                    fprintf(stderr, "[WARN] HW frame missing hw_frames_ctx, dropping\n");
                    av_frame_unref(ctx->video_frame); continue;
                }

                AVHWFramesContext *hwfc = (AVHWFramesContext*)ctx->video_frame->hw_frames_ctx->data;
                enum AVPixelFormat sw_fmt = hwfc->sw_format;
                if (sw_fmt == AV_PIX_FMT_NONE) sw_fmt = AV_PIX_FMT_NV12; // 兼容降级

                sw_frame = av_frame_alloc();
                sw_frame->format = sw_fmt;
                sw_frame->width = ctx->video_frame->width;
                sw_frame->height = ctx->video_frame->height;
                if (av_frame_get_buffer(sw_frame, 0) < 0) {
                    av_frame_free(&sw_frame); av_frame_unref(ctx->video_frame); continue;
                }

                if (av_hwframe_transfer_data(sw_frame, ctx->video_frame, 0) < 0) {
                    fprintf(stderr, "[WARN] HW->SW transfer failed (fmt=%s), dropping\n", av_get_pix_fmt_name(sw_fmt));
                    av_frame_free(&sw_frame); av_frame_unref(ctx->video_frame); continue;
                }
                display_frame = sw_frame;
            }

            // 动态重建 SwsContext
            if (display_frame->format != ctx->sws_src_fmt || 
                display_frame->width != ctx->sws_src_w || 
                display_frame->height != ctx->sws_src_h) {
                if (ctx->sws_ctx) sws_freeContext(ctx->sws_ctx);
                ctx->sws_ctx = sws_getContext(display_frame->width, display_frame->height, display_frame->format,
                                              ctx->width, ctx->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
                ctx->sws_src_w = display_frame->width; ctx->sws_src_h = display_frame->height; ctx->sws_src_fmt = display_frame->format;
            }
            if (!ctx->sws_ctx) { 
                if (sw_frame) av_frame_free(&sw_frame);
                av_frame_unref(ctx->video_frame); continue; 
            }

            AVFrame *out = av_frame_alloc();
            out->format = AV_PIX_FMT_RGB24; out->width = ctx->width; out->height = ctx->height;
            if (av_frame_get_buffer(out, 32) < 0) { av_frame_free(&out); if (sw_frame) av_frame_free(&sw_frame); av_frame_unref(ctx->video_frame); continue; }

            int h = sws_scale(ctx->sws_ctx, (const uint8_t *const *)display_frame->data, display_frame->linesize,
                              0, display_frame->height, out->data, out->linesize);
            if (h <= 0) { av_frame_free(&out); if (sw_frame) av_frame_free(&sw_frame); av_frame_unref(ctx->video_frame); continue; }

            out->pts = ctx->video_frame->pts;
            vframe_enqueue(ctx, out);
            
            if (sw_frame) av_frame_free(&sw_frame);
            av_frame_unref(ctx->video_frame);
        }
    }
    fprintf(stderr, "[VIDEO] Exit. Decoded: %d\n", ctx->frames_decoded);
    av_packet_unref(&pkt);
    return NULL;
}

// ================== 音频回调 ==================
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    PlayerContext *ctx = userdata;
    if (atomic_load(&ctx->quit) || atomic_load(&ctx->paused)) { memset(stream, 0, len); return; }
    AVPacket pkt = {0};
    uint8_t *buf = NULL; int buf_sz = 0, buf_idx = 0;
    while (len > 0 && !atomic_load(&ctx->quit)) {
        if (buf_idx < buf_sz) {
            int c = FFMIN(len, buf_sz - buf_idx);
            memcpy(stream, buf + buf_idx, c); stream += c; len -= c; buf_idx += c; continue;
        }
        av_freep(&buf); buf_sz = buf_idx = 0;
        if (pktq_get(&ctx->audio_pkt_queue, &pkt, 0) < 0) { memset(stream, 0, len); break; }
        int ret = avcodec_send_packet(ctx->audio_ctx, &pkt); av_packet_unref(&pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) { memset(stream, 0, len); break; }
        
        int got = 0;
        while ((ret = avcodec_receive_frame(ctx->audio_ctx, ctx->audio_frame)) == 0) {
            if (ctx->audio_frame->pts != AV_NOPTS_VALUE) {
                ctx->audio_clock = ctx->audio_frame->pts * av_q2d(ctx->audio_ctx->time_base);
                ctx->audio_clock_last_time = av_gettime_relative() / 1000000.0;
                atomic_store(&ctx->audio_clock_set, 1);
            }
            int out_s = swr_get_out_samples(ctx->swr_ctx, ctx->audio_frame->nb_samples);
            if (out_s <= 0) { av_frame_unref(ctx->audio_frame); continue; }
            uint8_t *out_buf = NULL;
            if (av_samples_alloc(&out_buf, NULL, 2, out_s, AV_SAMPLE_FMT_S16, 0) < 0) { av_frame_unref(ctx->audio_frame); continue; }
            int conv = swr_convert(ctx->swr_ctx, &out_buf, out_s, (const uint8_t **)ctx->audio_frame->data, ctx->audio_frame->nb_samples);
            av_frame_unref(ctx->audio_frame);
            if (conv < 0) { av_freep(&out_buf); continue; }
            int dsz = conv * 2 * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            int c = FFMIN(len, dsz); memcpy(stream, out_buf, c); stream += c; len -= c;
            if (dsz > c) { buf = out_buf; buf_sz = dsz; buf_idx = c; } else av_freep(&out_buf);
            got = 1; break;
        }
        if (!got) { memset(stream, 0, len); break; }
    }
    av_freep(&buf); av_packet_unref(&pkt);
}

// ================== 渲染 ==================
static void video_display(PlayerContext *ctx, AVFrame *frame) {
    SDL_RenderClear(ctx->renderer);
    SDL_UpdateTexture(ctx->texture, NULL, frame->data[0], frame->linesize[0]);
    SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, NULL);
    SDL_RenderPresent(ctx->renderer);
    ctx->frames_rendered++;
}

// ================== 清理 ==================
static void cleanup(PlayerContext *ctx) {
    atomic_store(&ctx->quit, 1); atomic_store(&ctx->demux_quit, 1);
    pthread_cond_broadcast(&ctx->vq_cond);
    pthread_cond_broadcast(&ctx->video_pkt_queue.cond);
    pthread_cond_broadcast(&ctx->audio_pkt_queue.cond);
    SDL_CloseAudio(); av_usleep(50000);
    pktq_destroy(&ctx->video_pkt_queue); pktq_destroy(&ctx->audio_pkt_queue);
    pthread_mutex_lock(&ctx->vq_mutex);
    for (int i = 0; i < VIDEO_QUEUE_SIZE; i++) if (ctx->video_queue[i]) av_frame_free(&ctx->video_queue[i]);
    pthread_mutex_unlock(&ctx->vq_mutex);
    pthread_mutex_destroy(&ctx->vq_mutex); pthread_cond_destroy(&ctx->vq_cond);
    if (ctx->sws_ctx) sws_freeContext(ctx->sws_ctx);
    if (ctx->swr_ctx) swr_free(&ctx->swr_ctx);
    av_frame_free(&ctx->video_frame); av_frame_free(&ctx->audio_frame);
    avcodec_free_context(&ctx->video_ctx); avcodec_free_context(&ctx->audio_ctx);
    if (ctx->hw_device_ctx) av_buffer_unref(&ctx->hw_device_ctx);
    avformat_close_input(&ctx->fmt_ctx);
    SDL_DestroyTexture(ctx->texture); SDL_DestroyRenderer(ctx->renderer); SDL_DestroyWindow(ctx->window);
    SDL_Quit(); avformat_network_deinit();
}

void request_seek(PlayerContext *ctx, double sec) {
    if (sec < 0) sec = 0;
    pthread_mutex_lock(&ctx->seek_mutex);
    ctx->seek_target = sec; atomic_store(&ctx->seek_request, 1);
    pthread_mutex_unlock(&ctx->seek_mutex);
    pthread_cond_broadcast(&ctx->video_pkt_queue.cond);
}

// ================== 主函数 ==================
int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <file>\n", argv[0]); return -1; }
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) { fprintf(stderr, "SDL init: %s\n", SDL_GetError()); return -1; }

    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

    PlayerContext ctx = {0};
    atomic_store(&ctx.quit, 0); atomic_store(&ctx.paused, 0); atomic_store(&ctx.demux_quit, 0);
    atomic_store(&ctx.audio_clock_set, 0); atomic_store(&ctx.seek_request, 0);
    pthread_mutex_init(&ctx.seek_mutex, NULL);

    if (avformat_open_input(&ctx.fmt_ctx, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(ctx.fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Open failed\n"); cleanup(&ctx); return -1;
    }
    ctx.video_stream_idx = av_find_best_stream(ctx.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    ctx.audio_stream_idx = av_find_best_stream(ctx.fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ctx.video_stream_idx < 0 || ctx.audio_stream_idx < 0) { fprintf(stderr, "No stream\n"); cleanup(&ctx); return -1; }
    ctx.video_time_base = ctx.fmt_ctx->streams[ctx.video_stream_idx]->time_base;

    AVStream *vst = ctx.fmt_ctx->streams[ctx.video_stream_idx];
    const AVCodec *vc = avcodec_find_decoder(vst->codecpar->codec_id);
    ctx.video_ctx = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(ctx.video_ctx, vst->codecpar);

    // 🚀 启用 VideoToolbox 硬解
    if (av_hwdevice_ctx_create(&ctx.hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0) == 0) {
        ctx.video_ctx->hw_device_ctx = av_buffer_ref(ctx.hw_device_ctx);
        fprintf(stderr, "[HW] VideoToolbox initialized\n");
    } else {
        fprintf(stderr, "[HW] VideoToolbox unavailable, using software decode\n");
    }

    AVDictionary *vo = NULL;
    av_dict_set(&vo, "threads", "auto", 0);
    av_dict_set(&vo, "fflags", "+genpts", 0);
    
    if (avcodec_open2(ctx.video_ctx, vc, &vo) < 0) { fprintf(stderr, "V codec fail\n"); av_dict_free(&vo); cleanup(&ctx); return -1; }
    av_dict_free(&vo);
    ctx.width = ctx.video_ctx->width; ctx.height = ctx.video_ctx->height;

    AVStream *ast = ctx.fmt_ctx->streams[ctx.audio_stream_idx];
    const AVCodec *ac = avcodec_find_decoder(ast->codecpar->codec_id);
    ctx.audio_ctx = avcodec_alloc_context3(ac); avcodec_parameters_to_context(ctx.audio_ctx, ast->codecpar);
    AVDictionary *ao = NULL; av_dict_set(&ao, "threads", "auto", 0);
    if (avcodec_open2(ctx.audio_ctx, ac, &ao) < 0) { cleanup(&ctx); return -1; }
    av_dict_free(&ao);

    ctx.video_frame = av_frame_alloc(); ctx.audio_frame = av_frame_alloc();
    // 初始 SwsContext，实际会在解码线程中动态重建
    ctx.sws_ctx = sws_getContext(ctx.width, ctx.height, ctx.video_ctx->pix_fmt, ctx.width, ctx.height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    ctx.sws_src_w = ctx.width; ctx.sws_src_h = ctx.height; ctx.sws_src_fmt = ctx.video_ctx->pix_fmt;

    ctx.swr_ctx = swr_alloc();
    av_opt_set_chlayout(ctx.swr_ctx, "in_chlayout", &ctx.audio_ctx->ch_layout, 0);
    av_opt_set_int(ctx.swr_ctx, "in_sample_rate", ctx.audio_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(ctx.swr_ctx, "in_sample_fmt", ctx.audio_ctx->sample_fmt, 0);
    av_opt_set_chlayout(ctx.swr_ctx, "out_chlayout", &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO, 0);
    av_opt_set_int(ctx.swr_ctx, "out_sample_rate", ctx.audio_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(ctx.swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(ctx.swr_ctx);

    ctx.window = SDL_CreateWindow(NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ctx.width, ctx.height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    SDL_ShowWindow(ctx.window);
    
    ctx.renderer = SDL_CreateRenderer(ctx.window, -1, SDL_RENDERER_ACCELERATED);
    if (!ctx.renderer) ctx.renderer = SDL_CreateRenderer(ctx.window, -1, SDL_RENDERER_SOFTWARE);
    if (!ctx.renderer) { fprintf(stderr, "Renderer fail: %s\n", SDL_GetError()); cleanup(&ctx); return -1; }

    ctx.texture = SDL_CreateTexture(ctx.renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, ctx.width, ctx.height);
    if (!ctx.texture) { fprintf(stderr, "Texture fail: %s\n", SDL_GetError()); cleanup(&ctx); return -1; }

    SDL_AudioSpec spec = {0};
    spec.freq = ctx.audio_ctx->sample_rate; spec.format = AUDIO_S16SYS; spec.channels = 2; spec.samples = 1024;
    spec.callback = audio_callback; spec.userdata = &ctx;
    SDL_OpenAudio(&spec, NULL); SDL_PauseAudio(0);
    fprintf(stderr, "[INFO] Ready. Playing...\n");

    pktq_init(&ctx.video_pkt_queue); pktq_init(&ctx.audio_pkt_queue);
    pthread_mutex_init(&ctx.vq_mutex, NULL); pthread_cond_init(&ctx.vq_cond, NULL);
    pthread_t dt, vt;
    pthread_create(&dt, NULL, demux_thread, &ctx);
    pthread_create(&vt, NULL, video_decode_thread, &ctx);

    SDL_Event ev;
    while (!atomic_load(&ctx.quit)) {
        SDL_PumpEvents();
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) atomic_store(&ctx.quit, 1);
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_SPACE: atomic_store(&ctx.paused, !atomic_load(&ctx.paused)); break;
                    case SDLK_q: atomic_store(&ctx.quit, 1); break;
                    case SDLK_LEFT: request_seek(&ctx, ctx.last_video_pts - 5); break;
                    case SDLK_RIGHT: request_seek(&ctx, ctx.last_video_pts + 5); break;
                    case SDLK_j: request_seek(&ctx, ctx.last_video_pts - 10); break;
                    case SDLK_l: request_seek(&ctx, ctx.last_video_pts + 10); break;
                }
            }
        }
        if (atomic_load(&ctx.paused)) { av_usleep(10000); continue; }

        AVFrame *frame = vframe_dequeue(&ctx);
        if (!frame) { av_usleep(10000); continue; }

        double pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * av_q2d(ctx.video_time_base) : ctx.last_video_pts + ctx.frame_duration;
        double master = get_master(&ctx);
        double delay = pts - master;

        if (master > 0.2) {
            if (delay < -0.05) {
                ctx.frames_dropped++;
                av_frame_free(&frame); continue;
            }
            if (delay > 0.0005) {
                int64_t us = (int64_t)(delay * 1000000);
                if (us < 500000) av_usleep(us);
            }
        }

        video_display(&ctx, frame);
        av_frame_free(&frame);
        ctx.last_video_pts = pts; ctx.frame_duration = (frame->duration > 0) ? frame->duration * av_q2d(ctx.video_time_base) : ctx.frame_duration;

        static double last_now = 0;
        double now = av_gettime_relative() / 1000000.0;
        if (now - last_now >= 3.0) {
            fprintf(stderr, "[PERF] FPS: %.1f | Drp: %d\n", ctx.frames_rendered / 3.0, ctx.frames_dropped);
            ctx.frames_rendered = 0; ctx.frames_dropped = 0;
            last_now = now;
        }
    }

    fprintf(stderr, "[INFO] Exit.\n");
    cleanup(&ctx);
    return 0;
}