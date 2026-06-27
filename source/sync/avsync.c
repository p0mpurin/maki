#include "avsync.h"
#include <string.h>
#include <3ds.h>

#define PTS_HZ 90000

void avsync_init(AvSync *av) {
    memset(av, 0, sizeof(AvSync));
}

void avsync_set_base(AvSync *av, uint64_t pts) {
    if (av->base_pts == 0) {
        av->base_pts = pts;
        av->base_tick = svcGetSystemTick();
    }
}

int64_t avsync_video_delay_us(AvSync *av, uint64_t video_pts) {
    if (av->base_pts == 0) return 0;
    if (video_pts == 0) return 0;

    av->last_video_pts = video_pts;

    uint64_t pts_elapsed = video_pts - av->base_pts;
    uint64_t target_us = pts_elapsed * 1000000 / PTS_HZ;

    uint64_t current_tick = svcGetSystemTick();
    uint64_t tick_elapsed = current_tick - av->base_tick;
    uint64_t elapsed_us = tick_elapsed * 1000000 / SYSCLOCK_ARM11;

    if (target_us > elapsed_us) {
        return (int64_t)(target_us - elapsed_us);
    }
    return 0;
}

void avsync_update_audio(AvSync *av, uint64_t audio_pts) {
    if (!av) return;
    av->last_audio_pts = audio_pts;
}
