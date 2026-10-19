/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standalone native GPU regressions; link against the configured Metal build.
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "graphics/metal/renderer.h"
#include "graphics/metal/geometry.h"
#include "common/system.h"
#include "common/memstream.h"
#include "graphics/metal/retro/pipeline.h"
#include "engines/stark/gfx/metal.h"
#include "engines/stark/gfx/metal-shaders.h"
#include "engines/twp/metal-shaders.h"
#include <cstdio>
#include <cstdlib>

OSystem *g_system = nullptr;
using namespace Graphics;
using namespace Graphics::Metal;

static uint checks;
static void check(bool ok, const char *name) {
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	++checks;
	if (!ok) exit(1);
}

static void pixel(Texture &texture, Renderer &r, int x, int y, int red, int green, int blue, const char *name) {
	Surface image;
	check(r.readTexture(texture, image, Common::Rect(texture.width(), texture.height())), "texture readback");
	byte a, rr, g, b;
	image.format.colorToARGB(image.getPixel(x, y), a, rr, g, b);
	if (abs(rr - red) > 2 || abs(g - green) > 2 || abs(b - blue) > 2)
		fprintf(stderr, "pixel (%d,%d): %u,%u,%u expected %d,%d,%d\n", x, y, rr, g, b, red, green, blue);
	check(abs(rr - red) <= 2 && abs(g - green) <= 2 && abs(b - blue) <= 2, name);
	image.free();
}

static Vertex quad[] = {
	{{-1, 1, 0}, {0, 0}, {1, 1, 1, 1}}, {{1, 1, 0}, {1, 0}, {1, 1, 1, 1}},
	{{-1, -1, 0}, {0, 1}, {1, 1, 1, 1}}, {{1, -1, 0}, {1, 1}, {1, 1, 1, 1}}
};

static void bufferVersions(Renderer &r) {
	RenderTarget *first = r.createRenderTarget(16, 16, 1, kTargetBGRA8, false, false);
	RenderTarget *second = r.createRenderTarget(16, 16, 1, kTargetBGRA8, false, false);
	Vertex vertices[4];
	memcpy(vertices, quad, sizeof(vertices));
	for (Vertex &v : vertices) { v.color[1] = v.color[2] = 0; }
	GeometryBinding geometry;
	geometry.vertices = r.createBuffer(vertices, sizeof(vertices));
	const uint16 indices[] = {99, 0, 1, 2, 3};
	geometry.indices = r.createBuffer(indices, sizeof(indices));
	geometry.indexOffset = sizeof(uint16); geometry.indices16 = true;
	DrawState state; state.primitive = kTriangleStrip;
	r.setRenderTarget(first); r.clear(0, 0, 0);
	r.drawBuffered(geometry, 4, 4, nullptr, state, Uniforms());
	uint64 uploaded = r.statistics().persistentBufferBytes;
	r.updateBuffer(*geometry.vertices, vertices, sizeof(vertices));
	check(r.statistics().persistentBufferBytes == uploaded, "identical persistent buffer update retains the immutable version");
	for (Vertex &v : vertices) { v.color[0] = 0; v.color[1] = 1; }
	r.updateBuffer(*geometry.vertices, vertices, sizeof(vertices));
	r.setRenderTarget(second); r.clear(0, 0, 0);
	r.drawBuffered(geometry, 4, 4, nullptr, state, Uniforms());
	delete geometry.vertices; delete geometry.indices;
	pixel(first->texture(), r, 8, 8, 255, 0, 0, "encoded immutable vertex version and 16-bit index offset");
	pixel(second->texture(), r, 8, 8, 0, 255, 0, "updated vertex version survives early destruction");
	r.setRenderTarget(nullptr); delete first; delete second;
}

static void transientSlices(Renderer &r) {
	r.setRenderTarget(nullptr); r.flush(true);
	GeometryBinding geometry;
	Vertex vertices[4]; memcpy(vertices, quad, sizeof(vertices));
	for (Vertex &v : vertices) { v.color[1] = v.color[2] = 0; }
	geometry.vertices = r.createTransientBuffer(vertices, sizeof(vertices));
	const uint16 indices[] = {99, 0, 1, 2, 3};
	geometry.indices = r.createTransientBuffer(indices, sizeof(indices));
	geometry.indices16 = true; geometry.indexOffset = 2;
	check(geometry.vertices->size() == sizeof(vertices) && geometry.indices->size() == sizeof(indices), "transient slices expose their logical length");
	DrawState state; state.primitive = kTriangleStrip;
	r.drawBuffered(geometry, 4, 4, nullptr, state, Uniforms()); r.flush(true);
	// Reuse the arena aggressively while older public slices remain alive.
	for (uint round = 0; round < 4; ++round) {
		Buffer *other = r.createTransientBuffer(quad, sizeof(quad)); delete other;
		r.flush(true);
	}
	r.clear(0, 0, 1); r.drawBuffered(geometry, 4, 4, nullptr, state, Uniforms());
	pixel(*r.screenTexture(), r, 8, 8, 255, 0, 0, "pinned transient vertices and index offsets survive later arena reuse");
	for (Vertex &v : vertices) { v.color[0] = 0; v.color[1] = 1; }
	r.updateBuffer(*geometry.vertices, vertices, sizeof(vertices));
	r.drawBuffered(geometry, 4, 4, nullptr, state, Uniforms());
	delete geometry.vertices; delete geometry.indices;
	pixel(*r.screenTexture(), r, 8, 8, 0, 255, 0, "transient slice can become an updated persistent version");
}

static void geometryLRU(Renderer &r) {
	GeometryCache cache(&r);
	DrawState state; state.primitive = kTriangleStrip;
	Vertex vertices[4]; memcpy(vertices, quad, sizeof(vertices));
	for (uint i = 0; i < 2048; ++i) {
		vertices[0].texcoord[0] = i;
		cache.draw(vertices, 4, nullptr, state, Uniforms());
		if (i % 128 == 127) r.flush(true);
	}
	vertices[0].texcoord[0] = 0; r.resetStatistics();
	cache.draw(vertices, 4, nullptr, state, Uniforms());
	check(r.statistics().persistentBufferBytes == 0, "LRU touch retains the oldest geometry without upload");
	vertices[0].texcoord[0] = 2048; cache.draw(vertices, 4, nullptr, state, Uniforms());
	r.resetStatistics(); vertices[0].texcoord[0] = 0; cache.draw(vertices, 4, nullptr, state, Uniforms());
	check(r.statistics().persistentBufferBytes == 0, "recently touched geometry survives cache capacity eviction");
	vertices[0].texcoord[0] = 1; cache.draw(vertices, 4, nullptr, state, Uniforms());
	check(r.statistics().persistentBufferBytes == sizeof(vertices), "least recently used geometry was evicted");
	for (Vertex &v : vertices) { v.color[0] = v.color[2] = 0; }
	cache.draw(vertices, 4, nullptr, state, Uniforms());
	pixel(*r.screenTexture(), r, 8, 8, 0, 255, 0, "geometry cache detects mutation at the same CPU address");
}

static void textureFormats(Renderer &r) {
	Surface source;
	source.create(8, 4, PixelFormat::createFormatRGBA32());
	source.fillRect(Common::Rect(8, 4), source.format.ARGBToColor(255, 0, 0, 255));
	Texture *texture = r.createTexture(source);
	source.fillRect(Common::Rect(2, 1, 6, 3), source.format.ARGBToColor(255, 255, 0, 0));
	r.updateTexture(*texture, source, Common::Rect(2, 1, 6, 3), Common::Point(0, 0));
	pixel(*texture, r, 0, 0, 255, 0, 0, "source patch copied to independent destination offset");
	pixel(*texture, r, 4, 0, 0, 0, 255, "patch preserves texels outside destination");
	delete texture; source.free();
	source.create(4, 4, PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0));
	source.fillRect(Common::Rect(4, 4), source.format.ARGBToColor(255, 255, 0, 0));
	texture = r.createTexture(source);
	pixel(*texture, r, 2, 2, 255, 0, 0, "native RGB565 red channel order");
	source.fillRect(Common::Rect(4, 4), source.format.ARGBToColor(255, 0, 0, 255));
	r.updateTexture(*texture, source, Common::Rect(1, 1, 3, 3));
	pixel(*texture, r, 2, 2, 0, 0, 255, "native RGB565 partial update");
	pixel(*texture, r, 0, 0, 255, 0, 0, "native RGB565 untouched region");
	delete texture; source.free();
	// RGB565 endpoints and selector zero: BC1 red, including a partial edge block.
	const byte bc1[] = {0x00, 0xf8, 0, 0, 0, 0, 0, 0};
	texture = r.createCompressedTexture(bc1, sizeof(bc1), 3, 2, kBC1);
	check(texture != nullptr, "BC1 allocation on the test device");
	pixel(*texture, r, 2, 1, 255, 0, 0, "BC1 partial edge block readback");
	source.create(3, 2, PixelFormat::createFormatRGBA32());
	source.fillRect(Common::Rect(3, 2), source.format.ARGBToColor(255, 0, 255, 0));
	r.updateTexture(*texture, source, Common::Rect(1, 0, 2, 1));
	pixel(*texture, r, 1, 0, 0, 255, 0, "compressed texture CPU patch promotes storage");
	pixel(*texture, r, 2, 1, 255, 0, 0, "compressed promotion preserves remaining image");
	delete texture; source.free();
	const byte bc2[] = {255,255,255,255,255,255,255,255, 0,0xf8,0,0,0,0,0,0};
	const byte bc3[] = {255,255,0,0,0,0,0,0, 0,0xf8,0,0,0,0,0,0};
	for (uint i = 0; i < 2; ++i) {
		texture = r.createCompressedTexture(i ? bc3 : bc2, 16, 4, 4, i ? kBC3 : kBC2);
		check(texture != nullptr, "BC2/BC3 native allocation");
		pixel(*texture, r, 2, 2, 255, 0, 0, "BC2/BC3 native readback");
		delete texture;
	}
}

static void hazards(Renderer &r) {
	Surface source; source.create(4, 4, PixelFormat::createFormatRGBA32());
	source.fillRect(Common::Rect(4, 4), source.format.ARGBToColor(255, 255, 0, 0));
	r.flush(true); r.resetStatistics();
	Texture *a = r.createTexture(source), *b = r.createTexture(source);
	check(r.statistics().queueBarriers == 1, "independent uploads share only the command boundary barrier");
	RenderTarget *first = r.createRenderTarget(16, 16, 1, kTargetBGRA8, false, false);
	RenderTarget *second = r.createRenderTarget(16, 16, 1, kTargetBGRA8, false, false);
	DrawState state; state.primitive = kTriangleStrip; state.linear = false;
	r.setRenderTarget(first); r.clear(0, 0, 0);
	r.draw(quad, 4, a, state, Uniforms());
	source.fillRect(Common::Rect(4, 4), source.format.ARGBToColor(255, 0, 0, 255));
	r.updateTexture(*a, source, Common::Rect(4, 4));
	r.setRenderTarget(second); r.clear(0, 0, 0);
	r.draw(quad, 4, a, state, Uniforms());
	pixel(first->texture(), r, 8, 8, 255, 0, 0, "read-before-write stage dependency preserves earlier draw");
	pixel(second->texture(), r, 8, 8, 0, 0, 255, "write-before-read stage dependency exposes updated texture");
	r.copyTexture(*b, Common::Rect(4, 4), *a, Common::Rect(4, 4));
	pixel(*b, r, 2, 2, 0, 0, 255, "native blit observes latest render/texture dependency");
	r.flush(true); r.resetStatistics();
	r.setRenderTarget(first); r.clear(1, 0, 0); r.flush(false);
	r.setRenderTarget(second); r.clear(0, 1, 0);
	pixel(first->texture(), r, 8, 8, 255, 0, 0, "independent later blit waits for earlier command-buffer rendering");
	check(r.statistics().queueBarriers >= 3, "vertex/fragment boundary wait does not replace the first blit boundary wait");
	r.setRenderTarget(nullptr); delete first; delete second; delete a; delete b; source.free();
}

static void mipPromotionAndTargetCopies(Renderer &r) {
	Surface source; source.create(4, 4, PixelFormat::createFormatRGBA32());
	for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
		source.setPixel(x, y, source.format.ARGBToColor(255, (x + y) & 1 ? 255 : 0, 0, (x + y) & 1 ? 0 : 255));
	Texture *texture = r.createTexture(source);
	r.flush(true);
	RenderTarget *warm = r.createRenderTarget(4, 4, 1, kTargetBGRA8, false, false);
	RenderTarget *reduced = r.createRenderTarget(1, 1, 1, kTargetBGRA8, false, false);
	r.setRenderTarget(warm); r.clear(0, 0, 0);
	r.generateMipmaps(*texture);
	r.setRenderTarget(reduced);
	uint64 barriers = r.statistics().queueBarriers;
	DrawState state; state.primitive = kTriangleStrip; state.linear = true;
	r.draw(quad, 4, texture, state, Uniforms());
	check(r.statistics().queueBarriers > barriers, "promoted mip storage has a blit-to-fragment dependency after warm stage boundaries");
	pixel(reduced->texture(), r, 0, 0, 128, 0, 128, "promoted mip chain is available to immediate minification");
	r.setRenderTarget(nullptr); delete texture; source.free(); delete warm; delete reduced;

	RenderTarget *from = r.createRenderTarget(16, 16, 1, kTargetBGRA8, false, false);
	RenderTarget *to = r.createRenderTarget(16, 16, 1, kTargetBGRA8, false, false);
	r.setRenderTarget(from); r.clear(1, 0, 0); r.setRenderTarget(nullptr); r.flush(true);
	r.resetStatistics();
	r.copyTexture(to->texture(), Common::Rect(4, 4, 12, 12), from->texture(), Common::Rect(8, 8));
	check(r.statistics().uploadedBytes == 0 && r.statistics().argumentTableWrites == 0,
		"single-sample target copy needs no quad, uniforms or argument tables");
	pixel(to->texture(), r, 8, 8, 255, 0, 0, "native target blit copies the selected region");
	pixel(to->texture(), r, 0, 0, 0, 0, 0, "partial native target blit preserves initial clear outside the patch");
	r.setRenderTarget(to);
	r.clearRect(Common::Rect(0, 0, 2, 2), 0, 1, 0, 1, true, false);
	pixel(to->texture(), r, 8, 8, 255, 0, 0, "render after native target blit preserves copied color");
	r.setRenderTarget(nullptr); delete to;
	if (r.supportsSamples(4)) {
		to = r.createRenderTarget(16, 16, 4, kTargetBGRA8, false, false);
		r.copyTexture(to->texture(), Common::Rect(16, 16), from->texture(), Common::Rect(16, 16));
		r.setRenderTarget(to);
		r.clearRect(Common::Rect(0, 0, 2, 2), 0, 1, 0, 1, true, false);
		pixel(to->texture(), r, 8, 8, 255, 0, 0, "multisample target copy survives subsequent resolve");
		r.setRenderTarget(nullptr); delete to;
	}
	delete from;
}

static void uniformsAndState(Renderer &r) {
	const char *source = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex { packed_float3 p; packed_float2 uv; packed_float4 color; };
struct Frame { float4x4 matrix; float4 unused[192]; };
struct Output { float4 position [[position]]; };
vertex Output testVertex(uint id [[vertex_id]], const device Vertex *v [[buffer(0)]], constant Frame &f [[buffer(1)]]) {
 Output o; o.position=f.matrix*float4(float3(v[id].p),1); return o;
}
fragment float4 testFragment(constant float4 &color [[buffer(2)]]) { return color; }
)metal";
	Program *program = r.createProgram(source, "testVertex", "testFragment");
	check(program && r.validateProgram(*program, kTargetBGRA8), "split uniform pipeline");
	float frame[16 + 192 * 4] = {};
	frame[0] = frame[5] = frame[10] = frame[15] = 1;
	GeometryBinding geometry; geometry.vertices = r.createBuffer(quad, sizeof(quad));
	UniformBindings bindings; bindings.vertex = r.createBuffer(frame, sizeof(frame));
	DrawState state; state.primitive = kTriangleStrip;
	r.setRenderTarget(nullptr); r.clear(0, 0, 0); r.resetStatistics();
	for (uint i = 0; i < 16; ++i) {
		float color[4] = {i & 1 ? 0.f : 1.f, i & 1 ? 1.f : 0.f, 0, 1};
		bindings.extraData = color; bindings.extraSize = sizeof(color);
		state.scissor = true; state.scissorRect = Common::Rect(i, 0, i + 1, 16);
		r.drawProgram(*program, nullptr, 4, sizeof(Vertex), nullptr, 0, nullptr, 0, nullptr, 0,
			state, nullptr, 0, &geometry, &bindings);
	}
	Statistics stats = r.statistics();
	check(stats.uploadedBytes == 16 * sizeof(float) * 4, "large frame and geometry never re-uploaded per material");
	check(stats.argumentTableReuses >= 15, "unchanged vertex bindings share immutable argument tables across materials");
	check(stats.encoderStateSetters < 60, "unchanged encoder state setter calls suppressed");
	for (uint i = 0; i < 16; ++i)
		pixel(*r.screenTexture(), r, i, 8, i & 1 ? 0 : 255, i & 1 ? 255 : 0, 0, "per-draw table and material snapshot");
	delete geometry.vertices; delete bindings.vertex; delete program;
}

static void depthAndQueries(Renderer &r) {
	RenderTarget *target = r.createRenderTarget(16, 16, 1, kTargetBGRA8, false, false);
	r.setRenderTarget(target); r.clear(0, 0, 1);
	DrawState state; state.primitive = kTriangleStrip; state.depthTest = state.depthWrite = true;
	Vertex v[4]; memcpy(v, quad, sizeof(v));
	for (Vertex &vertex : v) vertex.position[2] = -0.5f;
	Uniforms u; u.tint[1] = u.tint[2] = 0;
	r.draw(v, 4, nullptr, state, u);
	for (Vertex &vertex : v) vertex.position[2] = 0.5f;
	u.tint[0] = 0; u.tint[1] = 1;
	r.draw(v, 4, nullptr, state, u);
	pixel(target->texture(), r, 8, 8, 255, 0, 0, "color-only target gains working depth attachment lazily");
	r.clear(0, 0, 1);
	r.clearRect(Common::Rect(0, 0, 8, 16), 0, 0, 0, 1, false, false, true, 15, 1, 5);
	state = DrawState(); state.primitive = kTriangleStrip; state.stencil = true; state.stencilReference = 5;
	state.frontStencil.compare = state.backStencil.compare = kEqual;
	u = Uniforms(); u.tint[0] = u.tint[2] = 0;
	r.draw(quad, 4, nullptr, state, u);
	pixel(target->texture(), r, 4, 8, 0, 255, 0, "lazy stencil attachment accepts matching scissored clear");
	pixel(target->texture(), r, 12, 8, 0, 0, 255, "stencil comparison preserves unmatched half");
	r.setRenderTarget(nullptr); delete target;
	state = DrawState(); state.primitive = kTriangleStrip;
	OcclusionQuery *queries[3];
	r.flush(true); r.resetStatistics(); r.clear(0, 0, 0);
	for (uint i = 0; i < 3; ++i) {
		queries[i] = r.createOcclusionQuery(); r.beginOcclusionQuery(*queries[i]);
		r.draw(quad, 4, nullptr, state, Uniforms()); r.endOcclusionQuery(*queries[i]);
	}
	check(r.statistics().submissions == 0, "query end does not force one submission per query");
	for (OcclusionQuery *query : queries) {
		check(r.occlusionQueryResult(*query) == 256, "batched query sample count"); delete query;
	}
	r.flush(true); r.resetStatistics();
	for (uint i = 0; i < 12; ++i) { r.clear(0, 0, 0); r.flush(true); }
	check(r.statistics().submissions == 12 && r.statistics().submissionObjects <= 1 && r.statistics().residencySets <= 1,
		"completed submission containers and residency sets are reused");
	OcclusionQuery *split = r.createOcclusionQuery();
	r.beginOcclusionQuery(*split);
	r.draw(quad, 4, nullptr, state, Uniforms()); r.flush(false);
	r.draw(quad, 4, nullptr, state, Uniforms()); r.flush(false);
	uint64 submissions = r.statistics().submissions;
	r.endOcclusionQuery(*split);
	check(r.occlusionQueryResult(*split) == 512, "query crossing two submissions waits for its last writer");
	check(r.statistics().submissions == submissions, "ending an already submitted query does not create an unrelated empty submission");
	r.beginOcclusionQuery(*split); r.endOcclusionQuery(*split);
	check(r.occlusionQueryResult(*split) == 0, "reused empty query has no stale samples");
	delete split;
}

static void queues(Renderer &r) {
	Renderer consumer; check(consumer.attach(nullptr, &r) && consumer.resize(16, 16), "shared-device consumer queue");
	r.clear(1, 0, 0); consumer.waitFor(r);
	consumer.copyTexture(*consumer.screenTexture(), Common::Rect(16, 16), *r.screenTexture(), Common::Rect(16, 16));
	r.waitFor(consumer); r.clear(0, 0, 1);
	pixel(*consumer.screenTexture(), consumer, 8, 8, 255, 0, 0, "consumer copy precedes producer overwrite through GPU event join");
	pixel(*r.screenTexture(), r, 8, 8, 0, 0, 255, "producer continues after joined queue without intermediate CPU wait");
}

static void engineUniforms(Renderer &r) {
	DrawState state; state.primitive = kTriangleStrip;
	Program *program = r.createProgram(Stark::Gfx::kMetalShaderSource, "starkActor", "starkFragment");
	check(program != nullptr, "Stark actor native program");
	Stark::Gfx::MetalUniforms frame = {};
	for (uint i : {0u, 5u, 10u, 15u}) frame.modelView[i] = frame.projection[i] = frame.normal[i] = 1;
	for (uint i = 0; i < 70; ++i) frame.boneRotation[i][3] = 1;
	frame.ambient[0] = frame.ambient[1] = frame.ambient[2] = 1;
	struct ActorVertex { float p1[3], p2[3], b1, b2, weight, normal[3], uv[2]; } actor[4] = {};
	for (uint i = 0; i < 4; ++i) {
		memcpy(actor[i].p1, quad[i].position, 3 * sizeof(float));
		memcpy(actor[i].p2, quad[i].position, 3 * sizeof(float));
		actor[i].normal[2] = actor[i].weight = 1;
	}
	GeometryBinding geometry; geometry.vertices = r.createBuffer(actor, sizeof(actor));
	UniformBindings bindings; bindings.vertex = r.createBuffer(&frame, sizeof(frame));
	Stark::Gfx::MetalMaterialUniforms material;
	material.color[1] = material.color[2] = 0;
	bindings.extraData = &material; bindings.extraSize = sizeof(material);
	r.setRenderTarget(nullptr); r.clear(0, 0, 0);
	r.drawProgram(*program, nullptr, 4, sizeof(ActorVertex), nullptr, 0, nullptr, 0, nullptr, 0, state,
		nullptr, 0, &geometry, &bindings);
	pixel(*r.screenTexture(), r, 8, 8, 255, 0, 0, "Stark actor bone/frame and separate material ABI");
	delete program; delete geometry.vertices; delete bindings.vertex;

	program = r.createProgram(Stark::Gfx::kMetalShaderSource, "starkSurface", "starkFragment");
	material = Stark::Gfx::MetalMaterialUniforms(); material.fadeLevel = -0.5f;
	bindings = UniformBindings(); bindings.extraData = &material; bindings.extraSize = sizeof(material);
	r.clear(0, 0, 0);
	r.drawProgram(*program, quad, 4, sizeof(Vertex), nullptr, 0, nullptr, 0, nullptr, 0, state,
		nullptr, 0, nullptr, &bindings);
	pixel(*r.screenTexture(), r, 8, 8, 128, 128, 128, "Stark surface material works without large frame block");
	delete program;

	program = r.createProgram(Twp::kTwpMetalShaderSource, "twpVertex", "twpFragment");
	struct TwpVertex { float position[2], color[4], uv[2]; } twp[4] = {};
	for (uint i = 0; i < 4; ++i) {
		memcpy(twp[i].position, quad[i].position, 2 * sizeof(float));
		twp[i].color[0] = twp[i].color[3] = 1;
	}
	float transform[16] = {}; transform[0] = transform[5] = transform[10] = transform[15] = 1;
	// Fragment bank: int4 settings followed by fourteen float4-sized groups.
	uint32 fragment[60] = {};
	float lights[50 * 16] = {};
	bindings = UniformBindings(); bindings.extraFragment = r.createBuffer(lights, sizeof(lights));
	Surface white; white.create(1, 1, PixelFormat::createFormatRGBA32()); white.setPixel(0, 0, 0xffffffff);
	TextureBinding images[2]; images[0].texture = r.createTexture(white); images[1].texture = images[0].texture; white.free();
	r.clear(0, 0, 0);
	r.drawProgram(*program, twp, 4, sizeof(TwpVertex), nullptr, 0, transform, sizeof(transform), images, 2, state,
		fragment, sizeof(fragment), nullptr, &bindings);
	pixel(*r.screenTexture(), r, 8, 8, 255, 0, 0, "TWP split vertex/fragment and persistent light banks");
	delete bindings.extraFragment; delete images[0].texture; delete program;
}

class PresetArchive : public Common::Archive {
public:
	Common::String preset, first, second;
	bool hasFile(const Common::Path &path) const override {
		return path == Common::Path("test.glslp") || path == Common::Path("first.glsl") || path == Common::Path("second.glsl");
	}
	int listMembers(Common::ArchiveMemberList &list) const override {
		for (const char *name : {"test.glslp", "first.glsl", "second.glsl"}) list.push_back(getMember(Common::Path(name)));
		return 3;
	}
	const Common::ArchiveMemberPtr getMember(const Common::Path &path) const override {
		return hasFile(path) ? Common::ArchiveMemberPtr(new Common::GenericArchiveMember(path, *this)) : Common::ArchiveMemberPtr();
	}
	Common::SeekableReadStream *createReadStreamForMember(const Common::Path &path) const override {
		if (!hasFile(path)) return nullptr;
		const Common::String &text = path == Common::Path("test.glslp") ? preset : path == Common::Path("first.glsl") ? first : second;
		return new Common::MemoryReadStream((const byte *)text.c_str(), text.size());
	}
};

static Common::String presetSource(const char *uniforms, const char *expression) {
	return Common::String(R"glsl(
#ifdef VERTEX
attribute vec4 VertexCoord; uniform mat4 MVPMatrix;
void main() { gl_Position=MVPMatrix*VertexCoord; }
#endif
#ifdef FRAGMENT
)glsl") + uniforms + "\nvoid main() { gl_FragColor=" + expression + "; }\n#endif\n";
}

static void presets(Renderer &r) {
	PresetArchive archive;
	archive.preset = "shaders = 1\nshader0 = first.glsl\nmipmap_input0 = true\n";
	archive.first = presetSource("uniform sampler2D Texture;", "texture2D(Texture,vec2(0.5))");
	Common::SearchSet archives; archives.add("memory", &archive, 0, false);
	Retro::Pipeline pipeline(r);
	check(pipeline.open(Common::Path("test.glslp"), archives), "static preset opens from memory archive");
	check(!pipeline.isAnimated(), "static preset does not force idle redraws");
	Surface input; input.create(4, 4, PixelFormat::createFormatRGBA32());
	input.fillRect(Common::Rect(4, 4), input.format.ARGBToColor(255, 255, 0, 0));
	Texture *texture = r.createTexture(input);
	Texture *output = pipeline.render(*texture, 16, 16, false);
	check(output != nullptr, "static mipmapped preset renders");
	pixel(*output, r, 8, 8, 255, 0, 0, "static input and mip generation pixels");
	r.resetStatistics(); output = pipeline.render(*texture, 16, 16, false);
	check(output && r.statistics().persistentBufferBytes == 0, "unchanged preset dimensions reuse prepared GPU geometry");
	pixel(*output, r, 8, 8, 255, 0, 0, "reused preset geometry pixels");
	output = pipeline.render(*texture, 32, 24, true);
	check(output && output->width() == 32 && output->height() == 24, "preset viewport resize rebuilds derived target size");
	pixel(*output, r, 24, 20, 255, 0, 0, "resized preset geometry covers the new viewport");
	pipeline.close();
	archive.first = presetSource("uniform sampler2D Texture;", "texture2DLod(Texture,vec2(0.5),2.0)");
	check(pipeline.open(Common::Path("test.glslp"), archives), "explicit preset LOD pipeline");
	for (int y = 0; y < input.h; ++y) for (int x = 0; x < input.w; ++x)
		input.setPixel(x, y, (x + y) & 1 ? input.format.ARGBToColor(255, 255, 0, 0) : input.format.ARGBToColor(255, 0, 0, 255));
	r.updateTexture(*texture, input, Common::Rect(4, 4));
	output = pipeline.render(*texture, 16, 16, false);
	check(output != nullptr, "explicit LOD checkerboard renders");
	pixel(*output, r, 8, 8, 128, 0, 128, "mipmap_input supplies the requested reduced checkerboard level");
	pipeline.close();
	archive.first = presetSource("uniform sampler2D Prev2Texture;", "texture2D(Prev2Texture,vec2(0.5))");
	check(pipeline.open(Common::Path("test.glslp"), archives) && pipeline.isAnimated(), "Prev2 preset requests animation and history");
	for (uint frame = 0; frame < 8; ++frame) {
		input.fillRect(Common::Rect(4, 4), input.format.ARGBToColor(255, frame * 30, 0, 0));
		r.updateTexture(*texture, input, Common::Rect(4, 4));
		output = pipeline.render(*texture, 16, 16, false);
		check(output != nullptr, "history ring frame renders");
		if (frame >= 3) pixel(*output, r, 8, 8, (frame - 3) * 30, 0, 0, "Prev2 history remains three frames old across wraparound");
	}
	pipeline.close();
	archive.preset = "shaders = 2\nshader0 = first.glsl\nalias0 = First\nshader1 = second.glsl\nmipmap_input1 = true\n";
	archive.first = presetSource("", "vec4(0,1,0,1)");
	archive.second = presetSource("uniform sampler2D FirstTexture; uniform sampler2D OrigTexture;",
		"0.5*(texture2D(FirstTexture,vec2(0.5))+texture2D(OrigTexture,vec2(0.5)))");
	check(pipeline.open(Common::Path("test.glslp"), archives), "multipass preset resolves alias and original input");
	input.fillRect(Common::Rect(4, 4), input.format.ARGBToColor(255, 255, 0, 0));
	r.updateTexture(*texture, input, Common::Rect(4, 4));
	output = pipeline.render(*texture, 16, 16, false);
	check(output != nullptr, "multipass alias with mipmapped pass input renders");
	pixel(*output, r, 8, 8, 128, 128, 0, "pass alias and original texture remain distinct");
	pipeline.close();
	archive.preset = "shaders = 1\nshader0 = first.glsl\n";
	archive.first = presetSource("uniform int FrameCount; uniform vec2 InputSize; uniform vec2 OutputSize;",
		"vec4(float(FrameCount)/255.0,InputSize.x/255.0,OutputSize.y/255.0,1.0)");
	check(pipeline.open(Common::Path("test.glslp"), archives), "dynamic preset size and frame uniforms compile");
	for (uint frame = 0; frame < 3; ++frame) {
		output = pipeline.render(*texture, 24, 20, false);
		check(output != nullptr, "animated preset with cached geometry renders");
		pixel(*output, r, 12, 10, frame, 4, 20, "frame count advances while cached size uniforms stay correct");
	}
	delete texture; input.free(); input.create(8, 2, PixelFormat::createFormatRGBA32());
	input.fillRect(Common::Rect(8, 2), 0xffffffff); texture = r.createTexture(input);
	output = pipeline.render(*texture, 32, 24, false);
	check(output != nullptr, "preset input and viewport resize together");
	pixel(*output, r, 16, 12, 0, 8, 24, "input resize resets history and refreshes all derived uniforms");
	pipeline.close(); delete texture; input.free();
}

int main() {
	setvbuf(stdout, nullptr, _IONBF, 0);
	@autoreleasepool {
		Renderer renderer;
		check(renderer.attach(nullptr) && renderer.resize(16, 16, 1, false), "native offscreen Metal 4 renderer");
		bufferVersions(renderer);
		transientSlices(renderer);
		geometryLRU(renderer);
		textureFormats(renderer);
		hazards(renderer);
		mipPromotionAndTargetCopies(renderer);
		uniformsAndState(renderer);
		depthAndQueries(renderer);
		queues(renderer);
		engineUniforms(renderer);
		presets(renderer);
		renderer.flush(true);
		printf("Completed %u checks\n", checks);
	}
	return 0;
}
