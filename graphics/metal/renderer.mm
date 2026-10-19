/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "graphics/metal/renderer.h"
#include "graphics/metal/shaders.h"
#include "common/textconsole.h"
#include "common/hashmap.h"

#include <memory>
#include <vector>

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>

#include "graphics/metal/pipeline-cache.h"

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
	if (@available(macOS 26.0, *)) {
		if (!device || ![device supportsFamily:MTLGPUFamilyMetal4] || ![device supportsFamily:MTLGPUFamilyApple7])
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
	dispatch_group_t completion = nullptr;
	uint passes = 0;
	~Impl() { [buffer release]; if (completion) dispatch_release(completion); }
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
	bool depthRequested = true;
	bool clearColorPending = true, clearDepthPending = true, clearStencilPending = true;

	~Target() {
		[color release];
		[multisample release];
		[depth release];
	}
};

struct RenderTarget::Impl : Target {};

struct Buffer::Impl {
	id<MTLBuffer> buffer = nil;
	NSUInteger offset = 0;
	uint length = 0;
	// An arena page cannot return to the pool while a public slice exists.
	std::shared_ptr<byte> lease;
	~Impl() { [buffer release]; }
};

Buffer::~Buffer() { delete _impl; }
uint Buffer::size() const { return _impl->length; }

struct Program::Impl {
	id<MTLLibrary> library = nil;
	id<MTLLibrary> fragmentLibrary = nil;
	MTL4LibraryFunctionDescriptor *vertex = nil, *fragment = nil;
	MTLVertexDescriptor *vertexDescriptor = nil;
	NSMutableDictionary *pipelineCache = nil;
	NSMutableArray *pipelineKeys = nil;
	NSData *cacheIdentity = nil;
	uint vertexTextureMask = 0xffff, fragmentTextureMask = 0xffff;
	uint vertexBufferMask = 0xf, fragmentBufferMask = 0xf;
	bool reflectionKnown = false, reflectionFallback = false;
	~Impl() {
		[cacheIdentity release];
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
		TargetFormat format = kTargetBGRA8, bool mipmaps = false, bool depthStencil = true) {
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
	id<MTLTexture> depth = depthStencil ? [device newTextureWithDescriptor:desc] : nil;
	if (!color || (depthStencil && !depth) || (samples > 1 && !msaa)) {
		[color release]; [msaa release]; [depth release];
		return false;
	}
	[target.color release]; target.color = color;
	[target.multisample release]; target.multisample = msaa;
	[target.depth release]; target.depth = depth;
	target.w = w; target.h = h; target.samples = samples;
	target.depthRequested = depthStencil;
	target.viewport = Common::Rect(w, h);
	target.clearColorPending = target.clearDepthPending = target.clearStencilPending = true;
	return true;
}

static bool allocateDepth(id<MTLDevice> device, Target &target) {
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
		width:target.w height:target.h mipmapped:NO];
	desc.storageMode = MTLStorageModePrivate; desc.usage = MTLTextureUsageRenderTarget;
	if (target.samples > 1) { desc.textureType = MTLTextureType2DMultisample; desc.sampleCount = target.samples; }
	target.depth = [device newTextureWithDescriptor:desc];
	target.clearDepthPending = target.clearStencilPending = true;
	return target.depth != nil;
}

// Pipeline entry descriptors retain their source libraries. Shader mathematics and
// engine-facing layouts are shared with the original Metal implementation.
static MTL4LibraryFunctionDescriptor *functionDescriptor(id<MTLLibrary> library, NSString *name) {
	if (!library || ![library.functionNames containsObject:name]) return nil;
	MTL4LibraryFunctionDescriptor *function = [[MTL4LibraryFunctionDescriptor alloc] init];
	function.library = library; function.name = name;
	return function;
}

static id<MTLLibrary> compileLibrary(id<MTL4Compiler> compiler, const char *source, NSError **failure) {
	MTL4LibraryDescriptor *descriptor = [[[MTL4LibraryDescriptor alloc] init] autorelease];
	descriptor.source = [NSString stringWithUTF8String:source];
	MTLCompileOptions *options = [[[MTLCompileOptions alloc] init] autorelease];
	options.languageVersion = MTLLanguageVersion4_0;
	descriptor.options = options;
	return [compiler newLibraryWithDescriptor:descriptor error:failure];
}

static MTLPrimitiveTopologyClass topology(Primitive primitive) {
	return primitive == kPoints ? MTLPrimitiveTopologyClassPoint :
		primitive == kLines || primitive == kLineStrip ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassTriangle;
}

static uint reflectedTextures(NSArray<id<MTLBinding>> *bindings) {
	uint mask = 0;
	for (id<MTLBinding> binding in bindings) {
		if (!binding.isUsed || binding.type != MTLBindingTypeTexture) continue;
		NSUInteger count = MAX((NSUInteger)1, ((id<MTLTextureBinding>)binding).arrayLength);
		if (binding.index >= 16 || count > 16 - binding.index) error("Metal: shader texture binding exceeds the native interface");
		for (NSUInteger i = 0; i < count; ++i) mask |= 1U << (binding.index + i);
	}
	return mask;
}

static uint reflectedBuffers(NSArray<id<MTLBinding>> *bindings) {
	uint mask = 0;
	for (id<MTLBinding> binding in bindings) {
		if (!binding.isUsed || binding.type != MTLBindingTypeBuffer) continue;
		if (binding.index >= 4) error("Metal: shader buffer binding exceeds the native interface");
		mask |= 1U << binding.index;
	}
	return mask;
}

struct ArgumentBindings {
	uint64 buffers[4] = {}, textures[16] = {}, samplers[16] = {};
};

struct ArgumentTable {
	id<MTL4ArgumentTable> value = nil;
	ArgumentBindings bindings;
	~ArgumentTable() { [value release]; }
};

// Each submission owns its resources until feedback. Containers/residency sets
// can be reused afterwards; query completion groups are fresh for every epoch.
struct Submission {
	id<MTL4CommandBuffer> command = nil;
	id<MTL4CommandAllocator> allocator = nil;
	id<MTLResidencySet> residency = nil;
	NSMutableSet *resources = [[NSMutableSet alloc] init];
	Common::Array<ArgumentTable *> tables;
	Common::HashMap<uint64, Common::Array<ArgumentTable *> > bindingCache;
	dispatch_group_t completion = nullptr;
	void restart() {
		if (completion) dispatch_release(completion);
		completion = dispatch_group_create(); dispatch_group_enter(completion);
	}
	~Submission() {
		for (ArgumentTable *table : tables) delete table;
		[command release]; [allocator release]; [residency release];
		[resources release]; if (completion) dispatch_release(completion);
	}
};

struct Renderer::Impl {
	id<MTLDevice> device = nil;
	id<MTL4CommandQueue> queue = nil;
	id<MTL4Compiler> compiler = nil;
	PipelineArchiveCache archiveCache;
	NSData *builtinCacheIdentity = nil;
	id<MTLEvent> dependencyEvent = nil;
	uint64 dependencyValue = 0;
	bool queueSyncPending = false;
	NSMutableSet *pendingEvents = [[NSMutableSet alloc] init];
	id<MTLLibrary> library = nil;
	MTL4LibraryFunctionDescriptor *vertex = nil, *fragment = nil, *logicFragment = nil;
	CAMetalLayer *layer = nil;
	Target screen;
	Target *target = &screen;
	RenderTarget *renderTarget = nullptr;
	Texture *screenTexture = nullptr;
	id<MTLTexture> white = nil;
	id<MTL4FXSpatialScaler> metalFX = nil;
	id<MTLTexture> metalFXInput = nil;
	id<MTLFence> metalFXFence = nil;
	Texture *metalFXOutput = nullptr;
	uint metalFXWidth = 0, metalFXHeight = 0, metalFXOutputWidth = 0, metalFXOutputHeight = 0;
	MTLPixelFormat metalFXFormat = MTLPixelFormatInvalid;
	bool metalFXSupported = false;
	bool metalFXCopyFailed = false;
	id<MTL4CommandBuffer> command = nil;
	Submission *submission = nullptr;
	ArgumentBindings vertexArguments, fragmentArguments;
	Common::Array<Submission *> freeSubmissions;
	Common::Array<ArgumentTable *> freeTables;
	dispatch_group_t pending = dispatch_group_create();
	id<MTL4RenderCommandEncoder> encoder = nil;
	OcclusionQuery::Impl *query = nullptr;
	NSMutableDictionary *pipelines = nil, *depthStates = nil, *samplers = nil;
	NSCache *libraries = nil;
	NSData *lastPipelineKey = nil, *lastDepthKey = nil;
	NSMutableArray *freeDrawBuffers = nil, *drawBuffers = nil;
	NSLock *drawBufferLock = nil;
	id<MTLBuffer> drawBuffer = nil;
	NSUInteger drawBufferOffset = 0;
	std::shared_ptr<byte> drawBufferLease;
	std::vector<std::shared_ptr<byte> > drawBufferLeases;
	dispatch_semaphore_t inFlight;
	Common::Rect viewport;
	Statistics counters;
	bool boundStateValid = false;
	DrawState boundState;
	Common::Rect boundViewport, boundScissor;
	struct Access { MTLStages reads = 0, writes = 0; };
	Common::HashMap<uintptr, Access> history, passAccess;
	id<MTL4CommandEncoder> activePass = nil;
	MTLStages waitedStages[3] = {};
	MTLStages boundaryWaitedStages = 0;

	id<MTLRenderPipelineState> boundPipeline = nil;
	id<MTLDepthStencilState> boundDepth = nil;


	Impl() : inFlight(dispatch_semaphore_create(3)) {
		freeDrawBuffers = [[NSMutableArray alloc] init];

		drawBufferLock = [[NSLock alloc] init];
	}
	~Impl() {
		[pendingEvents release];
		[drawBuffers release];
		[freeDrawBuffers release];
		[drawBufferLock release];
		for (Submission *batch : freeSubmissions) delete batch;
		for (ArgumentTable *table : freeTables) delete table;
		dispatch_release(pending);
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
			if (!drawBuffer) {
				drawBuffer = [device newBufferWithLength:pageLength options:MTLResourceStorageModeShared];
				++counters.uploadPages;
			} else ++counters.uploadPageReuses;
			if (!drawBuffer) error("Metal: could not allocate upload arena");
			if (!drawBuffers) drawBuffers = [[NSMutableArray alloc] init];
			[drawBuffers addObject:drawBuffer];
			drawBufferLease = std::make_shared<byte>(0);
			drawBufferLeases.push_back(drawBufferLease);
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
		counters.uploadedBytes += length;
		return buffer;
	}

	void beginCommand() {
		if (command) return;
		dispatch_semaphore_wait(inFlight, DISPATCH_TIME_FOREVER);
		[drawBufferLock lock];
		if (!freeSubmissions.empty()) { submission = freeSubmissions.back(); freeSubmissions.pop_back(); }
		else submission = nullptr;
		[drawBufferLock unlock];
		if (!submission) {
			submission = new Submission(); ++counters.submissionObjects;
			submission->command = [device newCommandBuffer]; submission->allocator = [device newCommandAllocator];
			MTLResidencySetDescriptor *descriptor = [[[MTLResidencySetDescriptor alloc] init] autorelease]; descriptor.initialCapacity = 128;
			submission->residency = [device newResidencySetWithDescriptor:descriptor error:nil]; ++counters.residencySets;
		}
		if (!submission->command || !submission->allocator || !submission->residency) error("Metal 4: could not allocate command submission");
		submission->restart();
		command = submission->command;
		[submission->resources unionSet:pendingEvents]; [pendingEvents removeAllObjects];
		queueSyncPending = false; resetHazards();
		[submission->allocator reset];
		[command beginCommandBufferWithAllocator:submission->allocator];
	}

	void keep(id object) {
		if (object) { beginCommand(); [submission->resources addObject:object]; }
	}

	void useResource(id<MTLResource> resource) {
		if (!resource) return;
		beginCommand();
		if (![submission->resources containsObject:resource]) {
			[submission->resources addObject:resource];
			[submission->residency addAllocation:resource];
		}
	}

	void resetHazards() {
		history.clear(); passAccess.clear(); activePass = nil; boundaryWaitedStages = 0;
		for (MTLStages &stages : waitedStages) stages = 0;
	}

	void orderEncoder(id<MTL4CommandEncoder> pass, MTLStages stages = MTLStageVertex | MTLStageFragment) {
		if (!pass || activePass) error("Metal: invalid encoder dependency boundary");
		activePass = pass; passAccess.clear();
		for (MTLStages &waited : waitedStages) waited = 0;
		// Keep a conservative boundary between command buffers and around
		// opaque MetalFX work. Within a command buffer, only actual resource
		// hazards insert producer/consumer-stage barriers.
		MTLStages boundaryStages = stages & ~boundaryWaitedStages;
		if (boundaryStages) {
			// Waiting for vertex/fragment does not order an independent later
			// blit. Every consumer stage must cross the command-buffer boundary
			// before reading resources produced by an earlier submission.
			[pass barrierAfterQueueStages:MTLStageAll beforeStages:boundaryStages visibilityOptions:MTL4VisibilityOptionDevice];
			++counters.queueBarriers; boundaryWaitedStages |= boundaryStages;
			if (boundaryStages & MTLStageVertex) waitedStages[0] = MTLStageAll;
			if (boundaryStages & MTLStageFragment) waitedStages[1] = MTLStageAll;
			if (boundaryStages & MTLStageBlit) waitedStages[2] = MTLStageAll;
		}
	}

	void accessResource(id<MTLResource> resource, MTLStages stages, bool write = false) {
		if (!resource) return;
		useResource(resource);
		if (!activePass) error("Metal: resource access without an encoder");
		uintptr key = (uintptr)resource;
		const Access &previous = history[key];
		MTLStages dependencies = previous.writes | (write ? previous.reads : 0);
		const MTLStages consumers[] = {MTLStageVertex, MTLStageFragment, MTLStageBlit};
		MTLStages after = 0;
		for (uint i = 0; i < 3; ++i) if (stages & consumers[i]) after |= dependencies & ~waitedStages[i];
		if (after) {
			[activePass barrierAfterQueueStages:after beforeStages:stages visibilityOptions:MTL4VisibilityOptionDevice];
			++counters.queueBarriers;
			for (uint i = 0; i < 3; ++i) if (stages & consumers[i]) waitedStages[i] |= after;
		}
		Access &current = passAccess[key];
		if (write) current.writes |= stages; else current.reads |= stages;
	}

	void finishPass(id<MTL4CommandEncoder> pass) {
		[pass endEncoding];
		if (pass != activePass) return;
		for (const auto &entry : passAccess) {
			Access &previous = history[entry._key];
			if (entry._value.writes) previous = entry._value;
			else previous.reads |= entry._value.reads;
		}
		passAccess.clear(); activePass = nil;
	}

	id<MTL4ComputeCommandEncoder> beginCopy(id<MTLResource> source, id<MTLResource> destination = nil) {
		endEncoder(); beginCommand();
		id<MTL4ComputeCommandEncoder> blit = [command computeCommandEncoder];
		if (!blit) error("Metal 4: could not begin copy pass");
		orderEncoder(blit, MTLStageBlit);
		accessResource(source, MTLStageBlit, destination == nil);
		accessResource(destination, MTLStageBlit, true);
		return blit;
	}

	ArgumentTable *argumentTable(const ArgumentBindings &bindings) {
		beginCommand();
		uint64 hash = 14695981039346656037ULL;
		const byte *bytes = reinterpret_cast<const byte *>(&bindings);
		for (uint i = 0; i < sizeof(bindings); ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
		auto found = submission->bindingCache.find(hash);
		if (found != submission->bindingCache.end()) {
			for (ArgumentTable *table : found->_value) {
				if (!memcmp(&table->bindings, &bindings, sizeof(bindings))) {
					++counters.argumentTableReuses;
					return table;
				}
			}
		}
		ArgumentTable *table = nullptr;
		[drawBufferLock lock];
		if (!freeTables.empty()) { table = freeTables.back(); freeTables.pop_back(); }
		[drawBufferLock unlock];
		if (!table) {
			table = new ArgumentTable();
			MTL4ArgumentTableDescriptor *desc = [[[MTL4ArgumentTableDescriptor alloc] init] autorelease];
			desc.maxBufferBindCount = 4; desc.maxTextureBindCount = 16; desc.maxSamplerStateBindCount = 16;
			desc.initializeBindings = YES;
			table->value = [device newArgumentTableWithDescriptor:desc error:nil]; ++counters.argumentTables;
		}
		if (!table->value) error("Metal 4: could not allocate argument table");
		// Only completed tables reach the pool. Overwrite changed slots directly;
		// absent slots are zeroed, so sparse programs cannot inherit old bindings.
		for (uint i = 0; i < 4; ++i) if (table->bindings.buffers[i] != bindings.buffers[i]) {
			[table->value setAddress:bindings.buffers[i] atIndex:i];
			if (bindings.buffers[i]) ++counters.argumentTableWrites; else ++counters.argumentTableClears;
		}
		for (uint i = 0; i < 16; ++i) {
			if (table->bindings.textures[i] != bindings.textures[i]) {
				[table->value setTexture:MTLResourceID{bindings.textures[i]} atIndex:i];
				if (bindings.textures[i]) ++counters.argumentTableWrites; else ++counters.argumentTableClears;
			}
			if (table->bindings.samplers[i] != bindings.samplers[i]) {
				[table->value setSamplerState:MTLResourceID{bindings.samplers[i]} atIndex:i];
				if (bindings.samplers[i]) ++counters.argumentTableWrites; else ++counters.argumentTableClears;
			}
		}
		table->bindings = bindings;
		submission->tables.push_back(table);
		// Bound the lookup cache; all tables still live until submission feedback.
		if (submission->tables.size() <= 2048) submission->bindingCache[hash].push_back(table);
		return table;
	}

	void newBindings() {
		vertexArguments = ArgumentBindings(); fragmentArguments = ArgumentBindings();
	}
	void bindBuffer(ArgumentBindings &table, id<MTLBuffer> buffer, NSUInteger offset, uint index) {
		if (index >= 4) error("Metal: invalid argument-table buffer binding");
		accessResource(buffer, &table == &vertexArguments ? MTLStageVertex : MTLStageFragment);
		table.buffers[index] = buffer ? buffer.gpuAddress + offset : 0;
	}
	void bindBytes(ArgumentBindings &table, const void *bytes, NSUInteger length, uint index) {
		NSUInteger offset; id<MTLBuffer> buffer = uploadDrawData(bytes, length, offset);
		bindBuffer(table, buffer, offset, index);
	}
	void bindTexture(ArgumentBindings &table, id<MTLTexture> texture, id<MTLSamplerState> samplerState, uint index) {
		if (index >= 16) error("Metal: invalid argument-table texture binding");
		accessResource(texture, &table == &vertexArguments ? MTLStageVertex : MTLStageFragment); keep(samplerState);
		table.textures[index] = texture.gpuResourceID._impl;
		table.samplers[index] = samplerState.gpuResourceID._impl;
	}
	void bindArguments(id<MTL4RenderCommandEncoder> pass) {
		[pass setArgumentTable:argumentTable(vertexArguments)->value atStages:MTLRenderStageVertex];
		[pass setArgumentTable:argumentTable(fragmentArguments)->value atStages:MTLRenderStageFragment];
	}
	void waitIdle() { dispatch_group_wait(pending, DISPATCH_TIME_FOREVER); }

	void endEncoder() {
		boundStateValid = false;
		if (encoder) {
			finishPass(encoder);
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
		// A terminal CPU wait must include queued event operations even if no
		// rendering followed them. A small empty buffer supplies its feedback.
		if (wait && queueSyncPending && !command) beginCommand();
		if (!command) { if (wait) waitIdle(); return; }
		Submission *batch = submission;
		[batch->residency commit]; [command useResidencySet:batch->residency];
		[command endCommandBuffer];
		++counters.submissions;
		NSMutableArray *pages = drawBuffers, *pool = freeDrawBuffers;
		// The completion block owns the page leases independently of the next
		// submission. Still-live public slices exclude a page from recycling.
		const auto leases = std::make_shared<std::vector<std::shared_ptr<byte> > >(std::move(drawBufferLeases));
		drawBufferLease.reset();
		Impl *owner = this;
		NSLock *poolLock = drawBufferLock;
		dispatch_semaphore_t semaphore = inFlight;
		dispatch_group_t allPending = pending;
		dispatch_group_enter(allPending);
		MTL4CommitOptions *options = [[[MTL4CommitOptions alloc] init] autorelease];
		[options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
			@autoreleasepool {
			if (feedback.error) warning("Metal 4 command failed: %s", [[feedback.error localizedDescription] UTF8String]);
			[batch->residency removeAllAllocations]; [batch->residency commit];
			[batch->resources removeAllObjects];
			[poolLock lock];
			NSUInteger poolBytes = 0;
			for (id<MTLBuffer> page in pool) poolBytes += page.length;
			NSUInteger pageIndex = 0;
			for (id<MTLBuffer> page in pages) {
				bool unpinned = (*leases)[pageIndex++].use_count() == 1;
				if (unpinned && pool.count < 32 && page.length <= 16 * 1024 * 1024 && poolBytes + page.length <= 64 * 1024 * 1024) {
					[pool addObject:page]; poolBytes += page.length;
				}
			}
			for (ArgumentTable *table : batch->tables) {
				if (owner->freeTables.size() < 2048) owner->freeTables.push_back(table); else delete table;
			}
			batch->tables.clear(); batch->bindingCache.clear();
			dispatch_group_leave(batch->completion);
			if (owner->freeSubmissions.size() < 3) owner->freeSubmissions.push_back(batch); else delete batch;
			[poolLock unlock];
			dispatch_semaphore_signal(semaphore);
			dispatch_group_leave(allPending);
			}
		}];
		id<MTL4CommandBuffer> buffers[] = {command};
		[queue commit:buffers count:1 options:options];
		[drawBuffers release]; drawBuffers = nil;
		drawBuffer = nil; drawBufferOffset = 0;
		command = nil; submission = nullptr;
		newBindings();
		if (wait) waitIdle();
	}

	void beginEncoder() {
		if (encoder)
			return;
		beginCommand();
		MTL4RenderPassDescriptor *pass = [[[MTL4RenderPassDescriptor alloc] init] autorelease];
		pass.colorAttachments[0].texture = target->samples > 1 ? target->multisample : target->color;
		pass.colorAttachments[0].resolveTexture = target->samples > 1 ? target->color : nil;
		pass.colorAttachments[0].loadAction = target->clearColorPending ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = target->samples > 1 ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = target->clearColor;
		if (target->depth) {
			pass.depthAttachment.texture = target->depth;
			pass.depthAttachment.loadAction = target->clearDepthPending ? MTLLoadActionClear : MTLLoadActionLoad;
			pass.depthAttachment.storeAction = MTLStoreActionStore;
			pass.depthAttachment.clearDepth = target->clearDepth;
			pass.stencilAttachment.texture = target->depth;
			pass.stencilAttachment.loadAction = target->clearStencilPending ? MTLLoadActionClear : MTLLoadActionLoad;
			pass.stencilAttachment.storeAction = MTLStoreActionStore;
			pass.stencilAttachment.clearStencil = target->clearStencil;
		}
		if (target->samples > 1) {
			MTLSamplePosition positions[8];
			if (target->multisamplingActive)
				[device getDefaultSamplePositions:positions count:target->samples];
			else
				for (uint i = 0; i < target->samples; ++i) positions[i] = MTLSamplePositionMake(0.5, 0.5);
			[pass setSamplePositions:positions count:target->samples];
		}
		if (query) {
			pass.visibilityResultBuffer = query->buffer;
			// Slots start at zero when the query begins; preserve earlier passes
			// instead of Metal 4's default reset between render encoders.
			pass.visibilityResultType = MTLVisibilityResultTypeAccumulate;
		}
		useResource(target->color); useResource(target->multisample); useResource(target->depth);
		if (query) useResource(query->buffer);
		encoder = [[command renderCommandEncoderWithDescriptor:pass] retain];
		if (!encoder)
			error("Metal: could not begin render pass");
		orderEncoder(encoder);
		accessResource(target->color, MTLStageFragment, true);
		accessResource(target->multisample, MTLStageFragment, true);
		accessResource(target->depth, MTLStageFragment, true);
		if (query) accessResource(query->buffer, MTLStageFragment, true);
		if (query) {
			if (query->passes >= 512) error("Metal: too many render passes in an occlusion query");
			[encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:query->passes++ * sizeof(uint64)];
			// A query may cross flush()/present(). Its completion must belong
			// to the last submission that actually writes its result buffer.
			if (query->completion != submission->completion) {
				if (query->completion) dispatch_release(query->completion);
				query->completion = submission->completion;
				dispatch_retain(query->completion);
			}
		}
		target->clearColorPending = target->clearDepthPending = target->clearStencilPending = false;
	}

	id<MTLLibrary> cachedLibrary(const char *source, NSError **failure) {
		NSString *key = [NSString stringWithUTF8String:source];
		id<MTLLibrary> result = [libraries objectForKey:key];
		if (result) return [result retain];
		result = compileLibrary(compiler, source, failure);
		if (result) [libraries setObject:result forKey:key];
		return result;
	}

	id<MTLRenderPipelineState> pipeline(const DrawState &state, bool presentation = false, Program::Impl *program = nullptr,
			MTLPixelFormat colorFormat = MTLPixelFormatInvalid, uint sampleCount = 0, bool fatal = true) {
		if (colorFormat == MTLPixelFormatInvalid)
			colorFormat = presentation ? MTLPixelFormatBGRA8Unorm : target->color.pixelFormat;
		MTL4LibraryFunctionDescriptor *vertexFunction = program ? program->vertex : vertex;
		MTL4LibraryFunctionDescriptor *fragmentFunction = program ? program->fragment : state.logicXor ? logicFragment : fragment;
		// Fixed-width keys avoid formatting pointer and state strings for every draw.
		uint samples = sampleCount ? sampleCount : presentation ? 1 : target->samples;
		bool blending = state.blend && !state.logicXor;
		const uintptr values[] = {(uintptr)vertexFunction, (uintptr)fragmentFunction, (uintptr)colorFormat,
			samples, blending, blending ? state.srcRGB : 0U, blending ? state.dstRGB : 0U, blending ? state.srcAlpha : 0U,
			blending ? state.dstAlpha : 0U, blending ? state.rgbOp : 0U, blending ? state.alphaOp : 0U, state.colorMask, (uintptr)topology(state.primitive)};
		NSData *key = reuseStateKey(lastPipelineKey, values, sizeof(values));
		id<MTLRenderPipelineState> result = [pipelines objectForKey:key];
		if (!result) {
			MTL4RenderPipelineDescriptor *descriptor = [[[MTL4RenderPipelineDescriptor alloc] init] autorelease];
			MTL4PipelineOptions *options = [[[MTL4PipelineOptions alloc] init] autorelease];
			options.shaderReflection = MTL4ShaderReflectionBindingInfo; descriptor.options = options;
			descriptor.vertexFunctionDescriptor = vertexFunction;
			descriptor.fragmentFunctionDescriptor = fragmentFunction;
			descriptor.vertexDescriptor = program ? program->vertexDescriptor : nil;
			descriptor.rasterSampleCount = samples;
			descriptor.inputPrimitiveTopology = topology(state.primitive);
			descriptor.colorAttachments[0].pixelFormat = colorFormat;
			MTL4RenderPipelineColorAttachmentDescriptor *blend = descriptor.colorAttachments[0];
			blend.blendingState = state.blend && !state.logicXor ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
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
			NSString *archivePath = archiveCache.path(program ? program->cacheIdentity : builtinCacheIdentity,
				values + 2, sizeof(values) - 2 * sizeof(values[0]));
			// logicXor selects a distinct built-in fragment function.
			if (!program && state.logicXor) {
				NSMutableData *logicIdentity = [NSMutableData dataWithData:builtinCacheIdentity];
				[logicIdentity appendData:[fragmentFunction.name dataUsingEncoding:NSUTF8StringEncoding]];
				archivePath = archiveCache.path(logicIdentity, values + 2, sizeof(values) - 2 * sizeof(values[0]));
			}
			result = archiveCache.load(device, archivePath, descriptor, counters);
			if (!result) {
				++counters.pipelineCompilations;
				result = [compiler newRenderPipelineStateWithDescriptor:descriptor compilerTaskOptions:nil error:&failure];
				if (result && archivePath && !archiveCache.store(device, archivePath, counters)) {
					// A failed cache is optional. Drop its capturing compiler so a
					// read-only/full disk cannot accumulate unflushed binary data.
					MTL4CompilerDescriptor *plain = [[[MTL4CompilerDescriptor alloc] init] autorelease];
					id<MTL4Compiler> replacement = [device newCompilerWithDescriptor:plain error:nil];
					if (replacement) { [compiler release]; compiler = replacement; }
					archiveCache.close(); ++counters.pipelineArchiveDisables;
				}
			}
			if (!result) {
				if (fatal) error("Metal pipeline: %s", [[failure localizedDescription] UTF8String]);
				warning("Metal program interface: %s", failure ? [[failure localizedDescription] UTF8String] : "pipeline unavailable");
				return nil;
			}
			[pipelines setObject:result forKey:key];
			if (program) {
				MTLRenderPipelineReflection *reflection = result.reflection;
				if (!reflection) {
					program->reflectionFallback = true; program->vertexTextureMask = program->fragmentTextureMask = 0xffff;
					program->vertexBufferMask = program->fragmentBufferMask = 0xf;
				} else if (!program->reflectionFallback) {
					if (!program->reflectionKnown) {
						program->vertexTextureMask = program->fragmentTextureMask = 0;
						program->vertexBufferMask = program->fragmentBufferMask = 0;
					}
					program->vertexTextureMask |= reflectedTextures(reflection.vertexBindings);
					program->fragmentTextureMask |= reflectedTextures(reflection.fragmentBindings);
					program->vertexBufferMask |= reflectedBuffers(reflection.vertexBindings);
					program->fragmentBufferMask |= reflectedBuffers(reflection.fragmentBindings);
					program->reflectionKnown = true;
				}
			}
			if (program) [program->pipelineKeys addObject:key];
			[result release];
		}
		return result;
	}

	id<MTLDepthStencilState> depthState(const DrawState &state) {
		const StencilState &front = state.frontStencil, &back = state.backStencil;
		uint32 values[] = {state.depthTest, state.depthTest && state.depthWrite, state.depthTest ? uint32(state.depthCompare) : 0U, state.stencil,
			front.compare, front.fail, front.depthFail, front.pass, front.readMask, front.writeMask,
			back.compare, back.fail, back.depthFail, back.pass, back.readMask, back.writeMask};
		if (!state.stencil) memset(values + 4, 0, sizeof(values) - 4 * sizeof(uint32));
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
		NSNumber *key = [NSNumber numberWithUnsignedInt:state.linear | (state.wrapS << 1) | (state.wrapT << 3) | (mipmaps << 5) | ((mipmaps && state.mipLinear) << 6) | (state.wrapR << 7) | (anisotropy << 9)];
		id<MTLSamplerState> result = [samplers objectForKey:key];
		if (!result) {
			MTLSamplerDescriptor *descriptor = [[[MTLSamplerDescriptor alloc] init] autorelease];
			descriptor.supportArgumentBuffers = YES;
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
			Program::Impl *program = nullptr, bool bindDefaultTexture = true) {
		// Color-only destinations can still be promoted if an engine genuinely
		// starts using depth/stencil later. Reopen the pass with defined contents.
		if (!target->depth && (state.depthTest || state.stencil)) {
			endEncoder();
			if (!allocateDepth(device, *target)) error("Metal: could not allocate depth/stencil attachment");
		}
		beginEncoder(); newBindings();
		id<MTLRenderPipelineState> renderPipeline = pipeline(state, false, program);
		id<MTLDepthStencilState> depthStencil = depthState(state);
		keep(renderPipeline); keep(depthStencil);
		if (!boundStateValid || boundPipeline != renderPipeline) { [encoder setRenderPipelineState:renderPipeline]; ++counters.encoderStateSetters; }
		if (!boundStateValid || boundDepth != depthStencil) { [encoder setDepthStencilState:depthStencil]; ++counters.encoderStateSetters; }
		if (!boundStateValid || boundState.stencilReference != state.stencilReference) { [encoder setStencilReferenceValue:state.stencilReference]; ++counters.encoderStateSetters; }
		if (!boundStateValid || memcmp(boundState.blendColor, state.blendColor, sizeof(state.blendColor))) {
			[encoder setBlendColorRed:state.blendColor[0] green:state.blendColor[1] blue:state.blendColor[2] alpha:state.blendColor[3]]; ++counters.encoderStateSetters;
		}
		if (!boundStateValid || boundState.cull != state.cull || (state.cull && boundState.cullFront != state.cullFront)) {
			[encoder setCullMode:state.cull ? (state.cullFront ? MTLCullModeFront : MTLCullModeBack) : MTLCullModeNone]; ++counters.encoderStateSetters;
		}
		if (!boundStateValid || boundState.clockwise != state.clockwise) { [encoder setFrontFacingWinding:state.clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise]; ++counters.encoderStateSetters; }
		if (!boundStateValid || boundState.wireframe != state.wireframe) { [encoder setTriangleFillMode:state.wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill]; ++counters.encoderStateSetters; }
		if (!boundStateValid || boundState.depthClamp != state.depthClamp) { [encoder setDepthClipMode:state.depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip]; ++counters.encoderStateSetters; }
		if (!boundStateValid || boundState.depthBias != state.depthBias || boundState.slopeBias != state.slopeBias) {
			[encoder setDepthBias:state.depthBias slopeScale:state.slopeBias clamp:0]; ++counters.encoderStateSetters;
		}
		if (!boundStateValid || boundViewport != viewport || boundState.depthNear != state.depthNear || boundState.depthFar != state.depthFar) {
			MTLViewport vp = {(double)viewport.left, (double)viewport.top, (double)viewport.width(), (double)viewport.height(), state.depthNear, state.depthFar};
			[encoder setViewport:vp]; ++counters.encoderStateSetters;
		}
		Common::Rect scissor(target->w, target->h);
		if (state.scissor) scissor.clip(state.scissorRect);
		if (scissor.isEmpty()) return;
		if (!boundStateValid || boundScissor != scissor) {
			[encoder setScissorRect:MTLScissorRect{(NSUInteger)scissor.left, (NSUInteger)scissor.top, (NSUInteger)scissor.width(), (NSUInteger)scissor.height()}]; ++counters.encoderStateSetters;
		}
		boundStateValid = true; boundPipeline = renderPipeline; boundDepth = depthStencil;
		boundState = state; boundViewport = viewport; boundScissor = scissor;
		// Custom programs bind their own uniform buffers in drawProgram().
		if (!program) {
			Uniforms actual = uniforms;
			actual.textured = texture != nullptr;
			actual.framebufferHeight = target->h;
			// Both stages reference the same immutable arena allocation.
			NSUInteger offset; id<MTLBuffer> buffer = uploadDrawData(&actual, sizeof(actual), offset);
			bindBuffer(vertexArguments, buffer, offset, 1);
			bindBuffer(fragmentArguments, buffer, offset, 1);
		}
		if (bindDefaultTexture) {
			bindTexture(fragmentArguments, texture ? texture->texture : white, sampler(state, texture && texture->mipmaps && state.mipmaps), 0);
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
		_impl->queue = [_impl->device newMTL4CommandQueue];
		NSError *failure = nil;
		_impl->archiveCache.open(_impl->device);
		MTL4CompilerDescriptor *compilerDesc = [[[MTL4CompilerDescriptor alloc] init] autorelease];
		compilerDesc.pipelineDataSetSerializer = _impl->archiveCache.serializer;
		_impl->compiler = [_impl->device newCompilerWithDescriptor:compilerDesc error:&failure];
		_impl->builtinCacheIdentity = [PipelineArchiveCache::digest([NSData dataWithBytes:kShaderSource length:strlen(kShaderSource)]) retain];
		_impl->library = compileLibrary(_impl->compiler, kShaderSource, &failure);
		if (!_impl->queue || !_impl->compiler || !_impl->library) {
			warning("Metal initialization failed: %s", failure ? [[failure localizedDescription] UTF8String] : "command queue unavailable");
			detach();
			return false;
		}
		_impl->vertex = functionDescriptor(_impl->library, @"scummvmVertex");
		_impl->fragment = functionDescriptor(_impl->library, @"scummvmFragment");
		_impl->logicFragment = functionDescriptor(_impl->library, @"scummvmLogicFragment");
		_impl->pipelines = [[NSMutableDictionary alloc] init];
		_impl->depthStates = [[NSMutableDictionary alloc] init];
		_impl->samplers = [[NSMutableDictionary alloc] init];
		_impl->libraries = [[NSCache alloc] init]; _impl->libraries.countLimit = 64;
		MTLTextureDescriptor *white = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
		white.storageMode = MTLStorageModeShared;
		_impl->white = [_impl->device newTextureWithDescriptor:white];
		if (!_impl->vertex || !_impl->fragment || !_impl->logicFragment || !_impl->white) {
			detach();
			return false;
		}
		debug(1, "Metal4: native command queue, allocator, argument tables, residency and compiler on %s", [[_impl->device name] UTF8String]);
		uint32 pixel = 0xffffffff;
		[_impl->white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&pixel bytesPerRow:4];
		return true;
	}
}

void Renderer::detachLayer() {
	@autoreleasepool {
		resetMetalFX();
		_impl->submit(true);
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
		[_impl->libraries release]; _impl->libraries = nil;
		[_impl->lastPipelineKey release]; _impl->lastPipelineKey = nil;
		[_impl->lastDepthKey release]; _impl->lastDepthKey = nil;
		[_impl->vertex release]; _impl->vertex = nil;
		[_impl->fragment release]; _impl->fragment = nil;
		[_impl->logicFragment release]; _impl->logicFragment = nil;
		[_impl->library release]; _impl->library = nil;
		[_impl->queue release]; _impl->queue = nil;
		[_impl->dependencyEvent release]; _impl->dependencyEvent = nil; _impl->dependencyValue = 0;
		[_impl->pendingEvents removeAllObjects]; _impl->queueSyncPending = false;
		[_impl->compiler release]; _impl->compiler = nil;
		_impl->archiveCache.close();
		[_impl->builtinCacheIdentity release]; _impl->builtinCacheIdentity = nil;
		for (Submission *batch : _impl->freeSubmissions) delete batch; _impl->freeSubmissions.clear();
		for (ArgumentTable *table : _impl->freeTables) delete table; _impl->freeTables.clear();
		[_impl->layer release]; _impl->layer = nil;
		[_impl->device release]; _impl->device = nil;
		[_impl->drawBufferLock lock];
		[_impl->freeDrawBuffers removeAllObjects];
		[_impl->drawBufferLock unlock];
		_impl->screen.w = _impl->screen.h = 0;
	}
}

bool Renderer::resize(uint w, uint h, uint samples, bool depthStencil) {
	@autoreleasepool {
		if (!_impl->device || !w || !h || w > maxTextureSize() || h > maxTextureSize() || !supportsSamples(samples))
			return false;
		if (w == _impl->screen.w && h == _impl->screen.h && samples == _impl->screen.samples && depthStencil == _impl->screen.depthRequested)
			return true;
		_impl->submit(true);
		if (!allocateTarget(_impl->device, _impl->screen, w, h, samples, kTargetBGRA8, false, depthStencil))
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

RenderTarget *Renderer::createRenderTarget(uint w, uint h, uint samples, TargetFormat format, bool mipmaps, bool depthStencil) {
	@autoreleasepool {
		if (!_impl->device || !w || !h || w > maxTextureSize() || h > maxTextureSize() || !supportsSamples(samples))
			return nullptr;
		RenderTarget::Impl *target = new RenderTarget::Impl;
		if (!allocateTarget(_impl->device, *target, w, h, samples, format, mipmaps, depthStencil)) {
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
	if (_impl->renderTarget == target) return;
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
		[_impl->metalFXFence release]; _impl->metalFXFence = nil;
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
			_impl->metalFX = [desc newSpatialScalerWithDevice:_impl->device compiler:_impl->compiler];
			if (_impl->metalFX) {
				_impl->metalFXFence = [_impl->device newFence];
				if (!_impl->metalFXFence) { [_impl->metalFX release]; _impl->metalFX = nil; return nullptr; }
				_impl->metalFX.fence = _impl->metalFXFence;
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
			id<MTL4ComputeCommandEncoder> blit = _impl->beginCopy(input, _impl->metalFXInput);
			[blit copyFromTexture:input sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				sourceSize:MTLSizeMake(input.width, input.height, 1) toTexture:_impl->metalFXInput
				destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
			_impl->finishPass(blit);
			input = _impl->metalFXInput;
		}
		_impl->metalFX.colorTexture = input;
		_impl->metalFX.outputTexture = _impl->metalFXOutput->_impl->texture;
		_impl->metalFX.inputContentWidth = input.width;
		_impl->metalFX.inputContentHeight = input.height;
		_impl->useResource(input); _impl->useResource(_impl->metalFXOutput->_impl->texture);
		_impl->keep(_impl->metalFX);
		_impl->keep(_impl->metalFXFence);
		id<MTL4ComputeCommandEncoder> before = _impl->beginCopy(input, _impl->metalFXOutput->_impl->texture);
		[before barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch | MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
		++_impl->counters.queueBarriers;
		[before updateFence:_impl->metalFXFence afterEncoderStages:MTLStageDispatch | MTLStageBlit]; _impl->finishPass(before);
		[_impl->metalFX encodeToCommandBuffer:_impl->command];
		id<MTL4ComputeCommandEncoder> after = [_impl->command computeCommandEncoder];
		[after waitForFence:_impl->metalFXFence beforeEncoderStages:MTLStageDispatch | MTLStageBlit];
		[after barrierAfterStages:MTLStageDispatch | MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
		_impl->finishPass(after); _impl->resetHazards();
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

static PixelFormat rgb565() { return PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0); }
static MTLPixelFormat nativeTextureFormat(const PixelFormat *format) {
	if (format && *format == rgb565()) return MTLPixelFormatB5G6R5Unorm;
	if (format && *format == PixelFormat::createFormatBGRA32()) return MTLPixelFormatBGRA8Unorm;
	return MTLPixelFormatRGBA8Unorm;
}
static bool compressedFormat(MTLPixelFormat format) {
	return format == MTLPixelFormatBC1_RGBA || format == MTLPixelFormatBC2_RGBA || format == MTLPixelFormatBC3_RGBA;
}

Texture *Renderer::createTexture(const Surface &surface, bool mipmaps, uint mipLevels) {
	Texture *texture = allocateTexture(surface.w, surface.h, mipmaps, mipLevels, &surface.format);
	if (texture) updateTexture(*texture, surface, Common::Rect(surface.w, surface.h));
	return texture;
}

Texture *Renderer::allocateTexture(uint width, uint height, bool mipmaps, uint mipLevels, const PixelFormat *format) {
	@autoreleasepool {
		if (!_impl->device || !width || !height || width > maxTextureSize() || height > maxTextureSize())
			return nullptr;
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:nativeTextureFormat(format) width:width height:height mipmapped:mipmaps];
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
		return new Texture(impl);
	}
}

Texture *Renderer::createCompressedTexture(const void *data, uint bytes, uint width, uint height, CompressedFormat format) {
	@autoreleasepool {
		if (!_impl->device || !_impl->device.supportsBCTextureCompression || !data || !width || !height ||
		    width > maxTextureSize() || height > maxTextureSize() || format > kBC3) return nullptr;
		uint blockBytes = format == kBC1 ? 8 : 16;
		uint rowBytes = ((width + 3) / 4) * blockBytes, rows = (height + 3) / 4;
		if (uint64(rowBytes) * rows > bytes) return nullptr;
		MTLPixelFormat pixelFormat = format == kBC1 ? MTLPixelFormatBC1_RGBA : format == kBC2 ? MTLPixelFormatBC2_RGBA : MTLPixelFormatBC3_RGBA;
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat width:width height:height mipmapped:NO];
		desc.storageMode = MTLStorageModePrivate; desc.usage = MTLTextureUsageShaderRead;
		Texture::Impl *impl = new Texture::Impl; impl->mipmaps = false;
		impl->texture = [_impl->device newTextureWithDescriptor:desc];
		if (!impl->texture) { delete impl; return nullptr; }
		uint pitch = (rowBytes + 255) & ~255U;
		NSUInteger offset; id<MTLBuffer> staging = _impl->reserveUploadData(uint64(pitch) * rows, offset);
		for (uint row = 0; row < rows; ++row)
			memcpy((byte *)staging.contents + offset + row * pitch, (const byte *)data + row * rowBytes, rowBytes);
		id<MTL4ComputeCommandEncoder> copy = _impl->beginCopy(staging, impl->texture);
		[copy copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:uint64(pitch) * rows
			sourceSize:MTLSizeMake(width,height,1) toTexture:impl->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
		_impl->finishPass(copy);
		return new Texture(impl);
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
		id<MTL4ComputeCommandEncoder> blit = _impl->beginCopy(staging, texture._impl->texture);
		[blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:(NSUInteger)pitch * area.height()
			sourceSize:MTLSizeMake(area.width(), area.height(), 1) toTexture:texture._impl->texture destinationSlice:0
			destinationLevel:0 destinationOrigin:MTLOriginMake(area.left, area.top, 0)];
		_impl->finishPass(blit);
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
		id<MTL4ComputeCommandEncoder> blit = _impl->beginCopy(staging, impl->texture);
		[blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:imageBytes
			sourceSize:MTLSizeMake(w, h, d) toTexture:impl->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
		_impl->finishPass(blit);
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
		if (query._impl->completion) {
			if (_impl->submission && _impl->submission->completion == query._impl->completion) _impl->submit(false);
			dispatch_group_wait(query._impl->completion, DISPATCH_TIME_FOREVER);
			dispatch_release(query._impl->completion); query._impl->completion = nullptr;
		}
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
		if (!query._impl->completion) {
			_impl->beginCommand();
			query._impl->completion = _impl->submission->completion;
			dispatch_retain(query._impl->completion);
		}
		_impl->query = nullptr;
	}
}

bool Renderer::occlusionQueryReady(const OcclusionQuery &query) const {
	// Polling requests progress, but ending several queries can share one batch.
	if (_impl->submission && _impl->submission->completion == query._impl->completion) _impl->submit(false);
	return query._impl->completion && dispatch_group_wait(query._impl->completion, DISPATCH_TIME_NOW) == 0;
}

uint64 Renderer::occlusionQueryResult(OcclusionQuery &query, bool wait) {
	if (_impl->query == query._impl) error("Metal: reading an active occlusion query");
	if (!query._impl->completion) return 0;
	if (_impl->submission && _impl->submission->completion == query._impl->completion) _impl->submit(false);
	if (wait) dispatch_group_wait(query._impl->completion, DISPATCH_TIME_FOREVER);
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

void Renderer::updateTexture(Texture &texture, const Surface &surface, const Common::Rect &rect, const Common::Point &destination) {
	updateTextureLevel(texture, 0, surface, rect, 0, &destination);
	if (texture._impl->mipmaps) generateMipmaps(texture);
}

void Renderer::updateTextureLevel(Texture &texture, uint level, const Surface &surface, const Common::Rect &rect, uint slice,
		const Common::Point *destination) {
	@autoreleasepool {
		if (compressedFormat(texture._impl->texture.pixelFormat)) {
			// An exceptional CPU patch can promote immutable BC storage through
			// a GPU conversion, preserving all pixels outside the patch.
			Texture *expanded = allocateTexture(texture.width(), texture.height());
			if (!expanded) error("Metal: could not expand compressed texture");
			copyTexture(*expanded, Common::Rect(texture.width(), texture.height()), texture, Common::Rect(texture.width(), texture.height()));
			[texture._impl->texture release]; texture._impl->texture = [expanded->_impl->texture retain];
			texture._impl->mipmaps = false; delete expanded;
		}
		if (level >= texture._impl->texture.mipmapLevelCount)
			error("Metal: mipmap level out of range");
		uint slices = texture._impl->texture.textureType == MTLTextureTypeCube ? 6 : 1;
		if (slice >= slices || texture.depth() != 1)
			error("Metal: texture upload slice out of range");
		Common::Rect area = rect;
		area.clip(Common::Rect(surface.w, surface.h));
		Common::Point origin = destination ? *destination : Common::Point(rect.left, rect.top);
		origin.x += area.left - rect.left; origin.y += area.top - rect.top;
		if (origin.x < 0) { area.left -= origin.x; origin.x = 0; }
		if (origin.y < 0) { area.top -= origin.y; origin.y = 0; }
		area.right = MIN<int>(area.right, area.left + int(MAX(1U, texture.width() >> level)) - origin.x);
		area.bottom = MIN<int>(area.bottom, area.top + int(MAX(1U, texture.height() >> level)) - origin.y);
		if (area.isEmpty())
			return;
		Surface source = surface.getSubArea(area);
		MTLPixelFormat targetFormat = texture._impl->texture.pixelFormat;
		PixelFormat uploadFormat = targetFormat == MTLPixelFormatB5G6R5Unorm ? rgb565() :
			targetFormat == MTLPixelFormatBGRA8Unorm || targetFormat == MTLPixelFormatBGRA8Unorm_sRGB ? PixelFormat::createFormatBGRA32() : PixelFormat::createFormatRGBA32();
		bool needsConversion = source.format != uploadFormat;
		Surface *converted = needsConversion ? source.convertTo(uploadFormat) : nullptr;
		if (needsConversion && !converted)
			error("Metal: could not convert texture pixels");
		const Surface *rgba = converted ? converted : &source;
		uint rowBytes = rgba->w * uploadFormat.bytesPerPixel;
		uint pitch = (rowBytes + 255) & ~255U;
		NSUInteger offset;
		id<MTLBuffer> staging = _impl->reserveUploadData((NSUInteger)pitch * rgba->h, offset);
		for (int y = 0; y < rgba->h; ++y)
			memcpy((byte *)[staging contents] + offset + y * pitch, rgba->getBasePtr(0, y), rowBytes);
		if (converted) {
			converted->free();
			delete converted;
		}
		id<MTL4ComputeCommandEncoder> blit = _impl->beginCopy(staging, texture._impl->texture);
		[blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:pitch sourceBytesPerImage:pitch * area.height()
			sourceSize:MTLSizeMake(area.width(), area.height(), 1) toTexture:texture._impl->texture destinationSlice:slice
			destinationLevel:level destinationOrigin:MTLOriginMake(origin.x, origin.y, 0)];
		_impl->finishPass(blit);
	}
}

void Renderer::generateMipmaps(Texture &texture) {
	@autoreleasepool {
		if (compressedFormat(texture._impl->texture.pixelFormat)) {
			Texture *expanded = allocateTexture(texture.width(), texture.height(), true);
			if (!expanded) error("Metal: could not expand compressed mipmap texture");
			copyTexture(*expanded, Common::Rect(texture.width(), texture.height()), texture, Common::Rect(texture.width(), texture.height()));
			[texture._impl->texture release]; texture._impl->texture = [expanded->_impl->texture retain];
			texture._impl->mipmaps = true; delete expanded; return;
		}
		if (texture.width() == 1 && texture.height() == 1 && texture.depth() == 1)
			return;
		_impl->resolveTextureOwner(texture._impl);
		id<MTL4ComputeCommandEncoder> blit = _impl->beginCopy(texture._impl->texture);
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
			_impl->accessResource(replacement, MTLStageBlit, true);
			uint slices = old.textureType == MTLTextureTypeCube ? 6 : 1;
			for (uint slice = 0; slice < slices; ++slice)
				[blit copyFromTexture:old sourceSlice:slice sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
					sourceSize:MTLSizeMake(old.width, old.height, old.depth) toTexture:replacement destinationSlice:slice
					destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
			texture._impl->texture = replacement;
			texture._impl->mipmaps = true;
			[old release];
		}
		[blit barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
		[blit generateMipmapsForTexture:texture._impl->texture];
		_impl->finishPass(blit);
	}
}

void Renderer::copyTexture(Texture &destination, const Common::Rect &destinationRect,
		Texture &source, const Common::Rect &sourceRect, bool flipY) {
	@autoreleasepool {
		if (destinationRect.isEmpty() || sourceRect.isEmpty() ||
			!Common::Rect(destination.width(), destination.height()).contains(destinationRect) ||
			!Common::Rect(source.width(), source.height()).contains(sourceRect))
			error("Metal: invalid texture copy rectangles");
		_impl->resolveTextureOwner(source._impl);
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
			id<MTL4ComputeCommandEncoder> blit = _impl->beginCopy(source._impl->texture, temporary->texture);
			[blit copyFromTexture:source._impl->texture sourceSlice:0 sourceLevel:0
				sourceOrigin:MTLOriginMake(sourceRect.left, sourceRect.top, 0)
				sourceSize:MTLSizeMake(sourceRect.width(), sourceRect.height(), 1)
				toTexture:temporary->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
			_impl->finishPass(blit);
			copyTexture(destination, destinationRect, snapshot, Common::Rect(sourceRect.width(), sourceRect.height()), flipY);
			return;
		}
		// Single-sample targets can use the same blit as ordinary textures.
		// Resolve deferred clears first so partial copies preserve defined pixels.
		if (!flipY && sourceRect.width() == destinationRect.width() && sourceRect.height() == destinationRect.height() &&
		    source._impl->texture.pixelFormat == destination._impl->texture.pixelFormat &&
		    source._impl->texture.textureType == MTLTextureType2D && destination._impl->texture.textureType == MTLTextureType2D &&
		    (!destination._impl->owner || destination._impl->owner->samples == 1)) {
			_impl->resolveTextureOwner(destination._impl);
			id<MTL4ComputeCommandEncoder> blit = _impl->beginCopy(source._impl->texture, destination._impl->texture);
			[blit copyFromTexture:source._impl->texture sourceSlice:0 sourceLevel:0
				sourceOrigin:MTLOriginMake(sourceRect.left,sourceRect.top,0) sourceSize:MTLSizeMake(sourceRect.width(),sourceRect.height(),1)
				toTexture:destination._impl->texture destinationSlice:0 destinationLevel:0
				destinationOrigin:MTLOriginMake(destinationRect.left,destinationRect.top,0)];
			_impl->finishPass(blit);
			if (destination._impl->mipmaps) generateMipmaps(destination);
			return;
		}
		// Copies into multisample targets must update their multisample storage too.
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
		_impl->beginCommand();
		MTL4RenderPassDescriptor *pass = [[[MTL4RenderPassDescriptor alloc] init] autorelease];
		pass.colorAttachments[0].texture = destination._impl->texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		_impl->useResource(pass.colorAttachments[0].texture);
		id<MTL4RenderCommandEncoder> encoder = [_impl->command renderCommandEncoderWithDescriptor:pass];
		_impl->orderEncoder(encoder);
		_impl->accessResource(pass.colorAttachments[0].texture, MTLStageFragment, true); _impl->newBindings();
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
		id<MTLRenderPipelineState> pipeline = _impl->pipeline(state, true, nullptr, destination._impl->texture.pixelFormat);
		_impl->keep(pipeline); [encoder setRenderPipelineState:pipeline];
		[encoder setViewport:MTLViewport{(double)destinationRect.left, (double)destinationRect.top,
			(double)destinationRect.width(), (double)destinationRect.height(), 0, 1}];
		[encoder setScissorRect:MTLScissorRect{(NSUInteger)destinationRect.left, (NSUInteger)destinationRect.top,
			(NSUInteger)destinationRect.width(), (NSUInteger)destinationRect.height()}];
		_impl->bindBytes(_impl->vertexArguments, vertices, sizeof(vertices), 0);
		NSUInteger sharedUniformOffset;
		id<MTLBuffer> sharedUniforms = _impl->uploadDrawData(&uniforms, sizeof(uniforms), sharedUniformOffset);
		_impl->bindBuffer(_impl->vertexArguments, sharedUniforms, sharedUniformOffset, 1);
		_impl->bindBuffer(_impl->fragmentArguments, sharedUniforms, sharedUniformOffset, 1);
		_impl->bindTexture(_impl->fragmentArguments, source._impl->texture, _impl->sampler(state, false), 0);
		_impl->bindArguments(encoder);
		[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		_impl->finishPass(encoder);
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
				uint bytes = attribute.type == VertexAttribute::kUNorm8 ? 1 : 4;
				if (attribute.index >= 31 || !attribute.components || attribute.components > 4 ||
				    attribute.offset > vertexStride || attribute.components * bytes > vertexStride - attribute.offset ||
				    (attribute.type == VertexAttribute::kUNorm8 && attribute.components != 4)) {
					delete program; return nullptr;
				}
				MTLVertexAttributeDescriptor *descriptor = program->vertexDescriptor.attributes[attribute.index];
				descriptor.format = attribute.type == VertexAttribute::kUNorm8 ? MTLVertexFormatUChar4Normalized : formats[attribute.components];
				descriptor.offset = attribute.offset;
				descriptor.bufferIndex = 0;
			}
		}
		NSMutableData *identity = [NSMutableData data];
		for (const char *text : {source, fragmentSource, vertexFunction, fragmentFunction})
			[identity appendBytes:text length:strlen(text) + 1];
		const uint64 layout[] = {vertexStride, attributeCount};
		[identity appendBytes:layout length:sizeof(layout)];
		for (uint i = 0; i < attributeCount; ++i) {
			const uint64 attribute[] = {attributes[i].index, attributes[i].offset, attributes[i].components, attributes[i].type};
			[identity appendBytes:attribute length:sizeof(attribute)];
		}
		program->cacheIdentity = [PipelineArchiveCache::digest(identity) retain];
		NSError *failure = nil;
		program->library = _impl->cachedLibrary(source, &failure);
		if (program->library) {
			program->vertex = functionDescriptor(program->library, [NSString stringWithUTF8String:vertexFunction]);
			program->fragmentLibrary = source == fragmentSource ? [program->library retain] : _impl->cachedLibrary(fragmentSource, &failure);
			program->fragment = functionDescriptor(program->fragmentLibrary, [NSString stringWithUTF8String:fragmentFunction]);
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
		MTLPixelFormat pixelFormat = format == kTargetRGBA16Float ? MTLPixelFormatRGBA16Float :
			format == kTargetSRGBA8 ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
		DrawState state; state.primitive = kTriangleStrip;
		return _impl->pipeline(state, false, program._impl, pixelFormat, samples, false) != nil;
	}
}

Buffer *Renderer::createBuffer(const void *data, uint size) {
	@autoreleasepool {
		if (!_impl->device || !data || !size) return nullptr;
		Buffer::Impl *buffer = new Buffer::Impl;
		buffer->length = size;
		buffer->buffer = [_impl->device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
		_impl->counters.persistentBufferBytes += size;
		if (!buffer->buffer) { delete buffer; return nullptr; }
		return new Buffer(buffer);
	}
}

Buffer *Renderer::createTransientBuffer(const void *data, uint size) {
	@autoreleasepool {
		if (!_impl->device || !data || !size) return nullptr;
		Buffer::Impl *buffer = new Buffer::Impl;
		buffer->buffer = [_impl->uploadDrawData(data, size, buffer->offset) retain];
		buffer->length = size; buffer->lease = _impl->drawBufferLease;
		return new Buffer(buffer);
	}
}

void Renderer::updateBuffer(Buffer &buffer, const void *data, uint size) {
	@autoreleasepool {
		if (!data || !size) error("Metal: invalid persistent buffer update");
		// Keep the immutable version when animation/material setters repeat
		// identical data. The shared storage is only ever read by the GPU.
		if (buffer._impl->length == size && !memcmp((const byte *)buffer._impl->buffer.contents + buffer._impl->offset, data, size)) return;
		id<MTLBuffer> replacement = [_impl->device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
		if (!replacement) error("Metal: could not replace persistent buffer");
		[buffer._impl->buffer release]; buffer._impl->buffer = replacement;
		buffer._impl->offset = 0; buffer._impl->length = size; buffer._impl->lease.reset();
		_impl->counters.persistentBufferBytes += size;
	}
}

void Renderer::drawProgram(Program &program, const void *vertices, uint count, uint stride,
		const uint32 *indices, uint indexCount, const void *uniforms, uint uniformSize,
		const TextureBinding *textures, uint textureCount, const DrawState &state,
		const void *fragmentUniforms, uint fragmentUniformSize, const GeometryBinding *geometry, const UniformBindings *bindings) {
	@autoreleasepool {
		if ((!vertices && !(geometry && geometry->vertices)) || !count || !stride || !_impl->target->color || _impl->viewport.isEmpty() ||
			(state.scissor && !state.scissorRect.intersects(Common::Rect(width(), height()))))
			return;
		if ((indexCount && !indices && !(geometry && geometry->indices)) || (uniformSize && !uniforms) || (fragmentUniformSize && !fragmentUniforms) || (textureCount && !textures) || textureCount > 16)
			error("Metal: invalid native program draw");
		for (uint i = 0; indices && !(geometry && geometry->indices) && i < indexCount; ++i)
			if (indices[i] >= count)
				error("Metal: native program index out of range");
		// With explicit bindings, slot zero is set below before the draw. Keep
		// the default white texture only for programs with no supplied textures.
		_impl->prepareDraw(nullptr, state, Uniforms(), program._impl, textureCount == 0);
		NSUInteger vertexOffset = 0, uniformOffset = 0, indexOffset = 0, fragmentOffset = 0;
		id<MTLBuffer> vertexBuffer;
		if (geometry && geometry->vertices) {
			vertexBuffer = geometry->vertices->_impl->buffer; vertexOffset = geometry->vertexOffset;
			if (vertexOffset > geometry->vertices->size() || (uint64)count * stride > geometry->vertices->size() - vertexOffset)
				error("Metal: persistent vertex range out of bounds");
			vertexOffset += geometry->vertices->_impl->offset;
		} else vertexBuffer = _impl->uploadDrawData(vertices, (NSUInteger)count * stride, vertexOffset);
		bool vertexUniformUsed = program._impl->vertexBufferMask & 2U, fragmentUniformUsed = program._impl->fragmentBufferMask & 2U;
		const Buffer *vertexUniform = bindings ? bindings->vertex : nullptr;
		const Buffer *fragmentUniform = bindings && bindings->fragment ? bindings->fragment : !fragmentUniformSize ? vertexUniform : nullptr;
		id<MTLBuffer> uniformBuffer = nil, fragmentBuffer = nil;
		if (vertexUniform) uniformOffset = vertexUniform->_impl->offset;
		if (fragmentUniform) fragmentOffset = fragmentUniform->_impl->offset;
		if (vertexUniformUsed) uniformBuffer = vertexUniform ? vertexUniform->_impl->buffer : _impl->uploadDrawData(uniforms, uniformSize, uniformOffset);
		if (fragmentUniformUsed) {
			if (!fragmentUniformSize && fragmentUniform == vertexUniform && vertexUniformUsed) { fragmentBuffer = uniformBuffer; fragmentOffset = uniformOffset; }
			else fragmentBuffer = fragmentUniform ? fragmentUniform->_impl->buffer :
				_impl->uploadDrawData(fragmentUniformSize ? fragmentUniforms : uniforms, fragmentUniformSize ? fragmentUniformSize : uniformSize, fragmentOffset);
		}
		bool indices16 = geometry && geometry->indices && geometry->indices16;
		id<MTLBuffer> indexBuffer;
		if (geometry && geometry->indices) {
			indexBuffer = geometry->indices->_impl->buffer; indexOffset = geometry->indexOffset;
			uint indexSize = indices16 ? 2 : 4;
			if (indexOffset % indexSize || indexOffset > geometry->indices->size() || (uint64)indexCount * indexSize > geometry->indices->size() - indexOffset)
				error("Metal: persistent index range out of bounds");
			indexOffset += geometry->indices->_impl->offset;
		} else indexBuffer = _impl->uploadDrawData(indices, (NSUInteger)indexCount * sizeof(uint32), indexOffset);
		if (!vertexBuffer || (vertexUniformUsed && uniformSize && !uniformBuffer) || (fragmentUniformUsed && (fragmentUniformSize || uniformSize) && !fragmentBuffer) || (indexCount && !indexBuffer))
			error("Metal: could not allocate native program draw buffers");
		_impl->bindBuffer(_impl->vertexArguments, vertexBuffer, vertexOffset, 0);
		if (vertexUniformUsed) _impl->bindBuffer(_impl->vertexArguments, uniformBuffer, uniformOffset, 1);
		if (fragmentUniformUsed) _impl->bindBuffer(_impl->fragmentArguments, fragmentBuffer, fragmentOffset, 1);
		if (bindings) {
			if (bindings->extraSize && !bindings->extraData) error("Metal: invalid auxiliary uniforms");
			bool vertexExtraUsed = program._impl->vertexBufferMask & 4U, fragmentExtraUsed = program._impl->fragmentBufferMask & 4U;
			NSUInteger offset = 0;
			id<MTLBuffer> extra = nil;
			if ((vertexExtraUsed || fragmentExtraUsed) && bindings->extraSize)
				extra = _impl->uploadDrawData(bindings->extraData, bindings->extraSize, offset);
			if (vertexExtraUsed) _impl->bindBuffer(_impl->vertexArguments,
				bindings->extraVertex ? bindings->extraVertex->_impl->buffer : extra, bindings->extraVertex ? bindings->extraVertex->_impl->offset : offset, 2);
			if (fragmentExtraUsed) _impl->bindBuffer(_impl->fragmentArguments,
				bindings->extraFragment ? bindings->extraFragment->_impl->buffer : extra, bindings->extraFragment ? bindings->extraFragment->_impl->offset : offset, 2);
		}
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
			if (program._impl->fragmentTextureMask & (1U << i)) _impl->bindTexture(_impl->fragmentArguments, image, sampler, i);
			if (program._impl->vertexTextureMask & (1U << i)) _impl->bindTexture(_impl->vertexArguments, image, sampler, i);
		}
		_impl->accessResource(indexBuffer, MTLStageVertex);
		_impl->bindArguments(_impl->encoder);
		if (indexCount)
			[_impl->encoder drawIndexedPrimitives:primitiveType(state.primitive) indexCount:indexCount indexType:(indices16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32) indexBuffer:indexBuffer.gpuAddress + indexOffset indexBufferLength:indexBuffer.length - indexOffset];
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
		_impl->bindBuffer(_impl->vertexArguments, buffer, offset, 0);
		_impl->bindArguments(_impl->encoder);
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
		_impl->bindBuffer(_impl->vertexArguments, vertexBuffer, vertexOffset, 0);
		_impl->accessResource(indexBuffer, MTLStageVertex);
		_impl->bindArguments(_impl->encoder);
		[_impl->encoder drawIndexedPrimitives:primitiveType(state.primitive) indexCount:indexCount indexType:MTLIndexTypeUInt32 indexBuffer:indexBuffer.gpuAddress + indexOffset indexBufferLength:indexBuffer.length - indexOffset];
	}
}

void Renderer::drawBuffered(const GeometryBinding &geometry, uint count, uint indexCount,
		Texture *texture, const DrawState &state, const Uniforms &uniforms) {
	@autoreleasepool {
		if (!geometry.vertices || !count || !_impl->target->color || _impl->viewport.isEmpty() ||
		    (state.scissor && !state.scissorRect.intersects(Common::Rect(width(), height())))) return;
		id<MTLBuffer> vertices = geometry.vertices->_impl->buffer;
		id<MTLBuffer> indices = geometry.indices ? geometry.indices->_impl->buffer : nil;
		uint indexSize = geometry.indices16 ? 2 : 4;
		if (geometry.vertexOffset > geometry.vertices->size() || uint64(count) * sizeof(Vertex) > geometry.vertices->size() - geometry.vertexOffset ||
		    (indexCount && (!indices || geometry.indexOffset % indexSize || geometry.indexOffset > geometry.indices->size() ||
		    uint64(indexCount) * indexSize > geometry.indices->size() - geometry.indexOffset)))
			error("Metal: buffered geometry range out of bounds");
		_impl->prepareDraw(texture ? texture->_impl : nullptr, state, uniforms);
		_impl->bindBuffer(_impl->vertexArguments, vertices, geometry.vertices->_impl->offset + geometry.vertexOffset, 0);
		_impl->accessResource(indices, MTLStageVertex);
		_impl->bindArguments(_impl->encoder);
		if (indexCount)
			[_impl->encoder drawIndexedPrimitives:primitiveType(state.primitive) indexCount:indexCount
				indexType:(geometry.indices16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32)
				indexBuffer:indices.gpuAddress + geometry.indices->_impl->offset + geometry.indexOffset indexBufferLength:geometry.indices->size() - geometry.indexOffset];
		else
			[_impl->encoder drawPrimitives:primitiveType(state.primitive) vertexStart:0 vertexCount:count];
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
		if (_impl->target->clearColorPending || _impl->target->clearDepthPending || _impl->target->clearStencilPending)
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
		[_impl->queue waitForDrawable:drawable];
		MTL4RenderPassDescriptor *pass = [[[MTL4RenderPassDescriptor alloc] init] autorelease];
		pass.colorAttachments[0].texture = drawable.texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		_impl->useResource(pass.colorAttachments[0].texture);
		id<MTL4RenderCommandEncoder> encoder = [_impl->command renderCommandEncoderWithDescriptor:pass];
		_impl->orderEncoder(encoder);
		_impl->accessResource(pass.colorAttachments[0].texture, MTLStageFragment, true); _impl->newBindings();
		DrawState state;
		Uniforms uniforms;
		uniforms.textured = 1;
		Vertex vertices[] = {
			{{-1, 1, 0}, {0, 0}, {1, 1, 1, 1}}, {{-1, -1, 0}, {0, 1}, {1, 1, 1, 1}},
			{{1, 1, 0}, {1, 0}, {1, 1, 1, 1}}, {{1, -1, 0}, {1, 1}, {1, 1, 1, 1}}
		};
		id<MTLRenderPipelineState> pipeline = _impl->pipeline(state, true);
		_impl->keep(pipeline); [encoder setRenderPipelineState:pipeline];
		_impl->bindBytes(_impl->vertexArguments, vertices, sizeof(vertices), 0);
		NSUInteger sharedUniformOffset;
		id<MTLBuffer> sharedUniforms = _impl->uploadDrawData(&uniforms, sizeof(uniforms), sharedUniformOffset);
		_impl->bindBuffer(_impl->vertexArguments, sharedUniforms, sharedUniformOffset, 1);
		_impl->bindBuffer(_impl->fragmentArguments, sharedUniforms, sharedUniformOffset, 1);
		_impl->bindTexture(_impl->fragmentArguments, _impl->target->color, _impl->sampler(state, false), 0);
		_impl->bindArguments(encoder);
		[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		_impl->finishPass(encoder);
		_impl->submit(false);
		[_impl->queue signalDrawable:drawable];
		[drawable present];
		setRenderTarget(nullptr);
		return true;
	}
}

void Renderer::waitFor(Renderer &producer) {
	@autoreleasepool {
		if (&producer == this) return;
		if (!_impl->queue || !producer._impl->queue || _impl->device != producer._impl->device)
			error("Metal: queue dependency requires the same active device");
		_impl->submit(false);
		producer._impl->submit(false);
		if (!producer._impl->dependencyEvent) producer._impl->dependencyEvent = [_impl->device newEvent];
		if (!producer._impl->dependencyEvent) error("Metal: could not create queue dependency event");
		uint64 value = ++producer._impl->dependencyValue;
		[producer._impl->queue signalEvent:producer._impl->dependencyEvent value:value];
		[_impl->queue waitForEvent:producer._impl->dependencyEvent value:value];
		[_impl->pendingEvents addObject:producer._impl->dependencyEvent];
		producer._impl->queueSyncPending = _impl->queueSyncPending = true;
	}
}

Statistics Renderer::statistics() const { return _impl->counters; }
void Renderer::resetStatistics() { _impl->counters = Statistics(); }

void Renderer::flush(bool wait) {
	@autoreleasepool {
		_impl->submit(wait);
	}
}

bool Renderer::readPixels(Surface &surface, const Common::Rect &rect) {
	if (!_impl->target->color)
		return false;
	Texture::Impl *view = new Texture::Impl();
	view->texture = [_impl->target->color retain];
	view->mipmaps = false;
	view->owner = _impl->target;
	Texture texture(view);
	return readTexture(texture, surface, rect);
}

bool Renderer::readTexture(Texture &texture, Surface &surface, const Common::Rect &rect) {
	@autoreleasepool {
		if (rect.isEmpty() || !Common::Rect(texture.width(), texture.height()).contains(rect))
			return false;
		MTLPixelFormat inputFormat = texture._impl->texture.pixelFormat;
		if (inputFormat == MTLPixelFormatRGBA16Float || inputFormat == MTLPixelFormatB5G6R5Unorm ||
		    inputFormat == MTLPixelFormatR8Unorm || compressedFormat(inputFormat)) {
			RenderTarget *previous = renderTarget();
			Common::Rect previousViewport = viewport();
			RenderTarget *converted = createRenderTarget(rect.width(), rect.height(), 1, kTargetBGRA8, false, false);
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
		id<MTL4ComputeCommandEncoder> blit = _impl->beginCopy(texture._impl->texture, staging);
		[blit copyFromTexture:texture._impl->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(rect.left, rect.top, 0)
			sourceSize:MTLSizeMake(rect.width(), rect.height(), 1) toBuffer:staging destinationOffset:0
			destinationBytesPerRow:pitch destinationBytesPerImage:pitch * rect.height()];
		_impl->finishPass(blit);
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
