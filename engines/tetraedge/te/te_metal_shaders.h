/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TETRAEDGE_TE_METAL_SHADERS_H
#define TETRAEDGE_TE_METAL_SHADERS_H

namespace Tetraedge {

static const char *const kTeMetalShaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Light {float4 position, direction, ambient, diffuse, specular, attenuation, parameters;};
struct Uniforms {
    float4x4 modelView, projection, normal, texture, projectedTexture;
    float4 globalAmbient, ambient, diffuse, specular, emission;
    Light lights[8];
    float4 materialParameters;
    uint lighting, textured, textureMode, projected;
};
struct Vertex {packed_float3 position, normal; packed_float2 uv; packed_float4 color;};
struct Varying {
    float4 position [[position]];
    float4 uv;
    float4 color;
    float4 flatColor [[flat]];
    float pointSize [[point_size]];
};
vertex Varying teVertex(uint id [[vertex_id]], const device Vertex *vertices [[buffer(0)]], constant Uniforms &u [[buffer(1)]]) {
    const device Vertex &v = vertices[id];
    float4 eye = u.modelView * float4(float3(v.position), 1);
    Varying result;
    result.position = u.projection * eye;
    result.position.z = (result.position.z + result.position.w) * 0.5;
    result.uv = u.projected ? u.projectedTexture * eye : u.texture * float4(float2(v.uv), 0, 1);
    result.pointSize = 1;
    result.color = float4(v.color);
    if (u.lighting) {
        float3 n = (u.normal * float4(float3(v.normal), 0)).xyz;
        float3 color = u.emission.rgb + u.globalAmbient.rgb * u.ambient.rgb;
        for (uint i = 0; i < 8; ++i) {
            constant Light &l = u.lights[i];
            if (l.parameters.w == 0) continue;
            float3 direction = l.position.xyz - eye.xyz * l.position.w;
            float distance = length(direction);
            direction = normalize(direction);
            float attenuation = l.position.w == 0 ? 1 : 1 / max(0.00001, l.attenuation.x + l.attenuation.y*distance + l.attenuation.z*distance*distance);
            if (l.parameters.x != -1) {
                float cosine = dot(-direction, normalize(l.direction.xyz));
                attenuation *= cosine < l.parameters.x ? 0 : pow(max(0.0, cosine), l.parameters.y);
            }
            float diffuse = max(0.0, dot(n, direction));
            float3 halfway = normalize(direction + float3(0,0,1));
            float specular = diffuse > 0 ? pow(max(0.0, dot(n, halfway)), u.materialParameters.x) : 0;
            color += attenuation * (l.ambient.rgb*u.ambient.rgb + l.diffuse.rgb*u.diffuse.rgb*diffuse + l.specular.rgb*u.specular.rgb*specular);
        }
        result.color = float4(clamp(color, 0.0, 1.0), u.diffuse.a);
    }
    result.flatColor = result.color;
    return result;
}
fragment float4 teFragment(Varying in [[stage_in]], constant Uniforms &u [[buffer(1)]], texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]]) {
    float4 color = u.materialParameters.z != 0 ? in.flatColor : in.color;
    if (u.textured) {
        float2 uv = in.uv.xy / in.uv.w;
        if (u.projected) uv.y = 1 - uv.y;
        float4 texel = image.sample(filtering, uv);
        // The original shadow image is LUMINANCE_ALPHA, with red replicated.
        if (u.projected) texel.rgb = float3(texel.r);
        if (u.textureMode == 1) color = float4(mix(color.rgb, float3(0), texel.rgb), color.a * texel.a);
        else if (u.textureMode == 2) color = float4(color.rgb * texel.rgb, 1);
        else color *= texel;
    }
    if (u.materialParameters.w != 0 && color.a <= u.materialParameters.y) discard_fragment();
    return color;
}
)metal";

} // End of namespace Tetraedge

#endif
