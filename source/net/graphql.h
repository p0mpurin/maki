#ifndef GRAPHQL_H
#define GRAPHQL_H

#include "../net/http.h"
#include "../config/config.h"
#include <stddef.h>

typedef struct {
    char id[64];
    char name[256];
    int  episode_count;
} AnimeResult;

typedef struct {
    char number[16];
} Episode;

typedef struct {
    char url[1024];
    char source_name[64];
} EmbedSource;

int graphql_search(HttpConn *conn, const Config *cfg, const char *query,
                   AnimeResult *results, int max_results);
int graphql_episodes(HttpConn *conn, const Config *cfg, const char *show_id,
                     Episode *episodes, int max_episodes);
int graphql_sources(HttpConn *conn, const Config *cfg, const char *show_id,
                    const char *episode_str, EmbedSource *sources, int max_sources);

#endif
