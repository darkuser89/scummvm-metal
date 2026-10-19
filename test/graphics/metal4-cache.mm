/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone process probe for cold/warm Metal 4 binary archive validation.
 */
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "graphics/metal/renderer.h"
#include "common/system.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

OSystem *g_system = nullptr;
using namespace Graphics;
using namespace Graphics::Metal;
static uint checks;
static void check(bool ok, const char *name) {
	printf("%s %s\n", ok ? "PASS" : "FAIL", name); ++checks;
	if (!ok) exit(1);
}
int main(int argc, char **argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	@autoreleasepool {
		check(argc >= 2, "cache probe mode supplied");
		const bool unavailable = !strcmp(argv[1], "unavailable");
		const bool warm = !strcmp(argv[1], "warm"), corrupt = !strcmp(argv[1], "corrupt"), disabled = !strcmp(argv[1], "disabled");
		const char *variant = argc > 2 ? argv[2] : "";
		Common::String source = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Output { float4 position [[position]]; };
vertex Output v(uint i [[vertex_id]]) {
 const float2 p[4] = {float2(-1,1),float2(1,1),float2(-1,-1),float2(1,-1)};
 Output o; o.position=float4(p[i],0,1); return o;
}
fragment float4 f(constant float4 &color [[buffer(2)]]) { return color; }
)metal";
		if (!strcmp(variant, "source")) source += "\n// distinct shader source identity\n";
		Renderer r; check(r.attach(nullptr) && r.resize(8, 8, !strcmp(variant, "samples") ? 4 : 1), "cache probe attachment");
		VertexAttribute attribute; attribute.components = 1;
		bool layout = !strcmp(variant, "layout");
		Program *program = r.createProgram(source.c_str(), "v", "f", layout ? &attribute : nullptr, layout ? 1 : 0, layout ? 4 : 0); check(program != nullptr, "cache probe shader library");
		if (unavailable) check(chmod(getenv("SCUMMVM_METAL4_CACHE_DIR"), 0500) == 0, "test cache directory made read-only");
		float dummy[4] = {}, color[] = {1, 0, 0, 1};
		GeometryBinding geometry; geometry.vertices = r.createBuffer(dummy, sizeof(dummy));
		UniformBindings bindings; bindings.extraFragment = r.createBuffer(color, sizeof(color));
		DrawState state; state.primitive = kTriangleStrip; state.blend = !strcmp(variant, "blend");
		r.clear(0, 0, 1); r.resetStatistics();
		r.drawProgram(*program, nullptr, 4, sizeof(float), nullptr, 0, nullptr, 0, nullptr, 0, state, nullptr, 0, &geometry, &bindings);
		Statistics stats = r.statistics();
		const bool expectedWarm = warm || (!strcmp(argv[1], "concurrent") && stats.pipelineArchiveHits == 1);
		check(stats.pipelineArchiveHits == (expectedWarm ? 1u : 0u), "archive hit count matches cold/warm mode");
		check(stats.pipelineCompilations == (expectedWarm ? 0u : 1u), "warm archive bypasses pipeline compilation");
		check(stats.pipelineArchiveWrites == (expectedWarm || disabled || unavailable ? 0u : 1u), "successful cold pipeline stored once");
		check(stats.pipelineArchiveRejects == (corrupt ? 1u : 0u), "corrupt archive falls back to compilation");
		check(stats.pipelineArchiveDisables == (unavailable ? 1u : 0u), "cache write failure disables capture without failing rendering");
		if (unavailable) check(chmod(getenv("SCUMMVM_METAL4_CACHE_DIR"), 0700) == 0, "test cache permissions restored");
		uint64 writes = stats.argumentTableWrites;
		for (uint i = 0; i < 8; ++i)
			r.drawProgram(*program, nullptr, 4, sizeof(float), nullptr, 0, nullptr, 0, nullptr, 0, state, nullptr, 0, &geometry, &bindings);
		check(r.statistics().argumentTableWrites == writes && r.statistics().argumentTableReuses >= 16,
			"identical persistent bindings reuse both immutable tables without rewrites");
		delete program; delete geometry.vertices; delete bindings.extraFragment;
		Surface out; check(r.readPixels(out, Common::Rect(8, 8)), "cache probe readback");
		bool pixels = true;
		for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
			byte a, red, green, blue; out.format.colorToARGB(out.getPixel(x, y), a, red, green, blue);
			pixels &= red == 255 && green == 0 && blue == 0 && a == 255;
		}
		check(pixels, "cached pipeline reflection and auxiliary bindings produce exact pixels"); out.free();
		printf("Completed %u checks; hits=%llu compiles=%llu writes=%llu rejects=%llu\n", checks,
			(unsigned long long)stats.pipelineArchiveHits, (unsigned long long)stats.pipelineCompilations,
			(unsigned long long)stats.pipelineArchiveWrites, (unsigned long long)stats.pipelineArchiveRejects);
	}
	return 0;
}
