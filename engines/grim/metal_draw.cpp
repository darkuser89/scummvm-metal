/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engines/grim/metal_draw.h"
#include "engines/grim/metal_shader_sources.h"
#include "common/endian.h"
#include "graphics/metal/shader-source.h"
#include "common/textconsole.h"

namespace Grim {
namespace MetalBackend {

Context *Context::_current = nullptr;

Context::Context() {
	_current = this;
	_state.depthCompare = Graphics::Metal::kLess;
	_state.depthWrite = true;
	_state.srcAlpha = Graphics::Metal::kSrcAlpha;
}
Context::~Context() {
	invalidateGeometry();
	if (_renderer) _renderer->flush(true);
	for (auto &entry : _images) delete entry._value.texture;
	delete _white;
	if (_current == this) _current = nullptr;
}
Context &Context::current() {
	if (!_current) error("Grim: native draw context unavailable");
	return *_current;
}
void Context::attach(Graphics::Metal::Renderer &renderer) {
	_renderer = &renderer;
	Graphics::Surface white;
	white.create(1, 1, Graphics::PixelFormat::createFormatRGBA32());
	white.fillRect(Common::Rect(1, 1), white.format.ARGBToColor(255, 255, 255, 255));
	delete _white;
	_white = renderer.createTexture(white);
	white.free();
	if (!_white) error("Grim: cannot allocate native fallback texture");
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
		Common::String replacement = Graphics::Metal::shaderOverride("grim", name);
		msl += replacement.empty() ? source.source : replacement.c_str();
		Shader *shader = new Shader();
		shader->_program.reset(context._renderer->createProgram(msl.c_str(), source.vertexEntry, source.fragmentEntry));
		if (!shader->_program) error("Grim: cannot compile native shader %s", name);
		for (uint i = 0; i < 4; ++i) shader->_attributes[i]._idx = i;
		return shader;
	}
	error("Grim: native shader %s unavailable", name);
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
	if (count > binding.components * binding.count) error("Grim: invalid native uniform %s", name.c_str());
	memcpy((byte *)_uniforms.get() + binding.offset, values, count * sizeof(float));
	use();
	return true;
}
bool Shader::setUniform(const Common::String &name, const Math::Matrix4 &value) { return set(name, value.getData(), 16); }
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
	error("Grim: unknown native vertex attribute %s", name);
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
		invalidateGeometry(buffers[i]);
		_buffers.erase(buffers[i]);
		if (_arrayBuffer == buffers[i]) _arrayBuffer = 0;
		if (_elementBuffer == buffers[i]) _elementBuffer = 0;
	}
}
uint32 Context::boundBuffer(StateToken target) const { return target == kArrayBuffer ? _arrayBuffer : _elementBuffer; }
void Context::metalBindBuffer(StateToken target, uint32 buffer) {
	if (target == kArrayBuffer) _arrayBuffer = buffer;
	else if (target == kElementArrayBuffer) _elementBuffer = buffer;
	else error("Grim: invalid native buffer target");
}
void Context::metalBufferData(StateToken target, uint size, const void *data, StateToken usage) {
	invalidateGeometry(boundBuffer(target));
	Common::Array<byte> &buffer = _buffers[boundBuffer(target)];
	buffer.resize(size);
	if (size) { if (data) memcpy(buffer.data(), data, size); else memset(buffer.data(), 0, size); }
}
void Context::metalBufferSubData(StateToken target, uint offset, uint size, const void *data) {
	invalidateGeometry(boundBuffer(target));
	Common::Array<byte> &buffer = _buffers[boundBuffer(target)];
	if (offset > buffer.size() || size > buffer.size() - offset || (size && !data))
		error("Grim: native vertex buffer update out of bounds");
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
	if (unit > 1) error("Grim: invalid native texture unit");
	_activeUnit = unit;
}
void Context::metalBindTexture(StateToken target, uint32 texture) { _boundImages[_activeUnit] = texture; }
Context::Image &Context::image() {
	if (!_boundImages[_activeUnit]) error("Grim: no native texture bound");
	return _images[_boundImages[_activeUnit]];
}
void Context::metalTexParameteri(StateToken target, StateToken parameter, StateToken value) {
	Image &entry = image();
	if (parameter == kTextureMagFilter || parameter == kTextureMinFilter) entry.sampling.linear = value != kNearest;
	else if (parameter == kTextureWrapS) entry.sampling.wrapS = value == kRepeat ? Graphics::Metal::kRepeat : Graphics::Metal::kClamp;
	else if (parameter == kTextureWrapT) entry.sampling.wrapT = value == kRepeat ? Graphics::Metal::kRepeat : Graphics::Metal::kClamp;
}
void Context::metalPixelStorei(StateToken parameter, int value) {
	if (parameter != kUnpackAlignment || (value != 1 && value != 2 && value != 4 && value != 8))
		error("Grim: unsupported native pixel alignment");
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
	if (level || border || width <= 0 || height <= 0) error("Grim: invalid native texture dimensions");
	Image &entry = image();
	if (!data && entry.texture && entry.texture->width() == uint(width) && entry.texture->height() == uint(height)) return;
	Graphics::Metal::Texture *texture = nullptr;
	if (data) {
		Graphics::Surface surface = convertPixels(width, height, format, type, data);
		if (entry.texture && entry.texture->width() == uint(width) && entry.texture->height() == uint(height)) {
			_renderer->updateTexture(*entry.texture, surface, Common::Rect(width, height));
			surface.free(); return;
		}
		texture = _renderer->createTexture(surface); surface.free();
	} else texture = _renderer->allocateTexture(width, height);
	if (!texture) error("Grim: native texture allocation failed");
	delete entry.texture; entry.texture = texture;
}
void Context::metalTexSubImage2D(StateToken target, int level, int x, int y, int width, int height,
		StateToken format, StateToken type, const void *data) {
	Image &entry = image();
	if (!entry.texture || level || x < 0 || y < 0 || width <= 0 || height <= 0 ||
		uint(x + width) > entry.texture->width() || uint(y + height) > entry.texture->height())
		error("Grim: native texture update out of bounds");
	Graphics::Surface patch = convertPixels(width, height, format, type, data);
	_renderer->updateTexture(*entry.texture, patch, Common::Rect(width, height), Common::Point(x, y));
	patch.free();
}
void Context::metalCopyTexImage2D(StateToken target, int level, int format, int x, int y, int width, int height, int border) {
	Image &entry = image();
	if (!entry.texture || entry.texture->width() != uint(width) || entry.texture->height() != uint(height))
		metalTexImage2D(target, level, format, width, height, border, format, kUnsignedByte, nullptr);
	uint screenHeight = _renderer->screenTexture()->height();
	_renderer->copyTexture(*entry.texture, Common::Rect(width, height), *_renderer->screenTexture(),
		Common::Rect(x, screenHeight - y - height, x + width, screenHeight - y), true);
}

void Context::metalEnable(StateToken state) {
	if (state == kDepthTest) _state.depthTest = true;
	else if (state == kBlend) _state.blend = true;
	else if (state == kCullFace) _state.cull = true;
	else if (state == kStencilTest) _state.stencil = true;
	else if (state == kPolygonOffsetFill) { _state.depthBias = 0; _state.slopeBias = 0; }
}
void Context::metalDisable(StateToken state) {
	if (state == kDepthTest) _state.depthTest = false;
	else if (state == kBlend) _state.blend = false;
	else if (state == kCullFace) _state.cull = false;
	else if (state == kStencilTest) _state.stencil = false;
	else if (state == kPolygonOffsetFill) { _state.depthBias = 0; _state.slopeBias = 0; }
}
void Context::metalBlendFunc(StateToken source, StateToken destination) {
	_state.srcRGB = _state.srcAlpha = source == kOne ? Graphics::Metal::kBlendOne : Graphics::Metal::kSrcAlpha;
	_state.dstRGB = _state.dstAlpha = destination == kOne ? Graphics::Metal::kBlendOne : Graphics::Metal::kOneMinusSrcAlpha;
}
void Context::metalDepthFunc(StateToken function) { _state.depthCompare = function == kLequal ? Graphics::Metal::kLessEqual : Graphics::Metal::kLess; }
void Context::metalDepthMask(bool enabled) { _state.depthWrite = enabled; }
void Context::metalFrontFace(StateToken winding) { _state.clockwise = winding == kCw; }
void Context::metalColorMask(bool r, bool g, bool b, bool a) { _state.colorMask = r | (g << 1) | (b << 2) | (a << 3); }
void Context::metalStencilFunc(StateToken function, int reference, uint mask) {
	_state.frontStencil.compare = _state.backStencil.compare = function == kEqual ? Graphics::Metal::kEqual : Graphics::Metal::kAlways;
	_state.frontStencil.readMask = _state.backStencil.readMask = mask;
	_state.stencilReference = reference;
}
void Context::metalStencilOp(StateToken fail, StateToken depthFail, StateToken pass) {
	_state.frontStencil.fail = _state.backStencil.fail = fail == kReplace ? Graphics::Metal::kReplace : Graphics::Metal::kKeep;
	_state.frontStencil.depthFail = _state.backStencil.depthFail = depthFail == kReplace ? Graphics::Metal::kReplace : Graphics::Metal::kKeep;
	_state.frontStencil.pass = _state.backStencil.pass = pass == kReplace ? Graphics::Metal::kReplace : Graphics::Metal::kKeep;
}
void Context::metalClearStencil(int value) { _clearStencil = value; }
void Context::metalClear(uint mask) {
	_renderer->clearRect(Common::Rect(_renderer->width(), _renderer->height()), 0, 0, 0, 0,
		mask & kColorBufferBit, (mask & kDepthBufferBit) && _state.depthWrite,
		mask & kStencilBufferBit, _state.colorMask, 1, _clearStencil);
}
void Context::metalViewport(int x, int y, int width, int height) {
	uint h = _renderer->height();
	_renderer->setViewport(Common::Rect(x, h - y - height, x + width, h - y));
}
void Context::metalUniform4fv(int location, uint count, const float *data) {
	if (!_shader || location < 0 || uint(location) >= ARRAYSIZE(kUniformBindings)) error("Grim: invalid native uniform binding");
	const UniformBinding &binding = kUniformBindings[location];
	_shader->set(binding.name, data, count * 4);
}
void Context::metalEnableVertexAttribArray(uint location) {
	if (!_shader || location >= 4) error("Grim: invalid native attribute");
	_shader->_attributes[location]._enabled = true;
}
void Context::metalVertexAttribPointer(uint location, uint size, StateToken type, bool normalized, uint stride, const void *offset) {
	if (!_shader || location >= 4) error("Grim: invalid native attribute");
	Attribute &attribute = _shader->_attributes[location];
	attribute._vbo = _arrayBuffer; attribute._size = size; attribute._type = type;
	attribute._normalized = normalized; attribute._stride = stride; attribute._pointer = (uintptr)offset;
}

void Context::invalidateGeometry(uint32 buffer) {
	if (!buffer) {
		for (auto &entry : _indexBuffers) delete entry._value;
		_indexBuffers.clear();
	} else if (_indexBuffers.contains(buffer)) {
		delete _indexBuffers[buffer]; _indexBuffers.erase(buffer);
	}

	for (uint i = 0; i < _geometry.size();) {
		bool matches = !buffer;
		for (const Attribute &attribute : _geometry[i]->attributes)
			matches |= attribute._enabled && attribute._vbo == buffer;
		if (matches) { delete _geometry[i]; _geometry.remove_at(i); }
		else ++i;
	}
}

Context::Geometry *Context::geometry(uint minimumCount) {
	uint vertexCount = ~0U;
	bool enabled = false;
	for (const Attribute &attribute : _shader->_attributes) {
		if (!attribute._enabled) continue;
		enabled = true;
		if (!_buffers.contains(attribute._vbo)) error("Grim: native vertex buffer unavailable");
		const Common::Array<byte> &data = _buffers[attribute._vbo];
		uint bytes = attribute._type == kFloat ? 4 : 1;
		uint components = attribute._size * bytes;
		uint stride = attribute._stride ? attribute._stride : components;
		if (!attribute._size || attribute._size > 4 || attribute._pointer > data.size() || components > data.size() - attribute._pointer)
			error("Grim: native vertex attribute out of bounds");
		vertexCount = MIN(vertexCount, uint((data.size() - attribute._pointer - components) / stride + 1));
	}
	if (!enabled) vertexCount = minimumCount;
	if (minimumCount > vertexCount) error("Grim: native mesh index out of bounds");
	for (Geometry *entry : _geometry) {
		bool equal = entry->count == vertexCount;
		for (uint slot = 0; equal && slot < 4; ++slot) {
			const Attribute &a = entry->attributes[slot], &b = _shader->_attributes[slot];
			equal = a._enabled == b._enabled && a._vbo == b._vbo && a._size == b._size &&
				a._stride == b._stride && a._type == b._type && a._normalized == b._normalized &&
				a._pointer == b._pointer && !memcmp(a._constant, b._constant, sizeof(a._constant));
		}
		if (equal) return entry;
	}
	Common::Array<PackedVertex> vertices;
	vertices.resize(vertexCount);
	for (uint vertex = 0; vertex < vertices.size(); ++vertex) {
		float *attributes[] = {vertices[vertex].position, vertices[vertex].texcoord, vertices[vertex].color, vertices[vertex].normal};
		for (uint slot = 0; slot < 4; ++slot) {
			const Attribute &attribute = _shader->_attributes[slot];
			float *output = attributes[slot];
			memcpy(output, attribute._constant, 4 * sizeof(float));
			if (!attribute._enabled) continue;
			if (!_buffers.contains(attribute._vbo)) error("Grim: native vertex buffer unavailable");
			const Common::Array<byte> &buffer = _buffers[attribute._vbo];
			uint bytes = attribute._type == kFloat ? sizeof(float) : 1;
			uint stride = attribute._stride ? attribute._stride : bytes * attribute._size;
			uint64 offset = attribute._pointer + uint64(vertex) * stride;
			if (attribute._size > 4 || offset + bytes * attribute._size > buffer.size())
				error("Grim: native vertex attribute out of bounds");
			for (uint component = 0; component < attribute._size; ++component) {
				if (attribute._type == kFloat) memcpy(output + component, buffer.data() + offset + component * bytes, sizeof(float));
				else output[component] = buffer[offset + component] / (attribute._normalized ? 255.f : 1.f);
			}
		}
	}

	Geometry *entry = new Geometry;
	for (uint slot = 0; slot < 4; ++slot) entry->attributes[slot] = _shader->_attributes[slot];
	entry->count = vertexCount;
	entry->buffer = _renderer->createBuffer(vertices.data(), vertices.size() * sizeof(PackedVertex));
	if (!entry->buffer) { delete entry; error("Grim: could not allocate persistent geometry"); }
	// Bound uncommon attribute-layout combinations, retaining in-flight data
	// through the renderer even when a cache entry is evicted or invalidated.
	if (_geometry.size() >= 128) { delete _geometry[0]; _geometry.remove_at(0); }
	_geometry.push_back(entry);
	return entry;
}

void Context::draw(StateToken primitive, const uint32 *indices, uint count, Graphics::Metal::Buffer *indexBuffer, uint indexOffset, bool shortIndices, uint firstVertex) {
	if (!_shader || !count) return;
	uint maxIndex = firstVertex + count - 1;
	if (indices) {
		maxIndex = 0;
		for (uint i = 0; i < count; ++i) maxIndex = MAX(maxIndex, indices[i]);
	}
	Geometry *mesh = geometry(maxIndex + 1);
	Common::Array<uint32> triangles;
	if (primitive == kTriangleFan) {
		for (uint i = 1; i + 1 < count; ++i) {
			triangles.push_back(indices[0]); triangles.push_back(indices[i]); triangles.push_back(indices[i + 1]);
		}
		indices = triangles.data(); count = triangles.size();
	}
	Graphics::Metal::DrawState state = _state;
	state.primitive = primitive == kLines ? Graphics::Metal::kLines : primitive == kTriangleStrip ? Graphics::Metal::kTriangleStrip : Graphics::Metal::kTriangles;
	Graphics::Metal::TextureBinding bindings[2];
	for (uint i = 0; i < 2; ++i) {
		if (_boundImages[i] && _images.contains(_boundImages[i])) {
			Image &entry = _images[_boundImages[i]];
			bindings[i] = entry.sampling;
			bindings[i].texture = entry.texture ? entry.texture : _white;
		} else bindings[i].texture = _white;
	}
	_shader->_uniforms->nativeParameters[0] = _renderer->height();
	Graphics::Metal::GeometryBinding binding; binding.vertices = mesh->buffer;
	binding.indices = primitive == kTriangleFan ? nullptr : indexBuffer;
	binding.indexOffset = indexOffset; binding.indices16 = shortIndices;
	if (!indices) binding.vertexOffset = firstVertex * sizeof(PackedVertex);
	_renderer->drawProgram(*_shader->_program, nullptr, indices ? mesh->count : count, sizeof(PackedVertex), indices, indices ? count : 0,
		_shader->_uniforms.get(), sizeof(ShaderUniforms), bindings, 2, state, nullptr, 0, &binding);
}
void Context::metalDrawArrays(StateToken primitive, uint first, uint count) {
	if (!count) return;
	if (uint64(first) + count > ~0U) error("Metal: array draw range overflow");
	if (primitive != kTriangleFan) { draw(primitive, nullptr, count, nullptr, 0, false, first); return; }

	Common::Array<uint32> indices;
	indices.resize(count);
	for (uint i = 0; i < count; ++i) indices[i] = first + i;
	draw(primitive, indices.data(), count);
}
void Context::metalDrawElements(StateToken primitive, uint count, StateToken type, const void *pointer) {
	uint bytes = type == kUnsignedShort ? 2 : 4;
	const byte *data = (const byte *)pointer;
	Graphics::Metal::Buffer *nativeIndices = nullptr;
	if (_elementBuffer) {
		const Common::Array<byte> &buffer = _buffers[_elementBuffer];
		uintptr offset = (uintptr)pointer;
		if (offset > buffer.size() || uint64(count) * bytes > buffer.size() - offset)
			error("Grim: native index buffer out of bounds");
		data = buffer.data() + offset;
		if (count && primitive != kTriangleFan) {
			if (!_indexBuffers.contains(_elementBuffer)) {
				_indexBuffers[_elementBuffer] = _renderer->createBuffer(buffer.data(), buffer.size());
				if (!_indexBuffers[_elementBuffer]) error("Metal: could not allocate persistent element buffer");
			}
			nativeIndices = _indexBuffers[_elementBuffer];
		}
	}
	if (!data && count) error("Grim: native indices unavailable");
	Common::Array<uint32> indices;
	indices.resize(count);
	for (uint i = 0; i < count; ++i) indices[i] = bytes == 2 ? READ_UINT16(data + i * bytes) : READ_UINT32(data + i * bytes);
	draw(primitive, indices.data(), count, nativeIndices, _elementBuffer ? (uintptr)pointer : 0, bytes == 2);
}

void Context::metalCapture(uint32 texture, bool stored, Graphics::Surface &surface) {
	Graphics::Metal::Texture *image = stored && _images.contains(texture) ? _images[texture].texture : nullptr;
	// A save requested outside the game's menu may precede storeDisplay().
	// Only populated stored textures have the copied OpenGL row orientation.
	bool fromStored = image != nullptr;
	if (!image)
		image = _renderer->screenTexture();
	if (!image || !_renderer->readTexture(*image, surface, Common::Rect(image->width(), image->height())))
		error("Grim: native framebuffer readback failed");
	if (fromStored) {
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
	if (!_renderer->readPixels(surface, Common::Rect(x, y, x + width, y + height)))
		error("Grim: native specialty texture readback failed");
	for (int row = 0; row < height; ++row) memcpy(pixels + row * width * 4, surface.getBasePtr(0, row), width * 4);
	surface.free();
}

void Context::metalFilterTexture(uint32 texture, Shader &shader) {
	if (!_images.contains(texture) || !_images[texture].texture) return;
	Image &image = _images[texture];
	Graphics::Metal::Texture &source = *image.texture;
	Graphics::Metal::RenderTarget *target = _renderer->createRenderTarget(source.width(), source.height());
	if (!target) error("Grim: cannot allocate native filter target");
	Graphics::Metal::RenderTarget *previousTarget = _renderer->renderTarget();
	Common::Rect previousViewport = _renderer->viewport();
	Graphics::Metal::DrawState previousState = _state;
	Shader *previousShader = _shader;
	uint32 previousImage = _boundImages[0];
	_renderer->setRenderTarget(target);
	_renderer->setViewport(Common::Rect(source.width(), source.height()));
	_state = Graphics::Metal::DrawState();
	_boundImages[0] = texture;
	shader.use();
	metalDrawArrays(kTriangles, 0, 6);
	_renderer->setRenderTarget(previousTarget);
	_renderer->copyTexture(source, Common::Rect(source.width(), source.height()), target->texture(),
		Common::Rect(source.width(), source.height()), true);
	_renderer->setViewport(previousViewport);
	delete target;
	_state = previousState;
	_shader = previousShader;
	_boundImages[0] = previousImage;
}

} // End of namespace MetalBackend
} // End of namespace Grim
