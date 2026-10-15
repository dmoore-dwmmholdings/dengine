/*
 * brdf_ref.h: a scalar C reference of the BSDF building blocks of shaders/brdf/ (DENG-298), for
 * tests only (test_brdf.c compares the GPU's values with these; it is not part of de_render).
 * The same formulas, in the same order, in `float`, in the local shading frame (Z = the normal):
 * see the Slang files for what each function is.
 */
#ifndef DE_TEST_BRDF_REF_H
#define DE_TEST_BRDF_REF_H

#include <dengine/de_base.h>

typedef struct brdf_v3 {
    float x, y, z;
} brdf_v3;

brdf_v3 brdf_v3_make(float x, float y, float z);
float brdf_dot(brdf_v3 a, brdf_v3 b);
brdf_v3 brdf_normalize(brdf_v3 v);

/* ggx.slang */
float brdf_ggx_d(float alpha, float cos_h);
float brdf_ggx_d_aniso(float ax, float ay, brdf_v3 h);
float brdf_ggx_lambda(float ax, float ay, brdf_v3 w);
float brdf_ggx_g1(float ax, float ay, brdf_v3 w);
float brdf_ggx_g2(float ax, float ay, brdf_v3 wo, brdf_v3 wi);
brdf_v3 brdf_ggx_vndf_sample(float ax, float ay, brdf_v3 wo, float u0, float u1);
float brdf_ggx_vndf_pdf(float ax, float ay, brdf_v3 wo, brdf_v3 h);
float brdf_ggx_reflect_pdf(float ax, float ay, brdf_v3 wo, brdf_v3 wi);
brdf_v3 brdf_reflect(brdf_v3 wo, brdf_v3 h);
float brdf_ggx_specular(float ax, float ay, brdf_v3 wo, brdf_v3 wi);

/* fresnel.slang (one channel) */
float brdf_fresnel_schlick(float f0, float f90, float cos_theta);
float brdf_f0_from_ior(float ior, float outside);
float brdf_fresnel_dielectric(float cos_i, float eta);
float brdf_fresnel_f82_tint(float f0, float tint, float cos_theta);

/* diffuse.slang */
float brdf_diffuse_lambert(void);
float brdf_diffuse_burley(float roughness, float cos_o, float cos_i, float cos_ih);

/* sampling.slang */
brdf_v3 brdf_sample_cosine_hemisphere(float u0, float u1);
float brdf_cosine_hemisphere_pdf(float cos_theta);
brdf_v3 brdf_sample_uniform_sphere(float u0, float u1);
float brdf_radical_inverse2(u32 i);

/* The S13 Standard model of shaders/visbuffer/resolve_model.slang (ResolveStandardModel) for a
 * grey base colour: evaluate (BSDF x cos(theta_i)), pdf. */
float brdf_standard_evaluate(float base, float roughness, float metallic, float specular,
                             brdf_v3 wo, brdf_v3 wi);
float brdf_standard_pdf(float roughness, float metallic, brdf_v3 wo, brdf_v3 wi);

#endif /* DE_TEST_BRDF_REF_H */
