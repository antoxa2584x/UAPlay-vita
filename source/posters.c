#include "posters.h"
#include "platform.h"

#include <SDL.h>
#include <SDL_image.h>
#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Reduce GPU memory from up to roughly 80 MB to about 30 MB. */
#define POSTER_CACHE_MAX 48
#define MAX_POSTER_BYTES (1024 * 1024)
/* Eden's SSL implementation becomes unreliable under sustained concurrent
   HTTPS handshakes. A persistent single downloader is safer and can still
   warm the disk cache in the background without blocking the interface. */
#define POSTER_WORKERS 1
#define POSTER_RETRY_DELAY_MS 20000u
#define POSTER_TLS_COOLDOWN_MS 15000u
/* Fixed-size disk cache: at most 96 compressed posters, never an unbounded
   folder of thousands of image files from long TMDB searches. */
#define POSTER_DISK_SLOTS 96
#define POSTER_DISK_DIR UAPLAY_DATA_DIR "/posters"
#define POSTER_DISK_MAGIC 0x55415032u

typedef struct {
    int id;
    GLuint texture;
    Uint32 retry_after;
    unsigned long long last_used;
} PosterEntry;
typedef struct { unsigned char *data; size_t used; } ImageData;
typedef struct { uint32_t magic,bytes; uint64_t fingerprint; } DiskHeader;
typedef struct {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int quit,pending,busy,request_id;
    Uint32 blocked_until;
    char request_path[256];
    int ready,completed_id;
    ImageData completed_image;
} PosterLoader;

static PosterEntry cache[POSTER_CACHE_MAX];
static PosterLoader loaders[POSTER_WORKERS];
static SDL_mutex *disk_mutex;
static int used;
static unsigned long long cache_clock;
static void (*flush_draws)(void);

void posters_set_flush(void (*flush)(void)){flush_draws=flush;}
static unsigned upload_count;
static double upload_ms;
void posters_upload_stats(unsigned *count,double *milliseconds){
    *count=upload_count;*milliseconds=upload_ms;
}

static size_t receive(char *pointer,size_t size,size_t count,void *ctx){
    ImageData *image=ctx;
    if(size && count>MAX_POSTER_BYTES/size)return 0;
    size_t bytes=size*count;
    if(image->used>MAX_POSTER_BYTES || bytes>MAX_POSTER_BYTES-image->used)return 0;
    void *next=realloc(image->data,image->used+bytes);
    if(!next)return 0;
    image->data=next;
    memcpy(image->data+image->used,pointer,bytes);
    image->used+=bytes;
    return bytes;
}

static uint64_t path_fingerprint(const char *path){
    uint64_t hash=UINT64_C(14695981039346656037);
    for(const unsigned char *p=(const unsigned char*)path;*p;p++){
        hash^=*p;
        hash*=UINT64_C(1099511628211);
    }
    return hash;
}
static void disk_filename(uint64_t hash,char *filename,size_t size){
    snprintf(filename,size,POSTER_DISK_DIR "/%02u.cache",
             (unsigned)(hash%POSTER_DISK_SLOTS));
}
static ImageData disk_read(const char *path){
    ImageData result={0};
    if(!disk_mutex)return result;
    uint64_t hash=path_fingerprint(path);
    char filename[128];disk_filename(hash,filename,sizeof(filename));
    SDL_LockMutex(disk_mutex);
    FILE *file=fopen(filename,"rb");
    if(file){
        DiskHeader header={0};
        if(fread(&header,sizeof(header),1,file)==1 &&
           header.magic==POSTER_DISK_MAGIC && header.fingerprint==hash &&
           header.bytes>0 && header.bytes<=MAX_POSTER_BYTES){
            result.data=malloc(header.bytes);
            if(result.data){
                if(fread(result.data,1,header.bytes,file)==header.bytes)
                    result.used=header.bytes;
                else{free(result.data);result.data=NULL;}
            }
        }
        fclose(file);
    }
    SDL_UnlockMutex(disk_mutex);
    return result;
}
static void disk_write(const char *path,const ImageData *image){
    if(!disk_mutex || !image->data || !image->used ||
       image->used>MAX_POSTER_BYTES)return;
    uint64_t hash=path_fingerprint(path);
    char filename[128];disk_filename(hash,filename,sizeof(filename));
    DiskHeader header={POSTER_DISK_MAGIC,(uint32_t)image->used,hash};
    SDL_LockMutex(disk_mutex);
    FILE *file=fopen(filename,"wb");
    if(file){
        if(fwrite(&header,sizeof(header),1,file)==1)
            (void)fwrite(image->data,1,image->used,file);
        fclose(file);
    }
    SDL_UnlockMutex(disk_mutex);
}

/* Download raw bytes on a worker; decode and upload on the GL/UI thread. */
static ImageData download_image(const char *path,CURL *curl,int *ssl_failed){
    ImageData data={0};
    *ssl_failed=0;
    if(!path || path[0]!='/' || strstr(path,".."))return data;
    data=disk_read(path);
    if(data.used)return data;
    if(!curl)return data;
    char url[512];
    snprintf(url,sizeof(url),"https://image.tmdb.org/t/p/w342%s",path);
    /* Reset request-specific options but preserve the handle's persistent
       TCP/TLS connection pool and TLS session cache across poster requests. */
    curl_easy_reset(curl);
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,4L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,7L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl,CURLOPT_TCP_KEEPALIVE,1L);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,"UAPlay-nx/1.0");
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&data);
    net_tls_options(curl);
    char curl_error[CURL_ERROR_SIZE]="";
    curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,curl_error);
    Uint32 wait_started=SDL_GetTicks();
    net_background_acquire(0);
    Uint32 waited=SDL_GetTicks()-wait_started;
    CURLcode result=curl_easy_perform(curl);
    if(result==CURLE_SSL_CONNECT_ERROR){
        /* One bounded retry with a fresh connection; never disable TLS
           certificate or hostname verification to hide an SSL failure. */
        free(data.data);data=(ImageData){0};
        curl_easy_setopt(curl,CURLOPT_FRESH_CONNECT,1L);
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,2L);
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,4L);
        result=curl_easy_perform(curl);
    }
    net_background_release(0);
    net_profile_record('P',curl,url,waited,(int)result,(unsigned)data.used,
                       curl_error[0]?curl_error:curl_easy_strerror(result));
    long code=0;
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);
    *ssl_failed=result==CURLE_SSL_CONNECT_ERROR ||
                result==CURLE_COULDNT_CONNECT ||
                result==CURLE_OPERATION_TIMEDOUT;
    if(result!=CURLE_OK || code!=200 || !data.used){
        free(data.data);data.data=NULL;data.used=0;
    }else disk_write(path,&data);
    return data;
}

static int download_worker(void *opaque){
    PosterLoader *task=opaque;
    CURL *curl=NULL;
    for(;;){
        SDL_LockMutex(task->mutex);
        while(!task->quit && !task->pending)
            SDL_CondWait(task->cond,task->mutex);
        if(task->quit){SDL_UnlockMutex(task->mutex);break;}
        char path[256];
        snprintf(path,sizeof(path),"%s",task->request_path);
        int id=task->request_id;
        task->pending=0;task->busy=1;
        SDL_UnlockMutex(task->mutex);

        if(!curl)curl=curl_easy_init();
        int ssl_failed=0;
        ImageData image=download_image(path,curl,&ssl_failed);

        SDL_LockMutex(task->mutex);
        task->busy=0;
        if(ssl_failed)
            task->blocked_until=SDL_GetTicks()+POSTER_TLS_COOLDOWN_MS;
        if(!task->quit){
            task->completed_image=image;
            task->completed_id=id;
            task->ready=1;
        }else free(image.data);
        SDL_UnlockMutex(task->mutex);
    }
    if(curl)curl_easy_cleanup(curl);
    return 0;
}

void posters_init(void){
    mkdir(UAPLAY_DATA_DIR,0777);
    mkdir(POSTER_DISK_DIR,0777);
    disk_mutex=SDL_CreateMutex();
    for(int i=0;i<POSTER_WORKERS;i++){
        PosterLoader *task=&loaders[i];
        task->mutex=SDL_CreateMutex();
        task->cond=SDL_CreateCond();
        if(task->mutex && task->cond){
            char name[32];
            snprintf(name,sizeof(name),"poster-fetch-%d",i);
            task->thread=SDL_CreateThread(download_worker,name,task);
        }
    }
}

static GLuint image_texture_timed(const ImageData *data);
/* JPEG decoding runs here on the UI thread, so the profiler tracks it. */
static GLuint image_texture(const ImageData *data){
    if(!data->data || !data->used)return 0;
    Uint64 started=SDL_GetPerformanceCounter();
    GLuint texture=image_texture_timed(data);
    upload_ms+=(double)(SDL_GetPerformanceCounter()-started)*1000.0/
               (double)SDL_GetPerformanceFrequency();
    upload_count++;
    return texture;
}
static GLuint image_texture_timed(const ImageData *data){
    SDL_RWops *rw=SDL_RWFromConstMem(data->data,(int)data->used);
    SDL_Surface *image=rw?IMG_Load_RW(rw,1):NULL;
    if(!image)return 0;
    SDL_Surface *rgba=SDL_ConvertSurfaceFormat(image,SDL_PIXELFORMAT_ABGR8888,0);
    SDL_FreeSurface(image);
    if(!rgba)return 0;
    GLuint texture=0;
    glGenTextures(1,&texture);
    if(texture){
        glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,rgba->w,rgba->h,
                     0,GL_RGBA,GL_UNSIGNED_BYTE,rgba->pixels);
    }
    SDL_FreeSurface(rgba);
    return texture;
}

static void cache_store(int id,GLuint texture){
    int slot=used;
    if(used==POSTER_CACHE_MAX){
        slot=0;
        for(int i=1;i<used;i++)
            if(cache[i].last_used<cache[slot].last_used)slot=i;
        if(cache[slot].texture){
            if(flush_draws)flush_draws();
            glDeleteTextures(1,&cache[slot].texture);
        }
    }else used++;
    cache[slot].id=id;
    cache[slot].texture=texture;
    cache[slot].retry_after=texture?0:SDL_GetTicks()+POSTER_RETRY_DELAY_MS;
    cache[slot].last_used=++cache_clock;
}

static void collect_completed(void){
    for(int i=0;i<POSTER_WORKERS;i++){
        PosterLoader *task=&loaders[i];
        if(!task->mutex)continue;
        SDL_LockMutex(task->mutex);
        if(!task->ready){SDL_UnlockMutex(task->mutex);continue;}
        ImageData image=task->completed_image;
        int id=task->completed_id;
        task->completed_image=(ImageData){0};
        task->ready=0;
        SDL_UnlockMutex(task->mutex);
        GLuint texture=image_texture(&image);
        free(image.data);
        /* A failed request is remembered to prevent a network retry loop. */
        cache_store(id,texture);
    }
}

static int already_cached(int id){
    for(int i=0;i<used;i++)if(cache[i].id==id){
        if(!cache[i].texture && SDL_TICKS_PASSED(SDL_GetTicks(),cache[i].retry_after)){
            cache[i].id=0;
            return 0;
        }
        return 1;
    }
    return 0;
}
static int in_flight(int id){
    for(int i=0;i<POSTER_WORKERS;i++){
        PosterLoader *task=&loaders[i];
        if(!task->mutex)continue;
        SDL_LockMutex(task->mutex);
        int matched=task->request_id==id &&
                    (task->pending || task->busy || task->ready);
        SDL_UnlockMutex(task->mutex);
        if(matched)return 1;
    }
    return 0;
}
static int request_poster(const Movie *movie){
    int id=movie->series?-movie->id:movie->id;
    if(!movie->poster[0] || !strcmp(movie->poster,"null") ||
       already_cached(id) || in_flight(id))return 0;
    for(int i=0;i<POSTER_WORKERS;i++){
        PosterLoader *task=&loaders[i];
        if(!task->thread)continue;
        SDL_LockMutex(task->mutex);
        if(!task->pending && !task->busy && !task->ready &&
           (!task->blocked_until || SDL_TICKS_PASSED(SDL_GetTicks(),task->blocked_until))){
            task->request_id=id;
            snprintf(task->request_path,sizeof(task->request_path),
                     "%s",movie->poster);
            task->pending=1;
            SDL_CondSignal(task->cond);
            SDL_UnlockMutex(task->mutex);
            return 1;
        }
        SDL_UnlockMutex(task->mutex);
    }
    return 0;
}

GLuint poster_texture(const Movie *movie,int *budget){
    if(!movie || !movie->poster[0] || !strcmp(movie->poster,"null"))return 0;
    collect_completed();
    int identity=movie->series?-movie->id:movie->id;
    for(int i=0;i<used;i++){
        if(cache[i].id==identity){
            if(!cache[i].texture &&
               SDL_TICKS_PASSED(SDL_GetTicks(),cache[i].retry_after)){
                cache[i].id=0;
                break;
            }
            cache[i].last_used=++cache_clock;
            return cache[i].texture;
        }
    }
    if(budget && *budget>0 && request_poster(movie))--*budget;
    return 0;
}

void posters_prefetch(const Movie *movies,int count,int start,int ahead){
    if(!movies || count<=0 || start<0)return;
    collect_completed();
    int end=start+ahead;
    if(end>count)end=count;
    /* Only fill idle network slots. Visible poster requests are submitted by
       draw() first and always take precedence over speculative prefetches. */
    for(int i=start;i<end;i++)request_poster(&movies[i]);
}

void posters_destroy(void){
    for(int i=0;i<POSTER_WORKERS;i++){
        PosterLoader *task=&loaders[i];
        if(!task->mutex)continue;
        SDL_LockMutex(task->mutex);
        task->quit=1;
        if(task->cond)SDL_CondSignal(task->cond);
        SDL_UnlockMutex(task->mutex);
    }
    for(int i=0;i<POSTER_WORKERS;i++){
        PosterLoader *task=&loaders[i];
        if(task->thread)SDL_WaitThread(task->thread,NULL);
        free(task->completed_image.data);
        if(task->cond)SDL_DestroyCond(task->cond);
        if(task->mutex)SDL_DestroyMutex(task->mutex);
    }
    memset(loaders,0,sizeof(loaders));
    if(disk_mutex)SDL_DestroyMutex(disk_mutex);
    disk_mutex=NULL;
    for(int i=0;i<used;i++)
        if(cache[i].texture)glDeleteTextures(1,&cache[i].texture);
    used=0;cache_clock=0;
}
