/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma pack_matrix(row_major)

#include <donut/shaders/forward_cb.h>
#include <donut/shaders/scene_material.hlsli>
#include <donut/shaders/material_bindings.hlsli>
#include <donut/shaders/forward_vertex.hlsli>
#include <donut/shaders/lighting.hlsli>
#include <donut/shaders/shadows.hlsli>
#include <donut/shaders/vulkan.hlsli>
#include "./parallax_shadow_correction_cb.h"

cbuffer c_ForwardView : register(b1 VK_DESCRIPTOR_SET(1))
{
    ForwardShadingViewConstants g_ForwardView;
};

cbuffer c_ForwardLight : register(b2 VK_DESCRIPTOR_SET(1))
{
    ForwardShadingLightConstants g_ForwardLight;
};

cbuffer c_ParallaxShadowCorrection : register(b3 VK_DESCRIPTOR_SET(1))
{
    ParallaxShadowCorrectionConstants g_ParallaxShadow;
};

/*
Texture2DArray t_ShadowMapArray : register(t10 VK_DESCRIPTOR_SET(2));
TextureCubeArray t_DiffuseLightProbe : register(t11 VK_DESCRIPTOR_SET(2));
TextureCubeArray t_SpecularLightProbe : register(t12 VK_DESCRIPTOR_SET(2));
Texture2D t_EnvironmentBrdf : register(t13 VK_DESCRIPTOR_SET(2));

SamplerState s_ShadowSampler : register(s1 VK_DESCRIPTOR_SET(1));
SamplerState s_LightProbeSampler : register(s2 VK_DESCRIPTOR_SET(2));
SamplerState s_BrdfSampler : register(s3 VK_DESCRIPTOR_SET(2));
*/

Texture2D t_ShadowMap : register(t10 VK_DESCRIPTOR_SET(2));
SamplerState s_ShadowSampler : register(s1 VK_DESCRIPTOR_SET(1));

float3 GetIncidentVector(float4 directionOrPosition, float3 surfacePos)
{
    if (directionOrPosition.w > 0)
        return normalize(surfacePos.xyz - directionOrPosition.xyz);
    else
        return directionOrPosition.xyz;
}

void main(
    in float4 i_position : SV_Position,
    in SceneVertex i_vtx,
    in bool i_isFrontFace : SV_IsFrontFace,
    out float4 o_color : SV_Target0
#if TRANSMISSIVE_MATERIAL
    , out float4 o_backgroundBlendFactor : SV_Target1
#endif
)
{
    MaterialTextureSample textures = SampleMaterialTexturesAuto(i_vtx.texCoord);

    MaterialSample surfaceMaterial = EvaluateSceneMaterial(i_vtx.normal, i_vtx.tangent, g_Material, textures);
    float3 surfaceWorldPos = i_vtx.pos;

    if (!i_isFrontFace)
        surfaceMaterial.shadingNormal = -surfaceMaterial.shadingNormal;

    if (g_Material.domain != MaterialDomain_Opaque)
        clip(surfaceMaterial.opacity - g_Material.alphaCutoff);

    float4 cameraDirectionOrPosition = g_ForwardView.view.cameraDirectionOrPosition;
    float3 viewIncident = GetIncidentVector(cameraDirectionOrPosition, surfaceWorldPos);
    
    float3 diffuseTerm = 0;
    float3 specularTerm = 0;
    
    [loop]
    for(uint nLight = 0; nLight < g_ForwardLight.numLights; nLight++)
    {
        LightConstants light = g_ForwardLight.lights[nLight];

        float3 diffuseRadiance, specularRadiance;
        ShadeSurface(light, surfaceMaterial, surfaceWorldPos, viewIncident, diffuseRadiance, specularRadiance);

        diffuseTerm += (diffuseRadiance) * light.color;
        specularTerm += (specularRadiance) * light.color;
    }

    float4 shadowPos = mul(float4(i_vtx.pos, 1), g_ParallaxShadow.cacheWorldToShadow);
    shadowPos.xyz /= shadowPos.www;

    float4 shadowCasterZ = t_ShadowMap.Sample(s_ShadowSampler, shadowPos.xy * 0.5 + 0.5);
    float shadowTerm = shadowCasterZ.x < (shadowPos.z - 1e-5) ? 0 : 1;
    diffuseTerm.xyz *= shadowTerm;
    specularTerm.xyz *= shadowTerm;
    
    {
        float3 ambientColor = lerp(g_ForwardLight.ambientColorBottom.rgb, g_ForwardLight.ambientColorTop.rgb, surfaceMaterial.shadingNormal.y * 0.5 + 0.5);

        diffuseTerm += ambientColor * surfaceMaterial.diffuseAlbedo * surfaceMaterial.occlusion;
        specularTerm += ambientColor * surfaceMaterial.specularF0 * surfaceMaterial.occlusion;
    }

    
#if TRANSMISSIVE_MATERIAL
    
    // See https://github.com/KhronosGroup/glTF/blob/master/extensions/2.0/Khronos/KHR_materials_transmission/README.md#transmission-btdf
    float dielectricFresnel = 0.5;
    float backgroundScalar = surfaceMaterial.transmission
        * (1.0 - dielectricFresnel);

    if (g_Material.domain == MaterialDomain_TransmissiveAlphaBlended)
        backgroundScalar *= (1.0 - surfaceMaterial.opacity);
    
    o_backgroundBlendFactor.rgb = backgroundScalar;
    o_backgroundBlendFactor.a = 1.0;

#endif // TRANSMISSIVE_MATERIAL

    o_color.rgb = diffuseTerm + specularTerm;
    o_color.a = surfaceMaterial.opacity;


}
