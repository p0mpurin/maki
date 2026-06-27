#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <3ds.h>
#include <citro2d.h>
#include <sys/stat.h>
#include <dirent.h>

#include "config/config.h"
#include "net/http.h"
#include "net/graphql.h"
#include "ui/keyboard.h"
#include "ui/list.h"
#include "util/cjson.h"
#include "decode/video.h"

#include "ui/theme.h"

typedef enum {
    STATE_SEARCH, STATE_RESULTS, STATE_EPISODE_SELECT,
    STATE_PREPARE, STATE_POLL, STATE_STREAM,
    STATE_DOWNLOAD_AUDIO, STATE_PLAYING, STATE_CLEANUP, STATE_CLEAN_FILES,
    STATE_SETTINGS
} AppState;

static Config cfg;
static AnimeResult results[10]; static int result_count=0;
static Episode episodes[512]; static int episode_count=0;
static char sel_id[64], sel_name[256];
static int current_episode=1;
static ListView search_results_list, episode_list, settings_list;
static char *result_strings[10], *episode_strings[512], *settings_strings[5];

#define LOG_LINES 14
#define LOG_LINE_LEN 128
static C3D_RenderTarget *top_target=NULL,*bot_target=NULL;
static C2D_Font sys_font=NULL;
static C2D_TextBuf search_tbuf=NULL,list_tbuf=NULL,top_tbuf=NULL;
static char log_buf[LOG_LINES][LOG_LINE_LEN]; static int log_head=0;

static void log_add(const char *fmt, ...) {
    va_list a;va_start(a,fmt);vsnprintf(log_buf[log_head],LOG_LINE_LEN,fmt,a);va_end(a);
    log_head=(log_head+1)%LOG_LINES;
}

static void settings_update_strings(void) {
    for (int i = 0; i < 5; i++) {
        if (settings_strings[i]) free(settings_strings[i]);
    }
    
    char buf[128];
    snprintf(buf, sizeof(buf), "Theme: %s", cfg.theme);
    settings_strings[0] = strdup(buf);
    
    snprintf(buf, sizeof(buf), "Clean on exit: %s", cfg.delete_after_play ? "Yes" : "No");
    settings_strings[1] = strdup(buf);
    
    snprintf(buf, sizeof(buf), "Hardware MVD: %s", cfg.use_mvd ? "Yes" : "No");
    settings_strings[2] = strdup(buf);
    
    snprintf(buf, sizeof(buf), "Buffer Length: %d sec", cfg.buffer_seconds);
    settings_strings[3] = strdup(buf);

    snprintf(buf, sizeof(buf), "RGBA Fallback: %s", cfg.use_rgba_fallback ? "Yes" : "No");
    settings_strings[4] = strdup(buf);
    
    list_set_items(&settings_list, (const char**)settings_strings, 5);
}
static void draw_fit_text(C2D_Text *text, float x, float y, float base_scale,
                          float max_width, u32 color);

static void draw_bottom_chrome(const char *title, const char *legend) {
    if (!bot_target) return;
    
    // Draw top header panel background
    C2D_DrawRectSolid(0, 0, 0, 320.0f, 30.0f, theme_sel);
    // Draw header border line
    C2D_DrawRectSolid(0, 29.0f, 0, 320.0f, 1.0f, theme_accent);
    
    // Draw header title
    if (search_tbuf) {
        C2D_TextBufClear(search_tbuf);
        C2D_Text t;
        C2D_TextParse(&t, search_tbuf, title);
        C2D_TextOptimize(&t);
        draw_fit_text(&t, 10, 6, 0.65f, 300.0f, theme_accent);
    }
    
    // Draw bottom legend panel background
    C2D_DrawRectSolid(0, 210.0f, 0, 320.0f, 30.0f, theme_sel);
    // Draw legend border line
    C2D_DrawRectSolid(0, 210.0f, 0, 320.0f, 1.0f, theme_accent);
    // Accent footer bar
    C2D_DrawRectSolid(0, 237.0f, 0, 320.0f, 3.0f, theme_accent);
    
    // Draw legend text
    if (legend && search_tbuf) {
        C2D_Text t;
        C2D_TextParse(&t, search_tbuf, legend);
        C2D_TextOptimize(&t);
        draw_fit_text(&t, 10, 216, 0.55f, 300.0f, theme_text);
    }
}

static void draw_fit_text(C2D_Text *text, float x, float y, float base_scale,
                          float max_width, u32 color) {
    float w = 0.0f, h = 0.0f;
    float scale = base_scale;
    C2D_TextGetDimensions(text, scale, scale, &w, &h);
    if (w > max_width && w > 0.0f) {
        scale *= max_width / w;
        if (scale < 0.34f) scale = 0.34f;
    }
    C2D_DrawText(text, C2D_WithColor, x, y, 0, scale, scale, color);
}

static void draw_top_log(void) {
    if(!top_tbuf)return;C2D_TextBufClear(top_tbuf);
    C2D_Text t;C2D_TextParse(&t,top_tbuf,"maki");C2D_TextOptimize(&t);
    C2D_DrawText(&t,C2D_WithColor,4,4,0,0.6f,0.6f,C2D_Color32(0x00,0xCC,0xFF,0xFF));
    for(int i=0;i<LOG_LINES;i++){int idx=(log_head-1-i+LOG_LINES)%LOG_LINES;if(!log_buf[idx][0])continue;
        C2D_TextParse(&t,top_tbuf,log_buf[idx]);C2D_TextOptimize(&t);
        C2D_DrawText(&t,C2D_WithColor,4,20.0f+i*15.0f,0,0.45f,0.45f,C2D_Color32(0xAA,0xAA,0xAA,0xFF));}
}

static char job_id[64]={0};
static int video_size=0, audio_size=0;
static int audio_rate=22050, audio_channels=1;
static char dl_video[256]={0}, dl_audio[256]={0};
static float poll_progress=0, visual_progress=0;
static char poll_state[32]={0};
static int meta_fps=24, meta_srate=22050, meta_channels=1;

static bool mvd_ready=false;
static FILE *h264_fp=NULL;
static bool video_streaming=false;
static bool video_eof=false;
static HttpConn vconn;
static uint8_t h264_parse_buf[1024*1024]; static int h264_parse_len=0;
static int v_decoded=0, v_displayed=0, v_dropped=0;

static bool audio_ready=false;
static bool paused=false;
static bool dsp_ok=false;
static ndspWaveBuf wbufs[2];
static int wbuf_next=0;

static httpcContext audio_ctx;
static FILE *audio_dfp = NULL;
static u32 audio_dl_total = 0;
static u32 audio_dl_downloaded = 0;
static uint8_t *audio_dl_buf = NULL;
static bool audio_dl_started = false;

#define AUDIO_DL_CHUNK_SIZE (256 * 1024)
#define AUDIO_DL_MAX_CHUNKS_PER_FRAME 8
#define AUDIO_DL_FRAME_BUDGET_TICKS (SYSCLOCK_ARM11 / 20)
static int16_t *wbuf_data[2]={0};
/* pre-allocated stereo buffers — allocated once, reused forever */
static int16_t *abuf_stereo[2]={0};
static int16_t *abuf_mono=NULL;
static FILE *pcm_fp=NULL;
static u64 audio_samples_queued=0, audio_samples_played=0;
static u64 next_frame_tick=0;

static void audio_download_cancel(void) {
    if (audio_dl_buf) {
        free(audio_dl_buf);
        audio_dl_buf = NULL;
    }
    if (audio_dfp) {
        fclose(audio_dfp);
        audio_dfp = NULL;
    }
    if (audio_dl_started) {
        httpcCloseContext(&audio_ctx);
    }
    audio_dl_started = false;
    audio_dl_total = 0;
    audio_dl_downloaded = 0;
    if (dl_audio[0]) {
        remove(dl_audio);
        dl_audio[0] = '\0';
    }
}

static bool mvd_feed_nal(const uint8_t *nal, int nlen) {
    if (!nal || nlen <= 0 || nlen + 3 > 256*1024) return false;
    uint8_t *buf = (uint8_t*)linearAlloc(nlen + 3);
    if (!buf) return false;
    memcpy(buf, "\x00\x00\x01", 3);
    memcpy(buf + 3, nal, nlen);
    int ret = video_feed_nal(buf, nlen + 3, 0);
    linearFree(buf);
    if (ret > 0) {
        v_decoded++;
        return true;
    }
    return false;
}

static int h264_find_start_code(const uint8_t *buf, int pos, int len, int *sc_len) {
    for (int i = pos; i + 3 < len; i++) {
        if (buf[i] == 0 && buf[i+1] == 0) {
            if (buf[i+2] == 1) { *sc_len = 3; return i; }
            if (i + 4 <= len && buf[i+2] == 0 && buf[i+3] == 1) { *sc_len = 4; return i; }
        }
    }
    return -1;
}

static void h264_parser_reset(void) {
    h264_parse_len = 0;
    v_decoded = v_displayed = v_dropped = 0;
}

static bool h264_parser_decode_available(bool flush) {
    int sc_len = 0;
    int first = h264_find_start_code(h264_parse_buf, 0, h264_parse_len, &sc_len);
    if (first < 0) return false;
    if (first > 0) {
        memmove(h264_parse_buf, h264_parse_buf + first, h264_parse_len - first);
        h264_parse_len -= first;
        first = 0;
    }

    while (1) {
        int nal_start = first + sc_len;
        int next_sc_len = 0;
        int next = h264_find_start_code(h264_parse_buf, nal_start, h264_parse_len, &next_sc_len);
        if (next < 0) {
            if (flush && h264_parse_len > nal_start) {
                bool rendered = mvd_feed_nal(h264_parse_buf + nal_start, h264_parse_len - nal_start);
                h264_parse_len = 0;
                return rendered;
            }
            return false;
        }
        bool rendered = false;
        if (next > nal_start) rendered = mvd_feed_nal(h264_parse_buf + nal_start, next - nal_start);
        memmove(h264_parse_buf, h264_parse_buf + next, h264_parse_len - next);
        h264_parse_len -= next;
        first = 0;
        sc_len = next_sc_len;
        if (rendered) return true;
    }
}

static bool h264_decode_next_frame(FILE *fp) {
    if (!fp) return false;
    for (int tries = 0; tries < 64; tries++) {
        if (h264_parser_decode_available(false)) return true;
        if (feof(fp)) return h264_parser_decode_available(true);
        if (h264_parse_len + 4096 > (int)sizeof(h264_parse_buf)) h264_parse_len = 0;
        int n = (int)fread(h264_parse_buf + h264_parse_len, 1, 4096, fp);
        if (n <= 0) return h264_parser_decode_available(true);
        h264_parse_len += n;
    }
    return false;
}

/* streaming variants — read from HTTP connections instead of files */
static bool h264_decode_next_stream(bool *eof) {
    if (!video_streaming) return false;
    for (int tries = 0; tries < 64; tries++) {
        if (h264_parser_decode_available(false)) return true;
        if (h264_parse_len + 4096 > (int)sizeof(h264_parse_buf)) h264_parse_len = 0;
        int n = http_read_chunked(&vconn, h264_parse_buf + h264_parse_len, 4096);
        if (n == -2 || n < 0) {
            *eof = true; video_streaming = false; return h264_parser_decode_available(true);
        }
        if (n == 0) return false;
        h264_parse_len += n;
    }
    return false;
}

static int audio_get_free_wb(void) {
    for (int i = 0; i < 2; i++) {
        int idx = (wbuf_next + i) % 2;
        if (wbufs[idx].status == NDSP_WBUF_FREE || wbufs[idx].status == NDSP_WBUF_DONE) {
            return idx;
        }
    }
    return -1;
}

static void audio_service(void) {
    if (!audio_ready || !pcm_fp) return;

    int idx = audio_get_free_wb();
    if (idx < 0) return;

    if (feof(pcm_fp)) return;

    /* read mono PCM into pre-allocated buffer, expand to stereo in-place */
    int nread = (int)fread(abuf_mono, 2, 4096, pcm_fp);
    if (nread <= 0) return;

    int16_t *stereo = abuf_stereo[idx];
    for (int i = 0; i < nread; i++) {
        stereo[i*2]     = abuf_mono[i];
        stereo[i*2 + 1] = abuf_mono[i];
    }

    memset(&wbufs[idx], 0, sizeof(ndspWaveBuf));
    wbufs[idx].data_vaddr = stereo;
    wbufs[idx].nsamples = nread;
    wbuf_data[idx] = stereo;
    DSP_FlushDataCache(stereo, nread * 4);
    ndspChnWaveBufAdd(0, &wbufs[idx]);
    wbuf_next = (idx + 1) % 2;
    audio_samples_queued += nread;
}

int main(void) {
    gfxInitDefault();
    gfxSetWide(false);
    if(!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)){gfxExit();return -1;}
    if(!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)){C3D_Fini();gfxExit();return -1;}
    C2D_Prepare();svcSleepThread(100000000);
    top_target=C2D_CreateScreenTarget(GFX_TOP,GFX_LEFT);
    bot_target=C2D_CreateScreenTarget(GFX_BOTTOM,GFX_LEFT);
    sys_font=C2D_FontLoadSystem(CFG_REGION_USA);
    search_tbuf=C2D_TextBufNew(512);list_tbuf=C2D_TextBufNew(4096);top_tbuf=C2D_TextBufNew(4096);
    for(int i=0;i<10;i++)result_strings[i]=NULL;
    for(int i=0;i<512;i++)episode_strings[i]=NULL;
    log_add("maki ready");
    config_load(&cfg,"/3ds/maki/config.ini");
    theme_apply(cfg.theme);
    log_add("proxy=%s",cfg.proxy_base[0]?cfg.proxy_base:"(none)");
    Result ndsp_rc = ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    if (R_SUCCEEDED(ndsp_rc)) {
        struct stat st;
        dsp_ok = stat("/3ds/dspfirm.cdc", &st) == 0;
    }
    if (dsp_ok) {
        log_add("dsp: ok");
    } else {
        log_add("dsp: NO FIRMWARE - /3ds/dspfirm.cdc missing");
        log_add("Use Luma Rosalina -> Dump DSP firmware");
    }
    {
        Handle fsu; if(R_SUCCEEDED(srvGetServiceHandle(&fsu,"fs:USER"))){
            FS_Archive sdmc; if(R_SUCCEEDED(FSUSER_OpenArchive(&sdmc,ARCHIVE_SDMC,fsMakePath(PATH_EMPTY,"")))){
                FSUSER_CreateDirectory(sdmc,fsMakePath(PATH_ASCII,"/3ds/maki"),0);
                FSUSER_CloseArchive(sdmc);
            }
            svcCloseHandle(fsu);
        }
    }
    memset(wbufs,0,sizeof(wbufs)); memset(wbuf_data,0,sizeof(wbuf_data));
    list_init(&search_results_list,20);list_init(&episode_list,20);
    list_init(&settings_list, 10);
    for(int i=0;i<5;i++) settings_strings[i]=NULL;
    settings_update_strings();
    AppState state=STATE_SEARCH;
    static int http_retries=0;
    static u64 retry_deadline=0;

    while(aptMainLoop()){
        hidScanInput();u32 kdown=hidKeysDown();
        if(kdown&KEY_START)break;
        if (state == STATE_POLL) {
            visual_progress += (poll_progress - visual_progress) * 0.05f;
            if (visual_progress > 0.99f) visual_progress = 0.99f;
        }
        switch(state){
        case STATE_SEARCH:
            if(kdown&KEY_A){char*q=keyboard_get_input("Search...");if(q){
                log_add("search: %s",q);int n=graphql_search(NULL,&cfg,q,results,10);
                log_add("results: %d %s",n,n<=0?http_last_error():"");
                if(n>0){result_count=n;for(int i=0;i<n;i++)result_strings[i]=strdup(results[i].name);
                    list_set_items(&search_results_list,(const char**)result_strings,n);state=STATE_RESULTS;}
                free(q);
            }}
            if(kdown&KEY_Y)state=STATE_CLEAN_FILES;
            if(kdown&KEY_X)state=STATE_SETTINGS;
            break;
        case STATE_RESULTS:
            list_handle_input(&search_results_list,kdown);
            if((kdown&KEY_A)&&result_count>0){int idx=search_results_list.selected;if(idx<result_count){
                strncpy(sel_id,results[idx].id,63);strncpy(sel_name,results[idx].name,255);
                int n=graphql_episodes(NULL,&cfg,sel_id,episodes,512);
                if(n>0){episode_count=n;for(int i=0;i<n;i++){char b[64]={0};
                    strncat(b,"Ep ",62);strncat(b,episodes[i].number,63-strlen(b));episode_strings[i]=strdup(b);}
                    list_set_items(&episode_list,(const char**)episode_strings,n);state=STATE_EPISODE_SELECT;}
            }}if(kdown&KEY_B)state=STATE_SEARCH;break;
        case STATE_EPISODE_SELECT:
            list_handle_input(&episode_list,kdown);
            /* auto-retry on 500 errors */
            if (http_retries > 0 && svcGetSystemTick() >= retry_deadline && episode_count > 0 && cfg.proxy_base[0]) {
                int idx = episode_list.selected; if (idx < episode_count) goto do_prepare;
            }
            if((kdown&KEY_A)&&episode_count>0&&cfg.proxy_base[0]){int idx=episode_list.selected;if(idx<episode_count){
                http_retries = 0; /* fresh attempt */
                do_prepare:
                current_episode=idx+1;log_add("prepare ep %s",episodes[idx].number);
                char body[512];snprintf(body,sizeof(body),"{\"id\":\"%s\",\"episode\":\"%s\",\"quality\":\"400mq\"}",sel_id,episodes[idx].number);
                char url[1024];snprintf(url,sizeof(url),"%s/prepare",cfg.proxy_base);
                char *rj=(char*)malloc(65536);if(rj){int rl=httpc_post_json(url,cfg.agent,body,rj,65536);if(rl>0){
                    cJSON *root=cJSON_Parse(rj);if(root){
                        cJSON *jid=cJSON_GetObjectItem(root,"job_id");
                         if(jid&&cJSON_IsString(jid)){
                            strncpy(job_id,jid->valuestring,sizeof(job_id)-1);
                            log_add("job=%s",job_id);
                            poll_progress = 0.0f; visual_progress = 0.0f;
                            state=STATE_POLL;
                         }
                        cJSON_Delete(root);
                    }
                }else{
                    log_add("prepare fail: %s",http_last_error());
                    if (http_retries < 2 && strstr(http_last_error(), "500")) {
                        http_retries++; retry_deadline = svcGetSystemTick() + SYSCLOCK_ARM11;
                        log_add("retry %d/2...", http_retries);
                    } else { http_retries=0; }
                }free(rj);}
            }}if(kdown&KEY_B)state=STATE_RESULTS;break;
        case STATE_POLL: {
            static u64 last_poll=0;u64 now=svcGetSystemTick();
             if(now-last_poll > (SYSCLOCK_ARM11 * 3) / 2){
                last_poll=now;
                char url[1024];snprintf(url,sizeof(url),"%s/status/%s",cfg.proxy_base,job_id);
                char *rj=(char*)malloc(8192);if(rj){int rl=httpc_get(url,cfg.agent,rj,8192);if(rl>0){
                    cJSON *root=cJSON_Parse(rj);if(root){
                        cJSON *st=cJSON_GetObjectItem(root,"state");
                        cJSON *pr=cJSON_GetObjectItem(root,"progress");
                        cJSON *sz=cJSON_GetObjectItem(root,"size");
                        cJSON *as=cJSON_GetObjectItem(root,"audio_size");
                        cJSON *ar=cJSON_GetObjectItem(root,"audio_rate");
                        cJSON *ac=cJSON_GetObjectItem(root,"audio_channels");
                        if(st&&cJSON_IsString(st)){
                            strncpy(poll_state,st->valuestring,sizeof(poll_state)-1);
                            poll_progress=pr&&cJSON_IsNumber(pr)?(float)pr->valuedouble:0;
                            if((!strcmp(poll_state,"ready")||!strcmp(poll_state,"streaming"))&&as&&cJSON_IsNumber(as)&&as->valuedouble>0){
                                video_size=sz&&cJSON_IsNumber(sz)?(int)sz->valuedouble:0;
                                audio_size=as&&cJSON_IsNumber(as)?(int)as->valuedouble:0;
                                audio_rate=ar&&cJSON_IsNumber(ar)?(int)ar->valuedouble:22050;
                                audio_channels=ac&&cJSON_IsNumber(ac)?(int)ac->valuedouble:1;
                                state=STATE_DOWNLOAD_AUDIO;
                                log_add("->DL AUDIO %dMB",audio_size/1048576);
                            }else if(!strcmp(poll_state,"error")){
                                cJSON *msg=cJSON_GetObjectItem(root,"message");
                                log_add("job err: %s",msg&&cJSON_IsString(msg)?msg->valuestring:"?"); state=STATE_EPISODE_SELECT;
                            }
                        }
                        cJSON_Delete(root);
                    }
                } else {
                    log_add("poll fail: %s", http_last_error());
                }free(rj);}
            }
            if(kdown&KEY_B){state=STATE_EPISODE_SELECT;}
        }break;
        case STATE_STREAM: {
            char url[1024];
            snprintf(url,sizeof(url),"%s/cache/%s/video.h264",cfg.proxy_base,job_id);
            if (http_get_stream(&vconn, url, cfg.proxy_base, cfg.agent) == 0) {
                video_streaming = true;
                video_eof = false;
                http_chunked_reset();
                state = STATE_PLAYING;
                next_frame_tick = 0;
            } else {
                log_add("video stream err: %s", http_last_error());
                if (dl_audio[0]) remove(dl_audio); dl_audio[0] = '\0';
                state = STATE_EPISODE_SELECT;
            }
        }break;
        case STATE_DOWNLOAD_AUDIO: {
            if (kdown & KEY_B) {
                audio_download_cancel();
                log_add("audio download cancelled");
                state = STATE_EPISODE_SELECT;
                break;
            }
            if (!audio_dl_started) {
                // Initialize HTTPC if needed
                static bool httpc_inited = false;
                if (!httpc_inited) {
                    httpcInit(0x100000);
                    httpc_inited = true;
                }
                
                snprintf(dl_audio, sizeof(dl_audio), "/3ds/maki/%s_%d.pcm", sel_id, current_episode);
                char url[1024];
                snprintf(url, sizeof(url), "%s/cache/%s/audio.pcm", cfg.proxy_base, job_id);
                log_add("dl audio start...");
                
                Result r = httpcOpenContext(&audio_ctx, HTTPC_METHOD_GET, url, 0);
                if (R_FAILED(r)) {
                    log_add("audio open fail: 0x%08lx", r);
                    audio_download_cancel();
                    state = STATE_EPISODE_SELECT;
                    break;
                }
                audio_dl_started = true;
                
                httpcAddRequestHeaderField(&audio_ctx, "User-Agent", cfg.agent);
                httpcAddRequestHeaderField(&audio_ctx, "Connection", "close");
                
                r = httpcBeginRequest(&audio_ctx);
                if (R_FAILED(r)) {
                    log_add("audio begin fail: 0x%08lx", r);
                    audio_download_cancel();
                    state = STATE_EPISODE_SELECT;
                    break;
                }
                
                u32 status = 0;
                r = httpcGetResponseStatusCode(&audio_ctx, &status);
                if (R_FAILED(r) || status < 200 || status >= 300) {
                    log_add("audio status: %lu", status);
                    audio_download_cancel();
                    state = STATE_EPISODE_SELECT;
                    break;
                }
                
                u32 downloadSize = 0;
                httpcGetDownloadSizeState(&audio_ctx, &downloadSize, &audio_dl_total);
                if (audio_dl_total == 0) audio_dl_total = audio_size; // fallback to status info
                
                audio_dfp = fopen(dl_audio, "wb");
                if (!audio_dfp) {
                    log_add("audio fopen fail");
                    audio_download_cancel();
                    state = STATE_EPISODE_SELECT;
                    break;
                }
                
                audio_dl_buf = (uint8_t*)malloc(AUDIO_DL_CHUNK_SIZE);
                if (!audio_dl_buf) {
                    log_add("audio malloc fail");
                    audio_download_cancel();
                    state = STATE_EPISODE_SELECT;
                    break;
                }
                
                audio_dl_downloaded = 0;
            }
            
            Result r = HTTPC_RESULTCODE_DOWNLOADPENDING;
            u64 burst_start = svcGetSystemTick();
            for (int burst = 0; burst < AUDIO_DL_MAX_CHUNKS_PER_FRAME; burst++) {
                u32 chunk_size = 0;
                r = httpcDownloadData(&audio_ctx, audio_dl_buf, AUDIO_DL_CHUNK_SIZE, &chunk_size);
                if (chunk_size > 0) {
                    fwrite(audio_dl_buf, 1, chunk_size, audio_dfp);
                    audio_dl_downloaded += chunk_size;
                }

                if (r != HTTPC_RESULTCODE_DOWNLOADPENDING) break;
                if (chunk_size == 0) break;
                if (svcGetSystemTick() - burst_start >= AUDIO_DL_FRAME_BUDGET_TICKS) break;
            }
            
            if (r != HTTPC_RESULTCODE_DOWNLOADPENDING) {
                // Done!
                free(audio_dl_buf); audio_dl_buf = NULL;
                fclose(audio_dfp); audio_dfp = NULL;
                httpcCloseContext(&audio_ctx);
                audio_dl_started = false;
                
                if (audio_dl_downloaded > 0) {
                    log_add("audio ok: %lu", audio_dl_downloaded);
                    state = STATE_STREAM;
                } else {
                    log_add("audio empty");
                    remove(dl_audio); dl_audio[0] = '\0';
                    state = STATE_EPISODE_SELECT;
                }
            }
        }break;
        case STATE_PLAYING: {
            if(kdown&KEY_B)state=STATE_CLEANUP;
            if(kdown&KEY_SELECT) paused = !paused;
        }break;
        case STATE_CLEANUP: {
            mvd_ready=false; video_exit();
            if(h264_fp){fclose(h264_fp);h264_fp=NULL;}
            if(video_streaming){http_disconnect(&vconn);video_streaming=false;}
            audio_ready=false;
            ndspChnWaveBufClear(0);
            if(pcm_fp){fclose(pcm_fp);pcm_fp=NULL;}
            video_eof = false;
            if (abuf_mono) { free(abuf_mono); abuf_mono = NULL; }
            for (int i = 0; i < 2; i++) {
                if (abuf_stereo[i]) { linearFree(abuf_stereo[i]); abuf_stereo[i] = NULL; }
                wbuf_data[i] = NULL;
            }
            memset(wbufs,0,sizeof(wbufs));
            audio_samples_queued=0;audio_samples_played=0;
            if(cfg.delete_after_play && dl_audio[0]) { remove(dl_audio); }
            if(dl_video[0]) { remove(dl_video); }
            dl_video[0]=dl_audio[0]='\0'; paused=false; state=STATE_EPISODE_SELECT;
        }break;
        case STATE_CLEAN_FILES: {
            log_add("cleaning /3ds/maki/...");
            DIR *d = opendir("/3ds/maki");
            int removed = 0;
            if (d) {
                struct dirent *e;
                char path[256];
                while ((e = readdir(d)) != NULL) {
                    if (e->d_name[0] == '.') continue;
                    int len = strlen(e->d_name);
                    if ((len > 5 && !strcmp(e->d_name + len - 5, ".h264")) ||
                        (len > 4 && !strcmp(e->d_name + len - 4, ".pcm"))) {
                        snprintf(path, sizeof(path), "/3ds/maki/%s", e->d_name);
                        if (remove(path) == 0) removed++;
                    }
                }
                closedir(d);
            }
            log_add("removed %d episode files", removed);
            state = STATE_SEARCH;
        }break;
        case STATE_SETTINGS:
            list_handle_input(&settings_list, kdown);
            if (kdown & (KEY_A | KEY_RIGHT | KEY_LEFT)) {
                int idx = settings_list.selected;
                if (idx == 0) { // Theme
                    const char *theme_names[] = {"purple", "blue", "green", "red", "pink"};
                    int cur_idx = 0;
                    for (int i = 0; i < 5; i++) {
                        if (!strcmp(cfg.theme, theme_names[i])) { cur_idx = i; break; }
                    }
                    int dir = (kdown & KEY_LEFT) ? -1 : 1;
                    int next_idx = (cur_idx + dir + 5) % 5;
                    strncpy(cfg.theme, theme_names[next_idx], sizeof(cfg.theme) - 1);
                    theme_apply(cfg.theme);
                    log_add("theme: %s", cfg.theme);
                }
                else if (idx == 1) { // Delete after play
                    cfg.delete_after_play = !cfg.delete_after_play;
                }
                else if (idx == 2) { // MVD Hardware
                    cfg.use_mvd = !cfg.use_mvd;
                }
                else if (idx == 3) { // Buffer Seconds
                    int dir = (kdown & KEY_LEFT) ? -1 : 1;
                    cfg.buffer_seconds += dir;
                    if (cfg.buffer_seconds < 1) cfg.buffer_seconds = 10;
                    if (cfg.buffer_seconds > 10) cfg.buffer_seconds = 1;
                }
                else if (idx == 4) { // RGBA Fallback
                    cfg.use_rgba_fallback = !cfg.use_rgba_fallback;
                }
                
                settings_update_strings();
                config_save(&cfg, "/3ds/maki/config.ini");
            }
            if (kdown & KEY_B) {
                state = STATE_SEARCH;
            }
            break;
        }

        if (state == STATE_PLAYING) {
            if (!audio_ready && dl_audio[0] && dsp_ok) {
                struct stat st;
                if (stat(dl_audio, &st) == 0) {
                    log_add("pcm: %lld bytes", st.st_size);
                    pcm_fp = fopen(dl_audio, "rb");
                    if (pcm_fp) {
                        abuf_mono = (int16_t*)malloc(4096 * sizeof(int16_t));
                        abuf_stereo[0] = (int16_t*)linearMemAlign(4096 * 2 * sizeof(int16_t), 0x80);
                        abuf_stereo[1] = (int16_t*)linearMemAlign(4096 * 2 * sizeof(int16_t), 0x80);
                        if (!abuf_mono || !abuf_stereo[0] || !abuf_stereo[1]) {
                            log_add("audio alloc fail");
                            if (abuf_mono) { free(abuf_mono); abuf_mono = NULL; }
                            if (abuf_stereo[0]) { linearFree(abuf_stereo[0]); abuf_stereo[0] = NULL; }
                            if (abuf_stereo[1]) { linearFree(abuf_stereo[1]); abuf_stereo[1] = NULL; }
                            fclose(pcm_fp); pcm_fp = NULL;
                        } else {
                            float mix[12]; memset(mix, 0, sizeof(mix));
                            mix[0] = 1.0f; mix[1] = 1.0f;
                            ndspChnReset(0);
                            ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
                            ndspChnSetRate(0, (float)audio_rate);
                            ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
                            ndspChnSetMix(0, mix);
                            ndspChnSetPaused(0, false);
                            audio_ready = true;
                            log_add("audio ready");
                        }
                    }
                } else log_add("pcm not found");
            }
            /* video: from network stream */
            if (!mvd_ready && video_streaming) {
                if (video_init(400, 240) >= 0) {
                    mvd_ready = true; h264_parser_reset();
                    log_add("video ready (stream)");
                } else log_add("MVD fail");
            }
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        if (state == STATE_PLAYING) {
            /* audio: mark done buffers, then fill from stream or file */
            /* audio: mark done buffers (lightweight, always run) */
            if (audio_ready) {
                for (int i = 0; i < 2; i++) {
                    ndspWaveBuf *w = &wbufs[i];
                    if (w->status == NDSP_WBUF_DONE) {
                        audio_samples_played += w->nsamples;
                        w->status = NDSP_WBUF_FREE;
                    }
                }
            }
            /* video: decode from stream/file */
            if (mvd_ready) {
                u64 now = svcGetSystemTick();
                u64 fps_interval = SYSCLOCK_ARM11 / meta_fps;
                if (next_frame_tick == 0) next_frame_tick = now;
                if (now >= next_frame_tick) {
                    while (now >= next_frame_tick + fps_interval) {
                        next_frame_tick += fps_interval;
                        v_dropped++;
                    }
                    bool decoded = false;
                    if (!paused) {
                        if (video_streaming) {
                            if (!video_eof) decoded = h264_decode_next_stream(&video_eof);
                        }
                    }
                    if (decoded) { video_blit_frame(); next_frame_tick += fps_interval; }
                    else if (paused) { next_frame_tick = now + fps_interval; }
                }
            }
            if (audio_ready && !paused && pcm_fp) {
                audio_service();
            }
            v_displayed = v_decoded;
        } else if (top_target) {
            if (state >= STATE_PREPARE && state < STATE_PLAYING) {
                C2D_TargetClear(top_target, theme_bg);
                C2D_SceneBegin(top_target);
                C2D_TextBufClear(top_tbuf);
                C2D_Text t;
                C2D_TextParse(&t, top_tbuf, "MAKI PLAYER");
                C2D_TextOptimize(&t);
                C2D_DrawText(&t, C2D_WithColor, 20, 20, 0, 1.2f, 1.2f, theme_accent);

                char ep_title[256];
                snprintf(ep_title, sizeof(ep_title), "Playing: %s - Episode %d", sel_name, current_episode);
                C2D_TextParse(&t, top_tbuf, ep_title);
                C2D_TextOptimize(&t);
                C2D_DrawText(&t, C2D_WithColor, 20, 70, 0, 0.7f, 0.7f, theme_text);

                char status_msg[128];
                if (state == STATE_PREPARE) snprintf(status_msg, sizeof(status_msg), "Contacting proxy server...");
                else if (state == STATE_POLL) snprintf(status_msg, sizeof(status_msg), "Transcoding video stream... %d%%", (int)(visual_progress*100));
                else if (state == STATE_DOWNLOAD_AUDIO) {
                    u32 total_size = audio_dl_total > 0 ? audio_dl_total : (u32)audio_size;
                    if (total_size > 0) {
                        snprintf(status_msg, sizeof(status_msg), "Downloading audio... %d%% (%.1f/%.1f MB)",
                            (int)((audio_dl_downloaded / (float)total_size) * 100),
                            audio_dl_downloaded / (1024.0f * 1024.0f),
                            total_size / (1024.0f * 1024.0f));
                    } else {
                        snprintf(status_msg, sizeof(status_msg), "Downloading audio... %.1f MB", audio_dl_downloaded / (1024.0f * 1024.0f));
                    }
                }
                else if (state == STATE_STREAM) snprintf(status_msg, sizeof(status_msg), "Buffering video player...");
                else snprintf(status_msg, sizeof(status_msg), "Loading...");

                C2D_TextParse(&t, top_tbuf, status_msg);
                C2D_TextOptimize(&t);
                C2D_DrawText(&t, C2D_WithColor, 20, 110, 0, 0.65f, 0.65f, theme_accent);

                // Draw progress bar
                float bar_width = 360.0f;
                float bar_height = 8.0f;
                float bar_x = 20.0f;
                float bar_y = 150.0f;
                C2D_DrawRectSolid(bar_x, bar_y, 0, bar_width, bar_height, C2D_Color32(0x40,0x40,0x40,0xFF));
                
                float progress = 0.0f;
                if (state == STATE_POLL) progress = visual_progress;
                else if (state == STATE_DOWNLOAD_AUDIO) {
                    u32 total_size = audio_dl_total > 0 ? audio_dl_total : (u32)audio_size;
                    if (total_size > 0) progress = audio_dl_downloaded / (float)total_size;
                }
                else if (state == STATE_STREAM) progress = 0.95f;
                
                if (progress > 0.0f) {
                    C2D_DrawRectSolid(bar_x, bar_y, 0, bar_width * progress, bar_height, theme_accent);
                }
            } else {
                C2D_TargetClear(top_target, theme_bg);
                C2D_SceneBegin(top_target);
                C2D_TextBufClear(top_tbuf);
                C2D_Text t;
                C2D_TextParse(&t, top_tbuf, "MAKI - ANIME STREAMER");
                C2D_TextOptimize(&t);
                C2D_DrawText(&t, C2D_WithColor, 10, 10, 0, 0.8f, 0.8f, theme_accent);
                
                for(int i=0;i<LOG_LINES;i++){
                    int idx=(log_head-1-i+LOG_LINES)%LOG_LINES;
                    if(!log_buf[idx][0])continue;
                    C2D_TextParse(&t,top_tbuf,log_buf[idx]);C2D_TextOptimize(&t);
                    C2D_DrawText(&t,C2D_WithColor,10,35.0f+i*14.0f,0,0.45f,0.45f,theme_text);
                }
            }
        }
        /* during PLAYING before first frame, show black with log */
        if (state == STATE_PLAYING && top_target && v_displayed == 0) {
            C2D_TargetClear(top_target, C2D_Color32(0x00,0x00,0x00,0xFF));
            C2D_SceneBegin(top_target);
            draw_top_log();
        }
        if(bot_target){C2D_TargetClear(bot_target,theme_bg);C2D_SceneBegin(bot_target);}

        /* ── SEARCH ─────────────────────────────────────────── */
        if(state==STATE_SEARCH){
            draw_bottom_chrome("maki  |  Anime for 3DS", "[A] Search  [X] Settings  [Y] Clean");
            if(search_tbuf){
                C2D_TextBufClear(search_tbuf);
                C2D_Text t;
                // Big centred prompt
                C2D_TextParse(&t,search_tbuf,"Press A to search for anime");C2D_TextOptimize(&t);
                C2D_DrawText(&t,C2D_WithColor,10,80,0,0.62f,0.62f,theme_text);
                C2D_TextParse(&t,search_tbuf,"__________________________________");C2D_TextOptimize(&t);
                C2D_DrawText(&t,C2D_WithColor,10,97,0,0.5f,0.5f,theme_accent);
            }
        }

        /* ── RESULTS ─────────────────────────────────────────── */
        else if(state==STATE_RESULTS){
            draw_bottom_chrome("Search Results", "[A] Select  [B] Back");
            if(list_tbuf) list_draw(&search_results_list,list_tbuf,6,35,22);
        }

        /* ── EPISODE SELECT ──────────────────────────────────── */
        else if(state==STATE_EPISODE_SELECT){
            draw_bottom_chrome(sel_name[0]?sel_name:"Episode Select", "[A] Play  [B] Back");
            if(list_tbuf) list_draw(&episode_list,list_tbuf,6,35,22);
        }

        /* ── SETTINGS ────────────────────────────────────────── */
        else if(state==STATE_SETTINGS){
            draw_bottom_chrome("Settings", "[A/L/R] Change  [B] Back");
            if(list_tbuf) list_draw(&settings_list,list_tbuf,6,35,22);
        }

        /* ── PREPARE / POLL / DOWNLOAD / STREAM ─────────────── */
        else if(state==STATE_PREPARE||state==STATE_POLL||
                state==STATE_DOWNLOAD_AUDIO||state==STATE_STREAM){
            if(search_tbuf && bot_target){
                C2D_TextBufClear(search_tbuf); C2D_Text t;

                /* ---- header panel ---- */
                C2D_DrawRectSolid(0,0,0,320.0f,30.0f,theme_sel);
                C2D_DrawRectSolid(0,29.0f,0,320.0f,1.0f,theme_accent);
                {
                    char hdr[128];
                    if(state==STATE_PREPARE)        snprintf(hdr,sizeof(hdr),"Preparing episode...");
                    else if(state==STATE_POLL)      snprintf(hdr,sizeof(hdr),"Transcoding video");
                    else if(state==STATE_DOWNLOAD_AUDIO) snprintf(hdr,sizeof(hdr),"Downloading audio");
                    else                            snprintf(hdr,sizeof(hdr),"Starting stream...");
                    C2D_TextParse(&t,search_tbuf,hdr);C2D_TextOptimize(&t);
                    C2D_DrawText(&t,C2D_WithColor,10,7,0,0.6f,0.6f,theme_accent);
                }

                /* ---- show/episode subtitle ---- */
                {
                    char sub[128];
                    snprintf(sub,sizeof(sub),"%s  Ep %d",sel_name,current_episode);
                    C2D_TextParse(&t,search_tbuf,sub);C2D_TextOptimize(&t);
                    draw_fit_text(&t,10,40,0.5f,300.0f,theme_text);
                }

                /* ---- progress bar ---- */
                float progress=0.0f;
                if(state==STATE_POLL) progress=visual_progress;
                else if(state==STATE_DOWNLOAD_AUDIO){
                    u32 total_size=audio_dl_total>0?audio_dl_total:(u32)audio_size;
                    if(total_size>0) progress=audio_dl_downloaded/(float)total_size;
                }
                else if(state==STATE_STREAM) progress=0.97f;

                float bar_x=10.0f, bar_y=95.0f, bar_w=260.0f, bar_h=12.0f;
                // Track shadow
                C2D_DrawRectSolid(bar_x,bar_y+2,0,bar_w,bar_h,C2D_Color32(0x10,0x10,0x10,0xFF));
                // Track background
                C2D_DrawRectSolid(bar_x,bar_y,0,bar_w,bar_h,C2D_Color32(0x30,0x30,0x30,0xFF));
                // Fill
                if(progress>0.0f)
                    C2D_DrawRectSolid(bar_x,bar_y,0,bar_w*progress,bar_h,theme_accent);
                // Percentage text
                {
                    char pct_str[16];
                    snprintf(pct_str,sizeof(pct_str),"%d%%",(int)(progress*100));
                    C2D_TextParse(&t,search_tbuf,pct_str);C2D_TextOptimize(&t);
                    draw_fit_text(&t,bar_x+bar_w+6,bar_y-1,0.55f,44.0f,theme_accent);
                }

                /* ---- status text ---- */
                {
                    char status[128];
                    if(state==STATE_POLL){
                        snprintf(status,sizeof(status),"FFmpeg: %s",poll_state);
                    } else if(state==STATE_DOWNLOAD_AUDIO){
                        u32 total_size=audio_dl_total>0?audio_dl_total:(u32)audio_size;
                        if(total_size>0)
                            snprintf(status,sizeof(status),"%.1f / %.1f MB  (%.0f%%)",
                                audio_dl_downloaded/(1024.0f*1024.0f),
                                total_size/(1024.0f*1024.0f),
                                progress*100.0f);
                        else
                            snprintf(status,sizeof(status),"%.1f MB downloaded",
                                audio_dl_downloaded/(1024.0f*1024.0f));
                    } else if(state==STATE_STREAM){
                        snprintf(status,sizeof(status),"Buffering video stream...");
                    } else {
                        snprintf(status,sizeof(status),"Requesting from server...");
                    }
                    C2D_TextParse(&t,search_tbuf,status);C2D_TextOptimize(&t);
                    draw_fit_text(&t,bar_x,bar_y+20,0.5f,300.0f,theme_text);
                }

                /* ---- step indicators (dots) ---- */
                // Step 1: Transcode   Step 2: Audio   Step 3: Play
                const char *steps[3]={"Transcode","Audio","Play"};
                int cur_step = (state==STATE_POLL)?0:(state==STATE_DOWNLOAD_AUDIO)?1:2;
                for(int s=0;s<3;s++){
                    float sx=10.0f+s*100.0f, sy=145.0f;
                    u32 col=(s<=cur_step)?theme_accent:C2D_Color32(0x50,0x50,0x50,0xFF);
                    // Dot
                    C2D_DrawRectSolid(sx,sy,0,8.0f,8.0f,col);
                    // Connector
                    if(s<2) C2D_DrawRectSolid(sx+8,sy+3,0,92.0f,2.0f,
                        (s<cur_step)?theme_accent:C2D_Color32(0x40,0x40,0x40,0xFF));
                    // Label
                    C2D_TextParse(&t,search_tbuf,steps[s]);C2D_TextOptimize(&t);
                    C2D_DrawText(&t,C2D_WithColor,sx,sy+12,0,0.42f,0.42f,col);
                }

                /* ---- footer ---- */
                C2D_DrawRectSolid(0,210.0f,0,320.0f,1.0f,theme_accent);
                C2D_DrawRectSolid(0,237.0f,0,320.0f,3.0f,theme_accent);
                C2D_DrawRectSolid(0,210.0f,0,320.0f,30.0f,theme_sel);
                C2D_TextParse(&t,search_tbuf,"[B] Cancel");C2D_TextOptimize(&t);
                C2D_DrawText(&t,C2D_WithColor,10,216,0,0.55f,0.55f,theme_text);
            }
        }

        /* ── PLAYING ─────────────────────────────────────────── */
        else if(state==STATE_PLAYING && search_tbuf){
            C2D_TextBufClear(search_tbuf); C2D_Text t;
            // Dark bottom bar
            C2D_DrawRectSolid(0,200.0f,0,320.0f,40.0f,theme_sel);
            C2D_DrawRectSolid(0,199.0f,0,320.0f,1.0f,theme_accent);
            C2D_DrawRectSolid(0,237.0f,0,320.0f,3.0f,theme_accent);
            if(!dsp_ok){
                C2D_TextParse(&t,search_tbuf,"DSP firmware missing!");C2D_TextOptimize(&t);
                C2D_DrawText(&t,C2D_WithColor,10,40,0,0.65f,0.65f,C2D_Color32(0xFF,0x44,0x44,0xFF));
                C2D_TextParse(&t,search_tbuf,"Dump via Luma Rosalina");C2D_TextOptimize(&t);
                C2D_DrawText(&t,C2D_WithColor,10,60,0,0.5f,0.5f,theme_text);
            } else {
                int a_sec=(int)(audio_samples_played/(float)audio_rate);
                // Now-playing label at top
                C2D_DrawRectSolid(0,0,0,320.0f,25.0f,theme_sel);
                C2D_DrawRectSolid(0,25.0f,0,320.0f,1.0f,theme_accent);
                char ep_label[128];
                snprintf(ep_label,sizeof(ep_label),"%s  |  Ep %d",sel_name,current_episode);
                C2D_TextParse(&t,search_tbuf,ep_label);C2D_TextOptimize(&t);
                draw_fit_text(&t,10,5,0.5f,300.0f,theme_text);
                // Playback state icon + timestamp
                char time_str[64];
                snprintf(time_str,sizeof(time_str),"%s  %d:%02d",
                    paused?"[PAUSED]":"[PLAYING]",a_sec/60,a_sec%60);
                C2D_TextParse(&t,search_tbuf,time_str);C2D_TextOptimize(&t);
                C2D_DrawText(&t,C2D_WithColor,10,100,0,0.65f,0.65f,paused?C2D_Color32(0xFF,0xCC,0x00,0xFF):theme_accent);
                // Debug stats (smaller, dimmer)
                char dbg[64];
                snprintf(dbg,sizeof(dbg),"V:%d/%d  R:%08x",v_displayed,v_decoded,(unsigned int)video_last_result());
                C2D_TextParse(&t,search_tbuf,dbg);C2D_TextOptimize(&t);
                C2D_DrawText(&t,C2D_WithColor,10,125,0,0.42f,0.42f,C2D_Color32(0x70,0x70,0x70,0xFF));
                // Controls hint in footer
                C2D_TextParse(&t,search_tbuf,"[SELECT] Pause/Resume    [B] Stop");C2D_TextOptimize(&t);
                draw_fit_text(&t,10,207,0.52f,300.0f,theme_text);
            }
        }

        /* ── CLEANUP / CLEAN FILES ───────────────────────────── */
        else if(state==STATE_CLEANUP||state==STATE_CLEAN_FILES){
            draw_bottom_chrome("Cleaning up...", "Please wait");
            if(search_tbuf){
                C2D_TextBufClear(search_tbuf);C2D_Text t;
                const char *msg=(state==STATE_CLEANUP)?"Removing temp files...":"Clearing /3ds/maki/...";
                C2D_TextParse(&t,search_tbuf,msg);C2D_TextOptimize(&t);
                C2D_DrawText(&t,C2D_WithColor,10,90,0,0.55f,0.55f,theme_text);
            }
        }
        C3D_FrameEnd(0);
        if (state == STATE_PLAYING) video_blit_top();
    }

    video_exit();ndspExit();
    for(int i=0;i<10;i++)free(result_strings[i]);
    for(int i=0;i<512;i++)free(episode_strings[i]);
    for(int i=0;i<5;i++) { if(settings_strings[i]) free(settings_strings[i]); }
    if(search_tbuf)C2D_TextBufDelete(search_tbuf);if(list_tbuf)C2D_TextBufDelete(list_tbuf);
    if(top_tbuf)C2D_TextBufDelete(top_tbuf);if(sys_font)C2D_FontFree(sys_font);
    if(top_target)C3D_RenderTargetDelete(top_target);if(bot_target)C3D_RenderTargetDelete(bot_target);
    C2D_Fini();C3D_Fini();gfxExit();return 0;
}
