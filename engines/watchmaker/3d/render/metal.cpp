/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "watchmaker/3d/render/metal.h"
#include "graphics/metal/texture-decoder.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "math/glmath.h"

namespace Watchmaker {

namespace Metal = Graphics::Metal;

bool usingMetal() { return g_system->getMetalRenderer() != nullptr; }

static Matrix4x4 multiply(const Matrix4x4 &left, const Matrix4x4 &right) {
	Matrix4x4 result;
	for (uint row = 1; row <= 4; ++row)
		for (uint column = 1; column <= 4; ++column)
			for (uint k = 1; k <= 4; ++k)
				result.data[(column - 1) * 4 + row - 1] += left.getValue(row, k) * right.getValue(k, column);
	return result;
}

class MetalTexture : public Texture {
public:
	~MetalTexture() override {
		if (g_renderer) g_renderer->releaseMetalTexture(_texture);
		delete _texture;
	}
	void assignData(const TextureData &data) override {
		Graphics::Surface surface;
		uint width = data.getWidth(), height = data.getHeight();
		if (data._compression == DxtCompression::UNCOMPRESSED) {
			if (!width || !height || (uint64)width * height * 4 > (uint)data.getDataSize())
				::error("Watchmaker: invalid RGBA texture");
			surface.create(width, height, Graphics::PixelFormat::createFormatRGBA32());
			for (uint y = 0; y < height; ++y)
				memcpy(surface.getBasePtr(0, y), (const byte *)data.getData() + y * width * 4, width * 4);
		} else {
			Metal::S3TCFormat format;
			switch (data._compression) {
			case DxtCompression::DXT1: format = Metal::kDXT1; break;
			case DxtCompression::DXT2: format = Metal::kDXT2; break;
			case DxtCompression::DXT3: format = Metal::kDXT3; break;
			case DxtCompression::DXT4: format = Metal::kDXT4; break;
			case DxtCompression::DXT5: format = Metal::kDXT5; break;
			default: ::error("Watchmaker: unsupported DDS compression");
			}
			if (!Metal::decodeS3TC(data.getData(), data.getDataSize(), width, height, format, surface))
				::error("Watchmaker: invalid compressed texture");
		}
		if (g_renderer) g_renderer->releaseMetalTexture(_texture);
		delete _texture;
		_texture = g_system->getMetalRenderer()->createTexture(surface);
		surface.free();
		if (!_texture) ::error("Watchmaker: could not upload Metal texture");
	}
	void bind() override {
		if (!g_renderer) ::error("Watchmaker: native renderer unavailable");
		g_renderer->bindMetalTexture(_texture);
	}
	Metal::Texture *metalTexture() const override { return _texture; }

private:
	Metal::Texture *_texture = nullptr;
};

Texture *createMetalTexture() { return new MetalTexture(); }

void OpenGLRenderer::initMetal(float ratio) {
	_metalProjection.setIdentity();
	Math::Matrix4 perspective = Math::makePerspectiveMatrix(60, ratio, 1, 1024);
	memcpy(_metalModelView.data, perspective.getData(), sizeof(_metalModelView.data));
	_metalState.linear = false;
	_metalState.depthWrite = true;
	_metalState.srcAlpha = Metal::kSrcAlpha;
	_metalState.dstAlpha = Metal::kOneMinusSrcAlpha;
	g_system->getMetalRenderer()->setViewport(Common::Rect(g_system->getWidth(), g_system->getHeight()));
}

void OpenGLRenderer::setMetalProjection(const float *matrix) {
	memcpy(_metalProjection.data, matrix, sizeof(_metalProjection.data));
}

void OpenGLRenderer::setMetalTransform(TransformMatrix which, const Matrix4x4 &matrix) {
	if (which == TransformMatrix::PROJECTION) {
		_metalProjection = matrix;
		return;
	}
	_metalModelView = matrix;
	// The legacy view setter multiplies by a Z reflection after loading.
	for (uint row = 0; row < 4; ++row) _metalModelView.data[8 + row] *= -1;
}

void OpenGLRenderer::pushMetalModelView() { _metalModelStack.push_back(_metalModelView); }
void OpenGLRenderer::popMetalModelView() {
	if (_metalModelStack.empty()) ::error("Watchmaker: native matrix stack underflow");
	_metalModelView = _metalModelStack[_metalModelStack.size() - 1];
	_metalModelStack.pop_back();
}

void OpenGLRenderer::enterMetal2D() {
	_metalProjectionStack.push_back(_metalProjection);
	pushMetalModelView();
	_metalModelView = Matrix4x4(); _metalModelView.setIdentity();
	_metalProjection = Matrix4x4(); _metalProjection.setIdentity();
}
void OpenGLRenderer::exitMetal2D() {
	popMetalModelView();
	if (_metalProjectionStack.empty()) ::error("Watchmaker: native projection stack underflow");
	_metalProjection = _metalProjectionStack[_metalProjectionStack.size() - 1];
	_metalProjectionStack.pop_back();
}

void OpenGLRenderer::setMetalState(RenderState state, int value) {
	switch (state) {
	case RenderState::ZENABLE: _metalState.depthTest = value; break;
	case RenderState::ZWRITE_ENABLE: _metalState.depthWrite = value; break;
	case RenderState::ALPHABLEND: _metalState.blend = value; break;
	case RenderState::ALPHAREF: _metalState.blend = value; break; // Original falls through to ALPHABLEND.
	default: break;
	}
}

static Metal::BlendFactor blendFactor(BlendFactor factor) {
	switch (factor) {
	case BlendFactor::ONE: return Metal::kBlendOne;
	case BlendFactor::ZERO: return Metal::kBlendZero;
	case BlendFactor::SRCALPHA: return Metal::kSrcAlpha;
	case BlendFactor::INVSRCALPHA: return Metal::kOneMinusSrcAlpha;
	case BlendFactor::INVSRCCOLOR: return Metal::kOneMinusSrcColor;
	case BlendFactor::SRCCOLOR: return Metal::kSrcColor;
	case BlendFactor::DESTCOLOR: return Metal::kDstColor;
	case BlendFactor::INVDESTCOLOR: return Metal::kOneMinusDstColor;
	default: ::error("Watchmaker: invalid blend factor");
	}
}
void OpenGLRenderer::setMetalBlend(BlendFactor src, BlendFactor dst) {
	_metalState.srcRGB = _metalState.srcAlpha = blendFactor(src);
	_metalState.dstRGB = _metalState.dstAlpha = blendFactor(dst);
}
void OpenGLRenderer::setMetalWrap(TextureWrapMode mode) {
	_metalState.wrapS = _metalState.wrapT = mode == TextureWrapMode::WRAP ? Metal::kRepeat : Metal::kBorder;
}

void OpenGLRenderer::drawMetalBatch(PrimitiveType primitive, const VertexBuffer &buffer, const uint16 *faces, uint count) {
	if (!faces || !count || buffer._buffer.empty()) return;
	Common::Array<Metal::Vertex> vertices;
	vertices.resize(buffer._buffer.size());
	for (uint i = 0; i < vertices.size(); ++i) {
		const gVertex &source = buffer._buffer[i];
		Metal::Vertex &v = vertices[i];
		v.position[0] = source.x; v.position[1] = source.y; v.position[2] = -source.z;
		v.texcoord[0] = source.u1; v.texcoord[1] = source.v1;
		for (float &channel : v.color) channel = 1;
	}
	Common::Array<uint32> indices;
	indices.resize(count);
	for (uint i = 0; i < count; ++i) {
		if (faces[i] >= vertices.size()) ::error("Watchmaker: native mesh index outside vertex buffer");
		indices[i] = faces[i];
	}
	Matrix4x4 projection;
	Math::Matrix4 perspective = Math::makePerspectiveMatrix(60, 1024.0 / 768.0, 1, 15000);
	memcpy(projection.data, perspective.getData(), sizeof(projection.data));
	Matrix4x4 transform = multiply(projection, _metalModelView);
	Metal::Uniforms uniforms;
	memcpy(uniforms.transform, transform.data, sizeof(uniforms.transform));
	uniforms.textured = _metalTexture != nullptr;
	Metal::DrawState state = _metalState;
	state.blend = true;
	state.srcRGB = state.srcAlpha = Metal::kSrcAlpha;
	state.dstRGB = state.dstAlpha = Metal::kOneMinusSrcAlpha;
	state.primitive = primitive == PrimitiveType::LINE ? Metal::kLines : Metal::kTriangles;
	g_system->getMetalRenderer()->drawIndexed(vertices.data(), vertices.size(), indices.data(), indices.size(), _metalTexture, state, uniforms);
}

void OpenGLRenderer::drawMetalVertices(PrimitiveType primitive, Vertex *source, int count) {
	if (!source || count <= 0) return;
	Common::Array<Metal::Vertex> vertices;
	vertices.resize(count);
	for (int i = 0; i < count; ++i) {
		Metal::Vertex &v = vertices[i];
		v.position[0] = source[i].sx; v.position[1] = source[i].sy; v.position[2] = -source[i].sz;
		v.texcoord[0] = v.texcoord[1] = 0;
		for (float &channel : v.color) channel = 1;
	}
	Matrix4x4 transform = multiply(_metalProjection, _metalModelView);
	Metal::Uniforms uniforms;
	memcpy(uniforms.transform, transform.data, sizeof(uniforms.transform));
	uniforms.textured = _metalTexture != nullptr;
	Metal::DrawState state = _metalState;
	state.primitive = primitive == PrimitiveType::LINE ? Metal::kLines : Metal::kTriangles;
	g_system->getMetalRenderer()->draw(vertices.data(), vertices.size(), _metalTexture, state, uniforms);
}

void renderMetalTexture(const gTexture &texture, const Common::Rect &source,
		const Common::Rect &destination, uint width, uint height) {
	if (!texture._texture || !texture._texture->metalTexture() || !width || !height) return;
	float left = 2.0f * destination.left / width - 1, right = 2.0f * destination.right / width - 1;
	float top = 1 - 2.0f * destination.top / height, bottom = 1 - 2.0f * destination.bottom / height;
	float u0 = (float)source.left / texture.RealDimX, u1 = (float)source.right / texture.RealDimX;
	float v0 = (float)source.top / texture.RealDimY, v1 = (float)source.bottom / texture.RealDimY;
	Metal::Vertex vertices[] = {
		{{left, bottom, 0}, {u0, v1}, {1, 1, 1, 1}}, {{right, bottom, 0}, {u1, v1}, {1, 1, 1, 1}},
		{{right, top, 0}, {u1, v0}, {1, 1, 1, 1}}, {{left, top, 0}, {u0, v0}, {1, 1, 1, 1}}
	};
	const uint32 indices[] = {0, 1, 2, 0, 2, 3};
	Metal::DrawState state;
	state.blend = true; state.linear = false;
	state.srcAlpha = Metal::kSrcAlpha;
	Metal::Uniforms uniforms;
	uniforms.textured = 1;
	g_system->getMetalRenderer()->drawIndexed(vertices, 4, indices, 6, texture._texture->metalTexture(), state, uniforms);
}

} // End of namespace Watchmaker
