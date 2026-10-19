#ifndef DE_GPU_TEXTURE_LEASE_INTERNAL_H
#define DE_GPU_TEXTURE_LEASE_INTERNAL_H
#include <dengine/de_gpu.h>
/* A CPU packet can name a bindless texture before any queue submission exists. A lease
 * keeps that generation and descriptor alive across its owner's destroy call. Release
 * after the packet is abandoned or its GPU fence completes; the device must outlive it. */
bool de__gpu_texture_pin(de_gpu_device *device, de_gpu_texture texture);
void de__gpu_texture_unpin(de_gpu_device *device, de_gpu_texture texture);
#endif
