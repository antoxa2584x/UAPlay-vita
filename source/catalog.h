#ifndef LAMPA_CATALOG_H
#define LAMPA_CATALOG_H

#define CATALOG_MAX 200
#define CATALOG_LOCAL_MAX 256
#define CATALOG_PAGE_SIZE 20
/* TMDB exposes at most 500 result pages; search expands only while browsing. */
#define CATALOG_SEARCH_MAX (500 * CATALOG_PAGE_SIZE)
#define UAKINO_MAX 40
#define EPISODES_MAX 2048

typedef struct {
    int id;
    int series;
    char title[192];
    char original[192];
    char year[12];
    char overview[1024];
    char poster[256];
    float rating;
    float popularity;
    /* Loaded on demand from the TMDB movie/TV details endpoint. */
    int runtime_minutes;
    int season_count;
    int episode_count;
    int details_state; /* 0 pending, 1 loaded, -1 unavailable */
} Movie;

typedef struct {
    char title[192];
    char url[1024];
} Source;

typedef struct {
    int season;
    int number;
    char title[192];
    char url[1024];
    char voice[96];
} Episode;

/* Keep independent HTTPS connection pools: UI, background TMDB loader and
   source prefetch. Call cleanup only after the loader thread has stopped. */
void catalog_http_init(void);
void catalog_http_cleanup(void);
/* Give the calling thread (the source prefetch worker) its own pool. */
/* Roles for the calling thread: a source search worker (queued
   background, served first) or a foreground job the user waits on (never
   queued). Other non-UI threads count as catalogue background work. */
void catalog_http_bind_source_thread(void);
void catalog_http_bind_foreground_thread(void);
/* Requests made by the calling thread so far, for per-search counts. */
unsigned net_thread_request_count(void);
/* Background network gate (PS Vita). Its Wi-Fi is weak, so catalogue
   paging, source prefetch and poster downloads take turns: one request at
   a time, source prefetch first, none while a video plays. Requests made
   on the UI thread, which the user is waiting for, never wait here. */
void net_background_acquire(int priority);
void net_background_release(int priority);
void net_background_pause(int paused);
/* Wakes every waiter for good; call before joining worker threads. */
void net_background_shutdown(void);
/* Background requests in flight and waiting, for the profiler. */
void net_background_stats(int *active,int *waiting);

/* Request timings for the profiler. Kinds: 'U' UI thread, 'C' catalogue
   worker, 'S' source prefetch, 'P' posters. Times are milliseconds; tls
   and connect are 0 when a kept-alive connection was reused. */
typedef struct {
    unsigned sequence,at;
    char kind,host[39];
    int wait,dns,connect,tls,first_byte,total;
    long status;
    int result;
    unsigned bytes;
    char error[96];   /* curl's error text for a failed request */
} NetSample;
void net_profile_record(char kind,void *curl,const char *url,unsigned wait_ms,
                        int result,unsigned bytes,const char *error);
/* Copies samples newer than sequence after, oldest first; returns count. */
int net_profile_since(unsigned after,NetSample *out,int max);
unsigned net_request_count(char kind);
/* Certificate verification for a curl handle. On the Vita every handle
   shares one store parsed once from the bundled CA list (re-reading the
   PEM file per handle took seconds there); elsewhere it sets CAINFO. */
void net_tls_options(void *curl);
/* Certificates loaded and skipped, and the one-off parse time. */
void net_tls_stats(int *loaded,int *skipped,double *milliseconds);
/* One line on the TLS library: version, RNG state, first CA error. */
void net_tls_info(char *out,int size);

/* NRO includes a public TMDB key; optional SD override under switch/uaplay. */
int catalog_load(const char *category, const char *query, Movie *items, int max,
                 char *error, int error_size);
/* TMDB paginates 20 titles per page; used for asynchronous catalog expansion. */
int catalog_load_page(const char *category, const char *query, int page,
                      Movie *items, int max, char *error, int error_size,
                      int *has_more);
int catalog_load_page_sorted(const char *category, const char *query, int page,
                            int sort_mode, Movie *items, int max,
                            char *error, int error_size, int *has_more);
/* Exclude titles associated with Russia, using TMDB's country metadata. */
int catalog_filter_countries(Movie *items,int count);
/* Fetch runtime (movie) or total seasons/episodes (TV) for details screen. */
int catalog_fetch_details(Movie *movie);

/* Native source search: UAFlix, UASerials, Eneyida, UAFilm/Klon UA, UAKino. */
int uakino_search(const Movie *movie, Source *items, int max,
                  char *error, int error_size);
/* Search a single provider (0=all, 1=UAFlix, 2=UASerials,
   3=Eneyida, 4=UAFilm, 5=UAKino, 6=Mikai, 7=AnimeON). */
int uakino_search_provider(const Movie *movie, Source *items, int max,
                           int provider, char *error, int error_size);
/* Rank candidates by title match (Ukrainian and original titles), not by
   their position on provider search pages. preferred_provider: 0=all,
   1=UAFlix, 2=UASerials. Returns the count after removing clear mismatches. */
int catalog_rank_sources(const Movie *movie, Source *items, int count,
                         int preferred_provider);
int uakino_resolve(const Source *source, char *stream, int size,
                   char *error, int error_size);
/* Subtitle and alternate-dub links announced by the last resolved player.
   Indices remain valid until the next call to uakino_resolve(). */
#define UAKINO_PLAYER_TRACK_MAX 12
int uakino_resolved_subtitle_count(void);
const char *uakino_resolved_subtitle_url(int index);
const char *uakino_resolved_subtitle_label(int index);
int uakino_resolved_audio_count(void);
const char *uakino_resolved_audio_url(int index);
const char *uakino_resolved_audio_label(int index);
/* Compatibility accessor for the first external subtitle. */
const char *uakino_resolved_subtitle(void);
int uakino_episodes(const Source *source, Episode *items, int max,
                    char *error, int error_size);
/* Next chronologically available episode, preferring the current dub when
   the same episode exists in multiple voice variants. -1 if at the end. */
int catalog_next_episode(const Episode *items,int count,int current);
#endif
