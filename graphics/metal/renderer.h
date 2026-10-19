/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_RENDERER_H
#define GRAPHICS_METAL_RENDERER_H

#include "common/noncopyable.h"
#include "common/array.h"
#include "common/rect.h"
#include "graphics/surface.h"

namespace Graphics {
namespace Metal {

enum Primitive { kTriangles, kTriangleStrip, kLines, kLineStrip, kPoints };
enum Compare { kNever, kLess, kEqual, kLessEqual, kGreater, kNotEqual, kGreaterEqual, kAlways };
enum StencilOp { kKeep, kZero, kReplace, kIncrementClamp, kDecrementClamp, kInvert, kIncrementWrap, kDecrementWrap };
enum BlendFactor { kBlendZero, kBlendOne, kSrcColor, kOneMinusSrcColor, kSrcAlpha, kOneMinusSrcAlpha,
                   kDstColor, kOneMinusDstColor, kDstAlpha, kOneMinusDstAlpha, kSrcAlphaSaturate,
                   kConstantColor, kOneMinusConstantColor, kConstantAlpha, kOneMinusConstantAlpha };
enum BlendOp { kBlendAdd, kBlendSubtract, kBlendReverseSubtract, kBlendMin, kBlendMax };
enum Wrap { kClamp, kRepeat, kMirror, kBorder };
enum TargetFormat { kTargetBGRA8, kTargetSRGBA8, kTargetRGBA16Float };
enum CompressedFormat { kBC1, kBC2, kBC3 };

struct Vertex {
	float position[3];
	float texcoord[2];
	float color[4];
};

struct Uniforms {
	float transform[16];
	float tint[4];
	float alphaReference;
	uint32 alphaCompare;
	uint32 textured;
	uint32 stippled;
	uint32 stipple[32];
	float modelView[16];
	float fogColor[4];
	float fogParameters[4];
	uint32 framebufferHeight;
	uint32 textureMode;
	float lodBias;
	uint32 posterize;

	Uniforms();
};

struct StencilState {
	Compare compare = kAlways;
	StencilOp fail = kKeep, depthFail = kKeep, pass = kKeep;
	uint32 readMask = 0xff, writeMask = 0xff;
};

struct DrawState {
	Primitive primitive = kTriangles;
	bool blend = false;
	bool logicXor = false;
	float blendColor[4] = {0, 0, 0, 0};
	BlendFactor srcRGB = kSrcAlpha, dstRGB = kOneMinusSrcAlpha;
	BlendFactor srcAlpha = kBlendOne, dstAlpha = kOneMinusSrcAlpha;
	BlendOp rgbOp = kBlendAdd, alphaOp = kBlendAdd;
	uint32 colorMask = 15;
	bool depthTest = false, depthWrite = false;
	Compare depthCompare = kLessEqual;
	bool stencil = false;
	StencilState frontStencil, backStencil;
	uint32 stencilReference = 0;
	bool cull = false, cullFront = false, clockwise = false;
	bool wireframe = false;
	bool depthClamp = false;
	float depthNear = 0, depthFar = 1;
	float depthBias = 0, slopeBias = 0;
	bool linear = true;
	bool mipLinear = true;
	bool mipmaps = true;
	Wrap wrapS = kClamp, wrapT = kClamp, wrapR = kClamp;
	uint anisotropy = 1;
	bool scissor = false;
	Common::Rect scissorRect;
};

class Renderer;
class RenderTarget;

// Replacing a buffer publishes a new immutable version. Submissions retain the
// version they encoded, so callers can update or destroy it without a CPU wait.
class Buffer : public Common::NonCopyable {
public:
	~Buffer();
	uint size() const;
private:
	friend class Renderer;
	struct Impl;
	explicit Buffer(Impl *impl) : _impl(impl) {}
	Impl *_impl;
};

struct GeometryBinding {
	Buffer *vertices = nullptr;
	Buffer *indices = nullptr;
	uint vertexOffset = 0, indexOffset = 0;
	bool indices16 = false;
};

struct UniformBindings {
	const Buffer *vertex = nullptr, *fragment = nullptr;
	const Buffer *extraVertex = nullptr, *extraFragment = nullptr;
	const void *extraData = nullptr;
	uint extraSize = 0;
};

struct Statistics {
	uint64 uploadedBytes = 0, persistentBufferBytes = 0;
	uint64 pipelineArchiveDisables = 0;
	uint64 pipelineArchiveHits = 0, pipelineArchiveRejects = 0, pipelineArchiveWrites = 0, pipelineCompilations = 0;
	uint64 uploadPages = 0, uploadPageReuses = 0;
	uint64 submissions = 0, submissionObjects = 0, residencySets = 0;
	uint64 argumentTableReuses = 0;
	uint64 argumentTables = 0, argumentTableClears = 0, argumentTableWrites = 0;
	uint64 encoderStateSetters = 0, queueBarriers = 0;
};

class OcclusionQuery : public Common::NonCopyable {
public:
	~OcclusionQuery();

private:
	friend class Renderer;
	struct Impl;
	explicit OcclusionQuery(Impl *impl) : _impl(impl) {}
	Impl *_impl;
};

class Program : public Common::NonCopyable {
public:
	~Program();

private:
	friend class Renderer;
	struct Impl;
	explicit Program(Impl *impl) : _impl(impl) {}
	Impl *_impl;
};

class Texture : public Common::NonCopyable {
public:
	~Texture();
	uint width() const;
	uint height() const;
	uint depth() const;

private:
	friend class Renderer;
	struct Impl;
	explicit Texture(Impl *impl) : _impl(impl) {}
	Impl *_impl;
};

class RenderTarget : public Common::NonCopyable {
public:
	~RenderTarget();
	Texture &texture() const { return *_texture; }

private:
	friend class Renderer;
	struct Impl;
	RenderTarget(Impl *impl, Texture *texture) : _impl(impl), _texture(texture) {}
	Impl *_impl;
	Texture *_texture;
};

struct TextureBinding {
	Texture *texture = nullptr;
	bool linear = true;
	bool mipLinear = true;
	bool mipmaps = true;
	Wrap wrapS = kClamp, wrapT = kClamp, wrapR = kClamp;
	uint anisotropy = 1;
};

struct VertexAttribute {
	enum Type { kFloat, kUNorm8 };
	uint index = 0;
	uint offset = 0;
	uint components = 2;
	Type type = kFloat;
};

class Renderer : public Common::NonCopyable {
public:
	Renderer();
	~Renderer();
	static bool isSupported();
	static bool isMetalFXSupported();
	static Common::Array<uint> supportedSampleCounts();
	// A null layer initializes the same supported GPU for offscreen recording.
	bool attach(void *layer, Renderer *sharedDevice = nullptr);
	void detachLayer();
	void detach();
	bool resize(uint width, uint height, uint samples = 1, bool depthStencil = true);
	void setVSync(bool enabled);
	uint width() const;
	uint height() const;
	uint sampleCount() const;
	uint maxTextureSize() const;
	bool supportsSamples(uint count) const;
	void setMultisamplingActive(bool enabled);
	RenderTarget *createRenderTarget(uint width, uint height, uint samples = 1,
	                                 TargetFormat format = kTargetBGRA8, bool mipmaps = false, bool depthStencil = true);
	void setRenderTarget(RenderTarget *target);
	RenderTarget *renderTarget() const;
	Texture *screenTexture() const;
	// Returns a cached, renderer-owned opaque SDR image, or null for ordinary
	// scaling. Only enlargements of supported 2D color textures use MetalFX.
	Texture *upscaleMetalFX(Texture &source, uint width, uint height);
	void resetMetalFX();
	Buffer *createBuffer(const void *data, uint size);
	// Immutable arena slice; safe across submissions, callbacks and early deletion.
	Buffer *createTransientBuffer(const void *data, uint size);
	void updateBuffer(Buffer &buffer, const void *data, uint size);

	Texture *createTexture(const Surface &surface, bool mipmaps = false, uint mipLevels = 0);
	// Uninitialized storage: upload or copy its contents before sampling it.
	Texture *allocateTexture(uint width, uint height, bool mipmaps = false, uint mipLevels = 0, const PixelFormat *format = nullptr);
	// Returns null when BC storage is unsupported; callers retain their decoder.
	Texture *createCompressedTexture(const void *data, uint bytes, uint width, uint height, CompressedFormat format);
	Texture *createIndexTexture(const Surface &surface);
	void updateIndexTexture(Texture &texture, const Surface &surface, const Common::Rect &rect);
	Texture *createCubeTexture(const Surface *faces, bool mipmaps = false);
	Texture *createVolumeTexture(const void *rgba, uint bytes, uint width, uint height, uint depth, bool mipmaps = false);
	void updateTexture(Texture &texture, const Surface &surface, const Common::Rect &rect);
	void updateTexture(Texture &texture, const Surface &surface, const Common::Rect &rect, const Common::Point &destination);
	void updateTextureLevel(Texture &texture, uint level, const Surface &surface, const Common::Rect &rect, uint slice = 0,
	                        const Common::Point *destination = nullptr);
	void generateMipmaps(Texture &texture);
	void copyTexture(Texture &destination, const Common::Rect &destinationRect,
	                 Texture &source, const Common::Rect &sourceRect, bool flipY = false);
	bool readTexture(Texture &texture, Surface &surface, const Common::Rect &rect);
	// Native MSL programs use vertex data at buffer(0), uniforms at buffer(1),
	// and texture/sampler pairs in consecutive slots starting at zero.
	Program *createProgram(const char *source, const char *vertexFunction, const char *fragmentFunction,
	                       const VertexAttribute *attributes = nullptr, uint attributeCount = 0, uint vertexStride = 0);
	Program *createProgramStages(const char *vertexSource, const char *vertexFunction,
	                             const char *fragmentSource, const char *fragmentFunction,
	                             const VertexAttribute *attributes = nullptr, uint attributeCount = 0, uint vertexStride = 0);
	bool validateProgram(Program &program, TargetFormat format, uint samples = 1);
	void drawProgram(Program &program, const void *vertices, uint count, uint stride,
	                 const uint32 *indices, uint indexCount, const void *uniforms, uint uniformSize,
	                 const TextureBinding *textures, uint textureCount, const DrawState &state,
	                 const void *fragmentUniforms = nullptr, uint fragmentUniformSize = 0,
	                 const GeometryBinding *geometry = nullptr, const UniformBindings *bindings = nullptr);
	OcclusionQuery *createOcclusionQuery();
	void beginOcclusionQuery(OcclusionQuery &query);
	void endOcclusionQuery(OcclusionQuery &query);
	bool occlusionQueryReady(const OcclusionQuery &query) const;
	uint64 occlusionQueryResult(OcclusionQuery &query, bool wait = true);

	void clear(float r, float g, float b, float a = 1, bool color = true, bool depth = true, bool stencil = true,
	           float depthValue = 1, uint32 stencilValue = 0);
	void clearRect(const Common::Rect &rect, float r, float g, float b, float a = 1,
	               bool color = true, bool depth = true, bool stencil = false, uint32 colorMask = 15,
	               float depthValue = 1, uint32 stencilValue = 0);
	void setViewport(const Common::Rect &rect);
	Common::Rect viewport() const;
	void draw(const Vertex *vertices, uint count, Texture *texture, const DrawState &state, const Uniforms &uniforms);
	void drawWideLine(const Vertex &start, const Vertex &end, float lineWidth, const DrawState &state, const Uniforms &uniforms);
	void drawIndexed(const Vertex *vertices, uint count, const uint32 *indices, uint indexCount,
	                 Texture *texture, const DrawState &state, const Uniforms &uniforms);
	void drawBuffered(const GeometryBinding &geometry, uint count, uint indexCount,
	                  Texture *texture, const DrawState &state, const Uniforms &uniforms);
	bool present(RenderTarget *source = nullptr);
	void flush(bool wait = false);
	// Order subsequent work after a producer on the same device, on the GPU.
	void waitFor(Renderer &producer);
	Statistics statistics() const;
	void resetStatistics();
	bool readPixels(Surface &surface, const Common::Rect &rect);

private:
	struct Impl;
	Impl *_impl;
};

} // End of namespace Metal
} // End of namespace Graphics

#endif
