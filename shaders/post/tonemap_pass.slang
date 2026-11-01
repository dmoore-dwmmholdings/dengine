import "../tonemap/kernel";
import "../tonemap/agx";
import "../tonemap/aces_fitted";
import "../tonemap/pbr_neutral";
import "../tonemap/reinhard";
import "../tonemap/none";

[shader("compute")] [numthreads(8,8,1)]
void cs_agx(uint3 id : SV_DispatchThreadID, uniform TonemapPassParams pc) {
    de_tonemap_pixel<AgxTonemapper>(id,pc);
}
[shader("compute")] [numthreads(8,8,1)]
void cs_aces(uint3 id : SV_DispatchThreadID, uniform TonemapPassParams pc) {
    de_tonemap_pixel<AcesFittedTonemapper>(id,pc);
}
[shader("compute")] [numthreads(8,8,1)]
void cs_pbr_neutral(uint3 id : SV_DispatchThreadID, uniform TonemapPassParams pc) {
    de_tonemap_pixel<PbrNeutralTonemapper>(id,pc);
}
[shader("compute")] [numthreads(8,8,1)]
void cs_reinhard(uint3 id : SV_DispatchThreadID, uniform TonemapPassParams pc) {
    de_tonemap_pixel<ReinhardTonemapper>(id,pc);
}
[shader("compute")] [numthreads(8,8,1)]
void cs_none(uint3 id : SV_DispatchThreadID, uniform TonemapPassParams pc) {
    de_tonemap_pixel<NoneTonemapper>(id,pc);
}
