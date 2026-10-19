/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TWP_METAL_SHADERS_H
#define TWP_METAL_SHADERS_H

namespace Twp {

static const char *const kTwpMetalShaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Light {float4 position, color, cone, falloff;};
struct Uniforms {
    int4 settings;
    float4 effects, ghost;
    float randomValue[8];
    float4 shadows, midtones, highlights, ambient, sprite, sheet;
    float4 hudShadow, hudNormal, hudHighlight, hudRanges;
};
struct VertexUniforms {float4x4 transform;};
struct Vertex {packed_float2 position; packed_float4 color; packed_float2 uv;};
struct Varying {float4 position [[position]]; float4 color; float2 uv;};
vertex Varying twpVertex(uint id [[vertex_id]], const device Vertex *vertices [[buffer(0)]], constant VertexUniforms &u [[buffer(1)]]) {
    const device Vertex &v = vertices[id];
    Varying result;
    result.position = u.transform * float4(float2(v.position), 0, 1);
    result.position.z = (result.position.z + result.position.w) * 0.5;
    result.color = float4(v.color);
    result.uv = float2(v.uv);
    return result;
}
float4 sampleImage(texture2d<float> image, sampler filtering, float2 uv, int flip) {
    if (flip != 0) uv.y = 1 - uv.y;
    return image.sample(filtering, uv);
}

// Work in progress ghost shader.. Too over the top at the moment, it'll make you sick.

constant float speed = 0.1;
constant float emboss = 0.70;
constant float intensity = 0.6;
constant int steps = 4;
constant float frequency = 9.0;

float colour(float2 coord, float globalTime) {
	float col = 0.0;

	float timeSpeed = globalTime * speed;
	float2 adjc = coord;
	adjc.x += timeSpeed; // adjc0.x += fcos*timeSpeed;
	float sum0 = cos(adjc.x * frequency) * intensity;
	col += sum0;

	adjc = coord;
	float fcos = 0.623489797;
	float fsin = 0.781831503;
	adjc.x += fcos * timeSpeed;
	adjc.y -= fsin * timeSpeed;
	float sum1 = cos((adjc.x * fcos - adjc.y * fsin) * frequency) * intensity;
	col += sum1;

	adjc = coord;
	fcos = -0.900968909;
	fsin = 0.433883607;
	adjc.x += fcos * timeSpeed;
	adjc.y -= fsin * timeSpeed;
	col += cos((adjc.x * fcos - adjc.y * fsin) * frequency) * intensity;

	// do same in reverse.
	col += sum1;
	col += sum0;

	return cos(col);
}

float3 rgb2hsv(float3 c) {
	float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	float4 p = mix(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
	float4 q = mix(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));

	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 hsv2rgb(float3 c) {
	float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
	float3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
	return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float rand(float2 Input) {
	float dt = dot(Input, float2(12.9898, 78.233));
	float sn = (dt - 3.14 * floor(dt / 3.14));
	return fract(sin(sn) * 43758.5453123);
}

float color_balance(float col, float l, float3 change) {
	// NOTE: change = (shadow, midtones, highlights)

	float sup = 83.0;  // shadow upper bounds
	float mup = 166.0; // midtones upper bounds

	float value = col * 255.0;
	l = l * 100.0;

	if (l < sup) {
		// shadow
		float f = (sup - l + 1.0) / (sup + 1.0);
		value += change.x * f;
	} else if (l < mup) {
		// midtone
		float mrange = (mup - sup) / 2.0;
		float mid = mrange + sup;
		float diff = mid - l;
		diff = -diff;
		if (diff < 0.0) {
			float f = 1.0 - (diff + 1.0) / (mrange + 1.0);
			value += change.y * f;
		}
	} else {
		// highlight
		float f = (l - mup + 1.0) / (255.0 - mup + 1.0);
		value += change.z * f;
	}
	value = min(255.0, max(0.0, value));
	return value / 255.0;
}

float2 rgb2cv(float3 RGB) {
	float4 P = (RGB.g < RGB.b) ? float4(RGB.bg, -1.0, 2.0 / 3.0) : float4(RGB.gb, 0.0, -1.0 / 3.0);
	float4 Q = (RGB.r < P.x) ? float4(P.xyw, RGB.r) : float4(RGB.r, P.yzx);
	float C = Q.x - min(Q.w, Q.y);
	return float2(C, Q.x);
}

float rgbToLuminance(float3 RGB) {
	float cMax = max(max(RGB.x, RGB.y), RGB.z);
	float cMin = min(min(RGB.x, RGB.y), RGB.z);

	return (cMax + cMin) * 0.5;
}



float3 mod289(float3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float2 mod289(float2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float3 permute(float3 x) { return mod289(((x * 34.0) + 1.0) * x); }
float snoise(float2 v) {
	const float4 C = float4(0.211324865405187,  // (3.0-sqrt(3.0))/6.0
						0.366025403784439,  // 0.5*(sqrt(3.0)-1.0)
						-0.577350269189626, // -1.0 + 2.0 * C.x
						0.024390243902439); // 1.0 / 41.0

	// First corner
	float2 i = floor(v + dot(v, C.yy));
	float2 x0 = v - i + dot(i, C.xx);

	// Other corners
	float2 i1;
	i1 = (x0.x > x0.y) ? float2(1.0, 0.0) : float2(0.0, 1.0);
	float4 x12 = x0.xyxy + C.xxzz;
	x12.xy -= i1;

	// Permutations
	i = mod289(i); // Avoid truncation effects in permutation
	float3 p = permute(permute(i.y + float3(0.0, i1.y, 1.0)) + i.x + float3(0.0, i1.x, 1.0));

	float3 m = max(0.5 - float3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
	m = m * m;
	m = m * m;

	// Gradients: 41 points uniformly over a line, mapped onto a diamond.
	// The ring size 17*17 = 289 is close to a multiple of 41 (41*7 = 287)

	float3 x = 2.0 * fract(p * C.www) - 1.0;
	float3 h = abs(x) - 0.5;
	float3 ox = floor(x + 0.5);
	float3 a0 = x - ox;

	// Normalise gradients implicitly by scaling m
	// Approximation of: m *= inversesqrt( a0*a0 + h*h );
	m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);

	// Compute final noise value at P
	float3 g;
	g.x = a0.x * x0.x + h.x * x0.y;
	g.yz = a0.yz * x12.xz + h.yz * x12.yw;
	return 130.0 * dot(m, g);
}


fragment float4 twpFragment(Varying in [[stage_in]], constant Uniforms &u [[buffer(1)]], constant Light *lights [[buffer(2)]],
                           texture2d<float> image [[texture(0)]], texture2d<float> image2 [[texture(1)]],
                           sampler filtering [[sampler(0)]], sampler filtering2 [[sampler(1)]]) {
    if (u.settings.x == 2) {

	float2 c1 = in.uv;
	float cc1 = colour(c1, u.ghost.x);
	float2 offset;

	c1.x += (0.001 * u.ghost.z); // appx 12 pixels horizontal
	offset.x = emboss * (cc1 - colour(c1, u.ghost.x));

	c1.x = in.uv.x;
	c1.y += (0.002 * u.ghost.z); // appx 12 pixels verticle
	offset.y = emboss * (cc1 - colour(c1, u.ghost.x));

	// TODO: The effect should be centered around Franklyns position in the room, not the center
	// if ( emitFromCenter == 1)
	{
		float2 center = float2(0.5, 0.5);
		float distToCenter = distance(center, in.uv);
		offset *= distToCenter * 2.0;
	}

	c1 = in.uv;
	c1 += (offset * u.ghost.y);

	float3 col = float3(0, 0, 0);
	if (c1.x >= 0.0 && c1.x < (1.0 - 0.003125)) {
		col = sampleImage(image, filtering, c1, u.settings.w & 1).rgb;
		float intensity = rgbToLuminance(col); //(col.r + col.g + col.b) * 0.333333333;

		// Exponential Shadows
		float shadowsBleed = 1.0 - intensity;
		shadowsBleed *= shadowsBleed;
		shadowsBleed *= shadowsBleed;

		// Exponential u.midtones.rgb
		float midtonesBleed = 1.0 - abs(-1.0 + intensity * 2.0);
		midtonesBleed *= midtonesBleed;
		midtonesBleed *= midtonesBleed;

		// Exponential Hilights
		float hilightsBleed = intensity;
		hilightsBleed *= hilightsBleed;
		hilightsBleed *= hilightsBleed;

		float3 colorization = col.rgb + u.shadows.rgb * shadowsBleed +
							u.midtones.rgb * midtonesBleed +
							u.highlights.rgb * hilightsBleed;

		colorization = mix(col, colorization, u.ghost.y);

		// col = lerp(col, colorization, _Amount);
		col = min(float3(1.0), max(float3(0.0), colorization));
	}
	return in.color * float4(col, sampleImage(image, filtering, c1, u.settings.w & 1).a);

    }
    if (u.settings.x == 3) {

	const float RADIUS = 0.75;
	const float SOFTNESS = 0.45;
	const float ScratchValue = 0.3;

	float4 texColor = sampleImage(image, filtering, in.uv, u.settings.w & 1);
	float4 col = in.color * texColor;
	float gray = dot(col.rgb, float3(0.299, 0.587, 0.114));
	float2 dist = float2(in.uv.x - 0.5, in.uv.y - 0.5);
	float3 sepiaColor = float3(gray) * float3(0.9, 0.8, 0.6); // float3(1.2, 1.0, 0.8);
	float len = dot(dist, dist);
	float vignette = (1 - smoothstep(RADIUS - SOFTNESS, RADIUS, len));
	//   float vignette = (1.0 - len);
	col.rgb = mix(col.rgb, sepiaColor, 0.80) * vignette * u.effects.w; // Want to keep SOME of the original color, so only use 80% sepia
	//   col.rgb = float3( vignette ) * u.effects.w;

	for (int i = 0; i < 1; i++) {
		if (u.randomValue[i] < ScratchValue) {
			// Pick a random spot to show scratches
			float dist = 1.0 / ScratchValue;
			float d = distance(in.uv, float2(u.randomValue[i] * dist, u.randomValue[i] * dist));
			if (d < 0.4) {
				// Generate the scratch
				float xPeriod = 8.0;
				float yPeriod = 1.0;
				float pi = 3.141592;
				float phase = u.ghost.w;
				float turbulence = snoise(in.uv * 2.5);
				float vScratch = 0.5 + (sin(((in.uv.x * xPeriod + in.uv.y * yPeriod + turbulence)) * pi + phase) * 0.5);
				vScratch = clamp((vScratch * 10000.0) + 0.35, 0.0, 1.0);

				col.rgb *= vScratch;
			}
		}
	}
	return col;

    }
    if (u.settings.x == 4) {
        float2 uv = in.uv;
        uv.x += sin((u.effects.x + uv.y) * (3.142 * 2 * 4)) * u.effects.z;
        float4 first = in.color * sampleImage(image, filtering, uv, u.settings.w & 1);
        float4 second = in.color * sampleImage(image2, filtering2, uv, u.settings.w & 2);
        if (u.settings.z != 0) {
            float gray = dot(second.rgb, float3(0.299, 0.587, 0.114));
            float2 dist = uv - 0.5;
            float vignette = 1 - smoothstep(0.3, 0.75, dot(dist, dist));
            second.rgb = mix(second.rgb, float3(gray) * float3(0.9, 0.8, 0.6), 0.8) * vignette;
        }
        return float4(mix(first.rgb, second.rgb, u.effects.y), 1);
    }
    float4 texel = sampleImage(image, filtering, in.uv, u.settings.w & 1);
    float4 color = texel * in.color;
    if (u.settings.x == 1) {
        float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));
        return float4(float3(gray), color.a);
    }
    if (u.settings.x == 5 || u.settings.x == 6) {
        float2 spriteUV = (in.uv - u.sheet.xy) / u.sheet.zw;
        float2 pixel = spriteUV * u.sprite.xy + u.sprite.zw;
        pixel.y = -pixel.y;
        float3 diffuse = float3(0);
        for (int i = 0; i < min(u.settings.y, 50); ++i) {
            constant Light &light = lights[i];
            float2 lightVec = pixel - light.position.xy;
            float coneValue = dot(normalize(-lightVec), light.cone.xy);
            if (coneValue < light.cone.z) continue;
            float intercept = light.falloff.y * light.falloff.z;
            float dx1 = 0.5 / intercept;
            float dx2 = 0.5 / (light.falloff.y - intercept);
            float offset = 0.5 + intercept * dx2;
            float distance = length(lightVec);
            float nearTerm = clamp(1 - distance * dx1, 0.0, 1.0);
            float farTerm = clamp(offset - distance * dx2, 0.0, 1.0);
            float term = mix(nearTerm, farTerm, step(intercept, distance));
            float coneRange = 1 - light.cone.z;
            float halfConeRange = coneRange * light.cone.w;
            float conePos = 1 - coneValue;
            float coneFalloff = conePos > halfConeRange ? 1 - (conePos-halfConeRange)/(coneRange-halfConeRange) : 1;
            diffuse += light.falloff.x * term * light.color.rgb * coneFalloff;
        }
        if (u.settings.x == 6) return float4(color.rgb * u.ambient.rgb + diffuse, color.a);
        return float4(color.rgb * min(diffuse + u.ambient.rgb, float3(1)), color.a);
    }
    if (u.settings.x == 7) {
        float4 tint = texel.g <= u.hudRanges.x ? u.hudShadow : texel.g >= u.hudRanges.y ? u.hudHighlight : u.hudNormal;
        return color * tint;
    }
    return color;
}
)metal";

} // End of namespace Twp

#endif
