/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/metal/legacy-rasterizer.h"
#include "common/textconsole.h"

namespace Graphics {
namespace Metal {

static void identity(float *m) {
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1;
}

LegacyRasterizer::LegacyRasterizer() : _background(nullptr), _clearDepth(1),
		_matrixMode(TGL_MODELVIEW), _primitive(TGL_TRIANGLES), _boundImage(0),
		_initialized(false), _textured(false), _alphaTest(false) {
	identity(_projection);
	identity(_modelView);
	memset(&_vertex, 0, sizeof(_vertex));
	color4f(1, 1, 1, 1);
	clearColor(0, 0, 0, 1);
	_state.depthWrite = true;
	_state.depthCompare = kLess;
	_state.linear = false;
	_state.srcAlpha = kSrcAlpha;
	_images.push_back(Image());
}

LegacyRasterizer::~LegacyRasterizer() {
	_renderer.flush(true);
	for (auto &image : _images)
		delete image.texture;
	delete _background;
	_surface.free();
}

bool LegacyRasterizer::init(int width, int height, const PixelFormat &format) {
	if (_initialized)
		return width == _surface.w && height == _surface.h && format == _surface.format;
	if (width <= 0 || height <= 0 || format.bytesPerPixel < 2 || !_renderer.attach(nullptr) || !_renderer.resize(width, height))
		return false;
	_surface.create(width, height, format);
	_surface.fillRect(Common::Rect(width, height), 0);
	_initialized = true;
	return true;
}

void LegacyRasterizer::copyBackground(const Surface &surface) {
	if (surface.w != _surface.w || surface.h != _surface.h || surface.format != _surface.format)
		error("Metal rasterizer: incompatible background surface");
	if (surface.getPixels() != _surface.getPixels())
		_surface.copyRectToSurface(surface.getPixels(), surface.pitch, 0, 0, surface.w, surface.h);
	uploadBackground();
}

void LegacyRasterizer::uploadBackground() {
	if (!_background)
		_background = _renderer.createTexture(_surface);
	else
		_renderer.updateTexture(*_background, _surface, Common::Rect(_surface.w, _surface.h));
	if (!_background)
		error("Metal rasterizer: background allocation failed");
	Vertex quad[4] = {{{-1, 1, 0}, {0, 0}, {1, 1, 1, 1}}, {{1, 1, 0}, {1, 0}, {1, 1, 1, 1}},
	                 {{-1, -1, 0}, {0, 1}, {1, 1, 1, 1}}, {{1, -1, 0}, {1, 1}, {1, 1, 1, 1}}};
	DrawState state;
	state.primitive = kTriangleStrip;
	state.linear = false;
	Common::Rect previous = _renderer.viewport();
	_renderer.setViewport(Common::Rect(_surface.w, _surface.h));
	_renderer.draw(quad, 4, _background, state, Uniforms());
	_renderer.setViewport(previous);
}

void LegacyRasterizer::presentBuffer() {
	Surface pixels;
	if (!_renderer.readPixels(pixels, Common::Rect(_surface.w, _surface.h)))
		error("Metal rasterizer: readback failed");
	Surface *converted = pixels.convertTo(_surface.format);
	if (!converted)
		error("Metal rasterizer: readback conversion failed");
	_surface.copyRectToSurface(converted->getPixels(), converted->pitch, 0, 0, _surface.w, _surface.h);
	converted->free();
	delete converted;
	pixels.free();
}

void LegacyRasterizer::readInto(Surface &surface) {
	presentBuffer();
	if (surface.w != _surface.w || surface.h != _surface.h || surface.format != _surface.format)
		error("Metal rasterizer: incompatible output surface");
	surface.copyRectToSurface(_surface.getPixels(), _surface.pitch, 0, 0, surface.w, surface.h);
}

void LegacyRasterizer::viewport(int x, int y, int width, int height) {
	_renderer.setViewport(Common::Rect(x, _surface.h - y - height, x + width, _surface.h - y));
}

void LegacyRasterizer::setEnabled(uint cap, bool enabled) {
	switch (cap) {
	case TGL_DEPTH_TEST: _state.depthTest = enabled; break;
	case TGL_CULL_FACE: _state.cull = enabled; break;
	case TGL_BLEND: _state.blend = enabled; break;
	case TGL_TEXTURE_2D: _textured = enabled; break;
	case TGL_ALPHA_TEST: _alphaTest = enabled; break;
	case TGL_LIGHTING:
		if (enabled) error("Metal rasterizer: lighting must be evaluated by the engine");
		break;
	default: error("Metal rasterizer: unsupported capability %x", cap);
	}
}

void LegacyRasterizer::enable(uint cap) { setEnabled(cap, true); }
void LegacyRasterizer::disable(uint cap) { setEnabled(cap, false); }
void LegacyRasterizer::clearColor(float r, float g, float b, float a) {
	_clearColor[0] = r; _clearColor[1] = g; _clearColor[2] = b; _clearColor[3] = a;
}
void LegacyRasterizer::clear(uint mask) {
	_renderer.clear(_clearColor[0], _clearColor[1], _clearColor[2], _clearColor[3],
	                mask & TGL_COLOR_BUFFER_BIT, mask & TGL_DEPTH_BUFFER_BIT, false, _clearDepth);
}
void LegacyRasterizer::depthFunc(uint func) {
	if (func < TGL_NEVER || func > TGL_ALWAYS) error("Metal rasterizer: invalid depth comparison");
	_state.depthCompare = Compare(func - TGL_NEVER);
}
void LegacyRasterizer::shadeModel(uint mode) {
	if (mode != TGL_SMOOTH) error("Metal rasterizer: unsupported shade model");
}
void LegacyRasterizer::alphaFunc(uint func, float reference) {
	if (func < TGL_NEVER || func > TGL_ALWAYS) error("Metal rasterizer: invalid alpha comparison");
	_uniforms.alphaCompare = Compare(func - TGL_NEVER);
	_uniforms.alphaReference = reference;
}
void LegacyRasterizer::blendFunc(uint source, uint destination) {
	if (source != TGL_SRC_ALPHA || destination != TGL_ONE_MINUS_SRC_ALPHA)
		error("Metal rasterizer: unsupported blend factors");
	_state.srcRGB = _state.srcAlpha = kSrcAlpha;
	_state.dstRGB = _state.dstAlpha = kOneMinusSrcAlpha;
}
float *LegacyRasterizer::matrix() { return _matrixMode == TGL_PROJECTION ? _projection : _modelView; }
void LegacyRasterizer::matrixMode(uint mode) {
	if (mode != TGL_PROJECTION && mode != TGL_MODELVIEW) error("Metal rasterizer: unsupported matrix mode");
	_matrixMode = mode;
}
void LegacyRasterizer::loadIdentity() { identity(matrix()); }
void LegacyRasterizer::multiply(const float *right) {
	float result[16], *left = matrix();
	for (uint column = 0; column < 4; ++column)
		for (uint row = 0; row < 4; ++row) {
			result[column * 4 + row] = 0;
			for (uint k = 0; k < 4; ++k)
				result[column * 4 + row] += left[k * 4 + row] * right[column * 4 + k];
		}
	memcpy(left, result, sizeof(result));
}
void LegacyRasterizer::orthof(float l, float r, float b, float t, float n, float f) {
	float m[16]; identity(m);
	m[0] = 2 / (r - l); m[5] = 2 / (t - b); m[10] = -2 / (f - n);
	m[12] = -(r + l) / (r - l); m[13] = -(t + b) / (t - b); m[14] = -(f + n) / (f - n);
	multiply(m);
}
void LegacyRasterizer::frustum(float l, float r, float b, float t, float n, float f) {
	float m[16] = {};
	m[0] = 2 * n / (r - l); m[5] = 2 * n / (t - b); m[8] = (r + l) / (r - l);
	m[9] = (t + b) / (t - b); m[10] = -(f + n) / (f - n); m[11] = -1; m[14] = -2 * f * n / (f - n);
	multiply(m);
}
void LegacyRasterizer::scalef(float x, float y, float z) {
	float m[16]; identity(m); m[0] = x; m[5] = y; m[10] = z; multiply(m);
}
void LegacyRasterizer::genTextures(int count, uint *ids) {
	for (int i = 0; i < count; ++i) {
		uint id = 1;
		while (id < _images.size() && _images[id].allocated) ++id;
		if (id == _images.size()) _images.push_back(Image());
		_images[id] = Image();
		_images[id].allocated = true;
		ids[i] = id;
	}
}
void LegacyRasterizer::deleteTextures(int count, const uint *ids) {
	for (int i = 0; i < count; ++i) {
		if (ids[i] >= _images.size()) error("Metal rasterizer: invalid texture deletion");
		delete _images[ids[i]].texture;
		_images[ids[i]] = Image();
		if (_boundImage == ids[i]) _boundImage = 0;
	}
}
void LegacyRasterizer::bindTexture(uint target, uint id) {
	if (target != TGL_TEXTURE_2D || id >= _images.size() || (id && !_images[id].allocated))
		error("Metal rasterizer: invalid texture binding");
	_boundImage = id;
}
void LegacyRasterizer::texParameteri(uint target, uint parameter, int value) {
	if (target != TGL_TEXTURE_2D || !_boundImage) error("Metal rasterizer: invalid texture parameter target");
	Image &image = _images[_boundImage];
	if (parameter == TGL_TEXTURE_MIN_FILTER || parameter == TGL_TEXTURE_MAG_FILTER) {
		if (value != TGL_NEAREST && value != TGL_LINEAR) error("Metal rasterizer: unsupported texture filter");
		image.linear = value == TGL_LINEAR;
	} else if (parameter == TGL_TEXTURE_WRAP_S || parameter == TGL_TEXTURE_WRAP_T) {
		if (value != TGL_REPEAT && value != TGL_CLAMP_TO_EDGE) error("Metal rasterizer: unsupported texture wrap");
		(parameter == TGL_TEXTURE_WRAP_S ? image.wrapS : image.wrapT) = value == TGL_REPEAT ? kRepeat : kClamp;
	} else error("Metal rasterizer: unsupported texture parameter");
}
void LegacyRasterizer::texEnvi(uint target, uint parameter, int value) {
	if (target != TGL_TEXTURE_ENV || parameter != TGL_TEXTURE_ENV_MODE || value != TGL_MODULATE)
		error("Metal rasterizer: unsupported texture environment");
}
void LegacyRasterizer::texImage2D(uint target, int level, int internalFormat, int width, int height,
		int border, uint format, uint type, const void *pixels) {
	if (target != TGL_TEXTURE_2D || level || border || !_boundImage || type != TGL_UNSIGNED_BYTE ||
			(format != TGL_RGB && format != TGL_RGBA) || internalFormat != (int)format || !pixels || width <= 0 || height <= 0)
		error("Metal rasterizer: unsupported texture upload");
	Surface source;
	source.create(width, height, PixelFormat::createFormatRGBA32());
	const byte *in = static_cast<const byte *>(pixels);
	for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
		byte *out = static_cast<byte *>(source.getBasePtr(x, y));
		out[0] = *in++; out[1] = *in++; out[2] = *in++; out[3] = format == TGL_RGBA ? *in++ : 255;
	}
	Image &image = _images[_boundImage];
	delete image.texture;
	image.texture = _renderer.createTexture(source);
	source.free();
	if (!image.texture) error("Metal rasterizer: texture allocation failed");
}
void LegacyRasterizer::begin(uint primitive) {
	if (primitive != TGL_TRIANGLES && primitive != TGL_QUADS && primitive != TGL_QUAD_STRIP)
		error("Metal rasterizer: unsupported primitive");
	_primitive = primitive; _vertices.clear();
}
void LegacyRasterizer::color4ub(byte r, byte g, byte b, byte a) { color4f(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }
void LegacyRasterizer::color4f(float r, float g, float b, float a) {
	_vertex.color[0] = r; _vertex.color[1] = g; _vertex.color[2] = b; _vertex.color[3] = a;
}
void LegacyRasterizer::texCoord2f(float u, float v) { _vertex.texcoord[0] = u; _vertex.texcoord[1] = v; }
void LegacyRasterizer::vertex3f(float x, float y, float z) {
	_vertex.position[0] = x; _vertex.position[1] = y; _vertex.position[2] = z; _vertices.push_back(_vertex);
}
void LegacyRasterizer::end() {
	if (_vertices.empty()) return;
	Uniforms uniforms = _uniforms;
	if (!_alphaTest) uniforms.alphaCompare = kAlways;
	for (uint column = 0; column < 4; ++column) for (uint row = 0; row < 4; ++row) {
		uniforms.transform[column * 4 + row] = 0;
		for (uint k = 0; k < 4; ++k)
			uniforms.transform[column * 4 + row] += _projection[k * 4 + row] * _modelView[column * 4 + k];
	}
	DrawState state = _state;
	state.depthWrite = state.depthTest && state.depthWrite;
	Texture *texture = nullptr;
	if (_textured && _boundImage) {
		const Image &image = _images[_boundImage]; texture = image.texture;
		state.linear = image.linear; state.wrapS = image.wrapS; state.wrapT = image.wrapT;
	}
	Common::Array<uint32> indices;
	if (_primitive == TGL_QUADS) {
		if (_vertices.size() % 4) error("Metal rasterizer: incomplete quad");
		for (uint i = 0; i < _vertices.size(); i += 4) {
			indices.push_back(i); indices.push_back(i + 1); indices.push_back(i + 2);
			indices.push_back(i); indices.push_back(i + 2); indices.push_back(i + 3);
		}
	} else if (_primitive == TGL_QUAD_STRIP) {
		if (_vertices.size() < 4 || _vertices.size() % 2) error("Metal rasterizer: incomplete quad strip");
		for (uint i = 0; i + 3 < _vertices.size(); i += 2) {
			indices.push_back(i); indices.push_back(i + 1); indices.push_back(i + 2);
			indices.push_back(i + 1); indices.push_back(i + 3); indices.push_back(i + 2);
		}
	} else if (_vertices.size() % 3) error("Metal rasterizer: incomplete triangle");
	state.primitive = kTriangles;
	if (indices.empty()) _renderer.draw(_vertices.data(), _vertices.size(), texture, state, uniforms);
	else _renderer.drawIndexed(_vertices.data(), _vertices.size(), indices.data(), indices.size(), texture, state, uniforms);
}

} // namespace Metal
} // namespace Graphics
