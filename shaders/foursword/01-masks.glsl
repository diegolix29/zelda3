// PASS 1/3 - Maschere, alla risoluzione dello SNES.
// r = quanto il pixel "fa ombra": le zone scure del fondale (chiome, dirupi,
//     contorni) proiettano un'ombra morbida in basso a destra, come in Four
//     Swords Adventures. Sprite e HUD non contano.
// g = quanto il pixel e' chiaro: sorgente dell'alone sulle zone luminose.

#pragma parameter FSA_SHADOW_DARK "Soglia del buio che fa ombra" 0.30 0.05 0.60 0.01
#pragma parameter FSA_BLOOM_THRESHOLD "Soglia alone"             0.80 0.40 1.00 0.02

#include "fsa_common.inc"

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float FSA_SHADOW_DARK;
uniform COMPAT_PRECISION float FSA_BLOOM_THRESHOLD;
#else
#define FSA_SHADOW_DARK 0.30
#define FSA_BLOOM_THRESHOLD 0.80
#endif

void main()
{
    vec4 c = COMPAT_TEXTURE(Source, vTexCoord);
    float layer = floor(c.a * 255.0 + 0.5);
    // 2 = HUD, 3 = fondale (cielo e nuvole dietro la Piramide o sul Monte
    // della Morte), 4 e 6 = sprite: niente ombre da questi (vedi 03-final.glsl)
    float bg = 1.0 - step(1.5, layer) * step(layer, 4.5)
                   - step(5.5, layer) * step(layer, 6.5);
    float luma = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float occluder = (1.0 - smoothstep(FSA_SHADOW_DARK * 0.35, FSA_SHADOW_DARK, luma)) * bg;
    float bright = smoothstep(FSA_BLOOM_THRESHOLD, min(FSA_BLOOM_THRESHOLD + 0.15, 1.0), luma)
                 * (1.0 - step(1.5, layer) * step(layer, 3.5));
    FragColor = vec4(occluder, bright, 0.0, 1.0);
}

#endif
