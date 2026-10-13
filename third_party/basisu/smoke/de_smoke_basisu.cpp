// C++ half of the de_tp_basisu smoke test (DENG-98): includes <basisu_transcoder.h> with the
// engine's strict flags and the target's configuration, and gives the C test two functions.
#include <basisu_transcoder.h>

#include <cstddef>
#include <cstdint>

extern "C" int de_smoke_basisu_start(void);
extern "C" int de_smoke_basisu_ktx2_init(const void *data, std::size_t size);

// Starts the transcoder; 1 when it was built with KTX2 and zstd support.
extern "C" int de_smoke_basisu_start(void) {
    basist::basisu_transcoder_init();
    return basist::basisu_transcoder_supports_ktx2() && basist::basisu_transcoder_supports_ktx2_zstd()
               ? 1
               : 0;
}

// 1 when the KTX2 transcoder accepts the file header.
extern "C" int de_smoke_basisu_ktx2_init(const void *data, std::size_t size) {
    basist::ktx2_transcoder t;
    return t.init(data, static_cast<std::uint32_t>(size)) ? 1 : 0;
}
