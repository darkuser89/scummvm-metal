/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STARK_GFX_METAL_SHADERS_H
#define STARK_GFX_METAL_SHADERS_H

namespace Stark {
namespace Gfx {

static const char *const kMetalShaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;

struct Light {float4 position, direction, color, parameters;};
struct Uniforms {
    float4x4 modelView, projection, normal;
    Light lights[10];
    float4 boneRotation[70], bonePosition[70];
    float4 ambient;
};
struct Material {
    float4 color, shadowDirection;
    uint textured, doubleSided;
    float fadeLevel;
    uint padding;
};
struct ActorVertex {
    packed_float3 position1, position2;
    float bone1, bone2, weight;
    packed_float3 normal;
    packed_float2 texcoord;
};
struct PropVertex {packed_float3 position, normal, texcoord;};
struct SurfaceVertex {packed_float3 position; packed_float2 texcoord; packed_float4 color;};
struct Varying {float4 position [[position]]; float2 texcoord; float4 color;};

float3 qrot(float4 q, float3 v) {return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);}
float4 clipPosition(float4 p) {p.z = (p.z + p.w) * 0.5; return p;}
float3 lighting(float3 eyePosition, float3 eyeNormal, constant Uniforms &u) {
    float3 result = u.ambient.rgb;
    for (uint i = 0; i < 10; ++i) {
        constant Light &l = u.lights[i];
        int type = int(l.position.w);
        if (type == 2) result += l.color.rgb * max(0.0, dot(eyeNormal, -l.direction.xyz));
        else if (type == 1 || type == 4) {
            float3 toLight = l.position.xyz - eyePosition;
            float dist = length(toLight);
            toLight = normalize(toLight);
            float attenuation = clamp((l.parameters.y - dist) / max(0.001, l.parameters.y - l.parameters.x), 0.0, 1.0);
            float incidence = max(0.0, dot(eyeNormal, toLight));
            float cone = 1;
            if (type == 4) cone = clamp((max(0.0, dot(toLight, -l.direction.xyz)) - l.parameters.z) / max(0.001, l.parameters.w - l.parameters.z), 0.0, 1.0);
            result += l.color.rgb * attenuation * incidence * cone;
        }
    }
    return clamp(result, 0.0, 1.0);
}

vertex Varying starkActor(uint id [[vertex_id]], const device ActorVertex *vertices [[buffer(0)]], constant Uniforms &u [[buffer(1)]], constant Material &material [[buffer(2)]]) {
    const device ActorVertex &v = vertices[id];
    uint b1 = uint(v.bone1), b2 = uint(v.bone2);
    float3 position = mix(qrot(u.boneRotation[b2], float3(v.position2)) + u.bonePosition[b2].xyz,
                          qrot(u.boneRotation[b1], float3(v.position1)) + u.bonePosition[b1].xyz, v.weight);
    float3 normal = normalize(mix(qrot(u.boneRotation[b2], float3(v.normal)), qrot(u.boneRotation[b1], float3(v.normal)), v.weight));
    float4 eye = u.modelView * float4(position, 1);
    normal = normalize((u.normal * float4(normal, 0)).xyz);
    Varying result;
    result.position = clipPosition(u.projection * eye);
    result.texcoord = float2(v.texcoord);
    result.color = float4((material.textured ? float3(1) : material.color.rgb) * lighting(eye.xyz, normal, u), 1);
    return result;
}

vertex Varying starkProp(uint id [[vertex_id]], const device PropVertex *vertices [[buffer(0)]], constant Uniforms &u [[buffer(1)]], constant Material &material [[buffer(2)]]) {
    const device PropVertex &v = vertices[id];
    float4 eye = u.modelView * float4(float3(v.position), 1);
    float3 normal = normalize((u.normal * float4(float3(v.normal), 0)).xyz);
    Varying result;
    result.position = clipPosition(u.projection * eye);
    result.texcoord = float2(material.doubleSided ? v.texcoord.x : 1 - v.texcoord.x, 1 - v.texcoord.y);
    result.color = float4((material.textured ? float3(1) : material.color.rgb) * lighting(eye.xyz, normal, u), 1);
    return result;
}

vertex Varying starkShadow(uint id [[vertex_id]], const device ActorVertex *vertices [[buffer(0)]], constant Uniforms &u [[buffer(1)]], constant Material &material [[buffer(2)]]) {
    const device ActorVertex &v = vertices[id];
    uint b1 = uint(v.bone1), b2 = uint(v.bone2);
    float3 position = mix(qrot(u.boneRotation[b2], float3(v.position2)) + u.bonePosition[b2].xyz,
                          qrot(u.boneRotation[b1], float3(v.position1)) + u.bonePosition[b1].xyz, v.weight);
    position += material.shadowDirection.xyz * (-position.y / material.shadowDirection.y);
    position.y = 0;
    Varying result;
    result.position = clipPosition(u.projection * u.modelView * float4(position, 1));
    result.color = float4(0, 0, 0, 0.5);
    result.texcoord = float2(0);
    return result;
}

vertex Varying starkSurface(uint id [[vertex_id]], const device SurfaceVertex *vertices [[buffer(0)]]) {
    Varying result;
    result.position = float4(float3(vertices[id].position), 1);
    result.position.z = 0.5;
    result.texcoord = float2(vertices[id].texcoord);
    result.color = float4(vertices[id].color);
    return result;
}

fragment float4 starkFragment(Varying in [[stage_in]], constant Material &material [[buffer(2)]], texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]]) {
    float4 color = in.color;
    if (material.textured) color *= image.sample(filtering, in.texcoord);
    color.rgb += material.fadeLevel * color.a;
    return color;
}
)metal";

} // End of namespace Gfx
} // End of namespace Stark

#endif
