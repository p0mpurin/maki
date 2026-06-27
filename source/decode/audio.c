#include "audio.h"
#include "faad_stub.h"
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

#define WAVEBUF_COUNT 4
#define WAVEBUF_SAMPLES 2048

static NeAACDecHandle aac_decoder = NULL;
static bool audio_initialized = false;
static bool aac_decoder_inited = false;
static unsigned long sample_rate = 0;
static unsigned char channels = 0;

static ndspWaveBuf wave_bufs[WAVEBUF_COUNT];
static int    next_wavebuf = 0;

int audio_init(void) {
    if (audio_initialized) return 0;

    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    aac_decoder = NeAACDecOpen();
    if (!aac_decoder) {
        ndspExit();
        return -1;
    }

    NeAACDecConfigurationPtr aac_cfg = NeAACDecGetCurrentConfiguration(aac_decoder);
    aac_cfg->outputFormat = FAAD_FMT_16BIT;
    aac_cfg->defObjectType = LC;
    NeAACDecSetConfiguration(aac_decoder, aac_cfg);

    memset(wave_bufs, 0, sizeof(wave_bufs));
    next_wavebuf = 0;
    audio_initialized = true;

    return 0;
}

void audio_exit(void) {
    if (!audio_initialized) return;

    int i;
    for (i = 0; i < WAVEBUF_COUNT; i++) {
        if (wave_bufs[i].data_vaddr) {
            linearFree((void *)wave_bufs[i].data_vaddr);
            wave_bufs[i].data_vaddr = NULL;
        }
    }

    if (aac_decoder) {
        NeAACDecClose(aac_decoder);
        aac_decoder = NULL;
    }

    ndspExit();
    audio_initialized = false;
    aac_decoder_inited = false;
}

static int get_free_wavebuf(void) {
    int i;
    for (i = 0; i < WAVEBUF_COUNT; i++) {
        int idx = (next_wavebuf + i) % WAVEBUF_COUNT;
        if (wave_bufs[idx].status == NDSP_WBUF_FREE ||
            wave_bufs[idx].status == NDSP_WBUF_DONE) {
            if (wave_bufs[idx].data_vaddr) {
                linearFree((void *)wave_bufs[idx].data_vaddr);
                wave_bufs[idx].data_vaddr = NULL;
            }
            return idx;
        }
    }
    return -1;
}

int audio_feed_aac(const uint8_t *aac_data, int aac_len, uint64_t pts) {
    if (!audio_initialized) return -1;
    if (!aac_data || aac_len <= 0) return 0;

    NeAACDecFrameInfo info;
    memset(&info, 0, sizeof(info));

    if (!aac_decoder_inited) {
        long srate = 0;
        unsigned char ch = 0;
        NeAACDecInit(aac_decoder, (unsigned char *)aac_data, aac_len,
                     (unsigned long *)&srate, &ch);
        if (srate > 0) {
            sample_rate = srate;
            channels = ch;
            ndspChnSetRate(0, (float)sample_rate);
            if (ch == 2) {
                ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
            } else {
                ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
            }
            aac_decoder_inited = true;
        }
    }

    void *pcm = NeAACDecDecode(aac_decoder, &info,
                               (unsigned char *)aac_data, aac_len);
    if (info.error != 0 || !pcm) return -1;
    if (info.samples == 0) return 0;

    int pcm_size = info.samples * 2;
    if (pcm_size <= 0) return 0;

    int idx = get_free_wavebuf();
    if (idx < 0) return 0;

    void *linear_buf = linearAlloc(pcm_size);
    if (!linear_buf) return -1;

    memcpy(linear_buf, pcm, pcm_size);

    memset(&wave_bufs[idx], 0, sizeof(ndspWaveBuf));
    wave_bufs[idx].data_vaddr = linear_buf;
    wave_bufs[idx].nsamples = info.samples / (channels > 0 ? channels : 1);
    wave_bufs[idx].status = NDSP_WBUF_QUEUED;

    ndspChnWaveBufAdd(0, &wave_bufs[idx]);
    next_wavebuf = (idx + 1) % WAVEBUF_COUNT;

    return 0;
}
