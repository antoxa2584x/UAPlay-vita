#ifndef UAPLAY_VITA_MPV_CLIENT_H
#define UAPLAY_VITA_MPV_CLIENT_H

/* PS Vita has no libmpv. This header declares the small subset of the
   libmpv client API used by UAPlay; vita/mpv_vita.c implements it on top of
   FFmpeg demuxing, the Vita hardware H.264 decoder and SceAudioOut. */

#include <stddef.h>
#include <stdint.h>

typedef struct mpv_handle mpv_handle;

typedef enum mpv_error {
    MPV_ERROR_SUCCESS = 0,
    MPV_ERROR_NOMEM = -2,
    MPV_ERROR_INVALID_PARAMETER = -4,
    MPV_ERROR_OPTION_NOT_FOUND = -5,
    MPV_ERROR_PROPERTY_NOT_FOUND = -8,
    MPV_ERROR_PROPERTY_FORMAT = -9,
    MPV_ERROR_PROPERTY_UNAVAILABLE = -10,
    MPV_ERROR_COMMAND = -12,
    MPV_ERROR_LOADING_FAILED = -13,
    MPV_ERROR_NOTHING_TO_PLAY = -16,
    MPV_ERROR_UNKNOWN_FORMAT = -17,
    MPV_ERROR_GENERIC = -20
} mpv_error;

typedef enum mpv_format {
    MPV_FORMAT_NONE = 0,
    MPV_FORMAT_STRING = 1,
    MPV_FORMAT_OSD_STRING = 2,
    MPV_FORMAT_FLAG = 3,
    MPV_FORMAT_INT64 = 4,
    MPV_FORMAT_DOUBLE = 5,
    MPV_FORMAT_NODE = 6,
    MPV_FORMAT_NODE_ARRAY = 7,
    MPV_FORMAT_NODE_MAP = 8
} mpv_format;

typedef struct mpv_node {
    union {
        char *string;
        int flag;
        int64_t int64;
        double double_;
        struct mpv_node_list *list;
    } u;
    mpv_format format;
} mpv_node;

typedef struct mpv_node_list {
    int num;
    mpv_node *values;
    char **keys;
} mpv_node_list;

typedef enum mpv_event_id {
    MPV_EVENT_NONE = 0,
    MPV_EVENT_SHUTDOWN = 1,
    MPV_EVENT_LOG_MESSAGE = 2,
    MPV_EVENT_START_FILE = 6,
    MPV_EVENT_END_FILE = 7,
    MPV_EVENT_FILE_LOADED = 8
} mpv_event_id;

typedef enum mpv_end_file_reason {
    MPV_END_FILE_REASON_EOF = 0,
    MPV_END_FILE_REASON_STOP = 2,
    MPV_END_FILE_REASON_QUIT = 3,
    MPV_END_FILE_REASON_ERROR = 4
} mpv_end_file_reason;

typedef struct mpv_event_end_file {
    int reason;
    int error;
} mpv_event_end_file;

typedef struct mpv_event_log_message {
    const char *prefix;
    const char *level;
    const char *text;
    int log_level;
} mpv_event_log_message;

typedef struct mpv_event {
    mpv_event_id event_id;
    int error;
    uint64_t reply_userdata;
    void *data;
} mpv_event;

mpv_handle *mpv_create(void);
int mpv_initialize(mpv_handle *ctx);
void mpv_terminate_destroy(mpv_handle *ctx);
const char *mpv_error_string(int error);
int mpv_set_option_string(mpv_handle *ctx, const char *name, const char *data);
int mpv_set_property_string(mpv_handle *ctx, const char *name, const char *data);
int mpv_get_property(mpv_handle *ctx, const char *name, mpv_format format, void *data);
int mpv_command_async(mpv_handle *ctx, uint64_t reply_userdata, const char **args);
int mpv_request_log_messages(mpv_handle *ctx, const char *min_level);
mpv_event *mpv_wait_event(mpv_handle *ctx, double timeout);
void mpv_free_node_contents(mpv_node *node);

/* Vita extension: the subtitle cue visible at the current position, so the
   application can draw it with its own fonts. Returns 0 when none. */
int mpv_vita_subtitle(mpv_handle *ctx, char *text, size_t size);
/* Vita extension: reserve hardware decoder memory. Call before vglInit,
   which claims all remaining CDRAM. Returns 0 if only software decoding
   will be available. */
int mpv_vita_preinit(void);

#endif
