#include "catalog.h"
#include "platform.h"

#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef UAPLAY_CONSOLE
#include <SDL.h>
#endif
#ifdef __vita__
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

#define TMDB_KEY_PATH UAPLAY_DATA_DIR "/tmdb.key"
#define TMDB_LEGACY_KEY_PATH UAPLAY_DATA_ROOT "/lampa-nx/tmdb.key"
/* The release workflow injects this header from a GitHub Actions secret.
   Keep credentials out of the public source tree. */
#if defined(__has_include)
#  if __has_include("tmdb_build_key.h")
#    include "tmdb_build_key.h"
#  endif
#endif
#ifndef UAPLAY_TMDB_BUILD_KEY
#define UAPLAY_TMDB_BUILD_KEY ""
#endif
#define UAFRESH_WORKER "https://uakino.ofreshofresh.workers.dev"

typedef struct { char *data; size_t length, capacity; } Response;
static void json_field(const char *object,const char *end,const char *key,
                       char *value,size_t size);

#define COUNTRY_CACHE_LIMIT 4096
#define COUNTRY_CACHE_PATH UAPLAY_DATA_DIR "/country_cache.tsv"
#define COUNTRY_CACHE_TMP UAPLAY_DATA_DIR "/country_cache.tmp"
typedef struct { int id,series,allowed; } CountryCacheEntry;
static CountryCacheEntry country_cache[COUNTRY_CACHE_LIMIT];
static int country_cache_count,country_cache_evict;
#ifdef UAPLAY_CONSOLE
static SDL_mutex *country_cache_mutex;
#define COUNTRY_LOCK() do { if(country_cache_mutex) SDL_LockMutex(country_cache_mutex); } while(0)
#define COUNTRY_UNLOCK() do { if(country_cache_mutex) SDL_UnlockMutex(country_cache_mutex); } while(0)
#else
#define COUNTRY_LOCK() ((void)0)
#define COUNTRY_UNLOCK() ((void)0)
#endif

/* Recreating a curl easy handle for every source-page lookup repeatedly
   tears down HTTPS/TLS sockets. Eden's socket emulation can eventually fail
   new connections even though existing poster requests still work. Keep
   one reusable handle per calling thread, never shared concurrently. */
#ifdef UAPLAY_CONSOLE
static SDL_threadID catalog_ui_thread;
/* Each thread gets its own handle on first use. Role 'U' is the UI or a
   foreground job, 'S' a source search worker, 'C' other background work. */
typedef struct { SDL_threadID id; char role; CURL *curl; unsigned requests; } HttpThread;
#define HTTP_THREADS 12
static HttpThread http_threads[HTTP_THREADS];
static int http_thread_count;
static SDL_SpinLock http_threads_lock;
static HttpThread *http_thread(void){
    SDL_threadID self=SDL_ThreadID();
    HttpThread *found=NULL;
    SDL_AtomicLock(&http_threads_lock);
    for(int i=0;i<http_thread_count && !found;i++)
        if(http_threads[i].id==self)found=&http_threads[i];
    if(!found && http_thread_count<HTTP_THREADS){
        found=&http_threads[http_thread_count++];
        *found=(HttpThread){self,self==catalog_ui_thread?'U':'C',NULL,0};
    }
    SDL_AtomicUnlock(&http_threads_lock);
    return found;
}
#endif

#define NET_SAMPLES 128
static NetSample net_samples[NET_SAMPLES];
static unsigned net_sequence,net_counts[5];
#ifdef UAPLAY_CONSOLE
static SDL_SpinLock net_profile_lock;
#define NET_PROFILE_LOCK() SDL_AtomicLock(&net_profile_lock)
#define NET_PROFILE_UNLOCK() SDL_AtomicUnlock(&net_profile_lock)
static unsigned net_ticks(void){return SDL_GetTicks();}
#else
#define NET_PROFILE_LOCK() ((void)0)
#define NET_PROFILE_UNLOCK() ((void)0)
static unsigned net_ticks(void){return 0;}
#endif
static int net_kind_index(char kind){
    return kind=='U'?0:kind=='C'?1:kind=='S'?2:kind=='D'?4:3;
}
void net_profile_record(char kind,void *handle,const char *url,unsigned wait_ms,
                        int result,unsigned bytes,const char *error){
    NetSample sample;
    memset(&sample,0,sizeof(sample));
    sample.at=net_ticks();
    sample.kind=kind;
    sample.wait=(int)wait_ms;
    sample.result=result;
    sample.bytes=bytes;
    if(result && error)snprintf(sample.error,sizeof(sample.error),"%s",error);
    const char *host=url?strstr(url,"://"):NULL;
    host=host?host+3:url?url:"";
    size_t n=strcspn(host,"/:?");
    if(n>=sizeof(sample.host))n=sizeof(sample.host)-1;
    memcpy(sample.host,host,n);
    CURL *curl=handle;
    if(curl){
        curl_off_t dns=0,connect=0,tls=0,first=0,total=0;
        curl_easy_getinfo(curl,CURLINFO_NAMELOOKUP_TIME_T,&dns);
        curl_easy_getinfo(curl,CURLINFO_CONNECT_TIME_T,&connect);
        curl_easy_getinfo(curl,CURLINFO_APPCONNECT_TIME_T,&tls);
        curl_easy_getinfo(curl,CURLINFO_STARTTRANSFER_TIME_T,&first);
        curl_easy_getinfo(curl,CURLINFO_TOTAL_TIME_T,&total);
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&sample.status);
        /* curl reports cumulative microseconds from the request start. */
        sample.dns=(int)(dns/1000);
        sample.connect=connect>dns?(int)((connect-dns)/1000):0;
        sample.tls=tls>connect?(int)((tls-connect)/1000):0;
        sample.first_byte=(int)(first/1000);
        sample.total=(int)(total/1000);
    }
    NET_PROFILE_LOCK();
    sample.sequence=++net_sequence;
    net_samples[sample.sequence%NET_SAMPLES]=sample;
    net_counts[net_kind_index(kind)]++;
    NET_PROFILE_UNLOCK();
}
int net_profile_since(unsigned after,NetSample *out,int max){
    int count=0;
    NET_PROFILE_LOCK();
    unsigned first=net_sequence>NET_SAMPLES?net_sequence-NET_SAMPLES+1:1;
    if(after+1>first)first=after+1;
    if(net_sequence>=first && net_sequence-first+1>(unsigned)max)
        first=net_sequence-(unsigned)max+1;
    for(unsigned i=first;i<=net_sequence;i++)out[count++]=net_samples[i%NET_SAMPLES];
    NET_PROFILE_UNLOCK();
    return count;
}
unsigned net_request_count(char kind){
    NET_PROFILE_LOCK();
    unsigned count=net_counts[net_kind_index(kind)];
    NET_PROFILE_UNLOCK();
    return count;
}

#ifdef __vita__
static SDL_mutex *ca_mutex;
static X509_STORE *ca_store;
static int ca_attempted,ca_loaded,ca_skipped;
static double ca_ms;
static char ca_error[120];
/* Parse the bundle once. A certificate OpenSSL cannot read is skipped
   rather than failing the whole list, as SSL_CTX_load_verify_locations
   would. */
static X509_STORE *ca_store_get(void){
    if(ca_mutex)SDL_LockMutex(ca_mutex);
    if(!ca_attempted){
        ca_attempted=1;
        Uint64 started=SDL_GetPerformanceCounter();
        BIO *bio=BIO_new_file(UAPLAY_CA_BUNDLE,"r");
        X509_STORE *store=bio?X509_STORE_new():NULL;
        while(store){
            X509 *cert=PEM_read_bio_X509(bio,NULL,NULL,NULL);
            if(!cert){
                unsigned long error=ERR_peek_last_error();
                ERR_clear_error();
                if(ERR_GET_REASON(error)==PEM_R_NO_START_LINE || ++ca_skipped>64)break;
                if(!ca_error[0])ERR_error_string_n(error,ca_error,sizeof(ca_error));
                continue;
            }
            if(X509_STORE_add_cert(store,cert))ca_loaded++;
            else{
                unsigned long error=ERR_peek_last_error();
                if(!ca_error[0])ERR_error_string_n(error,ca_error,sizeof(ca_error));
                ca_skipped++;ERR_clear_error();
            }
            X509_free(cert);
        }
        if(bio)BIO_free(bio);
        if(store && !ca_loaded){X509_STORE_free(store);store=NULL;}
        ca_store=store;
        ca_ms=(double)(SDL_GetPerformanceCounter()-started)*1000.0/
              (double)SDL_GetPerformanceFrequency();
    }
    X509_STORE *store=ca_store;
    if(store)X509_STORE_up_ref(store);
    if(ca_mutex)SDL_UnlockMutex(ca_mutex);
    return store;
}
static CURLcode ca_attach(CURL *curl,void *ssl_ctx,void *opaque){
    (void)curl;(void)opaque;
    X509_STORE *store=ca_store_get();
    if(!store)return CURLE_SSL_CACERT_BADFILE;
    SSL_CTX_set_cert_store((SSL_CTX *)ssl_ctx,store);   /* takes our reference */
    return CURLE_OK;
}
void net_tls_options(void *handle){
    CURL *curl=handle;
    curl_easy_setopt(curl,CURLOPT_CAINFO,NULL);
    curl_easy_setopt(curl,CURLOPT_CAPATH,NULL);
    curl_easy_setopt(curl,CURLOPT_CA_CACHE_TIMEOUT,0L);
    curl_easy_setopt(curl,CURLOPT_SSL_CTX_FUNCTION,ca_attach);
}
void net_tls_stats(int *loaded,int *skipped,double *milliseconds){
    if(ca_mutex)SDL_LockMutex(ca_mutex);
    *loaded=ca_loaded;*skipped=ca_skipped;*milliseconds=ca_ms;
    if(ca_mutex)SDL_UnlockMutex(ca_mutex);
}
void net_tls_info(char *out,int size){
    char error[120];
    if(ca_mutex)SDL_LockMutex(ca_mutex);
    snprintf(error,sizeof(error),"%s",ca_error[0]?ca_error:"none");
    if(ca_mutex)SDL_UnlockMutex(ca_mutex);
    snprintf(out,(size_t)size,"tls: %s  rng %s  first CA error: %s",
             OpenSSL_version(OPENSSL_VERSION),RAND_status()==1?"ok":"NOT SEEDED",error);
}

static SDL_mutex *net_mutex;
static SDL_cond *net_cond;
/* Source searches spend seconds waiting for slow sites to answer with a
   few dozen kilobytes, so several may run together; everything else waits
   for them and runs alone. */
#define NET_SOURCE_PARALLEL 3
static int net_busy,net_busy_priority,net_paused,net_quit,net_priority_waiting,net_waiting;
void net_background_acquire(int priority){
    if(!net_mutex)return;
    SDL_LockMutex(net_mutex);
    net_waiting++;
    if(priority)net_priority_waiting++;
    while(!net_quit && (net_paused ||
          (priority ? net_busy>net_busy_priority || net_busy_priority>=NET_SOURCE_PARALLEL
                    : net_busy || net_priority_waiting)))
        SDL_CondWait(net_cond,net_mutex);
    if(priority){net_priority_waiting--;net_busy_priority++;}
    net_waiting--;
    net_busy++;
    SDL_UnlockMutex(net_mutex);
}
void net_background_release(int priority){
    if(!net_mutex)return;
    SDL_LockMutex(net_mutex);
    if(net_busy)net_busy--;
    if(priority && net_busy_priority)net_busy_priority--;
    SDL_CondBroadcast(net_cond);
    SDL_UnlockMutex(net_mutex);
}
void net_background_pause(int paused){
    if(!net_mutex)return;
    SDL_LockMutex(net_mutex);
    if(net_paused!=paused){net_paused=paused;SDL_CondBroadcast(net_cond);}
    SDL_UnlockMutex(net_mutex);
}
void net_background_shutdown(void){
    if(!net_mutex)return;
    SDL_LockMutex(net_mutex);
    net_quit=1;
    SDL_CondBroadcast(net_cond);
    SDL_UnlockMutex(net_mutex);
}
void net_background_stats(int *active,int *waiting){
    *active=*waiting=0;
    if(!net_mutex)return;
    SDL_LockMutex(net_mutex);
    *active=net_busy;*waiting=net_waiting;
    SDL_UnlockMutex(net_mutex);
}
#else
void net_background_acquire(int priority){(void)priority;}
void net_background_release(int priority){(void)priority;}
void net_background_pause(int paused){(void)paused;}
void net_background_shutdown(void){}
void net_background_stats(int *active,int *waiting){*active=*waiting=0;}
void net_tls_options(void *handle){
#ifdef UAPLAY_CA_BUNDLE
    curl_easy_setopt((CURL *)handle,CURLOPT_CAINFO,UAPLAY_CA_BUNDLE);
#else
    (void)handle;
#endif
}
void net_tls_stats(int *loaded,int *skipped,double *milliseconds){
    *loaded=*skipped=0;*milliseconds=0;
}
void net_tls_info(char *out,int size){if(size>0)out[0]=0;}
#endif

#ifdef UAPLAY_CONSOLE
static void country_cache_load(void){
    FILE *file=fopen(COUNTRY_CACHE_PATH,"r");
    if(!file)return;
    int id,series,allowed;
    while(country_cache_count<COUNTRY_CACHE_LIMIT &&
          fscanf(file,"%d|%d|%d",&id,&series,&allowed)==3){
        if(id>0 && (series==0 || series==1) && (allowed==0 || allowed==1))
            country_cache[country_cache_count++]=(CountryCacheEntry){id,series,allowed};
        int c;
        while((c=fgetc(file))!='\n' && c!=EOF){}
    }
    fclose(file);
}
static void country_cache_save(void){
    mkdir(UAPLAY_DATA_DIR,0777);
    FILE *file=fopen(COUNTRY_CACHE_TMP,"w");
    if(!file)return;
    for(int i=0;i<country_cache_count;i++)
        fprintf(file,"%d|%d|%d\n",country_cache[i].id,
                country_cache[i].series,country_cache[i].allowed);
    if(fclose(file)==0)rename(COUNTRY_CACHE_TMP,COUNTRY_CACHE_PATH);
}
void catalog_http_init(void) {
    catalog_ui_thread = SDL_ThreadID();
#ifdef __vita__
    ca_mutex=SDL_CreateMutex();
    net_mutex=SDL_CreateMutex();
    net_cond=SDL_CreateCond();
    if(!net_mutex || !net_cond){
        if(net_mutex)SDL_DestroyMutex(net_mutex);
        if(net_cond)SDL_DestroyCond(net_cond);
        net_mutex=NULL;net_cond=NULL;
    }
#endif
    country_cache_mutex=SDL_CreateMutex();
    country_cache_load();
}
void catalog_http_cleanup(void) {
    country_cache_save();
    /* Only the UI thread's handle: detached workers (source searches,
       foreground jobs) may still be mid-request at exit. */
    HttpThread *self = http_thread();
    if (self && self->curl) { curl_easy_cleanup(self->curl); self->curl = NULL; }
    if(country_cache_mutex){SDL_DestroyMutex(country_cache_mutex);country_cache_mutex=NULL;}
}
void catalog_http_bind_source_thread(void) {
    HttpThread *self = http_thread();
    if (self) self->role = 'S';
}
void catalog_http_bind_foreground_thread(void) {
    HttpThread *self = http_thread();
    if (self) self->role = 'U';
}
unsigned net_thread_request_count(void) {
    HttpThread *self = http_thread();
    return self ? self->requests : 0;
}
#else
void catalog_http_init(void) { }
void catalog_http_cleanup(void) { }
void catalog_http_bind_source_thread(void) { }
void catalog_http_bind_foreground_thread(void) { }
unsigned net_thread_request_count(void) { return 0; }
#endif

static size_t on_data(char *ptr, size_t size, size_t count, void *opaque) {
    Response *r = opaque;
    size_t bytes = size * count;
    if (r->length + bytes > 3 * 1024 * 1024) return 0;
    if (r->length + bytes + 1 > r->capacity) {
        size_t next = (r->length + bytes + 1) * 2;
        char *new_data = realloc(r->data, next);
        if (!new_data) return 0;
        r->data = new_data;
        r->capacity = next;
    }
    memcpy(r->data + r->length, ptr, bytes);
    r->length += bytes;
    r->data[r->length] = 0;
    return bytes;
}

static void request_options(CURL *curl, const char *url, Response *r,
                            long connection_timeout, long timeout) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connection_timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    /* Video hosts frequently reject the non-browser console User-Agent with
       HTTP 403. Keep the old identifier for unrelated catalogue APIs. */
    if(strstr(url,"ashdi.vip/") || strstr(url,"moonanime.net/")){
        curl_easy_setopt(curl,CURLOPT_USERAGENT,
            "Mozilla/5.0 (Linux; Android 12) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36");
        curl_easy_setopt(curl,CURLOPT_REFERER,
            strstr(url,"ashdi.vip/")?"https://ashdi.vip/":"https://moonanime.net/");
    }else curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lampa-nx/0.2 (Nintendo Switch)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
#ifdef UAPLAY_CONSOLE
    /* Console/Eden socket layers can advertise IPv6 without routing it. */
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 4L);
#endif
    net_tls_options(curl);
}

static int transient_network_error(CURLcode result) {
    return result == CURLE_SSL_CONNECT_ERROR ||
           result == CURLE_COULDNT_CONNECT ||
           result == CURLE_COULDNT_RESOLVE_HOST ||
           result == CURLE_OPERATION_TIMEDOUT ||
           result == CURLE_SEND_ERROR ||
           result == CURLE_RECV_ERROR ||
           result == CURLE_GOT_NOTHING;
}

/* Search forms on DLE-powered sites require POST. Keep the same connection,
   timeout, and retry behaviour as GET, including on Switch. */
static char *request_form(const char *url, const char *post_data,
                          const char *referer, char *error, int error_size) {
#ifdef UAPLAY_CONSOLE
    /* Past HTTP_THREADS threads, fall back to a handle per request. */
    HttpThread *thread = http_thread();
    CURL *spare = NULL;
    CURL **slot = thread ? &thread->curl : &spare;
    if (!*slot) *slot = curl_easy_init();
    CURL *curl = *slot;
    char role = thread ? thread->role : 'C';
    if (thread) thread->requests++;
#else
    CURL *curl = curl_easy_init();
#endif
    Response r = {0};
    if (!curl) { snprintf(error, error_size, "curl init failed"); return NULL; }
    struct curl_slist *headers=NULL;
    int ajax=strstr(url,"/engine/ajax/")!=NULL ||
             strstr(url,"/engine/lazydev/")!=NULL;
    if(ajax)headers=curl_slist_append(headers,"X-Requested-With: XMLHttpRequest");
    if(post_data)headers=curl_slist_append(headers,
                              "Content-Type: application/x-www-form-urlencoded; charset=UTF-8");
    request_options(curl, url, &r, post_data?4L:6L, post_data?8L:14L);
    if(headers)curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    if (post_data) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120.0 Safari/537.36");
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        if(referer)curl_easy_setopt(curl,CURLOPT_REFERER,referer);
    }
    char curl_error[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    char kind = 'U';
    unsigned wait_started = net_ticks();
#ifdef UAPLAY_CONSOLE
    int background = role != 'U';
    kind = role;
    if (background) net_background_acquire(role == 'S');
#endif
    unsigned waited = net_ticks() - wait_started;
    CURLcode result = curl_easy_perform(curl);
    if (transient_network_error(result)) {
        /* A failed connection may poison a reused transport. Destroy that
           handle before retrying so DNS, sockets and TLS state are fresh. */
        r.length = 0;
        if (r.data) r.data[0] = 0;
        curl_easy_cleanup(curl);
        curl = curl_easy_init();
#ifdef UAPLAY_CONSOLE
        *slot = curl;
#endif
        if (curl) {
            request_options(curl, url, &r, post_data?3L:4L, post_data?5L:8L);
            curl_error[0] = 0;
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
            if(headers)curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
            if (post_data) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
                curl_easy_setopt(curl, CURLOPT_USERAGENT,
                                 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120.0 Safari/537.36");
                curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
                if(referer)curl_easy_setopt(curl,CURLOPT_REFERER,referer);
            }
            result = curl_easy_perform(curl);
        } else result = CURLE_FAILED_INIT;
    }
#ifdef UAPLAY_CONSOLE
    if (background) net_background_release(role == 'S');
#endif
    net_profile_record(kind, curl, url, waited, (int)result, (unsigned)r.length,
                       curl_error[0] ? curl_error : curl_easy_strerror(result));
    long status = 0;
    if (curl) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
#ifdef UAPLAY_CONSOLE
    if ((result != CURLE_OK || slot == &spare) && curl) {
        curl_easy_cleanup(curl);
        *slot = NULL;
    }
#else
    if (curl) curl_easy_cleanup(curl);
#endif
    curl_slist_free_all(headers);
    if (result != CURLE_OK || status < 200 || status >= 300) {
        if (result == CURLE_OK) snprintf(error, error_size, "Server returned HTTP %ld", status);
        else snprintf(error, error_size, "HTTP %ld: %s", status, curl_easy_strerror(result));
        free(r.data);
        return NULL;
    }
    error[0] = 0;
    return r.data;
}

static char *request(const char *url, char *error, int error_size) {
    return request_form(url,NULL,NULL,error,error_size);
}

static char *encode(const char *value) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    char *encoded = curl_easy_escape(curl, value, 0);
    char *result = encoded ? strdup(encoded) : NULL;
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

/* A compact JSON string reader, safe for escaped quotation marks. */
static void json_field(const char *object, const char *end, const char *key,
                       char *value, size_t size) {
    value[0] = 0;
    char needle[80];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = object;
    while ((p = strstr(p, needle)) && p < end) {
        p += strlen(needle);
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != ':') continue;
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) return;
        int quoted = (*p == '"');
        if (quoted) p++;
        size_t n = 0;
        while (p < end && n + 1 < size) {
            if (quoted && *p == '"') break;
            if (!quoted && (*p == ',' || *p == '}' || isspace((unsigned char)*p))) break;
            if (*p == '\\' && p + 1 < end) {
                p++;
                if (*p == 'n' || *p == 'r') { value[n++] = ' '; p++; continue; }
                if (*p == 'u' && p + 4 < end) { p += 5; continue; }
            }
            value[n++] = *p++;
        }
        value[n] = 0;
        return;
    }
}

static const char *object_end(const char *p) {
    int depth = 0, quoted = 0, escaped = 0;
    for (; *p; p++) {
        if (escaped) { escaped = 0; continue; }
        if (quoted && *p == '\\') { escaped = 1; continue; }
        if (*p == '"') { quoted = !quoted; continue; }
        if (!quoted && *p == '{') depth++;
        if (!quoted && *p == '}' && --depth == 0) return p + 1;
    }
    return NULL;
}

static int already_released(const char *release_date,const char *today){
    return !today[0] ||
           (strlen(release_date)>=10 && strncmp(release_date,today,10)<=0);
}

static char *read_offline_catalog(const char *category) {
    const char *name = !strcmp(category, "series") ? "series" :
                       !strcmp(category, "top") ? "top" : "movies";
    char path[80];
    snprintf(path, sizeof(path), UAPLAY_ROMFS "%s.json", name);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 1 || length > 3 * 1024 * 1024) { fclose(file); return NULL; }
    rewind(file);
    char *result = malloc((size_t)length + 1);
    if (!result) { fclose(file); return NULL; }
    size_t got = fread(result, 1, (size_t)length, file);
    fclose(file);
    result[got] = 0;
    if (got != (size_t)length) { free(result); return NULL; }
    return result;
}

/* Case-insensitive substring search including Ukrainian Cyrillic, without
   depending on a console-wide Unicode locale. */
static unsigned folded_point(const unsigned char **cursor){
    const unsigned char *s=*cursor;
    unsigned cp=*s++;
    if(cp>=0xC0 && cp<=0xDF && s[0]){cp=((cp&31)<<6)|(s[0]&63);s++;}
    else if(cp>=0xE0 && cp<=0xEF && s[0] && s[1]){
        cp=((cp&15)<<12)|((s[0]&63)<<6)|(s[1]&63);s+=2;
    }
    *cursor=s;
    if(cp<128)return (unsigned)tolower((unsigned char)cp);
    if(cp>=0x0410 && cp<=0x042F)return cp+0x20;
    if(cp==0x0401)return 0x0451;
    if(cp==0x0404)return 0x0454;
    if(cp==0x0406)return 0x0456;
    if(cp==0x0407)return 0x0457;
    if(cp==0x0490)return 0x0491;
    return cp;
}
static int contains_title(const char *haystack,const char *needle){
    if(!needle || !*needle)return 1;
    for(const unsigned char *h=(const unsigned char*)haystack;*h;){
        const unsigned char *a=h,*b=(const unsigned char*)needle;
        while(*a && *b && folded_point(&a)==folded_point(&b)){}
        if(!*b)return 1;
        folded_point(&h);
    }
    return 0;
}

/* Movie list responses from TMDB do not contain production countries. Their
   detail endpoint does, while TV list responses already have origin_country.
   Do not infer nationality from original_language: Ukrainian films can be
   Russian-language and Russian productions need not be in Russian. */
static int json_has_ru(const char *object,const char *end,const char *field){
    char needle[64];
    snprintf(needle,sizeof(needle),"\"%s\"",field);
    const char *p=object;
    while((p=strstr(p,needle)) && p<end){
        p+=strlen(needle);
        while(p<end && isspace((unsigned char)*p))p++;
        if(p>=end || *p!=':')continue;
        p++;
        while(p<end && isspace((unsigned char)*p))p++;
        if(p>=end || *p!='[')continue;
        const char *close=strchr(p,']');
        if(!close || close>=end)return -1;
        const char *match=strstr(p,"\"RU\"");
        return match && match<close;
    }
    return -1;
}

static void tmdb_key(char *key,size_t capacity){
    snprintf(key,capacity,"%s",UAPLAY_TMDB_BUILD_KEY);
    FILE *f=fopen(TMDB_KEY_PATH,"r");
    if(!f)f=fopen(TMDB_LEGACY_KEY_PATH,"r");
    if(f){
        char override[256]={0};
        if(fgets(override,sizeof(override),f) && override[0])
            snprintf(key,capacity,"%s",override);
        fclose(f);
    }
    key[strcspn(key,"\r\n \t")]=0;
}

static int country_cached(const Movie *movie,int *allowed){
    int found=0;
    COUNTRY_LOCK();
    for(int i=0;i<country_cache_count;i++){
        if(country_cache[i].id==movie->id &&
           country_cache[i].series==movie->series){
            *allowed=country_cache[i].allowed;
            found=1;
            break;
        }
    }
    COUNTRY_UNLOCK();
    return found;
}
static void country_remember(const Movie *movie,int allowed){
    COUNTRY_LOCK();
    for(int i=0;i<country_cache_count;i++){
        if(country_cache[i].id==movie->id &&
           country_cache[i].series==movie->series){
            country_cache[i].allowed=allowed;
            COUNTRY_UNLOCK();
            return;
        }
    }
    int pos=country_cache_count<COUNTRY_CACHE_LIMIT?
            country_cache_count++:country_cache_evict++%COUNTRY_CACHE_LIMIT;
    country_cache[pos]=(CountryCacheEntry){movie->id,movie->series,allowed};
    COUNTRY_UNLOCK();
}

static int country_allowed(const Movie *movie,const char *object,
                           const char *end,const char *key){
    int allowed=0;
    if(country_cached(movie,&allowed))return allowed;
    int ru=object?json_has_ru(object,end,movie->series?
                              "origin_country":"production_countries"):-1;
    if(ru<0){
        if(!key || !key[0])return 0;
        char url[600],error[160]={0};
        snprintf(url,sizeof(url),"https://api.themoviedb.org/3/%s/%d?api_key=%s",
                 movie->series?"tv":"movie",movie->id,key);
        char *details=request(url,error,sizeof(error));
        if(!details)return 0; /* Unknown production country: fail closed. */
        ru=json_has_ru(details,details+strlen(details),movie->series?
                       "origin_country":"production_countries");
        free(details);
    }
    if(ru<0)return 0; /* Do not display unverified country metadata. */
    allowed=!ru;
    country_remember(movie,allowed);
    return allowed;
}

int catalog_filter_countries(Movie *items,int count){
    char key[256];tmdb_key(key,sizeof(key));
    if(!key[0])return 0;
    int kept=0;
    for(int i=0;i<count;i++)
        if(country_allowed(&items[i],NULL,NULL,key))items[kept++]=items[i];
    return kept;
}

static void catalog_parse_details_json(Movie *movie,const char *json){
    char value[32];
    const char *end=json+strlen(json);
    if(movie->series){
        json_field(json,end,"number_of_seasons",value,sizeof(value));
        movie->season_count=atoi(value);
        json_field(json,end,"number_of_episodes",value,sizeof(value));
        movie->episode_count=atoi(value);
    }else{
        json_field(json,end,"runtime",value,sizeof(value));
        movie->runtime_minutes=atoi(value);
    }
    movie->details_state=1;
}

int catalog_fetch_details(Movie *movie){
    if(!movie || movie->id<=0)return 0;
    char key[256];tmdb_key(key,sizeof(key));
    if(!key[0]){movie->details_state=-1;return 0;}
    char url[600],error[160]={0};
    snprintf(url,sizeof(url),"https://api.themoviedb.org/3/%s/%d?api_key=%s&language=uk-UA",
             movie->series?"tv":"movie",movie->id,key);
    char *json=request(url,error,sizeof(error));
    if(!json){movie->details_state=-1;return 0;}
    catalog_parse_details_json(movie,json);
    free(json);
    return 1;
}

int catalog_load_page_sorted(const char *category, const char *query, int page,
                             int sort_mode, Movie *items, int max,
                             char *error, int error_size, int *has_more) {
    if(has_more)*has_more=0;
    if (page<1 || page>500) return 0;
    char key[256];
    tmdb_key(key,sizeof(key));
    if (!key[0]) {
#ifdef __vita__
        snprintf(error,error_size,"TMDB: додайте ux0:data/uaplay/tmdb.key");
#else
        snprintf(error,error_size,"TMDB: додайте /switch/uaplay/tmdb.key на SD-карту");
#endif
        return 0;
    }

    char url[1800];
    char today[16]={0};
    if(sort_mode==2){
        time_t now=time(NULL);
        struct tm *calendar=localtime(&now);
        if(calendar)strftime(today,sizeof(today),"%Y-%m-%d",calendar);
    }
    int searching=query && *query;
    char *q = searching ? encode(query) : NULL;
    const char *movie_sort=sort_mode==1?"vote_average.desc":
                           sort_mode==2?"primary_release_date.desc":"popularity.desc";
    const char *tv_sort=sort_mode==1?"vote_average.desc":
                        sort_mode==2?"first_air_date.desc":"popularity.desc";
    const char *rating_filter=sort_mode==1?"&vote_count.gte=100":"";
    if (q) {
        snprintf(url, sizeof(url), "https://api.themoviedb.org/3/search/multi?api_key=%s&language=uk-UA&include_adult=false&query=%s", key, q);
    } else {
        if (!strcmp(category,"cartoon_series"))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/tv?api_key=%s&language=uk-UA&sort_by=%s&with_genres=16%s",key,tv_sort,rating_filter);
        else if (!strcmp(category,"anime"))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/tv?api_key=%s&language=uk-UA&sort_by=%s&with_genres=16&with_original_language=ja%s",key,tv_sort,rating_filter);
        else if (!strcmp(category,"cartoons"))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/movie?api_key=%s&language=uk-UA&sort_by=%s&with_genres=16%s",key,movie_sort,rating_filter);
        else if (!strncmp(category,"genre_",6))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/movie?api_key=%s&language=uk-UA&sort_by=%s&with_genres=%s%s",key,movie_sort,category+6,rating_filter);
        else if (!strcmp(category,"films"))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/movie?api_key=%s&language=uk-UA&sort_by=%s%s",key,movie_sort,rating_filter);
        else if (!strcmp(category,"series"))
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/discover/tv?api_key=%s&language=uk-UA&sort_by=%s%s",key,tv_sort,rating_filter);
        else {
            const char *path = !strcmp(category, "series") ? "trending/tv/week" :
                               !strcmp(category, "top") ? "movie/top_rated" : "trending/movie/week";
            snprintf(url, sizeof(url), "https://api.themoviedb.org/3/%s?api_key=%s&language=uk-UA", path, key);
        }
    }
    free(q);
    /* Discover endpoints accept an upper release-date bound. Without it,
       sorting newest-first gives unreleased films and premieres priority. */
    if(sort_mode==2 && today[0] && !searching){
        int tv_category=!strcmp(category,"series") ||
                        !strcmp(category,"cartoon_series") ||
                        !strcmp(category,"anime");
        int movie_category=!strcmp(category,"films") ||
                           !strcmp(category,"cartoons") ||
                           !strncmp(category,"genre_",6);
        if(tv_category || movie_category){
            size_t used=strlen(url);
            snprintf(url+used,sizeof(url)-used,"&%s.lte=%s",
                     tv_category?"first_air_date":"primary_release_date",today);
        }
    }
    /* The TMDB API defaults to page 1. Explicitly request subsequent pages. */
    size_t used=strlen(url);
    if (used+24<sizeof(url))snprintf(url+used,sizeof(url)-used,"&page=%d",page);
    char *json = request(url, error, error_size);
    int offline_search=0;
    int offline_catalog=0;
    if (!json) {
        /* Never repeat the offline page in background pagination. */
        if(page>1)return 0;
        json = read_offline_catalog(category);
        if (json) {
            offline_catalog=1;
            offline_search=searching;
            if(searching)snprintf(error,error_size,
                                  "Офлайн-пошук: лише збережені назви (TMDB недоступний)");
            else snprintf(error,error_size,"Offline catalog (network unavailable): %s",category);
        }else if(searching){
            char reason[160];snprintf(reason,sizeof(reason),"%s",error);
            snprintf(error,error_size,"Пошук TMDB недоступний: %.155s",reason);
            return 0;
        }else return 0;
    } else error[0] = 0;
    char total_pages[32]={0};
    json_field(json,json+strlen(json),"total_pages",total_pages,sizeof(total_pages));
    if(has_more && page<atoi(total_pages) && page<500)*has_more=1;
    const char *results = strstr(json, "\"results\"");
    if (!results || !(results = strchr(results, '['))) { free(json); snprintf(error, error_size, "Bad TMDB response"); return 0; }
    const char *p = results + 1;
    int count = 0;
    while (*p && count < max && count < CATALOG_MAX) {
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }
        const char *end = object_end(p);
        if (!end) break;
        Movie m = {0};
        char number[40], type[40], date[40], rating[40], popularity[40];
        json_field(p, end, "id", number, sizeof(number));
        json_field(p, end, "media_type", type, sizeof(type));
        json_field(p, end, "title", m.title, sizeof(m.title));
        if (!m.title[0]) json_field(p, end, "name", m.title, sizeof(m.title));
        json_field(p, end, "original_title", m.original, sizeof(m.original));
        if (!m.original[0]) json_field(p, end, "original_name", m.original, sizeof(m.original));
        json_field(p, end, "overview", m.overview, sizeof(m.overview));
        json_field(p, end, "poster_path", m.poster, sizeof(m.poster));
        json_field(p, end, "vote_average", rating, sizeof(rating));
        json_field(p, end, "popularity", popularity, sizeof(popularity));
        m.rating=(float)atof(rating);
        m.popularity=(float)atof(popularity);
        json_field(p, end, "release_date", date, sizeof(date));
        if (!date[0]) json_field(p, end, "first_air_date", date, sizeof(date));
        /* Apply this check even to search/offline responses, which cannot use
           discover filters, so future releases never enter date-sorted lists. */
        if(sort_mode==2 && !already_released(date,today)){
            p=end;
            continue;
        }
        memcpy(m.year, date, strlen(date) >= 4 ? 4 : strlen(date));
        m.id = atoi(number);
        m.series = !strcmp(type, "tv") || (!searching &&
                    (!strcmp(category,"series") ||
                     !strcmp(category,"cartoon_series") ||
                     !strcmp(category,"anime")));
        if (m.id && m.title[0] && strcmp(type, "person") &&
            (!offline_search || contains_title(m.title,query) ||
             contains_title(m.original,query)) &&
            country_allowed(&m,p,end,offline_catalog?NULL:key))items[count++] = m;
        p = end;
    }
    free(json);
    if (!count && !offline_search) snprintf(error, error_size, "No titles found");
    return count;
}

int catalog_load_page(const char *category, const char *query, int page,
                      Movie *items, int max, char *error, int error_size,
                      int *has_more) {
    return catalog_load_page_sorted(category,query,page,0,items,max,
                                    error,error_size,has_more);
}

int catalog_load(const char *category, const char *query, Movie *items, int max,
                 char *error, int error_size) {
    return catalog_load_page(category,query,1,items,max,error,error_size,NULL);
}

/* Source pages are requested directly whenever possible, with the existing
   UAFresh proxy retained as a network fallback for embedded players. */
static char *fetch_uafresh(const char *upstream, char *error, int error_size) {
    /* Native homebrew does not have browser CORS restrictions. A direct
       request originates from the Switch user's Ukrainian network, unlike
       the Cloudflare worker; UAFlix episode availability is geo-dependent.
       However, video hosts such as Zetvideo can return an HTTP 200 page
       containing an nginx 404 *body* from some regions. For iframe hosts,
       prefer the UAFresh proxy, then fall back to direct access. */
    int direct_first=!strncmp(upstream,"https://uafix.net/",18) ||
                     !strncmp(upstream,"https://eneyida.tv/",19) ||
                     !strncmp(upstream,"https://klonua.com/",19) ||
                     !strncmp(upstream,"https://uakino.best/",20) ||
                     !strncmp(upstream,"https://uakino.eu/",18) ||
                     !strncmp(upstream,"https://ashdi.vip/",18);
    int video_host=!strncmp(upstream,"https://zetvideo.net/",21) ||
                   !strncmp(upstream,"https://hdvbua.pro/",19) ||
                   !strncmp(upstream,"https://ashdi.vip/",18);
    if(direct_first){
        char *direct=request(upstream,error,error_size);
        if(direct)return direct;
        /* Existing public UAFresh worker remains a normal network fallback. */
    }
    char *encoded = encode(upstream);
    if (!encoded) { snprintf(error, error_size, "URL encoding failed"); return NULL; }
    /* Public proxy only: never read, store, or forward authentication data. */
    size_t length = strlen(encoded) + strlen(UAFRESH_WORKER) + 24;
    char *url = malloc(length);
    if (!url) { free(encoded); return NULL; }
    snprintf(url, length, UAFRESH_WORKER "/proxy?url=%s", encoded);
    free(encoded);
    char *html = request(url, error, error_size);
    free(url);
    /* The public worker is a single point of failure. When it cannot be
       reached, try the Ukrainian provider directly rather than leaving the
       sources page empty. A successful proxy request still takes priority. */
    int provider_page = !strncmp(upstream,"https://uaserials.my/",21) ||
                        !strncmp(upstream,"https://uafix.net/",18) ||
                        !strncmp(upstream,"https://eneyida.tv/",19) ||
                        !strncmp(upstream,"https://klonua.com/",19) ||
                        !strncmp(upstream,"https://uakino.best/",20);
    if(!html && (video_host || (provider_page && !direct_first)))
        return request(upstream,error,error_size);
    return html;
}

static int looks_like_result(const char *url, const char *host, int provider,
                             int want_series) {
    size_t hostlen = strlen(host);
    if (strncmp(url, host, hostlen)) return 0;
    const char *path = url + hostlen;
    if (*path != '/') return 0;
    if (provider == 0) { /* UAFlix: film/series slug, not category or menu */
        const char *end = strchr(path+1, '/');
        if (!end) return 0;
        int group_ok = ((size_t)(end-(path+1)) == 5 && !strncmp(path+1, "films", 5)) ||
                       ((size_t)(end-(path+1)) == 7 && !strncmp(path+1, "serials", 7)) ||
                       ((size_t)(end-(path+1)) == 5 && !strncmp(path+1, "anime", 5));
        if (!group_ok || end[1] == 0) return 0;
        /* A search for a TV series must not list unrelated film cards (and
           vice versa), even when the provider returns both in one search. */
        int films=(size_t)(end-(path+1))==5 && !strncmp(path+1,"films",5);
        if ((want_series && films) || (!want_series && !films))return 0;
        const char *slug_end = strchr(end+1, '/');
        if (!slug_end || slug_end[1] != 0 || slug_end == end+1) return 0;
        for (const char *p=end+1; p<slug_end; p++)
            if (!(isalnum((unsigned char)*p) || *p=='-')) return 0;
        return 1;
    }
    if(provider==2){ /* UAKino DLE: category/.../123-title.html */
        const char *last=strrchr(path,'/');
        if(!last || last==path)return 0;
        last++;
        if(!isdigit((unsigned char)*last))return 0;
        while(isdigit((unsigned char)*last))last++;
        return *last=='-' && strstr(last,".html")!=NULL;
    }
    /* UASerials: /2389-avatar-2009.html */
    const char *p = path+1;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    return *p == '-' && strstr(p, ".html") != NULL;
}

static void html_title(const char *source,char *target,size_t capacity){
    size_t written=0;
    while(*source && written+1<capacity){
        const char *replacement=NULL;
        size_t consumed=0;
        if(!strncmp(source,"&#039;",6)){replacement="'";consumed=6;}
        else if(!strncmp(source,"&apos;",6)){replacement="'";consumed=6;}
        else if(!strncmp(source,"&quot;",6)){replacement="\"";consumed=6;}
        else if(!strncmp(source,"&amp;",5)){replacement="&";consumed=5;}
        else if(!strncmp(source,"&nbsp;",6)){replacement=" ";consumed=6;}
        if(replacement){target[written++]=*replacement;source+=consumed;}
        else target[written++]=*source++;
    }
    target[written]=0;
}

static void search_provider(const char *host, const char *name, int provider,
                            const char *query, int want_series,
                            Source *items, int *count, int limit,
                            char *error, int error_size) {
    char *q = encode(query);
    if (!q) return;
    size_t length = strlen(q)+strlen(host)+92;
    char *search_url = malloc(length);
    if (!search_url) { free(q); return; }
    snprintf(search_url, length, "%s/index.php?do=search&subaction=search&story=%s", host, q);
    free(q);
    char *html = fetch_uafresh(search_url, error, error_size);
    free(search_url);
    if (!html) return;
    const char *p = html;
    /* UAFlix includes its entire category-navigation menu BEFORE the search
       results. Scanning every href mistakenly returned films/action/ and
       serials/school/ as playable titles, filling the list with folders.
       Real search result cards are <a class="sres-wrap ..." href="...">. */
    while (*count < limit &&
           (p = strstr(p,provider==0?"class=\"sres-wrap":"href=")) != NULL) {
        const char *card=p;
        if(provider==0){
            const char *tag_end=strchr(card,'>');
            if(!tag_end)break;
            const char *href=strstr(card,"href=");
            if(!href || href>=tag_end){p=tag_end+1;continue;}
            p=href;
        }
        p += 5;
        if (*p != '"' && *p != '\'') continue;
        char quote = *p++;
        const char *end = strchr(p, quote);
        if (!end) break;
        size_t n = (size_t)(end-p);
        char candidate[1024];
        if (n > 12 && n < sizeof(candidate)) {
            if(*p=='/' && n+strlen(host)+1<sizeof(candidate))
                snprintf(candidate,sizeof(candidate),"%s%.*s",host,(int)n,p);
            else{
                memcpy(candidate,p,n);
                candidate[n]=0;
            }
            if (looks_like_result(candidate, host, provider,want_series)) {
                int exists=0;
                for (int i=0; i<*count; i++)
                    if (!strcmp(items[i].url,candidate)) exists=1;
                if (!exists) {
                    Source *dst = &items[(*count)++];
                    snprintf(dst->url,sizeof(dst->url),"%s",candidate);
                    /* Use the card's real human-readable title, not URL slug. */
                    const char *title_start=NULL,*title_end=NULL;
                    if(provider==0){
                        const char *heading=strstr(end,"<h2>");
                        if(heading && heading-end<800){
                            title_start=heading+4;
                            title_end=strstr(title_start,"</h2>");
                            if(!title_end || title_end-title_start>300)title_start=NULL;
                        }
                    }else if(provider==1){
                        /* UASerials search cards put the human title in
                           .th-title, often followed by the English name.
                           Never label a source with its raw numeric slug. */
                        const char *heading=strstr(end,"class=\"th-title");
                        if(heading && heading-end<900 &&
                           strncmp(heading,"class=\"th-title-oname",21)){
                            const char *opening=strchr(heading,'>');
                            if(opening && opening-heading<140){
                                title_start=opening+1;
                                title_end=strchr(title_start,'<');
                                if(!title_end || title_end-title_start>300)
                                    title_start=NULL;
                            }
                        }
                    }
                    if(title_start){
                        char label[176];size_t len=(size_t)(title_end-title_start);
                        if(len>=sizeof(label))len=sizeof(label)-1;
                        /* Never cut inside a UTF-8 continuation byte. */
                        while(len && ((unsigned char)title_start[len]&0xC0)==0x80)len--;
                        memcpy(label,title_start,len);label[len]=0;
                        char human[176];html_title(label,human,sizeof(human));
                        snprintf(dst->title,sizeof(dst->title),"%s: %s",name,human);
                    }else snprintf(dst->title,sizeof(dst->title),"%s: %s",name,
                                  candidate+strlen(host)+1);
                }
            }
        }
        p=end+1;
    }
    free(html);
}

/* The newer Ukrainian providers don't use the UAFlix/UASerials card layout.
   Parse only their result cards, never the navigation links of a full page. */
static int html_attr(const char *start,const char *end,const char *name,
                     char *out,size_t cap){
    char needle[48];
    snprintf(needle,sizeof(needle),"%s=",name);
    const char *p=start;
    while((p=strstr(p,needle)) && p<end){
        if(p>start && (isalnum((unsigned char)p[-1]) || p[-1]=='-')){p++;continue;}
        p+=strlen(needle);
        if(p>=end || (*p!='"' && *p!='\''))continue;
        char quote=*p++;
        const char *stop=memchr(p,quote,(size_t)(end-p));
        if(!stop)return 0;
        size_t length=(size_t)(stop-p);
        if(!length || length>=cap)return 0;
        memcpy(out,p,length);out[length]=0;return 1;
    }
    return 0;
}

static int card_label(const char *begin,const char *end,const char *class_name,
                      char *label,size_t capacity){
    char needle[96];
    snprintf(needle,sizeof(needle),"%s",class_name);
    const char *p=begin;
    while((p=strstr(p,needle)) && p<end){
        const char *open=memchr(p,'>',(size_t)(end-p));
        if(!open)return 0;
        p=open+1;
        size_t n=0;
        while(p<end && *p && n+1<capacity){
            if(*p=='<'){
                const char *tag=memchr(p,'>',(size_t)(end-p));
                if(!tag)break;
                if(p[1]=='/' && n)break;
                p=tag+1;
                continue;
            }
            label[n++]=*p++;
        }
        while(n && isspace((unsigned char)label[n-1]))n--;
        label[n]=0;
        if(n){
            char decoded[192];
            html_title(label,decoded,sizeof(decoded));
            snprintf(label,capacity,"%s",decoded);
            return 1;
        }
    }
    return 0;
}

static int source_page_url(const char *host,const char *href,int provider,
                           int want_series,char *out,size_t size){
    if(!href || !*href)return 0;
    if(strlen(href)+strlen(host)+10>=size)return 0;
    if(!strncmp(href,"https://",8)){
        snprintf(out,size,"%s",href);
    }else if(!strncmp(href,"//",2)){
        snprintf(out,size,"https:%s",href);
    }else if(*href=='/'){
        snprintf(out,size,"%s%s",host,href);
    }else return 0;
    if(strncmp(out,host,strlen(host)))return 0;
    const char *path=out+strlen(host);
    const char *last=strrchr(path,'/');
    if(!last || !last[1] || !isdigit((unsigned char)last[1]))return 0;
    const char *number=last+1;
    while(isdigit((unsigned char)*number))number++;
    if(*number!='-' || !strstr(number,".html"))return 0;
    if(provider==3){ /* Klon UA catalog separates films and serials by path. */
        int serial=!!(strstr(path,"/serialy/") || strstr(path,"/multserialy/"));
        int film=!!(strstr(path,"/filmy/") || strstr(path,"/multfilmy/"));
        if((want_series && film) || (!want_series && serial))return 0;
    }
    if(provider==4){ /* UAKino separates TV by URL categories. */
        int serial=!!(strstr(path,"/seriesss/") || strstr(path,"/anime-series/"));
        if(!want_series && serial)return 0;
    }
    return 1;
}

static void add_search_card(const char *host,const char *name,int provider,
                            int want_series,const char *href,const char *label,
                            Source *items,int *count,int limit){
    if(*count>=limit || !label || !label[0])return;
    char url[1024];
    if(!source_page_url(host,href,provider,want_series,url,sizeof(url)))return;
    for(int i=0;i<*count;i++)if(!strcmp(items[i].url,url))return;
    Source *dst=&items[(*count)++];
    snprintf(dst->url,sizeof(dst->url),"%s",url);
    char clipped[176];
    size_t n=strlen(label);
    if(n>=sizeof(clipped))n=sizeof(clipped)-1;
    while(n && ((unsigned char)label[n]&0xc0)==0x80)n--;
    memcpy(clipped,label,n);clipped[n]=0;
    snprintf(dst->title,sizeof(dst->title),"%s: %s",name,clipped);
}

static void parse_post_cards(const char *html,const char *host,const char *name,
                             int provider,int want_series,Source *items,
                             int *count,int limit){
    const char *p=html;
    while(*count<limit && *p){
        const char *card=NULL,*end=NULL;
        if(provider==2){ /* Eneyida uses <article> + .short_title. */
            card=strstr(p,"<article");
            if(!card)break;
            end=strstr(card,"</article>");
            if(!end)break;
            end+=10;
        }else if(provider==3){ /* Klon UA uses two variants of short cards. */
            const char *slide=strstr(p,"short-news__slide-item");
            const char *small=strstr(p,"short-news__small");
            card=slide && (!small || slide<small)?slide:small;
            if(!card)break;
            /* Limit each candidate to the beginning of the next result card.
               Avoid assigning a nearby recommendation title to this URL. */
            end=card+strlen(card);
            while(*end && end-card<3500)end++;
        }else{ /* UAKino AJAX returns <a class=search-result-link ...>. */
            card=strstr(p,"search-result-link");
            if(!card)break;
            end=card+strlen(card);
            while(*end && end-card<2000 && strncmp(end,"</a>",4))end++;
        }
        const char *a=card;
        const char *href_end=NULL;
        char href[1024]={0},title[192]={0};
        if(provider==4){
            /* The marker is inside the opening <a>, so include that tag. */
            while(a>html && *a!='<')a--;
            if(strncmp(a,"<a",2))a=card;
        }
        while((a=strstr(a,"<a")) && a<end){
            const char *tag_end=memchr(a,'>',(size_t)(end-a));
            if(!tag_end)break;
            if(html_attr(a,tag_end,"href",href,sizeof(href)) &&
               source_page_url(host,href,provider,want_series,
                               (char[1024]){0},1024)){
                href_end=tag_end;break;
            }
            a=tag_end+1;
        }
        if(href_end){
            const char *title_class=provider==2?"short_title":
                                    provider==3?"card-link__text":"search-result-title";
            if(!card_label(card,end,title_class,title,sizeof(title))){
                if(provider==3)card_label(card,end,"card-title__block",title,sizeof(title));
            }
            add_search_card(host,name,provider,want_series,href,title,
                            items,count,limit);
        }
        p=provider==2?end:(href_end?href_end+1:card+strlen(card));
    }
}

/* DLE search is POST on Eneyida and Klon; a GET URL alone usually returns
   the home page. UAKino uses a separate JSON-over-AJAX endpoint. */
static void search_uakino_mirror(const char *query,int series,Source *items,
                                int *count,int limit,char *error,int error_size){
    const char *host="https://uakino.eu";
    char *encoded=encode(query);
    if(!encoded)return;
    size_t size=strlen(encoded)+128;
    char *url=malloc(size);
    if(!url){free(encoded);return;}
    snprintf(url,size,"%s/index.php?do=search&subaction=search&story=%s",host,encoded);
    free(encoded);
    char *html=request(url,error,error_size);
    free(url);
    if(!html)return;
    const char *p=html;
    while(*count<limit && (p=strstr(p,"class=\"movie-item short-item\""))){
        const char *end=strstr(p,"class=\"movie-item short-item\"");
        end=end?end+strlen("class=\"movie-item short-item\""):NULL;
        const char *next=end?strstr(end,"class=\"movie-item short-item\""):NULL;
        if(!next)next=html+strlen(html);
        const char *title_tag=strstr(p,"class=\"movie-title\"");
        if(title_tag && title_tag<next){
            const char *a=title_tag;
            while(a>p && *a!='<')a--;
            const char *tag_end=strchr(title_tag,'>');
            char href[1024]={0},title[192]={0};
            if(tag_end && tag_end<next &&
               html_attr(a,tag_end,"href",href,sizeof(href)) &&
               card_label(a,next,"movie-title",title,sizeof(title)))
                add_search_card(host,"UAKino",4,series,href,title,items,count,limit);
        }
        p=next;
    }
    free(html);
}

static char *json_string_field_alloc(const char *json,const char *key){
    char needle[64];
    snprintf(needle,sizeof(needle),"\"%s\"",key);
    const char *p=strstr(json,needle);
    if(!p)return NULL;
    p=strchr(p+strlen(needle),':');
    if(!p)return NULL;
    do {p++;} while(isspace((unsigned char)*p));
    if(*p!='"')return NULL;
    p++;
    size_t cap=strlen(p)+1;
    char *out=malloc(cap);
    if(!out)return NULL;
    size_t n=0;
    while(*p && *p!='"' && n+1<cap){
        if(*p!='\\'){out[n++]=*p++;continue;}
        p++;
        if(!*p)break;
        if(*p=='u'){
            p++;unsigned int cp=0;
            for(int i=0;i<4;i++){
                int digit=(unsigned char)*p;
                if(!isxdigit(digit)){cp=0;break;}
                cp=(cp<<4)|(digit>='0' && digit<='9'?digit-'0':
                   ((digit|32)-'a'+10));
                p++;
            }
            if(cp<128)out[n++]=(char)cp;
            else if(cp<2048 && n+2<cap){out[n++]=(char)(0xc0|(cp>>6));
                             out[n++]=(char)(0x80|(cp&63));}
            else if(n+3<cap){out[n++]=(char)(0xe0|(cp>>12));
                  out[n++]=(char)(0x80|((cp>>6)&63));
                  out[n++]=(char)(0x80|(cp&63));}
            continue;
        }
        char c=*p++;
        out[n++]=c=='n'||c=='r'||c=='t'?' ':c;
    }
    out[n]=0;
    return out;
}

static void search_new_provider(const char *host,const char *name,int provider,
                                const char *query,int series,Source *items,
                                int *count,int limit,char *error,int error_size){
    if(!query || !*query || *count>=limit)return;
    /* The original .best AJAX endpoint now serves a Cloudflare challenge.
       The accessible UAKino mirror exposes full GET search result cards. */
    if(provider==4){
        search_uakino_mirror(query,series,items,count,limit,error,error_size);
        return;
    }
    char *encoded=encode(query);
    if(!encoded)return;
    char url[256],referer[256],*body;
    snprintf(referer,sizeof(referer),"%s/",host);
    if(provider==4){
        snprintf(url,sizeof(url),"%s/engine/lazydev/dle_search/ajax.php",host);
        body=malloc(strlen(encoded)+40);
        if(body)sprintf(body,"story=%s&thisUrl=%%2Fua%%2F",encoded);
    }else{
        snprintf(url,sizeof(url),"%s/index.php?do=search",host);
        body=malloc(strlen(encoded)+100);
        if(body)snprintf(body,strlen(encoded)+100,
                         "do=search&subaction=search&search_start=0&full_search=0&result_from=1&story=%s",
                         encoded);
    }
    free(encoded);
    if(!body)return;
    char *html=request_form(url,body,referer,error,error_size);
    free(body);
    if(!html)return;
    if(provider==4){
        char *content=json_string_field_alloc(html,"content");
        free(html);
        html=content;
    }
    if(html){
        parse_post_cards(html,host,name,provider,series,items,count,limit);
        free(html);
    }
}

/* Normalize Latin and Cyrillic titles without depending on a UTF-8 locale
   (the Switch C locale does not perform Ukrainian case folding). Preserve
   spaces as word boundaries so "Home" cannot match "Homeboy". */
static void normalize_title(const char *s, size_t length, char *out, size_t cap) {
    size_t i=0,n=0;
    int space=0;
    while(i<length && n+4<cap){
        unsigned int cp=(unsigned char)s[i++];
        if(cp>=0xc2 && cp<=0xdf && i<length &&
           ((unsigned char)s[i]&0xc0)==0x80)
            cp=((cp&0x1f)<<6)|((unsigned char)s[i++]&0x3f);
        else if(cp>=0xe0 && cp<=0xef && i+1<length &&
                ((unsigned char)s[i]&0xc0)==0x80 &&
                ((unsigned char)s[i+1]&0xc0)==0x80){
            cp=((cp&0x0f)<<12)|(((unsigned char)s[i]&0x3f)<<6)|
               ((unsigned char)s[i+1]&0x3f);
            i+=2;
        }
        if(cp>='A' && cp<='Z')cp+=32;
        if(cp>=0x410 && cp<=0x42f)cp+=32;
        else if(cp>=0x400 && cp<=0x40f)cp+=0x50;
        else if(cp==0x490)cp=0x491; /* Ґ -> ґ */
        int letter=(cp>='a' && cp<='z') || (cp>='0' && cp<='9') ||
                   (cp>=0x430 && cp<=0x52f) ||
                   (cp>=0xc0 && cp<=0x24f);
        if(!letter){space=n>0;continue;}
        if(space && n+1<cap)out[n++]=' ';
        space=0;
        if(cp<0x80)out[n++]=(char)cp;
        else if(cp<0x800){out[n++]=(char)(0xc0|(cp>>6));
            out[n++]=(char)(0x80|(cp&0x3f));}
        else {out[n++]=(char)(0xe0|(cp>>12));
            out[n++]=(char)(0x80|((cp>>6)&0x3f));
            out[n++]=(char)(0x80|(cp&0x3f));}
    }
    out[n]=0;
}

static int whole_phrase(const char *haystack, const char *phrase) {
    size_t n=strlen(phrase);
    if(!n)return 0;
    const char *p=haystack;
    while((p=strstr(p,phrase))){
        if((p==haystack || p[-1]==' ') &&
           (p[n]==0 || p[n]==' '))return 1;
        p++;
    }
    return 0;
}

static int title_variant_score(const char *variant, size_t length,
                               const char *localized, const char *original) {
    char candidate[512];
    normalize_title(variant,length,candidate,sizeof(candidate));
    if(!candidate[0])return 0;
    if(localized[0] && !strcmp(candidate,localized))return 120;
    if(original[0] && !strcmp(candidate,original))return 110;
    if(localized[0] && whole_phrase(candidate,localized))return 78;
    if(original[0] && whole_phrase(candidate,original))return 68;
    /* Fuzzy matches are only a last resort; short names like Home should
       never match Homeboy solely because they share a prefix. */
    if(strlen(localized)>=6 && strstr(candidate,localized))return 32;
    if(strlen(original)>=6 && strstr(candidate,original))return 28;
    return 0;
}

static int source_match_score(const Movie *movie, const Source *source) {
    char localized[512],original[512];
    normalize_title(movie->title,strlen(movie->title),localized,sizeof(localized));
    normalize_title(movie->original,strlen(movie->original),original,sizeof(original));
    const char *name=strstr(source->title,": ");
    name=name?name+2:source->title;
    int best=0;
    const char *part=name;
    for(;;){
        const char *slash=strchr(part,'/');
        size_t length=slash?(size_t)(slash-part):strlen(part);
        int score=title_variant_score(part,length,localized,original);
        if(score>best)best=score;
        if(!slash)break;
        part=slash+1;
    }
    return best;
}

int catalog_rank_sources(const Movie *movie, Source *items, int count,
                         int preferred_provider) {
    if(!movie || !items || count<=1)return count;
    int *scores=malloc((size_t)count*sizeof(int));
    if(!scores)return count;
    for(int i=0;i<count;i++)scores[i]=source_match_score(movie,&items[i]);
    /* Stable insertion sort: source preference only breaks equal-score ties.
       An exact title always outranks an unrelated preferred provider. */
    const char *preferred=preferred_provider==1?"UAFlix:":
                          preferred_provider==2?"UASerials:":NULL;
    for(int i=1;i<count;i++){
        Source saved=items[i];
        int value=scores[i],j=i;
        int saved_preferred=preferred && strstr(saved.title,preferred)==saved.title;
        while(j>0 && (scores[j-1]<value ||
               (scores[j-1]==value && saved_preferred &&
                strstr(items[j-1].title,preferred)!=items[j-1].title))){
            items[j]=items[j-1];scores[j]=scores[j-1];j--;
        }
        items[j]=saved;scores[j]=value;
    }
    /* When a provider offers an exact film title, don't bury it under
       pages of unrelated keyword hits. For ambiguous/alternative titles,
       keep the full fallback list so discoverability is not lost. */
    if(scores[0]>=110){
        while(count>0 && scores[count-1]<100)count--;
    }else if(scores[0]>=68){
        while(count>0 && scores[count-1]==0)count--;
    }
    free(scores);
    return count;
}

/* Mikai provides a documented, keyless anime catalogue. Keep its results
   separate from HTML scraping, and use stable numeric Mikai identifiers. */
static void search_mikai(const char *query,Source *items,int *count,int max){
    if(!query || !*query || *count>=max)return;
    char *encoded=encode(query);
    if(!encoded)return;
    char url[1024],error[160]={0};
    snprintf(url,sizeof(url),
        "https://api.mikai.me/public/v1/anime?search=%s&limit=12",encoded);
    free(encoded);
    char *json=request(url,error,sizeof(error));
    if(!json)return;
    const char *result=strstr(json,"\"result\"");
    const char *p=result?strchr(result,'['):NULL;
    if(p) p++;
    while(p && *p && *p!=']' && *count<max){
        while(*p && *p!='{' && *p!=']')p++;
        if(*p!='{')break;
        const char *end=object_end(p);
        if(!end)break;
        char id[32]={0},title[192]={0};
        json_field(p,end,"mikai",id,sizeof(id));
        json_field(p,end,"ua",title,sizeof(title));
        if(!title[0])json_field(p,end,"english",title,sizeof(title));
        int numeric=atoi(id);
        if(numeric>0 && title[0]){
            Source *dst=&items[(*count)++];
            snprintf(dst->title,sizeof(dst->title),"Mikai: %s",title);
            snprintf(dst->url,sizeof(dst->url),
                "https://api.mikai.me/public/v1/anime/%d/player",numeric);
        }
        p=end;
    }
    free(json);
}

static void search_animeon(const char *query,Source *items,int *count,int max){
    if(!query || !*query || *count>=max)return;
    char *encoded=encode(query);
    if(!encoded)return;
    char url[1024],error[160]={0};
    snprintf(url,sizeof(url),"https://animeon.club/api/anime?search=%s&take=12",encoded);
    free(encoded);
    char *json=request(url,error,sizeof(error));
    if(!json)return;
    const char *p=strstr(json,"\"results\"");
    p=p?strchr(p,'['):NULL;
    if(p)p++;
    char normalized_query[512];
    normalize_title(query,strlen(query),normalized_query,sizeof(normalized_query));
    /* The API returns OVAs, films and unrelated keyword matches together.
       Prefer exact anime titles when any exist in the response. */
    int exact_found=0;
    const char *scan=p;
    const char *array_end=p?strchr(p,']'):NULL;
    while(scan && array_end && scan<array_end){
        const char *obj=strchr(scan,'{');
        if(!obj || obj>=array_end)break;
        const char *end=object_end(obj);
        if(!end)break;
        char title[192]={0},normalized[512]={0};
        json_field(obj,end,"titleUa",title,sizeof(title));
        if(!title[0])json_field(obj,end,"titleEn",title,sizeof(title));
        normalize_title(title,strlen(title),normalized,sizeof(normalized));
        if(!strcmp(normalized,normalized_query))exact_found=1;
        scan=end;
    }
    while(p && *p && *p!=']' && *count<max){
        while(*p && *p!='{' && *p!=']')p++;
        if(*p!='{')break;
        const char *end=object_end(p);
        if(!end)break;
        char id[32]={0},title[192]={0};
        json_field(p,end,"id",id,sizeof(id));
        json_field(p,end,"titleUa",title,sizeof(title));
        if(!title[0])json_field(p,end,"titleEn",title,sizeof(title));
        int numeric=atoi(id);
        char normalized[512]={0};
        normalize_title(title,strlen(title),normalized,sizeof(normalized));
        if(numeric>0 && title[0] && (!exact_found ||
           !strcmp(normalized,normalized_query))){
            Source *dst=&items[(*count)++];
            snprintf(dst->title,sizeof(dst->title),"AnimeON: %s",title);
            snprintf(dst->url,sizeof(dst->url),
                     "https://animeon.club/api/player/%d",numeric);
        }
        p=end;
    }
    free(json);
}

int uakino_search_provider(const Movie *movie, Source *items, int max,
                           int selected, char *error, int error_size) {
    int count=0;
    /* Each provider gets room for its own cards. Searching the original title
       is a fallback only if the localized search found no cards there. */
    const char *localized=movie->title;
    const char *original=movie->original;
    error[0]=0;
    int provider_limit=selected? (max>=4?max/4:max) : (max>=7?max/7:max);
    if(selected==0 || selected==1){
    search_provider("https://uafix.net", "UAFlix", 0, localized,movie->series,
                    items, &count, provider_limit, error, error_size);
    if(count==0 && !error[0] && original[0] && strcmp(original,localized))
        search_provider("https://uafix.net", "UAFlix", 0, original,movie->series,
                        items, &count, provider_limit, error, error_size);
    }
    int before=count;
    if(selected==0 || selected==2){
    search_provider("https://uaserials.my", "UASerials", 1, localized,movie->series,
                    items, &count, provider_limit*2, error, error_size);
    if(count==before && !error[0] && original[0] && strcmp(original,localized))
        search_provider("https://uaserials.my", "UASerials", 1, original,movie->series,
                        items, &count, provider_limit*2, error, error_size);
    }
    before=count;
    if(selected==0 || selected==3){
    search_new_provider("https://eneyida.tv","Eneyida",2,localized,movie->series,
                        items,&count,provider_limit*3,error,error_size);
    if(count==before && !error[0] && original[0] && strcmp(original,localized))
        search_new_provider("https://eneyida.tv","Eneyida",2,original,movie->series,
                            items,&count,provider_limit*3,error,error_size);
    }
    before=count;
    if(selected==0 || selected==4){
    /* The old UAFilm.tv address now redirects to Klon UA. */
    search_new_provider("https://klonua.com","UAFilm",3,localized,movie->series,
                        items,&count,provider_limit*4,error,error_size);
    if(count==before && !error[0] && original[0] && strcmp(original,localized))
        search_new_provider("https://klonua.com","UAFilm",3,original,movie->series,
                            items,&count,provider_limit*4,error,error_size);
    }
    before=count;
    if(selected==0 || selected==5){
    int uakino_limit=movie->series && max>2*provider_limit?
                     max-2*provider_limit:max;
    search_new_provider("https://uakino.best","UAKino",4,localized,movie->series,
                        items,&count,uakino_limit,error,error_size);
    if(count==before && !error[0] && original[0] && strcmp(original,localized))
        search_new_provider("https://uakino.best","UAKino",4,original,movie->series,
                            items,&count,uakino_limit,error,error_size);
    }
    if(movie->series && count<max && (selected==0 || selected==6)){
        int before_mikai=count;
        int mikai_limit=max>provider_limit?max-provider_limit:max;
        search_mikai(localized,items,&count,mikai_limit);
        if(count==before_mikai && original[0] && strcmp(original,localized))
            search_mikai(original,items,&count,mikai_limit);
    }
    if(movie->series && count<max && (selected==0 || selected==7)){
        int before_animeon=count;
        search_animeon(localized,items,&count,max);
        if(count==before_animeon && original[0] && strcmp(original,localized))
            search_animeon(original,items,&count,max);
    }
    if (count) error[0]=0;
    else if (!error[0]) snprintf(error,error_size,"UAFresh: no matching pages");
    return count;
}

int uakino_search(const Movie *movie, Source *items, int max,
                  char *error, int error_size){
    return uakino_search_provider(movie,items,max,0,error,error_size);
}

typedef struct {
    char label[96];
    char url[1024];
} ResolvedPlayerLink;
static ResolvedPlayerLink resolved_audio[UAKINO_PLAYER_TRACK_MAX];
static ResolvedPlayerLink resolved_subtitles[UAKINO_PLAYER_TRACK_MAX];
static int resolved_audio_count, resolved_subtitle_count;

int uakino_resolved_audio_count(void) { return resolved_audio_count; }
int uakino_resolved_subtitle_count(void) { return resolved_subtitle_count; }
const char *uakino_resolved_audio_url(int i) {
    return i>=0 && i<resolved_audio_count?resolved_audio[i].url:"";
}
const char *uakino_resolved_audio_label(int i) {
    return i>=0 && i<resolved_audio_count?resolved_audio[i].label:"";
}
const char *uakino_resolved_subtitle_url(int i) {
    return i>=0 && i<resolved_subtitle_count?resolved_subtitles[i].url:"";
}
const char *uakino_resolved_subtitle_label(int i) {
    return i>=0 && i<resolved_subtitle_count?resolved_subtitles[i].label:"";
}
const char *uakino_resolved_subtitle(void) {
    return uakino_resolved_subtitle_url(0);
}

/* PlayerJS accepts 'subtitle' and 'file' as quoted JS/JSON properties.
   Read only the value of that property, rather than taking the first URL
   anywhere after the word (which used to mistake a video URL for a subtitle).
   The caller can continue searching from the returned cursor. */
static const char *player_field(const char *from,const char *key,
                                char *value,size_t capacity) {
    size_t keylen=strlen(key);
    const char *p=from;
    while((p=strstr(p,key))!=NULL){
        const char *begin=p, *q=p+keylen;
        if((begin>from && (isalnum((unsigned char)begin[-1]) ||
                           begin[-1]=='_' || begin[-1]=='-')) ||
           (isalnum((unsigned char)*q) || *q=='_' || *q=='-')){p=q;continue;}
        if(*q=='"' || *q=='\'')q++;
        while(isspace((unsigned char)*q))q++;
        if(*q!=':' && *q!='='){p+=keylen;continue;}
        q++;
        while(isspace((unsigned char)*q))q++;
        if(*q!='"' && *q!='\''){p+=keylen;continue;}
        char quote=*q++;
        size_t len=0;
        while(*q && *q!=quote && len+1<capacity){
            if(*q=='\\' && q[1]){
                q++;
                if(*q=='u' && !strncmp(q+1,"0026",4)){
                    value[len++]='&';q+=5;continue;
                }
                if(*q=='n'||*q=='r'||*q=='t'){
                    value[len++]=' ';q++;continue;
                }
            }
            value[len++]=*q++;
        }
        value[len]=0;
        return *q==quote?q+1:q;
    }
    return NULL;
}
static int is_video_link(const char *url) {
    return (strstr(url,".m3u8") || strstr(url,".mp4") ||
            strstr(url,".mpd")) && !strstr(url,"trailer");
}
static void add_resolved_link(ResolvedPlayerLink *list,int *count,
                              const char *label,const char *url) {
    if(!url[0] || *count>=UAKINO_PLAYER_TRACK_MAX)return;
    for(int i=0;i<*count;i++)
        if(!strcmp(list[i].url,url))return;
    ResolvedPlayerLink *dst=&list[(*count)++];
    snprintf(dst->label,sizeof(dst->label),"%s",
             label && label[0]?label:"Основна");
    snprintf(dst->url,sizeof(dst->url),"%s",url);
}
static void extract_subtitle_links(const char *field) {
    const char *p=field;
    while(*p && resolved_subtitle_count<UAKINO_PLAYER_TRACK_MAX){
        while(*p==',' || *p==';' || *p==')' ||
              isspace((unsigned char)*p))p++;
        char label[96]="Субтитри";
        if(*p=='['){
            const char *end=strchr(++p,']');
            if(!end)break;
            snprintf(label,sizeof(label),"%.*s",(int)(end-p),p);
            p=end+1;
        }
        const char *start=p;
        if(!strncmp(start,"//",2)){}
        else if(strncmp(start,"https://",8) && strncmp(start,"http://",7)){
            while(*p && *p!=',' && *p!=';' && *p!='[' && *p!=')')p++;
            continue;
        }
        while(*p && *p!=',' && *p!=';' && *p!='[' && *p!=')' &&
              *p!='"' && *p!='\'' && !isspace((unsigned char)*p))p++;
        size_t n=(size_t)(p-start);
        char url[1024];
        if(n>8 && n<sizeof(url)-7){
            if(!strncmp(start,"//",2))
                snprintf(url,sizeof(url),"https:%.*s",(int)n,start);
            else snprintf(url,sizeof(url),"%.*s",(int)n,start);
            add_resolved_link(resolved_subtitles,&resolved_subtitle_count,label,url);
        }
    }
}
static void extract_subtitle(const char *html) {
    char value[8192];
    const char *p=html;
    for(int i=0;i<20 && (p=player_field(p,"subtitle",value,sizeof(value)));i++)
        extract_subtitle_links(value);
}
static int extract_playerjs_audio(const char *html,char *stream,int size) {
    char field[8192];
    const char *search=html;
    for(int attempt=0;attempt<24 &&
        (search=player_field(search,"file",field,sizeof(field)));attempt++){
        ResolvedPlayerLink choices[UAKINO_PLAYER_TRACK_MAX]={0};
        int count=0;
        const char *p=field;
        while(*p && count<UAKINO_PLAYER_TRACK_MAX){
            while(*p==',' || *p==';' || isspace((unsigned char)*p))p++;
            if(*p=='['){
                const char *end=strchr(p,']');
                if(!end)break;
                p=end+1; /* Quality tag, not an audio language. */
            }
            char label[96]="Основна";
            if(*p=='{'){
                const char *end=strchr(++p,'}');
                if(!end)break;
                snprintf(label,sizeof(label),"%.*s",(int)(end-p),p);
                p=end+1;
            }
            const char *start=p;
            if(strncmp(start,"https://",8) && strncmp(start,"http://",7) &&
               strncmp(start,"//",2)){
                while(*p && *p!=',' && *p!=';')p++;
                continue;
            }
            while(*p && *p!=',' && *p!=';' && *p!='(' && *p!='{' &&
                  *p!='"' && *p!='\'' && !isspace((unsigned char)*p))p++;
            size_t n=(size_t)(p-start);
            char url[1024]={0};
            if(n>12 && n<sizeof(url)-7){
                if(!strncmp(start,"//",2))
                    snprintf(url,sizeof(url),"https:%.*s",(int)n,start);
                else snprintf(url,sizeof(url),"%.*s",(int)n,start);
                if(is_video_link(url)){
                    int exists=0;
                    for(int i=0;i<count;i++)
                        if(!strcmp(choices[i].label,label))exists=1;
                    if(!exists)add_resolved_link(choices,&count,label,url);
                }
            }
            /* PlayerJS also supports (subtitle:[Uk]URL[En]URL) on a
               specific media URL. Collect the linked tracks as well. */
            if(*p=='(' && !strncmp(p,"(subtitle:",10)){
                const char *end=strchr(p+10,')');
                if(end && (size_t)(end-(p+10))<sizeof(field)){
                    char subtitles[8192];
                    snprintf(subtitles,sizeof(subtitles),"%.*s",
                             (int)(end-(p+10)),p+10);
                    extract_subtitle_links(subtitles);
                    p=end+1;
                }
            }
        }
        if(count){
            resolved_audio_count=count;
            memcpy(resolved_audio,choices,sizeof(choices));
            snprintf(stream,(size_t)size,"%s",resolved_audio[0].url);
            return 1;
        }
    }
    return 0;
}

static int extract_media(const char *html, char *stream, int size) {
    extract_subtitle(html);
    if(extract_playerjs_audio(html,stream,size))return 1;
    const char *p=html;
    while ((p=strstr(p,"https://")) != NULL) {
        const char *end=p;
        while (*end && *end!='"' && *end!='\'' && *end!='<' &&
               *end!=' ' && *end!='\\' && end-p<2047) end++;
        size_t length=(size_t)(end-p);
        if (length < (size_t)size && length > 12) {
            char url[2048];
            memcpy(url,p,length);url[length]=0;
            if ((strstr(url,".m3u8") || strstr(url,".mp4") || strstr(url,".mpd")) &&
                !strstr(url,"trailer")) {
                snprintf(stream,(size_t)size,"%s",url);
                return 1;
            }
        }
        p+=8;
    }
    return 0;
}

/* Parse actual iframe tags, not just the single hard-coded video host.
   Return a cursor for the next candidate so one failed player does not
   prevent trying the next tab/embedded player on the episode page. */
static const char *find_embed_after(const char *cursor,char *url,int size) {
    const char *tag=cursor;
    while((tag=strstr(tag,"<iframe"))!=NULL){
        const char *tag_end=strchr(tag,'>');
        if(!tag_end)break;
        const char *attr=tag;
        while((attr=strstr(attr,"src=")) && attr<tag_end){
            const char *value=attr+4;
            if(*value=='"' || *value=='\''){
                char quote=*value++;
                const char *end=strchr(value,quote);
                if(end && end<=tag_end){
                    size_t len=(size_t)(end-value);
                    if(len>8 && len<(size_t)size-8){
                        if(!strncmp(value,"https://",8)){
                            memcpy(url,value,len);url[len]=0;
                        }else if(!strncmp(value,"//",2)){
                            snprintf(url,(size_t)size,"https:%.*s",(int)len,value);
                        }else url[0]=0;
                        if(url[0] && !strstr(url,"youtube") &&
                           !strstr(url,"trailer") && !strstr(url,"doubleclick") &&
                           !strstr(url,"google"))return tag_end+1;
                    }
                }
            }
            attr+=4;
        }
        tag=tag_end+1;
    }
    return NULL;
}

static int resolve_embedded(const char *html,char *stream,int size,
                            int depth,int *requests_left,char *error,int error_size){
    if(extract_media(html,stream,size))return 1;
    if(depth<=0)return 0;
    const char *next=html;
    for(int attempt=0;attempt<3 && *requests_left>0;attempt++){
        char embedded[1024]={0};
        next=find_embed_after(next,embedded,sizeof(embedded));
        if(!next)break;
        --*requests_left;
        char *content=fetch_uafresh(embedded,error,error_size);
        if(!content)continue;
        extract_subtitle(content);
        int found=resolve_embedded(content,stream,size,depth-1,requests_left,error,error_size);
        free(content);
        if(found)return 1;
    }
    /* Keep the original non-iframe provider fallback: several film pages
       store a player URL in a script attribute instead of an iframe tag. */
    if(depth>0){
        const char *hosts[]={"https://zetvideo.net/vod/","https://hdvbua.pro/",
                              "https://ashdi.vip/","https://tortuga."};
        for(size_t i=0;i<sizeof(hosts)/sizeof(hosts[0]) && *requests_left>0;i++){
            const char *p=strstr(html,hosts[i]);
            if(!p)continue;
            const char *end=p;
            while(*end && *end!='"' && *end!='\'' && *end!='<' &&
                  *end!=' ' && *end!='\\' && end-p<1000)end++;
            size_t len=(size_t)(end-p);
            if(len<15 || len>999)continue;
            char link[1024];memcpy(link,p,len);link[len]=0;
            if(strstr(link,"trailer") || strstr(link,"youtube"))continue;
            --*requests_left;
            char *content=fetch_uafresh(link,error,error_size);
            if(!content)continue;
            extract_subtitle(content);
            int found=extract_media(content,stream,size);
            free(content);
            if(found)return 1;
        }
    }
    return 0;
}

/* UAKino embeds are not on the film page: an AJAX endpoint returns li tags
   with data-file video URLs. This is shared by movies and episode lists. */
static char *uakino_playlist_page(const char *url,char *error,int error_size){
    static const char prefix[]="https://uakino.best/";
    if(strncmp(url,prefix,strlen(prefix)))return NULL;
    const char *last=strrchr(url,'/');
    if(!last || !isdigit((unsigned char)last[1]))return NULL;
    int news_id=0;
    if(sscanf(last+1,"%d-",&news_id)!=1 || news_id<1)return NULL;
    char endpoint[256];
    snprintf(endpoint,sizeof(endpoint),
             "https://uakino.best/engine/ajax/playlists.php?news_id=%d&xfield=playlist",
             news_id);
    char *json=request(endpoint,error,error_size);
    if(!json)return NULL;
    char *html=json_string_field_alloc(json,"response");
    free(json);
    return html;
}

static const char *next_uakino_playlist_item(const char *html,char *file,
                                             size_t file_size,char *voice,
                                             size_t voice_size,char *label,
                                             size_t label_size){
    const char *p=html;
    while((p=strstr(p,"<li"))){
        const char *tag_end=strchr(p,'>');
        if(!tag_end)return NULL;
        if(html_attr(p,tag_end,"data-file",file,file_size)){
            if(!html_attr(p,tag_end,"data-voice",voice,voice_size))
                voice[0]=0;
            const char *close=strstr(tag_end,"</li>");
            if(!close)return NULL;
            size_t n=0;
            const char *t=tag_end+1;
            while(t<close && n+1<label_size){
                if(*t=='<'){
                    const char *inner=strchr(t,'>');
                    if(!inner || inner>close)break;
                    t=inner+1;
                }else label[n++]=*t++;
            }
            while(n && isspace((unsigned char)label[n-1]))n--;
            label[n]=0;
            if(!strncmp(file,"//",2)){
                char temporary[1024];
                snprintf(temporary,sizeof(temporary),"https:%s",file);
                snprintf(file,file_size,"%s",temporary);
            }
            return close+5;
        }
        p=tag_end+1;
    }
    return NULL;
}

int uakino_resolve(const Source *source, char *stream, int size,
                   char *error, int error_size) {
    resolved_subtitle_count=0;
    resolved_audio_count=0;
    /* A Playerjs playlist leaf already contains the actual media URL. */
    if((strstr(source->url,".m3u8") || strstr(source->url,".mp4") ||
        strstr(source->url,".mpd")) &&
       (!strncmp(source->url,"https://",8) || !strncmp(source->url,"http://",7))){
        snprintf(stream,(size_t)size,"%s",source->url);
        error[0]=0;
        return 1;
    }
    char *playlist=uakino_playlist_page(source->url,error,error_size);
    if(playlist){
        const char *next=playlist;
        int requests_left=5;
        int found=0;
        for(int attempt=0;attempt<4 && !found;attempt++){
            char file[1024],voice[96],label[192];
            next=next_uakino_playlist_item(next,file,sizeof(file),voice,
                                            sizeof(voice),label,sizeof(label));
            if(!next)break;
            if(strstr(file,".m3u8") || strstr(file,".mp4") || strstr(file,".mpd")){
                snprintf(stream,(size_t)size,"%s",file);
                found=1;
            }else if(!strncmp(file,"https://",8)){
                char embed[1152];
                snprintf(embed,sizeof(embed),"<iframe src=\"%s\"></iframe>",file);
                found=resolve_embedded(embed,stream,size,2,&requests_left,
                                       error,error_size);
            }
        }
        free(playlist);
        if(found){error[0]=0;return 1;}
    }
    char *page = fetch_uafresh(source->url,error,error_size);
    if (!page) return 0;
    extract_subtitle(page);
    int requests_left=3;
    int found=resolve_embedded(page,stream,size,2,&requests_left,error,error_size);
    free(page);
    if(found)return 1;
    /* UAFlix episodes are publicly playable on the site. A missing stream
       means our extraction did not find the player, not an auth requirement. */
    snprintf(error,error_size,"Не знайдено прямий потік цієї серії. Спробуйте інший плеєр або джерело.");
    return 0;
}

static int episode_order(const void *a, const void *b) {
    const Episode *one=a, *two=b;
    if (one->season != two->season) return one->season - two->season;
    return one->number - two->number;
}

int catalog_next_episode(const Episode *items,int count,int current){
    if(!items || current<0 || current>=count)return -1;
    const Episode *now=&items[current];
    int best=-1;
    for(int i=0;i<count;i++){
        const Episode *candidate=&items[i];
        if(candidate->season<now->season ||
           (candidate->season==now->season && candidate->number<=now->number))
            continue;
        if(best<0 || candidate->season<items[best].season ||
           (candidate->season==items[best].season &&
            candidate->number<items[best].number))best=i;
    }
    if(best<0)return -1;
    /* Each dubbing is usually a different streaming URL. Keep the preferred
       voice on advancing to a new episode whenever that variant exists. */
    if(now->voice[0])for(int i=0;i<count;i++)
        if(items[i].season==items[best].season &&
           items[i].number==items[best].number &&
           !strcmp(items[i].voice,now->voice))return i;
    return best;
}

/* Playerjs serials often contain a JSON tree voice -> season -> episode in
   their iframe's `file` field. Unlike UAFlix episode cards, these have no
   /season-N-episode-M/ hrefs (e.g. South Park and many animated shows). */
static const char *json_ws(const char *p){
    while(*p && isspace((unsigned char)*p))p++;
    return p;
}
static const char *json_string(const char *p,char *out,size_t size){
    if(*p!='"')return NULL;
    p++;size_t n=0;
    while(*p && *p!='"'){
        unsigned char c=(unsigned char)*p++;
        if(c=='\\' && *p){
            c=(unsigned char)*p++;
            if(c=='n'||c=='r'||c=='t')c=' ';
            else if(c=='u'){
                for(int i=0;i<4 && isxdigit((unsigned char)*p);i++)p++;
                continue;
            }
        }
        if(n+1<size)out[n++]=(char)c;
    }
    out[n]=0;
    return *p=='"'?p+1:NULL;
}
static const char *json_skip(const char *p,int depth){
    p=json_ws(p);
    if(depth>16 || !*p)return NULL;
    if(*p=='"'){
        p++;while(*p && *p!='"'){
            if(*p=='\\' && p[1])p++;
            p++;
        }
        return *p=='"'?p+1:NULL;
    }
    if(*p=='[' || *p=='{'){
        char end=*p=='['?']':'}';p++;
        while(*(p=json_ws(p)) && *p!=end){
            if(*p==':' || *p==','){p++;continue;}
            p=json_skip(p,depth+1);
            if(!p)return NULL;
        }
        return *p==end?p+1:NULL;
    }
    while(*p && *p!=',' && *p!=']' && *p!='}' && !isspace((unsigned char)*p))p++;
    return p;
}
static int title_number(const char *title){
    while(*title && !isdigit((unsigned char)*title))title++;
    return *title?atoi(title):0;
}
static int is_season_title(const char *title){
    return strstr(title,"Сезон") || strstr(title,"сезон") ||
           strstr(title,"Season") || strstr(title,"season");
}
static int is_episode_title(const char *title){
    return strstr(title,"Серія") || strstr(title,"серія") ||
           strstr(title,"Episode") || strstr(title,"episode");
}
static void playlist_add(Episode *items,int *count,int max,int season,int episode,
                         const char *voice,const char *url){
    if(!url || !*url || *count>=max)return;
    const char *direct=strstr(url,"https://");
    if(!direct)direct=strstr(url,"http://");
    if(!direct || (!strstr(direct,".m3u8") && !strstr(direct,".mp4") &&
                   !strstr(direct,".mpd")))return;
    const char *end=direct;
    while(*end && *end!=',' && *end!='|' && *end!=' ' && *end!='"' &&
          *end!='\'' && end-direct<1023)end++;
    size_t length=(size_t)(end-direct);
    if(length<15 || length>=sizeof(items[0].url))return;
    if(season<1)season=1;
    if(episode<1)episode=1;
    for(int i=0;i<*count;i++)
        if(items[i].season==season && items[i].number==episode &&
           !strcmp(items[i].voice,voice?voice:""))return;
    Episode *entry=&items[(*count)++];
    memset(entry,0,sizeof(*entry));
    entry->season=season;entry->number=episode;
    memcpy(entry->url,direct,length);entry->url[length]=0;
    snprintf(entry->voice,sizeof(entry->voice),"%s",voice && *voice?voice:"Основна");
    snprintf(entry->title,sizeof(entry->title),"Сезон %d · Серія %d",season,episode);
}
static const char *playlist_walk(const char *p,int season,int episode,
                                 const char *voice,int ordinal,int depth,
                                 Episode *items,int *count,int max){
    p=json_ws(p);
    if(depth>16 || !*p || *count>=max)return json_skip(p,depth);
    if(*p=='['){
        p++;int index=0;
        while(*(p=json_ws(p)) && *p!=']'){
            if(*p==','){p++;continue;}
            const char *next=playlist_walk(p,season,episode,voice,++index,depth+1,
                                           items,count,max);
            if(!next || next<=p)return NULL;
            p=next;
        }
        return *p==']'?p+1:NULL;
    }
    if(*p!='{')return json_skip(p,depth);
    const char *cursor=p+1,*folder=NULL;
    char title[192]={0},file[2048]={0};
    while(*(cursor=json_ws(cursor)) && *cursor!='}'){
        if(*cursor==','){cursor++;continue;}
        char key[48];
        cursor=json_string(cursor,key,sizeof(key));
        if(!cursor)return NULL;
        cursor=json_ws(cursor);
        if(*cursor++!=':')return NULL;
        cursor=json_ws(cursor);
        if(!strcmp(key,"title") && *cursor=='"')
            cursor=json_string(cursor,title,sizeof(title));
        else if(!strcmp(key,"file") && *cursor=='"')
            cursor=json_string(cursor,file,sizeof(file));
        else {
            if(!strcmp(key,"folder") && (*cursor=='[' || *cursor=='{'))folder=cursor;
            cursor=json_skip(cursor,depth+1);
        }
        if(!cursor)return NULL;
    }
    if(*cursor!='}')return NULL;
    int s=season,e=episode;
    const char *v=voice;
    int num=title_number(title);
    if(is_season_title(title) && num>0){s=num;e=0;}
    else if(is_episode_title(title) && num>0)e=num;
    else if(depth<=2 && title[0] && !file[0])v=title;
    if(file[0])playlist_add(items,count,max,s,e?e:ordinal,v,file);
    if(folder)playlist_walk(folder,s,e,v,0,depth+1,items,count,max);
    return cursor+1;
}
static int playlist_from_html(const char *html,Episode *items,int max){
    const char *p=html;int count=0;
    while((p=strstr(p,"file"))!=NULL && !count){
        const char *value=json_ws(p+4);
        if(*value!=':' && *value!='='){p+=4;continue;}
        value=json_ws(value+1);
        char *decoded=NULL;
        if(*value=='\'' || *value=='"'){
            char quote=*value++;
            size_t capacity=strlen(value)+1;
            if(capacity>3*1024*1024){p+=4;continue;}
            decoded=malloc(capacity);
            if(!decoded)return 0;
            size_t n=0;
            while(*value && *value!=quote){
                char c=*value++;
                if(c=='\\' && *value){
                    c=*value++;
                    if(c=='n'||c=='r'||c=='t')c=' ';
                }
                decoded[n++]=c;
            }
            decoded[n]=0;
        }else if(*value=='[' || *value=='{'){
            const char *end=json_skip(value,0);
            if(end && (size_t)(end-value)<3*1024*1024){
                decoded=malloc((size_t)(end-value)+1);
                if(!decoded)return 0;
                memcpy(decoded,value,(size_t)(end-value));
                decoded[end-value]=0;
            }
        }
        if(decoded){
            const char *start=json_ws(decoded);
            if(*start=='[' || *start=='{')
                playlist_walk(start,1,0,"Основна",0,0,items,&count,max);
            free(decoded);
        }
        p+=4;
    }
    return count;
}

/* UAFlix keeps earlier episodes on /sezon-N/ pages for many long-running
   serials. The original catalog scanned only the first (usually newest)
   page, so whole seasons were absent from the Switch picker. */
static int uaflix_href(const char *begin,size_t length,char *url,size_t size){
    static const char host[]="https://uafix.net";
    if(length==0 || length>=size-25)return 0;
    if(!strncmp(begin,host,strlen(host)))
        snprintf(url,size,"%.*s",(int)length,begin);
    else if(begin[0]=='/' && (length<2 || begin[1]!='/'))
        snprintf(url,size,"%s%.*s",host,(int)length,begin);
    else if(!strncmp(begin,"//uafix.net/",13))
        snprintf(url,size,"https:%.*s",(int)length,begin);
    else return 0;
    return 1;
}
static void parse_uaflix_episodes(const char *html,const char *root,
                                  Episode *items,int *count,int max,
                                  char season_urls[][1024],int *seasons,int max_seasons){
    const char *p=html;
    while(*count<max && (p=strstr(p,"href="))){
        p+=5;
        if(*p!='"' && *p!='\'')continue;
        char quote=*p++;
        const char *end=strchr(p,quote);
        if(!end)break;
        char url[1024];
        if(uaflix_href(p,(size_t)(end-p),url,sizeof(url)) &&
           !strncmp(url,root,strlen(root))){
            const char *ep=strstr(url+strlen(root),"season-");
            int season=0,number=0;
            if(ep && sscanf(ep,"season-%d-episode-%d/",&season,&number)==2 &&
               season>=1 && season<=100 && number>=1 && number<=1000){
                int exists=0;
                for(int i=0;i<*count;i++)
                    if(items[i].season==season && items[i].number==number)exists=1;
                if(!exists){
                    Episode *entry=&items[(*count)++];
                    memset(entry,0,sizeof(*entry));
                    entry->season=season;entry->number=number;
                    snprintf(entry->url,sizeof(entry->url),"%s",url);
                    snprintf(entry->title,sizeof(entry->title),
                             "Сезон %d · Серія %d",season,number);
                }
            }else if(season_urls && seasons && *seasons<max_seasons){
                const char *season_part=strstr(url+strlen(root),"sezon-");
                int season_number=0;
                if(season_part && sscanf(season_part,"sezon-%d/",&season_number)==1 &&
                   season_number>0 && season_number<=100){
                    int exists=0;
                    for(int i=0;i<*seasons;i++)
                        if(!strcmp(season_urls[i],url))exists=1;
                    if(!exists)snprintf(season_urls[(*seasons)++],1024,"%s",url);
                }
            }
        }
        p=end+1;
    }
}

int uakino_episodes(const Source *source, Episode *items, int max,
                    char *error, int error_size) {
    if(strstr(source->url,"animeon.club/api/player/")){
        char *response=request(source->url,error,error_size);
        if(!response)return 0;
        /* AnimeON returns multiple players in arbitrary order. Moon stores
           `videos[].video_url`; Ashdi stores folder leaves with `file`.
           Locate a folder playlist across ALL players, not only the first. */
        char *episodes_json=NULL;
        const char *player_cursor=response;
        while((player_cursor=strstr(player_cursor,"\"json\""))){
            char *candidate=json_string_field_alloc(player_cursor,"json");
            if(candidate && strstr(candidate,"\"file\"")){
                episodes_json=candidate;
                break;
            }
            free(candidate);
            player_cursor+=6;
        }
        free(response);
        if(!episodes_json){
            snprintf(error,error_size,"AnimeON: немає доступних серій");
            return 0;
        }
        const char *p=episodes_json;
        int count=0;
        const char *last_title=NULL;
        int season=1;
        while(count<max && *p){
            const char *title=strstr(p,"\"title\"");
            const char *file=strstr(p,"\"file\"");
            if(!title && !file)break;
            if(title && (!file || title<file)){
                char label[192]={0};
                const char *after=strchr(title+7,',');
                if(after)json_field(title,after,"title",label,sizeof(label));
                int next_season=0;
                if(sscanf(label," Сезон %d",&next_season)==1 && next_season>0)
                    season=next_season;
                last_title=title;
                p=title+7;
                continue;
            }
            char stream[1024]={0},label[192]={0};
            const char *end=strchr(file,',');
            if(!end)end=file+strlen(file);
            json_field(file,end,"file",stream,sizeof(stream));
            if(last_title)json_field(last_title,file,"title",label,sizeof(label));
            int episode=0;
            const char *number=strstr(label,"ерія");
            if(number)sscanf(number+strlen("ерія"),"%d",&episode);
            if(episode>0 && episode<=1000 &&
               (!strncmp(stream,"https://",8) || !strncmp(stream,"http://",7))){
                Episode *dst=&items[count++];
                memset(dst,0,sizeof(*dst));
                dst->season=season;dst->number=episode;
                snprintf(dst->title,sizeof(dst->title),"Серія %d · AnimeON",episode);
                snprintf(dst->url,sizeof(dst->url),"%s",stream);
            }
            p=file+6;
        }
        free(episodes_json);
        if(count)qsort(items,(size_t)count,sizeof(*items),episode_order);
        else snprintf(error,error_size,"AnimeON: немає доступних відеоджерел");
        return count;
    }
    if(strstr(source->url,"api.mikai.me/public/v1/anime/") &&
       strstr(source->url,"/player")){
        char *json=request(source->url,error,error_size);
        if(!json)return 0;
        int count=0;
        const char *p=strstr(json,"\"releases\"");
        while(p && count<max && (p=strstr(p,"{\"number\""))){
            const char *end=object_end(p);
            if(!end)break;
            char number[24]={0},stream[1024]={0};
            json_field(p,end,"number",number,sizeof(number));
            /* Prefer ASHDI when offered; Moon embeds can return HTTP 500. */
            const char *ashdi=strstr(p,"\"provider\":\"ashdi\"");
            if(ashdi && ashdi<end)
                json_field(ashdi,end,"embedUrl",stream,sizeof(stream));
            if(!stream[0])json_field(p,end,"embedUrl",stream,sizeof(stream));
            int episode=atoi(number);
            if(episode>0 && episode<=1000 &&
               (!strncmp(stream,"https://",8) || !strncmp(stream,"http://",7))){
                Episode *dst=&items[count++];
                memset(dst,0,sizeof(*dst));
                dst->season=1;dst->number=episode;
                snprintf(dst->title,sizeof(dst->title),"Серія %d · Mikai",episode);
                snprintf(dst->url,sizeof(dst->url),"%s",stream);
            }
            p=end;
        }
        free(json);
        if(count)qsort(items,(size_t)count,sizeof(*items),episode_order);
        else snprintf(error,error_size,"Mikai: немає доступних відеоджерел");
        return count;
    }
    char *playlist=uakino_playlist_page(source->url,error,error_size);
    if(playlist){
        int count=0,serial=0;
        const char *next=playlist;
        while(count<max){
            char file[1024],voice[96],label[192];
            next=next_uakino_playlist_item(next,file,sizeof(file),voice,
                                            sizeof(voice),label,sizeof(label));
            if(!next)break;
            int episode=0;
            /* Prefer the visible episode number. If all titles are voice
               names, preserve the playlist position as a fallback. */
            const char *word=strstr(label,"ерія");
            if(word){word+=strlen("ерія");sscanf(word,"%d",&episode);}
            if(episode<1)sscanf(label,"%d",&episode);
            if(episode<1)episode=count+1;
            if(episode>1000 || strncmp(file,"https://",8))continue;
            Episode *dst=&items[count++];
            memset(dst,0,sizeof(*dst));
            dst->season=1;dst->number=episode;
            snprintf(dst->title,sizeof(dst->title),"Сезон 1 · Серія %d",episode);
            snprintf(dst->url,sizeof(dst->url),"%s",file);
            if(voice[0])snprintf(dst->voice,sizeof(dst->voice),"%s",voice);
            serial=1;
        }
        free(playlist);
        if(serial){
            qsort(items,(size_t)count,sizeof(*items),episode_order);
            error[0]=0;
            return count;
        }
    }
    char *html=fetch_uafresh(source->url,error,error_size);
    if (!html) return 0;
    int count=0;
    if(!strncmp(source->url,"https://uafix.net/serials/",25)){
        char season_urls[40][1024];int season_pages=0;
        parse_uaflix_episodes(html,source->url,items,&count,max,
                              season_urls,&season_pages,40);
        /* Fetch the season archive pages even when the main page lists some
           episodes. Those may only cover the latest season or latest items. */
        for(int i=0;i<season_pages && count<max;i++){
            char *archive=fetch_uafresh(season_urls[i],error,error_size);
            if(!archive)continue;
            parse_uaflix_episodes(archive,source->url,items,&count,max,
                                  NULL,NULL,0);
            free(archive);
        }
    }
    if(!count){
        count=playlist_from_html(html,items,max);
        const char *next=html;
        for(int attempt=0;!count && attempt<4;attempt++){
            char iframe[1024]={0};
            next=find_embed_after(next,iframe,sizeof(iframe));
            if(!next)break;
            char *player_html=fetch_uafresh(iframe,error,error_size);
            if(player_html){
                count=playlist_from_html(player_html,items,max);
                free(player_html);
            }
        }
    }
    free(html);
    if (count) {
        qsort(items,(size_t)count,sizeof(*items),episode_order);
        error[0]=0;
    } else snprintf(error,error_size,"Плеєр не надав доступний перелік серій. Спробуйте інше джерело.");
    return count;
}
