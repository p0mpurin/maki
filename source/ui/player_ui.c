#include "player_ui.h"
#include <stdio.h>
#include <string.h>
#include <3ds.h>

static void draw_text_fit(C2D_Text *text, float x, float y, float base_scale,
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

void player_ui_draw(C2D_TextBuf tbuf, const char *title, int episode,
                    int current_seg, int total_segs,
                    bool buffering) {
    if (!tbuf) return;
    C2D_TextBufClear(tbuf);

    char line_buf[256];
    snprintf(line_buf, sizeof(line_buf), "%s - Ep %d", title, episode);
    C2D_Text title_text;
    C2D_TextParse(&title_text, tbuf, line_buf);
    C2D_TextOptimize(&title_text);
    draw_text_fit(&title_text, 10, 10, 1.0f, 300.0f,
                  C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));

    if (total_segs > 0) {
        float bar_width = 300.0f;
        float bar_height = 8.0f;
        float bar_x = 10.0f;
        float bar_y = 30.0f;

        C2D_DrawRectSolid(bar_x, bar_y, 0, bar_width, bar_height,
                          C2D_Color32(0x40, 0x40, 0x40, 0xFF));

        float progress = (float)current_seg / (float)total_segs;
        if (progress > 1.0f) progress = 1.0f;

        C2D_DrawRectSolid(bar_x, bar_y, 0, bar_width * progress, bar_height,
                          C2D_Color32(0x00, 0x80, 0xFF, 0xFF));

        snprintf(line_buf, sizeof(line_buf), "%d / %d", current_seg, total_segs);
    } else {
        snprintf(line_buf, sizeof(line_buf), "Loading segments...");
    }

    C2D_Text seg_text;
    C2D_TextParse(&seg_text, tbuf, line_buf);
    C2D_TextOptimize(&seg_text);
    C2D_DrawText(&seg_text, C2D_WithColor, 10, 42, 0, 0.5f, 0.5f,
                 C2D_Color32(0xCC, 0xCC, 0xCC, 0xFF));

    if (buffering) {
        C2D_Text buf_text;
        C2D_TextParse(&buf_text, tbuf, "Buffering...");
        C2D_TextOptimize(&buf_text);
        C2D_DrawText(&buf_text, C2D_WithColor, 10, 60, 0, 0.5f, 0.5f,
                     C2D_Color32(0xFF, 0xFF, 0x00, 0xFF));
    }
}
