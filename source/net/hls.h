#ifndef HLS_H
#define HLS_H

#include "../net/http.h"
#include "../config/config.h"
#include <stdbool.h>

typedef struct {
    char url[512];
    int  bandwidth;
    int  width;
    int  height;
} HlsVariant;

typedef struct {
    char     seg_urls[512][512];
    int      seg_count;
    int      target_duration;
    bool     is_endlist;
    char     base_url[512];
} HlsPlaylist;

int  hls_pick_variant(HttpConn *conn, const Config *cfg, const char *master_url,
                      char *out_url, int out_url_size);
int  hls_fetch_playlist(HttpConn *conn, const Config *cfg, const char *playlist_url,
                        HlsPlaylist *out);
void hls_resolve_url(const char *base, const char *seg, char *out, int out_size);

#endif
