/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common/scummsys.h"

#ifdef USE_METAL

#include "engines/myst3/gfx.h"
#include "graphics/metal/renderer.h"
#include "graphics/metal/geometry.h"
#include "common/textconsole.h"

namespace Myst3 {

class MetalTexture : public Texture {
public:
	MetalTexture(Graphics::Metal::Renderer *renderer, const Graphics::Surface &surface) : _renderer(renderer) {
		width = surface.w;
		height = surface.h;
		format = surface.format;
		texture = renderer->createTexture(surface);
		if (!texture)
			error("Myst3: could not create Metal texture");
	}
	MetalTexture(Graphics::Metal::Renderer *renderer, Graphics::Metal::RenderTarget *target) : _renderer(renderer), _target(target) {
		texture = &target->texture(); width = texture->width(); height = texture->height();
		format = Graphics::PixelFormat::createFormatRGBA32();
	}
	~MetalTexture() override { if (_target) delete _target; else delete texture; }
	void update(const Graphics::Surface *surface) override {
		updatePartial(surface, Common::Rect(surface->w, surface->h));
	}
	void updatePartial(const Graphics::Surface *surface, const Common::Rect &rect) override {
		assert(uint(surface->w) == width && uint(surface->h) == height);
		_renderer->updateTexture(*texture, *surface, rect);
	}
	Graphics::Metal::Texture *texture;

private:
	Graphics::Metal::Renderer *_renderer;
	Graphics::Metal::RenderTarget *_target = nullptr;
};

class MetalRenderer : public Renderer {
public:
	explicit MetalRenderer(OSystem *system) : Renderer(system), _metal(system->getMetalRenderer()),
		_logicalWidth(kOriginalWidth), _logicalHeight(kOriginalHeight), _geometry(_metal) {
		if (!_metal)
			error("Myst3: Metal backend is unavailable");
	}

	void init() override { computeScreenViewport(); }
	void clear() override { _metal->clear(0, 0, 0); }
	Texture *createTexture3D(const Graphics::Surface *surface) override { return new MetalTexture(_metal, *surface); }

	void selectTargetWindow(Window *window, bool is3D, bool scaled) override {
		Common::Rect target;
		if (window) {
			target = window->getPosition();
			Common::Rect logical = scaled ? window->getOriginalPosition() : target;
			_logicalWidth = logical.width();
			_logicalHeight = logical.height();
		} else {
			target = scaled ? viewport() : Common::Rect(_system->getWidth(), _system->getHeight());
			_logicalWidth = scaled ? kOriginalWidth : target.width();
			_logicalHeight = scaled ? kOriginalHeight : target.height();
		}
		_metal->setViewport(target);
	}

	void drawRect2D(const Common::Rect &rect, uint8 a, uint8 r, uint8 g, uint8 b) override {
		Graphics::Metal::Uniforms uniforms = uniforms2D();
		uniforms.tint[0] = r / 255.f; uniforms.tint[1] = g / 255.f;
		uniforms.tint[2] = b / 255.f; uniforms.tint[3] = a / 255.f;
		Graphics::Metal::DrawState state;
		state.primitive = Graphics::Metal::kTriangleStrip;
		state.blend = a != 255;
		drawQuad(rect, 0, 0, 1, 1, nullptr, state, uniforms);
	}

	void drawTexturedRect2D(const Common::Rect &screenRect, const Common::Rect &textureRect,
			Texture *texture, float transparency, bool additiveBlending) override {
		Graphics::Metal::Uniforms uniforms = uniforms2D();
		uniforms.tint[3] = transparency >= 0 ? transparency : 1;
		Graphics::Metal::DrawState state;
		state.primitive = Graphics::Metal::kTriangleStrip;
		state.blend = transparency >= 0;
		if (additiveBlending)
			state.dstRGB = state.dstAlpha = Graphics::Metal::kBlendOne;
		drawQuad(screenRect, textureRect.left / (float)texture->width, textureRect.top / (float)texture->height,
			textureRect.right / (float)texture->width, textureRect.bottom / (float)texture->height,
			static_cast<MetalTexture *>(texture)->texture, state, uniforms);
	}

	void drawTexturedRect3D(const Math::Vector3d &topLeft, const Math::Vector3d &bottomLeft,
			const Math::Vector3d &topRight, const Math::Vector3d &bottomRight, Texture *texture) override {
		const Math::Vector3d points[] = {topLeft, bottomLeft, topRight, bottomRight};
		Graphics::Metal::Vertex vertices[4] = {};
		for (uint i = 0; i < 4; ++i) {
			vertices[i].position[0] = -points[i].x();
			vertices[i].position[1] = points[i].y();
			vertices[i].position[2] = points[i].z();
			vertices[i].texcoord[0] = i >= 2 ? 1 : 0;
			vertices[i].texcoord[1] = i & 1;
			for (uint c = 0; c < 4; ++c)
				vertices[i].color[c] = 1;
		}
		Graphics::Metal::DrawState state;
		state.primitive = Graphics::Metal::kTriangleStrip;
		state.depthTest = true;
		state.blend = true;
		_geometry.draw(vertices, 4, static_cast<MetalTexture *>(texture)->texture, state, uniforms3D());
	}

	void drawCube(Texture **textures) override {
		Graphics::Metal::DrawState state;
		state.primitive = Graphics::Metal::kTriangleStrip;
		state.depthTest = true;
		Graphics::Metal::Uniforms uniforms = uniforms3D();
		for (uint face = 0; face < 6; ++face) {
			Graphics::Metal::Vertex vertices[4] = {};
			for (uint i = 0; i < 4; ++i) {
				const float *source = cubeVertices + 5 * (4 * face + i);
				memcpy(vertices[i].texcoord, source, 2 * sizeof(float));
				memcpy(vertices[i].position, source + 2, 3 * sizeof(float));
				for (uint c = 0; c < 4; ++c)
					vertices[i].color[c] = 1;
			}
			_geometry.draw(vertices, 4, static_cast<MetalTexture *>(textures[face])->texture, state, uniforms);
		}
	}

	void draw2DText(const Common::String &text, const Common::Point &position) override {
		if (!_font)
			return;
		Common::String uppercase = text;
		uppercase.toUppercase();
		Graphics::Metal::DrawState state;
		state.primitive = Graphics::Metal::kTriangleStrip;
		state.blend = true;
		if (uppercase != _text || position != _textPosition || _textFontWidth != _font->width || _textFontHeight != _font->height) {
			_text = uppercase; _textPosition = position; _textFontWidth = _font->width; _textFontHeight = _font->height;
			_textVertices.clear();
			int x = position.x;
			for (uint i = 0; i < uppercase.size(); ++i) {
				Common::Rect atlas = getFontCharacterRect(uppercase[i]);
				float left = x, top = position.y, right = x + atlas.width(), bottom = position.y + atlas.height();
				float u0 = atlas.left / float(_font->width), u1 = atlas.right / float(_font->width);
				float v0 = atlas.bottom / float(_font->height), v1 = atlas.top / float(_font->height);
				Graphics::Metal::Vertex quad[] = {{{left,top,0},{u0,v0},{1,1,1,1}},{{left,bottom,0},{u0,v1},{1,1,1,1}},
					{{right,top,0},{u1,v0},{1,1,1,1}},{{right,bottom,0},{u1,v1},{1,1,1,1}}};
				static const uint order[] = {0,1,2,2,1,3};
				for (uint vertex : order) _textVertices.push_back(quad[vertex]);
				x += atlas.width() - 3;
			}
		}
		state.primitive = Graphics::Metal::kTriangles;
		_geometry.draw(_textVertices.data(), _textVertices.size(), static_cast<MetalTexture *>(_font)->texture, state, uniforms2D());
	}

	Texture *copyScreenshotToTexture() override {
		Common::Rect screen = viewport();
		Graphics::Metal::RenderTarget *target = _metal->createRenderTarget(screen.width(), screen.height(), 1, Graphics::Metal::kTargetBGRA8, false, false);
		if (!target) error("Myst3: could not allocate GPU transition image");
		_metal->copyTexture(target->texture(), Common::Rect(screen.width(), screen.height()), *_metal->screenTexture(), screen);
		return new MetalTexture(_metal, target);
	}

	Graphics::Surface *getScreenshot() override {
		Graphics::Surface *surface = new Graphics::Surface();
		if (!_metal->readPixels(*surface, viewport())) {
			delete surface;
			error("Myst3: could not read Metal screenshot");
		}
		return surface;
	}

private:
	Graphics::Metal::Renderer *_metal;
	uint _logicalWidth, _logicalHeight;
	Graphics::Metal::GeometryCache _geometry;
	Common::String _text;
	Common::Point _textPosition;
	uint _textFontWidth = 0, _textFontHeight = 0;
	Common::Array<Graphics::Metal::Vertex> _textVertices;

	Graphics::Metal::Uniforms uniforms2D() const {
		Graphics::Metal::Uniforms uniforms;
		uniforms.transform[0] = 2.f / _logicalWidth;
		uniforms.transform[5] = -2.f / _logicalHeight;
		uniforms.transform[12] = -1;
		uniforms.transform[13] = 1;
		return uniforms;
	}
	Graphics::Metal::Uniforms uniforms3D() const {
		Graphics::Metal::Uniforms uniforms;
		memcpy(uniforms.transform, _mvpMatrix.getData(), sizeof(uniforms.transform));
		return uniforms;
	}
	void drawQuad(const Common::Rect &rect, float u0, float v0, float u1, float v1,
			Graphics::Metal::Texture *texture, const Graphics::Metal::DrawState &state,
			const Graphics::Metal::Uniforms &uniforms) {
		Graphics::Metal::Vertex vertices[] = {
			{{(float)rect.left, (float)rect.top, 0}, {u0, v0}, {1, 1, 1, 1}},
			{{(float)rect.left, (float)rect.bottom, 0}, {u0, v1}, {1, 1, 1, 1}},
			{{(float)rect.right, (float)rect.top, 0}, {u1, v0}, {1, 1, 1, 1}},
			{{(float)rect.right, (float)rect.bottom, 0}, {u1, v1}, {1, 1, 1, 1}}
		};
		_geometry.draw(vertices, 4, texture, state, uniforms);
	}
};

Renderer *CreateGfxMetal(OSystem *system) { return new MetalRenderer(system); }

} // End of namespace Myst3

#endif
