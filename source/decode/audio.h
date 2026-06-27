#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

int  audio_init(void);
void audio_exit(void);
int  audio_feed_aac(const uint8_t *aac_data, int aac_len, uint64_t pts);

#endif
