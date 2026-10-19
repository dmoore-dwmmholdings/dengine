#include "layout_internal.h"
#include <string.h>
de_result de__text_bidi(const u32 *cp, u32 count, de_text_dir dir, de__bidi *out) {
    memset(out, 0, sizeof *out);
    if (!count)
        return DE_OK;
    SBCodepointSequence sequence = {SBStringEncodingUTF32, (void *)cp, count};
    out->algorithm = SBAlgorithmCreate(&sequence);
    if (!out->algorithm)
        return DE_ERR_OUT_OF_MEMORY;
    SBLevel level = dir == DE_DIR_AUTO ? SBLevelDefaultLTR : dir == DE_DIR_RTL ? 1 : 0;
    out->paragraph = SBAlgorithmCreateParagraph(out->algorithm, 0, count, level);
    if (!out->paragraph) {
        SBAlgorithmRelease(out->algorithm);
        out->algorithm = NULL;
        return DE_ERR_OUT_OF_MEMORY;
    }
    return DE_OK;
}
void de__text_bidi_free(de__bidi *b) {
    if (b->paragraph)
        SBParagraphRelease(b->paragraph);
    if (b->algorithm)
        SBAlgorithmRelease(b->algorithm);
    memset(b, 0, sizeof *b);
}
