#include "widgets_internal.h"
#include "ui/layout/layout_internal.h"
/* Reuse the loaded-texture intrinsic registry; drawing preserves UV/tint/nine-slice. */
void de_ui__image_register(de_ui *ui) {
    (void)de_ui__register_measure(ui, ui->ids.image, de_ui__measure_image);
}
