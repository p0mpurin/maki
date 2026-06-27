#ifndef PLAYER_UI_H
#define PLAYER_UI_H

#include <stdbool.h>
#include <citro2d.h>

void player_ui_draw(C2D_TextBuf tbuf, const char *title, int episode,
                    int current_seg, int total_segs,
                    bool buffering);

#endif
