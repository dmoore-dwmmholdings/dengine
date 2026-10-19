// Shared environment evaluation/MIS lives in pt_closest_hit.pt_step so the native Metal wrapper
// and native Vulkan/DXR shaders consume the same implementation.
module pt_miss;
__exported import pt_closest_hit;
