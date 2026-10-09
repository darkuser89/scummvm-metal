/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "alcachofa/alcachofa.h"
#include "alcachofa/graphics-opengl-base.h"
#include "common/system.h"
#include "engines/util.h"
#include "graphics/metal/renderer.h"

namespace Alcachofa {

namespace Metal = Graphics::Metal;

class MetalTexture : public ITexture {
public:
	MetalTexture(int32 w, int32 h, bool mipmaps, Metal::Renderer &renderer)
		: ITexture(Common::Point(w, h)), _renderer(renderer), _mipmaps(mipmaps) {
		Graphics::Surface surface;
		surface.create(w, h, Graphics::PixelFormat::createFormatRGBA32());
		surface.fillRect(Common::Rect(w, h), 0);
		_texture = renderer.createTexture(surface, mipmaps);
		surface.free();
		if (!_texture)
			error("Alcachofa: could not create Metal texture");
	}
	~MetalTexture() override { delete _texture; }
	void update(const Graphics::Surface &surface) override {
		assert(surface.w == size().x && surface.h == size().y);
		_renderer.updateTexture(*_texture, surface, Common::Rect(surface.w, surface.h));
		if (_mipmaps)
			_renderer.generateMipmaps(*_texture);
	}
	Metal::Texture *texture() const { return _texture; }

private:
	Metal::Renderer &_renderer;
	Metal::Texture *_texture = nullptr;
	bool _mipmaps;
};

class MetalRenderer : public OpenGLRendererBase, public virtual IDebugRenderer {
public:
	explicit MetalRenderer(Common::Point resolution) : OpenGLRendererBase(resolution) {
		initGraphics3d(resolution.x, resolution.y, true);
		_renderer = g_system->getMetalRenderer();
		if (!_renderer)
			error("Alcachofa: Metal renderer unavailable");
	}
	~MetalRenderer() override {
		_renderer->setRenderTarget(nullptr);
		_renderer->flush(true);
		delete _output;
	}
	Common::ScopedPtr<ITexture> createTexture(int32 w, int32 h, bool mipmaps) override {
		return Common::ScopedPtr<ITexture>(new MetalTexture(w, h, mipmaps, *_renderer));
	}
	Graphics::PixelFormat getPixelFormat() const override { return Graphics::PixelFormat::createFormatRGBA32(); }
	bool requiresPoTTextures() const override { return false; }
	void begin() override {
		_renderer->setRenderTarget(nullptr);
		resetState();
		_texture = nullptr;
		_currentLodBias = 0;
		setBlendMode(BlendMode::AdditiveAlpha);
	}
	void setTexture(ITexture *texture) override {
		_texture = dynamic_cast<MetalTexture *>(texture);
		assert(!texture || _texture);
	}
	void setBlendMode(BlendMode mode) override {
		_currentBlendMode = mode;
		_state.blend = true;
		_state.srcRGB = _state.srcAlpha = Metal::kBlendOne;
		_state.dstRGB = _state.dstAlpha = Metal::kOneMinusSrcAlpha;
		_uniforms.textureMode = 1;
		switch (mode) {
		case BlendMode::AdditiveAlpha: break;
		case BlendMode::Additive:
			_state.dstRGB = _state.dstAlpha = Metal::kBlendOne;
			break;
		case BlendMode::Multiply:
			_state.srcRGB = _state.srcAlpha = Metal::kDstColor;
			_state.dstRGB = _state.dstAlpha = Metal::kBlendOne;
			break;
		case BlendMode::Alpha:
			_state.srcRGB = _state.srcAlpha = Metal::kSrcAlpha;
			_uniforms.textureMode = 2;
			break;
		case BlendMode::Tinted:
			_uniforms.textureMode = 3;
			break;
		}
	}
	void setLodBias(float bias) override { _currentLodBias = bias; }
	void setOutput(Graphics::Surface &surface) override {
		assert(_isFirstDrawCommand);
		assert(surface.format == getPixelFormat());
		_renderer->setRenderTarget(nullptr);
		if (!_output || _output->texture().width() != (uint)surface.w || _output->texture().height() != (uint)surface.h) {
			_renderer->flush(true);
			delete _output;
			_output = _renderer->createRenderTarget(surface.w, surface.h);
			if (!_output)
				error("Alcachofa: could not allocate Metal output target");
		}
		_currentOutput = &surface;
		_outputSize = Common::Point(surface.w, surface.h);
		_renderer->setRenderTarget(_output);
		_renderer->setViewport(Common::Rect(surface.w, surface.h));
		_viewport = Common::Rect(surface.w, surface.h);
		setMatrices(true);
	}
	void end() override {
		firstDraw();
		if (_currentOutput) {
			Graphics::Surface pixels;
			if (_renderer->readPixels(pixels, Common::Rect(_outputSize.x, _outputSize.y))) {
				for (int y = 0; y < pixels.h; ++y)
					memcpy(_currentOutput->getBasePtr(0, y), pixels.getBasePtr(0, y), pixels.w * 4);
				pixels.free();
			}
		} else {
			_renderer->flush();
		}
		_renderer->setRenderTarget(nullptr);
	}
	void quad(Math::Vector2d topLeft, Math::Vector2d size, Color color, Math::Angle rotation,
	          Math::Vector2d texMin, Math::Vector2d texMax) override {
		firstDraw();
		Math::Vector2d positions[4], uv[4];
		getQuadPositions(topLeft, size, rotation, positions);
		getQuadTexCoords(texMin, texMax, uv);
		Metal::Vertex vertices[4];
		for (uint i = 0; i < 4; ++i) {
			vertices[i] = vertex(positions[i], color);
			vertices[i].texcoord[0] = uv[i].getX();
			vertices[i].texcoord[1] = uv[i].getY();
		}
		bool mirror = texMin != Math::Vector2d() || texMax != Math::Vector2d(1, 1);
		_state.wrapS = _state.wrapT = mirror ? Metal::kMirror : Metal::kClamp;
		_state.linear = g_engine->config().texFilter();
		_uniforms.lodBias = _currentLodBias;
		_uniforms.posterize = g_engine->config().bits32() ? 0 : 1;
		const uint32 indices[] = {0, 1, 2, 0, 2, 3};
		_renderer->drawIndexed(vertices, 4, indices, 6, _texture ? _texture->texture() : nullptr, _state, _uniforms);
	}
	void debugPolygon(Common::Span<Math::Vector2d> points, Color color) override {
		firstDraw();
		setTexture(nullptr);
		setBlendMode(BlendMode::Alpha);
		Common::Array<Metal::Vertex> vertices;
		Common::Array<uint32> indices;
		for (uint i = 0; i < points.size(); ++i)
			vertices.push_back(vertex(points[i], color));
		for (uint i = 1; i + 1 < points.size(); ++i) {
			indices.push_back(0); indices.push_back(i); indices.push_back(i + 1);
		}
		if (!indices.empty())
			_renderer->drawIndexed(vertices.data(), vertices.size(), indices.data(), indices.size(), nullptr, _state, _uniforms);
		color.a = MIN(255.0f, color.a * 1.3f);
		if (points.size() > 1)
			_renderer->drawWideLine(vertex(points[points.size() - 1], color), vertex(points[0], color), 4, _state, _uniforms);
		debugPolyline(points, color);
	}
	void debugPolyline(Common::Span<Math::Vector2d> points, Color color) override {
		firstDraw();
		setTexture(nullptr);
		setBlendMode(BlendMode::Alpha);
		for (uint i = 1; i < points.size(); ++i)
			_renderer->drawWideLine(vertex(points[i - 1], color), vertex(points[i], color), 4, _state, _uniforms);
		color.a = MIN(255.0f, color.a * 1.3f);
		const uint32 indices[] = {0, 1, 2, 0, 2, 3};
		float dx = 4.0f * _resolution.x / _viewport.width();
		float dy = 4.0f * _resolution.y / _viewport.height();
		for (uint i = 0; i < points.size(); ++i) {
			float x = points[i].getX(), y = points[i].getY();
			Metal::Vertex v[] = {vertex(Math::Vector2d(x-dx, y-dy), color), vertex(Math::Vector2d(x-dx, y+dy), color),
			                     vertex(Math::Vector2d(x+dx, y+dy), color), vertex(Math::Vector2d(x+dx, y-dy), color)};
			_renderer->drawIndexed(v, 4, indices, 6, nullptr, _state, _uniforms);
		}
	}

protected:
	void setViewportInner(int x, int y, int width, int height) override {
		_viewport = Common::Rect(x, y, x + width, y + height);
		_renderer->setViewport(_viewport);
	}
	void setMatrices(bool flipped) override {
		_uniforms = Metal::Uniforms();
		_uniforms.transform[0] = 2.0f / _resolution.x;
		_uniforms.transform[5] = (flipped ? -2.0f : 2.0f) / _resolution.y;
		_uniforms.transform[10] = -1;
		_uniforms.transform[12] = -1;
		_uniforms.transform[13] = flipped ? 1 : -1;
		// Preserve the selected texture-combine operation across viewport changes.
		setBlendMode(_currentBlendMode == (BlendMode)-1 ? BlendMode::AdditiveAlpha : _currentBlendMode);
	}

private:
	static Metal::Vertex vertex(const Math::Vector2d &p, Color c) {
		return {{p.getX(), p.getY(), 0}, {0, 0}, {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f}};
	}
	void firstDraw() {
		if (_isFirstDrawCommand) {
			_isFirstDrawCommand = false;
			_renderer->clear(0, 0, 0, 1, true, false, false);
		}
	}
	Metal::Renderer *_renderer = nullptr;
	Metal::RenderTarget *_output = nullptr;
	MetalTexture *_texture = nullptr;
	Metal::DrawState _state;
	Metal::Uniforms _uniforms;
	Common::Rect _viewport;
};

IRenderer *IRenderer::createMetalRenderer(Common::Point resolution) {
	return new MetalRenderer(resolution);
}

} // End of namespace Alcachofa
