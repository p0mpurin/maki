#include "video.h"
#include <string.h>
#include <stdlib.h>
#include <3ds.h>

static bool mvd_initialized = false;
static bool config_set = false;
static int src_width = 0;
static int src_height = 0;
static int out_width = 0;
static int out_height = 0;
static Result last_result = 0;
static int frames_ready = 0;
static int frames_rendered = 0;
static uint16_t *mvd_output = NULL;
static MVDSTD_Config mvd_cfg;

int video_init(int width, int height) {
    if (!width || !height) {
        width = 1280;
        height = 720;
    }
    src_width = width;
    src_height = height;
    out_width = width;
    out_height = height;

    Result res = mvdstdInit(MVDMODE_VIDEOPROCESSING,
                             MVD_INPUT_H264,
                             MVD_OUTPUT_BGR565,
                             MVD_DEFAULT_WORKBUF_SIZE,
                             NULL);
    last_result = res;
    if (R_FAILED(res)) return -2;

    mvd_initialized = true;
    frames_ready = 0;
    frames_rendered = 0;
    mvd_output = (uint16_t*)linearMemAlign(out_width * out_height * sizeof(uint16_t), 0x80);
    if (!mvd_output) {
        mvdstdExit();
        mvd_initialized = false;
        return -2;
    }
    return 0;
}

void video_exit(void) {
    if (mvd_initialized) {
        mvdstdExit();
        mvd_initialized = false;
    }
    if (mvd_output) {
        linearFree(mvd_output);
        mvd_output = NULL;
    }
    config_set = false;
    last_result = 0;
}

int video_feed_nal(const uint8_t *nal_data, int nal_len, uint64_t pts) {
    (void)pts;
    if (!mvd_initialized) return -2;
    if (!nal_data || nal_len <= 0) return -1;

    if (!config_set) {
        memset(&mvd_cfg, 0, sizeof(mvd_cfg));
        mvdstdGenerateDefaultConfig(&mvd_cfg, src_width, src_height,
                                     out_width, out_height, NULL, (u32*)mvd_output, NULL);
        MVDSTD_SetConfig(&mvd_cfg);
        config_set = true;
    }

    void *nal_buf = linearAlloc(nal_len);
    if (!nal_buf) return -2;
    memcpy(nal_buf, nal_data, nal_len);
    GSPGPU_FlushDataCache(nal_buf, nal_len);

    MVDSTD_ProcessNALUnitOut out;
    memset(&out, 0, sizeof(out));
    Result res = mvdstdProcessVideoFrame(nal_buf, nal_len, 0, &out);
    linearFree(nal_buf);
    last_result = res;

    if (!MVD_CHECKNALUPROC_SUCCESS(res)) return -2;
    if (res == MVD_STATUS_FRAMEREADY) {
        frames_ready++;
        return 1;
    }
    return 0;
}

void video_blit_frame(void) {
    if (!mvd_initialized || !config_set || !mvd_output) return;
    mvd_cfg.physaddr_outdata0 = (u32)osConvertVirtToPhys(mvd_output);
    Result res = mvdstdRenderVideoFrame(&mvd_cfg, true);
    last_result = res;
    if (res == MVD_STATUS_OK || res == MVD_STATUS_FRAMEREADY) {
        frames_rendered++;
    }
}

void video_blit_top(void) {
    if (!mvd_initialized || !config_set || !mvd_output) return;  
    if (frames_rendered == 0) return;
    u16 fb_w = 0, fb_h = 0;
    uint8_t *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h);
    if (!fb) return;
    int total_pixels = out_width * out_height;
    GSPGPU_InvalidateDataCache(mvd_output, total_pixels * sizeof(uint16_t));
    /* 3DS framebuffer is column-major (rotated 90 deg).
       Physical LCD width = 240 pixels → memory stride = 240.
       MVD output is row-major out_width×out_height.
       Transpose: MVD(vx,vy) -> FB offset = vx*240 + vy */
    for (int vx = 0; vx < out_width; vx++) {
        for (int vy = 0; vy < out_height; vy++) {
            uint16_t p = mvd_output[(out_height - 1 - vy) * out_width + vx];
            int fi = (vx * 240 + vy) * 3;
            uint8_t r = (uint8_t)(((p >> 11) & 0x1F) << 3);
            uint8_t g = (uint8_t)(((p >> 5) & 0x3F) << 2);
            uint8_t b = (uint8_t)((p & 0x1F) << 3);
            fb[fi + 0] = b;
            fb[fi + 1] = g;
            fb[fi + 2] = r;
        }
    }
    GSPGPU_FlushDataCache(fb, fb_w * fb_h * 3);
    gfxScreenSwapBuffers(GFX_TOP, false);
}

void video_update_fb(void) {
    if (!mvd_initialized || !config_set) return;
    mvd_cfg.physaddr_outdata0 = (u32)osConvertVirtToPhys(mvd_output);
    MVDSTD_SetConfig(&mvd_cfg);
}

int video_last_result(void) {
    return (int)last_result;
}

int video_frames_ready(void) {
    return frames_ready;
}

int video_frames_rendered(void) {
    return frames_rendered;
}
