/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WME_METAL_DRAW_H
#define WME_METAL_DRAW_H

#include "common/array.h"
#include "common/hashmap.h"
#include "common/ptr.h"
#include "math/matrix4.h"
#include "math/vector2d.h"
#include "math/vector3d.h"
#include "math/vector4d.h"
#include "graphics/metal/renderer.h"
#include "engines/wintermute/base/gfx/metal/metal_shader_types.h"

namespace Wintermute {
namespace MetalBackend {

// State tokens describe this engine's native batching interface. Their values
// are independent of OpenGL and no GL implementation is called by this backend.
typedef uint StateToken;
enum {
	kAlpha = 402,
	kAlways = 200,
	kArrayBuffer = 201,
	kBack = 202,
	kBlend = 203,
	kCcw = 204,
	kClampToEdge = 205,
	kColorBufferBit = 1,
	kCullFace = 207,
	kCw = 208,
	kDecr = 209,
	kDepthBufferBit = 2,
	kDepthTest = 211,
	kDynamicDraw = 212,
	kElementArrayBuffer = 213,
	kEqual = 214,
	kFalse = 0,
	kFloat = 216,
	kFront = 217,
	kIncr = 218,
	kKeep = 219,
	kLequal = 220,
	kLinear = 221,
	kLinearMipmapLinear = 222,
	kLines = 223,
	kLuminanceAlpha = 403,
	kNearest = 404,
	kOne = 224,
	kOneMinusSrcAlpha = 225,
	kOneMinusSrcColor = 226,
	kPolygonOffsetFill = 407,
	kRepeat = 227,
	kReplace = 228,
	kRgb = 229,
	kRgba = 230,
	kSrcAlpha = 231,
	kStaticDraw = 232,
	kStencilBufferBit = 4,
	kStencilTest = 234,
	kTexture0 = 0,
	kTexture1 = 1,
	kTexture2D = 236,
	kTextureBinding2D = 237,
	kTextureMagFilter = 238,
	kTextureMinFilter = 239,
	kTextureWrapS = 240,
	kTextureWrapT = 241,
	kTriangles = 242,
	kTriangleFan = 406,
	kTriangleStrip = 243,
	kTrue = 1,
	kUnpackAlignment = 405,
	kUnsignedByte = 245,
	kUnsignedInt = 246,
	kUnsignedShort = 400,
	kUnsignedShort565 = 401,
	kZero = 247,
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
	bool setUniform(const Common::String &name, const Math::Matrix3 &value);
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
	ShaderUniforms _uniforms = {};
	Attribute _attributes[4];
	bool set(const Common::String &name, const float *values, uint count);
};

class Context {
public:
	Context();
	~Context();
	void attach(Graphics::Metal::Renderer &renderer);
	void setLogicalSize(uint width, uint height) { _logicalWidth = width; _logicalHeight = height; }
	Common::Rect mapLogicalRect(const Common::Rect32 &rect) const;
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
	void metalClearColor(float r, float g, float b, float a);
	void metalCullFace(StateToken face);
	void metalGenerateMipmap(StateToken target);
	void metalGetBooleanv(StateToken state, bool *value) const;
	void metalGetIntegerv(StateToken state, int *value) const;
	void metalUniform1i(int location, int value);
	void metalReadPixels(int x, int y, int width, int height, StateToken format, StateToken type, void *pixels);

private:
	friend class Shader;
	static Context *_current;
	Graphics::Metal::Renderer *_nativeRenderer = nullptr;
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
	float _clearColor[4] = {};
	uint _logicalWidth = 0, _logicalHeight = 0;
	Graphics::Metal::DrawState _drawState;
	Image &image();
	uint32 boundBuffer(StateToken target) const;
	void draw(StateToken primitive, const uint32 *indices, uint count, Graphics::Metal::Buffer *indexBuffer = nullptr, uint indexOffset = 0, bool shortIndices = false, uint firstVertex = 0);
	Graphics::Surface convertPixels(uint width, uint height, StateToken format, StateToken type, const void *pixels) const;
};


inline void metalGenBuffers(uint count, uint32 *buffers) { Context::current().metalGenBuffers(count, buffers); }
inline void metalDeleteBuffers(uint count, const uint32 *buffers) { Context::current().metalDeleteBuffers(count, buffers); }
inline void metalBindBuffer(StateToken target, uint32 buffer) { Context::current().metalBindBuffer(target, buffer); }
inline void metalBufferData(StateToken target, uint size, const void *data, StateToken usage) { Context::current().metalBufferData(target, size, data, usage); }
inline void metalBufferSubData(StateToken target, uint offset, uint size, const void *data) { Context::current().metalBufferSubData(target, offset, size, data); }
inline void metalGenTextures(uint count, uint32 *textures) { Context::current().metalGenTextures(count, textures); }
inline void metalDeleteTextures(uint count, const uint32 *textures) { Context::current().metalDeleteTextures(count, textures); }
inline void metalActiveTexture(StateToken unit) { Context::current().metalActiveTexture(unit); }
inline void metalBindTexture(StateToken target, uint32 texture) { Context::current().metalBindTexture(target, texture); }
inline void metalTexParameteri(StateToken target, StateToken parameter, StateToken value) { Context::current().metalTexParameteri(target, parameter, value); }
inline void metalPixelStorei(StateToken parameter, int value) { Context::current().metalPixelStorei(parameter, value); }
inline void metalTexImage2D(StateToken target, int level, int internalFormat, int width, int height,int border, StateToken format, StateToken type, const void *data) { Context::current().metalTexImage2D(target, level, internalFormat, width, height, border, format, type, data); }
inline void metalTexSubImage2D(StateToken target, int level, int x, int y, int width, int height,StateToken format, StateToken type, const void *data) { Context::current().metalTexSubImage2D(target, level, x, y, width, height, format, type, data); }
inline void metalCopyTexImage2D(StateToken target, int level, int format, int x, int y, int width, int height, int border) { Context::current().metalCopyTexImage2D(target, level, format, x, y, width, height, border); }
inline void metalEnable(StateToken state) { Context::current().metalEnable(state); }
inline void metalDisable(StateToken state) { Context::current().metalDisable(state); }
inline void metalBlendFunc(StateToken source, StateToken destination) { Context::current().metalBlendFunc(source, destination); }
inline void metalDepthFunc(StateToken function) { Context::current().metalDepthFunc(function); }
inline void metalDepthMask(bool enabled) { Context::current().metalDepthMask(enabled); }
inline void metalFrontFace(StateToken winding) { Context::current().metalFrontFace(winding); }
inline void metalColorMask(bool r, bool g, bool b, bool a) { Context::current().metalColorMask(r, g, b, a); }
inline void metalStencilFunc(StateToken function, int reference, uint mask) { Context::current().metalStencilFunc(function, reference, mask); }
inline void metalStencilOp(StateToken fail, StateToken depthFail, StateToken pass) { Context::current().metalStencilOp(fail, depthFail, pass); }
inline void metalClearStencil(int value) { Context::current().metalClearStencil(value); }
inline void metalClear(uint mask) { Context::current().metalClear(mask); }
inline void metalViewport(int x, int y, int width, int height) { Context::current().metalViewport(x, y, width, height); }
inline void metalUniform4fv(int location, uint count, const float *data) { Context::current().metalUniform4fv(location, count, data); }
inline void metalEnableVertexAttribArray(uint location) { Context::current().metalEnableVertexAttribArray(location); }
inline void metalVertexAttribPointer(uint location, uint size, StateToken type, bool normalized, uint stride, const void *offset) { Context::current().metalVertexAttribPointer(location, size, type, normalized, stride, offset); }
inline void metalDrawArrays(StateToken primitive, uint first, uint count) { Context::current().metalDrawArrays(primitive, first, count); }
inline void metalDrawElements(StateToken primitive, uint count, StateToken type, const void *offset) { Context::current().metalDrawElements(primitive, count, type, offset); }
inline void metalCapture(uint32 texture, bool stored, Graphics::Surface &surface) { Context::current().metalCapture(texture, stored, surface); }
inline void metalCaptureRect(int x, int y, int width, int height, byte *pixels) { Context::current().metalCaptureRect(x, y, width, height, pixels); }
inline void metalFilterTexture(uint32 texture, Shader &shader) { Context::current().metalFilterTexture(texture, shader); }

inline void metalClearColor(float r, float g, float b, float a) { Context::current().metalClearColor(r, g, b, a); }
inline void metalCullFace(StateToken face) { Context::current().metalCullFace(face); }
inline void metalGenerateMipmap(StateToken target) { Context::current().metalGenerateMipmap(target); }
inline void metalGetBooleanv(StateToken state, bool *value) { Context::current().metalGetBooleanv(state, value); }
inline void metalGetIntegerv(StateToken state, int *value) { Context::current().metalGetIntegerv(state, value); }
inline void metalUniform1i(int location, int value) { Context::current().metalUniform1i(location, value); }
inline void metalReadPixels(int x, int y, int width, int height, StateToken format, StateToken type, void *pixels) { Context::current().metalReadPixels(x, y, width, height, format, type, pixels); }

} // End of namespace MetalBackend
} // End of namespace Wintermute

#endif
