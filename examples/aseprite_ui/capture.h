#ifndef ASEPRITE_UI_CAPTURE_H
#define ASEPRITE_UI_CAPTURE_H
#include <dengine/dengine.h>
de_result sprite_ui_capture_init(de_engine *engine);
void sprite_ui_capture_poll(void);
void sprite_ui_capture_shutdown(void);
#endif
