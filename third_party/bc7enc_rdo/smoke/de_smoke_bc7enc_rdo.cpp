// C++ half of the de_tp_bc7enc_rdo smoke test (DENG-99): includes the encoder headers with the
// engine's strict flags and the target's configuration, and gives the C test one function per
// format: encode one 4x4 RGBA8 block, unpack it with the library's own unpacker, and return the
// largest channel error.
#include <bc7decomp.h>
#include <bc7enc.h>
#include <ert.h>
#include <rgbcx.h>

#include <cstdint>
#include <cstdlib>

extern "C" int de_smoke_bc7enc_rdo_bc7(const std::uint8_t *rgba);
extern "C" int de_smoke_bc7enc_rdo_bc1(const std::uint8_t *rgba);
extern "C" int de_smoke_bc7enc_rdo_bc4(const std::uint8_t *rgba);

namespace {

int max_error(const std::uint8_t *a, const std::uint8_t *b, int channels, int stride) {
    int worst = 0;
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < channels; ++c) {
            int d = std::abs(int(a[i * stride + c]) - int(b[i * stride + c]));
            worst = d > worst ? d : worst;
        }
    return worst;
}

} // namespace

extern "C" int de_smoke_bc7enc_rdo_bc7(const std::uint8_t *rgba) {
    bc7enc_compress_block_init();
    bc7enc_compress_block_params p;
    bc7enc_compress_block_params_init(&p);
    std::uint8_t block[16];
    bc7enc_compress_block(block, rgba, &p);
    std::uint8_t out[64];
    if (!bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba *>(out)))
        return 1000;
    return max_error(rgba, out, 4, 4);
}

extern "C" int de_smoke_bc7enc_rdo_bc1(const std::uint8_t *rgba) {
    rgbcx::init();
    std::uint8_t block[8];
    rgbcx::encode_bc1(10, block, rgba, false, false);
    std::uint8_t out[64];
    rgbcx::unpack_bc1(block, out);
    return max_error(rgba, out, 3, 4);
}

extern "C" int de_smoke_bc7enc_rdo_bc4(const std::uint8_t *rgba) {
    rgbcx::init();
    std::uint8_t block[8];
    rgbcx::encode_bc4(block, rgba, 4);
    std::uint8_t out[64] = {};
    rgbcx::unpack_bc4(block, out, 4);
    return max_error(rgba, out, 1, 4);
}
