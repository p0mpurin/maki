#include "hls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RESPONSE (128 * 1024)

void hls_resolve_url(const char *base, const char *seg, char *out, int out_size) {
    if (strncmp(seg, "http://", 7) == 0 || strncmp(seg, "https://", 8) == 0) {
        strncpy(out, seg, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    char base_copy[512];
    strncpy(base_copy, base, sizeof(base_copy) - 1);
    base_copy[sizeof(base_copy) - 1] = '\0';

    char *last_slash = strrchr(base_copy, '/');
    if (last_slash) *(last_slash + 1) = '\0';
    else strcat(base_copy, "/");

    snprintf(out, out_size, "%s%s", base_copy, seg);
}

int hls_pick_variant(HttpConn *conn, const Config *cfg, const char *master_url,
                     char *out_url, int out_url_size) {
    char *resp = (char *)malloc(MAX_RESPONSE);
    if (!resp) return -1;

    int ret = http_get_stream(conn, master_url, cfg->referer, cfg->agent);
    if (ret < 0) { free(resp); return -1; }

    int len = 0;
    while (len < MAX_RESPONSE - 1) {
        int n = http_read_stream(conn, (uint8_t *)(resp + len), MAX_RESPONSE - 1 - len);
        if (n <= 0) break;
        len += n;
    }
    http_disconnect(conn);
    resp[len] = '\0';

    char *lines = strdup(resp);
    free(resp);
    if (!lines) return -1;

    char *saveptr = NULL;
    char *line = strtok_r(lines, "\n", &saveptr);

    HlsVariant best_variant;
    memset(&best_variant, 0, sizeof(best_variant));
    int target_height = cfg->quality;
    int best_diff = 99999;
    int found = 0;

    while (line) {
        while (*line == '\r' || *line == '\n') line++;
        char trimmed[512];
        strncpy(trimmed, line, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        char *cr = strchr(trimmed, '\r');
        if (cr) *cr = '\0';

        if (strncmp(trimmed, "#EXT-X-STREAM-INF", 17) == 0) {
            HlsVariant v;
            memset(&v, 0, sizeof(v));

            char *res_ptr = strstr(trimmed, "RESOLUTION=");
            if (res_ptr) {
                sscanf(res_ptr + 11, "%dx%d", &v.width, &v.height);
            }

            char *bw_ptr = strstr(trimmed, "BANDWIDTH=");
            if (bw_ptr) {
                v.bandwidth = atoi(bw_ptr + 10);
            }

            line = strtok_r(NULL, "\n", &saveptr);
            if (line) {
                while (*line == '\r' || *line == '\n') line++;
                strncpy(trimmed, line, sizeof(trimmed) - 1);
                trimmed[sizeof(trimmed) - 1] = '\0';
                cr = strchr(trimmed, '\r');
                if (cr) *cr = '\0';

                if (trimmed[0] != '#') {
                    strncpy(v.url, trimmed, sizeof(v.url) - 1);
                    int diff = abs(v.height - target_height);
                    if (diff < best_diff || (!found && v.height <= target_height)) {
                        best_diff = diff;
                        best_variant = v;
                        found = 1;
                    }
                }
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(lines);

    if (!found) return -1;

    hls_resolve_url(master_url, best_variant.url, out_url, out_url_size);
    return 0;
}

int hls_fetch_playlist(HttpConn *conn, const Config *cfg, const char *playlist_url,
                       HlsPlaylist *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(HlsPlaylist));

    int ret = http_get_stream(conn, playlist_url, cfg->referer, cfg->agent);
    if (ret < 0) { http_disconnect(conn); return -1; }

    char *resp = (char *)malloc(MAX_RESPONSE);
    if (!resp) { http_disconnect(conn); return -1; }

    int total = 0;
    while (total < MAX_RESPONSE - 1) {
        int n = http_read_stream(conn, (uint8_t *)(resp + total), MAX_RESPONSE - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    http_disconnect(conn);
    resp[total] = '\0';

    strncpy(out->base_url, playlist_url, sizeof(out->base_url) - 1);
    char *last_slash = strrchr(out->base_url, '/');
    if (last_slash) *(last_slash + 1) = '\0';

    char *lines = strdup(resp);
    free(resp);
    if (!lines) return -1;

    char *saveptr = NULL;
    char *line = strtok_r(lines, "\n", &saveptr);

    while (line && out->seg_count < 512) {
        while (*line == '\r' || *line == '\n') line++;
        char trimmed[512];
        strncpy(trimmed, line, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        char *cr = strchr(trimmed, '\r');
        if (cr) *cr = '\0';

        if (strncmp(trimmed, "#EXT-X-TARGETDURATION:", 22) == 0) {
            out->target_duration = atoi(trimmed + 22);
        } else if (strcmp(trimmed, "#EXT-X-ENDLIST") == 0) {
            out->is_endlist = true;
        } else if (trimmed[0] != '#' && trimmed[0] != '\0') {
            hls_resolve_url(out->base_url, trimmed,
                           out->seg_urls[out->seg_count], 512);
            out->seg_count++;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(lines);
    return 0;
}
