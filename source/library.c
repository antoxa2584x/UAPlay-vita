#include "library.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LIBRARY_DIR UAPLAY_DATA_DIR
#define LIBRARY_FILE LIBRARY_DIR "/library.tsv"
#define LIBRARY_TMP LIBRARY_DIR "/library.tmp"
#define LIBRARY_BACKUP LIBRARY_DIR "/library.bak"
/* Older UAFlix/UAPlay builds used this folder. Import once, retaining the
   original file as a safety copy so an NRO update never loses favorites. */
#define LEGACY_LIBRARY_FILE UAPLAY_DATA_ROOT "/uaflix/library.tsv"
#define MAX_LIBRARY_ITEMS 1024

typedef struct {
    Movie movie;
    int favorite;
    int watched;
    double progress;
    int last_season,last_episode;
} Entry;

static Entry entries[MAX_LIBRARY_ITEMS];
static int total;

static Entry *entry_for(const Movie *movie, int create) {
    for (int i=0; i<total; i++) {
        if (entries[i].movie.id == movie->id &&
            entries[i].movie.series == movie->series){
            if(create){
                if(movie->title[0])entries[i].movie=*movie;
            }
            return &entries[i];
        }
    }
    if (!create || total == MAX_LIBRARY_ITEMS) return NULL;
    Entry *new_entry = &entries[total++];
    memset(new_entry,0,sizeof(*new_entry));
    new_entry->movie=*movie;
    return new_entry;
}

static void clean_field(char *out, size_t capacity, const char *in) {
    size_t n=0;
    while (*in && n+1<capacity) {
        unsigned char c=(unsigned char)*in++;
        if (c=='\n' || c=='\r' || c=='\t' || c=='|') c=' ';
        out[n++]=(char)c;
    }
    out[n]=0;
}

static void library_save(void) {
    mkdir(UAPLAY_DATA_ROOT,0777);
    mkdir(LIBRARY_DIR,0777);
    FILE *file=fopen(LIBRARY_TMP,"wb");
    if (!file) return;
    for (int i=0;i<total;i++) {
        Entry *e=&entries[i];
        if (!e->favorite && !e->watched) continue;
        char title[192], original[192], poster[256], overview[1024];
        clean_field(title,sizeof(title),e->movie.title);
        clean_field(original,sizeof(original),e->movie.original);
        clean_field(poster,sizeof(poster),e->movie.poster);
        clean_field(overview,sizeof(overview),e->movie.overview);
        fprintf(file,"%d|%d|%d|%d|%.1f|%s|%s|%s|%s|%d|%d|%.2f|%.3f|%s\n",
                e->movie.id,e->movie.series,e->favorite,e->watched,
                e->progress,title,original,e->movie.year,poster,
                e->last_season,e->last_episode,e->movie.rating,e->movie.popularity,
                overview);
    }
    if (fclose(file)!=0) { remove(LIBRARY_TMP); return; }
    /* FAT does not reliably replace an existing destination with rename().
       Keep the previous complete library until the new file is installed. */
    FILE *previous=fopen(LIBRARY_FILE,"rb");
    int had_previous=previous!=NULL;
    if(previous)fclose(previous);
    if(had_previous){
        remove(LIBRARY_BACKUP);
        if(rename(LIBRARY_FILE,LIBRARY_BACKUP)!=0){
            remove(LIBRARY_TMP);
            return;
        }
    }
    if(rename(LIBRARY_TMP,LIBRARY_FILE)!=0){
        if(had_previous)rename(LIBRARY_BACKUP,LIBRARY_FILE);
        remove(LIBRARY_TMP);
    }
}

void library_init(void) {
    total=0;
    FILE *file=fopen(LIBRARY_FILE,"rb");
    int legacy=0;
    if(!file)file=fopen(LIBRARY_BACKUP,"rb");
    if(!file){file=fopen(LEGACY_LIBRARY_FILE,"rb");legacy=file!=NULL;}
    if (!file) return;
    char line[2400];
    while (total<MAX_LIBRARY_ITEMS && fgets(line,sizeof(line),file)) {
        char *fields[9]={0}, *p=line;
        for (int i=0;i<9;i++) {
            fields[i]=p;
            if (i==8) break;
            char *separator=strchr(p,'|');
            if (!separator) { fields[0]=NULL; break; }
            *separator=0;p=separator+1;
        }
        if (!fields[0]) continue;
        Entry *entry=&entries[total++];
        memset(entry,0,sizeof(*entry));
        entry->movie.id=atoi(fields[0]);
        entry->movie.series=atoi(fields[1]);
        entry->favorite=atoi(fields[2]);
        entry->watched=atoi(fields[3]);
        entry->progress=atof(fields[4]);
        /* Legacy records stop after the poster. New records append season
           and episode, preserving every old favorite and watch-history row. */
        char *extra=strchr(fields[8],'|');
        if(extra){
            *extra++=0;
            entry->last_season=atoi(extra);
            char *episode=strchr(extra,'|');
            if(episode){
                entry->last_episode=atoi(episode+1);
                char *rating=strchr(episode+1,'|');
                if(rating){
                    entry->movie.rating=(float)atof(rating+1);
                    char *popularity=strchr(rating+1,'|');
                    if(popularity){
                        entry->movie.popularity=(float)atof(popularity+1);
                        char *overview=strchr(popularity+1,'|');
                        if(overview){
                            *overview++=0;
                            snprintf(entry->movie.overview,sizeof(entry->movie.overview),
                                     "%s",overview);
                            entry->movie.overview[strcspn(entry->movie.overview,"\r\n")]=0;
                        }
                    }
                }
            }
        }
        snprintf(entry->movie.title,sizeof(entry->movie.title),"%s",fields[5]);
        snprintf(entry->movie.original,sizeof(entry->movie.original),"%s",fields[6]);
        snprintf(entry->movie.year,sizeof(entry->movie.year),"%s",fields[7]);
        snprintf(entry->movie.poster,sizeof(entry->movie.poster),"%s",fields[8]);
        entry->movie.poster[strcspn(entry->movie.poster,"\r\n")]=0;
    }
    fclose(file);
    if(legacy && total)library_save();
}

int library_favorite(const Movie *movie) {
    Entry *e=entry_for(movie,0);
    return e && e->favorite;
}
void library_toggle_favorite(const Movie *movie) {
    Entry *e=entry_for(movie,1);
    if (e) {e->favorite=!e->favorite;library_save();}
}
void library_mark_watched(const Movie *movie) {
    Entry *e=entry_for(movie,1);
    if (e) {e->watched=1;library_save();}
}
void library_set_progress(const Movie *movie,double seconds) {
    Entry *e=entry_for(movie,1);
    if (e) {e->watched=1;e->progress=seconds>0?seconds:0;library_save();}
}
double library_progress(const Movie *movie) {
    Entry *e=entry_for(movie,0);
    return e?e->progress:0;
}
void library_set_episode_progress(const Movie *movie,int season,int episode,double seconds){
    Entry *e=entry_for(movie,1);
    if(e && season>0 && episode>0){
        e->watched=1;e->last_season=season;e->last_episode=episode;
        e->progress=seconds>0?seconds:0;
        library_save();
    }
}
int library_last_episode(const Movie *movie,int *season,int *episode,double *seconds){
    Entry *e=entry_for(movie,0);
    if(!e || e->last_season<1 || e->last_episode<1)return 0;
    if(season)*season=e->last_season;
    if(episode)*episode=e->last_episode;
    if(seconds)*seconds=e->progress;
    return 1;
}
int library_list(Movie *items,int capacity,int favorites) {
    int count=0;
    for (int i=total-1;i>=0 && count<capacity;i--) {
        if ((favorites && entries[i].favorite) || (!favorites && entries[i].watched))
            items[count++]=entries[i].movie;
    }
    return count;
}
int library_list_to_watch(Movie *items,int capacity) {
    int count=0;
    for(int i=total-1;i>=0 && count<capacity;i--){
        if(entries[i].favorite && !entries[i].watched)
            items[count++]=entries[i].movie;
    }
    return count;
}
int library_clear_history(void) {
    int removed=0,kept=0;
    for(int i=0;i<total;i++){
        Entry entry=entries[i];
        if(entry.watched){
            removed++;
            entry.watched=0;
            entry.progress=0;
            entry.last_season=0;
            entry.last_episode=0;
        }
        if(entry.favorite)entries[kept++]=entry;
    }
    total=kept;
    if(removed)library_save();
    return removed;
}
