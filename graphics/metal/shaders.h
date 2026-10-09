/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_SHADERS_H
#define GRAPHICS_METAL_SHADERS_H

namespace Graphics {
namespace Metal {

static const char *const kShaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;

struct Vertex { packed_float3 position; packed_float2 texcoord; packed_float4 color; };
struct Uniforms {
    float4x4 transform;
    float4 tint;
    float alphaReference;
    uint alphaCompare;
    uint textured;
    uint stippled;
    uint stipple[32];
    float4x4 modelView;
    float4 fogColor;
    float4 fogParameters;
    uint framebufferHeight;
    uint textureMode;
    float lodBias;
    uint posterize;
};
struct Varying { float4 position [[position]]; float2 texcoord; float4 color; float fogDistance; };

vertex Varying scummvmVertex(uint id [[vertex_id]], const device Vertex *vertices [[buffer(0)]],
                            constant Uniforms &uniforms [[buffer(1)]]) {
    Varying result;
    result.position = uniforms.transform * float4(float3(vertices[id].position), 1.0);
    if (uniforms.textureMode == 4) result.position *= vertices[id].texcoord.y;
    result.position.z = (result.position.z + result.position.w) * 0.5;
    result.texcoord = float2(vertices[id].texcoord);
    result.color = float4(vertices[id].color) * uniforms.tint;
    result.fogDistance = abs((uniforms.modelView * float4(float3(vertices[id].position), 1.0)).z);
    if (uniforms.textureMode == 4) result.fogDistance = vertices[id].texcoord.x;
    return result;
}

float4 shadePixel(Varying in, constant Uniforms &uniforms, texture2d<float> image, sampler filtering) {
    float4 color = in.color;
    if (uniforms.stippled) {
        uint x = uint(in.position.x) & 31;
        uint y = (uniforms.framebufferHeight - 1 - uint(in.position.y)) & 31;
        uint offset = y * 4 + x / 8;
        uint bits = (uniforms.stipple[offset / 4] >> ((offset & 3) * 8)) & 255;
        if (!(bits & (1u << (7 - (x & 7))))) discard_fragment();
    }
    if (uniforms.textured) {
        float4 texel = image.sample(filtering, in.texcoord, bias(uniforms.lodBias));
        switch (uniforms.textureMode) {
        case 1: color = float4(texel.rgb * color.a, texel.a); break;
        case 2: color = float4(texel.rgb, color.a); break;
        case 3: color = float4(texel.rgb * color.rgb * color.a, texel.a); break;
        default: color *= texel; break;
        }
    }
    float alpha = color.a, reference = uniforms.alphaReference;
    bool pass = true;
    switch (uniforms.alphaCompare) {
    case 0: pass = false; break;
    case 1: pass = alpha < reference; break;
    case 2: pass = alpha == reference; break;
    case 3: pass = alpha <= reference; break;
    case 4: pass = alpha > reference; break;
    case 5: pass = alpha != reference; break;
    case 6: pass = alpha >= reference; break;
    }
    if (!pass) discard_fragment();
    float mode = uniforms.fogParameters.w;
    if (mode != 0) {
        float distance = in.fogDistance;
        float fog = 1;
        if (mode == 1) fog = (uniforms.fogParameters.z - distance) / (uniforms.fogParameters.z - uniforms.fogParameters.y);
        else if (mode == 2) fog = exp(-uniforms.fogParameters.x * distance);
        else fog = exp(-pow(uniforms.fogParameters.x * distance, 2.0));
        color.rgb = mix(uniforms.fogColor.rgb, color.rgb, clamp(fog, 0.0, 1.0));
    }
    if (uniforms.posterize) color = floor(color * 32.0) / 32.0;
    return color;
}

fragment float4 scummvmFragment(Varying in [[stage_in]], constant Uniforms &uniforms [[buffer(1)]],
                                texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]]) {
    return shadePixel(in, uniforms, image, filtering);
}

struct LogicColor { float4 color [[color(0), raster_order_group(0)]]; };
fragment LogicColor scummvmLogicFragment(Varying in [[stage_in]], constant Uniforms &uniforms [[buffer(1)]],
                                        texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]],
                                        float4 destination [[color(0), raster_order_group(0)]], uint sample [[sample_id]]) {
    uint4 source = uint4(round(clamp(shadePixel(in, uniforms, image, filtering), 0.0, 1.0) * 255.0));
    uint4 stored = uint4(round(clamp(destination, 0.0, 1.0) * 255.0));
    LogicColor result; result.color = float4(source ^ stored) / 255.0;
    return result;
}
)metal";

} // End of namespace Metal
} // End of namespace Graphics

#endif
