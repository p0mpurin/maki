#include "ts_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_PACKET_SIZE 188
#define TS_SYNC_BYTE   0x47

void ts_demux_init(TsDemux *ctx) {
    memset(ctx, 0, sizeof(TsDemux));
    ctx->video_pid = 0xFFFF;
    ctx->audio_pid = 0xFFFF;
    ctx->pmt_pid = 0xFFFF;
    ctx->_video_continuity = -1;
    ctx->_audio_continuity = -1;
}

static uint64_t extract_pts(const uint8_t *pes_data, int pes_len) {
    if (pes_len < 14) return 0;
    if (pes_data[0] != 0x00 || pes_data[1] != 0x00 || pes_data[2] != 0x01) return 0;

    uint8_t stream_id = pes_data[3];
    if (stream_id == 0xBC || stream_id == 0xBE || stream_id == 0xBF ||
        stream_id == 0xF0 || stream_id == 0xF1 || stream_id == 0xF2 ||
        stream_id == 0xF8 || stream_id == 0xFF) return 0;

    int header_data_len = pes_data[8];
    if (9 + header_data_len > pes_len) return 0;

    uint8_t pts_dts_flags = (pes_data[7] >> 6) & 0x03;

    if (pts_dts_flags & 0x02) {
        int off = 9;
        if (off + 5 > pes_len) return 0;
        return ((uint64_t)(pes_data[off]     & 0x0E) << 29) |
               ((uint64_t)(pes_data[off + 1]        ) << 22) |
               ((uint64_t)(pes_data[off + 2] & 0xFE) << 14) |
               ((uint64_t)(pes_data[off + 3]        ) <<  7) |
               ((uint64_t)(pes_data[off + 4] & 0xFE) >>  1);
    }

    return 0;
}

static void emit_pes(TsDemux *ctx, uint16_t pid, uint8_t *buf, int len, uint64_t pts) {
    if (len <= 0) return;
    PesPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    if (len > (int)sizeof(pkt.data)) len = (int)sizeof(pkt.data);
    memcpy(pkt.data, buf, len);
    pkt.len = len;
    pkt.pts = pts;
    pkt.has_pts = (pts != 0);

    if (pid == ctx->video_pid && ctx->video_cb) {
        ctx->video_cb(&pkt, ctx->video_userdata);
    } else if (pid == ctx->audio_pid && ctx->audio_cb) {
        ctx->audio_cb(&pkt, ctx->audio_userdata);
    }
}

static void parse_pat(TsDemux *ctx, const uint8_t *payload, int payload_len) {
    if (ctx->_pat_done) return;
    if (payload_len < 8) return;

    int section_len = ((payload[1] & 0x0F) << 8) | payload[2];
    if (section_len + 3 > payload_len) return;

    int pos = 8;
    while (pos + 4 <= section_len + 3) {
        uint16_t program_num = ((uint16_t)payload[pos] << 8) | payload[pos + 1];
        if (program_num != 0) {
            uint16_t pid = ((uint16_t)(payload[pos + 2] & 0x1F) << 8) | payload[pos + 3];
            ctx->pmt_pid = pid;
            ctx->_pat_done = true;
            break;
        }
        pos += 4;
    }
}

static void parse_pmt(TsDemux *ctx, const uint8_t *payload, int payload_len) {
    if (payload_len < 12) return;

    int section_len = ((payload[1] & 0x0F) << 8) | payload[2];
    if (section_len + 3 > payload_len) return;

    int prog_info_len = ((payload[10] & 0x0F) << 8) | payload[11];
    int pos = 12 + prog_info_len;

    while (pos + 5 <= section_len + 3) {
        uint8_t stream_type = payload[pos];
        uint16_t elem_pid = ((uint16_t)(payload[pos + 1] & 0x1F) << 8) | payload[pos + 2];
        int es_info_len = ((payload[pos + 3] & 0x0F) << 8) | payload[pos + 4];

        if (ctx->video_pid == 0xFFFF &&
            (stream_type == 0x1B || stream_type == 0x10 || stream_type == 0x02)) {
            ctx->video_pid = elem_pid;
        }
        if (ctx->audio_pid == 0xFFFF &&
            (stream_type == 0x0F || stream_type == 0x03 || stream_type == 0x04)) {
            ctx->audio_pid = elem_pid;
        }

        pos += 5 + es_info_len;
    }
}

static int find_sync(const uint8_t *data, int len) {
    int i;
    for (i = 0; i < len; i++) {
        if (data[i] == TS_SYNC_BYTE) return i;
    }
    return -1;
}

void ts_demux_push(TsDemux *ctx, const uint8_t *data, int len) {
    static uint8_t packet_buf[TS_PACKET_SIZE * 16];
    static int packet_buf_len = 0;

    if (packet_buf_len + len > (int)sizeof(packet_buf)) {
        int skip = packet_buf_len + len - (int)sizeof(packet_buf) + TS_PACKET_SIZE;
        if (skip > packet_buf_len) {
            packet_buf_len = 0;
            return;
        }
        memmove(packet_buf, packet_buf + skip, packet_buf_len - skip);
        packet_buf_len -= skip;
    }

    memcpy(packet_buf + packet_buf_len, data, len);
    packet_buf_len += len;

    while (packet_buf_len >= TS_PACKET_SIZE) {
        int sync = find_sync(packet_buf, 4);
        if (sync < 0) {
            packet_buf_len = 0;
            return;
        }
        if (sync > 0) {
            memmove(packet_buf, packet_buf + sync, packet_buf_len - sync);
            packet_buf_len -= sync;
        }

        if (packet_buf_len < TS_PACKET_SIZE) break;
        if (packet_buf[0] != TS_SYNC_BYTE) {
            packet_buf_len = 0;
            return;
        }

        uint16_t pid = ((uint16_t)(packet_buf[1] & 0x1F) << 8) | packet_buf[2];
        uint8_t afc = (packet_buf[3] >> 4) & 0x03;
        (void)packet_buf[3];
        int has_payload = afc & 0x01;
        int has_adaptation = afc & 0x02;

        int payload_start = 4;
        if (has_adaptation) {
            int af_len = packet_buf[4];
            if (af_len < 0 || 4 + 1 + af_len > TS_PACKET_SIZE) {
                memmove(packet_buf, packet_buf + TS_PACKET_SIZE, packet_buf_len - TS_PACKET_SIZE);
                packet_buf_len -= TS_PACKET_SIZE;
                continue;
            }
            payload_start = 5 + af_len;
        }

        int payload_len = 0;
        if (has_payload) {
            payload_len = TS_PACKET_SIZE - payload_start;
            if (payload_len < 0) payload_len = 0;
        }

        if (pid == 0x0000 && has_payload) {
            parse_pat(ctx, packet_buf + payload_start, payload_len);
        } else if (pid == ctx->pmt_pid && has_payload && ctx->pmt_pid != 0xFFFF) {
            parse_pmt(ctx, packet_buf + payload_start, payload_len);
        } else if (pid == ctx->video_pid && ctx->video_pid != 0xFFFF) {
            int pusl = (packet_buf[1] & 0x40) ? 1 : 0;

            if (pusl && ctx->_video_pes_len > 0 && has_payload) {
                uint8_t *pes_start = packet_buf + payload_start;
                int pes_len = payload_len;
                int pkt_start = 0;

                if (pes_start[0] == 0x00 && pes_start[1] == 0x00 && pes_start[2] == 0x01) {
                    uint64_t pts_val = extract_pts(pes_start, pes_len);
                    (void)pts_val;
                    int header_len = 9 + pes_start[8];
                    if (header_len > pes_len) header_len = pes_len;
                    pkt_start = header_len;
                    emit_pes(ctx, ctx->video_pid,
                            ctx->_video_pes_buf, ctx->_video_pes_len, 0);
                    ctx->_video_pes_len = 0;
                    memcpy(ctx->_video_pes_buf, pes_start + pkt_start, pes_len - pkt_start);
                    ctx->_video_pes_len = pes_len - pkt_start;
                } else {
                    emit_pes(ctx, ctx->video_pid,
                            ctx->_video_pes_buf, ctx->_video_pes_len, 0);
                    ctx->_video_pes_len = 0;
                    memcpy(ctx->_video_pes_buf, pes_start, pes_len);
                    ctx->_video_pes_len = pes_len;
                }
            } else if (has_payload) {
                int remaining = (int)sizeof(ctx->_video_pes_buf) - ctx->_video_pes_len;
                if (payload_len > remaining) payload_len = remaining;
                memcpy(ctx->_video_pes_buf + ctx->_video_pes_len,
                       packet_buf + payload_start, payload_len);
                ctx->_video_pes_len += payload_len;
            }
        } else if (pid == ctx->audio_pid && ctx->audio_pid != 0xFFFF) {
            int pusl = (packet_buf[1] & 0x40) ? 1 : 0;

            if (pusl && ctx->_audio_pes_len > 0 && has_payload) {
                uint8_t *pes_start = packet_buf + payload_start;
                int pes_len = payload_len;
                int pkt_start = 0;

                if (pes_start[0] == 0x00 && pes_start[1] == 0x00 && pes_start[2] == 0x01) {
                    uint64_t pts_val = extract_pts(pes_start, pes_len);
                    (void)pts_val;
                    int header_len = 9 + pes_start[8];
                    if (header_len > pes_len) header_len = pes_len;
                    pkt_start = header_len;
                    emit_pes(ctx, ctx->audio_pid,
                            ctx->_audio_pes_buf, ctx->_audio_pes_len, pts_val);
                    ctx->_audio_pes_len = 0;
                    memcpy(ctx->_audio_pes_buf, pes_start + pkt_start, pes_len - pkt_start);
                    ctx->_audio_pes_len = pes_len - pkt_start;
                } else {
                    emit_pes(ctx, ctx->audio_pid,
                            ctx->_audio_pes_buf, ctx->_audio_pes_len, 0);
                    ctx->_audio_pes_len = 0;
                    memcpy(ctx->_audio_pes_buf, pes_start, pes_len);
                    ctx->_audio_pes_len = pes_len;
                }
            } else if (has_payload) {
                int remaining = (int)sizeof(ctx->_audio_pes_buf) - ctx->_audio_pes_len;
                if (payload_len > remaining) payload_len = remaining;
                memcpy(ctx->_audio_pes_buf + ctx->_audio_pes_len,
                       packet_buf + payload_start, payload_len);
                ctx->_audio_pes_len += payload_len;
            }
        }

        memmove(packet_buf, packet_buf + TS_PACKET_SIZE, packet_buf_len - TS_PACKET_SIZE);
        packet_buf_len -= TS_PACKET_SIZE;
    }
}
