#ifndef AVSYNC_H
#define AVSYNC_H

#include <stdint.h>

typedef struct {
    uint64_t base_pts;
    uint64_t base_tick;
    uint64_t last_video_pts;
    uint64_t last_audio_pts;
} AvSync;

void    avsync_init(AvSync *av);
void    avsync_set_base(AvSync *av, uint64_t pts);
int64_t avsync_video_delay_us(AvSync *av, uint64_t video_pts);
void    avsync_update_audio(AvSync *av, uint64_t audio_pts);

#endif
