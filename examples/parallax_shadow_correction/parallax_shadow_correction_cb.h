#ifndef PARALLAX_SHADOW_CORRECTION_CB_H
#define PARALLAX_SHADOW_CORRECTION_CB_H


struct ParallaxShadowCorrectionConstants
{
    float4      cacheLightDir;
    float4      frameLightDir;

    float4x4    cacheWorldToShadow;
    float4x4    frameWorldToShadow;
};

#endif // PARALLAX_SHADOW_CORRECTION_CB_H