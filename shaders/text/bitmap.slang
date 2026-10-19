// Bitmap glyphs use the per-instance nearest-sampled branch of the shared raster pipeline.
// Keeping one entry pair preserves submission order when bitmap and MSDF glyphs interleave.
#include "msdf.slang"
