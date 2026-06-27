#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

typedef struct {
    char api_base[128];
    char proxy_base[256];
    char referer[256];
    char agent[256];
    char xor_key[256];
    uint8_t aes_key[32];
    uint8_t aes_iv[16];
    int  quality;
    int  buffer_seconds;
    int  use_mvd;
    char player_quality[16];
    char theme[16];
    int  delete_after_play;
    char download_dir[128];
    int  use_rgba_fallback;
} Config;

int config_load(Config *cfg, const char *path);
void config_save(const Config *cfg, const char *path);

#endif
