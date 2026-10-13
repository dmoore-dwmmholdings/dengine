// The one translation unit that holds the tinyexr implementation (de_tp_tinyexr, DENG-98).
//
// TINYEXR_USE_STB_ZLIB declares stbi_zlib_decode_buffer (stb_image, de_tp_stb) and
// stbi_zlib_compress (stb_image_write). The engine only decodes EXR, and stb_image_write may join
// de_tp_stb later (DENG-327), so the compressor is renamed here and defined as a stub that fails:
// a SaveEXR* call returns an error instead of clashing with, or needing, stb_image_write.
#define stbi_zlib_compress de_tp_tinyexr_zlib_compress_unsupported
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

extern "C" unsigned char *de_tp_tinyexr_zlib_compress_unsupported(unsigned char *data, int data_len,
                                                                  int *out_len, int quality) {
    (void)data;
    (void)data_len;
    (void)quality;
    if (out_len != nullptr)
        *out_len = 0;
    return nullptr;
}
