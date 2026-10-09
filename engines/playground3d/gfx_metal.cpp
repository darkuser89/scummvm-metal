/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common/scummsys.h"

#ifdef USE_METAL

#include "engines/playground3d/gfx.h"
#include "graphics/metal/renderer.h"
#include "common/textconsole.h"
#include "math/quat.h"

namespace Playground3d {

class MetalRenderer : public Renderer {
public:
	explicit MetalRenderer(OSystem *system) : Renderer(system), _metal(system->getMetalRenderer()),
		_textures{}, _fog(false), _animationX(0), _animationY(0) {
		if (!_metal)
			error("Playground3d: Metal backend is unavailable");
	}
	~MetalRenderer() override { deinit(); }
	void init() override { computeScreenViewport(); }
	void deinit() override {
		for (uint i = 0; i < ARRAYSIZE(_textures); ++i) {
			delete _textures[i];
			_textures[i] = nullptr;
		}
	}
	void clear(const Math::Vector4d &color) override { _metal->clear(color.x(), color.y(), color.z(), color.w()); }
	void setupViewport(int x, int y, int w, int h) override {
		_metal->setViewport(Common::Rect(x, _system->getHeight() - y - h, x + w, _system->getHeight() - y));
	}
	void loadTextureRGBA(Graphics::Surface *surface) override { loadTexture(TextureType::RGBA8888, *surface); }
	void loadTextureRGB(Graphics::Surface *surface) override { loadTexture(TextureType::RGB888, *surface); }
	void loadTextureRGB565(Graphics::Surface *surface) override { loadTexture(TextureType::RGB565, *surface); }
	void loadTextureRGBA5551(Graphics::Surface *surface) override { loadTexture(TextureType::RGBA5551, *surface); }
	void loadTextureRGBA4444(Graphics::Surface *surface) override { loadTexture(TextureType::RGBA4444, *surface); }
	void enableFog(const Math::Vector4d &color) override { _fog = true; _fogColor = color; }
	void disableFog() override { _fog = false; }
	void enableScissor(int x, int y, int w, int h) override {
		_state.scissor = true;
		_state.scissorRect = Common::Rect(x, _system->getHeight() - y - h, x + w, _system->getHeight() - y);
	}
	void disableScissor() override { _state.scissor = false; }
	void drawCube(const Math::Vector3d &position, const Math::Vector3d &roll) override {
		Math::Matrix4 rotation = Math::Quaternion::fromEuler(roll.x(), roll.y(), roll.z(), Math::EO_XYZ).inverse().toMatrix();
		Graphics::Metal::DrawState state = _state;
		state.primitive = Graphics::Metal::kTriangleStrip;
		state.depthTest = state.depthWrite = true;
		for (uint face = 0; face < 6; ++face) {
			Graphics::Metal::Vertex vertices[4] = {};
			for (uint i = 0; i < 4; ++i) {
				const float *source = cubeVertices + 11 * (face * 4 + i);
				vertices[i] = transformed(Math::Vector3d(source[2], source[3], source[4]), rotation, position);
				memcpy(vertices[i].texcoord, source, sizeof(vertices[i].texcoord));
				memcpy(vertices[i].color, source + 8, 3 * sizeof(float));
			}
			_metal->draw(vertices, 4, nullptr, state, uniforms3D());
		}
	}
	void drawPolyOffsetTest(const Math::Vector3d &position, const Math::Vector3d &roll) override {
		Math::Matrix4 rotation = Math::Quaternion::fromEuler(0, roll.y(), 0, Math::EO_XYZ).inverse().toMatrix();
		Graphics::Metal::DrawState state = _state;
		state.depthTest = state.depthWrite = true;
		Graphics::Metal::Vertex large[] = {
			transformed(Math::Vector3d(-1, 1, 0), rotation, position), transformed(Math::Vector3d(1, 1, 0), rotation, position),
			transformed(Math::Vector3d(0, -1, 0), rotation, position)
		};
		Graphics::Metal::Uniforms uniforms = uniforms3D();
		uniforms.tint[0] = uniforms.tint[2] = 0;
		_metal->draw(large, 3, nullptr, state, uniforms);
		Graphics::Metal::Vertex small[] = {
			transformed(Math::Vector3d(-0.5f, 0.5f, 0), rotation, position), transformed(Math::Vector3d(0.5f, 0.5f, 0), rotation, position),
			transformed(Math::Vector3d(0, -0.5f, 0), rotation, position)
		};
		uniforms.tint[0] = uniforms.tint[2] = 1;
		state.slopeBias = -1;
		_metal->draw(small, 3, nullptr, state, uniforms);
	}
	void drawQuadStripTest() override {
		Graphics::Metal::Uniforms uniforms;
		uniforms.tint[0] = 0; uniforms.tint[1] = 0.75f; uniforms.tint[2] = 0.2f;
		drawQuad(-0.8f, 0.7f, 0.8f, -0.7f, nullptr, overlayState(), uniforms);
		Graphics::Metal::Vertex triangle[] = {
			{{-0.12f, 0.12f, 0}, {0, 0}, {1, 0, 0, 1}}, {{0.12f, 0.12f, 0}, {0, 0}, {1, 0, 0, 1}},
			{{0, -0.12f, 0}, {0, 0}, {1, 0, 0, 1}}
		};
		_metal->draw(triangle, 3, nullptr, _state, Graphics::Metal::Uniforms());
	}
	void dimRegionInOut(float fade) override {
		Graphics::Metal::Uniforms uniforms;
		uniforms.tint[0] = uniforms.tint[1] = uniforms.tint[2] = 0;
		uniforms.tint[3] = 1 - fade;
		drawQuad(-0.5f, 0.5f, 0.5f, -0.5f, nullptr, overlayState(), uniforms);
	}
	void drawInViewport() override {
		Graphics::Metal::Uniforms uniforms;
		uniforms.tint[0] = uniforms.tint[2] = 0;
		drawQuad(-1, 1, 1, -1, nullptr, overlayState(), uniforms);
		_animationX += 0.01f; _animationY += 0.01f;
		if (_animationX >= 1.1f)
			_animationX = _animationY = -1.1f;
		uniforms.tint[0] = 1; uniforms.tint[1] = 0;
		Graphics::Metal::DrawState state = overlayState();
		state.slopeBias = -1;
		drawQuad(_animationX - 0.1f, _animationY + 0.1f, _animationX + 0.1f, _animationY - 0.1f, nullptr, state, uniforms);
	}
	void drawRgbaTexture() override {
		for (uint i = 0; i < TextureType::MAX; ++i) {
			if (!_textures[i])
				continue;
			float x = i == TextureType::RGBA4444 ? -0.8f : -0.8f + i * 0.5f;
			float y = i == TextureType::RGBA4444 ? 0.3f : 0.8f;
			drawQuad(x - 0.2f, y + 0.2f, x + 0.2f, y - 0.2f, _textures[i], overlayState(), Graphics::Metal::Uniforms());
		}
	}

private:
	Graphics::Metal::Renderer *_metal;
	Graphics::Metal::Texture *_textures[TextureType::MAX];
	Graphics::Metal::DrawState _state;
	bool _fog;
	Math::Vector4d _fogColor;
	float _animationX, _animationY;
	void loadTexture(uint type, const Graphics::Surface &surface) {
		delete _textures[type];
		_textures[type] = _metal->createTexture(surface);
		if (!_textures[type])
			error("Playground3d: could not upload Metal texture");
	}
	Graphics::Metal::Vertex transformed(const Math::Vector3d &point, const Math::Matrix4 &rotation, const Math::Vector3d &position) const {
		Graphics::Metal::Vertex vertex = {};
		const float *matrix = rotation.getData();
		for (uint row = 0; row < 3; ++row)
			vertex.position[row] = matrix[row] * point.x() + matrix[row + 4] * point.y() + matrix[row + 8] * point.z() + position.getValue(row);
		vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 1;
		return vertex;
	}
	Graphics::Metal::Uniforms uniforms3D() const {
		Graphics::Metal::Uniforms uniforms;
		memcpy(uniforms.transform, _mvpMatrix.getData(), sizeof(uniforms.transform));
		memcpy(uniforms.modelView, _modelViewMatrix.getData(), sizeof(uniforms.modelView));
		if (_fog) {
			memcpy(uniforms.fogColor, _fogColor.getData(), sizeof(uniforms.fogColor));
			uniforms.fogParameters[0] = 0.1f;
			uniforms.fogParameters[3] = 2;
		}
		return uniforms;
	}
	Graphics::Metal::DrawState overlayState() const {
		Graphics::Metal::DrawState state = _state;
		state.blend = true;
		state.srcRGB = state.srcAlpha = Graphics::Metal::kBlendOne;
		state.dstRGB = state.dstAlpha = Graphics::Metal::kOneMinusSrcAlpha;
		state.primitive = Graphics::Metal::kTriangleStrip;
		return state;
	}
	void drawQuad(float left, float top, float right, float bottom, Graphics::Metal::Texture *texture,
			const Graphics::Metal::DrawState &state, const Graphics::Metal::Uniforms &uniforms) {
		Graphics::Metal::Vertex vertices[] = {
			{{left, top, 0}, {0, 0}, {1, 1, 1, 1}}, {{left, bottom, 0}, {0, 1}, {1, 1, 1, 1}},
			{{right, top, 0}, {1, 0}, {1, 1, 1, 1}}, {{right, bottom, 0}, {1, 1}, {1, 1, 1, 1}}
		};
		_metal->draw(vertices, 4, texture, state, uniforms);
	}
};

Renderer *CreateGfxMetal(OSystem *system) { return new MetalRenderer(system); }

} // End of namespace Playground3d

#endif
