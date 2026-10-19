/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common/scummsys.h"

#ifdef USE_METAL

#include "freescape/gfx.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "graphics/metal/renderer.h"
#include "graphics/metal/geometry.h"
#include "math/glmath.h"

namespace Freescape {

class MetalTexture : public Texture {
public:
	MetalTexture(Graphics::Metal::Renderer *renderer, const Graphics::Surface &surface) : _renderer(renderer) {
		_width = surface.w; _height = surface.h; _format = surface.format;
		texture = renderer->createTexture(surface);
		if (!texture)
			error("Freescape: could not create Metal texture");
	}
	~MetalTexture() override { delete texture; }
	void update(const Graphics::Surface *surface) override { updatePartial(surface, Common::Rect(surface->w, surface->h)); }
	void updatePartial(const Graphics::Surface *surface, const Common::Rect &rect) override {
		assert(uint(surface->w) == _width && uint(surface->h) == _height);
		_renderer->updateTexture(*texture, *surface, rect);
	}
	Graphics::Metal::Texture *texture;
private:
	Graphics::Metal::Renderer *_renderer;
};

class MetalRenderer : public Renderer {
public:
	MetalRenderer(int w, int h, Common::RenderMode mode, bool authentic) : Renderer(w, h, mode, authentic),
		_metal(g_system->getMetalRenderer()), _geometry(_metal), _stippled(false) {
		if (!_metal)
			error("Freescape: Metal backend is unavailable");
		_texturePixelFormat = getRGBAPixelFormat();
		_isAccelerated = true;
		_variableStippleArray = _defaultStippleArray;
		_state.depthTest = _state.depthWrite = _state.depthClamp = true;
		_state.cull = _state.clockwise = true;
		_color[0] = _color[1] = _color[2] = _color[3] = 1;
	}
	void init() override {
		computeScreenViewport();
		scaleStipplePattern(_defaultStippleArray, _stipples[15]);
		memcpy(_defaultStippleArray, _stipples[15], 128);
		setViewport(_viewport);
	}
	Common::Point nativeResolution() override { return Common::Point(_viewport.width(), _viewport.height()); }
	Texture *createTexture(const Graphics::Surface *surface, bool is3D) override { return new MetalTexture(_metal, *surface); }
	void freeTexture(Texture *texture) override { delete texture; }
	void setViewport(const Common::Rect &rect) override {
		_viewport = Common::Rect(_screenViewport.width() * rect.width() / _screenW,
			_screenViewport.height() * rect.height() / _screenH);
		_viewport.translate(_screenViewport.left + _screenViewport.width() * rect.left / _screenW,
			_screenViewport.top + _screenViewport.height() * rect.top / _screenH);
		_unscaledViewport = rect;
		_metal->setViewport(_viewport);
		_state.scissor = true;
		_state.scissorRect = _viewport;
	}
	void clear(uint8 r, uint8 g, uint8 b, bool ignoreViewport) override {
		if (_stereoEye != kStereoEyeNone)
			applyStereoTint(r, g, b);
		_metal->clearRect(ignoreViewport ? Common::Rect(_metal->width(), _metal->height()) : _viewport,
			r / 255.f, g / 255.f, b / 255.f, 1, true, true, false, _state.colorMask);
	}
	void clearDepthBuffer(bool ignoreViewport) override {
		_metal->clearRect(ignoreViewport ? Common::Rect(_metal->width(), _metal->height()) : _viewport,
			0, 0, 0, 1, false, true);
	}
	void useColor(uint8 r, uint8 g, uint8 b) override {
		if (_stereoEye != kStereoEyeNone)
			applyStereoTint(r, g, b);
		_color[0] = r / 255.f; _color[1] = g / 255.f; _color[2] = b / 255.f;
	}
	void enableCulling(bool enabled) override { _state.cull = enabled; }
	void polygonOffset(bool enabled) override {
		_state.slopeBias = enabled ? -1 : 0;
		_state.depthBias = enabled ? 1 : 0;
	}
	void setStippleData(byte *data) override { _variableStippleArray = data ? data : _defaultStippleArray; }
	void useStipple(bool enabled) override {
		_stippled = enabled;
		_state.slopeBias = enabled ? _state.slopeBias - 0.5f : 0;
		_state.depthBias = enabled ? -1 : 0;
	}
	void setStereoEye(StereoEye eye) override {
		Renderer::setStereoEye(eye);
		_state.colorMask = eye == kStereoEyeLeft ? 9 : eye == kStereoEyeRight ? 12 : eye == kStereoEyeFlatAnaglyph ? 13 : 15;
	}
	void updateProjectionMatrix(float fov, float aspect, float nearPlane, float farPlane) override {
		float xmax = nearPlane * tan(Math::deg2rad(fov) / 2), ymax = xmax / aspect;
		float offset = _stereoEye == kStereoEyeLeft || _stereoEye == kStereoEyeRight ? getStereoFrustumOffset(nearPlane, true) : 0;
		_projectionMatrix = Math::makeFrustumMatrix(xmax + offset, -xmax + offset, -ymax, ymax, nearPlane, farPlane);
	}
	void positionCamera(const Math::Vector3d &position, const Math::Vector3d &interest, float rollAngle) override {
		Math::Vector3d eye = position, target = interest;
		if (_stereoEye == kStereoEyeLeft || _stereoEye == kStereoEyeRight)
			getStereoCamera(position, interest, eye, target);
		Math::Matrix4 look = Math::makeLookAtMatrix(eye, target, Math::Vector3d(0, 1, 0));
		Math::Matrix4 translation;
		translation.translate(-eye);
		translation.transpose();
		float c = cos(Math::deg2rad(rollAngle)), s = sin(Math::deg2rad(rollAngle));
		Math::Matrix4 roll;
		roll(0, 0) = c; roll(0, 1) = s; roll(1, 0) = -s; roll(1, 1) = c;
		roll(2, 2) = roll(3, 3) = 1;
		_modelViewMatrix = translation * roll * look;
		_mvpMatrix = combined(_modelViewMatrix);
	}
	void renderFace(const Common::Array<Math::Vector3d> &points) override {
		if (points.size() < 2)
			return;
		Graphics::Metal::DrawState state = _state;
		state.wireframe = _debugRenderWireframe;
		if (points.size() == 2) {
			_metal->drawWideLine(vertex(points[0]), vertex(points[1]), MAX(1, g_system->getWidth() / 640), state, uniforms3D());
			return;
		}
		drawFan(points, state, uniforms3D());
		if (_debugRenderNormals) {
			Math::Vector3d center;
			for (uint i = 0; i < points.size(); ++i)
				center += points[i];
			center /= points.size();
			Math::Vector3d normal = Math::Vector3d::crossProduct(points[1] - points[0], points[2] - points[0]);
			normal.normalize();
			Graphics::Metal::Vertex start = vertex(center), end = vertex(center + normal * 50.f);
			start.color[0] = end.color[0] = 1; start.color[1] = end.color[1] = 0; start.color[2] = end.color[2] = 1;
			_metal->drawWideLine(start, end, 1, state, uniforms3D());
		}
	}
	void drawTexturedRect2D(const Common::Rect &screen, const Common::Rect &area, Texture *texture) override {
		Graphics::Metal::Vertex vertices[] = {
			{{(float)screen.left, (float)screen.top, 0}, {area.left / (float)texture->_width, area.top / (float)texture->_height}, {1, 1, 1, 1}},
			{{(float)screen.left, (float)screen.bottom, 0}, {area.left / (float)texture->_width, area.bottom / (float)texture->_height}, {1, 1, 1, 1}},
			{{(float)screen.right, (float)screen.top, 0}, {area.right / (float)texture->_width, area.top / (float)texture->_height}, {1, 1, 1, 1}},
			{{(float)screen.right, (float)screen.bottom, 0}, {area.right / (float)texture->_width, area.bottom / (float)texture->_height}, {1, 1, 1, 1}}
		};
		Graphics::Metal::DrawState state = screenState();
		state.primitive = Graphics::Metal::kTriangleStrip;
		state.blend = true;
		_geometry.draw(vertices, 4, static_cast<MetalTexture *>(texture)->texture, state, uniforms2D());
	}
	void renderSensorShoot(byte color, const Math::Vector3d sensor, const Math::Vector3d player, const Common::Rect &viewArea) override {
		Graphics::Metal::DrawState state = _state;
		state.blend = true; state.srcRGB = Graphics::Metal::kOneMinusDstColor; state.dstRGB = Graphics::Metal::kBlendZero;
		useColor(255, 255, 255);
		_metal->drawWideLine(vertex(sensor), vertex(player), 20, state, uniforms3D());
	}
	void renderCrossair(const Common::Point &point) override {
		useColor(255, 255, 255);
		Graphics::Metal::DrawState state = invertState();
		float offsets[][4] = {{-3, 0, -1, 0}, {1, 0, 3, 0}, {0, -3, 0, -1}, {0, 1, 0, 3}};
		for (uint i = 0; i < 4; ++i)
			_metal->drawWideLine(vertex(Math::Vector3d(point.x + offsets[i][0], point.y + offsets[i][1], 0)),
				vertex(Math::Vector3d(point.x + offsets[i][2], point.y + offsets[i][3], 0)),
				MAX(2, g_system->getWidth() / 640), state, uniforms2D());
	}
	void renderPlayerShootRay(byte color, const Common::Point &point, const Common::Rect &viewArea) override {
		shotColor();
		Graphics::Metal::DrawState state = shotState();
		for (uint side = 0; side < 2; ++side)
			for (uint offset = 0; offset < 2; ++offset)
				_metal->drawWideLine(vertex(Math::Vector3d(side ? viewArea.right : viewArea.left, viewArea.bottom + offset * 3, 0)),
					vertex(Math::Vector3d(point.x, point.y, 0)), 5, state, uniforms2D());
	}
	void renderPlayerShootBall(byte color, const Common::Point &point, int frame, const Common::Rect &viewArea) override {
		shotColor();
		float coefficient = 1 - powf(0.5f, (8 - frame + 1) / 2.f), radius = 1 + frame * 0.5f;
		float x = coefficient * point.x + (1 - coefficient) * (viewArea.left + viewArea.width() / 2.f + 2);
		float y = coefficient * point.y + (1 - coefficient) * viewArea.bottom;
		Common::Array<Math::Vector3d> points;
		points.push_back(Math::Vector3d(x, y, 0));
		for (uint i = 0; i <= 20; ++i)
			points.push_back(Math::Vector3d(x + radius * cos(i * 2 * M_PI / 20), y + radius * sin(i * 2 * M_PI / 20), 0));
		drawFan(points, shotState(), uniforms2D());
	}
	void drawFloor(uint8 color) override {
		uint8 r1, g1, b1, r2, g2, b2; byte *stipple;
		if (!getRGBAt(color, 0, r1, g1, b1, r2, g2, b2, stipple))
			return;
		useColor(r1, g1, b1);
		Common::Array<Math::Vector3d> points;
		points.push_back(Math::Vector3d(-100000, 0, -100000)); points.push_back(Math::Vector3d(100000, 0, -100000));
		points.push_back(Math::Vector3d(100000, 0, 100000)); points.push_back(Math::Vector3d(-100000, 0, 100000));
		drawFan(points, _state, uniforms3D());
	}
	void fillViewportStippled(uint8 r1, uint8 g1, uint8 b1, uint8 r2, uint8 g2, uint8 b2, byte *stipple) override {
		Graphics::Metal::DrawState state = screenState();
		Common::Array<Math::Vector3d> points;
		points.push_back(Math::Vector3d(0, 0, 0)); points.push_back(Math::Vector3d(_screenW, 0, 0));
		points.push_back(Math::Vector3d(_screenW, _screenH, 0)); points.push_back(Math::Vector3d(0, _screenH, 0));
		useColor(r1, g1, b1);
		Graphics::Metal::Uniforms uniforms = uniforms2D();
		drawFan(points, state, uniforms);
		setStippleData(stipple);
		useColor(r2, g2, b2);
		uniforms.stippled = 1;
		memcpy(uniforms.stipple, stipplePattern(), sizeof(uniforms.stipple));
		drawFan(points, state, uniforms);
	}
	void drawSkybox(Texture *texture, Math::Vector3d camera) override {
		const float (*uv)[2] = texture->_width == 1008 ? _skyUvs1008 : texture->_width == 672 ? _skyUvs672 : _skyUvs128;
		if (texture->_width != 1008 && texture->_width != 672 && texture->_width != 128)
			error("Freescape: unsupported skybox texture width %u", texture->_width);
		Math::Matrix4 model = _modelViewMatrix;
		model(3, 0) = model(3, 1) = model(3, 2) = 0;
		Graphics::Metal::Uniforms uniforms;
		Math::Matrix4 mvp = combined(model);
		memcpy(uniforms.transform, mvp.getData(), sizeof(uniforms.transform));
		Graphics::Metal::Vertex vertices[16] = {};
		for (uint i = 0; i < 16; ++i) {
			memcpy(vertices[i].position, _skyVertices[i], sizeof(vertices[i].position));
			memcpy(vertices[i].texcoord, uv[i], sizeof(vertices[i].texcoord));
			for (uint c = 0; c < 4; ++c) vertices[i].color[c] = 1;
		}
		uint32 indices[24];
		for (uint q = 0; q < 4; ++q) {
			uint32 base = q * 4, quad[] = {base, base + 1, base + 2, base, base + 2, base + 3};
			memcpy(indices + q * 6, quad, sizeof(quad));
		}
		Graphics::Metal::DrawState state = _state;
		state.cull = false; state.depthWrite = false; state.wrapS = Graphics::Metal::kRepeat;
		_geometry.draw(vertices, 16, static_cast<MetalTexture *>(texture)->texture, state, uniforms, indices, 24);
	}
	void drawThunder(Texture *texture, Math::Vector3d position, float size) override {
		const float *mv = _modelViewMatrix.getData();
		Math::Matrix4 billboard;
		float *data = billboard.getData();
		memset(data, 0, sizeof(float) * 16);
		data[1] = -1; data[4] = 1; data[10] = data[15] = 1;
		for (uint row = 0; row < 3; ++row)
			data[12 + row] = mv[row] * position.x() + mv[4 + row] * position.y() + mv[8 + row] * position.z() + mv[12 + row];
		Math::Matrix4 mvp = combined(billboard);
		Graphics::Metal::Uniforms uniforms;
		memcpy(uniforms.transform, mvp.getData(), sizeof(uniforms.transform));
		float half = size / 2;
		Graphics::Metal::Vertex vertices[] = {
			{{-half, -half, 0}, {0, 0}, {1, 1, 1, 1}}, {{half, -half, 0}, {0, 0.72f}, {1, 1, 1, 1}},
			{{-half, half, 0}, {1, 0}, {1, 1, 1, 1}}, {{half, half, 0}, {1, 0.72f}, {1, 1, 1, 1}}
		};
		Graphics::Metal::DrawState state = _state;
		state.cull = false; state.blend = true; state.srcRGB = state.srcAlpha = state.dstRGB = state.dstAlpha = Graphics::Metal::kBlendOne;
		state.primitive = Graphics::Metal::kTriangleStrip;
		_geometry.draw(vertices, 4, static_cast<MetalTexture *>(texture)->texture, state, uniforms);
	}
	void drawCelestialBody(Math::Vector3d position, float radius, uint8 color) override {
		uint8 r1, g1, b1, r2, g2, b2; byte *stipple;
		if (!getRGBAt(color, 0, r1, g1, b1, r2, g2, b2, stipple))
			return;
		Math::Matrix4 model = _modelViewMatrix;
		for (uint i = 1; i < 4; ++i)
			if (i != 2)
				for (uint j = 0; j < 4; ++j)
					model(i, j) = i == j ? 1 : 0;
		Math::Matrix4 mvp = combined(model);
		Graphics::Metal::Uniforms uniforms;
		memcpy(uniforms.transform, mvp.getData(), sizeof(uniforms.transform));
		Common::Array<Math::Vector3d> points;
		points.push_back(position);
		for (uint i = 0; i <= 20; ++i)
			points.push_back(Math::Vector3d(position.x(), position.y() + radius * cos(i * 2 * M_PI / 20),
				position.z() + 1.25f * radius * sin(i * 2 * M_PI / 20)));
		Graphics::Metal::DrawState state = _state;
		state.depthWrite = false;
		useColor(r1, g1, b1);
		drawFan(points, state, uniforms);
		if (r1 != r2 || g1 != g2 || b1 != b2) {
			setStippleData(stipple); useColor(r2, g2, b2);
			uniforms.stippled = 1;
			memcpy(uniforms.stipple, stipplePattern(), sizeof(uniforms.stipple));
			state.slopeBias -= 0.5f; state.depthBias = -1;
			drawFan(points, state, uniforms);
		}
	}
	void drawAABB(const Math::AABB &box, uint8 r, uint8 g, uint8 b) override {
		if (!box.isValid()) return;
		Math::Vector3d minimum = box.getMin(), maximum = box.getMax();
		Math::Vector3d corners[8];
		for (uint i = 0; i < 8; ++i)
			corners[i] = Math::Vector3d(i & 1 ? maximum.x() : minimum.x(), i & 2 ? maximum.y() : minimum.y(), i & 4 ? maximum.z() : minimum.z());
		Graphics::Metal::DrawState state = _state;
		state.depthTest = state.depthWrite = state.cull = state.wireframe = false;
		Graphics::Metal::Uniforms uniforms = uniforms3D(); uniforms.stippled = false;
		for (uint i = 0; i < 8; ++i) {
			for (uint bit = 1; bit <= 4; bit <<= 1) {
				if (i & bit) continue;
				Graphics::Metal::Vertex start = vertex(corners[i]), end = vertex(corners[i | bit]);
				start.color[0] = end.color[0] = r / 255.f; start.color[1] = end.color[1] = g / 255.f;
				start.color[2] = end.color[2] = b / 255.f; start.color[3] = end.color[3] = 1;
				_metal->drawWideLine(start, end, 2, state, uniforms);
			}
		}
	}

	Graphics::Surface *getScreenshot() override {
		Graphics::Surface *surface = new Graphics::Surface;
		if (!_metal->readPixels(*surface, _screenViewport)) {
			delete surface;
			error("Freescape: could not read Metal screenshot");
		}
		return surface;
	}

private:
	Graphics::Metal::Renderer *_metal;
	Graphics::Metal::GeometryCache _geometry;
	Graphics::Metal::DrawState _state;
	float _color[4];
	bool _stippled;
	Math::Matrix4 combined(const Math::Matrix4 &model) const {
		Math::Matrix4 projection = _projectionMatrix, view = model;
		projection.transpose(); view.transpose();
		Math::Matrix4 result = projection * view;
		result.transpose();
		return result;
	}
	const byte *stipplePattern() const {
		return _renderMode == Common::kRenderZX || _renderMode == Common::kRenderCPC || _renderMode == Common::kRenderCGA ||
			_renderMode == Common::kRenderHercG ? _variableStippleArray : _defaultStippleArray;
	}
	Graphics::Metal::Vertex vertex(const Math::Vector3d &point) const {
		Graphics::Metal::Vertex result = {};
		result.position[0] = point.x(); result.position[1] = point.y(); result.position[2] = point.z();
		memcpy(result.color, _color, sizeof(_color));
		return result;
	}
	Graphics::Metal::Uniforms uniforms3D() const {
		Graphics::Metal::Uniforms uniforms;
		memcpy(uniforms.transform, _mvpMatrix.getData(), sizeof(uniforms.transform));
		for (uint column = 0; column < 4; ++column) {
			uniforms.transform[column * 4] += _shakeOffset.x * 0.025f * uniforms.transform[column * 4 + 3];
			uniforms.transform[column * 4 + 1] += _shakeOffset.y * 0.025f * uniforms.transform[column * 4 + 3];
		}
		uniforms.stippled = _stippled;
		memcpy(uniforms.stipple, stipplePattern(), sizeof(uniforms.stipple));
		return uniforms;
	}
	Graphics::Metal::Uniforms uniforms2D() const {
		Graphics::Metal::Uniforms uniforms;
		uniforms.transform[0] = 2.f / _screenW; uniforms.transform[5] = -2.f / _screenH;
		uniforms.transform[12] = -1; uniforms.transform[13] = 1;
		return uniforms;
	}
	Graphics::Metal::DrawState screenState() const {
		Graphics::Metal::DrawState state = _state;
		state.cull = state.depthTest = state.depthWrite = state.wireframe = false;
		state.depthBias = state.slopeBias = 0;
		return state;
	}
	Graphics::Metal::DrawState invertState() const {
		Graphics::Metal::DrawState state = screenState();
		state.blend = true; state.srcRGB = Graphics::Metal::kOneMinusDstColor; state.dstRGB = Graphics::Metal::kBlendZero;
		return state;
	}
	Graphics::Metal::DrawState shotState() const {
		return _renderMode == Common::kRenderCGA || _renderMode == Common::kRenderZX ? screenState() : invertState();
	}
	void shotColor() {
		if (_renderMode == Common::kRenderHercG) useColor(0, 255, 0);
		else if (_renderMode == Common::kRenderHercA) useColor(255, 191, 0);
		else useColor(255, 255, 255);
	}
	void drawFan(const Common::Array<Math::Vector3d> &points, const Graphics::Metal::DrawState &state,
			const Graphics::Metal::Uniforms &uniforms) {
		if (points.size() < 3)
			return;
		Common::Array<Graphics::Metal::Vertex> vertices;
		for (uint i = 1; i + 1 < points.size(); ++i) {
			vertices.push_back(vertex(points[0])); vertices.push_back(vertex(points[i])); vertices.push_back(vertex(points[i + 1]));
		}
		_geometry.draw(vertices.data(), vertices.size(), nullptr, state, uniforms);
	}
};

Renderer *CreateGfxMetal(int w, int h, Common::RenderMode mode, bool authentic) {
	return new MetalRenderer(w, h, mode, authentic);
}

} // End of namespace Freescape

#endif
