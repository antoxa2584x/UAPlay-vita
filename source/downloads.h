#ifndef UAPLAY_DOWNLOADS_H
#define UAPLAY_DOWNLOADS_H

#include <stddef.h>
#include "catalog.h"

/* Offline copies of films and episodes. One worker downloads the queue in
   order through the background network gate, so it pauses while a video
   plays. Direct files are saved as they are; HLS streams are fetched
   segment by segment (AES-128 decrypted on the Vita) into one file. */
#define DOWNLOADS_MAX 64
enum { DOWNLOAD_QUEUED, DOWNLOAD_ACTIVE, DOWNLOAD_DONE, DOWNLOAD_FAILED };

typedef struct {
    unsigned id;
    Movie movie;
    int season,episode;     /* 0 for a film */
    char label[96];         /* episode name, empty for a film */
    char url[2048];         /* resolved stream address */
    char file[160];         /* finished file */
    int state;
    int permille;           /* progress, 0..1000; -1 while the size is unknown */
    unsigned long long bytes;
    char error[96];
} Download;

/* Light copy for drawing the list every frame. */
typedef struct {
    unsigned id;
    char title[192],label[96],error[96];
    int state,permille;
    unsigned long long bytes;
} DownloadInfo;

/* Call after catalog_http_init; shutdown before catalog_http_cleanup. */
void downloads_init(void);
void downloads_shutdown(void);
/* Queue a stream: 1 queued, 0 already in the list, -1 list full. */
int downloads_add(const Movie *movie, int season, int episode, const char *label,
                  const char *url);
int downloads_list(DownloadInfo *out, int max);
int downloads_get(unsigned id, Download *out);
/* Delete the item and its file (a running download is cancelled). */
void downloads_remove(unsigned id);
/* Whether something is queued or downloading. */
int downloads_busy(void);
/* Unfinished items (queued or downloading), with the running one's
   progress (-1 when its size is unknown) and bytes so far. */
int downloads_progress(int *permille, unsigned long long *bytes);
/* Put a failed item back in the queue. */
void downloads_retry(unsigned id);
/* Address the player opens for a finished download. */
void downloads_play_path(const Download *d, char *out, size_t size);
/* Referer some video hosts require (shared with the player). */
const char *stream_referer(const char *url);

#endif
