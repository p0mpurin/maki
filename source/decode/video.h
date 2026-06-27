#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

int  video_init(int width, int height);
void video_exit(void);
int  video_feed_nal(const uint8_t *nal_data, int nal_len, uint64_t pts);
void video_blit_frame(void);
void video_blit_top(void);
void video_update_fb(void);
int  video_last_result(void);
int  video_frames_ready(void);
int  video_frames_rendered(void);

#endif
