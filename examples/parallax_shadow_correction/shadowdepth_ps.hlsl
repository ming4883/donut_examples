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

cbuffer c_ForwardView : register(b1 VK_DESCRIPTOR_SET(1))
{
    ForwardShadingViewConstants g_ForwardView;
};

cbuffer c_ForwardLight : register(b2 VK_DESCRIPTOR_SET(1))
{
    ForwardShadingLightConstants g_ForwardLight;
};

Texture2DArray t_ShadowMapArray : register(t10 VK_DESCRIPTOR_SET(2));
TextureCubeArray t_DiffuseLightProbe : register(t11 VK_DESCRIPTOR_SET(2));
TextureCubeArray t_SpecularLightProbe : register(t12 VK_DESCRIPTOR_SET(2));
Texture2D t_EnvironmentBrdf : register(t13 VK_DESCRIPTOR_SET(2));

SamplerState s_ShadowSampler : register(s1 VK_DESCRIPTOR_SET(1));
SamplerState s_LightProbeSampler : register(s2 VK_DESCRIPTOR_SET(2));
SamplerState s_BrdfSampler : register(s3 VK_DESCRIPTOR_SET(2));

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
)
{
    o_color = i_position.zzzz / i_position.w;
}
