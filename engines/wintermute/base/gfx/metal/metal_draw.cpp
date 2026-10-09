/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engines/wintermute/base/gfx/metal/metal_draw.h"
#include "engines/wintermute/base/gfx/metal/metal_shader_sources.h"
#include "common/endian.h"
#include "common/textconsole.h"

namespace Wintermute {
namespace MetalBackend {

Context *Context::_current = nullptr;

Context::Context() {
	_current = this;
	_drawState.depthCompare = Graphics::Metal::kLess;
	_drawState.depthWrite = true;
	_drawState.srcAlpha = Graphics::Metal::kSrcAlpha;
}
Context::~Context() {
	if (_nativeRenderer) _nativeRenderer->flush(true);
	for (auto &entry : _images) delete entry._value.texture;
	delete _white;
	if (_current == this) _current = nullptr;
}
Context &Context::current() {
	if (!_current) error("Wintermute: native draw context unavailable");
	return *_current;
}
void Context::attach(Graphics::Metal::Renderer &renderer) {
	_nativeRenderer = &renderer;
	Graphics::Surface white;
	white.create(1, 1, Graphics::PixelFormat::createFormatRGBA32());
	white.fillRect(Common::Rect(1, 1), white.format.ARGBToColor(255, 255, 255, 255));
	delete _white;
	_white = renderer.createTexture(white);
	white.free();
	if (!_white) error("Wintermute: cannot allocate native fallback texture");
}

Shader::~Shader() {
	if (Context::_current && Context::_current->_shader == this)
		Context::_current->_shader = nullptr;
}
Shader *Shader::fromFiles(const char *name, const char *const *attributes) {
	Context &context = Context::current();
	for (const ShaderSource &source : kShaderSources) {
		if (strcmp(source.name, name)) continue;
		Common::String msl(kShaderPrelude);
		msl += source.source;
		Shader *shader = new Shader();
		shader->_program.reset(context._nativeRenderer->createProgram(msl.c_str(), source.vertexEntry, source.fragmentEntry));
		if (!shader->_program) error("Wintermute: cannot compile native shader %s", name);
		for (uint i = 0; i < 4; ++i) shader->_attributes[i]._idx = i;
		return shader;
	}
	error("Wintermute: native shader %s unavailable", name);
}
void Shader::use(bool force) { Context::current()._shader = this; }
int Shader::getUniformLocation(const Common::String &name) const {
	for (uint i = 0; i < ARRAYSIZE(kUniformBindings); ++i)
		if (name == kUniformBindings[i].name) return i;
	return -1;
}
bool Shader::set(const Common::String &name, const float *values, uint count) {
	int index = getUniformLocation(name);
	if (index < 0) return false;
	const UniformBinding &binding = kUniformBindings[index];
	if (count > binding.components * binding.count) error("Wintermute: invalid native uniform %s", name.c_str());
	memcpy((byte *)&_uniforms + binding.offset, values, count * sizeof(float));
	use();
	return true;
}
bool Shader::setUniform(const Common::String &name, const Math::Matrix4 &value) { return set(name, value.getData(), 16); }
bool Shader::setUniform(const Common::String &name, const Math::Matrix3 &value) {
	int index = getUniformLocation(name);
	if (index < 0) return false;
	const UniformBinding &binding = kUniformBindings[index];
	if (binding.components != 9) error("Wintermute: invalid native 3x3 matrix uniform");
	float *destination = (float *)((byte *)&_uniforms + binding.offset);
	for (uint column = 0; column < 3; ++column) {
		memcpy(destination + column * 4, value.getData() + column * 3, 3 * sizeof(float));
		destination[column * 4 + 3] = 0;
	}
	use();
	return true;
}
bool Shader::setUniform(const Common::String &name, const Math::Vector2d &value) {
	float data[] = {value.getX(), value.getY()}; return set(name, data, 2);
}
bool Shader::setUniform(const Common::String &name, const Math::Vector3d &value) {
	float data[] = {value.x(), value.y(), value.z()}; return set(name, data, 3);
}
bool Shader::setUniform(const Common::String &name, const Math::Vector4d &value) {
	float data[] = {value.x(), value.y(), value.z(), value.w()}; return set(name, data, 4);
}
bool Shader::setUniform(const Common::String &name, int value) { float data = value; return set(name, &data, 1); }
bool Shader::setUniform1f(const Common::String &name, float value) { return set(name, &value, 1); }
Attribute &Shader::getAttribute(const char *name) {
	static const char *const names[] = {"position", "texcoord", "color", "normal"};
	for (uint i = 0; i < ARRAYSIZE(names); ++i)
		if (!strcmp(name, names[i])) return _attributes[i];
	error("Wintermute: unknown native vertex attribute %s", name);
}
void Shader::enableVertexAttribute(const char *name, uint32 buffer, uint size, StateToken type,
		bool normalized, uint stride, uintptr offset) {
	Attribute &attribute = getAttribute(name);
	attribute._enabled = true; attribute._vbo = buffer; attribute._size = size;
	attribute._type = type; attribute._normalized = normalized;
	attribute._stride = stride; attribute._pointer = offset;
}
void Shader::disableVertexAttribute(const char *name, const Math::Vector4d &value) {
	Attribute &attribute = getAttribute(name);
	attribute._enabled = false;
	attribute._constant[0] = value.x(); attribute._constant[1] = value.y();
	attribute._constant[2] = value.z(); attribute._constant[3] = value.w();
}
uint32 Shader::createBuffer(StateToken target, uint size, const void *data, StateToken usage) {
	Context &context = Context::current();
	uint32 buffer;
	context.metalGenBuffers(1, &buffer);
	context.metalBindBuffer(target, buffer);
	context.metalBufferData(target, size, data, usage);
	return buffer;
}
void Shader::freeBuffer(uint32 buffer) { Context::current().metalDeleteBuffers(1, &buffer); }

void Context::metalGenBuffers(uint count, uint32 *buffers) {
	for (uint i = 0; i < count; ++i) { buffers[i] = _nextBuffer++; _buffers[buffers[i]] = Common::Array<byte>(); }
}
void Context::metalDeleteBuffers(uint count, const uint32 *buffers) {
	for (uint i = 0; i < count; ++i) {
		_buffers.erase(buffers[i]);
		if (_arrayBuffer == buffers[i]) _arrayBuffer = 0;
		if (_elementBuffer == buffers[i]) _elementBuffer = 0;
	}
}
uint32 Context::boundBuffer(StateToken target) const { return target == kArrayBuffer ? _arrayBuffer : _elementBuffer; }
void Context::metalBindBuffer(StateToken target, uint32 buffer) {
	if (target == kArrayBuffer) _arrayBuffer = buffer;
	else if (target == kElementArrayBuffer) _elementBuffer = buffer;
	else error("Wintermute: invalid native buffer target");
}
void Context::metalBufferData(StateToken target, uint size, const void *data, StateToken usage) {
	Common::Array<byte> &buffer = _buffers[boundBuffer(target)];
	buffer.resize(size);
	if (size) { if (data) memcpy(buffer.data(), data, size); else memset(buffer.data(), 0, size); }
}
void Context::metalBufferSubData(StateToken target, uint offset, uint size, const void *data) {
	Common::Array<byte> &buffer = _buffers[boundBuffer(target)];
	if (offset > buffer.size() || size > buffer.size() - offset || (size && !data))
		error("Wintermute: native vertex buffer update out of bounds");
	if (size) memcpy(buffer.data() + offset, data, size);
}

void Context::metalGenTextures(uint count, uint32 *textures) {
	for (uint i = 0; i < count; ++i) { textures[i] = _nextImage++; _images[textures[i]] = Image(); }
}
void Context::metalDeleteTextures(uint count, const uint32 *textures) {
	for (uint i = 0; i < count; ++i) {
		if (_images.contains(textures[i])) { delete _images[textures[i]].texture; _images.erase(textures[i]); }
		for (uint slot = 0; slot < 2; ++slot) if (_boundImages[slot] == textures[i]) _boundImages[slot] = 0;
	}
}
void Context::metalActiveTexture(StateToken unit) {
	if (unit > 1) error("Wintermute: invalid native texture unit");
	_activeUnit = unit;
}
void Context::metalBindTexture(StateToken target, uint32 texture) { _boundImages[_activeUnit] = texture; }
Context::Image &Context::image() {
	if (!_boundImages[_activeUnit]) error("Wintermute: no native texture bound");
	return _images[_boundImages[_activeUnit]];
}
void Context::metalTexParameteri(StateToken target, StateToken parameter, StateToken value) {
	Image &entry = image();
	if (parameter == kTextureMagFilter || parameter == kTextureMinFilter) entry.sampling.linear = value != kNearest;
	if (parameter == kTextureMinFilter) {
		entry.sampling.mipmaps = value == kLinearMipmapLinear;
		entry.sampling.mipLinear = value == kLinearMipmapLinear;
	}
	else if (parameter == kTextureWrapS) entry.sampling.wrapS = value == kRepeat ? Graphics::Metal::kRepeat : Graphics::Metal::kClamp;
	else if (parameter == kTextureWrapT) entry.sampling.wrapT = value == kRepeat ? Graphics::Metal::kRepeat : Graphics::Metal::kClamp;
}
void Context::metalPixelStorei(StateToken parameter, int value) {
	if (parameter != kUnpackAlignment || (value != 1 && value != 2 && value != 4 && value != 8))
		error("Wintermute: unsupported native pixel alignment");
	_unpackAlignment = value;
}
Graphics::Surface Context::convertPixels(uint width, uint height, StateToken format, StateToken type, const void *pixels) const {
	Graphics::Surface result;
	result.create(width, height, Graphics::PixelFormat::createFormatRGBA32());
	uint bytes = type == kUnsignedShort565 ? 2 : format == kRgba ? 4 : format == kRgb ? 3 : format == kLuminanceAlpha ? 2 : 1;
	uint pitch = (width * bytes + _unpackAlignment - 1) & ~(_unpackAlignment - 1);
	for (uint y = 0; y < height; ++y) {
		byte *destination = (byte *)result.getBasePtr(0, y);
		for (uint x = 0; x < width; ++x) {
			byte r = 0, g = 0, b = 0, a = 0;
			if (pixels) {
				const byte *pixel = (const byte *)pixels + y * pitch + x * bytes;
				a = 255;
				if (type == kUnsignedShort565) {
					uint16 packed = READ_UINT16(pixel);
					r = ((packed >> 11) * 255 + 15) / 31;
					g = (((packed >> 5) & 63) * 255 + 31) / 63;
					b = ((packed & 31) * 255 + 15) / 31;
				} else if (format == kAlpha) { r = g = b = 255; a = pixel[0]; }
				else if (format == kLuminanceAlpha) { r = g = b = pixel[0]; a = pixel[1]; }
				else { r = pixel[0]; g = pixel[1]; b = pixel[2]; if (format == kRgba) a = pixel[3]; }
			}
			destination[x * 4] = r; destination[x * 4 + 1] = g;
			destination[x * 4 + 2] = b; destination[x * 4 + 3] = a;
		}
	}
	return result;
}
void Context::metalTexImage2D(StateToken target, int level, int internalFormat, int width, int height,
		int border, StateToken format, StateToken type, const void *data) {
	if (level || border || width <= 0 || height <= 0) error("Wintermute: invalid native texture dimensions");
	Image &entry = image();
	Graphics::Surface surface = convertPixels(width, height, format, type, data);
	Graphics::Metal::Texture *texture = _nativeRenderer->createTexture(surface);
	surface.free();
	if (!texture) error("Wintermute: native texture allocation failed");
	delete entry.texture; entry.texture = texture;
}
void Context::metalTexSubImage2D(StateToken target, int level, int x, int y, int width, int height,
		StateToken format, StateToken type, const void *data) {
	Image &entry = image();
	if (!entry.texture || level || x < 0 || y < 0 || width <= 0 || height <= 0 ||
		uint(x + width) > entry.texture->width() || uint(y + height) > entry.texture->height())
		error("Wintermute: native texture update out of bounds");
	Graphics::Surface patch = convertPixels(width, height, format, type, data);
	// updateTextureLevel interprets the source rect in the same coordinates as the destination.
	Graphics::Surface canvas;
	canvas.create(entry.texture->width(), entry.texture->height(), patch.format);
	for (int row = 0; row < height; ++row)
		memcpy(canvas.getBasePtr(x, y + row), patch.getBasePtr(0, row), width * 4);
	_nativeRenderer->updateTexture(*entry.texture, canvas, Common::Rect(x, y, x + width, y + height));
	canvas.free(); patch.free();
}
void Context::metalCopyTexImage2D(StateToken target, int level, int format, int x, int y, int width, int height, int border) {
	Image &entry = image();
	Common::Rect source = mapLogicalRect(Common::Rect32(x, _logicalHeight - y - height, x + width, _logicalHeight - y));
	if (!entry.texture || entry.texture->width() != uint(source.width()) || entry.texture->height() != uint(source.height()))
		metalTexImage2D(target, level, format, source.width(), source.height(), border, format, kUnsignedByte, nullptr);
	_nativeRenderer->copyTexture(*entry.texture, Common::Rect(source.width(), source.height()), *_nativeRenderer->screenTexture(), source, true);
}

void Context::metalEnable(StateToken state) {
	if (state == kDepthTest) _drawState.depthTest = true;
	else if (state == kBlend) _drawState.blend = true;
	else if (state == kCullFace) _drawState.cull = true;
	else if (state == kStencilTest) _drawState.stencil = true;
	else if (state == kPolygonOffsetFill) { _drawState.depthBias = 0; _drawState.slopeBias = 0; }
}
void Context::metalDisable(StateToken state) {
	if (state == kDepthTest) _drawState.depthTest = false;
	else if (state == kBlend) _drawState.blend = false;
	else if (state == kCullFace) _drawState.cull = false;
	else if (state == kStencilTest) _drawState.stencil = false;
	else if (state == kPolygonOffsetFill) { _drawState.depthBias = 0; _drawState.slopeBias = 0; }
}
void Context::metalBlendFunc(StateToken source, StateToken destination) {
	_drawState.srcRGB = _drawState.srcAlpha = source == kOne ? Graphics::Metal::kBlendOne : source == kZero ? Graphics::Metal::kBlendZero : Graphics::Metal::kSrcAlpha;
	_drawState.dstRGB = _drawState.dstAlpha = destination == kOne ? Graphics::Metal::kBlendOne : destination == kZero ? Graphics::Metal::kBlendZero :
		destination == kOneMinusSrcColor ? Graphics::Metal::kOneMinusSrcColor : Graphics::Metal::kOneMinusSrcAlpha;
}
void Context::metalDepthFunc(StateToken function) { _drawState.depthCompare = function == kLequal ? Graphics::Metal::kLessEqual : Graphics::Metal::kLess; }
void Context::metalDepthMask(bool enabled) { _drawState.depthWrite = enabled; }
void Context::metalFrontFace(StateToken winding) { _drawState.clockwise = winding == kCw; }
void Context::metalColorMask(bool r, bool g, bool b, bool a) { _drawState.colorMask = r | (g << 1) | (b << 2) | (a << 3); }
void Context::metalStencilFunc(StateToken function, int reference, uint mask) {
	_drawState.frontStencil.compare = _drawState.backStencil.compare = function == kEqual ? Graphics::Metal::kEqual : function == kLequal ? Graphics::Metal::kLessEqual : Graphics::Metal::kAlways;
	_drawState.frontStencil.readMask = _drawState.backStencil.readMask = mask;
	_drawState.stencilReference = reference;
}
void Context::metalStencilOp(StateToken fail, StateToken depthFail, StateToken pass) {
	auto operation = [](StateToken token) {
		return token == kReplace ? Graphics::Metal::kReplace : token == kIncr ? Graphics::Metal::kIncrementClamp :
			token == kDecr ? Graphics::Metal::kDecrementClamp : Graphics::Metal::kKeep;
	};
	_drawState.frontStencil.fail = _drawState.backStencil.fail = operation(fail);
	_drawState.frontStencil.depthFail = _drawState.backStencil.depthFail = operation(depthFail);
	_drawState.frontStencil.pass = _drawState.backStencil.pass = operation(pass);
}
void Context::metalClearStencil(int value) { _clearStencil = value; }
void Context::metalClear(uint mask) {
	_nativeRenderer->clearRect(Common::Rect(_nativeRenderer->width(), _nativeRenderer->height()), _clearColor[0], _clearColor[1], _clearColor[2], _clearColor[3],
		mask & kColorBufferBit, (mask & kDepthBufferBit) && _drawState.depthWrite,
		mask & kStencilBufferBit, _drawState.colorMask, 1, _clearStencil);
}
void Context::metalViewport(int x, int y, int width, int height) {
	_nativeRenderer->setViewport(mapLogicalRect(Common::Rect32(x, int(_logicalHeight) - y - height, x + width, int(_logicalHeight) - y)));
}

Common::Rect Context::mapLogicalRect(const Common::Rect32 &rect) const {
	if (!_logicalWidth || !_logicalHeight) error("Wintermute: native logical viewport unavailable");
	Graphics::Metal::Texture *screen = _nativeRenderer->screenTexture();
	return Common::Rect(int64(rect.left) * screen->width() / _logicalWidth,
		int64(rect.top) * screen->height() / _logicalHeight,
		int64(rect.right) * screen->width() / _logicalWidth,
		int64(rect.bottom) * screen->height() / _logicalHeight);
}

void Context::metalClearColor(float r, float g, float b, float a) {
	_clearColor[0] = r; _clearColor[1] = g; _clearColor[2] = b; _clearColor[3] = a;
}
void Context::metalCullFace(StateToken face) { _drawState.cullFront = face == kFront; }
void Context::metalGenerateMipmap(StateToken target) {
	if (image().texture) _nativeRenderer->generateMipmaps(*image().texture);
}
void Context::metalGetBooleanv(StateToken state, bool *value) const {
	if (state == kBlend) *value = _drawState.blend;
	else if (state == kCullFace) *value = _drawState.cull;
	else error("Wintermute: unsupported native boolean state");
}
void Context::metalGetIntegerv(StateToken state, int *value) const {
	if (state != kTextureBinding2D) error("Wintermute: unsupported native integer state");
	*value = _boundImages[_activeUnit];
}
void Context::metalUniform1i(int location, int value) {
	if (!_shader || location < 0 || uint(location) >= ARRAYSIZE(kUniformBindings)) error("Wintermute: invalid native integer uniform");
	_shader->setUniform(kUniformBindings[location].name, value);
}
void Context::metalReadPixels(int x, int y, int width, int height, StateToken format, StateToken type, void *pixels) {
	if (format != kRgba || type != kUnsignedByte) error("Wintermute: unsupported native readback format");
	Graphics::Surface surface;
	Common::Rect rect = mapLogicalRect(Common::Rect32(x, int(_logicalHeight) - y - height, x + width, int(_logicalHeight) - y));
	if (!_nativeRenderer->readTexture(*_nativeRenderer->screenTexture(), surface, rect))
		error("Wintermute: native readback failed");
	Graphics::Surface *scaled = surface.w == width && surface.h == height ? nullptr : surface.scale(width, height);
	const Graphics::Surface &source = scaled ? *scaled : surface;
	for (int row = 0; row < height; ++row) memcpy((byte *)pixels + row * width * 4, source.getBasePtr(0, height - row - 1), width * 4);
	if (scaled) { scaled->free(); delete scaled; }
	surface.free();
}
void Context::metalUniform4fv(int location, uint count, const float *data) {
	if (!_shader || location < 0 || uint(location) >= ARRAYSIZE(kUniformBindings)) error("Wintermute: invalid native uniform binding");
	const UniformBinding &binding = kUniformBindings[location];
	_shader->set(binding.name, data, count * 4);
}
void Context::metalEnableVertexAttribArray(uint location) {
	if (!_shader || location >= 4) error("Wintermute: invalid native attribute");
	_shader->_attributes[location]._enabled = true;
}
void Context::metalVertexAttribPointer(uint location, uint size, StateToken type, bool normalized, uint stride, const void *offset) {
	if (!_shader || location >= 4) error("Wintermute: invalid native attribute");
	Attribute &attribute = _shader->_attributes[location];
	attribute._vbo = _arrayBuffer; attribute._size = size; attribute._type = type;
	attribute._normalized = normalized; attribute._stride = stride; attribute._pointer = (uintptr)offset;
}

void Context::draw(StateToken primitive, const uint32 *indices, uint count) {
	if (!_shader || !count) return;
	uint maxIndex = 0;
	for (uint i = 0; i < count; ++i) maxIndex = MAX(maxIndex, indices[i]);
	Common::Array<PackedVertex> vertices;
	vertices.resize(maxIndex + 1);
	for (uint vertex = 0; vertex < vertices.size(); ++vertex) {
		float *attributes[] = {vertices[vertex].position, vertices[vertex].texcoord, vertices[vertex].color, vertices[vertex].normal};
		for (uint slot = 0; slot < 4; ++slot) {
			const Attribute &attribute = _shader->_attributes[slot];
			float *output = attributes[slot];
			memcpy(output, attribute._constant, 4 * sizeof(float));
			if (!attribute._enabled) continue;
			if (!_buffers.contains(attribute._vbo)) error("Wintermute: native vertex buffer unavailable");
			const Common::Array<byte> &buffer = _buffers[attribute._vbo];
			uint bytes = attribute._type == kFloat ? sizeof(float) : 1;
			uint stride = attribute._stride ? attribute._stride : bytes * attribute._size;
			uint64 offset = attribute._pointer + uint64(vertex) * stride;
			if (attribute._size > 4 || offset + bytes * attribute._size > buffer.size())
				error("Wintermute: native vertex attribute out of bounds");
			for (uint component = 0; component < attribute._size; ++component) {
				if (attribute._type == kFloat) memcpy(output + component, buffer.data() + offset + component * bytes, sizeof(float));
				else output[component] = buffer[offset + component] / (attribute._normalized ? 255.f : 1.f);
			}
		}
	}
	Common::Array<uint32> triangles;
	if (primitive == kTriangleFan) {
		for (uint i = 1; i + 1 < count; ++i) {
			triangles.push_back(indices[0]); triangles.push_back(indices[i]); triangles.push_back(indices[i + 1]);
		}
		indices = triangles.data(); count = triangles.size();
	}
	Graphics::Metal::DrawState state = _drawState;
	state.primitive = primitive == kLines ? Graphics::Metal::kLines : primitive == kTriangleStrip ? Graphics::Metal::kTriangleStrip : Graphics::Metal::kTriangles;
	Graphics::Metal::TextureBinding bindings[2];
	for (uint i = 0; i < 2; ++i) {
		if (_boundImages[i] && _images.contains(_boundImages[i])) {
			Image &entry = _images[_boundImages[i]];
			bindings[i] = entry.sampling;
			bindings[i].texture = entry.texture ? entry.texture : _white;
		} else bindings[i].texture = _white;
	}
	_shader->_uniforms.nativeParameters[0] = _nativeRenderer->height();
	_nativeRenderer->drawProgram(*_shader->_program, vertices.data(), vertices.size(), sizeof(PackedVertex), indices, count,
		&_shader->_uniforms, sizeof(ShaderUniforms), bindings, 2, state);
}
void Context::metalDrawArrays(StateToken primitive, uint first, uint count) {
	Common::Array<uint32> indices;
	indices.resize(count);
	for (uint i = 0; i < count; ++i) indices[i] = first + i;
	draw(primitive, indices.data(), count);
}
void Context::metalDrawElements(StateToken primitive, uint count, StateToken type, const void *pointer) {
	uint bytes = type == kUnsignedShort ? 2 : 4;
	const byte *data = (const byte *)pointer;
	if (_elementBuffer) {
		const Common::Array<byte> &buffer = _buffers[_elementBuffer];
		uintptr offset = (uintptr)pointer;
		if (offset > buffer.size() || uint64(count) * bytes > buffer.size() - offset)
			error("Wintermute: native index buffer out of bounds");
		data = buffer.data() + offset;
	}
	if (!data && count) error("Wintermute: native indices unavailable");
	Common::Array<uint32> indices;
	indices.resize(count);
	for (uint i = 0; i < count; ++i) indices[i] = bytes == 2 ? READ_UINT16(data + i * bytes) : READ_UINT32(data + i * bytes);
	draw(primitive, indices.data(), count);
}

void Context::metalCapture(uint32 texture, bool stored, Graphics::Surface &surface) {
	Graphics::Metal::Texture *image = stored && _images.contains(texture) ? _images[texture].texture : _nativeRenderer->screenTexture();
	if (!image || !_nativeRenderer->readTexture(*image, surface, Common::Rect(image->width(), image->height())))
		error("Wintermute: native framebuffer readback failed");
	if (stored) {
		Common::Array<byte> row;
		row.resize(surface.pitch);
		for (int y = 0; y < surface.h / 2; ++y) {
			byte *top = (byte *)surface.getBasePtr(0, y), *bottom = (byte *)surface.getBasePtr(0, surface.h - y - 1);
			memcpy(row.data(), top, surface.pitch); memcpy(top, bottom, surface.pitch); memcpy(bottom, row.data(), surface.pitch);
		}
	}
}
void Context::metalCaptureRect(int x, int y, int width, int height, byte *pixels) {
	Graphics::Surface surface;
	if (!_nativeRenderer->readPixels(surface, Common::Rect(x, y, x + width, y + height)))
		error("Wintermute: native specialty texture readback failed");
	for (int row = 0; row < height; ++row) memcpy(pixels + row * width * 4, surface.getBasePtr(0, row), width * 4);
	surface.free();
}

void Context::metalFilterTexture(uint32 texture, Shader &shader) {
	if (!_images.contains(texture) || !_images[texture].texture) return;
	Image &image = _images[texture];
	Graphics::Metal::Texture &source = *image.texture;
	Graphics::Metal::RenderTarget *target = _nativeRenderer->createRenderTarget(source.width(), source.height());
	if (!target) error("Wintermute: cannot allocate native filter target");
	Graphics::Metal::RenderTarget *previousTarget = _nativeRenderer->renderTarget();
	Common::Rect previousViewport = _nativeRenderer->viewport();
	Graphics::Metal::DrawState previousState = _drawState;
	Shader *previousShader = _shader;
	uint32 previousImage = _boundImages[0];
	_nativeRenderer->setRenderTarget(target);
	_nativeRenderer->setViewport(Common::Rect(source.width(), source.height()));
	_drawState = Graphics::Metal::DrawState();
	_boundImages[0] = texture;
	shader.use();
	metalDrawArrays(kTriangles, 0, 6);
	_nativeRenderer->setRenderTarget(previousTarget);
	_nativeRenderer->copyTexture(source, Common::Rect(source.width(), source.height()), target->texture(),
		Common::Rect(source.width(), source.height()), true);
	_nativeRenderer->setViewport(previousViewport);
	delete target;
	_drawState = previousState;
	_shader = previousShader;
	_boundImages[0] = previousImage;
}

} // End of namespace MetalBackend
} // End of namespace Wintermute
