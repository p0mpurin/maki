#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void trim_whitespace(char *str) {
    char *start = str;
    char *end;
    while (isspace((unsigned char)*start)) start++;
    if (start != str) memmove(str, start, strlen(start) + 1);
    if (*str == 0) return;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
}

static int hex_to_byte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *hex, uint8_t *out, int out_len) {
    int len = (int)strlen(hex);
    int i, j = 0;
    for (i = 0; i < len && j < out_len; i += 2) {
        while (i < len && isspace((unsigned char)hex[i])) i++;
        if (i >= len) break;
        int hi = hex_to_byte(hex[i]);
        if (hi < 0) return -1;
        int lo = 0;
        if (i + 1 < len && !isspace((unsigned char)hex[i+1])) {
            lo = hex_to_byte(hex[i+1]);
            if (lo < 0) return -1;
        }
        out[j++] = (uint8_t)((hi << 4) | lo);
    }
    return j;
}

int config_load(Config *cfg, const char *path) {
    memset(cfg, 0, sizeof(Config));
    strcpy(cfg->api_base, "api.allanime.day");
    cfg->proxy_base[0] = '\0';
    strcpy(cfg->referer, "https://youtu-chan.com");
    strcpy(cfg->agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    strcpy(cfg->xor_key, "56789abc");
    cfg->quality = 360;
    cfg->buffer_seconds = 5;
    cfg->use_mvd = 0;
    strcpy(cfg->player_quality, "best");
    strcpy(cfg->theme, "purple");
    cfg->delete_after_play = 1;
    strcpy(cfg->download_dir, "/3ds/maki");
    cfg->use_rgba_fallback = 0;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    char section[64] = "";
    int error = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        trim_whitespace(p);

        if (*p == ';' || *p == '#' || *p == '\0') continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = '\0';
                strncpy(section, p + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *value = eq + 1;
        trim_whitespace(key);
        trim_whitespace(value);

        if (strcmp(section, "allanime") == 0) {
            if (strcmp(key, "api_base") == 0) strncpy(cfg->api_base, value, sizeof(cfg->api_base) - 1);
            else if (strcmp(key, "referer") == 0) strncpy(cfg->referer, value, sizeof(cfg->referer) - 1);
            else if (strcmp(key, "agent") == 0) strncpy(cfg->agent, value, sizeof(cfg->agent) - 1);
        } else if (strcmp(section, "proxy") == 0) {
            if (strcmp(key, "base_url") == 0) strncpy(cfg->proxy_base, value, sizeof(cfg->proxy_base) - 1);
        } else if (strcmp(section, "crypto") == 0) {
            if (strcmp(key, "xor_key") == 0) strncpy(cfg->xor_key, value, sizeof(cfg->xor_key) - 1);
            else if (strcmp(key, "aes_key") == 0) {
                if (parse_hex(value, cfg->aes_key, 32) != 32) error = 1;
            } else if (strcmp(key, "aes_iv") == 0) {
                if (parse_hex(value, cfg->aes_iv, 16) != 16) error = 1;
            }
        } else if (strcmp(section, "stream") == 0) {
            if (strcmp(key, "quality") == 0) cfg->quality = atoi(value);
            else if (strcmp(key, "buffer_seconds") == 0) cfg->buffer_seconds = atoi(value);
        } else if (strcmp(section, "device") == 0) {
            if (strcmp(key, "use_mvd") == 0) cfg->use_mvd = atoi(value);
        } else if (strcmp(section, "player") == 0) {
            if (strcmp(key, "quality") == 0) strncpy(cfg->player_quality, value, sizeof(cfg->player_quality) - 1);
            else if (strcmp(key, "theme") == 0) strncpy(cfg->theme, value, sizeof(cfg->theme) - 1);
            else if (strcmp(key, "delete_after_play") == 0) cfg->delete_after_play = atoi(value);
            else if (strcmp(key, "download_dir") == 0) strncpy(cfg->download_dir, value, sizeof(cfg->download_dir) - 1);
            else if (strcmp(key, "use_rgba_fallback") == 0) cfg->use_rgba_fallback = atoi(value);
        }
    }

    fclose(f);
    return error ? -1 : 0;
}

void config_save(const Config *cfg, const char *path) {
    FILE *fout = fopen(path, "w");
    if (!fout) return;

    fprintf(fout, "; maki config\n");
    fprintf(fout, "[allanime]\n");
    fprintf(fout, "api_base = %s\n", cfg->api_base);
    fprintf(fout, "referer = %s\n", cfg->referer);
    fprintf(fout, "agent = %s\n\n", cfg->agent);

    fprintf(fout, "[proxy]\n");
    fprintf(fout, "base_url = %s\n\n", cfg->proxy_base);

    fprintf(fout, "[stream]\n");
    fprintf(fout, "quality = %d\n", cfg->quality);
    fprintf(fout, "buffer_seconds = %d\n\n", cfg->buffer_seconds);

    fprintf(fout, "[device]\n");
    fprintf(fout, "use_mvd = %d\n\n", cfg->use_mvd);

    fprintf(fout, "[player]\n");
    fprintf(fout, "quality = 400mq\n");
    fprintf(fout, "theme = %s\n", cfg->theme);
    fprintf(fout, "delete_after_play = %d\n", cfg->delete_after_play);
    fprintf(fout, "download_dir = %s\n", cfg->download_dir);
    fprintf(fout, "use_rgba_fallback = %d\n", cfg->use_rgba_fallback);
    fclose(fout);
}
