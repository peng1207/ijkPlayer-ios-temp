/*
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2013 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ff_ffplay.h"

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#if CONFIG_AVDEVICE
#include "libavdevice/avdevice.h"
#endif
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

#if CONFIG_AVFILTER
# include "libavcodec/avcodec.h"
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif

#include "ijksdl/ijksdl_log.h"
#include "ijkavformat/ijkavformat.h"
#include "ff_cmdutils.h"
#include "ff_fferror.h"
#include "ff_ffpipeline.h"
#include "ff_ffpipenode.h"
#include "ff_ffplay_debug.h"
#include "ijkmeta.h"
#include "ijkversion.h"
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "AudioUnitRecordController.h"

static int audio_record_start = 0;

#ifndef AV_CODEC_FLAG2_FAST
#define AV_CODEC_FLAG2_FAST CODEC_FLAG2_FAST
#endif

#ifndef AV_CODEC_CAP_DR1
#define AV_CODEC_CAP_DR1 CODEC_CAP_DR1
#endif

// FIXME: 9 work around NDKr8e or gcc4.7 bug
// isnan() may not recognize some double NAN, so we test both double and float
#if defined(__ANDROID__)
#ifdef isnan
#undef isnan
#endif
#define isnan(x) (isnan((double)(x)) || isnanf((float)(x)))
#endif

#if defined(__ANDROID__)
#define printf(...) ALOGD(__VA_ARGS__)
#endif

#define FFP_IO_STAT_STEP (50 * 1024)

#define FFP_BUF_MSG_PERIOD (3)

#define MAX_RECORD_CACHE 250

// static const AVOption ffp_context_options[] = ...
#include "ff_ffplay_options.h"

static AVPacket flush_pkt;
typedef struct RECORD_PACKET{
    AVPacket *recordPkt;
}RECORD_PACKET;

typedef struct RECORD_CACHE_QUEUE{
    RECORD_PACKET rePkt[MAX_RECORD_CACHE];
    int readIndex;
    int writeIndex;
    int max_size;
}RECORD_CACHE_QUEUE;

static RECORD_CACHE_QUEUE recorQueue;

#if CONFIG_AVFILTER
// FFP_MERGE: opt_add_vfilter
#endif

#define IJKVERSION_GET_MAJOR(x)     ((x >> 16) & 0xFF)
#define IJKVERSION_GET_MINOR(x)     ((x >>  8) & 0xFF)
#define IJKVERSION_GET_MICRO(x)     ((x      ) & 0xFF)

#if CONFIG_AVFILTER
static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static inline
int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
{
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}
#endif

static void free_picture(Frame *vp);

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;

#ifdef FFP_MERGE
    pkt1 = av_malloc(sizeof(MyAVPacketList));
#else
    pkt1 = q->recycle_pkt;
    if (pkt1) {
        q->recycle_pkt = pkt1->next;
        q->recycle_count++;
    } else {
        q->alloc_count++;
        pkt1 = av_malloc(sizeof(MyAVPacketList));
    }
#ifdef FFP_SHOW_PKT_RECYCLE
    int total_count = q->recycle_count + q->alloc_count;
    if (!(total_count % 50)) {
        av_log(ffp, AV_LOG_DEBUG, "pkt-recycle \t%d + \t%d = \t%d\n", q->recycle_count, q->alloc_count, total_count);
    }
#endif
#endif
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);

    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
#ifdef FFP_MERGE
        av_freep(&pkt);
#else
        pkt->next = q->recycle_pkt;
        q->recycle_pkt = pkt;
#endif
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);

    SDL_LockMutex(q->mutex);
    while(q->recycle_pkt) {
        MyAVPacketList *pkt = q->recycle_pkt;
        if (pkt)
            q->recycle_pkt = pkt->next;
        av_freep(&pkt);
    }
    SDL_UnlockMutex(q->mutex);

    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->duration -= pkt1->pkt.duration;
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
#ifdef FFP_MERGE
            av_free(pkt1);
#else
            pkt1->next = q->recycle_pkt;
            q->recycle_pkt = pkt1;
#endif
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int packet_queue_get_or_buffering(FFPlayer *ffp, PacketQueue *q, AVPacket *pkt, int *serial, int *finished)
{
    assert(finished);
    if (!ffp->packet_buffering)
        return packet_queue_get(q, pkt, 1, serial);

    while (1) {
        int new_packet = packet_queue_get(q, pkt, 0, serial);
        if (new_packet < 0)
            return -1;
        else if (new_packet == 0) {
            if (q->is_buffer_indicator && !*finished)
                ffp_toggle_buffering(ffp, 0);
            new_packet = packet_queue_get(q, pkt, 1, serial);
            if (new_packet < 0)
                return -1;
        }

        if (*finished == *serial) {
            av_packet_unref(pkt);
            continue;
        }
        else
            break;
    }

    return 1;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;

    d->first_frame_decoded_time = SDL_GetTickHR();
    d->first_frame_decoded = 0;

    SDL_ProfilerReset(&d->decode_profiler, -1);
}

static int decoder_decode_frame(FFPlayer *ffp, Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int got_frame = 0;

    do {
        int ret = -1;

        if (d->queue->abort_request)
            return -1;

        if (!d->packet_pending || d->queue->serial != d->pkt_serial) {
            AVPacket pkt;
            do {
                if (d->queue->nb_packets == 0)
                    SDL_CondSignal(d->empty_queue_cond);
                if (packet_queue_get_or_buffering(ffp, d->queue, &pkt, &d->pkt_serial, &d->finished) < 0)
                    return -1;
                if (pkt.data == flush_pkt.data) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            } while (pkt.data == flush_pkt.data || d->queue->serial != d->pkt_serial);
            av_packet_unref(&d->pkt);
            d->pkt_temp = d->pkt = pkt;
            d->packet_pending = 1;
        }

        switch (d->avctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO: {
                ret = avcodec_decode_video2(d->avctx, frame, &got_frame, &d->pkt_temp);
                if (got_frame) {
                    ffp->stat.vdps = SDL_SpeedSamplerAdd(&ffp->vdps_sampler, FFP_SHOW_VDPS_AVCODEC, "vdps[avcodec]");
                    if (ffp->decoder_reorder_pts == -1) {
                        frame->pts = av_frame_get_best_effort_timestamp(frame);
                    } else if (!ffp->decoder_reorder_pts) {
                        frame->pts = frame->pkt_dts;
                    }
                }
                }
                break;
            case AVMEDIA_TYPE_AUDIO:
                ret = avcodec_decode_audio4(d->avctx, frame, &got_frame, &d->pkt_temp);
                if (got_frame) {
                    AVRational tb = (AVRational){1, frame->sample_rate};
                    if (frame->pts != AV_NOPTS_VALUE)
                        frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(d->avctx), tb);
                    else if (d->next_pts != AV_NOPTS_VALUE)
                        frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                    if (frame->pts != AV_NOPTS_VALUE) {
                        d->next_pts = frame->pts + frame->nb_samples;
                        d->next_pts_tb = tb;
                    }
                }
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &d->pkt_temp);
                break;
            default:
                break;
        }

        if (ret < 0) {
            d->packet_pending = 0;
        } else {
            d->pkt_temp.dts =
            d->pkt_temp.pts = AV_NOPTS_VALUE;
            if (d->pkt_temp.data) {
                if (d->avctx->codec_type != AVMEDIA_TYPE_AUDIO)
                    ret = d->pkt_temp.size;
                d->pkt_temp.data += ret;
                d->pkt_temp.size -= ret;
                if (d->pkt_temp.size <= 0)
                    d->packet_pending = 0;
            } else {
                if (!got_frame) {
                    d->packet_pending = 0;
                    d->finished = d->pkt_serial;
                }
            }
        }
    } while (!got_frame && !d->finished);

    return got_frame;
}

static void decoder_destroy(Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    SDL_VoutUnrefYUVOverlay(vp->bmp);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destory(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
        free_picture(vp);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
#ifdef FFP_MERGE
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}
#endif

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

// FFP_MERGE: fill_rectangle
// FFP_MERGE: fill_border
// FFP_MERGE: ALPHA_BLEND
// FFP_MERGE: RGBA_IN
// FFP_MERGE: YUVA_IN
// FFP_MERGE: YUVA_OUT
// FFP_MERGE: BPP
// FFP_MERGE: blend_subrect

static void free_picture(Frame *vp)
{
    if (vp->bmp) {
        SDL_VoutFreeYUVOverlay(vp->bmp);
        vp->bmp = NULL;
    }
}

static int save_bmp_to_file(FFPlayer *ffp, AVFrame * frame)
{
    AVFormatContext* pFormatCtx;
    AVOutputFormat* fmt;
    AVStream* video_st;
    AVCodecContext* pCodecCtx;
    AVCodec* pCodec;
    AVPacket *pkt;
    int got_picture = 0;
    int ret =0;
    
    VideoState *is = ffp->is;
    //Method 1
    pFormatCtx = avformat_alloc_context();
    //Guess format
    fmt = av_guess_format("mjpeg", NULL, NULL);
    pFormatCtx->oformat = fmt;
    //avformat_alloc_output_context2(&pFormatCtx, NULL, NULL, ffp->screenShotFile);
    //fmt = pFormatCtx->oformat;
    //Output URL
    if (avio_open(&pFormatCtx->pb, ffp->screenShotFile, AVIO_FLAG_READ_WRITE) < 0) {
        printf("Couldn't open output file.");
        return -1;
    }
    video_st = avformat_new_stream(pFormatCtx, 0);
    if (video_st == NULL) {
        return -1;
    }
    pCodecCtx = video_st->codec;
    pCodecCtx->codec_id = fmt->video_codec;
    pCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    pCodecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    pCodecCtx->width = is->viddec.avctx->width;
    pCodecCtx->height = is->viddec.avctx->height;
    pCodecCtx->time_base.num = 1;
    pCodecCtx->time_base.den = 25;
    //Output some information
    av_dump_format(pFormatCtx, 0, ffp->screenShotFile, 1);
    pCodec = avcodec_find_encoder(pCodecCtx->codec_id);
    if (!pCodec) {
        printf("Codec not found.");
        return -1;
    }
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        printf("Could not open codec.");
        return -1;
    }
    avformat_write_header(pFormatCtx, NULL);
    pkt = av_packet_alloc();
    //Encode
    ret = avcodec_encode_video2(pCodecCtx, pkt, frame, &got_picture);
    if (ret < 0) {
        printf("Encode Error.\n");
        return -1;
    }
    if (got_picture == 1) {
        pkt->stream_index = video_st->index;
        ret = av_write_frame(pFormatCtx, pkt);
    }
    av_free_packet(pkt);
    //Write Trailer
    av_write_trailer(pFormatCtx);
    
    printf("Encode Successful.\n");
    if (video_st) {
        avcodec_close(video_st->codec);
        //av_free(picture_buf);
    }
    avio_close(pFormatCtx->pb);
    avformat_free_context(pFormatCtx);
    
    if (ffp->screenshot_callback != NULL) {
        ffp->screenshot_callback(ffp->screenShotFile,ffp->player);
    }
    
    return 0;
}



// FFP_MERGE: realloc_texture
// FFP_MERGE: calculate_display_rect
// FFP_MERGE: upload_texture
// FFP_MERGE: video_image_display

static size_t parse_ass_subtitle(const char *ass, char *output)
{
    char *tok = NULL;
    tok = strchr(ass, ':'); if (tok) tok += 1; // skip event
    tok = strchr(tok, ','); if (tok) tok += 1; // skip layer
    tok = strchr(tok, ','); if (tok) tok += 1; // skip start_time
    tok = strchr(tok, ','); if (tok) tok += 1; // skip end_time
    tok = strchr(tok, ','); if (tok) tok += 1; // skip style
    tok = strchr(tok, ','); if (tok) tok += 1; // skip name
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_l
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_r
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_v
    tok = strchr(tok, ','); if (tok) tok += 1; // skip effect
    if (tok) {
        char *text = tok;
        size_t idx = 0;
        do {
            char *found = strstr(text, "\\N");
            if (found) {
                size_t n = found - text;
                memcpy(output+idx, text, n);
                output[idx + n] = '\n';
                idx = n + 1;
                text = found + 2;
            }
            else {
                size_t left_text_len = strlen(text);
                memcpy(output+idx, text, left_text_len);
                if (output[idx + left_text_len - 1] == '\n')
                    output[idx + left_text_len - 1] = '\0';
                else
                    output[idx + left_text_len] = '\0';
                break;
            }
        } while(1);
        return strlen(output) + 1;
    }
    return 0;
}

static void video_image_display2(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    Frame *vp;
    Frame *sp = NULL;

    vp = frame_queue_peek_last(&is->pictq);

    int latest_seek_load_serial = __atomic_exchange_n(&(is->latest_seek_load_serial), -1, memory_order_seq_cst);
    if (latest_seek_load_serial == vp->serial)
        ffp->stat.latest_seek_load_duration = (av_gettime() - is->latest_seek_load_start_at) / 1000;

    if (vp->bmp) {
        if (is->subtitle_st) {
            if (frame_queue_nb_remaining(&is->subpq) > 0) {
                sp = frame_queue_peek(&is->subpq);

                if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                    if (!sp->uploaded) {
                        if (sp->sub.num_rects > 0) {
                            char buffered_text[4096];
                            if (sp->sub.rects[0]->text) {
                                strncpy(buffered_text, sp->sub.rects[0]->text, 4096);
                            }
                            else if (sp->sub.rects[0]->ass) {
                                parse_ass_subtitle(sp->sub.rects[0]->ass, buffered_text);
                            }
                            ffp_notify_msg4(ffp, FFP_MSG_TIMED_TEXT, 0, 0, buffered_text, sizeof(buffered_text));
                        }
                        sp->uploaded = 1;
                    }
                }
            }
        }
        SDL_VoutDisplayYUVOverlay(ffp->vout, vp->bmp);
        ffp->stat.vfps = SDL_SpeedSamplerAdd(&ffp->vfps_sampler, FFP_SHOW_VFPS_FFPLAY, "vfps[ffplay]");
        if (!ffp->first_video_frame_rendered) {
            ffp->first_video_frame_rendered = 1;
            ffp_notify_msg1(ffp, FFP_MSG_VIDEO_RENDERING_START);
        }
    }
}

// FFP_MERGE: compute_mod
// FFP_MERGE: video_audio_display

static void stream_component_close(FFPlayer *ffp, int stream_index)
{
    VideoState *is = ffp->is;
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_AoutCloseAudio(ffp->aout);

        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

#ifdef FFP_MERGE
        if (is->rdft) {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
#endif
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static void record_queue_init(){
    for(int i = 0; i<MAX_RECORD_CACHE; i++){
        recorQueue.rePkt[i].recordPkt = av_packet_alloc();
    }
}

static void record_queue_destroy(){
    for(int i = 0; i<MAX_RECORD_CACHE; i++){
        AVPacket *recPkt = recorQueue.rePkt[i].recordPkt;
        av_free_packet(recPkt);
    }
}

static void stream_close(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    packet_queue_abort(&is->videoq);
    packet_queue_abort(&is->audioq);
    av_log(NULL, AV_LOG_DEBUG, "wait for read_tid\n");
    SDL_WaitThread(is->read_tid, NULL);
    
    record_queue_destroy();
    if(is->ofmt_ctx){
        mw_closeOutPutStream(ffp);
    }
    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(ffp, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(ffp, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(ffp, is->subtitle_stream);

    avformat_close_input(&is->ic);

    av_log(NULL, AV_LOG_DEBUG, "wait for video_refresh_tid\n");
    SDL_WaitThread(is->video_refresh_tid, NULL);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);
    SDL_DestroyCond(is->continue_read_thread);
    SDL_DestroyMutex(is->play_mutex);
#if !CONFIG_AVFILTER
    sws_freeContext(is->img_convert_ctx);
#endif
#ifdef FFP_MERGE
    sws_freeContext(is->sub_convert_ctx);
#endif
    av_free(is->filename);
    av_free(is);
    ffp->is = NULL;
}

// FFP_MERGE: do_exit
// FFP_MERGE: sigterm_handler
// FFP_MERGE: video_open
// FFP_MERGE: video_display

/* display the current picture, if any */
static void video_display2(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    if (is->video_st)
        video_image_display2(ffp);
}

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

static void check_external_clock_speed(VideoState *is) {
   if ((is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) ||
       (is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)) {
       set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
   } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
              (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
       set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
   } else {
       double speed = is->extclk.speed;
       if (speed != 1.0)
           set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
   }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause_l(FFPlayer *ffp, int pause_on)
{
    VideoState *is = ffp->is;
    if (is->paused && !pause_on) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;

#ifdef FFP_MERGE
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
#endif
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    } else {
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = pause_on;

    SDL_AoutPauseAudio(ffp->aout, pause_on);
}

static void stream_update_pause_l(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    if (!is->step && (is->pause_req || is->buffering_on)) {
        stream_toggle_pause_l(ffp, 1);
    } else {
        stream_toggle_pause_l(ffp, 0);
    }
}

static void toggle_pause_l(FFPlayer *ffp, int pause_on)
{
    VideoState *is = ffp->is;
    is->pause_req = pause_on;
    ffp->auto_resume = !pause_on;
    stream_update_pause_l(ffp);
    is->step = 0;
}

static void toggle_pause(FFPlayer *ffp, int pause_on)
{
    SDL_LockMutex(ffp->is->play_mutex);
    toggle_pause_l(ffp, pause_on);
    SDL_UnlockMutex(ffp->is->play_mutex);
}

// FFP_MERGE: toggle_mute
// FFP_MERGE: update_volume

static void step_to_next_frame_l(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause_l(ffp, 0);
    is->step = 1;
}

static double compute_target_delay(FFPlayer *ffp, double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        /* -- by bbcallen: replace is->max_frame_duration with AV_NOSYNC_THRESHOLD */
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    if (ffp) {
        ffp->stat.avdelay = delay;
        ffp->stat.avdiff  = diff;
    }
#ifdef FFP_SHOW_AUDIO_DELAY
    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);
#endif

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(FFPlayer *opaque, double *remaining_time)
{
    FFPlayer *ffp = opaque;
    VideoState *is = ffp->is;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    if (!ffp->display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + ffp->rdftspeed < time) {
            video_display2(ffp);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + ffp->rdftspeed - time);
    }

    if (is->video_st) {
retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(ffp, last_duration, is);

            time= av_gettime_relative()/1000000.0;
            if (isnan(is->frame_timer) || time < is->frame_timer)
                is->frame_timer = time;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            SDL_UnlockMutex(is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if(!is->step && (ffp->framedrop > 0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration) {
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != is->subtitleq.serial
                            || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                            || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                    {
                        if (sp->uploaded) {
                            ffp_notify_msg4(ffp, FFP_MSG_TIMED_TEXT, 0, 0, "", 1);
                        }
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            SDL_LockMutex(ffp->is->play_mutex);
            if (is->step) {
                is->step = 0;
                if (!is->paused)
                    stream_update_pause_l(ffp);
            }
            SDL_UnlockMutex(ffp->is->play_mutex);
        }
display:
        /* display picture */
        if (!ffp->display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
            video_display2(ffp);
    }
    is->force_refresh = 0;
    if (ffp->show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize __unused;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
#ifdef FFP_MERGE
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
#else
            sqsize = 0;
#endif
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = get_master_clock(is) - get_clock(&is->audclk);
            av_log(NULL, AV_LOG_INFO,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                   get_master_clock(is),
                   (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                   av_diff,
                   is->frame_drops_early + is->frame_drops_late,
                   aqsize / 1024,
                   vqsize / 1024,
                   sqsize,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
}

/* allocate a picture (needs to do that in main thread to avoid
   potential locking problems */
static void alloc_picture(FFPlayer *ffp, int frame_format)
{
    VideoState *is = ffp->is;
    Frame *vp;
#ifdef FFP_MERGE
    int sdl_format;
#endif

    vp = &is->pictq.queue[is->pictq.windex];

    free_picture(vp);

#ifdef FFP_MERGE
    video_open(is, vp);
#endif

    SDL_VoutSetOverlayFormat(ffp->vout, ffp->overlay_format);
    vp->bmp = SDL_Vout_CreateOverlay(vp->width, vp->height,
                                   frame_format,
                                   ffp->vout);
#ifdef FFP_MERGE
    if (vp->format == AV_PIX_FMT_YUV420P)
        sdl_format = SDL_PIXELFORMAT_YV12;
    else
        sdl_format = SDL_PIXELFORMAT_ARGB8888;

    if (realloc_texture(&vp->bmp, sdl_format, vp->width, vp->height, SDL_BLENDMODE_NONE, 0) < 0) {
#else
    /* RV16, RV32 contains only one plane */
    if (!vp->bmp || (!vp->bmp->is_private && vp->bmp->pitches[0] < vp->width)) {
#endif
        /* SDL allocates a buffer smaller than requested if the video
         * overlay hardware is unable to support the requested size. */
        av_log(NULL, AV_LOG_FATAL,
               "Error: the video system does not support an image\n"
                        "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
                        "to reduce the image size.\n", vp->width, vp->height );
        free_picture(vp);
    }

    SDL_LockMutex(is->pictq.mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq.cond);
    SDL_UnlockMutex(is->pictq.mutex);
}

static int queue_picture(FFPlayer *ffp, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    VideoState *is = ffp->is;
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
#ifdef FFP_MERGE
    vp->uploaded = 0;
#endif

    /* alloc or resize hardware picture buffer */
    if (!vp->bmp || !vp->allocated ||
        vp->width  != src_frame->width ||
        vp->height != src_frame->height ||
        vp->format != src_frame->format) {

        if (vp->width != src_frame->width || vp->height != src_frame->height)
            ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, src_frame->width, src_frame->height);

        vp->allocated = 0;
        vp->width = src_frame->width;
        vp->height = src_frame->height;
        vp->format = src_frame->format;

        /* the allocation must be done in the main thread to avoid
           locking problems. */
        alloc_picture(ffp, src_frame->format);

        if (is->videoq.abort_request)
            return -1;
    }

    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        /* get a pointer on the bitmap */
        SDL_VoutLockYUVOverlay(vp->bmp);

#ifdef FFP_MERGE
#if CONFIG_AVFILTER
        // FIXME use direct rendering
        av_image_copy(data, linesize, (const uint8_t **)src_frame->data, src_frame->linesize,
                        src_frame->format, vp->width, vp->height);
#else
        // sws_getCachedContext(...);
#endif
#endif
        // FIXME: set swscale options
        if (SDL_VoutFillFrameYUVOverlay(vp->bmp, src_frame) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            exit(1);
        }
        /* update the bitmap content */
        SDL_VoutUnlockYUVOverlay(vp->bmp);

        vp->pts = pts;
        vp->duration = duration;
        vp->pos = pos;
        vp->serial = serial;
        vp->sar = src_frame->sample_aspect_ratio;
        vp->bmp->sar_num = vp->sar.num;
        vp->bmp->sar_den = vp->sar.den;

#ifdef FFP_MERGE
        av_frame_move_ref(vp->frame, src_frame);
#endif
        frame_queue_push(&is->pictq);
        if (!is->viddec.first_frame_decoded) {
            ALOGD("Video: first frame decoded\n");
            is->viddec.first_frame_decoded_time = SDL_GetTickHR();
            is->viddec.first_frame_decoded = 1;
        }
    }
    return 0;
}

static int get_video_frame(FFPlayer *ffp, AVFrame *frame)
{
    VideoState *is = ffp->is;
    int got_picture;

    ffp_video_statistic_l(ffp);
    if ((got_picture = decoder_decode_frame(ffp, &is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;
        if (ffp->m_screenShot)
        {
            save_bmp_to_file(ffp, frame);
            ffp->m_screenShot = 0;
        }
        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (ffp->framedrop>0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    is->continuous_frame_drops_early++;
                    if (is->continuous_frame_drops_early > ffp->framedrop) {
                        is->continuous_frame_drops_early = 0;
                    } else {
                        av_frame_unref(frame);
                        got_picture = 0;
                    }
                }
            }
        }
    }

    return got_picture;
}

#if CONFIG_AVFILTER
static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(FFPlayer *ffp, AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGRA, AV_PIX_FMT_NONE };
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    AVDictionaryEntry *e = NULL;

    while ((e = av_dict_get(ffp->sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, NULL,
                                            graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

    if (ffp->autorotate) {
        double theta  = get_rotation(is->video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", NULL);
            INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        }
    }

#ifdef FFP_AVFILTER_PLAYBACK_RATE
    if (fabsf(ffp->pf_playback_rate) > 0.00001 &&
        fabsf(ffp->pf_playback_rate - 1.0f) > 0.00001) {
        char setpts_buf[256];
        float rate = 1.0f / ffp->pf_playback_rate;
        rate = av_clipf_c(rate, 0.5f, 2.0f);
        av_log(ffp, AV_LOG_INFO, "vf_rate=%f(1/%f)\n", ffp->pf_playback_rate, rate);
        snprintf(setpts_buf, sizeof(setpts_buf), "%f*PTS", rate);
        INSERT_FILT("setpts", setpts_buf);
    }
#endif

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

static int configure_audio_filters(FFPlayer *ffp, const char *afilters, int force_output_format)
{
    VideoState *is = ffp->is;
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;
    char afilters_args[4096];

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);

    while ((e = av_dict_get(ffp->swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%"PRIx64,  is->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;


    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels       [0] = is->audio_tgt.channels;
        sample_rates   [0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts" , channels       ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }

    afilters_args[0] = 0;
    if (afilters)
        snprintf(afilters_args, sizeof(afilters_args), "%s", afilters);

#ifdef FFP_AVFILTER_PLAYBACK_RATE
    if (fabsf(ffp->pf_playback_rate) > 0.00001 &&
        fabsf(ffp->pf_playback_rate - 1.0f) > 0.00001) {
        if (afilters_args[0])
            av_strlcatf(afilters_args, sizeof(afilters_args), ",");

        av_log(ffp, AV_LOG_INFO, "af_rate=%f\n", ffp->pf_playback_rate);
        av_strlcatf(afilters_args, sizeof(afilters_args), "atempo=%f", ffp->pf_playback_rate);
    }
#endif

    if ((ret = configure_filtergraph(is->agraph, afilters_args[0] ? afilters_args : NULL, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}
#endif  /* CONFIG_AVFILTER */

static int audio_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        ffp_audio_statistic_l(ffp);
        if ((got_frame = decoder_decode_frame(ffp, &is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};

#if CONFIG_AVFILTER
                dec_channel_layout = get_valid_channel_layout(frame->channel_layout, av_frame_get_channels(frame));

                reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                                   frame->format, av_frame_get_channels(frame))    ||
                    is->audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio_filter_src.freq           != frame->sample_rate ||
                    is->auddec.pkt_serial               != last_serial        ||
                    ffp->af_changed;

                if (reconfigure) {
                    SDL_LockMutex(ffp->af_mutex);
                    ffp->af_changed = 0;
                    char buf1[1024], buf2[1024];
                    av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                    av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                    av_log(NULL, AV_LOG_DEBUG,
                           "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, av_frame_get_channels(frame), av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);

                    is->audio_filter_src.fmt            = frame->format;
                    is->audio_filter_src.channels       = av_frame_get_channels(frame);
                    is->audio_filter_src.channel_layout = dec_channel_layout;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->auddec.pkt_serial;

                    if ((ret = configure_audio_filters(ffp, ffp->afilters, 1)) < 0) {
                        SDL_UnlockMutex(ffp->af_mutex);
                        goto the_end;
                    }
                    SDL_UnlockMutex(ffp->af_mutex);
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                tb = is->out_audio_filter->inputs[0]->time_base;
#endif
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = av_frame_get_pkt_pos(frame);
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), void *arg, const char *name)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThreadEx(&d->_decoder_tid, fn, arg, name);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int ffplay_video_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
    if (!graph) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

#else
    ffp_notify_msg2(ffp, FFP_MSG_VIDEO_ROTATION_CHANGED, ffp_get_video_rotate_degrees(ffp));
#endif

    if (!frame) {
#if CONFIG_AVFILTER
        avfilter_graph_free(&graph);
#endif
        return AVERROR(ENOMEM);
    }

    for (;;) {
        ret = get_video_frame(ffp, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

#if CONFIG_AVFILTER
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || ffp->vf_changed
            || last_vfilter_idx != is->vfilter_idx) {
            SDL_LockMutex(ffp->vf_mutex);
            ffp->vf_changed = 0;
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = configure_video_filters(ffp, graph, is, ffp->vfilters_list ? ffp->vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                // FIXME: post error
                SDL_UnlockMutex(ffp->vf_mutex);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = filt_out->inputs[0]->frame_rate;
            SDL_UnlockMutex(ffp->vf_mutex);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = filt_out->inputs[0]->time_base;
#endif
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(ffp, frame, pts, duration, av_frame_get_pkt_pos(frame), is->viddec.pkt_serial);
            av_frame_unref(frame);
#if CONFIG_AVFILTER
        }
#endif

        if (ret < 0)
            goto the_end;
    }
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

static int video_thread(void *arg)
{
    FFPlayer *ffp = (FFPlayer *)arg;
    int       ret = 0;

    if (ffp->node_vdec) {
        ret = ffpipenode_run_sync(ffp->node_vdec);
    }
    return ret;
}

static int subtitle_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(ffp, &is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;
#ifdef FFP_MERGE
        if (got_subtitle && sp->sub.format == 0) {
#else
        if (got_subtitle) {
#endif
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
#ifdef FFP_MERGE
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
#endif
        }
    }
    return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused || is->step)
        return -1;

    if (ffp->sync_av_start &&                       /* sync enabled */
        is->video_st &&                             /* has video stream */
        !is->viddec.first_frame_decoded &&          /* not hot */
        is->viddec.finished != is->videoq.serial) { /* not finished */
        /* waiting for first video frame */
        Uint64 now = SDL_GetTickHR();
        if (now < is->viddec.first_frame_decoded_time ||
            now > is->viddec.first_frame_decoded_time + 2000) {
            is->viddec.first_frame_decoded = 1;
        } else {
            /* video pipeline is not ready yet */
            return -1;
        }
    }

    do {
#if defined(_WIN32) || defined(__APPLE__)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - ffp->audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(af->frame),
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    dec_channel_layout =
        (af->frame->channel_layout && av_frame_get_channels(af->frame) == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
        af->frame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(af->frame));
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format        != is->audio_src.fmt            ||
        dec_channel_layout       != is->audio_src.channel_layout ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        AVDictionary *swr_opts = NULL;
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL,
                                         is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                         dec_channel_layout,           af->frame->format, af->frame->sample_rate,
                                         0, NULL);
        if (!is->swr_ctx) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), av_frame_get_channels(af->frame),
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            return -1;
        }
        av_dict_copy(&swr_opts, ffp->swr_opts, 0);
        if (af->frame->channel_layout == AV_CH_LAYOUT_5POINT1_BACK)
            av_opt_set_double(is->swr_ctx, "center_mix_level", ffp->preset_5_1_center_mix_level, 0);
        av_opt_set_dict(is->swr_ctx, &swr_opts);
        av_dict_free(&swr_opts);

        if (swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), av_frame_get_channels(af->frame),
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels       = av_frame_get_channels(af->frame);
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int)((int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256);
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef FFP_SHOW_AUDIO_DELAY
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    if (!is->auddec.first_frame_decoded) {
        ALOGD("avcodec/Audio: first frame decoded\n");
        is->auddec.first_frame_decoded_time = SDL_GetTickHR();
        is->auddec.first_frame_decoded = 1;
    }
    if (!ffp->first_audio_frame_rendered) {
        ffp->first_audio_frame_rendered = 1;
        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_RENDERING_START);
    }
    return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    FFPlayer *ffp = opaque;
    VideoState *is = ffp->is;
    int audio_size, len1;
    if (!ffp || !is) {
        memset(stream, 0, len);
        return;
    }

    ffp->audio_callback_time = av_gettime_relative();

    if (ffp->pf_playback_rate_changed) {
        ffp->pf_playback_rate_changed = 0;
        SDL_AoutSetPlaybackRate(ffp->aout, ffp->pf_playback_rate);
    }
    if (ffp->pf_playback_volume_changed) {
        ffp->pf_playback_volume_changed = 0;
        SDL_AoutSetPlaybackVolume(ffp->aout, ffp->pf_playback_volume);
    }

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           audio_size = audio_decode_frame(ffp);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf = NULL;
               is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
           } else {
               if (is->show_mode != SHOW_MODE_VIDEO)
                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        if (is->auddec.pkt_serial != is->audioq.serial) {
            is->audio_buf_index = is->audio_buf_size;
            memset(stream, 0, len);
            // stream += len;
            // len = 0;
            SDL_AoutFlushAudio(ffp->aout);
            break;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double)(is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec - SDL_AoutGetLatencySeconds(ffp->aout), is->audio_clock_serial, ffp->audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(FFPlayer *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    FFPlayer *ffp = opaque;
    VideoState *is = ffp->is;
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
#ifdef FFP_MERGE
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
#endif
    static const int next_sample_rates[] = {0, 44100, 48000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AoutGetAudioPerSecondCallBacks(ffp->aout)));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (SDL_AoutOpenAudio(ffp->aout, &wanted_spec, &spec) < 0) {
        /* avoid infinity loop on exit. --by bbcallen */
        if (is->abort_request)
            return -1;
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    SDL_AoutSetDefaultLatencySeconds(ffp->aout, ((double)(2 * spec.size)) / audio_hw_params->bytes_per_sec);
    return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(FFPlayer *ffp, int stream_index)
{
    VideoState *is = ffp->is;
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec = NULL;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = ffp->lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name = ffp->audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = ffp->subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name = ffp->video_codec_name; break;
        default: break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with id %d\n", avctx->codec_id);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if(stream_lowres > av_codec_get_max_lowres(codec)){
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                av_codec_get_max_lowres(codec));
        stream_lowres = av_codec_get_max_lowres(codec);
    }
    av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
    if(stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
    if (ffp->fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
    if(codec->capabilities & AV_CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif

    opts = filter_codec_opts(ffp->codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
#ifdef FFP_MERGE
        ret =  AVERROR_OPTION_NOT_FOUND;
        goto fail;
#endif
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterLink *link;

            is->audio_filter_src.freq           = avctx->sample_rate;
            is->audio_filter_src.channels       = avctx->channels;
            is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            SDL_LockMutex(ffp->af_mutex);
            if ((ret = configure_audio_filters(ffp, ffp->afilters, 0)) < 0) {
                SDL_UnlockMutex(ffp->af_mutex);
                goto fail;
            }
            ffp->af_changed = 0;
            SDL_UnlockMutex(ffp->af_mutex);
            link = is->out_audio_filter->inputs[0];
            sample_rate    = link->sample_rate;
            nb_channels    = avfilter_link_get_channels(link);
            channel_layout = link->channel_layout;
        }
#else
        sample_rate    = avctx->sample_rate;
        nb_channels    = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif

        /* prepare audio output */
        if ((ret = audio_open(ffp, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        ffp_set_audio_codec_info(ffp, AVCODEC_MODULE_NAME, avcodec_get_name(avctx->codec_id));
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = 2.0 * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, ffp, "ff_audio_dec")) < 0)
            goto out;
        SDL_AoutPauseAudio(ffp->aout, 0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
        ffp->node_vdec = ffpipeline_open_video_decoder(ffp->pipeline, ffp);
        if (!ffp->node_vdec)
            goto fail;
        if ((ret = decoder_start(&is->viddec, video_thread, ffp, "ff_video_dec")) < 0)
            goto out;
        is->queue_attachments_req = 1;

        if (ffp->max_fps >= 0) {
            if(is->video_st->avg_frame_rate.den && is->video_st->avg_frame_rate.num) {
                double fps = av_q2d(is->video_st->avg_frame_rate);
                SDL_ProfilerReset(&is->viddec.decode_profiler, fps + 0.5);
                if (fps > ffp->max_fps && fps < 130.0) {
                    is->is_video_high_fps = 1;
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (too high)\n", fps);
                } else {
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (normal)\n", fps);
                }
            }
            if(is->video_st->r_frame_rate.den && is->video_st->r_frame_rate.num) {
                double tbr = av_q2d(is->video_st->r_frame_rate);
                if (tbr > ffp->max_fps && tbr < 130.0) {
                    is->is_video_high_fps = 1;
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (too high)\n", tbr);
                } else {
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (normal)\n", tbr);
                }
            }
        }

        if (is->is_video_high_fps) {
            avctx->skip_frame       = FFMAX(avctx->skip_frame, AVDISCARD_NONREF);
            avctx->skip_loop_filter = FFMAX(avctx->skip_loop_filter, AVDISCARD_NONREF);
            avctx->skip_idct        = FFMAX(avctx->skip_loop_filter, AVDISCARD_NONREF);
        }

        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (!ffp->subtitle) break;

        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        ffp_set_subtitle_codec_info(ffp, AVCODEC_MODULE_NAME, avcodec_get_name(avctx->codec_id));

        decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
        if ((ret = decoder_start(&is->subdec, subtitle_thread, ffp, "ff_subtitle_dec")) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue, int min_frames) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
#ifdef FFP_MERGE
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
#endif
           queue->nb_packets > min_frames;
}

static int is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->filename, "rtp:", 4)
                 || !strncmp(s->filename, "udp:", 4)
                )
    )
        return 1;
    return 0;
}
    
    //初始化输出流
static AVStream* add_output_stream(AVFormatContext* output_format_context, AVStream* input_stream)
{
    AVCodecContext* input_codec_context = NULL;
    AVCodecContext* output_codec_context = NULL;
        
    AVStream* output_stream = NULL;
    output_stream = avformat_new_stream(output_format_context, NULL);
    if (!output_stream)
    {
        return NULL;
    }
        
    input_codec_context = input_stream->codec;
    output_codec_context = output_stream->codec;
        
    output_codec_context->codec_id = input_codec_context->codec_id;
    output_codec_context->codec_type = input_codec_context->codec_type;
    output_codec_context->codec_tag = input_codec_context->codec_tag;
    output_codec_context->bit_rate = input_codec_context->bit_rate;
    output_codec_context->extradata = input_codec_context->extradata;
    output_codec_context->extradata_size = input_codec_context->extradata_size;
        
    if (av_q2d(input_codec_context->time_base) * input_codec_context->ticks_per_frame > av_q2d(input_stream->time_base) && av_q2d(input_stream->time_base) < 1.0 / 1000)
    {
        output_codec_context->time_base = input_codec_context->time_base;
        output_codec_context->time_base.num *= input_codec_context->ticks_per_frame;
    }
    else
    {
        output_codec_context->time_base = input_stream->time_base;
    }
    switch (input_codec_context->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
            output_codec_context->channel_layout = input_codec_context->channel_layout;
            output_codec_context->sample_rate = input_codec_context->sample_rate;
            output_codec_context->channels = input_codec_context->channels;
            output_codec_context->frame_size = input_codec_context->frame_size;
            if ((input_codec_context->block_align == 1 && input_codec_context->codec_id == AV_CODEC_ID_MP3) ||
                input_codec_context->codec_id == AV_CODEC_ID_AC3 || input_codec_context->codec_id == AV_CODEC_ID_AAC)
            {
                output_codec_context->block_align = 0;
            }
            else
            {
                output_codec_context->block_align = input_codec_context->block_align;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            output_codec_context->pix_fmt = input_codec_context->pix_fmt;
            output_codec_context->width = input_codec_context->width;
            output_codec_context->height = input_codec_context->height;
            output_codec_context->has_b_frames = input_codec_context->has_b_frames;
            if (output_format_context->oformat->flags & AVFMT_GLOBALHEADER)
            {
                output_codec_context->flags |= CODEC_FLAG_GLOBAL_HEADER;
            }
            break;
        default:
            break;
}
        
        return output_stream;
    }

    
static int init_record_file(FFPlayer *ffp)
{
    int ret;
    int i;
    VideoState *is = ffp->is;
    if(is->m_oformat_ctx)
        return 1;
    if(is->video_stream == -1 || is->video_st->codec->codec_id != AV_CODEC_ID_H264)
        return 0;
    if (is->audio_stream != -1)
    {
        if ( is->audio_st->codec->codec_id == AV_CODEC_ID_MP3 ||
            is->audio_st->codec->codec_id == AV_CODEC_ID_AC3 ||
            is->audio_st->codec->codec_id == AV_CODEC_ID_AAC)
        {
            /* allocate the output media context */
            avformat_alloc_output_context2(&is->m_oformat_ctx, NULL, NULL, ffp->mwRecFile);
            if(is->m_oformat_ctx)
            {
                is->m_output_fmt = is->m_oformat_ctx->oformat;
                if (is->video_stream != -1)
                {
                    is->m_out_video_st = add_output_stream(is->m_oformat_ctx, is->video_st);
                }
                if (is->audio_stream != -1)
                    is->m_out_audio_st = add_output_stream(is->m_oformat_ctx, is->audio_st);
                if (avio_open(&is->m_oformat_ctx->pb, ffp->mwRecFile, AVIO_FLAG_WRITE) >= 0)
                {
                    if(avformat_write_header(is->m_oformat_ctx, NULL) >= 0)//写入文件头
                    {
                        ret = 1;
                    }else
                    {
                        if (is->m_oformat_ctx)
                        {
                                
                            for(i = 0; i < is->m_oformat_ctx->nb_streams; i++)
                            {
                                av_freep(&is->m_oformat_ctx->streams[i]->codec);
                                av_freep(&is->m_oformat_ctx->streams[i]);
                            }
                                
                            if (!(is->m_output_fmt->flags & AVFMT_NOFILE))
                                avio_close(is->m_oformat_ctx->pb);
                            av_free(is->m_oformat_ctx);
                                
                            if(is->m_key_pkt)
                                av_free_packet(is->m_key_pkt);
                                
                            is->m_output_fmt = NULL;
                            is->m_oformat_ctx = NULL;
                            is->m_out_video_st = NULL;
                            is->m_out_audio_st = NULL;
                            is->m_key_pkt = NULL;
                            ffp->m_bRecorder = 0;
                        }
                    }
                }else{
                    if (is->m_oformat_ctx)
                    {
                        for(i = 0; i < is->m_oformat_ctx->nb_streams; i++)
                        {
                            av_freep(&is->m_oformat_ctx->streams[i]->codec);
                            av_freep(&is->m_oformat_ctx->streams[i]);
                        }
                            
                        av_free(is->m_oformat_ctx);
                            
                        if(is->m_key_pkt)
                            av_free_packet(is->m_key_pkt);
                            
                        is->m_output_fmt = NULL;
                        is->m_oformat_ctx = NULL;
                        is->m_out_video_st = NULL;
                        is->m_out_audio_st = NULL;
                        is->m_key_pkt = NULL;
                        ffp->m_bRecorder = 0;
                    }
                }
            }
        }
    }
    return ret;
}
    
    //关闭录像文件
static void close_record_file(FFPlayer *ffp)
{
    int i;
    VideoState *is = ffp->is;
        
    if (is->m_oformat_ctx)
    {
        av_write_trailer(is->m_oformat_ctx);
            
        for(i = 0; i < is->m_oformat_ctx->nb_streams; i++)
        {
            av_freep(&is->m_oformat_ctx->streams[i]->codec);
            av_freep(&is->m_oformat_ctx->streams[i]);
        }
            
        if (!(is->m_output_fmt->flags & AVFMT_NOFILE))
            avio_close(is->m_oformat_ctx->pb);
        av_free(is->m_oformat_ctx);
            
        if(is->m_key_pkt)
            av_free_packet(is->m_key_pkt);
            
        is->m_output_fmt = NULL;
        is->m_oformat_ctx = NULL;
        is->m_out_video_st = NULL;
        is->m_out_audio_st = NULL;
        is->m_key_pkt = NULL;
        ffp->m_bRecorder = 0;
    }
}
    
    static void drop_queue_until_pts(PacketQueue *q, int64_t drop_to_pts) {
        MyAVPacketList *pkt1 = NULL;
        int del_nb_packets = 0;
        for (;;) {
            pkt1 = q->first_pkt;
            if (!pkt1) {
                break;
            }
            // video need key frame? 这里如果不判断是否是关键帧会导致视频画面花屏。但是这样会导致全部清空的可能也会出现花屏
            // 所以这里推流端设置好 GOP 的大小，如果 max_cached_duration > 2 * GOP，可以尽可能规避全部清空
            // 也可以在调用control_queue_duration之前判断新进来的视频pkt是否是关键帧，这样即使全部清空了也不会花屏
            if ((pkt1->pkt.flags & AV_PKT_FLAG_KEY) && pkt1->pkt.pts >= drop_to_pts) {
                //        if (pkt1->pkt.pts >= drop_to_pts) {
                break;
            }
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            ++del_nb_packets;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            if (pkt1->pkt.duration > 0)
                q->duration -= pkt1->pkt.duration;
            av_free_packet(&pkt1->pkt);
#ifdef FFP_MERGE
            av_free(pkt1);
#else
            pkt1->next = q->recycle_pkt;
            q->recycle_pkt = pkt1;
#endif
        }
        av_log(NULL, AV_LOG_INFO, "233 del_nb_packets = %d.\n", del_nb_packets);
    }
    
    static void control_video_queue_duration(FFPlayer *ffp, VideoState *is) {
        int time_base_valid = 0;
        int64_t cached_duration = -1;
        int nb_packets = 0;
        int64_t duration = 0;
        int64_t drop_to_pts = 0;
        
        //Lock
        SDL_LockMutex(is->videoq.mutex);
        
        time_base_valid = is->video_st->time_base.den > 0 && is->video_st->time_base.num > 0;
        nb_packets = is->videoq.nb_packets;
        
        // TOFIX: if time_base_valid false, calc duration with nb_packets and framerate
        // 为什么不用 videoq.duration？因为遇到过videoq.duration 一直为0，audioq也一样
        if (time_base_valid) {
            if (is->videoq.first_pkt && is->videoq.last_pkt) {
                duration = is->videoq.last_pkt->pkt.pts - is->videoq.first_pkt->pkt.pts;
                cached_duration = duration * av_q2d(is->video_st->time_base) * 1000;
            }
        }
        
        if (cached_duration > is->max_cached_duration) {
            // drop
            av_log(NULL, AV_LOG_INFO, "233 video cached_duration = %lld, nb_packets = %d.\n", cached_duration, nb_packets);
            drop_to_pts = is->videoq.last_pkt->pkt.pts - is->max_cached_duration;  // 这里删掉一半，你也可以自己修改，依据设置进来的max_cached_duration大小
            drop_queue_until_pts(&is->videoq, drop_to_pts);
        }
        
        //Unlock
        SDL_UnlockMutex(is->videoq.mutex);
    }
    
    static void control_audio_queue_duration(FFPlayer *ffp, VideoState *is) {
        int time_base_valid = 0;
        int64_t cached_duration = -1;
        int nb_packets = 0;
        int64_t duration = 0;
        int64_t drop_to_pts = 0;
        
        //Lock
        SDL_LockMutex(is->audioq.mutex);
        
        time_base_valid = is->audio_st->time_base.den > 0 && is->audio_st->time_base.num > 0;
        nb_packets = is->audioq.nb_packets;
        
        // TOFIX: if time_base_valid false, calc duration with nb_packets and samplerate
        if (time_base_valid) {
            if (is->audioq.first_pkt && is->audioq.last_pkt) {
                duration = is->audioq.last_pkt->pkt.pts - is->audioq.first_pkt->pkt.pts;
                cached_duration = duration * av_q2d(is->audio_st->time_base) * 1000;
            }
        }
        
        if (cached_duration > is->max_cached_duration) {
            // drop
            av_log(NULL, AV_LOG_INFO, "233 audio cached_duration = %lld, nb_packets = %d.\n", cached_duration, nb_packets);
            drop_to_pts = is->audioq.last_pkt->pkt.pts - is->max_cached_duration;
            drop_queue_until_pts(&is->audioq, drop_to_pts);
        }
        
        //Unlock
        SDL_UnlockMutex(is->audioq.mutex);
    }
    
    static void control_queue_duration(FFPlayer *ffp, VideoState *is) {
        if (is->max_cached_duration <= 0) {
            return;
        }
        
        if (is->audio_st) {
            return control_audio_queue_duration(ffp, is);
        }
        if (is->video_st) {
            return control_video_queue_duration(ffp, is);
        }
        
}

static void save_record_data(FFPlayer *ffp, AVPacket *pkt, int64_t *first_rec_pts, int64_t *first_rec_dts, int64_t *first_audio_rec_pts, int64_t *first_audio_rec_dts){
        //write record
        VideoState *is = ffp->is;
        AVStream *in_stream, *out_stream;
        int out_index;
        in_stream = is->ic->streams[pkt->stream_index];
        // out_stream = is->ofmt_ctx->streams[pkt->stream_index];
        AVPacket *repkt = av_malloc(sizeof(AVPacket));
        av_init_packet(repkt);
        repkt->data = NULL;
        repkt->size = 0;
        av_new_packet(repkt, pkt->size);
        memcpy(repkt->data, pkt->data, pkt->size);
        repkt->size = pkt->size;
        
        if (pkt->stream_index == is->video_stream){
            out_index = is->video_out_stream_index;
            out_stream = is->ofmt_ctx->streams[is->video_out_stream_index];
            if (*first_rec_pts == 0){
                *first_rec_pts = pkt->pts;
            }
            
            if (*first_rec_dts == 0){
                *first_rec_dts = pkt->dts;
            }
            
        }
        
        if (pkt->stream_index == is->audio_stream){
            out_index = is->audio_out_stream_index;
            out_stream = is->ofmt_ctx->streams[is->audio_out_stream_index];
            //av_log(NULL, AV_LOG_DEBUG,"%s:%d\n", __func__, __LINE__);
            if (*first_audio_rec_pts == 0){
                *first_audio_rec_pts = pkt->pts;
            }
            
            if (*first_audio_rec_dts == 0){
                *first_audio_rec_dts = pkt->dts;
            }
            
        }
        
        repkt->pts = pkt->pts - (pkt->stream_index == is->video_stream ? *first_rec_pts : *first_audio_rec_pts);
        repkt->dts = pkt->dts - (pkt->stream_index == is->video_stream ? *first_rec_dts : *first_audio_rec_dts);
        //repkt->pts = pkt->pts;
        //repkt->dts = pkt->dts;
        repkt->stream_index = out_index;
        repkt->flags = pkt->flags;
        //repkt->duration = pkt->duration;
        repkt->pts = av_rescale_q_rnd(repkt->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        repkt->dts = av_rescale_q_rnd(repkt->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        repkt->duration = av_rescale_q(repkt->duration, in_stream->time_base, out_stream->time_base);
        repkt->pos = -1;
        if (is->ic->streams[pkt->stream_index]->codec->codec_id == AV_CODEC_ID_AAC){
            av_bitstream_filter_filter(is->aacbsfc, is->ic->streams[pkt->stream_index]->codec, NULL, &repkt->data, &repkt->size, repkt->data, repkt->size, 0);
        }
        
        av_interleaved_write_frame(is->ofmt_ctx, repkt);
        av_free_packet(repkt);
}


/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    AVFormatContext *ic = NULL;
    int err, i, ret __unused;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int completed = 0;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    AVDictionary **opts;
    int orig_nb_streams;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;
    int last_error = 0;
    int64_t prev_io_tick_counter = 0;
    int64_t io_tick_counter = 0;
    int initRecordFile = 0;
    AVPacket *repkt = NULL;
    int iFirstWrite = 1;
    int can_be_write = 0;
    int can_be_put_vid_packet = 0;
	int64_t first_rec_pts=0, first_rec_dts=0;
	int64_t first_audio_rec_pts = 0, first_audio_rec_dts = 0;
    
    record_queue_init();
    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(ffp->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&ffp->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    if (av_stristart(is->filename, "rtmp", NULL) ||
        av_stristart(is->filename, "rtsp", NULL)) {
        // There is total different meaning for 'timeout' option in rtmp
        av_log(ffp, AV_LOG_WARNING, "remove 'timeout' option for rtmp.\n");
        av_dict_set(&ffp->format_opts, "timeout", NULL, 0);
    }
    if (ffp->iformat_name)
        is->iformat = av_find_input_format(ffp->iformat_name);
    err = avformat_open_input(&ic, is->filename, is->iformat, &ffp->format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    if (scan_all_pmts_set)
        av_dict_set(&ffp->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(ffp->format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
#ifdef FFP_MERGE
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
#endif
    }
    is->ic = ic;

    if (ffp->genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);

    opts = setup_find_stream_info_opts(ic, ffp->codec_opts);
    orig_nb_streams = ic->nb_streams;

    err = avformat_find_stream_info(ic, opts);

    for (i = 0; i < orig_nb_streams; i++)
        av_dict_free(&opts[i]);
    av_freep(&opts);

    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "%s: could not find codec parameters\n", is->filename);
        ret = -1;
        goto fail;
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (ffp->seek_by_bytes < 0)
        ffp->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
//    is->max_frame_duration = 10.0;
    av_log(ffp, AV_LOG_INFO, "max_frame_duration: %.3f\n", is->max_frame_duration);

#ifdef FFP_MERGE
    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

#endif
    /* if seeking requested, we execute it */
    if (ffp->start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = ffp->start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);
    //is->realtime = 0;
    AVDictionaryEntry *e = av_dict_get(ffp->player_opts, "max_cached_duration", NULL, 0);
    if (e) {
        int max_cached_duration = atoi(e->value);
        if (max_cached_duration <= 0) {
            is->max_cached_duration = 0;
        } else {
            is->max_cached_duration = max_cached_duration;
        }
    } else {
        is->max_cached_duration = 0;
    }

    if (true || ffp->show_status)
        av_dump_format(ic, 0, is->filename, 0);

    int video_stream_count = 0;
    int h264_stream_count = 0;
    int first_h264_stream = -1;
    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && ffp->wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, ffp->wanted_stream_spec[type]) > 0)
                st_index[type] = i;

        // choose first h264

        if (type == AVMEDIA_TYPE_VIDEO) {
            enum AVCodecID codec_id = st->codecpar->codec_id;
            video_stream_count++;
            if (codec_id == AV_CODEC_ID_H264) {
                h264_stream_count++;
                if (first_h264_stream < 0)
                    first_h264_stream = i;
            }
        }
    }
    if (video_stream_count > 1 && st_index[AVMEDIA_TYPE_VIDEO] < 0) {
        st_index[AVMEDIA_TYPE_VIDEO] = first_h264_stream;
        av_log(NULL, AV_LOG_WARNING, "multiple video stream found, prefer first h264 stream: %d\n", first_h264_stream);
    }
    if (!ffp->video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!ffp->audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (!ffp->video_disable && !ffp->subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);

    is->show_mode = ffp->show_mode;
#ifdef FFP_MERGE // bbc: dunno if we need this
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height, sar);
    }
#endif

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(ffp, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(ffp, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(ffp, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    ijkmeta_set_avformat_context_l(ffp->meta, ic);
    ffp->stat.bit_rate = ic->bit_rate;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_VIDEO_STREAM, st_index[AVMEDIA_TYPE_VIDEO]);
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0)
        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_AUDIO_STREAM, st_index[AVMEDIA_TYPE_AUDIO]);
    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0)
        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_TIMEDTEXT_STREAM, st_index[AVMEDIA_TYPE_SUBTITLE]);

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }
    if (is->audio_stream >= 0) {
        is->audioq.is_buffer_indicator = 1;
        is->buffer_indicator_queue = &is->audioq;
    } else if (is->video_stream >= 0) {
        is->videoq.is_buffer_indicator = 1;
        is->buffer_indicator_queue = &is->videoq;
    } else {
        assert("invalid streams");
    }

    if (ffp->infinite_buffer < 0 && is->realtime)
        ffp->infinite_buffer = 1;

    if (!ffp->start_on_prepared)
        toggle_pause(ffp, 1);
    if (is->video_st && is->video_st->codecpar) {
        AVCodecParameters *codecpar = is->video_st->codecpar;
        ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, codecpar->width, codecpar->height);
        ffp_notify_msg3(ffp, FFP_MSG_SAR_CHANGED, codecpar->sample_aspect_ratio.num, codecpar->sample_aspect_ratio.den);
    }
    ffp->prepared = true;
    ffp_notify_msg1(ffp, FFP_MSG_PREPARED);
    if (!ffp->start_on_prepared) {
        while (is->pause_req && !is->abort_request) {
            SDL_Delay(100);
        }
    }
    if (ffp->auto_resume) {
        ffp_notify_msg1(ffp, FFP_REQ_START);
        ffp->auto_resume = 0;
    }
    /* offset should be seeked*/
    if (ffp->seek_at_start > 0) {
        ffp_seek_to_l(ffp, ffp->seek_at_start);
    }

    for (;;) {
        if (is->abort_request)
            break;
#ifdef FFP_MERGE
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#endif
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(ffp->input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ffp_toggle_buffering(ffp, 1);
            ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_UPDATE, 0, 0);
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->filename);
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    if (ffp->node_vdec) {
                        ffpipenode_flush(ffp->node_vdec);
                    }
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->extclk, NAN, 0);
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }

                is->latest_seek_load_serial = is->videoq.serial;
                is->latest_seek_load_start_at = av_gettime();
            }
            ffp->dcc.current_high_water_mark_in_ms = ffp->dcc.first_high_water_mark_in_ms;
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
#ifdef FFP_MERGE
            if (is->paused)
                step_to_next_frame(is);
#endif
            completed = 0;
            SDL_LockMutex(ffp->is->play_mutex);
            if (ffp->auto_resume) {
                is->pause_req = 0;
                if (ffp->packet_buffering)
                    is->buffering_on = 1;
                ffp->auto_resume = 0;
                stream_update_pause_l(ffp);
            }
            if (is->pause_req)
                step_to_next_frame_l(ffp);
            SDL_UnlockMutex(ffp->is->play_mutex);
            ffp_notify_msg3(ffp, FFP_MSG_SEEK_COMPLETE, (int)fftime_to_milliseconds(seek_target), ret);
            ffp_toggle_buffering(ffp, 1);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                AVPacket copy;
                if ((ret = av_copy_packet(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (ffp->infinite_buffer<1 && !is->seek_req &&
#ifdef FFP_MERGE
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
#else
              (is->audioq.size + is->videoq.size + is->subtitleq.size > ffp->dcc.max_buffer_size
#endif
            || (   stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq, MIN_FRAMES)
                && stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq, MIN_FRAMES)
                && stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq, MIN_FRAMES)))) {
            if (!is->eof) {
                ffp_toggle_buffering(ffp, 0);
            }
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if ((!is->paused || completed) &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (ffp->loop != 1 && (!ffp->loop || --ffp->loop)) {
                stream_seek(is, ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0, 0, 0);
            } else if (ffp->autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            } else {
                ffp_statistic_l(ffp);
                if (completed) {
                    av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: eof\n");
                    SDL_LockMutex(wait_mutex);
                    // infinite wait may block shutdown
                    while(!is->abort_request && !is->seek_req)
                        SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 100);
                    SDL_UnlockMutex(wait_mutex);
                    if (!is->abort_request)
                        continue;
                } else {
                    completed = 1;
                    ffp->auto_resume = 0;

                    // TODO: 0 it's a bit early to notify complete here
                    ffp_toggle_buffering(ffp, 0);
                    toggle_pause(ffp, 1);
                    if (ffp->error) {
                        av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: error: %d\n", ffp->error);
                        ffp_notify_msg1(ffp, FFP_MSG_ERROR);
                    } else {
                        av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: completed: OK\n");
                        ffp_notify_msg1(ffp, FFP_MSG_COMPLETED);
                    }
                }
            }
        }
        pkt->flags = 0;
            //本地录像初始化文件输出格式
        if(ffp->m_bRecorder && !initRecordFile)
        {
            initRecordFile = init_record_file(ffp);
                
            if(!initRecordFile)
            {
                //PostMessage(play->m_hWnd,WM_Wireless_INFO,INFO_DEBUG_INIT_RECORD_ERR,0);//初始化录像文件失败
                av_log(NULL, AV_LOG_DEBUG, "init recorder file failed");
                ffp->m_bRecorder = 0;
            }
            
        }
            
            //停止录像
        if(!ffp->m_bRecorder && initRecordFile)
        {
            close_record_file(ffp);
            initRecordFile = 0;
        }
        ret = av_read_frame(ic, pkt);
        av_log(ffp, AV_LOG_DEBUG, "new stream_index == %d\n", pkt->stream_index);
        if (ret < 0) {
            int pb_eof = 0;
            int pb_error = 0;
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                pb_eof = 1;
                // check error later
            }
            if (ic->pb && ic->pb->error) {
                pb_eof = 1;
                pb_error = ic->pb->error;
                //关闭录像
                if(ffp->m_bRecorder)
                    close_record_file(ffp);
            }
            if (ret == AVERROR_EXIT) {
                pb_eof = 1;
                pb_error = AVERROR_EXIT;
            }

            if (pb_eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
            }
            if (pb_error) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
                ffp->error = pb_error;
                av_log(ffp, AV_LOG_ERROR, "av_read_frame error: %x(%c,%c,%c,%c): %s\n", ffp->error,
                      (char) (0xff & (ffp->error >> 24)),
                      (char) (0xff & (ffp->error >> 16)),
                      (char) (0xff & (ffp->error >> 8)),
                      (char) (0xff & (ffp->error)),
                      ffp_get_error_string(ffp->error));
                // break;
            } else {
                ffp->error = 0;
            }
            if (is->eof) {
                ffp_toggle_buffering(ffp, 0);
                SDL_Delay(100);
            }
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            ffp_statistic_l(ffp);
            continue;
        } else {
            is->eof = 0;
        }

        if (pkt->flags & AV_PKT_FLAG_DISCONTINUITY) {
            if (is->audio_stream >= 0) {
                packet_queue_put(&is->audioq, &flush_pkt);
            }
            if (is->subtitle_stream >= 0) {
                packet_queue_put(&is->subtitleq, &flush_pkt);
            }
            if (is->video_stream >= 0) {
                packet_queue_put(&is->videoq, &flush_pkt);
            }
        }

        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = ffp->duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0) / 1000000
                <= ((double)ffp->duration / 1000000);
            //本地录像
            if(ffp->m_bRecorder == 1 && initRecordFile == 1)
            {
                repkt = av_malloc(sizeof(AVPacket));
                av_init_packet(repkt);
                repkt->data = NULL;
                repkt->size = 0;
                av_new_packet(repkt, pkt->size);
                memcpy( repkt->data, pkt->data, pkt->size);
                repkt->pts = pkt->pts;
                repkt->dts = pkt->dts;
                repkt->stream_index = pkt->stream_index;
                repkt->flags = pkt->flags;
                repkt->duration = pkt->duration;
                repkt->pos = pkt->pos;
                
                //第一次写文件， 则是必须等到视频关键帧再写文件
                if(iFirstWrite == 1)
                {
                    if (pkt->stream_index == is->video_stream)//视频帧
                    {
                        if ((pkt->flags & AV_PKT_FLAG_KEY) == AV_PKT_FLAG_KEY)//关键帧
                        {
                            
                            if(is->m_oformat_ctx)//MP4文件
                            {
                                repkt->stream_index = 0;
                                if(repkt->pts!=AV_NOPTS_VALUE);
                                repkt->pts = av_rescale_q(repkt->pts,is->ic->streams[is->video_stream]->time_base,
                                                          is->m_oformat_ctx->streams[is->video_stream]->time_base);
                                if(repkt->dts!=AV_NOPTS_VALUE);
                                repkt->dts = av_rescale_q(repkt->dts,is->ic->streams[is->video_stream]->time_base,
                                                          is->m_oformat_ctx->streams[is->video_stream]->time_base);
                                av_interleaved_write_frame(is->m_oformat_ctx, repkt);
                            }
                            iFirstWrite = 0;
                        }
                    }
                }
                else
                {
                    if(is->m_oformat_ctx)//MP4文件
                    {
                        int packetindex = 0;
                        if (repkt->stream_index == is->video_stream)
                            packetindex = 0;
                        if (repkt->stream_index == is->audio_stream)
                            packetindex = 1;
                        if(repkt->pts!=AV_NOPTS_VALUE);
                        repkt->pts = av_rescale_q(repkt->pts,is->ic->streams[repkt->stream_index]->time_base,
                                                  is->m_oformat_ctx->streams[packetindex]->time_base);
                        if(repkt->dts!=AV_NOPTS_VALUE);
                        repkt->dts = av_rescale_q(repkt->dts,is->ic->streams[repkt->stream_index]->time_base,
                                                  is->m_oformat_ctx->streams[packetindex]->time_base);
                        repkt->stream_index = packetindex;
                        av_interleaved_write_frame(is->m_oformat_ctx, repkt);
                        
                    }
                    
                }
                av_free_packet(repkt);
                av_freep(&repkt);
                //pkt = NULL;	
            }
            else
            iFirstWrite = 0;
            
            //local file record start
            AVDictionaryEntry *e = av_dict_get(ffp->player_opts, "local_record_start", NULL, 0);
            if (e) {
                int local_record_start = atoi(e->value);
                if (local_record_start <= 0) {
                    is->local_record_start = 0;
                } else {
                    is->local_record_start = local_record_start;
                    AVDictionaryEntry *e = av_dict_get(ffp->player_opts, "local_record_filename", NULL, 0);
                    
                    if (e) {
                        is->local_record_filename = e->value;
                    }else{
                        is->local_record_filename = "";
                    }
                }
            } else {
                is->local_record_start = 0;
            }
            
            if((pkt->flags & AV_PKT_FLAG_KEY) == AV_PKT_FLAG_KEY&&pkt->stream_index == is->video_stream){//±£¥Ê…œ“ª∏ˆIFrame
                //*keyPkt = *pkt;
                //av_log(NULL, AV_LOG_ERROR, "%s:%d, recorQueue.max_size = %d\n", __func__, __LINE__, recorQueue.max_size);
                recorQueue.writeIndex = 0;
                recorQueue.readIndex = 0;
                recorQueue.max_size = 0;
                AVPacket *tmpPkt= recorQueue.rePkt[recorQueue.writeIndex].recordPkt;
                av_copy_packet(tmpPkt, pkt);
                recorQueue.writeIndex++;
                recorQueue.max_size++;
                
            }else if(recorQueue.writeIndex > 0){
                if(recorQueue.writeIndex == MAX_RECORD_CACHE){
                    //av_log(NULL, AV_LOG_ERROR, "%s:%d, recorQueue.max_size = %d\n", __func__, __LINE__, recorQueue.max_size);
                    recorQueue.writeIndex = 0;
                    recorQueue.max_size = 0;
                    av_log(NULL, AV_LOG_ERROR, "There is dangerous that first frame not a I frame\n");
                }
                AVPacket *tmpPkt= recorQueue.rePkt[recorQueue.writeIndex].recordPkt;
                av_copy_packet(tmpPkt, pkt);
                recorQueue.writeIndex++;
                recorQueue.max_size++;
            }
            if(is->local_record_start){
                
                if (!can_be_write)
                {
                    if (!is->ofmt_ctx)
                        mw_initOutPutStream(ffp, is->local_record_filename);
                    
                    if (is->ofmt_ctx)
                    {
                        can_be_write = 1;
                        for(int rIndex = 0; rIndex < recorQueue.max_size-1; rIndex++){
                            AVPacket *mRecPacket = recorQueue.rePkt[rIndex].recordPkt;
                            save_record_data(ffp, mRecPacket, &first_rec_pts, &first_rec_dts, &first_audio_rec_pts, &first_audio_rec_dts);
                        }
                        //save_record_data(ffp, keyPkt, &first_rec_pts, &first_rec_dts, &first_audio_rec_pts, &first_audio_rec_dts);
                    }
                }
                
                if (can_be_write)
                {
                    save_record_data(ffp, pkt, &first_rec_pts, &first_rec_dts, &first_audio_rec_pts, &first_audio_rec_dts);
                }
            }else if(!is->local_record_start&&is->ofmt_ctx){
                can_be_write = 0;
                first_rec_pts=0;
                first_rec_dts=0;
                first_audio_rec_pts = 0;
                first_audio_rec_dts = 0;
                mw_closeOutPutStream(ffp);
        }
        // 每次读取一个pkt，都去判断处理
        // TODO:优化，不用每次都调用
        if (is->max_cached_duration > 0) {
            control_queue_duration(ffp, is);
        }
        
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))) {
            if((pkt->flags & AV_PKT_FLAG_KEY) == AV_PKT_FLAG_KEY && !can_be_put_vid_packet){
                AVDictionaryEntry *e = av_dict_get(ffp->player_opts, "iframe-root", NULL, 0);
                if(e){
                    FILE *pFile = fopen(e->value, "wb");
                    if(pFile){
                        fwrite(pkt->data, pkt->size, 1, pFile);
                    }
                    fclose(pFile);
                }
                can_be_put_vid_packet = 1;
            }
            if(can_be_put_vid_packet){
                packet_queue_put(&is->videoq, pkt);
            }else{
                AVDictionaryEntry *e = av_dict_get(ffp->player_opts, "iframe-root", NULL, 0);
                if(e){
                FILE *pFile = fopen(e->value, "rb");
                if(pFile){
                    AVPacket *keypkt = av_malloc(sizeof(AVPacket));
                    av_init_packet(keypkt);
                    keypkt->data = NULL;
                    keypkt->size = 0;
                    
                    //求得文件的大小
                    fseek(pFile, 0, SEEK_END);
                    int bufSize = ftell(pFile);
                    if(bufSize > 0){
                        av_new_packet(keypkt, bufSize);
                        fseek(pFile,0,SEEK_SET);
                        
                        //申请一块能装下整个文件的空间
                        uint8_t *ar = (uint8_t*)malloc(sizeof(uint8_t)*bufSize);
                        fread(ar, 1, bufSize, pFile);
                        if(bufSize>0){
                            //av_log(NULL, AV_LOG_DEBUG, "%s:%d\n", __func__, __LINE__);
                            //memset(pkt->data, 0, pkt->size);
                            //av_log(NULL, AV_LOG_DEBUG, "%s:%d\n", __func__, __LINE__);
                            memcpy(keypkt->data, ar, bufSize);
                            keypkt->pts = pkt->pts;
                            keypkt->flags = AV_PKT_FLAG_KEY;
                            //av_log(NULL, AV_LOG_DEBUG, "%s:%d\n", __func__, __LINE__);
                            pkt->size = bufSize;
                        }
                        free(ar);
                        ar = NULL;
                        //can_be_put_vid_packet =1;
                        packet_queue_put(&is->videoq, keypkt);
                    }
                    fclose(pFile);
                }
                }
            }
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }

        ffp_statistic_l(ffp);

        if (ffp->packet_buffering) {
            io_tick_counter = SDL_GetTickHR();
            if (abs((int)(io_tick_counter - prev_io_tick_counter)) > BUFFERING_CHECK_PER_MILLISECONDS) {
                prev_io_tick_counter = io_tick_counter;
                ffp_check_buffering_l(ffp);
            }
        }
    }

    ret = 0;
 fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    if (!ffp->prepared || !is->abort_request) {
        ffp->last_error = last_error;
        ffp_notify_msg2(ffp, FFP_MSG_ERROR, last_error);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

static int video_refresh_thread(void *arg);
static VideoState *stream_open(FFPlayer *ffp, const char *filename, AVInputFormat *iformat)
{
    assert(!ffp->is);
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->iformat = iformat;
    is->ytop    = 0;
    is->xleft   = 0;

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, ffp->pictq_size, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    is->audio_volume = SDL_MIX_MAXVOLUME;
    is->muted = 0;
    is->av_sync_type = ffp->av_sync_type;

    is->play_mutex = SDL_CreateMutex();
    ffp->is = is;
    is->pause_req = !ffp->start_on_prepared;

    is->video_refresh_tid = SDL_CreateThreadEx(&is->_video_refresh_tid, video_refresh_thread, ffp, "ff_vout");
    if (!is->video_refresh_tid) {
        av_freep(&ffp->is);
        return NULL;
    }

    is->read_tid = SDL_CreateThreadEx(&is->_read_tid, read_thread, ffp, "ff_read");
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        is->abort_request = true;
        if (is->video_refresh_tid)
            SDL_WaitThread(is->video_refresh_tid, NULL);
        stream_close(ffp);
        return NULL;
    }
    return is;
}

// FFP_MERGE: stream_cycle_channel
// FFP_MERGE: toggle_full_screen
// FFP_MERGE: toggle_audio_display
// FFP_MERGE: refresh_loop_wait_event
// FFP_MERGE: event_loop
// FFP_MERGE: opt_frame_size
// FFP_MERGE: opt_width
// FFP_MERGE: opt_height
// FFP_MERGE: opt_format
// FFP_MERGE: opt_frame_pix_fmt
// FFP_MERGE: opt_sync
// FFP_MERGE: opt_seek
// FFP_MERGE: opt_duration
// FFP_MERGE: opt_show_mode
// FFP_MERGE: opt_input_file
// FFP_MERGE: opt_codec
// FFP_MERGE: dummy
// FFP_MERGE: options
// FFP_MERGE: show_usage
// FFP_MERGE: show_help_default
static int video_refresh_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    double remaining_time = 0.0;
    while (!is->abort_request) {
        if (remaining_time > 0.0)
            av_usleep((int)(int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
            video_refresh(ffp, &remaining_time);
    }

    return 0;
}

static int lockmgr(void **mtx, enum AVLockOp op)
{
    switch (op) {
    case AV_LOCK_CREATE:
        *mtx = SDL_CreateMutex();
        if (!*mtx) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
            return 1;
        }
        return 0;
    case AV_LOCK_OBTAIN:
        return !!SDL_LockMutex(*mtx);
    case AV_LOCK_RELEASE:
        return !!SDL_UnlockMutex(*mtx);
    case AV_LOCK_DESTROY:
        SDL_DestroyMutex(*mtx);
        return 0;
    }
    return 1;
}

// FFP_MERGE: main

/*****************************************************************************
 * end last line in ffplay.c
 ****************************************************************************/

static bool g_ffmpeg_global_inited = false;

inline static int log_level_av_to_ijk(int av_level)
{
    int ijk_level = IJK_LOG_VERBOSE;
    if      (av_level <= AV_LOG_PANIC)      ijk_level = IJK_LOG_FATAL;
    else if (av_level <= AV_LOG_FATAL)      ijk_level = IJK_LOG_FATAL;
    else if (av_level <= AV_LOG_ERROR)      ijk_level = IJK_LOG_ERROR;
    else if (av_level <= AV_LOG_WARNING)    ijk_level = IJK_LOG_WARN;
    else if (av_level <= AV_LOG_INFO)       ijk_level = IJK_LOG_INFO;
    // AV_LOG_VERBOSE means detailed info
    else if (av_level <= AV_LOG_VERBOSE)    ijk_level = IJK_LOG_INFO;
    else if (av_level <= AV_LOG_DEBUG)      ijk_level = IJK_LOG_DEBUG;
    else if (av_level <= AV_LOG_TRACE)      ijk_level = IJK_LOG_VERBOSE;
    else                                    ijk_level = IJK_LOG_VERBOSE;
    return ijk_level;
}

inline static int log_level_ijk_to_av(int ijk_level)
{
    int av_level = IJK_LOG_VERBOSE;
    if      (ijk_level >= IJK_LOG_SILENT)   av_level = AV_LOG_QUIET;
    else if (ijk_level >= IJK_LOG_FATAL)    av_level = AV_LOG_FATAL;
    else if (ijk_level >= IJK_LOG_ERROR)    av_level = AV_LOG_ERROR;
    else if (ijk_level >= IJK_LOG_WARN)     av_level = AV_LOG_WARNING;
    else if (ijk_level >= IJK_LOG_INFO)     av_level = AV_LOG_INFO;
    // AV_LOG_VERBOSE means detailed info
    else if (ijk_level >= IJK_LOG_DEBUG)    av_level = AV_LOG_DEBUG;
    else if (ijk_level >= IJK_LOG_VERBOSE)  av_level = AV_LOG_TRACE;
    else if (ijk_level >= IJK_LOG_DEFAULT)  av_level = AV_LOG_TRACE;
    else if (ijk_level >= IJK_LOG_UNKNOWN)  av_level = AV_LOG_TRACE;
    else                                    av_level = AV_LOG_TRACE;
    return av_level;
}

static void ffp_log_callback_brief(void *ptr, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;

    int ffplv __unused = log_level_av_to_ijk(level);
    VLOG(ffplv, IJK_LOG_TAG, fmt, vl);
}

static void ffp_log_callback_report(void *ptr, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;

    int ffplv __unused = log_level_av_to_ijk(level);

    va_list vl2;
    char line[1024];
    static int print_prefix = 1;

    va_copy(vl2, vl);
    // av_log_default_callback(ptr, level, fmt, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
    va_end(vl2);

    ALOG(ffplv, IJK_LOG_TAG, "%s", line);
}

int ijkav_register_all(void);
void ffp_global_init()
{
    if (g_ffmpeg_global_inited)
        return;

    /* register all codecs, demux and protocols */
    avcodec_register_all();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    avfilter_register_all();
#endif
    av_register_all();

    ijkav_register_all();

    avformat_network_init();

    av_lockmgr_register(lockmgr);
    av_log_set_callback(ffp_log_callback_brief);

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

    g_ffmpeg_global_inited = true;
}

void ffp_global_uninit()
{
    if (!g_ffmpeg_global_inited)
        return;

    av_lockmgr_register(NULL);

    // FFP_MERGE: uninit_opts

    avformat_network_deinit();

    g_ffmpeg_global_inited = false;
}

void ffp_global_set_log_report(int use_report)
{
    if (use_report) {
        av_log_set_callback(ffp_log_callback_report);
    } else {
        av_log_set_callback(ffp_log_callback_brief);
    }
}

void ffp_global_set_log_level(int log_level)
{
    int av_level = log_level_ijk_to_av(log_level);
    av_log_set_level(av_level);
}

static ijk_inject_callback s_inject_callback;
int inject_callback(void *opaque, int type, void *data, size_t data_size)
{
    if (s_inject_callback)
        return s_inject_callback(opaque, type, data, data_size);
    return 0;
}

void ffp_global_set_inject_callback(ijk_inject_callback cb)
{
    s_inject_callback = cb;
}

void ffp_io_stat_register(void (*cb)(const char *url, int type, int bytes))
{
    // avijk_io_stat_register(cb);
}

void ffp_io_stat_complete_register(void (*cb)(const char *url,
                                              int64_t read_bytes, int64_t total_size,
                                              int64_t elpased_time, int64_t total_duration))
{
    // avijk_io_stat_complete_register(cb);
}

static const char *ffp_context_to_name(void *ptr)
{
    return "FFPlayer";
}


static void *ffp_context_child_next(void *obj, void *prev)
{
    return NULL;
}

static const AVClass *ffp_context_child_class_next(const AVClass *prev)
{
    return NULL;
}

const AVClass ffp_context_class = {
    .class_name       = "FFPlayer",
    .item_name        = ffp_context_to_name,
    .option           = ffp_context_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .child_next       = ffp_context_child_next,
    .child_class_next = ffp_context_child_class_next,
};

static const char *ijk_version_info()
{
    return IJKPLAYER_VERSION;
}

FFPlayer *ffp_create()
{
    av_log(NULL, AV_LOG_INFO, "av_version_info: %s\n", av_version_info());
    av_log(NULL, AV_LOG_INFO, "ijk_version_info: %s\n", ijk_version_info());

    FFPlayer* ffp = (FFPlayer*) av_mallocz(sizeof(FFPlayer));
    if (!ffp)
        return NULL;

    msg_queue_init(&ffp->msg_queue);
    ffp->af_mutex = SDL_CreateMutex();
    ffp->vf_mutex = SDL_CreateMutex();

    ffp_reset_internal(ffp);
    ffp->av_class = &ffp_context_class;
    ffp->meta = ijkmeta_create();

    av_opt_set_defaults(ffp);

    return ffp;
}

void ffp_destroy(FFPlayer *ffp)
{
    if (!ffp)
        return;

    if (ffp->is) {
        av_log(NULL, AV_LOG_WARNING, "ffp_destroy_ffplayer: force stream_close()");
        stream_close(ffp);
        ffp->is = NULL;
    }

    SDL_VoutFreeP(&ffp->vout);
    SDL_AoutFreeP(&ffp->aout);
    ffpipenode_free_p(&ffp->node_vdec);
    ffpipeline_free_p(&ffp->pipeline);
    ijkmeta_destroy_p(&ffp->meta);
    ffp_reset_internal(ffp);

    SDL_DestroyMutexP(&ffp->af_mutex);
    SDL_DestroyMutexP(&ffp->vf_mutex);

    msg_queue_destroy(&ffp->msg_queue);


    av_free(ffp);
}

void ffp_destroy_p(FFPlayer **pffp)
{
    if (!pffp)
        return;

    ffp_destroy(*pffp);
    *pffp = NULL;
}

static AVDictionary **ffp_get_opt_dict(FFPlayer *ffp, int opt_category)
{
    assert(ffp);

    switch (opt_category) {
        case FFP_OPT_CATEGORY_FORMAT:   return &ffp->format_opts;
        case FFP_OPT_CATEGORY_CODEC:    return &ffp->codec_opts;
        case FFP_OPT_CATEGORY_SWS:      return &ffp->sws_dict;
        case FFP_OPT_CATEGORY_PLAYER:   return &ffp->player_opts;
        case FFP_OPT_CATEGORY_SWR:      return &ffp->swr_opts;
        default:
            av_log(ffp, AV_LOG_ERROR, "unknown option category %d\n", opt_category);
            return NULL;
    }
}

static int app_func_event(AVApplicationContext *h, int message ,void *data, size_t size)
{
    if (!h || !h->opaque || !data)
        return 0;

    FFPlayer *ffp = (FFPlayer *)h->opaque;
    if (!ffp->inject_opaque)
        return 0;
    if (message == AVAPP_EVENT_IO_TRAFFIC && sizeof(AVAppIOTraffic) == size) {
        AVAppIOTraffic *event = (AVAppIOTraffic *)(intptr_t)data;
        if (event->bytes > 0)
            SDL_SpeedSampler2Add(&ffp->stat.tcp_read_sampler, event->bytes);
    } else if (message == AVAPP_EVENT_ASYNC_STATISTIC && sizeof(AVAppAsyncStatistic) == size) {
        AVAppAsyncStatistic *statistic =  (AVAppAsyncStatistic *) (intptr_t)data;
        ffp->stat.buf_backwards = statistic->buf_backwards;
        ffp->stat.buf_forwards = statistic->buf_forwards;
        ffp->stat.buf_capacity = statistic->buf_capacity;
    }
    return inject_callback(ffp->inject_opaque, message , data, size);
}

void *ffp_set_inject_opaque(FFPlayer *ffp, void *opaque)
{
    if (!ffp)
        return NULL;
    void *prev_weak_thiz = ffp->inject_opaque;
    ffp->inject_opaque = opaque;

    av_application_closep(&ffp->app_ctx);
    av_application_open(&ffp->app_ctx, ffp);
    ffp_set_option_int(ffp, FFP_OPT_CATEGORY_FORMAT, "ijkapplication", (int64_t)(intptr_t)ffp->app_ctx);

    ffp->app_ctx->func_on_app_event = app_func_event;
    return prev_weak_thiz;
}

void ffp_set_option(FFPlayer *ffp, int opt_category, const char *name, const char *value)
{
    if (!ffp)
        return;

    AVDictionary **dict = ffp_get_opt_dict(ffp, opt_category);
    av_dict_set(dict, name, value, 0);
}

void ffp_set_option_int(FFPlayer *ffp, int opt_category, const char *name, int64_t value)
{
    if (!ffp)
        return;

    AVDictionary **dict = ffp_get_opt_dict(ffp, opt_category);
    av_dict_set_int(dict, name, value, 0);
}

void ffp_set_overlay_format(FFPlayer *ffp, int chroma_fourcc)
{
    switch (chroma_fourcc) {
        case SDL_FCC__GLES2:
        case SDL_FCC_I420:
        case SDL_FCC_YV12:
        case SDL_FCC_RV16:
        case SDL_FCC_RV24:
        case SDL_FCC_RV32:
            ffp->overlay_format = chroma_fourcc;
            break;
#ifdef __APPLE__
        case SDL_FCC_I444P10LE:
            ffp->overlay_format = chroma_fourcc;
            break;
#endif
        default:
            av_log(ffp, AV_LOG_ERROR, "ffp_set_overlay_format: unknown chroma fourcc: %d\n", chroma_fourcc);
            break;
    }
}

int ffp_get_video_codec_info(FFPlayer *ffp, char **codec_info)
{
    if (!codec_info)
        return -1;

    // FIXME: not thread-safe
    if (ffp->video_codec_info) {
        *codec_info = strdup(ffp->video_codec_info);
    } else {
        *codec_info = NULL;
    }
    return 0;
}

int ffp_get_audio_codec_info(FFPlayer *ffp, char **codec_info)
{
    if (!codec_info)
        return -1;

    // FIXME: not thread-safe
    if (ffp->audio_codec_info) {
        *codec_info = strdup(ffp->audio_codec_info);
    } else {
        *codec_info = NULL;
    }
    return 0;
}

static void ffp_show_dict(FFPlayer *ffp, const char *tag, AVDictionary *dict)
{
    AVDictionaryEntry *t = NULL;

    while ((t = av_dict_get(dict, "", t, AV_DICT_IGNORE_SUFFIX))) {
        av_log(ffp, AV_LOG_INFO, "%-*s: %-*s = %s\n", 12, tag, 28, t->key, t->value);
    }
}

#define FFP_VERSION_MODULE_NAME_LENGTH 13
static void ffp_show_version_str(FFPlayer *ffp, const char *module, const char *version)
{
        av_log(ffp, AV_LOG_INFO, "%-*s: %s\n", FFP_VERSION_MODULE_NAME_LENGTH, module, version);
}

static void ffp_show_version_int(FFPlayer *ffp, const char *module, unsigned version)
{
    av_log(ffp, AV_LOG_INFO, "%-*s: %u.%u.%u\n",
           FFP_VERSION_MODULE_NAME_LENGTH, module,
           (unsigned int)IJKVERSION_GET_MAJOR(version),
           (unsigned int)IJKVERSION_GET_MINOR(version),
           (unsigned int)IJKVERSION_GET_MICRO(version));
}

int ffp_prepare_async_l(FFPlayer *ffp, const char *file_name)
{
    assert(ffp);
    assert(!ffp->is);
    assert(file_name);

    if (av_stristart(file_name, "rtmp", NULL) ||
        av_stristart(file_name, "rtsp", NULL)) {
        // There is total different meaning for 'timeout' option in rtmp
        av_log(ffp, AV_LOG_WARNING, "remove 'timeout' option for rtmp.\n");
        av_dict_set(&ffp->format_opts, "timeout", NULL, 0);
    }

    /* there is a length limit in avformat */
    if (strlen(file_name) + 1 > 1024) {
        av_log(ffp, AV_LOG_ERROR, "%s too long url\n", __func__);
        if (avio_find_protocol_name("ijklongurl:")) {
            av_dict_set(&ffp->format_opts, "ijklongurl-url", file_name, 0);
            file_name = "ijklongurl:";
        }
    }

    av_log(NULL, AV_LOG_INFO, "===== versions =====\n");
    ffp_show_version_str(ffp, "ijkplayer",      ijk_version_info());
    ffp_show_version_str(ffp, "FFmpeg",         av_version_info());
    ffp_show_version_int(ffp, "libavutil",      avutil_version());
    ffp_show_version_int(ffp, "libavcodec",     avcodec_version());
    ffp_show_version_int(ffp, "libavformat",    avformat_version());
    ffp_show_version_int(ffp, "libswscale",     swscale_version());
    ffp_show_version_int(ffp, "libswresample",  swresample_version());
    av_log(NULL, AV_LOG_INFO, "===== options =====\n");
    ffp_show_dict(ffp, "player-opts", ffp->player_opts);
    ffp_show_dict(ffp, "format-opts", ffp->format_opts);
    ffp_show_dict(ffp, "codec-opts ", ffp->codec_opts);
    ffp_show_dict(ffp, "sws-opts   ", ffp->sws_dict);
    ffp_show_dict(ffp, "swr-opts   ", ffp->swr_opts);
    av_log(NULL, AV_LOG_INFO, "===================\n");

    av_opt_set_dict(ffp, &ffp->player_opts);
    if (!ffp->aout) {
        ffp->aout = ffpipeline_open_audio_output(ffp->pipeline, ffp);
        if (!ffp->aout)
            return -1;
    }

#if CONFIG_AVFILTER
    if (ffp->vfilter0) {
        GROW_ARRAY(ffp->vfilters_list, ffp->nb_vfilters);
        ffp->vfilters_list[ffp->nb_vfilters - 1] = ffp->vfilter0;
    }
#endif

    VideoState *is = stream_open(ffp, file_name, NULL);
    if (!is) {
        av_log(NULL, AV_LOG_WARNING, "ffp_prepare_async_l: stream_open failed OOM");
        return EIJK_OUT_OF_MEMORY;
    }

    ffp->is = is;
    ffp->input_filename = av_strdup(file_name);
    return 0;
}

int ffp_start_from_l(FFPlayer *ffp, long msec)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return EIJK_NULL_IS_PTR;

    ffp->auto_resume = 1;
    ffp_toggle_buffering(ffp, 1);
    ffp_seek_to_l(ffp, msec);
    return 0;
}

int ffp_start_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return EIJK_NULL_IS_PTR;

    toggle_pause(ffp, 0);
    return 0;
}

int ffp_pause_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return EIJK_NULL_IS_PTR;

    toggle_pause(ffp, 1);
    return 0;
}

int ffp_is_paused_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return 1;

    return is->paused;
}

int ffp_stop_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (is) {
        is->abort_request = 1;
        toggle_pause(ffp, 1);
    }

    msg_queue_abort(&ffp->msg_queue);
    return 0;
}

int ffp_wait_stop_l(FFPlayer *ffp)
{
    assert(ffp);

    if (ffp->is) {
        ffp_stop_l(ffp);
        stream_close(ffp);
        ffp->is = NULL;
    }
    return 0;
}

int ffp_seek_to_l(FFPlayer *ffp, long msec)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return EIJK_NULL_IS_PTR;

    int64_t seek_pos = milliseconds_to_fftime(msec);
    int64_t start_time = is->ic->start_time;
    if (start_time > 0 && start_time != AV_NOPTS_VALUE)
        seek_pos += start_time;

    // FIXME: 9 seek by bytes
    // FIXME: 9 seek out of range
    // FIXME: 9 seekable
    av_log(ffp, AV_LOG_DEBUG, "stream_seek %"PRId64"(%d) + %"PRId64", \n", seek_pos, (int)msec, start_time);
    stream_seek(is, seek_pos, 0, 0);
    return 0;
}

long ffp_get_current_position_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is || !is->ic)
        return 0;

    int64_t start_time = is->ic->start_time;
    int64_t start_diff = 0;
    if (start_time > 0 && start_time != AV_NOPTS_VALUE)
        start_diff = fftime_to_milliseconds(start_time);

    int64_t pos = 0;
    double pos_clock = get_master_clock(is);
    if (isnan(pos_clock)) {
        pos = fftime_to_milliseconds(is->seek_pos);
    } else {
        pos = pos_clock * 1000;
    }

    // If using REAL time and not ajusted, then return the real pos as calculated from the stream
    // the use case for this is primarily when using a custom non-seekable data source that starts
    // with a buffer that is NOT the start of the stream.  We want the get_current_position to
    // return the time in the stream, and not the player's internal clock.
    if (ffp->no_time_adjust) {
        return (long)pos;
    }

    if (pos < 0 || pos < start_diff)
        return 0;

    int64_t adjust_pos = pos - start_diff;
    return (long)adjust_pos;
}

long ffp_get_duration_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is || !is->ic)
        return 0;

    int64_t duration = fftime_to_milliseconds(is->ic->duration);
    if (duration < 0)
        return 0;

    return (long)duration;
}

long ffp_get_playable_duration_l(FFPlayer *ffp)
{
    assert(ffp);
    if (!ffp)
        return 0;

    return (long)ffp->playable_duration_ms;
}

void ffp_set_loop(FFPlayer *ffp, int loop)
{
    assert(ffp);
    if (!ffp)
        return;
    ffp->loop = loop;
}

int ffp_get_loop(FFPlayer *ffp)
{
    assert(ffp);
    if (!ffp)
        return 1;
    return ffp->loop;
}

int ffp_packet_queue_init(PacketQueue *q)
{
    return packet_queue_init(q);
}

void ffp_packet_queue_destroy(PacketQueue *q)
{
    return packet_queue_destroy(q);
}

void ffp_packet_queue_abort(PacketQueue *q)
{
    return packet_queue_abort(q);
}

void ffp_packet_queue_start(PacketQueue *q)
{
    return packet_queue_start(q);
}

void ffp_packet_queue_flush(PacketQueue *q)
{
    return packet_queue_flush(q);
}

int ffp_packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    return packet_queue_get(q, pkt, block, serial);
}

int ffp_packet_queue_get_or_buffering(FFPlayer *ffp, PacketQueue *q, AVPacket *pkt, int *serial, int *finished)
{
    return packet_queue_get_or_buffering(ffp, q, pkt, serial, finished);
}

int ffp_packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    return packet_queue_put(q, pkt);
}

bool ffp_is_flush_packet(AVPacket *pkt)
{
    if (!pkt)
        return false;

    return pkt->data == flush_pkt.data;
}

Frame *ffp_frame_queue_peek_writable(FrameQueue *f)
{
    return frame_queue_peek_writable(f);
}

void ffp_frame_queue_push(FrameQueue *f)
{
    return frame_queue_push(f);
}

int ffp_queue_picture(FFPlayer *ffp, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    return queue_picture(ffp, src_frame, pts, duration, pos, serial);
}

int ffp_get_master_sync_type(VideoState *is)
{
    return get_master_sync_type(is);
}

double ffp_get_master_clock(VideoState *is)
{
    return get_master_clock(is);
}

void ffp_toggle_buffering_l(FFPlayer *ffp, int buffering_on)
{
    if (!ffp->packet_buffering)
        return;

    VideoState *is = ffp->is;
    if (buffering_on && !is->buffering_on) {
        av_log(ffp, AV_LOG_DEBUG, "ffp_toggle_buffering_l: start\n");
        is->buffering_on = 1;
        stream_update_pause_l(ffp);
        ffp_notify_msg1(ffp, FFP_MSG_BUFFERING_START);
    } else if (!buffering_on && is->buffering_on){
        av_log(ffp, AV_LOG_DEBUG, "ffp_toggle_buffering_l: end\n");
        is->buffering_on = 0;
        stream_update_pause_l(ffp);
        ffp_notify_msg1(ffp, FFP_MSG_BUFFERING_END);
    }
}

void ffp_toggle_buffering(FFPlayer *ffp, int start_buffering)
{
    SDL_LockMutex(ffp->is->play_mutex);
    ffp_toggle_buffering_l(ffp, start_buffering);
    SDL_UnlockMutex(ffp->is->play_mutex);
}

void ffp_track_statistic_l(FFPlayer *ffp, AVStream *st, PacketQueue *q, FFTrackCacheStatistic *cache)
{
    assert(cache);

    if (q) {
        cache->bytes   = q->size;
        cache->packets = q->nb_packets;
    }

    if (st && st->time_base.den > 0 && st->time_base.num > 0) {
        cache->duration = q->duration * av_q2d(st->time_base) * 1000;
    }
}

void ffp_audio_statistic_l(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    ffp_track_statistic_l(ffp, is->audio_st, &is->audioq, &ffp->stat.audio_cache);
}

void ffp_video_statistic_l(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    ffp_track_statistic_l(ffp, is->video_st, &is->videoq, &ffp->stat.video_cache);
}

void ffp_statistic_l(FFPlayer *ffp)
{
    ffp_audio_statistic_l(ffp);
    ffp_video_statistic_l(ffp);
}

void ffp_check_buffering_l(FFPlayer *ffp)
{
    VideoState *is            = ffp->is;
    int hwm_in_ms             = ffp->dcc.current_high_water_mark_in_ms; // use fast water mark for first loading
    int buf_size_percent      = -1;
    int buf_time_percent      = -1;
    int hwm_in_bytes          = ffp->dcc.high_water_mark_in_bytes;
    int need_start_buffering  = 0;
    int audio_time_base_valid = 0;
    int video_time_base_valid = 0;
    int64_t buf_time_position = -1;

    if(is->audio_st)
        audio_time_base_valid = is->audio_st->time_base.den > 0 && is->audio_st->time_base.num > 0;
    if(is->video_st)
        video_time_base_valid = is->video_st->time_base.den > 0 && is->video_st->time_base.num > 0;

    if (hwm_in_ms > 0) {
        int     cached_duration_in_ms = -1;
        int64_t audio_cached_duration = -1;
        int64_t video_cached_duration = -1;

        if (is->audio_st && audio_time_base_valid) {
            audio_cached_duration = ffp->stat.audio_cache.duration;
#ifdef FFP_SHOW_DEMUX_CACHE
            int audio_cached_percent = (int)av_rescale(audio_cached_duration, 1005, hwm_in_ms * 10);
            av_log(ffp, AV_LOG_DEBUG, "audio cache=%%%d milli:(%d/%d) bytes:(%d/%d) packet:(%d/%d)\n", audio_cached_percent,
                  (int)audio_cached_duration, hwm_in_ms,
                  is->audioq.size, hwm_in_bytes,
                  is->audioq.nb_packets, MIN_FRAMES);
#endif
        }

        if (is->video_st && video_time_base_valid) {
            video_cached_duration = ffp->stat.video_cache.duration;
#ifdef FFP_SHOW_DEMUX_CACHE
            int video_cached_percent = (int)av_rescale(video_cached_duration, 1005, hwm_in_ms * 10);
            av_log(ffp, AV_LOG_DEBUG, "video cache=%%%d milli:(%d/%d) bytes:(%d/%d) packet:(%d/%d)\n", video_cached_percent,
                  (int)video_cached_duration, hwm_in_ms,
                  is->videoq.size, hwm_in_bytes,
                  is->videoq.nb_packets, MIN_FRAMES);
#endif
        }

        if (video_cached_duration > 0 && audio_cached_duration > 0) {
            cached_duration_in_ms = (int)IJKMIN(video_cached_duration, audio_cached_duration);
        } else if (video_cached_duration > 0) {
            cached_duration_in_ms = (int)video_cached_duration;
        } else if (audio_cached_duration > 0) {
            cached_duration_in_ms = (int)audio_cached_duration;
        }

        if (cached_duration_in_ms >= 0) {
            buf_time_position = ffp_get_current_position_l(ffp) + cached_duration_in_ms;
            ffp->playable_duration_ms = buf_time_position;

            buf_time_percent = (int)av_rescale(cached_duration_in_ms, 1005, hwm_in_ms * 10);
#ifdef FFP_SHOW_DEMUX_CACHE
            av_log(ffp, AV_LOG_DEBUG, "time cache=%%%d (%d/%d)\n", buf_time_percent, cached_duration_in_ms, hwm_in_ms);
#endif
#ifdef FFP_NOTIFY_BUF_TIME
            ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_TIME_UPDATE, cached_duration_in_ms, hwm_in_ms);
#endif
        }
    }

    int cached_size = is->audioq.size + is->videoq.size;
    if (hwm_in_bytes > 0) {
        buf_size_percent = (int)av_rescale(cached_size, 1005, hwm_in_bytes * 10);
#ifdef FFP_SHOW_DEMUX_CACHE
        av_log(ffp, AV_LOG_DEBUG, "size cache=%%%d (%d/%d)\n", buf_size_percent, cached_size, hwm_in_bytes);
#endif
#ifdef FFP_NOTIFY_BUF_BYTES
        ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_BYTES_UPDATE, cached_size, hwm_in_bytes);
#endif
    }

    int buf_percent = -1;
    if (buf_time_percent >= 0) {
        // alwas depend on cache duration if valid
        if (buf_time_percent >= 100)
            need_start_buffering = 1;
        buf_percent = buf_time_percent;
    } else {
        if (buf_size_percent >= 100)
            need_start_buffering = 1;
        buf_percent = buf_size_percent;
    }

    if (buf_time_percent >= 0 && buf_size_percent >= 0) {
        buf_percent = FFMIN(buf_time_percent, buf_size_percent);
    }
    if (buf_percent) {
#ifdef FFP_SHOW_BUF_POS
        av_log(ffp, AV_LOG_DEBUG, "buf pos=%"PRId64", %%%d\n", buf_time_position, buf_percent);
#endif
        ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_UPDATE, (int)buf_time_position, buf_percent);
    }

    if (need_start_buffering) {
        if (hwm_in_ms < ffp->dcc.next_high_water_mark_in_ms) {
            hwm_in_ms = ffp->dcc.next_high_water_mark_in_ms;
        } else {
            hwm_in_ms *= 2;
        }

        if (hwm_in_ms > ffp->dcc.last_high_water_mark_in_ms)
            hwm_in_ms = ffp->dcc.last_high_water_mark_in_ms;

        ffp->dcc.current_high_water_mark_in_ms = hwm_in_ms;

        if (is->buffer_indicator_queue && is->buffer_indicator_queue->nb_packets > 0) {
            if (   (is->audioq.nb_packets > MIN_MIN_FRAMES || is->audio_stream < 0 || is->audioq.abort_request)
                && (is->videoq.nb_packets > MIN_MIN_FRAMES || is->video_stream < 0 || is->videoq.abort_request)) {
                ffp_toggle_buffering(ffp, 0);
            }
        }
    }
}

int ffp_video_thread(FFPlayer *ffp)
{
    return ffplay_video_thread(ffp);
}

void ffp_set_video_codec_info(FFPlayer *ffp, const char *module, const char *codec)
{
    av_freep(&ffp->video_codec_info);
    ffp->video_codec_info = av_asprintf("%s, %s", module ? module : "", codec ? codec : "");
    av_log(ffp, AV_LOG_INFO, "VideoCodec: %s\n", ffp->video_codec_info);
}

void ffp_set_audio_codec_info(FFPlayer *ffp, const char *module, const char *codec)
{
    av_freep(&ffp->audio_codec_info);
    ffp->audio_codec_info = av_asprintf("%s, %s", module ? module : "", codec ? codec : "");
    av_log(ffp, AV_LOG_INFO, "AudioCodec: %s\n", ffp->audio_codec_info);
}

void ffp_set_subtitle_codec_info(FFPlayer *ffp, const char *module, const char *codec)
{
    av_freep(&ffp->subtitle_codec_info);
    ffp->subtitle_codec_info = av_asprintf("%s, %s", module ? module : "", codec ? codec : "");
    av_log(ffp, AV_LOG_INFO, "SubtitleCodec: %s\n", ffp->subtitle_codec_info);
}

void ffp_set_playback_rate(FFPlayer *ffp, float rate)
{
    if (!ffp)
        return;

    ffp->pf_playback_rate = rate;
    ffp->pf_playback_rate_changed = 1;
}

void ffp_set_playback_volume(FFPlayer *ffp, float volume)
{
    if (!ffp)
        return;
    ffp->pf_playback_volume = volume;
    ffp->pf_playback_volume_changed = 1;
}

int ffp_get_video_rotate_degrees(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    if (!is)
        return 0;

    int theta  = abs((int)((int64_t)round(fabs(get_rotation(is->video_st))) % 360));
    switch (theta) {
        case 0:
        case 90:
        case 180:
        case 270:
            break;
        case 360:
            theta = 0;
            break;
        default:
            ALOGW("Unknown rotate degress: %d\n", theta);
            theta = 0;
            break;
    }

    return theta;
}

int ffp_set_stream_selected(FFPlayer *ffp, int stream, int selected)
{
    VideoState        *is = ffp->is;
    AVFormatContext   *ic = NULL;
    AVCodecParameters *codecpar = NULL;
    if (!is)
        return -1;
    ic = is->ic;
    if (!ic)
        return -1;

    if (stream < 0 || stream >= ic->nb_streams) {
        av_log(ffp, AV_LOG_ERROR, "invalid stream index %d >= stream number (%d)\n", stream, ic->nb_streams);
        return -1;
    }

    codecpar = ic->streams[stream]->codecpar;

    if (selected) {
        switch (codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if (stream != is->video_stream && is->video_stream >= 0)
                    stream_component_close(ffp, is->video_stream);
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (stream != is->audio_stream && is->audio_stream >= 0)
                    stream_component_close(ffp, is->audio_stream);
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                if (stream != is->subtitle_stream && is->subtitle_stream >= 0)
                    stream_component_close(ffp, is->subtitle_stream);
                break;
            default:
                av_log(ffp, AV_LOG_ERROR, "select invalid stream %d of video type %d\n", stream, codecpar->codec_type);
                return -1;
        }
        return stream_component_open(ffp, stream);
    } else {
        switch (codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if (stream == is->video_stream)
                    stream_component_close(ffp, is->video_stream);
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (stream == is->audio_stream)
                    stream_component_close(ffp, is->audio_stream);
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                if (stream == is->subtitle_stream)
                    stream_component_close(ffp, is->subtitle_stream);
                break;
            default:
                av_log(ffp, AV_LOG_ERROR, "select invalid stream %d of audio type %d\n", stream, codecpar->codec_type);
                return -1;
        }
        return 0;
    }
}

float ffp_get_property_float(FFPlayer *ffp, int id, float default_value)
{
    switch (id) {
        case FFP_PROP_FLOAT_VIDEO_DECODE_FRAMES_PER_SECOND:
            return ffp ? ffp->stat.vdps : default_value;
        case FFP_PROP_FLOAT_VIDEO_OUTPUT_FRAMES_PER_SECOND:
            return ffp ? ffp->stat.vfps : default_value;
        case FFP_PROP_FLOAT_PLAYBACK_RATE:
            return ffp ? ffp->pf_playback_rate : default_value;
        case FFP_PROP_FLOAT_AVDELAY:
            return ffp ? ffp->stat.avdelay : default_value;
        case FFP_PROP_FLOAT_AVDIFF:
            return ffp ? ffp->stat.avdiff : default_value;
        case FFP_PROP_FLOAT_PLAYBACK_VOLUME:
            return ffp ? ffp->pf_playback_volume : default_value;
        default:
            return default_value;
    }
}

void ffp_set_property_float(FFPlayer *ffp, int id, float value)
{
    switch (id) {
        case FFP_PROP_FLOAT_PLAYBACK_RATE:
            ffp_set_playback_rate(ffp, value);
            break;
        case FFP_PROP_FLOAT_PLAYBACK_VOLUME:
            ffp_set_playback_volume(ffp, value);
            break;
        default:
            return;
    }
}

int64_t ffp_get_property_int64(FFPlayer *ffp, int id, int64_t default_value)
{
    switch (id) {
        case FFP_PROP_INT64_SELECTED_VIDEO_STREAM:
            if (!ffp || !ffp->is)
                return default_value;
            return ffp->is->video_stream;
        case FFP_PROP_INT64_SELECTED_AUDIO_STREAM:
            if (!ffp || !ffp->is)
                return default_value;
            return ffp->is->audio_stream;
        case FFP_PROP_INT64_SELECTED_TIMEDTEXT_STREAM:
            if (!ffp || !ffp->is)
                return default_value;
            return ffp->is->subtitle_stream;
        case FFP_PROP_INT64_VIDEO_DECODER:
            if (!ffp)
                return default_value;
            return ffp->stat.vdec_type;
        case FFP_PROP_INT64_AUDIO_DECODER:
            return FFP_PROPV_DECODER_AVCODEC;

        case FFP_PROP_INT64_VIDEO_CACHED_DURATION:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.duration;
        case FFP_PROP_INT64_AUDIO_CACHED_DURATION:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.duration;
        case FFP_PROP_INT64_VIDEO_CACHED_BYTES:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.bytes;
        case FFP_PROP_INT64_AUDIO_CACHED_BYTES:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.bytes;
        case FFP_PROP_INT64_VIDEO_CACHED_PACKETS:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.packets;
        case FFP_PROP_INT64_AUDIO_CACHED_PACKETS:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.packets;
        case FFP_PROP_INT64_BIT_RATE:
            return ffp ? ffp->stat.bit_rate : default_value;
        case FFP_PROP_INT64_TCP_SPEED:
            return ffp ? SDL_SpeedSampler2GetSpeed(&ffp->stat.tcp_read_sampler) : default_value;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_BACKWARDS:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_backwards;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_FORWARDS:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_forwards;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_CAPACITY:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_capacity;
        case FFP_PROP_INT64_LATEST_SEEK_LOAD_DURATION:
            return ffp ? ffp->stat.latest_seek_load_duration : default_value;
        default:
            return default_value;
    }
}

void ffp_set_property_int64(FFPlayer *ffp, int id, int64_t value)
{
    switch (id) {
        // case FFP_PROP_INT64_SELECTED_VIDEO_STREAM:
        // case FFP_PROP_INT64_SELECTED_AUDIO_STREAM:
        default:
            break;
    }
}

IjkMediaMeta *ffp_get_meta_l(FFPlayer *ffp)
{
    if (!ffp)
        return NULL;

    return ffp->meta;
}
            
void mw_start_record(FFPlayer *ffp, const char *recRootPath)
{
    av_log(NULL, AV_LOG_DEBUG, "mw_start_record, start");
            
    struct timeval tv;
            
    if(access(recRootPath, F_OK) == 0)
    {
    }else{
                
        if(mkdir(recRootPath, 0775) == -1)
        {
            av_log(NULL, AV_LOG_DEBUG, "mw_start_record, rootPath=%s,create failed",recRootPath);
            return;
        }
    }
            
    gettimeofday(&tv,NULL);
    long currrntTime = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    sprintf(ffp->mwRecFile ,"%s/%ld.mp4", recRootPath, currrntTime);
            
    ffp->m_bRecorder = 1;
            
            
    }
            
void mw_stop_record(FFPlayer *ffp)
{
    av_log(NULL, AV_LOG_DEBUG, "mw_stop_record");
    ffp->m_bRecorder = 0;
}

#define AUDIO_BUF_SIZE 1024*64

#define CAT_AUDIO_BUF_SIZE 4096 * 2
            
#define AUDIO_SEND_BUF_SIZE 1024 * 64
            
typedef struct{
    char addr[512];
    int port;
    int client_fd;
}Cmd_Addr;
            
void *read_record_audio_thread(void *arg){
    AVCodec *pCodec;
    AVCodecContext *pCodecCtx= NULL;
    int i, ret, got_output;
    int autioType = 2;
    
    AVFrame *pFrame;
    uint8_t* frame_buf;
    int size=0;
    
    AVPacket pkt;
    int framecnt=0;
    int total_cpy_len = 0, excpect_cpy_len = 0;
    
    
    enum AVCodecID codec_id=AV_CODEC_ID_AAC;
    
    char audioFrame[AUDIO_BUF_SIZE];
    uint8_t sendbuf[AUDIO_BUF_SIZE];
    int length = 0;
    
    Cmd_Addr *cmd_addr = (Cmd_Addr *)arg;
    
    int clientSocket;
    //ÃèÊö·þÎñÆ÷µÄsocket
    struct sockaddr_in serverAddr;
    
    if((clientSocket = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR,"create socket error\n");
        return 1;
    }
    
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(cmd_addr->port);
    
    serverAddr.sin_addr.s_addr = inet_addr(cmd_addr->addr);
    if(connect(clientSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR,"client connect failed");
        return 1;
    }
    
    //avcodec_register_all();
    
    pCodec = avcodec_find_encoder_by_name("libfdk_aac");//avcodec_find_encoder(codec_id);
    if (!pCodec) {
        printf("Codec not found\n");
        return -1;
    }
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        printf("Could not allocate video codec context\n");
        return -1;
    }
    av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    
    pCodecCtx->codec_id = codec_id;
    pCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    pCodecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
    pCodecCtx->sample_rate= 16000;
    pCodecCtx->channel_layout= AV_CH_LAYOUT_MONO;
    pCodecCtx->channels = 1;
    pCodecCtx->bit_rate = 64000;
    
    //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
        printf("Could not open codec\n");
        return -1;
    }
    //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    
    pFrame = av_frame_alloc();
    pFrame->nb_samples= pCodecCtx->frame_size;
    pFrame->format= pCodecCtx->sample_fmt;
    size = av_samples_get_buffer_size(NULL, pCodecCtx->channels,pCodecCtx->frame_size,pCodecCtx->sample_fmt, 1);
    frame_buf = (uint8_t *)av_malloc(size);
    avcodec_fill_audio_frame(pFrame, pCodecCtx->channels, pCodecCtx->sample_fmt,(const uint8_t*)frame_buf, size, 1);
    
    //Input raw data
    //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    /*fp_in = fopen(filename_in, "rb");
     if (!fp_in) {
     printf("Could not open %s\n", filename_in);
     return -1;
     }*/
    //Output bitstream
    /*fp_out = fopen(filename_out, "wb");
     if (!fp_out) {
     printf("Could not open %s\n", filename_out);
     return -1;
     }*/
    
    //Encode
    while(audio_record_start) {
        av_init_packet(&pkt);
        pkt.data = NULL;    // packet data will be allocated by the encoder
        pkt.size = 0;
        //Read raw data
        length    = getRecordFrameData(audioFrame);
        if(length <= 0)
            continue;
        excpect_cpy_len = (size -total_cpy_len) < length ? size -total_cpy_len : length;
        memcpy(frame_buf+total_cpy_len, audioFrame, excpect_cpy_len);
        total_cpy_len += excpect_cpy_len;
        if(total_cpy_len == size ){
            pFrame->pts = 0;
            ret = avcodec_encode_audio2(pCodecCtx, &pkt, pFrame, &got_output);
            if (ret < 0) {
                printf("Error encoding frame\n");
                return -1;
            }
            if (got_output) {
                printf("Succeed to encode frame: %5d\tsize:%5d\n",framecnt,pkt.size);
                framecnt++;
                //fwrite(pkt.data, 1, pkt.size, fp_out);
                memcpy(sendbuf, &autioType, 4);
                memcpy(sendbuf+4, &(pkt.size), 4);
                memcpy(sendbuf+8, pkt.data, pkt.size);
                
                int totalSend = send(clientSocket, sendbuf, pkt.size+8, 0);
                av_log(NULL, AV_LOG_ERROR, "%s:%d, totalSend = %d\n", __func__, __LINE__, totalSend);
                av_free_packet(&pkt);
            }
            total_cpy_len = 0;
            memcpy(frame_buf+total_cpy_len, audioFrame + excpect_cpy_len, length - excpect_cpy_len);
            total_cpy_len += length - excpect_cpy_len;
        }
    }
    //Flush Encoder
    for (got_output = 1; got_output; i++) {
        ret = avcodec_encode_audio2(pCodecCtx, &pkt, NULL, &got_output);
        if (ret < 0) {
            printf("Error encoding frame\n");
            return -1;
        }
        if (got_output) {
            printf("Flush Encoder: Succeed to encode 1 frame!\tsize:%5d\n",pkt.size);
            //fwrite(pkt.data, 1, pkt.size, fp_out);
            av_free_packet(&pkt);
        }
    }
    
    // fclose(fp_out);
    avcodec_close(pCodecCtx);
    av_free(pCodecCtx);
    av_freep(&pFrame->data[0]);
    av_frame_free(&pFrame);
    close(clientSocket);
    free(cmd_addr);

    return 0;
}

void mw_start_p2p_intercom(FFPlayer *ffp, const char * urlStr, const char * ipStr, int port)
{
    pthread_t record_tid ;
    startRecord();
    audio_record_start = 1;
    Cmd_Addr* cmd_addr= (Cmd_Addr*)malloc(sizeof(Cmd_Addr));
    int length = strlen(ipStr);
    memcpy(cmd_addr->addr, ipStr, length);
    cmd_addr->addr[length] = '\0';
    cmd_addr->port = port;

    pthread_create(&record_tid, NULL, read_record_audio_thread,  cmd_addr);
}

void mw_stop_p2p_intercom(FFPlayer *ffp)
{
    audio_record_start = 0;
    stopRecord();
}
            
void mp_screenshot(FFPlayer *ffp, const char *screenshotRootPath)
{
    time_t   now;
    struct   tm  *timenow;
    time(&now);
    timenow = localtime(&now);
    sprintf(ffp->screenShotFile, "%s/%d-%02d-%02d-%02d-%02d-%02d.jpg", screenshotRootPath,
            timenow->tm_year+1900,
            timenow->tm_mon+1,
            timenow->tm_mday,
            timenow->tm_hour,
            timenow->tm_min,
            timenow->tm_sec);
    ffp->m_screenShot = 1;
}

void mp_screenshot_with_name(FFPlayer *ffp, const char *screenshotRootPath, const char *picName)
{
    sprintf(ffp->screenShotFile, "%s/%s", screenshotRootPath, picName);
    ffp->m_screenShot = 1;
}

void mw_initOutPutStream(FFPlayer *ffp, const char *out_filename){
                
    int ret;
	VideoState *is = ffp->is;
	
	//AVOutputFormat *ofmt = NULL;
	//AVFormatContext *ofmt_ctx = NULL;
	avformat_alloc_output_context2(&is->ofmt_ctx, NULL, NULL, out_filename);
    if (!is->ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        mw_closeOutPutStream(ffp);
		return ;
    }

	 is->ofmt = is->ofmt_ctx->oformat;
	  for (int i = 0; i < is->ic->nb_streams; i++) {
        AVStream *in_stream = is->ic->streams[i];
        AVStream *out_stream = avformat_new_stream(is->ofmt_ctx, NULL);
        if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            mw_closeOutPutStream(ffp);
			return ;
        }
        /*ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy context from input to output stream codec context\n");
            mw_closeOutPutStream(ffp);
			return ;
        }*/
        out_stream->codec->codec_tag = 0;
		out_stream->codec->codec_type = in_stream->codec->codec_type;

		//av_log(NULL, AV_LOG_DEBUG, "%s:%d, codec_type = %d\n", __func__, __LINE__, out_stream->codec->codec_type);
		AVCodecParameters *par = NULL;
		AVCodecContext *dec_ctx, *enc_ctx;
		switch(out_stream->codec->codec_type){
			case AVMEDIA_TYPE_AUDIO:
				is->audio_out_stream_index = out_stream->index;
				dec_ctx = is->auddec.avctx;
				enc_ctx = out_stream->codec;
				enc_ctx->codec_id   = dec_ctx->codec_id;
            	enc_ctx->codec_type = dec_ctx->codec_type;
				enc_ctx->channel_layout     = dec_ctx->channel_layout;
                enc_ctx->sample_rate        = dec_ctx->sample_rate;
                enc_ctx->channels           = dec_ctx->channels;
                enc_ctx->frame_size         = dec_ctx->frame_size;
                enc_ctx->audio_service_type = dec_ctx->audio_service_type;
                enc_ctx->block_align        = dec_ctx->block_align;
                enc_ctx->initial_padding    = dec_ctx->delay;
				par = out_stream->codecpar;
				par->codec_type = AVMEDIA_TYPE_AUDIO;
				par->sample_rate = dec_ctx->sample_rate;
				par->codec_id = dec_ctx->codec_id;
				//av_log(NULL, AV_LOG_DEBUG, "%s:%d, width=%d, height=%d, codec_id2 = %s\n", __func__, __LINE__, par->width, par->height, avcodec_get_name(dec_ctx->codec_id));
				break;
			case AVMEDIA_TYPE_VIDEO:
				//par = out_stream->codecpar;
				//par->width = in_stream->codec->width;
				//par->height = in_stream->codec->height;
				is->video_out_stream_index = out_stream->index;
				dec_ctx = is->viddec.avctx;
				enc_ctx = out_stream->codec;
				enc_ctx->codec_id   = dec_ctx->codec_id;
            	enc_ctx->codec_type = dec_ctx->codec_type;
				enc_ctx->pix_fmt            = dec_ctx->pix_fmt;
                enc_ctx->width              = dec_ctx->width;
                enc_ctx->height             = dec_ctx->height;
				par = out_stream->codecpar;
				par->width = dec_ctx->width;
				par->height = dec_ctx->height;
				par->codec_type = AVMEDIA_TYPE_VIDEO;
				par->codec_id = dec_ctx->codec_id;
				//av_log(NULL, AV_LOG_DEBUG, "%s:%d, width=%d, height=%d, codec_id2 = %s\n", __func__, __LINE__, par->width, par->height, avcodec_get_name(dec_ctx->codec_id));
				break;
		}

		if (par && in_stream->codec->extradata)
		{
			par->extradata = av_mallocz(in_stream->codec->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
			if (par->extradata)
			{
				memcpy(par->extradata, in_stream->codec->extradata, in_stream->codec->extradata_size);
				par->extradata_size = in_stream->codec->extradata_size;
			}
			else
			{
				//return AVERROR(ENOMEM);
			}
		}
        
		/*AVCodecContext *enc_ctx;
		AVCodecContext *dec_ctx = NULL;
		enc_ctx = out_stream->codec;
		dec_ctx = in_stream->codec;
		out_stream->disposition          = in_stream->disposition;
        enc_ctx->bits_per_raw_sample    = dec_ctx->bits_per_raw_sample;
        enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;

		AVRational sar;
		uint64_t extra_size;
		 
		extra_size = (uint64_t)dec_ctx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE;
		 
		if (extra_size > INT_MAX) {
			return AVERROR(EINVAL);
		}

		 if (!enc_ctx->codec_tag) {
                unsigned int codec_tag;
                if (!is->ofmt_ctx->oformat->codec_tag ||
                     av_codec_get_id (is->ofmt_ctx->oformat->codec_tag, dec_ctx->codec_tag) == enc_ctx->codec_id ||
                     !av_codec_get_tag2(is->ofmt_ctx->oformat->codec_tag, dec_ctx->codec_id, &codec_tag))
                    enc_ctx->codec_tag = dec_ctx->codec_tag;
            }

            enc_ctx->bit_rate       = dec_ctx->bit_rate;
            enc_ctx->rc_max_rate    = dec_ctx->rc_max_rate;
            enc_ctx->rc_buffer_size = dec_ctx->rc_buffer_size;
            enc_ctx->field_order    = dec_ctx->field_order;
            if (dec_ctx->extradata_size) {
                enc_ctx->extradata      = av_mallocz(extra_size);
                if (!enc_ctx->extradata) {
                    return AVERROR(ENOMEM);
                }
                memcpy(enc_ctx->extradata, dec_ctx->extradata, dec_ctx->extradata_size);
            }
            enc_ctx->extradata_size= dec_ctx->extradata_size;
            enc_ctx->bits_per_coded_sample  = dec_ctx->bits_per_coded_sample;

            enc_ctx->time_base = in_stream->time_base;

			  /*
             * Avi is a special case here because it supports variable fps but
             * having the fps and timebase differe significantly adds quite some
             * overhead
             */
           /*  int copy_tb = -1;
          if(!strcmp(is->ofmt_ctx->oformat->name, "avi")) {
                if ( copy_tb<0 && av_q2d(in_stream->r_frame_rate) >= av_q2d(in_stream->avg_frame_rate)
                               && 0.5/av_q2d(in_stream->r_frame_rate) > av_q2d(in_stream->time_base)
                               && 0.5/av_q2d(in_stream->r_frame_rate) > av_q2d(dec_ctx->time_base)
                               && av_q2d(in_stream->time_base) < 1.0/500 && av_q2d(dec_ctx->time_base) < 1.0/500
                     || copy_tb==2){
                    enc_ctx->time_base.num = in_stream->r_frame_rate.den;
                    enc_ctx->time_base.den = 2*in_stream->r_frame_rate.num;
                    enc_ctx->ticks_per_frame = 2;
                } else if (   copy_tb<0 && av_q2d(dec_ctx->time_base)*dec_ctx->ticks_per_frame > 2*av_q2d(in_stream->time_base)
                                 && av_q2d(in_stream->time_base) < 1.0/500
                    || copy_tb==0){
                    enc_ctx->time_base = dec_ctx->time_base;
                    enc_ctx->time_base.num *= dec_ctx->ticks_per_frame;
                    enc_ctx->time_base.den *= 2;
                    enc_ctx->ticks_per_frame = 2;
                }
            } else if(!(is->ofmt_ctx->oformat->flags & AVFMT_VARIABLE_FPS)
                      && strcmp(is->ofmt_ctx->oformat->name, "mov") && strcmp(is->ofmt_ctx->oformat->name, "mp4") && strcmp(is->ofmt_ctx->oformat->name, "3gp")
                      && strcmp(is->ofmt_ctx->oformat->name, "3g2") && strcmp(is->ofmt_ctx->oformat->name, "psp") && strcmp(is->ofmt_ctx->oformat->name, "ipod")
                      && strcmp(is->ofmt_ctx->oformat->name, "f4v")
            ) {
                if(   copy_tb<0 && dec_ctx->time_base.den
                                && av_q2d(dec_ctx->time_base)*dec_ctx->ticks_per_frame > av_q2d(in_stream->time_base)
                                && av_q2d(in_stream->time_base) < 1.0/500
                   || copy_tb==0){
                    enc_ctx->time_base = dec_ctx->time_base;
                    enc_ctx->time_base.num *= dec_ctx->ticks_per_frame;
                }
            }*/
           /* if (   enc_ctx->codec_tag == AV_RL32("tmcd")
                && dec_ctx->time_base.num < dec_ctx->time_base.den
                && dec_ctx->time_base.num > 0
                && 121LL*dec_ctx->time_base.num > dec_ctx->time_base.den) {
                enc_ctx->time_base = dec_ctx->time_base;
            }*/

            /*if (ist && !ost->frame_rate.num)
                ost->frame_rate = ist->framerate;
            if(ost->frame_rate.num)
                enc_ctx->time_base = av_inv_q(ost->frame_rate);*/

           // av_reduce(&enc_ctx->time_base.num, &enc_ctx->time_base.den,
           //             enc_ctx->time_base.num, enc_ctx->time_base.den, INT_MAX);

           /* if (in_stream->nb_side_data) {
                out_stream->side_data = av_realloc_array(NULL, in_stream->nb_side_data,
                                                      sizeof(*in_stream->side_data));
                if (!ost->st->side_data)
                    return AVERROR(ENOMEM);

                ost->st->nb_side_data = 0;
                for (j = 0; j < in_stream->nb_side_data; j++) {
                    const AVPacketSideData *sd_src = &in_stream->side_data[j];
                    AVPacketSideData *sd_dst = &out_stream->side_data[out_stream->nb_side_data];

                    if (ost->rotate_overridden && sd_src->type == AV_PKT_DATA_DISPLAYMATRIX)
                        continue;

                    sd_dst->data = av_malloc(sd_src->size);
                    if (!sd_dst->data)
                        return AVERROR(ENOMEM);
                    memcpy(sd_dst->data, sd_src->data, sd_src->size);
                    sd_dst->size = sd_src->size;
                    sd_dst->type = sd_src->type;
                    out_stream->nb_side_data++;
                }
            }*/

           /* out_stream->parser = av_parser_init(enc_ctx->codec_id);
			  AVCodecParameters *par;

            switch (enc_ctx->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                
                enc_ctx->channel_layout     = dec_ctx->channel_layout;
                enc_ctx->sample_rate        = dec_ctx->sample_rate;
                enc_ctx->channels           = dec_ctx->channels;
                enc_ctx->frame_size         = dec_ctx->frame_size;
                enc_ctx->audio_service_type = dec_ctx->audio_service_type;
                enc_ctx->block_align        = dec_ctx->block_align;
                enc_ctx->initial_padding    = dec_ctx->delay;
#if FF_API_AUDIOENC_DELAY
                enc_ctx->delay              = dec_ctx->delay;
#endif
                if((enc_ctx->block_align == 1 || enc_ctx->block_align == 1152 || enc_ctx->block_align == 576) && enc_ctx->codec_id == AV_CODEC_ID_MP3)
                    enc_ctx->block_align= 0;
                if(enc_ctx->codec_id == AV_CODEC_ID_AC3)
                    enc_ctx->block_align= 0;
				par = out_stream->codecpar;
				par->codec_type = AVMEDIA_TYPE_AUDIO;
                break;
            case AVMEDIA_TYPE_VIDEO:
                enc_ctx->pix_fmt            = dec_ctx->pix_fmt;
                enc_ctx->width              = dec_ctx->width;
                enc_ctx->height             = dec_ctx->height;
                enc_ctx->has_b_frames       = dec_ctx->has_b_frames;
        		par = out_stream->codecpar;
				par->width = dec_ctx->width;
				par->height = dec_ctx->height;
				par->codec_type = AVMEDIA_TYPE_VIDEO;
                /*if (out_stream->frame_aspect_ratio.num) { // overridden by the -aspect cli option
                    sar =
                        av_mul_q(out_stream->frame_aspect_ratio,
                                 getAVRational(enc_ctx->height, enc_ctx->width ));
                    av_log(NULL, AV_LOG_WARNING, "Overriding aspect ratio "
                           "with stream copy may produce invalid files\n");
                }
                else*/ if (in_stream->sample_aspect_ratio.num)
                /*    sar = in_stream->sample_aspect_ratio;
                else
                    sar = dec_ctx->sample_aspect_ratio;
                out_stream->sample_aspect_ratio = enc_ctx->sample_aspect_ratio = sar;
                out_stream->avg_frame_rate = in_stream->avg_frame_rate;
                out_stream->r_frame_rate = in_stream->r_frame_rate;
                break;
            }*/
			
        if (is->ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }
    av_dump_format(is->ofmt_ctx, 0, out_filename, 1);
    if (!(is->ofmt->flags & AVFMT_NOFILE)) {
        //ret = avio_open(&is->ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        ret = avio_open2(&is->ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE, NULL, NULL);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            mw_closeOutPutStream(ffp);
			return ;
        }
    }
    ret = avformat_write_header(is->ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        mw_closeOutPutStream(ffp);
		return ;
    }
	//is->hevcbsfc = av_bitstream_filter_init("hevc_mp4toannexb");
	is->aacbsfc = av_bitstream_filter_init("aac_adtstoasc");   
 }
            
void mw_closeOutPutStream(FFPlayer *ffp){
   //	int ret;
	VideoState *is = ffp->is;
	if(is->ofmt_ctx){
		av_write_trailer(is->ofmt_ctx);
	}
    /* close output */
    if (is->ofmt_ctx && !(is->ofmt->flags & AVFMT_NOFILE)){
        avio_close(is->ofmt_ctx->pb);
    }
    avformat_free_context(is->ofmt_ctx);
/*
    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
       // return 1;
    }
*/
	is->ofmt_ctx = NULL;
	//av_bitstream_filter_close(is->hevcbsfc);
	av_bitstream_filter_close(is->aacbsfc);
	//is->hevcbsfc = NULL;
	is->aacbsfc = NULL;
}

void *wechat_send_audio_thread(void *arg){
    av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    AVCodec *pCodec;
    AVCodecContext *pCodecCtx= NULL;
    int i, ret, got_output;
    int autioType = 2;
    FILE *fp_in;
    FILE *fp_out;
    
    AVFrame *pFrame;
    uint8_t* frame_buf;
    int size=0;
    
    AVPacket pkt;
    int y_size;
    int framecnt=0;
    int total_cpy_len = 0, excpect_cpy_len = 0;
    
    char filename_in[]="/sdcard/play16k.pcm";
    
    enum AVCodecID codec_id=AV_CODEC_ID_AAC;
    char filename_out[]="/sdcard/play16k.aac";
    
    int framenum=1000;
    char audioFrame[AUDIO_BUF_SIZE];
    uint8_t sendbuf[AUDIO_BUF_SIZE];
    
    Cmd_Addr *cmd_addr = (Cmd_Addr *)arg;
    
    int clientSocket = cmd_addr->client_fd;
    av_log(NULL, AV_LOG_ERROR, "%s:%d, clientSocket = %d\n", __func__, __LINE__, clientSocket);
    //√Ë ˆ∑˛ŒÒ∆˜µƒsocket
    /*struct sockaddr_in serverAddr;
     
     if((clientSocket = socket(AF_INET, SOCK_STREAM, 0)) < 0)
     {
     av_log(NULL, AV_LOG_ERROR,"create socket error\n");
     return 1;
     }
     
     serverAddr.sin_family = AF_INET;
     serverAddr.sin_port = htons(cmd_addr->port);
     
     serverAddr.sin_addr.s_addr = inet_addr(cmd_addr->addr);
     if(connect(clientSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
     {
     av_log(NULL, AV_LOG_ERROR,"client connect failed");
     return 1;
     }*/
    
    //avcodec_register_all();
    
    pCodec = avcodec_find_encoder_by_name("libfdk_aac");//avcodec_find_encoder(codec_id);
    if (!pCodec) {
        printf("Codec not found\n");
        return NULL;
    }
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        printf("Could not allocate video codec context\n");
        return NULL;
    }
    //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    
    pCodecCtx->codec_id = codec_id;
    pCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    pCodecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
    pCodecCtx->sample_rate= 16000;
    pCodecCtx->channel_layout= AV_CH_LAYOUT_MONO;
    pCodecCtx->channels = 1;
    pCodecCtx->bit_rate = 64000;
    
    //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
        printf("Could not open codec\n");
        return NULL;
    }
    //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    
    pFrame = av_frame_alloc();
    pFrame->nb_samples= pCodecCtx->frame_size;
    pFrame->format= pCodecCtx->sample_fmt;
    size = av_samples_get_buffer_size(NULL, pCodecCtx->channels,pCodecCtx->frame_size,pCodecCtx->sample_fmt, 1);
    frame_buf = (uint8_t *)av_malloc(size);
    avcodec_fill_audio_frame(pFrame, pCodecCtx->channels, pCodecCtx->sample_fmt,(const uint8_t*)frame_buf, size, 1);
    
    //Input raw data
    //av_log(NULL, AV_LOG_ERROR, "%s:%d", __func__, __LINE__);
    /*fp_in = fopen(filename_in, "rb");
     if (!fp_in) {
     printf("Could not open %s\n", filename_in);
     return -1;
     }*/
    //Output bitstream
    /*fp_out = fopen(filename_out, "wb");
     if (!fp_out) {
     printf("Could not open %s\n", filename_out);
     return -1;
     }*/
    
    //Encode
    while(audio_record_start) {
        av_init_packet(&pkt);
        pkt.data = NULL;    // packet data will be allocated by the encoder
        pkt.size = 0;
        //Read raw data
        int length  = getRecordFrameData(audioFrame);
        if(length <= 0)
            continue;
        excpect_cpy_len = (size -total_cpy_len) < length ? size -total_cpy_len : length;
        memcpy(frame_buf+total_cpy_len, audioFrame, excpect_cpy_len);
        total_cpy_len += excpect_cpy_len;
        if(total_cpy_len == size ){
            pFrame->pts = 0;
            ret = avcodec_encode_audio2(pCodecCtx, &pkt, pFrame, &got_output);
            if (ret < 0) {
                printf("Error encoding frame\n");
                return NULL;
            }
            if (got_output) {
                printf("Succeed to encode frame: %5d\tsize:%5d\n",framecnt,pkt.size);
                framecnt++;
                //fwrite(pkt.data, 1, pkt.size, fp_out);
                memcpy(sendbuf, &autioType, 4);
                memcpy(sendbuf+4, &(pkt.size), 4);
                memcpy(sendbuf+8, pkt.data, pkt.size);
                
                int totalSend = send(clientSocket, sendbuf, pkt.size+8, 0);
                av_log(NULL, AV_LOG_ERROR, "%s:%d, totalSend = %d, clientSocket=%d, length = %d\n", __func__, __LINE__, totalSend, clientSocket, length);
                av_free_packet(&pkt);
            }
            total_cpy_len = 0;
            memcpy(frame_buf+total_cpy_len, audioFrame + excpect_cpy_len, length - excpect_cpy_len);
            total_cpy_len += length - excpect_cpy_len;
        }
    }
    //Flush Encoder
    for (got_output = 1; got_output; i++) {
        ret = avcodec_encode_audio2(pCodecCtx, &pkt, NULL, &got_output);
        if (ret < 0) {
            printf("Error encoding frame\n");
            return NULL;
        }
        if (got_output) {
            printf("Flush Encoder: Succeed to encode 1 frame!\tsize:%5d\n",pkt.size);
            //fwrite(pkt.data, 1, pkt.size, fp_out);
            av_free_packet(&pkt);
        }
    }
    
    // fclose(fp_out);
    avcodec_close(pCodecCtx);
    av_free(pCodecCtx);
    av_freep(&pFrame->data[0]);
    av_frame_free(&pFrame);
    free(cmd_addr);
    cmd_addr = NULL;
    //close(clientSocket);
    
    return NULL;
}

void mw_start_wechat_intercom2(FFPlayer *ffp, int clientSocket){
    
    if(audio_record_start){
        return;
    }
   pthread_t wechat_send_tid;
   startRecord();
   audio_record_start = 1;
   Cmd_Addr* cmd_addr= (Cmd_Addr*)malloc(sizeof(Cmd_Addr));
   cmd_addr->addr[0] = '\0';
   cmd_addr->port = 0;
   cmd_addr->client_fd = clientSocket;
   pthread_create(&wechat_send_tid, NULL, wechat_send_audio_thread, cmd_addr);
}
            
void mw_stop_wechat_intercom2(FFPlayer *ffp, const char *user_id, const char *user_name, const char bemaster, int client_fd){
    if(!audio_record_start){
        return;
    }   
    audio_record_start = 0;
    
    stopRecord();
    MwCmdPacket cmdPkt;
    cmdPkt.packet.header.cmd_type = CMD_HEARTBEAT;
    cmdPkt.packet.header.data_len = sizeof(cmdPkt.packet.content);//strlen(pClt->user_id);
    cmdPkt.packet.content.bMaster = bemaster;
    cmdPkt.packet.content.bLeave = 1;
    memcpy(cmdPkt.packet.content.Account, user_id, sizeof(cmdPkt.packet.content.Account));
    memcpy(cmdPkt.packet.content.UserName, user_name, sizeof(cmdPkt.packet.content.UserName));
    int send_length = send(client_fd, &cmdPkt.packet, sizeof(cmdPkt.packet), 0);
    //av_log(NULL, AV_LOG_ERROR, "intercom %s:%d, send_length = %d,user_id=%s, user_name = %s,bemaster = %d\n",__func__, __LINE__, send_length, user_id, user_name, bemaster);
    
    
}
            
