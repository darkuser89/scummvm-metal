/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HPL_METAL_FIXED_SHADERS_H
#define HPL_METAL_FIXED_SHADERS_H

#include "common/str.h"
#include "hpl1/engine/impl/metal_shader_sources.h"

namespace hpl {

static const char *const kMetalFixedFunctions = R"metal(
struct FixedOutput {
    float4 position [[position]];
    float4 color;
    float4 uv0, uv1, uv2, uv3, uv4, uv5, uv6, uv7;
    float clipDistances [[clip_distance]] [6];
};
struct FixedInput {
    float4 position [[position]];
    float4 color;
    float4 uv0, uv1, uv2, uv3, uv4, uv5, uv6, uv7;
};
vertex FixedOutput hplFixedVertex(const device Vertex *vertices [[buffer(0)]],
                                 constant Uniforms &u [[buffer(1)]], uint id [[vertex_id]]) {
    Vertex v = vertices[id];
    FixedOutput out;
    float4 p = float4(v.position);
    out.position = u.mvp * p;
    out.position.z = (out.position.z + out.position.w) * 0.5;
    out.color = float4(v.color);
    out.uv0 = u.textureMatrices[0] * float4(v.texcoord[0]);
    out.uv1 = u.textureMatrices[1] * float4(v.texcoord[1]);
    out.uv2 = u.textureMatrices[2] * float4(v.texcoord[2]);
    out.uv3 = u.textureMatrices[3] * float4(v.texcoord[3]);
    out.uv4 = u.textureMatrices[4] * float4(v.texcoord[4]);
    out.uv5 = u.textureMatrices[5] * float4(v.texcoord[5]);
    out.uv6 = u.textureMatrices[6] * float4(v.texcoord[6]);
    out.uv7 = u.textureMatrices[7] * float4(v.texcoord[7]);
    float4 eye = u.backendModelView * p;
    for (uint i = 0; i < 6; ++i)
        out.clipDistances[i] = (u.controls.x & (1u << i)) ? dot(u.clipPlanes[i], eye) : 1;
    return out;
}
float4 sourceColor(uint source, float4 texel, float4 constantColor, float4 primary, float4 previous) {
    if (source == 0) return texel;
    if (source == 1) return constantColor;
    if (source == 2) return primary;
    return previous;
}
float4 operandColor(float4 value, uint operand) {
    if (operand == 1) return 1 - value;
    if (operand == 2) return float4(value.a);
    if (operand == 3) return float4(1 - value.a);
    return value;
}
float4 combineColor(uint function, float4 a, float4 b, float4 c) {
    if (function == 0) return a * b;
    if (function == 1) return a;
    if (function == 2) return a + b;
    if (function == 3) return a - b;
    if (function == 4) return a + b - 0.5;
    if (function == 5) return a * c + b * (1 - c);
    return float4(4 * dot(a.rgb - 0.5, b.rgb - 0.5));
}
float4 environmentColor(constant Environment &env, float4 texel, float4 primary, float4 previous) {
    float4 rgb[3], alpha[3];
    for (uint i = 0; i < 3; ++i) {
        rgb[i] = operandColor(sourceColor(env.rgbSources[i], texel, env.color, primary, previous), env.rgbOperands[i]);
        alpha[i] = operandColor(sourceColor(env.alphaSources[i], texel, env.color, primary, previous), env.alphaOperands[i]);
    }
    float4 result = combineColor(env.functions.x, rgb[0], rgb[1], rgb[2]);
    if (env.functions.x != 7)
        result.a = combineColor(env.functions.y, alpha[0], alpha[1], alpha[2]).a * env.functions.w;
    result.rgb *= env.functions.z;
    return clamp(result, 0.0f, 1.0f);
}
)metal";

// Each variant has native Metal texture types matching its bound resources.
// The source is MSL throughout; no runtime GLSL translation is involved.
static Common::String metalFixedSource(const uint kinds[8]) {
	Common::String source(kMetalShaderPrelude);
	source += kMetalFixedFunctions;
	source += "fragment float4 hplFixedFragment(FixedInput in [[stage_in]], constant Uniforms &u [[buffer(1)]]";
	for (uint i = 0; i < 8; ++i) {
		const char *type = kinds[i] == 4 ? "texturecube" : kinds[i] == 5 ? "texture3d" : "texture2d";
		source += Common::String::format(", %s<float> image%u [[texture(%u)]], sampler sampler%u [[sampler(%u)]]", type, i, i, i, i);
	}
	source += ") { float4 previous = in.color;";
	for (uint i = 0; i < 8; ++i) {
		const char *helper = kinds[i] == 1 ? "sample1D" : kinds[i] == 3 ? "sampleRect" : kinds[i] == 4 ? "sampleCube" : kinds[i] == 5 ? "sample3D" : "sample2D";
		const char *swizzle = kinds[i] == 1 ? "x" : kinds[i] >= 4 ? "xyz" : "xy";
		source += Common::String::format("if (u.controls.z & %uu) { float4 texel = %s(image%u, sampler%u, (in.uv%u / in.uv%u.w).%s, u.textureSizes[%u]); previous = environmentColor(u.environments[%u], texel, in.color, previous); }", 1u << i, helper, i, i, i, i, swizzle, i, i);
	}
	source += "alphaTest(previous.a, u); return previous; }";
	return source;
}

static const char *const kMetalGammaSource = R"metal(
#include <metal_stdlib>
using namespace metal;
struct GammaVertex {packed_float3 position; packed_float2 uv; packed_float4 color;};
struct GammaOutput {float4 position [[position]]; float2 uv;};
vertex GammaOutput hplGammaVertex(const device GammaVertex *v [[buffer(0)]], uint id [[vertex_id]]) {
    return {float4(float3(v[id].position), 1), float2(v[id].uv)};
}
fragment float4 hplGammaFragment(GammaOutput in [[stage_in]], constant float &gamma [[buffer(1)]],
                                texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]]) {
    float4 color = image.sample(filtering, in.uv);
    return float4(pow(color.rgb, float3(1 / max(gamma, 0.0001f))), color.a);
}
)metal";

} // End of namespace hpl

#endif
