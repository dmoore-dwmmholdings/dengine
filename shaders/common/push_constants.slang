// shaders/common/push_constants.slang: the per-draw push-constant convention (SPEC §12.1,
// DENG-289).
//
// A shader takes its per-draw data as one `uniform` entry-point parameter of at most 128 bytes:
//
//     import push_constants;
//     struct DrawData { uint albedo; uint smp; uint out_buf; float scale; };
//     [shader("compute")] [numthreads(64, 1, 1)]
//     void main(uint3 id : SV_DispatchThreadID, uniform DrawData pc) { ... }
//
// What each backend binds:
//   Vulkan   a push-constant range at offset 0 (all stages).
//   D3D12    root constants at register b0, space 0.
//   Metal    setBytes at buffer index 1 (index 0 is the bindless heap, bindless.slang).
//
// The size is the parameter's layout on each target. The importer fails the cook of an entry point
// whose push constants exceed DE_PUSH_CONSTANT_MAX_BYTES on any target, and records the size in
// the cooked reflection (de_gpu_pipeline_desc.push_constant_bytes). A vertex/pixel pair shares
// one block: declare the same struct in both.
module push_constants;

__exported import bindless;

/// The largest push-constant block, in bytes, on every backend.
public static const uint DE_PUSH_CONSTANT_MAX_BYTES = 128;
