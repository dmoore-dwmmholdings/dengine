// texture_basis.cpp: the Basis Universal transcoder behind the texture importer's C interface
// (DENG-98; DENG-2 Q1: C++ only behind a C interface inside one module). It exports two C
// functions, de_tex_basis_decode and de_tex_basis_start (texture_internal.h).
//
// A KTX2 file with ETC1S (BasisLZ) or UASTC LDR payloads is transcoded at cook time: level 0 of
// every layer and face to RGBA8 (transcoder_texture_format::cTFRGBA32), which the importer then
// filters and encodes like any other 8-bit source: since DENG-99 the `compression` rules pick BC7
// (or BC1 with compression = "bc1") and the BC encoders write it, so max_size, alpha coverage and
// a full mip chain work for Basis sources too. The transcoder's direct BC targets are not used.
//
// The transcoder allocates with new/malloc (tools only). Its global tables are set up once, by a
// function-local static in de_tex_basis_start (thread-safe in C++11 and later); every call has its
// own ktx2_transcoder, so imports on several threads do not share state.
#include "texture_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <basisu_transcoder.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace {

de_result decode(const de_allocator *a, const void *data, u64 size, de_tex_image *out) {
    if (size > UINT32_MAX)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "KTX2: the file is larger than 4 GiB");
    de_tex_basis_start();
    basist::ktx2_transcoder t;
    if (!t.init(data, static_cast<std::uint32_t>(size)))
        return DE_FAIL(DE_ERR_PARSE, "KTX2: the Basis transcoder refused the file");
    if (!t.is_etc1s() && !t.is_uastc())
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "KTX2: Basis payload format %d is not supported (ETC1S and UASTC LDR 4x4 "
                       "are)",
                       static_cast<int>(t.get_basis_tex_format()));
    if (!t.start_transcoding())
        return DE_FAIL(DE_ERR_PARSE, "KTX2: the Basis payload is damaged (global data)");
    const u32 w = t.get_width(), h = t.get_height();
    const u32 layers = t.get_layers() != 0 ? t.get_layers() : 1u;
    const u32 faces = t.get_faces();
    DE_TRY(de_tex_check_size(w, h, "KTX2"));
    std::memset(out, 0, sizeof *out);
    out->width = w;
    out->height = h;
    out->layers = layers * faces;
    out->channels = t.get_has_alpha() ? 4u : 3u;
    DE_TRY(de_tex_image_alloc(a, out));
    std::vector<std::uint8_t> rgba8(static_cast<std::size_t>(w) * h * 4u);
    for (u32 layer = 0; layer < layers; ++layer)
        for (u32 face = 0; face < faces; ++face) {
            basist::ktx2_image_level_info info;
            if (!t.get_image_level_info(info, 0, layer, face) || info.m_orig_width != w ||
                info.m_orig_height != h) {
                de_tex_image_free(a, out);
                return DE_FAIL(DE_ERR_PARSE, "KTX2: Basis level 0 of layer %u face %u is damaged",
                               layer, face);
            }
            if (!t.transcode_image_level(0, layer, face, rgba8.data(), w * h,
                                         basist::transcoder_texture_format::cTFRGBA32, 0, w, h)) {
                de_tex_image_free(a, out);
                return DE_FAIL(DE_ERR_PARSE,
                               "KTX2: transcoding layer %u face %u failed (damaged payload)", layer,
                               face);
            }
            f32 *dst = out->rgba + (static_cast<u64>(layer) * faces + face) * w * h * 4u;
            for (std::size_t i = 0; i < rgba8.size(); ++i)
                dst[i] = static_cast<f32>(rgba8[i]) / 255.0f;
        }
    return DE_OK;
}

} // namespace

// The one start of the transcoder's global tables. The BC6H encoder (texture_bc_enc.cpp, DENG-99)
// calls it too: two separate statics could run basisu_transcoder_init on two threads at once.
extern "C" void de_tex_basis_start(void) {
    static const bool started = [] {
        basist::basisu_transcoder_init();
        return true;
    }();
    (void)started;
}

extern "C" de_result de_tex_basis_decode(const de_allocator *a, const void *data, u64 size,
                                         de_tex_image *out) {
    try {
        return decode(a, data, size, out);
    } catch (const std::bad_alloc &) {
        de_tex_image_free(a, out);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "KTX2: out of memory in the Basis transcoder");
    } catch (...) {
        de_tex_image_free(a, out);
        return DE_FAIL(DE_ERR_INTERNAL, "KTX2: the Basis transcoder threw an exception");
    }
}
