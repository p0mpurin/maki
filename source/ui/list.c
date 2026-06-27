#include "list.h"
#include <string.h>
#include <stdlib.h>

void list_init(ListView *lv, int visible_rows) {
    memset(lv, 0, sizeof(ListView));
    lv->visible_rows = visible_rows > 0 ? visible_rows : 1;
}

void list_set_items(ListView *lv, const char **items, int count) {
    lv->count = count > 64 ? 64 : count;
    int i;
    for (i = 0; i < lv->count; i++) {
        strncpy(lv->lines[i], items[i], 199);
        lv->lines[i][199] = '\0';
    }
    lv->selected = 0;
    lv->scroll_offset = 0;
}

void list_handle_input(ListView *lv, u32 keys_down) {
    if (lv->count == 0) return;

    if (keys_down & KEY_DOWN) {
        if (lv->selected < lv->count - 1) {
            lv->selected++;
            if (lv->selected >= lv->scroll_offset + lv->visible_rows) {
                lv->scroll_offset = lv->selected - lv->visible_rows + 1;
            }
        }
    }

    if (keys_down & KEY_UP) {
        if (lv->selected > 0) {
            lv->selected--;
            if (lv->selected < lv->scroll_offset) {
                lv->scroll_offset = lv->selected;
            }
        }
    }
}

#include "theme.h"

static void draw_row_text_fit(C2D_Text *text, float x, float y, float base_scale,
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

void list_draw(ListView *lv, C2D_TextBuf tbuf, float x, float y, float row_h) {
    if (lv->count == 0) return;

    C2D_TextBufClear(tbuf);

    int i;
    for (i = 0; i < lv->visible_rows; i++) {
        int idx = lv->scroll_offset + i;
        if (idx >= lv->count) break;

        float row_y = y + i * row_h;

        if (idx == lv->selected) {
            C2D_DrawRectSolid(x, row_y, 0, 300.0f, row_h, theme_sel);
            C2D_DrawRectSolid(x, row_y, 0, 4.0f, row_h, theme_accent);
        }

        C2D_Text text;
        C2D_TextParse(&text, tbuf, lv->lines[idx]);
        C2D_TextOptimize(&text);

        draw_row_text_fit(&text, x + 8, row_y + 2, 0.5f, 284.0f,
                          idx == lv->selected ? theme_accent : theme_text);
    }

    // Draw dynamic scrollbar
    if (lv->count > lv->visible_rows) {
        float track_x = x + 302.0f;
        float track_w = 4.0f;
        float total_h = lv->visible_rows * row_h;
        
        // Track background
        C2D_DrawRectSolid(track_x, y, 0, track_w, total_h, C2D_Color32(0x30,0x30,0x30,0xFF));
        
        // Scroll thumb
        float thumb_h = total_h * ((float)lv->visible_rows / lv->count);
        if (thumb_h < 10.0f) thumb_h = 10.0f;
        
        float scrollable_h = total_h - thumb_h;
        float thumb_y = y + (scrollable_h * ((float)lv->scroll_offset / (lv->count - lv->visible_rows)));
        
        C2D_DrawRectSolid(track_x, thumb_y, 0, track_w, thumb_h, theme_accent);
    }
}
