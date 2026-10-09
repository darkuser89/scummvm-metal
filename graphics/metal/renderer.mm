/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "graphics/metal/renderer.h"
#include "graphics/metal/shaders.h"
#include "common/textconsole.h"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>

namespace Graphics {
namespace Metal {

static_assert(sizeof(Vertex) == 36, "Metal vertex layout must match the packed MSL structure");
static_assert(sizeof(Uniforms) == 336, "Metal uniform layout must match the MSL structure");

Uniforms::Uniforms() : alphaReference(0), alphaCompare(kAlways), textured(0), stippled(0), framebufferHeight(0), textureMode(0), lodBias(0), posterize(0) {
	memset(transform, 0, sizeof(transform));
	transform[0] = transform[5] = transform[10] = transform[15] = 1;
	tint[0] = tint[1] = tint[2] = tint[3] = 1;
	memset(stipple, 0xff, sizeof(stipple));
	memset(modelView, 0, sizeof(modelView));
	modelView[0] = modelView[5] = modelView[10] = modelView[15] = 1;
	memset(fogColor, 0, sizeof(fogColor));
	memset(fogParameters, 0, sizeof(fogParameters));
}

static bool supportedDevice(id<MTLDevice> device) {
#if defined(__arm64__) && defined(MACOSX)
	if (@available(macOS 13.0, *)) {
		if (!device || ![device supportsFamily:MTLGPUFamilyMetal3] || ![device supportsFamily:MTLGPUFamilyApple7])
			return false;
		const char *name = [[device name] UTF8String];
		if (strncmp(name, "Apple M", 7) != 0 || name[7] < '1' || name[7] > '6')
			return false;
		return name[8] == '\0' || name[8] == ' ';
	}
#endif
	return false;
}

struct Target;

struct Texture::Impl {
	id<MTLTexture> texture;
	bool mipmaps;
	Target *owner = nullptr;
};

Texture::~Texture() {
	[_impl->texture release];
	delete _impl;
}

uint Texture::width() const { return [_impl->texture width]; }
uint Texture::height() const { return [_impl->texture height]; }
uint Texture::depth() const { return [_impl->texture depth]; }

struct OcclusionQuery::Impl {
	id<MTLBuffer> buffer = nil;
	id<MTLCommandBuffer> command = nil;
	uint passes = 0;
	~Impl() { [buffer release]; [command release]; }
};

OcclusionQuery::~OcclusionQuery() { delete _impl; }

static MTLCompareFunction compareFunction(Compare value) {
	static const MTLCompareFunction functions[] = {
		MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual, MTLCompareFunctionLessEqual,
		MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways
	};
	return functions[value];
}

static MTLStencilOperation stencilOperation(StencilOp value) {
	static const MTLStencilOperation operations[] = {
		MTLStencilOperationKeep, MTLStencilOperationZero, MTLStencilOperationReplace,
		MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
		MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap
	};
	return operations[value];
}

static MTLBlendFactor blendFactor(BlendFactor value) {
	static const MTLBlendFactor factors[] = {
		MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
		MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationColor,
		MTLBlendFactorOneMinusDestinationColor, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
		MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor,
		MTLBlendFactorBlendAlpha, MTLBlendFactorOneMinusBlendAlpha
	};
	return factors[value];
}

static MTLBlendOperation blendOperation(BlendOp value) {
	static const MTLBlendOperation operations[] = {
		MTLBlendOperationAdd, MTLBlendOperationSubtract, MTLBlendOperationReverseSubtract,
		MTLBlendOperationMin, MTLBlendOperationMax
	};
	return operations[value];
}

static MTLSamplerAddressMode wrapMode(Wrap value) {
	static const MTLSamplerAddressMode modes[] = {
		MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
		MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeClampToBorderColor
	};
	return modes[value];
}

static MTLStencilDescriptor *stencilDescriptor(const StencilState &state) {
	MTLStencilDescriptor *descriptor = [[[MTLStencilDescriptor alloc] init] autorelease];
	descriptor.stencilCompareFunction = compareFunction(state.compare);
	descriptor.stencilFailureOperation = stencilOperation(state.fail);
	descriptor.depthFailureOperation = stencilOperation(state.depthFail);
	descriptor.depthStencilPassOperation = stencilOperation(state.pass);
	descriptor.readMask = state.readMask;
	descriptor.writeMask = state.writeMask;
	return descriptor;
}

// Repeated draws often use the same key. Keep its immutable NSData instead
// of allocating another object; still look up the dictionary so program
// destruction can remove pipeline entries without leaving a stale state.
static NSData *reuseStateKey(NSData *&key, const void *values, NSUInteger length) {
	if (!key || key.length != length || memcmp(key.bytes, values, length)) {
		[key release];
		key = [[NSData alloc] initWithBytes:values length:length];
	}
	return key;
}

struct Target {
	id<MTLTexture> color = nil, multisample = nil, depth = nil;
	uint w = 0, h = 0, samples = 1;
	Common::Rect viewport;
	MTLClearColor clearColor = MTLClearColorMake(0, 0, 0, 1);
	double clearDepth = 1;
	uint32 clearStencil = 0;
	bool multisamplingActive = true;
	bool clearColorPending = true, clearDepthPending = true, clearStencilPending = true;

	~Target() {
		[color release];
		[multisample release];
		[depth release];
	}
};

struct RenderTarget::Impl : Target {};

struct Program::Impl {
	id<MTLLibrary> library = nil;
	id<MTLLibrary> fragmentLibrary = nil;
	id<MTLFunction> vertex = nil, fragment = nil;
	MTLVertexDescriptor *vertexDescriptor = nil;
	NSMutableDictionary *pipelineCache = nil;
	NSMutableArray *pipelineKeys = nil;
	~Impl() {
		for (id key in pipelineKeys) [pipelineCache removeObjectForKey:key];
		[pipelineKeys release];
		[pipelineCache release];
		[vertexDescriptor release];
		[vertex release];
		[fragment release];
		[library release];
		[fragmentLibrary release];
	}
};

Program::~Program() { delete _impl; }

RenderTarget::~RenderTarget() {
	delete _texture;
	delete _impl;
}

static bool allocateTarget(id<MTLDevice> device, Target &target, uint w, uint h, uint samples,
		TargetFormat format = kTargetBGRA8, bool mipmaps = false) {
	MTLPixelFormat pixelFormat = format == kTargetRGBA16Float ? MTLPixelFormatRGBA16Float :
		format == kTargetSRGBA8 ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat width:w height:h mipmapped:mipmaps];
	desc.storageMode = MTLStorageModePrivate;
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	id<MTLTexture> color = [device newTextureWithDescriptor:desc];
	desc.mipmapLevelCount = 1;
	id<MTLTexture> msaa = nil;
	if (samples > 1) {
		desc.textureType = MTLTextureType2DMultisample;
		desc.sampleCount = samples;
		desc.usage = MTLTextureUsageRenderTarget;
		msaa = [device newTextureWithDescriptor:desc];
	}
	desc.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
	desc.usage = MTLTextureUsageRenderTarget;
	id<MTLTexture> depth = [device newTextureWithDescriptor:desc];
	if (!color || !depth || (samples > 1 && !msaa)) {
		[color release]; [msaa release]; [depth release];
		return false;
	}
	[target.color release]; target.color = color;
	[target.multisample release]; target.multisample = msaa;
	[target.depth release]; target.depth = depth;
	target.w = w; target.h = h; target.samples = samples;
	target.viewport = Common::Rect(w, h);
	target.clearColorPending = target.clearDepthPending = target.clearStencilPending = true;
	return true;
}

struct Renderer::Impl {
	id<MTLDevice> device = nil;
	id<MTLCommandQueue> queue = nil;
	id<MTLLibrary> library = nil;
	id<MTLFunction> vertex = nil, fragment = nil, logicFragment = nil;
	CAMetalLayer *layer = nil;
	Target screen;
	Target *target = &screen;
	RenderTarget *renderTarget = nullptr;
	Texture *screenTexture = nullptr;
	id<MTLTexture> white = nil;
	id<MTLFXSpatialScaler> metalFX = nil;
	id<MTLTexture> metalFXInput = nil;
	Texture *metalFXOutput = nullptr;
	uint metalFXWidth = 0, metalFXHeight = 0, metalFXOutputWidth = 0, metalFXOutputHeight = 0;
	MTLPixelFormat metalFXFormat = MTLPixelFormatInvalid;
	bool metalFXSupported = false;
	bool metalFXCopyFailed = false;
	id<MTLCommandBuffer> command = nil;
	id<MTLRenderCommandEncoder> encoder = nil;
	OcclusionQuery::Impl *query = nullptr;
	NSMutableDictionary *pipelines = nil, *depthStates = nil, *samplers = nil;
	NSData *lastPipelineKey = nil, *lastDepthKey = nil;
	NSMutableArray *freeDrawBuffers = nil, *drawBuffers = nil;
	NSLock *drawBufferLock = nil;
	id<MTLBuffer> drawBuffer = nil;
	NSUInteger drawBufferOffset = 0;
	dispatch_semaphore_t inFlight;
	Common::Rect viewport;

	Impl() : inFlight(dispatch_semaphore_create(3)) {
		freeDrawBuffers = [[NSMutableArray alloc] init];
		drawBufferLock = [[NSLock alloc] init];
	}
	~Impl() {
		[drawBuffers release];
		[freeDrawBuffers release];
		[drawBufferLock release];
	}

	// Draws and texture uploads share an arena. Completed command buffers return
	// pages to the pool; CPU writes never overlap GPU reads still in flight.
	id<MTLBuffer> reserveUploadData(NSUInteger length, NSUInteger &offset) {
		if (!length) { offset = 0; return nil; }
		beginCommand();
		NSUInteger aligned = (drawBufferOffset + 255) & ~(NSUInteger)255;
		if (!drawBuffer || aligned > drawBuffer.length || length > drawBuffer.length - aligned) {
			NSUInteger pageLength = MAX((NSUInteger)(1024 * 1024), (length + 255) & ~(NSUInteger)255);
			drawBuffer = nil;
			[drawBufferLock lock];
			for (NSUInteger i = 0; i < freeDrawBuffers.count; ++i) {
				id<MTLBuffer> candidate = [freeDrawBuffers objectAtIndex:i];
				if (candidate.length >= pageLength) {
					drawBuffer = [candidate retain];
					[freeDrawBuffers removeObjectAtIndex:i];
					break;
				}
			}
			[drawBufferLock unlock];
			if (!drawBuffer) drawBuffer = [device newBufferWithLength:pageLength options:MTLResourceStorageModeShared];
			if (!drawBuffer) error("Metal: could not allocate upload arena");
			if (!drawBuffers) drawBuffers = [[NSMutableArray alloc] init];
			[drawBuffers addObject:drawBuffer];
			[drawBuffer release];
			aligned = 0;
		}
		offset = aligned;
		drawBufferOffset = offset + length;
		return drawBuffer;
	}

	id<MTLBuffer> uploadDrawData(const void *data, NSUInteger length, NSUInteger &offset) {
		id<MTLBuffer> buffer = reserveUploadData(length, offset);
		if (length) memcpy((byte *)buffer.contents + offset, data, length);
		return buffer;
	}

	void beginCommand() {
		if (!command) {
			command = [[queue commandBuffer] retain];
			if (!command)
				error("Metal: could not allocate command buffer");
		}
	}

	void endEncoder() {
		if (encoder) {
			[encoder endEncoding];
			[encoder release];
			encoder = nil;
		}
	}

	void resolveTextureOwner(Texture::Impl *texture) {
		endEncoder();
		if (!texture->owner) return;
		Target *source = texture->owner;
		if (!source->clearColorPending && !source->clearDepthPending && !source->clearStencilPending) return;
		Target *previous = target;
		OcclusionQuery::Impl *previousQuery = query;
		target = source; query = nullptr;
		beginEncoder(); endEncoder();
		target = previous; query = previousQuery;
	}

	void submit(bool wait) {
		endEncoder();
		if (!command)
			return;
		dispatch_semaphore_wait(inFlight, DISPATCH_TIME_FOREVER);
		dispatch_semaphore_t semaphore = inFlight;
		NSMutableArray *pages = drawBuffers;
		NSMutableArray *pool = freeDrawBuffers;
		NSLock *poolLock = drawBufferLock;
		[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
			if (completed.status == MTLCommandBufferStatusError)
				warning("Metal command failed: %s", [[completed.error localizedDescription] UTF8String]);
			[poolLock lock];
			// Bound both page count and retained bytes after unusually large uploads.
			NSUInteger poolBytes = 0;
			for (id<MTLBuffer> page in pool) poolBytes += page.length;
			for (id<MTLBuffer> page in pages) {
				if (pool.count < 32 && page.length <= 16 * 1024 * 1024 &&
						poolBytes + page.length <= 64 * 1024 * 1024) {
					[pool addObject:page];
					poolBytes += page.length;
				}
			}
			[poolLock unlock];
			dispatch_semaphore_signal(semaphore);
		}];
		[drawBuffers release]; drawBuffers = nil;
		drawBuffer = nil; drawBufferOffset = 0;
		[command commit];
		if (wait)
			[command waitUntilCompleted];
		[command release];
		command = nil;
	}

	void beginEncoder() {
		if (encoder)
			return;
		beginCommand();
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = target->samples > 1 ? target->multisample : target->color;
		pass.colorAttachments[0].resolveTexture = target->samples > 1 ? target->color : nil;
		pass.colorAttachments[0].loadAction = target->clearColorPending ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = target->samples > 1 ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = target->clearColor;
		pass.depthAttachment.texture = target->depth;
		pass.depthAttachment.loadAction = target->clearDepthPending ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.depthAttachment.clearDepth = target->clearDepth;
		pass.stencilAttachment.texture = target->depth;
		pass.stencilAttachment.loadAction = target->clearStencilPending ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.stencilAttachment.storeAction = MTLStoreActionStore;
		pass.stencilAttachment.clearStencil = target->clearStencil;
		if (target->samples > 1) {
			MTLSamplePosition positions[8];
			if (target->multisamplingActive)
				[device getDefaultSamplePositions:positions count:target->samples];
			else
				for (uint i = 0; i < target->samples; ++i) positions[i] = MTLSamplePositionMake(0.5, 0.5);
			[pass setSamplePositions:positions count:target->samples];
			pass.depthAttachment.storeActionOptions = MTLStoreActionOptionCustomSamplePositions;
		}
		if (query) pass.visibilityResultBuffer = query->buffer;
		encoder = [[command renderCommandEncoderWithDescriptor:pass] retain];
		if (!encoder)
			error("Metal: could not begin render pass");
		if (query) {
			if (query->passes >= 512) error("Metal: too many render passes in an occlusion query");
			[encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:query->passes++ * sizeof(uint64)];
		}
		target->clearColorPending = target->clearDepthPending = target->clearStencilPending = false;
	}

	id<MTLRenderPipelineState> pipeline(const DrawState &state, bool presentation = false, Program::Impl *program = nullptr,
			MTLPixelFormat colorFormat = MTLPixelFormatInvalid) {
		if (colorFormat == MTLPixelFormatInvalid)
			colorFormat = presentation ? MTLPixelFormatBGRA8Unorm : target->color.pixelFormat;
		id<MTLFunction> vertexFunction = program ? program->vertex : vertex;
		id<MTLFunction> fragmentFunction = program ? program->fragment : state.logicXor ? logicFragment : fragment;
		// Fixed-width keys avoid formatting pointer and state strings for every draw.
		const uintptr values[] = {(uintptr)vertexFunction, (uintptr)fragmentFunction, (uintptr)colorFormat, presentation,
			presentation ? 1U : target->samples, state.blend, state.srcRGB, state.dstRGB, state.srcAlpha,
			state.dstAlpha, state.rgbOp, state.alphaOp, state.colorMask};
		NSData *key = reuseStateKey(lastPipelineKey, values, sizeof(values));
		id<MTLRenderPipelineState> result = [pipelines objectForKey:key];
		if (!result) {
			MTLRenderPipelineDescriptor *descriptor = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
			descriptor.vertexFunction = vertexFunction;
			descriptor.fragmentFunction = fragmentFunction;
			descriptor.vertexDescriptor = program ? program->vertexDescriptor : nil;
			descriptor.rasterSampleCount = presentation ? 1 : target->samples;
			descriptor.colorAttachments[0].pixelFormat = colorFormat;
			descriptor.depthAttachmentPixelFormat = presentation ? MTLPixelFormatInvalid : MTLPixelFormatDepth32Float_Stencil8;
			descriptor.stencilAttachmentPixelFormat = descriptor.depthAttachmentPixelFormat;
			MTLRenderPipelineColorAttachmentDescriptor *blend = descriptor.colorAttachments[0];
			blend.blendingEnabled = state.blend && !state.logicXor;
			blend.sourceRGBBlendFactor = blendFactor(state.srcRGB);
			blend.destinationRGBBlendFactor = blendFactor(state.dstRGB);
			blend.sourceAlphaBlendFactor = blendFactor(state.srcAlpha);
			blend.destinationAlphaBlendFactor = blendFactor(state.dstAlpha);
			blend.rgbBlendOperation = blendOperation(state.rgbOp);
			blend.alphaBlendOperation = blendOperation(state.alphaOp);
			blend.writeMask = (MTLColorWriteMask)((state.colorMask & 1 ? MTLColorWriteMaskRed : 0) |
				(state.colorMask & 2 ? MTLColorWriteMaskGreen : 0) | (state.colorMask & 4 ? MTLColorWriteMaskBlue : 0) |
				(state.colorMask & 8 ? MTLColorWriteMaskAlpha : 0));
			NSError *failure = nil;
			result = [device newRenderPipelineStateWithDescriptor:descriptor error:&failure];
			if (!result)
				error("Metal pipeline: %s", [[failure localizedDescription] UTF8String]);
			[pipelines setObject:result forKey:key];
			if (program) [program->pipelineKeys addObject:key];
			[result release];
		}
		return result;
	}

	id<MTLDepthStencilState> depthState(const DrawState &state) {
		const StencilState &front = state.frontStencil, &back = state.backStencil;
		const uint32 values[] = {state.depthTest, state.depthWrite, state.depthCompare, state.stencil,
			front.compare, front.fail, front.depthFail, front.pass, front.readMask, front.writeMask,
			back.compare, back.fail, back.depthFail, back.pass, back.readMask, back.writeMask};
		NSData *key = reuseStateKey(lastDepthKey, values, sizeof(values));
		id<MTLDepthStencilState> result = [depthStates objectForKey:key];
		if (!result) {
			MTLDepthStencilDescriptor *descriptor = [[[MTLDepthStencilDescriptor alloc] init] autorelease];
			descriptor.depthCompareFunction = state.depthTest ? compareFunction(state.depthCompare) : MTLCompareFunctionAlways;
			descriptor.depthWriteEnabled = state.depthTest && state.depthWrite;
			if (state.stencil) {
				descriptor.frontFaceStencil = stencilDescriptor(front);
				descriptor.backFaceStencil = stencilDescriptor(back);
			}
			result = [device newDepthStencilStateWithDescriptor:descriptor];
			if (!result)
				error("Metal: could not create depth/stencil state");
			[depthStates setObject:result forKey:key];
			[result release];
		}
		return result;
	}

	id<MTLSamplerState> sampler(const DrawState &state, bool mipmaps) {
		uint anisotropy = CLIP(state.anisotropy, 1U, 16U);
		NSNumber *key = [NSNumber numberWithUnsignedInt:state.linear | (state.wrapS << 1) | (state.wrapT << 3) | (mipmaps << 5) | (state.mipLinear << 6) | (state.wrapR << 7) | (anisotropy << 9)];
		id<MTLSamplerState> result = [samplers objectForKey:key];
		if (!result) {
			MTLSamplerDescriptor *descriptor = [[[MTLSamplerDescriptor alloc] init] autorelease];
			descriptor.minFilter = descriptor.magFilter = state.linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
			descriptor.mipFilter = mipmaps ? (state.mipLinear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest) : MTLSamplerMipFilterNotMipmapped;
			descriptor.sAddressMode = wrapMode(state.wrapS);
			descriptor.tAddressMode = wrapMode(state.wrapT);
			descriptor.rAddressMode = wrapMode(state.wrapR);
			descriptor.maxAnisotropy = anisotropy;
			result = [device newSamplerStateWithDescriptor:descriptor];
			if (!result)
				error("Metal: could not create sampler");
			[samplers setObject:result forKey:key];
			[result release];
		}
		return result;
	}

	void prepareDraw(Texture::Impl *texture, const DrawState &state, const Uniforms &uniforms,
			Program::Impl *program = nullptr, bool bindTexture = true) {
		beginEncoder();
		[encoder setRenderPipelineState:pipeline(state, false, program)];
		[encoder setDepthStencilState:depthState(state)];
		[encoder setStencilReferenceValue:state.stencilReference];
		[encoder setBlendColorRed:state.blendColor[0] green:state.blendColor[1] blue:state.blendColor[2] alpha:state.blendColor[3]];
		[encoder setCullMode:state.cull ? (state.cullFront ? MTLCullModeFront : MTLCullModeBack) : MTLCullModeNone];
		[encoder setFrontFacingWinding:state.clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise];
		[encoder setTriangleFillMode:state.wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
		[encoder setDepthClipMode:state.depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
		[encoder setDepthBias:state.depthBias slopeScale:state.slopeBias clamp:0];
		MTLViewport vp = {(double)viewport.left, (double)viewport.top, (double)viewport.width(), (double)viewport.height(), state.depthNear, state.depthFar};
		[encoder setViewport:vp];
		Common::Rect scissor(target->w, target->h);
		if (state.scissor)
			scissor.clip(state.scissorRect);
		if (scissor.isEmpty())
			return;
		[encoder setScissorRect:MTLScissorRect{(NSUInteger)scissor.left, (NSUInteger)scissor.top,
			(NSUInteger)scissor.width(), (NSUInteger)scissor.height()}];
		// Custom programs bind their own uniform buffers in drawProgram().
		if (!program) {
			Uniforms actual = uniforms;
			actual.textured = texture != nullptr;
			actual.framebufferHeight = target->h;
			[encoder setVertexBytes:&actual length:sizeof(actual) atIndex:1];
			[encoder setFragmentBytes:&actual length:sizeof(actual) atIndex:1];
		}
		if (bindTexture) {
			[encoder setFragmentTexture:texture ? texture->texture : white atIndex:0];
			[encoder setFragmentSamplerState:sampler(state, texture && texture->mipmaps && state.mipmaps) atIndex:0];
		}
	}
};

Renderer::Renderer() : _impl(new Impl()) {}

Renderer::~Renderer() {
	detach();
	dispatch_release(_impl->inFlight);
	delete _impl;
}

bool Renderer::isSupported() {
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		bool supported = supportedDevice(device);
		[device release];
		return supported;
	}
}

bool Renderer::isMetalFXSupported() {
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		bool supported = supportedDevice(device) && [MTLFXSpatialScalerDescriptor supportsDevice:device];
		[device release];
		return supported;
	}
}

Common::Array<uint> Renderer::supportedSampleCounts() {
	@autoreleasepool {
		Common::Array<uint> counts;
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (supportedDevice(device)) {
			const uint candidates[] = {2, 4, 8};
			for (uint i = 0; i < ARRAYSIZE(candidates); ++i)
				if ([device supportsTextureSampleCount:candidates[i]])
					counts.push_back(candidates[i]);
		}
		[device release];
		return counts;
	}
}

bool Renderer::attach(void *layer, Renderer *sharedDevice) {
	@autoreleasepool {
		// Window changes keep the same device and all engine-owned resources.
		if (_impl->device && _impl->library && _impl->queue) {
			detachLayer();
			_impl->layer = [(CAMetalLayer *)layer retain];
			_impl->layer.device = _impl->device;
			_impl->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
			_impl->layer.framebufferOnly = false;
			_impl->layer.opaque = true;
			return true;
		}
		detach();
		_impl->device = sharedDevice ? [sharedDevice->_impl->device retain] : MTLCreateSystemDefaultDevice();
		if (!supportedDevice(_impl->device)) {
			detach();
			return false;
		}
		_impl->metalFXSupported = [MTLFXSpatialScalerDescriptor supportsDevice:_impl->device];
		_impl->layer = [(CAMetalLayer *)layer retain];
		_impl->layer.device = _impl->device;
		_impl->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		_impl->layer.framebufferOnly = false;
		_impl->layer.opaque = true;
		_impl->queue = [_impl->device newCommandQueue];
		MTLCompileOptions *options = [[[MTLCompileOptions alloc] init] autorelease];
		options.languageVersion = MTLLanguageVersion3_0;
		NSError *failure = nil;
		_impl->library = [_impl->device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:options error:&failure];
		if (!_impl->queue || !_impl->library) {
			warning("Metal initialization failed: %s", failure ? [[failure localizedDescription] UTF8String] : "command queue unavailable");
			detach();
			return false;
		}
		_impl->vertex = [_impl->library newFunctionWithName:@"scummvmVertex"];
		_impl->fragment = [_impl->library newFunctionWithName:@"scummvmFragment"];
		_impl->logicFragment = [_impl->library newFunctionWithName:@"scummvmLogicFragment"];
		_impl->pipelines = [[NSMutableDictionary alloc] init];
		_impl->depthStates = [[NSMutableDictionary alloc] init];
		_impl->samplers = [[NSMutableDictionary alloc] init];
		MTLTextureDescriptor *white = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
		white.storageMode = MTLStorageModeShared;
		_impl->white = [_impl->device newTextureWithDescriptor:white];
		if (!_impl->vertex || !_impl->fragment || !_impl->logicFragment || !_impl->white) {
			detach();
			return false;
		}
		uint32 pixel = 0xffffffff;
		[_impl->white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&pixel bytesPerRow:4];
		return true;
	}
}

void Renderer::detachLayer() {
	@autoreleasepool {
		resetMetalFX();
		_impl->submit(true);
		if (_impl->queue) {
			id<MTLCommandBuffer> fence = [_impl->queue commandBuffer];
			[fence commit];
			[fence waitUntilCompleted];
		}
		_impl->target = &_impl->screen;
		_impl->renderTarget = nullptr;
		_impl->query = nullptr;
		[_impl->layer release];
		_impl->layer = nil;
	}
}

void Renderer::detach() {
	@autoreleasepool {
		resetMetalFX();
		_impl->metalFXSupported = false;
		_impl->submit(true);
		if (_impl->queue) {
			id<MTLCommandBuffer> fence = [_impl->queue commandBuffer];
			[fence commit];
			[fence waitUntilCompleted];
		}
		_impl->target = &_impl->screen;
		_impl->renderTarget = nullptr;
		_impl->query = nullptr;
		delete _impl->screenTexture; _impl->screenTexture = nullptr;
		[_impl->screen.color release]; _impl->screen.color = nil;
		[_impl->screen.multisample release]; _impl->screen.multisample = nil;
		[_impl->screen.depth release]; _impl->screen.depth = nil;
		[_impl->white release]; _impl->white = nil;
		[_impl->pipelines release]; _impl->pipelines = nil;
		[_impl->depthStates release]; _impl->depthStates = nil;
		[_impl->samplers release]; _impl->samplers = nil;
		[_impl->lastPipelineKey release]; _impl->lastPipelineKey = nil;
		[_impl->lastDepthKey release]; _impl->lastDepthKey = nil;
		[_impl->vertex release]; _impl->vertex = nil;
		[_impl->fragment release]; _impl->fragment = nil;
		[_impl->logicFragment release]; _impl->logicFragment = nil;
		[_impl->library release]; _impl->library = nil;
		[_impl->queue release]; _impl->queue = nil;
		[_impl->layer release]; _impl->layer = nil;
		[_impl->device release]; _impl->device = nil;
		[_impl->drawBufferLock lock];
		[_impl->freeDrawBuffers removeAllObjects];
		[_impl->drawBufferLock unlock];
		_impl->screen.w = _impl->screen.h = 0;
	}
}

bool Renderer::resize(uint w, uint h, uint samples) {
	@autoreleasepool {
		if (!_impl->device || !w || !h || w > maxTextureSize() || h > maxTextureSize() || !supportsSamples(samples))
			return false;
		if (w == _impl->screen.w && h == _impl->screen.h && samples == _impl->screen.samples)
			return true;
		_impl->submit(true);
		if (!allocateTarget(_impl->device, _impl->screen, w, h, samples))
			return false;
		if (!_impl->screenTexture) {
			Texture::Impl *texture = new Texture::Impl;
			texture->texture = [_impl->screen.color retain];
			texture->mipmaps = false;
			texture->owner = &_impl->screen;
			_impl->screenTexture = new Texture(texture);
		} else {
			// Engine-held references to the screen view remain valid after resize.
			[_impl->screenTexture->_impl->texture release];
			_impl->screenTexture->_impl->texture = [_impl->screen.color retain];
		}
		if (_impl->target == &_impl->screen)
			_impl->viewport = Common::Rect(w, h);
		return true;
	}
}

RenderTarget *Renderer::createRenderTarget(uint w, uint h, uint samples, TargetFormat format, bool mipmaps) {
	@autoreleasepool {
		if (!_impl->device || !w || !h || w > maxTextureSize() || h > maxTextureSize() || !supportsSamples(samples))
			return nullptr;
		RenderTarget::Impl *target = new RenderTarget::Impl;
		if (!allocateTarget(_impl->device, *target, w, h, samples, format, mipmaps)) {
			delete target;
			return nullptr;
		}
		Texture::Impl *texture = new Texture::Impl;
		texture->texture = [target->color retain];
		texture->mipmaps = mipmaps;
		texture->owner = target;
		return new RenderTarget(target, new Texture(texture));
	}
}

void Renderer::setRenderTarget(RenderTarget *target) {
	if (_impl->target->color && (_impl->target->clearColorPending || _impl->target->clearDepthPending || _impl->target->clearStencilPending))
		_impl->beginEncoder();
	_impl->endEncoder();
	_impl->target->viewport = _impl->viewport;
	_impl->target = target ? target->_impl : &_impl->screen;
	_impl->renderTarget = target;
	_impl->viewport = _impl->target->viewport;
}

Texture *Renderer::screenTexture() const { return _impl->screenTexture; }
RenderTarget *Renderer::renderTarget() const { return _impl->renderTarget; }

void Renderer::resetMetalFX() {
	@autoreleasepool {
		if (_impl->metalFX || _impl->metalFXOutput || _impl->metalFXInput)
			flush(true);
		[_impl->metalFX release]; _impl->metalFX = nil;
		[_impl->metalFXInput release]; _impl->metalFXInput = nil;
		delete _impl->metalFXOutput; _impl->metalFXOutput = nullptr;
		_impl->metalFXWidth = _impl->metalFXHeight = 0;
		_impl->metalFXOutputWidth = _impl->metalFXOutputHeight = 0;
		_impl->metalFXFormat = MTLPixelFormatInvalid;
		_impl->metalFXCopyFailed = false;
	}
}

Texture *Renderer::upscaleMetalFX(Texture &source, uint width, uint height) {
	@autoreleasepool {
		id<MTLTexture> input = source._impl->texture;
		if (&source == _impl->metalFXOutput || !_impl->metalFXSupported || input.device != _impl->device ||
			input.textureType != MTLTextureType2D || !width || !height ||
			width > maxTextureSize() || height > maxTextureSize() ||
			width < input.width || height < input.height ||
			(width == input.width && height == input.height) ||
			(input.pixelFormat != MTLPixelFormatBGRA8Unorm && input.pixelFormat != MTLPixelFormatRGBA8Unorm))
			return nullptr;

		if (_impl->metalFXWidth != input.width || _impl->metalFXHeight != input.height ||
			_impl->metalFXOutputWidth != width || _impl->metalFXOutputHeight != height ||
			_impl->metalFXFormat != input.pixelFormat) {
			resetMetalFX();
			// Cache failures as well: a rejected descriptor must not be retried
			// or logged on every frame. Resize or a new mode can try again.
			_impl->metalFXWidth = input.width; _impl->metalFXHeight = input.height;
			_impl->metalFXOutputWidth = width; _impl->metalFXOutputHeight = height;
			_impl->metalFXFormat = input.pixelFormat;
			MTLFXSpatialScalerDescriptor *desc = [[[MTLFXSpatialScalerDescriptor alloc] init] autorelease];
			desc.inputWidth = input.width; desc.inputHeight = input.height;
			desc.outputWidth = width; desc.outputHeight = height;
			desc.colorTextureFormat = input.pixelFormat;
			desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
			// Legacy engines supply display-encoded SDR values in unorm textures.
			// Linear processing here would apply the wrong transfer function.
			desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
			_impl->metalFX = [desc newSpatialScalerWithDevice:_impl->device];
			if (_impl->metalFX) {
				MTLTextureDescriptor *output = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
					MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
				output.storageMode = MTLStorageModePrivate;
				output.usage = _impl->metalFX.outputTextureUsage | MTLTextureUsageShaderRead;
				id<MTLTexture> texture = [_impl->device newTextureWithDescriptor:output];
				if (texture) {
					texture.label = @"ScummVM MetalFX spatial output";
					Texture::Impl *view = new Texture::Impl;
					view->texture = texture; view->mipmaps = false;
					_impl->metalFXOutput = new Texture(view);
				}
				if ((input.usage & _impl->metalFX.colorTextureUsage) != _impl->metalFX.colorTextureUsage) {
					MTLTextureDescriptor *copy = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
						input.pixelFormat width:input.width height:input.height mipmapped:NO];
					copy.storageMode = MTLStorageModePrivate;
					copy.usage = _impl->metalFX.colorTextureUsage;
					_impl->metalFXInput = [_impl->device newTextureWithDescriptor:copy];
					if (!_impl->metalFXInput) {
						delete _impl->metalFXOutput; _impl->metalFXOutput = nullptr;
					}
				}
			}
			if (!_impl->metalFXOutput) {
				[_impl->metalFX release]; _impl->metalFX = nil;
				[_impl->metalFXInput release]; _impl->metalFXInput = nil;
				warning("MetalFX: spatial scaler unavailable for %ux%u -> %ux%u; using ordinary scaling",
					(uint)input.width, (uint)input.height, width, height);
			} else {
				debug(2, "MetalFX: spatial scaler %ux%u -> %ux%u (perceptual SDR)",
					(uint)input.width, (uint)input.height, width, height);
			}
		}
		if (!_impl->metalFXOutput || _impl->metalFXCopyFailed) return nullptr;
		_impl->resolveTextureOwner(source._impl);
		_impl->beginCommand();
		// Usage requirements can differ between devices. A source with fewer
		// usage bits is copied into a compatible private texture on the GPU.
		if ((input.usage & _impl->metalFX.colorTextureUsage) != _impl->metalFX.colorTextureUsage) {
			if (!_impl->metalFXInput) {
				MTLTextureDescriptor *copy = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
					input.pixelFormat width:input.width height:input.height mipmapped:NO];
				copy.storageMode = MTLStorageModePrivate;
				copy.usage = _impl->metalFX.colorTextureUsage;
				_impl->metalFXInput = [_impl->device newTextureWithDescriptor:copy];
				if (!_impl->metalFXInput) {
					_impl->metalFXCopyFailed = true;
					warning("MetalFX: compatible input allocation failed; using ordinary scaling");
					return nullptr;
				}
			}
			id<MTLBlitCommandEncoder> blit = [_impl->command blitCommandEncoder];
			[blit copyFromTexture:input sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				sourceSize:MTLSizeMake(input.width, input.height, 1) toTexture:_impl->metalFXInput
				destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
			[blit endEncoding];
			input = _impl->metalFXInput;
		}
		_impl->metalFX.colorTexture = input;
		_impl->metalFX.outputTexture = _impl->metalFXOutput->_impl->texture;
		_impl->metalFX.inputContentWidth = input.width;
		_impl->metalFX.inputContentHeight = input.height;
		[_impl->metalFX encodeToCommandBuffer:_impl->command];
		_impl->metalFX.colorTexture = nil;
		_impl->metalFX.outputTexture = nil;
		return _impl->metalFXOutput;
	}
}

void Renderer::setVSync(bool enabled) { _impl->layer.displaySyncEnabled = enabled; }
uint Renderer::width() const { return _impl->target->w; }
uint Renderer::height() const { return _impl->target->h; }
uint Renderer::sampleCount() const { return _impl->target->samples; }
uint Renderer::maxTextureSize() const { return _impl->device ? 16384 : 0; }
bool Renderer::supportsSamples(uint count) const { return _impl->device && [_impl->device supportsTextureSampleCount:count]; }

Texture *Renderer::createTexture(const Surface &surface, bool mipmaps, uint mipLevels) {
	@autoreleasepool {
		if (!_impl->device || surface.w <= 0 || surface.h <= 0 || uint(surface.w) > maxTextureSize() || uint(surface.h) > maxTextureSize())
			return nullptr;
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:surface.w height:surface.h mipmapped:mipmaps];
		if (mipLevels) {
			if (mipLevels > desc.mipmapLevelCount)
				return nullptr;
			desc.mipmapLevelCount = mipLevels;
		}
		desc.storageMode = MTLStorageModePrivate;
		desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
		Texture::Impl *impl = new Texture::Impl;
		impl->texture = [_impl->device newTextureWithDescriptor:desc];
		impl->mipmaps = mipmaps;
		if (!impl->texture) {
			delete impl;
			return nullptr;
		}
		Texture *texture = new Texture(impl);
		updateTexture(*texture, surface, Common::Rect(surface.w, surface.h));
		return texture;
	}
}

Texture *Renderer::createIndexTexture(const Surface &surface) {
	@autoreleasepool {
		if (!_impl->device || !surface.format.isCLUT8() || surface.w <= 0 || surface.h <= 0 ||
		    (uint)surface.w > maxTextureSize() || (uint)surface.h > maxTextureSize())
			return nullptr;
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
			width:surface.w height:surface.h mipmapped:NO];
		descriptor.storageMode = MTLStorageModePrivate;
		descriptor.usage = MTLTextureUsageShaderRead;
		Texture::Impl *impl = new Texture::Impl;
		impl->texture = [_impl->device newTextureWithDescriptor:descriptor];
		impl->mipmaps = false;
		if (!impl->texture) { delete impl; return nullptr; }
		Texture *texture = new Texture(impl);
		updateIndexTexture(*texture, surface, Common::Rect(surface.w, surface.h));
		return texture;
	}
}

void Renderer::updateIndexTexture(Texture &texture, const Surface &surface, const Common::Rect &rect) {
	@autoreleasepool {
		if (!surface.format.isCLUT8() || texture._impl->texture.pixelFormat != MTLPixelFormatR8Unorm)
			error("Metal: index upload requires an R8 texture and CLUT8 surface");
		Common::Rect area = rect;
		area.clip(Common::Rect(surface.w, surface.h));
		area.clip(Common::Rect(texture.width(), texture.height()));
		if (area.isEmpty()) return;
		uint pitch = (area.width() + 255) & ~255U;
		NSUInteger offset;
		id<MTLBuffer> staging = _impl->reserveUploadData((NSUInteger)pitch * area.height(), offset);
		for (int y = 0; y < area.height(); ++y)
			memcpy((byte *)staging.contents + offset + y * pitch, surface.getBasePtr(area.left, area.top + y), area.width());
		_impl->endEncoder(); _impl->beginCommand();
		id<MTLBlitCommandEncoder> blit = [_impl->command blitCommandEncoder];
		[blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:(NSUInteger)pitch * area.height()
			sourceSize:MTLSizeMake(area.width(), area.height(), 1) toTexture:texture._impl->texture destinationSlice:0
			destinationLevel:0 destinationOrigin:MTLOriginMake(area.left, area.top, 0)];
		[blit endEncoding];
	}
}

Texture *Renderer::createCubeTexture(const Surface *faces, bool mipmaps) {
	@autoreleasepool {
		if (!faces || faces[0].w <= 0 || faces[0].w != faces[0].h || (uint)faces[0].w > maxTextureSize()) return nullptr;
		for (uint i = 1; i < 6; ++i)
			if (faces[i].w != faces[0].w || faces[i].h != faces[0].h) return nullptr;
		MTLTextureDescriptor *desc = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:faces[0].w mipmapped:mipmaps];
		desc.storageMode = MTLStorageModePrivate;
		desc.usage = MTLTextureUsageShaderRead;
		Texture::Impl *impl = new Texture::Impl;
		impl->texture = [_impl->device newTextureWithDescriptor:desc]; impl->mipmaps = mipmaps;
		if (!impl->texture) { delete impl; return nullptr; }
		Texture *texture = new Texture(impl);
		for (uint i = 0; i < 6; ++i)
			updateTextureLevel(*texture, 0, faces[i], Common::Rect(faces[i].w, faces[i].h), i);
		if (mipmaps) generateMipmaps(*texture);
		return texture;
	}
}

Texture *Renderer::createVolumeTexture(const void *rgba, uint bytes, uint w, uint h, uint d, bool mipmaps) {
	@autoreleasepool {
		if (!rgba || !w || !h || !d || w > 2048 || h > 2048 || d > 2048 || (uint64)w * h * d * 4 > bytes) return nullptr;
		MTLTextureDescriptor *desc = [[[MTLTextureDescriptor alloc] init] autorelease];
		desc.textureType = MTLTextureType3D;
		desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
		desc.width = w; desc.height = h; desc.depth = d;
		desc.storageMode = MTLStorageModePrivate; desc.usage = MTLTextureUsageShaderRead;
		uint levels = 1;
		if (mipmaps) for (uint size = MAX(MAX(w, h), d); size > 1; size >>= 1) ++levels;
		desc.mipmapLevelCount = levels;
		Texture::Impl *impl = new Texture::Impl;
		impl->texture = [_impl->device newTextureWithDescriptor:desc]; impl->mipmaps = mipmaps;
		if (!impl->texture) { delete impl; return nullptr; }
		uint pitch = (w * 4 + 255) & ~255U;
		NSUInteger imageBytes = (NSUInteger)pitch * h;
		NSUInteger offset;
		id<MTLBuffer> staging = _impl->reserveUploadData(imageBytes * d, offset);
		for (uint z = 0; z < d; ++z) for (uint y = 0; y < h; ++y)
			memcpy((byte *)staging.contents + offset + z * imageBytes + y * pitch, (const byte *)rgba + ((NSUInteger)z * h + y) * w * 4, w * 4);
		_impl->endEncoder(); _impl->beginCommand();
		id<MTLBlitCommandEncoder> blit = [_impl->command blitCommandEncoder];
		[blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:imageBytes
			sourceSize:MTLSizeMake(w, h, d) toTexture:impl->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		Texture *texture = new Texture(impl);
		if (mipmaps) generateMipmaps(*texture);
		return texture;
	}
}

OcclusionQuery *Renderer::createOcclusionQuery() {
	@autoreleasepool {
		OcclusionQuery::Impl *impl = new OcclusionQuery::Impl();
		impl->buffer = [_impl->device newBufferWithLength:512 * sizeof(uint64) options:MTLResourceStorageModeShared];
		if (!impl->buffer) { delete impl; return nullptr; }
		return new OcclusionQuery(impl);
	}
}

void Renderer::beginOcclusionQuery(OcclusionQuery &query) {
	@autoreleasepool {
		if (_impl->query) error("Metal: overlapping occlusion queries");
		if (query._impl->command) [query._impl->command waitUntilCompleted];
		[query._impl->command release]; query._impl->command = nil;
		memset(query._impl->buffer.contents, 0, query._impl->buffer.length);
		query._impl->passes = 0;
		_impl->endEncoder();
		_impl->query = query._impl;
	}
}

void Renderer::endOcclusionQuery(OcclusionQuery &query) {
	@autoreleasepool {
		if (_impl->query != query._impl) error("Metal: ending an inactive occlusion query");
		_impl->endEncoder();
		_impl->beginCommand();
		query._impl->command = [_impl->command retain];
		_impl->query = nullptr;
		_impl->submit(false);
	}
}

bool Renderer::occlusionQueryReady(const OcclusionQuery &query) const {
	return query._impl->command && query._impl->command.status >= MTLCommandBufferStatusCompleted;
}

uint64 Renderer::occlusionQueryResult(OcclusionQuery &query, bool wait) {
	if (_impl->query == query._impl) error("Metal: reading an active occlusion query");
	if (!query._impl->command) return 0;
	if (wait) [query._impl->command waitUntilCompleted];
	if (!occlusionQueryReady(query)) return 0;
	uint64 result = 0;
	const uint64 *counts = (const uint64 *)query._impl->buffer.contents;
	for (uint i = 0; i < query._impl->passes; ++i) result += counts[i];
	return result;
}

void Renderer::updateTexture(Texture &texture, const Surface &surface, const Common::Rect &rect) {
	updateTextureLevel(texture, 0, surface, rect);
	if (texture._impl->mipmaps)
		generateMipmaps(texture);
}

void Renderer::updateTextureLevel(Texture &texture, uint level, const Surface &surface, const Common::Rect &rect, uint slice) {
	@autoreleasepool {
		if (level >= texture._impl->texture.mipmapLevelCount)
			error("Metal: mipmap level out of range");
		uint slices = texture._impl->texture.textureType == MTLTextureTypeCube ? 6 : 1;
		if (slice >= slices || texture.depth() != 1)
			error("Metal: texture upload slice out of range");
		Common::Rect area = rect;
		area.clip(Common::Rect(surface.w, surface.h));
		area.clip(Common::Rect(MAX(1U, texture.width() >> level), MAX(1U, texture.height() >> level)));
		if (area.isEmpty())
			return;
		Surface source = surface.getSubArea(area);
		bool needsConversion = source.format != PixelFormat::createFormatRGBA32();
		Surface *converted = needsConversion ? source.convertTo(PixelFormat::createFormatRGBA32()) : nullptr;
		if (needsConversion && !converted)
			error("Metal: could not convert texture pixels");
		const Surface *rgba = converted ? converted : &source;
		uint pitch = (rgba->w * 4 + 255) & ~255U;
		NSUInteger offset;
		id<MTLBuffer> staging = _impl->reserveUploadData((NSUInteger)pitch * rgba->h, offset);
		for (int y = 0; y < rgba->h; ++y)
			memcpy((byte *)[staging contents] + offset + y * pitch, rgba->getBasePtr(0, y), rgba->w * 4);
		if (converted) {
			converted->free();
			delete converted;
		}
		_impl->endEncoder();
		_impl->beginCommand();
		id<MTLBlitCommandEncoder> blit = [_impl->command blitCommandEncoder];
		[blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:pitch * area.height()
			sourceSize:MTLSizeMake(area.width(), area.height(), 1) toTexture:texture._impl->texture destinationSlice:slice
			destinationLevel:level destinationOrigin:MTLOriginMake(area.left, area.top, 0)];
		[blit endEncoding];
	}
}

void Renderer::generateMipmaps(Texture &texture) {
	@autoreleasepool {
		if (texture.width() == 1 && texture.height() == 1 && texture.depth() == 1)
			return;
		_impl->resolveTextureOwner(texture._impl);
		_impl->beginCommand();
		id<MTLBlitCommandEncoder> blit = [_impl->command blitCommandEncoder];
		if (!texture._impl->mipmaps) {
			if (texture._impl->owner) error("Metal: cannot replace a render target with a mipmapped image");
			id<MTLTexture> old = texture._impl->texture;
			MTLTextureDescriptor *descriptor = [[[MTLTextureDescriptor alloc] init] autorelease];
			descriptor.textureType = old.textureType;
			descriptor.pixelFormat = old.pixelFormat;
			descriptor.width = old.width; descriptor.height = old.height; descriptor.depth = old.depth;
			descriptor.storageMode = MTLStorageModePrivate;
			descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
			uint extent = MAX<uint>(old.width, MAX<uint>(old.height, old.depth));
			descriptor.mipmapLevelCount = 1;
			while (extent > 1) { ++descriptor.mipmapLevelCount; extent >>= 1; }
			id<MTLTexture> replacement = [_impl->device newTextureWithDescriptor:descriptor];
			if (!replacement) error("Metal: could not allocate mipmapped image");
			uint slices = old.textureType == MTLTextureTypeCube ? 6 : 1;
			for (uint slice = 0; slice < slices; ++slice)
				[blit copyFromTexture:old sourceSlice:slice sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
					sourceSize:MTLSizeMake(old.width, old.height, old.depth) toTexture:replacement destinationSlice:slice
					destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
			texture._impl->texture = replacement;
			texture._impl->mipmaps = true;
			[old release];
		}
		[blit generateMipmapsForTexture:texture._impl->texture];
		[blit endEncoding];
	}
}

void Renderer::copyTexture(Texture &destination, const Common::Rect &destinationRect,
		Texture &source, const Common::Rect &sourceRect, bool flipY) {
	@autoreleasepool {
		if (destinationRect.isEmpty() || sourceRect.isEmpty() ||
			!Common::Rect(destination.width(), destination.height()).contains(destinationRect) ||
			!Common::Rect(source.width(), source.height()).contains(sourceRect))
			error("Metal: invalid texture copy rectangles");
		// Resolve pending rendering before sampling the source texture.
		if (_impl->target->color && (_impl->target->clearColorPending || _impl->target->clearDepthPending || _impl->target->clearStencilPending))
			_impl->beginEncoder();
		_impl->resolveTextureOwner(source._impl);
		_impl->beginCommand();
		if (source._impl->texture == destination._impl->texture) {
			// Stage overlapping or aliased copies entirely on the GPU.
			MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source._impl->texture.pixelFormat
				width:sourceRect.width() height:sourceRect.height() mipmapped:NO];
			descriptor.storageMode = MTLStorageModePrivate;
			descriptor.usage = MTLTextureUsageShaderRead;
			Texture::Impl *temporary = new Texture::Impl;
			temporary->texture = [_impl->device newTextureWithDescriptor:descriptor];
			temporary->mipmaps = false;
			if (!temporary->texture) { delete temporary; error("Metal: could not stage aliased copy"); }
			Texture snapshot(temporary);
			id<MTLBlitCommandEncoder> blit = [_impl->command blitCommandEncoder];
			[blit copyFromTexture:source._impl->texture sourceSlice:0 sourceLevel:0
				sourceOrigin:MTLOriginMake(sourceRect.left, sourceRect.top, 0)
				sourceSize:MTLSizeMake(sourceRect.width(), sourceRect.height(), 1)
				toTexture:temporary->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
			[blit endEncoding];
			copyTexture(destination, destinationRect, snapshot, Common::Rect(sourceRect.width(), sourceRect.height()), flipY);
			return;
		}
		// Copies into native targets must update their multisample storage too.
		// Otherwise a later load/resolve would overwrite the copied color.
		if (destination._impl->owner) {
			Target *previous = _impl->target;
			Common::Rect previousViewport = _impl->viewport;
			OcclusionQuery::Impl *previousQuery = _impl->query;
			_impl->query = nullptr;
			_impl->target = destination._impl->owner;
			_impl->viewport = destinationRect;
			float left = sourceRect.left / float(source.width()), right = sourceRect.right / float(source.width());
			float top = sourceRect.top / float(source.height()), bottom = sourceRect.bottom / float(source.height());
			if (flipY) SWAP(top, bottom);
			Vertex vertices[] = {{{-1,1,0},{left,top},{1,1,1,1}}, {{-1,-1,0},{left,bottom},{1,1,1,1}},
			                     {{1,1,0},{right,top},{1,1,1,1}}, {{1,-1,0},{right,bottom},{1,1,1,1}}};
			DrawState state;
			state.linear = false;
			state.primitive = kTriangleStrip;
			draw(vertices, 4, &source, state, Uniforms());
			_impl->endEncoder();
			_impl->target = previous;
			_impl->viewport = previousViewport;
			_impl->query = previousQuery;
			return;
		}
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = destination._impl->texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> encoder = [_impl->command renderCommandEncoderWithDescriptor:pass];
		if (!encoder)
			error("Metal: could not create texture copy pass");
		float left = sourceRect.left / (float)source.width(), right = sourceRect.right / (float)source.width();
		float top = sourceRect.top / (float)source.height(), bottom = sourceRect.bottom / (float)source.height();
		if (flipY) SWAP(top, bottom);
		Vertex vertices[] = {{{-1,1,0},{left,top},{1,1,1,1}}, {{-1,-1,0},{left,bottom},{1,1,1,1}},
		                     {{1,1,0},{right,top},{1,1,1,1}}, {{1,-1,0},{right,bottom},{1,1,1,1}}};
		Uniforms uniforms;
		uniforms.textured = 1;
		DrawState state;
		state.linear = false;
		[encoder setRenderPipelineState:_impl->pipeline(state, true, nullptr, destination._impl->texture.pixelFormat)];
		[encoder setViewport:MTLViewport{(double)destinationRect.left, (double)destinationRect.top,
			(double)destinationRect.width(), (double)destinationRect.height(), 0, 1}];
		[encoder setScissorRect:MTLScissorRect{(NSUInteger)destinationRect.left, (NSUInteger)destinationRect.top,
			(NSUInteger)destinationRect.width(), (NSUInteger)destinationRect.height()}];
		[encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
		[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
		[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];
		[encoder setFragmentTexture:source._impl->texture atIndex:0];
		[encoder setFragmentSamplerState:_impl->sampler(state, false) atIndex:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[encoder endEncoding];
		if (destination._impl->mipmaps)
			generateMipmaps(destination);
	}
}

void Renderer::clear(float r, float g, float b, float a, bool color, bool depth, bool stencil, float depthValue, uint32 stencilValue) {
	@autoreleasepool {
		if (!_impl->target->color)
			return;
		_impl->endEncoder();
		_impl->target->clearColor = MTLClearColorMake(r, g, b, a);
		_impl->target->clearDepth = CLIP<double>(depthValue, 0, 1);
		_impl->target->clearStencil = stencilValue;
		_impl->target->clearColorPending |= color;
		_impl->target->clearDepthPending |= depth;
		_impl->target->clearStencilPending |= stencil;
		_impl->beginEncoder();
	}
}

void Renderer::setViewport(const Common::Rect &rect) { _impl->viewport = rect; }
void Renderer::setMultisamplingActive(bool enabled) {
	if (_impl->target->multisamplingActive == enabled) return;
	_impl->endEncoder();
	_impl->target->multisamplingActive = enabled;
}
Common::Rect Renderer::viewport() const { return _impl->viewport; }

void Renderer::clearRect(const Common::Rect &rect, float r, float g, float b, float a, bool color, bool depth, bool stencil, uint32 colorMask, float depthValue, uint32 stencilValue) {
	Common::Rect area = rect;
	area.clip(Common::Rect(width(), height()));
	if (area.isEmpty())
		return;
	if (area == Common::Rect(width(), height()) && colorMask == 15) {
		clear(r, g, b, a, color, depth, stencil, depthValue, stencilValue);
		return;
	}
	Common::Rect viewport = _impl->viewport;
	setViewport(Common::Rect(width(), height()));
	float left = 2.f * area.left / width() - 1, right = 2.f * area.right / width() - 1;
	float top = 1 - 2.f * area.top / height(), bottom = 1 - 2.f * area.bottom / height();
	Vertex vertices[] = {
		{{left, top, 1}, {0, 0}, {r, g, b, a}}, {{left, bottom, 1}, {0, 0}, {r, g, b, a}},
		{{right, top, 1}, {0, 0}, {r, g, b, a}}, {{right, bottom, 1}, {0, 0}, {r, g, b, a}}
	};
	for (uint i = 0; i < ARRAYSIZE(vertices); ++i)
		vertices[i].position[2] = 2 * CLIP<float>(depthValue, 0, 1) - 1;
	DrawState state;
	state.primitive = kTriangleStrip;
	state.colorMask = color ? colorMask : 0;
	state.depthTest = state.depthWrite = depth;
	state.depthCompare = kAlways;
	state.stencil = stencil;
	state.stencilReference = stencilValue;
	state.frontStencil.pass = state.backStencil.pass = kReplace;
	// A scissored clear is implemented as a draw, but GL clears do not
	// contribute samples to an active visibility query.
	OcclusionQuery::Impl *previousQuery = _impl->query;
	if (previousQuery) { _impl->endEncoder(); _impl->query = nullptr; }
	draw(vertices, 4, nullptr, state, Uniforms());
	if (previousQuery) { _impl->endEncoder(); _impl->query = previousQuery; }
	setViewport(viewport);
}

static MTLPrimitiveType primitiveType(Primitive value) {
	static const MTLPrimitiveType types[] = {MTLPrimitiveTypeTriangle, MTLPrimitiveTypeTriangleStrip,
		MTLPrimitiveTypeLine, MTLPrimitiveTypeLineStrip, MTLPrimitiveTypePoint};
	return types[value];
}

Program *Renderer::createProgram(const char *source, const char *vertexFunction, const char *fragmentFunction,
		const VertexAttribute *attributes, uint attributeCount, uint vertexStride) {
	return createProgramStages(source, vertexFunction, source, fragmentFunction, attributes, attributeCount, vertexStride);
}

Program *Renderer::createProgramStages(const char *source, const char *vertexFunction,
		const char *fragmentSource, const char *fragmentFunction,
		const VertexAttribute *attributes, uint attributeCount, uint vertexStride) {
	@autoreleasepool {
		if (!_impl->device || !source || !vertexFunction || !fragmentSource || !fragmentFunction)
			return nullptr;
		Program::Impl *program = new Program::Impl();
		program->pipelineCache = [_impl->pipelines retain];
		program->pipelineKeys = [[NSMutableArray alloc] init];
		if (attributeCount) {
			if (!attributes || !vertexStride || attributeCount > 31) { delete program; return nullptr; }
			program->vertexDescriptor = [[MTLVertexDescriptor alloc] init];
			program->vertexDescriptor.layouts[0].stride = vertexStride;
			program->vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
			const MTLVertexFormat formats[] = {MTLVertexFormatInvalid, MTLVertexFormatFloat,
				MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4};
			for (uint i = 0; i < attributeCount; ++i) {
				const VertexAttribute &attribute = attributes[i];
				if (attribute.index >= 31 || !attribute.components || attribute.components > 4 ||
				    attribute.offset > vertexStride || attribute.components * 4 > vertexStride - attribute.offset) {
					delete program; return nullptr;
				}
				MTLVertexAttributeDescriptor *descriptor = program->vertexDescriptor.attributes[attribute.index];
				descriptor.format = formats[attribute.components];
				descriptor.offset = attribute.offset;
				descriptor.bufferIndex = 0;
			}
		}
		MTLCompileOptions *options = [[[MTLCompileOptions alloc] init] autorelease];
		options.languageVersion = MTLLanguageVersion3_0;
		NSError *failure = nil;
		program->library = [_impl->device newLibraryWithSource:[NSString stringWithUTF8String:source] options:options error:&failure];
		if (program->library) {
			program->vertex = [program->library newFunctionWithName:[NSString stringWithUTF8String:vertexFunction]];
			if (source == fragmentSource)
				program->fragmentLibrary = [program->library retain];
			else
				program->fragmentLibrary = [_impl->device newLibraryWithSource:[NSString stringWithUTF8String:fragmentSource] options:options error:&failure];
			if (program->fragmentLibrary)
				program->fragment = [program->fragmentLibrary newFunctionWithName:[NSString stringWithUTF8String:fragmentFunction]];
		}
		if (!program->vertex || !program->fragment) {
			warning("Metal program '%s/%s': %s", vertexFunction, fragmentFunction,
				failure ? [[failure localizedDescription] UTF8String] : "entry point unavailable");
			delete program;
			return nullptr;
		}
		return new Program(program);
	}
}

bool Renderer::validateProgram(Program &program, TargetFormat format, uint samples) {
	@autoreleasepool {
		if (!_impl->device || !supportsSamples(samples)) return false;
		MTLRenderPipelineDescriptor *descriptor = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
		descriptor.vertexFunction = program._impl->vertex;
		descriptor.fragmentFunction = program._impl->fragment;
		descriptor.vertexDescriptor = program._impl->vertexDescriptor;
		descriptor.rasterSampleCount = samples;
		descriptor.colorAttachments[0].pixelFormat = format == kTargetRGBA16Float ? MTLPixelFormatRGBA16Float :
			format == kTargetSRGBA8 ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
		descriptor.depthAttachmentPixelFormat = descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
		NSError *failure = nil;
		id<MTLRenderPipelineState> pipeline = [_impl->device newRenderPipelineStateWithDescriptor:descriptor error:&failure];
		if (!pipeline) warning("Metal program interface: %s", failure ? [[failure localizedDescription] UTF8String] : "pipeline unavailable");
		bool success = pipeline != nil;
		[pipeline release];
		return success;
	}
}

void Renderer::drawProgram(Program &program, const void *vertices, uint count, uint stride,
		const uint32 *indices, uint indexCount, const void *uniforms, uint uniformSize,
		const TextureBinding *textures, uint textureCount, const DrawState &state,
		const void *fragmentUniforms, uint fragmentUniformSize) {
	@autoreleasepool {
		if (!vertices || !count || !stride || !_impl->target->color || _impl->viewport.isEmpty() ||
			(state.scissor && !state.scissorRect.intersects(Common::Rect(width(), height()))))
			return;
		if ((indexCount && !indices) || (uniformSize && !uniforms) || (fragmentUniformSize && !fragmentUniforms) || (textureCount && !textures) || textureCount > 16)
			error("Metal: invalid native program draw");
		for (uint i = 0; i < indexCount; ++i)
			if (indices[i] >= count)
				error("Metal: native program index out of range");
		// With explicit bindings, slot zero is set below before the draw. Keep
		// the default white texture only for programs with no supplied textures.
		_impl->prepareDraw(nullptr, state, Uniforms(), program._impl, textureCount == 0);
		NSUInteger vertexOffset = 0, uniformOffset = 0, indexOffset = 0, fragmentOffset = 0;
		id<MTLBuffer> vertexBuffer = _impl->uploadDrawData(vertices, (NSUInteger)count * stride, vertexOffset);
		id<MTLBuffer> uniformBuffer = _impl->uploadDrawData(uniforms, uniformSize, uniformOffset);
		id<MTLBuffer> fragmentBuffer = fragmentUniformSize ? _impl->uploadDrawData(fragmentUniforms, fragmentUniformSize, fragmentOffset) : uniformBuffer;
		if (!fragmentUniformSize) fragmentOffset = uniformOffset;
		id<MTLBuffer> indexBuffer = _impl->uploadDrawData(indices, (NSUInteger)indexCount * sizeof(uint32), indexOffset);
		if (!vertexBuffer || (uniformSize && !uniformBuffer) || (fragmentUniformSize && !fragmentBuffer) || (indexCount && !indexBuffer))
			error("Metal: could not allocate native program draw buffers");
		[_impl->encoder setVertexBuffer:vertexBuffer offset:vertexOffset atIndex:0];
		[_impl->encoder setVertexBuffer:uniformBuffer offset:uniformOffset atIndex:1];
		[_impl->encoder setFragmentBuffer:fragmentBuffer offset:fragmentOffset atIndex:1];
		for (uint i = 0; i < textureCount; ++i) {
			Texture::Impl *texture = textures[i].texture ? textures[i].texture->_impl : nullptr;
			DrawState sampling;
			sampling.linear = textures[i].linear;
			sampling.mipLinear = textures[i].mipLinear;
			sampling.mipmaps = textures[i].mipmaps;
			sampling.wrapS = textures[i].wrapS;
			sampling.wrapT = textures[i].wrapT;
			sampling.wrapR = textures[i].wrapR;
			sampling.anisotropy = textures[i].anisotropy;
			id<MTLTexture> image = texture ? texture->texture : _impl->white;
			id<MTLSamplerState> sampler = _impl->sampler(sampling, texture && texture->mipmaps && sampling.mipmaps);
			[_impl->encoder setFragmentTexture:image atIndex:i];
			[_impl->encoder setFragmentSamplerState:sampler atIndex:i];
			[_impl->encoder setVertexTexture:image atIndex:i];
			[_impl->encoder setVertexSamplerState:sampler atIndex:i];
		}
		if (indexCount)
			[_impl->encoder drawIndexedPrimitives:primitiveType(state.primitive) indexCount:indexCount indexType:MTLIndexTypeUInt32 indexBuffer:indexBuffer indexBufferOffset:indexOffset];
		else
			[_impl->encoder drawPrimitives:primitiveType(state.primitive) vertexStart:0 vertexCount:count];
	}
}

void Renderer::draw(const Vertex *vertices, uint count, Texture *texture, const DrawState &state, const Uniforms &uniforms) {
	@autoreleasepool {
		if (!vertices || !count || !_impl->target->color || _impl->viewport.isEmpty() ||
			(state.scissor && !state.scissorRect.intersects(Common::Rect(width(), height()))))
			return;
		_impl->prepareDraw(texture ? texture->_impl : nullptr, state, uniforms);
		NSUInteger offset;
		id<MTLBuffer> buffer = _impl->uploadDrawData(vertices, sizeof(Vertex) * (NSUInteger)count, offset);
		if (!buffer)
			error("Metal: could not allocate vertices");
		[_impl->encoder setVertexBuffer:buffer offset:offset atIndex:0];
		[_impl->encoder drawPrimitives:primitiveType(state.primitive) vertexStart:0 vertexCount:count];
	}
}

void Renderer::drawIndexed(const Vertex *vertices, uint count, const uint32 *indices, uint indexCount,
		Texture *texture, const DrawState &state, const Uniforms &uniforms) {
	@autoreleasepool {
		if (!vertices || !indices || !count || !indexCount || !_impl->target->color || _impl->viewport.isEmpty() ||
			(state.scissor && !state.scissorRect.intersects(Common::Rect(width(), height()))))
			return;
		for (uint i = 0; i < indexCount; ++i)
			if (indices[i] >= count)
				error("Metal: vertex index out of range");
		_impl->prepareDraw(texture ? texture->_impl : nullptr, state, uniforms);
		NSUInteger vertexOffset, indexOffset;
		id<MTLBuffer> vertexBuffer = _impl->uploadDrawData(vertices, sizeof(Vertex) * (NSUInteger)count, vertexOffset);
		id<MTLBuffer> indexBuffer = _impl->uploadDrawData(indices, sizeof(uint32) * (NSUInteger)indexCount, indexOffset);
		if (!vertexBuffer || !indexBuffer)
			error("Metal: could not allocate indexed vertices");
		[_impl->encoder setVertexBuffer:vertexBuffer offset:vertexOffset atIndex:0];
		[_impl->encoder drawIndexedPrimitives:primitiveType(state.primitive) indexCount:indexCount indexType:MTLIndexTypeUInt32 indexBuffer:indexBuffer indexBufferOffset:indexOffset];
	}
}

void Renderer::drawWideLine(const Vertex &start, const Vertex &end, float lineWidth, const DrawState &state, const Uniforms &uniforms) {
	if (lineWidth <= 0 || _impl->viewport.isEmpty())
		return;
	float clip[2][4];
	float colors[2][4];
	float fogDistances[2];
	const Vertex *points[] = {&start, &end};
	for (uint i = 0; i < 2; ++i) {
		float eyeZ = uniforms.modelView[14];
		for (uint c = 0; c < 3; ++c) eyeZ += uniforms.modelView[2 + c * 4] * points[i]->position[c];
		fogDistances[i] = fabsf(eyeZ);
		for (uint row = 0; row < 4; ++row) {
			clip[i][row] = uniforms.transform[row + 12];
			for (uint col = 0; col < 3; ++col)
				clip[i][row] += uniforms.transform[row + col * 4] * points[i]->position[col];
			colors[i][row] = points[i]->color[row];
		}
	}
	for (uint plane = 0; plane < (state.depthClamp ? 4U : 6U); ++plane) {
		uint axis = plane / 2;
		float sign = plane & 1 ? -1 : 1;
		float distances[] = {clip[0][3] + sign * clip[0][axis], clip[1][3] + sign * clip[1][axis]};
		if (distances[0] < 0 && distances[1] < 0)
			return;
		if (distances[0] < 0 || distances[1] < 0) {
			float t = distances[0] / (distances[0] - distances[1]);
			uint outside = distances[0] < 0 ? 0 : 1;
			fogDistances[outside] = fogDistances[0] + t * (fogDistances[1] - fogDistances[0]);
			for (uint c = 0; c < 4; ++c) {
				clip[outside][c] = clip[0][c] + t * (clip[1][c] - clip[0][c]);
				colors[outside][c] = colors[0][c] + t * (colors[1][c] - colors[0][c]);
			}
		}
	}
	if (clip[0][3] <= 0 || clip[1][3] <= 0)
		return;
	float ndc[2][3];
	for (uint i = 0; i < 2; ++i)
		for (uint c = 0; c < 3; ++c)
			ndc[i][c] = clip[i][c] / clip[i][3];
	float dx = (ndc[1][0] - ndc[0][0]) * _impl->viewport.width();
	float dy = (ndc[1][1] - ndc[0][1]) * _impl->viewport.height();
	float length = sqrtf(dx * dx + dy * dy);
	if (length <= 0)
		return;
	float offsetX = dy * lineWidth / (length * _impl->viewport.width());
	float offsetY = -dx * lineWidth / (length * _impl->viewport.height());
	Vertex vertices[4] = {};
	for (uint i = 0; i < 4; ++i) {
		uint endpoint = i / 2;
		float sign = i & 1 ? -1 : 1;
		vertices[i].position[0] = ndc[endpoint][0] + sign * offsetX;
		vertices[i].position[1] = ndc[endpoint][1] + sign * offsetY;
		vertices[i].position[2] = ndc[endpoint][2];
		vertices[i].texcoord[0] = fogDistances[endpoint];
		vertices[i].texcoord[1] = clip[endpoint][3];
		memcpy(vertices[i].color, colors[endpoint], sizeof(vertices[i].color));
	}
	Uniforms actual = uniforms;
	memset(actual.transform, 0, sizeof(actual.transform));
	actual.transform[0] = actual.transform[5] = actual.transform[10] = actual.transform[15] = 1;
	actual.textureMode = 4;
	DrawState lineState = state;
	lineState.primitive = kTriangleStrip;
	lineState.cull = false;
	draw(vertices, 4, nullptr, lineState, actual);
}

bool Renderer::present(RenderTarget *source) {
	@autoreleasepool {
		setRenderTarget(source);
		if (!_impl->target->color)
			return false;
		_impl->beginEncoder();
		_impl->endEncoder();
		if (!_impl->layer) {
			_impl->submit(false);
			setRenderTarget(nullptr);
			return true;
		}
		id<CAMetalDrawable> drawable = [_impl->layer nextDrawable];
		if (!drawable) {
			_impl->submit(false);
			setRenderTarget(nullptr);
			return false;
		}
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = drawable.texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> encoder = [_impl->command renderCommandEncoderWithDescriptor:pass];
		DrawState state;
		Uniforms uniforms;
		uniforms.textured = 1;
		Vertex vertices[] = {
			{{-1, 1, 0}, {0, 0}, {1, 1, 1, 1}}, {{-1, -1, 0}, {0, 1}, {1, 1, 1, 1}},
			{{1, 1, 0}, {1, 0}, {1, 1, 1, 1}}, {{1, -1, 0}, {1, 1}, {1, 1, 1, 1}}
		};
		[encoder setRenderPipelineState:_impl->pipeline(state, true)];
		[encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
		[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
		[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];
		[encoder setFragmentTexture:_impl->target->color atIndex:0];
		[encoder setFragmentSamplerState:_impl->sampler(state, false) atIndex:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[encoder endEncoding];
		[_impl->command presentDrawable:drawable];
		_impl->submit(false);
		setRenderTarget(nullptr);
		return true;
	}
}

void Renderer::flush(bool wait) {
	@autoreleasepool {
		_impl->submit(wait);
		if (wait && _impl->queue) {
			id<MTLCommandBuffer> fence = [_impl->queue commandBuffer];
			[fence commit];
			[fence waitUntilCompleted];
		}
	}
}

bool Renderer::readPixels(Surface &surface, const Common::Rect &rect) {
	if (!_impl->target->color)
		return false;
	Texture::Impl *view = new Texture::Impl();
	view->texture = [_impl->target->color retain];
	view->mipmaps = false;
	Texture texture(view);
	return readTexture(texture, surface, rect);
}

bool Renderer::readTexture(Texture &texture, Surface &surface, const Common::Rect &rect) {
	@autoreleasepool {
		if (rect.isEmpty() || !Common::Rect(texture.width(), texture.height()).contains(rect))
			return false;
		if (texture._impl->texture.pixelFormat == MTLPixelFormatRGBA16Float) {
			RenderTarget *previous = renderTarget();
			Common::Rect previousViewport = viewport();
			RenderTarget *converted = createRenderTarget(rect.width(), rect.height());
			if (!converted) return false;
			copyTexture(converted->texture(), Common::Rect(rect.width(), rect.height()), texture, rect);
			bool success = readTexture(converted->texture(), surface, Common::Rect(rect.width(), rect.height()));
			setRenderTarget(previous); setViewport(previousViewport);
			delete converted;
			return success;
		}
		if (_impl->target->color)
			_impl->beginEncoder();
		_impl->resolveTextureOwner(texture._impl);
		_impl->beginCommand();
		uint pitch = (rect.width() * 4 + 255) & ~255U;
		id<MTLBuffer> staging = [_impl->device newBufferWithLength:pitch * rect.height() options:MTLResourceStorageModeShared];
		if (!staging)
			return false;
		id<MTLBlitCommandEncoder> blit = [_impl->command blitCommandEncoder];
		[blit copyFromTexture:texture._impl->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(rect.left, rect.top, 0)
			sourceSize:MTLSizeMake(rect.width(), rect.height(), 1) toBuffer:staging destinationOffset:0
			destinationBytesPerRow:pitch destinationBytesPerImage:pitch * rect.height()];
		[blit endEncoding];
		_impl->submit(true);
		surface.free();
		surface.create(rect.width(), rect.height(), PixelFormat::createFormatRGBA32());
		for (int y = 0; y < rect.height(); ++y) {
			const byte *src = (const byte *)[staging contents] + y * pitch;
			byte *dst = (byte *)surface.getBasePtr(0, y);
			if (texture._impl->texture.pixelFormat == MTLPixelFormatRGBA8Unorm ||
			    texture._impl->texture.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB) {
				memcpy(dst, src, rect.width() * 4);
				continue;
			}
			if (texture._impl->texture.pixelFormat == MTLPixelFormatR8Unorm) {
				for (int x = 0; x < rect.width(); ++x) {
					dst[x * 4] = dst[x * 4 + 1] = dst[x * 4 + 2] = src[x];
					dst[x * 4 + 3] = 255;
				}
				continue;
			}
			for (int x = 0; x < rect.width(); ++x) {
				dst[x * 4] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1];
				dst[x * 4 + 2] = src[x * 4]; dst[x * 4 + 3] = src[x * 4 + 3];
			}
		}
		[staging release];
		return true;
	}
}

} // End of namespace Metal
} // End of namespace Graphics
