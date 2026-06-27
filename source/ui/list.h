#ifndef LIST_H
#define LIST_H

#include <3ds.h>
#include <citro2d.h>

typedef struct {
    char   lines[64][200];
    int    count;
    int    selected;
    int    scroll_offset;
    int    visible_rows;
} ListView;

void list_init(ListView *lv, int visible_rows);
void list_set_items(ListView *lv, const char **items, int count);
void list_handle_input(ListView *lv, u32 keys_down);
void list_draw(ListView *lv, C2D_TextBuf tbuf, float x, float y, float row_h);

#endif
