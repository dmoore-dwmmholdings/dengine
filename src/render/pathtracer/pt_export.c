#include "render/pathtracer/pt_internal.h"
#include "render/renderer_internal.h"
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#include <tinyexr.h>
#include <stdlib.h>
#include <string.h>

de_result de_renderer_export_exr(de_renderer *r, de_view view, const char *vpath,
                                 bool include_aovs) {
    de__pt_info info;
    if (!r || !vpath || !*vpath || !de__pt_get(r, view, &info) || !info.samples)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "EXR export requires a sampled live PT view");
    const u32 pitch = (info.width * 16u + 255u) & ~255u;
    const u64 plane_bytes = (u64)pitch * info.height;
    de_gpu_buffer staging = de_gpu_buffer_create(
        r->dev, &(de_gpu_buffer_desc){.size = plane_bytes * (include_aovs ? 4u : 1u),
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = "Explicit PT EXR readback"});
    if (!staging.id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "EXR staging");
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->dev, DE_QUEUE_GRAPHICS, "Explicit PT EXR export");
    if (!cmd) {
        de_gpu_buffer_destroy(r->dev, staging);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "EXR commands");
    }
    de_gpu_texture textures[] = {info.accumulation, info.albedo, info.normal, info.depth};
    for (u32 i = 0; i < (include_aovs ? 4u : 1u); ++i) {
        de_gpu_barrier b = {
            .texture = textures[i], .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_copy_texture_to_buffer(cmd, staging, i * plane_bytes, pitch, textures[i], 0, 0);
        b.before = DE_ACCESS_COPY_SRC;
        b.after = DE_ACCESS_SHADER_READ;
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    de_result result = de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS,
                                           &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
    if (result != DE_OK) {
        de_gpu_buffer_destroy(r->dev, staging);
        return result;
    }
    de_gpu_wait_idle(r->dev);
    const u8 *mapped = de_gpu_buffer_map(r->dev, staging);
    const u32 channels = include_aovs ? 12u : 4u;
    const u64 count = (u64)info.width * info.height, bytes = count * channels * sizeof(f32);
    f32 *pixels = DE_ALLOC(r->alloc, bytes, 16, DE_MEM_RENDER);
    if (!mapped || !pixels) {
        if (pixels)
            DE_FREE(r->alloc, pixels, bytes);
        de_gpu_buffer_destroy(r->dev, staging);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "EXR planes");
    }
    const char *names[] = {"A",        "B",       "G",        "R",        "albedo.B", "albedo.G",
                           "albedo.R", "depth.Z", "normal.X", "normal.Y", "normal.Z", "samples.Y"};
    unsigned char *images[12];
    EXRChannelInfo channel_info[12];
    int types[12];
    memset(channel_info, 0, sizeof channel_info);
    for (u32 c = 0; c < channels; ++c) {
        images[c] = (unsigned char *)(pixels + c * count);
        memcpy(channel_info[c].name, names[c], strlen(names[c]) + 1);
        types[c] = TINYEXR_PIXELTYPE_FLOAT;
    }
    for (u32 y = 0; y < info.height; ++y)
        for (u32 x = 0; x < info.width; ++x) {
            u64 index = (u64)y * info.width + x;
            const f32 *sum = (const f32 *)(mapped + (u64)y * pitch) + x * 4;
            f32 n = sum[3] > 0 ? sum[3] : 1;
            pixels[index] = sum[3] > 0 ? 1.f : 0.f;
            pixels[count + index] = sum[2] / n;
            pixels[count * 2 + index] = sum[1] / n;
            pixels[count * 3 + index] = sum[0] / n;
            if (include_aovs) {
                const f32 *a = (const f32 *)(mapped + plane_bytes + (u64)y * pitch) + x * 4;
                const f32 *normal =
                    (const f32 *)(mapped + plane_bytes * 2 + (u64)y * pitch) + x * 4;
                const f32 *depth = (const f32 *)(mapped + plane_bytes * 3 + (u64)y * pitch) + x;
                pixels[count * 4 + index] = a[2];
                pixels[count * 5 + index] = a[1];
                pixels[count * 6 + index] = a[0];
                pixels[count * 7 + index] = *depth;
                for (u32 c = 0; c < 3; ++c)
                    pixels[count * (8 + c) + index] = normal[c];
                pixels[count * 11 + index] = sum[3];
            }
        }
    EXRHeader header;
    InitEXRHeader(&header);
    header.num_channels = (int)channels;
    header.channels = channel_info;
    header.pixel_types = types;
    header.requested_pixel_types = types;
    header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
    EXRImage image;
    InitEXRImage(&image);
    image.num_channels = (int)channels;
    image.width = (int)info.width;
    image.height = (int)info.height;
    image.images = images;
    unsigned char *encoded = NULL;
    const char *error = NULL;
    size_t size = SaveEXRImageToMemory(&image, &header, &encoded, &error);
    if (!size) {
        result = DE_FAIL(DE_ERR_IO, "EXR encoding: %s", error ? error : "unknown");
        if (error)
            FreeEXRErrorMessage(error);
    } else {
        de_file *file = NULL;
        result = de_file_open(vpath, DE_FILE_WRITE, &file);
        if (result == DE_OK) {
            result = de_file_write(file, encoded, (u64)size);
            de_file_close(file);
        }
    }
    free(encoded);
    DE_FREE(r->alloc, pixels, bytes);
    de_gpu_buffer_destroy(r->dev, staging);
    return result;
}
#endif
