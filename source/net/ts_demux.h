#ifndef TS_DEMUX_H
#define TS_DEMUX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t  data[65536];
    int      len;
    uint64_t pts;
    bool     has_pts;
} PesPacket;

typedef void (*pes_cb_fn)(const PesPacket *pkt, void *userdata);

typedef struct {
    uint16_t video_pid;
    uint16_t audio_pid;
    uint16_t pmt_pid;
    pes_cb_fn video_cb;
    pes_cb_fn audio_cb;
    void     *video_userdata;
    void     *audio_userdata;
    uint8_t  _video_pes_buf[262144];
    int      _video_pes_len;
    uint8_t  _audio_pes_buf[65536];
    int      _audio_pes_len;
    int      _video_continuity;
    int      _audio_continuity;
    bool     _pat_done;
} TsDemux;

void ts_demux_init(TsDemux *ctx);
void ts_demux_push(TsDemux *ctx, const uint8_t *data, int len);

#endif
