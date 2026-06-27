#include "keyboard.h"
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

char *keyboard_get_input(const char *hint) {
    SwkbdState swkbd;
    char buf[256];

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, 255);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "Search", true);

    SwkbdButton btn = swkbdInputText(&swkbd, buf, sizeof(buf));
    if (btn == SWKBD_BUTTON_CONFIRM) {
        return strdup(buf);
    }
    return NULL;
}
