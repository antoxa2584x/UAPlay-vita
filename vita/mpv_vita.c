/* libmpv subset for PS Vita.

   One playback session per "loadfile": a demux thread (FFmpeg, including the
   HLS demuxer) feeds packet queues read by a video thread and an audio
   thread. H.264 is decoded by the Vita hardware decoder (sceAvcdec) straight
   into vitaGL texture memory, with FFmpeg's software decoder as a fallback.
   Audio is decoded by FFmpeg, resampled to 48 kHz stereo and written to
   SceAudioOut, which is also the master clock. The render context only draws
   the newest finished texture, so every GL call stays on the UI thread. */

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <psp2/audioout.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <vitaGL.h>

#include "../source/catalog.h"
#include "../source/platform.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define TEXTURE_SLOTS 3
#define EVENT_QUEUE 32
#define MAX_AUDIO_TRACKS 16
#define MAX_SUB_TRACKS 8
#define AUDIO_RATE 48000
#define AUDIO_GRAIN 1024
#define QUEUE_HARD_BYTES (20 * 1024 * 1024)
#define VIDEO_QUEUE_TARGET 150
#define AUDIO_QUEUE_TARGET 230
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static void vlog(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    sceClibPrintf("[uaplay-player] %s\n", line);
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void deadline_after(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (long)ms * 1000000L;
    ts->tv_sec += ts->tv_nsec / 1000000000L;
    ts->tv_nsec %= 1000000000L;
}

/* ---------- packet queues ---------- */

typedef struct PacketNode {
    AVPacket *packet;
    int serial;
    struct PacketNode *next;
} PacketNode;

typedef struct {
    PacketNode *head, *tail;
    int count;
    size_t bytes;
} PacketQueue;

static void queue_put(PacketQueue *q, AVPacket *packet, int serial) {
    PacketNode *node = malloc(sizeof(*node));
    if (!node) { av_packet_free(&packet); return; }
    node->packet = packet;
    node->serial = serial;
    node->next = NULL;
    if (q->tail) q->tail->next = node; else q->head = node;
    q->tail = node;
    q->count++;
    q->bytes += (size_t)packet->size;
}

static AVPacket *queue_take(PacketQueue *q, int *serial) {
    PacketNode *node = q->head;
    if (!node) return NULL;
    q->head = node->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    q->bytes -= (size_t)node->packet->size;
    AVPacket *packet = node->packet;
    *serial = node->serial;
    free(node);
    return packet;
}

static void queue_clear(PacketQueue *q) {
    int serial;
    AVPacket *packet;
    while ((packet = queue_take(q, &serial))) av_packet_free(&packet);
}

/* ---------- subtitles ---------- */

typedef struct {
    double start, end;
    char *text;
} SubCue;

typedef struct {
    int id;
    char url[1024];
    char title[128];
    char lang[32];
    SubCue *cues;
    int count;
} SubTrack;

/* ---------- session ---------- */

typedef struct Session Session;

typedef struct {
    mpv_event_id id;
    int reason, error;
} QueuedEvent;

struct mpv_handle {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    char user_agent[256];
    char referrer[512];
    long bitrate_cap;          /* 0 = automatic choice for the Vita screen */
    double volume, sub_delay, panscan;
    int paused;
    int requested_aid;         /* 0 = default track */
    Session *active;
    int live_sessions;
    int generation;
    SubTrack subs[MAX_SUB_TRACKS];
    int sub_count, sid, next_sub_id;
    GLuint garbage[TEXTURE_SLOTS * 8];
    int garbage_count;
    QueuedEvent queue[EVENT_QUEUE];
    int queue_head, queue_count;
    mpv_event current;
    mpv_event_end_file current_end;
};

struct Session {
    mpv_handle *owner;
    int generation;
    char url[2048];
    char user_agent[256];
    char referrer[512];
    long bitrate_cap, preferred_bandwidth;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    volatile int abort;
    pthread_t audio_thread, video_thread;
    int audio_started, video_started;

    AVFormatContext *format;
    int video_index;
    int audio_index, pending_audio_index;
    int audio_streams[MAX_AUDIO_TRACKS];
    int audio_count;
    PacketQueue video_queue, audio_queue;
    int serial;
    int eof, video_done, audio_done;
    int seek_pending;
    double seek_target;        /* seconds from the start of the media */
    double drop_before;        /* absolute stream seconds, NAN when unused */
    double start_time, duration;
    int loaded;

    /* Master clock in absolute stream seconds. */
    double clock_pts;
    int64_t clock_set_us;
    int clock_valid, clock_from_audio;
    int paused;
    double volume;
    double position;

    /* Texture ring, created on the UI thread on request. */
    int texture_w, texture_h, texture_request, texture_ready;
    GLuint textures[TEXTURE_SLOTS];
    void *texture_data[TEXTURE_SLOTS];
    int current_slot, frame_w, frame_h;
    double frame_aspect;
};

static int hw_library_ready;

static void post_event(mpv_handle *h, mpv_event_id id, int reason, int error) {
    if (h->queue_count == EVENT_QUEUE) return;
    QueuedEvent *e = &h->queue[(h->queue_head + h->queue_count) % EVENT_QUEUE];
    e->id = id;
    e->reason = reason;
    e->error = error;
    h->queue_count++;
}

/* Events from a session that is no longer current must never reach the UI. */
static void session_event(Session *s, mpv_event_id id, int reason, int error) {
    mpv_handle *h = s->owner;
    pthread_mutex_lock(&h->lock);
    if (h->active == s && !s->abort) post_event(h, id, reason, error);
    pthread_mutex_unlock(&h->lock);
}

static int interrupt_cb(void *opaque) {
    return ((Session *)opaque)->abort;
}

static double clock_now(Session *s) {
    if (!s->clock_valid) return NAN;
    if (s->paused) return s->clock_pts;
    double elapsed = (double)(now_us() - s->clock_set_us) / 1e6;
    /* Audio updates the clock every ~21 ms; do not run far ahead of it
       while the audio queue is starved. */
    if (s->clock_from_audio && !s->audio_done && elapsed > 0.15) elapsed = 0.15;
    return s->clock_pts + elapsed;
}

static void clock_set(Session *s, double pts, int from_audio) {
    s->clock_pts = pts;
    s->clock_set_us = now_us();
    s->clock_valid = 1;
    s->clock_from_audio = from_audio;
}

static double stream_seconds(AVStream *stream, int64_t ts) {
    if (ts == AV_NOPTS_VALUE) return NAN;
    return ts * av_q2d(stream->time_base);
}

/* ---------- input ---------- */

static const char *metadata(AVDictionary *dict, const char *key) {
    AVDictionaryEntry *entry = av_dict_get(dict, key, NULL, 0);
    return entry && entry->value ? entry->value : "";
}

static int program_has(AVProgram *program, int index) {
    for (unsigned i = 0; i < program->nb_stream_indexes; i++)
        if ((int)program->stream_index[i] == index) return 1;
    return 0;
}

/* HLS masters expose one AVProgram per variant. Keep the variant that best
   fits the 960x544 screen (or the user's bitrate ceiling) and discard all
   others so the demuxer never downloads their segments. */
typedef struct { char *data; size_t length; } Playlist;

static size_t playlist_chunk(char *ptr, size_t size, size_t count, void *opaque) {
    Playlist *p = opaque;
    size_t bytes = size * count;
    if (p->length + bytes > 512 * 1024) return 0;
    char *grown = realloc(p->data, p->length + bytes + 1);
    if (!grown) return 0;
    p->data = grown;
    memcpy(p->data + p->length, ptr, bytes);
    p->length += bytes;
    p->data[p->length] = 0;
    return bytes;
}

/* FFmpeg only learns variant resolutions after probing every variant's
   segments, so read RESOLUTION/BANDWIDTH from the master playlist and pick
   the variant before opening. Returns its BANDWIDTH, or 0 if not a master. */
static long preferred_bandwidth(Session *s) {
    if (!strstr(s->url, ".m3u8") && !strstr(s->url, "m3u")) return 0;
    Playlist p = {0};
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    curl_easy_setopt(curl, CURLOPT_URL, s->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, s->user_agent);
    if (s->referrer[0]) curl_easy_setopt(curl, CURLOPT_REFERER, s->referrer);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    net_tls_options(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, playlist_chunk);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &p);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    long best = 0, best_fit_height = -1, lowest = 0, lowest_height = 1 << 30;
    for (char *line = p.data; line && (line = strstr(line, "#EXT-X-STREAM-INF:")); line++) {
        char *end = strchr(line, '\n');
        char *bw = strstr(line, "BANDWIDTH=");
        char *res = strstr(line, "RESOLUTION=");
        if (!bw || (end && bw > end)) continue;
        long rate = atol(bw + 10);
        int height = 0;
        if (res && (!end || res < end)) {
            char *x = strchr(res, 'x');
            if (x) height = atoi(x + 1);
        }
        if (height && height < lowest_height) { lowest_height = height; lowest = rate; }
        int fits = s->bitrate_cap > 0 ? rate <= s->bitrate_cap : height > 0 && height <= SCREEN_H;
        if (fits && (height > best_fit_height || (height == best_fit_height && rate > best))) {
            best = rate;
            best_fit_height = height;
        }
    }
    free(p.data);
    return best ? best : lowest;
}

static AVProgram *choose_variant(Session *s) {
    AVFormatContext *f = s->format;
    if (f->nb_programs < 2) return f->nb_programs ? f->programs[0] : NULL;
    if (s->preferred_bandwidth > 0)
        for (unsigned p = 0; p < f->nb_programs; p++)
            if (atol(metadata(f->programs[p]->metadata, "variant_bitrate")) == s->preferred_bandwidth)
                return f->programs[p];
    AVProgram *best = NULL;
    long best_rate = -1;
    int best_height = 0;
    AVProgram *lowest = NULL;
    long lowest_rate = 0;
    for (unsigned p = 0; p < f->nb_programs; p++) {
        AVProgram *program = f->programs[p];
        long rate = atol(metadata(program->metadata, "variant_bitrate"));
        int height = 0, h264 = 0;
        for (unsigned i = 0; i < program->nb_stream_indexes; i++) {
            AVCodecParameters *par = f->streams[program->stream_index[i]]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                height = par->height;
                h264 = par->codec_id == AV_CODEC_ID_H264;
            }
        }
        if (!h264) continue;
        if (!lowest || rate < lowest_rate) { lowest = program; lowest_rate = rate; }
        /* Auto: the sharpest variant that is not taller than the panel
           (960x544, so typically 540p), saving bandwidth and decode time. */
        int fits = s->bitrate_cap > 0 ? rate <= s->bitrate_cap
                                      : (height == 0 || height <= SCREEN_H);
        if (!fits) continue;
        if (rate > best_rate || (rate == best_rate && height > best_height)) {
            best = program; best_rate = rate; best_height = height;
        }
    }
    return best ? best : lowest ? lowest : f->programs[0];
}

static int open_input(Session *s) {
    s->preferred_bandwidth = preferred_bandwidth(s);
    AVDictionary *options = NULL;
    av_dict_set(&options, "user_agent", s->user_agent, 0);
    if (s->referrer[0]) av_dict_set(&options, "referer", s->referrer, 0);
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "4", 0);
    av_dict_set(&options, "rw_timeout", "20000000", 0);
    s->format = avformat_alloc_context();
    if (!s->format) return AVERROR(ENOMEM);
    s->format->interrupt_callback.callback = interrupt_cb;
    s->format->interrupt_callback.opaque = s;
    s->format->probesize = 1 << 20;
    s->format->max_analyze_duration = 3 * AV_TIME_BASE;
    int result = avformat_open_input(&s->format, s->url, NULL, &options);
    av_dict_free(&options);
    if (result < 0) { s->format = NULL; return result; }

    AVFormatContext *f = s->format;
    AVProgram *program = choose_variant(s);
    s->video_index = -1;
    s->audio_count = 0;
    for (unsigned i = 0; i < f->nb_streams; i++) {
        AVStream *st = f->streams[i];
        int wanted = !program || program_has(program, (int)i);
        st->discard = AVDISCARD_ALL;
        if (!wanted) continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && s->video_index < 0 &&
            st->codecpar->codec_id == AV_CODEC_ID_H264)
            s->video_index = (int)i;
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                 s->audio_count < MAX_AUDIO_TRACKS)
            s->audio_streams[s->audio_count++] = (int)i;
    }
    if (s->video_index < 0) return AVERROR_DECODER_NOT_FOUND;
    f->streams[s->video_index]->discard = AVDISCARD_DEFAULT;

    int chosen = -1;
    int requested = s->owner->requested_aid;
    if (requested > 0 && requested <= s->audio_count) chosen = s->audio_streams[requested - 1];
    for (int i = 0; chosen < 0 && i < s->audio_count; i++)
        if (f->streams[s->audio_streams[i]]->disposition & AV_DISPOSITION_DEFAULT)
            chosen = s->audio_streams[i];
    if (chosen < 0 && s->audio_count) chosen = s->audio_streams[0];
    s->audio_index = s->pending_audio_index = chosen;
    if (chosen >= 0) f->streams[chosen]->discard = AVDISCARD_DEFAULT;

    result = avformat_find_stream_info(f, NULL);
    if (result < 0) vlog("find_stream_info: %d", result);
    s->start_time = f->start_time != AV_NOPTS_VALUE ? (double)f->start_time / AV_TIME_BASE : 0;
    s->duration = f->duration > 0 ? (double)f->duration / AV_TIME_BASE : 0;
    AVCodecParameters *par = f->streams[s->video_index]->codecpar;
    vlog("opened %dx%d, %d audio tracks, duration %.1f", par->width, par->height,
         s->audio_count, s->duration);
    return 0;
}

/* ---------- presentation ---------- */

/* Ask the UI thread for a texture ring big enough for w x h. */
static int request_textures(Session *s, int w, int h) {
    pthread_mutex_lock(&s->lock);
    if (s->texture_ready && s->texture_w >= w && s->texture_h >= h) {
        pthread_mutex_unlock(&s->lock);
        return 1;
    }
    s->texture_w = ALIGN(w, 16);
    s->texture_h = ALIGN(h, 16);
    s->texture_ready = 0;
    s->texture_request = 1;
    s->current_slot = -1;
    while (!s->abort && !s->texture_ready) pthread_cond_wait(&s->cond, &s->lock);
    int ok = s->texture_ready && s->texture_data[0];
    pthread_mutex_unlock(&s->lock);
    return ok;
}

static int next_slot(Session *s, int *rotor) {
    int slot = (*rotor + 1) % TEXTURE_SLOTS;
    if (slot == s->current_slot) slot = (slot + 1) % TEXTURE_SLOTS;
    *rotor = slot;
    return slot;
}

/* Wait until the master clock reaches pts, then publish the slot. Returns 0
   when the frame was dropped because of a seek, abort or lateness. */
static int present(Session *s, int slot, double pts, int serial, int w, int h) {
    pthread_mutex_lock(&s->lock);
    int published = 0;
    if (!isnan(s->drop_before) && !isnan(pts) && pts < s->drop_before - 0.02) goto out;
    int64_t waiting_since = now_us();
    while (!s->abort && s->serial == serial) {
        if (!s->clock_valid) {
            /* Without audio (or after a silent second) video drives the clock. */
            if (s->audio_index < 0 || s->audio_done || now_us() - waiting_since > 1000000)
                clock_set(s, isnan(pts) ? 0 : pts, 0);
            else {
                struct timespec ts;
                deadline_after(&ts, 10);
                pthread_cond_timedwait(&s->cond, &s->lock, &ts);
                continue;
            }
        }
        double clock = clock_now(s);
        double diff = isnan(pts) ? 0 : pts - clock;
        if (diff <= 0.005) {
            if (diff < -0.35 && s->current_slot >= 0) goto out;   /* too late */
            break;
        }
        struct timespec ts;
        deadline_after(&ts, s->paused ? 20 : (int)fmin(diff * 1000.0, 20.0) + 1);
        pthread_cond_timedwait(&s->cond, &s->lock, &ts);
    }
    if (s->abort || s->serial != serial) goto out;
    s->current_slot = slot;
    s->frame_w = w;
    s->frame_h = h;
    if (!isnan(pts)) s->position = pts - s->start_time;
    s->drop_before = NAN;
    published = 1;
out:
    pthread_mutex_unlock(&s->lock);
    return published;
}

/* ---------- video ---------- */

/* vitaGL claims all free CDRAM at start-up, so the decoder's working memory
   (sized for 1080p) is reserved once before vglInit and shared by sessions,
   one decoder at a time. */
static void *hw_memory;
static unsigned hw_memory_size;
static int hw_busy;
/* Set once the hardware decoder has proven unable to return pictures, so
   later sessions start on the CPU straight away. */
static int hw_unusable;
static pthread_mutex_t hw_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hw_cond = PTHREAD_COND_INITIALIZER;

typedef struct {
    SceAvcdecCtrl ctrl;
    int open, failures, silent, pictures;
} HardwareDecoder;

static void hw_close(HardwareDecoder *d) {
    if (d->open) {
        sceAvcdecDeleteDecoder(&d->ctrl);
        pthread_mutex_lock(&hw_mutex);
        hw_busy = 0;
        pthread_cond_broadcast(&hw_cond);
        pthread_mutex_unlock(&hw_mutex);
    }
    memset(d, 0, sizeof(*d));
}

static int hw_open(HardwareDecoder *d, Session *s, int w, int h) {
    memset(d, 0, sizeof(*d));
    if (!hw_library_ready || !hw_memory || hw_unusable) return 0;
    SceAvcdecQueryDecoderInfo query = { (uint32_t)ALIGN(w, 16), (uint32_t)ALIGN(h, 16), 5 };
    SceAvcdecDecoderInfo info = {0};
    if (sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info) < 0 ||
        info.frameMemSize > hw_memory_size) return 0;
    /* The previous session's video thread may still be shutting down. */
    pthread_mutex_lock(&hw_mutex);
    for (int i = 0; hw_busy && !s->abort && i < 300; i++) {
        struct timespec ts;
        deadline_after(&ts, 10);
        pthread_cond_timedwait(&hw_cond, &hw_mutex, &ts);
    }
    int available = !hw_busy && !s->abort;
    if (available) hw_busy = 1;
    pthread_mutex_unlock(&hw_mutex);
    if (!available) return 0;
    d->ctrl.frameBuf.pBuf = hw_memory;
    d->ctrl.frameBuf.size = hw_memory_size;
    int result = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &d->ctrl, &query);
    if (result < 0) {
        vlog("sceAvcdecCreateDecoder %dx%d: 0x%08X", w, h, result);
        pthread_mutex_lock(&hw_mutex);
        hw_busy = 0;
        pthread_cond_broadcast(&hw_cond);
        pthread_mutex_unlock(&hw_mutex);
        return 0;
    }
    d->open = 1;
    return 1;
}

int mpv_vita_preinit(void) {
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC) < 0) return 0;
    SceVideodecQueryInitInfoHwAvcdec init = { sizeof(init), 1920, 1088, 5, 1 };
    int result = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (result < 0) {
        vlog("sceVideodecInitLibrary: 0x%08X", result);
        return 0;
    }
    hw_library_ready = 1;
    /* Prefer room for 1080p; some implementations only accept smaller. */
    static const uint32_t sizes[][2] = { {1920, 1088}, {1280, 720}, {960, 544} };
    SceAvcdecDecoderInfo info = {0};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        SceAvcdecQueryDecoderInfo query = { sizes[i][0], sizes[i][1], 5 };
        result = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info);
        if (result >= 0 && info.frameMemSize) break;
        info.frameMemSize = 0;
    }
    if (!info.frameMemSize) {
        vlog("sceAvcdecQueryDecoderMemSize: 0x%08X", result);
        return 0;
    }
    unsigned size = ALIGN(info.frameMemSize, 1024 * 1024);
    SceUID block = sceKernelAllocMemBlock("uaplay-avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    if (block < 0)
        block = sceKernelAllocMemBlock("uaplay-avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, NULL);
    if (block < 0 || sceKernelGetMemBlockBase(block, &hw_memory) < 0) {
        vlog("no memory for the hardware decoder (%u bytes)", size);
        hw_memory = NULL;
        return 0;
    }
    hw_memory_size = size;
    return 1;
}

typedef struct {
    Session *s;
    AVStream *stream;
    HardwareDecoder hw;
    AVBSFContext *annexb;
    AVCodecContext *sw;
    struct SwsContext *scaler;
    AVFrame *frame;
    int rotor;
    int w, h;
} VideoState;

static int open_software(VideoState *v) {
    const AVCodec *codec = avcodec_find_decoder(v->stream->codecpar->codec_id);
    if (!codec) return 0;
    v->sw = avcodec_alloc_context3(codec);
    if (!v->sw) return 0;
    avcodec_parameters_to_context(v->sw, v->stream->codecpar);
    /* Two frame threads use a second core; their stacks come from
       __wrap_pthread_create, as FFmpeg passes no stack size. */
    v->sw->thread_count = 2;
    /* The Cortex-A9 cannot afford the deblocking filter at 720p. */
    if (v->stream->codecpar->height > 480) v->sw->skip_loop_filter = AVDISCARD_ALL;
    v->sw->pkt_timebase = v->stream->time_base;
    if (avcodec_open2(v->sw, codec, NULL) < 0) { avcodec_free_context(&v->sw); return 0; }
    v->frame = av_frame_alloc();
    vlog("software H.264 decoder");
    return v->frame != NULL;
}

static void decode_hardware(VideoState *v, AVPacket *packet, int serial) {
    Session *s = v->s;
    int slot = next_slot(s, &v->rotor);
    SceAvcdecPicture picture;
    memset(&picture, 0, sizeof(picture));
    picture.size = sizeof(picture);
    picture.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
    picture.frame.framePitch = (uint32_t)s->texture_w;
    picture.frame.frameWidth = (uint32_t)s->texture_w;
    picture.frame.frameHeight = (uint32_t)s->texture_h;
    picture.frame.opt.rgba.alpha = 0xFF;
    picture.frame.pPicture[0] = s->texture_data[slot];
    /* Until the decoder has proven it writes pictures, plant a marker with
       alpha 1; RGBA output always stores alpha 0xFF. */
    volatile uint32_t *marker = (uint32_t *)s->texture_data[slot];
    if (!v->hw.pictures) *marker = 0x01020304;
    SceAvcdecPicture *pictures[1] = { &picture };
    SceAvcdecArrayPicture array = { 0, 1, pictures };
    SceAvcdecAu au;
    memset(&au, 0, sizeof(au));
    int64_t pts90 = packet->pts != AV_NOPTS_VALUE ?
        av_rescale_q(packet->pts, v->stream->time_base, (AVRational){1, 90000}) : -1;
    au.pts.upper = pts90 < 0 ? 0xFFFFFFFF : (uint32_t)(pts90 >> 32);
    au.pts.lower = pts90 < 0 ? 0xFFFFFFFF : (uint32_t)pts90;
    au.dts.upper = au.dts.lower = 0xFFFFFFFF;
    au.es.pBuf = packet->data;
    au.es.size = (uint32_t)packet->size;
    int result = sceAvcdecDecode(&v->hw.ctrl, &au, &array);
    if (result < 0) {
        if (++v->hw.failures == 1 || v->hw.failures % 50 == 0)
            vlog("sceAvcdecDecode: 0x%08X (%d)", result, v->hw.failures);
        return;
    }
    v->hw.failures = 0;
    if (!array.numOfOutput) {
        v->hw.silent++;
        return;
    }
    if (!v->hw.pictures && *marker == 0x01020304) {
        v->hw.silent++;
        return;
    }
    v->hw.pictures++;
    v->hw.silent = 0;
    double pts = NAN;
    if (picture.info.pts.lower != 0xFFFFFFFF) {
        int64_t value = ((int64_t)picture.info.pts.upper << 32) | picture.info.pts.lower;
        pts = value / 90000.0;
    } else if (pts90 >= 0) pts = pts90 / 90000.0;
    int w = picture.frame.horizontalSize ? (int)picture.frame.horizontalSize : v->w;
    int h = picture.frame.verticalSize ? (int)picture.frame.verticalSize : v->h;
    w -= (int)(picture.frame.frameCropLeftOffset + picture.frame.frameCropRightOffset);
    h -= (int)(picture.frame.frameCropTopOffset + picture.frame.frameCropBottomOffset);
    present(s, slot, pts, serial, w > 0 ? w : v->w, h > 0 ? h : v->h);
}

static void decode_software(VideoState *v, AVPacket *packet, int serial) {
    Session *s = v->s;
    if (avcodec_send_packet(v->sw, packet) < 0) return;
    while (!s->abort && avcodec_receive_frame(v->sw, v->frame) >= 0) {
        AVFrame *f = v->frame;
        if (!request_textures(s, f->width, f->height)) { av_frame_unref(f); return; }
        double pts = stream_seconds(v->stream, f->best_effort_timestamp);
        pthread_mutex_lock(&s->lock);
        int skip = !isnan(s->drop_before) && !isnan(pts) && pts < s->drop_before - 0.02;
        pthread_mutex_unlock(&s->lock);
        if (!skip) {
            int slot = next_slot(s, &v->rotor);
            v->scaler = sws_getCachedContext(v->scaler, f->width, f->height, f->format,
                                             f->width, f->height, AV_PIX_FMT_RGBA,
                                             SWS_POINT, NULL, NULL, NULL);
            if (v->scaler) {
                uint8_t *dst[4] = { s->texture_data[slot], NULL, NULL, NULL };
                int stride[4] = { s->texture_w * 4, 0, 0, 0 };
                sws_scale(v->scaler, (const uint8_t * const *)f->data, f->linesize,
                          0, f->height, dst, stride);
                present(s, slot, pts, serial, f->width, f->height);
            }
        }
        av_frame_unref(f);
    }
}

static void video_reset(VideoState *v) {
    if (v->sw) avcodec_flush_buffers(v->sw);
    if (v->annexb) av_bsf_flush(v->annexb);
    if (v->hw.open) {
        /* The hardware decoder has no public flush; a new instance drops
           reference pictures from before the seek. */
        hw_close(&v->hw);
        if (!hw_open(&v->hw, v->s, v->w, v->h) && !v->sw) open_software(v);
    }
}

static void *video_main(void *opaque) {
    Session *s = opaque;
    VideoState v;
    memset(&v, 0, sizeof(v));
    v.s = s;
    v.stream = s->format->streams[s->video_index];
    AVCodecParameters *par = v.stream->codecpar;
    v.w = par->width > 0 ? par->width : 1920;
    v.h = par->height > 0 ? par->height : 1088;
    if (hw_open(&v.hw, s, v.w, v.h) && request_textures(s, v.w, v.h)) {
        vlog("hardware H.264 decoder %dx%d", v.w, v.h);
        /* MP4 streams carry avcC NAL lengths; the hardware wants Annex B. */
        if (par->extradata_size > 0 && par->extradata[0] == 1) {
            const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
            if (filter && av_bsf_alloc(filter, &v.annexb) >= 0) {
                avcodec_parameters_copy(v.annexb->par_in, par);
                v.annexb->time_base_in = v.stream->time_base;
                if (av_bsf_init(v.annexb) < 0) av_bsf_free(&v.annexb);
            }
        }
    } else {
        hw_close(&v.hw);
        if (!open_software(&v)) {
            vlog("no usable H.264 decoder");
            pthread_mutex_lock(&s->lock);
            s->video_done = 1;
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->lock);
            return NULL;
        }
    }
    AVPacket *filtered = av_packet_alloc();
    int serial = -1;
    while (!s->abort) {
        pthread_mutex_lock(&s->lock);
        int packet_serial = 0;
        AVPacket *packet = NULL;
        while (!s->abort && !(packet = queue_take(&s->video_queue, &packet_serial))) {
            if (s->eof && !s->video_done) {
                s->video_done = 1;
                pthread_cond_broadcast(&s->cond);
            }
            pthread_cond_wait(&s->cond, &s->lock);
        }
        if (packet) s->video_done = 0;
        pthread_cond_broadcast(&s->cond);    /* room in the queue */
        pthread_mutex_unlock(&s->lock);
        if (!packet) break;
        if (packet_serial != serial) {
            if (serial >= 0) video_reset(&v);
            serial = packet_serial;
        }
        if (v.hw.open) {
            if (v.annexb) {
                if (av_bsf_send_packet(v.annexb, packet) >= 0)
                    while (av_bsf_receive_packet(v.annexb, filtered) >= 0) {
                        decode_hardware(&v, filtered, serial);
                        av_packet_unref(filtered);
                    }
            } else decode_hardware(&v, packet, serial);
            if (v.hw.failures > 60 || (!v.hw.pictures && v.hw.silent > 48)) {
                /* Profiles the hardware rejects (for example 4:2:2 or
                   level > 4.2) continue on the CPU, as does a decoder that
                   accepts data but never returns pictures (Vita3K has no
                   RGBA output). */
                if (!v.hw.pictures) hw_unusable = 1;
                vlog("falling back to software decoding");
                hw_close(&v.hw);
                if (!open_software(&v)) { av_packet_free(&packet); break; }
            }
        } else decode_software(&v, packet, serial);
        av_packet_free(&packet);
    }
    av_packet_free(&filtered);
    hw_close(&v.hw);
    av_bsf_free(&v.annexb);
    avcodec_free_context(&v.sw);
    av_frame_free(&v.frame);
    sws_freeContext(v.scaler);
    return NULL;
}

/* ---------- audio ---------- */

static int open_audio_port(Session *s) {
    for (int attempt = 0; attempt < 100 && !s->abort; attempt++) {
        int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN,
                                       AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0)
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_GRAIN,
                                       AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
        if (port >= 0) return port;
        /* The previous session may still be releasing its port. */
        struct timespec pause = {0, 20 * 1000000L};
        nanosleep(&pause, NULL);
    }
    return -1;
}

static AVCodecContext *open_audio_decoder(AVStream *stream) {
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return NULL;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NULL;
    avcodec_parameters_to_context(ctx, stream->codecpar);
    ctx->pkt_timebase = stream->time_base;
    if (avcodec_open2(ctx, codec, NULL) < 0) avcodec_free_context(&ctx);
    return ctx;
}

static void *audio_main(void *opaque) {
    Session *s = opaque;
    int port = open_audio_port(s);
    if (port < 0) vlog("no audio port");
    AVCodecContext *decoder = NULL;
    SwrContext *resampler = NULL;
    AVFrame *frame = av_frame_alloc();
    int16_t *fifo = NULL;
    int fifo_samples = 0, fifo_capacity = 0;
    double fifo_pts = NAN;
    int16_t block[AUDIO_GRAIN * 2];
    int serial = -1, decoder_stream = -1;
    while (!s->abort && port >= 0) {
        pthread_mutex_lock(&s->lock);
        int packet_serial = 0;
        AVPacket *packet = NULL;
        while (!s->abort && !(packet = queue_take(&s->audio_queue, &packet_serial))) {
            if (s->eof && !s->audio_done) {
                s->audio_done = 1;
                pthread_cond_broadcast(&s->cond);
            }
            pthread_cond_wait(&s->cond, &s->lock);
        }
        if (packet) s->audio_done = 0;
        int stream_index = s->audio_index;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        if (!packet) break;
        if (packet_serial != serial) {
            serial = packet_serial;
            fifo_samples = 0;
            fifo_pts = NAN;
            if (decoder) avcodec_flush_buffers(decoder);
        }
        if (packet->stream_index != stream_index) { av_packet_free(&packet); continue; }
        AVStream *stream = s->format->streams[stream_index];
        if (decoder_stream != stream_index) {
            avcodec_free_context(&decoder);
            swr_free(&resampler);
            decoder = open_audio_decoder(stream);
            decoder_stream = stream_index;
        }
        if (!decoder || avcodec_send_packet(decoder, packet) < 0) {
            av_packet_free(&packet);
            continue;
        }
        av_packet_free(&packet);
        while (!s->abort && avcodec_receive_frame(decoder, frame) >= 0) {
            double pts = stream_seconds(stream, frame->best_effort_timestamp);
            if (!resampler) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, AUDIO_RATE,
                                    &frame->ch_layout, frame->format, frame->sample_rate,
                                    0, NULL);
                if (!resampler || swr_init(resampler) < 0) {
                    swr_free(&resampler);
                    av_frame_unref(frame);
                    continue;
                }
            }
            int room = (int)swr_get_out_samples(resampler, frame->nb_samples);
            if (fifo_samples + room > fifo_capacity) {
                int capacity = fifo_samples + room + AUDIO_GRAIN;
                int16_t *grown = realloc(fifo, (size_t)capacity * 2 * sizeof(int16_t));
                if (!grown) { av_frame_unref(frame); continue; }
                fifo = grown;
                fifo_capacity = capacity;
            }
            uint8_t *out[1] = { (uint8_t *)(fifo + fifo_samples * 2) };
            int converted = swr_convert(resampler, out, room,
                                        (const uint8_t **)frame->extended_data, frame->nb_samples);
            av_frame_unref(frame);
            if (converted <= 0) continue;
            if (!fifo_samples || isnan(fifo_pts))
                fifo_pts = isnan(pts) ? NAN : pts - (double)fifo_samples / AUDIO_RATE;
            fifo_samples += converted;

            while (fifo_samples >= AUDIO_GRAIN && !s->abort) {
                pthread_mutex_lock(&s->lock);
                while (s->paused && !s->abort && s->serial == serial)
                    pthread_cond_wait(&s->cond, &s->lock);
                int stale = s->serial != serial;
                double drop = s->drop_before;
                double volume = s->volume;
                pthread_mutex_unlock(&s->lock);
                if (stale) { fifo_samples = 0; break; }
                double block_end = fifo_pts + (double)AUDIO_GRAIN / AUDIO_RATE;
                if (!isnan(drop) && !isnan(fifo_pts) && block_end < drop) {
                    /* Before the seek target: discard silently. */
                } else {
                    float gain = (float)(volume / 100.0);
                    for (int i = 0; i < AUDIO_GRAIN * 2; i++) {
                        int value = (int)(fifo[i] * gain);
                        block[i] = (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
                    }
                    sceAudioOutOutput(port, block);
                    if (!isnan(fifo_pts)) {
                        pthread_mutex_lock(&s->lock);
                        /* One grain is queued in the hardware while this one
                           plays, so the audible sample lags by a grain. */
                        if (s->serial == serial) {
                            clock_set(s, fifo_pts, 1);
                            pthread_cond_broadcast(&s->cond);
                        }
                        pthread_mutex_unlock(&s->lock);
                    }
                }
                fifo_samples -= AUDIO_GRAIN;
                memmove(fifo, fifo + AUDIO_GRAIN * 2, (size_t)fifo_samples * 2 * sizeof(int16_t));
                if (!isnan(fifo_pts)) fifo_pts += (double)AUDIO_GRAIN / AUDIO_RATE;
            }
        }
    }
    if (port >= 0) sceAudioOutReleasePort(port);
    avcodec_free_context(&decoder);
    swr_free(&resampler);
    av_frame_free(&frame);
    free(fifo);
    pthread_mutex_lock(&s->lock);
    s->audio_done = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

/* ---------- demux ---------- */

/* Linked with --wrap=pthread_create: threads created without attributes
   (FFmpeg's decoder workers) would get a stack too small for H.264. */
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*body)(void *), void *arg);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*body)(void *), void *arg) {
    if (attr) return __real_pthread_create(thread, attr, body, arg);
    pthread_attr_t sized;
    pthread_attr_init(&sized);
    pthread_attr_setstacksize(&sized, 512 * 1024);
    int result = __real_pthread_create(thread, &sized, body, arg);
    pthread_attr_destroy(&sized);
    return result;
}

static int start_thread(pthread_t *thread, void *(*body)(void *), void *arg, size_t stack) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    int result = pthread_create(thread, &attr, body, arg);
    pthread_attr_destroy(&attr);
    return result == 0;
}

static void do_seek(Session *s) {
    double target = s->seek_target;
    if (s->duration > 0 && target > s->duration - 1) target = s->duration - 1;
    if (target < 0) target = 0;
    int new_audio = s->pending_audio_index;
    s->seek_pending = 0;
    pthread_mutex_unlock(&s->lock);
    AVFormatContext *f = s->format;
    if (new_audio != s->audio_index) {
        if (s->audio_index >= 0) f->streams[s->audio_index]->discard = AVDISCARD_ALL;
        if (new_audio >= 0) f->streams[new_audio]->discard = AVDISCARD_DEFAULT;
    }
    int64_t ts = (int64_t)((target + s->start_time) * AV_TIME_BASE);
    int result = avformat_seek_file(f, -1, INT64_MIN, ts, ts, 0);
    if (result < 0) vlog("seek to %.1f failed: %d", target, result);
    pthread_mutex_lock(&s->lock);
    s->audio_index = new_audio;
    queue_clear(&s->video_queue);
    queue_clear(&s->audio_queue);
    s->serial++;
    s->eof = s->video_done = s->audio_done = 0;
    s->clock_valid = 0;
    s->drop_before = target + s->start_time;
    s->position = target;
    pthread_cond_broadcast(&s->cond);
}

static void *demux_main(void *opaque) {
    Session *s = opaque;
    mpv_handle *h = s->owner;
    int result = open_input(s);
    if (result < 0) {
        char reason[96];
        av_strerror(result, reason, sizeof(reason));
        vlog("open failed: %s", reason);
        session_event(s, MPV_EVENT_END_FILE, MPV_END_FILE_REASON_ERROR,
                      result == AVERROR_DECODER_NOT_FOUND ? MPV_ERROR_UNKNOWN_FORMAT
                                                          : MPV_ERROR_LOADING_FAILED);
        goto finish;
    }
    pthread_mutex_lock(&s->lock);
    s->loaded = 1;
    pthread_mutex_unlock(&s->lock);
    s->video_started = start_thread(&s->video_thread, video_main, s, 1024 * 1024);
    if (s->audio_index >= 0)
        s->audio_started = start_thread(&s->audio_thread, audio_main, s, 256 * 1024);
    if (!s->audio_started) {
        s->audio_index = -1;
        s->audio_done = 1;
    }
    session_event(s, MPV_EVENT_FILE_LOADED, 0, 0);

    AVPacket *packet = av_packet_alloc();
    while (!s->abort) {
        pthread_mutex_lock(&s->lock);
        for (;;) {
            if (s->abort) break;
            if (s->seek_pending) { do_seek(s); continue; }
            if (s->eof) {
                if (s->video_done && s->audio_done) break;
            } else {
                size_t bytes = s->video_queue.bytes + s->audio_queue.bytes;
                int satisfied = s->video_queue.count >= VIDEO_QUEUE_TARGET &&
                                (s->audio_index < 0 || s->audio_queue.count >= AUDIO_QUEUE_TARGET);
                if (bytes < QUEUE_HARD_BYTES && !satisfied) break;
            }
            pthread_cond_wait(&s->cond, &s->lock);
        }
        int finished = s->eof && s->video_done && s->audio_done && !s->seek_pending;
        int serial = s->serial;
        pthread_mutex_unlock(&s->lock);
        if (s->abort) break;
        if (finished) {
            session_event(s, MPV_EVENT_END_FILE, MPV_END_FILE_REASON_EOF, 0);
            break;
        }
        result = av_read_frame(s->format, packet);
        if (result < 0) {
            if (result == AVERROR(EAGAIN)) continue;
            pthread_mutex_lock(&s->lock);
            if (s->serial == serial) s->eof = 1;
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->lock);
            continue;
        }
        pthread_mutex_lock(&s->lock);
        if (s->serial != serial) av_packet_unref(packet);   /* raced with a seek */
        else if (packet->stream_index == s->video_index) {
            queue_put(&s->video_queue, av_packet_clone(packet), serial);
        } else if (packet->stream_index == s->audio_index) {
            queue_put(&s->audio_queue, av_packet_clone(packet), serial);
        }
        av_packet_unref(packet);
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }
    av_packet_free(&packet);

finish:
    /* Detach from the UI first: track-list and the renderer read the format
       context through h->active. */
    pthread_mutex_lock(&h->lock);
    if (h->active == s) h->active = NULL;
    pthread_mutex_unlock(&h->lock);
    pthread_mutex_lock(&s->lock);
    s->abort = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    if (s->video_started) pthread_join(s->video_thread, NULL);
    if (s->audio_started) pthread_join(s->audio_thread, NULL);
    queue_clear(&s->video_queue);
    queue_clear(&s->audio_queue);
    if (s->format) avformat_close_input(&s->format);
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < TEXTURE_SLOTS; i++)
        if (s->textures[i] && h->garbage_count < (int)(sizeof(h->garbage) / sizeof(h->garbage[0])))
            h->garbage[h->garbage_count++] = s->textures[i];
    h->live_sessions--;
    pthread_cond_broadcast(&h->cond);
    pthread_mutex_unlock(&h->lock);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s);
    return NULL;
}

/* The caller holds h->lock. Threads finish on their own; nothing blocks the
   UI even when a DNS lookup or TLS handshake is still in progress. */
static void stop_active(mpv_handle *h) {
    Session *s = h->active;
    if (!s) return;
    h->active = NULL;
    pthread_mutex_lock(&s->lock);
    s->abort = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static void clear_subtitles(mpv_handle *h) {
    for (int i = 0; i < h->sub_count; i++) {
        for (int c = 0; c < h->subs[i].count; c++) free(h->subs[i].cues[c].text);
        free(h->subs[i].cues);
    }
    h->sub_count = 0;
    h->sid = 0;
}

static void load_file(mpv_handle *h, const char *url) {
    stop_active(h);
    clear_subtitles(h);
    h->generation++;
    h->paused = 0;
    Session *s = calloc(1, sizeof(*s));
    if (!s) return;
    s->owner = h;
    s->generation = h->generation;
    snprintf(s->url, sizeof(s->url), "%s", url);
    snprintf(s->user_agent, sizeof(s->user_agent), "%s", h->user_agent);
    snprintf(s->referrer, sizeof(s->referrer), "%s", h->referrer);
    s->bitrate_cap = h->bitrate_cap;
    s->volume = h->volume;
    s->video_index = s->audio_index = s->pending_audio_index = -1;
    s->current_slot = -1;
    s->drop_before = NAN;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    pthread_t thread;
    h->active = s;
    h->live_sessions++;
    post_event(h, MPV_EVENT_START_FILE, 0, 0);
    if (!start_thread(&thread, demux_main, s, 512 * 1024)) {
        h->active = NULL;
        h->live_sessions--;
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->cond);
        free(s);
        post_event(h, MPV_EVENT_END_FILE, MPV_END_FILE_REASON_ERROR, MPV_ERROR_NOMEM);
        return;
    }
    pthread_detach(thread);
}

/* ---------- external subtitles ---------- */

typedef struct { char *data; size_t length; } Download;

static size_t download_chunk(char *ptr, size_t size, size_t count, void *opaque) {
    Download *d = opaque;
    size_t bytes = size * count;
    if (d->length + bytes > 4 * 1024 * 1024) return 0;
    char *grown = realloc(d->data, d->length + bytes + 1);
    if (!grown) return 0;
    d->data = grown;
    memcpy(d->data + d->length, ptr, bytes);
    d->length += bytes;
    d->data[d->length] = 0;
    return bytes;
}

static double cue_time(const char *text, const char **end) {
    int part[4] = {0}, count = 0;
    const char *p = text;
    while (count < 4) {
        if (!isdigit((unsigned char)*p)) break;
        part[count++] = (int)strtol(p, (char **)&p, 10);
        if (*p == ':' || *p == '.' || *p == ',') p++;
        else break;
    }
    if (end) *end = p;
    if (count == 4) return part[0] * 3600.0 + part[1] * 60.0 + part[2] + part[3] / 1000.0;
    if (count == 3) return part[0] * 60.0 + part[1] + part[2] / 1000.0;
    return -1;
}

/* Remove markup ({\an8}, <i>) and decode the common entities. */
static void clean_cue(char *text) {
    char *out = text;
    for (char *in = text; *in;) {
        if (*in == '<') { char *close = strchr(in, '>'); if (close) { in = close + 1; continue; } }
        if (*in == '{') { char *close = strchr(in, '}'); if (close) { in = close + 1; continue; } }
        if (*in == '&') {
            const char *names[] = {"&amp;", "&lt;", "&gt;", "&nbsp;", "&quot;"};
            const char values[] = {'&', '<', '>', ' ', '"'};
            int matched = 0;
            for (int i = 0; i < 5; i++)
                if (!strncmp(in, names[i], strlen(names[i]))) {
                    *out++ = values[i];
                    in += strlen(names[i]);
                    matched = 1;
                    break;
                }
            if (matched) continue;
        }
        if (*in != '\r') *out++ = *in;
        in++;
    }
    *out = 0;
    while (out > text && (out[-1] == '\n' || out[-1] == ' ')) *--out = 0;
}

/* Parses both WebVTT and SubRip; they differ only in headers and commas. */
static int parse_subtitles(char *data, SubCue **cues) {
    int count = 0, capacity = 0;
    *cues = NULL;
    char *line = data;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *arrow = strstr(line, "-->");
        if (arrow) {
            double start = cue_time(line + strspn(line, " \t"), NULL);
            double end = cue_time(arrow + 3 + strspn(arrow + 3, " \t"), NULL);
            char text[1024];
            size_t used = 0;
            text[0] = 0;
            line = next;
            while (line && *line && *line != '\r') {
                next = strchr(line, '\n');
                if (next) *next++ = 0;
                size_t length = strlen(line);
                if (used + length + 2 < sizeof(text)) {
                    if (used) text[used++] = '\n';
                    memcpy(text + used, line, length);
                    used += length;
                    text[used] = 0;
                }
                line = next;
            }
            clean_cue(text);
            if (start >= 0 && end > start && text[0]) {
                if (count == capacity) {
                    capacity = capacity ? capacity * 2 : 256;
                    SubCue *grown = realloc(*cues, (size_t)capacity * sizeof(SubCue));
                    if (!grown) break;
                    *cues = grown;
                }
                (*cues)[count].start = start;
                (*cues)[count].end = end;
                (*cues)[count].text = strdup(text);
                count++;
            }
            continue;
        }
        line = next;
    }
    return count;
}

typedef struct {
    mpv_handle *owner;
    int generation, select;
    char url[1024], title[128], lang[32];
    char user_agent[256], referrer[512];
} SubRequest;

static void *subtitle_main(void *opaque) {
    SubRequest *r = opaque;
    Download d = {0};
    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, r->url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, r->user_agent);
        if (r->referrer[0]) curl_easy_setopt(curl, CURLOPT_REFERER, r->referrer);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        net_tls_options(curl);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_chunk);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &d);
        CURLcode result = curl_easy_perform(curl);
        if (result != CURLE_OK) vlog("subtitle download: %s", curl_easy_strerror(result));
        curl_easy_cleanup(curl);
    }
    SubCue *cues = NULL;
    int count = d.data ? parse_subtitles(d.data, &cues) : 0;
    free(d.data);
    mpv_handle *h = r->owner;
    pthread_mutex_lock(&h->lock);
    if (count && h->generation == r->generation && h->sub_count < MAX_SUB_TRACKS) {
        SubTrack *t = &h->subs[h->sub_count++];
        memset(t, 0, sizeof(*t));
        t->id = ++h->next_sub_id;
        snprintf(t->url, sizeof(t->url), "%s", r->url);
        snprintf(t->title, sizeof(t->title), "%s", r->title);
        snprintf(t->lang, sizeof(t->lang), "%s", r->lang);
        t->cues = cues;
        t->count = count;
        cues = NULL;
        if (r->select || !h->sid) h->sid = t->id;
    }
    h->live_sessions--;
    pthread_cond_broadcast(&h->cond);
    pthread_mutex_unlock(&h->lock);
    if (cues) {
        for (int i = 0; i < count; i++) free(cues[i].text);
        free(cues);
    }
    free(r);
    return NULL;
}

static void add_subtitle(mpv_handle *h, const char **args) {
    SubRequest *r = calloc(1, sizeof(*r));
    if (!r) return;
    r->owner = h;
    r->generation = h->generation;
    r->select = args[2] && !strcmp(args[2], "select");
    snprintf(r->url, sizeof(r->url), "%s", args[1]);
    if (args[2] && args[3]) snprintf(r->title, sizeof(r->title), "%s", args[3]);
    if (args[2] && args[3] && args[4]) snprintf(r->lang, sizeof(r->lang), "%s", args[4]);
    snprintf(r->user_agent, sizeof(r->user_agent), "%s", h->user_agent);
    snprintf(r->referrer, sizeof(r->referrer), "%s", h->referrer);
    pthread_t thread;
    h->live_sessions++;
    if (!start_thread(&thread, subtitle_main, r, 128 * 1024)) {
        h->live_sessions--;
        free(r);
        return;
    }
    pthread_detach(thread);
}

/* ---------- client API ---------- */

mpv_handle *mpv_create(void) {
    mpv_handle *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    pthread_mutex_init(&h->lock, NULL);
    pthread_cond_init(&h->cond, NULL);
    snprintf(h->user_agent, sizeof(h->user_agent), "Mozilla/5.0 (PlayStation Vita 3.74) UAPlay/1.0");
    h->volume = 100;
    return h;
}

int mpv_initialize(mpv_handle *h) {
    if (!h) return MPV_ERROR_INVALID_PARAMETER;
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();
    return 0;
}

void mpv_terminate_destroy(mpv_handle *h) {
    if (!h) return;
    pthread_mutex_lock(&h->lock);
    stop_active(h);
    h->generation++;
    for (int i = 0; i < 300 && h->live_sessions > 0; i++) {
        struct timespec ts;
        deadline_after(&ts, 10);
        pthread_cond_timedwait(&h->cond, &h->lock, &ts);
    }
    int pending = h->live_sessions;
    clear_subtitles(h);
    pthread_mutex_unlock(&h->lock);
    if (hw_library_ready && !pending) {
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        hw_library_ready = 0;
    }
    /* A thread stuck in DNS keeps the handle; the process is exiting. */
    if (pending) return;
    pthread_mutex_destroy(&h->lock);
    pthread_cond_destroy(&h->cond);
    free(h);
}

const char *mpv_error_string(int error) {
    switch (error) {
        case MPV_ERROR_SUCCESS: return "success";
        case MPV_ERROR_NOMEM: return "memory allocation failed";
        case MPV_ERROR_INVALID_PARAMETER: return "invalid parameter";
        case MPV_ERROR_PROPERTY_NOT_FOUND: return "property not found";
        case MPV_ERROR_PROPERTY_UNAVAILABLE: return "property unavailable";
        case MPV_ERROR_LOADING_FAILED: return "loading failed";
        case MPV_ERROR_UNKNOWN_FORMAT: return "unrecognized file format";
        default: return "error";
    }
}

static void set_value(mpv_handle *h, const char *name, const char *value) {
    if (!strcmp(name, "user-agent"))
        snprintf(h->user_agent, sizeof(h->user_agent), "%s", value);
    else if (!strcmp(name, "referrer"))
        snprintf(h->referrer, sizeof(h->referrer), "%s", value);
    else if (!strcmp(name, "hls-bitrate") || !strcmp(name, "options/hls-bitrate"))
        h->bitrate_cap = !strcmp(value, "max") || !strcmp(value, "min") ? 0 : atol(value);
    else if (!strcmp(name, "volume")) {
        h->volume = atof(value);
        if (h->active) {
            pthread_mutex_lock(&h->active->lock);
            h->active->volume = h->volume;
            pthread_mutex_unlock(&h->active->lock);
        }
    } else if (!strcmp(name, "sub-delay")) h->sub_delay = atof(value);
    else if (!strcmp(name, "panscan")) h->panscan = fmin(1.0, fmax(0.0, atof(value)));
    else if (!strcmp(name, "pause")) {
        int paused = !strcmp(value, "yes");
        h->paused = paused;
        Session *s = h->active;
        if (s) {
            pthread_mutex_lock(&s->lock);
            if (paused != s->paused) {
                if (paused && s->clock_valid) s->clock_pts = clock_now(s);
                s->paused = paused;
                s->clock_set_us = now_us();
                pthread_cond_broadcast(&s->cond);
            }
            pthread_mutex_unlock(&s->lock);
        }
    } else if (!strcmp(name, "sid")) {
        h->sid = !strcmp(value, "no") ? 0 : atoi(value);
    } else if (!strcmp(name, "aid")) {
        int id = atoi(value);
        h->requested_aid = id;
        Session *s = h->active;
        if (s) {
            pthread_mutex_lock(&s->lock);
            if (s->loaded && id >= 1 && id <= s->audio_count &&
                s->audio_streams[id - 1] != s->audio_index) {
                /* mpv performs a refresh seek on track switches as well. */
                s->pending_audio_index = s->audio_streams[id - 1];
                s->seek_target = s->position;
                s->seek_pending = 1;
                pthread_cond_broadcast(&s->cond);
            }
            pthread_mutex_unlock(&s->lock);
        }
    }
    /* Other options (cache, hwdec, sub-visibility, speed...) have no
       equivalent here and are accepted silently, as libmpv would. */
}

int mpv_set_option_string(mpv_handle *h, const char *name, const char *data) {
    if (!h || !name || !data) return MPV_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&h->lock);
    set_value(h, name, data);
    pthread_mutex_unlock(&h->lock);
    return 0;
}

int mpv_set_property_string(mpv_handle *h, const char *name, const char *data) {
    return mpv_set_option_string(h, name, data);
}

static double current_position(Session *s) {
    if (s->seek_pending) return s->seek_target;
    if (s->current_slot < 0 && !isnan(s->drop_before)) return s->position;
    return s->position < 0 ? 0 : s->position;
}

static mpv_node_list *node_list(int count, int map) {
    mpv_node_list *list = calloc(1, sizeof(*list));
    if (!list) return NULL;
    list->values = calloc((size_t)(count ? count : 1), sizeof(mpv_node));
    if (map) list->keys = calloc((size_t)(count ? count : 1), sizeof(char *));
    return list;
}

static void map_string(mpv_node_list *map, const char *key, const char *value) {
    map->keys[map->num] = strdup(key);
    map->values[map->num].format = MPV_FORMAT_STRING;
    map->values[map->num].u.string = strdup(value ? value : "");
    map->num++;
}

static void map_int(mpv_node_list *map, const char *key, int64_t value, mpv_format format) {
    map->keys[map->num] = strdup(key);
    map->values[map->num].format = format;
    if (format == MPV_FORMAT_FLAG) map->values[map->num].u.flag = (int)value;
    else map->values[map->num].u.int64 = value;
    map->num++;
}

static void track_list(mpv_handle *h, mpv_node *out) {
    Session *s = h->active;
    int audio = 0;
    if (s) {
        pthread_mutex_lock(&s->lock);
        audio = s->loaded ? s->audio_count : 0;
    }
    mpv_node_list *list = node_list(audio + h->sub_count, 0);
    out->format = MPV_FORMAT_NODE_ARRAY;
    out->u.list = list;
    for (int i = 0; list && i < audio; i++) {
        AVStream *st = s->format->streams[s->audio_streams[i]];
        mpv_node_list *map = node_list(6, 1);
        if (!map) break;
        map_string(map, "type", "audio");
        map_int(map, "id", i + 1, MPV_FORMAT_INT64);
        map_string(map, "lang", metadata(st->metadata, "language"));
        const char *title = metadata(st->metadata, "title");
        if (!*title) title = metadata(st->metadata, "comment");
        map_string(map, "title", title);
        map_int(map, "selected", s->audio_streams[i] == s->audio_index, MPV_FORMAT_FLAG);
        list->values[list->num].format = MPV_FORMAT_NODE_MAP;
        list->values[list->num].u.list = map;
        list->num++;
    }
    if (s) pthread_mutex_unlock(&s->lock);
    for (int i = 0; list && i < h->sub_count; i++) {
        mpv_node_list *map = node_list(6, 1);
        if (!map) break;
        map_string(map, "type", "sub");
        map_int(map, "id", h->subs[i].id, MPV_FORMAT_INT64);
        map_string(map, "lang", h->subs[i].lang);
        map_string(map, "title", h->subs[i].title);
        map_int(map, "selected", h->subs[i].id == h->sid, MPV_FORMAT_FLAG);
        map_string(map, "external-filename", h->subs[i].url);
        list->values[list->num].format = MPV_FORMAT_NODE_MAP;
        list->values[list->num].u.list = map;
        list->num++;
    }
}

int mpv_get_property(mpv_handle *h, const char *name, mpv_format format, void *data) {
    if (!h || !name || !data) return MPV_ERROR_INVALID_PARAMETER;
    int result = 0;
    pthread_mutex_lock(&h->lock);
    Session *s = h->active;
    if (!strcmp(name, "track-list") && format == MPV_FORMAT_NODE) {
        track_list(h, data);
    } else if (!strcmp(name, "pause") && format == MPV_FORMAT_FLAG) {
        *(int *)data = h->paused;
    } else if ((!strcmp(name, "time-pos") || !strcmp(name, "duration")) &&
               format == MPV_FORMAT_DOUBLE) {
        if (!s) result = MPV_ERROR_PROPERTY_UNAVAILABLE;
        else {
            pthread_mutex_lock(&s->lock);
            if (!s->loaded) result = MPV_ERROR_PROPERTY_UNAVAILABLE;
            else if (name[0] == 't') *(double *)data = current_position(s);
            else if (s->duration > 0) *(double *)data = s->duration;
            else result = MPV_ERROR_PROPERTY_UNAVAILABLE;
            pthread_mutex_unlock(&s->lock);
        }
    } else if (!strcmp(name, "volume") && format == MPV_FORMAT_DOUBLE) {
        *(double *)data = h->volume;
    } else result = MPV_ERROR_PROPERTY_NOT_FOUND;
    pthread_mutex_unlock(&h->lock);
    return result;
}

static void seek_to(mpv_handle *h, double value, int relative) {
    Session *s = h->active;
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    if (s->loaded) {
        double target = relative ? current_position(s) + value : value;
        if (target < 0) target = 0;
        if (s->duration > 0 && target > s->duration) target = s->duration;
        s->seek_target = target;
        s->seek_pending = 1;
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->lock);
}

int mpv_command_async(mpv_handle *h, uint64_t reply_userdata, const char **args) {
    (void)reply_userdata;
    if (!h || !args || !args[0]) return MPV_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&h->lock);
    const char *verb = args[0];
    if (!strcmp(verb, "loadfile") && args[1]) load_file(h, args[1]);
    else if (!strcmp(verb, "stop")) { stop_active(h); clear_subtitles(h); h->generation++; }
    else if (!strcmp(verb, "seek") && args[1])
        seek_to(h, atof(args[1]), !args[2] || strcmp(args[2], "absolute"));
    else if (!strcmp(verb, "cycle") && args[1] && !strcmp(args[1], "pause"))
        set_value(h, "pause", h->paused ? "no" : "yes");
    else if (!strcmp(verb, "set") && args[1] && args[2]) set_value(h, args[1], args[2]);
    else if (!strcmp(verb, "sub-add") && args[1]) add_subtitle(h, args);
    pthread_mutex_unlock(&h->lock);
    return 0;
}

int mpv_request_log_messages(mpv_handle *h, const char *min_level) {
    (void)h; (void)min_level;
    return 0;
}

mpv_event *mpv_wait_event(mpv_handle *h, double timeout) {
    (void)timeout;
    pthread_mutex_lock(&h->lock);
    memset(&h->current, 0, sizeof(h->current));
    if (h->queue_count) {
        QueuedEvent *e = &h->queue[h->queue_head];
        h->queue_head = (h->queue_head + 1) % EVENT_QUEUE;
        h->queue_count--;
        h->current.event_id = e->id;
        if (e->id == MPV_EVENT_END_FILE) {
            h->current_end.reason = e->reason;
            h->current_end.error = e->error;
            h->current.error = e->error;
            h->current.data = &h->current_end;
        }
    }
    pthread_mutex_unlock(&h->lock);
    return &h->current;
}

void mpv_free_node_contents(mpv_node *node) {
    if (!node) return;
    if (node->format == MPV_FORMAT_STRING) free(node->u.string);
    else if ((node->format == MPV_FORMAT_NODE_ARRAY || node->format == MPV_FORMAT_NODE_MAP) &&
             node->u.list) {
        mpv_node_list *list = node->u.list;
        for (int i = 0; i < list->num; i++) {
            mpv_free_node_contents(&list->values[i]);
            if (list->keys) free(list->keys[i]);
        }
        free(list->values);
        free(list->keys);
        free(list);
    }
    memset(node, 0, sizeof(*node));
}

int mpv_vita_subtitle(mpv_handle *h, char *text, size_t size) {
    if (!h || !size) return 0;
    text[0] = 0;
    pthread_mutex_lock(&h->lock);
    Session *s = h->active;
    SubTrack *track = NULL;
    for (int i = 0; i < h->sub_count; i++)
        if (h->subs[i].id == h->sid) track = &h->subs[i];
    if (track && s) {
        pthread_mutex_lock(&s->lock);
        double at = s->loaded ? current_position(s) - h->sub_delay : -1;
        pthread_mutex_unlock(&s->lock);
        int lo = 0, hi = track->count - 1, found = -1;
        while (lo <= hi) {   /* last cue starting at or before "at" */
            int mid = (lo + hi) / 2;
            if (track->cues[mid].start <= at) { found = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        /* Overlapping cues: look back a few for one still on screen. */
        for (int i = found; i >= 0 && i > found - 4; i--)
            if (at < track->cues[i].end) {
                snprintf(text, size, "%s", track->cues[i].text);
                break;
            }
    }
    pthread_mutex_unlock(&h->lock);
    return text[0] != 0;
}

/* ---------- render context ---------- */

struct mpv_render_context {
    mpv_handle *mpv;
    GLuint program, buffer;
    GLint sampler;
};

static const char *video_vs =
    "attribute vec2 pos;attribute vec2 uv;varying vec2 tc;"
    "void main(){gl_Position=vec4(pos,0.,1.);tc=uv;}";
static const char *video_fs =
    "precision mediump float;varying vec2 tc;uniform sampler2D image;"
    "void main(){gl_FragColor=vec4(texture2D(image,tc).rgb,1.0);}";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    return shader;
}

int mpv_render_context_create(mpv_render_context **res, mpv_handle *mpv,
                              mpv_render_param *params) {
    (void)params;
    mpv_render_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return MPV_ERROR_NOMEM;
    ctx->mpv = mpv;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, video_vs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, video_fs);
    ctx->program = glCreateProgram();
    glAttachShader(ctx->program, vs);
    glAttachShader(ctx->program, fs);
    glBindAttribLocation(ctx->program, 0, "pos");
    glBindAttribLocation(ctx->program, 1, "uv");
    glLinkProgram(ctx->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    ctx->sampler = glGetUniformLocation(ctx->program, "image");
    glGenBuffers(1, &ctx->buffer);
    *res = ctx;
    return 0;
}

static void draw_quad(GLuint buffer, GLuint texture) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void create_textures(Session *s) {
    for (int i = 0; i < TEXTURE_SLOTS; i++) {
        if (s->textures[i]) glDeleteTextures(1, &s->textures[i]);
        s->textures[i] = 0;
        s->texture_data[i] = NULL;
    }
    glGenTextures(TEXTURE_SLOTS, s->textures);
    for (int i = 0; i < TEXTURE_SLOTS; i++) {
        glBindTexture(GL_TEXTURE_2D, s->textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->texture_w, s->texture_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s->texture_data[i] = vglGetTexDataPointer(GL_TEXTURE_2D);
        if (!s->texture_data[i]) {
            vlog("no texture memory for %dx%d", s->texture_w, s->texture_h);
            s->texture_data[0] = NULL;
            break;
        }
    }
}

int mpv_render_context_render(mpv_render_context *ctx, mpv_render_param *params) {
    (void)params;
    mpv_handle *h = ctx->mpv;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < h->garbage_count; i++) glDeleteTextures(1, &h->garbage[i]);
    h->garbage_count = 0;
    Session *s = h->active;
    GLuint texture = 0;
    float u = 1, v = 1, aspect = 16.f / 9.f;
    float panscan = (float)h->panscan;
    if (s) {
        pthread_mutex_lock(&s->lock);
        if (s->texture_request) {
            create_textures(s);
            s->texture_request = 0;
            s->texture_ready = 1;
            pthread_cond_broadcast(&s->cond);
        }
        if (s->texture_ready && s->current_slot >= 0 && s->texture_data[0]) {
            texture = s->textures[s->current_slot];
            u = (float)s->frame_w / (float)s->texture_w;
            v = (float)s->frame_h / (float)s->texture_h;
            AVRational sar = s->format && s->video_index >= 0 ?
                s->format->streams[s->video_index]->codecpar->sample_aspect_ratio :
                (AVRational){0, 1};
            double ratio = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
            if (s->frame_h > 0) aspect = (float)(s->frame_w * ratio / s->frame_h);
        }
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_unlock(&h->lock);

    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(ctx->program);
    glUniform1i(ctx->sampler, 0);
    glActiveTexture(GL_TEXTURE0);
    /* vita_swap() starts every frame from black, so the letterbox needs no
       extra clear here. */
    if (!texture) { glEnable(GL_BLEND); return 0; }
    float sx = 1, sy = 1, screen = (float)SCREEN_W / SCREEN_H;
    if (aspect > screen) sy = screen / aspect; else sx = aspect / screen;
    /* panscan (as in mpv): 0 letterboxes, 1 scales until the picture covers
       the screen; the overflow is clipped by the viewport. */
    float cover = 1.f / fminf(sx, sy);
    sx *= 1.f + (cover - 1.f) * panscan;
    sy *= 1.f + (cover - 1.f) * panscan;
    float vertices[] = {-sx, sy, 0, 0,   sx, sy, u, 0,   -sx, -sy, 0, v,
                         sx, sy, u, 0,   sx, -sy, u, v,  -sx, -sy, 0, v};
    glBindBuffer(GL_ARRAY_BUFFER, ctx->buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    draw_quad(ctx->buffer, texture);
    glEnable(GL_BLEND);
    return 0;
}

void mpv_render_context_free(mpv_render_context *ctx) {
    if (!ctx) return;
    mpv_handle *h = ctx->mpv;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < h->garbage_count; i++) glDeleteTextures(1, &h->garbage[i]);
    h->garbage_count = 0;
    pthread_mutex_unlock(&h->lock);
    glDeleteBuffers(1, &ctx->buffer);
    glDeleteProgram(ctx->program);
    free(ctx);
}
