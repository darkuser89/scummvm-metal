/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRIM_METAL_DRAW_H
#define GRIM_METAL_DRAW_H

#include "common/array.h"
#include "common/hashmap.h"
#include "common/ptr.h"
#include "math/matrix4.h"
#include "math/vector2d.h"
#include "math/vector3d.h"
#include "math/vector4d.h"
#include "graphics/metal/renderer.h"
#include "engines/grim/metal_shader_types.h"

namespace Grim {
namespace MetalBackend {

// State tokens describe this engine's native batching interface. Their values
// are independent of OpenGL and no GL implementation is called by this backend.
typedef uint StateToken;
enum {
	kAlpha = 100,
	kAlways = 101,
	kArrayBuffer = 102,
	kBlend = 103,
	kClampToEdge = 104,
	kColorAttachment0 = 105,
	kColorBufferBit = 1,
	kCullFace = 107,
	kCw = 108,
	kDepthBufferBit = 2,
	kDepthTest = 110,
	kDynamicDraw = 111,
	kElementArrayBuffer = 112,
	kEqual = 113,
	kFalse = 0,
	kFloat = 115,
	kFramebuffer = 116,
	kFramebufferBinding = 117,
	kKeep = 118,
	kLequal = 119,
	kLinear = 120,
	kLines = 121,
	kLuminanceAlpha = 122,
	kNearest = 123,
	kOne = 124,
	kOneMinusSrcAlpha = 125,
	kPolygonOffsetFill = 126,
	kRepeat = 127,
	kReplace = 128,
	kRgb = 129,
	kRgba = 130,
	kSrcAlpha = 131,
	kStaticDraw = 132,
	kStencilBufferBit = 4,
	kStencilTest = 134,
	kStreamDraw = 135,
	kTexture0 = 0,
	kTexture1 = 1,
	kTexture2D = 138,
	kTextureMagFilter = 139,
	kTextureMinFilter = 140,
	kTextureWrapS = 141,
	kTextureWrapT = 142,
	kTriangles = 143,
	kTriangleFan = 144,
	kTriangleStrip = 145,
	kTrue = 1,
	kUnpackAlignment = 147,
	kUnsignedByte = 148,
	kUnsignedShort = 149,
	kUnsignedShort565 = 150,
};

struct Attribute {
	uint32 _idx = 0, _vbo = 0;
	uint _size = 4, _stride = 0, _type = kFloat;
	uintptr _pointer = 0;
	bool _enabled = false, _normalized = false;
	float _constant[4] = {0, 0, 0, 1};
};

class Context;

class Shader {
public:
	~Shader();
	static Shader *fromFiles(const char *name, const char *const *attributes);
	Shader *clone() const { return new Shader(*this); }
	void use(bool force = false);
	bool setUniform(const Common::String &name, const Math::Matrix4 &value);
	bool setUniform(const Common::String &name, const Math::Vector2d &value);
	bool setUniform(const Common::String &name, const Math::Vector3d &value);
	bool setUniform(const Common::String &name, const Math::Vector4d &value);
	bool setUniform(const Common::String &name, int value);
	bool setUniform1f(const Common::String &name, float value);
	int getUniformLocation(const Common::String &name) const;
	void enableVertexAttribute(const char *name, uint32 buffer, uint size, StateToken type,
		bool normalized, uint stride, uintptr offset);
	void disableVertexAttribute(const char *name, const Math::Vector4d &value);
	Attribute &getAttribute(const char *name);
	Attribute &getAttributeAt(uint index) { return _attributes[index]; }
	static uint32 createBuffer(StateToken target, uint size, const void *data, StateToken usage);
	static void freeBuffer(uint32 buffer);

private:
	friend class Context;
	Common::SharedPtr<Graphics::Metal::Program> _program;
	// Clones retain the same program, whose uniform state was shared by the
	// OpenGL implementation. Keep vertex attributes local to each clone.
	Common::SharedPtr<ShaderUniforms> _uniforms{new ShaderUniforms{}};
	Attribute _attributes[4];
	bool set(const Common::String &name, const float *values, uint count);
};

class Context {
public:
	Context();
	~Context();
	void attach(Graphics::Metal::Renderer &renderer);
	static Context &current();
	void metalGenBuffers(uint count, uint32 *buffers);
	void metalDeleteBuffers(uint count, const uint32 *buffers);
	void metalBindBuffer(StateToken target, uint32 buffer);
	void metalBufferData(StateToken target, uint size, const void *data, StateToken usage);
	void metalBufferSubData(StateToken target, uint offset, uint size, const void *data);
	void metalGenTextures(uint count, uint32 *textures);
	void metalDeleteTextures(uint count, const uint32 *textures);
	void metalActiveTexture(StateToken unit);
	void metalBindTexture(StateToken target, uint32 texture);
	void metalTexParameteri(StateToken target, StateToken parameter, StateToken value);
	void metalPixelStorei(StateToken parameter, int value);
	void metalTexImage2D(StateToken target, int level, int internalFormat, int width, int height,
		int border, StateToken format, StateToken type, const void *data);
	void metalTexSubImage2D(StateToken target, int level, int x, int y, int width, int height,
		StateToken format, StateToken type, const void *data);
	void metalCopyTexImage2D(StateToken target, int level, int format, int x, int y, int width, int height, int border);
	void metalEnable(StateToken state);
	void metalDisable(StateToken state);
	void metalBlendFunc(StateToken source, StateToken destination);
	void metalDepthFunc(StateToken function);
	void metalDepthMask(bool enabled);
	void metalFrontFace(StateToken winding);
	void metalColorMask(bool r, bool g, bool b, bool a);
	void metalStencilFunc(StateToken function, int reference, uint mask);
	void metalStencilOp(StateToken fail, StateToken depthFail, StateToken pass);
	void metalClearStencil(int value);
	void metalClear(uint mask);
	void metalViewport(int x, int y, int width, int height);
	void metalUniform4fv(int location, uint count, const float *data);
	void metalEnableVertexAttribArray(uint location);
	void metalVertexAttribPointer(uint location, uint size, StateToken type, bool normalized, uint stride, const void *offset);
	void metalDrawArrays(StateToken primitive, uint first, uint count);
	void metalDrawElements(StateToken primitive, uint count, StateToken type, const void *offset);
	void metalCapture(uint32 texture, bool stored, Graphics::Surface &surface);
	void metalCaptureRect(int x, int y, int width, int height, byte *pixels);
	void metalFilterTexture(uint32 texture, Shader &shader);

private:
	friend class Shader;
	static Context *_current;
	Graphics::Metal::Renderer *_renderer = nullptr;
	Graphics::Metal::Texture *_white = nullptr;
	Shader *_shader = nullptr;
	struct Image {
		Graphics::Metal::Texture *texture = nullptr;
		Graphics::Metal::TextureBinding sampling;
	};
	Common::HashMap<uint32, Image> _images;
	Common::HashMap<uint32, Common::Array<byte> > _buffers;
	struct Geometry {
		Attribute attributes[4];
		Graphics::Metal::Buffer *buffer = nullptr;
		uint count = 0;
		~Geometry() { delete buffer; }
	};
	Common::Array<Geometry *> _geometry;
	Common::HashMap<uint32, Graphics::Metal::Buffer *> _indexBuffers;
	void invalidateGeometry(uint32 buffer = 0);
	Geometry *geometry(uint minimumCount);

	uint32 _nextBuffer = 1, _nextImage = 1;
	uint32 _arrayBuffer = 0, _elementBuffer = 0, _boundImages[2] = {};
	uint _activeUnit = 0, _unpackAlignment = 4;
	uint32 _clearStencil = 0;
	Graphics::Metal::DrawState _state;
	Image &image();
	uint32 boundBuffer(StateToken target) const;
	void draw(StateToken primitive, const uint32 *indices, uint count, Graphics::Metal::Buffer *indexBuffer = nullptr, uint indexOffset = 0, bool shortIndices = false, uint firstVertex = 0);
	Graphics::Surface convertPixels(uint width, uint height, StateToken format, StateToken type, const void *pixels) const;
};

} // End of namespace MetalBackend
} // End of namespace Grim

#endif
